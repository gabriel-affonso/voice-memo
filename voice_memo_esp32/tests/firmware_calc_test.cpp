// Host-side test for the mono sample budget and the WAV duration arithmetic.
//
// firmware_calc.h and wav_format.cpp have no Arduino/FreeRTOS/I2S dependency,
// so the two quantities that were wrong while the RX captured two slots per
// frame can be pinned here without a board:
//
//   * sampleCount_ is now a count of real 16 kHz mono samples, so
//     duration_ms_for_samples() is elapsed wall-clock time, and
//   * the 45 s cap (VM_MAX_RECORDING_SECONDS) still corresponds to 45 s of
//     real audio, not 22.5 s.
//
//     c++ -std=c++11 -Wall -Wextra -o /tmp/firmware_calc_test \
//         tests/firmware_calc_test.cpp wav_format.cpp
//     /tmp/firmware_calc_test
//
// This directory is not compiled into the firmware: Arduino only builds the
// sketch root and src/, so tests/ is ignored by arduino-cli.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../config.h"
#include "../firmware_calc.h"
#include "../wav_format.h"

namespace {

int g_failures = 0;

void checkU32(const char* label, uint32_t actual, uint32_t expected) {
    if (actual == expected) {
        std::printf("PASS %s = %u\n", label, static_cast<unsigned int>(actual));
        return;
    }
    std::printf("FAIL %s: expected %u, got %u\n",
                label,
                static_cast<unsigned int>(expected),
                static_cast<unsigned int>(actual));
    ++g_failures;
}

void checkTrue(const char* label, bool value) {
    if (value) {
        std::printf("PASS %s\n", label);
        return;
    }
    std::printf("FAIL %s\n", label);
    ++g_failures;
}

// Second of audio the WAV header advertises for a payload of `dataBytes`.
double wavDurationSeconds(uint32_t sampleRate, uint16_t channels, uint16_t bits, uint32_t dataBytes) {
    uint8_t header[44] = {};
    voice_memo_firmware::write_wav_header(header, sampleRate, channels, bits, dataBytes);
    const uint32_t byteRate =
        static_cast<uint32_t>(header[28]) |
        (static_cast<uint32_t>(header[29]) << 8U) |
        (static_cast<uint32_t>(header[30]) << 16U) |
        (static_cast<uint32_t>(header[31]) << 24U);
    return byteRate == 0 ? 0.0 : static_cast<double>(dataBytes) / static_cast<double>(byteRate);
}

}  // namespace

int main() {
    using namespace voice_memo_firmware;

    std::printf("== capture format ==\n");
    checkU32("VM_SAMPLE_RATE", VM_SAMPLE_RATE, 16000);
    checkU32("VM_CHANNELS", VM_CHANNELS, 1);
    checkU32("VM_BITS_PER_SAMPLE", VM_BITS_PER_SAMPLE, 16);
    checkU32("VM_BYTES_PER_SAMPLE", VM_BYTES_PER_SAMPLE, 2);
    checkU32("payload bytes per second", payload_bytes_per_second(), 32000);

    std::printf("\n== duration_ms tracks real mono samples ==\n");
    // The regression: while the RX returned two slots per frame, a 9 s press
    // produced 18 s worth of sampleCount_ and reported duration_ms = 18000.
    checkU32("0 samples", duration_ms_for_samples(0), 0);
    checkU32("1 s of samples", duration_ms_for_samples(VM_SAMPLE_RATE), 1000);
    checkU32("10 s of samples (160000)", duration_ms_for_samples(160000), 10000);
    checkU32("100 ms of samples (1600)", duration_ms_for_samples(1600), 100);
    checkU32("max_samples() duration", duration_ms_for_samples(max_samples()), 45000);

    std::printf("\n== 45 s cap is 45 s of real audio ==\n");
    checkU32("max_samples()", max_samples(), 720000);
    checkU32("VM_MAX_RECORDING_SECONDS", VM_MAX_RECORDING_SECONDS, 45);
    checkU32("max_samples() / VM_SAMPLE_RATE", max_samples() / VM_SAMPLE_RATE, 45);
    checkU32("max payload bytes", max_payload_bytes(), 1440000);
    checkTrue("max payload == 45 * bytes per second",
              max_payload_bytes() == payload_bytes_per_second() * VM_MAX_RECORDING_SECONDS);
    checkTrue("max payload holds exactly max_samples() mono samples",
              max_payload_bytes() == static_cast<size_t>(max_samples()) * VM_BYTES_PER_SAMPLE);
    checkU32("max wav bytes", max_wav_bytes(), 1440044);

    std::printf("\n== WAV header advertises the real duration ==\n");
    const uint32_t tenSecondsPayload = static_cast<uint32_t>(VM_SAMPLE_RATE) * 10U * VM_BYTES_PER_SAMPLE;
    checkU32("10 s payload bytes", tenSecondsPayload, 320000);
    checkTrue("10 s recording advertises 10.000 s",
              wavDurationSeconds(VM_SAMPLE_RATE, VM_CHANNELS, VM_BITS_PER_SAMPLE, tenSecondsPayload) > 9.999 &&
              wavDurationSeconds(VM_SAMPLE_RATE, VM_CHANNELS, VM_BITS_PER_SAMPLE, tenSecondsPayload) < 10.001);
    checkTrue("45 s recording advertises 45.000 s",
              wavDurationSeconds(VM_SAMPLE_RATE, VM_CHANNELS, VM_BITS_PER_SAMPLE,
                                 static_cast<uint32_t>(max_payload_bytes())) > 44.999 &&
              wavDurationSeconds(VM_SAMPLE_RATE, VM_CHANNELS, VM_BITS_PER_SAMPLE,
                                 static_cast<uint32_t>(max_payload_bytes())) < 45.001);

    uint8_t header[44] = {};
    write_wav_header(header, VM_SAMPLE_RATE, VM_CHANNELS, VM_BITS_PER_SAMPLE,
                     static_cast<uint32_t>(max_payload_bytes()));
    checkTrue("header validates against the 45 s payload",
              validate_wav_header(header, sizeof(header), VM_SAMPLE_RATE, VM_CHANNELS,
                                  VM_BITS_PER_SAMPLE, static_cast<uint32_t>(max_payload_bytes())));
    checkTrue("header is mono PCM 16-bit at 16 kHz",
              header[20] == 1 && header[21] == 0 &&
              header[22] == 1 && header[23] == 0 &&
              header[34] == 16 && header[35] == 0);

    std::printf("\n== I2S RX slot selection ==\n");
    checkU32("exactly one RX slot is selected",
              static_cast<uint32_t>(VM_I2S_RX_SLOT_LEFT) + static_cast<uint32_t>(VM_I2S_RX_SLOT_RIGHT), 1);
    checkU32("the ES8311 ADC slot (LEFT) is the default", VM_I2S_RX_SLOT_LEFT, 1);

    if (g_failures == 0) {
        std::printf("\nall firmware_calc tests passed\n");
        return 0;
    }
    std::printf("\n%d firmware_calc test(s) FAILED\n", g_failures);
    return 1;
}
