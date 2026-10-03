#pragma once

// A sequential, read-only byte source for the multipart upload body.
//
// The uploader never needs the whole WAV in RAM: it frames the multipart
// headers, then copies the audio through a small fixed chunk buffer. Two
// sources implement that:
//
//   * MemoryByteSource - the finalized PSRAM WAV (the legacy/volatile path);
//   * the SD file reader in recording_app.cpp, which reads one chunk at a time
//     from the persistent queue while holding the filesystem lock only for that
//     read (never across HTTP).
//
// Arduino free, so the framing logic can be reasoned about (and, later, tested)
// without a board.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace voice_memo_firmware {

class ByteSource {
public:
    virtual ~ByteSource() {}
    // Total number of bytes this source will produce.
    virtual size_t size() const = 0;
    // Copies up to `maxBytes` into `dest` and returns how many were produced.
    // Returns 0 at end of stream or on error.
    virtual size_t read(uint8_t* dest, size_t maxBytes) = 0;
};

class MemoryByteSource : public ByteSource {
public:
    MemoryByteSource(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    size_t size() const override { return size_; }

    size_t read(uint8_t* dest, size_t maxBytes) override {
        if (data_ == nullptr) {
            return 0;
        }
        const size_t remaining = size_ - offset_;
        const size_t want = maxBytes < remaining ? maxBytes : remaining;
        if (want == 0) {
            return 0;
        }
        std::memcpy(dest, data_ + offset_, want);
        offset_ += want;
        return want;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t offset_ = 0;
};

}  // namespace voice_memo_firmware
