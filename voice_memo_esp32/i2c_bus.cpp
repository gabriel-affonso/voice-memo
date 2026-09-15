#include "i2c_bus.h"

#include <Wire.h>

#include "config.h"

namespace voice_memo_firmware {
namespace {

bool g_i2cStarted = false;
constexpr uint32_t kI2cClockHz = 100000U;

}  // namespace

bool vm_i2c_begin() {
    if (g_i2cStarted) {
        return true;
    }

    // The pins are the ones the ES8311 path already used, and they match the
    // official Waveshare board definition for this product
    // ("Board: S3_ePaper_1_54", i2c: {sda: 47, scl: 48}).
    if (!Wire.begin(VM_ES8311_I2C_SDA_PIN, VM_ES8311_I2C_SCL_PIN, kI2cClockHz)) {
        return false;
    }

    g_i2cStarted = true;
    return true;
}

bool vm_i2c_ready() {
    return g_i2cStarted;
}

bool vm_i2c_write_reg(uint8_t address, uint8_t reg, uint8_t value) {
    if (!g_i2cStarted) {
        return false;
    }
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool vm_i2c_write_regs(uint8_t address, uint8_t first_reg, const uint8_t* values, size_t length) {
    if (!g_i2cStarted || values == nullptr || length == 0) {
        return false;
    }
    Wire.beginTransmission(address);
    Wire.write(first_reg);
    for (size_t i = 0; i < length; ++i) {
        Wire.write(values[i]);
    }
    return Wire.endTransmission() == 0;
}

bool vm_i2c_read_regs(uint8_t address, uint8_t first_reg, uint8_t* out_values, size_t length) {
    if (!g_i2cStarted || out_values == nullptr || length == 0) {
        return false;
    }
    Wire.beginTransmission(address);
    Wire.write(first_reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    const size_t received = Wire.requestFrom(address, static_cast<uint8_t>(length));
    if (received != length) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (Wire.available() == 0) {
            return false;
        }
        out_values[i] = static_cast<uint8_t>(Wire.read());
    }
    return true;
}

bool vm_i2c_read_reg(uint8_t address, uint8_t reg, uint8_t* out_value) {
    return vm_i2c_read_regs(address, reg, out_value, 1);
}

bool vm_i2c_probe(uint8_t address) {
    if (!g_i2cStarted) {
        return false;
    }
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

}  // namespace voice_memo_firmware
