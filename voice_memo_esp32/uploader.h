#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "upload_source.h"
#include "upload_status.h"

class Esp32IngressUploader {
public:
    // Legacy entry point: uploads a WAV that already lives in RAM (the
    // single-PSRAM-buffer fallback used when no card is available). Delegates
    // to uploadStream() with a MemoryByteSource, so the framing exists once.
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

    // Streaming entry point. The body is written chunk by chunk, so a WAV that
    // lives on the card is never copied into RAM. `abortFlag` (optional) is
    // polled between chunks so a newly finalized recording can take the upload
    // worker without waiting for a slow POST to finish.
    voice_memo_firmware::UploadStatus uploadStream(
        const String& baseUrl,
        const String& token,
        const String& deviceId,
        const String& recordingId,
        voice_memo_firmware::ByteSource& source,
        uint32_t durationMs,
        const String& firmwareVersion,
        const volatile bool* abortFlag = nullptr
    );

    // Forcibly drops the socket of an upload that is in flight, from another
    // task. Needed for shutdown: `abortFlag` alone is only polled between
    // chunks, so a worker blocked inside a socket write could otherwise hold the
    // device on for as long as the HTTP timeout. Closing the socket makes that
    // write fail immediately, the worker reports Failed, and the recording stays
    // recoverable (still in pending/ or uploading/, which boot recovery returns
    // to pending/). Nothing is deleted and no HTTP 2xx is ever invented.
    //
    // Safe to call when no upload is running: the pointer is null between jobs.
    void abortActiveTransfer();

private:
    // The client of the transfer currently running, published for
    // abortActiveTransfer(). Only ever written by the worker task.
    WiFiClient* activeClient_ = nullptr;
};
