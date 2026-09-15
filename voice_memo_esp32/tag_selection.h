#pragma once

// Selected vs frozen tag, as a pure state holder.
//
// Two distinct values are deliberately kept apart:
//
//   selected()  - what the next recording will be tagged with; changed by the
//                 touch screen.
//   recording() - a copy taken by freeze() when a recording starts, so the tag
//                 of audio that already exists can never be rewritten by a later
//                 tap.
//
// RecordingApp owns one instance, so the host test in tests/ui_model_test.cpp
// exercises the exact rules the firmware runs. No Arduino dependency.

#include "recording_state.h"
#include "voice_tags.h"

namespace voice_memo_firmware {

class TagSelection {
public:
    VoiceTag selected() const { return selected_; }
    VoiceTag recording() const { return recording_; }

    // Touch-driven selection. Refused - and left unchanged - unless the state
    // allows a new recording (buffer free), which is the same condition that
    // guards startRecording().
    bool select(VoiceTag desired, AppState state) {
        if (!allows_new_recording(state)) {
            return false;
        }
        selected_ = desired;
        return true;
    }

    // Freeses the current selection as the tag of the recording about to start.
    VoiceTag freeze() {
        recording_ = selected_;
        return recording_;
    }

private:
    VoiceTag selected_ = VoiceTag::Work;
    VoiceTag recording_ = VoiceTag::Work;
};

}  // namespace voice_memo_firmware
