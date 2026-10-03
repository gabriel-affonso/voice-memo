#pragma once

// Minimal Arduino shim for the host tests.
//
// Included through a `tests/Arduino.h` facade (see that file), so a header that
// does `#include <Arduino.h>` resolves to this when the include path contains
// tests/. Note the angle brackets: for a quoted `#include "Arduino.h"` the
// directory of the including file wins, which would pick up this very header.
//
// power_manager.h is deliberately almost Arduino-free: it owns no GPIO, calls no
// millis() of its own and paints nothing, so the only thing it needs from the
// board is Serial. This shim provides that, which lets the real PowerManager be
// driven on a Mac:
//
//     c++ -std=c++11 -Wall -Wextra -Itests -I. -o /tmp/power_manager_test \
//         tests/power_manager_test.cpp
//
// It is NOT a general Arduino emulation and must never be included by firmware
// code - only tests/ uses it.

#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace vm_test_shim {

struct SerialShim {
    // Output is suppressed by default so the tests assert behaviour rather than
    // log text. lineCount() still lets a test prove that no line is printed per
    // loop iteration, which is one of the logging requirements.
    bool verbose = false;
    uint32_t lines = 0;

    void begin(unsigned long) {}

    void println() {
        ++lines;
        if (verbose) {
            std::fputc('\n', stdout);
        }
    }

    void println(const char* text) {
        ++lines;
        if (verbose) {
            std::printf("%s\n", text);
        }
    }

    void print(const char* text) {
        if (verbose) {
            std::printf("%s", text);
        }
    }

    // printf-style, matching HardwareSerial::printf.
    void printf(const char* format, ...) {
        ++lines;
        if (!verbose) {
            return;
        }
        va_list args;
        va_start(args, format);
        std::vprintf(format, args);
        va_end(args);
    }

    void flush() {}
};

inline SerialShim& serial() {
    static SerialShim instance;
    return instance;
}

}  // namespace vm_test_shim

#define Serial (::vm_test_shim::serial())

// ---------------------------------------------------------------------------
// GPIO + clock fakes, used only by the button suite (tests/button_test.cpp).
// ---------------------------------------------------------------------------
// button.cpp is the one firmware file that touches the board through the plain
// Arduino API. The test drives the pin level and the millisecond clock itself,
// so the debounce window and the edge semantics can be asserted on the Mac:
// a single physical press must produce exactly one Pressed edge, and the
// matching release exactly one Released edge.
//
// This is intentionally the smallest possible surface - three functions and
// three constants - and stays out of the way of the other suites, which never
// call them.
#ifndef LOW
#define LOW 0
#endif
#ifndef HIGH
#define HIGH 1
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP 2
#endif

namespace vm_test_shim {

constexpr uint8_t kMaxShimPins = 64;

// One level per pin; the test sets it to simulate the physical line.
inline int& pinLevel(uint8_t pin) {
    static int levels[kMaxShimPins] = {};
    return levels[pin % kMaxShimPins];
}

// The clock the firmware reads through millis(). Never advances by itself: a
// test step is an explicit assignment.
inline unsigned long& clockMs() {
    static unsigned long now = 0;
    return now;
}

}  // namespace vm_test_shim

inline void pinMode(uint8_t, uint8_t) {}
inline int digitalRead(uint8_t pin) { return ::vm_test_shim::pinLevel(pin); }
inline unsigned long millis() { return ::vm_test_shim::clockMs(); }
