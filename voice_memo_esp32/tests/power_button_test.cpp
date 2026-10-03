// Host-side tests for the PWR key debounce + mandatory boot arming in
// power_button.h.
//
// This is the safety-critical half of the power feature: if the post-boot
// release were read as a "short press", holding PWR to power the board on would
// switch it straight back off the moment the user let go. The rule is therefore
// pure (no Arduino) and driven here millisecond by millisecond:
//
//     c++ -std=c++11 -Wall -Wextra -o /tmp/power_button_test tests/power_button_test.cpp
//     /tmp/power_button_test
//
// tests/ is not compiled into the firmware.

#include <cstdio>
#include <string>
#include <vector>

#include "../power_button.h"

namespace {

using voice_memo_firmware::PowerButton;

constexpr int kLow = 0;   // key pressed: BAT_KEY is shorted to GND
constexpr int kHigh = 1;  // key released: pulled up by R58 + the internal pull-up
constexpr uint32_t kDebounce = 50;

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

void checkPressed(const char* label, bool actual, bool expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s isPressed=%s\n", label, actual ? "true" : "false");
        return;
    }
    std::printf("FAIL %s: expected isPressed=%s, got %s\n",
                label,
                expected ? "true" : "false",
                actual ? "true" : "false");
    ++g_failures;
}

// Counts each event kind so a suite can assert "exactly one shutdown request"
// instead of eyeballing a log.
struct EventCounts {
    uint32_t none = 0;
    uint32_t shortPress = 0;
    uint32_t released = 0;
    uint32_t armed = 0;

    uint32_t shutdownRequests() const { return shortPress; }
};

EventCounts countsOf(const std::vector<PowerButton::Event>& events) {
    EventCounts counts;
    for (PowerButton::Event event : events) {
        switch (event) {
            case PowerButton::Event::None:
                ++counts.none;
                break;
            case PowerButton::Event::ShortPress:
                ++counts.shortPress;
                break;
            case PowerButton::Event::Released:
                ++counts.released;
                break;
            case PowerButton::Event::Armed:
                ++counts.armed;
                break;
        }
    }
    return counts;
}

// Drives the button at a fixed 1 ms cadence, which is what the firmware's main
// loop does, and collects every event.
class Driver {
public:
    explicit Driver(bool activeLow = true, uint32_t debounceMs = kDebounce)
        : button_(activeLow, debounceMs) {}

    // Runs the key for `ms` milliseconds at the given raw level.
    void hold(int level, uint32_t ms) {
        for (uint32_t i = 0; i < ms; ++i) {
            const PowerButton::Event event = button_.update(now_, level);
            if (event != PowerButton::Event::None) {
                events_.push_back(event);
            }
            ++now_;
        }
    }

    // Polls at the level reported by a callback, so noise can be described as a
    // pattern of 1 ms samples.
    void pollPattern(const std::vector<int>& pattern) {
        for (int level : pattern) {
            const PowerButton::Event event = button_.update(now_, level);
            if (event != PowerButton::Event::None) {
                events_.push_back(event);
            }
            ++now_;
        }
    }

    void silence(uint32_t ms) { hold(lastLevel_, ms); }

    uint32_t now() const { return now_; }
    const std::vector<PowerButton::Event>& events() const { return events_; }
    EventCounts counts() const { return countsOf(events_); }
    bool isArmed() const { return button_.isArmed(); }
    bool isPressed() const { return button_.isPressed(); }

private:
    PowerButton button_;
    uint32_t now_ = 0;
    int lastLevel_ = kHigh;
    std::vector<PowerButton::Event> events_;
};

void section(const char* title) {
    std::printf("\n--- %s\n", title);
}

}  // namespace

int main() {
    // -----------------------------------------------------------------------
    // Case 1: boot with PWR already released (USB power, or a second run while
    // the key is not held). The first stable release arms the button and
    // produces no shutdown request.
    // -----------------------------------------------------------------------
    section("case 1: boot with PWR released");
    {
        Driver driver;
        driver.hold(kHigh, 200);
        check("button is armed after a stable release", driver.isArmed());
        checkU32("no shutdown request during arming", driver.counts().shutdownRequests(), 0);
        checkU32("exactly one Armed event", driver.counts().armed, 1);
        checkPressed("not pressed while the key is up", driver.isPressed(), false);
    }

    // -----------------------------------------------------------------------
    // Case 2: boot with PWR held (the battery power-on press). Nothing may be
    // requested while it is held.
    // -----------------------------------------------------------------------
    section("case 2: boot with PWR held down");
    {
        Driver driver;
        driver.hold(kLow, 3000);  // the user keeps holding it after setup()
        check("still disarmed while the key is held", !driver.isArmed());
        checkU32("no shutdown request while held", driver.counts().shutdownRequests(), 0);
        checkU32("no Armed event while held", driver.counts().armed, 0);
        checkPressed("isPressed reflects the held key", driver.isPressed(), true);
    }

    // -----------------------------------------------------------------------
    // Case 3: the power-on press is held through all of setup(), then released.
    // The release ARMS the button - it is never a shutdown request. This is the
    // acceptance criterion A.
    // -----------------------------------------------------------------------
    section("case 3: power-on press held through setup, then released");
    {
        Driver driver;
        driver.hold(kLow, 800);    // held while setup() runs
        driver.hold(kHigh, 200);   // the user lets go
        check("armed by the release", driver.isArmed());
        checkU32("no shutdown request", driver.counts().shutdownRequests(), 0);
        checkU32("exactly one Armed event", driver.counts().armed, 1);
    }

    // -----------------------------------------------------------------------
    // Case 4: release only arms. A separate assertion for the "does not power
    // off" half, kept apart so a regression names the right thing.
    // -----------------------------------------------------------------------
    section("case 4: the arming release is not a shutdown request");
    {
        Driver driver;
        driver.hold(kHigh, 60);   // arming release
        driver.hold(kLow, 200);   // a real press
        driver.hold(kHigh, 200);  // its release
        checkU32("one request from one press/release cycle", driver.counts().shutdownRequests(), 1);
    }

    // -----------------------------------------------------------------------
    // Case 5: a deliberate short press after arming produces exactly one
    // shutdown request. This is acceptance criterion B.
    // -----------------------------------------------------------------------
    section("case 5: short press after arming -> exactly one request");
    {
        Driver driver;
        driver.hold(kHigh, 100);  // arm
        driver.hold(kLow, 150);   // press
        checkU32("the press itself already requests", driver.counts().shutdownRequests(), 1);
        driver.hold(kHigh, 150);  // release
        checkU32("release adds no second request", driver.counts().shutdownRequests(), 1);
        checkU32("one Released event", driver.counts().released, 1);
    }

    // -----------------------------------------------------------------------
    // Case 6: electrical bounce must not produce two requests. Every burst is
    // shorter than the debounce window, which is what a real tactile switch
    // produces.
    // -----------------------------------------------------------------------
    section("case 6: contact bounce does not duplicate the event");
    {
        Driver driver;
        driver.hold(kHigh, 100);  // arm
        // Press with bounce: five 3 ms glitches at the leading edge.
        driver.pollPattern({kLow, kHigh, kLow, kHigh, kLow, kHigh, kLow, kLow, kLow, kLow, kLow});
        driver.hold(kLow, 100);   // settle pressed
        checkU32("one request despite the leading-edge bounce",
                 driver.counts().shutdownRequests(), 1);

        // Release with bounce: four 4 ms glitches at the trailing edge.
        driver.pollPattern({kHigh, kLow, kHigh, kLow, kHigh, kLow, kHigh, kHigh, kHigh, kHigh});
        driver.hold(kHigh, 100);
        checkU32("bounce on release still yields one request",
                 driver.counts().shutdownRequests(), 1);
        checkU32("exactly one Released event", driver.counts().released, 1);
    }

    // -----------------------------------------------------------------------
    // Case 7: a press shorter than the debounce window is not a press at all.
    // -----------------------------------------------------------------------
    section("case 7: press shorter than the debounce window is ignored");
    {
        Driver driver;
        driver.hold(kHigh, 100);  // arm
        driver.hold(kLow, 20);    // deliberate short spike, below kDebounce
        driver.hold(kHigh, 200);
        checkU32("no request from a sub-window spike", driver.counts().shutdownRequests(), 0);
        checkPressed("debounced state never went down", driver.isPressed(), false);
    }

    // -----------------------------------------------------------------------
    // Case 8: a press with no release never powers off prematurely, and neither
    // does holding the key for a long time (case 28 in the specification).
    // -----------------------------------------------------------------------
    section("case 8: press without release does not repeat");
    {
        Driver driver;
        driver.hold(kHigh, 100);  // arm
        driver.hold(kLow, 10000); // held for ten seconds
        checkU32("exactly one request while held", driver.counts().shutdownRequests(), 1);
        checkU32("no Released event while still held", driver.counts().released, 0);
    }

    // -----------------------------------------------------------------------
    // Case 9: two short presses produce two requests and leave the button in a
    // valid, still-armed state (no invalid state, no missed arming).
    // -----------------------------------------------------------------------
    section("case 9: two short presses");
    {
        Driver driver;
        driver.hold(kHigh, 100);  // arm
        driver.hold(kLow, 100);
        driver.hold(kHigh, 100);
        driver.hold(kLow, 100);
        driver.hold(kHigh, 100);
        checkU32("two requests", driver.counts().shutdownRequests(), 2);
        checkU32("two releases", driver.counts().released, 2);
        checkU32("armed exactly once", driver.counts().armed, 1);
        check("still armed afterwards", driver.isArmed());
        checkPressed("released at rest", driver.isPressed(), false);
    }

    // -----------------------------------------------------------------------
    // Case 10: a press that begins while still disarmed is ignored entirely, even
    // if it is long. This is the case that protects a user who holds PWR for a
    // long time to power on and then lets go.
    // -----------------------------------------------------------------------
    section("case 10: long press started while disarmed is ignored");
    {
        Driver driver;
        driver.hold(kLow, 5000);   // disarmed, held
        driver.hold(kHigh, 200);   // released -> arms
        driver.hold(kLow, 5000);   // now a real press
        driver.hold(kHigh, 200);
        checkU32("only the post-arming press counts", driver.counts().shutdownRequests(), 1);
        checkU32("armed exactly once", driver.counts().armed, 1);
    }

    // -----------------------------------------------------------------------
    // Case 11: a glitch during the arming window must not arm on noise.
    // -----------------------------------------------------------------------
    section("case 11: noise does not arm the button");
    {
        Driver driver;
        // Bounce while disarmed: the first sub-window dip must not emit Armed.
        driver.pollPattern({kLow, kHigh, kLow, kHigh, kLow, kHigh});
        checkU32("no Armed event from noise", driver.counts().armed, 0);
        check("not armed by noise", !driver.isArmed());
        driver.hold(kHigh, 200);
        check("armed by the real stable release", driver.isArmed());
    }

    // -----------------------------------------------------------------------
    // Case 12: millis() wrap during a debounce window must not create or lose an
    // event, and must not make the arming window appear already elapsed.
    // -----------------------------------------------------------------------
    section("case 12: millis() wrap across the debounce window");
    {
        PowerButton button(true, kDebounce);
        uint32_t now = 0xFFFFFFF0U;  // 16 ms before the wrap
        std::vector<PowerButton::Event> events;
        const auto poll = [&](int level, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i) {
                const PowerButton::Event event = button.update(now, level);
                if (event != PowerButton::Event::None) {
                    events.push_back(event);
                }
                now += 1;  // wraps through 0 naturally
            }
        };
        poll(kHigh, 200);  // arms before the wrap
        check("armed before the wrap", button.isArmed());
        poll(kLow, 100);   // a press that spans the wrap
        poll(kHigh, 100);
        const EventCounts counts = countsOf(events);
        checkU32("one request across the wrap", counts.shutdownRequests(), 1);
        checkU32("one release across the wrap", counts.released, 1);
    }

    // -----------------------------------------------------------------------
    // Case 13: the active-low assumption is explicit, and an active-high build
    // (VM_PWR_KEY_ACTIVE_LOW 0) behaves consistently if the polarity were ever
    // disproved on hardware.
    // -----------------------------------------------------------------------
    section("case 13: polarity is parameterised, not hard-coded");
    {
        // The debouncer's `pressed` means "the key reads the ACTIVE level", so
        // the raw levels are polarity-relative: an active-high key rests at LOW
        // and is pressed at HIGH, which is the mirror of the active-low key the
        // board actually has.
        Driver activeHigh(false);
        activeHigh.hold(kLow, 200);   // released for an active-high key
        check("arms on the released level", activeHigh.isArmed());
        activeHigh.hold(kHigh, 150);  // pressed for an active-high key
        activeHigh.hold(kLow, 150);
        checkU32("one request", activeHigh.counts().shutdownRequests(), 1);
        // The same raw sequence must NOT arm an active-low driver, which is what
        // proves the level is interpreted through the polarity rather than
        // hard-coded.
        Driver activeLow;
        activeLow.hold(kHigh, 200);
        check("an active-low driver arms on the opposite level",
              activeLow.isArmed() && activeHigh.isArmed());
        Driver activeLowSwapped;
        activeLowSwapped.hold(kLow, 200);
        check("an active-low driver does not arm on the active-high idle level",
              !activeLowSwapped.isArmed());
    }

    // -----------------------------------------------------------------------
    // Case 14: a zero debounce window is clamped, so a single-sample spike can
    // never be treated as a stable level.
    // -----------------------------------------------------------------------
    section("case 14: a zero debounce window is clamped to one millisecond");
    {
        Driver driver(true, 0);
        driver.hold(kHigh, 50);
        check("arms with a clamped window", driver.isArmed());
        checkU32("no request yet", driver.counts().shutdownRequests(), 0);
    }

    std::printf("\n=========================================\n");
    std::printf("%d check(s), %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
