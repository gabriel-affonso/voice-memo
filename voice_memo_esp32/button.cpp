#include "button.h"

#include "config.h"

Button::Button(uint8_t pin, unsigned long debounceMs, bool activeLow)
    : pin_(pin), debounceMs_(debounceMs), activeLow_(activeLow), lastRaw_(false),
      pressed_(false), lastChangeMs_(0), pressedEdge_(Edge::None), releasedEdge_(Edge::None) {}

void Button::begin() {
    pinMode(pin_, INPUT_PULLUP);
    lastRaw_ = readRaw();
    pressed_ = lastRaw_;
    lastChangeMs_ = millis();
}

void Button::update() {
    const bool raw = readRaw();
    if (raw != lastRaw_) {
        lastRaw_ = raw;
        lastChangeMs_ = millis();
        return;
    }

    if ((millis() - lastChangeMs_) < debounceMs_) {
        return;
    }

    if (raw != pressed_) {
        pressed_ = raw;
        if (pressed_) {
            pressedEdge_ = Edge::Pressed;
        } else {
            releasedEdge_ = Edge::Released;
        }
    }
}

bool Button::isPressed() const {
    return pressed_;
}

Button::Edge Button::takePressedEdge() {
    const Edge edge = pressedEdge_;
    pressedEdge_ = Edge::None;
    return edge;
}

Button::Edge Button::takeReleasedEdge() {
    const Edge edge = releasedEdge_;
    releasedEdge_ = Edge::None;
    return edge;
}

bool Button::readRaw() const {
    const int level = digitalRead(pin_);
    return activeLow_ ? (level == LOW) : (level == HIGH);
}
