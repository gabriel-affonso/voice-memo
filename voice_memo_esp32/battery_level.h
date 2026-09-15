#pragma once

// Approximate single-cell Li-ion state-of-charge mapping.
//
// What is official and what is not
// --------------------------------
// * Official (Waveshare example 01_ADC_Test of this exact product): the battery
//   rail is read on ADC1 channel 3 (GPIO4) with 12 dB attenuation and the
//   curve-fitting calibration scheme, and the measured voltage is doubled
//   because the board has a /2 divider. See battery_monitor.cpp.
// * Not from any datasheet: the voltage -> percent curve below. The board has
//   no fuel gauge, so the percentage is a documented approximation of a Li-ion
//   discharge curve, never a claim of accuracy.
//
// No Arduino dependency: tests/ui_model_test.cpp checks the clamp and the
// monotonic behaviour.

#include <cstdint>

namespace voice_memo_ui {

constexpr uint16_t kBatteryPercentUnknown = 0xFFFF;

// Voltage/percent points of a typical single-cell Li-ion discharge curve,
// highest first. Linear interpolation between neighbours, clamped outside.
struct BatteryPoint {
    uint16_t millivolts;
    uint8_t percent;
};

constexpr BatteryPoint kBatteryCurve[] = {
    {4200, 100},
    {4100, 92},
    {4000, 85},
    {3900, 75},
    {3800, 60},
    {3700, 45},
    {3600, 25},
    {3500, 10},
    {3400, 3},
    {3300, 0},
};

constexpr uint8_t kBatteryCurvePoints =
    static_cast<uint8_t>(sizeof(kBatteryCurve) / sizeof(kBatteryCurve[0]));

// Percentage for a battery voltage in millivolts. Always 0..100.
inline uint8_t battery_percent_for_millivolts(uint16_t millivolts) {
    if (millivolts >= kBatteryCurve[0].millivolts) {
        return 100;
    }
    const uint16_t last = static_cast<uint16_t>(kBatteryCurvePoints - 1);
    if (millivolts <= kBatteryCurve[last].millivolts) {
        return 0;
    }
    for (uint8_t i = 0; i + 1 < kBatteryCurvePoints; ++i) {
        const BatteryPoint high = kBatteryCurve[i];
        const BatteryPoint low = kBatteryCurve[i + 1];
        if (millivolts <= high.millivolts && millivolts >= low.millivolts) {
            const uint16_t span = static_cast<uint16_t>(high.millivolts - low.millivolts);
            const uint16_t offset = static_cast<uint16_t>(millivolts - low.millivolts);
            const uint16_t delta = static_cast<uint16_t>(high.percent - low.percent);
            return static_cast<uint8_t>(low.percent + (offset * delta) / span);
        }
    }
    return 0;
}

}  // namespace voice_memo_ui
