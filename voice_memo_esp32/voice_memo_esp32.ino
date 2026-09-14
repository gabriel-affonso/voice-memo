#include <Arduino.h>

#include "audio_capture.h"
#include "button.h"
#include "config.h"
#include "device_id.h"
#include "recording_app.h"
#include "secrets.h"
#include "uploader.h"
#include "wav_recording.h"
#include "wifi_manager.h"

Button button(VM_BOOT_BUTTON_PIN, VM_BUTTON_DEBOUNCE_MS, VM_BUTTON_ACTIVE_LOW == 1);
DeviceIdProvider deviceId;
AudioCapture audio;
WavRecordingBuffer buffer;
Esp32IngressUploader uploader;
WifiManager wifi(WIFI_SSID, WIFI_PASSWORD);
RecordingApp app(button, deviceId, audio, buffer, uploader, wifi);

void setup() {
    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.printf("[boot] VoiceMemo ESP32 firmware %s\n", VM_FIRMWARE_VERSION);
    Serial.println("[boot] board: ESP32-S3 Dev Module / Waveshare ESP32-S3-Touch-ePaper-1.54");
    Serial.printf("[boot] PSRAM detected=%s total=%u free=%u\n",
                  ESP.getPsramSize() > 0 ? "yes" : "no",
                  static_cast<unsigned int>(ESP.getPsramSize()),
                  static_cast<unsigned int>(ESP.getFreePsram()));

    button.begin();
    deviceId.begin();
    app.begin();

    if (!audio.begin()) {
        Serial.println("[boot] I2S/ES8311 initialization failed");
    } else {
        Serial.println("[boot] I2S/ES8311 initialization success");
    }

#if VM_ENABLE_UPLOAD
    Serial.println("[boot] TEST_B mode: upload enabled");
#else
    Serial.println("[boot] TEST_A mode: microphone-only; upload disabled");
#endif
}

void loop() {
#if VM_ENABLE_UPLOAD
    wifi.ensureConnected();
#endif
    app.tick();
    delay(1);
}
