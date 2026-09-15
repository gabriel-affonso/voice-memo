#include "board_power.h"

#include "config.h"

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

void BoardPower::keepBatteryPowerOn() {
    // Write the output latch BEFORE enabling the driver. gpio_set_level() only
    // updates the output register (it does not require the pin to be an output
    // yet), so the pad rises from high-Z straight to HIGH and never emits the
    // LOW pulse that a config-then-set order would produce. On battery that
    // glitch is the difference between latching and a brown-out.
    gpio_set_level(static_cast<gpio_num_t>(VM_VBAT_PWR_PIN), 1);
    configurePin();
    gpio_set_level(static_cast<gpio_num_t>(VM_VBAT_PWR_PIN), 1);

    latchOn_ = true;
}

void BoardPower::powerOff() {
    configurePin();
    gpio_set_level(static_cast<gpio_num_t>(VM_VBAT_PWR_PIN), 0);
    latchOn_ = false;
    Serial.printf("[power] battery latch gpio=%d level=LOW\n", VM_VBAT_PWR_PIN);
}

void BoardPower::begin() {
    // Idempotent: begin() runs right after keepBatteryPowerOn(), and re-driving
    // an already asserted latch cannot glitch it.
    keepBatteryPowerOn();

    if (reported_) {
        return;
    }
    reported_ = true;

    // One-shot, never in loop(). GPIO18 is only named here: it is the PWR key
    // and is deliberately left untouched by the firmware.
    Serial.printf("[power] battery latch gpio=%d level=HIGH\n", VM_VBAT_PWR_PIN);
    Serial.printf("[power] PWR key gpio=%d\n", VM_PWR_KEY_PIN);
}
