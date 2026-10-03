#include "sd_storage.h"

#if VM_ENABLE_SD

#include <cstring>

#include "recording_paths.h"

namespace voice_memo_firmware {
namespace {

// File::name() has returned both "name" and "/dir/name" across Arduino-ESP32
// versions, so the basename is always taken explicitly.
std::string baseName(const char* raw) {
    if (raw == nullptr) {
        return std::string();
    }
    const std::string value(raw);
    const size_t slash = value.find_last_of('/');
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

void printCapacityLine() {
    const uint64_t total = SD_MMC.totalBytes();
    const uint64_t used = SD_MMC.usedBytes();
    const uint64_t free = total > used ? total - used : 0;
    const sdcard_type_t type = SD_MMC.cardType();
    const char* typeName = "unknown";
    switch (type) {
        case CARD_NONE:
            typeName = "none";
            break;
        case CARD_MMC:
            typeName = "mmc";
            break;
        case CARD_SD:
            typeName = "sd";
            break;
        case CARD_SDHC:
            typeName = "sdhc";
            break;
        default:
            typeName = "other";
            break;
    }
    Serial.printf("[sd] mounted type=%s total=%llu free=%llu\n",
                  typeName,
                  static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(free));
}

// Sequential File reader handed to the upload task. The store wraps this in its
// own volume lock, so the File is only touched while that lock is held.
class SdFileHandle : public FileHandle {
public:
    explicit SdFileHandle(File file) : file_(file) {}

    size_t size() const override { return file_ ? file_.size() : 0; }

    size_t read(uint8_t* dest, size_t maxBytes) override {
        if (!file_) {
            return 0;
        }
        return file_.read(dest, maxBytes);
    }

    void close() override {
        if (file_) {
            file_.close();
        }
    }

private:
    File file_;
};

}  // namespace

SdStorage::~SdStorage() {
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

void SdStorage::createMutex() {
    if (mutex_ == nullptr) {
        // Recursive on purpose: RecordingStore takes the volume lock around a
        // multi-step operation (for example the tmp -> pending commit) and the
        // individual FileSystem calls it makes internally take the same lock
        // again. A plain mutex would deadlock the task against itself.
        mutex_ = xSemaphoreCreateRecursiveMutex();
    }
}

void SdStorage::lockVolume() {
    createMutex();
    if (mutex_ != nullptr) {
        xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
    }
}

void SdStorage::unlockVolume() {
    if (mutex_ != nullptr) {
        xSemaphoreGiveRecursive(mutex_);
    }
}

bool SdStorage::mounted() {
    return mounted_;
}

bool SdStorage::mountLocked() {
    if (mounted_) {
        return true;
    }

    // Official pins, official 1-bit width, official mount point (see config.h).
    if (!SD_MMC.setPins(VM_SD_CLK_PIN, VM_SD_CMD_PIN, VM_SD_D0_PIN)) {
        Serial.println("[sd] setPins failed");
        return false;
    }

    // begin(mountpoint, mode1bit, format_if_mount_failed, frequency, maxOpenFiles)
    // format_if_mount_failed is false: firmware must never reformat a card.
    // Absence is reported once by RecordingStore (which owns the retry policy),
    // not by every remount attempt.
    if (!SD_MMC.begin(VM_SD_MOUNT_POINT, true, false, VM_SD_FREQ_KHZ, VM_SD_MAX_OPEN_FILES)) {
        return false;
    }
    if (SD_MMC.cardType() == CARD_NONE) {
        SD_MMC.end();
        return false;
    }

    mounted_ = true;
    printCapacityLine();
    return true;
}

bool SdStorage::mount() {
    ScopedVolumeLock lock(*this);
    if (mounted_) {
        return true;
    }
    Serial.println("[sd] initializing...");
    return mountLocked();
}

void SdStorage::unmount() {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return;
    }
    SD_MMC.end();
    mounted_ = false;
    Serial.println("[sd] unmounted");
}

bool SdStorage::makeDirectory(const std::string& path) {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return false;
    }
    return SD_MMC.mkdir(path.c_str());
}

bool SdStorage::exists(const std::string& path) {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return false;
    }
    return SD_MMC.exists(path.c_str());
}

bool SdStorage::isDirectory(const std::string& path) {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return false;
    }
    File file = SD_MMC.open(path.c_str(), FILE_READ);
    if (!file) {
        return false;
    }
    const bool isDir = file.isDirectory();
    file.close();
    return isDir;
}

size_t SdStorage::fileSize(const std::string& path) {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return 0;
    }
    File file = SD_MMC.open(path.c_str(), FILE_READ);
    if (!file) {
        return 0;
    }
    const size_t size = file.size();
    file.close();
    return size;
}

uint32_t SdStorage::fileModTime(const std::string& path) {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return 0;
    }
    File file = SD_MMC.open(path.c_str(), FILE_READ);
    if (!file) {
        return 0;
    }
    const time_t modified = file.getLastWrite();
    file.close();
    return modified < 0 ? 0U : static_cast<uint32_t>(modified);
}

bool SdStorage::listDirectory(const std::string& dir, std::vector<std::string>& out) {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return false;
    }
    File directory = SD_MMC.open(dir.c_str(), FILE_READ);
    if (!directory || !directory.isDirectory()) {
        return false;
    }
    for (;;) {
        File entry = directory.openNextFile();
        if (!entry) {
            break;
        }
        const std::string name = baseName(entry.name());
        entry.close();
        if (!name.empty()) {
            out.push_back(name);
        }
    }
    directory.close();
    return true;
}

bool SdStorage::readFile(const std::string& path, std::vector<uint8_t>& out) {
    ScopedVolumeLock lock(*this);
    out.clear();
    if (!mounted_) {
        return false;
    }
    File file = SD_MMC.open(path.c_str(), FILE_READ);
    if (!file) {
        return false;
    }
    const size_t size = file.size();
    if (size == 0 || size > 65536U) {
        // Sidecars are a few hundred bytes; anything else is not one.
        file.close();
        return false;
    }
    out.resize(size);
    size_t total = 0;
    while (total < size) {
        const size_t got = file.read(out.data() + total, size - total);
        if (got == 0) {
            break;
        }
        total += got;
    }
    file.close();
    if (total != size) {
        out.clear();
        return false;
    }
    return true;
}

bool SdStorage::writeFile(const std::string& path, const uint8_t* data, size_t length) {
    ScopedVolumeLock lock(*this);
    if (!mounted_ || data == nullptr) {
        return false;
    }
    File file = SD_MMC.open(path.c_str(), FILE_WRITE, true);
    if (!file) {
        return false;
    }
    size_t total = 0;
    while (total < length) {
        const size_t written = file.write(data + total, length - total);
        if (written == 0) {
            break;
        }
        total += written;
    }
    file.flush();
    file.close();
    return total == length;
}

bool SdStorage::remove(const std::string& path) {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return false;
    }
    if (!SD_MMC.exists(path.c_str())) {
        // Removing a missing file is a success for every caller here.
        return true;
    }
    return SD_MMC.remove(path.c_str());
}

bool SdStorage::rename(const std::string& from, const std::string& to) {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return false;
    }
    return SD_MMC.rename(from.c_str(), to.c_str());
}

FileHandle* SdStorage::openRead(const std::string& path) {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return nullptr;
    }
    File file = SD_MMC.open(path.c_str(), FILE_READ);
    if (!file) {
        return nullptr;
    }
    return new SdFileHandle(file);
}

uint64_t SdStorage::totalBytes() {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return 0;
    }
    return SD_MMC.totalBytes();
}

uint64_t SdStorage::freeBytes() {
    ScopedVolumeLock lock(*this);
    if (!mounted_) {
        return 0;
    }
    const uint64_t total = SD_MMC.totalBytes();
    const uint64_t used = SD_MMC.usedBytes();
    return total > used ? total - used : 0;
}

bool SdStorage::selfTest() {
    if (!mounted_) {
        Serial.println("[sd] self-test skipped: card not mounted");
        return false;
    }

    static const uint8_t kPattern[64] = {
        0x56, 0x4D, 0x53, 0x44, 0x54, 0x45, 0x53, 0x54,  // "VMSDTEST"
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    };

    const std::string directory = store_paths::directory(store_paths::kTmpDirName);
    if (!isDirectory(directory)) {
        makeDirectory(store_paths::root());
        makeDirectory(directory);
    }
    const std::string path = store_paths::join(directory, "sd_test.tmp");

    if (!writeFile(path, kPattern, sizeof(kPattern))) {
        Serial.println("[sd] self-test write failed");
        return false;
    }
    if (fileSize(path) != sizeof(kPattern)) {
        Serial.println("[sd] self-test size mismatch");
        remove(path);
        return false;
    }

    std::vector<uint8_t> readback;
    if (!readFile(path, readback) || readback.size() != sizeof(kPattern) ||
        std::memcmp(readback.data(), kPattern, sizeof(kPattern)) != 0) {
        Serial.println("[sd] self-test read-back mismatch");
        remove(path);
        return false;
    }

    if (!remove(path) || exists(path)) {
        Serial.println("[sd] self-test cleanup failed");
        return false;
    }

    Serial.println("[sd] self-test ok (write/read/verify/remove)");
    return true;
}

}  // namespace voice_memo_firmware

#endif  // VM_ENABLE_SD
