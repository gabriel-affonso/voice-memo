#include "recording_app.h"

#include <math.h>
#include <cstring>

#include "config.h"
#include "firmware_calc.h"
#include "recording_state.h"
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

#if VM_ENABLE_UPLOAD
    uploadResultQueue_ = xQueueCreate(1, sizeof(UploadOutcome));
    if (uploadResultQueue_ == nullptr) {
        Serial.println("[app] FATAL: could not create upload result queue");
        return;
    }

    // One long-lived worker instead of a task per upload: it blocks on a task
    // notification (zero CPU) until a finalized WAV is handed to it, so it can
    // never race itself or run two uploads against the same buffer.
    const BaseType_t created = xTaskCreate(
        &RecordingApp::uploadTaskEntry,
        "vm_upload",
        VM_UPLOAD_TASK_STACK_BYTES,
        this,
        VM_UPLOAD_TASK_PRIORITY,
        &uploadTaskHandle_
    );
    if (created != pdPASS) {
        uploadTaskHandle_ = nullptr;
        Serial.println("[app] FATAL: could not create upload task");
        return;
    }
    Serial.printf("[app] upload task ready stack=%u priority=%u\n",
                  static_cast<unsigned int>(VM_UPLOAD_TASK_STACK_BYTES),
                  static_cast<unsigned int>(VM_UPLOAD_TASK_PRIORITY));
#endif
}

void RecordingApp::tick() {
    button_.update();

    if (state_ == State::Uploading) {
        // The HTTP POST already runs on the upload task, so tick() keeps
        // running and BOOT is still sampled and debounced every iteration.
        // A press is refused because the PSRAM buffer belongs to the in-flight
        // upload; recording_id_, duration_ms_ and the WAV stay frozen.
        if (button_.takePressedEdge() == Button::Edge::Pressed) {
            logRefusedBootPress();
        }
        button_.takeReleasedEdge();
        pollUploadOutcome();
        return;
    }

    if (state_ == State::RetryWait) {
        // Same responsiveness rule while a failed upload waits for its next
        // background attempt: BOOT is read, but a new recording is refused so
        // the pending WAV is not overwritten.
        if (button_.takePressedEdge() == Button::Edge::Pressed) {
            logRefusedBootPress();
        }
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
            startAsyncUpload();
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
            finishRecordingAndUpload();
        }
        return;
    }

    if (state_ == State::Idle) {
        const Button::Edge pressed = button_.takePressedEdge();
        // Releases carry no meaning while idle. Dropping a stale release here
        // also prevents a press that was refused during an upload from stopping
        // the next recording the instant it starts.
        button_.takeReleasedEdge();
        if (pressed == Button::Edge::Pressed) {
            Serial.println("[app] BOOT press");
            startRecording();
        }
        return;
    }

    if (state_ == State::Recording) {
        if (button_.takeReleasedEdge() == Button::Edge::Released) {
            Serial.println("[app] BOOT release");
            stopRecordingAndFinalize(false);
            finishRecordingAndUpload();
            return;
        }

        const uint32_t maxSamples = voice_memo_firmware::max_samples();
        if (sampleCount_ >= maxSamples) {
            stopRecordingAndFinalize(true);
            state_ = voice_memo_firmware::state_after_max_duration();
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
            state_ = voice_memo_firmware::state_after_max_duration();
            Serial.println("[app] MAX_RECORDING_DURATION_REACHED; waiting for button release");
        }
    }
}

void RecordingApp::startRecording() {
    if (!voice_memo_firmware::allows_new_recording(state_)) {
        // Defensive: startRecording() is only reachable from the Idle branch,
        // but refuse loudly instead of touching a buffer an upload may own.
        Serial.printf("[app] cannot start recording state=%s\n",
                      voice_memo_firmware::app_state_name(state_));
        return;
    }

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
    retryAttempt_ = 0;
    nextProgressSample_ = VM_SAMPLE_RATE;
    recordingId_ = String(recordingIdGenerator_.generate(static_cast<uint32_t>(millis())).c_str());
    // Freeze the tag for the whole recording lifetime, so a touch later in the
    // session can never rewrite the metadata of this note.
    const voice_memo_firmware::VoiceTag frozenTag = tagSelection_.freeze();
    state_ = voice_memo_firmware::state_after_boot_press(state_);

    Serial.printf("[app] recording started id=%s tag=%s\n",
                  recordingId_.c_str(),
                  voice_memo_firmware::voiceTagLabel(frozenTag));
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

void RecordingApp::finishRecordingAndUpload() {
#if VM_ENABLE_UPLOAD
    const bool uploadEnabled = true;
#else
    const bool uploadEnabled = false;
#endif

    const State next = voice_memo_firmware::state_after_finalize(uploadEnabled, wifi_.isConnected());

    if (next == State::Idle) {
        // TEST A: nothing consumes the WAV, so drop it and go back to Idle.
        Serial.println("[app] TEST_A upload disabled; recording retained only for serial summary");
        clearRecording();
        state_ = State::Idle;
        return;
    }

    // next is Uploading, or RetryWait when Wi-Fi is down. startAsyncUpload()
    // re-checks Wi-Fi and falls back to RetryWait itself, so both converge on
    // the same call and the main loop never waits for the network.
    startAsyncUpload();
}

bool RecordingApp::startAsyncUpload() {
    // There is a single PSRAM WAV buffer and a single upload task, so a second
    // request while one is in flight is a programming error: refuse it instead
    // of racing two sockets or letting the buffer be rewritten.
    if (state_ == State::Uploading) {
        Serial.printf("[app] upload already in flight id=%s; duplicate request ignored\n",
                      recordingId_.c_str());
        return false;
    }

    // A fresh recording starts at attempt 1; retries re-enter here and simply
    // continue the existing counter so the serial log shows progress.
    if (retryAttempt_ == 0) {
        retryAttempt_ = 1;
    }

    if (voice_memo_firmware::state_when_retry_due(wifi_.isConnected()) != State::Uploading) {
        Serial.printf("[app] upload skipped id=%s attempt=%u reason=wifi_not_connected; keeping recording for retry\n",
                      recordingId_.c_str(),
                      static_cast<unsigned int>(retryAttempt_));
        nextWifiNoticeMs_ = millis() + VM_UPLOAD_RETRY_INTERVAL_MS;
        nextPendingNoticeMs_ = nextWifiNoticeMs_;
        nextRetryMs_ = 0;  // 0 means "as soon as Wi-Fi connects".
        state_ = State::RetryWait;
        return false;
    }

#if VM_ENABLE_UPLOAD
    if (uploadTaskHandle_ == nullptr || uploadResultQueue_ == nullptr) {
        Serial.printf("[app] upload task unavailable id=%s; keeping recording for retry\n",
                      recordingId_.c_str());
        nextRetryMs_ = millis() + VM_UPLOAD_RETRY_INTERVAL_MS;
        nextPendingNoticeMs_ = nextRetryMs_;
        state_ = State::RetryWait;
        return false;
    }

    // Freeze the recording *before* waking the task. While state_ is Uploading
    // the main loop refuses new recordings and never calls clearRecording(), so
    // recordingId_, durationMs_, buffer_.data() and buffer_.totalBytes() stay
    // valid and immutable until the outcome is drained from the queue.
    state_ = State::Uploading;
    // Defensive: no stale outcome can exist here, but acting on one later would
    // misreport this upload.
    xQueueReset(uploadResultQueue_);
    Serial.printf("[app] async upload started id=%s attempt=%u\n",
                  recordingId_.c_str(),
                  static_cast<unsigned int>(retryAttempt_));
    xTaskNotifyGive(uploadTaskHandle_);
    return true;
#else
    // TEST A: no upload task is created, so the finalized WAV is dropped.
    Serial.println("[app] TEST_A upload disabled; recording retained only for serial summary");
    clearRecording();
    state_ = State::Idle;
    return true;
#endif
}

void RecordingApp::pollUploadOutcome() {
#if VM_ENABLE_UPLOAD
    UploadOutcome outcome;
    if (xQueueReceive(uploadResultQueue_, &outcome, 0) != pdTRUE) {
        // No result yet: the task is still sending. Return immediately so the
        // loop keeps servicing the button.
        return;
    }
    handleUploadOutcome(static_cast<voice_memo_firmware::UploadStatus>(outcome.status));
#endif
}

void RecordingApp::handleUploadOutcome(voice_memo_firmware::UploadStatus status) {
    if (status == voice_memo_firmware::UploadStatus::Accepted) {
        Serial.printf("[app] async upload accepted id=%s http=201\n", recordingId_.c_str());
    } else if (status == voice_memo_firmware::UploadStatus::AlreadyKnown) {
        Serial.printf("[app] async upload already known id=%s http=200\n", recordingId_.c_str());
    }

    state_ = voice_memo_firmware::state_after_upload_result(status);

    if (state_ == State::Idle) {
        // Accepted/AlreadyKnown: the ingress has the audio. Publish the success
        // for the UI *before* the buffer is released, so the SENT feedback
        // always corresponds to a delivered note.
        ++uploadSuccessCount_;
        lastUploadSuccessMs_ = millis();
        // The ingress has the audio: the buffer is free for the next recording.
        retryAttempt_ = 0;
        clearRecording();
        return;
    }

    // Failed: keep the exact same WAV and recording_id for the next background
    // attempt, and keep the loop responsive while waiting.
    ++retryAttempt_;
    Serial.printf("[app] async upload failed id=%s next_attempt=%u; keeping recording for retry\n",
                  recordingId_.c_str(),
                  static_cast<unsigned int>(retryAttempt_));
    nextRetryMs_ = millis() + VM_UPLOAD_RETRY_INTERVAL_MS;
    nextPendingNoticeMs_ = nextRetryMs_;
}

void RecordingApp::clearRecording() {
    buffer_.reset();
    sampleCount_ = 0;
    durationMs_ = 0;
    maxReached_ = false;
}

void RecordingApp::logRefusedBootPress() {
    // This line is the field-test proof that tick() kept running during the
    // upload. State is appended for diagnosis; the token is never involved.
    ++refusedPressCount_;
    lastRefusedPressMs_ = millis();
    Serial.printf("[app] BOOT press ignored: upload busy id=%s state=%s\n",
                  recordingId_.c_str(),
                  voice_memo_firmware::app_state_name(state_));
}

RecordingApp::UiSnapshot RecordingApp::uiSnapshot() const {
    UiSnapshot snapshot;
    snapshot.state = state_;
    snapshot.selected_tag = tagSelection_.selected();
    snapshot.recording_tag = tagSelection_.recording();
    // While capturing, the elapsed time is derived from the samples already in
    // the buffer - the same quantity durationMs_ is computed from at finalize.
    snapshot.live_elapsed_ms = voice_memo_firmware::duration_ms_for_samples(sampleCount_);
    snapshot.frozen_duration_ms = durationMs_;
    snapshot.upload_success_count = uploadSuccessCount_;
    snapshot.refused_press_count = refusedPressCount_;
    snapshot.last_upload_success_ms = lastUploadSuccessMs_;
    snapshot.last_refused_press_ms = lastRefusedPressMs_;
    snapshot.wifi_connected = wifi_.isConnected();
    snapshot.upload_enabled = VM_ENABLE_UPLOAD == 1;
    snapshot.buffer_ready = buffer_.valid();
    snapshot.audio_ready = audio_.isReady();
    return snapshot;
}

uint32_t RecordingApp::recordingDurationMs() const {
    // Live while Recording, frozen afterwards: exactly the two quantities the
    // UI needs and nothing that could drift from the state machine.
    if (state_ == State::Recording) {
        return voice_memo_firmware::duration_ms_for_samples(sampleCount_);
    }
    return durationMs_;
}

bool RecordingApp::setSelectedTag(voice_memo_firmware::VoiceTag tag) {
    // Touch may only change the tag where a new recording may start, so the
    // frozen tag of an in-flight recording (Recording/Uploading/RetryWait) is
    // untouchable by construction.
    return tagSelection_.select(tag, state_);
}

#if VM_ENABLE_UPLOAD
void RecordingApp::uploadTaskEntry(void* context) {
    static_cast<RecordingApp*>(context)->uploadTaskLoop();
}

void RecordingApp::uploadTaskLoop() {
    for (;;) {
        // Block indefinitely (no polling, no CPU) until startAsyncUpload()
        // freezes a finalized WAV and notifies this task.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // This is the only caller of uploader_.upload(). It reads members that
        // the main loop froze when it entered State::Uploading, plus the
        // pointers into the single PSRAM buffer, which nobody may rewrite while
        // the upload is in flight.
        const voice_memo_firmware::UploadStatus status = uploader_.upload(
            INGEST_URL,
            INGEST_TOKEN,
            deviceId_,
            recordingId_,
            buffer_.data(),
            buffer_.totalBytes(),
            durationMs_,
            VM_FIRMWARE_VERSION
        );

        UploadOutcome outcome;
        outcome.status = static_cast<uint8_t>(status);
        // One slot, and the main loop drains it before another upload can
        // start, so this send never blocks for long. The queue is also the
        // memory barrier that hands the result safely to the main loop.
        xQueueSend(uploadResultQueue_, &outcome, portMAX_DELAY);
    }
}
#endif
