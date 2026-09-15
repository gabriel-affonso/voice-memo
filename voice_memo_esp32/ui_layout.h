#pragma once

// Layout geometry for the 200x200 e-paper panel, in panel pixels.
//
// The panel is monochrome and small, so the layout is fully static and lives in
// one pure header: the renderer (gfx_canvas + ui_controller) and the touch hit
// test (ui_model.h) must agree pixel for pixel, and the host test checks the
// four tag rectangles against these constants.
//
// Origine (0,0) is the top-left corner as used by the official Waveshare
// FT6336 example (it maps the reported x/y straight onto the 200x200 panel,
// with no rotation or swap).
//
// No Arduino dependency.

#include <cstdint>

#include "voice_tags.h"

namespace voice_memo_ui {

// Panel geometry, from the official Waveshare example (EPD_WIDTH/EPD_HEIGHT).
constexpr uint16_t kScreenWidth = 200;
constexpr uint16_t kScreenHeight = 200;

// Outer margin: 8 px keeps a visible white frame on all four sides.
constexpr uint16_t kMargin = 8;

// Top status bar: time on the left, Wi-Fi state and battery on the right.
// Height 21 px including the separator line drawn at kTopBarRuleY.
constexpr uint16_t kTopBarTextY = 6;
constexpr uint16_t kTopBarRuleY = 20;

// Right edge used to right-align the battery text and the Wi-Fi indicator.
constexpr uint16_t kTopBarRight = kScreenWidth - kMargin;  // 192 (exclusive)

// Wi-Fi indicator drawn as three arcs; the box is 12x9 px.
constexpr uint16_t kWifiIconWidth = 12;
constexpr uint16_t kWifiIconHeight = 9;
constexpr uint16_t kWifiIconGapToBattery = 8;

// 2x2 tag grid. Each cell is 89x30 px with a 6 px gutter, so the grid spans
// x 8..191 and y 100..165 and every cell is a well separated touch target.
constexpr uint16_t kGridX = kMargin;                              // 8
constexpr uint16_t kGridY = 100;
constexpr uint16_t kCellWidth = 89;
constexpr uint16_t kCellHeight = 30;
constexpr uint16_t kColGap = 6;
constexpr uint16_t kRowGap = 6;

// Footer instruction line.
constexpr uint16_t kFooterTextY = 180;

// Text scales used on the 5x7 font (advance = 6 * scale px).
constexpr uint8_t kScaleSmall = 1;   // 6x8 px cell  -> footer, top bar
constexpr uint8_t kScaleMedium = 2;  // 12x14 px cell -> tag labels, UPLOADING
constexpr uint8_t kScaleLarge = 3;   // 18x21 px cell -> READY, REC timer

// A plain integer rectangle. Kept trivial so the hit test can be a pure
// function with no display or framework dependency.
struct UiRect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
};

constexpr bool ui_rect_contains(const UiRect& rect, int x, int y) {
    return x >= rect.x && x < (rect.x + rect.w) && y >= rect.y && y < (rect.y + rect.h);
}

// Exact hitbox of one tag cell (index 0..kVoiceTagCount-1).
//
// Referenced by enumerator name on purpose: the visible label strings belong to
// voice_tags.h alone.
//
//   index 0 = VoiceTag::Work     : x   8.. 96, y 100..129  (top-left)
//   index 1 = VoiceTag::Idea     : x 103..191, y 100..129  (top-right)
//   index 2 = VoiceTag::Todo     : x   8.. 96, y 136..165  (bottom-left)
//   index 3 = VoiceTag::Personal : x 103..191, y 136..165  (bottom-right)
constexpr UiRect ui_tag_rect(uint8_t index) {
    // Single expression on purpose: the host test compiles this header with
    // -std=c++11, where a constexpr function may not declare local variables.
    return UiRect{
        static_cast<int16_t>(kGridX + (index % 2) * (kCellWidth + kColGap)),
        static_cast<int16_t>(kGridY + (index / 2) * (kCellHeight + kRowGap)),
        static_cast<int16_t>(kCellWidth),
        static_cast<int16_t>(kCellHeight),
    };
}

}  // namespace voice_memo_ui
