#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "upload_status.h"

class Esp32IngressUploader {
public:
    voice_memo_firmware::UploadStatus upload(
        const String& baseUrl,
        const String& token,
        const String& deviceId,
        const String& recordingId,
        const uint8_t* wavData,
        size_t wavBytes,
        uint32_t durationMs,
        const String& firmwareVersion
    );
};
