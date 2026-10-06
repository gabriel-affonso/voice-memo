/*
 * Frank - Cardputer ADV v0.1
 * ============================================================================
 * Hardware bring-up + visual identity test for the M5Stack Cardputer ADV.
 *
 * Scope of v0.1 (deliberately small):
 *   - initialize the Cardputer ADV (board auto-detected at runtime)
 *   - initialize the LCD in landscape
 *   - initialize the physical keyboard through the official abstraction
 *   - render a pixel-art Frank face (READY / KEYBOARD_OK)
 *   - toggle the state on SPACE, exactly once per physical press
 *
 * Explicitly NOT in v0.1: audio, microSD, Wi-Fi, NUC upload, Notion, tags,
 * power management, recording. No partition or OTA code: this firmware is a
 * plain application image that M5Launcher installs into the app partition.
 *
 * Target hardware : M5Stack Cardputer ADV (ESP32-S3, 240x135 ST7789 LCD,
 *                   TCA8418 I2C keyboard) and, incidentally, the original
 *                   Cardputer (GPIO matrix keyboard).
 * Library         : M5Cardputer (>= 1.2.0) -> M5Unified (>= 0.2.25)
 *                   -> M5GFX (>= 0.2.32)
 *
 * The Cardputer / Cardputer ADV difference (LCD panel ID probe + keyboard
 * controller) is resolved at runtime by M5GFX / M5Unified, so the binary is
 * board-agnostic; the fallback board is pinned to the ADV because this is an
 * ADV-first firmware.
 * ============================================================================
 */

#include <M5Cardputer.h>

// ---------------------------------------------------------------------------
// Palette. M5GFX (lgfx) constants only: black ground, terminal green, white.
// ---------------------------------------------------------------------------
static constexpr uint16_t kBg     = lgfx::colors::TFT_BLACK;
static constexpr uint16_t kInk    = lgfx::colors::TFT_GREEN;     // face, frame, READY
static constexpr uint16_t kInkAlt = lgfx::colors::TFT_WHITE;     // eyes, labels, KEYBOARD OK
static constexpr uint16_t kInkDim = lgfx::colors::TFT_DARKGREEN; // unlit indicator LED

// ---------------------------------------------------------------------------
// Pixel-art grid. The face is drawn as 4x4 blocks on a 20x21 cell grid so the
// result stays blocky and monochrome-disciplined, never anti-aliased.
// ---------------------------------------------------------------------------
static constexpr int kCell     = 4;                     // one pixel-art "pixel"
static constexpr int kFaceCols = 20;
static constexpr int kFaceRows = 21;
static constexpr int kFaceW    = kFaceCols * kCell;     // 80 px
static constexpr int kFaceH    = kFaceRows * kCell;     // 84 px

// Built-in font 0 (GLCD 6x8) metrics, used to centre text deterministically.
static constexpr int kGlyphW = 6;
static constexpr int kGlyphH = 8;

// ---------------------------------------------------------------------------
// State machine. One expression per state in v0.1; later stages can decouple
// them (e.g. state = Recording, expression = Listening) without touching the
// renderer's call site.
// ---------------------------------------------------------------------------
enum class FrankState : uint8_t {
    Ready,
    KeyboardOk,
};

using FrankExpression = FrankState;

struct FrankLayout {
    int faceX;
    int faceY;
    int titleY;
    int chipX;
    int chipY;
    int chipW;
    int chipH;
    int hintY;
};

static FrankLayout g_ui;

static FrankState g_state        = FrankState::Ready;
static bool       g_spaceWasDown = false;  // previous sample, for edge detection

// Edge detection is the primary mechanism; this guard only swallows the
// contact ripple of a single physical press (a few ms at most). It is not a
// "wait and see" debounce and never blocks a deliberate second press.
static constexpr uint32_t kSpaceEdgeGuardMs = 25;
static uint32_t           g_lastSpaceEdgeMs = 0;

// Serial diagnostic id (see the log lines in the task spec).
static const char *stateName(FrankState state)
{
    return state == FrankState::KeyboardOk ? "KEYBOARD_OK" : "READY";
}

// On-screen label.
static const char *stateLabel(FrankState state)
{
    return state == FrankState::KeyboardOk ? "KEYBOARD OK" : "READY";
}

// ---------------------------------------------------------------------------
// Layout: designed for the Cardputer landscape framebuffer (240x135).
// Horizontal placement follows the runtime width; vertical placement assumes
// the 135 px landscape height.
// ---------------------------------------------------------------------------
static void layoutUi()
{
    const int w = M5Cardputer.Display.width();

    g_ui.faceX  = (w - kFaceW) / 2;
    g_ui.faceY  = 1;
    g_ui.titleY = g_ui.faceY + kFaceH + 2;          // 87 on a 135 px screen
    g_ui.chipW  = 132;
    g_ui.chipH  = 16;
    g_ui.chipX  = (w - g_ui.chipW) / 2;
    g_ui.chipY  = g_ui.titleY + (kGlyphH * 2) + 2;  // 105
    g_ui.hintY  = g_ui.chipY + g_ui.chipH + 2;      // 123
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static void drawTextCentered(const char *text, int y, int size, uint16_t color)
{
    const int w = (int)strlen(text) * kGlyphW * size;

    M5Cardputer.Display.setTextSize((float)size);
    M5Cardputer.Display.setTextColor(color, kBg);
    M5Cardputer.Display.setCursor((M5Cardputer.Display.width() - w) / 2, y);
    M5Cardputer.Display.print(text);
}

// One block of the face grid -> one solid rectangle on screen.
static void faceBlock(int col, int row, int w, int h, uint16_t color)
{
    M5Cardputer.Display.fillRect(g_ui.faceX + col * kCell, g_ui.faceY + row * kCell,
                                 w * kCell, h * kCell, color);
}

// Screen frame: four blocky corner ticks, no rounded corners, no gradients.
static void drawCornerTicks()
{
    auto &d      = M5Cardputer.Display;
    const int w  = d.width();
    const int h  = d.height();
    const int arm = 8;
    const int th  = 2;

    d.fillRect(0, 0, arm, th, kInk);
    d.fillRect(0, 0, th, arm, kInk);

    d.fillRect(w - arm, 0, arm, th, kInk);
    d.fillRect(w - th, 0, th, arm, kInk);

    d.fillRect(0, h - th, arm, th, kInk);
    d.fillRect(0, h - arm, th, arm, kInk);

    d.fillRect(w - arm, h - th, arm, th, kInk);
    d.fillRect(w - th, h - arm, th, arm, kInk);
}

// Frank himself: boxy head, flat hair cap with a jagged fringe, neck bolts,
// white blocky eyes, stitched cheeks. Only two expressions exist in v0.1.
static void drawFrank(FrankExpression expression)
{
    const bool open = (expression == FrankExpression::KeyboardOk);

    // Neck bolts, left and right of the head.
    faceBlock(0, 8, 2, 2, kInk);
    faceBlock(18, 8, 2, 2, kInk);

    // Hair cap and head block.
    faceBlock(4, 0, 12, 1, kInk);   // hair, widest step
    faceBlock(3, 1, 14, 1, kInk);   // hair
    faceBlock(2, 2, 16, 1, kInk);   // hair, meets the skull
    faceBlock(2, 3, 16, 15, kInk);  // head: cols 2..17, rows 3..17

    // Jagged fringe where the hair meets the forehead (symmetric about col 9.5).
    faceBlock(5, 2, 1, 1, kBg);
    faceBlock(8, 2, 1, 1, kBg);
    faceBlock(11, 2, 1, 1, kBg);
    faceBlock(14, 2, 1, 1, kBg);

    // Hollow the head so only a thick green outline remains.
    faceBlock(3, 4, 14, 13, kBg);  // cols 3..16, rows 4..16

    // Neck and shoulders.
    faceBlock(8, 18, 4, 2, kInk);
    faceBlock(3, 20, 14, 1, kInk);

    // Brows: one cell higher when the keyboard answers.
    const int browRow = open ? 4 : 5;
    faceBlock(4, browRow, 3, 1, kInk);
    faceBlock(13, browRow, 3, 1, kInk);

    // Eyes: 3 cells wide, one cell wider (outwards) on KEYBOARD_OK.
    const int widen = open ? 1 : 0;
    faceBlock(4 - widen, 6, 3 + widen, 2, kInkAlt);  // left eye
    faceBlock(13, 6, 3 + widen, 2, kInkAlt);         // right eye
    faceBlock(5, 7, 1, 1, kBg);                      // left pupil
    faceBlock(14, 7, 1, 1, kBg);                     // right pupil

    // Nose and cheek stitches.
    faceBlock(9, 9, 2, 2, kInk);
    faceBlock(4, 9, 1, 2, kInk);
    faceBlock(15, 9, 1, 2, kInk);

    // Mouth: flat and deadpan, corners lift on KEYBOARD_OK.
    faceBlock(6, 13, 8, 1, kInk);
    if (open) {
        faceBlock(5, 12, 1, 1, kInk);
        faceBlock(14, 12, 1, 1, kInk);
    }
}

// Status readout: HUD-style corner brackets + one indicator LED.
static void drawStatusChip(FrankState state)
{
    auto &d      = M5Cardputer.Display;
    const bool ok = (state == FrankState::KeyboardOk);
    const int x  = g_ui.chipX;
    const int y  = g_ui.chipY;
    const int w  = g_ui.chipW;
    const int h  = g_ui.chipH;
    const int arm = 8;
    const int th  = 2;

    d.fillRect(x, y, arm, th, kInk);
    d.fillRect(x, y, th, arm, kInk);

    d.fillRect(x + w - arm, y, arm, th, kInk);
    d.fillRect(x + w - th, y, th, arm, kInk);

    d.fillRect(x, y + h - th, arm, th, kInk);
    d.fillRect(x, y + h - arm, th, arm, kInk);

    d.fillRect(x + w - arm, y + h - th, arm, th, kInk);
    d.fillRect(x + w - th, y + h - arm, th, arm, kInk);

    // Indicator: dim when idle, lit once the keyboard has answered.
    d.fillRect(x + w - 14, y + (h / 2) - 2, 4, 4, ok ? kInkAlt : kInkDim);

    drawTextCentered(stateLabel(state), y + (h - kGlyphH) / 2, 1, ok ? kInkAlt : kInk);
}

static void renderScreen(FrankState state)
{
    auto &d = M5Cardputer.Display;

    d.startWrite();
    d.fillScreen(kBg);
    drawCornerTicks();
    drawFrank(state);
    drawTextCentered("F R A N K", g_ui.titleY, 2, kInk);
    drawStatusChip(state);
    drawTextCentered(state == FrankState::KeyboardOk ? "SPACE : BACK" : "SPACE : TEST",
                     g_ui.hintY, 1, kInkAlt);
    d.endWrite();
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
static void setState(FrankState next)
{
    if (next == g_state) {
        return;
    }
    g_state = next;
    renderScreen(g_state);
    Serial.printf("[frank] state=%s\n", stateName(g_state));
}

static void onSpacePressed()
{
    Serial.println("[frank] SPACE pressed");
    setState(g_state == FrankState::Ready ? FrankState::KeyboardOk : FrankState::Ready);
}

// ---------------------------------------------------------------------------
// Serial diagnostics
// ---------------------------------------------------------------------------
static bool g_serialAnnounced = false;

// The USB CDC port may be opened after boot; repeat the header so the
// diagnostics are visible to a monitor that attaches late.
static void announceSerialIfConnected()
{
    const bool connected = (bool)Serial;
    if (connected == g_serialAnnounced) {
        return;
    }
    g_serialAnnounced = connected;
    if (!connected) {
        return;
    }
    Serial.println("[frank] Cardputer ADV v0.1");
    Serial.printf("[frank] state=%s\n", stateName(g_state));
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("[frank] Cardputer ADV v0.1");

    auto cfg           = M5.config();
    cfg.fallback_board = m5::board_t::board_M5CardputerADV;  // ADV-first firmware
    cfg.internal_mic   = false;  // v0.1 has no audio at all
    cfg.internal_spk   = false;

    M5Cardputer.begin(cfg, true);  // M5Unified bring-up + keyboard

    M5Cardputer.Display.setRotation(1);  // landscape (240x135)
    layoutUi();

    const bool adv = (M5.getBoard() == m5::board_t::board_M5CardputerADV);
    Serial.printf("[frank] board=%s\n", adv ? "Cardputer ADV" : "Cardputer");
    Serial.printf("[frank] display initialized (%dx%d)\n",
                  (int)M5Cardputer.Display.width(), (int)M5Cardputer.Display.height());
    Serial.printf("[frank] keyboard initialized (%s)\n", adv ? "TCA8418" : "GPIO matrix");

    renderScreen(g_state);
    Serial.printf("[frank] %s\n", stateName(g_state));

    g_spaceWasDown    = M5Cardputer.Keyboard.keysState().space;
    g_lastSpaceEdgeMs = millis();
    g_serialAnnounced = (bool)Serial;
}

void loop()
{
    M5Cardputer.update();  // M5Unified + keyboard key list / key state

    // Edge detection on the SPACE key: one toggle per physical press, no
    // auto-repeat while the key is held, no delay-based debouncing.
    const bool spaceDown = M5Cardputer.Keyboard.keysState().space;
    if (spaceDown && !g_spaceWasDown) {
        const uint32_t now = millis();
        if (now - g_lastSpaceEdgeMs >= kSpaceEdgeGuardMs) {
            g_lastSpaceEdgeMs = now;
            onSpacePressed();
        }
    }
    g_spaceWasDown = spaceDown;

    announceSerialIfConnected();
}
