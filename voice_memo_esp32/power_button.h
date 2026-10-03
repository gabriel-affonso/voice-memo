#pragma once

// Debounced PWR (BAT_KEY, GPIO18) key with mandatory post-boot arming.
//
// Why this file is Arduino free
// -----------------------------
// The arming rule is the single most dangerous piece of this firmware: get it
// wrong and "hold PWR to power on, then release" is read as "short press to
// power off", so the device switches itself off the instant the user lets go of
// the key. That rule therefore lives in a pure class which the host test can
// drive millisecond by millisecond, and the firmware merely feeds it the pin
// level and millis().
//
// The arming rule
// ---------------
//   DISARMED_AFTER_BOOT  the only state at reset. A press here is ignored
//                        (and remembered, so it can never be counted twice).
//   ARMED                entered ONLY after the key has been observed
//                        electrically RELEASED and stable for the full
//                        debounce window. From here on, one clean
//                        press -> release cycle emits exactly one request.
//
// Consequences that the tests pin down:
//   * power-on press: the key is still down when setup() runs, so the state
//     stays DISARMED_AFTER_BOOT until the user lets go. The release arms the
//     button, it never requests a shutdown.
//   * a press that starts before arming is ignored, so the power-on press can
//     never become the power-off press - not even if the user holds the key for
//     a very long time and then releases it.
//   * a bounce (a run of opposite samples shorter than the debounce window)
//     never produces an edge at all, so one gesture cannot emit two requests.
//   * holding the key down produces exactly one event (the press) and nothing
//     else until it is actually released.

#include <cstdint>

#include "config.h"

namespace voice_memo_firmware {

class PowerButton {
public:
    enum class Event {
        None,
        // The key has been electrically down for a full debounce window while
        // ARMED. This is the only event that may start a shut-down request.
        ShortPress,
        // The key has been electrically up for a full debounce window while
        // ARMED.
        Released,
        // Same as Released, but it happened while DISARMED_AFTER_BOOT: it is the
        // transition that arms the button. Reported separately so the log can
        // say so, and so it can never be mistaken for a shutdown trigger.
        Armed,
    };

    // `activeLow` matches VM_PWR_KEY_ACTIVE_LOW (the confirmed GPIO18 polarity:
    // pressed = LOW). `debounceMs` is the window an input must hold before it
    // counts; 0 is clamped to 1 so a zero-debounce build cannot alias a sample.
    PowerButton(bool activeLow, uint32_t debounceMs);

    // One sample. `rawLevel` is the raw GPIO level (LOW == 0, HIGH == 1), not a
    // logical "pressed" flag, because the debounce rule has to be applied to the
    // electrical signal before the polarity is interpreted. Returns every event
    // that became true on this sample; a sample can arm the button, but arming
    // and a press can never happen on the same call.
    Event update(uint32_t nowMs, int rawLevel);

    // Debounced physical state of the key: true while it reads pressed. Note
    // this is the debounced *level*, not "a press was accepted" - while the
    // button is still DISARMED_AFTER_BOOT a held key reports true here and still
    // produces no event, which is exactly the distinction that makes the boot
    // arming safe. Only events, never this flag, may request a shutdown.
    bool isPressed() const {
        return stableKnown_ && (activeLow_ ? !stableRaw_ : stableRaw_);
    }

    // False until the post-boot release has been confirmed. Exposed so the log
    // can state it once and so the power manager can report "not armed yet".
    bool isArmed() const { return state_ == State::Armed; }

private:
    enum class State {
        DisarmedAfterBoot,
        Armed,
    };

    bool activeLow_;
    uint32_t debounceMs_;

    State state_ = State::DisarmedAfterBoot;

    bool stableRaw_ = false;      // debounced raw level: false = LOW, true = HIGH
    bool stableKnown_ = false;    // false until the first level has been committed
    bool candidateRaw_ = false;   // level the pin has to keep showing
    bool sampled_ = false;        // false until the first update() call
    uint32_t candidateSinceMs_ = 0;
};

inline PowerButton::PowerButton(bool activeLow, uint32_t debounceMs)
    : activeLow_(activeLow), debounceMs_(debounceMs == 0 ? 1 : debounceMs) {
    // The debounced state starts UNKNOWN rather than at a guessed idle level, so
    // the first stable reading is always a change that gets observed (and, for a
    // released key, arms the button). Seeding it with an idle level would make
    // the firmware treat "nothing happened yet" as an already-committed release,
    // which is exactly the assumption the boot arming exists to avoid.
    stableRaw_ = false;
    candidateRaw_ = false;
}

inline PowerButton::Event PowerButton::update(uint32_t nowMs, int rawLevel) {
    const bool raw = rawLevel != 0;

    // The first sample only starts the stability window and emits nothing. It
    // must not be able to "commit" on its own, and it must not be able to arm the
    // button either: at t=0 the firmware knows nothing about the key yet.
    if (!sampled_) {
        sampled_ = true;
        candidateRaw_ = raw;
        candidateSinceMs_ = nowMs;
        return Event::None;
    }

    if (raw != candidateRaw_) {
        // A new candidate level starts its stability window. The debounced
        // state is deliberately left untouched: a spike shorter than the window
        // must not be able to change anything, in either direction.
        candidateRaw_ = raw;
        candidateSinceMs_ = nowMs;
        return Event::None;
    }

    // Unsigned difference: correct across the millis() wrap.
    if (static_cast<uint32_t>(nowMs - candidateSinceMs_) < debounceMs_) {
        return Event::None;
    }

    if (stableKnown_ && candidateRaw_ == stableRaw_) {
        return Event::None;  // already committed; nothing changed
    }

    // The candidate held for a full window: commit it. The first commit can only
    // ever arm the button, never request a shutdown (see the pressed branch).
    stableRaw_ = candidateRaw_;
    stableKnown_ = true;
    const bool pressed = activeLow_ ? !stableRaw_ : stableRaw_;

    if (pressed) {
        // A press that was already down when the firmware started, or that began
        // before the button was armed, is not a request: it is ignored and the
        // button stays disarmed until it is seen released. This is what makes
        // "hold PWR to power on, then let go" safe.
        return state_ == State::Armed ? Event::ShortPress : Event::None;
    }

    if (state_ == State::DisarmedAfterBoot) {
        state_ = State::Armed;
        return Event::Armed;
    }
    return Event::Released;
}

}  // namespace voice_memo_firmware
