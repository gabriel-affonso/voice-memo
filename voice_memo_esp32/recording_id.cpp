#include "recording_id.h"

#include <cstdio>

namespace voice_memo_firmware {

std::string RecordingIdGenerator::generate(uint32_t timestamp_ms) {
    ++sequence_;
    char buffer[40];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "rec-%08lX-%08lX",
        static_cast<unsigned long>(sequence_),
        static_cast<unsigned long>(timestamp_ms)
    );
    return std::string(buffer);
}

}  // namespace voice_memo_firmware
