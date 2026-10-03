#pragma once

// Firmware backend for vm_fs.h: the ONLY translation unit in the sketch that
// knows about SD_MMC, the mount point, the SDMMC pins or the card's health.
//
// Everything above it (recording_store) works on paths and byte streams, so the
// persistence policy is host-testable and the card can disappear without
// crashing anything: every method returns false instead of aborting, and a
// failed operation marks the volume unhealthy so the caller can fall back and
// retry the mount later.
//
// Only compiled when VM_ENABLE_SD is 1; see the stub at the bottom of the
// header for the disabled build.

#include <Arduino.h>
#include <FS.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "vm_fs.h"

#if VM_ENABLE_SD
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace voice_memo_firmware {

#if VM_ENABLE_SD

class SdStorage : public FileSystem {
public:
    SdStorage() = default;
    ~SdStorage() override;

    // Mounts the card. Idempotent and never formats it.
    bool mount() override;
    void unmount() override;
    bool mounted() override;

    bool makeDirectory(const std::string& path) override;

    bool exists(const std::string& path) override;
    bool isDirectory(const std::string& path) override;
    size_t fileSize(const std::string& path) override;
    uint32_t fileModTime(const std::string& path) override;
    bool listDirectory(const std::string& dir, std::vector<std::string>& out) override;

    bool readFile(const std::string& path, std::vector<uint8_t>& out) override;
    bool writeFile(const std::string& path, const uint8_t* data, size_t length) override;

    bool remove(const std::string& path) override;
    bool rename(const std::string& from, const std::string& to) override;

    FileHandle* openRead(const std::string& path) override;

    uint64_t totalBytes() override;
    uint64_t freeBytes() override;

    void lockVolume() override;
    void unlockVolume() override;

    // Physical bring-up test (VM_SD_SELF_TEST): write a known pattern into
    // <root>/tmp, flush, close, reopen, verify and remove it. Never runs in a
    // production build unless the flag is set.
    bool selfTest();

private:
    // Best-effort mount bookkeeping. Called with the volume lock held.
    bool mountLocked();
    void createMutex();

    bool mounted_ = false;
    SemaphoreHandle_t mutex_ = nullptr;
};

#else  // VM_ENABLE_SD

// Build without microSD: the store is never constructed, so this only exists to
// keep type references valid.
class SdStorage : public FileSystem {
public:
    bool mount() override { return false; }
    void unmount() override {}
    bool mounted() override { return false; }
    bool makeDirectory(const std::string&) override { return false; }
    bool exists(const std::string&) override { return false; }
    bool isDirectory(const std::string&) override { return false; }
    size_t fileSize(const std::string&) override { return 0; }
    uint32_t fileModTime(const std::string&) override { return 0; }
    bool listDirectory(const std::string&, std::vector<std::string>&) override { return false; }
    bool readFile(const std::string&, std::vector<uint8_t>&) override { return false; }
    bool writeFile(const std::string&, const uint8_t*, size_t) override { return false; }
    bool remove(const std::string&) override { return false; }
    bool rename(const std::string&, const std::string&) override { return false; }
    FileHandle* openRead(const std::string&) override { return nullptr; }
    uint64_t totalBytes() override { return 0; }
    uint64_t freeBytes() override { return 0; }
    void lockVolume() override {}
    void unlockVolume() override {}
};

#endif  // VM_ENABLE_SD

}  // namespace voice_memo_firmware
