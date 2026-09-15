#pragma once

#include <Arduino.h>
#include "config.h"

class Button {
public:
    enum class Edge {
        None,
        Pressed,
        Released,
    };

    Button(uint8_t pin, unsigned long debounceMs = VM_BUTTON_DEBOUNCE_MS, bool activeLow = true);
    void begin();
    void update();
    bool isPressed() const;
    Edge takePressedEdge();
    Edge takeReleasedEdge();

private:
    bool readRaw() const;

    uint8_t pin_;
    unsigned long debounceMs_;
    bool activeLow_;
    bool lastRaw_;
    bool pressed_;
    unsigned long lastChangeMs_;
    Edge pressedEdge_;
    Edge releasedEdge_;
};
