#include "ui_screens.h"

#include <cstdio>

#include "config.h"

namespace voice_memo_ui {
namespace {

constexpr int kCenterX = kScreenWidth / 2;

// "MM:SS" plus the terminator.
constexpr size_t kTimerTextLength = 6;

void formatTimer(uint32_t elapsedMs, char* out, size_t outSize) {
    const uint32_t totalSeconds = elapsedMs / 1000U;
    const uint32_t minutes = (totalSeconds / 60U) % 100U;
    const uint32_t seconds = totalSeconds % 60U;
    snprintf(out, outSize, "%02u:%02u",
             static_cast<unsigned int>(minutes),
             static_cast<unsigned int>(seconds));
}

void drawTopBar(GfxCanvas& canvas, const UiView& view) {
    canvas.drawText(kMargin, kTopBarTextY, view.time_text, GfxColor::Black, kScaleSmall);

    char batteryText[8];
    if (view.battery_known) {
        snprintf(batteryText, sizeof(batteryText), "%u%%",
                 static_cast<unsigned int>(view.battery_percent));
    } else {
        snprintf(batteryText, sizeof(batteryText), "--%%");
    }
    canvas.drawTextRightAligned(kTopBarRight, kTopBarTextY, batteryText, GfxColor::Black, kScaleSmall);

    const int batteryWidth = GfxCanvas::textWidth(batteryText, kScaleSmall);
    const int iconRight = kTopBarRight - batteryWidth - kWifiIconGapToBattery;
    if (view.wifi_connected) {
        canvas.drawWifiIcon(iconRight - kWifiIconWidth, kTopBarTextY - 1, GfxColor::Black);
    } else {
        // The spec allows either a struck-through icon or the text "OFF"; at
        // 12x9 px the text is far more legible on this panel.
        canvas.drawTextRightAligned(iconRight, kTopBarTextY, "OFF", GfxColor::Black, kScaleSmall);
    }

    canvas.fillRect(kMargin, kTopBarRuleY, kTopBarRight - kMargin, 1, GfxColor::Black);
}

void drawFooter(GfxCanvas& canvas, const char* text) {
    canvas.drawTextCentered(kCenterX, kFooterTextY, text, GfxColor::Black, kScaleSmall);
}

void drawTagGrid(GfxCanvas& canvas, voice_memo_firmware::VoiceTag selected) {
    for (uint8_t index = 0; index < voice_memo_firmware::kVoiceTagCount; ++index) {
        const UiRect rect = ui_tag_rect(index);
        const voice_memo_firmware::VoiceTag tag = voice_memo_firmware::voiceTagFromIndex(index);
        const char* label = voice_memo_firmware::voiceTagLabel(tag);
        const int textY = rect.y + (rect.h - GfxCanvas::textHeight(kScaleMedium)) / 2;

        if (tag == selected) {
            // Exactly one tag is ever drawn inverted: black box, white label.
            canvas.fillRect(rect.x, rect.y, rect.w, rect.h, GfxColor::Black);
            canvas.drawTextCentered(rect.x + rect.w / 2, textY, label, GfxColor::White, kScaleMedium);
        } else {
            canvas.drawRect(rect.x, rect.y, rect.w, rect.h, GfxColor::Black);
            canvas.drawTextCentered(rect.x + rect.w / 2, textY, label, GfxColor::Black, kScaleMedium);
        }
    }
}

void drawReady(GfxCanvas& canvas, const UiView& view) {
    const bool offline = view.screen == UiScreen::ReadyOffline;
    canvas.drawTextCentered(kCenterX, 40, offline ? "OFFLINE" : "READY", GfxColor::Black, kScaleLarge);
    drawTagGrid(canvas, view.tag);
    drawFooter(canvas, "Hold BOOT to record");
}

void drawRecording(GfxCanvas& canvas, const UiView& view) {
    // "● REC" group, centred: 10 px dot + 6 px gap + label.
    const int dotDiameter = 10;
    const int groupWidth = dotDiameter + 6 + GfxCanvas::textWidth("REC", kScaleMedium);
    int x = kCenterX - groupWidth / 2;
    canvas.fillCircle(x + dotDiameter / 2, 36 + dotDiameter / 2, dotDiameter / 2, GfxColor::Black);
    x += dotDiameter + 6;
    canvas.drawText(x, 34, "REC", GfxColor::Black, kScaleMedium);

    char timer[kTimerTextLength];
    formatTimer(view.elapsed_ms, timer, sizeof(timer));
    canvas.drawTextCentered(kCenterX, 60, timer, GfxColor::Black, kScaleLarge);

    // Frozen tag of the recording in progress.
    canvas.drawTextCentered(kCenterX, 98, voice_memo_firmware::voiceTagLabel(view.tag),
                            GfxColor::Black, kScaleMedium);
    drawFooter(canvas, "Release to finish");
}

void drawMaxReached(GfxCanvas& canvas) {
    char limit[16];
    snprintf(limit, sizeof(limit), "MAX %ds", VM_MAX_RECORDING_SECONDS);
    canvas.drawTextCentered(kCenterX, 50, limit, GfxColor::Black, kScaleLarge);
    canvas.drawTextCentered(kCenterX, 95, "Release BOOT", GfxColor::Black, kScaleMedium);
}

void drawUploading(GfxCanvas& canvas, const UiView& view) {
    canvas.drawTextCentered(kCenterX, 40, "UPLOADING", GfxColor::Black, kScaleMedium);
    // Static arrow: the uploader reports no progress, so no percentage is
    // invented, and an e-paper must not pretend to animate.
    canvas.drawUpArrow(kCenterX, 66, 16, 18, GfxColor::Black);

    const char* tag = voice_memo_firmware::voiceTagLabel(view.tag);
    char duration[12];
    snprintf(duration, sizeof(duration), "%us", static_cast<unsigned int>(view.elapsed_ms / 1000U));

    const int gap = 8;
    const int dotWidth = 3;
    const int tagWidth = GfxCanvas::textWidth(tag, kScaleMedium);
    const int durationWidth = GfxCanvas::textWidth(duration, kScaleMedium);
    const int groupWidth = tagWidth + gap + dotWidth + gap + durationWidth;
    int x = kCenterX - groupWidth / 2;

    canvas.drawText(x, 100, tag, GfxColor::Black, kScaleMedium);
    x += tagWidth + gap;
    canvas.drawMiddleDot(x + dotWidth / 2, 100 + GfxCanvas::textHeight(kScaleMedium) / 2, GfxColor::Black);
    x += dotWidth + gap;
    canvas.drawText(x, 100, duration, GfxColor::Black, kScaleMedium);

    drawFooter(canvas, "Please wait");
}

void drawRetryWait(GfxCanvas& canvas) {
    canvas.drawTextCentered(kCenterX, 36, "WAITING", GfxColor::Black, kScaleMedium);
    canvas.drawTextCentered(kCenterX, 56, "FOR WIFI", GfxColor::Black, kScaleMedium);
    canvas.drawTextCentered(kCenterX, 110, "Note preserved", GfxColor::Black, kScaleSmall);
    canvas.drawTextCentered(kCenterX, 140, "RETRYING", GfxColor::Black, kScaleMedium);
}

void drawSent(GfxCanvas& canvas) {
    const int size = 34;
    canvas.drawCheckMark(kCenterX - size / 2, 34, size, GfxColor::Black);
    canvas.drawTextCentered(kCenterX, 78, "SENT", GfxColor::Black, kScaleLarge);
    canvas.drawTextCentered(kCenterX, 120, "Voice saved", GfxColor::Black, kScaleSmall);
}

void drawBusyHint(GfxCanvas& canvas) {
    // Small corner box: a BOOT press was refused while the buffer was busy.
    const char* label = "BUSY";
    const int padding = 3;
    const int textWidth = GfxCanvas::textWidth(label, kScaleSmall);
    const int boxWidth = textWidth + padding * 2;
    const int boxHeight = GfxCanvas::textHeight(kScaleSmall) + padding * 2;
    const int boxX = kTopBarRight - boxWidth;
    const int boxY = kScreenHeight - kMargin - boxHeight;

    canvas.fillRect(boxX, boxY, boxWidth, boxHeight, GfxColor::Black);
    canvas.drawText(boxX + padding, boxY + padding, label, GfxColor::White, kScaleSmall);
}

}  // namespace

void ui_draw_screen(GfxCanvas& canvas, const UiView& view) {
    // A complete screen is always painted, so a partial or deferred flush can
    // never leave pixels from a previous screen behind.
    canvas.clear(GfxColor::White);
    drawTopBar(canvas, view);

    switch (view.screen) {
        case UiScreen::Ready:
        case UiScreen::ReadyOffline:
            drawReady(canvas, view);
            break;
        case UiScreen::Recording:
            drawRecording(canvas, view);
            break;
        case UiScreen::MaxReached:
            drawMaxReached(canvas);
            break;
        case UiScreen::Uploading:
            drawUploading(canvas, view);
            break;
        case UiScreen::RetryWait:
            drawRetryWait(canvas);
            break;
        case UiScreen::Sent:
            drawSent(canvas);
            break;
    }

    if (view.busy_hint) {
        drawBusyHint(canvas);
    }
}

}  // namespace voice_memo_ui
