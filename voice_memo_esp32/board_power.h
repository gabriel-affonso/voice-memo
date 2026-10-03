#pragma once

// Battery power latch (BAT_Control / VBAT_PWR, GPIO17) and PWR key (BAT_KEY,
// GPIO18) for the Waveshare ESP32-S3-Touch-ePaper-1.54.
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
//     02_Example/Arduino/11_RTC_Sleep_Test/src/power/board_power_bsp.cpp
//         gpio_hold_en(GPIO17); esp_sleep_enable_ext1_wakeup_io(1ULL << 18,
//         ESP_EXT1_WAKEUP_ANY_LOW); esp_deep_sleep_start();
//
// So: GPIO17 HIGH maintains battery power, GPIO17 LOW releases it, and GPIO18 is
// the key itself - an input that must never be driven to hold the rail.
//
// This class is the ONLY place in the firmware allowed to touch GPIO17. Nothing
// above it writes the latch pin, so the app can never cut its own power by
// accident and there is exactly one place to audit.
//
// Timing: the battery boot critical path
// --------------------------------------
// On battery the rail exists only while the PWR key is held or GPIO17 is HIGH,
// so the latch must be asserted at the FIRST moment the firmware is able to run.
// keepBatteryPowerOn() is therefore the first statement of setup(), ahead of
// Serial, the display, touch, I2C, Wi-Fi, audio, the RTC, the battery monitor and
// every diagnostic. It performs no logging, no delay() and no I2C/SPI access, and
// it cannot block: nothing on this path is allowed to spend the key-hold window.
//
// The window from reset to setup() (ROM + second stage bootloader) is not
// covered by firmware, so a PWR press must last long enough to reach setup(); a
// press shorter than the boot time cannot latch the rail by any firmware means.
//
// Why the deep-sleep hold release is on that critical path
// -------------------------------------------------------
// A previous power-off holds GPIO17 LOW through deep sleep so the board cannot
// pull its own latch gate back up. On the next reset that hold may still be
// engaged, and an engaged hold OVERRIDES the output: gpio_set_level() would be
// ignored and the latch could never be asserted. The hold-release therefore has
// to happen before (or with) the assertion - it cannot be deferred to a "after
// the rail is safe" step, because until it is dropped the rail cannot be made
// safe at all.
//
// The ESP-IDF contract for gpio_hold_dis() is exactly what makes this safe to do
// first (esp_driver_gpio/include/driver/gpio.h):
//
//   "When the chip is woken up from peripheral power-down sleep, the gpio will
//    be set to the default mode, so, the gpio will output the default level if
//    this function is called. If you don't want the level changes, the gpio
//    should be configured to a known state before this function is called."
//
// So the level is written to the output register FIRST, while the hold is still
// doing its job of keeping the pad steady, and only then is the hold dropped: the
// pad is already being driven to the level we want, so releasing it cannot glitch
// the gate. This is also why the assertion is written twice - once before the pin
// is switched to output mode (see keepBatteryPowerOn()) and once after.
//
// The two calls are cheap register operations with no bus traffic, no logging and
// no delay. On a cold boot the hold is not engaged and gpio_hold_dis() is a
// harmless no-op, so there is no cost to paying for it on every boot.
//
// What happens on power-off (and why it is not just "GPIO17 LOW")
// --------------------------------------------------------------
// Cutting GPIO17 alone is a complete power-off only on battery. The official
// schematic gives the board an independent VBUS/VBAT power path, so with USB
// attached the MCU keeps running after the latch is released. powerOff()
// therefore does both halves of what Waveshare's own RTC-sleep example does:
// release the latch, configure GPIO18 as an EXT1 (ANY_LOW) deep-sleep wake
// source, and enter deep sleep. On battery the rail collapses first and the
// sleep never matters; on USB the deep sleep is what makes "off" real, and
// pressing PWR wakes the device again.
//
// The e-paper panel is bistable: it holds whatever it was last given with no
// power at all, so the POWERED OFF screen stays visible either way. That also
// means a visible screen is NOT proof that the MCU is running - see README.
//
// A stale GPIO17 hold from a previous deep sleep
// ----------------------------------------------
// The ESP32-S3 RTC domain keeps the pin hold across a deep-sleep wake, and a
// hold at LOW would fight the latch assertion and can brown the board out in a
// reset loop. Releasing it is part of keepBatteryPowerOn() (see above), so the
// latch is asserted in the same breath. GPIO18 - which the RTC took over as a
// wake pad - is handed back to the digital GPIO matrix by
// releaseSleepPadsIfNeeded(), which is deliberately NOT on the critical path
// because it touches a pin the latch does not depend on.

#include <Arduino.h>
#include <driver/gpio.h>

class BoardPower {
public:
    // One-shot: re-asserts the latch (idempotent) and prints the pin map.
    // Call after Serial.begin(). Nothing is printed continuously.
    void begin();

    // THE BATTERY BOOT CRITICAL PATH. Must be the first statement of setup().
    //
    //     // Battery boot critical path:
    //     // assert VBAT latch before any non-essential initialization.
    //     boardPower.keepBatteryPowerOn();
    //
    // Drops any pad hold left by a previous deep sleep (which would otherwise
    // override the output and make the assertion impossible), configures GPIO17
    // as an output and drives it HIGH to hold the battery rail. Idempotent, safe
    // before Serial is up, and deliberately free of logging, delay() and bus
    // access: it touches nothing but that one GPIO.
    void keepBatteryPowerOn();

    // The rest of the deep-sleep wake cleanup, called immediately AFTER
    // keepBatteryPowerOn() so the rail is already secured. Hands GPIO18 - taken
    // over by the RTC as the EXT1 wake pad - back to the digital GPIO matrix so
    // it reads as a normal input, and reports whether this reset was a
    // deep-sleep wake.
    //
    // Nothing here is needed to assert the latch: it is grouped separately and
    // runs second only because diagnostics must never delay GPIO17.
    // Returns true when the reset was a deep-sleep wake rather than a power-on.
    bool releaseSleepPadsIfNeeded();

    // True when the last reset was an EXT1 wake, i.e. the user pressed PWR while
    // the device was in deep sleep.
    bool wokeFromPowerButton() const { return wokeFromPwr_; }

    // Drives GPIO17 LOW, releasing the latch; on battery the board then powers
    // down. Then, if VM_PWR_WAKE_ON_PWR is enabled, arms GPIO18 as a deep-sleep
    // wake source and enters deep sleep so the USB-powered case also behaves as
    // "off". Never returns: on battery the rail is already gone, on USB the chip
    // is asleep until PWR is pressed again.
    [[noreturn]] void powerOff();

    // True once keepBatteryPowerOn() has asserted the latch.
    bool latchOn() const { return latchOn_; }

private:
    void configurePin();
    void configureWakeSource();

    bool configured_ = false;
    bool latchOn_ = false;
    bool reported_ = false;
    bool wokeFromPwr_ = false;
};
