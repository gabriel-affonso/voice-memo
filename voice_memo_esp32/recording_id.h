#pragma once

#include <cstdint>
#include <string>

namespace voice_memo_firmware {

// Generates the idempotency key shared with the ingress.
//
// The id is minted exactly once per recording and reused for every retry, so
// the ingress can deduplicate a replay. `nonce` exists so the id does not
// depend only on values that reset at reboot: `sequence_` restarts at 1 and
// `timestamp_ms` is boot-relative, so without a nonce the first recording after
// two boots could collide, and the ingress would answer "already_known" for a
// brand-new note. The firmware passes a hardware random value.
class RecordingIdGenerator {
public:
    std::string generate(uint32_t timestamp_ms, uint32_t nonce);

private:
    uint32_t sequence_ = 0;
};

}  // namespace voice_memo_firmware
