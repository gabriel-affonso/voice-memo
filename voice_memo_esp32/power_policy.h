#pragma once

// Pure power policy: what counts as user activity, what may block a shutdown,
// and whether the inactivity timeout has expired. No Arduino, no FreeRTOS, no
// millis() call of its own - every input is a plain value - so tests can drive
// the whole policy from a fake clock.
//
// Two rules live here and nowhere else:
//
//  1. The inactivity timer measures "how long since the *user* interacted", not
//     "how long since the firmware did something". A Wi-Fi reconnect, an NTP
//     reply, an RTC read, an upload starting or finishing, a retry, an SD
//     remount, an e-paper refresh, a pending-count change and a log line are all
//     background work and must never keep the device awake forever.
//
//  2. A shutdown request is refused (never silently dropped) while any state
//     holds audio that exists nowhere but PSRAM, and deferred while a commit is
//     in flight. The distinction matters: deferring completes the work and then
//     powers off, refusing keeps the device alive because powering off would
//     destroy the only copy of a note.

#include <cstdint>

#include "config.h"
#include "recording_state.h"

namespace voice_memo_firmware {

// ---------------------------------------------------------------------------
// Activity
// ---------------------------------------------------------------------------

// Every event that touches the inactivity timer. The enum exists so that
// "should this reset the timer?" is a question answered once, in source, for
// every event the firmware can produce - rather than an implicit decision made
// by whoever happens to call a reset function.
enum class ActivityEvent {
    // --- user interactions: these reset the inactivity timer ----------------
    TouchTap,          // a valid FT6336 press edge
    TagSelected,       // the touch actually changed the selected tag
    BootButton,        // BOOT press or release (the recording control)
    RecordingStarted,  // BOOT started a capture
    // The user's second BOOT press finalized the capture (toggle-to-record).
    // Never emitted for the safety timeout: that end is not an interaction, and
    // counting it would keep a device awake that nobody is holding.
    RecordingStopped,
    PwrButton,         // a PWR interaction the user can perceive
    UiInteraction,     // any future explicit UI interaction

    // --- background events: these must NOT reset the timer ------------------
    WifiConnected,
    WifiDisconnected,
    NtpSynced,
    RtcRead,
    UploadStarted,
    UploadFinished,
    UploadRetryScheduled,
    SdRemounted,
    EpdRefreshed,
    PendingCountChanged,
    LogLine,
    InternalTimer,
};

// The single source of truth for the classification. Written as a switch over
// the whole enum (no default) so adding an event without classifying it is a
// -Wswitch warning in every build, which is how the build enforces the rule.
inline bool activity_is_user_interaction(ActivityEvent event) {
    switch (event) {
        case ActivityEvent::TouchTap:
        case ActivityEvent::TagSelected:
        case ActivityEvent::BootButton:
        case ActivityEvent::RecordingStarted:
        case ActivityEvent::RecordingStopped:
        case ActivityEvent::PwrButton:
        case ActivityEvent::UiInteraction:
            return true;

        case ActivityEvent::WifiConnected:
        case ActivityEvent::WifiDisconnected:
        case ActivityEvent::NtpSynced:
        case ActivityEvent::RtcRead:
        case ActivityEvent::UploadStarted:
        case ActivityEvent::UploadFinished:
        case ActivityEvent::UploadRetryScheduled:
        case ActivityEvent::SdRemounted:
        case ActivityEvent::EpdRefreshed:
        case ActivityEvent::PendingCountChanged:
        case ActivityEvent::LogLine:
        case ActivityEvent::InternalTimer:
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Power notices (what the user is told when a request cannot be honoured)
// ---------------------------------------------------------------------------

// Feedback vocabulary shared by the power manager (which decides) and the UI
// (which paints). Deliberately not an enum in ui_model.h: the UI must not be the
// place where "why was I refused" is decided.
enum class PowerNotice {
    None,
    // A manual PWR press arrived while audio was being captured. The request is
    // refused (never queued) and the user is told to stop recording first. PWR is
    // never a stop control - BOOT is.
    StopRecordingFirst,
    // A shutdown was refused because a recording exists only in PSRAM and the
    // ingress has not accepted it yet. The device deliberately stays on rather
    // than silently losing the note.
    UnsentNote,
};

inline const char* power_notice_name(PowerNotice notice) {
    switch (notice) {
        case PowerNotice::None:
            return "none";
        case PowerNotice::StopRecordingFirst:
            return "stop_recording_first";
        case PowerNotice::UnsentNote:
            return "unsent_note";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Shutdown inhibition
// ---------------------------------------------------------------------------// Why a shutdown is not allowed right now. One enum instead of a pile of
// independent booleans, so "can we power off?" has a single answer and the UI
// and the log can name the reason.
enum class ShutdownInhibit {
    // Nothing holds data that only exists in RAM: powering off is safe. A
    // non-empty pending/ queue is NOT an inhibit - those recordings are already
    // durable on the card and are recovered on the next boot.
    None,
    // Hard block. The only copy of a recording lives in PSRAM and has not been
    // confirmed by the ingress (states Uploading / RetryWait, i.e. the
    // no-microSD fallback path). Powering off here loses a note, so the device
    // stays on and keeps retrying. Never overridable.
    VolatileUnsavedRecording,
    // Soft block. Audio is being captured: finalize it first. The user must
    // release BOOT; a shutdown request is refused, not queued.
    Recording,
    // Soft block. A commit is in flight (WAV/metadata -> tmp -> rename ->
    // pending). The request is deferred and completes as soon as the commit
    // returns; a commit is never abandoned halfway.
    Saving,
    // Soft block. Short-lived maintenance (a remount, a chunk read) is touching
    // the volume. Deferred, like Saving.
    FilesystemCritical,
};

inline const char* shutdown_inhibit_name(ShutdownInhibit reason) {
    switch (reason) {
        case ShutdownInhibit::None:
            return "none";
        case ShutdownInhibit::VolatileUnsavedRecording:
            return "volatile_unsaved_recording";
        case ShutdownInhibit::Recording:
            return "recording";
        case ShutdownInhibit::Saving:
            return "saving";
        case ShutdownInhibit::FilesystemCritical:
            return "filesystem_critical";
    }
    return "unknown";
}

// True when the reason is a data-safety block rather than a "not yet" block.
// The distinction is what lets a deferred request be completed later while a
// refused one is dropped.
constexpr bool shutdown_inhibit_is_hard(ShutdownInhibit reason) {
    return reason == ShutdownInhibit::VolatileUnsavedRecording;
}

// True while a shutdown request should be HELD rather than granted or dropped:
// the work in progress is safe to finish, and finishing it is what makes the
// shutdown possible ("conclude the commit, then proceed to power-off"). Never
// true for the hard block, which is refused rather than held, and never true
// when nothing is in the way.
inline bool shutdown_inhibit_is_soft(ShutdownInhibit reason) {
    return reason == ShutdownInhibit::Recording || reason == ShutdownInhibit::Saving ||
           reason == ShutdownInhibit::FilesystemCritical;
}

// Maps the recording state machine onto an inhibit reason.
//
//   Idle                     -> None. Any number of persisted recordings may be
//                               waiting in pending/; that is not a reason to
//                               stay awake.
//   Recording                -> Recording.
//   MaxReachedWaitingRelease -> Recording (still inside the capture gesture;
//                               finalized but not yet handed to the store).
//   Saving                   -> Saving (defer, never abandon the commit).
//   Uploading / RetryWait    -> VolatileUnsavedRecording: on these states the
//                               buffered WAV exists only in PSRAM and has not
//                               been accepted by the ingress yet.
inline ShutdownInhibit shutdown_inhibit_for_state(AppState state) {
    switch (state) {
        case AppState::Idle:
            return ShutdownInhibit::None;
        case AppState::Recording:
        case AppState::MaxReachedWaitingRelease:
            return ShutdownInhibit::Recording;
        case AppState::Saving:
            return ShutdownInhibit::Saving;
        case AppState::Uploading:
        case AppState::RetryWait:
            return ShutdownInhibit::VolatileUnsavedRecording;
    }
    return ShutdownInhibit::None;
}

// ---------------------------------------------------------------------------
// Inactivity timeout
// ---------------------------------------------------------------------------

// millis()-safe elapsed test: `now - last` is unsigned, so the comparison stays
// correct when the 32-bit counter wraps (0xFFFFFFFF -> 0). Never write
// `now > last + timeout`, which breaks at the wrap.
constexpr bool inactivity_timeout_expired(uint32_t nowMs, uint32_t lastActivityMs, uint32_t timeoutMs) {
    return static_cast<uint32_t>(nowMs - lastActivityMs) >= timeoutMs;
}

// One stop for the whole decision. Auto power-off happens only when the timeout
// has genuinely expired AND nothing holds data that a power cut would destroy.
//
// While the timeout is unexpired this is false regardless of the inhibit, so a
// user who is actively using the device is never interrupted. While an inhibit
// is active the timeout keeps its real age: nothing resets it, so the moment the
// inhibit clears the shutdown proceeds without waiting another two minutes.
constexpr bool auto_power_off_due(
    uint32_t nowMs,
    uint32_t lastActivityMs,
    uint32_t timeoutMs,
    bool autoOffEnabled,
    ShutdownInhibit inhibit
) {
    return autoOffEnabled && inactivity_timeout_expired(nowMs, lastActivityMs, timeoutMs) &&
           inhibit == ShutdownInhibit::None;
}

}  // namespace voice_memo_firmware
