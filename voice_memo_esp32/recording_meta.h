#pragma once

// Persistent metadata for one recording, stored as a small flat JSON file next
// to the WAV (<recording_id>.json).
//
// Why a sidecar at all
// --------------------
// A file name can carry the recording id, but not the tag, the duration, the
// creation time, the CRC or the retry count. Losing those on reboot would mean
// losing information the backend contract already relies on (duration_ms,
// recording_id), so every recording that reaches the card gets a sidecar.
//
// Why not SQLite / a binary blob
// ------------------------------
// The card is 4-64 GB of FAT32 and the queue is bounded to a few dozen items.
// A human-readable, trivially recoverable, one-parse JSON object is the cheapest
// thing that survives a power cut; SQLite would add flash, RAM and failure modes
// for no benefit at this scale.
//
// The parser is deliberately small and strict: it accepts exactly one flat
// object of string/number values and rejects anything else, so a truncated or
// corrupt sidecar is detected instead of being half-applied. Arduino free.

#include <cstdint>
#include <string>

namespace voice_memo_firmware {

// Bump when the on-card schema changes incompatibly. Readers accept anything
// <= kRecordingMetaSchemaVersion; newer files are treated as unreadable rather
// than guessed at.
constexpr uint32_t kRecordingMetaSchemaVersion = 1;

struct RecordingMeta {
    uint32_t schema_version = kRecordingMetaSchemaVersion;
    // Idempotency key shared with the ingress. Never regenerated on retry.
    std::string recording_id;
    // Device that produced the audio (same value as the multipart field).
    std::string device_id;
    // voice_memo_firmware::VoiceTag index, the tag frozen at startRecording().
    uint32_t tag_index = 0;
    // Duration advertised to the ingress; derived from the captured samples.
    uint32_t duration_ms = 0;
    // Size of the complete WAV (44-byte header + PCM payload).
    uint32_t wav_bytes = 0;
    // Capture format, stored so recovery can re-validate the header.
    uint32_t sample_rate = 0;
    // Unix UTC seconds when the recording was committed; 0 when no trustworthy
    // clock was available (offline boot with a never-set RTC).
    uint32_t created_utc = 0;
    // CRC-32 of the complete WAV. `has_crc` distinguishes "not computed" from a
    // legitimate CRC of 0.
    uint32_t crc32 = 0;
    bool has_crc = false;
    // Failed attempts so far; diagnostic only, never used for correctness.
    uint32_t attempt_count = 0;
    // Persistent FIFO order. Assigned by the store and always > 0 for items it
    // committed; 0 means "unknown" (a WAV recovered without its sidecar) and
    // such items sort after the numbered ones.
    uint32_t sequence = 0;
};

// Canonical single-object JSON, one key per line, keys in a fixed order.
std::string recording_meta_to_json(const RecordingMeta& meta);

// Strict parse. Returns false when the text is not a flat object, when a value
// has the wrong type, or when a required field (recording_id, wav_bytes) is
// missing. On failure `out` is left untouched.
bool recording_meta_from_json(const std::string& json, RecordingMeta& out);

}  // namespace voice_memo_firmware
