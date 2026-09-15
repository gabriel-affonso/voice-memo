#pragma once

#include <Arduino.h>
#include <cstdint>

#include "config.h"

#if VM_ENABLE_UPLOAD
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

#include "audio_capture.h"
#include "recording_id.h"
#include "button.h"
#include "device_id.h"
#include "recording_state.h"
#include "tag_selection.h"
#include "upload_status.h"
#include "uploader.h"
#include "voice_tags.h"
#include "wav_recording.h"
#include "wifi_manager.h"

class RecordingApp {
public:
    // The whole lifecycle (including Uploading/RetryWait) lives in the pure,
    // host-tested recording_state.h so the firmware and the host test share the
    // exact same transition rules. Declared first so the public observation API
    // below can use it.
    using State = voice_memo_firmware::AppState;

    // Read-only projection of the firmware handed to the UI.
    //
    // The UI needs to *observe* the real state machine, never to duplicate it:
    // every field below is copied out of RecordingApp's own members, and the two
    // counters exist so the UI can react to one-off events (a successful upload,
    // a BOOT press refused because the buffer was busy) without polling
    // millis()-based flags.
    struct UiSnapshot {
        voice_memo_firmware::AppState state = voice_memo_firmware::AppState::Idle;
        // Tag chosen for the next recording (only changed through
        // setSelectedTag(), only while Idle).
        voice_memo_firmware::VoiceTag selected_tag = voice_memo_firmware::VoiceTag::Work;
        // Copy frozen by startRecording(): the tag the buffered/uploaded audio
        // actually belongs to.
        voice_memo_firmware::VoiceTag recording_tag = voice_memo_firmware::VoiceTag::Work;
        // Live elapsed time while Recording, derived from the sample counter
        // exactly like duration_ms_for_samples() is used at finalize time.
        uint32_t live_elapsed_ms = 0;
        // Duration of the finalized WAV; stays valid through
        // Uploading/RetryWait because the buffer is frozen there.
        uint32_t frozen_duration_ms = 0;
        // Monotonic counters; the UI renders feedback when they change.
        uint32_t upload_success_count = 0;
        uint32_t refused_press_count = 0;
        uint32_t last_upload_success_ms = 0;
        uint32_t last_refused_press_ms = 0;
        bool wifi_connected = false;
        bool upload_enabled = false;
        bool buffer_ready = false;
        bool audio_ready = false;
    };

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

    // --- Read-only observation API for the UI -------------------------------
    // None of these change the behaviour of startRecording(),
    // stopRecordingAndFinalize(), startAsyncUpload(), the retry path or the
    // ownership of the single PSRAM buffer.
    UiSnapshot uiSnapshot() const;
    State state() const { return state_; }
    uint32_t recordingDurationMs() const;
    voice_memo_firmware::VoiceTag selectedTag() const { return tagSelection_.selected(); }
    voice_memo_firmware::VoiceTag recordingTag() const { return tagSelection_.recording(); }

    // The only mutation the UI is allowed to perform. Returns false (and changes
    // nothing) unless the buffer is free, i.e. state == Idle, so the tag of an
    // in-flight recording can never be rewritten from the touch screen.
    bool setSelectedTag(voice_memo_firmware::VoiceTag tag);

private:
    // Result handed from the upload task back to the main loop. Plain POD so it
    // can travel through a FreeRTOS queue by value (no owned pointers).
    struct UploadOutcome {
        uint8_t status = 0;  // cast of voice_memo_firmware::UploadStatus
    };

    void startRecording();
    void stopRecordingAndFinalize(bool maxReached);
    // Decides what happens to a finalized WAV (drop / retry / async upload)
    // without ever blocking the main loop.
    void finishRecordingAndUpload();
    // Freezes the recording and wakes the background upload task.
    // Returns false when the attempt was deferred to RetryWait instead.
    bool startAsyncUpload();
    // Non-blocking drain of the single-slot result queue.
    void pollUploadOutcome();
    void handleUploadOutcome(voice_memo_firmware::UploadStatus status);
    void clearRecording();
    void logRefusedBootPress();

#if VM_ENABLE_UPLOAD
    static void uploadTaskEntry(void* context);
    // Runs on its own FreeRTOS task; blocks on a task notification until
    // startAsyncUpload() hands it a frozen recording.
    void uploadTaskLoop();

    TaskHandle_t uploadTaskHandle_ = nullptr;
    QueueHandle_t uploadResultQueue_ = nullptr;
#endif

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

    // Tag selection. The pure TagSelection holder keeps the choice for the next
    // recording apart from the copy frozen at startRecording(), so touching the
    // screen during an upload can never rewrite existing metadata.
    voice_memo_firmware::TagSelection tagSelection_;

    // UI feedback counters. Written only from the main loop (the upload task
    // reports through a queue), so the UI can read them without locking.
    uint32_t uploadSuccessCount_ = 0;
    uint32_t lastUploadSuccessMs_ = 0;
    uint32_t refusedPressCount_ = 0;
    uint32_t lastRefusedPressMs_ = 0;
};
