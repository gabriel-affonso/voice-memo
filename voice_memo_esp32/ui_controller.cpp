#include "ui_controller.h"

#include <cstring>

#include "config.h"
#include "frank_face.h"
#include "ui_screens.h"

namespace voice_memo_ui {

UiController::UiController(
    RecordingApp& app,
    EpdDisplay& display,
    Ft6336Touch& touch,
    BatteryMonitor& battery,
    TimeManager& time
)
    : app_(app), display_(display), touch_(touch), battery_(battery), time_(time) {}

void UiController::begin() {
    // The clock is not a UI peripheral: bring it up even when the panel is
    // missing, so the RTC is read as UTC and NTP can still discipline it.
    time_.begin();

    if (!display_.begin()) {
        Serial.println("[ui] e-paper unavailable; continuing without a display");
        uiAvailable_ = false;
        return;
    }

    touch_.begin();
    battery_.begin();

    uiAvailable_ = true;
    Serial.printf("[ui] ready: touch=%s battery=%s rtc=%s\n",
                  touch_.ready() ? "yes" : "no",
                  battery_.available() ? "yes" : "no",
                  time_.rtcAvailable() ? "yes" : "no");

    // Read-only health check of the parts the panel reports on. The UI never
    // touches them; this is diagnostics only.
    const RecordingApp::UiSnapshot snapshot = app_.uiSnapshot();
    if (!snapshot.audio_ready) {
        Serial.println("[ui] warning: audio capture is not ready");
    }
    if (!snapshot.buffer_ready) {
        Serial.println("[ui] warning: PSRAM recording buffer is unavailable");
    }

    // Exact touch targets, so the physical test can be checked against numbers
    // instead of guesses (tab-separated: tag, x, y, w, h).
    for (uint8_t index = 0; index < voice_memo_firmware::kVoiceTagCount; ++index) {
        const UiRect rect = ui_tag_rect(index);
        Serial.printf("[ui] hitbox %s x=%d y=%d w=%d h=%d\n",
                      voice_memo_firmware::voiceTagLabel(voice_memo_firmware::voiceTagFromIndex(index)),
                      rect.x, rect.y, rect.w, rect.h);
    }
}

// Frank's power-on animation. It is a transition and nothing else: the panel is
// handed back to the normal screen painter as soon as it finishes, and the next
// render (the home screen) happens on the first update() from loop().
void UiController::playBootAnimation() {
    if (!uiAvailable_ || display_.faulted()) {
        Serial.println("[ui] no usable panel; Frank's boot animation skipped");
        return;
    }

    frankInit();
    if (!frankBootAnimation(display_)) {
        // Never fatal: a panel that failed midway has set its own fault flag and
        // update() will simply stop touching it.
        Serial.println("[ui] Frank's boot animation did not complete; continuing to the home screen");
    }

    // The glass no longer shows the blank boot picture the driver left there, so
    // the first home screen must go on with the full waveform: a partial paint
    // over Frank's dark head would ghost behind the tag grid. This reuses the
    // existing "too many partials since the last full refresh" rule instead of
    // adding a second reason to block the loop.
    partialsSinceFull_ = VM_UI_FULL_REFRESH_EVERY_PARTIALS;
}

bool UiController::wantsTouchPolling(voice_memo_firmware::AppState state) {
    switch (state) {
        case voice_memo_firmware::AppState::Idle:
        case voice_memo_firmware::AppState::Uploading:
        case voice_memo_firmware::AppState::RetryWait:
            // Idle: the tap selects a tag. Uploading/RetryWait: the tap is
            // refused, but polling lets the log say so explicitly.
            return true;
        case voice_memo_firmware::AppState::Recording:
        case voice_memo_firmware::AppState::MaxReachedWaitingRelease:
        case voice_memo_firmware::AppState::Saving:
            // No I2C traffic at all while audio capture, the end of the
            // push-to-talk gesture, or the SD commit is in progress.
            return false;
    }
    return false;
}

void UiController::pollTouch(const RecordingApp::UiSnapshot& snapshot) {
    if (!touch_.ready() || !wantsTouchPolling(snapshot.state)) {
        return;
    }

    uint16_t x = 0;
    uint16_t y = 0;
    if (!touch_.pollTap(&x, &y)) {
        return;
    }

    // Exactly one line per tap; nothing is printed while the finger rests on
    // the panel, because pollTap() only fires on the press edge.
    Serial.printf("[ui] touch x=%u y=%u\n", static_cast<unsigned int>(x), static_cast<unsigned int>(y));
    // A real user interaction. Reported through RecordingApp, which owns the
    // single activity counter the power policy reads; deliberately NOT reported
    // for every poll, only for an actual press edge.
    app_.notifyTouchInteraction();

    if (!ui_tag_selection_allowed(snapshot.state)) {
        Serial.printf("[ui] tag selection ignored in state=%s\n",
                      voice_memo_firmware::app_state_name(snapshot.state));
        return;
    }

    voice_memo_firmware::VoiceTag hit = snapshot.selected_tag;
    if (!ui_tag_hit_test(x, y, &hit)) {
        Serial.println("[ui] touch outside the tag grid; selection unchanged");
        return;
    }

    if (app_.setSelectedTag(hit)) {
        Serial.printf("[ui] tag selected=%s\n", voice_memo_firmware::voiceTagLabel(hit));
    }
}

void UiController::pollSensors(bool wifiConnected) {
    battery_.poll();
    // TimeManager owns the timezone, the NTP request and the RTC->local
    // conversion; the UI only consumes the resulting "HH:MM". Neither of these
    // is a user interaction: they must never reset the inactivity timer.
    time_.update(wifiConnected);
}

voice_memo_firmware::PowerNotice UiController::servicePowerNotice(uint32_t now) {
    // Notifications are counted, not flagged: one notification produces exactly
    // one render, and a repeated condition cannot repaint on every iteration.
    const RecordingApp::PowerNoticeSignal signal = app_.powerNoticeSignal();
    if (signal.count != lastNoticeCount_) {
        lastNoticeCount_ = signal.count;
        activeNotice_ = signal.notice;
        noticeMinHoldDeadlineMs_ = now + VM_UI_POWER_NOTICE_MIN_HOLD_MS;
    }

    switch (activeNotice_) {
        case voice_memo_firmware::PowerNotice::None:
            break;
        case voice_memo_firmware::PowerNotice::StopRecordingFirst:
            // Belongs to the capture it interrupted: as soon as the user stops
            // recording the hint is stale, but it keeps a minimum hold so it was
            // actually legible.
            if (!ui_deadline_pending(now, noticeMinHoldDeadlineMs_)) {
                activeNotice_ = voice_memo_firmware::PowerNotice::None;
            }
            break;
        case voice_memo_firmware::PowerNotice::UnsentNote:
            // Owns the screen until the note is no longer volatile, i.e. until
            // the upload was confirmed or the card took it. This is the only
            // case where the device refuses to power off, so the user must be
            // able to see why without pressing anything again.
            if (!app_.hasVolatileRecording()) {
                activeNotice_ = voice_memo_firmware::PowerNotice::None;
            }
            break;
    }

    return activeNotice_;
}

bool UiController::showShutdownScreen(uint32_t pendingNotes) {
    if (!uiAvailable_ || display_.faulted()) {
        Serial.println("[power] no usable panel; skipping the shutdown screen");
        return false;
    }

    // Frank says goodbye first: AWAKE -> BLINK -> CLOSED -> DEAD. The animation
    // waits for its final FRANK_DEAD full refresh, so once it returns true the
    // panel is already holding the image that survives the power-off, and the
    // eyes are X X on the glass before the battery latch is touched.
    const uint32_t frankStartMs = millis();
    if (frankShutdownAnimation(display_)) {
        // The panel rail is left powered and the panel idle, which is what keeps
        // the image driven until power is cut; it is also the state the official
        // Waveshare examples leave the panel in when they release the latch.
        // Powering the rail down here would buy ~30 uA and would risk corrupting
        // the very image the user is meant to be left looking at.
        shutdownScreenVisible_ = true;
        renderedScreen_ = UiScreen::PowerOff;
        hasRendered_ = true;
        ++renderCount_;
        Serial.printf("[power] Frank shutdown animation on panel (%u ms)\n",
                      static_cast<unsigned int>(millis() - frankStartMs));
        return true;
    }

    // Fallback only: the animation could not be completed (a panel that stopped
    // answering, the realistic case), so the pre-Frank POWERED OFF screen is
    // painted instead. It carries the same meaning - the device is going away
    // and these notes are safe on the card - and it is still a full refresh, so
    // it is just as crisp and just as permanent.
    UiView view;
    view.screen = UiScreen::PowerOff;
    view.power_off_pending = pendingNotes;
    ui_draw_screen(display_.canvas(), view);
    frankInvalidate();

    // Two bounded waits, both on the shutdown-specific budget rather than the
    // normal UI one: first for whatever update the panel is already finishing,
    // then for the shutdown image itself. A panel that never releases BUSY must
    // not hold the power-off, so each is a deadline and the failure of either
    // is reported and survived.
    const uint32_t startMs = millis();
    if (!display_.waitIdleFor(VM_PWR_SHUTDOWN_EPD_TIMEOUT_MS)) {
        Serial.println("[power] panel busy before the shutdown screen; painting anyway");
    }
    if (!display_.refreshFull()) {
        Serial.println("[power] shutdown screen refresh failed; continuing shutdown");
        return false;
    }
    if (!display_.waitIdleFor(VM_PWR_SHUTDOWN_EPD_TIMEOUT_MS)) {
        Serial.println("[power] shutdown screen did not finish within the BUSY timeout");
        return false;
    }

    shutdownScreenVisible_ = true;
    renderedScreen_ = UiScreen::PowerOff;
    hasRendered_ = true;
    ++renderCount_;
    Serial.printf("[power] shutdown screen on panel (%u ms)\n",
                  static_cast<unsigned int>(millis() - startMs));
    return true;
}

void UiController::update() {
    const RecordingApp::UiSnapshot snapshot = app_.uiSnapshot();

    // Serviced before the panel guard: the RTC/NTP clock must keep running even
    // if the e-paper is missing or has faulted, so an offline boot still shows
    // the correct local time once the panel works.
    pollSensors(snapshot.wifi_connected);

    if (!uiAvailable_ || display_.faulted()) {
        return;
    }

    const uint32_t now = millis();

    pollTouch(snapshot);

    // A shutdown request that could not be honoured is the most important thing
    // on the screen, so it is resolved before any other feedback.
    const voice_memo_firmware::PowerNotice notice = servicePowerNotice(now);
    const bool unsentNoteVisible = notice == voice_memo_firmware::PowerNotice::UnsentNote;
    const bool stopRecordingHint = notice == voice_memo_firmware::PowerNotice::StopRecordingFirst;
    if (unsentNoteVisible) {
        // The notice replaces the whole panel and outranks every other screen,
        // so SENT/BUSY must not fight it.
        sentActive_ = false;
        busyHintActive_ = false;
    }

    // ---- transient feedback bookkeeping -----------------------------------
    if (snapshot.upload_success_count != lastSuccessCount_) {
        lastSuccessCount_ = snapshot.upload_success_count;
        sentActive_ = true;
        // Armed only once the SENT screen is actually on the panel, so the
        // visible hold time is not eaten by the refresh itself.
        sentDeadlineMs_ = 0;
        Serial.printf("[ui] showing SENT (uploads accepted=%u)\n",
                      static_cast<unsigned int>(snapshot.upload_success_count));
    }
    if (sentActive_ && snapshot.state != voice_memo_firmware::AppState::Idle) {
        // The confirmation belongs to the idle screen only: a new recording
        // started inside the feedback window must not be masked by "SENT", and
        // a later return to Idle must not resurrect a stale confirmation.
        sentActive_ = false;
    }
    if (snapshot.refused_press_count != lastRefusedPressCount_) {
        lastRefusedPressCount_ = snapshot.refused_press_count;
        busyHintActive_ = true;
        busyHintDeadlineMs_ = now + VM_UI_BUSY_HINT_MS;
    }
    if (busyHintActive_ && !ui_deadline_pending(now, busyHintDeadlineMs_)) {
        busyHintActive_ = false;
    }
    if (sentActive_ && sentDeadlineMs_ != 0 && !ui_deadline_pending(now, sentDeadlineMs_)) {
        sentActive_ = false;
    }

    // ---- what should be on screen -----------------------------------------
    UiModel model;
    model.state = snapshot.state;
    // While the buffer belongs to a recording (Recording/Uploading/RetryWait)
    // the panel shows the frozen tag, never the live selection.
    model.tag = (snapshot.state == voice_memo_firmware::AppState::Idle)
                    ? snapshot.selected_tag
                    : snapshot.recording_tag;
    model.wifi_connected = snapshot.wifi_connected;
    model.sent_visible = sentActive_;
    model.busy_hint_visible = busyHintActive_;
    model.stop_recording_hint_visible = stopRecordingHint;

    const UiScreen screen = ui_screen_for(model);

    // ---- event-driven render decision -------------------------------------
    const bool batteryKnown = battery_.available();
    const char* timeText = time_.timeText();
    // The boot picture is already on the panel (the driver leaves it blank and
    // in partial mode), so "changed" only means something after the first paint.
    const bool screenChanged = hasRendered_ && (screen != renderedScreen_);
    const bool busyHintToggled = busyHintActive_ != renderedBusyHint_;
    const bool powerNoticeToggled = stopRecordingHint != renderedStopRecordingHint_ ||
                                    unsentNoteVisible != renderedUnsentNote_;
    // Short lived overlays must not be drawn with the slow flashing waveform:
    // a multi-second refresh would outlive their own display window.
    const bool transientScreen = (screen == UiScreen::Sent) || busyHintToggled ||
                                 powerNoticeToggled;

    bool needsRender = pendingFlush_ || !hasRendered_;
    if (!needsRender && powerNoticeToggled) {
        needsRender = true;
    }
    if (!needsRender && screenChanged) {
        needsRender = true;
    }
    if (!needsRender && model.tag != renderedTag_) {
        needsRender = true;
    }
    if (!needsRender && model.wifi_connected != renderedWifi_) {
        needsRender = true;
    }
    if (!needsRender && batteryKnown != renderedBatteryKnown_) {
        needsRender = true;
    }
    if (!needsRender && batteryKnown) {
        const int delta = static_cast<int>(battery_.percent()) - static_cast<int>(renderedBatteryPercent_);
        if (delta >= VM_UI_BATTERY_RENDER_DELTA || delta <= -VM_UI_BATTERY_RENDER_DELTA) {
            needsRender = true;
        }
    }
    if (!needsRender && strcmp(timeText, renderedTime_) != 0) {
        needsRender = true;
    }
    if (!needsRender && busyHintToggled) {
        needsRender = true;
    }
    // Persistent-queue status: a newly queued note, a completed background
    // upload, a full card or a storage failure all change the READY screen.
    if (!needsRender && snapshot.sd_available != renderedSdAvailable_) {
        needsRender = true;
    }
    if (!needsRender && snapshot.sd_error != renderedSdError_) {
        needsRender = true;
    }
    if (!needsRender && snapshot.storage_full != renderedStorageFull_) {
        needsRender = true;
    }
    if (!needsRender && snapshot.pending_count != renderedPendingCount_) {
        needsRender = true;
    }
    if (!needsRender && snapshot.background_upload != renderedBackgroundUpload_) {
        needsRender = true;
    }
    if (!needsRender && screen == UiScreen::Recording) {
        // Timer: at most one update per VM_UI_RECORDING_TIMER_INTERVAL_MS.
        const uint32_t second = snapshot.live_elapsed_ms / 1000U;
        if (second != renderedTimerSecond_ && !ui_deadline_pending(now, nextTimerRefreshMs_)) {
            needsRender = true;
        }
    }
    if (!needsRender) {
        return;
    }

    // The canvas is rebuilt on every render attempt, including a retry after a
    // busy panel. Rebuilding only when there was no pending flush would leave
    // the older picture in the framebuffer while the rendered* cache below was
    // updated from the newer snapshot, so a pending-count or SD-status change
    // could be silently swallowed until some unrelated change repainted.
    {
        UiView view;
        view.screen = screen;
        view.tag = model.tag;
        view.wifi_connected = model.wifi_connected;
        view.battery_known = batteryKnown;
        view.battery_percent = battery_.percent();
        view.time_text = timeText;
        // Live while recording, frozen duration once the WAV is finalized.
        view.elapsed_ms = (screen == UiScreen::Recording) ? snapshot.live_elapsed_ms
                                                         : snapshot.frozen_duration_ms;
        view.busy_hint = busyHintActive_;
        view.stop_recording_hint = stopRecordingHint;
        view.unsent_note_hint = unsentNoteVisible;
        view.power_off_pending = snapshot.pending_count;
        // Storage status is a plain projection of the app snapshot: the UI
        // never reads the card itself.
        view.sd_available = snapshot.sd_available;
        view.sd_error = snapshot.sd_error;
        view.storage_full = snapshot.storage_full;
        view.pending_count = snapshot.pending_count;
        view.background_upload = snapshot.background_upload;
        ui_draw_screen(display_.canvas(), view);
        // The canvas, and with it the panel, no longer holds a Frank frame: the
        // face module must not believe otherwise (it uses that belief to pick
        // the partial waveform and to skip a redundant repaint).
        frankInvalidate();
    }

    // ---- flush ------------------------------------------------------------
    UiRefresh refresh = UiRefresh::Partial;
    if (pendingFlush_) {
        refresh = pendingRefresh_;
    } else {
        UiRefreshInput refreshInput;
        refreshInput.screen_changed = screenChanged;
        refreshInput.transient_screen = transientScreen;
        refreshInput.shutdown_screen = false;
        refreshInput.state = snapshot.state;
        refreshInput.partials_since_full = partialsSinceFull_;
        refresh = ui_refresh_for(refreshInput);
    }

    if (refresh == UiRefresh::Full) {
        const uint32_t startMs = millis();
        if (!display_.refreshFull()) {
            // The panel faulted (BUSY timeout or SPI error). Stop driving it and
            // keep the voice memo fully functional.
            uiAvailable_ = false;
            return;
        }
        partialsSinceFull_ = 0;
        Serial.printf("[ui] full refresh screen=%s %u ms\n",
                      ui_screen_name(screen),
                      static_cast<unsigned int>(millis() - startMs));
    } else {
        const uint32_t startMs = millis();
        if (!display_.startPartial()) {
            // Panel still busy with the previous update: keep the painted
            // picture and retry on the next loop() iteration. Never wait here,
            // because the loop may be feeding I2S.
            pendingFlush_ = true;
            pendingRefresh_ = UiRefresh::Partial;
            return;
        }
        ++partialsSinceFull_;
        if (screen == UiScreen::Recording) {
            Serial.printf("[ui] partial refresh screen=recording %u ms\n",
                          static_cast<unsigned int>(millis() - startMs));
        }
    }

    pendingFlush_ = false;
    ++renderCount_;
    hasRendered_ = true;
    renderedScreen_ = screen;
    renderedTag_ = model.tag;
    renderedWifi_ = model.wifi_connected;
    renderedBatteryKnown_ = batteryKnown;
    renderedBatteryPercent_ = battery_.percent();
    renderedBusyHint_ = busyHintActive_;
    renderedStopRecordingHint_ = stopRecordingHint;
    renderedUnsentNote_ = unsentNoteVisible;
    renderedSdAvailable_ = snapshot.sd_available;
    renderedSdError_ = snapshot.sd_error;
    renderedStorageFull_ = snapshot.storage_full;
    renderedPendingCount_ = snapshot.pending_count;
    renderedBackgroundUpload_ = snapshot.background_upload;
    strncpy(renderedTime_, timeText, sizeof(renderedTime_) - 1);
    renderedTime_[sizeof(renderedTime_) - 1] = '\0';

    if (screen == UiScreen::Recording) {
        renderedTimerSecond_ = snapshot.live_elapsed_ms / 1000U;
        nextTimerRefreshMs_ = millis() + VM_UI_RECORDING_TIMER_INTERVAL_MS;
    }

    if (sentActive_ && sentDeadlineMs_ == 0) {
        sentDeadlineMs_ = millis() + VM_UI_SENT_HOLD_MS;
        Serial.printf("[ui] SENT visible for %u ms\n", static_cast<unsigned int>(VM_UI_SENT_HOLD_MS));
    }
}

}  // namespace voice_memo_ui
