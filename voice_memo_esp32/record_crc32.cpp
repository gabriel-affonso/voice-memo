#include "record_crc32.h"

namespace voice_memo_firmware {
namespace {

// 256-entry table for the reflected polynomial. Built once on first use. The
// two writers that can reach this (the loop task committing a recording and,
// on recovery, the same task) are serialised by the store's volume lock, so a
// benign duplicate build of the identical table is the worst case.
struct Crc32Table {
    uint32_t entries[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256U; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
            }
            entries[i] = value;
        }
    }
};

const Crc32Table& table() {
    static const Crc32Table instance;
    return instance;
}

}  // namespace

uint32_t crc32_ieee(const uint8_t* data, size_t length, uint32_t seed) {
    if (data == nullptr) {
        return seed ^ 0xFFFFFFFFU;
    }

    const Crc32Table& crcTable = table();
    uint32_t crc = seed ^ 0xFFFFFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc = crcTable.entries[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

}  // namespace voice_memo_firmware
