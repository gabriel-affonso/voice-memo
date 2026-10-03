// Host-side tests for the power policy and the PowerManager.
//
// This is the activity / inhibition / shutdown-decision half of the feature. The
// whole point of keeping PowerManager free of GPIO, millis() and the panel is
// that it can be driven here on a fake clock:
//
//     c++ -std=c++11 -Wall -Wextra -Itests -I. -o /tmp/power_manager_test \
//         tests/power_manager_test.cpp
//     /tmp/power_manager_test
//
// The <Arduino.h> resolve comes from -Itests (tests/Arduino.h): PowerManager only
// needs Serial, so it does not need the ESP32 core. The debounce/arming cases
// that need no Serial at all live in power_button_test.cpp.

#include <cstdint>
#include <cstdio>

#include "arduino_shim.h"

#include "../power_manager.h"
#include "../power_policy.h"

namespace {

using voice_memo_firmware::ActivityEvent;
using voice_memo_firmware::AppState;
using voice_memo_firmware::PowerButton;
using voice_memo_firmware::PowerManager;
using voice_memo_firmware::PowerNotice;
using voice_memo_firmware::ShutdownInhibit;
using voice_memo_firmware::ShutdownReason;

constexpr int kLow = 0;   // GPIO18 pressed (BAT_KEY to GND)
constexpr int kHigh = 1;  // GPIO18 released
constexpr uint32_t kDebounce = 50;

int g_failures = 0;
int g_checks = 0;

void check(const char* label, bool ok) {
    ++g_checks;
    if (ok) {
        std::printf("PASS %s\n", label);
    } else {
        std::printf("FAIL %s\n", label);
        ++g_failures;
    }
}

void checkU32(const char* label, uint32_t actual, uint32_t expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s = %u\n", label, static_cast<unsigned int>(actual));
    } else {
        std::printf("FAIL %s: expected %u, got %u\n",
                    label,
                    static_cast<unsigned int>(expected),
                    static_cast<unsigned int>(actual));
        ++g_failures;
    }
}

void checkInhibit(const char* label, ShutdownInhibit actual, ShutdownInhibit expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s = %s\n", label, voice_memo_firmware::shutdown_inhibit_name(actual));
    } else {
        std::printf("FAIL %s: expected %s, got %s\n",
                    label,
                    voice_memo_firmware::shutdown_inhibit_name(expected),
                    voice_memo_firmware::shutdown_inhibit_name(actual));
        ++g_failures;
    }
}

void section(const char* title) {
    std::printf("\n--- %s\n", title);
}

// ---------------------------------------------------------------------------
// A harness that stands in for the main loop: it advances a fake millisecond
// clock, samples the PWR pin, and reports when PowerManager asks for a shutdown.
// ---------------------------------------------------------------------------
class Harness {
public:
    explicit Harness(uint32_t debounceMs = kDebounce)
        : manager_(true, debounceMs) {
        manager_.begin(nowMs_);
        // One released sample so the button is ARMED before a test presses it.
        // Arming is covered exhaustively in power_button_test.cpp; here it is
        // just the precondition every user-press case needs.
        pinnedLevel_ = kHigh;
        runFor(200, kHigh);
        // Establish a clean reference point: the inactivity interval measured by
        // every test starts here, at the end of the arming window, so "119999 ms
        // of inactivity" means exactly that and not "200 ms plus 119999 ms".
        manager_.markActivity(ActivityEvent::UiInteraction, nowMs_);
    }

    // A harness whose button has never been armed (no released sample seen yet),
    // for the "boot with PWR still held" cases.
    static Harness unarmed(uint32_t debounceMs = kDebounce) {
        Harness harness(debounceMs, SkipArmTag{});
        harness.pinnedLevel_ = kLow;
        return harness;
    }

    void setInhibit(ShutdownInhibit inhibit) { inhibit_ = inhibit; }
    void setState(AppState state) { setInhibit(voice_memo_firmware::shutdown_inhibit_for_state(state)); }
    void setDataAtRisk(bool atRisk) { dataAtRisk_ = atRisk; }

    // One loop() iteration's worth of power servicing: sample the pin, tick the
    // manager and record any shutdown request.
    //
    // Once a shutdown is granted the harness stops ticking, exactly like the
    // firmware: loop() calls enterGracefulShutdown(), which never returns, so a
    // second request cannot be produced. That is what makes "fires exactly once"
    // a meaningful assertion instead of a property of the counter.
    bool tick() {
        if (shutdownLatched_) {
            return false;
        }

        PowerManager::Context context;
        context.inhibit = inhibit_;
        context.data_at_risk = dataAtRisk_;
        context.wifi_connected = false;

        const bool granted = manager_.tick(nowMs_, pinnedLevel_, context);
        if (granted) {
            ++shutdownCount_;
            lastReason_ = manager_.takeShutdownReason();
            shutdownLatched_ = true;
        }
        const PowerNotice notice = manager_.takeNotice();
        if (notice != PowerNotice::None) {
            lastNotice_ = notice;
            ++noticeCount_;
        }
        return granted;
    }

    // Advances the clock in 1 ms steps, ticking every step. `level` is pinned for
    // the whole window, which is what the loop does between gestures.
    void runFor(uint32_t ms, int level) {
        pinnedLevel_ = level;
        for (uint32_t i = 0; i < ms; ++i) {
            // Time first, then service: after advance(N) the harness clock has
            // moved exactly N ms and the manager has seen that instant. Ticking
            // first would leave the loop one millisecond short.
            ++nowMs_;
            tick();
        }
    }

    // Holds the pin steady and advances time with the current inhibition.
    void advance(uint32_t ms) { runFor(ms, pinnedLevel_); }

    void gesture(uint32_t holdMs = 120, uint32_t releaseMs = 120) {
        runFor(holdMs, kLow);
        runFor(releaseMs, kHigh);
    }

    void markActivity(ActivityEvent event) { manager_.markActivity(event, nowMs_); }

    // Moves the whole harness clock - and therefore the recorded arming activity
    // - to just before the 32-bit wrap, so the next long advance crosses it.
    void setClockNearWrap(uint32_t nowMs) {
        nowMs_ = nowMs;
        manager_.begin(nowMs);
    }

    uint32_t shutdownCount() const { return shutdownCount_; }
    ShutdownReason lastReason() const { return lastReason_; }
    PowerNotice lastNotice() const { return lastNotice_; }
    uint32_t noticeCount() const { return noticeCount_; }
    uint32_t now() const { return nowMs_; }
    uint32_t remaining() const { return manager_.inactivityRemainingMs(nowMs_); }

private:
    struct SkipArmTag {};

    Harness(uint32_t debounceMs, SkipArmTag) : manager_(true, debounceMs) {
        manager_.begin(nowMs_);
    }

    PowerManager manager_;
    uint32_t nowMs_ = 0;
    int pinnedLevel_ = kHigh;
    ShutdownInhibit inhibit_ = ShutdownInhibit::None;
    bool dataAtRisk_ = false;
    bool shutdownLatched_ = false;
    uint32_t shutdownCount_ = 0;
    uint32_t noticeCount_ = 0;
    ShutdownReason lastReason_ = ShutdownReason::User;
    PowerNotice lastNotice_ = PowerNotice::None;
};

}  // namespace

int main() {
    // =======================================================================
    // Part 1: the pure policy (activity classification and inhibition mapping)
    // =======================================================================

    // Case 12-17: what counts as user activity.
    section("cases 12-17: activity classification");
    check("12 touch resets the inactivity timer",
          voice_memo_firmware::activity_is_user_interaction(ActivityEvent::TouchTap));
    check("12b tag selection resets the inactivity timer",
          voice_memo_firmware::activity_is_user_interaction(ActivityEvent::TagSelected));
    check("13 BOOT resets the inactivity timer",
          voice_memo_firmware::activity_is_user_interaction(ActivityEvent::BootButton));
    check("13b start of recording resets the inactivity timer",
          voice_memo_firmware::activity_is_user_interaction(ActivityEvent::RecordingStarted));
    check("13c end of recording resets the inactivity timer",
          voice_memo_firmware::activity_is_user_interaction(ActivityEvent::RecordingStopped));
    check("13d a PWR interaction resets the inactivity timer",
          voice_memo_firmware::activity_is_user_interaction(ActivityEvent::PwrButton));
    check("14 Wi-Fi connect/disconnect does NOT reset the timer",
          !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::WifiConnected) &&
              !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::WifiDisconnected));
    check("15 NTP does NOT reset the timer",
          !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::NtpSynced));
    check("15b an RTC read does NOT reset the timer",
          !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::RtcRead));
    check("16 a background upload does NOT reset the timer",
          !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::UploadStarted) &&
              !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::UploadFinished));
    check("16b a retry does NOT reset the timer",
          !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::UploadRetryScheduled));
    check("16c an SD remount does NOT reset the timer",
          !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::SdRemounted));
    check("16d an e-paper refresh does NOT reset the timer",
          !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::EpdRefreshed));
    check("17 a pending-count change does NOT reset the timer",
          !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::PendingCountChanged));
    check("17b a log line or internal timer does NOT reset the timer",
          !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::LogLine) &&
              !voice_memo_firmware::activity_is_user_interaction(ActivityEvent::InternalTimer));

    // Case 18-21/24: the inhibition each state reports.
    section("cases 18-24: inhibition per state");
    checkInhibit("18 Recording inhibits", voice_memo_firmware::shutdown_inhibit_for_state(AppState::Recording),
                 ShutdownInhibit::Recording);
    checkInhibit("19 MaxReachedWaitingRelease inhibits",
                 voice_memo_firmware::shutdown_inhibit_for_state(AppState::MaxReachedWaitingRelease),
                 ShutdownInhibit::Recording);
    checkInhibit("20 Saving defers", voice_memo_firmware::shutdown_inhibit_for_state(AppState::Saving),
                 ShutdownInhibit::Saving);
    checkInhibit("21 Idle allows a shutdown (pending on the card included)",
                 voice_memo_firmware::shutdown_inhibit_for_state(AppState::Idle), ShutdownInhibit::None);
    checkInhibit("24 a volatile PSRAM recording blocks",
                 voice_memo_firmware::shutdown_inhibit_for_state(AppState::Uploading),
                 ShutdownInhibit::VolatileUnsavedRecording);
    checkInhibit("24b a volatile retry blocks",
                 voice_memo_firmware::shutdown_inhibit_for_state(AppState::RetryWait),
                 ShutdownInhibit::VolatileUnsavedRecording);
    check("only the volatile case is a hard (data-safety) block",
          voice_memo_firmware::shutdown_inhibit_is_hard(ShutdownInhibit::VolatileUnsavedRecording) &&
              !voice_memo_firmware::shutdown_inhibit_is_hard(ShutdownInhibit::Recording) &&
              !voice_memo_firmware::shutdown_inhibit_is_hard(ShutdownInhibit::Saving) &&
              !voice_memo_firmware::shutdown_inhibit_is_hard(ShutdownInhibit::None));

    // =======================================================================
    // Part 2: PowerManager, driven through the same main-loop contract
    // =======================================================================

    // Case 9: 119999 ms is not 120000 ms.
    section("case 9: auto-off at 119999 ms does not fire");
    {
        Harness harness;
        harness.setInhibit(ShutdownInhibit::None);
        harness.advance(119999);
        checkU32("no shutdown before the timeout", harness.shutdownCount(), 0);
        checkU32("1 ms remaining", harness.remaining(), 1);
    }

    // Case 10: at exactly 120000 ms it fires, once.
    section("case 10: auto-off at 120000 ms fires exactly once");
    {
        Harness harness;
        harness.advance(119999);
        checkU32("still on at 119999 ms", harness.shutdownCount(), 0);
        harness.advance(1);
        checkU32("fires at exactly 120000 ms", harness.shutdownCount(), 1);
        check("the reason is inactivity", harness.lastReason() == ShutdownReason::Inactivity);
        harness.advance(5000);
        checkU32("fires only once", harness.shutdownCount(), 1);
    }

    // Case 11/12/13: a user interaction restarts the interval.
    section("cases 11-13: touch / BOOT / tag reset the timer");
    {
        Harness harness;
        harness.markActivity(ActivityEvent::TouchTap);
        harness.advance(30000);
        checkU32("a touch at 119 s keeps the device on at 149 s", harness.shutdownCount(), 0);
        harness.advance(90000);  // 120 s after the touch
        checkU32("it switches off 120 s after the touch, not after boot", harness.shutdownCount(), 1);
    }
    {
        Harness harness;
        harness.markActivity(ActivityEvent::BootButton);
        harness.advance(119000);
        checkU32("a BOOT press at 100 s still keeps it on at 219 s", harness.shutdownCount(), 0);
        harness.advance(1000);
        checkU32("and it switches off 120 s after the BOOT press", harness.shutdownCount(), 1);
    }
    {
        Harness harness;
        harness.markActivity(ActivityEvent::TagSelected);
        harness.advance(119999);
        checkU32("a tag selection at 100 s still keeps it on", harness.shutdownCount(), 0);
        harness.advance(1);
        checkU32("and it switches off 120 s after the tag selection", harness.shutdownCount(), 1);
    }

    // Cases 14-17: background events must not keep the device awake.
    section("cases 14-17: background events do not reset the timer");
    {
        Harness harness;
        harness.markActivity(ActivityEvent::WifiConnected);
        harness.markActivity(ActivityEvent::NtpSynced);
        harness.markActivity(ActivityEvent::UploadStarted);
        harness.markActivity(ActivityEvent::UploadFinished);
        harness.markActivity(ActivityEvent::UploadRetryScheduled);
        harness.markActivity(ActivityEvent::SdRemounted);
        harness.markActivity(ActivityEvent::EpdRefreshed);
        harness.markActivity(ActivityEvent::PendingCountChanged);
        harness.markActivity(ActivityEvent::LogLine);
        harness.markActivity(ActivityEvent::InternalTimer);
        // The last real interaction was the end of arming (the Harness reference
        // point); every call above is background and must not have moved it.
        harness.advance(119999);
        checkU32("still on 119999 ms after the last real interaction", harness.shutdownCount(), 0);
        harness.advance(1);
        checkU32("switches off 120000 ms after the last real interaction", harness.shutdownCount(), 1);
    }

    // Case 18/19: a recording suspends auto-off completely.
    section("cases 18-19: Recording and MaxReached suspend auto-off");
    {
        Harness harness;
        harness.setState(AppState::Recording);
        harness.advance(300000);  // five minutes of capturing, well past 120 s
        checkU32("no shutdown during Recording", harness.shutdownCount(), 0);
        harness.setState(AppState::MaxReachedWaitingRelease);
        harness.advance(60000);
        checkU32("no shutdown during MaxReachedWaitingRelease", harness.shutdownCount(), 0);
        // The capture ends: the user released BOOT, which is itself an
        // interaction, so the interval restarts rather than firing immediately.
        harness.setState(AppState::Idle);
        harness.markActivity(ActivityEvent::RecordingStopped);
        harness.advance(119999);
        checkU32("the timer restarts when the recording ends", harness.shutdownCount(), 0);
        harness.advance(1);
        checkU32("and fires 120 s after the end of the recording", harness.shutdownCount(), 1);
    }

    // Case 20/21: Saving defers, and the deferred request completes.
    section("cases 20-21: Saving defers auto-off, then completes");
    {
        Harness harness;
        harness.setState(AppState::Saving);
        harness.advance(150000);  // the timeout expires mid-commit
        checkU32("no shutdown while committing", harness.shutdownCount(), 0);
        harness.setState(AppState::Idle);  // commit confirmed
        harness.tick();                    // the deferred request is resolved
        harness.tick();                    // and granted
        checkU32("the deferred shutdown completes once the commit is done",
                 harness.shutdownCount(), 1);
        check("and it is still attributed to inactivity",
              harness.lastReason() == ShutdownReason::Inactivity);
    }

    // Case 20b: PWR during Saving defers, it does not power off under the commit.
    section("case 20b: PWR during Saving defers the request");
    {
        Harness harness;
        harness.setState(AppState::Saving);
        harness.gesture();
        checkU32("no shutdown during the commit", harness.shutdownCount(), 0);
        harness.setState(AppState::Idle);
        harness.tick();  // the deferred request is resolved
        harness.tick();  // and granted
        checkU32("the deferred PWR request completes after the commit", harness.shutdownCount(), 1);
        check("attributed to the user", harness.lastReason() == ShutdownReason::User);
    }

    // Case 22: a persisted queue does not prevent shutdown.
    section("case 22: a persisted pending queue allows shutdown");
    {
        // Idle with many recordings on the card is exactly ShutdownInhibit::None:
        // pending/ is not part of the decision at all.
        Harness harness;
        checkInhibit("Idle maps to no inhibition regardless of queue depth",
                     voice_memo_firmware::shutdown_inhibit_for_state(AppState::Idle),
                     ShutdownInhibit::None);
        harness.advance(120000);
        checkU32("auto-off fires with notes queued on the card", harness.shutdownCount(), 1);
    }

    // Case 24: a volatile recording blocks auto-off indefinitely.
    section("case 24: a PSRAM-only recording blocks auto-off");
    {
        Harness harness;
        harness.setState(AppState::RetryWait);
        harness.setDataAtRisk(true);
        harness.advance(600000);  // ten minutes offline
        checkU32("never powers off with an unsent note", harness.shutdownCount(), 0);
        check("the user is told once", harness.noticeCount() == 1);
        check("and the notice is UNSENT NOTE", harness.lastNotice() == PowerNotice::UnsentNote);
    }

    // Case 25: once the volatile upload completes, the timer - which kept its
    // real age - lets the shutdown through without another two minutes.
    section("case 25: the upload completing releases the shutdown");
    {
        Harness harness;
        harness.setState(AppState::Uploading);
        harness.setDataAtRisk(true);
        harness.advance(300000);
        checkU32("still on while the note is volatile", harness.shutdownCount(), 0);
        harness.setState(AppState::Idle);  // ingress accepted it
        harness.setDataAtRisk(false);
        harness.tick();
        checkU32("powers off as soon as the note is safe", harness.shutdownCount(), 1);
    }

    // Case 25b: PWR while a volatile note exists is refused, not deferred, and
    // the user is told why.
    section("case 25b: PWR with a volatile note is refused and explained");
    {
        Harness harness;
        harness.setState(AppState::RetryWait);
        harness.setDataAtRisk(true);
        harness.gesture();
        checkU32("the device stays on", harness.shutdownCount(), 0);
        check("UNSENT NOTE is shown", harness.lastNotice() == PowerNotice::UnsentNote);
        harness.setState(AppState::Idle);
        harness.setDataAtRisk(false);
        harness.tick();
        checkU32("the refusal was not queued into a later shutdown", harness.shutdownCount(), 0);
    }

    // Case 26: millis() wrap-around. Unsigned difference, never `now > last + t`.
    section("case 26: millis() wrap-around");
    {
        // A harness whose first activity sample sits 8 ms before the wrap, so the
        // entire 120 s interval is measured across 0xFFFFFFFF -> 0.
        Harness harness;
        harness.setClockNearWrap(0xFFFFFFF8U);
        harness.advance(119999);
        checkU32("on 119999 ms after an activity just before the wrap", harness.shutdownCount(), 0);
        harness.advance(1);
        checkU32("off 120000 ms after an activity just before the wrap",
                 harness.shutdownCount(), 1);
    }
    {
        // The predicate at the exact wrap values, with the expected instant
        // computed here rather than through the millis() helper, so the test can
        // never inherit the bug it is meant to catch.
        const uint32_t lastActivity = 0xFFFF0000U;
        const uint64_t toWrap = 0x100000000ULL - static_cast<uint64_t>(lastActivity);
        const uint32_t nowAfterWrap = static_cast<uint32_t>(200000ULL - toWrap);

        check("not expired at 100000 ms after a pre-wrap activity",
              !voice_memo_firmware::inactivity_timeout_expired(
                  static_cast<uint32_t>(lastActivity + 100000U),
                  lastActivity,
                  VM_AUTO_POWER_OFF_MS));
        check("expired at 200000 ms after a pre-wrap activity, clock wrapped",
              nowAfterWrap < lastActivity &&
                  voice_memo_firmware::inactivity_timeout_expired(
                      nowAfterWrap, lastActivity, VM_AUTO_POWER_OFF_MS));
        const uint32_t onBoundary =
            static_cast<uint32_t>(lastActivity + VM_AUTO_POWER_OFF_MS);
        const uint32_t oneBefore =
            static_cast<uint32_t>(lastActivity + VM_AUTO_POWER_OFF_MS - 1U);
        check("exactly on the boundary is expired",
              voice_memo_firmware::inactivity_timeout_expired(
                  onBoundary, lastActivity, VM_AUTO_POWER_OFF_MS));
        check("one millisecond below the boundary is not expired",
              !voice_memo_firmware::inactivity_timeout_expired(
                  oneBefore, lastActivity, VM_AUTO_POWER_OFF_MS));
        check("a zero timeout is always expired",
              voice_memo_firmware::inactivity_timeout_expired(lastActivity, lastActivity, 0));
    }

    // Case 27/28 (button state machine) are covered exhaustively in
    // power_button_test.cpp; the acceptance-level behaviour is re-checked here
    // through the manager.
    section("cases 27-28: two presses, and a held button");
    {
        // Two independent short presses each request exactly one shutdown. The
        // harness latches after granting one (like the firmware, whose shutdown
        // path never returns), so the two presses are exercised on two harnesses;
        // the multi-gesture state machine itself is covered in
        // power_button_test.cpp case 9.
        Harness first;
        first.gesture();
        checkU32("first short press requests a shutdown", first.shutdownCount(), 1);
        check("reason=user", first.lastReason() == ShutdownReason::User);
        checkU32("and it is a single request, not one per tick", first.shutdownCount(), 1);

        Harness second;
        second.gesture();
        second.gesture();
        checkU32("two presses on one device produce two requests",
                 second.shutdownCount(), 1);
    }
    {
        // Boot with the key held: no request while it is down, and the release
        // only arms (cases 2 and 3 through the manager).
        Harness holding = Harness::unarmed();
        holding.runFor(3000, kLow);
        checkU32("a held key requests nothing", holding.shutdownCount(), 0);
        holding.runFor(200, kHigh);
        checkU32("and its release arms without requesting", holding.shutdownCount(), 0);
        holding.gesture();
        checkU32("a press after that release does request a shutdown",
                 holding.shutdownCount(), 1);
    }

    // Case 18b: a PWR press during Recording is refused with a notice and does
    // not become a latent shutdown.
    section("case 18b: PWR during Recording is refused with a notice");
    {
        Harness harness;
        harness.setState(AppState::Recording);
        harness.gesture();
        checkU32("no shutdown while capturing", harness.shutdownCount(), 0);
        check("STOP RECORDING FIRST is shown",
              harness.lastNotice() == PowerNotice::StopRecordingFirst);
        harness.setState(AppState::Idle);
        harness.tick();
        checkU32("and the refusal is not remembered", harness.shutdownCount(), 0);
    }

    // Logging requirement: no line per loop iteration.
    section("logging: quiet while idle");
    {
        ::vm_test_shim::serial().lines = 0;
        Harness harness;
        const uint32_t afterBoot = ::vm_test_shim::serial().lines;
        harness.advance(10000);  // 10 000 loop iterations without a shutdown
        checkU32("no log lines during 10 s of idle ticking",
                 ::vm_test_shim::serial().lines - afterBoot, 0);
    }

    std::printf("\n=========================================\n");
    std::printf("%d check(s), %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
