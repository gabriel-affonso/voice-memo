#pragma once

// Base-URL parsing for the NUC ingress upload target.
//
// INGEST_URL in secrets.h is a BASE URL only, for example:
//
//     #define INGEST_URL "http://192.168.1.244:8090"
//
// The ingress exposes exactly one upload endpoint, so the firmware always
// POSTs to /api/v1/audio no matter what path (if any) is present in the base
// URL. Normalizing to a single constant keeps the documented contract true and
// prevents a doubled path such as /api/v1/audio/api/v1/audio.
//
// This header is intentionally free of Arduino/WiFi dependencies so the same
// code path can be unit-tested on the host (see tests/ingest_target_test.cpp).

#include <cstddef>
#include <cstdint>
#include <string>

namespace voice_memo_firmware {

// Fixed ingress endpoint. The firmware appends this itself; users must not put
// it in secrets.h.
inline const char* ingest_request_path() {
    return "/api/v1/audio";
}

struct IngestTarget {
    std::string host;
    uint16_t port;
    std::string path;
};

// Parses "http://host[:port][/ignored-path]" into host/port and the fixed
// ingress request path.
//
// Returns false when the scheme is missing, the host is empty, or the port is
// missing/invalid/zero.
inline bool parse_ingest_target(const std::string& input, IngestTarget& target) {
    static const char* const kWhitespace = " \t\r\n";

    std::string url = input;
    const std::size_t first = url.find_first_not_of(kWhitespace);
    if (first == std::string::npos) {
        return false;
    }
    const std::size_t last = url.find_last_not_of(kWhitespace);
    url = url.substr(first, last - first + 1);

    static const char* const kScheme = "http://";
    const std::size_t schemeLength = std::char_traits<char>::length(kScheme);
    if (url.compare(0, schemeLength, kScheme) != 0) {
        return false;
    }
    url = url.substr(schemeLength);

    const std::size_t slashIndex = url.find('/');
    std::string hostPort = slashIndex == std::string::npos ? url : url.substr(0, slashIndex);
    if (hostPort.empty()) {
        return false;
    }

    uint16_t port = 80;
    const std::size_t colonIndex = hostPort.rfind(':');
    if (colonIndex == 0) {
        // ":8090" has no host.
        return false;
    }
    if (colonIndex != std::string::npos) {
        const std::string portText = hostPort.substr(colonIndex + 1);
        if (portText.empty()) {
            return false;
        }

        uint32_t parsedPort = 0;
        for (std::size_t i = 0; i < portText.size(); ++i) {
            const char c = portText[i];
            if (c < '0' || c > '9') {
                return false;
            }
            parsedPort = parsedPort * 10u + static_cast<uint32_t>(c - '0');
            if (parsedPort > 65535u) {
                return false;
            }
        }
        if (parsedPort == 0) {
            return false;
        }

        port = static_cast<uint16_t>(parsedPort);
        hostPort = hostPort.substr(0, colonIndex);
    }

    if (hostPort.empty()) {
        return false;
    }

    target.host = hostPort;
    target.port = port;
    // A path in the base URL is intentionally ignored: the ingress endpoint is
    // fixed, so empty, "/" and explicit paths all normalize to the same target.
    target.path = ingest_request_path();
    return true;
}

}  // namespace voice_memo_firmware
