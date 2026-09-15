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

    // Called from the main loop after RecordingApp::tick().
    void update();

    // Last screen successfully pushed to the panel (diagnostics).
    UiScreen renderedScreen() const { return renderedScreen_; }
    bool uiAvailable() const { return uiAvailable_; }
    uint32_t renderCount() const { return renderCount_; }
    uint32_t partialsSinceFullRefresh() const { return partialsSinceFull_; }

private:
    void pollTouch(const RecordingApp::UiSnapshot& snapshot);
    // Battery and clock are device services: they are serviced even when the
    // panel is down, and they only do work when their own timer says so.
    void pollSensors(bool wifiConnected);
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
    uint32_t renderedTimerSecond_ = 0xFFFFFFFFU;
    uint32_t nextTimerRefreshMs_ = 0;

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

    uint32_t renderCount_ = 0;
};

}  // namespace voice_memo_ui
