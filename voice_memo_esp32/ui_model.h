#pragma once

// Pure UI decision logic: which screen represents the firmware, when the panel
// must be fully refreshed, and how a touch point maps onto a tag.
//
// Everything in this header is free of Arduino, WiFi, FreeRTOS and e-paper
// dependencies, so tests/ui_model_test.cpp can verify the rules on the Mac:
//
//     c++ -std=c++11 -Wall -Wextra -o /tmp/ui_model_test tests/ui_model_test.cpp
//     /tmp/ui_model_test
//
// Hard rule modelled here: the UI never owns state. `UiModel` is a read-only
// projection of RecordingApp, and the only action the touch layer may perform
// is changing the selected tag, which `ui_tag_selection_allowed()` restricts to
// the states where the single PSRAM WAV buffer is free.

#include <cstdint>

// config.h only contains preprocessor constants (no Arduino headers), so the
// UI tuning knobs can stay in the project's single configuration file.
#include "config.h"
#include "recording_state.h"
#include "ui_layout.h"
#include "voice_tags.h"

namespace voice_memo_ui {

// One screen per firmware state, plus the feedback screens the UI adds:
//   Sent       : transient "SENT" confirmation after a successful upload
//   ReadyOffline: Idle while Wi-Fi is down (the firmware still allows one
//                 recording, which will then wait in the buffer)
//   PowerOff   : the shutdown screen. Painted once, immediately before the
//                 battery latch is released, and deliberately kept on the panel
//                 by the e-paper's bistability with no power at all.
enum class UiScreen {
    Ready,
    ReadyOffline,
    Recording,
    MaxReached,
    Saving,
    Uploading,
    RetryWait,
    Sent,
    PowerOff,
};

// Short name for serial diagnostics. Never null.
inline const char* ui_screen_name(UiScreen screen) {
    switch (screen) {
        case UiScreen::Ready:
            return "ready";
        case UiScreen::ReadyOffline:
            return "ready_offline";
        case UiScreen::Recording:
            return "recording";
        case UiScreen::MaxReached:
            return "max_reached";
        case UiScreen::Saving:
            return "saving";
        case UiScreen::Uploading:
            return "uploading";
        case UiScreen::RetryWait:
            return "retry_wait";
        case UiScreen::Sent:
            return "sent";
        case UiScreen::PowerOff:
            return "power_off";
    }
    return "unknown";
}

// Read-only projection of the firmware that the renderer consumes. It contains
// no state of its own: `state` and `tag` come straight from RecordingApp, and
// the two transient flags are owned by UiController (they are UI-only feedback
// and can never diverge from the app state machine).
struct UiModel {
    voice_memo_firmware::AppState state = voice_memo_firmware::AppState::Idle;
    voice_memo_firmware::VoiceTag tag = voice_memo_firmware::VoiceTag::Work;
    bool wifi_connected = false;
    bool sent_visible = false;
    bool busy_hint_visible = false;
    // A shutdown request was refused because a capture is in progress. The
    // screen keeps showing the truth (RECORDING); the notice is the explicit
    // "STOP RECORDING FIRST" overlay the specification asks for.
    bool stop_recording_hint_visible = false;
};

// Screen for the current model.
//
// The SENT confirmation temporarily overrides the Idle screen: the upload has
// already returned to Idle by the time the ingress answer is drained, so without
// an override the success feedback would never be shown. It is deliberately
// scoped to Idle - if the user starts the next recording inside the feedback
// window, the RECORDING screen must win, because the panel has to show the
// state the device is actually in.
inline UiScreen ui_screen_for(const UiModel& model) {
    if (model.sent_visible && model.state == voice_memo_firmware::AppState::Idle) {
        return UiScreen::Sent;
    }

    switch (model.state) {
        case voice_memo_firmware::AppState::Idle:
            // Offline is not a firmware state: it is Idle with no Wi-Fi, which
            // is exactly the case where the next recording will be accepted and
            // then held in the buffer until the network comes back.
            return model.wifi_connected ? UiScreen::Ready : UiScreen::ReadyOffline;
        case voice_memo_firmware::AppState::Recording:
            return UiScreen::Recording;
        case voice_memo_firmware::AppState::MaxReachedWaitingRelease:
            return UiScreen::MaxReached;
        case voice_memo_firmware::AppState::Saving:
            return UiScreen::Saving;
        case voice_memo_firmware::AppState::Uploading:
            return UiScreen::Uploading;
        case voice_memo_firmware::AppState::RetryWait:
            return UiScreen::RetryWait;
    }
    return UiScreen::Ready;
}

// Touch may change the tag only where a new recording may start, i.e. exactly
// where `allows_new_recording()` is true. During Recording the tag is frozen
// metadata of the audio being captured; during Uploading/RetryWait the state is
// kept untouched so the buffered WAV keeps describing the recording it belongs
// to, and MaxReachedWaitingRelease is still inside the recording gesture.
inline bool ui_tag_selection_allowed(voice_memo_firmware::AppState state) {
    return voice_memo_firmware::allows_new_recording(state);
}

// Maps a touch point to a tag. Returns false and leaves `out_tag` untouched
// when the point is outside all four cells, so a stray tap can never change the
// selection.
inline bool ui_tag_hit_test(int x, int y, voice_memo_firmware::VoiceTag* out_tag) {
    for (uint8_t index = 0; index < voice_memo_firmware::kVoiceTagCount; ++index) {
        if (ui_rect_contains(ui_tag_rect(index), x, y)) {
            if (out_tag != nullptr) {
                *out_tag = voice_memo_firmware::voiceTagFromIndex(index);
            }
            return true;
        }
    }
    return false;
}

// Same hit test expressed as "new selection, or unchanged when nothing was hit".
inline voice_memo_firmware::VoiceTag ui_tag_for_touch(
    int x,
    int y,
    voice_memo_firmware::VoiceTag current
) {
    voice_memo_firmware::VoiceTag hit = current;
    if (ui_tag_hit_test(x, y, &hit)) {
        return hit;
    }
    return current;
}

// Non-blocking transient helper: true while `now` has not reached `deadline`.
// Written with a signed difference so it also behaves across the millis() wrap.
inline bool ui_deadline_pending(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(deadline - now) > 0;
}

// Which e-paper flush a render needs.
enum class UiRefresh {
    // Partial waveform: no flashing, ~0.3-0.5 s, and - critically - it is
    // started asynchronously so the main loop never waits for the panel.
    Partial,
    // Full waveform: clears ghosting, but the panel is busy for seconds and it
    // reloads the LUTs. Only allowed when the firmware is idle.
    Full,
};

// Inputs of the flush policy. Grouped in a struct so the rule stays readable
// and the host test can enumerate the combinations.
struct UiRefreshInput {
    // The screen differs from the one currently on the panel.
    bool screen_changed = false;
    // The change is a short lived overlay (SENT confirmation, BUSY hint) that
    // must appear quickly: a flashing multi-second full refresh would miss its
    // own display window.
    bool transient_screen = false;
    // The screen is the final POWERED OFF screen. It must be painted with the
    // full waveform (so the image is crisp and ghost-free, because it is the
    // last thing the panel will ever be given before power is cut) even when the
    // firmware is not Idle - which is why it overrides the state rule below.
    bool shutdown_screen = false;
    // Firmware state, used to decide whether blocking is acceptable.
    voice_memo_firmware::AppState state = voice_memo_firmware::AppState::Idle;
    // Partial refreshes since the last full one, to bound ghosting.
    uint32_t partials_since_full = 0;
};

// Blocking the loop is only acceptable in Idle, where nothing time critical is
// running:
//   Recording  : I2S capture would lose samples;
//   Saving     : the SD commit must finish so the PSRAM buffer can be freed and
//                the next recording may start; a multi-second flashing refresh
//                would delay exactly that;
//   Uploading  : a BOOT press must still be sampled (it is refused, but the
//                refusal must be logged, and the loop must stay alive);
//   RetryWait  : same, plus the retry timer;
//   MaxReached : the BOOT *release* that starts the upload would be swallowed.
// In practice this means one full refresh per recording cycle, when the panel
// returns to READY, which is also exactly when ghosting is cleaned up.
inline bool ui_refresh_can_block(voice_memo_firmware::AppState state) {
    return state == voice_memo_firmware::AppState::Idle;
}

inline UiRefresh ui_refresh_for(const UiRefreshInput& input) {
    if (input.shutdown_screen) {
        // Nothing is running any more: the shutdown sequence only reaches this
        // point once no recording, commit or volatile note is at risk, so the
        // panel may take as long as it needs for a clean final image.
        return UiRefresh::Full;
    }
    if (input.transient_screen) {
        return UiRefresh::Partial;
    }
    if (!ui_refresh_can_block(input.state)) {
        return UiRefresh::Partial;
    }
    if (input.screen_changed) {
        // Large visual change while nothing else needs the CPU: full refresh.
        return UiRefresh::Full;
    }
    if (input.partials_since_full >= VM_UI_FULL_REFRESH_EVERY_PARTIALS) {
        return UiRefresh::Full;
    }
    return UiRefresh::Partial;
}

}  // namespace voice_memo_ui
