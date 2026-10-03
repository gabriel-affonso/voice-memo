// Host-side test for the recording/upload state machine in recording_state.h.
//
// recording_state.h has no Arduino/Wi-Fi/FreeRTOS dependency on purpose, so the
// exact rules RecordingApp uses to protect the single PSRAM WAV buffer are
// verified here without a board:
//
//     c++ -std=c++11 -Wall -Wextra -o /tmp/recording_state_test tests/recording_state_test.cpp
//     /tmp/recording_state_test
//
// This directory is not compiled into the firmware: Arduino only builds the
// sketch root and src/, so tests/ is ignored by arduino-cli.

#include <cstdio>

#include "../recording_state.h"

namespace {

using voice_memo_firmware::AppState;
using voice_memo_firmware::UploadStatus;

int g_failures = 0;

void checkState(const char* label, AppState actual, AppState expected) {
    if (actual == expected) {
        std::printf("PASS %s -> %s\n", label, voice_memo_firmware::app_state_name(actual));
        return;
    }
    std::printf("FAIL %s: expected %s, got %s\n",
                label,
                voice_memo_firmware::app_state_name(expected),
                voice_memo_firmware::app_state_name(actual));
    ++g_failures;
}

void checkBool(const char* label, bool actual, bool expected) {
    if (actual == expected) {
        std::printf("PASS %s = %s\n", label, actual ? "true" : "false");
        return;
    }
    std::printf("FAIL %s: expected %s, got %s\n",
                label, expected ? "true" : "false", actual ? "true" : "false");
    ++g_failures;
}

void section(const char* title) {
    std::printf("\n--- %s\n", title);
}

void checkAction(
    const char* label,
    voice_memo_firmware::ButtonAction actual,
    voice_memo_firmware::ButtonAction expected
) {
    const char* names[] = {"None", "StartRecording", "StopRecording"};
    const unsigned int actualIndex = static_cast<unsigned int>(actual);
    const unsigned int expectedIndex = static_cast<unsigned int>(expected);
    if (actual == expected) {
        std::printf("PASS %s -> %s\n", label, names[actualIndex]);
        return;
    }
    std::printf("FAIL %s: expected %s, got %s\n",
                label,
                names[expectedIndex],
                names[actualIndex]);
    ++g_failures;
}

}  // namespace

int main() {
    // Case 1: Idle -> BOOT press -> Recording.
    section("case 1: Idle -> BOOT press -> Recording");
    checkState(
        "state_after_boot_press(Idle)",
        voice_memo_firmware::state_after_boot_press(AppState::Idle),
        AppState::Recording
    );
    checkBool("allows_new_recording(Idle)", voice_memo_firmware::allows_new_recording(AppState::Idle), true);

    // Case 2: Recording -> BOOT release (finalize) -> Uploading.
    section("case 2: Recording -> BOOT release -> Uploading");
    checkState(
        "state_after_finalize(uploads enabled, wifi up)",
        voice_memo_firmware::state_after_finalize(true, true),
        AppState::Uploading
    );

    // Case 3: Uploading + BOOT press -> stays Uploading, new recording refused.
    section("case 3: Uploading + BOOT press -> refused");
    checkState(
        "state_after_boot_press(Uploading)",
        voice_memo_firmware::state_after_boot_press(AppState::Uploading),
        AppState::Uploading
    );
    checkBool(
        "allows_new_recording(Uploading)",
        voice_memo_firmware::allows_new_recording(AppState::Uploading),
        false
    );

    // Case 4: Uploading -> Accepted -> Idle.
    section("case 4: Uploading -> Accepted -> Idle");
    checkState(
        "state_after_upload_result(Accepted)",
        voice_memo_firmware::state_after_upload_result(UploadStatus::Accepted),
        AppState::Idle
    );

    // Case 5: Uploading -> AlreadyKnown -> Idle.
    section("case 5: Uploading -> AlreadyKnown -> Idle");
    checkState(
        "state_after_upload_result(AlreadyKnown)",
        voice_memo_firmware::state_after_upload_result(UploadStatus::AlreadyKnown),
        AppState::Idle
    );

    // Case 6: Uploading -> Failed -> RetryWait.
    section("case 6: Uploading -> Failed -> RetryWait");
    checkState(
        "state_after_upload_result(Failed)",
        voice_memo_firmware::state_after_upload_result(UploadStatus::Failed),
        AppState::RetryWait
    );

    // Case 7: RetryWait -> retry due -> Uploading (only with Wi-Fi).
    section("case 7: RetryWait -> retry -> Uploading");
    checkState(
        "state_when_retry_due(wifi up)",
        voice_memo_firmware::state_when_retry_due(true),
        AppState::Uploading
    );
    checkState(
        "state_when_retry_due(wifi down)",
        voice_memo_firmware::state_when_retry_due(false),
        AppState::RetryWait
    );

    // Case 8: Uploading/RetryWait never let a new recording overwrite the WAV.
    section("case 8: buffer stays frozen while an upload owns it");
    checkBool(
        "buffer_is_frozen(Uploading)",
        voice_memo_firmware::buffer_is_frozen(AppState::Uploading),
        true
    );
    checkBool(
        "buffer_is_frozen(RetryWait)",
        voice_memo_firmware::buffer_is_frozen(AppState::RetryWait),
        true
    );
    checkBool(
        "allows_new_recording(RetryWait)",
        voice_memo_firmware::allows_new_recording(AppState::RetryWait),
        false
    );
    checkBool(
        "state_after_boot_press(RetryWait) stays RetryWait",
        voice_memo_firmware::state_after_boot_press(AppState::RetryWait) == AppState::RetryWait,
        true
    );
    checkBool(
        "buffer_is_frozen(Idle)",
        voice_memo_firmware::buffer_is_frozen(AppState::Idle),
        false
    );
    checkBool(
        "buffer_is_frozen(Recording)",
        voice_memo_firmware::buffer_is_frozen(AppState::Recording),
        false
    );
    checkBool(
        "buffer_is_frozen(MaxReachedWaitingRelease)",
        voice_memo_firmware::buffer_is_frozen(AppState::MaxReachedWaitingRelease),
        false
    );

    // Case 9: the persistent path freezes the buffer only while the SD commit
    // runs, then hands ownership to the card and returns to Idle.
    section("case 9: Saving -> committed -> Idle");
    checkBool(
        "buffer_is_frozen(Saving)",
        voice_memo_firmware::buffer_is_frozen(AppState::Saving),
        true
    );
    checkBool(
        "allows_new_recording(Saving)",
        voice_memo_firmware::allows_new_recording(AppState::Saving),
        false
    );
    checkBool(
        "state_after_boot_press(Saving) stays Saving",
        voice_memo_firmware::state_after_boot_press(AppState::Saving) == AppState::Saving,
        true
    );
    checkState(
        "state_after_persist_ok()",
        voice_memo_firmware::state_after_persist_ok(),
        AppState::Idle
    );
    checkBool(
        "allows_new_recording(Idle) after a commit",
        voice_memo_firmware::allows_new_recording(voice_memo_firmware::state_after_persist_ok()),
        true
    );

    // Toggle-to-record. This is the whole button rule the recorder executes
    // (RecordingApp::tickIdle/tickRecording): one press starts from Idle, a
    // second press finalizes, and the *release* is never an action - which is
    // what keeps the capture running with the finger lifted AND what stops the
    // release of the starting press from being read as a stop.
    section("toggle: Idle -> start, Recording -> stop, everything else refused");
    checkAction(
        "Idle + press",
        voice_memo_firmware::button_action_for_state(AppState::Idle),
        voice_memo_firmware::ButtonAction::StartRecording
    );
    checkAction(
        "Recording + press",
        voice_memo_firmware::button_action_for_state(AppState::Recording),
        voice_memo_firmware::ButtonAction::StopRecording
    );
    checkAction(
        "Saving + press is refused (the PSRAM copy is the only one)",
        voice_memo_firmware::button_action_for_state(AppState::Saving),
        voice_memo_firmware::ButtonAction::None
    );
    checkAction(
        "Uploading + press is refused (the upload owns the WAV)",
        voice_memo_firmware::button_action_for_state(AppState::Uploading),
        voice_memo_firmware::ButtonAction::None
    );
    checkAction(
        "RetryWait + press is refused (the WAV must survive for the retry)",
        voice_memo_firmware::button_action_for_state(AppState::RetryWait),
        voice_memo_firmware::ButtonAction::None
    );
    checkAction(
        "MaxReachedWaitingRelease + press is refused",
        voice_memo_firmware::button_action_for_state(AppState::MaxReachedWaitingRelease),
        voice_memo_firmware::ButtonAction::None
    );
    checkBool(
        "the start press leaves the machine in Recording, never Idle",
        voice_memo_firmware::button_action_for_state(AppState::Idle)
                == voice_memo_firmware::ButtonAction::StartRecording &&
            voice_memo_firmware::state_after_boot_press(AppState::Idle) == AppState::Recording,
        true
    );

    // Extra guards around the paths that used to block the main loop. Note that
    // the timeout no longer routes through state_after_max_duration(): it calls
    // stopRecordingAndFinalize(StopReason::Timeout) and goes straight to the
    // store/upload pipeline. The transition is pinned below only because
    // MaxReachedWaitingRelease is still named by the UI model and the power
    // policy.
    section("extra: legacy max-duration state, Wi-Fi down and TEST A");
    checkState(
        "state_after_max_duration()",
        voice_memo_firmware::state_after_max_duration(),
        AppState::MaxReachedWaitingRelease
    );
    checkState(
        "state_after_boot_press(MaxReachedWaitingRelease) stays put",
        voice_memo_firmware::state_after_boot_press(AppState::MaxReachedWaitingRelease),
        AppState::MaxReachedWaitingRelease
    );
    checkState(
        "state_after_boot_press(Recording) does not restart",
        voice_memo_firmware::state_after_boot_press(AppState::Recording),
        AppState::Recording
    );
    checkState(
        "state_after_finalize(wifi down) -> RetryWait",
        voice_memo_firmware::state_after_finalize(true, false),
        AppState::RetryWait
    );
    checkState(
        "state_after_finalize(uploads disabled) -> Idle",
        voice_memo_firmware::state_after_finalize(false, true),
        AppState::Idle
    );
    checkState(
        "state_after_finalize(uploads disabled, wifi down) -> Idle",
        voice_memo_firmware::state_after_finalize(false, false),
        AppState::Idle
    );

    section("extra: state names are usable in logs");
    checkBool(
        "app_state_name(Idle) not empty",
        voice_memo_firmware::app_state_name(AppState::Idle) != nullptr
            && voice_memo_firmware::app_state_name(AppState::Idle)[0] != '\0',
        true
    );
    checkBool(
        "app_state_name(Uploading) not empty",
        voice_memo_firmware::app_state_name(AppState::Uploading) != nullptr
            && voice_memo_firmware::app_state_name(AppState::Uploading)[0] != '\0',
        true
    );
    checkBool(
        "app_state_name(Saving) not empty",
        voice_memo_firmware::app_state_name(AppState::Saving) != nullptr
            && voice_memo_firmware::app_state_name(AppState::Saving)[0] != '\0',
        true
    );

    if (g_failures == 0) {
        std::printf("\nall recording_state tests passed\n");
        return 0;
    }
    std::printf("\n%d recording_state test(s) FAILED\n", g_failures);
    return 1;
}
