#include <Arduino.h>
#include <esp_wifi.h>

#include "audio_capture.h"
#include "battery_monitor.h"
#include "board_power.h"
#include "button.h"
#include "config.h"
#include "device_id.h"
#include "epaper_display.h"
#include "power_manager.h"
#include "power_policy.h"
#include "recording_app.h"
#include "rtc_pcf85063.h"
#include "sd_storage.h"
#include "secrets.h"
#include "time_manager.h"
#include "touch_ft6336.h"
#include "ui_controller.h"
#include "uploader.h"
#include "wav_recording.h"
#include "wifi_manager.h"

// secrets.h may predate the fallback network. An absent fallback simply leaves
// the list with a single network; no SSID is ever hard-coded in the firmware.
// The fallback password placeholder is intentionally not a real credential.
#ifndef WIFI_FALLBACK_SSID
#define WIFI_FALLBACK_SSID ""
#endif
#ifndef WIFI_FALLBACK_PASSWORD
#define WIFI_FALLBACK_PASSWORD ""
#endif

// The battery latch. It is not a UI peripheral and it is not owned by any
// driver: it is the first thing setup() touches, before everything else.
BoardPower boardPower;

// Power policy: the debounced PWR key, the inactivity timer, the shutdown
// inhibit reasons and the graceful shutdown sequence. It never touches GPIO17
// itself - BoardPower owns the latch - and it never paints - the UI does.
voice_memo_firmware::PowerManager powerManager(
    VM_PWR_KEY_ACTIVE_LOW == 1,
    VM_PWR_DEBOUNCE_MS
);

Button button(VM_BOOT_BUTTON_PIN, VM_BUTTON_DEBOUNCE_MS, VM_BUTTON_ACTIVE_LOW == 1);
DeviceIdProvider deviceId;
AudioCapture audio;
WavRecordingBuffer buffer;
Esp32IngressUploader uploader;

// Ordered list of known networks: primary first, then the fallbacks. Every
// SSID and password comes from the local, git-ignored secrets.h, so adding or
// reordering a network is a secrets.h-only change - the firmware logic in
// WifiManager is shared by all of them.
static const WifiNetwork kKnownWifiNetworks[] = {
    {WIFI_SSID, WIFI_PASSWORD},
    {WIFI_FALLBACK_SSID, WIFI_FALLBACK_PASSWORD},
};

WifiManager wifi(kKnownWifiNetworks, sizeof(kKnownWifiNetworks) / sizeof(kKnownWifiNetworks[0]));

#if VM_ENABLE_SD
// The microSD card is an optional peripheral. SdStorage is the only object that
// touches SD_MMC; RecordingStore owns the directory layout, the atomic commit
// and the persistent queue. Both are harmless when no card is inserted.
voice_memo_firmware::SdStorage sdStorage;
voice_memo_firmware::RecordingStore recordingStore(
    sdStorage,
    VM_SD_MAX_PENDING,
    static_cast<uint64_t>(VM_SD_MIN_FREE_BYTES),
    VM_SD_KEEP_SENT == 1,
    VM_SD_VERIFY_CRC_ON_BOOT == 1
);
RecordingApp app(button, deviceId, audio, buffer, uploader, wifi, &recordingStore);
#else
RecordingApp app(button, deviceId, audio, buffer, uploader, wifi);
#endif

#if VM_ENABLE_UI
// Single-instance peripherals: the display, the touch controller, the battery
// monitor, the RTC and the TimeManager that turns RTC UTC into Europe/Lisbon
// local time. They are driven from the Arduino loop task only.
voice_memo_ui::EpdDisplay display;
voice_memo_ui::Ft6336Touch touch;
voice_memo_ui::BatteryMonitor battery;
voice_memo_ui::RtcPcf85063 rtc;
voice_memo_ui::TimeManager timeManager(rtc);
voice_memo_ui::UiController ui(app, display, touch, battery, timeManager);
#endif

namespace {

// Reports a user interaction to the power policy. The event *type* is decided
// where the interaction happened (RecordingApp for BOOT and recording edges,
// UiController for touch); this only forwards it, so the classification rule
// stays in one place: power_policy.h.
void publishUserActivity() {
    powerManager.markActivity(app.lastActivityEvent(), millis());
}

void logShutdownBlocked(voice_memo_firmware::ShutdownInhibit reason) {
    if (reason == voice_memo_firmware::ShutdownInhibit::VolatileUnsavedRecording) {
        Serial.println("[power] shutdown blocked: volatile recording not persisted");
        return;
    }
    Serial.printf("[power] shutdown blocked: %s\n",
                  voice_memo_firmware::shutdown_inhibit_name(reason));
}

// Waits, bounded, until the shared worker owns no job. Nothing new can start:
// the main loop stops pumping queued uploads during shutdown, and the volume is
// never remounted here.
bool waitForWorkerIdle(uint32_t timeoutMs) {
    const uint32_t startMs = millis();
    for (;;) {
        app.serviceShutdownWork();
        if (!app.workerBusy()) {
            return true;
        }
        if (static_cast<uint32_t>(millis() - startMs) >= timeoutMs) {
            return false;
        }
        delay(VM_PWR_SHUTDOWN_POLL_MS);
    }
}

// The graceful shutdown sequence. Called at most once, from loop(), and it never
// returns on success.
//
// Order matters and is the whole point of this function:
//   1. refuse outright if any recording exists only in PSRAM (goal E);
//   2. let an in-flight commit finish - a commit is never abandoned halfway;
//   3. interrupt an upload from the card (its audio is already durable, so this
//      is recoverable) and stop the radio so nothing can wait on the network;
//   4. release the card so no handle or dirty FAT metadata survives the cut;
//   5. play Frank's shutdown animation, whose last frame (FRANK_DEAD) is a full
//      refresh that is waited for, so the panel is left holding it;
//   6. put audio and Wi-Fi in a safe state;
//   7. release the battery latch (and deep sleep when USB keeps the board up).
void enterGracefulShutdown() {
    const voice_memo_firmware::ShutdownReason reason = powerManager.takeShutdownReason();
    Serial.printf("[power] preparing shutdown reason=%s\n",
                  voice_memo_firmware::shutdown_reason_name(reason));

    // (1) Charged twice, on purpose. The request itself already came from the
    // power manager, which refuses to emit one while data is at risk; this is the
    // last line of defence between the sequence and a destroyed note, and it is
    // cheap.
    const voice_memo_firmware::ShutdownInhibit inhibit = app.shutdownInhibit();
    if (inhibit != voice_memo_firmware::ShutdownInhibit::None) {
        logShutdownBlocked(inhibit);
        return;
    }

    // (2) A commit may still be running even though the inhibit just cleared
    // (the state machine flips only when the outcome is drained). Wait for it,
    // bounded, so the shutdown is never stuck behind a wedged card.
    if (!waitForWorkerIdle(VM_PWR_UPLOAD_ABORT_TIMEOUT_MS * 4)) {
        Serial.println("[power] worker still busy after the drain window; re-checking safety");
        const voice_memo_firmware::ShutdownInhibit recheck = app.shutdownInhibit();
        if (recheck != voice_memo_firmware::ShutdownInhibit::None) {
            logShutdownBlocked(recheck);
            return;
        }
    }

    // (3) Only now: an upload from the card may be interrupted. The item stays on
    // the card (pending/ or uploading/, which boot recovery returns to pending/),
    // nothing is deleted, and no HTTP 2xx is ever invented.
    app.abortUploadForShutdown();
    app.serviceShutdownWork();

    // The radio goes down before any further waiting, so a worker blocked on a
    // socket cannot turn into a 15 s stall. No blocking reconnect ever runs
    // during shutdown because wifi.ensureConnected() is not called again.
#if VM_ENABLE_UPLOAD
    esp_wifi_disconnect();
    esp_wifi_stop();
    Serial.println("[power] wifi stopped");
#endif

    (void)waitForWorkerIdle(VM_PWR_UPLOAD_ABORT_TIMEOUT_MS);

    // (4) The volume must be quiet before it is released: `closeVolume()` calls
    // SD_MMC.end(), and unmounting the card under a worker that still holds an
    // open file (or is mid-rename of a sidecar) is the one thing this sequence
    // must not do. A commit that got slower than the drain window is waited out
    // here rather than interrupted - the alternative is exactly the torn commit
    // step 2 exists to prevent. `releaseStorageForShutdown()` refuses while the
    // worker owns a job, so this is a bounded poll of a condition, not a sleep.
    const uint32_t safePending = app.pendingCount();
    const uint32_t releaseStartMs = millis();
    bool released = app.releaseStorageForShutdown();
    while (!released && static_cast<uint32_t>(millis() - releaseStartMs) < VM_PWR_SHUTDOWN_CARD_RELEASE_TIMEOUT_MS) {
        app.serviceShutdownWork();
        delay(VM_PWR_SHUTDOWN_POLL_MS);
        released = app.releaseStorageForShutdown();
    }
    if (!released) {
        // Bounded on purpose. Every committed recording is already durable (each
        // write is flush()ed and closed, and a commit ends in a rename), and the
        // store's `tmp -> pending` protocol is recoverable, so continuing is
        // survivable even in this worst case. Hanging the shutdown on a wedged
        // card would not be.
        Serial.println("[power] WARNING: card still held by the worker after the release window; continuing shutdown");
    }

    // (5) The last image: Frank dying, ending on FRANK_DEAD with a full refresh
    // that is waited for. Bounded, and a missing or faulted panel never blocks a
    // shutdown (the POWERED OFF screen is the fallback when even that fails).
#if VM_ENABLE_UI
    ui.showShutdownScreen(safePending);
#endif
    Serial.printf("[power] SD queue safe pending=%lu\n", static_cast<unsigned long>(safePending));

    // (6) Audio in a safe state (the codec stops driving I2S and the amplifier
    // stays disabled; playback was never enabled in this firmware).
    audio.stop();

    // (7) Release the latch. Never returns: on battery the rail is gone, on USB
    // the chip deep sleeps with PWR armed as the wake source.
    boardPower.powerOff();
}

}  // namespace

void setup() {
    // Battery boot critical path:
    // assert VBAT latch before any non-essential initialization.
    //
    // This is the FIRST instruction of setup(), exactly as in v0.4.1, and nothing
    // may be inserted before it. On battery the PWR key is the only thing holding
    // the rail up, so the firmware has to take over the latch within the key-hold
    // window. Serial.begin(), delay(), logging, the display, touch, I2C, Wi-Fi,
    // SD, the RTC and the audio codec all wait until the rail is secured.
    //
    // No separate "release the hold first" step is needed: a stale GPIO17 pad
    // hold from a previous deep sleep would override the output, so
    // keepBatteryPowerOn() drops it internally, in the correct order (level
    // written first, then the hold released, then the pin driven as an output).
    // See board_power.h for why that order is the safe one.
    boardPower.keepBatteryPowerOn();

    // Immediately after the latch, still before Serial: hand GPIO18 back from the
    // RTC wake pad to the digital GPIO matrix and learn whether this reset was a
    // deep-sleep wake. This touches a pin the latch does not depend on, but it is
    // kept here - second, not later - so the pool of "things that happened before
    // the rail was safe" stays exactly one call long.
    const bool wakeFromSleep = boardPower.releaseSleepPadsIfNeeded();

    // The rail is secure from here on: everything below may log, block and touch
    // buses.
    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.printf("[boot] VoiceMemo ESP32 firmware %s\n", VM_FIRMWARE_VERSION);
    Serial.println("[boot] board: ESP32-S3 Dev Module / Waveshare ESP32-S3-Touch-ePaper-1.54");
    if (wakeFromSleep) {
        Serial.printf("[boot] woke from deep sleep; PWR pressed=%s\n",
                      boardPower.wokeFromPowerButton() ? "yes" : "no");
    }
    Serial.printf("[boot] PSRAM detected=%s total=%u free=%u\n",
                  ESP.getPsramSize() > 0 ? "yes" : "no",
                  static_cast<unsigned int>(ESP.getPsramSize()),
                  static_cast<unsigned int>(ESP.getFreePsram()));

    // One-shot pin map. The latch itself was already asserted above; this only
    // re-drives the same HIGH and prints the [power] lines when Serial is up.
    boardPower.begin();

#if VM_ENABLE_SD
    // Mount the card before anything else touches storage. A missing card is
    // logged and never blocks the boot; RecordingApp then falls back to the
    // single-PSRAM-buffer path. RecordingStore::begin() (inside app.begin())
    // re-mounts idempotently and rebuilds the persistent queue.
    sdStorage.mount();
#if VM_SD_SELF_TEST
    sdStorage.selfTest();
#endif
#else
    Serial.println("[boot] microSD support disabled (VM_ENABLE_SD=0)");
#endif

    button.begin();
    deviceId.begin();
    app.begin();

    if (!audio.begin()) {
        Serial.println("[boot] I2S/ES8311 initialization failed");
    } else {
        Serial.println("[boot] I2S/ES8311 initialization success");
    }

#if VM_ENABLE_UI
    // The display powers up the panel and performs the initial full refresh.
    // Audio capture is already initialised, but nothing is recording yet, so the
    // blocking boot refresh cannot cost samples.
    ui.begin();

    // Frank wakes up (CLOSED -> AWAKE, two partial frames) on the panel that
    // begin() has just brought up, and the home screen is painted by the first
    // ui.update() from loop() - the animation is a transition, never a screen of
    // its own. It is deliberately this short: it sits between the power button and
    // the home screen.
    //
    // It runs on every boot, a deep-sleep wake included: that wake is itself a
    // PWR press, i.e. the user turning the device on from "off", so the same
    // wake-up is what they expect to see. If a future wake source (a timer,
    // a sensor) must NOT animate, this single call site is where
    // `wakeFromSleep`/esp_sleep_get_wakeup_cause() is consulted - the UI module
    // itself stays unaware of why the device booted.
    ui.playBootAnimation();
#else
    Serial.println("[boot] UI disabled (VM_ENABLE_UI=0)");
#endif

#if VM_ENABLE_UPLOAD
    Serial.println("[boot] TEST_B mode: upload enabled");
    // Count only: never prints an SSID or a password.
    Serial.printf("[boot] known wifi networks=%u\n",
                  static_cast<unsigned int>(wifi.usableNetworkCount()));
    // Station mode, the core's auto-reconnect disabled and the disconnect
    // listener installed. No WiFi.begin() is issued here; ensureConnected()
    // does that from loop(), exactly once per attempt.
    wifi.begin();
#else
    Serial.println("[boot] TEST_A mode: microphone-only; upload disabled");
#endif

    // Last: the power policy starts counting inactivity from here, and the PWR
    // key starts out DISARMED_AFTER_BOOT, so the press that powered the board on
    // can never be mistaken for a request to power it off. Nothing above this line
    // counts as user activity.
    powerManager.begin(millis());
}

void loop() {
    // Compared against app.activityCount() below; the counter, not a timestamp,
    // so the UI and the loop cannot swallow each other's event.
    static uint32_t lastActivityCount = 0;

#if VM_ENABLE_UPLOAD
    wifi.ensureConnected();
#endif
    app.tick();
#if VM_ENABLE_UI
    // Runs on the loop task, after the state machine has been serviced, so the
    // BOOT button always gets its edge before the panel is touched.
    ui.update();
#endif

    // A touch or a BOOT gesture is a real user interaction and restarts the
    // inactivity interval. Background work (Wi-Fi, NTP, RTC, uploads, retries,
    // remounts, e-paper refreshes, log lines) never reaches this path.
    const uint32_t activityCount = app.activityCount();
    if (activityCount != lastActivityCount) {
        lastActivityCount = activityCount;
        publishUserActivity();
    }

    // The context is assembled from cheap queries only: a UiSnapshot is a full
    // copy of the app projection, and building one per loop iteration just for
    // the notice wording would be wasteful.
    voice_memo_firmware::PowerManager::Context context;
    context.inhibit = app.shutdownInhibit();
    context.data_at_risk = app.hasVolatileRecording();
#if VM_ENABLE_UPLOAD
    context.wifi_connected = wifi.isConnected();
#endif

    if (powerManager.tick(millis(), digitalRead(VM_PWR_KEY_PIN), context)) {
        enterGracefulShutdown();
    }

    // A request that could not be honoured is reported to the UI once, from the
    // event, so the user is told why instead of pressing PWR again and again.
    const voice_memo_firmware::PowerNotice notice = powerManager.takeNotice();
    if (notice != voice_memo_firmware::PowerNotice::None) {
        app.notifyPowerNotice(notice, millis());
    }

    delay(1);
}
