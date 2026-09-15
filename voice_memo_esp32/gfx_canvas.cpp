#include "gfx_canvas.h"

#include <cstring>

#include "ui_layout.h"

namespace voice_memo_ui {
namespace {

// bit 7 is the leftmost pixel of the byte, matching the official Waveshare
// driver (EPD_DrawColorPixel uses `bit = 7 - (x & 0x07)`).
inline uint8_t pixelMask(int x) {
    return static_cast<uint8_t>(0x80U >> (x & 0x07));
}

inline int absInt(int value) {
    return value < 0 ? -value : value;
}

}  // namespace

void GfxCanvas::begin(uint8_t* buffer, uint16_t width, uint16_t height) {
    buffer_ = buffer;
    width_ = width;
    height_ = height;
    rowBytes_ = static_cast<uint16_t>((width + 7) / 8);
}

void GfxCanvas::clear(GfxColor color) {
    if (!valid()) {
        return;
    }
    memset(buffer_, color == GfxColor::White ? 0xFF : 0x00, bufferBytes());
}

void GfxCanvas::drawPixel(int x, int y, GfxColor color) {
    if (!valid() || x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }
    uint8_t* byte = buffer_ + static_cast<size_t>(y) * rowBytes_ + (x >> 3);
    const uint8_t mask = pixelMask(x);
    if (color == GfxColor::White) {
        *byte = static_cast<uint8_t>(*byte | mask);
    } else {
        *byte = static_cast<uint8_t>(*byte & static_cast<uint8_t>(~mask));
    }
}

GfxColor GfxCanvas::pixelAt(int x, int y) const {
    if (!valid() || x < 0 || y < 0 || x >= width_ || y >= height_) {
        return GfxColor::White;
    }
    const uint8_t byte = buffer_[static_cast<size_t>(y) * rowBytes_ + (x >> 3)];
    return (byte & pixelMask(x)) != 0 ? GfxColor::White : GfxColor::Black;
}

void GfxCanvas::fillRect(int x, int y, int w, int h, GfxColor color) {
    if (!valid() || w <= 0 || h <= 0) {
        return;
    }
    for (int row = y; row < y + h; ++row) {
        for (int column = x; column < x + w; ++column) {
            drawPixel(column, row, color);
        }
    }
}

void GfxCanvas::drawRect(int x, int y, int w, int h, GfxColor color) {
    if (!valid() || w <= 0 || h <= 0) {
        return;
    }
    for (int column = x; column < x + w; ++column) {
        drawPixel(column, y, color);
        drawPixel(column, y + h - 1, color);
    }
    for (int row = y; row < y + h; ++row) {
        drawPixel(x, row, color);
        drawPixel(x + w - 1, row, color);
    }
}

void GfxCanvas::fillCircle(int centerX, int centerY, int radius, GfxColor color) {
    if (!valid() || radius <= 0) {
        return;
    }
    const int radiusSquared = radius * radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= radiusSquared) {
                drawPixel(centerX + dx, centerY + dy, color);
            }
        }
    }
}

void GfxCanvas::drawLine(int x0, int y0, int x1, int y1, GfxColor color, int thickness) {
    if (!valid()) {
        return;
    }
    if (thickness < 1) {
        thickness = 1;
    }
    // Integer Bresenham, with a square brush of `thickness` for the check mark.
    const int dx = absInt(x1 - x0);
    const int dy = -absInt(y1 - y0);
    const int stepX = x0 < x1 ? 1 : -1;
    const int stepY = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    const int half = thickness / 2;

    int x = x0;
    int y = y0;
    for (;;) {
        if (thickness == 1) {
            drawPixel(x, y, color);
        } else {
            fillRect(x - half, y - half, thickness, thickness, color);
        }
        if (x == x1 && y == y1) {
            break;
        }
        const int error2 = 2 * error;
        if (error2 >= dy) {
            error += dy;
            x += stepX;
        }
        if (error2 <= dx) {
            error += dx;
            y += stepY;
        }
    }
}

int GfxCanvas::textWidth(const char* text, uint8_t scale) {
    if (text == nullptr || *text == '\0') {
        return 0;
    }
    int characters = 0;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        ++characters;
    }
    if (scale == 0) {
        scale = 1;
    }
    // kFontGlyphAdvance includes the 1 px inter-character gap, so the last
    // character has no trailing gap.
    return characters * kFontGlyphAdvance * scale - scale;
}

int GfxCanvas::textHeight(uint8_t scale) {
    if (scale == 0) {
        scale = 1;
    }
    return kFontGlyphHeight * scale;
}

void GfxCanvas::drawText(int x, int y, const char* text, GfxColor color, uint8_t scale) {
    if (!valid() || text == nullptr) {
        return;
    }
    if (scale == 0) {
        scale = 1;
    }

    int cursorX = x;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        const uint8_t character = static_cast<uint8_t>(*cursor);
        if (character >= kFontFirstChar && character <= kFontLastChar) {
            const uint8_t* glyph = &kFont5x7[(character - kFontFirstChar) * kFontGlyphWidth];
            for (uint8_t column = 0; column < kFontGlyphWidth; ++column) {
                const uint8_t bits = glyph[column];
                for (uint8_t row = 0; row < kFontGlyphHeight; ++row) {
                    if ((bits & (1U << row)) == 0) {
                        continue;
                    }
                    const int pixelX = cursorX + column * scale;
                    const int pixelY = y + row * scale;
                    if (scale == 1) {
                        drawPixel(pixelX, pixelY, color);
                    } else {
                        fillRect(pixelX, pixelY, scale, scale, color);
                    }
                }
            }
        }
        cursorX += kFontGlyphAdvance * scale;
    }
}

void GfxCanvas::drawTextCentered(int centerX, int y, const char* text, GfxColor color, uint8_t scale) {
    drawText(centerX - textWidth(text, scale) / 2, y, text, color, scale);
}

void GfxCanvas::drawTextRightAligned(int rightX, int y, const char* text, GfxColor color, uint8_t scale) {
    drawText(rightX - textWidth(text, scale), y, text, color, scale);
}

void GfxCanvas::drawCheckMark(int x, int y, int size, GfxColor color) {
    if (size <= 0) {
        return;
    }
    // Two segments: short stroke down-right, long stroke up-right.
    const int shortDX = size / 3;
    const int shortDY = size / 2;
    drawLine(x, y + shortDY, x + shortDX, y + size - 1, color, 3);
    drawLine(x + shortDX, y + size - 1, x + size - 1, y, color, 3);
}

void GfxCanvas::drawUpArrow(int centerX, int topY, int width, int height, GfxColor color) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const int headHeight = height / 2;
    const int shaftWidth = width / 5 > 0 ? width / 5 : 1;
    // Triangular head, filled row by row.
    for (int row = 0; row < headHeight; ++row) {
        const int half = (width / 2) * (row + 1) / headHeight;
        fillRect(centerX - half, topY + row, half * 2 + 1, 1, color);
    }
    fillRect(centerX - shaftWidth / 2, topY + headHeight, shaftWidth, height - headHeight, color);
}

void GfxCanvas::drawWifiIcon(int x, int y, GfxColor color) {
    // Classic Wi-Fi fan: three 1 px concentric arcs restricted to the 45..135
    // degree cone (|dy| >= |dx|) so they stay inside the 12x9 px box, centred
    // on the bottom edge, plus the dot. Integer only, so the shape is
    // deterministic; it is verified visually through the ASCII-art dump in
    // tests/ui_model_test.cpp.
    const int centerX = x + kWifiIconWidth / 2;
    const int centerY = y + kWifiIconHeight;
    const int radii[3] = {3, 6, 9};

    for (int py = y; py < y + kWifiIconHeight; ++py) {
        for (int px = x; px < x + kWifiIconWidth; ++px) {
            const int dx = px - centerX;
            const int dy = py - centerY;
            if (dy >= 0 || absInt(dy) < absInt(dx)) {
                continue;
            }
            const int distanceSquared = dx * dx + dy * dy;
            for (int ring = 0; ring < 3; ++ring) {
                const int outer = radii[ring] * radii[ring];
                const int inner = (radii[ring] - 1) * (radii[ring] - 1);
                if (distanceSquared <= outer && distanceSquared > inner) {
                    drawPixel(px, py, color);
                    break;
                }
            }
        }
    }

    fillRect(centerX - 1, y + kWifiIconHeight - 2, 2, 2, color);
}

void GfxCanvas::drawMiddleDot(int centerX, int centerY, GfxColor color) {
    fillRect(centerX - 1, centerY - 1, 3, 3, color);
}

}  // namespace voice_memo_ui
