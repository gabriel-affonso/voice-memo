#pragma once

#include <Arduino.h>

class WavRecordingBuffer {
public:
    static constexpr size_t kHeaderSize = 44;

    bool allocate(size_t maxPayloadBytes);
    bool valid() const;
    void reset();
    void release();

    uint8_t* payload();
    const uint8_t* data() const;
    size_t payloadCapacity() const;
    size_t totalBytes() const;
    void finalize(size_t payloadBytes);

private:
    uint8_t* data_ = nullptr;
    size_t capacityPayload_ = 0;
    size_t payloadBytes_ = 0;
};
