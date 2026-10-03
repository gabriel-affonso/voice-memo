#pragma once

// Event-driven renderer for the 200x200 e-paper panel.
//
// Design rules this class enforces:
//
//  * it never owns state: every render reads a RecordingApp::UiSnapshot, so the
//    screen can never disagree with the firmware state machine;
//  * it never renders from loop() unconditionally - a render only happens when
//    something visible actually changed (state, tag, Wi-Fi, battery, clock, the
//    recording second, or one of the two transient hints);
//  * it never blocks the main loop while audio is being captured: during
//    Recording the only allowed flush is the asynchronous partial refresh, and a
//    busy panel simply defers the update to the next loop() iteration;
//  * it never talks to the panel from the upload task - update() is called from
//    the Arduino loop task only.

#include <Arduino.h>

#include "battery_monitor.h"
#include "epaper_display.h"
#include "power_policy.h"
#include "recording_app.h"
#include "time_manager.h"
#include "touch_ft6336.h"
#include "ui_model.h"

namespace voice_memo_ui {

class UiController {
public:
    UiController(
        RecordingApp& app,
        EpdDisplay& display,
        Ft6336Touch& touch,
        BatteryMonitor& battery,
        TimeManager& time
    );

    // Initialises the panel, touch, battery and clock. Safe to call once in
    // setup(); a missing panel disables the UI and never blocks the firmware.
    void begin();

    // Plays Frank's power-on animation (DEAD -> ... -> AWAKE) on the panel that
    // begin() has already brought up, and arms the flush policy so the first
    // home screen is painted with the full waveform - a partial paint over
    // Frank's dark head would ghost. Called once from setup(), right after
    // begin(); a missing or faulted panel makes it a no-op.
    void playBootAnimation();

    // Called from the main loop after RecordingApp::tick().
    void update();

    // Paints the final POWERED OFF screen and blocks until the panel has
    // finished it (bounded by VM_PWR_SHUTDOWN_EPD_TIMEOUT_MS). Called once by
    // the shutdown sequence, after nothing is at risk any more and before the
    // battery latch is released. Never called from the loop.
    //
    // The panel is bistable, so this image is what the user sees with the MCU
    // powered off; it deliberately carries no live status (clock, battery,
    // Wi-Fi) that would freeze into a lie. Returns false when the panel is
    // unavailable or faulted, in which case shutdown continues regardless - the
    // screen is the last thing that may block a power-off, never the first.
    bool showShutdownScreen(uint32_t pendingNotes);

    // True once showShutdownScreen() has put a complete image on the panel.
    bool shutdownScreenVisible() const { return shutdownScreenVisible_; }

    // Last screen successfully pushed to the panel (diagnostics).
    UiScreen renderedScreen() const { return renderedScreen_; }
    bool uiAvailable() const { return uiAvailable_; }
    uint32_t renderCount() const { return renderCount_; }
    uint32_t partialsSinceFullRefresh() const { return partialsSinceFull_; }

    // The power notice currently painted, or None. Exposed so the shutdown
    // sequence can avoid painting over a notice before the user has seen it.
    voice_memo_firmware::PowerNotice activeNotice() const { return activeNotice_; }

private:
    void pollTouch(const RecordingApp::UiSnapshot& snapshot);
    // Battery and clock are device services: they are serviced even when the
    // panel is down, and they only do work when their own timer says so.
    void pollSensors(bool wifiConnected);
    // Consumes RecordingApp's power-notice counter and decides which notice is
    // still current. Returns the notice to paint, or None.
    voice_memo_firmware::PowerNotice servicePowerNotice(uint32_t now);
    // Touch is only polled where a tap can mean something. Recording and
    // MaxReachedWaitingRelease are skipped so the I2C reads can never compete
    // with I2S capture.
    static bool wantsTouchPolling(voice_memo_firmware::AppState state);

    RecordingApp& app_;
    EpdDisplay& display_;
    Ft6336Touch& touch_;
    BatteryMonitor& battery_;
    TimeManager& time_;

    bool uiAvailable_ = false;

    // What the panel currently shows. Only updated after a successful flush, so
    // a deferred refresh is retried instead of being forgotten.
    bool hasRendered_ = false;
    UiScreen renderedScreen_ = UiScreen::Ready;
    voice_memo_firmware::VoiceTag renderedTag_ = voice_memo_firmware::VoiceTag::Work;
    bool renderedWifi_ = false;
    bool renderedBatteryKnown_ = false;
    uint8_t renderedBatteryPercent_ = 0;
    char renderedTime_[6] = {'-', '-', ':', '-', '-', '\0'};
    bool renderedBusyHint_ = false;
    bool renderedStopRecordingHint_ = false;
    bool renderedUnsentNote_ = false;
    uint32_t renderedTimerSecond_ = 0xFFFFFFFFU;
    uint32_t nextTimerRefreshMs_ = 0;

    // Persistent-queue status the panel last showed. Cached so a changed count
    // (a note committed, a note uploaded) repaints, while a steady queue does
    // not.
    bool renderedSdAvailable_ = false;
    bool renderedSdError_ = false;
    bool renderedStorageFull_ = false;
    uint32_t renderedPendingCount_ = 0;
    bool renderedBackgroundUpload_ = false;

    uint32_t partialsSinceFull_ = 0;

    // A render computed but not yet pushed because the panel was busy.
    bool pendingFlush_ = false;
    UiRefresh pendingRefresh_ = UiRefresh::Partial;

    // Transient feedback owned by the UI (never by the app state machine).
    bool sentActive_ = false;
    uint32_t sentDeadlineMs_ = 0;
    uint32_t lastSuccessCount_ = 0;
    bool busyHintActive_ = false;
    uint32_t busyHintDeadlineMs_ = 0;
    uint32_t lastRefusedPressCount_ = 0;

    // Power notice currently painted. Owned by the UI, sourced from
    // RecordingApp::powerNoticeSignal() so exactly one render happens per
    // notification. Unlike SENT and BUSY it is not a timer: a blocked shutdown
    // stays on the panel until the condition behind it actually clears.
    voice_memo_firmware::PowerNotice activeNotice_ = voice_memo_firmware::PowerNotice::None;
    uint32_t lastNoticeCount_ = 0;
    // A notice whose condition has cleared still gets this long on the panel, so
    // "STOP RECORDING FIRST" is always legible to a human.
    uint32_t noticeMinHoldDeadlineMs_ = 0;
    bool shutdownScreenVisible_ = false;

    uint32_t renderCount_ = 0;
};

}  // namespace voice_memo_ui
