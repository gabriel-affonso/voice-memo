#pragma once

#include <cstdint>
#include <string>

namespace voice_memo_firmware {

class RecordingIdGenerator {
public:
    std::string generate(uint32_t timestamp_ms);

private:
    uint32_t sequence_ = 0;
};

}  // namespace voice_memo_firmware
