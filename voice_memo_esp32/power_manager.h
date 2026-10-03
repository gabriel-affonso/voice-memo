#pragma once

// Power policy manager: the layer between the physical latch/key (BoardPower),
// the recording state machine (RecordingApp) and the main loop.
//
// Responsibilities
// ----------------
//   * own the debounced PWR key and its mandatory post-boot arming;
//   * own the user-activity timer that drives the automatic power-off;
//   * decide whether a shutdown is allowed, deferred or refused;
//   * run the graceful shutdown sequence (finalize, tear down, screen, latch).
//
// It deliberately does NOT own GPIO17 or the panel: BoardPower owns the latch
// and the UI is asked to paint the final screen, so the policy can be reasoned
// about (and tested) without either.
//
// The three outcomes of a shutdown request
// ----------------------------------------
//   ALLOWED   -> emit the request; the caller runs shutdown().
//   DEFERRED  -> the inhibit is transient (a commit in flight, or a capture that
//                is about to be finalized). The request is remembered and fires
//                by itself as soon as the inhibit clears.
//   REFUSED   -> the inhibit is a data-safety block (a recording that exists
//                only in PSRAM). Nothing is remembered; the device stays on and
//                keeps retrying, because powering off would destroy a note.
//
// A DEFERRED request caused by a capture is dropped when the capture ends,
// because that end is itself a user interaction: the user pressed BOOT to stop
// recording and did not ask to power off. A DEFERRED commit is completed.

#include <cstdint>

#include "config.h"
#include "power_button.h"
#include "power_policy.h"
#include "recording_state.h"

namespace voice_memo_firmware {

// Why a shutdown was asked for. Only used for logging and for the final screen.
enum class ShutdownReason {
    User,        // a deliberate short press of PWR
    Inactivity,  // VM_AUTO_POWER_OFF_MS with no user interaction
};

inline const char* shutdown_reason_name(ShutdownReason reason) {
    return reason == ShutdownReason::User ? "user" : "inactivity";
}

class PowerManager {
public:
    using ActivityEvent = voice_memo_firmware::ActivityEvent;
    using Inhibit = voice_memo_firmware::ShutdownInhibit;
    using Notice = voice_memo_firmware::PowerNotice;

    PowerManager(bool pwrActiveLow, uint32_t debounceMs);

    void begin(uint32_t nowMs);

    // Full context supplied by the application every loop() iteration. Keeping it
    // explicit (rather than letting PowerManager reach into RecordingApp) is what
    // keeps the policy testable.
    struct Context {
        // Inhibit derived from the recording state machine (see
        // shutdown_inhibit_for_state).
        Inhibit inhibit = Inhibit::None;
        // A data-safety block is in effect (a recording that only exists in
        // PSRAM). Used only to emit the UNSENT NOTE notice exactly once per
        // episode instead of once per loop iteration.
        bool data_at_risk = false;
        // True while Wi-Fi is up; passed to the UI for the notice wording.
        bool wifi_connected = false;
    };

    // Feeds one PWR sample, services the inactivity timer and returns true when
    // the caller must now run the graceful shutdown sequence. `takeShutdownReason`
    // says why.
    bool tick(uint32_t nowMs, int pwrRawLevel, const Context& context);

    // Registers a real user interaction. Background events (Wi-Fi, NTP, uploads,
    // retries, remounts, refreshes, logs) must NOT be reported here - see
    // activity_is_user_interaction() for the classification.
    void markActivity(ActivityEvent event, uint32_t nowMs);

    // --- output -------------------------------------------------------------
    ShutdownReason takeShutdownReason();

    // The notice the caller must show, or None. Consumed by the call.
    Notice takeNotice();

    // --- observation (logging / diagnostics) --------------------------------
    Inhibit inhibit() const { return inhibit_; }
    uint32_t lastActivityMs() const { return lastActivityMs_; }
    // Milliseconds left before the automatic power-off, or 0 when it has already
    // expired (or is disabled).
    uint32_t inactivityRemainingMs(uint32_t nowMs) const;
    bool pwrArmed() const { return button_.isArmed(); }
    bool pwrPressed() const { return button_.isPressed(); }

private:
    // Shared by the manual and automatic paths so both obey the same rules.
    // `fromManualPress` distinguishes a deliberate PWR press (which the user must
    // be told about when it is refused) from an unattended timeout.
    void requestShutdown(uint32_t nowMs, ShutdownReason reason, bool fromManualPress);

    PowerButton button_;

    // Time of the most recent real user interaction. begin() seeds it with the
    // boot instant, so the interval is always measured from something meaningful
    // and no "no activity yet" flag is needed.
    uint32_t lastActivityMs_ = 0;

    Inhibit inhibit_ = Inhibit::None;

    bool shutdownRequest_ = false;
    ShutdownReason shutdownReason_ = ShutdownReason::User;
    // True while a request is waiting on a TRANSIENT inhibit (a commit in
    // flight). It does not latch a boolean that could outlive its reason: the
    // grant condition below re-tests the current inhibit, so the moment the
    // commit finishes the request is granted without another event.
    bool shutdownDeferred_ = false;
    ShutdownReason shutdownDeferredReason_ = ShutdownReason::User;

    Notice notice_ = Notice::None;
    bool hardBlockNoticed_ = false;

    // Latches so a transition is logged once instead of every iteration.
    bool announcedTimeout_ = false;
    bool announcedRecordingInhibit_ = false;
    bool announcedSavingInhibit_ = false;
};

// ---------------------------------------------------------------------------
// Implementation (header-only: it has no Arduino dependency, so the host test
// compiles this file directly and the firmware inlines it)
// ---------------------------------------------------------------------------

inline PowerManager::PowerManager(bool pwrActiveLow, uint32_t debounceMs)
    : button_(pwrActiveLow, debounceMs) {}

inline void PowerManager::begin(uint32_t nowMs) {
    lastActivityMs_ = nowMs;
    announcedTimeout_ = false;
    // Deliberately NOT "armed": the key is DISARMED_AFTER_BOOT at this point,
    // because the level read at t=0 may be the power-on press the user is still
    // holding. The arming line is printed when the release has been debounced.
    Serial.println("[power] PWR disarmed after boot; waiting for a stable release");
}

inline void PowerManager::markActivity(ActivityEvent event, uint32_t nowMs) {
    if (!activity_is_user_interaction(event)) {
        // Defensive: a caller that mislabels background work as an interaction
        // would silently disable the automatic power-off forever, so the mistake
        // is refused here rather than trusted.
        return;
    }
    lastActivityMs_ = nowMs;
    announcedTimeout_ = false;
    hardBlockNoticed_ = false;

    // The user is demonstrably present: a queued request must not fire the
    // instant the inhibit clears, because that is not an unattended timeout any
    // more, it is a power cut in the user's face. A request that has already been
    // granted is left alone (its owner is in shutdown() by then).
    shutdownDeferred_ = false;
}

inline void PowerManager::requestShutdown(uint32_t nowMs, ShutdownReason reason, bool fromManualPress) {
    (void)nowMs;

    if (shutdown_inhibit_is_hard(inhibit_)) {
        // Never queue this: a hard block means powering off destroys the only
        // copy of a note. The request is dropped and the user is told once.
        Serial.printf("[power] shutdown blocked: %s\n", shutdown_inhibit_name(inhibit_));
        if (!hardBlockNoticed_) {
            hardBlockNoticed_ = true;
            notice_ = Notice::UnsentNote;
        }
        return;
    }

    if (inhibit_ == Inhibit::Recording) {
        // Refused, and deliberately NOT remembered. PWR is not a recording
        // control: the user must stop the capture with BOOT first. Queueing the
        // request would mean the device powers itself off the moment the
        // recording is committed, which the user never asked for.
        Serial.printf("[power] shutdown deferred: %s\n", shutdown_inhibit_name(inhibit_));
        if (fromManualPress) {
            notice_ = Notice::StopRecordingFirst;
        }
        return;
    }

    if (inhibit_ == Inhibit::Saving || inhibit_ == Inhibit::FilesystemCritical) {
        // Transient and safe to remember: the work completes, and then the
        // shutdown continues. A commit is never abandoned halfway.
        Serial.printf("[power] shutdown deferred: %s\n", shutdown_inhibit_name(inhibit_));
        shutdownRequest_ = true;
        shutdownDeferred_ = true;
        shutdownDeferredReason_ = reason;
        return;
    }

    Serial.printf("[power] shutdown requested reason=%s\n", shutdown_reason_name(reason));
    shutdownRequest_ = true;
    shutdownDeferred_ = false;
    shutdownReason_ = reason;
}

inline bool PowerManager::tick(uint32_t nowMs, int pwrRawLevel, const Context& context) {
    inhibit_ = context.inhibit;

    // The two transient states are announced once per episode, so the log shows
    // why a request did not become a power-off without a line per loop.
    if (context.inhibit == Inhibit::Recording) {
        if (!announcedRecordingInhibit_) {
            announcedRecordingInhibit_ = true;
            Serial.println("[power] capture in progress: shutdown requests are refused");
        }
    } else {
        announcedRecordingInhibit_ = false;
    }
    if (context.inhibit == Inhibit::Saving) {
        if (!announcedSavingInhibit_) {
            announcedSavingInhibit_ = true;
            Serial.println("[power] commit in progress: shutdown requests are deferred");
        }
    } else {
        announcedSavingInhibit_ = false;
    }

    // ---- PWR key -----------------------------------------------------------
    switch (button_.update(nowMs, pwrRawLevel)) {
        case PowerButton::Event::Armed:
            // begin() logs the same line, but that one is an assumption about
            // the pin at t=0 while this one is the debounced observation. Both
            // are useful when diagnosing a stuck key.
            Serial.println("[power] PWR release confirmed; shutdown button armed");
            break;
        case PowerButton::Event::ShortPress:
            Serial.println("[power] PWR short press");
            markActivity(ActivityEvent::PwrButton, nowMs);
#if VM_ENABLE_MANUAL_POWER_OFF
            requestShutdown(nowMs, ShutdownReason::User, true);
#endif
            break;
        case PowerButton::Event::Released:
            break;
        case PowerButton::Event::None:
            break;
    }

    // ---- inactivity --------------------------------------------------------
    if (inactivity_timeout_expired(nowMs, lastActivityMs_, VM_AUTO_POWER_OFF_MS)) {
        if (!announcedTimeout_) {
            announcedTimeout_ = true;
            Serial.printf("[power] inactivity timeout %lu ms\n",
                          static_cast<unsigned long>(VM_AUTO_POWER_OFF_MS));
        }
#if VM_ENABLE_AUTO_POWER_OFF
        // A timeout during a capture or a commit is refused, not queued. The end
        // of the recording is itself a user interaction and restarts the
        // interval, so the device is never switched off mid-capture; a commit is
        // short and the timer simply fires a moment later.
        // `!shutdownRequest_` keeps this to one request and one log line per
        // expired interval: a request already waiting on a soft block (or just
        // granted) must not be re-issued on every loop iteration.
        if (inhibit_ == Inhibit::None && !shutdownRequest_) {
            requestShutdown(nowMs, ShutdownReason::Inactivity, false);
        } else if (shutdown_inhibit_is_hard(inhibit_)) {
            if (!hardBlockNoticed_) {
                hardBlockNoticed_ = true;
                notice_ = Notice::UnsentNote;
            }
        }
#endif
    }

    // ---- decide whether this iteration must power the device off -----------
    // A deferred request is granted as soon as its inhibit clears, which is what
    // "conclude the commit and then proceed to power-off" means. It is not
    // granted while the inhibit is still active, and it is not lost either: the
    // request stays pending for as long as the block lasts.
    if (shutdownDeferred_) {
        if (!shutdown_inhibit_is_soft(inhibit_)) {
            return false;  // still committing, still capturing, or a hard block
        }
        Serial.printf("[power] deferred shutdown proceeding reason=%s\n",
                      shutdown_reason_name(shutdownDeferredReason_));
        shutdownReason_ = shutdownDeferredReason_;
        shutdownDeferred_ = false;
    }

    return shutdownRequest_ && inhibit_ == Inhibit::None;
}

inline ShutdownReason PowerManager::takeShutdownReason() {
    shutdownRequest_ = false;
    shutdownDeferred_ = false;
    return shutdownReason_;
}

inline PowerManager::Notice PowerManager::takeNotice() {
    const Notice notice = notice_;
    notice_ = Notice::None;
    return notice;
}

inline uint32_t PowerManager::inactivityRemainingMs(uint32_t nowMs) const {
#if VM_ENABLE_AUTO_POWER_OFF
    const uint32_t elapsed = static_cast<uint32_t>(nowMs - lastActivityMs_);
    if (elapsed >= VM_AUTO_POWER_OFF_MS) {
        return 0;
    }
    return VM_AUTO_POWER_OFF_MS - elapsed;
#else
    (void)nowMs;
    return 0;
#endif
}

}  // namespace voice_memo_firmware
