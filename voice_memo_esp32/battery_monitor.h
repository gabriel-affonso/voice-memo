#pragma once

// Battery monitor for the Waveshare ESP32-S3-Touch-ePaper-1.54.
//
// Hardware mapping, confirmed against the official Waveshare example for this
// exact product (02_Example/Arduino/01_ADC_Test/adc_bsp.cpp):
//
//   * ADC unit  ADC1
//   * channel   ADC_CHANNEL_3  (GPIO4 on the ESP32-S3)
//   * atten     ADC_ATTEN_DB_12, 12 bit
//   * the official example applies `* 2` because the board divides the battery
//     rail by 2 before the ADC pin
//
// The voltage -> percentage curve is an approximation and is documented in
// battery_level.h; this board has no fuel gauge.
//
// The monitor is sampled at most every VM_UI_BATTERY_INTERVAL_MS, so the UI can
// call poll() every loop() iteration without hammering the ADC.
//
// `available()` is deliberately false both when the ADC cannot be configured and
// when the measured rail is below any plausible Li-ion voltage (USB powered
// board, battery absent, or the "-EN" model without a battery). The UI then
// shows "--%" instead of a made-up "0%".

#include <cstdint>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>

#include "battery_level.h"

namespace voice_memo_ui {

class BatteryMonitor {
public:
    // Configures the ADC and takes one reading. Returns false when the ADC
    // could not be set up.
    bool begin();

    // True when the last reading is a plausible battery voltage.
    bool available() const { return capable_ && valid_; }

    // Samples when the refresh interval has elapsed. Returns true when the
    // displayed percentage changed.
    bool poll();

    // Calibrated battery voltage in millivolts of the last plausible reading.
    uint16_t millivolts() const { return millivolts_; }

    // Last computed percentage (meaningful only while available()).
    uint8_t percent() const { return percent_; }

    // Raw flag: the rail was below the plausible minimum on the last reading.
    bool belowPlausibleVoltage() const { return belowPlausible_; }

private:
    void takeReading();

    adc_oneshot_unit_handle_t unit_ = nullptr;
    adc_cali_handle_t calibration_ = nullptr;
    bool calibrated_ = false;
    bool capable_ = false;
    bool valid_ = false;
    bool belowPlausible_ = false;
    bool reportedImplausible_ = false;
    uint16_t millivolts_ = 0;
    uint8_t percent_ = 0;
    uint32_t nextSampleMs_ = 0;
};

}  // namespace voice_memo_ui
