#pragma once

// Battery power latch (BAT_Control / VBAT_PWR, GPIO17) for the Waveshare
// ESP32-S3-Touch-ePaper-1.54.
//
// Why this exists
// ---------------
// On battery the board's rails are held up by a latch, not by the PWR key. The
// PWR key (BAT_KEY, GPIO18) is a momentary switch: pressing it connects the
// battery long enough for the firmware to run, and the firmware must then drive
// GPIO17 HIGH to keep it there. Release the key with GPIO17 still LOW/floating
// and the board loses power immediately, even though the firmware is mid-boot.
//
// Official Waveshare sources for this product:
//
//   waveshareteam/ESP32-S3-ePaper-1.54 @ main
//     02_Example/Arduino/07_BATT_PWR_Test/user_config.h
//         #define VBAT_PWR_PIN    GPIO_NUM_17
//         #define PWR_BUTTON_PIN  GPIO_NUM_18
//     02_Example/Arduino/07_BATT_PWR_Test/src/power/board_power_bsp.cpp
//         VBAT_POWER_ON()  -> gpio_set_level(vbat_power_pin, 1)
//         VBAT_POWER_OFF() -> gpio_set_level(vbat_power_pin, 0)
//
// So: GPIO17 HIGH maintains battery power, GPIO17 LOW releases it, and GPIO18 is
// the key itself - an input that must never be driven to hold the rail.
//
// Timing
// ------
// keepBatteryPowerOn() is the *first* statement of setup(), ahead of Serial, the
// display, touch, I2C, Wi-Fi, audio, the RTC and the battery monitor. It only
// touches one GPIO, so it is safe that early and it never blocks. Everything the
// board does afterwards keeps the pin HIGH for the rest of the run: once
// configured as an output and written HIGH, nothing else in the firmware writes
// GPIO17, so the latch stays asserted.
//
// The window from reset to setup() (ROM + second stage bootloader) is not
// covered here. A PWR press must last long enough to reach setup(); a press
// shorter than the boot time cannot latch the rail by any firmware means.
//
// What was deliberately NOT done
// ------------------------------
// * No deep sleep, no esp_sleep_enable_*, no gpio_hold_en(). The board never
//   enters deep sleep, and a hold would only matter if it did.
// * powerOff() exists as the documented counterpart of the latch and is
//   intentionally never called by the firmware.
//
// A visible UI is not proof of a running MCU
// ------------------------------------------
// An e-paper panel is bistable: it holds its last image with no power at all.
// So "the screen still shows the UI" after a battery disconnect says nothing
// about the MCU, the touch controller or Wi-Fi - in fact a frozen screen, dead
// touch and dead Wi-Fi together mean the rails dropped while the image stayed.
// Never diagnose the latch from the picture alone; read the [power] and [boot]
// serial lines, or check that touch/Wi-Fi still respond.

#include <Arduino.h>
#include <driver/gpio.h>

class BoardPower {
public:
    // One-shot: re-asserts the latch (idempotent) and prints the two pin-map
    // lines. Call after Serial.begin(). Nothing is printed continuously.
    void begin();

    // Configures GPIO17 as an output and drives it HIGH to hold the battery
    // rail. Idempotent and safe to call before Serial is up: it touches nothing
    // but that one GPIO. This is the first statement of setup().
    void keepBatteryPowerOn();

    // Drives GPIO17 LOW, releasing the latch; on battery the board then powers
    // down. Intentionally unused by this firmware (no deep sleep yet).
    void powerOff();

    // True once keepBatteryPowerOn() has asserted the latch.
    bool latchOn() const { return latchOn_; }

private:
    void configurePin();

    bool configured_ = false;
    bool latchOn_ = false;
    bool reported_ = false;
};
