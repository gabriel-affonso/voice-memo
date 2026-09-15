#pragma once

// Pure state machine for the voice-memo recording/upload lifecycle.
//
// This header intentionally has no Arduino, Wi-Fi or FreeRTOS dependency (the
// same idea as ingest_target.h), so the exact transitions the firmware runs can
// be exercised on the host without a board:
//
//     c++ -std=c++11 -Wall -Wextra -o /tmp/recording_state_test tests/recording_state_test.cpp
//     /tmp/recording_state_test
//
// Hard constraint modelled here: the firmware owns exactly one PSRAM WAV
// buffer. While an upload (or the wait before a retry) owns that buffer, no new
// recording may start, because starting one would reset and overwrite the WAV
// that is still being sent to the ingress.

#include "upload_status.h"

namespace voice_memo_firmware {

enum class AppState {
    // Buffer free; waiting for a BOOT press.
    Idle,
    // Actively capturing into the PSRAM buffer.
    Recording,
    // 45 s limit hit: WAV finalized, waiting for the pushed button to be
    // released so push-to-talk stays consistent.
    MaxReachedWaitingRelease,
    // Upload task is sending the buffered WAV; buffer is frozen.
    Uploading,
    // Previous attempt failed: same WAV and recording_id are kept for a later
    // background retry; buffer stays frozen.
    RetryWait,
};

// Short name for diagnostic logs. Never returns null.
inline const char* app_state_name(AppState state) {
    switch (state) {
        case AppState::Idle:
            return "idle";
        case AppState::Recording:
            return "recording";
        case AppState::MaxReachedWaitingRelease:
            return "max_waiting_release";
        case AppState::Uploading:
            return "uploading";
        case AppState::RetryWait:
            return "retry_wait";
    }
    return "unknown";
}

// A BOOT press starts a recording only from Idle. Everywhere else the press is
// refused: Recording is already capturing, and Uploading/RetryWait must leave
// the buffered WAV untouched for the upload task.
constexpr AppState state_after_boot_press(AppState current) {
    return current == AppState::Idle ? AppState::Recording : current;
}

// The buffer is free only in Idle, which is therefore the only state where a
// new recording (which resets the WAV) may start.
constexpr bool allows_new_recording(AppState state) {
    return state == AppState::Idle;
}

// Hitting the recording limit finalizes the WAV immediately but keeps
// push-to-talk semantics: the pending release must not trigger a second upload,
// so the app waits for it.
constexpr AppState state_after_max_duration() {
    return AppState::MaxReachedWaitingRelease;
}

// What to do with a finalized WAV:
//   uploads disabled (TEST A) -> drop it, back to Idle;
//   Wi-Fi down                -> keep it, RetryWait;
//   otherwise                 -> start the background upload, Uploading.
constexpr AppState state_after_finalize(bool uploadEnabled, bool wifiConnected) {
    return !uploadEnabled ? AppState::Idle
                          : (wifiConnected ? AppState::Uploading : AppState::RetryWait);
}

// Result published by the background upload task. Accepted/AlreadyKnown means
// the ingress has the audio, so the buffer can be freed. Failed keeps the same
// WAV and recording_id for a later attempt.
constexpr AppState state_after_upload_result(UploadStatus status) {
    return status == UploadStatus::Failed ? AppState::RetryWait : AppState::Idle;
}

// A due retry only starts the task when Wi-Fi is usable; otherwise the
// recording stays buffered and the state stays RetryWait.
constexpr AppState state_when_retry_due(bool wifiConnected) {
    return wifiConnected ? AppState::Uploading : AppState::RetryWait;
}

// True while the buffered WAV belongs to the upload path (in-flight upload or
// pending retry). In these states the main loop must not reset, finalize,
// overwrite or free the buffer, and must not change recording_id/duration_ms.
constexpr bool buffer_is_frozen(AppState state) {
    return state == AppState::Uploading || state == AppState::RetryWait;
}

}  // namespace voice_memo_firmware
