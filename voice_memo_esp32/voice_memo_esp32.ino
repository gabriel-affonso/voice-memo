#include <Arduino.h>

#include "audio_capture.h"
#include "battery_monitor.h"
#include "board_power.h"
#include "button.h"
#include "config.h"
#include "device_id.h"
#include "epaper_display.h"
#include "recording_app.h"
#include "rtc_pcf85063.h"
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
RecordingApp app(button, deviceId, audio, buffer, uploader, wifi);

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

void setup() {
    // STEP 1, before anything else. On battery the board only stays alive while
    // the PWR key is held or GPIO17 is HIGH, so a slow start lets the rail
    // collapse the moment the key is released. This runs ahead of Serial, the
    // display, touch, I2C, Wi-Fi, audio, the RTC and the battery monitor, and it
    // touches nothing but GPIO17.
    boardPower.keepBatteryPowerOn();

    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.printf("[boot] VoiceMemo ESP32 firmware %s\n", VM_FIRMWARE_VERSION);
    Serial.println("[boot] board: ESP32-S3 Dev Module / Waveshare ESP32-S3-Touch-ePaper-1.54");
    Serial.printf("[boot] PSRAM detected=%s total=%u free=%u\n",
                  ESP.getPsramSize() > 0 ? "yes" : "no",
                  static_cast<unsigned int>(ESP.getPsramSize()),
                  static_cast<unsigned int>(ESP.getFreePsram()));

    // One-shot pin map. The latch itself was already asserted above; this only
    // re-drives the same HIGH and prints the [power] lines when Serial is up.
    boardPower.begin();

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
}

void loop() {
#if VM_ENABLE_UPLOAD
    wifi.ensureConnected();
#endif
    app.tick();
#if VM_ENABLE_UI
    // Runs on the loop task, after the state machine has been serviced, so the
    // BOOT button always gets its edge before the panel is touched.
    ui.update();
#endif
    delay(1);
}
