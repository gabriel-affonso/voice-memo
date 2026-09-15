#pragma once

// PCF85063 real-time clock (I2C address 0x51) on the shared I2C bus.
//
// The chip only counts seconds: it has no concept of a timezone, a UTC offset or
// daylight saving. The firmware therefore uses it strictly as a **UTC** clock.
// readUtc()/writeUtc() carry a full UTC calendar and the Europe/Lisbon
// conversion happens later, in time_zone.cpp (see time_manager.h). Nothing here
// stores or applies a "+1 hour".
//
// Register map and BCD decoding follow the official Waveshare sources for this
// board (01_Arduino_Libraries/SensorLib/src/REG/PCF85063Constants.h and
// SensorPCF85063.hpp, used by 02_Example/Arduino/02_I2C_PCF85063):
//
//   0x00 control_1: bit 5 STOP (0 = oscillator running), bit 1 12/24 (0 = 24 h)
//   0x04 seconds (bit 7 = OS, oscillator stopped) | 0x05 minutes
//   0x06 hours | 0x07 day | 0x08 weekday | 0x09 month | 0x0A year (yy)
//
// begin() forces 24-hour mode and starts the oscillator, exactly like
// SensorPCF85063::initImpl(). When the chip is absent, does not answer, or its
// OS flag is set (never set / backup cell dead), readUtc() fails and the UI
// shows "--:--" unless NTP can supply the time.

#include <Arduino.h>

#include "time_zone.h"

namespace voice_memo_ui {

class RtcPcf85063 {
public:
    // Probes the chip, forces 24-hour mode and starts the oscillator.
    bool begin();

    // True when the chip answered the address probe.
    bool available() const { return available_; }

    // Reads the full UTC calendar. False when the bus fails, a BCD field or the
    // date is implausible, or the OS flag says the oscillator stopped.
    bool readUtc(voice_memo_time::UtcDateTime* out);

    // Writes the full UTC calendar (and clears the OS flag). False on I2C failure.
    bool writeUtc(const voice_memo_time::UtcDateTime& value);

    // OS (oscillator stop) flag of the last read, for the serial log.
    bool oscillatorStopped() const { return oscillatorStopped_; }

private:
    bool force24HourMode();

    bool available_ = false;
    bool oscillatorStopped_ = true;
};

}  // namespace voice_memo_ui
