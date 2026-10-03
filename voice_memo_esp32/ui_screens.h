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
    // is finalized (Saving/Uploading/RetryWait).
    uint32_t elapsed_ms = 0;
    bool busy_hint = false;

    // --- power feedback -----------------------------------------------------
    // "STOP RECORDING FIRST": a PWR press was refused mid-capture.
    bool stop_recording_hint = false;
    // "UNSENT NOTE": a shutdown was refused because the only copy of a
    // recording lives in PSRAM. Owns the screen when set, because it is the
    // explanation the user needs and the state may already be back to READY.
    bool unsent_note_hint = false;
    // Number of persisted recordings still waiting on the card, shown on the
    // POWERED OFF screen so the user knows the notes survived the shutdown.
    uint32_t power_off_pending = 0;

    // --- persistent queue status -------------------------------------------
    // Card mounted and layout present. false renders "NO SD".
    bool sd_available = false;
    // I/O failure: the oldest, most urgent thing to tell the user.
    bool sd_error = false;
    // The card refused the last commit for space.
    bool storage_full = false;
    // Recordings on the card the ingress has not confirmed yet.
    uint32_t pending_count = 0;
    // A background upload from the card is in flight right now.
    bool background_upload = false;
};

// Paints a complete screen into the canvas (the canvas is cleared first).
void ui_draw_screen(GfxCanvas& canvas, const UiView& view);

}  // namespace voice_memo_ui
