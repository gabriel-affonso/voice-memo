#pragma once

#include <Arduino.h>
#include <Preferences.h>

class DeviceIdProvider {
public:
    void begin();
    String getOrCreate();

private:
    bool isValid(const String& value) const;

    Preferences preferences_;
};
