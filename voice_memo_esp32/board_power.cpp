#include "board_power.h"

#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "config.h"

namespace {

// GPIO18 is RTC_GPIO18 on the ESP32-S3 (the RTC domain covers GPIO0..GPIO21),
// so it is a legal EXT1 wake source. The official Waveshare RTC-sleep example
// uses exactly this mask for the PWR key, with ESP_EXT1_WAKEUP_ANY_LOW matching
// the confirmed active-low polarity of BAT_KEY.
constexpr uint64_t kPwrWakeMask = 1ULL << VM_PWR_KEY_PIN;

constexpr gpio_num_t latchPin() {
    return static_cast<gpio_num_t>(VM_VBAT_PWR_PIN);
}

constexpr gpio_num_t pwrPin() {
    return static_cast<gpio_num_t>(VM_PWR_KEY_PIN);
}

}  // namespace

void BoardPower::configurePin() {
    if (configured_) {
        return;
    }

    // Matches the official board_power_bsp_t constructor: plain output, no
    // interrupt, internal pull-up enabled. The pull-up keeps the gate biased
    // HIGH during the brief moment the pin is high-Z at reset.
    gpio_config_t config = {};
    config.intr_type = GPIO_INTR_DISABLE;
    config.mode = GPIO_MODE_OUTPUT;
    config.pin_bit_mask = (0x1ULL << VM_VBAT_PWR_PIN);
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&config);

    configured_ = true;
}

bool BoardPower::releaseSleepPadsIfNeeded() {
    // Deliberately runs AFTER keepBatteryPowerOn(): none of this is needed to
    // assert the latch, and nothing here may sit between reset and GPIO17 HIGH.
    //
    // GPIO18 was handed to the RTC controller as the EXT1 wake pad. Deinit
    // returns it to the digital GPIO matrix so it reads as a normal input; the
    // `pwrPin()` RTC pull configured by configureWakeSource() is disconnected by
    // the same call, and configurePin()'s digital pull-up takes over once
    // Button/BoardPower configure the pin.
    //
    // Unconditional: on a cold boot this is a harmless no-op that returns
    // ESP_ERR_INVALID_STATE, and on a wake it is required.
    rtc_gpio_deinit(pwrPin());

    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_EXT1) {
        return false;
    }

    // EXT1 can cover several pins; only PWR is armed by this firmware, but the
    // status is still checked explicitly rather than assumed.
    const uint64_t status = esp_sleep_get_ext1_wakeup_status();
    wokeFromPwr_ = (status & kPwrWakeMask) != 0;
    return true;
}

void BoardPower::keepBatteryPowerOn() {
    // BATTERY BOOT CRITICAL PATH. No logging, no delay(), no bus access: on
    // battery the PWR key is the only thing holding the rail up right now.
    //
    // (1) Write the output latch BEFORE anything else. gpio_set_level() only
    // updates the output register (it does not require the pin to be an output
    // yet), so the pad rises from high-Z straight to HIGH and never emits the LOW
    // pulse that a config-then-set order would produce. On battery that glitch is
    // the difference between latching and a brown-out.
    gpio_set_level(latchPin(), 1);

    // (2) Drop any pad hold left by a previous deep sleep. An engaged hold
    // OVERRIDES the output, so until it is dropped step (1) has no effect on the
    // pad. Order is load-bearing: the level was just written to the output
    // register, so when the hold is released the pad is already being driven HIGH
    // and cannot glitch - which is what the ESP-IDF contract for gpio_hold_dis()
    // requires ("the gpio should be configured to a known state before this
    // function is called"). Releasing it first would instead let the pad fall
    // back to its default level first.
    //
    // Also drops the global deep-sleep pad hold: this firmware never enables it,
    // but releasing it here is free and guarantees a clean slate.
    gpio_hold_dis(latchPin());
    gpio_deep_sleep_hold_dis();

    // (3) Drive the pin as an output and assert the level again, now that nothing
    // can override it.
    configurePin();
    gpio_set_level(latchPin(), 1);

    latchOn_ = true;
}

void BoardPower::configureWakeSource() {
#if VM_PWR_WAKE_ON_PWR
    // Arduino.h -> esp32-hal.h -> esp_sleep.h already provides these, but the
    // include above is explicit so the dependency is visible at the call site.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // Belt and braces on top of the board's external 10K pull-up R58: with the
    // RTC peripherals powered down the internal digital pull-up no longer
    // applies, so the RTC pull is enabled explicitly. This is what keeps the key
    // from floating while the device is asleep.
    rtc_gpio_pulldown_dis(pwrPin());
    rtc_gpio_pullup_en(pwrPin());

    // Non-zero result is purposeful: a wake source that cannot be armed is
    // exactly the situation where the device would sleep with no way to wake, so
    // it is reported rather than ignored.
    const esp_err_t err = esp_sleep_enable_ext1_wakeup_io(kPwrWakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        Serial.printf("[power] WARNING: could not arm PWR wake source: %s\n", esp_err_to_name(err));
    }
#else
    // Compile-time opt-out: with no wake source the deep sleep below would be a
    // one-way trip on USB, so the wake arming is skipped and only the latch is
    // released. Documented in README.
    Serial.println("[power] PWR wake source disabled (VM_PWR_WAKE_ON_PWR=0)");
#endif
}

void BoardPower::powerOff() {
    configurePin();

    // Hold the released level through deep sleep. Without this the RTC domain
    // would let GPIO17 float while the digital domain is off; the board's 100K
    // R63 would then pull the latch gate back up to VBAT and the device would
    // power itself on again. The hold is released by keepBatteryPowerOn() on the
    // next boot, in the same call that re-asserts the latch.
    gpio_set_level(latchPin(), 0);
    gpio_hold_en(latchPin());
    latchOn_ = false;

    configureWakeSource();

    Serial.printf("[power] battery latch OFF gpio=%d level=LOW\n", VM_VBAT_PWR_PIN);
    Serial.printf("[power] entering deep sleep; wake source=PWR gpio=%d level=LOW\n", VM_PWR_KEY_PIN);
    Serial.flush();

    // On battery the rail is already collapsing and this call never returns
    // either way; with USB attached it is what actually turns the device off
    // while the e-paper keeps the POWERED OFF image.
    esp_deep_sleep_start();
}

void BoardPower::begin() {
    // Idempotent: begin() runs right after keepBatteryPowerOn(), and re-driving
    // an already asserted latch cannot glitch it.
    keepBatteryPowerOn();

    if (reported_) {
        return;
    }
    reported_ = true;

    // One-shot, never in loop().
    Serial.printf("[power] battery latch gpio=%d level=HIGH\n", VM_VBAT_PWR_PIN);
    Serial.printf("[power] PWR key gpio=%d active_low=%d debounce_ms=%u\n",
                  VM_PWR_KEY_PIN,
                  VM_PWR_KEY_ACTIVE_LOW,
                  static_cast<unsigned int>(VM_PWR_DEBOUNCE_MS));
#if VM_ENABLE_AUTO_POWER_OFF
    Serial.printf("[power] auto power-off enabled timeout_ms=%lu\n",
                  static_cast<unsigned long>(VM_AUTO_POWER_OFF_MS));
#else
    Serial.println("[power] auto power-off disabled");
#endif
#if VM_PWR_WAKE_ON_PWR
    Serial.println("[power] deep-sleep wake on PWR enabled (USB-attached off state)");
#endif
}
