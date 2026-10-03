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

    // One line of storage truth between the heading and the tag grid. It is the
    // only place the user learns that notes are waiting on the card, that the
    // card is gone, or that it is full - all without a second screen.
    char storage[24];
    storage[0] = '\0';
    if (view.sd_error) {
        snprintf(storage, sizeof(storage), "SD ERROR");
    } else if (view.storage_full) {
        snprintf(storage, sizeof(storage), "SD FULL");
    } else if (!view.sd_available) {
        snprintf(storage, sizeof(storage), "NO SD");
    } else if (view.pending_count > 0) {
        snprintf(storage, sizeof(storage), view.background_upload ? "up %u pending" : "%u pending",
                 static_cast<unsigned int>(view.pending_count));
    }
    if (storage[0] != '\0') {
        canvas.drawTextCentered(kCenterX, 72, storage, GfxColor::Black, kScaleSmall);
    }

    drawTagGrid(canvas, view.tag);
    drawFooter(canvas, "Tap BOOT to record");
}

void drawSaving(GfxCanvas& canvas, const UiView& view) {
    // The recording is already finalized and only the PSRAM copy exists: this
    // screen is deliberately short lived, but it must be honest about the card
    // not being removable yet.
    canvas.drawTextCentered(kCenterX, 38, "SAVING", GfxColor::Black, kScaleLarge);
    canvas.drawTextCentered(kCenterX, 72, "to microSD", GfxColor::Black, kScaleSmall);
    canvas.drawTextCentered(kCenterX, 100, voice_memo_firmware::voiceTagLabel(view.tag),
                            GfxColor::Black, kScaleMedium);
    drawFooter(canvas, "Do not remove card");
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
    drawFooter(canvas, "Tap BOOT to finish");
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

void drawPowerOff(GfxCanvas& canvas, const UiView& view) {
    // This is the last image the panel will ever be given before the battery
    // latch is released, and the e-paper keeps it with no power at all. So it
    // carries no live status (no clock, no battery, no Wi-Fi): a stale "--:--"
    // or a dead Wi-Fi icon left frozen on the glass would be misinformation.
    // The top bar is therefore not drawn on this screen.
    canvas.drawTextCentered(kCenterX, 62, "POWERED OFF", GfxColor::Black, kScaleMedium);
    canvas.drawTextCentered(kCenterX, 96, "Press PWR", GfxColor::Black, kScaleMedium);

    if (view.power_off_pending > 0) {
        // Reassurance, not decoration: the user just watched the device switch
        // itself off with notes still queued, and this is where they learn the
        // notes are on the card and will be uploaded on the next boot.
        char pending[24];
        snprintf(pending, sizeof(pending), "%u note%s saved",
                 static_cast<unsigned int>(view.power_off_pending),
                 view.power_off_pending == 1 ? "" : "s");
        canvas.drawTextCentered(kCenterX, 132, pending, GfxColor::Black, kScaleSmall);
    }
}

void drawStopRecordingHint(GfxCanvas& canvas) {
    // Warning strip across the middle of the RECORDING screen, where the tag
    // label normally sits: the user pressed PWR mid-capture and is told, in
    // place, that the recording has to stop first. PWR never stops a recording.
    const char* label = "STOP RECORDING";
    const char* label2 = "FIRST";
    const int padding = 4;
    const int width = GfxCanvas::textWidth(label, kScaleSmall);
    const int boxWidth = width + padding * 2;
    const int boxHeight = GfxCanvas::textHeight(kScaleSmall) * 2 + padding * 3;
    const int boxX = kCenterX - boxWidth / 2;
    const int boxY = 96;

    canvas.fillRect(boxX, boxY, boxWidth, boxHeight, GfxColor::Black);
    canvas.drawTextCentered(kCenterX, boxY + padding, label, GfxColor::White, kScaleSmall);
    canvas.drawTextCentered(kCenterX, boxY + padding * 2 + GfxCanvas::textHeight(kScaleSmall),
                            label2, GfxColor::White, kScaleSmall);
}

void drawUnsentNoteHint(GfxCanvas& canvas, const UiView& view) {
    // Owns the screen when set. A volatile recording is the one case where the
    // device refuses to power off at all, so it must not be a corner badge that
    // is easy to miss: a user who presses PWR and sees nothing change will press
    // again, and again.
    canvas.clear(GfxColor::White);
    canvas.drawTextCentered(kCenterX, 46, "UNSENT", GfxColor::Black, kScaleLarge);
    canvas.drawTextCentered(kCenterX, 78, "NOTE", GfxColor::Black, kScaleLarge);
    canvas.drawTextCentered(kCenterX, 116, "Shutdown blocked", GfxColor::Black, kScaleSmall);
    canvas.drawTextCentered(kCenterX, 132, "Waiting for upload", GfxColor::Black, kScaleSmall);
    drawFooter(canvas, view.wifi_connected ? "Keep device on" : "No WiFi - note kept");
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

    // Two screens replace the whole panel instead of decorating it.
    if (view.screen == UiScreen::PowerOff) {
        drawPowerOff(canvas, view);
        return;
    }
    if (view.unsent_note_hint) {
        drawUnsentNoteHint(canvas, view);
        return;
    }

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
        case UiScreen::Saving:
            drawSaving(canvas, view);
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
        case UiScreen::PowerOff:
            // Handled above; listed so the switch stays exhaustive.
            break;
    }

    if (view.stop_recording_hint) {
        drawStopRecordingHint(canvas);
    }
    if (view.busy_hint) {
        drawBusyHint(canvas);
    }
}

}  // namespace voice_memo_ui
