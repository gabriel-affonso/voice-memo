#pragma once

// Minimal 1bpp drawing surface for the 200x200 e-paper panel.
//
// Why not LVGL/GxEPD2/Adafruit_GFX: the UI is four static screens on a
// monochrome 200x200 panel. A ~200 line direct framebuffer layer keeps the UI
// fully under our control (no continuous redraw, no extra flash) and, more
// importantly, keeps every drawing primitive free of Arduino dependencies so
// the renderer can be verified byte for byte by a host test that dumps the
// framebuffer as ASCII art.
//
// Bit convention: this matches the official Waveshare driver, which stores one
// bit per pixel with bit 7 of each byte as the leftmost pixel of the group of
// eight, and where a SET bit means WHITE. The touch hit test and the renderer
// both use panel pixel coordinates with (0,0) at the top-left.

#include <cstdint>

#include "font5x7.h"

namespace voice_memo_ui {

enum class GfxColor : uint8_t {
    Black = 0,
    White = 1,
};

class GfxCanvas {
public:
    // `buffer` must hold rowBytes() * height bytes. The canvas does not own it:
    // EpdDisplay owns the PSRAM framebuffer and hands it over.
    void begin(uint8_t* buffer, uint16_t width, uint16_t height);

    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }
    uint16_t rowBytes() const { return rowBytes_; }
    uint16_t bufferBytes() const { return static_cast<uint16_t>(rowBytes_ * height_); }
    uint8_t* buffer() { return buffer_; }
    const uint8_t* buffer() const { return buffer_; }
    bool valid() const { return buffer_ != nullptr && rowBytes_ != 0; }

    // Fills the whole canvas.
    void clear(GfxColor color);

    void drawPixel(int x, int y, GfxColor color);
    // Read back one pixel. Drives the ASCII-art dump used by the host test.
    GfxColor pixelAt(int x, int y) const;
    void fillRect(int x, int y, int w, int h, GfxColor color);
    // One pixel thick border, drawn inside the given box.
    void drawRect(int x, int y, int w, int h, GfxColor color);
    void fillCircle(int centerX, int centerY, int radius, GfxColor color);
    void drawLine(int x0, int y0, int x1, int y1, GfxColor color, int thickness = 1);

    // 5x7 font (see font5x7.h). `x`/`y` is the top-left corner of the text box.
    // Characters outside 0x20..0x7E are drawn as spaces.
    void drawText(int x, int y, const char* text, GfxColor color, uint8_t scale = 1);
    void drawTextCentered(int centerX, int y, const char* text, GfxColor color, uint8_t scale = 1);
    void drawTextRightAligned(int rightX, int y, const char* text, GfxColor color, uint8_t scale = 1);

    static int textWidth(const char* text, uint8_t scale = 1);
    static int textHeight(uint8_t scale = 1);

    // UI-specific primitives that would be silly to encode in a 5x7 font.
    void drawCheckMark(int x, int y, int size, GfxColor color);
    void drawUpArrow(int centerX, int topY, int width, int height, GfxColor color);
    // Three nested Wi-Fi arcs (connected state). The disconnected state is
    // rendered as the text "OFF" by ui_screens.cpp, because a 12x9 px crossed
    // icon is not legible on this panel.
    void drawWifiIcon(int x, int y, GfxColor color);
    // Small centred dot used as the "tag · duration" separator.
    void drawMiddleDot(int centerX, int centerY, GfxColor color);

private:
    uint8_t* buffer_ = nullptr;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint16_t rowBytes_ = 0;
};

}  // namespace voice_memo_ui
