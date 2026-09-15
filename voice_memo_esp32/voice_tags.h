#pragma once

// Central definition of the four voice-memo tags.
//
// The labels live here only. Nothing else in the firmware should hardcode
// "Operas" / "Tese" / "To-do" / "Eu", so renaming a tag (or adding a fourth
// language) is a one-line change in this file.
//
// No Arduino dependency: the tag logic is exercised by the host test
// tests/ui_model_test.cpp.

#include <cstdint>

namespace voice_memo_firmware {

// The enumerator names are stable identifiers (serial log, tests, selection
// state); voiceTagLabel() is the only thing that decides what the panel shows.
enum class VoiceTag {
    Work,
    Idea,
    Todo,
    Personal,
};

// Number of entries in VoiceTag. Used to size the touch grid and to validate
// values coming from outside this file.
constexpr uint8_t kVoiceTagCount = 4;

// Human readable label for the e-paper UI and the serial log. Never null.
inline const char* voiceTagLabel(VoiceTag tag) {
    switch (tag) {
        case VoiceTag::Work:
            return "Operas";
        case VoiceTag::Idea:
            return "Tese";
        case VoiceTag::Todo:
            return "To-do";
        case VoiceTag::Personal:
            return "Eu";
    }
    return "?";
}

// Stable index 0..kVoiceTagCount-1. Used as the key for the touch hitboxes.
inline uint8_t voiceTagIndex(VoiceTag tag) {
    switch (tag) {
        case VoiceTag::Work:
            return 0;
        case VoiceTag::Idea:
            return 1;
        case VoiceTag::Todo:
            return 2;
        case VoiceTag::Personal:
            return 3;
    }
    return 0;
}

// Inverse of voiceTagIndex(). Returns VoiceTag::Work for out-of-range input so
// a corrupt value can never index outside the tag grid.
inline VoiceTag voiceTagFromIndex(uint8_t index) {
    switch (index) {
        case 0:
            return VoiceTag::Work;
        case 1:
            return VoiceTag::Idea;
        case 2:
            return VoiceTag::Todo;
        case 3:
            return VoiceTag::Personal;
    }
    return VoiceTag::Work;
}

}  // namespace voice_memo_firmware
