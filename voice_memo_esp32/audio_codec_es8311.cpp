#include "audio_codec_es8311.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "i2c_bus.h"

namespace {

constexpr uint8_t kResetReg = 0x00;
constexpr uint8_t kClkManagerReg01 = 0x01;
constexpr uint8_t kClkManagerReg02 = 0x02;
constexpr uint8_t kClkManagerReg03 = 0x03;
constexpr uint8_t kClkManagerReg04 = 0x04;
constexpr uint8_t kClkManagerReg05 = 0x05;
constexpr uint8_t kClkManagerReg06 = 0x06;
constexpr uint8_t kClkManagerReg07 = 0x07;
constexpr uint8_t kClkManagerReg08 = 0x08;
constexpr uint8_t kSdpInReg09 = 0x09;
constexpr uint8_t kSdpOutReg0A = 0x0A;
constexpr uint8_t kSystemReg0B = 0x0B;
constexpr uint8_t kSystemReg0C = 0x0C;
constexpr uint8_t kSystemReg0D = 0x0D;
constexpr uint8_t kSystemReg0E = 0x0E;
constexpr uint8_t kSystemReg10 = 0x10;
constexpr uint8_t kSystemReg11 = 0x11;
constexpr uint8_t kSystemReg12 = 0x12;
constexpr uint8_t kSystemReg13 = 0x13;
constexpr uint8_t kSystemReg14 = 0x14;
constexpr uint8_t kAdcReg15 = 0x15;
constexpr uint8_t kAdcReg16 = 0x16;
constexpr uint8_t kAdcReg17 = 0x17;
constexpr uint8_t kAdcReg1B = 0x1B;
constexpr uint8_t kAdcReg1C = 0x1C;
constexpr uint8_t kDacReg32 = 0x32;
constexpr uint8_t kDacReg37 = 0x37;
constexpr uint8_t kGpioReg44 = 0x44;
constexpr uint8_t kGpReg45 = 0x45;
constexpr uint8_t kChipId1 = 0xFD;
constexpr uint8_t kChipId2 = 0xFE;

bool i2cStarted = false;
bool codecDetected = false;

bool writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(VM_ES8311_I2C_ADDRESS);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

int readReg(uint8_t reg) {
    Wire.beginTransmission(VM_ES8311_I2C_ADDRESS);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return -1;
    }
    if (Wire.requestFrom(static_cast<uint8_t>(VM_ES8311_I2C_ADDRESS), static_cast<uint8_t>(1)) != 1) {
        return -1;
    }
    return Wire.read();
}

bool initI2c() {
    if (i2cStarted) {
        return true;
    }

    // The bus itself is owned by i2c_bus.cpp so the ES8311, the FT6336 touch
    // controller and the PCF85063 RTC share one Wire instance and one set of
    // pins (SDA 47 / SCL 48) instead of racing to initialise I2C.
    if (!voice_memo_firmware::vm_i2c_begin()) {
        return false;
    }

    pinMode(VM_PA_EN_PIN, OUTPUT);
    pinMode(VM_PA_CTRL_PIN, OUTPUT);
    digitalWrite(VM_PA_EN_PIN, LOW);
    digitalWrite(VM_PA_CTRL_PIN, LOW);

    i2cStarted = true;
    return true;
}

void configure16kClock() {
    // ESP32 I2S MCLK = 16000 * 256 = 4,096,000 Hz.
    // Adapted from the ESP-ADF coeff_div table entry:
    // {4096000, 16000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20}
    uint8_t regv = static_cast<uint8_t>(readReg(kClkManagerReg02)) & 0x07U;
    regv |= (0x01U - 1U) << 5U;
    regv |= 0U << 3U;
    writeReg(kClkManagerReg02, regv);

    regv = static_cast<uint8_t>(readReg(kClkManagerReg05)) & 0x00U;
    regv |= (0x01U - 1U) << 4U;
    regv |= (0x01U - 1U) << 0U;
    writeReg(kClkManagerReg05, regv);

    regv = static_cast<uint8_t>(readReg(kClkManagerReg03)) & 0x80U;
    regv |= 0x00U << 6U;
    regv |= 0x10U;
    writeReg(kClkManagerReg03, regv);

    regv = static_cast<uint8_t>(readReg(kClkManagerReg04)) & 0x80U;
    regv |= 0x20U;
    writeReg(kClkManagerReg04, regv);

    regv = static_cast<uint8_t>(readReg(kClkManagerReg07)) & 0xC0U;
    regv |= 0x00U;
    writeReg(kClkManagerReg07, regv);

    regv = static_cast<uint8_t>(readReg(kClkManagerReg08)) & 0x00U;
    regv |= 0xFFU;
    writeReg(kClkManagerReg08, regv);

    regv = static_cast<uint8_t>(readReg(kClkManagerReg06)) & 0xE0U;
    regv |= (0x04U - 1U) << 0U;
    writeReg(kClkManagerReg06, regv);
}

}  // namespace

bool audio_codec_init() {
    if (!initI2c()) {
        Serial.println("[es8311] I2C init failed");
        return false;
    }

    writeReg(kGpioReg44, 0x08);
    writeReg(kGpioReg44, 0x08);

    const int chipId1 = readReg(kChipId1);
    const int chipId2 = readReg(kChipId2);
    if (chipId1 >= 0 || chipId2 >= 0) {
        codecDetected = true;
        Serial.printf("[es8311] detected id1=0x%02X id2=0x%02X\n",
                      chipId1 < 0 ? 0 : chipId1,
                      chipId2 < 0 ? 0 : chipId2);
    } else {
        codecDetected = false;
        Serial.println("[es8311] no ACK on I2C bus");
        return false;
    }

    writeReg(kClkManagerReg01, 0x30);
    writeReg(kClkManagerReg02, 0x00);
    writeReg(kClkManagerReg03, 0x10);
    writeReg(kAdcReg16, 0x24);
    writeReg(kClkManagerReg04, 0x10);
    writeReg(kClkManagerReg05, 0x00);
    writeReg(kSystemReg0B, 0x00);
    writeReg(kSystemReg0C, 0x00);
    writeReg(kSystemReg10, 0x1F);
    writeReg(kSystemReg11, 0x7F);
    writeReg(kResetReg, 0x80);

    int regv = readReg(kResetReg);
    regv &= 0xBF;  // ES8311 slave mode; ESP32 provides MCLK/BCLK/LRCK.
    writeReg(kResetReg, static_cast<uint8_t>(regv));

    writeReg(kClkManagerReg01, 0x3F);

    regv = readReg(kClkManagerReg01);
    regv &= 0x7F;  // FROM_MCLK_PIN
    writeReg(kClkManagerReg01, static_cast<uint8_t>(regv));

    configure16kClock();

    regv = readReg(kClkManagerReg01);
    regv &= ~0x40;  // do not invert MCLK
    writeReg(kClkManagerReg01, static_cast<uint8_t>(regv));

    regv = readReg(kClkManagerReg06);
    regv &= ~0x20;  // do not invert SCLK
    writeReg(kClkManagerReg06, static_cast<uint8_t>(regv));

    writeReg(kSystemReg13, 0x10);
    writeReg(kAdcReg1B, 0x0A);
    writeReg(kAdcReg1C, 0x6A);

    Serial.println("[es8311] initialized for microphone capture");
    return true;
}

bool es8311_microphone_config(bool analog) {
    // false selects the analog microphone path and keeps PDM/digital mode disabled.
    writeReg(kSystemReg14, analog ? 0x1A : 0x1A);
    return true;
}

bool audio_codec_start_adc() {
    if (!codecDetected) {
        return false;
    }

    // Set ADC I2S format: normal I2S, 16-bit data.
    uint8_t dac_iface = static_cast<uint8_t>(readReg(kSdpInReg09)) & 0xFCU;
    uint8_t adc_iface = static_cast<uint8_t>(readReg(kSdpOutReg0A)) & 0xFCU;
    dac_iface |= 0x0CU;
    adc_iface |= 0x0CU;

    dac_iface = (dac_iface & 0xBFU) | 0x40U;
    adc_iface = (adc_iface & 0xBFU) | 0x40U;
    adc_iface &= ~0x40U;  // ADC enabled; DAC remains muted/disabled.

    writeReg(kSdpInReg09, dac_iface);
    writeReg(kSdpOutReg0A, adc_iface);

    writeReg(kAdcReg17, 0xBF);
    writeReg(kSystemReg0E, 0x02);
    writeReg(kSystemReg12, 0x00);
    es8311_microphone_config(false);
    writeReg(kSystemReg0D, 0x01);
    writeReg(kAdcReg15, 0x40);
    writeReg(kDacReg37, 0x08);
    writeReg(kGpReg45, 0x00);
    writeReg(kGpioReg44, 0x58);

    writeReg(kAdcReg16, VM_ES8311_MIC_GAIN_REG);
    return true;
}

bool audio_codec_stop_adc() {
    if (!codecDetected) {
        return false;
    }

    writeReg(kDacReg32, 0x00);
    writeReg(kAdcReg17, 0x00);
    writeReg(kSystemReg0E, 0xFF);
    writeReg(kSystemReg12, 0x02);
    writeReg(kSystemReg14, 0x00);
    writeReg(kSystemReg0D, 0xFA);
    writeReg(kAdcReg15, 0x00);
    writeReg(kGpReg45, 0x01);
    return true;
}

bool audio_codec_is_detected() {
    return codecDetected;
}
