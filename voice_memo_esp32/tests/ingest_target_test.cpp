// Host-side test for the INGEST_URL base-URL handling in ingest_target.h.
//
// It runs on the Mac (no Arduino/ESP32 toolchain needed) because the parsing
// code has no Arduino or WiFi dependency:
//
//     c++ -std=c++11 -Wall -Wextra -o /tmp/ingest_target_test tests/ingest_target_test.cpp
//     /tmp/ingest_target_test
//
// This directory is not compiled into the firmware: Arduino only builds the
// sketch root and src/, so tests/ is ignored by arduino-cli.

#include <cstdio>
#include <string>

#include "../ingest_target.h"

namespace {

int g_failures = 0;

void checkString(const std::string& label, const std::string& actual, const std::string& expected) {
    if (actual == expected) {
        std::printf("PASS %s = \"%s\"\n", label.c_str(), actual.c_str());
        return;
    }
    std::printf("FAIL %s: expected \"%s\", got \"%s\"\n",
                label.c_str(), expected.c_str(), actual.c_str());
    ++g_failures;
}

void checkPort(const std::string& label, uint16_t actual, uint16_t expected) {
    if (actual == expected) {
        std::printf("PASS %s = %u\n", label.c_str(), static_cast<unsigned int>(actual));
        return;
    }
    std::printf("FAIL %s: expected %u, got %u\n",
                label.c_str(),
                static_cast<unsigned int>(expected),
                static_cast<unsigned int>(actual));
    ++g_failures;
}

// The primary TEST B contract: the base URL in secrets.h must resolve to the
// ingress endpoint /api/v1/audio, not "/".
void checkAccepted(
    const std::string& baseUrl,
    const std::string& expectedHost,
    uint16_t expectedPort,
    const std::string& expectedPath
) {
    voice_memo_firmware::IngestTarget target;
    if (!voice_memo_firmware::parse_ingest_target(baseUrl, target)) {
        std::printf("FAIL parse rejected \"%s\"\n", baseUrl.c_str());
        ++g_failures;
        return;
    }
    std::printf("--- base URL \"%s\"\n", baseUrl.c_str());
    checkString("host", target.host, expectedHost);
    checkPort("port", target.port, expectedPort);
    checkString("path", target.path, expectedPath);
}

void checkRejected(const std::string& baseUrl) {
    voice_memo_firmware::IngestTarget target;
    if (voice_memo_firmware::parse_ingest_target(baseUrl, target)) {
        std::printf("FAIL parse accepted invalid URL \"%s\"\n", baseUrl.c_str());
        ++g_failures;
        return;
    }
    std::printf("PASS rejected invalid URL \"%s\"\n", baseUrl.c_str());
}

}  // namespace

int main() {
    const std::string ingressPath = voice_memo_firmware::ingest_request_path();
    checkString("ingest_request_path()", ingressPath, "/api/v1/audio");

    // Requirement: the real INGEST_URL must produce the real final target.
    checkAccepted("http://192.168.1.244:8090", "192.168.1.244", 8090, "/api/v1/audio");
    checkString(
        "final target",
        "http://192.168.1.244:8090" + ingressPath,
        "http://192.168.1.244:8090/api/v1/audio"
    );

    // Trailing slash must not change the result.
    checkAccepted("http://192.168.1.244:8090/", "192.168.1.244", 8090, "/api/v1/audio");

    // An explicit path is normalized consistently (never doubled).
    checkAccepted(
        "http://192.168.1.244:8090/api/v1/audio",
        "192.168.1.244", 8090, "/api/v1/audio"
    );
    checkAccepted("http://192.168.1.244:8090/other", "192.168.1.244", 8090, "/api/v1/audio");

    // Port 80 default and whitespace tolerance.
    checkAccepted("http://192.168.1.10", "192.168.1.10", 80, "/api/v1/audio");
    checkAccepted("  http://192.168.1.10:8090  ", "192.168.1.10", 8090, "/api/v1/audio");

    // Invalid inputs are rejected instead of silently POSTing somewhere odd.
    checkRejected("https://192.168.1.10:8090");
    checkRejected("192.168.1.10:8090");
    checkRejected("");
    checkRejected("http://");
    checkRejected("http://:8090");
    checkRejected("http://192.168.1.10:0");
    checkRejected("http://192.168.1.10:abc");

    if (g_failures == 0) {
        std::printf("\nall ingest_target tests passed\n");
        return 0;
    }
    std::printf("\n%d ingest_target test(s) FAILED\n", g_failures);
    return 1;
}
