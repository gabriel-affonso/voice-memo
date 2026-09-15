#pragma once

// Screen painter: everything that turns a view model into pixels.
//
// Kept free of Arduino dependencies (it only needs GfxCanvas) so the host test
// can render all six screens and dump them as ASCII art. That is the only way to
// review the layout of a 200x200 panel without the physical board, and it also
// verifies that the touch hitboxes in ui_layout.h line up with what is drawn.

#include <cstdint>

#include "gfx_canvas.h"
#include "ui_model.h"
#include "voice_tags.h"

namespace voice_memo_ui {

// Everything the panel needs to display, already resolved to strings and flags
// by UiController. The painter never reads the clock, the ADC, the Wi-Fi state
// machine or RecordingApp, so it is deterministic.
struct UiView {
    UiScreen screen = UiScreen::Ready;
    // Tag to display: the selection in READY, the frozen tag everywhere else.
    voice_memo_firmware::VoiceTag tag = voice_memo_firmware::VoiceTag::Work;
    bool wifi_connected = false;
    // Battery is optional hardware capability; false renders "--%".
    bool battery_known = false;
    uint8_t battery_percent = 0;
    // "HH:MM" or "--:--".
    const char* time_text = "--:--";
    // Elapsed seconds source: live while Recording, frozen duration once the WAV
    // is finalized (Uploading/RetryWait).
    uint32_t elapsed_ms = 0;
    bool busy_hint = false;
};

// Paints a complete screen into the canvas (the canvas is cleared first).
void ui_draw_screen(GfxCanvas& canvas, const UiView& view);

}  // namespace voice_memo_ui
