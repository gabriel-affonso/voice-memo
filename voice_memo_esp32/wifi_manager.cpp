#include "wifi_manager.h"

#include "config.h"

namespace {

bool isUsable(const WifiNetwork& network) {
    return network.ssid != nullptr && network.ssid[0] != '\0';
}

// Open networks are allowed: a null password becomes the empty string the
// Arduino API expects, and is never logged either way.
const char* passwordOrEmpty(const WifiNetwork& network) {
    return network.password != nullptr ? network.password : "";
}

size_t countUsableNetworks(const WifiNetwork* networks, size_t count) {
    size_t usable = 0;
    for (size_t i = 0; i < count; ++i) {
        if (isUsable(networks[i])) {
            ++usable;
        }
    }
    return usable;
}

}  // namespace

WifiManager::WifiManager(const WifiNetwork* networks, size_t count)
    : networks_(networks),
      count_(count),
      selector_(countUsableNetworks(networks, count),
                VM_WIFI_CONNECT_TIMEOUT_MS,
                VM_WIFI_RETRY_INTERVAL_MS,
                VM_WIFI_DISCONNECT_TIMEOUT_MS) {}

void WifiManager::begin() {
    if (began_) {
        return;
    }
    began_ = true;

    // Idempotent in the core (WiFiGenericClass::mode returns early when the
    // mode already matches).
    WiFi.mode(WIFI_STA);

    // STAClass::_autoReconnect defaults to true, and the core retries every
    // reconnectable failure itself - for the network it already has configured.
    // That second reconnect machine is what made the multi-SSID switch race:
    // while it is running, esp_wifi_set_config() for the next SSID is refused
    // with "sta is connecting, cannot set config". From here on the selector in
    // wifi_network_selector.h is the only reconnect authority.
    WiFi.setAutoReconnect(false);

    // ARDUINO_EVENT_WIFI_STA_DISCONNECTED is both the completion signal of the
    // explicit disconnect that precedes every network switch and the only place
    // the real reason code (and the SSID that was being attempted) is
    // available. The callback runs on the Arduino network event task.
    WiFiEventFuncCb onDisconnected = [this](arduino_event_id_t, arduino_event_info_t info) {
        onStaDisconnected(info.wifi_sta_disconnected);
    };
    WiFi.onEvent(onDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    if (WiFi.getAutoReconnect()) {
        Serial.println("[wifi] WARNING autoReconnect could not be disabled");
    }
    Serial.printf("[wifi] sta ready autoReconnect=%u\n", WiFi.getAutoReconnect() ? 1u : 0u);
}

void WifiManager::onStaDisconnected(const wifi_event_sta_disconnected_t& info) {
    // Network event task: no driver calls, no blocking, only diagnostics.
    uint8_t length = info.ssid_len;
    if (length > sizeof(info.ssid)) {
        length = sizeof(info.ssid);
    }
    // The event SSID is not guaranteed to be NUL-terminated.
    char ssid[33];
    size_t copied = 0;
    while (copied < length && info.ssid[copied] != '\0') {
        ssid[copied] = static_cast<char>(info.ssid[copied]);
        ++copied;
    }
    ssid[copied] = '\0';

    const unsigned reason = info.reason;
    const char* reasonName = WiFi.STA.disconnectReasonName(static_cast<wifi_err_reason_t>(reason));
    if (reasonName == nullptr || reasonName[0] == '\0') {
        reasonName = "UNKNOWN";
    }

    // Release for loop(): the switch phase reads this to learn that the driver
    // really left the connecting state.
    disconnectEvents_.fetch_add(1, std::memory_order_release);

    Serial.printf("[wifi] disconnected ssid=%s reason=%u name=%s\n",
                  copied > 0 ? ssid : "(none)",
                  reason,
                  reasonName);
}

size_t WifiManager::usableNetworkCount() const {
    return countUsableNetworks(networks_, count_);
}

int WifiManager::slotForUsable(size_t usableIndex) const {
    size_t usable = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (!isUsable(networks_[i])) {
            continue;
        }
        if (usable == usableIndex) {
            return static_cast<int>(i);
        }
        ++usable;
    }
    return -1;
}

bool WifiManager::autoReconnectDisabled() const {
    return !WiFi.getAutoReconnect();
}

bool WifiManager::ensureConnected() {
    // Single source of truth, exactly as in the reference implementation.
    const wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED) {
        if (!wasConnected_) {
            wasConnected_ = true;
            // WL_CONNECTED is published by the core on GOT_IP, so localIP() is
            // already valid here. The password is never printed.
            Serial.printf("[wifi] connected ssid=%s ip=%s status=%d\n",
                          WiFi.SSID().c_str(),
                          WiFi.localIP().toString().c_str(),
                          static_cast<int>(status));
        }
        // Tells the selector a connection exists so it stays Idle: no rescan
        // and no switch back to the primary, so an in-flight upload over the
        // fallback is never interrupted.
        selector_.update(true, false, millis());
        return true;
    }

    if (wasConnected_) {
        wasConnected_ = false;
        Serial.printf("[wifi] disconnected status=%d\n", static_cast<int>(status));
    }

    if (usableNetworkCount() == 0) {
        // TEST_A, or no credentials configured on purpose: stay quiet.
        return false;
    }

    const bool disconnectObserved = (disconnectEvents_.load(std::memory_order_acquire) != disconnectBaseline_);
    const voice_memo_firmware::WifiDecision decision = selector_.update(false, disconnectObserved, millis());
    applyDecision(decision, status);
    return false;
}

bool WifiManager::isConnected() const {
    // Unchanged from the reference implementation.
    return WiFi.status() == WL_CONNECTED;
}

void WifiManager::applyDecision(const voice_memo_firmware::WifiDecision& decision, wl_status_t status) {
    using voice_memo_firmware::WifiCommand;
    using voice_memo_firmware::wifi_connect_state_name;

    const int statusCode = static_cast<int>(status);

    // One line per real transition; the numeric WiFi.status() is always shown
    // so a physical test can tell "still associating" from "no AP found".
    if (decision.events.primaryTimeout) {
        Serial.printf("[wifi] primary timeout status=%d\n", statusCode);
    }
    if (decision.events.fallbackTimeout) {
        Serial.printf("[wifi] fallback timeout status=%d\n", statusCode);
    }
    if (decision.events.switchStarted) {
        Serial.printf("[wifi] switching: disconnecting %s\n",
                      decision.events.switchFromPrimary ? "primary" : "fallback");
    }
    if (decision.events.retrying) {
        Serial.printf("[wifi] retrying known networks status=%d\n", statusCode);
    }

    if (decision.events.stationIdle) {
        // The driver confirmed it left the connecting state. Only from here may
        // the next network be configured.
        if (decision.command == WifiCommand::BeginPrimary) {
            Serial.println("[wifi] station idle; starting primary");
        } else if (decision.command == WifiCommand::BeginFallback) {
            Serial.println("[wifi] station idle; starting fallback");
        } else {
            Serial.println("[wifi] station idle");
        }
    }

    if (decision.events.noNetworkAvailable) {
        // Entering RetryWait is the only transition that reports this, so the
        // state line is printed once and not once per idle loop().
        Serial.printf("[wifi] no known network available status=%d\n", statusCode);
        Serial.printf("[wifi] state=%s\n", wifi_connect_state_name(decision.state));
    }

    if (decision.command == WifiCommand::None) {
        // Waiting in TryingPrimary / TryingFallback / DisconnectingForSwitch /
        // RetryWait: WiFi.begin() must NOT be re-issued here.
        return;
    }

    if (decision.command == WifiCommand::DisconnectStation) {
        // Snapshot before the request: only a disconnect event that arrives
        // after it proves the driver left the connecting state.
        disconnectBaseline_ = disconnectEvents_;

        // Explicitly disconnect the station, without touching anything else:
        //   * wifioff = false    -> the radio and the netif stay up;
        //   * eraseap = false    -> credentials / NVS are never erased;
        //   * timeoutLength = 0  -> no blocking wait loop; the non-blocking
        //                           wait is the DisconnectingForSwitch state.
        if (!WiFi.disconnect(false, false, 0)) {
            Serial.println("[wifi] disconnect request failed; waiting for the safety deadline");
        }
        return;
    }

    const int slot = slotForUsable(decision.index);
    if (slot < 0) {
        return;
    }

    if (decision.command == WifiCommand::BeginPrimary) {
        Serial.printf("[wifi] state=trying_primary ssid=%s status=%d\n",
                      networks_[slot].ssid,
                      statusCode);
    } else {
        Serial.printf("[wifi] state=trying_fallback ssid=%s status=%d\n",
                      networks_[slot].ssid,
                      statusCode);
    }

    // Exactly one begin() per state entry. Passwords are never printed.
    const wl_status_t beginStatus = WiFi.begin(networks_[slot].ssid, passwordOrEmpty(networks_[slot]));
    if (beginStatus == WL_CONNECT_FAILED) {
        // STAClass::connect() refused the configuration - in 3.3.11 that is
        // esp_wifi_set_config() returning ESP_ERR_WIFI_STATE because the driver
        // is still connecting, or an invalid SSID/passphrase. The attempt
        // window keeps running and the next switch waits for a real disconnect.
        Serial.printf("[wifi] begin refused ssid=%s status=%d; the attempt window keeps running\n",
                      networks_[slot].ssid,
                      static_cast<int>(beginStatus));
    }
}
