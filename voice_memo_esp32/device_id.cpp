#include "device_id.h"

#include "identifier_utils.h"
#include "secrets.h"

#include <cctype>
#include <esp_system.h>

void DeviceIdProvider::begin() {
    preferences_.begin("voice-memo", false);
}

String DeviceIdProvider::getOrCreate() {
    const String existing = preferences_.getString("device_id", "");
    if (isValid(existing)) {
        return existing;
    }

#if defined(DEVICE_ID)
    const String configured = String(DEVICE_ID);
    if (configured.length() > 0 && isValid(configured)) {
        preferences_.putString("device_id", configured);
        return configured;
    }
#endif

    const uint64_t mac = ESP.getEfuseMac();
    char generated[24];
    snprintf(generated, sizeof(generated), "esp32-%012llX",
             static_cast<unsigned long long>(mac & 0xFFFFFFFFFFFFULL));
    String value(generated);
    value.toLowerCase();
    preferences_.putString("device_id", value);
    return value;
}

bool DeviceIdProvider::isValid(const String& value) const {
    return voice_memo_firmware::is_valid_device_id(value.c_str());
}
