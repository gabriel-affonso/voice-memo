#include "recording_store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "firmware_calc.h"
#include "record_crc32.h"
#include "recording_paths.h"
#include "wav_format.h"

namespace voice_memo_firmware {
namespace {

using store_paths::kCorruptDirName;
using store_paths::kPendingDirName;
using store_paths::kSentDirName;
using store_paths::kTmpDirName;
using store_paths::kUploadingDirName;

// Chunk used whenever a whole file is hashed or validated. Small enough to live
// on the caller's stack, large enough to keep the SD transaction count low.
constexpr size_t kScanChunkBytes = 2048;

// Serialises one FileHandle behind the store's volume lock. The size is taken
// once at open time, so size() never touches the card and the lock is only held
// for the duration of a single read()/close().
class LockedFileHandle : public FileHandle {
public:
    LockedFileHandle(FileSystem& fs, FileHandle* inner, size_t size)
        : fs_(fs), inner_(inner), size_(size) {}

    ~LockedFileHandle() { close(); }

    size_t size() const override { return size_; }

    size_t read(uint8_t* dest, size_t maxBytes) override {
        if (inner_ == nullptr) {
            return 0;
        }
        ScopedVolumeLock lock(fs_);
        return inner_->read(dest, maxBytes);
    }

    void close() override {
        if (inner_ == nullptr) {
            return;
        }
        {
            ScopedVolumeLock lock(fs_);
            inner_->close();
        }
        delete inner_;
        inner_ = nullptr;
        size_ = 0;
    }

private:
    FileSystem& fs_;
    FileHandle* inner_;
    size_t size_;
};

bool containsId(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Oldest first. The persistent sequence is authoritative; items recovered
// without a sidecar (sequence 0) fall to the end and are ordered by their
// on-card modification time and then by name, so a legacy WAV is never lost
// behind a numbered one.
bool olderFirst(const StoreItem& a, const StoreItem& b) {
    const bool a_known = a.meta.sequence != 0;
    const bool b_known = b.meta.sequence != 0;
    if (a_known != b_known) {
        return a_known;
    }
    if (a_known && b_known && a.meta.sequence != b.meta.sequence) {
        return a.meta.sequence < b.meta.sequence;
    }
    if (a.mod_time != b.mod_time) {
        return a.mod_time < b.mod_time;
    }
    if (a.meta.created_utc != b.meta.created_utc) {
        return a.meta.created_utc < b.meta.created_utc;
    }
    return a.meta.recording_id < b.meta.recording_id;
}

const char* saveOutcomeName(SaveOutcome outcome) {
    switch (outcome) {
        case SaveOutcome::Ok:
            return "ok";
        case SaveOutcome::StorageFull:
            return "storage_full";
        case SaveOutcome::IoError:
            return "io_error";
        case SaveOutcome::NotMounted:
            return "not_mounted";
        case SaveOutcome::InvalidId:
            return "invalid_id";
        case SaveOutcome::DuplicateId:
            return "duplicate_id";
    }
    return "unknown";
}

}  // namespace

RecordingStore::RecordingStore(
    FileSystem& fs,
    uint32_t max_pending,
    uint64_t min_free_bytes,
    bool keep_sent,
    bool verify_crc_on_boot
)
    : fs_(fs),
      max_pending_(max_pending == 0 ? 1U : max_pending),
      min_free_bytes_(min_free_bytes),
      keep_sent_(keep_sent),
      verify_crc_on_boot_(verify_crc_on_boot) {}

void RecordingStore::logLine(const char* line) const {
    if (log_ != nullptr && line != nullptr) {
        log_(line);
    }
}

bool RecordingStore::ensureDirectories() {
    ScopedVolumeLock lock(fs_);
    const std::string paths[] = {
        store_paths::root(),
        store_paths::directory(store_paths::kPendingDirName),
        store_paths::directory(store_paths::kUploadingDirName),
        store_paths::directory(store_paths::kTmpDirName),
        store_paths::directory(store_paths::kCorruptDirName),
        store_paths::directory(store_paths::kSentDirName),
    };
    for (const std::string& path : paths) {
        if (fs_.isDirectory(path)) {
            continue;
        }
        if (!fs_.makeDirectory(path)) {
            // mkdir on an existing directory can report failure on some FAT
            // implementations; accept it when the directory now exists.
            if (!fs_.isDirectory(path)) {
                char line[160];
                std::snprintf(line, sizeof(line), "[sd] cannot create %s", path.c_str());
                logLine(line);
                return false;
            }
        }
    }
    return true;
}

bool RecordingStore::begin() {
    available_ = false;
    healthy_ = false;

    if (!fs_.mount()) {
        // Reported once per absence, not on every 30 s remount attempt.
        if (!mount_notice_logged_) {
            logLine("[sd] card not present");
            mount_notice_logged_ = true;
        }
        return false;
    }
    mount_notice_logged_ = false;
    if (!ensureDirectories()) {
        logLine("[sd] directory layout unavailable");
        return false;
    }
    if (!recoverOnBoot()) {
        logLine("[sd] recovery failed");
        return false;
    }

    available_ = true;
    healthy_ = true;
    storage_full_ = false;
    next_mount_attempt_ms_ = 0;
    return true;
}

void RecordingStore::markUnhealthy() {
    if (!healthy_) {
        return;
    }
    healthy_ = false;
    logLine("[sd] volume marked unhealthy; will retry mount");
}

void RecordingStore::closeVolume() {
    if (!available_) {
        return;
    }
    // Deliberate: available_/healthy_/items_ are left describing the queue as it
    // was, because the only caller is the shutdown sequence and the process ends
    // immediately afterwards. A later begin() remounts and rebuilds the queue.
    fs_.unmount();
    logLine("[sd] volume released for shutdown");
}

bool RecordingStore::serviceMount(uint32_t now_ms) {
    if (available_ && healthy_) {
        return false;
    }
    if (next_mount_attempt_ms_ != 0 &&
        static_cast<int32_t>(now_ms - next_mount_attempt_ms_) < 0) {
        return false;
    }
    next_mount_attempt_ms_ = now_ms + VM_SD_RETRY_MOUNT_MS;

    // A clean unmount first: the card may have been removed, in which case the
    // old mount is stale and must not be reused.
    fs_.unmount();
    if (!begin()) {
        available_ = false;
        healthy_ = false;
        return false;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "[sd] remounted; pending=%lu",
                  static_cast<unsigned long>(items_.size()));
    logLine(line);
    return true;
}

bool RecordingStore::loadMeta(const std::string& path, RecordingMeta& out) const {
    std::vector<uint8_t> bytes;
    if (!fs_.readFile(path, bytes) || bytes.empty()) {
        return false;
    }
    const std::string text(bytes.begin(), bytes.end());
    return recording_meta_from_json(text, out);
}

bool RecordingStore::loadMetaFor(
    const std::string& recordingId,
    bool inUploading,
    RecordingMeta& out
) const {
    const char* first = inUploading ? kUploadingDirName : kPendingDirName;
    const char* second = inUploading ? kPendingDirName : kUploadingDirName;
    if (loadMeta(store_paths::metaIn(first, recordingId), out)) {
        return true;
    }
    return loadMeta(store_paths::metaIn(second, recordingId), out);
}

bool RecordingStore::readWavHeader(const std::string& path, uint8_t* out44) const {
    // The volume lock is taken here too: recovery runs before the worker exists
    // and remounts are gated on jobInFlight_, but the invariant "every access
    // goes through lockVolume()" must hold without exceptions.
    ScopedVolumeLock lock(fs_);
    FileHandle* handle = fs_.openRead(path);
    if (handle == nullptr) {
        return false;
    }
    const size_t got = handle->read(out44, VM_WAV_HEADER_BYTES);
    handle->close();
    delete handle;
    return got >= VM_WAV_HEADER_BYTES;
}

bool RecordingStore::verifyWav(const std::string& path, uint32_t expectedBytes, RecordingMeta& meta) const {
    const size_t actual = fs_.fileSize(path);
    if (actual < VM_WAV_HEADER_BYTES) {
        return false;
    }

    uint8_t header[VM_WAV_HEADER_BYTES];
    if (!readWavHeader(path, header)) {
        return false;
    }

    uint32_t dataBytes = 0;
    if (!validate_wav_structure(
            header,
            actual,
            static_cast<uint32_t>(VM_SAMPLE_RATE),
            static_cast<uint16_t>(VM_CHANNELS),
            static_cast<uint16_t>(VM_BITS_PER_SAMPLE),
            &dataBytes)) {
        return false;
    }

    // The advertised size and the size on the card must agree; a mismatch is
    // corrected from the card (the file is the ground truth) instead of
    // discarding audio.
    const uint32_t fileBytes = static_cast<uint32_t>(actual);
    if (meta.wav_bytes != fileBytes) {
        meta.wav_bytes = fileBytes;
    }
    if (expectedBytes != 0 && expectedBytes != fileBytes) {
        meta.wav_bytes = fileBytes;
    }
    if (meta.duration_ms == 0) {
        meta.duration_ms = duration_ms_for_samples(dataBytes / VM_BYTES_PER_SAMPLE);
    }
    if (meta.sample_rate == 0) {
        meta.sample_rate = static_cast<uint32_t>(VM_SAMPLE_RATE);
    }

    if (verify_crc_on_boot_ && meta.has_crc) {
        uint32_t crc = 0;
        if (!computeFileCrc(path, &crc)) {
            return false;
        }
        if (crc != meta.crc32) {
            return false;
        }
    }
    return true;
}

bool RecordingStore::computeFileCrc(const std::string& path, uint32_t* outCrc) const {
    ScopedVolumeLock lock(fs_);
    FileHandle* handle = fs_.openRead(path);
    if (handle == nullptr) {
        return false;
    }
    uint8_t chunk[kScanChunkBytes];
    uint32_t crc = 0;
    bool first = true;
    for (;;) {
        const size_t got = handle->read(chunk, sizeof(chunk));
        if (got == 0) {
            break;
        }
        crc = crc32_ieee(chunk, got, first ? 0U : crc);
        first = false;
    }
    handle->close();
    delete handle;
    if (outCrc != nullptr) {
        *outCrc = crc;
    }
    return true;
}

bool RecordingStore::synthesizeMeta(
    const std::string& recordingId,
    const std::string& wavPath,
    uint32_t wavBytes,
    RecordingMeta& out
) const {
    if (wavBytes < VM_WAV_HEADER_BYTES) {
        return false;
    }
    uint8_t header[VM_WAV_HEADER_BYTES];
    if (!readWavHeader(wavPath, header)) {
        return false;
    }
    uint32_t dataBytes = 0;
    if (!validate_wav_structure(
            header,
            wavBytes,
            static_cast<uint32_t>(VM_SAMPLE_RATE),
            static_cast<uint16_t>(VM_CHANNELS),
            static_cast<uint16_t>(VM_BITS_PER_SAMPLE),
            &dataBytes)) {
        return false;
    }

    out = RecordingMeta();
    out.recording_id = recordingId;
    out.wav_bytes = wavBytes;
    out.sample_rate = static_cast<uint32_t>(VM_SAMPLE_RATE);
    out.duration_ms = duration_ms_for_samples(dataBytes / VM_BYTES_PER_SAMPLE);
    out.tag_index = 0;
    out.sequence = 0;
    // The sidecar is missing, so this is the one place where hashing the audio
    // is worth it: it gives the rebuilt sidecar a truthful CRC instead of a
    // sentinel that VM_SD_VERIFY_CRC_ON_BOOT would later reject.
    out.has_crc = computeFileCrc(wavPath, &out.crc32);
    return true;
}

bool RecordingStore::moveToCorrupt(const std::string& recordingId, bool inUploading, bool withMeta) {
    const char* sourceDir = inUploading ? kUploadingDirName : kPendingDirName;
    const char* otherDir = inUploading ? kPendingDirName : kUploadingDirName;
    bool moved = false;

    const std::string sourceWav = store_paths::wavIn(sourceDir, recordingId);
    const std::string targetWav = store_paths::wavIn(kCorruptDirName, recordingId);
    if (fs_.exists(sourceWav)) {
        fs_.remove(targetWav);
        moved = fs_.rename(sourceWav, targetWav) || moved;
    }

    if (withMeta) {
        // The sidecar may have stayed behind in the other directory after a
        // half-moved pair, so both are checked.
        const std::string sourceMeta = store_paths::metaIn(sourceDir, recordingId);
        const std::string otherMeta = store_paths::metaIn(otherDir, recordingId);
        const std::string targetMeta = store_paths::metaIn(kCorruptDirName, recordingId);
        const std::string* from = nullptr;
        if (fs_.exists(sourceMeta)) {
            from = &sourceMeta;
        } else if (fs_.exists(otherMeta)) {
            from = &otherMeta;
        }
        if (from != nullptr) {
            fs_.remove(targetMeta);
            fs_.rename(*from, targetMeta);
        }
    }
    return moved;
}

bool RecordingStore::recoverOnBoot() {
    items_.clear();
    recovered_count_ = 0;
    corrupt_count_ = 0;
    discarded_tmp_count_ = 0;
    next_sequence_ = 1;

    std::vector<std::string> names;

    // --- 1. tmp/: never a valid recording -----------------------------------
    // The only exception is a .wav.tmp whose sidecar was already committed to
    // pending/: that is a commit interrupted between the two renames, and
    // finishing it is exactly what makes the commit protocol crash safe.
    names.clear();
    if (fs_.listDirectory(store_paths::directory(kTmpDirName), names)) {
        for (const std::string& name : names) {
            std::string id;
            if (store_paths::idFromFileName(name, ".wav.tmp", &id)) {
                const bool committedSidecar =
                    fs_.exists(store_paths::metaIn(kPendingDirName, id)) &&
                    !fs_.exists(store_paths::wavIn(kPendingDirName, id));
                if (committedSidecar) {
                    if (fs_.rename(
                            store_paths::tmpWav(id),
                            store_paths::wavIn(kPendingDirName, id))) {
                        ++recovered_count_;
                        char line[160];
                        std::snprintf(line, sizeof(line), "[store] recovered id=%s (interrupted commit)", id.c_str());
                        logLine(line);
                    } else {
                        // This tmp WAV is the ONLY copy of the audio. Never
                        // delete it because the rename failed: keep it and the
                        // committed sidecar so the next boot can try again.
                        char line[200];
                        std::snprintf(line, sizeof(line),
                                      "[store] interrupted commit not completed id=%s (kept for the next boot)",
                                      id.c_str());
                        logLine(line);
                    }
                    continue;
                }
            }
            fs_.remove(store_paths::join(store_paths::directory(kTmpDirName), name));
            ++discarded_tmp_count_;
        }
    }

    // --- 2. uploading/: an interrupted attempt is still a valid recording ----
    // It goes back to pending/ so the queue is rebuilt honestly (case D). This
    // runs before the pending/ scan so a half-moved pair re-pairs first.
    names.clear();
    if (fs_.listDirectory(store_paths::directory(kUploadingDirName), names)) {
        for (const std::string& name : names) {
            std::string id;
            if (store_paths::idFromFileName(name, ".wav", &id)) {
                if (fs_.rename(
                        store_paths::wavIn(kUploadingDirName, id),
                        store_paths::wavIn(kPendingDirName, id))) {
                    fs_.rename(
                        store_paths::metaIn(kUploadingDirName, id),
                        store_paths::metaIn(kPendingDirName, id));
                    ++recovered_count_;
                    char line[160];
                    std::snprintf(line, sizeof(line), "[store] recovered id=%s (interrupted upload)", id.c_str());
                    logLine(line);
                }
                // On failure the item stays in uploading/ and is picked up from
                // there by the scan below.
            } else if (store_paths::idFromFileName(name, ".json", &id)) {
                fs_.rename(
                    store_paths::metaIn(kUploadingDirName, id),
                    store_paths::metaIn(kPendingDirName, id));
            } else {
                fs_.remove(store_paths::join(store_paths::directory(kUploadingDirName), name));
            }
        }
    }

    // --- 3. pending/ + uploading/: build the queue ---------------------------
    // Both directories are scanned: normally step 2 emptied uploading/, but if a
    // rename failed there the item is still a valid, undelivered recording and
    // must not become invisible. The item loop below picks the directory the
    // file actually lives in.
    names.clear();
    if (!fs_.listDirectory(store_paths::directory(kPendingDirName), names)) {
        return false;
    }

    std::vector<std::string> ids;
    for (const std::string& name : names) {
        std::string id;
        if (store_paths::idFromFileName(name, ".wav", &id) && !containsId(ids, id)) {
            ids.push_back(id);
        }
    }

    std::vector<std::string> uploadingNames;
    if (fs_.listDirectory(store_paths::directory(kUploadingDirName), uploadingNames)) {
        for (const std::string& name : uploadingNames) {
            std::string id;
            if (store_paths::idFromFileName(name, ".wav", &id) && !containsId(ids, id)) {
                ids.push_back(id);
            }
        }
    }

    // A sidecar without any audio (and with no tmp to finish from) is metadata
    // for something that no longer exists: quarantine it, never guess.
    for (const std::string& name : names) {
        std::string id;
        if (!store_paths::idFromFileName(name, ".json", &id)) {
            continue;
        }
        if (containsId(ids, id)) {
            continue;
        }
        if (fs_.exists(store_paths::wavIn(kUploadingDirName, id))) {
            continue;
        }
        // A tmp WAV that could not be promoted means the commit is still
        // completable: never quarantine the sidecar that proves the intent.
        if (fs_.exists(store_paths::tmpWav(id))) {
            continue;
        }
        // Move (not copy-then-delete) so the quarantined sidecar keeps its
        // bytes; a stale quarantined copy from an earlier boot is replaced.
        fs_.remove(store_paths::metaIn(kCorruptDirName, id));
        fs_.rename(
            store_paths::metaIn(kPendingDirName, id),
            store_paths::metaIn(kCorruptDirName, id));
        ++corrupt_count_;
        char line[160];
        std::snprintf(line, sizeof(line), "[store] quarantined metadata without audio id=%s", id.c_str());
        logLine(line);
    }

    for (const std::string& id : ids) {
        StoreItem item;
        const std::string pendingWav = store_paths::wavIn(kPendingDirName, id);
        const std::string uploadingWav = store_paths::wavIn(kUploadingDirName, id);
        const bool inUploading = !fs_.exists(pendingWav) && fs_.exists(uploadingWav);
        const std::string wavPath = inUploading ? uploadingWav : pendingWav;
        item.in_uploading = inUploading;
        item.mod_time = fs_.fileModTime(wavPath);

        const uint32_t fileBytes = static_cast<uint32_t>(fs_.fileSize(wavPath));

        RecordingMeta meta;
        // The sidecar may still be in the other directory after a half-moved
        // pair (WAV renamed, sidecar not, or the reverse).
        const bool hasMeta = loadMetaFor(id, inUploading, meta);
        if (hasMeta) {
            // The file name is authoritative for the id, and the card is
            // authoritative for the size.
            meta.recording_id = id;
        } else if (!synthesizeMeta(id, wavPath, fileBytes, meta)) {
            moveToCorrupt(id, inUploading, true);
            ++corrupt_count_;
            char line[160];
            std::snprintf(line, sizeof(line), "[store] quarantined invalid audio id=%s", id.c_str());
            logLine(line);
            continue;
        }

        // Validate (and repair) against the card. Reading a 44-byte header is
        // cheap, so it always runs; the full CRC is opt-in.
        RecordingMeta validated = meta;
        if (!verifyWav(wavPath, static_cast<uint32_t>(validated.wav_bytes), validated)) {
            moveToCorrupt(id, inUploading, true);
            ++corrupt_count_;
            char line[160];
            std::snprintf(line, sizeof(line), "[store] quarantined corrupt audio id=%s", id.c_str());
            logLine(line);
            continue;
        }

        item.meta = validated;
        if (!hasMeta) {
            // A recovered WAV without a sidecar is re-persisted with a real
            // sidecar so the next boot does not have to guess again.
            rewriteMeta(item);
        }
        items_.push_back(item);
    }

    std::sort(items_.begin(), items_.end(), olderFirst);

    for (const StoreItem& item : items_) {
        if (item.meta.sequence >= next_sequence_) {
            next_sequence_ = item.meta.sequence + 1;
        }
    }

    return true;
}

SaveOutcome RecordingStore::savePending(
    const RecordingMeta& meta,
    const uint8_t* wav,
    size_t wavBytes
) {
    if (!available_) {
        return SaveOutcome::NotMounted;
    }
    if (!store_paths::isSafeRecordingId(meta.recording_id)) {
        return SaveOutcome::InvalidId;
    }
    if (wav == nullptr || wavBytes < VM_WAV_HEADER_BYTES) {
        return SaveOutcome::IoError;
    }
    if (find(meta.recording_id) != nullptr) {
        return SaveOutcome::DuplicateId;
    }

    // Backpressure first: refuse before touching the card so nothing on it can
    // be lost or overwritten.
    if (items_.size() >= max_pending_) {
        storage_full_ = true;
        logLine("[sd] storage full (queue limit reached); recording kept in RAM");
        return SaveOutcome::StorageFull;
    }
    const uint64_t required = static_cast<uint64_t>(wavBytes) + min_free_bytes_;
    if (freeBytes() < required) {
        storage_full_ = true;
        logLine("[sd] storage full (free space reserve); recording kept in RAM");
        return SaveOutcome::StorageFull;
    }

    RecordingMeta stored = meta;
    stored.schema_version = kRecordingMetaSchemaVersion;
    stored.wav_bytes = static_cast<uint32_t>(wavBytes);
    if (stored.sample_rate == 0) {
        stored.sample_rate = static_cast<uint32_t>(VM_SAMPLE_RATE);
    }
    stored.crc32 = crc32_ieee(wav, wavBytes);
    stored.has_crc = true;
    stored.sequence = next_sequence_;

    const std::string json = recording_meta_to_json(stored);
    const std::string pendingWav = store_paths::wavIn(kPendingDirName, stored.recording_id);
    const std::string pendingMeta = store_paths::metaIn(kPendingDirName, stored.recording_id);
    const std::string uploadingWav = store_paths::wavIn(kUploadingDirName, stored.recording_id);
    const std::string tmpWav = store_paths::tmpWav(stored.recording_id);
    const std::string tmpMeta = store_paths::tmpMeta(stored.recording_id);

    SaveOutcome outcome = SaveOutcome::Ok;
    {
        ScopedVolumeLock lock(fs_);

        // The in-memory queue is not the only authority: a file left on the card
        // by an earlier boot (or by a half-recovered queue) must never be
        // overwritten or have its sidecar deleted. Refuse instead.
        if (fs_.exists(pendingWav) || fs_.exists(pendingMeta) || fs_.exists(uploadingWav)) {
            return SaveOutcome::DuplicateId;
        }

        // Stale partials from a previous attempt with the same id can never be
        // part of this commit.
        fs_.remove(tmpWav);
        fs_.remove(tmpMeta);

        // 1. metadata sidecar first: it is the marker that lets recovery finish
        //    an interrupted commit.
        if (!fs_.writeFile(tmpMeta, reinterpret_cast<const uint8_t*>(json.data()), json.size())) {
            outcome = SaveOutcome::IoError;
        } else if (!fs_.writeFile(tmpWav, wav, wavBytes)) {
            outcome = SaveOutcome::IoError;
        } else if (!fs_.rename(tmpMeta, pendingMeta)) {
            outcome = SaveOutcome::IoError;
        } else if (!fs_.rename(tmpWav, pendingWav)) {
            // Roll the sidecar back: leaving a committed sidecar with no audio
            // would be quarantined at boot, and this attempt is a clean failure.
            fs_.remove(pendingMeta);
            outcome = SaveOutcome::IoError;
        }

        if (outcome != SaveOutcome::Ok) {
            fs_.remove(tmpWav);
            fs_.remove(tmpMeta);
            fs_.remove(pendingMeta);
        }
    }

    if (outcome != SaveOutcome::Ok) {
        char line[200];
        std::snprintf(line, sizeof(line), "[sd] write failed id=%s bytes=%lu (%s)",
                      stored.recording_id.c_str(),
                      static_cast<unsigned long>(wavBytes),
                      saveOutcomeName(outcome));
        logLine(line);
        return outcome;
    }

    StoreItem item;
    item.meta = stored;
    item.in_uploading = false;
    item.mod_time = 0;
    items_.push_back(item);
    std::sort(items_.begin(), items_.end(), olderFirst);
    ++next_sequence_;
    storage_full_ = false;

    char line[200];
    std::snprintf(line, sizeof(line), "[store] committed id=%s bytes=%lu crc=%08lX",
                  stored.recording_id.c_str(),
                  static_cast<unsigned long>(wavBytes),
                  static_cast<unsigned long>(stored.crc32));
    logLine(line);
    return SaveOutcome::Ok;
}

const StoreItem* RecordingStore::oldestPending() const {
    return items_.empty() ? nullptr : &items_.front();
}

const StoreItem* RecordingStore::find(const std::string& recordingId) const {
    for (const StoreItem& item : items_) {
        if (item.meta.recording_id == recordingId) {
            return &item;
        }
    }
    return nullptr;
}

bool RecordingStore::rewriteMeta(const StoreItem& item) {
    const char* dirName = item.in_uploading ? kUploadingDirName : kPendingDirName;
    const std::string json = recording_meta_to_json(item.meta);
    const std::string tmpMeta = store_paths::tmpMeta(item.meta.recording_id);
    const std::string targetMeta = store_paths::metaIn(dirName, item.meta.recording_id);

    ScopedVolumeLock lock(fs_);
    if (!fs_.writeFile(tmpMeta, reinterpret_cast<const uint8_t*>(json.data()), json.size())) {
        fs_.remove(tmpMeta);
        return false;
    }
    if (!fs_.rename(tmpMeta, targetMeta)) {
        fs_.remove(tmpMeta);
        return false;
    }
    return true;
}

bool RecordingStore::markUploading(const std::string& recordingId) {
    if (!available_) {
        return false;
    }
    StoreItem* item = nullptr;
    for (StoreItem& candidate : items_) {
        if (candidate.meta.recording_id == recordingId) {
            item = &candidate;
            break;
        }
    }
    if (item == nullptr) {
        return false;
    }
    if (item->in_uploading) {
        return true;
    }

    ScopedVolumeLock lock(fs_);
    const std::string fromWav = store_paths::wavIn(kPendingDirName, recordingId);
    const std::string toWav = store_paths::wavIn(kUploadingDirName, recordingId);
    if (!fs_.exists(fromWav)) {
        // The pair is already where it should be (or the card changed): trust
        // the card and retry from the uploading directory.
        if (fs_.exists(toWav)) {
            item->in_uploading = true;
            return true;
        }
        return false;
    }
    if (!fs_.rename(fromWav, toWav)) {
        // A rename that should succeed failed: the volume is suspect. Let the
        // caller's remount/recovery cycle repair it rather than guessing here.
        markUnhealthy();
        return false;
    }
    fs_.rename(
        store_paths::metaIn(kPendingDirName, recordingId),
        store_paths::metaIn(kUploadingDirName, recordingId));
    item->in_uploading = true;
    return true;
}

bool RecordingStore::dropIfMissing(const std::string& recordingId) {
    const StoreItem* item = find(recordingId);
    if (item == nullptr) {
        return false;
    }
    // Never guess on an unmounted or suspect volume: absence must be proven.
    if (!available_ || !healthy_) {
        return false;
    }

    ScopedVolumeLock lock(fs_);
    if (fs_.exists(store_paths::wavIn(kPendingDirName, recordingId)) ||
        fs_.exists(store_paths::wavIn(kUploadingDirName, recordingId))) {
        return false;
    }

    auto it = std::find_if(
        items_.begin(), items_.end(),
        [&recordingId](const StoreItem& candidate) {
            return candidate.meta.recording_id == recordingId;
        });
    if (it != items_.end()) {
        items_.erase(it);
        return true;
    }
    return false;
}

const StoreItem* RecordingStore::nextPending(uint32_t maxAttempts) const {
    for (const StoreItem& item : items_) {
        // items_ is oldest-first, so the first item under the cap is the one to
        // try next; a recording the ingress keeps rejecting no longer starves
        // the ones behind it.
        if (item.meta.attempt_count < maxAttempts) {
            return &item;
        }
    }
    return nullptr;
}

bool RecordingStore::markPendingAgain(const std::string& recordingId, uint32_t attemptCount) {
    StoreItem* item = nullptr;
    for (StoreItem& candidate : items_) {
        if (candidate.meta.recording_id == recordingId) {
            item = &candidate;
            break;
        }
    }
    if (item == nullptr) {
        return false;
    }

    if (available_ && item->in_uploading) {
        ScopedVolumeLock lock(fs_);
        const std::string fromWav = store_paths::wavIn(kUploadingDirName, recordingId);
        const std::string toWav = store_paths::wavIn(kPendingDirName, recordingId);
        if (fs_.exists(fromWav)) {
            if (fs_.rename(fromWav, toWav)) {
                fs_.rename(
                    store_paths::metaIn(kUploadingDirName, recordingId),
                    store_paths::metaIn(kPendingDirName, recordingId));
                item->in_uploading = false;
            } else {
                // The move back failed: the volume is suspect, so trigger the
                // remount/recovery cycle instead of leaving it half-moved.
                markUnhealthy();
            }
        }
        // If the file was not in uploading/ either, the item stays as it is and
        // is still a valid, retryable recording.
    }

    item->meta.attempt_count = attemptCount;
    rewriteMeta(*item);
    return true;
}

bool RecordingStore::markUploaded(const std::string& recordingId) {
    auto it = std::find_if(
        items_.begin(), items_.end(),
        [&recordingId](const StoreItem& item) { return item.meta.recording_id == recordingId; });
    if (it == items_.end()) {
        return false;
    }

    bool ok = true;
    if (available_) {
        ScopedVolumeLock lock(fs_);
        const std::string pendingWav = store_paths::wavIn(kPendingDirName, recordingId);
        const std::string pendingMeta = store_paths::metaIn(kPendingDirName, recordingId);
        const std::string uploadingWav = store_paths::wavIn(kUploadingDirName, recordingId);
        const std::string uploadingMeta = store_paths::metaIn(kUploadingDirName, recordingId);

        if (keep_sent_) {
            // Retention: move both files to sent/ instead of deleting them.
            const std::string sentWav = store_paths::wavIn(kSentDirName, recordingId);
            const std::string sentMeta = store_paths::metaIn(kSentDirName, recordingId);
            fs_.remove(sentWav);
            fs_.remove(sentMeta);
            if (fs_.exists(pendingWav)) {
                ok = fs_.rename(pendingWav, sentWav) && ok;
                if (fs_.exists(pendingMeta)) {
                    fs_.rename(pendingMeta, sentMeta);
                }
            } else if (fs_.exists(uploadingWav)) {
                ok = fs_.rename(uploadingWav, sentWav) && ok;
                if (fs_.exists(uploadingMeta)) {
                    fs_.rename(uploadingMeta, sentMeta);
                }
            }
        } else {
            // Default policy: the ingress confirmed delivery, so the audio is
            // not kept on the card. Only this recording's files are touched.
            const bool hadPending = fs_.exists(pendingWav);
            const bool hadUploading = fs_.exists(uploadingWav);
            if (hadPending) {
                ok = fs_.remove(pendingWav) && ok;
            }
            if (hadUploading) {
                ok = fs_.remove(uploadingWav) && ok;
            }
            fs_.remove(pendingMeta);
            fs_.remove(uploadingMeta);
        }
    }

    items_.erase(it);
    // A confirmed upload is the only thing that reclaims space, so whatever
    // filled the card before is now less full.
    storage_full_ = false;
    if (!ok) {
        // The ingress has the audio but the card could not be updated: flag the
        // volume so the caller remounts and recovery re-delivers (which is
        // idempotent on recording_id).
        markUnhealthy();
    }
    return ok;
}

bool RecordingStore::markCorrupt(const std::string& recordingId) {
    auto it = std::find_if(
        items_.begin(), items_.end(),
        [&recordingId](const StoreItem& item) { return item.meta.recording_id == recordingId; });
    if (it == items_.end()) {
        return false;
    }
    const bool moved = available_ ? moveToCorrupt(recordingId, it->in_uploading, true) : false;
    items_.erase(it);
    return moved;
}

FileHandle* RecordingStore::openRead(const std::string& recordingId) {
    if (!available_) {
        return nullptr;
    }
    const StoreItem* item = find(recordingId);
    if (item == nullptr) {
        return nullptr;
    }
    const std::string path = store_paths::wavIn(
        item->in_uploading ? kUploadingDirName : kPendingDirName, recordingId);

    FileHandle* raw = nullptr;
    size_t size = 0;
    {
        ScopedVolumeLock lock(fs_);
        raw = fs_.openRead(path);
        if (raw != nullptr) {
            size = raw->size();
        }
    }
    if (raw == nullptr) {
        char line[200];
        std::snprintf(line, sizeof(line), "[sd] cannot open id=%s for upload", recordingId.c_str());
        logLine(line);
        // The item exists in the queue but its file could not be opened: treat
        // the volume as suspect so the caller remounts and recovery decides
        // whether the recording is really gone.
        markUnhealthy();
        return nullptr;
    }

    LockedFileHandle* handle = new LockedFileHandle(fs_, raw, size);
    if (handle == nullptr) {
        raw->close();
        delete raw;
        return nullptr;
    }
    return handle;
}

uint64_t RecordingStore::totalBytes() {
    if (!available_) {
        return 0;
    }
    ScopedVolumeLock lock(fs_);
    return fs_.totalBytes();
}

uint64_t RecordingStore::freeBytes() {
    if (!available_) {
        return 0;
    }
    ScopedVolumeLock lock(fs_);
    return fs_.freeBytes();
}

}  // namespace voice_memo_firmware
