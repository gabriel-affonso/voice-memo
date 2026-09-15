#include "battery_monitor.h"

#include <Arduino.h>

#include "config.h"

namespace voice_memo_ui {
namespace {

// Averaging a handful of conversions smooths the ADC noise on a Li-ion rail;
// the official example reads once, so the average is our own refinement and
// only affects how stable the displayed percentage is.
constexpr uint8_t kSamplesPerRead = 8;

// Below this the rail cannot be a Li-ion cell: it means the board is running
// from USB with no battery, or the battery is absent/disconnected, so the UI
// must show "--%" rather than 0%.
constexpr uint16_t kPlausibleMinimumMillivolts = 2500;

}  // namespace

bool BatteryMonitor::begin() {
    adc_oneshot_unit_init_cfg_t unitConfig = {};
    unitConfig.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unitConfig, &unit_) != ESP_OK) {
        Serial.println("[battery] ADC1 init failed; battery percentage unavailable");
        return false;
    }

    adc_oneshot_chan_cfg_t channelConfig = {};
    channelConfig.atten = ADC_ATTEN_DB_12;
    channelConfig.bitwidth = ADC_BITWIDTH_12;
    if (adc_oneshot_config_channel(unit_, static_cast<adc_channel_t>(VM_BATTERY_ADC_CHANNEL), &channelConfig) != ESP_OK) {
        Serial.println("[battery] ADC channel config failed; battery percentage unavailable");
        return false;
    }

    // Same calibration scheme as the official example (curve fitting, 12 dB).
    adc_cali_curve_fitting_config_t calibrationConfig = {};
    calibrationConfig.unit_id = ADC_UNIT_1;
    calibrationConfig.atten = ADC_ATTEN_DB_12;
    calibrationConfig.bitwidth = ADC_BITWIDTH_12;
    calibrated_ = adc_cali_create_scheme_curve_fitting(&calibrationConfig, &calibration_) == ESP_OK;
    if (!calibrated_) {
        Serial.println("[battery] ADC calibration unavailable; using the raw 3.3 V/4096 fallback");
    }

    capable_ = true;
    // First reading immediately, so the boot screen shows a real value (or an
    // explicit "--%") instead of flickering from unknown to known.
    takeReading();
    nextSampleMs_ = millis() + VM_UI_BATTERY_INTERVAL_MS;

    Serial.printf("[battery] ADC1 channel %d (GPIO4) ready; divider ratio x%d; update every %u ms\n",
                  VM_BATTERY_ADC_CHANNEL,
                  VM_BATTERY_DIVIDER_RATIO,
                  static_cast<unsigned int>(VM_UI_BATTERY_INTERVAL_MS));
    return true;
}

void BatteryMonitor::takeReading() {
    if (!capable_) {
        return;
    }

    uint32_t accumulatedMillivolts = 0;
    uint8_t valid = 0;
    for (uint8_t i = 0; i < kSamplesPerRead; ++i) {
        int raw = 0;
        if (adc_oneshot_read(unit_, static_cast<adc_channel_t>(VM_BATTERY_ADC_CHANNEL), &raw) != ESP_OK) {
            continue;
        }
        int pinMillivolts = 0;
        if (calibrated_) {
            if (adc_cali_raw_to_voltage(calibration_, raw, &pinMillivolts) != ESP_OK) {
                continue;
            }
        } else {
            pinMillivolts = (raw * 3300) / 4096;
        }
        accumulatedMillivolts += static_cast<uint32_t>(pinMillivolts) * VM_BATTERY_DIVIDER_RATIO;
        ++valid;
    }

    if (valid == 0) {
        valid_ = false;
        millivolts_ = 0;
        return;
    }

    const uint16_t batteryMillivolts = static_cast<uint16_t>(accumulatedMillivolts / valid);
    millivolts_ = batteryMillivolts;
    belowPlausible_ = batteryMillivolts < kPlausibleMinimumMillivolts;
    valid_ = !belowPlausible_;
    if (valid_) {
        percent_ = battery_percent_for_millivolts(batteryMillivolts);
        return;
    }

    if (!reportedImplausible_) {
        reportedImplausible_ = true;
        Serial.printf("[battery] %u mV is below the plausible Li-ion minimum (%u mV); showing --%%\n",
                      static_cast<unsigned int>(batteryMillivolts),
                      static_cast<unsigned int>(kPlausibleMinimumMillivolts));
    }
}

bool BatteryMonitor::poll() {
    if (!capable_) {
        return false;
    }
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextSampleMs_) < 0) {
        return false;
    }
    nextSampleMs_ = now + VM_UI_BATTERY_INTERVAL_MS;

    const bool wasAvailable = available();
    const uint8_t previousPercent = percent_;
    takeReading();
    return wasAvailable != available() || (available() && previousPercent != percent_);
}

}  // namespace voice_memo_ui
