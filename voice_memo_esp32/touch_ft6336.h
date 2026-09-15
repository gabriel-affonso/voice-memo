#pragma once

// FT6336 capacitive touch controller (I2C address 0x38).
//
// Register usage and the reset sequence are taken from the official Waveshare
// example for this board:
//
//   waveshareteam/ESP32-S3-ePaper-1.54 @ main
//   02_Example/Arduino/12_FT6336_Test/{12_FT6336_Test.ino,ft6336_bsp.cpp,user_config.h}
//
// * address 0x38, reset on GPIO7, INT on GPIO21 (EPD_TP_INT_PIN)
// * register 0x02 = number of touch points
// * registers 0x03..0x06 = P1 XH/XL/YH/YL, 12 bit values
// * the official example feeds the reported x/y straight to the 200x200 panel
//   with no rotation or swap, so no coordinate transformation is applied here
//   either - inventing one would silently break the hitboxes
//
// No I2C traffic happens while the finger is down and unchanged: the caller
// only polls, and pollTap() reports one event per press edge.

#include <Arduino.h>

namespace voice_memo_ui {

class Ft6336Touch {
public:
    // Resets the controller and probes the bus. Blocking for ~300 ms (the
    // official reset delays), so it runs once in setup().
    bool begin();

    bool ready() const { return ready_; }

    // Polls at VM_UI_TOUCH_POLL_INTERVAL_MS. Returns true exactly once per
    // touch: on the transition from "no finger" to "finger down". The caller
    // must keep polling so the release is seen before the next tap is reported.
    bool pollTap(uint16_t* x, uint16_t* y);

    // Number of touch points reported by the last successful read.
    uint8_t lastPointCount() const { return lastPointCount_; }
    // Level of the INT line sampled during begin(), for diagnostics only: the
    // driver polls instead of using the interrupt to stay off the ISR path.
    int interruptIdleLevel() const { return interruptIdleLevel_; }

private:
    bool readTouch(uint8_t* pointCount, uint16_t* x, uint16_t* y);
    void resetController();

    bool ready_ = false;
    bool touching_ = false;
    uint8_t lastPointCount_ = 0;
    int interruptIdleLevel_ = -1;
    uint32_t nextPollMs_ = 0;
};

}  // namespace voice_memo_ui
