#include "wav_recording.h"

#include "config.h"
#include "wav_format.h"

#include <esp_heap_caps.h>

#include <cstring>

bool WavRecordingBuffer::allocate(size_t maxPayloadBytes) {
    const size_t totalBytes = kHeaderSize + maxPayloadBytes;
    capacityPayload_ = maxPayloadBytes;

    data_ = static_cast<uint8_t*>(ps_malloc(totalBytes));
    if (data_ == nullptr) {
        data_ = static_cast<uint8_t*>(heap_caps_malloc(totalBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (data_ == nullptr) {
        data_ = static_cast<uint8_t*>(malloc(totalBytes));
    }
    if (data_ == nullptr) {
        capacityPayload_ = 0;
        payloadBytes_ = 0;
        return false;
    }

    std::memset(data_, 0, kHeaderSize);
    payloadBytes_ = 0;
    return true;
}

bool WavRecordingBuffer::valid() const {
    return data_ != nullptr && capacityPayload_ > 0;
}

void WavRecordingBuffer::reset() {
    if (data_ != nullptr) {
        std::memset(data_, 0, kHeaderSize);
    }
    payloadBytes_ = 0;
}

void WavRecordingBuffer::release() {
    if (data_ != nullptr) {
        free(data_);
        data_ = nullptr;
    }
    capacityPayload_ = 0;
    payloadBytes_ = 0;
}

uint8_t* WavRecordingBuffer::payload() {
    return data_ == nullptr ? nullptr : data_ + kHeaderSize;
}

const uint8_t* WavRecordingBuffer::data() const {
    return data_;
}

size_t WavRecordingBuffer::payloadCapacity() const {
    return capacityPayload_;
}

size_t WavRecordingBuffer::totalBytes() const {
    return kHeaderSize + payloadBytes_;
}

void WavRecordingBuffer::finalize(size_t payloadBytes) {
    if (data_ == nullptr) {
        payloadBytes_ = 0;
        return;
    }

    payloadBytes_ = payloadBytes;
    if (payloadBytes_ > capacityPayload_) {
        payloadBytes_ = capacityPayload_;
    }

    voice_memo_firmware::write_wav_header(
        data_,
        static_cast<uint32_t>(VM_SAMPLE_RATE),
        static_cast<uint16_t>(VM_CHANNELS),
        static_cast<uint16_t>(VM_BITS_PER_SAMPLE),
        static_cast<uint32_t>(payloadBytes_)
    );
}
