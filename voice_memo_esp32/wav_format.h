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

// Structural validation used by boot recovery, where the expected payload size
// is unknown and must be derived from what is on the card.
//
// Checks, on the first 44 bytes:
//   RIFF/WAVE/fmt /data, fmt chunk size 16, PCM (format 1), one channel,
//   16 bits, and a data chunk that exactly fills the rest of the file.
// `expected_sample_rate` is checked too (16000 Hz for this firmware).
//
// Returns false for a null/too-short buffer or any inconsistency, and reports
// the data-chunk size it found through `out_data_bytes` when it succeeds.
bool validate_wav_structure(
    const uint8_t* data,
    size_t total_bytes,
    uint32_t expected_sample_rate,
    uint16_t expected_channels,
    uint16_t expected_bits_per_sample,
    uint32_t* out_data_bytes
);

// Two-byte alignment sanity helper: a mono 16-bit payload must be a whole
// number of samples, i.e. an even number of bytes.
constexpr bool wav_payload_is_aligned(uint32_t data_bytes) {
    return (data_bytes % 2U) == 0U;
}

}  // namespace voice_memo_firmware
