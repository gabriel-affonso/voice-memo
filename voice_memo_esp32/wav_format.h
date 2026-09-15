#pragma once

#include <cstddef>
#include <cstdint>

namespace voice_memo_firmware {

void write_wav_header(
    uint8_t* out,
    uint32_t sample_rate,
    uint16_t channels,
    uint16_t bits_per_sample,
    uint32_t data_bytes
);

bool validate_wav_header(
    const uint8_t* data,
    size_t total_bytes,
    uint32_t expected_sample_rate,
    uint16_t expected_channels,
    uint16_t expected_bits_per_sample,
    uint32_t expected_data_bytes
);

}  // namespace voice_memo_firmware
