#include "frank_face.h"

#include <Arduino.h>

#include <cstddef>

#include "config.h"
#include "epaper_display.h"
#include "frank_sprites.h"

namespace voice_memo_ui {
namespace {

// One step of an animation.
struct FrankFrame {
    FrankState state;
    // Copy only the 96x52 face overlay instead of the whole 200x200 sprite. This
    // is exactly equivalent (proved by tests/frank_face_test.cpp) for every state
    // an animation visits, because those states differ from each other only
    // inside the face rectangle. FRANK_RECORDING is the exception - it also
    // changes pixels elsewhere - which is why it is never animated this way.
    bool face_only;
    // Full waveform for this frame. Used for the first frame of an animation
    // (crisp start, no ghost of the previous screen) and for the final
    // FRANK_DEAD image that must survive the power-off.
    bool full_refresh;
    // Floor, in milliseconds, for how long the frame stays on the glass. The
    // panel's own update time dominates it - hundreds of ms for a partial,
    // seconds for a full - so this only keeps a short frame from being replaced
    // before a human could see it. It is a pacing floor, not a sleep.
    uint32_t min_visible_ms;
};

// Frank waking up: two frames, nothing else. The boot is on the critical path to
// the home screen, so it deliberately does not replay the death, does not blink
// and does not animate the mouth - it is one transition, "he opened his eyes".
//
// Every frame uses the PARTIAL waveform, the first one included:
//
//  * EpdDisplay::begin() has just written the cleared framebuffer as the panel's
//    base image and left the panel in partial mode, so the displayed image and the
//    base the controller compares against agree. That is exactly the condition a
//    partial update needs, and it is why no full refresh is required to start.
//  * The previous power-off left FRANK_DEAD on the glass; the panel is bistable,
//    so there is nothing to redraw at power-on. (The driver's mandatory init does
//    clear it to white first - the DEAD image is what the user sees while the
//    device is off, not what the animation starts from.)
//
// The first frame must be COMPLETE: the framebuffer holds the driver's white
// boot picture, so a face-only patch would leave a pair of eyes floating with no
// head. The second frame patches only the 96x52 face over that complete frame,
// which tests/frank_face_test.cpp proves is pixel-identical to the full sprite.
//
// `min_visible_ms` is a floor, not a sleep: a partial update of this panel takes
// ~0.4 s, which is already longer, so the two frames cost one panel update each.
constexpr uint32_t kBootFrameMinVisibleMs = 150;

const FrankFrame kBootFrames[] = {
    {FRANK_CLOSED, false, false, kBootFrameMinVisibleMs},
    {FRANK_AWAKE,  true,  false, kBootFrameMinVisibleMs},
};

// Frank dying. The first three frames are partial and replace each other within
// a few hundred ms, so the ghost of the home screen behind them is short lived
// and is wiped by the last frame. That last frame is the one that matters: a
// complete DEAD sprite, full waveform, waited on - it is the image the panel
// keeps with no power at all.
const FrankFrame kShutdownFrames[] = {
    {FRANK_AWAKE,  false, false, 250},
    {FRANK_BLINK,  true,  false, 200},
    {FRANK_CLOSED, true,  false, 250},
    {FRANK_DEAD,   false, true,  0},
};

// What this module has put on the glass. Only the module writes Frank frames, so
// this is authoritative as long as nothing else paints in between; a complete
// Frank frame is what makes a partial flush of the next one valid.
bool g_completeFrameOnPanel = false;
FrankState g_panelState = FRANK_AWAKE;

const char* frankStateName(FrankState state) {
    switch (state) {
        case FRANK_DEAD:
            return "dead";
        case FRANK_CLOSED:
            return "closed";
        case FRANK_HALF_OPEN:
            return "half_open";
        case FRANK_AWAKE:
            return "awake";
        case FRANK_BLINK:
            return "blink";
        case FRANK_HAPPY:
            return "happy";
        case FRANK_RECORDING:
            return "recording";
        case FRANK_ERROR:
            return "error";
    }
    return "unknown";
}

// Paints one frame into the framebuffer and flushes it. The panel is given the
// whole 200x200 framebuffer either way - this driver's partial refresh is not
// window based - so the only difference between the two paths is waveform, cost
// and where the source pixels come from.
bool frankPaintFrame(EpdDisplay& display, const FrankFrame& frame) {
    if (!display.ready() || display.faulted()) {
        return false;
    }

    if (frame.face_only) {
        frankBlitPacked(display.canvas(), frankFaceSprite(frame.state),
                        FRANK_FACE_X, FRANK_FACE_Y, FRANK_FACE_W, FRANK_FACE_H);
    } else {
        frankBlitPacked(display.canvas(), frankFullSprite(frame.state), 0, 0, FRANK_W, FRANK_H);
    }

    const uint32_t startMs = millis();

    // A frame is never started on a panel that is still updating: the driver
    // would either drop it or corrupt the update in progress.
    if (!display.waitIdleFor(VM_EPD_BUSY_TIMEOUT_MS)) {
        Serial.println("[frank] panel never went idle; frame dropped");
        return false;
    }

    const bool flushed = frame.full_refresh ? display.refreshFull() : display.startPartial();
    if (!flushed) {
        Serial.printf("[frank] %s frame flush failed (panel faulted=%s)\n",
                      frankStateName(frame.state),
                      display.faulted() ? "yes" : "no");
        return false;
    }

    // A full refresh waits for BUSY internally; a partial one returns as soon as
    // the data is on the wire. Waiting here is what makes the frame visible
    // before the next one is prepared, and it is also the only bound on an
    // animation whose panel stopped answering.
    if (!display.waitIdleFor(VM_EPD_BUSY_TIMEOUT_MS)) {
        Serial.println("[frank] frame did not finish within the BUSY timeout");
        return false;
    }

    const uint32_t elapsedMs = millis() - startMs;
    if (elapsedMs < frame.min_visible_ms) {
        delay(frame.min_visible_ms - elapsedMs);
    }

    g_completeFrameOnPanel = true;
    g_panelState = frame.state;
    Serial.printf("[frank] frame %s %s %u ms\n",
                  frankStateName(frame.state),
                  frame.full_refresh ? "full" : "partial",
                  static_cast<unsigned int>(elapsedMs));
    return true;
}

// Plays a fixed frame list. Bounded by construction: the list is a compile-time
// array and every frame has a panel-BUSY deadline, so the animation can neither
// run forever nor hang when the panel stops responding.
bool frankPlay(EpdDisplay& display, const FrankFrame* frames, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (!frankPaintFrame(display, frames[index])) {
            return false;
        }
    }
    return true;
}

template <typename T, size_t N>
constexpr size_t countOf(const T (&)[N]) {
    return N;
}

}  // namespace

void frankInit() {
    g_completeFrameOnPanel = false;
    g_panelState = FRANK_AWAKE;
}

void frankInvalidate() {
    g_completeFrameOnPanel = false;
}

bool frankShow(EpdDisplay& display, FrankState state) {
    if (g_completeFrameOnPanel && g_panelState == state) {
        // Already on the glass: refreshing it again would only flash for nothing.
        return true;
    }
    const FrankFrame frame = {state, false, !g_completeFrameOnPanel, 0};
    return frankPaintFrame(display, frame);
}

bool frankBootAnimation(EpdDisplay& display) {
    if (!display.ready() || display.faulted()) {
        Serial.println("[frank] no usable panel; boot animation skipped");
        return false;
    }

    Serial.println("[frank] boot animation");
    const uint32_t startMs = millis();

    // CLOSED, then AWAKE, then straight to the home screen. Two partial updates
    // and no full refresh: the panel was left by EpdDisplay::begin() with a base
    // image that matches what it displays, so the fast waveform is valid from the
    // very first frame. See kBootFrames above for why the first one is complete.
    if (!frankPlay(display, kBootFrames, countOf(kBootFrames))) {
        Serial.println("[frank] boot animation aborted; the home screen will still be painted");
        return false;
    }
    Serial.printf("[frank] boot animation done in %u ms\n",
                  static_cast<unsigned int>(millis() - startMs));
    return true;
}

bool frankShutdownAnimation(EpdDisplay& display) {
    if (!display.ready() || display.faulted()) {
        Serial.println("[frank] no usable panel; shutdown animation skipped");
        return false;
    }

    Serial.println("[frank] shutdown animation");
    const uint32_t startMs = millis();
    // The last frame is FRANK_DEAD on a full refresh that frankPaintFrame()
    // waits for, so when this returns true the eyes are already X X on the glass
    // and releasing the battery latch cannot lose them.
    if (!frankPlay(display, kShutdownFrames, countOf(kShutdownFrames))) {
        Serial.println("[frank] shutdown animation aborted before FRANK_DEAD; falling back");
        return false;
    }
    Serial.printf("[frank] shutdown animation done in %u ms\n",
                  static_cast<unsigned int>(millis() - startMs));
    return true;
}

}  // namespace voice_memo_ui
