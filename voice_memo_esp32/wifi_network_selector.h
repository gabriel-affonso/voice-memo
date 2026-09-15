#pragma once

// Pure, Arduino-free connect state machine for the ordered list of known Wi-Fi
// networks.
//
// Same idea as recording_state.h / ingest_target.h / time_zone.h: the decision
// rules live in a header with no Arduino, Wi-Fi or FreeRTOS dependency, so the
// exact connect / fail-over / retry timeline can be exercised on the host:
//
//     c++ -std=c++11 -Wall -Wextra -o /tmp/wifi_fallback_test tests/wifi_fallback_test.cpp
//     /tmp/wifi_fallback_test
//
// wifi_manager.cpp only adapts the real WiFi driver (WiFi.mode / WiFi.begin /
// WiFi.disconnect / WiFi.status / WiFi.localIP) to this selector and performs
// the logging.
//
// ============================================================================
// BUG 1: A DRIVER FAILURE STATUS IS NOT THE END OF AN ATTEMPT
// ============================================================================
// The first version classified WL_NO_SSID_AVAIL and WL_CONNECT_FAILED as
// terminal ("this network will not come up") and abandoned the attempt after a
// 1 s settle constant (VM_WIFI_FAIL_SETTLE_MS, since removed), then called
// WiFi.begin() with the *other* network.
//
// In arduino-esp32 3.3.11 those two statuses are NOT terminal: STA.cpp maps
// WIFI_REASON_NO_AP_FOUND -> WL_NO_SSID_AVAIL and WIFI_REASON_ASSOC_FAIL /
// AUTH_FAIL -> WL_CONNECT_FAILED, lists both in _is_staReconnectableReason()
// and, with autoReconnect enabled by default, retries the SAME network itself.
// They are ordinary transient states of an in-flight association.
//
// The rule below is the fix: ONLY an established connection ends an attempt
// early. A driver failure status never does; the attempt simply runs out its
// full window, so the credentials are not rewritten under the driver.
//
// ============================================================================
// BUG 2: TWO COMPETING RECONNECT MACHINES, AND A CONFIG WRITTEN MID-CONNECT
// ============================================================================
// Even with the full attempt window, the previous version did this:
//
//     timeout -> WiFi.begin(otherSsid, otherPassword)
//
// in the very same loop() iteration. That is racy by construction:
//
//   * STAClass::_autoReconnect is true by default, so the core runs its own
//     reconnect loop for the network it is already trying (:STA.cpp
//     _is_staReconnectableReason() -> disconnect() + connect());
//   * WiFi.begin() -> STAClass::connect() -> esp_wifi_set_config() +
//     esp_wifi_connect();
//   * esp_wifi_set_config() REFUSES to touch the configuration while the
//     station is still connecting. The driver prints
//     "wifi:sta is connecting, cannot set config" and the new SSID is never
//     installed - the station stays on the old, failing network.
//
// The selector is the single reconnect authority (the manager disables the
// core's autoReconnect), and every switch is split into two explicit phases:
//
//     TryingPrimary
//       -> timeout
//       -> DisconnectingForSwitch   (one disconnect request, no begin)
//       -> the driver confirms it left the connecting state
//       -> TryingFallback           (exactly one begin)
//       -> timeout
//       -> DisconnectingForSwitch
//       -> RetryWait
//
// `disconnectObserved` is the completion signal: the manager raises it when it
// sees ARDUINO_EVENT_WIFI_STA_DISCONNECTED after the disconnect request (or
// when the safety deadline expires). A begin is structurally impossible while
// the state is DisconnectingForSwitch.
// ============================================================================

#include <cstddef>

namespace voice_memo_firmware {

// Sentinel for "no entry".
constexpr std::size_t kNoNetworkIndex = static_cast<std::size_t>(-1);

// One state per phase of an attempt. WiFi.begin() is issued only on a
// transition INTO TryingPrimary / TryingFallback, never from a state that is
// already waiting and never from DisconnectingForSwitch, so the driver is
// never reconfigured while an attempt is in flight.
enum class WifiConnectState {
    // No attempt in flight: either connected, or nothing configured.
    Idle,
    // Primary network (usable entry 0) is being attempted.
    TryingPrimary,
    // A fallback entry is being attempted.
    TryingFallback,
    // A timed-out attempt is being abandoned. The station was asked to
    // disconnect and the selector is waiting for the driver to actually leave
    // the connecting state before anything else is configured.
    DisconnectingForSwitch,
    // Every known network timed out; waiting before restarting at the primary.
    RetryWait,
};

// Short name for diagnostic logs. Never returns null.
inline const char* wifi_connect_state_name(WifiConnectState state) {
    switch (state) {
        case WifiConnectState::Idle:
            return "idle";
        case WifiConnectState::TryingPrimary:
            return "trying_primary";
        case WifiConnectState::TryingFallback:
            return "trying_fallback";
        case WifiConnectState::DisconnectingForSwitch:
            return "disconnecting";
        case WifiConnectState::RetryWait:
            return "retry_wait";
    }
    return "unknown";
}

// The only reasons WifiManager may touch the driver.
enum class WifiCommand {
    None,
    // Exactly one WiFi.begin() for usable entry 0.
    BeginPrimary,
    // Exactly one WiFi.begin() for another usable entry.
    BeginFallback,
    // One explicit, non-blocking disconnect of the station
    // (WiFi.disconnect(false, false, 0): radio on, no NVS erase, no wait).
    DisconnectStation,
};

// Transitions worth one log line each.
struct WifiEvents {
    // The primary attempt ran out its whole window.
    bool primaryTimeout = false;
    // A fallback attempt ran out its whole window.
    bool fallbackTimeout = false;
    // The last known network timed out: going offline (entering RetryWait).
    bool noNetworkAvailable = false;
    // RetryWait expired and the whole list is being tried again from the top.
    bool retrying = false;
    // A timed-out attempt is being abandoned: DisconnectStation was issued.
    bool switchStarted = false;
    // True when switchStarted refers to the primary attempt (log wording).
    bool switchFromPrimary = false;
    // The driver left the connecting state: the next decision may configure the
    // station again. Set when the switch completes.
    bool stationIdle = false;
};

struct WifiDecision {
    WifiCommand command = WifiCommand::None;
    WifiEvents events;
    WifiConnectState state = WifiConnectState::Idle;
    // Usable index the begin command refers to, or kNoNetworkIndex.
    std::size_t index = kNoNetworkIndex;
};

class WifiNetworkSelector {
public:
    WifiNetworkSelector(std::size_t networkCount, unsigned long attemptMs, unsigned long retryWaitMs, unsigned long disconnectMs)
        : networkCount_(networkCount), attemptMs_(attemptMs), retryWaitMs_(retryWaitMs), disconnectMs_(disconnectMs) {}

    // Non-blocking; call once per loop() iteration.
    //
    // `connected` must be exactly (WiFi.status() == WL_CONNECTED). No other
    // driver status is an input: a failure status must never shorten an
    // attempt, for the reason documented at the top of this file.
    //
    // `disconnectObserved` must be true once the driver has confirmed it is out
    // of the connecting state after the DisconnectStation command (the manager
    // raises it from ARDUINO_EVENT_WIFI_STA_DISCONNECTED). It is only consulted
    // in DisconnectingForSwitch.
    WifiDecision update(bool connected, bool disconnectObserved, unsigned long now) {
        WifiDecision decision;

        if (connected) {
            // Connected is final: no rescan, no switch back to the primary, so
            // an upload over the fallback is never interrupted.
            state_ = WifiConnectState::Idle;
            index_ = kNoNetworkIndex;
            pending_ = PendingAction::None;
            pendingIndex_ = kNoNetworkIndex;
            decision.state = state_;
            return decision;
        }

        if (networkCount_ == 0) {
            // TEST_A / no credentials configured on purpose.
            decision.state = state_;
            return decision;
        }

        switch (state_) {
            case WifiConnectState::Idle:
                startPrimary(now, decision);
                break;

            case WifiConnectState::TryingPrimary:
                if (elapsed(now) >= attemptMs_) {
                    decision.events.primaryTimeout = true;
                    requestSwitch(/*fromPrimary=*/true, 1, networkCount_ > 1, now, decision);
                }
                break;

            case WifiConnectState::TryingFallback: {
                if (elapsed(now) >= attemptMs_) {
                    decision.events.fallbackTimeout = true;
                    const std::size_t next = index_ + 1;
                    requestSwitch(/*fromPrimary=*/false, next, next < networkCount_, now, decision);
                }
                break;
            }

            case WifiConnectState::DisconnectingForSwitch:
                // The begin for the next network is emitted from here, and only
                // here, after the driver confirmed it stopped connecting.
                if (disconnectObserved || elapsed(now) >= disconnectMs_) {
                    decision.events.stationIdle = true;
                    completeSwitch(now, decision);
                }
                break;

            case WifiConnectState::RetryWait:
                if (elapsed(now) >= retryWaitMs_) {
                    decision.events.retrying = true;
                    startPrimary(now, decision);
                }
                break;
        }

        decision.state = state_;
        return decision;
    }

    WifiConnectState state() const { return state_; }
    std::size_t networkCount() const { return networkCount_; }
    // Usable index currently being attempted, or kNoNetworkIndex.
    std::size_t index() const { return index_; }
    // True while a switch is waiting for the driver to leave the connecting
    // state. No begin may be issued in this state.
    bool disconnecting() const { return state_ == WifiConnectState::DisconnectingForSwitch; }

private:
    // What DisconnectingForSwitch has to do once the station is idle.
    enum class PendingAction {
        None,
        BeginNetwork,
        EnterRetryWait,
    };

    void startPrimary(unsigned long now, WifiDecision& decision) {
        index_ = 0;
        state_ = WifiConnectState::TryingPrimary;
        pending_ = PendingAction::None;
        pendingIndex_ = kNoNetworkIndex;
        phaseStartedMs_ = now;
        decision.command = WifiCommand::BeginPrimary;
        decision.index = index_;
    }

    void startFallback(std::size_t index, unsigned long now, WifiDecision& decision) {
        index_ = index;
        state_ = WifiConnectState::TryingFallback;
        pending_ = PendingAction::None;
        pendingIndex_ = kNoNetworkIndex;
        phaseStartedMs_ = now;
        decision.command = WifiCommand::BeginFallback;
        decision.index = index_;
    }

    // Leaves the current attempt and asks for an explicit disconnect. Nothing
    // is configured here: the begin (if any) waits for the station to be idle.
    void requestSwitch(bool fromPrimary, std::size_t nextIndex, bool hasNext, unsigned long now, WifiDecision& decision) {
        state_ = WifiConnectState::DisconnectingForSwitch;
        index_ = kNoNetworkIndex;
        pending_ = hasNext ? PendingAction::BeginNetwork : PendingAction::EnterRetryWait;
        pendingIndex_ = hasNext ? nextIndex : kNoNetworkIndex;
        phaseStartedMs_ = now;
        decision.command = WifiCommand::DisconnectStation;
        decision.events.switchStarted = true;
        decision.events.switchFromPrimary = fromPrimary;
    }

    void completeSwitch(unsigned long now, WifiDecision& decision) {
        switch (pending_) {
            case PendingAction::BeginNetwork:
                if (pendingIndex_ == 0) {
                    startPrimary(now, decision);
                } else {
                    startFallback(pendingIndex_, now, decision);
                }
                break;

            case PendingAction::EnterRetryWait:
            case PendingAction::None:
                enterRetryWait(now, decision);
                break;
        }
    }

    void enterRetryWait(unsigned long now, WifiDecision& decision) {
        state_ = WifiConnectState::RetryWait;
        index_ = kNoNetworkIndex;
        pending_ = PendingAction::None;
        pendingIndex_ = kNoNetworkIndex;
        phaseStartedMs_ = now;
        decision.events.noNetworkAvailable = true;
    }

    // Wrap-safe: unsigned arithmetic, so a millis() overflow is harmless.
    unsigned long elapsed(unsigned long now) const { return now - phaseStartedMs_; }

    std::size_t networkCount_;
    unsigned long attemptMs_;
    unsigned long retryWaitMs_;
    unsigned long disconnectMs_;

    WifiConnectState state_ = WifiConnectState::Idle;
    std::size_t index_ = kNoNetworkIndex;
    PendingAction pending_ = PendingAction::None;
    std::size_t pendingIndex_ = kNoNetworkIndex;
    unsigned long phaseStartedMs_ = 0;
};

}  // namespace voice_memo_firmware
