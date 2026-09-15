#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

namespace voice_memo_firmware {

inline constexpr size_t wav_header_bytes() {
    return static_cast<size_t>(VM_WAV_HEADER_BYTES);
}

inline constexpr size_t payload_bytes_per_second() {
    return static_cast<size_t>(VM_BYTES_PER_SECOND);
}

inline constexpr size_t max_payload_bytes() {
    return static_cast<size_t>(VM_MAX_PAYLOAD_BYTES);
}

inline constexpr size_t max_wav_bytes() {
    return wav_header_bytes() + max_payload_bytes();
}

inline constexpr uint32_t max_samples() {
    return static_cast<uint32_t>(VM_SAMPLE_RATE) * VM_MAX_RECORDING_SECONDS;
}

inline constexpr uint32_t duration_ms_for_samples(uint32_t samples) {
    return (samples * 1000U) / static_cast<uint32_t>(VM_SAMPLE_RATE);
}

}  // namespace voice_memo_firmware
