#include "recording_id.h"

#include <cstdio>

namespace voice_memo_firmware {

std::string RecordingIdGenerator::generate(uint32_t timestamp_ms, uint32_t nonce) {
    ++sequence_;
    char buffer[40];
    // Same shape as before (rec-<sequence>-<component>), so nothing that parses
    // or displays a recording_id changes. The second component mixes in a
    // caller-supplied random nonce, which removes the cross-boot collision that
    // a pure (sequence, millis) pair had.
    std::snprintf(
        buffer,
        sizeof(buffer),
        "rec-%08lX-%08lX",
        static_cast<unsigned long>(sequence_),
        static_cast<unsigned long>(timestamp_ms ^ nonce)
    );
    return std::string(buffer);
}

}  // namespace voice_memo_firmware
