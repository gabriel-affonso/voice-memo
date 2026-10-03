#pragma once

// Tiny filesystem abstraction us by the persistent recording store.
//
// Why this exists
// ---------------
// The microSD card is an *optional* peripheral on this board. The code that
// decides what to do with a finalized recording (paths, metadata, atomic
// tmp -> pending commit, boot recovery, queue ordering, retention) must not
// depend on Arduino, SD_MMC, FreeRTOS or a physical card, so that:
//
//   * the exact rules can be exercised on the host with an in-memory volume
//     (tests/recording_store_test.cpp), and
//   * the firmware can keep working when the card is missing, removed or full.
//
// Only the thin backend (sd_storage.{h,cpp}) knows about SD_MMC; only
// recording_store.{h,cpp} knows about the directory layout. Nothing else in the
// firmware opens a file.
//
// Concurrency
// -----------
// The volume is shared by the Arduino loop task (persist, mark, remove) and the
// background upload task (sequential reads of a pending WAV). Every access MUST
// go through lockVolume()/unlockVolume(); the firmware backend maps that onto a
// FreeRTOS mutex and the host backend uses a no-op. Locks are only ever held
// for short filesystem operations - never across HTTP.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voice_memo_firmware {

// A read-only, sequential view of one file. Ownership: the caller must
// close() and then delete the handle (see FileByteSource in the firmware).
class FileHandle {
public:
    virtual ~FileHandle() {}
    // Total size in bytes, 0 when unknown.
    virtual size_t size() const = 0;
    // Sequential read; returns the number of bytes read (0 at EOF or on error).
    virtual size_t read(uint8_t* dest, size_t maxBytes) = 0;
    // Releases the underlying file. Must be idempotent.
    virtual void close() = 0;
};

// Minimal volume interface. Implementations must never throw and must return
// false instead of aborting when the card is absent/removed.
class FileSystem {
public:
    virtual ~FileSystem() {}

    // --- mount lifecycle ----------------------------------------------------
    // mount() is idempotent: it returns true when the volume is already (or now)
    // usable. It must not format the card.
    virtual bool mount() = 0;
    virtual void unmount() = 0;
    virtual bool mounted() = 0;

    // --- directories --------------------------------------------------------
    virtual bool makeDirectory(const std::string& path) = 0;

    // --- queries ------------------------------------------------------------
    virtual bool exists(const std::string& path) = 0;
    virtual bool isDirectory(const std::string& path) = 0;
    virtual size_t fileSize(const std::string& path) = 0;
    // Last-write time in seconds since the epoch, 0 when unknown.
    virtual uint32_t fileModTime(const std::string& path) = 0;
    // Names (not paths) directly inside `dir`; false on I/O error.
    virtual bool listDirectory(const std::string& dir, std::vector<std::string>& out) = 0;

    // --- whole-file helpers (small files and the PSRAM WAV) ------------------
    virtual bool readFile(const std::string& path, std::vector<uint8_t>& out) = 0;
    virtual bool writeFile(const std::string& path, const uint8_t* data, size_t length) = 0;

    // --- mutation -----------------------------------------------------------
    virtual bool remove(const std::string& path) = 0;
    virtual bool rename(const std::string& from, const std::string& to) = 0;

    // --- streaming ----------------------------------------------------------
    // Returns nullptr when the file cannot be opened. The handle is owned by the
    // caller.
    virtual FileHandle* openRead(const std::string& path) = 0;

    // --- capacity -----------------------------------------------------------
    virtual uint64_t totalBytes() = 0;
    virtual uint64_t freeBytes() = 0;

    // --- serialisation ------------------------------------------------------
    virtual void lockVolume() = 0;
    virtual void unlockVolume() = 0;
};

// RAII helper so a filesystem operation can never return with the volume
// locked, whatever path it takes.
class ScopedVolumeLock {
public:
    explicit ScopedVolumeLock(FileSystem& fs) : fs_(fs) { fs_.lockVolume(); }
    ~ScopedVolumeLock() { fs_.unlockVolume(); }

    ScopedVolumeLock(const ScopedVolumeLock&) = delete;
    ScopedVolumeLock& operator=(const ScopedVolumeLock&) = delete;

private:
    FileSystem& fs_;
};

}  // namespace voice_memo_firmware
