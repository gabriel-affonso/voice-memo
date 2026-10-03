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
    // Buffer free; waiting for a BOOT press. The persistent queue may be
    // non-empty: uploads from the microSD card run in the background and are
    // deliberately invisible to this state machine.
    Idle,
    // Actively capturing into the PSRAM buffer. In toggle-to-record the
    // recording continues after the finger is lifted; a second BOOT press
    // finalizes it.
    Recording,
    // Legacy hold-to-record state: "the limit was hit, waiting for the button
    // to be released". Retained because the UI model and the power policy still
    // map it (ui_model.h, power_policy.h), but the recorder no longer enters
    // it: the safety timeout finalizes the WAV and hands it straight to the
    // store, exactly like a manual stop, so there is no release tail left.
    MaxReachedWaitingRelease,
    // Finalized WAV is being committed to the microSD card. Short, and the only
    // state where the buffer is frozen without an upload owning it. A new
    // recording is refused because the single PSRAM buffer is still the only
    // copy of the audio until the commit returns.
    Saving,
    // Upload task is sending the buffered WAV; buffer is frozen. Used by the
    // volatile fallback path (no card, or the card refused the commit).
    Uploading,
    // Previous attempt failed: same WAV and recording_id are kept for a later
    // background retry; buffer stays frozen. Volatile fallback path only.
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
        case AppState::Saving:
            return "saving";
        case AppState::Uploading:
            return "uploading";
        case AppState::RetryWait:
            return "retry_wait";
    }
    return "unknown";
}

// The recording control is a *toggle*, not push-to-talk: one BOOT press starts
// a capture from Idle, and one BOOT press while Recording finalizes it. The
// release is never an action, which is what makes the capture continue after
// the finger is lifted.
//
// The button layer (button.h) already emits at most one Pressed edge per
// physical press after a debounce window, so this rule is what guarantees that
// the *release* of the press that started a recording can never be read as the
// stop of that same press: only a fresh HIGH -> LOW transition produces the
// second edge.
//
// Every other state refuses the press: Saving/Uploading/RetryWait own the only
// copy of the finalized WAV and must not have it reset by a new capture.
enum class ButtonAction {
    None,
    StartRecording,
    StopRecording,
};

constexpr ButtonAction button_action_for_state(AppState state) {
    return state == AppState::Idle
               ? ButtonAction::StartRecording
               : (state == AppState::Recording ? ButtonAction::StopRecording
                                               : ButtonAction::None);
}

// Result of a BOOT press edge on the state. Idle -> Recording starts a capture;
// every other state keeps its state, Recording included, because there the
// press is consumed as a STOP by the recorder and the finalize path moves the
// state itself. See button_action_for_state() for what the press *means*.
constexpr AppState state_after_boot_press(AppState current) {
    return current == AppState::Idle ? AppState::Recording : current;
}

// The buffer is free only in Idle, which is therefore the only state where a
// new recording (which resets the WAV) may start. With a card present this is
// the normal state: recordings are committed and then uploaded in the
// background, so the user can keep recording.
constexpr bool allows_new_recording(AppState state) {
    return state == AppState::Idle;
}

// Legacy hold-to-record transition. It is no longer part of the recorder's
// timeout path - stopRecordingAndFinalize(StopReason::Timeout) finalizes the WAV
// and goes straight to the store/upload pipeline - but it is kept, and pinned
// by the host test, because AppState::MaxReachedWaitingRelease is still named by
// the UI model and the power policy.
constexpr AppState state_after_max_duration() {
    return AppState::MaxReachedWaitingRelease;
}

// What to do with a finalized WAV when the persistent store cannot take it
// (uploads disabled, no card, card full or card unhealthy):
//   uploads disabled (TEST A) -> drop it, back to Idle;
//   Wi-Fi down                -> keep it, RetryWait;
//   otherwise                 -> start the background upload, Uploading.
constexpr AppState state_after_finalize(bool uploadEnabled, bool wifiConnected) {
    return !uploadEnabled ? AppState::Idle
                          : (wifiConnected ? AppState::Uploading : AppState::RetryWait);
}

// A successful commit hands ownership of the audio to the card, so the PSRAM
// buffer is free again and the upload happens from the queue.
constexpr AppState state_after_persist_ok() {
    return AppState::Idle;
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

// True while the buffered WAV belongs to the persist/upload path. In these
// states the main loop must not reset, finalize, overwrite or free the buffer,
// and must not change recording_id/duration_ms.
constexpr bool buffer_is_frozen(AppState state) {
    return state == AppState::Saving || state == AppState::Uploading ||
           state == AppState::RetryWait;
}

}  // namespace voice_memo_firmware
