#pragma once

// In-memory FileSystem for the host tests of recording_store.
//
// It behaves like a small FAT volume: a flat map of absolute paths, explicit
// directories, capacity accounting, and injectable faults. That is enough to
// drive every branch of the store - atomic commit, recovery, quarantine,
// backpressure, card removal - with no Arduino and no hardware.

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../vm_fs.h"

namespace vm_test {

inline std::string parentOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

inline std::string baseNameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

class MemoryFileHandle : public voice_memo_firmware::FileHandle {
public:
    explicit MemoryFileHandle(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

    size_t size() const override { return bytes_.size(); }

    size_t read(uint8_t* dest, size_t maxBytes) override {
        const size_t remaining = bytes_.size() - offset_;
        const size_t want = maxBytes < remaining ? maxBytes : remaining;
        for (size_t i = 0; i < want; ++i) {
            dest[i] = bytes_[offset_ + i];
        }
        offset_ += want;
        return want;
    }

    void close() override { bytes_.clear(); }

private:
    std::vector<uint8_t> bytes_;
    size_t offset_ = 0;
};

class MemoryFileSystem : public voice_memo_firmware::FileSystem {
public:
    // --- fault injection ----------------------------------------------------
    bool mount_succeeds = true;
    bool fail_writes = false;
    bool fail_renames = false;
    bool fail_removes = false;
    uint64_t total_bytes = 64ULL * 1024ULL * 1024ULL;
    uint64_t free_bytes = 64ULL * 1024ULL * 1024ULL;

    int write_count = 0;
    int rename_count = 0;
    int remove_count = 0;
    int mount_count = 0;

    // --- FileSystem ---------------------------------------------------------
    bool mount() override {
        ++mount_count;
        if (!mount_succeeds) {
            mounted_ = false;
            return false;
        }
        mounted_ = true;
        return true;
    }

    void unmount() override { mounted_ = false; }

    bool mounted() override { return mounted_; }

    bool makeDirectory(const std::string& path) override {
        if (!mounted_) {
            return false;
        }
        dirs_.insert(path);
        return true;
    }

    bool exists(const std::string& path) override {
        return files_.count(path) > 0 || dirs_.count(path) > 0;
    }

    bool isDirectory(const std::string& path) override { return dirs_.count(path) > 0; }

    size_t fileSize(const std::string& path) override {
        const auto it = files_.find(path);
        return it == files_.end() ? 0U : it->second.size();
    }

    uint32_t fileModTime(const std::string& path) override {
        const auto it = mod_times_.find(path);
        return it == mod_times_.end() ? 0U : it->second;
    }

    bool listDirectory(const std::string& dir, std::vector<std::string>& out) override {
        if (!mounted_ || dirs_.count(dir) == 0) {
            return false;
        }
        for (const auto& entry : files_) {
            if (parentOf(entry.first) == dir) {
                out.push_back(baseNameOf(entry.first));
            }
        }
        for (const auto& entry : dirs_) {
            if (entry != dir && parentOf(entry) == dir) {
                out.push_back(baseNameOf(entry));
            }
        }
        return true;
    }

    bool readFile(const std::string& path, std::vector<uint8_t>& out) override {
        out.clear();
        const auto it = files_.find(path);
        if (it == files_.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    bool writeFile(const std::string& path, const uint8_t* data, size_t length) override {
        if (!mounted_ || fail_writes || data == nullptr) {
            return false;
        }
        // Capacity accounting, so a full card is a real failure and not only a
        // pre-flight check.
        const size_t existing = fileSize(path);
        const int64_t growth = static_cast<int64_t>(length) - static_cast<int64_t>(existing);
        if (growth > 0) {
            if (static_cast<uint64_t>(growth) > free_bytes) {
                return false;
            }
            free_bytes -= static_cast<uint64_t>(growth);
        } else if (growth < 0) {
            free_bytes += static_cast<uint64_t>(-growth);
        }
        files_[path] = std::vector<uint8_t>(data, data + length);
        if (mod_times_.count(path) == 0) {
            mod_times_[path] = next_mod_time_++;
        }
        ++write_count;
        return true;
    }

    bool remove(const std::string& path) override {
        if (!mounted_ || fail_removes) {
            return false;
        }
        const auto it = files_.find(path);
        if (it != files_.end()) {
            free_bytes += it->second.size();
            files_.erase(it);
            mod_times_.erase(path);
            ++remove_count;
            return true;
        }
        const size_t removedDirs = dirs_.erase(path);
        if (removedDirs > 0) {
            ++remove_count;
        }
        return removedDirs > 0;
    }

    bool rename(const std::string& from, const std::string& to) override {
        if (!mounted_ || fail_renames) {
            return false;
        }
        const auto it = files_.find(from);
        if (it != files_.end()) {
            files_[to] = it->second;
            files_.erase(it);
            const auto timeIt = mod_times_.find(from);
            if (timeIt != mod_times_.end()) {
                mod_times_[to] = timeIt->second;
                mod_times_.erase(timeIt);
            }
            ++rename_count;
            return true;
        }
        const auto dirIt = dirs_.find(from);
        if (dirIt != dirs_.end()) {
            dirs_.erase(dirIt);
            dirs_.insert(to);
            ++rename_count;
            return true;
        }
        return false;
    }

    voice_memo_firmware::FileHandle* openRead(const std::string& path) override {
        const auto it = files_.find(path);
        if (it == files_.end()) {
            return nullptr;
        }
        return new MemoryFileHandle(it->second);
    }

    uint64_t totalBytes() override { return total_bytes; }

    uint64_t freeBytes() override { return free_bytes; }

    void lockVolume() override { ++lock_count; }

    void unlockVolume() override { ++unlock_count; }

    // --- inspection helpers -------------------------------------------------
    bool hasFile(const std::string& path) const { return files_.count(path) > 0; }

    bool hasDir(const std::string& path) const { return dirs_.count(path) > 0; }

    size_t countFilesIn(const std::string& dir) const {
        size_t count = 0;
        for (const auto& entry : files_) {
            if (parentOf(entry.first) == dir) {
                ++count;
            }
        }
        return count;
    }

    // Files (not sidecars) directly inside `dir`, i.e. one per recording.
    size_t countFilesWithSuffixIn(const std::string& dir, const std::string& suffix) const {
        size_t count = 0;
        for (const auto& entry : files_) {
            if (parentOf(entry.first) != dir) {
                continue;
            }
            const std::string& name = entry.first;
            if (name.size() >= suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                ++count;
            }
        }
        return count;
    }

    void putFile(const std::string& path, const std::vector<uint8_t>& bytes) {
        files_[path] = bytes;
        if (mod_times_.count(path) == 0) {
            mod_times_[path] = next_mod_time_++;
        }
    }

    void putText(const std::string& path, const std::string& text) {
        putFile(path, std::vector<uint8_t>(text.begin(), text.end()));
    }

    std::string readText(const std::string& path) const {
        const auto it = files_.find(path);
        if (it == files_.end()) {
            return std::string();
        }
        return std::string(it->second.begin(), it->second.end());
    }

    std::vector<uint8_t> bytesAt(const std::string& path) const {
        const auto it = files_.find(path);
        return it == files_.end() ? std::vector<uint8_t>() : it->second;
    }

    int lock_count = 0;
    int unlock_count = 0;

private:
    bool mounted_ = false;
    uint32_t next_mod_time_ = 1;
    std::map<std::string, std::vector<uint8_t>> files_;
    std::set<std::string> dirs_;
    std::map<std::string, uint32_t> mod_times_;
};

}  // namespace vm_test
