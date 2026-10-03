#!/usr/bin/env bash
# Host-side test runner for the VoiceMemo ESP32 firmware.
#
# Everything under test here is deliberately free of Arduino, Wi-Fi and FreeRTOS
# so it runs on the Mac with nothing but a C++ compiler:
#
#     cd voice_memo_esp32
#     ./tests/run_host_tests.sh
#
# Exit code 0 means every suite passed. Builds go to a throwaway directory so
# the repo stays clean.

set -u

cd "$(dirname "$0")/.." || exit 2

OUT_DIR="${TMPDIR:-/tmp}/vm_host_tests"
mkdir -p "$OUT_DIR"

CXX="${CXX:-c++}"
CXXFLAGS=(-std=c++11 -Wall -Wextra -O1 -I.)

failures=0
suites=0

run_suite() {
    local name="$1"
    local binary="$OUT_DIR/$name"
    shift
    suites=$((suites + 1))
    printf '\n=== %s ===\n' "$name"
    if ! "$CXX" "${CXXFLAGS[@]}" -o "$binary" "$@"; then
        printf 'BUILD FAILED: %s\n' "$name"
        failures=$((failures + 1))
        return
    fi
    if "$binary"; then
        return
    fi
    failures=$((failures + 1))
}

run_suite ingest_target_test tests/ingest_target_test.cpp
run_suite recording_state_test tests/recording_state_test.cpp
# The BOOT button is the recording control, so its debounce, its edge semantics
# and the toggle rule (1st tap -> REC, 2nd tap -> STOP) are pinned here against
# the real button.cpp, driven by the GPIO/clock fakes in tests/arduino_shim.h.
run_suite button_test -Itests tests/button_test.cpp button.cpp
run_suite wifi_fallback_test tests/wifi_fallback_test.cpp
run_suite time_zone_test tests/time_zone_test.cpp time_zone.cpp
run_suite firmware_calc_test tests/firmware_calc_test.cpp wav_format.cpp
run_suite recording_store_test \
    tests/recording_store_test.cpp recording_store.cpp recording_meta.cpp \
    record_crc32.cpp wav_format.cpp identifier_utils.cpp
run_suite ui_model_test tests/ui_model_test.cpp gfx_canvas.cpp ui_screens.cpp
# Frank's sprite pipeline is pure data plus a bit copy, so the bit order, the
# polarity and the asset geometry are verified here. -Itests is needed because
# the generated sprite pack does #include <Arduino.h> (for PROGMEM/uint8_t),
# which resolves to the shim; PROGMEM itself is defined by the test.
run_suite frank_face_test -Itests tests/frank_face_test.cpp gfx_canvas.cpp
# The power suites are the reason -Itests exists: power_button.h and
# power_policy.h are pure, but power_manager.h includes <Arduino.h> for Serial,
# and tests/Arduino.h provides exactly that shim. No ESP32 core is involved.
run_suite power_button_test -Itests tests/power_button_test.cpp
run_suite power_manager_test -Itests tests/power_manager_test.cpp

# Source-level guard, not a compiled suite: the battery boot critical path is a
# property of statement order inside setup() (the VBAT latch must be asserted
# before any non-essential initialization), which no executable host test can
# observe. It counts as a suite so a regression fails the whole run.
suites=$((suites + 1))
printf '\n=== power_critical_path_check ===\n'
if ! ./tests/power_critical_path_check.sh; then
    failures=$((failures + 1))
fi

printf '\n=========================================\n'
printf '%d host test suite(s), %d failed\n' "$suites" "$failures"
if [ "$failures" -ne 0 ]; then
    exit 1
fi
printf 'all host test suites passed\n'
