#pragma once

// TEMPLATE ONLY. Copy to secrets.h and fill in real local values:
//
//     cp secrets.example.h secrets.h
//
// secrets.h is ignored by git (see .gitignore) and must never be committed.
// Every value below is a placeholder. Do not put real credentials in this file.

// --- Wi-Fi (required for TEST B, unused in TEST A) ---
// Must be the same LAN/Wi-Fi network as the NUC ingress.
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

// --- Wi-Fi fallback (optional, same rules as above) ---
// A second known network (for example a phone hotspot) that the firmware tries
// automatically when the primary network above is unavailable or the
// connection fails. Order is: WIFI_SSID first, then WIFI_FALLBACK_SSID. More
// fallbacks can be added later without touching the Wi-Fi code. Leave the
// password empty ("") for an open network. This template must never hold a real
// credential; put it in secrets.h only.
#define WIFI_FALLBACK_SSID "salvação"
#define WIFI_FALLBACK_PASSWORD "your-hotspot-password"

// --- NUC ingress ---
// Base URL only: plain string, scheme + host + optional port, with no Markdown
// brackets. The firmware always POSTs to /api/v1/audio itself, so do NOT put
// that path here. Any path in this value is ignored and normalized to
// /api/v1/audio.
// Example (replace with the NUC LAN IPv4 address from `hostname -I`):
#define INGEST_URL "http://192.168.1.10:8090"

// Optional bearer token. Leave empty when the ingress runs without auth
// (--token unset). Must match the ingress --token value when auth is enabled.
#define INGEST_TOKEN ""

// Stable device id used as the server-side idempotency key together with
// recording_id. Leave empty to derive one from the ESP32 MAC and persist it in
// NVS. For the first integration test set it explicitly, for example:
//     #define DEVICE_ID "bel-esp32-01"
#define DEVICE_ID ""
