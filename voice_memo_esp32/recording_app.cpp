#include "recording_app.h"

#include <math.h>
#include <cstring>

#include "config.h"
#include "firmware_calc.h"
#include "secrets.h"
#include "upload_status.h"

RecordingApp::RecordingApp(
    Button& button,
    DeviceIdProvider& deviceId,
    AudioCapture& audio,
    WavRecordingBuffer& buffer,
    Esp32IngressUploader& uploader,
    WifiManager& wifi
)
    : button_(button), deviceIdProvider_(deviceId), audio_(audio), buffer_(buffer),
      uploader_(uploader), wifi_(wifi) {}

void RecordingApp::begin() {
    deviceId_ = deviceIdProvider_.getOrCreate();
    Serial.printf("[app] device_id=%s\n", deviceId_.c_str());

    const size_t maxPayload = voice_memo_firmware::max_payload_bytes();
    if (!buffer_.allocate(maxPayload)) {
        Serial.println("[app] FATAL: could not allocate PSRAM recording buffer");
        return;
    }
    Serial.printf("[app] PSRAM recording buffer allocated: payload=%u total=%u free_psram=%u\n",
                  static_cast<unsigned int>(maxPayload),
                  static_cast<unsigned int>(voice_memo_firmware::max_wav_bytes()),
                  static_cast<unsigned int>(ESP.getFreePsram()));
}

void RecordingApp::tick() {
    button_.update();

    if (state_ == State::RetryWait) {
        button_.takePressedEdge();
        button_.takeReleasedEdge();

        if (!wifi_.isConnected()) {
            if ((long)(millis() - nextWifiNoticeMs_) >= 0) {
                nextWifiNoticeMs_ = millis() + VM_UPLOAD_RETRY_INTERVAL_MS;
                Serial.printf("[app] retry pending id=%s reason=wifi\n", recordingId_.c_str());
            }
            return;
        }

        if ((long)(millis() - nextRetryMs_) >= 0) {
            Serial.printf("[app] retrying previous upload id=%s\n", recordingId_.c_str());
            tryUpload();
            return;
        }

        if (nextRetryMs_ != 0 && (long)(millis() - nextPendingNoticeMs_) >= 0) {
            nextPendingNoticeMs_ = millis() + VM_UPLOAD_RETRY_INTERVAL_MS;
            const unsigned long remainingMs = nextRetryMs_ - millis();
            Serial.printf("[app] retry pending id=%s seconds_until_retry=%lu\n",
                          recordingId_.c_str(),
                          static_cast<unsigned long>((remainingMs + 999) / 1000));
        }
        return;
    }

    if (state_ == State::MaxReachedWaitingRelease) {
        button_.takePressedEdge();
        if (button_.takeReleasedEdge() == Button::Edge::Released) {
            Serial.println("[app] button released after max duration; uploading now");
            tryUpload();
        }
        return;
    }

    if (state_ == State::Idle) {
        if (button_.takePressedEdge() == Button::Edge::Pressed) {
            Serial.println("[app] BOOT press");
            startRecording();
        }
        return;
    }

    if (state_ == State::Recording) {
        if (button_.takeReleasedEdge() == Button::Edge::Released) {
            Serial.println("[app] BOOT release");
            stopRecordingAndFinalize(false);
            tryUpload();
            return;
        }

        const uint32_t maxSamples = voice_memo_firmware::max_samples();
        if (sampleCount_ >= maxSamples) {
            stopRecordingAndFinalize(true);
            state_ = State::MaxReachedWaitingRelease;
            Serial.println("[app] MAX_RECORDING_DURATION_REACHED; waiting for button release");
            return;
        }

        uint8_t chunk[1024];
        const size_t maxRead = (maxSamples - sampleCount_) * VM_BYTES_PER_SAMPLE;
        const size_t wantRead = maxRead < sizeof(chunk) ? maxRead : sizeof(chunk);
        const size_t bytesRead = audio_.read(chunk, wantRead, 20);
        if (bytesRead == 0) {
            return;
        }

        const size_t usableBytes = bytesRead - (bytesRead % VM_BYTES_PER_SAMPLE);
        if (usableBytes == 0) {
            return;
        }

        const size_t chunkSamples = usableBytes / VM_BYTES_PER_SAMPLE;
        const int16_t* samples = reinterpret_cast<const int16_t*>(chunk);
        for (size_t i = 0; i < chunkSamples; ++i) {
            const int32_t sample = static_cast<int32_t>(samples[i]);
            const int32_t magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peakSample_) {
                peakSample_ = magnitude;
            }
            sumSquares_ += static_cast<uint64_t>(sample) * static_cast<uint64_t>(sample);
        }

        uint8_t* destination = buffer_.payload() + (sampleCount_ * VM_BYTES_PER_SAMPLE);
        memcpy(destination, chunk, usableBytes);
        sampleCount_ += chunkSamples;

        if (sampleCount_ >= nextProgressSample_) {
            Serial.printf("[app] recording bytes=%u peak=%ld\n",
                          static_cast<unsigned int>(sampleCount_ * VM_BYTES_PER_SAMPLE),
                          static_cast<long>(peakSample_));
            nextProgressSample_ += VM_SAMPLE_RATE;
        }

        if (sampleCount_ >= maxSamples) {
            stopRecordingAndFinalize(true);
            state_ = State::MaxReachedWaitingRelease;
            Serial.println("[app] MAX_RECORDING_DURATION_REACHED; waiting for button release");
        }
    }
}

void RecordingApp::startRecording() {
    if (!buffer_.valid()) {
        Serial.println("[app] cannot start recording: buffer unavailable");
        return;
    }

    if (!audio_.start()) {
        Serial.println("[app] cannot start recording: audio input unavailable");
        return;
    }

    buffer_.reset();
    sampleCount_ = 0;
    durationMs_ = 0;
    maxReached_ = false;
    peakSample_ = 0;
    sumSquares_ = 0;
    nextProgressSample_ = VM_SAMPLE_RATE;
    recordingId_ = String(recordingIdGenerator_.generate(static_cast<uint32_t>(millis())).c_str());
    state_ = State::Recording;

    Serial.printf("[app] recording started id=%s\n", recordingId_.c_str());
}

void RecordingApp::stopRecordingAndFinalize(bool maxReached) {
    audio_.stop();
    const size_t payloadBytes = sampleCount_ * VM_BYTES_PER_SAMPLE;
    buffer_.finalize(payloadBytes);
    durationMs_ = voice_memo_firmware::duration_ms_for_samples(sampleCount_);
    maxReached_ = maxReached;

    const uint32_t rms = sampleCount_ == 0
        ? 0
        : static_cast<uint32_t>(sqrtf(static_cast<float>(sumSquares_) / static_cast<float>(sampleCount_)));

    Serial.printf("[app] recording stopped id=%s duration_ms=%u pcm_bytes=%u wav_bytes=%u\n",
                  recordingId_.c_str(),
                  static_cast<unsigned int>(durationMs_),
                  static_cast<unsigned int>(sampleCount_ * VM_BYTES_PER_SAMPLE),
                  static_cast<unsigned int>(buffer_.totalBytes()));
    Serial.printf("[app] audio_peak=%ld audio_rms=%u\n",
                  static_cast<long>(peakSample_), static_cast<unsigned int>(rms));
    if (peakSample_ < 100) {
        Serial.println("[app] MIC AUDIO APPEARS SILENT");
    }
    Serial.printf("[app] WAV finalized id=%s bytes=%u duration_ms=%u\n",
                  recordingId_.c_str(), static_cast<unsigned int>(buffer_.totalBytes()),
                  static_cast<unsigned int>(durationMs_));
}

void RecordingApp::tryUpload() {
#if !VM_ENABLE_UPLOAD
    Serial.println("[app] TEST_A upload disabled; recording retained only for serial summary");
    clearRecording();
    state_ = State::Idle;
    return;
#endif

    // A fresh recording always starts at attempt 1; retries re-enter here and
    // simply continue the existing counter so the serial log shows progress.
    if (retryAttempt_ == 0) {
        retryAttempt_ = 1;
    }

    if (!wifi_.isConnected()) {
        Serial.printf("[app] upload skipped id=%s attempt=%u reason=wifi_not_connected; keeping recording for retry\n",
                      recordingId_.c_str(),
                      static_cast<unsigned int>(retryAttempt_));
        nextWifiNoticeMs_ = millis() + VM_UPLOAD_RETRY_INTERVAL_MS;
        nextPendingNoticeMs_ = millis() + VM_UPLOAD_RETRY_INTERVAL_MS;
        nextRetryMs_ = 0;  // 0 means "as soon as Wi-Fi connects".
        state_ = State::RetryWait;
        return;
    }

    Serial.printf("[app] upload attempt id=%s attempt=%u\n",
                  recordingId_.c_str(),
                  static_cast<unsigned int>(retryAttempt_));

    const voice_memo_firmware::UploadStatus result = uploader_.upload(
        INGEST_URL,
        INGEST_TOKEN,
        deviceId_,
        recordingId_,
        buffer_.data(),
        buffer_.totalBytes(),
        durationMs_,
        VM_FIRMWARE_VERSION
    );

    if (result == voice_memo_firmware::UploadStatus::Accepted) {
        Serial.printf("[app] upload accepted id=%s http=201\n", recordingId_.c_str());
        retryAttempt_ = 0;
        clearRecording();
        state_ = State::Idle;
        return;
    }

    if (result == voice_memo_firmware::UploadStatus::AlreadyKnown) {
        Serial.printf("[app] upload already known id=%s http=200\n", recordingId_.c_str());
        retryAttempt_ = 0;
        clearRecording();
        state_ = State::Idle;
        return;
    }

    ++retryAttempt_;
    Serial.printf("[app] upload failed id=%s next_attempt=%u; keeping recording for retry\n",
                  recordingId_.c_str(),
                  static_cast<unsigned int>(retryAttempt_));
    nextRetryMs_ = millis() + VM_UPLOAD_RETRY_INTERVAL_MS;
    nextPendingNoticeMs_ = nextRetryMs_;
    state_ = State::RetryWait;
}

void RecordingApp::clearRecording() {
    buffer_.reset();
    sampleCount_ = 0;
    durationMs_ = 0;
    maxReached_ = false;
}
