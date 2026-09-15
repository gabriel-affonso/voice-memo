#pragma once

// Single entry point for the shared I2C bus.
//
// ES8311 (0x18), FT6336 touch (0x38) and the PCF85063 RTC (0x51) all sit on the
// same two pins (SDA 47 / SCL 48 on this board). The Arduino Wire instance is
// therefore initialised exactly once, here, and every peripheral reuses it -
// creating a second bus (or calling Wire.begin() again with different pins)
// would break the audio codec that is already validated on hardware.

#include <Arduino.h>

namespace voice_memo_firmware {

// Idempotent: safe to call from any subsystem's begin(). Returns true when the
// bus is usable. Keeps the 100 kHz clock the ES8311 path was validated with.
bool vm_i2c_begin();

// True once vm_i2c_begin() succeeded.
bool vm_i2c_ready();

// Small register helpers over the shared Wire instance. They never throw and
// report failure by returning false so a missing peripheral degrades into
// "feature unavailable" instead of a hang.
bool vm_i2c_write_reg(uint8_t address, uint8_t reg, uint8_t value);
// Burst write used by the PCF85063 calendar (0x04..0x0A): one transaction, so
// the seven time registers are latched together instead of one by one.
bool vm_i2c_write_regs(uint8_t address, uint8_t first_reg, const uint8_t* values, size_t length);
bool vm_i2c_read_reg(uint8_t address, uint8_t reg, uint8_t* out_value);
bool vm_i2c_read_regs(uint8_t address, uint8_t first_reg, uint8_t* out_values, size_t length);

// True when the device answers its address.
bool vm_i2c_probe(uint8_t address);

}  // namespace voice_memo_firmware
