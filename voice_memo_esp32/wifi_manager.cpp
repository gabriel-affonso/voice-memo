#include "wifi_manager.h"

#include "config.h"

WifiManager::WifiManager(const char* ssid, const char* password)
    : ssid_(ssid), password_(password) {}

bool WifiManager::ensureConnected() {
    // Report a connection that completed since the last call, including the
    // DHCP address the ingress must be reachable from.
    if (isConnected()) {
        if (!wasConnected_) {
            wasConnected_ = true;
            reportPending_ = false;
            Serial.println("[wifi] connected");
        }
        if (!ipReported_) {
            ipReported_ = true;
            Serial.printf("[wifi] ESP32 local IP: %s\n", WiFi.localIP().toString().c_str());
        }
        return true;
    }

    wasConnected_ = false;
    ipReported_ = false;

    if (String(ssid_).length() == 0) {
        // In TEST_A mode no SSID is configured on purpose; stay quiet.
        return false;
    }

    const unsigned long now = millis();
    if (reportPending_ && (now - lastAttemptMs_) >= VM_WIFI_RETRY_INTERVAL_MS) {
        reportPending_ = false;
        Serial.println("[wifi] not connected yet; retrying");
    }

    if ((now - lastAttemptMs_) < VM_WIFI_RETRY_INTERVAL_MS) {
        return false;
    }

    lastAttemptMs_ = now;
    reportPending_ = true;
    Serial.println("[wifi] connecting");
    WiFi.mode(WIFI_STA);
    // Never print the SSID or password here.
    WiFi.begin(ssid_, password_);
    return false;
}

bool WifiManager::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}
