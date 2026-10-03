// Host-side tests for Frank's sprite pipeline.
//
// Frank's face is drawn from a generated asset pack (frank_sprites.h) straight
// into the e-paper framebuffer, so the two things that can silently ruin the
// animation are the bit conventions and the asset geometry. Both are pure data
// properties, which means they can be verified on the Mac with no board at all:
//
//     c++ -std=c++11 -Wall -Wextra -Itests -I. -o /tmp/frank_face_test \
//         tests/frank_face_test.cpp gfx_canvas.cpp
//     /tmp/frank_face_test          # assertions
//     /tmp/frank_face_test --dump   # + ASCII art of the animated frames
//
// Covered here:
//   * the array sizes are exactly W*H/8 for every frame and every overlay
//   * bit order (bit 7 = leftmost) and polarity (set bit = ink) of the copy
//   * the copy writes both colours, so a region is never half defined
//   * the 96x52 overlay is pixel-identical to the full sprite's face rectangle
//     (this is what makes the partial-refresh frames valid)
//   * the animated states differ from each other ONLY inside that rectangle
//     (FRANK_RECORDING does not, which is why it is never animated that way)
//   * the animated frames are actually distinct, i.e. the animation is not a
//     sequence of identical pictures
//
// The animated frame lists themselves (order, waveform, pacing) live in
// frank_face.cpp and need the panel driver, so they are reviewed there and
// exercised on the board; everything they depend on is pinned down here.
//
// This directory is not compiled into the firmware: Arduino only builds the
// sketch root and src/, so tests/ is ignored by arduino-cli.

#include <cstdio>
#include <cstring>

// The sprite pack includes <Arduino.h> for PROGMEM/uint8_t. On the host that is
// the test shim (hence -Itests in the runner), and PROGMEM means nothing at all:
// on the ESP32 the const arrays already live in flash.
#include "arduino_shim.h"
#ifndef PROGMEM
#define PROGMEM
#endif

#include "../frank_face.h"
#include "../frank_sprites.h"
#include "../gfx_canvas.h"

namespace {

using namespace voice_memo_ui;

constexpr int kWidth = 200;
constexpr int kHeight = 200;
constexpr int kBufferBytes = (kWidth + 7) / 8 * kHeight;  // 5000

int g_failures = 0;
int g_checks = 0;
bool g_dump = false;

void checkBool(const char* label, bool actual) {
    ++g_checks;
    if (actual) {
        std::printf("PASS %s\n", label);
        return;
    }
    std::printf("FAIL %s\n", label);
    ++g_failures;
}

void checkInt(const char* label, long actual, long expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s = %ld\n", label, actual);
        return;
    }
    std::printf("FAIL %s: expected %ld, got %ld\n", label, expected, actual);
    ++g_failures;
}

void section(const char* title) {
    std::printf("\n--- %s\n", title);
}

const FrankState kStates[] = {
    FRANK_DEAD, FRANK_CLOSED, FRANK_HALF_OPEN, FRANK_AWAKE,
    FRANK_BLINK, FRANK_HAPPY, FRANK_RECORDING, FRANK_ERROR,
};
const char* const kStateNames[] = {
    "dead", "closed", "half_open", "awake", "blink", "happy", "recording", "error",
};
constexpr int kStateCount = static_cast<int>(sizeof(kStates) / sizeof(kStates[0]));

// The states the module is allowed to animate with the face-only shortcut.
// Everything below that talks about "animated states" means this set: it is what
// the two animations use (the boot is CLOSED -> AWAKE, the shutdown is AWAKE ->
// BLINK -> CLOSED -> DEAD), plus FRANK_HALF_OPEN, which is kept in the pack for a
// future sequence and whose assets satisfy the same invariant. FRANK_RECORDING is
// deliberately absent: it paints outside the face rectangle, so the shortcut would
// leave a stale body behind. HAPPY/ERROR are unused.
const FrankState kAnimatedStates[] = {
    FRANK_DEAD, FRANK_CLOSED, FRANK_HALF_OPEN, FRANK_AWAKE, FRANK_BLINK,
};
constexpr int kAnimatedCount = static_cast<int>(sizeof(kAnimatedStates) / sizeof(kAnimatedStates[0]));

int stateIndex(FrankState state) {
    for (int index = 0; index < kStateCount; ++index) {
        if (kStates[index] == state) {
            return index;
        }
    }
    return -1;
}

// Independent decoder: a flat bit index over the whole image, deliberately
// written differently from frankBlitPacked()'s row/column form. Valid because
// every Frank bitmap is a whole number of bytes wide (200 and 96 are both
// multiples of 8), so there is no row padding to skip.
bool spriteBit(const uint8_t* packed, int width, int x, int y) {
    const size_t bitIndex = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
    return ((packed[bitIndex >> 3] >> (7 - (bitIndex & 7))) & 0x01U) != 0;
}

// A canvas plus the 1bpp framebuffer it draws into.
struct Surface {
    uint8_t buffer[kBufferBytes];
    GfxCanvas canvas;

    Surface() {
        canvas.begin(buffer, kWidth, kHeight);
        canvas.clear(GfxColor::White);
    }

    void paintFull(const uint8_t* packed) {
        canvas.clear(GfxColor::White);
        frankBlitPacked(canvas, packed, 0, 0, FRANK_W, FRANK_H);
    }

    bool black(int x, int y) const { return canvas.pixelAt(x, y) == GfxColor::Black; }
};

int countBlack(const GfxCanvas& canvas) {
    int black = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (canvas.pixelAt(x, y) == GfxColor::Black) {
                ++black;
            }
        }
    }
    return black;
}

int countSpriteBits(const uint8_t* packed, int width, int height) {
    int ink = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (spriteBit(packed, width, x, y)) {
                ++ink;
            }
        }
    }
    return ink;
}

bool faceRegionEqual(const Surface& a, const Surface& b) {
    for (int y = FRANK_FACE_Y; y < FRANK_FACE_Y + FRANK_FACE_H; ++y) {
        for (int x = FRANK_FACE_X; x < FRANK_FACE_X + FRANK_FACE_W; ++x) {
            if (a.black(x, y) != b.black(x, y)) {
                return false;
            }
        }
    }
    return true;
}

bool outsideFaceEqual(const Surface& a, const Surface& b) {
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (x >= FRANK_FACE_X && x < FRANK_FACE_X + FRANK_FACE_W &&
                y >= FRANK_FACE_Y && y < FRANK_FACE_Y + FRANK_FACE_H) {
                continue;
            }
            if (a.black(x, y) != b.black(x, y)) {
                return false;
            }
        }
    }
    return true;
}

void dumpFace(const char* label, const uint8_t* packed) {
    std::printf("\n--- %s face (96x52, '#' = ink, 2x2 pixels per character)\n", label);
    for (int y = 0; y < FRANK_FACE_H; y += 2) {
        for (int x = 0; x < FRANK_FACE_W; x += 2) {
            const bool ink = spriteBit(packed, FRANK_FACE_W, x, y) ||
                             spriteBit(packed, FRANK_FACE_W, x + 1, y) ||
                             spriteBit(packed, FRANK_FACE_W, x, y + 1) ||
                             spriteBit(packed, FRANK_FACE_W, x + 1, y + 1);
            std::putchar(ink ? '#' : '.');
        }
        std::putchar('\n');
    }
}

// ---------------------------------------------------------------------------

void testAssetGeometry() {
    section("asset geometry (one 1bpp frame = W*H/8 bytes)");

    checkInt("FRANK_W", FRANK_W, 200);
    checkInt("FRANK_H", FRANK_H, 200);
    checkInt("FRANK_FACE_W", FRANK_FACE_W, 96);
    checkInt("FRANK_FACE_H", FRANK_FACE_H, 52);

    checkInt("frank_dead_200x200", static_cast<long>(sizeof(frank_dead_200x200)), FRANK_W * FRANK_H / 8);
    checkInt("frank_closed_200x200", static_cast<long>(sizeof(frank_closed_200x200)), FRANK_W * FRANK_H / 8);
    checkInt("frank_half_open_200x200", static_cast<long>(sizeof(frank_half_open_200x200)), FRANK_W * FRANK_H / 8);
    checkInt("frank_awake_200x200", static_cast<long>(sizeof(frank_awake_200x200)), FRANK_W * FRANK_H / 8);
    checkInt("frank_blink_200x200", static_cast<long>(sizeof(frank_blink_200x200)), FRANK_W * FRANK_H / 8);
    checkInt("frank_happy_200x200", static_cast<long>(sizeof(frank_happy_200x200)), FRANK_W * FRANK_H / 8);
    checkInt("frank_recording_200x200", static_cast<long>(sizeof(frank_recording_200x200)), FRANK_W * FRANK_H / 8);
    checkInt("frank_error_200x200", static_cast<long>(sizeof(frank_error_200x200)), FRANK_W * FRANK_H / 8);

    checkInt("frank_face_dead_96x52", static_cast<long>(sizeof(frank_face_dead_96x52)), FRANK_FACE_W * FRANK_FACE_H / 8);
    checkInt("frank_face_closed_96x52", static_cast<long>(sizeof(frank_face_closed_96x52)), FRANK_FACE_W * FRANK_FACE_H / 8);
    checkInt("frank_face_half_open_96x52", static_cast<long>(sizeof(frank_face_half_open_96x52)), FRANK_FACE_W * FRANK_FACE_H / 8);
    checkInt("frank_face_awake_96x52", static_cast<long>(sizeof(frank_face_awake_96x52)), FRANK_FACE_W * FRANK_FACE_H / 8);
    checkInt("frank_face_blink_96x52", static_cast<long>(sizeof(frank_face_blink_96x52)), FRANK_FACE_W * FRANK_FACE_H / 8);
    checkInt("frank_face_happy_96x52", static_cast<long>(sizeof(frank_face_happy_96x52)), FRANK_FACE_W * FRANK_FACE_H / 8);
    checkInt("frank_face_recording_96x52", static_cast<long>(sizeof(frank_face_recording_96x52)), FRANK_FACE_W * FRANK_FACE_H / 8);
    checkInt("frank_face_error_96x52", static_cast<long>(sizeof(frank_face_error_96x52)), FRANK_FACE_W * FRANK_FACE_H / 8);

    // The frame the animations start from must be reachable through the accessor
    // the module uses, for every state.
    bool accessorsMatch = true;
    for (int index = 0; index < kStateCount; ++index) {
        if (frankFullSprite(kStates[index]) == nullptr || frankFaceSprite(kStates[index]) == nullptr) {
            accessorsMatch = false;
        }
    }
    checkBool("frankFullSprite()/frankFaceSprite() return a frame for every state", accessorsMatch);
}

void testBitConvention() {
    section("bit order and polarity of the copy");

    Surface surface;

    // A single set bit is the LEFTMOST pixel of its byte.
    const uint8_t leftmost[] = {0x80};
    surface.canvas.clear(GfxColor::White);
    frankBlitPacked(surface.canvas, leftmost, 8, 5, 8, 1);
    checkBool("bit 7 -> (8,5) is ink", surface.black(8, 5));
    checkBool("bit 6 -> (9,5) is background", !surface.black(9, 5));
    checkBool("bit 0 -> (15,5) is background", !surface.black(15, 5));

    // A set bit in the least significant position is the RIGHTMOST pixel.
    const uint8_t rightmost[] = {0x01};
    surface.canvas.clear(GfxColor::White);
    frankBlitPacked(surface.canvas, rightmost, 8, 5, 8, 1);
    checkBool("bit 0 -> (15,5) is ink", surface.black(15, 5));
    checkBool("bit 0 -> (14,5) is background", !surface.black(14, 5));

    // Four bits, in order, from the middle of the byte.
    const uint8_t nibble[] = {0x1E};  // 0b00011110 -> pixels 3,4,5,6 of the byte
    surface.canvas.clear(GfxColor::White);
    frankBlitPacked(surface.canvas, nibble, 16, 5, 8, 1);
    checkBool("0x1E -> (19,5) is ink", surface.black(19, 5));
    checkBool("0x1E -> (22,5) is ink", surface.black(22, 5));
    checkBool("0x1E -> (18,5) is background", !surface.black(18, 5));
    checkBool("0x1E -> (23,5) is background", !surface.black(23, 5));

    // Rows advance by the padded row stride: a two-row bitmap whose second row
    // is empty must leave the second row background.
    const uint8_t twoRows[] = {0x80, 0x00};
    surface.canvas.clear(GfxColor::Black);
    frankBlitPacked(surface.canvas, twoRows, 0, 0, 8, 2);
    checkBool("row 0 is written", surface.black(0, 0));
    checkBool("row 1 comes from the second byte", !surface.black(0, 1));

    // Both colours are written, so a blit fully defines its rectangle: an
    // all-zero bitmap must turn a black rectangle white.
    const uint8_t blank[] = {0x00, 0x00, 0x00, 0x00};
    surface.canvas.clear(GfxColor::Black);
    frankBlitPacked(surface.canvas, blank, 10, 10, 16, 2);
    checkBool("an all-zero bitmap paints background, not nothing", !surface.black(10, 10));
    checkBool("an all-zero bitmap paints the whole rectangle", !surface.black(25, 11));
    checkBool("pixels outside the rectangle are untouched", surface.black(26, 11));

    // Out-of-range arguments degrade to a no-op instead of touching memory.
    surface.canvas.clear(GfxColor::White);
    frankBlitPacked(surface.canvas, nullptr, 0, 0, 8, 1);
    frankBlitPacked(surface.canvas, blank, 0, 0, 0, 1);
    checkBool("a null or empty blit is a no-op", countBlack(surface.canvas) == 0);
}

void testFullFrames() {
    section("full 200x200 frames land in the framebuffer byte for byte");

    Surface surface;
    for (int index = 0; index < kStateCount; ++index) {
        const uint8_t* sprite = frankFullSprite(kStates[index]);
        surface.paintFull(sprite);

        // Every pixel must agree with the independent decoder...
        int mismatches = 0;
        for (int y = 0; y < kHeight && mismatches == 0; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const bool expectedInk = spriteBit(sprite, kWidth, x, y);
                if (surface.black(x, y) != expectedInk) {
                    ++mismatches;
                    break;
                }
            }
        }
        ++g_checks;
        if (mismatches == 0) {
            std::printf("PASS %s: 40000 pixels match the packed bitmap\n", kStateNames[index]);
        } else {
            std::printf("FAIL %s: %d pixel(s) disagree with the packed bitmap\n",
                        kStateNames[index], mismatches);
            ++g_failures;
        }

        // ...and no set bit may be lost or invented.
        const int ink = countBlack(surface.canvas);
        const int expectedInk = countSpriteBits(sprite, kWidth, kHeight);
        ++g_checks;
        if (ink == expectedInk) {
            std::printf("PASS %s: ink pixels = %d\n", kStateNames[index], ink);
        } else {
            std::printf("FAIL %s: %d ink pixels, expected %d\n", kStateNames[index], ink, expectedInk);
            ++g_failures;
        }

        // The artwork is a character on a white panel, not a negative: the
        // corners are background and the head is not empty.
        if (index == 0) {
            checkBool("the panel corners are background", !surface.black(0, 0) && !surface.black(199, 0) &&
                                                            !surface.black(0, 199) && !surface.black(199, 199));
            checkBool("the sprite is not blank", ink > 100);
        }
    }
}

void testFaceOverlayEquivalence() {
    section("the 96x52 overlay equals the full sprite's face rectangle");

    for (int index = 0; index < kStateCount; ++index) {
        Surface full;
        Surface face;
        full.paintFull(frankFullSprite(kStates[index]));
        // The overlay is blitted onto a white canvas: it must define its whole
        // rectangle, so starting from white is enough.
        face.canvas.clear(GfxColor::White);
        frankBlitPacked(face.canvas, frankFaceSprite(kStates[index]),
                        FRANK_FACE_X, FRANK_FACE_Y, FRANK_FACE_W, FRANK_FACE_H);

        ++g_checks;
        if (faceRegionEqual(full, face)) {
            std::printf("PASS %s: overlay == full sprite inside the face rectangle\n", kStateNames[index]);
        } else {
            std::printf("FAIL %s: overlay disagrees with the full sprite inside the face rectangle\n",
                        kStateNames[index]);
            ++g_failures;
        }
    }
}

void testAnimatedStatesDifferOnlyInTheFace() {
    section("animated states differ only inside the face rectangle");

    Surface awake;
    awake.paintFull(frankFullSprite(FRANK_AWAKE));

    for (int index = 0; index < kAnimatedCount; ++index) {
        Surface other;
        other.paintFull(frankFullSprite(kAnimatedStates[index]));
        const char* name = kStateNames[stateIndex(kAnimatedStates[index])];
        ++g_checks;
        if (outsideFaceEqual(awake, other)) {
            std::printf("PASS %s: identical to AWAKE outside the face rectangle\n", name);
        } else {
            std::printf("FAIL %s: differs from AWAKE outside the face rectangle "
                        "(the face-only frames would leave a stale body)\n", name);
            ++g_failures;
        }
    }

    // FRANK_RECORDING is the documented exception and must stay one: if it ever
    // becomes face-only-compatible this test should be tightened, not loosened.
    Surface recording;
    recording.paintFull(frankFullSprite(FRANK_RECORDING));
    checkBool("FRANK_RECORDING does change pixels outside the face (never animated face-only)",
              !outsideFaceEqual(awake, recording));
}

void testAnimationFramesAreDistinct() {
    section("the animated frames are actually different pictures");

    // Inside the face rectangle, every animated state must be unique: an
    // animation that repeated a frame would be a visible stall.
    for (int a = 0; a < kAnimatedCount; ++a) {
        for (int b = a + 1; b < kAnimatedCount; ++b) {
            Surface first;
            Surface second;
            first.canvas.clear(GfxColor::White);
            second.canvas.clear(GfxColor::White);
            frankBlitPacked(first.canvas, frankFaceSprite(kAnimatedStates[a]),
                            FRANK_FACE_X, FRANK_FACE_Y, FRANK_FACE_W, FRANK_FACE_H);
            frankBlitPacked(second.canvas, frankFaceSprite(kAnimatedStates[b]),
                            FRANK_FACE_X, FRANK_FACE_Y, FRANK_FACE_W, FRANK_FACE_H);
            const char* label = kStateNames[stateIndex(kAnimatedStates[a])];
            const char* other = kStateNames[stateIndex(kAnimatedStates[b])];
            ++g_checks;
            if (!faceRegionEqual(first, second)) {
                std::printf("PASS %s and %s have different faces\n", label, other);
            } else {
                std::printf("FAIL %s and %s are the same face\n", label, other);
                ++g_failures;
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--dump") == 0) {
            g_dump = true;
        }
    }

    testAssetGeometry();
    testBitConvention();
    testFullFrames();
    testFaceOverlayEquivalence();
    testAnimatedStatesDifferOnlyInTheFace();
    testAnimationFramesAreDistinct();

    if (g_dump) {
        std::printf("\n=== boot animation faces ===\n");
        const FrankState boot[] = {FRANK_CLOSED, FRANK_AWAKE};
        for (FrankState state : boot) {
            dumpFace(kStateNames[stateIndex(state)], frankFaceSprite(state));
        }
    }

    std::printf("\n=========================================\n");
    std::printf("%d check(s), %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
