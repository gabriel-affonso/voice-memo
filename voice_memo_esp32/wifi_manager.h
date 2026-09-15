#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include <atomic>

#include "wifi_network_selector.h"

// One entry of the ordered list of known Wi-Fi networks. The order is the
// fail-over order: usable entry 0 is the primary network, every later usable
// entry is a fallback tried in turn. Both pointers are borrowed, never freed,
// and normally point at the string literals defined in the local secrets.h.
struct WifiNetwork {
    const char* ssid;
    const char* password;
};

// Non-blocking, state-oriented Wi-Fi manager for an ordered list of known
// networks, and the ONLY reconnect authority for the station.
//
// begin() runs once from setup(): it selects WIFI_STA, calls
// WiFi.setAutoReconnect(false) so the core cannot run a second reconnect
// machine against the same SSID, and subscribes to
// ARDUINO_EVENT_WIFI_STA_DISCONNECTED for the real disconnect reason.
//
// ensureConnected() is called once per loop() iteration (VM_ENABLE_UPLOAD) and
// never blocks: no delay(), no wait loop, no blocking disconnect(). The connect
// state machine lives in wifi_network_selector.h; this class adapts the WiFi
// driver to it and performs the logging.
//
// Hard rules:
//  * WiFi.begin() is called exactly once per state transition into
//    TryingPrimary / TryingFallback, and NEVER while the selector is in
//    DisconnectingForSwitch;
//  * switching networks is two-phase: WiFi.disconnect(false, false, 0) first
//    (radio stays on, no NVS erase, no blocking wait), then wait for the driver
//    to confirm it left the connecting state, then exactly one WiFi.begin().
//    This is what makes "sta is connecting, cannot set config" impossible;
//  * while WiFi.status() == WL_CONNECTED no other network is started and the
//    selector stays Idle, so an in-flight upload over the fallback is never
//    interrupted.
//
// Passwords and tokens are never printed. SSIDs are printed, because telling
// primary from fallback is the whole point of the fail-over logs.
class WifiManager {
public:
    WifiManager(const WifiNetwork* networks, size_t count);

    // One-time station setup. Safe to call more than once; non-blocking.
    void begin();

    bool ensureConnected();
    bool isConnected() const;

    // Entries with a non-empty SSID. 0 in TEST_A or when nothing is configured.
    size_t usableNetworkCount() const;
    // Array slot of the n-th usable entry, or -1 when out of range.
    int slotForUsable(size_t usableIndex) const;

    // False once begin() disabled the core's internal reconnect (regression
    // check: the core must not compete with the multi-SSID selector).
    bool autoReconnectDisabled() const;

private:
    const WifiNetwork* networks_;
    size_t count_;
    voice_memo_firmware::WifiNetworkSelector selector_;

    // Observability only: the connected transition is reported once, and the
    // credentials are never printed.
    bool wasConnected_ = false;
    bool began_ = false;

    // Incremented from the Arduino network event task on every
    // ARDUINO_EVENT_WIFI_STA_DISCONNECTED, read from loop(). The baseline is
    // snapshotted just before the explicit disconnect, so only an event that
    // arrives AFTER the request counts as the completion signal.
    std::atomic<uint32_t> disconnectEvents_{0};
    uint32_t disconnectBaseline_ = 0;

    void applyDecision(const voice_memo_firmware::WifiDecision& decision, wl_status_t status);
    void onStaDisconnected(const wifi_event_sta_disconnected_t& info);
};
