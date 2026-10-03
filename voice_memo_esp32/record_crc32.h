#pragma once

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) over a byte range.
//
// Used to record the integrity of a persisted WAV in its metadata: the CRC is
// computed once when the recording is committed to the card, stored next to it,
// and can be re-checked later without re-deriving anything. Arduino free, so it
// is covered by tests/recording_store_test.cpp.

#include <cstddef>
#include <cstdint>

namespace voice_memo_firmware {

// Standard CRC-32. `seed` allows continuing a computation across chunks; the
// usual call is crc32_ieee(data, length) and the usual chained call is
// crc32_ieee(next, nextLength, crc32_ieee(first, firstLength)).
uint32_t crc32_ieee(const uint8_t* data, size_t length, uint32_t seed = 0);

}  // namespace voice_memo_firmware
