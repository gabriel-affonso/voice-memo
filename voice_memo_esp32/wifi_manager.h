#pragma once

#include <Arduino.h>
#include <WiFi.h>

class WifiManager {
public:
    WifiManager(const char* ssid, const char* password);
    bool ensureConnected();
    bool isConnected() const;

private:
    const char* ssid_;
    const char* password_;
    unsigned long lastAttemptMs_ = 0;
    // Observability only: lets setup/loop report connection transitions and the
    // DHCP address once, without printing credentials.
    bool wasConnected_ = false;
    bool reportPending_ = false;
    bool ipReported_ = false;
};
