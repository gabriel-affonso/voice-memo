#include "recording_app.h"

#include <math.h>
#include <time.h>
#include <cstring>

#include "config.h"
#include "firmware_calc.h"
#include "recording_state.h"
#include "secrets.h"
#include "upload_source.h"
#include "upload_status.h"

#if VM_ENABLE_SD
#include "recording_meta.h"
#include "vm_fs.h"
#endif

namespace {

#if VM_ENABLE_SD
// Routes the store's diagnostics into the same [store]/[sd]/[queue] serial
// stream as the rest of the firmware without making the (Arduino-free) store
// depend on Serial.
void storeLogSink(const char* line) {
    Serial.println(line);
}
#endif

#if VM_ENABLE_UPLOAD
// Streams a WAV that lives on the card into the multipart body. Each read()
// takes the filesystem lock for one chunk only; the socket write that follows
// runs unlocked.
class FileByteSource : public voice_memo_firmware::ByteSource {
public:
    explicit FileByteSource(voice_memo_firmware::FileHandle* handle) : handle_(handle) {}

    ~FileByteSource() override { close(); }

    size_t size() const override { return handle_ == nullptr ? 0 : handle_->size(); }

    size_t read(uint8_t* dest, size_t maxBytes) override {
        return handle_ == nullptr ? 0 : handle_->read(dest, maxBytes);
    }

    void close() {
        if (handle_ != nullptr) {
            handle_->close();
            delete handle_;
            handle_ = nullptr;
        }
    }

private:
    voice_memo_firmware::FileHandle* handle_;
};
#endif  // VM_ENABLE_UPLOAD

}  // namespace

RecordingApp::RecordingApp(
    Button& button,
    DeviceIdProvider& deviceId,
    AudioCapture& audio,
    WavRecordingBuffer& buffer,
    Esp32IngressUploader& uploader,
    WifiManager& wifi,
    voice_memo_firmware::RecordingStore* store
)
    : button_(button), deviceIdProvider_(deviceId), audio_(audio), buffer_(buffer),
      uploader_(uploader), wifi_(wifi), store_(store) {}

void RecordingApp::begin() {
    deviceId_ = deviceIdProvider_.getOrCreate();
    Serial.printf("[app] device_id=%s\n", deviceId_.c_str());

    const size_t maxPayload = voice_memo_firmware::max_payload_bytes();
    if (!buffer_.allocate(maxPayload)) {
        // This is now a realistic failure mode: the safety cap sizes the single
        // capture buffer, so a board with less PSRAM than the one the cap was
        // chosen for (8 MB) refuses the whole recording feature at boot. Say so
        // with the numbers instead of leaving a bare FATAL, so the field log
        // shows exactly which of the two knobs to change.
        Serial.printf("[app] FATAL: could not allocate PSRAM recording buffer: "
                      "wanted=%u bytes (VM_MAX_RECORDING_SECONDS=%u) psram_total=%u psram_free=%u\n",
                      static_cast<unsigned int>(voice_memo_firmware::max_wav_bytes()),
                      static_cast<unsigned int>(VM_MAX_RECORDING_SECONDS),
                      static_cast<unsigned int>(ESP.getPsramSize()),
                      static_cast<unsigned int>(ESP.getFreePsram()));
        Serial.println("[app] no recording is possible until this fits; see config.h (VM_MAX_RECORDING_SECONDS)");
        return;
    }
    Serial.printf("[app] PSRAM recording buffer allocated: payload=%u total=%u free_psram=%u\n",
                  static_cast<unsigned int>(maxPayload),
                  static_cast<unsigned int>(voice_memo_firmware::max_wav_bytes()),
                  static_cast<unsigned int>(ESP.getFreePsram()));

#if VM_ENABLE_SD
    if (store_ != nullptr) {
        // Mount (never formats), create the layout and rebuild the queue from
        // the card. A missing card is not an error: the firmware simply
        // continues with the single-PSRAM-buffer path.
        store_->setLogSink(&storeLogSink);
        if (store_->begin()) {
            sdWasAvailable_ = true;
            pendingCount_ = store_->pendingCount();
            Serial.printf("[sd] root=%s\n", VM_SD_ROOT);
            Serial.printf("[store] recovered=%u discarded_tmp=%u corrupt=%u\n",
                          static_cast<unsigned int>(store_->recoveredCount()),
                          static_cast<unsigned int>(store_->discardedTmpCount()),
                          static_cast<unsigned int>(store_->corruptCount()));
            Serial.printf("[queue] pending=%u\n",
                          static_cast<unsigned int>(pendingCount_));
            Serial.printf("[sd] total=%llu free=%llu\n",
                          static_cast<unsigned long long>(store_->totalBytes()),
                          static_cast<unsigned long long>(store_->freeBytes()));
        } else {
            // No card (or an unusable one) is a capability the firmware
            // supports, not a failure: sd_available stays false, so the UI
            // shows NO SD rather than SD ERROR.
            Serial.println("[sd] persistence disabled; falling back to the single PSRAM buffer");
        }
    }
#endif

#if VM_ENABLE_UPLOAD
    uploadResultQueue_ = xQueueCreate(1, sizeof(JobOutcome));
    if (uploadResultQueue_ == nullptr) {
        Serial.println("[app] FATAL: could not create upload result queue");
        return;
    }

    // One long-lived worker instead of a task per upload: it blocks on a task
    // notification (zero CPU) until the main loop hands it a job, so it can
    // never race itself or run two jobs against the same buffer.
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

    // Deadline-based volume maintenance (no polling storm) and the two
    // state-independent services: drain the worker outcome, then give the
    // worker the next queued upload if one is due.
    serviceStorage();
    serviceDeferredQueuedOutcome();
    pollJobOutcome();
    pumpQueuedUpload();

    switch (state_) {
        case State::Idle:
            tickIdle();
            break;
        case State::Recording:
            tickRecording();
            break;
        case State::MaxReachedWaitingRelease:
            tickMaxReached();
            break;
        case State::Saving:
            tickSaving();
            break;
        case State::Uploading:
            tickUploading();
            break;
        case State::RetryWait:
            tickRetryWait();
            break;
    }
}

void RecordingApp::tickIdle() {
    const Button::Edge pressed = button_.takePressedEdge();
    // Releases carry no meaning: recording is a toggle, so the action lives on
    // the press. Dropping a stale release here also prevents a press that was
    // refused during an upload from stopping the next recording the instant it
    // starts.
    const Button::Edge released = button_.takeReleasedEdge();
    if (pressed == Button::Edge::Pressed) {
        Serial.println("[app] BOOT press");
        // BOOT is the recording control: pressing it always counts as a real
        // interaction, even when the state machine then refuses the recording.
        noteActivity(voice_memo_firmware::ActivityEvent::BootButton);
        // The toggle rule itself lives in the pure, host-tested
        // recording_state.h: from Idle a press starts a capture.
        if (voice_memo_firmware::button_action_for_state(state_) ==
            voice_memo_firmware::ButtonAction::StartRecording) {
            startRecording();
        }
    } else if (released == Button::Edge::Released) {
        noteActivity(voice_memo_firmware::ActivityEvent::BootButton);
    }
}

void RecordingApp::tickRecording() {
    // Toggle-to-record. The press that started this capture happened *before*
    // this state was entered and only its edge was consumed, so the matching
    // release is dropped right here: that is what lets the recording continue
    // with the finger lifted, and it is also why one physical press can never be
    // read as start + stop. A second, independent press is the stop.
    const Button::Edge pressed = button_.takePressedEdge();
    button_.takeReleasedEdge();

    if (pressed == Button::Edge::Pressed) {
        Serial.println("[app] BOOT press (stop)");
        noteActivity(voice_memo_firmware::ActivityEvent::RecordingStopped);
        stopRecordingAndFinalize(StopReason::User);
        return;
    }

    const uint32_t maxSamples = voice_memo_firmware::max_samples();
    if (sampleCount_ >= maxSamples) {
        // Safety net only: this finalizes, saves and queues the note exactly
        // like a manual stop. Nothing is discarded, and no log is emitted per
        // loop iteration because the state leaves Recording immediately.
        stopRecordingAndFinalize(StopReason::Timeout);
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
        stopRecordingAndFinalize(StopReason::Timeout);
    }
}

void RecordingApp::tickMaxReached() {
    button_.takePressedEdge();
    if (button_.takeReleasedEdge() == Button::Edge::Released) {
        Serial.println("[app] button released after max duration; finalizing now");
        noteActivity(voice_memo_firmware::ActivityEvent::RecordingStopped);
        finishRecordingAndUpload();
    }
}

void RecordingApp::tickSaving() {
    // The only copy of the WAV is the frozen PSRAM buffer, so a new recording is
    // refused until the commit returns.
    if (button_.takePressedEdge() == Button::Edge::Pressed) {
        logRefusedBootPress();
    }
    button_.takeReleasedEdge();

#if VM_ENABLE_UPLOAD
    if (uploadTaskHandle_ != nullptr) {
        if (!jobInFlight_) {
            startPersistJob();
        }
        return;
    }
#endif

    // No background worker (upload disabled): commit synchronously. Nothing is
    // capturing audio and no upload is in flight, so the short blocking write
    // cannot cost samples.
    handlePersistOutcome(persistRecording());
}

void RecordingApp::tickUploading() {
    // The HTTP POST runs on the worker, so tick() keeps running and BOOT is
    // still sampled and debounced every iteration. A press is refused because
    // the PSRAM buffer belongs to the in-flight upload; recording_id_,
    // duration_ms_ and the WAV stay frozen.
    if (button_.takePressedEdge() == Button::Edge::Pressed) {
        logRefusedBootPress();
    }
    button_.takeReleasedEdge();
}

void RecordingApp::tickRetryWait() {
    // Same responsiveness rule while a failed upload waits for its next
    // background attempt: BOOT is read, but a new recording is refused so the
    // pending WAV is not overwritten.
    if (button_.takePressedEdge() == Button::Edge::Pressed) {
        logRefusedBootPress();
    }
    button_.takeReleasedEdge();

    if (!wifi_.isConnected()) {
        if ((long)(millis() - nextWifiNoticeMs_) >= 0) {
            nextWifiNoticeMs_ = millis() + VM_UPLOAD_RETRY_INTERVAL_MS;
            Serial.printf("[app] retry pending id=%s reason=wifi source=ram\n", recordingId_.c_str());
        }
        return;
    }

    if (nextRetryMs_ == 0 || (long)(millis() - nextRetryMs_) >= 0) {
        if (jobInFlight_) {
            // The worker is finishing a queued upload: defer the volatile retry
            // instead of queueing a second job on the single-slot worker.
            nextRetryMs_ = millis() + 500;
            return;
        }
        Serial.printf("[app] retrying previous upload id=%s source=ram\n", recordingId_.c_str());
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
}

void RecordingApp::startRecording() {
    if (!voice_memo_firmware::allows_new_recording(state_)) {
        // Defensive: startRecording() is only reachable from the Idle branch,
        // but refuse loudly instead of touching a buffer the worker may own.
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
    recordingId_ = String(recordingIdGenerator_
                              .generate(static_cast<uint32_t>(millis()), esp_random())
                              .c_str());
    // Freeze the tag for the whole recording lifetime, so a touch later in the
    // session can never rewrite the metadata of this note.
    const voice_memo_firmware::VoiceTag frozenTag = tagSelection_.freeze();
    state_ = voice_memo_firmware::state_after_boot_press(state_);

    Serial.printf("[record] started id=%s tag=%s max_ms=%lu\n",
                  recordingId_.c_str(),
                  voice_memo_firmware::voiceTagLabel(frozenTag),
                  static_cast<unsigned long>(VM_MAX_RECORDING_MS));
}

void RecordingApp::stopRecordingAndFinalize(StopReason reason) {
    audio_.stop();
    const size_t payloadBytes = sampleCount_ * VM_BYTES_PER_SAMPLE;
    buffer_.finalize(payloadBytes);
    durationMs_ = voice_memo_firmware::duration_ms_for_samples(sampleCount_);
    maxReached_ = (reason == StopReason::Timeout);

    const uint32_t rms = sampleCount_ == 0
        ? 0
        : static_cast<uint32_t>(sqrtf(static_cast<float>(sumSquares_) / static_cast<float>(sampleCount_)));

    if (reason == StopReason::Timeout) {
        // Printed exactly once per capture: the timeout leaves Recording in the
        // same tick, so there is no per-loop log storm. The note itself is
        // saved and queued normally - the cap never discards audio.
        Serial.printf("[record] safety timeout reached (%lu ms)\n",
                      static_cast<unsigned long>(VM_MAX_RECORDING_MS));
        Serial.printf("[record] stopped by %lu-minute timeout id=%s duration_ms=%u pcm_bytes=%u wav_bytes=%u\n",
                      static_cast<unsigned long>(VM_MAX_RECORDING_SECONDS / 60UL),
                      recordingId_.c_str(),
                      static_cast<unsigned int>(durationMs_),
                      static_cast<unsigned int>(sampleCount_ * VM_BYTES_PER_SAMPLE),
                      static_cast<unsigned int>(buffer_.totalBytes()));
    } else {
        Serial.printf("[record] stopped by user id=%s duration_ms=%u pcm_bytes=%u wav_bytes=%u\n",
                      recordingId_.c_str(),
                      static_cast<unsigned int>(durationMs_),
                      static_cast<unsigned int>(sampleCount_ * VM_BYTES_PER_SAMPLE),
                      static_cast<unsigned int>(buffer_.totalBytes()));
    }
    Serial.printf("[app] audio_peak=%ld audio_rms=%u\n",
                  static_cast<long>(peakSample_), static_cast<unsigned int>(rms));
    if (peakSample_ < 100) {
        Serial.println("[app] MIC AUDIO APPEARS SILENT");
    }
    Serial.printf("[app] WAV finalized id=%s bytes=%u duration_ms=%u\n",
                  recordingId_.c_str(), static_cast<unsigned int>(buffer_.totalBytes()),
                  static_cast<unsigned int>(durationMs_));

    // The one hand-off, identical for both stop reasons: commit to the card
    // (Saving) or take the volatile fallback (Uploading/RetryWait/Idle). This
    // always leaves State::Recording, so the caller must not touch the buffer
    // afterwards.
    finishRecordingAndUpload();
}

uint32_t RecordingApp::currentUtcSeconds() {
    const time_t now = time(nullptr);
    // 2020-09-13. Anything earlier means neither the RTC nor NTP ever set the
    // clock, and a bogus timestamp is worse than an explicit "unknown" (0).
    if (now < static_cast<time_t>(1600000000)) {
        return 0;
    }
    return static_cast<uint32_t>(now);
}

void RecordingApp::finishRecordingAndUpload() {
#if VM_ENABLE_SD
    if (store_ != nullptr && store_->available() && store_->healthy()) {
        // Persist first. Once committed, neither Wi-Fi nor a reboot can lose the
        // recording, the PSRAM buffer is free again and the upload continues in
        // the background from the card.
        persistMeta_ = voice_memo_firmware::RecordingMeta();
        persistMeta_.recording_id = std::string(recordingId_.c_str());
        persistMeta_.device_id = std::string(deviceId_.c_str());
        persistMeta_.tag_index = static_cast<uint32_t>(
            voice_memo_firmware::voiceTagIndex(tagSelection_.recording()));
        persistMeta_.duration_ms = durationMs_;
        persistMeta_.wav_bytes = static_cast<uint32_t>(buffer_.totalBytes());
        persistMeta_.sample_rate = static_cast<uint32_t>(VM_SAMPLE_RATE);
        persistMeta_.created_utc = currentUtcSeconds();
        persistMeta_.attempt_count = 0;

        state_ = State::Saving;
        Serial.printf("[store] saving id=%s bytes=%u\n",
                      recordingId_.c_str(),
                      static_cast<unsigned int>(buffer_.totalBytes()));
        startPersistJob();
        return;
    }
#endif

    fallbackToVolatile();
}

void RecordingApp::fallbackToVolatile() {
#if VM_ENABLE_UPLOAD
    const bool uploadEnabled = true;
#else
    const bool uploadEnabled = false;
#endif

    const State next = voice_memo_firmware::state_after_finalize(uploadEnabled, wifi_.isConnected());

    if (next == State::Idle) {
        // TEST A / uploads disabled: nothing consumes the WAV, so drop it.
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

void RecordingApp::startPersistJob() {
#if VM_ENABLE_UPLOAD
    if (jobInFlight_) {
        // The worker is uploading an older, already-persisted recording. That
        // audio is safe on the card, so ask it to stop at the next chunk and
        // let the brand-new recording (still only in PSRAM) be committed.
        if (jobKind_ == JobKind::UploadQueued) {
            abortUpload_ = true;
        }
        return;
    }

    if (uploadTaskHandle_ == nullptr || uploadResultQueue_ == nullptr) {
        // begin() failed to create the worker; commit on the main loop instead
        // of leaving the recording unpersisted.
        handlePersistOutcome(persistRecording());
        return;
    }

    jobInFlight_ = true;
    jobKind_ = JobKind::PersistToSd;
    xQueueReset(uploadResultQueue_);
    xTaskNotifyGive(uploadTaskHandle_);
#else
    handlePersistOutcome(persistRecording());
#endif
}

voice_memo_firmware::SaveOutcome RecordingApp::persistRecording() {
#if VM_ENABLE_SD
    if (store_ == nullptr) {
        return voice_memo_firmware::SaveOutcome::NotMounted;
    }
    return store_->savePending(persistMeta_, buffer_.data(), buffer_.totalBytes());
#else
    return voice_memo_firmware::SaveOutcome::NotMounted;
#endif
}

void RecordingApp::handlePersistOutcome(voice_memo_firmware::SaveOutcome outcome) {
#if VM_ENABLE_SD
    if (outcome == voice_memo_firmware::SaveOutcome::Ok) {
        // The card now owns the audio: free the PSRAM buffer and let the next
        // recording start immediately. The upload happens from the queue.
        refreshQueueCounts();
        storageFull_ = false;
        persistIdRetries_ = 0;
        clearRecording();
        state_ = voice_memo_firmware::state_after_persist_ok();
        Serial.printf("[queue] pending=%u\n", static_cast<unsigned int>(pendingCount_));
        return;
    }

    if (outcome == voice_memo_firmware::SaveOutcome::DuplicateId) {
        // The card already holds a recording with this id (a cross-boot id
        // collision, or a file left by an earlier boot). Nothing has been
        // uploaded yet, so minting a fresh id is safe and is the only way to
        // avoid the ingress silently deduplicating the new note as
        // already_known. Bounded, so a pathological card cannot loop.
        if (persistIdRetries_ < 3) {
            ++persistIdRetries_;
            recordingId_ = String(recordingIdGenerator_
                                      .generate(static_cast<uint32_t>(millis()), esp_random())
                                      .c_str());
            persistMeta_.recording_id = std::string(recordingId_.c_str());
            Serial.printf("[store] recording_id already on the card; retrying as id=%s\n",
                          recordingId_.c_str());
            startPersistJob();
            return;
        }
        Serial.println("[store] could not find a free recording_id; keeping the recording in RAM");
        fallbackToVolatile();
        return;
    }
    persistIdRetries_ = 0;

    switch (outcome) {
        case voice_memo_firmware::SaveOutcome::StorageFull:
            storageFull_ = true;
            Serial.println("[sd] storage full; keeping the recording in RAM for the legacy path");
            break;
        case voice_memo_firmware::SaveOutcome::IoError:
        case voice_memo_firmware::SaveOutcome::NotMounted:
            if (store_ != nullptr) {
                store_->markUnhealthy();
            }
            Serial.println("[sd] write failed; keeping the recording in RAM for the legacy path");
            break;
        case voice_memo_firmware::SaveOutcome::InvalidId:
        case voice_memo_firmware::SaveOutcome::DuplicateId:
        case voice_memo_firmware::SaveOutcome::Ok:
            Serial.printf("[store] commit refused (outcome=%u); keeping the recording in RAM\n",
                          static_cast<unsigned int>(outcome));
            break;
    }
#else
    (void)outcome;
#endif

    // The card could not take the recording. Fall back to the exact behaviour
    // this firmware had before microSD existed: keep it in PSRAM and upload (or
    // wait to retry) before another recording may start. Nothing is lost.
    fallbackToVolatile();
}

bool RecordingApp::startAsyncUpload() {
    // There is a single PSRAM WAV buffer and a single worker, so a second
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
        // begin() could not create the worker (only possible on allocation
        // failure). Doing the POST inline is far better than disabling recording
        // forever behind a RetryWait that can never start a job.
        Serial.printf("[app] upload worker unavailable id=%s; uploading on the main loop\n",
                      recordingId_.c_str());
        state_ = State::Uploading;
        voice_memo_firmware::MemoryByteSource source(buffer_.data(), buffer_.totalBytes());
        const voice_memo_firmware::UploadStatus status = uploader_.uploadStream(
            INGEST_URL,
            INGEST_TOKEN,
            deviceId_,
            recordingId_,
            source,
            durationMs_,
            VM_FIRMWARE_VERSION,
            nullptr);
        handleUploadOutcome(status);
        return true;
    }

    if (jobInFlight_) {
        // The worker is finishing a queued upload from the card. Defer this
        // volatile attempt; the buffer stays frozen in RetryWait.
        Serial.printf("[app] upload deferred id=%s: worker busy with a queued upload\n",
                      recordingId_.c_str());
        nextRetryMs_ = millis() + 500;
        nextPendingNoticeMs_ = nextRetryMs_;
        state_ = State::RetryWait;
        return false;
    }

    // Freeze the recording *before* waking the worker. While state_ is Uploading
    // the main loop refuses new recordings and never calls clearRecording(), so
    // recordingId_, durationMs_, buffer_.data() and buffer_.totalBytes() stay
    // valid and immutable until the outcome is drained from the queue.
    state_ = State::Uploading;
    jobInFlight_ = true;
    jobKind_ = JobKind::UploadVolatile;
    inFlightId_ = "";
    // Defensive: no stale outcome can exist here, but acting on one later would
    // misreport this upload.
    xQueueReset(uploadResultQueue_);
    Serial.printf("[app] async upload started id=%s attempt=%u source=ram\n",
                  recordingId_.c_str(),
                  static_cast<unsigned int>(retryAttempt_));
    xTaskNotifyGive(uploadTaskHandle_);
    return true;
#else
    // TEST A: no worker is created, so the finalized WAV is dropped.
    Serial.println("[app] TEST_A upload disabled; recording retained only for serial summary");
    clearRecording();
    state_ = State::Idle;
    return true;
#endif
}

void RecordingApp::pollJobOutcome() {
#if VM_ENABLE_UPLOAD
    if (uploadResultQueue_ == nullptr) {
        // begin() could not create the queue; there is never an outcome.
        return;
    }
    JobOutcome outcome;
    if (xQueueReceive(uploadResultQueue_, &outcome, 0) != pdTRUE) {
        // No result yet: the worker is still busy. Return immediately so the
        // loop keeps servicing the button.
        return;
    }

    // While audio is being captured, a queued upload's outcome must not touch
    // the card: markUploaded removes a ~1.4 MB file and markPendingAgain
    // renames and rewrites a sidecar. Hold the result (and keep jobInFlight_
    // true so no new job starts and no remount runs) until capture is over.
    if (static_cast<JobKind>(outcome.kind) == JobKind::UploadQueued &&
        (state_ == State::Recording || state_ == State::MaxReachedWaitingRelease)) {
        pendingQueuedOutcome_ = true;
        pendingQueuedStatus_ = outcome.status;
        return;
    }

    jobInFlight_ = false;
    abortUpload_ = false;

    switch (static_cast<JobKind>(outcome.kind)) {
        case JobKind::PersistToSd:
            handlePersistOutcome(static_cast<voice_memo_firmware::SaveOutcome>(outcome.status));
            break;
        case JobKind::UploadVolatile:
            handleUploadOutcome(static_cast<voice_memo_firmware::UploadStatus>(outcome.status));
            break;
        case JobKind::UploadQueued:
            handleQueuedUploadOutcome(static_cast<voice_memo_firmware::UploadStatus>(outcome.status));
            break;
        case JobKind::None:
            break;
    }
#endif
}

void RecordingApp::serviceDeferredQueuedOutcome() {
#if VM_ENABLE_SD && VM_ENABLE_UPLOAD
    if (!pendingQueuedOutcome_) {
        return;
    }
    // Capture is over (or was never running): the short filesystem bookkeeping
    // is safe now.
    if (state_ == State::Recording || state_ == State::MaxReachedWaitingRelease) {
        return;
    }
    pendingQueuedOutcome_ = false;
    jobInFlight_ = false;
    abortUpload_ = false;
    handleQueuedUploadOutcome(static_cast<voice_memo_firmware::UploadStatus>(pendingQueuedStatus_));
#endif
}

void RecordingApp::handleUploadOutcome(voice_memo_firmware::UploadStatus status) {
    if (status == voice_memo_firmware::UploadStatus::Accepted) {
        Serial.printf("[app] async upload accepted id=%s http=201 source=ram\n", recordingId_.c_str());
    } else if (status == voice_memo_firmware::UploadStatus::AlreadyKnown) {
        Serial.printf("[app] async upload already known id=%s http=200 source=ram\n", recordingId_.c_str());
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

void RecordingApp::handleQueuedUploadOutcome(voice_memo_firmware::UploadStatus status) {
#if VM_ENABLE_SD
    const String id = inFlightId_;
    inFlightId_ = "";
    if (store_ == nullptr || id.length() == 0) {
        return;
    }

    const std::string key(id.c_str());

    if (status == voice_memo_firmware::UploadStatus::Accepted ||
        status == voice_memo_firmware::UploadStatus::AlreadyKnown) {
        // Confirmed by the ingress: only now may the on-card copy go away.
        const bool removed = store_->markUploaded(key);
        refreshQueueCounts();
        storageFull_ = false;
        ++uploadSuccessCount_;
        lastUploadSuccessMs_ = millis();
        nextQueuedRetryMs_ = 0;
        if (status == voice_memo_firmware::UploadStatus::Accepted) {
            Serial.printf("[upload] accepted id=%s http=201 source=sd\n", id.c_str());
        } else {
            Serial.printf("[upload] accepted id=%s http=200 already_known source=sd\n", id.c_str());
        }
        if (removed) {
            Serial.printf("[store] removed id=%s\n", id.c_str());
        } else {
            Serial.printf("[store] cleanup deferred id=%s (card unavailable; will be retried)\n",
                          id.c_str());
        }
        Serial.printf("[queue] pending=%u\n", static_cast<unsigned int>(pendingCount_));
        return;
    }

    // Failed: the recording stays on the card, with the same recording_id, and
    // is retried later. Nothing is deleted before a valid HTTP confirmation.
    const voice_memo_firmware::StoreItem* item = store_->find(key);
    const uint32_t attempt = item == nullptr ? 1U : item->meta.attempt_count + 1U;
    store_->markPendingAgain(key, attempt);
    refreshQueueCounts();
    // A slow backoff once the item has reached the attempt cap, so the queue
    // does not hammer an endpoint that keeps rejecting it.
    const uint32_t retryDelay = attempt >= VM_SD_QUEUE_MAX_ATTEMPTS
                                    ? VM_SD_QUEUE_MAX_BACKOFF_MS
                                    : VM_SD_QUEUE_RETRY_INTERVAL_MS;
    nextQueuedRetryMs_ = millis() + retryDelay;
    Serial.printf("[upload] failed id=%s source=sd attempt=%u; keeping it on the card for retry\n",
                  id.c_str(),
                  static_cast<unsigned int>(attempt));
    Serial.printf("[queue] pending=%u\n", static_cast<unsigned int>(pendingCount_));
#else
    (void)status;
#endif
}

void RecordingApp::pumpQueuedUpload() {
#if VM_ENABLE_SD && VM_ENABLE_UPLOAD
    if (store_ == nullptr || !store_->available() || !store_->healthy()) {
        return;
    }
    if (uploadTaskHandle_ == nullptr || uploadResultQueue_ == nullptr) {
        return;
    }
    // Only while Idle. An upload already in flight keeps running during a
    // recording (that is the point of the queue), but *starting* one never
    // competes with I2S for the card, and Saving/Uploading own the worker.
    if (state_ != State::Idle) {
        return;
    }
    if (jobInFlight_ || pendingQueuedOutcome_) {
        return;
    }
    if (store_->pendingCount() == 0) {
        return;
    }

    const uint32_t now = millis();
    if (nextQueuedRetryMs_ != 0 &&
        static_cast<int32_t>(now - nextQueuedRetryMs_) < 0) {
        return;
    }

    if (!wifi_.isConnected()) {
        if (static_cast<int32_t>(now - nextQueueWifiNoticeMs_) >= 0) {
            nextQueueWifiNoticeMs_ = now + VM_SD_QUEUE_RETRY_INTERVAL_MS;
            Serial.printf("[queue] pending=%u waiting for wifi\n",
                          static_cast<unsigned int>(store_->pendingCount()));
        }
        return;
    }

    // Prefer an item still under the attempt cap so one recording the ingress
    // keeps rejecting cannot starve the rest of the queue. When every item has
    // reached the cap, fall back to the oldest one and retry it slowly: nothing
    // is ever dropped or quarantined for failing to upload.
    const voice_memo_firmware::StoreItem* item =
        store_->nextPending(VM_SD_QUEUE_MAX_ATTEMPTS);
    queuedRetryDelayMs_ = VM_SD_QUEUE_RETRY_INTERVAL_MS;
    if (item == nullptr) {
        item = store_->oldestPending();
        queuedRetryDelayMs_ = VM_SD_QUEUE_MAX_BACKOFF_MS;
        if (item != nullptr && nextQueuedRetryMs_ == 0) {
            Serial.printf("[queue] all items reached %u attempts; retrying the oldest slowly\n",
                          static_cast<unsigned int>(VM_SD_QUEUE_MAX_ATTEMPTS));
        }
    }
    if (item == nullptr) {
        return;
    }
    const std::string id = item->meta.recording_id;
    const uint32_t attempt = item->meta.attempt_count + 1U;
    const uint32_t durationMs = item->meta.duration_ms;
    const uint32_t wavBytes = item->meta.wav_bytes;

    // Move the item into uploading/ before the worker opens it, so an
    // interrupted attempt is visible on the card and recovery can return it to
    // pending/ (FASE 9/10).
    const String idString(id.c_str());
    if (!store_->markUploading(id)) {
        // Either the audio is provably gone (drop it and move on) or the volume
        // refused the rename (markUnhealthy() already ran inside the store, so
        // the next serviceStorage() remounts and recovers). Either way nothing
        // is deleted or quarantined here.
        if (store_->dropIfMissing(id)) {
            Serial.printf("[store] dropped missing recording id=%s\n", idString.c_str());
            refreshQueueCounts();
            nextQueuedRetryMs_ = 0;
            return;
        }
        Serial.printf("[store] cannot start upload for id=%s; will retry\n", idString.c_str());
        nextQueuedRetryMs_ = now + queuedRetryDelayMs_;
        return;
    }

    inFlightId_ = idString;
    jobInFlight_ = true;
    jobKind_ = JobKind::UploadQueued;
    xQueueReset(uploadResultQueue_);
    Serial.printf("[upload] start id=%s source=sd bytes=%lu duration_ms=%lu attempt=%lu\n",
                  inFlightId_.c_str(),
                  static_cast<unsigned long>(wavBytes),
                  static_cast<unsigned long>(durationMs),
                  static_cast<unsigned long>(attempt));
    xTaskNotifyGive(uploadTaskHandle_);
#endif
}

void RecordingApp::serviceStorage() {
#if VM_ENABLE_SD
    if (store_ == nullptr) {
        return;
    }
    if (store_->available() && store_->healthy()) {
        return;
    }
    // Never unmount while the worker holds an open file handle, and never while
    // audio is being captured: SD_MMC::begin()/end() and the recovery scan are
    // blocking, and a stall there would drop microphone samples. The remount is
    // simply deferred to the next Idle.
    if (jobInFlight_ || pendingQueuedOutcome_) {
        return;
    }
    if (state_ == State::Recording || state_ == State::MaxReachedWaitingRelease ||
        state_ == State::Saving) {
        return;
    }
    if (store_->serviceMount(millis())) {
        sdWasAvailable_ = true;
        refreshQueueCounts();
        Serial.printf("[queue] pending=%u\n", static_cast<unsigned int>(pendingCount_));
    }
#endif
}

void RecordingApp::refreshQueueCounts() {
#if VM_ENABLE_SD
    if (store_ != nullptr) {
        pendingCount_ = store_->pendingCount();
        storageFull_ = store_->storageFull();
    }
#endif
}

void RecordingApp::clearRecording() {
    buffer_.reset();
    sampleCount_ = 0;
    durationMs_ = 0;
    maxReached_ = false;
}

void RecordingApp::noteActivity(voice_memo_firmware::ActivityEvent event) {
    // One place, one counter, one timestamp. The UI's touch and the state
    // machine's BOOT/recording edges both land here, so the power policy
    // consumes a single activity stream. The classification is applied by
    // PowerManager::markActivity(), which refuses anything that is not a real
    // user interaction - so a mistake here cannot silently disable auto-off.
    lastInteractionMs_ = millis();
    ++activityCount_;
    lastActivityEvent_ = event;
}

void RecordingApp::logRefusedBootPress() {
    // This line is the field-test proof that tick() kept running while the
    // worker was busy. State is appended for diagnosis; the token is never
    // involved.
    ++refusedPressCount_;
    lastRefusedPressMs_ = millis();
    Serial.printf("[app] BOOT press ignored: buffer busy id=%s state=%s\n",
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

    // Persistent queue status. Every field comes from a cached counter owned by
    // the main loop, so the UI never touches the card.
    snapshot.pending_count = pendingCount_;
    snapshot.storage_full = storageFull_;
    // Derived from the store rather than tracked separately, so the panel cannot
    // disagree with the volume's real health. A card that was never mounted is
    // reported as "no SD"; a card that was mounted and is now unusable, or that
    // is mounted but refused an operation, is an error.
    snapshot.sd_available = store_ != nullptr && store_->available();
    snapshot.sd_error = store_ != nullptr && sdWasAvailable_ &&
                        !(store_->available() && store_->healthy());
    snapshot.background_upload = (jobInFlight_ || pendingQueuedOutcome_) &&
                                 jobKind_ == JobKind::UploadQueued;
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
    // frozen tag of an in-flight recording (Recording/Saving/Uploading/
    // RetryWait) is untouchable by construction.
    return tagSelection_.select(tag, state_);
}

// ---------------------------------------------------------------------------
// Power management surface
// ---------------------------------------------------------------------------

voice_memo_firmware::ShutdownInhibit RecordingApp::shutdownInhibit() const {
    if (state_ == State::Saving) {
        // The commit protocol is write tmp -> rename meta -> rename WAV. The
        // single PSRAM copy is still the only copy until it returns, so this is
        // a soft block: the request is deferred, never abandoned.
        return voice_memo_firmware::ShutdownInhibit::Saving;
    }
    return voice_memo_firmware::shutdown_inhibit_for_state(state_);
}

bool RecordingApp::hasVolatileRecording() const {
    // Uploading/RetryWait are the volatile fallback path: a finalized WAV that
    // the card never took (no card, card full, card unhealthy) and that the
    // ingress has not confirmed. Losing power here loses the note.
    return state_ == State::Uploading || state_ == State::RetryWait;
}

void RecordingApp::notifyPowerNotice(voice_memo_firmware::PowerNotice notice, uint32_t now_ms) {
    powerNotice_.notice = notice;
    powerNotice_.at_ms = now_ms;
    ++powerNotice_.count;
}

bool RecordingApp::serviceShutdownWork() {
    // Deliberately the same two services tick() runs, minus everything that can
    // start new work: no pumpQueuedUpload(), no serviceStorage(), so the
    // shutdown cannot be extended by a new upload or a blocking remount that
    // starts after the request.
    serviceDeferredQueuedOutcome();
    pollJobOutcome();

#if VM_ENABLE_UPLOAD
    if (uploadTaskHandle_ != nullptr) {
        return !jobInFlight_ && !pendingQueuedOutcome_;
    }
#endif
    return !jobInFlight_ && !pendingQueuedOutcome_;
}

void RecordingApp::abortUploadForShutdown() {
#if VM_ENABLE_UPLOAD
    if (!jobInFlight_) {
        return;
    }
    // Cooperative flag first (cheap, honoured between chunks and while reading
    // the response), then the socket close so a write already blocked inside
    // lwIP returns immediately instead of holding the device on for the HTTP
    // timeout.
    abortUpload_ = true;
    uploader_.abortActiveTransfer();
#endif
}

bool RecordingApp::releaseStorageForShutdown() {
#if VM_ENABLE_SD
    if (store_ == nullptr || !store_->available()) {
        return true;
    }
    if (jobInFlight_ || pendingQueuedOutcome_) {
        // Never tear the volume down under the worker: it may hold an open file
        // and is mid-rename of a sidecar.
        Serial.println("[power] SD release deferred: upload worker still active");
        return false;
    }
    // Every write is flush()ed and closed by SdStorage::writeFile() and the
    // commit ends in a rename, so there is no dirty page to sync here; the
    // unmount is what guarantees the FAT metadata is on the card before the rail
    // drops and guarantees no handle survives the power cut.
    store_->setLogSink(&storeLogSink);
    Serial.printf("[power] SD queue safe pending=%u\n",
                  static_cast<unsigned int>(store_->pendingCount()));
    store_->closeVolume();
    return true;
#else
    return true;
#endif
}

#if VM_ENABLE_UPLOAD
void RecordingApp::uploadTaskEntry(void* context) {
    static_cast<RecordingApp*>(context)->uploadTaskLoop();
}

void RecordingApp::uploadTaskLoop() {
    for (;;) {
        // Block indefinitely (no polling, no CPU) until the main loop hands
        // this task a job.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        JobOutcome outcome;
        outcome.kind = static_cast<uint8_t>(jobKind_);

        switch (jobKind_) {
            case JobKind::PersistToSd:
                // Writes the frozen PSRAM WAV + metadata to the card. The main
                // loop froze recording_id_, duration_ms_ and the buffer before
                // notifying, and refuses new recordings while state == Saving.
                outcome.status = static_cast<uint8_t>(persistRecording());
                break;

            case JobKind::UploadVolatile: {
                // Reads the frozen PSRAM buffer through a memory source, so the
                // legacy path uploads the exact same bytes as before.
                voice_memo_firmware::MemoryByteSource source(buffer_.data(), buffer_.totalBytes());
                outcome.status = static_cast<uint8_t>(uploader_.uploadStream(
                    INGEST_URL,
                    INGEST_TOKEN,
                    deviceId_,
                    recordingId_,
                    source,
                    durationMs_,
                    VM_FIRMWARE_VERSION,
                    nullptr));
                break;
            }

            case JobKind::UploadQueued:
                runQueuedUploadJob(outcome);
                break;

            case JobKind::None:
                outcome.status = 0;
                break;
        }

        // One slot, and the main loop drains it before another job can start,
        // so this send never blocks for long. The queue is also the memory
        // barrier that hands the result safely to the main loop.
        xQueueSend(uploadResultQueue_, &outcome, portMAX_DELAY);
    }
}

void RecordingApp::runQueuedUploadJob(JobOutcome& outcome) {
    outcome.status = static_cast<uint8_t>(voice_memo_firmware::UploadStatus::Failed);
#if VM_ENABLE_SD
    if (store_ == nullptr || inFlightId_.length() == 0) {
        return;
    }

    const std::string key(inFlightId_.c_str());
    const voice_memo_firmware::StoreItem* item = store_->find(key);
    const uint32_t durationMs = item == nullptr ? 0U : item->meta.duration_ms;

    voice_memo_firmware::FileHandle* handle = store_->openRead(key);
    if (handle == nullptr) {
        return;
    }

    FileByteSource source(handle);
    const voice_memo_firmware::UploadStatus status = uploader_.uploadStream(
        INGEST_URL,
        INGEST_TOKEN,
        deviceId_,
        inFlightId_,
        source,
        durationMs,
        VM_FIRMWARE_VERSION,
        &abortUpload_);
    source.close();
    outcome.status = static_cast<uint8_t>(status);
#endif
}
#endif  // VM_ENABLE_UPLOAD
