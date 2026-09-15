#include "wav_format.h"

#include <cstring>

namespace voice_memo_firmware {
namespace {

void write_u32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFFU);
    p[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    p[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    p[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

void write_u16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFFU);
    p[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8U);
}

}  // namespace

void write_wav_header(
    uint8_t* out,
    uint32_t sample_rate,
    uint16_t channels,
    uint16_t bits_per_sample,
    uint32_t data_bytes
) {
    const uint32_t bytes_per_sample = bits_per_sample / 8U;
    const uint32_t byte_rate = sample_rate * channels * bytes_per_sample;
    const uint16_t block_align = static_cast<uint16_t>(channels * bytes_per_sample);

    std::memcpy(out, "RIFF", 4);
    write_u32(out + 4, 36U + data_bytes);
    std::memcpy(out + 8, "WAVE", 4);
    std::memcpy(out + 12, "fmt ", 4);
    write_u32(out + 16, 16U);
    write_u16(out + 20, 1U);
    write_u16(out + 22, channels);
    write_u32(out + 24, sample_rate);
    write_u32(out + 28, byte_rate);
    write_u16(out + 32, block_align);
    write_u16(out + 34, bits_per_sample);
    std::memcpy(out + 36, "data", 4);
    write_u32(out + 40, data_bytes);
}

bool validate_wav_header(
    const uint8_t* data,
    size_t total_bytes,
    uint32_t expected_sample_rate,
    uint16_t expected_channels,
    uint16_t expected_bits_per_sample,
    uint32_t expected_data_bytes
) {
    if (data == nullptr || total_bytes < 44U) {
        return false;
    }
    if (std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "WAVE", 4) != 0 ||
        std::memcmp(data + 12, "fmt ", 4) != 0 ||
        std::memcmp(data + 36, "data", 4) != 0) {
        return false;
    }
    if (read_u32(data + 4) != 36U + expected_data_bytes) {
        return false;
    }
    if (read_u32(data + 24) != expected_sample_rate ||
        read_u16(data + 22) != expected_channels ||
        read_u16(data + 34) != expected_bits_per_sample) {
        return false;
    }
    return read_u32(data + 40) == expected_data_bytes;
}

}  // namespace voice_memo_firmware
