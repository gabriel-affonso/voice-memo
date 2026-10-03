// Host-side tests for the BOOT button as the toggle-to-record control.
//
// Two layers are covered here, both on the Mac:
//
//   1. Button (button.cpp) - the real firmware class, driven through the GPIO
//      and millis() fakes in tests/arduino_shim.h. This is where the debounce
//      and the "one physical press == one edge" rule are asserted, which is
//      what makes toggle-to-record safe: a single tap can never be delivered as
//      both a start and a stop.
//   2. The toggle rule itself (button_action_for_state() in recording_state.h),
//      consumed exactly the way RecordingApp::tickIdle()/tickRecording() do:
//      Pressed -> act, Released -> ignored.
//
//     c++ -std=c++11 -Wall -Wextra -Itests -I. -o /tmp/button_test \
//         tests/button_test.cpp button.cpp
//     /tmp/button_test
//
// tests/ is not compiled into the firmware.

#include <cstdio>

#include "../button.h"
#include "../config.h"
#include "../recording_state.h"

namespace {

using voice_memo_firmware::AppState;
using voice_memo_firmware::ButtonAction;

constexpr int kLow = LOW;    // pressed: the button shorts the pin to GND
constexpr int kHigh = HIGH;  // released: internal pull-up

int g_failures = 0;
int g_checks = 0;

void check(const char* label, bool ok) {
    ++g_checks;
    if (ok) {
        std::printf("PASS %s\n", label);
        return;
    }
    std::printf("FAIL %s\n", label);
    ++g_failures;
}

void checkU32(const char* label, uint32_t actual, uint32_t expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s = %u\n", label, static_cast<unsigned int>(actual));
        return;
    }
    std::printf("FAIL %s: expected %u, got %u\n",
                label,
                static_cast<unsigned int>(expected),
                static_cast<unsigned int>(actual));
    ++g_failures;
}

void section(const char* title) {
    std::printf("\n--- %s\n", title);
}

// The toggle consumer, mirroring RecordingApp::tickIdle()/tickRecording(): a
// press edge is an action, a release edge is dropped, and the action comes from
// the pure rule in recording_state.h.
struct ToggleConsumer {
    AppState state = AppState::Idle;
    uint32_t starts = 0;
    uint32_t stops = 0;
    uint32_t refused = 0;

    void onPress() {
        switch (voice_memo_firmware::button_action_for_state(state)) {
            case ButtonAction::StartRecording:
                state = AppState::Recording;
                ++starts;
                break;
            case ButtonAction::StopRecording:
                state = AppState::Idle;
                ++stops;
                break;
            case ButtonAction::None:
                ++refused;
                break;
        }
    }
};

// The fake board: the test owns the pin level and the clock, and every call to
// sample() is one pass of the firmware's button_.update().
class Bench {
public:
    explicit Bench(uint8_t pin) : button_(pin, VM_BUTTON_DEBOUNCE_MS, true), pin_(pin) {}

    void begin(uint32_t nowMs, int level) {
        vm_test_shim::clockMs() = nowMs;
        vm_test_shim::pinLevel(pin_) = level;
        button_.begin();
    }

    void setLevel(int level) { vm_test_shim::pinLevel(pin_) = level; }

    // One tick of the main loop at millisecond `nowMs`.
    void tickAt(uint32_t nowMs) {
        vm_test_shim::clockMs() = nowMs;
        button_.update();
    }

    bool takePressed() { return button_.takePressedEdge() == Button::Edge::Pressed; }
    bool takeReleased() { return button_.takeReleasedEdge() == Button::Edge::Released; }
    bool isPressed() const { return button_.isPressed(); }

    // Finger down, past the debounce window: delivers the Pressed edge.
    void pressDown(uint32_t& now) {
        setLevel(kLow);
        tickAt(now);
        now += VM_BUTTON_DEBOUNCE_MS + 1;
        tickAt(now);
    }

    // Finger up, past the debounce window: delivers the Released edge.
    void liftUp(uint32_t& now) {
        setLevel(kHigh);
        tickAt(now);
        now += VM_BUTTON_DEBOUNCE_MS + 1;
        tickAt(now);
    }

    // One full tap, which is therefore one Pressed edge plus one Released edge.
    void tap(uint32_t& now) {
        pressDown(now);
        liftUp(now);
    }

    // The recorder's consumption of one tick: press acts, release is ignored.
    void service(ToggleConsumer& consumer) {
        if (takePressed()) {
            consumer.onPress();
        }
        takeReleased();
    }

private:
    Button button_;
    uint8_t pin_;
};

}  // namespace

int main() {
    section("one physical tap is exactly one press + one release");
    {
        Bench bench(VM_BOOT_BUTTON_PIN);
        uint32_t now = 1000;
        bench.begin(now, kHigh);

        bench.tickAt(now);
        now += 5;
        bench.tickAt(now);
        check("no edge while the button is untouched",
              !bench.takePressed() && !bench.takeReleased());

        // A tap shorter than the debounce window is ignored entirely: the
        // firmware never sees a start whose end it cannot also see.
        bench.setLevel(kLow);
        bench.tickAt(now);
        now += 5;
        bench.tickAt(now);
        bench.setLevel(kHigh);
        bench.tickAt(now);
        now += 100;
        bench.tickAt(now);
        check("a sub-debounce tap produces no edge at all",
              !bench.takePressed() && !bench.takeReleased());

        bench.pressDown(now);
        check("a real press delivers exactly one Pressed edge", bench.takePressed());
        check("and no Released edge before the finger is lifted", !bench.takeReleased());
        bench.liftUp(now);
        check("lifting the finger delivers exactly one Released edge", bench.takeReleased());
        check("and no second Pressed edge from the same tap", !bench.takePressed());
        check("and no second Released edge", !bench.takeReleased());
    }

    section("holding the button does not repeat");
    {
        Bench bench(VM_BOOT_BUTTON_PIN);
        uint32_t now = 2000;
        bench.begin(now, kHigh);

        bench.setLevel(kLow);
        bench.tickAt(now);
        now += VM_BUTTON_DEBOUNCE_MS + 1;
        bench.tickAt(now);
        check("Pressed edge on the way down", bench.takePressed());

        // Keep it down for two simulated seconds, sampled every 20 ms like the
        // main loop does.
        uint32_t repeats = 0;
        for (int i = 0; i < 100; ++i) {
            now += 20;
            bench.tickAt(now);
            if (bench.takePressed()) {
                ++repeats;
            }
        }
        checkU32("extra Pressed edges while held", repeats, 0);
        check("the button still reports as held", bench.isPressed());
    }

    section("contact bounce inside the window is filtered");
    {
        Bench bench(VM_BOOT_BUTTON_PIN);
        uint32_t now = 3000;
        bench.begin(now, kHigh);

        // Real switches chatter for a few hundred microseconds; the model here is
        // deliberately coarser (a bounce every simulated millisecond) and must
        // still produce a single edge.
        bench.setLevel(kLow);
        bench.tickAt(now);
        for (int i = 0; i < 10; ++i) {
            now += 1;
            bench.setLevel(i % 2 == 0 ? kHigh : kLow);
            bench.tickAt(now);
        }
        bench.setLevel(kLow);
        bench.tickAt(now);
        now += VM_BUTTON_DEBOUNCE_MS + 1;
        bench.tickAt(now);

        check("the settled press delivers exactly one Pressed edge", bench.takePressed());
        check("bounce produced no Released edge", !bench.takeReleased());
    }

    section("toggle: 1st tap -> REC, 2nd tap -> STOP (tests A, D, E)");
    {
        Bench bench(VM_BOOT_BUTTON_PIN);
        ToggleConsumer consumer;
        uint32_t now = 4000;
        bench.begin(now, kHigh);

        // Test D + A: tap and let go. The release is sampled on later ticks and
        // must not stop anything.
        bench.tap(now);
        bench.service(consumer);
        checkU32("one tap started exactly one recording", consumer.starts, 1);
        checkU32("the release did not stop it", consumer.stops, 0);
        check("still RECORDING after the finger is lifted",
              consumer.state == AppState::Recording);

        // The user keeps talking for 10 s with no button activity.
        for (int i = 0; i < 25; ++i) {
            now += 400;
            bench.tickAt(now);
            bench.service(consumer);
        }
        check("still RECORDING 10 s later", consumer.state == AppState::Recording);
        checkU32("with no extra start or stop", consumer.starts + consumer.stops, 1);

        // Second tap: stop (tests A / B).
        bench.tap(now);
        bench.service(consumer);
        checkU32("the second tap stopped it", consumer.stops, 1);
        checkU32("and started nothing new", consumer.starts, 1);
        check("back to IDLE", consumer.state == AppState::Idle);

        // Test E: a third tap starts a brand-new recording normally.
        bench.tap(now);
        bench.service(consumer);
        checkU32("a tap after finishing starts a new recording", consumer.starts, 2);
        checkU32("with no refusals along the way", consumer.refused, 0);
        check("and the new recording is running", consumer.state == AppState::Recording);
    }

    section("consecutive recordings keep toggling (test F)");
    {
        Bench bench(VM_BOOT_BUTTON_PIN);
        ToggleConsumer consumer;
        uint32_t now = 20000;
        bench.begin(now, kHigh);

        // Three complete record/stop cycles: each pair of taps is one note.
        for (int i = 0; i < 3; ++i) {
            bench.tap(now);
            bench.service(consumer);
            bench.tap(now);
            bench.service(consumer);
        }

        checkU32("3 cycles started 3 recordings", consumer.starts, 3);
        checkU32("and stopped 3 of them", consumer.stops, 3);
        checkU32("nothing was refused", consumer.refused, 0);
        check("back to IDLE between notes", consumer.state == AppState::Idle);

        // And the machine is still armed for the next one.
        bench.tap(now);
        bench.service(consumer);
        checkU32("a tap after the cycles starts a 4th recording", consumer.starts, 4);
        check("which is running", consumer.state == AppState::Recording);
        checkU32("with the stop count untouched", consumer.stops, 3);
    }

    section("a press is refused while the WAV is owned elsewhere");
    {
        const AppState owned[] = {AppState::Saving, AppState::Uploading, AppState::RetryWait};
        for (AppState state : owned) {
            Bench bench(VM_BOOT_BUTTON_PIN);
            ToggleConsumer consumer;
            uint32_t now = 30000;
            bench.begin(now, kHigh);
            consumer.state = state;

            bench.tap(now);
            bench.service(consumer);

            checkU32("refused press counted", consumer.refused, 1);
            checkU32("no recording started", consumer.starts, 0);
            check("the state is untouched", consumer.state == state);
        }
    }

    if (g_failures == 0) {
        std::printf("\n%d check(s), all button tests passed\n", g_checks);
        return 0;
    }
    std::printf("\n%d button test(s) FAILED\n", g_failures);
    return 1;
}
