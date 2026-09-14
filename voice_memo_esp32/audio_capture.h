#pragma once

#include <Arduino.h>
#include <driver/i2s_common.h>
#include <driver/i2s_std.h>

class AudioCapture {
public:
    bool begin();
    bool start();
    size_t read(uint8_t* dest, size_t maxBytes, uint32_t timeoutMs);
    void stop();
    bool isReady() const;

private:
    // The TX channel is created by i2s_new_channel but deliberately never
    // initialized or enabled: playback is not used in this firmware. Track
    // enabled state per channel so disable is only ever issued for a channel
    // that is actually enabled.
    bool enableChannel(i2s_chan_handle_t handle, const char* label);
    void disableChannel(i2s_chan_handle_t handle, bool& enabled, const char* label);

    i2s_chan_handle_t rxHandle_ = nullptr;
    i2s_chan_handle_t txHandle_ = nullptr;
    bool rxEnabled_ = false;
    bool txEnabled_ = false;
    bool ready_ = false;
};
