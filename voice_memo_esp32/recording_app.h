#pragma once

#include <Arduino.h>
#include <cstdint>

#include "audio_capture.h"
#include "recording_id.h"
#include "button.h"
#include "device_id.h"
#include "uploader.h"
#include "wav_recording.h"
#include "wifi_manager.h"

class RecordingApp {
public:
    RecordingApp(
        Button& button,
        DeviceIdProvider& deviceId,
        AudioCapture& audio,
        WavRecordingBuffer& buffer,
        Esp32IngressUploader& uploader,
        WifiManager& wifi
    );

    void begin();
    void tick();

private:
    enum class State {
        Idle,
        Recording,
        MaxReachedWaitingRelease,
        RetryWait,
    };

    void startRecording();
    void stopRecordingAndFinalize(bool maxReached);
    void tryUpload();
    void clearRecording();

    Button& button_;
    DeviceIdProvider& deviceIdProvider_;
    AudioCapture& audio_;
    WavRecordingBuffer& buffer_;
    Esp32IngressUploader& uploader_;
    voice_memo_firmware::RecordingIdGenerator recordingIdGenerator_;
    WifiManager& wifi_;

    State state_ = State::Idle;
    String deviceId_;
    String recordingId_;
    uint32_t sampleCount_ = 0;
    uint32_t durationMs_ = 0;
    uint32_t nextProgressSample_ = 0;
    int32_t peakSample_ = 0;
    uint64_t sumSquares_ = 0;
    unsigned long nextRetryMs_ = 0;
    unsigned long nextWifiNoticeMs_ = 0;
    unsigned long nextPendingNoticeMs_ = 0;
    uint32_t retryAttempt_ = 0;
    bool maxReached_ = false;
};
