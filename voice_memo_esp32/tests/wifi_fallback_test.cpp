// Host-side test for the Wi-Fi connect/fail-over state machine in
// wifi_network_selector.h.
//
// wifi_network_selector.h has no Arduino/Wi-Fi/FreeRTOS dependency on purpose,
// so the exact timeline the firmware runs - one WiFi.begin() per attempt, a
// full attempt window per network, then an explicit disconnect before ANY other
// network is configured - is verified here without a board:
//
//     c++ -std=c++11 -Wall -Wextra -o /tmp/wifi_fallback_test tests/wifi_fallback_test.cpp
//     /tmp/wifi_fallback_test
//
// This directory is not compiled into the firmware: Arduino only builds the
// sketch root and src/, so tests/ is ignored by arduino-cli.
//
// REGRESSIONS UNDER TEST
// ----------------------
// BUG 1 - A DRIVER FAILURE STATUS IS NOT THE END OF AN ATTEMPT.
// The first fail-over implementation treated WL_NO_SSID_AVAIL /
// WL_CONNECT_FAILED as terminal and abandoned the primary after 1 s, then
// called WiFi.begin() with the fallback. In arduino-esp32 3.3.11 those statuses
// are reconnectable: the core retries the same network itself, and a new
// WiFi.begin() overwrites the stored credentials. The cases below therefore
// assert that a non-connected status NEVER produces a command inside an attempt
// window, and that WiFi.begin() is issued exactly once per attempt.
//
// BUG 2 - TWO COMPETING RECONNECT MACHINES, AND A CONFIG WRITTEN MID-CONNECT.
// The core's autoReconnect (true by default) kept retrying the old SSID while
// the selector called WiFi.begin(otherSsid). WiFi.begin() runs
// esp_wifi_set_config(), which refuses to change the configuration while the
// station is still connecting ("sta is connecting, cannot set config"), so the
// new SSID was never installed. The selector now switches in two phases:
// timeout -> DisconnectStation -> wait for the driver to leave the connecting
// state -> exactly one WiFi.begin(). Cases 1-4 and 8 pin that sequence down.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "../wifi_network_selector.h"

namespace {

using voice_memo_firmware::WifiCommand;
using voice_memo_firmware::WifiConnectState;
using voice_memo_firmware::WifiDecision;
using voice_memo_firmware::WifiNetworkSelector;
using voice_memo_firmware::wifi_connect_state_name;
using voice_memo_firmware::kNoNetworkIndex;

// Mirror config.h (VM_WIFI_CONNECT_TIMEOUT_MS, VM_WIFI_RETRY_INTERVAL_MS,
// VM_WIFI_DISCONNECT_TIMEOUT_MS). Literals so the test documents the timeline
// it asserts.
const unsigned long kAttemptMs = 10000;
const unsigned long kRetryWaitMs = 10000;
const unsigned long kDisconnectMs = 1500;

int g_failures = 0;
int g_checks = 0;

const char* commandName(WifiCommand command) {
    switch (command) {
        case WifiCommand::None:
            return "none";
        case WifiCommand::BeginPrimary:
            return "begin_primary";
        case WifiCommand::BeginFallback:
            return "begin_fallback";
        case WifiCommand::DisconnectStation:
            return "disconnect_station";
    }
    return "unknown";
}

void section(const char* title) {
    std::printf("\n--- %s\n", title);
}

void checkCommand(const char* label, const WifiDecision& actual, WifiCommand expected) {
    ++g_checks;
    if (actual.command == expected) {
        std::printf("PASS %s -> %s\n", label, commandName(actual.command));
        return;
    }
    std::printf("FAIL %s: expected %s, got %s\n",
                label, commandName(expected), commandName(actual.command));
    ++g_failures;
}

void checkState(const char* label, WifiConnectState actual, WifiConnectState expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s -> %s\n", label, wifi_connect_state_name(actual));
        return;
    }
    std::printf("FAIL %s: expected %s, got %s\n",
                label, wifi_connect_state_name(expected), wifi_connect_state_name(actual));
    ++g_failures;
}

void checkIndex(const char* label, const WifiDecision& actual, std::size_t expected) {
    ++g_checks;
    if (actual.index == expected) {
        std::printf("PASS %s -> index=%u\n", label, static_cast<unsigned>(expected));
        return;
    }
    std::printf("FAIL %s: expected index=%u, got %u\n",
                label,
                static_cast<unsigned>(expected),
                static_cast<unsigned>(actual.index));
    ++g_failures;
}

void checkBool(const char* label, bool actual, bool expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s = %s\n", label, actual ? "true" : "false");
        return;
    }
    std::printf("FAIL %s: expected %s, got %s\n",
                label, expected ? "true" : "false", actual ? "true" : "false");
    ++g_failures;
}

void checkCount(const char* label, int actual, int expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s = %d\n", label, expected);
        return;
    }
    std::printf("FAIL %s: expected %d, got %d\n", label, expected, actual);
    ++g_failures;
}

bool isBegin(WifiCommand command) {
    return command == WifiCommand::BeginPrimary || command == WifiCommand::BeginFallback;
}

// Everything the manager would do, counted over a timeline, plus the
// architecture invariants that must never be violated.
struct Tally {
    int beginPrimary = 0;
    int beginFallback = 0;
    int disconnects = 0;
    int primaryTimeouts = 0;
    int fallbackTimeouts = 0;
    int noNetworkAvailable = 0;
    int retrying = 0;

    // Invariant violations: all of these MUST stay 0.
    int beginsWhileDisconnecting = 0;
    int beginsWithoutEntry = 0;
    int entriesWithoutBegin = 0;
    int disconnectsOutsideTimeout = 0;
    int duplicateDisconnects = 0;
    int commandsWhileWaitingInDisconnect = 0;
    int commandsWhileConnected = 0;
    int commandsWhileRetryWaiting = 0;

    int commands() const { return beginPrimary + beginFallback + disconnects; }
};

// Drives a selector exactly like WifiManager does: one update per loop()
// iteration, with `disconnectObserved` raised only after the DisconnectStation
// command was issued (that is what the ARDUINO_EVENT_WIFI_STA_DISCONNECTED
// listener in wifi_manager.cpp provides).
class Harness {
public:
    explicit Harness(std::size_t networks)
        : selector_(networks, kAttemptMs, kRetryWaitMs, kDisconnectMs) {}

    WifiDecision step(bool connected, bool disconnectObserved, unsigned long now, Tally& t) {
        const WifiConnectState before = selector_.state();
        const WifiDecision d = selector_.update(connected, disconnectObserved, now);

        const bool begin = isBegin(d.command);
        const bool enteredTrying =
            (d.state == WifiConnectState::TryingPrimary || d.state == WifiConnectState::TryingFallback) && before != d.state;

        if (d.command == WifiCommand::BeginPrimary) {
            ++t.beginPrimary;
        } else if (d.command == WifiCommand::BeginFallback) {
            ++t.beginFallback;
        } else if (d.command == WifiCommand::DisconnectStation) {
            ++t.disconnects;
        }
        if (d.events.primaryTimeout) {
            ++t.primaryTimeouts;
        }
        if (d.events.fallbackTimeout) {
            ++t.fallbackTimeouts;
        }
        if (d.events.noNetworkAvailable) {
            ++t.noNetworkAvailable;
        }
        if (d.events.retrying) {
            ++t.retrying;
        }

        // ---- invariants -------------------------------------------------
        if (begin && d.state == WifiConnectState::DisconnectingForSwitch) {
            ++t.beginsWhileDisconnecting;
        }
        if (begin && !enteredTrying) {
            ++t.beginsWithoutEntry;
        }
        if (enteredTrying) {
            if (!begin) {
                ++t.entriesWithoutBegin;
            }
        }
        if (d.command == WifiCommand::DisconnectStation) {
            if (before != WifiConnectState::TryingPrimary && before != WifiConnectState::TryingFallback) {
                ++t.disconnectsOutsideTimeout;
            }
            if (before == WifiConnectState::DisconnectingForSwitch) {
                ++t.duplicateDisconnects;
            }
            switchStartedAt_ = now;
        }
        // Regression 1: while the selector is waiting for the driver to leave
        // the connecting state (no completion signal yet, safety deadline not
        // reached) no command of any kind may be issued.
        if (before == WifiConnectState::DisconnectingForSwitch && !disconnectObserved
            && (now - switchStartedAt_) < kDisconnectMs && d.command != WifiCommand::None) {
            ++t.commandsWhileWaitingInDisconnect;
        }
        if (before == WifiConnectState::RetryWait && d.command != WifiCommand::None && !d.events.retrying) {
            ++t.commandsWhileRetryWaiting;
        }
        if (connected && d.command != WifiCommand::None) {
            ++t.commandsWhileConnected;
        }
        return d;
    }

    WifiConnectState state() const { return selector_.state(); }
    bool disconnecting() const { return selector_.disconnecting(); }

private:
    WifiNetworkSelector selector_;
    unsigned long switchStartedAt_ = 0;
};

void checkInvariants(const char* label, const Tally& t) {
    checkCount((std::string(label) + ": no begin while disconnecting").c_str(), t.beginsWhileDisconnecting, 0);
    checkCount((std::string(label) + ": every begin is an entry transition").c_str(), t.beginsWithoutEntry, 0);
    checkCount((std::string(label) + ": every entry transition has exactly one begin").c_str(), t.entriesWithoutBegin, 0);
    checkCount((std::string(label) + ": disconnect only after a timeout").c_str(), t.disconnectsOutsideTimeout, 0);
    checkCount((std::string(label) + ": no duplicate disconnect").c_str(), t.duplicateDisconnects, 0);
    checkCount((std::string(label) + ": no command while waiting for the disconnect").c_str(),
               t.commandsWhileWaitingInDisconnect, 0);
    checkCount((std::string(label) + ": no command while online").c_str(), t.commandsWhileConnected, 0);
    checkCount((std::string(label) + ": no stray command in retry_wait").c_str(), t.commandsWhileRetryWaiting, 0);
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path.c_str());
    if (!in) {
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

int countOccurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    int count = 0;
    std::string::size_type pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

}  // namespace

int main() {
    // ---------------------------------------------------------------------
    section("regression 3: primary timeout -> Disconnecting -> only after the driver is idle -> fallback");
    {
        Harness h(2);
        Tally t;

        WifiDecision d = h.step(false, false, 0, t);
        checkCommand("first update starts the primary once", d, WifiCommand::BeginPrimary);
        checkState("state is trying_primary", d.state, WifiConnectState::TryingPrimary);

        // Regression guard: loop() keeps calling update() every few ms while
        // the association is in flight. None of these may produce a command.
        const int commandsAfterStart = t.commands();
        for (unsigned long now = 1; now < kAttemptMs; ++now) {
            h.step(false, false, now, t);
        }
        checkCount("no command inside the primary window", t.commands() - commandsAfterStart, 0);

        d = h.step(false, false, kAttemptMs, t);
        checkBool("primary is reported timed out", d.events.primaryTimeout, true);
        checkCommand("the timeout asks for an explicit disconnect, not a begin", d, WifiCommand::DisconnectStation);
        checkState("state is disconnecting", d.state, WifiConnectState::DisconnectingForSwitch);
        checkBool("no fallback was started yet", isBegin(d.command), false);

        // Two loop() passes can share one millisecond; what matters is that the
        // completion flag can only be raised by a later pass, after the
        // ARDUINO_EVENT_WIFI_STA_DISCONNECTED listener in wifi_manager.cpp saw
        // the disconnect.
        d = h.step(false, true, kAttemptMs, t);
        checkBool("driver reported idle", d.events.stationIdle, true);
        checkCommand("only now is the fallback started, exactly once", d, WifiCommand::BeginFallback);
        checkState("state is trying_fallback", d.state, WifiConnectState::TryingFallback);
        checkIndex("fallback is usable entry 1", d, 1);

        checkInvariants("primary timeout", t);
    }

    // ---------------------------------------------------------------------
    section("regression 3b: with no disconnect event at all the switch still completes at the safety deadline");
    {
        Harness h(2);
        Tally t;
        h.step(false, false, 0, t);
        WifiDecision d = h.step(false, false, kAttemptMs, t);
        checkCommand("the timeout disconnects", d, WifiCommand::DisconnectStation);

        int waitingCommands = 0;
        bool stayedDisconnecting = true;
        for (unsigned long now = kAttemptMs + 1; now < kAttemptMs + kDisconnectMs; ++now) {
            d = h.step(false, false, now, t);
            if (d.command != WifiCommand::None) {
                ++waitingCommands;
            }
            if (d.state != WifiConnectState::DisconnectingForSwitch) {
                stayedDisconnecting = false;
            }
        }
        checkCount("no command while the driver is still connecting", waitingCommands, 0);
        checkBool("the selector stays in disconnecting until the deadline", stayedDisconnecting, true);

        d = h.step(false, false, kAttemptMs + kDisconnectMs, t);
        checkBool("the safety deadline reports the station idle", d.events.stationIdle, true);
        checkCommand("the fallback starts once even without the event", d, WifiCommand::BeginFallback);
        checkInvariants("safety deadline", t);
    }

    // ---------------------------------------------------------------------
    section("regression 4: fallback timeout -> Disconnecting -> RetryWait");
    {
        Harness h(2);
        Tally t;
        h.step(false, false, 0, t);
        h.step(false, false, kAttemptMs, t);                     // disconnect
        WifiDecision d = h.step(false, true, kAttemptMs, t);      // fallback begins
        checkCommand("fallback started", d, WifiCommand::BeginFallback);

        // Nothing may be re-issued inside the fallback window.
        const int beginsBeforeWindow = t.beginPrimary + t.beginFallback;
        for (unsigned long now = kAttemptMs + 1; now < kAttemptMs * 2; ++now) {
            h.step(false, false, now, t);
        }
        checkCount("no command inside the fallback window", t.beginPrimary + t.beginFallback, beginsBeforeWindow);

        d = h.step(false, false, kAttemptMs * 2, t);
        checkBool("fallback is reported timed out", d.events.fallbackTimeout, true);
        checkCommand("fallback timeout disconnects first", d, WifiCommand::DisconnectStation);
        checkState("state is disconnecting", d.state, WifiConnectState::DisconnectingForSwitch);

        d = h.step(false, true, kAttemptMs * 2, t);
        checkBool("driver reported idle", d.events.stationIdle, true);
        checkBool("list reported unavailable", d.events.noNetworkAvailable, true);
        checkCommand("nothing to begin while offline", d, WifiCommand::None);
        checkState("state is retry_wait", d.state, WifiConnectState::RetryWait);

        const unsigned long retryAt = kAttemptMs * 2 + kRetryWaitMs;
        checkCommand("still waiting just before the retry",
                     h.step(false, false, retryAt - 1, t), WifiCommand::None);

        d = h.step(false, false, retryAt, t);
        checkBool("retry transition reported", d.events.retrying, true);
        checkCommand("retry starts the primary again", d, WifiCommand::BeginPrimary);
        checkState("state is trying_primary again", d.state, WifiConnectState::TryingPrimary);

        checkInvariants("fallback timeout", t);
    }

    // ---------------------------------------------------------------------
    section("regression 1+2+8: long offline soak - controlled cycles, one begin per entry, no begin while disconnecting");
    {
        // Models the manager faithfully: the disconnect event is observed on
        // the loop() iteration after the DisconnectStation command.
        Harness h(2);
        Tally t;
        bool pendingDisconnect = false;
        int cycles = 0;
        for (unsigned long now = 0; now <= 5 * 60 * 1000UL; now += 10) {
            const WifiDecision d = h.step(false, pendingDisconnect, now, t);
            pendingDisconnect = (d.command == WifiCommand::DisconnectStation);
            if (d.events.noNetworkAvailable) {
                ++cycles;
            }
        }
        checkInvariants("offline soak", t);
        checkBool("at least 8 full offline cycles happened", cycles >= 8, true);
        checkBool("the list is retried automatically", t.retrying >= 7, true);
        // Every timeout asks for exactly one disconnect, and no disconnect is
        // ever issued without one.
        checkCount("one disconnect per timeout", t.disconnects, t.primaryTimeouts + t.fallbackTimeouts);
        checkBool("primary begins match primary timeouts within one live attempt",
                  t.beginPrimary == t.primaryTimeouts || t.beginPrimary == t.primaryTimeouts + 1, true);
        std::printf("     timeline: begin_p=%d begin_f=%d disconnect=%d primary_to=%d fallback_to=%d offline=%d retry=%d cycles=%d\n",
                    t.beginPrimary, t.beginFallback, t.disconnects, t.primaryTimeouts, t.fallbackTimeouts,
                    t.noNetworkAvailable, t.retrying, cycles);
    }

    // ---------------------------------------------------------------------
    section("regression 6: primary available -> connects without ever trying the fallback");
    {
        Harness h(2);
        Tally t;
        WifiDecision d = h.step(false, false, 0, t);
        checkCommand("primary started", d, WifiCommand::BeginPrimary);

        d = h.step(true, false, 4000, t);
        checkCommand("connected -> nothing to do", d, WifiCommand::None);
        checkState("connected -> idle", d.state, WifiConnectState::Idle);
        checkCount("never any fallback begin", t.beginFallback, 0);
        checkCount("never any disconnect", t.disconnects, 0);
        checkInvariants("primary available", t);
    }

    // ---------------------------------------------------------------------
    section("regression 7: primary absent, fallback available -> connects over the fallback");
    {
        Harness h(2);
        Tally t;
        h.step(false, false, 0, t);                              // primary
        h.step(false, false, kAttemptMs, t);                     // disconnect
        WifiDecision d = h.step(false, true, kAttemptMs, t);     // fallback
        checkCommand("fallback started once", d, WifiCommand::BeginFallback);
        checkCount("primary never restarted inside the cycle", t.beginPrimary, 1);

        d = h.step(true, false, kAttemptMs + 3000, t);
        checkCommand("fallback came up -> nothing to do", d, WifiCommand::None);
        checkState("connected -> idle", d.state, WifiConnectState::Idle);
        checkCount("exactly one begin per network", t.beginPrimary + t.beginFallback, 2);
        checkInvariants("fallback available", t);
    }

    // ---------------------------------------------------------------------
    section("regression 9: a network that appears during RetryWait is joined on the next cycle");
    {
        Harness h(2);
        Tally t;
        h.step(false, false, 0, t);
        h.step(false, false, kAttemptMs, t);
        h.step(false, true, kAttemptMs, t);                      // fallback
        h.step(false, false, kAttemptMs * 2, t);                 // disconnect
        WifiDecision d = h.step(false, true, kAttemptMs * 2, t);
        checkState("offline, waiting", d.state, WifiConnectState::RetryWait);

        const unsigned long retryAt = kAttemptMs * 2 + kRetryWaitMs;
        d = h.step(false, false, retryAt, t);
        checkCommand("retry tries the primary first", d, WifiCommand::BeginPrimary);

        d = h.step(true, false, retryAt + 2000, t);
        checkCommand("the network came back -> nothing to do", d, WifiCommand::None);
        checkState("connected -> idle", d.state, WifiConnectState::Idle);
        checkInvariants("retry wait recovery", t);
    }

    // ---------------------------------------------------------------------
    section("regression 10: while connected no begin and no disconnect is ever issued");
    {
        Harness h(2);
        Tally t;
        unsigned long now = 0;
        for (; now < 120000; now += 50) {
            h.step(true, false, now, t);
        }
        checkCount("zero commands over 120 s connected", t.commands(), 0);
        checkState("state stays idle", h.state(), WifiConnectState::Idle);
        checkCount("no begin while online", t.commandsWhileConnected, 0);
        checkInvariants("stays connected", t);

        // Only a real disconnect restarts the list.
        WifiDecision d = h.step(false, false, now, t);
        checkCommand("link lost -> primary restarted once", d, WifiCommand::BeginPrimary);
        checkCommand("not restarted again on the next loop", h.step(false, false, now + 1, t), WifiCommand::None);
        checkCount("exactly one begin after the drop", t.beginPrimary, 1);
    }

    // ---------------------------------------------------------------------
    section("regression 5: the core's autoReconnect is disabled in the adapter");
    {
        // The pure selector cannot see WiFi.h, so this regression is checked
        // against the adapter source. The runtime proof is the boot log line
        // "[wifi] sta ready autoReconnect=0".
        std::string source;
        const char* candidates[] = {"wifi_manager.cpp", "../wifi_manager.cpp", "tests/../wifi_manager.cpp"};
        bool found = false;
        for (std::size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
            if (readFile(candidates[i], source)) {
                found = true;
                break;
            }
        }

        if (!found) {
            std::printf("SKIP wifi_manager.cpp not readable from this working directory\n");
        } else {
            checkBool("WiFi.setAutoReconnect(false) is called",
                      source.find("setAutoReconnect(false)") != std::string::npos, true);
            checkBool("it is verified with WiFi.getAutoReconnect()",
                      source.find("getAutoReconnect()") != std::string::npos, true);
            checkBool("the station is disconnected without blocking or erasing",
                      source.find("WiFi.disconnect(false, false, 0)") != std::string::npos, true);
            checkBool("no NVS erase (eraseAP / erase() / disconnect(true, true))",
                      source.find("eraseAP") == std::string::npos && source.find("disconnect(true") == std::string::npos
                          && source.find(".erase()") == std::string::npos,
                      true);
            checkBool("the radio is never turned off (no WIFI_OFF)", source.find("WIFI_OFF") == std::string::npos, true);
            checkBool("WiFi.begin() has exactly one call site",
                      countOccurrences(source, "WiFi.begin(networks_") == 1, true);
            checkBool("no blocking delay() in the adapter", source.find("delay(") == std::string::npos, true);
        }
    }

    // ---------------------------------------------------------------------
    section("case: a third network is reached through TryingFallback");
    {
        Harness h(3);
        Tally t;
        h.step(false, false, 0, t);                          // primary
        h.step(false, false, kAttemptMs, t);                 // disconnect
        WifiDecision d = h.step(false, true, kAttemptMs, t);
        checkIndex("fallback 1 is usable entry 1", d, 1);

        d = h.step(false, false, kAttemptMs * 2, t);         // fallback 1 timeout
        checkBool("fallback 1 timed out", d.events.fallbackTimeout, true);
        checkCommand("disconnect before fallback 2", d, WifiCommand::DisconnectStation);

        d = h.step(false, true, kAttemptMs * 2, t);
        checkCommand("fallback 2 is tried next", d, WifiCommand::BeginFallback);
        checkIndex("fallback 2 is usable entry 2", d, 2);
        checkBool("list not exhausted yet", d.events.noNetworkAvailable, false);

        d = h.step(false, false, kAttemptMs * 3, t);
        checkBool("fallback 2 timed out", d.events.fallbackTimeout, true);
        checkCommand("disconnect before going offline", d, WifiCommand::DisconnectStation);

        d = h.step(false, true, kAttemptMs * 3, t);
        checkBool("now the list is exhausted", d.events.noNetworkAvailable, true);
        checkState("goes to retry_wait", d.state, WifiConnectState::RetryWait);
        checkInvariants("three networks", t);
    }

    // ---------------------------------------------------------------------
    section("case: a single network goes straight to RetryWait, through a disconnect");
    {
        Harness h(1);
        Tally t;
        WifiDecision d = h.step(false, false, 0, t);
        checkCommand("only entry is the primary", d, WifiCommand::BeginPrimary);

        d = h.step(false, false, kAttemptMs, t);
        checkBool("primary timed out", d.events.primaryTimeout, true);
        checkBool("no fallback exists", d.events.fallbackTimeout, false);
        checkCommand("the station is disconnected before anything else", d, WifiCommand::DisconnectStation);

        d = h.step(false, true, kAttemptMs, t);
        checkBool("list unavailable", d.events.noNetworkAvailable, true);
        checkCommand("no fallback to begin", d, WifiCommand::None);
        checkState("goes to retry_wait", d.state, WifiConnectState::RetryWait);
        checkInvariants("single network", t);
    }

    // ---------------------------------------------------------------------
    section("case: no configured network -> stays quiet");
    {
        Harness h(0);
        Tally t;
        for (unsigned long now = 0; now < 60000; now += 100) {
            h.step(false, false, now, t);
        }
        checkCount("zero commands", t.commands(), 0);
        checkState("state stays idle", h.state(), WifiConnectState::Idle);
    }

    // ---------------------------------------------------------------------
    section("case: millis() wrap-around is handled");
    {
        Harness h(2);
        Tally t;
        const unsigned long start = 0xFFFFFF00UL;
        WifiDecision d = h.step(false, false, start, t);
        checkCommand("primary starts before the wrap", d, WifiCommand::BeginPrimary);

        checkCommand("no command just before the wrap deadline",
                     h.step(false, false, start + kAttemptMs - 1, t), WifiCommand::None);

        d = h.step(false, false, start + kAttemptMs, t);
        checkBool("timeout detected across the wrap", d.events.primaryTimeout, true);
        checkCommand("disconnect before the fallback", d, WifiCommand::DisconnectStation);

        d = h.step(false, true, start + kAttemptMs, t);
        checkCommand("fallback after the wrap", d, WifiCommand::BeginFallback);
        checkInvariants("wrap", t);
    }

    // ---------------------------------------------------------------------
    section("case: state names are usable in logs");
    {
        checkBool("idle name not empty", wifi_connect_state_name(WifiConnectState::Idle)[0] != '\0', true);
        checkBool("trying_primary name is trying_primary",
                  std::string(wifi_connect_state_name(WifiConnectState::TryingPrimary)) == "trying_primary", true);
        checkBool("trying_fallback name is trying_fallback",
                  std::string(wifi_connect_state_name(WifiConnectState::TryingFallback)) == "trying_fallback", true);
        checkBool("disconnecting name not empty",
                  wifi_connect_state_name(WifiConnectState::DisconnectingForSwitch)[0] != '\0', true);
        checkBool("retry_wait name is retry_wait",
                  std::string(wifi_connect_state_name(WifiConnectState::RetryWait)) == "retry_wait", true);
    }

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("all wifi fallback tests passed\n");
        return 0;
    }
    std::printf("%d wifi fallback test(s) FAILED\n", g_failures);
    return 1;
}
