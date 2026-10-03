#pragma once

// Frank on the 200x200 e-paper panel.
//
// Frank is the device's character: a simple, pixelated, friendly take on
// Frankenstein. The artwork is a generated pack (frank_sprites.h) holding eight
// complete 200x200 frames plus one 96x52 face overlay per state, all 1bpp,
// MSB-first, one bit per pixel, bit 7 of each byte = leftmost, and a SET bit
// meaning INK (black).
//
// This module is the ONLY place that knows how to turn those assets into panel
// pixels. It owns exactly two animations:
//
//   boot      CLOSED -> AWAKE                 (two frames, both partial)
//   shutdown  AWAKE -> BLINK -> CLOSED -> DEAD (ends on a full refresh)
//
// and nothing else in the firmware draws Frank. The recording home screen is
// untouched: after the boot animation the existing UiController paints it, and
// the shutdown animation runs inside the existing shutdown path.
//
// The boot is on the critical path to the home screen, so it is one transition -
// "he opened his eyes" - and nothing more: no dead frame, no blink, no mouth
// animation. The states it no longer uses (DEAD, HALF_OPEN, BLINK, HAPPY,
// RECORDING, ERROR) all stay in the asset pack and remain reachable through
// frankShow().
//
// Refresh policy (the panel is the slow, lossy part, so this is deliberate):
//   * the first frame of an animation is always a complete 200x200 sprite, which
//     is what makes a partial flush of the frames after it valid at all;
//   * the boot animation uses the PARTIAL waveform for both frames, the first
//     one included: EpdDisplay::begin() has just written a base image that matches
//     what the panel displays, which is exactly the condition a partial update
//     needs. No full refresh is spent on the boot;
//   * every frame after the first copies only the 96x52 face overlay (624 bytes
//     instead of 5000) and flushes with the PARTIAL waveform, which is the same
//     proven path the recording timer already uses;
//   * the shutdown animation uses the partial waveform on its first three frames
//     (the goodbye must be quick, and the ghost of the home screen behind it is
//     wiped by the last frame) and ends on DEAD painted complete and FULL, waited
//     on: the panel is bistable, so that image is what stays on the glass with the
//     MCU powered off, and it must be perfect.
//
// The pixel copy itself is a pure function - no Arduino, no driver, no state -
// so the host test can verify the bit order, the polarity, the overlay/full
// sprite equivalence and the array sizes on the Mac:
// see tests/frank_face_test.cpp.

#include <cstddef>
#include <cstdint>

#include "gfx_canvas.h"

// The sprite state ids, defined at global scope by the generated asset pack
// (frank_sprites.h). Declared here as an opaque enum - same underlying type, so
// both see one enumeration - because that is the only way to name the type in
// this header without pulling <Arduino.h> in with it. This header must stay free
// of Arduino and ESP-IDF headers so the host test can include it.
enum FrankState : uint8_t;

namespace voice_memo_ui {

// The panel driver. Forward declared for the same reason as FrankState.
class EpdDisplay;

// Copies a packed 1bpp bitmap into the canvas: one bit per pixel, bit 7 of each
// byte leftmost, a SET bit meaning INK (black).
//
// Both colours are written, so the target rectangle is fully defined and never
// has to be cleared first. Note the polarity: the sprite pack uses
// "set bit = black", the panel framebuffer uses "set bit = white" (see
// gfx_canvas.h), so this copy inverts every byte on the way in. That is the
// whole reason the two conventions never meet anywhere else in the firmware.
inline void frankBlitPacked(GfxCanvas& canvas, const uint8_t* packed, int x, int y, int w, int h) {
    if (packed == nullptr || !canvas.valid() || w <= 0 || h <= 0) {
        return;
    }
    const int rowBytes = (w + 7) / 8;
    for (int row = 0; row < h; ++row) {
        const uint8_t* source = packed + static_cast<size_t>(row) * static_cast<size_t>(rowBytes);
        for (int column = 0; column < w; ++column) {
            const bool ink = ((source[column >> 3] >> (7 - (column & 7))) & 0x01U) != 0;
            canvas.drawPixel(x + column, y + row, ink ? GfxColor::Black : GfxColor::White);
        }
    }
}

// Forgets which Frank frame the module last put on the glass, so the next
// frankShow() paints with the full waveform. Called once per boot, before the
// first animation.
void frankInit();

// Tells the module that the canvas - and therefore the panel - no longer holds a
// Frank frame, because the normal UI has just painted a screen of its own (the
// home screen, the POWERED OFF fallback). Without this the module would believe
// Frank is still on the glass and would use the partial waveform - and skip
// frames as redundant - against a screen that is not actually there.
void frankInvalidate();

// Paints one complete Frank frame and waits for the panel to finish it.
//
// This is the on-demand entry point for a single state (the states the boot no
// longer visits - HAPPY for a confirmation, RECORDING while capturing, ERROR on a
// failure - are all reachable through it). The two animations do NOT use it: they
// drive their own frame tables, where the waveform and the pacing of every frame
// are stated explicitly.
//
// The first frame after frankInit()/frankInvalidate() uses the full waveform (a
// safe, crisp default for a caller that does not know what the panel is showing);
// a frame that replaces another Frank frame uses the partial one. Idempotent:
// showing the state that is already on the glass changes nothing and never
// flashes.
//
// Blocking, bounded by the panel BUSY timeout. Returns false when the panel is
// unavailable or has faulted (the caller must then simply carry on without a
// face - the voice memo never depends on the display).
bool frankShow(EpdDisplay& display, FrankState state);

// Frank wakes up: CLOSED -> AWAKE, two partial refreshes and straight to the home
// screen. Called once from setup(), after the panel is up and before the home
// screen is painted. Returns false (without hanging) when the panel cannot be
// used.
bool frankBootAnimation(EpdDisplay& display);

// Frank dies: AWAKE -> BLINK -> CLOSED -> DEAD, ending on a full refresh that is
// waited on, so FRANK_DEAD is the last image and survives power-off. Called from
// the single shutdown path, immediately before the battery latch is released.
bool frankShutdownAnimation(EpdDisplay& display);

}  // namespace voice_memo_ui
