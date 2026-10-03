#pragma once

// Semantic layer above the SD/filesystem: everything the firmware needs to
// treat the card as a persistent, recoverable queue of recordings.
//
// Responsibilities
// ----------------
//   * own the directory layout (never exposed to the rest of the firmware);
//   * commit a finalized WAV + metadata atomically (tmp -> pending);
//   * rebuild the queue at boot from whatever is on the card, quarantining
//     anything that does not validate;
//   * keep an in-memory, oldest-first cache of the queue so the loop never
//     scans the card (FASE 22);
//   * marshal an attempt's lifecycle: pending -> uploading -> pending/removed;
//   * enforce free-space and queue-length backpressure.
//
// RecordingApp only sees ids, metadata and outcomes; it never builds a path and
// never calls SD_MMC. Arduino free, so the whole policy is host-tested with an
// in-memory volume.
//
// Concurrency (FASE 17)
// ---------------------
// The volume lock is taken only for filesystem work and released before any
// HTTP. openRead() returns a handle whose read()/close() each take the lock for
// one short operation, so a background upload never blocks the main loop for
// longer than a chunk read.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "recording_meta.h"
#include "vm_fs.h"

namespace voice_memo_firmware {

enum class SaveOutcome {
    Ok,
    // Backpressure: queue full, or writing would drop free space below the
    // reserve. Nothing on the card is touched and no old recording is deleted.
    StorageFull,
    // An I/O failure (card removed, write/rename failed). The caller should
    // treat the volume as unhealthy and fall back to the volatile path.
    IoError,
    NotMounted,
    InvalidId,
    DuplicateId,
};

// One queued recording. `in_uploading` means the WAV currently sits in
// uploading/ because an attempt was interrupted (or is in flight); it is still
// a valid, not-yet-delivered recording and is retried from wherever it lives.
struct StoreItem {
    RecordingMeta meta;
    bool in_uploading = false;
    // Last-write time of the WAV as reported by the volume; used only to order
    // items whose persistent sequence number is unknown.
    uint32_t mod_time = 0;
};

// Optional diagnostics hook, one line per call. Default is silent so the host
// test can run without an Arduino. The firmware points it at Serial.
typedef void (*StoreLogFn)(const char* line);

class RecordingStore {
public:
    RecordingStore(
        FileSystem& fs,
        uint32_t max_pending,
        uint64_t min_free_bytes,
        bool keep_sent,
        bool verify_crc_on_boot
    );

    void setLogSink(StoreLogFn sink) { log_ = sink; }

    // Mounts the volume (idempotent), creates the layout and rebuilds the queue
    // from the card. Safe to call at boot and again after a remount.
    bool begin();

    // True once the layout exists and the queue cache is valid.
    bool available() const { return available_; }
    // False after an I/O error until a remount succeeds.
    bool healthy() const { return healthy_; }
    void markUnhealthy();
    // Deadline-based remount after a failure. Must only be called when no
    // upload handle is open (RecordingApp guards this with its job flag).
    // Returns true when a remount was attempted and succeeded.
    bool serviceMount(uint32_t now_ms);

    // Clean shutdown of the volume: unmounts the card so no file handle and no
    // FAT metadata can survive the power cut. Used only by the shutdown path,
    // which is why it deliberately does NOT invalidate the in-memory queue
    // cache or the "was available" state: the process is about to end, and
    // clearing them would only make the final UI wrong. A later begin() mounts
    // again and rebuilds the queue from the card as usual.
    void closeVolume();

    bool ensureDirectories();

    uint32_t pendingCount() const { return static_cast<uint32_t>(items_.size()); }
    bool storageFull() const { return storage_full_; }
    void clearStorageFull() { storage_full_ = false; }

    // Atomic commit. On Ok the recording is durable and the queue grew by one;
    // on any other value nothing observable changed.
    SaveOutcome savePending(const RecordingMeta& meta, const uint8_t* wav, size_t wavBytes);

    // Oldest not-yet-delivered item (pending or uploading), or nullptr.
    const StoreItem* oldestPending() const;
    // Oldest item with fewer than `maxAttempts` failed uploads, or nullptr when
    // every pending item has reached the cap. Lets the queue move past a
    // recording the ingress keeps rejecting instead of starving the rest.
    const StoreItem* nextPending(uint32_t maxAttempts) const;
    const StoreItem* find(const std::string& recordingId) const;

    // Drops an item whose WAV has provably disappeared from both directories,
    // so a file removed behind the firmware's back cannot wedge the queue head.
    // Never guesses: returns false while the volume is unmounted or unhealthy,
    // and never moves or deletes anything on the card.
    bool dropIfMissing(const std::string& recordingId);

    // Attempt lifecycle. All return false only when the id is unknown or the
    // volume refused the operation; the in-memory queue is kept consistent with
    // the card whenever possible.
    bool markUploading(const std::string& recordingId);
    bool markPendingAgain(const std::string& recordingId, uint32_t attemptCount);
    bool markUploaded(const std::string& recordingId);
    bool markCorrupt(const std::string& recordingId);

    // Streaming read of an item's WAV. Ownership passes to the caller, which
    // must close() and delete the handle. nullptr when it cannot be opened.
    FileHandle* openRead(const std::string& recordingId);

    uint64_t totalBytes();
    uint64_t freeBytes();

    // Boot diagnostics.
    uint32_t recoveredCount() const { return recovered_count_; }
    uint32_t corruptCount() const { return corrupt_count_; }
    uint32_t discardedTmpCount() const { return discarded_tmp_count_; }

private:
    bool recoverOnBoot();
    bool loadMeta(const std::string& path, RecordingMeta& out) const;
    // Tries the item's own directory first and the other one second, so a
    // half-moved pair (WAV in uploading/, sidecar still in pending/) is read
    // correctly instead of being treated as metadata-less.
    bool loadMetaFor(const std::string& recordingId, bool inUploading, RecordingMeta& out) const;
    bool readWavHeader(const std::string& path, uint8_t* out44) const;
    bool synthesizeMeta(
        const std::string& recordingId,
        const std::string& wavPath,
        uint32_t wavBytes,
        RecordingMeta& out
    ) const;
    bool verifyWav(const std::string& path, uint32_t expectedBytes, RecordingMeta& meta) const;
    bool computeFileCrc(const std::string& path, uint32_t* outCrc) const;
    bool moveToCorrupt(const std::string& recordingId, bool inUploading, bool withMeta);
    bool rewriteMeta(const StoreItem& item);
    void logLine(const char* line) const;

    FileSystem& fs_;
    uint32_t max_pending_;
    uint64_t min_free_bytes_;
    bool keep_sent_;
    bool verify_crc_on_boot_;
    StoreLogFn log_ = nullptr;

    std::vector<StoreItem> items_;
    uint32_t next_sequence_ = 1;
    uint32_t next_mount_attempt_ms_ = 0;
    bool available_ = false;
    bool healthy_ = false;
    bool storage_full_ = false;
    // "card not present" is a single line per absence, not one per remount try.
    bool mount_notice_logged_ = false;

    uint32_t recovered_count_ = 0;
    uint32_t corrupt_count_ = 0;
    uint32_t discarded_tmp_count_ = 0;
};

}  // namespace voice_memo_firmware
