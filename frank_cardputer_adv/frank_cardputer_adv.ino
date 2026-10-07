/*
 * Frank - Cardputer ADV v0.2.1  --  AUDIO DIAGNOSTIC
 * ============================================================================
 * v0.2 (record -> playback) produced silence on real hardware, while display and
 * keyboard were confirmed working. This build exists only to find out WHERE the
 * audio chain breaks, by testing the three links independently:
 *
 *   1 : SPEAKER      - generated tones only, no microphone data at all
 *   2 : MIC LEVEL    - live PCM level meter, no playback at all
 *   3 : RECORD/PLAY  - capture then play back the captured buffer
 *
 * It keeps the validated v0.2 display init, keyboard handling (TCA8418, edge
 * detected keys) and the Frank pixel-art identity. No microSD, no Wi-Fi, no
 * upload, no RecordingApp.
 *
 * The two links that v0.2 could not separate:
 *   - playback plays whatever was captured, so "silent playback" is also the
 *     expected result of a silent capture. Test 1 removes the microphone from
 *     the question entirely.
 *   - M5Unified 0.2.25 drives the ESP32-S3 I2S clock through a RAW divider
 *     override (M5UNIFIED_I2S_DRIVER_MANAGED_CLK = 0 for ESP32-S3): it selects
 *     the PLL_D2 / "PLL_240M" source (rx/tx_clk_sel = 1) and computes the
 *     divider from a 120 MHz base. ESP-IDF only gained I2S_CLK_SRC_PLL_240M for
 *     the ESP32-S3 recently (espressif/esp-idf#17056), so this path is exactly
 *     where an IDF 5.4 -> 5.5 change can break the ES8311 clock. Test 2 and
 *     test 3 therefore also report the EFFECTIVE sample rate (samples / wall
 *     time), which is the fastest way to see a wrong I2S clock.
 *
 * Build A : Arduino ESP32 core 3.3.11 / ESP-IDF 5.5.5
 * Build B : Arduino ESP32 core 3.2.1  / ESP-IDF 5.4.2
 * The running build prints its own IDF and core version on screen and on Serial.
 * ============================================================================
 */

#include <M5Cardputer.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_arduino_version.h>

// ---------------------------------------------------------------------------
// Palette (unchanged from v0.1/v0.2)
// ---------------------------------------------------------------------------
static constexpr uint16_t kBg     = lgfx::colors::TFT_BLACK;
static constexpr uint16_t kInk    = lgfx::colors::TFT_GREEN;
static constexpr uint16_t kInkAlt = lgfx::colors::TFT_WHITE;
static constexpr uint16_t kInkDim = lgfx::colors::TFT_DARKGREEN;

// ---------------------------------------------------------------------------
// Pixel-art grid (unchanged face, drawn at different scales per screen)
// ---------------------------------------------------------------------------
static constexpr int kCell     = 4;
static constexpr int kFaceCols = 20;
static constexpr int kFaceRows = 21;

static constexpr int kGlyphW = 6;
static constexpr int kGlyphH = 8;

// ---------------------------------------------------------------------------
// Audio constants
// ---------------------------------------------------------------------------
static constexpr uint32_t kSampleRate    = 16000;   // Frank backend format
static constexpr size_t   kBlockSamples  = 320;     // 20 ms block (level test + capture chunk)
static constexpr size_t   kMaxInFlight   = 2;       // M5Unified has two request slots
static constexpr uint32_t kMicWarmupMs   = 1200;    // ES8311 stabilisation, never recorded
static constexpr uint32_t kRecMaxSecs    = 5;       // diagnostic capture limit
static constexpr size_t   kMinFreeHeapAfterBuffer = 32 * 1024;
static constexpr uint8_t  kVolume        = 180;     // speaker master volume 0..255

// Level meter: 29 blocks of 6 px + 2 px gap = 230 px
static constexpr int kBarBlocks = 29;
static constexpr int kBarX      = 4;
// Record screen meter starts at x=50: 50 + 23*8 - 2 = 232 px, stays on screen.
static constexpr int kRecordBarBlocks = 23;

// Window / refresh rates
static constexpr uint32_t kLevelBlocksPerWindow = 5;    // 5 x 20 ms = 100 ms -> 10 Hz
static constexpr uint32_t kSerialEveryWindows   = 5;    // -> 2 Hz serial
static constexpr uint32_t kScreenRedrawMs       = 100;  // 10 Hz partial redraw

// ---------------------------------------------------------------------------
// Screens and sub-states
// ---------------------------------------------------------------------------
enum class Screen : uint8_t { Menu, Speaker, MicLevel, RecordPlay };
enum class FrankExpression : uint8_t { Neutral, Listening };
enum class SpkPhase : uint8_t { Idle, Tone1, Tone2, Ask };
enum class RecPhase : uint8_t { Idle, Warmup, Ready, Capturing, Playback, Captured };

static Screen  g_screen = Screen::Menu;

// ---------------------------------------------------------------------------
// Key edges (validated v0.2 mechanism, reused for every key)
// ---------------------------------------------------------------------------
static constexpr uint32_t kKeyEdgeGuardMs = 25;

struct KeyEdge {
    bool     wasDown;
    uint32_t lastMs;
};

static KeyEdge g_spaceKey = { false, 0 };
static KeyEdge g_enterKey = { false, 0 };
static KeyEdge g_escKey   = { false, 0 };
static KeyEdge g_oneKey   = { false, 0 };
static KeyEdge g_twoKey   = { false, 0 };
static KeyEdge g_threeKey = { false, 0 };
static KeyEdge g_yKey     = { false, 0 };
static KeyEdge g_nKey     = { false, 0 };

static bool keyEdge(KeyEdge &key, bool down)
{
    const bool edge = down && !key.wasDown;
    key.wasDown     = down;
    if (!edge) {
        return false;
    }
    const uint32_t now = millis();
    if (now - key.lastMs < kKeyEdgeGuardMs) {
        return false;
    }
    key.lastMs = now;
    return true;
}

static bool wordHas(const Keyboard_Class::KeysState &ks, char c)
{
    for (size_t i = 0; i < ks.word.size(); ++i) {
        if (ks.word[i] == c) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Diagnostics state
// ---------------------------------------------------------------------------
static int  g_spkHeard   = -1;       // -1 unknown, 0 no, 1 yes
static int32_t g_micPeakSeen = 0;    // best peak observed in the level test
static bool g_micBeginOk = false;    // Mic.begin() && Mic.isEnabled() of the current test

// ---------------------------------------------------------------------------
// Test 1 - speaker
// ---------------------------------------------------------------------------
static SpkPhase g_spkPhase = SpkPhase::Idle;
static uint32_t g_spkPhaseMs = 0;

// ---------------------------------------------------------------------------
// Test 2 - microphone level
// ---------------------------------------------------------------------------
static int16_t  g_block[kBlockSamples];
static bool     g_blockPending = false;

static uint32_t g_winBlocks   = 0;    // blocks in the current window
static int32_t  g_winMin      = 0;
static int32_t  g_winMax      = 0;
static int32_t  g_winPeak     = 0;
static uint64_t g_winSumSq    = 0;
static uint32_t g_winZeros    = 0;
static uint32_t g_winSamples  = 0;
static int32_t  g_winConstant = 0;    // value when every sample is identical
static bool     g_winAllSame  = true;
static uint32_t g_winCount    = 0;    // completed windows (for serial throttle)
static int16_t  g_lastRaw[8]  = { 0 };
static bool     g_haveRaw     = false;
static uint32_t g_lastSerialMs = 0;

// Displayed window values
static int32_t g_dispMin = 0, g_dispMax = 0, g_dispPeak = 0;
static double  g_dispRms = 0.0;
static double  g_dispZeroPct = 0.0;
static bool    g_dispConstant = false;
static int32_t g_dispConstantValue = 0;

// ---------------------------------------------------------------------------
// Test 3 - record / playback (v0.2 pipeline + warm-up + drain)
// ---------------------------------------------------------------------------
static int16_t *g_pcm        = nullptr;
static size_t   g_capacity   = 0;
static size_t   g_target     = 0;
static size_t   g_bufferBytes = 0;
static uint32_t g_bufferSecs  = 0;
static uint32_t g_heapBefore  = 0;
static uint32_t g_heapAfter   = 0;

static volatile size_t g_queued  = 0;
static volatile size_t g_ready   = 0;
static volatile bool   g_capture = false;
static volatile int32_t g_lastChunkPeak = 0;
static size_t          g_samples = 0;

static RecPhase g_recPhase    = RecPhase::Idle;
static uint32_t g_recPhaseMs  = 0;
static uint32_t g_recWallMs   = 0;   // wall clock of the capture itself
static uint32_t g_lastScreenMs = 0;

// Recording diagnostics
static int32_t g_recPeak = 0;
static int32_t g_recMin  = 0;
static int32_t g_recMax  = 0;
static double  g_recRms  = 0.0;
static double  g_recZeroPct = 0.0;
static double  g_recRate = 0.0;
static bool    g_recConstant = false;
static int32_t g_recConstantValue = 0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static void drawTextCentered(const char *text, int y, int size, uint16_t color)
{
    const int w = (int)strlen(text) * kGlyphW * size;
    M5Cardputer.Display.setTextSize((float)size);
    M5Cardputer.Display.setTextColor(color, kBg);
    M5Cardputer.Display.setCursor((M5Cardputer.Display.width() - w) / 2, y);
    M5Cardputer.Display.print(text);
}

static void drawTextAt(int x, int y, const char *text, int size, uint16_t color)
{
    M5Cardputer.Display.setTextSize((float)size);
    M5Cardputer.Display.setTextColor(color, kBg);
    M5Cardputer.Display.setCursor(x, y);
    M5Cardputer.Display.print(text);
}

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

// ---------------------------------------------------------------------------
// Frank face, drawable at any scale/origin
// ---------------------------------------------------------------------------
static int g_faceX = 0, g_faceY = 0, g_faceCell = kCell;

static void faceBlock(int col, int row, int w, int h, uint16_t color)
{
    M5Cardputer.Display.fillRect(g_faceX + col * g_faceCell, g_faceY + row * g_faceCell,
                                 w * g_faceCell, h * g_faceCell, color);
}

static void drawFrank(FrankExpression expression, int x, int y, int cell)
{
    g_faceX    = x;
    g_faceY    = y;
    g_faceCell = cell;

    const bool listening = (expression == FrankExpression::Listening);

    faceBlock(0, 8, 2, 2, kInk);
    faceBlock(18, 8, 2, 2, kInk);

    faceBlock(4, 0, 12, 1, kInk);
    faceBlock(3, 1, 14, 1, kInk);
    faceBlock(2, 2, 16, 1, kInk);
    faceBlock(2, 3, 16, 15, kInk);

    faceBlock(5, 2, 1, 1, kBg);
    faceBlock(8, 2, 1, 1, kBg);
    faceBlock(11, 2, 1, 1, kBg);
    faceBlock(14, 2, 1, 1, kBg);

    faceBlock(3, 4, 14, 13, kBg);

    faceBlock(8, 18, 4, 2, kInk);
    faceBlock(3, 20, 14, 1, kInk);

    const int browRow = listening ? 4 : 5;
    faceBlock(4, browRow, 3, 1, kInk);
    faceBlock(13, browRow, 3, 1, kInk);

    const int widen = listening ? 1 : 0;
    faceBlock(4 - widen, 6, 3 + widen, 2, kInkAlt);
    faceBlock(13, 6, 3 + widen, 2, kInkAlt);
    faceBlock(5, 7, 1, 1, kBg);
    faceBlock(14, 7, 1, 1, kBg);

    faceBlock(9, 9, 2, 2, kInk);
    faceBlock(4, 9, 1, 2, kInk);
    faceBlock(15, 9, 1, 2, kInk);

    if (listening) {
        faceBlock(7, 13, 6, 1, kInk);
    } else {
        faceBlock(6, 13, 8, 1, kInk);
    }
}

// Blocky level meter: log scale so quiet speech still moves it.
static void drawLevelBar(int x, int y, int blocks, int32_t peak, bool withBackground)
{
    const float full  = log10f(32768.0f);
    const float value = (peak > 0) ? (log10f((float)peak) / full) : 0.0f;
    int lit = (int)(value * (float)blocks + 0.5f);
    if (lit < 0) {
        lit = 0;
    }
    if (lit > blocks) {
        lit = blocks;
    }

    for (int i = 0; i < blocks; ++i) {
        const bool on = (i < lit);
        if (on || withBackground) {
            M5Cardputer.Display.fillRect(x + i * 8, y, 6, 10, on ? kInk : kInkDim);
        }
    }
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
static void drawMenuScreen()
{
    char line[32];

    M5Cardputer.Display.startWrite();
    M5Cardputer.Display.fillScreen(kBg);
    drawCornerTicks();

    drawTextCentered("F R A N K", 2, 2, kInk);
    drawFrank(FrankExpression::Neutral, 10, 24, 3);

    drawTextAt(84, 26, "AUDIO TEST", 1, kInkAlt);
    drawTextAt(84, 42, "1 : SPEAKER", 1, kInk);
    drawTextAt(84, 54, "2 : MIC LEVEL", 1, kInk);
    drawTextAt(84, 66, "3 : RECORD/PLAY", 1, kInk);
    drawTextAt(84, 78, "ESC : BACK", 1, kInkDim);

    snprintf(line, sizeof(line), "IDF %d.%d.%d  core %d.%d.%d",
             ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH,
             ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
    drawTextAt(4, 100, line, 1, kInkDim);

    const char *heard = (g_spkHeard < 0) ? "?" : (g_spkHeard ? "YES" : "NO");
    snprintf(line, sizeof(line), "SPK heard : %s", heard);
    drawTextAt(4, 112, line, 1, kInkAlt);

    snprintf(line, sizeof(line), "MIC peak  : %d", (int)g_micPeakSeen);
    drawTextAt(4, 124, line, 1, kInkAlt);

    M5Cardputer.Display.endWrite();
}

static void drawSpeakerScreen()
{
    M5Cardputer.Display.startWrite();
    M5Cardputer.Display.fillScreen(kBg);
    drawCornerTicks();

    drawFrank(FrankExpression::Neutral, 6, 2, 2);
    drawTextAt(56, 6, "SPEAKER TEST", 2, kInk);

    const char *line = "READY";
    if (g_spkPhase == SpkPhase::Tone1) {
        line = "TONE 1 : 440 Hz";
    } else if (g_spkPhase == SpkPhase::Tone2) {
        line = "TONE 2 : 880 Hz";
    } else if (g_spkPhase == SpkPhase::Ask) {
        line = "COMPLETE";
    }
    drawTextAt(56, 28, line, 1, kInkAlt);

    if (g_spkPhase == SpkPhase::Ask) {
        drawTextCentered("HEARD TONES ?", 58, 2, kInkAlt);
        drawTextCentered("Y : YES    N : NO", 80, 1, kInk);
    } else {
        drawTextAt(56, 44, "speaker only, no mic", 1, kInkDim);
    }

    drawTextAt(8, 118, "ESC : BACK", 1, kInkDim);
    M5Cardputer.Display.endWrite();
}

static void drawMicScreen()
{
    char line[40];

    M5Cardputer.Display.startWrite();
    M5Cardputer.Display.fillScreen(kBg);
    drawCornerTicks();

    drawFrank(FrankExpression::Listening, 6, 2, 2);
    drawTextAt(56, 6, "MIC LEVEL", 2, kInk);
    drawTextAt(56, 28, "SPEAK / WHISTLE / CLAP", 1, kInkDim);

    drawTextAt(4, 46, "LEVEL", 1, kInkAlt);
    drawLevelBar(kBarX, 56, kBarBlocks, g_dispPeak, true);

    snprintf(line, sizeof(line), "PEAK %d   RMS %.0f", (int)g_dispPeak, g_dispRms);
    drawTextAt(4, 74, line, 1, kInkAlt);

    snprintf(line, sizeof(line), "MIN %d   MAX %d", (int)g_dispMin, (int)g_dispMax);
    drawTextAt(4, 86, line, 1, kInkAlt);

    snprintf(line, sizeof(line), "ZERO %.2f%%", g_dispZeroPct);
    drawTextAt(4, 98, line, 1, kInkAlt);

    if (!g_micBeginOk) {
        drawTextAt(4, 110, "MIC BEGIN FAILED", 1, kInkAlt);
    } else if (g_dispConstant) {
        snprintf(line, sizeof(line), "CONSTANT SAMPLE %d", (int)g_dispConstantValue);
        drawTextAt(4, 110, line, 1, kInkAlt);
    } else {
        drawTextAt(4, 110, "SAMPLES VARY : OK", 1, kInk);
    }

    drawTextAt(4, 122, "ESC : BACK", 1, kInkDim);
    M5Cardputer.Display.endWrite();
}

// Only the dynamic part of the record screen is redrawn (10 Hz).
static void drawRecordStatus()
{
    char line[40];
    auto &d = M5Cardputer.Display;

    d.startWrite();
    d.fillRect(50, 28, 190, 62, kBg);

    const char *status = "READY";
    switch (g_recPhase) {
        case RecPhase::Warmup: {
            const uint32_t left = (kMicWarmupMs > (millis() - g_recPhaseMs))
                                      ? (kMicWarmupMs - (millis() - g_recPhaseMs)) : 0;
            snprintf(line, sizeof(line), "CODEC WARM-UP %ums", (unsigned)left);
            drawTextAt(56, 28, line, 1, kInkAlt);
            drawTextAt(56, 44, "not recorded", 1, kInkDim);
            d.endWrite();
            return;
        }
        case RecPhase::Ready:     status = g_micBeginOk ? "READY TO RECORD" : "MIC BEGIN FAILED"; break;
        case RecPhase::Capturing: status = "RECORDING";       break;
        case RecPhase::Playback:  status = "PLAYING";         break;
        case RecPhase::Captured:  status = "RECORDED";        break;
        case RecPhase::Idle:      status = "IDLE";            break;
    }
    drawTextAt(56, 28, status, 1, kInkAlt);

    if (g_recPhase == RecPhase::Capturing) {
        snprintf(line, sizeof(line), "%.1f s   %u samples",
                 (double)g_ready / (double)kSampleRate, (unsigned)g_ready);
        drawTextAt(56, 42, line, 1, kInkAlt);
        drawLevelBar(50, 56, kRecordBarBlocks, g_lastChunkPeak, true);
    } else if (g_recPhase == RecPhase::Captured || g_recPhase == RecPhase::Playback) {
        snprintf(line, sizeof(line), "%.2f s  peak %d", (double)g_samples / (double)kSampleRate,
                 (int)g_recPeak);
        drawTextAt(56, 42, line, 1, kInkAlt);
        snprintf(line, sizeof(line), "rms %.0f  zero %.1f%%  rate %.0f",
                 g_recRms, g_recZeroPct, g_recRate);
        drawTextAt(56, 56, line, 1, kInkAlt);
        if (g_recConstant) {
            snprintf(line, sizeof(line), "CONSTANT %d", (int)g_recConstantValue);
            drawTextAt(56, 68, line, 1, kInkAlt);
        }
    }

    d.endWrite();
}

static void drawRecordScreen()
{
    M5Cardputer.Display.startWrite();
    M5Cardputer.Display.fillScreen(kBg);
    drawCornerTicks();
    drawFrank((g_recPhase == RecPhase::Capturing) ? FrankExpression::Listening
                                                  : FrankExpression::Neutral,
              6, 2, 2);
    drawTextAt(56, 6, "RECORD/PLAY", 2, kInk);

    const char *hint = "SPACE : START";
    if (g_recPhase == RecPhase::Capturing) {
        hint = "SPACE : STOP";
    } else if (g_recPhase == RecPhase::Captured) {
        hint = "ENTER : PLAY   SPACE : REC";
    } else if (g_recPhase == RecPhase::Playback) {
        hint = "PLAYING ...";
    }
    drawTextAt(4, 116, hint, 1, kInkAlt);
    drawTextAt(4, 126, "ESC : MENU", 1, kInkDim);
    M5Cardputer.Display.endWrite();

    drawRecordStatus();
}

// ---------------------------------------------------------------------------
// Logging helpers
// ---------------------------------------------------------------------------
static void logVersions()
{
    Serial.printf("[audio-test] idf=%d.%d.%d arduino_core=%d.%d.%d\n",
                  ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH,
                  ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
}

static void logMicConfig()
{
    const auto mc = M5Cardputer.Mic.config();
    Serial.printf("[audio-test] mic_cfg rate=%u over_sampling=%u magnification=%u "
                  "pin_in=%d pin_ws=%d pin_bck=%d pin_mck=%d i2s_port=%d stereo=%u enabled=%d\n",
                  (unsigned)mc.sample_rate, (unsigned)mc.over_sampling,
                  (unsigned)mc.magnification, mc.pin_data_in, mc.pin_ws, mc.pin_bck,
                  mc.pin_mck, (int)mc.i2s_port, (unsigned)mc.stereo,
                  M5Cardputer.Mic.isEnabled() ? 1 : 0);
}

static void logSpeakerConfig()
{
    const auto sc = M5Cardputer.Speaker.config();
    Serial.printf("[audio-test] spk_cfg pin_out=%d pin_ws=%d pin_bck=%d i2s_port=%d "
                  "magnification=%u stereo=%u buzzer=%u\n",
                  sc.pin_data_out, sc.pin_ws, sc.pin_bck, (int)sc.i2s_port,
                  (unsigned)sc.magnification, (unsigned)sc.stereo, (unsigned)sc.buzzer);
}

static bool i2cProbe(uint8_t addr)
{
    if (!M5Cardputer.In_I2C.start(addr, false, 100000)) {
        return false;
    }
    M5Cardputer.In_I2C.stop();
    return true;
}

static void logI2cDevices()
{
    Serial.printf("[audio-test] i2c es8311(0x18)=%d tca8418(0x34)=%d\n",
                  i2cProbe(0x18) ? 1 : 0, i2cProbe(0x34) ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Test 1 - speaker
// ---------------------------------------------------------------------------
static void enterSpeakerTest()
{
    M5Cardputer.Mic.end();  // microphone and speaker share the ES8311

    const bool begun = M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(kVolume);

    Serial.println("[audio-test] --- TEST 1 : SPEAKER ---");
    Serial.printf("[audio-test] speaker begin=%d\n", begun ? 1 : 0);
    logSpeakerConfig();
    logI2cDevices();

    const bool t1 = M5Cardputer.Speaker.tone(440.0f, 400);
    Serial.printf("[audio-test] tone 1 (440 Hz, 400 ms) queued=%d\n", t1 ? 1 : 0);

    g_spkPhase  = SpkPhase::Tone1;
    g_spkPhaseMs = millis();
    g_screen    = Screen::Speaker;
    drawSpeakerScreen();
}

static void updateSpeakerTest()
{
    const uint32_t elapsed = millis() - g_spkPhaseMs;

    if (g_spkPhase == SpkPhase::Tone1 && elapsed >= 600) {
        const bool t2 = M5Cardputer.Speaker.tone(880.0f, 400);
        Serial.printf("[audio-test] tone 2 (880 Hz, 400 ms) queued=%d\n", t2 ? 1 : 0);
        g_spkPhase   = SpkPhase::Tone2;
        g_spkPhaseMs = millis();
        drawSpeakerScreen();
    } else if (g_spkPhase == SpkPhase::Tone2 && elapsed >= 600) {
        M5Cardputer.Speaker.end();
        Serial.println("[audio-test] speaker test complete");
        Serial.println("[audio-test] speaker heard=WAITING_FOR_ANSWER");
        g_spkPhase   = SpkPhase::Ask;
        g_spkPhaseMs = millis();
        drawSpeakerScreen();
    }
}

// ---------------------------------------------------------------------------
// Test 2 - microphone level
// ---------------------------------------------------------------------------
static void resetWindow()
{
    g_winBlocks   = 0;
    g_winMin      = 0;
    g_winMax      = 0;
    g_winPeak     = 0;
    g_winSumSq    = 0;
    g_winZeros    = 0;
    g_winSamples  = 0;
    g_winAllSame  = true;
    g_winConstant = 0;
}

static void analyzeBlock(const int16_t *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const int32_t v = b[i];
        const int32_t a = (v < 0) ? -v : v;

        if (g_winSamples == 0) {
            g_winMin = g_winMax = v;
            g_winConstant = v;
        } else {
            if (v < g_winMin) { g_winMin = v; }
            if (v > g_winMax) { g_winMax = v; }
            if (v != g_winConstant) { g_winAllSame = false; }
        }
        if (a > g_winPeak) { g_winPeak = a; }
        if (v == 0) { ++g_winZeros; }
        g_winSumSq += (uint64_t)(v * v);
        ++g_winSamples;

        if (i < 8) {
            g_lastRaw[i] = (int16_t)v;
        }
    }
    g_haveRaw = true;
    ++g_winBlocks;
}

static void publishWindow()
{
    if (g_winSamples == 0) {
        return;
    }

    g_dispMin       = g_winMin;
    g_dispMax       = g_winMax;
    g_dispPeak      = g_winPeak;
    g_dispRms       = sqrt((double)g_winSumSq / (double)g_winSamples);
    g_dispZeroPct   = 100.0 * (double)g_winZeros / (double)g_winSamples;
    g_dispConstant  = g_winAllSame;
    g_dispConstantValue = g_winConstant;

    if (g_winPeak > g_micPeakSeen) {
        g_micPeakSeen = g_winPeak;
    }

    ++g_winCount;

    const uint32_t now = millis();
    if (now - g_lastSerialMs >= 500) {
        g_lastSerialMs = now;
        Serial.printf("[audio-test] min=%d\n", (int)g_winMin);
        Serial.printf("[audio-test] max=%d\n", (int)g_winMax);
        Serial.printf("[audio-test] peak=%d\n", (int)g_winPeak);
        Serial.printf("[audio-test] rms=%.1f\n", g_dispRms);
        Serial.printf("[audio-test] zero_pct=%.2f\n", g_dispZeroPct);
        if (g_haveRaw) {
            Serial.printf("[audio-test] raw=%d %d %d %d %d %d %d %d\n",
                          g_lastRaw[0], g_lastRaw[1], g_lastRaw[2], g_lastRaw[3],
                          g_lastRaw[4], g_lastRaw[5], g_lastRaw[6], g_lastRaw[7]);
        }
        if (g_winAllSame) {
            Serial.printf("[audio-test] constant_sample=%d\n", (int)g_winConstant);
        }
    }

    drawMicScreen();
    resetWindow();
}

static void enterMicLevelTest()
{
    M5Cardputer.Speaker.end();  // never both at once

    Serial.println("[audio-test] --- TEST 2 : MIC LEVEL ---");
    logMicConfig();

    const bool begun = M5Cardputer.Mic.begin();
    g_micBeginOk = begun && M5Cardputer.Mic.isEnabled();
    Serial.printf("[audio-test] mic begin=%d enabled=%d\n",
                  begun ? 1 : 0, M5Cardputer.Mic.isEnabled() ? 1 : 0);
    logI2cDevices();

    // The ES8311 needs ~1 s to stabilise; samples before that can be zero.
    g_recPhaseMs   = millis();
    g_blockPending = false;
    g_lastSerialMs = 0;
    resetWindow();

    g_dispMin = 0;
    g_dispMax = 0;
    g_dispPeak = 0;
    g_dispRms = 0.0;
    g_dispZeroPct = 0.0;
    g_dispConstant = false;
    g_dispConstantValue = 0;
    g_winCount = 0;

    Serial.printf("[audio-test] warming up %u ms before measuring\n", (unsigned)kMicWarmupMs);
    g_recPhase = RecPhase::Warmup;  // reused for the countdown display
    g_screen   = Screen::MicLevel;
    drawMicScreen();
}

static void updateMicLevelTest()
{
    if (millis() - g_recPhaseMs < kMicWarmupMs) {
        return;  // warm-up: do not measure yet
    }

    if (!g_blockPending) {
        if (M5Cardputer.Mic.record(g_block, kBlockSamples, kSampleRate)) {
            g_blockPending = true;
        }
        return;
    }

    // The block is complete once no request is in flight.
    if (M5Cardputer.Mic.isRecording() == 0) {
        g_blockPending = false;
        analyzeBlock(g_block, kBlockSamples);
        if (g_winBlocks >= kLevelBlocksPerWindow) {
            publishWindow();
        }
    }
}

// ---------------------------------------------------------------------------
// Test 3 - record / playback
// ---------------------------------------------------------------------------
static void onMicBufferReleased(void *args, void *data, size_t length)
{
    (void)args;

    if (g_pcm == nullptr) {
        return;
    }
    const int16_t *p = (const int16_t *)data;
    if (p < g_pcm || p >= g_pcm + g_capacity) {
        return;  // not a recording buffer (e.g. the level-test block)
    }

    int32_t peak = 0;
    for (size_t i = 0; i < length; ++i) {
        const int32_t a = (p[i] < 0) ? -p[i] : p[i];
        if (a > peak) {
            peak = a;
        }
    }
    g_lastChunkPeak = peak;

    const size_t endOffset = (size_t)(p - g_pcm) + length;
    if (endOffset > g_ready) {
        g_ready = endOffset;  // requests complete in order
    }

    if (!g_capture) {
        return;
    }
    const size_t offset = g_queued;
    if (offset + kBlockSamples <= g_capacity &&
        M5Cardputer.Mic.record(&g_pcm[offset], kBlockSamples, kSampleRate)) {
        g_queued = offset + kBlockSamples;
    }
}

// Allocate the recording buffer and log every decision, so a shorter-than-
// requested duration can never be a guess.
static bool allocateAudioBuffer()
{
    static const uint32_t ladderSecs[] = { kRecMaxSecs, 4, 3, 2 };

    g_heapBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    for (size_t i = 0; i < sizeof(ladderSecs) / sizeof(ladderSecs[0]); ++i) {
        const size_t usable   = (size_t)ladderSecs[i] * kSampleRate;
        const size_t capacity = usable + kMaxInFlight * kBlockSamples;
        const size_t bytes    = capacity * sizeof(int16_t);

        if (g_heapBefore < bytes + kMinFreeHeapAfterBuffer) {
            Serial.printf("[audio-test] buffer %us rejected: need %u bytes + %u guard, heap=%u\n",
                          (unsigned)ladderSecs[i], (unsigned)bytes,
                          (unsigned)kMinFreeHeapAfterBuffer, (unsigned)g_heapBefore);
            continue;
        }
        void *p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        if (p == nullptr) {
            Serial.printf("[audio-test] buffer %us rejected: malloc failed\n",
                          (unsigned)ladderSecs[i]);
            continue;
        }

        g_pcm         = (int16_t *)p;
        g_capacity    = capacity;
        g_target      = usable;
        g_bufferBytes = bytes;
        g_bufferSecs  = ladderSecs[i];
        return true;
    }

    // Last resort: PSRAM (only allocated if the build enabled it).
    if (psramFree > 0) {
        const size_t usable   = (size_t)kRecMaxSecs * kSampleRate;
        const size_t capacity = usable + kMaxInFlight * kBlockSamples;
        const size_t bytes    = capacity * sizeof(int16_t);
        if (bytes + kMinFreeHeapAfterBuffer < psramFree) {
            void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
            if (p != nullptr) {
                g_pcm         = (int16_t *)p;
                g_capacity    = capacity;
                g_target      = usable;
                g_bufferBytes = bytes;
                g_bufferSecs  = kRecMaxSecs;
                Serial.println("[audio-test] audio buffer allocated in PSRAM");
                return true;
            }
        }
    }
    return false;
}

static void logBufferDecision()
{
    g_heapAfter = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    Serial.printf("[audio-test] free_heap_before=%u\n", (unsigned)g_heapBefore);
    Serial.printf("[audio-test] psram_free=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    Serial.printf("[audio-test] buffer_duration_selected=%u\n", (unsigned)g_bufferSecs);
    Serial.printf("[audio-test] buffer_bytes=%u\n", (unsigned)g_bufferBytes);
    Serial.printf("[audio-test] free_heap_after=%u\n", (unsigned)g_heapAfter);
}

static void primeCapture()
{
    size_t queued = 0;
    while (queued + kBlockSamples <= g_capacity &&
           M5Cardputer.Mic.isRecording() < kMaxInFlight) {
        if (!M5Cardputer.Mic.record(&g_pcm[queued], kBlockSamples, kSampleRate)) {
            break;
        }
        queued += kBlockSamples;
    }
    g_queued  = queued;
    g_capture = (queued != 0);
}

// Ask the microphone to finish what it already accepted instead of throwing the
// in-flight samples away, then stop the port.
static void drainCapture(uint32_t timeoutMs)
{
    const uint32_t start = millis();
    while (M5Cardputer.Mic.isRecording() != 0 && (millis() - start) < timeoutMs) {
        delay(1);
    }
}

static void computeRecordDiagnostics(size_t samples, uint32_t wallMs)
{
    int32_t  peak    = 0;
    int32_t  mn      = 0;
    int32_t  mx      = 0;
    uint64_t sumSq   = 0;
    uint32_t zeros   = 0;
    bool     allSame = (samples > 0);
    int32_t  first   = (samples > 0) ? g_pcm[0] : 0;

    for (size_t i = 0; i < samples; ++i) {
        const int32_t v = g_pcm[i];
        const int32_t a = (v < 0) ? -v : v;
        if (i == 0) {
            mn = mx = v;
        } else {
            if (v < mn) { mn = v; }
            if (v > mx) { mx = v; }
            if (v != first) { allSame = false; }
        }
        if (a > peak) { peak = a; }
        if (v == 0) { ++zeros; }
        sumSq += (uint64_t)(v * v);
    }

    g_recPeak    = peak;
    g_recMin     = mn;
    g_recMax     = mx;
    g_recRms     = (samples != 0) ? sqrt((double)sumSq / (double)samples) : 0.0;
    g_recZeroPct = (samples != 0) ? (100.0 * (double)zeros / (double)samples) : 0.0;
    g_recRate    = (wallMs != 0) ? ((double)samples * 1000.0 / (double)wallMs) : 0.0;
    g_recConstant = allSame && (samples > 0);
    g_recConstantValue = first;
}

static void startCapture()
{
    g_queued        = 0;
    g_ready         = 0;
    g_samples       = 0;
    g_lastChunkPeak = 0;
    g_capture       = false;

    primeCapture();
    if (!g_capture) {
        Serial.println("[audio-test] ERROR capture did not start");
        g_recPhase = RecPhase::Ready;
        return;
    }

    g_recPhaseMs  = millis();
    g_lastScreenMs = 0;
    g_recPhase    = RecPhase::Capturing;
    drawRecordScreen();
    Serial.printf("[audio-test] recording started (limit %us)\n", (unsigned)kRecMaxSecs);
}

static void stopCapture(const char *reason)
{
    g_capture = false;
    drainCapture(200);          // let accepted requests finish
    M5Cardputer.Mic.end();

    g_recWallMs = millis() - g_recPhaseMs;
    g_samples   = g_ready;
    if (g_samples > g_target) {
        g_samples = g_target;
    }

    computeRecordDiagnostics(g_samples, g_recWallMs);

    Serial.printf("[audio-test] recording stopped reason=%s\n", reason);
    Serial.printf("[audio-test] samples=%u\n", (unsigned)g_samples);
    Serial.printf("[audio-test] duration_ms=%u\n",
                  (unsigned)((uint64_t)g_samples * 1000u / kSampleRate));
    Serial.printf("[audio-test] wall_ms=%u\n", (unsigned)g_recWallMs);
    Serial.printf("[audio-test] rate_measured=%.0f (requested %u)\n",
                  g_recRate, (unsigned)kSampleRate);
    Serial.printf("[audio-test] peak=%d rms=%.1f zero_pct=%.2f\n",
                  (int)g_recPeak, g_recRms, g_recZeroPct);
    Serial.printf("[audio-test] min=%d max=%d\n", (int)g_recMin, (int)g_recMax);
    if (g_recConstant) {
        Serial.printf("[audio-test] constant_sample=%d\n", (int)g_recConstantValue);
    }

    g_recPhase = RecPhase::Captured;
    drawRecordScreen();
    drawRecordStatus();
}

static void startPlayback()
{
    if (g_samples == 0) {
        Serial.println("[audio-test] ERROR nothing to play");
        return;
    }

    M5Cardputer.Mic.end();
    const bool begun = M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(kVolume);

    const bool queued = M5Cardputer.Speaker.playRaw(g_pcm, g_samples, kSampleRate, false, 1);
    Serial.printf("[audio-test] speaker begin=%d playback queued=%d samples=%u\n",
                  begun ? 1 : 0, queued ? 1 : 0, (unsigned)g_samples);

    g_recPhase    = RecPhase::Playback;
    g_recPhaseMs  = millis();
    g_lastScreenMs = 0;
    drawRecordScreen();
}

static void updatePlayback()
{
    const uint32_t elapsed  = millis() - g_recPhaseMs;
    const uint32_t expected = (uint32_t)((uint64_t)g_samples * 1000u / kSampleRate);

    if ((!M5Cardputer.Speaker.isPlaying() && elapsed >= expected) ||
        (elapsed > expected + 750u)) {
        M5Cardputer.Speaker.end();
        Serial.println("[audio-test] playback finished");
        g_recPhase = RecPhase::Captured;
        drawRecordScreen();
    }
}

static void enterRecordPlayTest()
{
    Serial.println("[audio-test] --- TEST 3 : RECORD/PLAY ---");

    M5Cardputer.Speaker.end();
    logMicConfig();

    const bool begun = M5Cardputer.Mic.begin();
    g_micBeginOk = begun && M5Cardputer.Mic.isEnabled();
    Serial.printf("[audio-test] mic begin=%d enabled=%d\n",
                  begun ? 1 : 0, M5Cardputer.Mic.isEnabled() ? 1 : 0);

    g_recPhase   = RecPhase::Warmup;  // warm-up is never part of the recording
    g_recPhaseMs = millis();
    g_screen     = Screen::RecordPlay;
    drawRecordScreen();
}

static void updateRecordPlay()
{
    const uint32_t elapsed = millis() - g_recPhaseMs;

    switch (g_recPhase) {
        case RecPhase::Warmup:
            if (elapsed >= kMicWarmupMs) {
                g_recPhase = RecPhase::Ready;
                Serial.println("[audio-test] codec warm-up done, READY TO RECORD");
                drawRecordScreen();
            } else if (elapsed - g_lastScreenMs >= kScreenRedrawMs) {
                g_lastScreenMs = elapsed;
                drawRecordStatus();
            }
            break;

        case RecPhase::Capturing:
            if (g_ready >= g_target) {
                stopCapture("validation_limit");
            } else if (elapsed - g_lastScreenMs >= kScreenRedrawMs) {
                g_lastScreenMs = elapsed;
                drawRecordStatus();
            }
            break;

        case RecPhase::Playback:
            updatePlayback();
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Screen transitions
// ---------------------------------------------------------------------------
static void goToMenu()
{
    M5Cardputer.Mic.end();
    M5Cardputer.Speaker.end();
    g_capture      = false;
    g_spkPhase     = SpkPhase::Idle;
    g_recPhase     = RecPhase::Idle;
    g_blockPending = false;
    g_screen       = Screen::Menu;
    drawMenuScreen();
}

// ---------------------------------------------------------------------------
// Serial diagnostics
// ---------------------------------------------------------------------------
static bool g_serialAnnounced = false;

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
    Serial.println("[frank] Cardputer ADV v0.2.1 AUDIO DIAGNOSTIC");
    logVersions();
    Serial.println("[frank] AUDIO TEST MENU");
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("[frank] Cardputer ADV v0.2.1 AUDIO DIAGNOSTIC");

    auto cfg           = M5.config();
    cfg.fallback_board = m5::board_t::board_M5CardputerADV;
    cfg.internal_mic   = true;   // required: without it M5Unified never configures the mic pins
    cfg.internal_spk   = true;   // required: without it M5Unified never configures the DAC path

    M5Cardputer.begin(cfg, true);

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Mic.setBufferReleaseCallback(nullptr, onMicBufferReleased);

    const bool adv = (M5.getBoard() == m5::board_t::board_M5CardputerADV);
    Serial.printf("[frank] board=%s\n", adv ? "Cardputer ADV" : "Cardputer");
    Serial.printf("[frank] display initialized (%dx%d)\n",
                  (int)M5Cardputer.Display.width(), (int)M5Cardputer.Display.height());
    Serial.printf("[frank] keyboard initialized (%s)\n", adv ? "TCA8418" : "GPIO matrix");
    Serial.printf("[frank] audio sample_rate=%u format=PCM16 mono\n", (unsigned)kSampleRate);
    logVersions();

    if (allocateAudioBuffer()) {
        logBufferDecision();
    } else {
        g_bufferBytes = 0;
        logBufferDecision();
        Serial.println("[audio-test] ERROR audio buffer allocation failed (test 3 disabled)");
    }

    logMicConfig();
    logSpeakerConfig();
    logI2cDevices();

    Serial.println("[frank] AUDIO TEST MENU");
    drawMenuScreen();

    const auto &keys = M5Cardputer.Keyboard.keysState();
    g_spaceKey = { keys.space, millis() };
    g_enterKey = { keys.enter, millis() };
    g_escKey   = { keys.esc, millis() };
    g_oneKey   = { wordHas(keys, '1'), millis() };
    g_twoKey   = { wordHas(keys, '2'), millis() };
    g_threeKey = { wordHas(keys, '3'), millis() };
    g_yKey     = { wordHas(keys, 'y'), millis() };
    g_nKey     = { wordHas(keys, 'n'), millis() };

    g_serialAnnounced = (bool)Serial;
}

void loop()
{
    M5Cardputer.update();

    const auto &keys         = M5Cardputer.Keyboard.keysState();
    const bool  spacePressed = keyEdge(g_spaceKey, keys.space);
    const bool  enterPressed = keyEdge(g_enterKey, keys.enter);
    const bool  escPressed   = keyEdge(g_escKey, keys.esc);
    const bool  onePressed   = keyEdge(g_oneKey, wordHas(keys, '1'));
    const bool  twoPressed   = keyEdge(g_twoKey, wordHas(keys, '2'));
    const bool  threePressed = keyEdge(g_threeKey, wordHas(keys, '3'));
    const bool  yPressed     = keyEdge(g_yKey, wordHas(keys, 'y'));
    const bool  nPressed     = keyEdge(g_nKey, wordHas(keys, 'n'));

    switch (g_screen) {
        case Screen::Menu:
            if (onePressed) {
                enterSpeakerTest();
            } else if (twoPressed) {
                enterMicLevelTest();
            } else if (threePressed) {
                if (g_pcm == nullptr) {
                    Serial.println("[audio-test] test 3 unavailable: no audio buffer");
                } else {
                    enterRecordPlayTest();
                }
            }
            break;

        case Screen::Speaker:
            if (g_spkPhase == SpkPhase::Ask) {
                if (yPressed) {
                    g_spkHeard = 1;
                    Serial.println("[audio-test] speaker heard=YES");
                    goToMenu();
                } else if (nPressed) {
                    g_spkHeard = 0;
                    Serial.println("[audio-test] speaker heard=NO");
                    goToMenu();
                }
            } else {
                updateSpeakerTest();
            }
            if (escPressed) {
                goToMenu();
            }
            break;

        case Screen::MicLevel:
            updateMicLevelTest();
            if (escPressed) {
                M5Cardputer.Mic.end();
                Serial.printf("[audio-test] mic level test finished, best peak=%d\n",
                              (int)g_micPeakSeen);
                goToMenu();
            }
            break;

        case Screen::RecordPlay:
            updateRecordPlay();
            if (escPressed) {
                goToMenu();
            } else if (g_recPhase == RecPhase::Ready && spacePressed) {
                startCapture();
            } else if (g_recPhase == RecPhase::Capturing && spacePressed) {
                stopCapture("space");
            } else if (g_recPhase == RecPhase::Captured) {
                if (enterPressed) {
                    startPlayback();
                } else if (spacePressed) {
                    // record again, warm-up included
                    M5Cardputer.Speaker.end();
                    M5Cardputer.Mic.begin();
                    g_recPhase   = RecPhase::Warmup;
                    g_recPhaseMs = millis();
                    drawRecordScreen();
                }
            }
            break;
    }

    announceSerialIfConnected();
}
