#pragma once

#include <Arduino.h>
#include <cstdint>

#include "config.h"

#if VM_ENABLE_UPLOAD
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

#include "audio_capture.h"
#include "recording_id.h"
#include "button.h"
#include "device_id.h"
#include "power_policy.h"
#include "recording_state.h"
#include "tag_selection.h"
#include "upload_status.h"
#include "uploader.h"
#include "voice_tags.h"
#include "wav_recording.h"
#include "wifi_manager.h"

// The persistent store is a pure class; RecordingApp holds it behind a pointer
// so the exact same firmware builds with VM_ENABLE_SD 0 (store == nullptr) and
// falls back to the original single-PSRAM-buffer behaviour.
#include "recording_store.h"

class RecordingApp {
public:
    // The whole lifecycle (including Saving/Uploading/RetryWait) lives in the
    // pure, host-tested recording_state.h so the firmware and the host test
    // share the exact same transition rules.
    using State = voice_memo_firmware::AppState;

    // Why a capture ended. Both reasons run the *same* finalize and pipeline
    // code; this only drives the serial wording, so the field logs can tell a
    // deliberate stop from the safety net. There is deliberately no Error value
    // until a real error path needs to finalize a capture.
    enum class StopReason {
        // A second BOOT press (toggle-to-record): the user stopped the capture.
        User,
        // VM_MAX_RECORDING_MS elapsed without a second press. Treated exactly
        // like a manual stop: saved, queued and uploaded normally.
        Timeout,
    };

    // Read-only projection of the firmware handed to the UI.
    //
    // The UI needs to *observe* the real state machine, never to duplicate it:
    // every field below is copied out of RecordingApp's own members, and the
    // counters exist so the UI can react to one-off events (a successful
    // upload, a BOOT press refused because the buffer was busy) without polling
    // millis()-based flags.
    struct UiSnapshot {
        voice_memo_firmware::AppState state = voice_memo_firmware::AppState::Idle;
        // Tag chosen for the next recording (only changed through
        // setSelectedTag(), only while Idle).
        voice_memo_firmware::VoiceTag selected_tag = voice_memo_firmware::VoiceTag::Work;
        // Copy frozen by startRecording(): the tag the buffered/uploaded audio
        // actually belongs to.
        voice_memo_firmware::VoiceTag recording_tag = voice_memo_firmware::VoiceTag::Work;
        // Live elapsed time while Recording, derived from the sample counter
        // exactly like duration_ms_for_samples() is used at finalize time.
        uint32_t live_elapsed_ms = 0;
        // Duration of the finalized WAV; stays valid through
        // Saving/Uploading/RetryWait because the buffer is frozen there.
        uint32_t frozen_duration_ms = 0;
        // Monotonic counters; the UI renders feedback when they change.
        uint32_t upload_success_count = 0;
        uint32_t refused_press_count = 0;
        uint32_t last_upload_success_ms = 0;
        uint32_t last_refused_press_ms = 0;
        bool wifi_connected = false;
        bool upload_enabled = false;
        bool buffer_ready = false;
        bool audio_ready = false;

        // --- persistent queue (microSD) -------------------------------------
        // True once the card is mounted and the layout exists.
        bool sd_available = false;
        // True after an I/O failure while the card is (or was) unusable. The UI
        // renders SD ERROR and the firmware keeps working in RAM.
        bool sd_error = false;
        // True when the last commit was refused for space; cleared by a
        // successful commit or a successful background upload.
        bool storage_full = false;
        // Recordings on the card that the ingress has not confirmed yet.
        uint32_t pending_count = 0;
        // A background upload from the card is in flight right now. Never a
        // firmware state: recording stays possible while this is true.
        bool background_upload = false;
    };

    // `store` is null when the firmware is built without microSD support; every
    // SD code path is then compiled out or skipped.
    RecordingApp(
        Button& button,
        DeviceIdProvider& deviceId,
        AudioCapture& audio,
        WavRecordingBuffer& buffer,
        Esp32IngressUploader& uploader,
        WifiManager& wifi,
        voice_memo_firmware::RecordingStore* store = nullptr
    );

    void begin();
    void tick();

    // --- Read-only observation API for the UI -------------------------------
    UiSnapshot uiSnapshot() const;
    State state() const { return state_; }
    uint32_t recordingDurationMs() const;
    voice_memo_firmware::VoiceTag selectedTag() const { return tagSelection_.selected(); }
    voice_memo_firmware::VoiceTag recordingTag() const { return tagSelection_.recording(); }

    // The only mutation the UI is allowed to perform. Returns false (and changes
    // nothing) unless the buffer is free, i.e. state == Idle, so the tag of an
    // in-flight recording can never be rewritten from the touch screen.
    bool setSelectedTag(voice_memo_firmware::VoiceTag tag);

    // --- power management ---------------------------------------------------
    // Why the device may not be powered off right now, as a single answer
    // derived from the state machine (see shutdown_inhibit_for_state).
    //
    //   VolatileUnsavedRecording  the only copy of a WAV is in PSRAM: a power
    //                             cut would destroy a note. Hard block.
    //   Recording                 a capture is being serviced (including the
    //                             waiting-for-release tail). Soft block.
    //   Saving                    a commit is in flight. Soft block, deferred.
    //   None                      safe. A non-empty pending/ queue is NOT a
    //                             block: those recordings are already durable on
    //                             the card and are recovered on the next boot.
    voice_memo_firmware::ShutdownInhibit shutdownInhibit() const;

    // True when the finalized/buffered WAV exists only in PSRAM and the ingress
    // has not confirmed it. Used by the UI for the UNSENT NOTE notice.
    bool hasVolatileRecording() const;

    // Called from the shutdown sequence only. Polls the worker (draining a
    // queued-upload outcome and servicing a deferred one) without starting any
    // new work, and defers remounts exactly like tick(). Returns true once the
    // worker is idle, i.e. once no file handle is open and the volume is quiet.
    bool serviceShutdownWork();
    // Interrupts a running upload: sets the cooperative abort flag and drops the
    // socket, so a worker blocked in a socket write returns immediately. The
    // recording is never deleted - the queue keeps it and boot recovery makes it
    // pending again.
    void abortUploadForShutdown();
    // Flushes and releases the card so the shutdown power cut cannot interrupt a
    // filesystem operation. Refuses (and returns false) while a job is in
    // flight, so a commit in progress is never interrupted.
    bool releaseStorageForShutdown();
    // True while the shared worker still owns a job.
    bool workerBusy() const { return jobInFlight_ || pendingQueuedOutcome_; }

    // Real user interactions observed here rather than in the UI, so the
    // inactivity timer is driven by what actually happened instead of by what was
    // rendered.
    //
    // A monotonically increasing counter rather than a timestamp: the loop and
    // the UI must not be able to swallow each other's event by happening to
    // sample the same millisecond. The loop compares activityCount() against the
    // value it saw last and reports every missed increment.
    uint32_t activityCount() const { return activityCount_; }
    // The interaction type behind the most recent increment.
    voice_memo_firmware::ActivityEvent lastActivityEvent() const { return lastActivityEvent_; }

    // Called by the touch layer on a real press edge (never per poll, never for
    // a ghost sample). Kept as a named entry point so the UI cannot label a
    // background event as a user interaction by accident.
    void notifyTouchInteraction() {
        noteActivity(voice_memo_firmware::ActivityEvent::TouchTap);
    }

    // Notification the UI consumes to paint "STOP RECORDING FIRST" or
    // "UNSENT NOTE". A monotonically increasing counter plus the event's
    // timestamp: exactly one render per notification, no polling.
    struct PowerNoticeSignal {
        uint32_t count = 0;
        uint32_t at_ms = 0;
        voice_memo_firmware::PowerNotice notice = voice_memo_firmware::PowerNotice::None;
    };
    void notifyPowerNotice(voice_memo_firmware::PowerNotice notice, uint32_t now_ms);
    PowerNoticeSignal powerNoticeSignal() const { return powerNotice_; }

    // Number of committed recordings still waiting on the card, for the final
    // POWERED OFF screen. Cached, so it never touches the volume.
    uint32_t pendingCount() const { return pendingCount_; }

private:
    // What the single background worker is currently doing. One job at a time:
    // the main loop freezes everything the job needs before notifying the task
    // and only issues the next job after the outcome has been drained.
    enum class JobKind : uint8_t {
        None = 0,
        // Commit the frozen PSRAM WAV to the microSD card (state: Saving).
        PersistToSd = 1,
        // Upload the frozen PSRAM WAV (volatile fallback: no card).
        UploadVolatile = 2,
        // Upload one recording from the persistent queue (state-independent).
        UploadQueued = 3,
    };

    // Result handed from the worker back to the main loop. Plain POD so it can
    // travel through a FreeRTOS queue by value (no owned pointers).
    struct JobOutcome {
        uint8_t kind = 0;    // cast of JobKind
        uint8_t status = 0;  // cast of UploadStatus or SaveOutcome
    };

    // --- per-state handlers (main loop) -------------------------------------
    // Recording is a toggle: tickIdle() consumes a press edge as "start" and
    // tickRecording() consumes one as "stop". The release edge is never an
    // action, so a capture keeps running with the finger lifted, and the
    // release of the starting press cannot stop the recording it started.
    void tickIdle();
    void tickRecording();
    // Retained with the AppState::MaxReachedWaitingRelease mapping in
    // recording_state.h. The timeout path no longer enters that state, so this
    // handler is currently unreachable; it is kept so the enum, the UI model and
    // the power policy stay consistent.
    void tickMaxReached();
    void tickSaving();
    void tickUploading();
    void tickRetryWait();

    void startRecording();
    // The single exit point of a capture. Finalizes the WAV (audio stop, header,
    // duration and the peak/RMS diagnostics) and hands it to the one existing
    // pipeline: persistRecording() -> pending/ -> background upload, or the
    // volatile fallback when no card can take it. Both a manual stop and the
    // safety timeout go through here, so neither can drift from the other.
    void stopRecordingAndFinalize(StopReason reason);

    // Decides what happens to a finalized WAV: persist it to the card when one
    // is available, otherwise use the volatile single-buffer path.
    void finishRecordingAndUpload();
    // Volatile fallback: uploads (or waits to retry) the PSRAM WAV, refusing new
    // recordings exactly like the pre-microSD firmware.
    void fallbackToVolatile();

    // --- persistent path ----------------------------------------------------
    voice_memo_firmware::SaveOutcome persistRecording();
    void startPersistJob();
    void handlePersistOutcome(voice_memo_firmware::SaveOutcome outcome);

    // --- upload path --------------------------------------------------------
    bool startAsyncUpload();
    void pollJobOutcome();
    void handleUploadOutcome(voice_memo_firmware::UploadStatus status);
    void handleQueuedUploadOutcome(voice_memo_firmware::UploadStatus status);
    // Starts one background upload from the queue when the worker is free,
    // Wi-Fi is up and an item is due. Only runs in Idle, so no card work ever
    // competes with audio capture; a new recording is still free to start while
    // an already-running upload finishes.
    void pumpQueuedUpload();
    // Deadline-based remount after an I/O failure; never called while the
    // worker holds an open file handle and never while audio is being captured.
    void serviceStorage();
    // Drains a queued-upload result that was held back during Recording.
    void serviceDeferredQueuedOutcome();
    void refreshQueueCounts();

    void clearRecording();
    void logRefusedBootPress();
    // Reports a real user interaction to the power policy's activity timer.
    // Called only from the places below that correspond to something the user
    // actually did; never from background work (see power_policy.h).
    void noteActivity(voice_memo_firmware::ActivityEvent event);

    static uint32_t currentUtcSeconds();

#if VM_ENABLE_UPLOAD
    static void uploadTaskEntry(void* context);
    // Runs on its own FreeRTOS task; blocks on a task notification until the
    // main loop hands it a job (persist or upload).
    void uploadTaskLoop();
    void runQueuedUploadJob(JobOutcome& outcome);

    TaskHandle_t uploadTaskHandle_ = nullptr;
    QueueHandle_t uploadResultQueue_ = nullptr;
#endif

    Button& button_;
    DeviceIdProvider& deviceIdProvider_;
    AudioCapture& audio_;
    WavRecordingBuffer& buffer_;
    Esp32IngressUploader& uploader_;
    voice_memo_firmware::RecordingIdGenerator recordingIdGenerator_;
    WifiManager& wifi_;
    voice_memo_firmware::RecordingStore* store_;

    State state_ = State::Idle;
    String deviceId_;
    String recordingId_;
    uint32_t sampleCount_ = 0;
    uint32_t durationMs_ = 0;
    uint32_t nextProgressSample_ = 0;
    int32_t peakSample_ = 0;
    uint64_t sumSquares_ = 0;
    unsigned long nextRetryMs_ = 0;
    unsigned long nextWifiNoticeMs_ = 0;
    unsigned long nextPendingNoticeMs_ = 0;
    uint32_t retryAttempt_ = 0;
    // True when the last capture ended on the safety timeout rather than on a
    // user press. Diagnostics only: the WAV, its id and its path through the
    // pipeline are identical either way.
    bool maxReached_ = false;

    // Tag selection. The pure TagSelection holder keeps the choice for the next
    // recording apart from the copy frozen at startRecording(), so touching the
    // screen during an upload can never rewrite existing metadata.
    voice_memo_firmware::TagSelection tagSelection_;

    // UI feedback counters. Written only from the main loop (the worker reports
    // through a queue), so the UI can read them without locking.
    uint32_t uploadSuccessCount_ = 0;
    uint32_t lastUploadSuccessMs_ = 0;
    uint32_t refusedPressCount_ = 0;
    uint32_t lastRefusedPressMs_ = 0;

    // --- background worker bookkeeping --------------------------------------
    // Set before notifying the worker, read by it after the notification (which
    // is also the memory barrier). Owned by the main loop otherwise.
    JobKind jobKind_ = JobKind::None;
    bool jobInFlight_ = false;
    // Set by the main loop when a newly finalized recording needs the worker
    // more urgently than the queued upload it is running. Read by the worker
    // between HTTP chunks.
    volatile bool abortUpload_ = false;
    // Metadata frozen by the main loop for the next persist job.
    voice_memo_firmware::RecordingMeta persistMeta_;
    // Id of the queued recording currently being uploaded ("" when none).
    String inFlightId_;

    // --- persistent queue status (main loop only) ---------------------------
    uint32_t pendingCount_ = 0;
    bool storageFull_ = false;
    // Power feedback (main loop only). `lastInteractionMs_` is the time of the
    // most recent real interaction; `activityCount_` is what the loop samples.
    uint32_t lastInteractionMs_ = 0;
    uint32_t activityCount_ = 0;
    voice_memo_firmware::ActivityEvent lastActivityEvent_ =
        voice_memo_firmware::ActivityEvent::UiInteraction;
    PowerNoticeSignal powerNotice_;
    // True once the card has been mounted at least once, so "was there, now
    // gone" (SD ERROR) is distinguishable from "never there" (NO SD).
    bool sdWasAvailable_ = false;
    unsigned long nextQueuedRetryMs_ = 0;
    unsigned long nextQueueWifiNoticeMs_ = 0;
    // Delay applied after the next queued failure. Normally the short retry
    // interval; the long backoff when every pending item has hit the attempt
    // cap (which lets later, healthy items still be tried in between).
    uint32_t queuedRetryDelayMs_ = 0;
    // Re-mint the recording id at most this many times if the card already
    // holds a recording with the same id (only possible via a cross-boot id
    // collision). Bounded so a pathological card cannot loop forever.
    uint32_t persistIdRetries_ = 0;
    // A queued-upload outcome held back while audio is being captured, so no
    // filesystem mutation competes with I2S. jobInFlight_ stays true meanwhile.
    bool pendingQueuedOutcome_ = false;
    uint8_t pendingQueuedStatus_ = 0;
};
