#include "uploader.h"

#include "config.h"
#include "ingest_target.h"
#include "upload_status.h"

namespace {
// Clears the published-transfer pointer on every exit path, including the
// early returns below, so abortActiveTransfer() can never touch a WiFiClient
// that the worker task has already destroyed.
struct ActiveClientGuard {
    WiFiClient** slot;
    ~ActiveClientGuard() { *slot = nullptr; }
};

bool writeAll(WiFiClient& client, const uint8_t* data, size_t length) {
    size_t written = 0;
    while (written < length) {
        const int chunk = client.write(data + written, length - written);
        if (chunk <= 0) {
            return false;
        }
        written += static_cast<size_t>(chunk);
    }
    return true;
}

bool writeString(WiFiClient& client, const String& value) {
    return writeAll(client, reinterpret_cast<const uint8_t*>(value.c_str()), value.length());
}
}  // namespace

voice_memo_firmware::UploadStatus Esp32IngressUploader::upload(
    const String& baseUrl,
    const String& token,
    const String& deviceId,
    const String& recordingId,
    const uint8_t* wavData,
    size_t wavBytes,
    uint32_t durationMs,
    const String& firmwareVersion
) {
    voice_memo_firmware::MemoryByteSource source(wavData, wavBytes);
    return uploadStream(
        baseUrl, token, deviceId, recordingId, source, durationMs, firmwareVersion, nullptr);
}

void Esp32IngressUploader::abortActiveTransfer() {
    WiFiClient* client = activeClient_;
    if (client == nullptr) {
        return;
    }
    // close() on the underlying descriptor makes a write that is already blocked
    // in lwIP fail immediately. The worker then takes its normal failure path:
    // client.stop(), an outcome of Failed, and the recording stays on the card.
    client->stop();
}

voice_memo_firmware::UploadStatus Esp32IngressUploader::uploadStream(
    const String& baseUrl,
    const String& token,
    const String& deviceId,
    const String& recordingId,
    voice_memo_firmware::ByteSource& source,
    uint32_t durationMs,
    const String& firmwareVersion,
    const volatile bool* abortFlag
) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[upload] wifi is not connected");
        return voice_memo_firmware::UploadStatus::Failed;
    }

    // INGEST_URL is a base URL. parse_ingest_target() normalizes every form
    // (empty path, "/" or an explicit path) to the fixed ingress endpoint
    // /api/v1/audio, so the firmware never POSTs to "/" again.
    voice_memo_firmware::IngestTarget target;
    if (!voice_memo_firmware::parse_ingest_target(baseUrl.c_str(), target)) {
        Serial.printf("[upload] invalid URL: %s\n", baseUrl.c_str());
        return voice_memo_firmware::UploadStatus::Failed;
    }

    const String host(target.host.c_str());
    const uint16_t port = target.port;
    const String path(target.path.c_str());
    const size_t wavBytes = source.size();

    Serial.printf("[upload] recording_id=%s bytes=%u duration_ms=%u\n",
                  recordingId.c_str(),
                  static_cast<unsigned int>(wavBytes),
                  static_cast<unsigned int>(durationMs));
    // Target URL is reported so TEST B failures are diagnosable from serial.
    // The ingest token is intentionally never printed.
    Serial.printf("[upload] target http://%s:%u%s\n",
                  host.c_str(), static_cast<unsigned int>(port), path.c_str());
    Serial.println("[upload] started");

    const String boundary = "----VoiceMemoESP32" + String(millis(), HEX);

    String prefix;
    prefix += "--" + boundary + "\r\n";
    prefix += "Content-Disposition: form-data; name=\"device_id\"\r\n\r\n";
    prefix += deviceId + "\r\n";
    prefix += "--" + boundary + "\r\n";
    prefix += "Content-Disposition: form-data; name=\"recording_id\"\r\n\r\n";
    prefix += recordingId + "\r\n";
    prefix += "--" + boundary + "\r\n";
    prefix += "Content-Disposition: form-data; name=\"duration_ms\"\r\n\r\n";
    prefix += String(durationMs) + "\r\n";
    prefix += "--" + boundary + "\r\n";
    prefix += "Content-Disposition: form-data; name=\"sample_rate\"\r\n\r\n";
    prefix += String(VM_SAMPLE_RATE) + "\r\n";
    prefix += "--" + boundary + "\r\n";
    prefix += "Content-Disposition: form-data; name=\"format\"\r\n\r\n";
    prefix += "wav\r\n";
    if (firmwareVersion.length() > 0) {
        prefix += "--" + boundary + "\r\n";
        prefix += "Content-Disposition: form-data; name=\"firmware_version\"\r\n\r\n";
        prefix += firmwareVersion + "\r\n";
    }
    prefix += "--" + boundary + "\r\n";
    prefix += "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n";
    prefix += "Content-Type: application/octet-stream\r\n\r\n";

    const String suffix = "\r\n--" + boundary + "--\r\n";
    const size_t contentLength = prefix.length() + wavBytes + suffix.length();

    WiFiClient client;
    client.setTimeout(VM_UPLOAD_TIMEOUT_MS);
    // Published for abortActiveTransfer(). The guard clears it on every return
    // path, including the early ones below.
    activeClient_ = &client;
    const ActiveClientGuard activeClientGuard{&activeClient_};
    if (!client.connect(host.c_str(), port)) {
        Serial.printf("[upload] connect failed: %s:%u\n",
                      host.c_str(), static_cast<unsigned int>(port));
        Serial.println("[upload] check that the ingress binds the NUC LAN address and that Wi-Fi is on the same network");
        return voice_memo_firmware::UploadStatus::Failed;
    }

    client.print("POST " + path + " HTTP/1.1\r\n");
    client.print("Host: " + host + (port == 80 ? String("") : ":" + String(port)) + "\r\n");
    client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
    client.print("Content-Length: " + String(contentLength) + "\r\n");
    if (token.length() > 0) {
        client.print("Authorization: Bearer " + token + "\r\n");
    }
    client.print("Connection: close\r\n\r\n");

    if (!writeString(client, prefix)) {
        client.stop();
        return voice_memo_firmware::UploadStatus::Failed;
    }

    // Streaming body: a fixed 4 KB chunk is copied from the source (PSRAM or SD
    // file) into the socket. The SD read holds the filesystem lock for the read
    // only; the socket write runs unlocked.
    uint8_t chunk[VM_UPLOAD_CHUNK_BYTES];
    size_t sent = 0;
    while (sent < wavBytes) {
        if (abortFlag != nullptr && *abortFlag) {
            Serial.println("[upload] aborted: the worker is needed to persist a new recording");
            client.stop();
            return voice_memo_firmware::UploadStatus::Failed;
        }
        const size_t want = (wavBytes - sent) < sizeof(chunk) ? (wavBytes - sent) : sizeof(chunk);
        const size_t got = source.read(chunk, want);
        if (got == 0) {
            Serial.println("[upload] source ended before the advertised size; aborting");
            client.stop();
            return voice_memo_firmware::UploadStatus::Failed;
        }
        if (!writeAll(client, chunk, got)) {
            client.stop();
            return voice_memo_firmware::UploadStatus::Failed;
        }
        sent += got;
    }

    if (!writeString(client, suffix)) {
        client.stop();
        return voice_memo_firmware::UploadStatus::Failed;
    }
    client.flush();

    // Bounded response read. client.setTimeout() only bounds Stream reads, not
    // a `while (client.connected())` loop: a peer that leaves the TCP
    // connection open without sending a FIN would otherwise spin here forever,
    // and because the whole persist/upload model funnels through this one
    // worker, that would wedge the queue and permanently block new recordings.
    const uint32_t responseDeadlineMs = millis() + VM_UPLOAD_TIMEOUT_MS;

    String statusLine = client.readStringUntil('\n');
    statusLine.trim();
    // "HTTP/1.1 201 Created" -> substring(9, 12) == "201"
    const int statusCode = statusLine.substring(9, 12).toInt();

    // Skip response headers so the body can be reported on one line.
    // readStringUntil returns an empty String on timeout or when a bare "\n"
    // terminates the blank separator line, which ends the loop either way.
    while ((client.connected() || client.available()) &&
           static_cast<int32_t>(millis() - responseDeadlineMs) < 0) {
        if (client.readStringUntil('\n').length() == 0) {
            break;
        }
    }

    String responseBody;
    while ((client.connected() || client.available()) &&
           static_cast<int32_t>(millis() - responseDeadlineMs) < 0) {
        if (abortFlag != nullptr && *abortFlag) {
            Serial.println("[upload] aborted while reading the response");
            client.stop();
            return voice_memo_firmware::UploadStatus::Failed;
        }
        if (client.available()) {
            responseBody += client.readStringUntil('\n');
            if (responseBody.length() >= 512) {
                break;
            }
        } else {
            delay(1);
        }
    }
    responseBody.replace("\r", " ");
    responseBody.replace("\n", " ");
    responseBody.trim();
    client.stop();

    // Only the request/response summary is printed; the token is never echoed.
    if (statusCode <= 0) {
        Serial.println("[upload] no HTTP response from ingress (connection dropped or timed out)");
        return voice_memo_firmware::UploadStatus::Failed;
    }

    Serial.printf("[upload] HTTP %d\n", statusCode);
    if (responseBody.length() > 0) {
        Serial.printf("[upload] response=%s\n", responseBody.c_str());
    }
    return voice_memo_firmware::classify_upload_status(statusCode);
}
