#include "rtc_pcf85063.h"

#include "config.h"
#include "i2c_bus.h"

namespace voice_memo_ui {
namespace {

constexpr uint8_t kRegControl1 = 0x00;
constexpr uint8_t kRegSeconds = 0x04;
constexpr uint8_t kCalendarRegisters = 7;  // 0x04..0x0A

// control_1 bits (Waveshare PCF85063Constants.h).
constexpr uint8_t kControl1Stop = 1U << 5U;      // 1 = oscillator stopped
constexpr uint8_t kControl1Hour12 = 1U << 1U;    // 1 = 12-hour mode

uint8_t bcdToDecimal(uint8_t value) {
    return static_cast<uint8_t>(((value >> 4) & 0x0F) * 10 + (value & 0x0F));
}

uint8_t decimalToBcd(uint8_t value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

bool validBcd(uint8_t value, uint8_t maxTens) {
    const uint8_t tens = static_cast<uint8_t>((value >> 4) & 0x0F);
    const uint8_t units = static_cast<uint8_t>(value & 0x0F);
    return tens <= maxTens && units <= 9;
}

// PCF85063 weekday register: 0 = Sunday .. 6 = Saturday.
uint8_t weekdayIndex(int year, int month, int day) {
    // 1970-01-01 was a Thursday, so day 0 maps to weekday 4 with Sunday = 0.
    const int64_t days = voice_memo_time::daysFromCivil(year, month, day);
    return static_cast<uint8_t>(((days % 7) + 7 + 4) % 7);
}

}  // namespace

bool RtcPcf85063::force24HourMode() {
    uint8_t control1 = 0;
    if (!voice_memo_firmware::vm_i2c_read_reg(VM_RTC_I2C_ADDRESS, kRegControl1, &control1)) {
        return false;
    }

    // 24-hour mode keeps a written UTC hour unambiguous, and clearing STOP
    // starts the oscillator (the vendor driver does the same in initImpl()).
    const uint8_t fixed = static_cast<uint8_t>(control1 & ~(kControl1Stop | kControl1Hour12));
    if (fixed == control1) {
        return true;
    }
    return voice_memo_firmware::vm_i2c_write_reg(VM_RTC_I2C_ADDRESS, kRegControl1, fixed);
}

bool RtcPcf85063::begin() {
    if (!voice_memo_firmware::vm_i2c_begin()) {
        Serial.println("[rtc] shared I2C bus unavailable");
        return false;
    }
    if (!voice_memo_firmware::vm_i2c_probe(VM_RTC_I2C_ADDRESS)) {
        Serial.printf("[rtc] PCF85063 not answering at 0x%02X; no RTC time available\n",
                      VM_RTC_I2C_ADDRESS);
        return false;
    }
    if (!force24HourMode()) {
        Serial.printf("[rtc] PCF85063 at 0x%02X did not accept 24-hour mode\n", VM_RTC_I2C_ADDRESS);
        return false;
    }

    available_ = true;
    Serial.printf("[rtc] PCF85063 detected at 0x%02X (24-hour mode, oscillator running)\n",
                  VM_RTC_I2C_ADDRESS);
    return true;
}

bool RtcPcf85063::readUtc(voice_memo_time::UtcDateTime* out) {
    if (!available_ || out == nullptr) {
        return false;
    }

    uint8_t registers[kCalendarRegisters] = {0};
    if (!voice_memo_firmware::vm_i2c_read_regs(
            VM_RTC_I2C_ADDRESS, kRegSeconds, registers, kCalendarRegisters)) {
        return false;
    }

    // Bit 7 of the seconds register is the OS flag: set means the oscillator
    // stopped (typically a dead backup cell), so the time cannot be trusted.
    oscillatorStopped_ = (registers[0] & 0x80U) != 0;
    if (oscillatorStopped_) {
        return false;
    }

    const uint8_t seconds = static_cast<uint8_t>(registers[0] & 0x7FU);
    const uint8_t minutes = static_cast<uint8_t>(registers[1] & 0x7FU);
    const uint8_t rawHour = registers[2];
    const uint8_t rawDay = static_cast<uint8_t>(registers[3] & 0x3FU);
    const uint8_t rawMonth = static_cast<uint8_t>(registers[5] & 0x1FU);
    const uint8_t rawYear = registers[6];

    if (!validBcd(seconds, 5) || !validBcd(minutes, 5) || !validBcd(rawDay, 3) ||
        !validBcd(rawMonth, 1) || !validBcd(rawYear, 9)) {
        return false;
    }

    // Another firmware may have left the chip in 12-hour mode; decode it rather
    // than read a plausible-looking wrong hour.
    uint8_t hours = 0;
    if ((rawHour & 0x40U) != 0) {
        const uint8_t raw12 = static_cast<uint8_t>(rawHour & 0x1FU);
        if (!validBcd(raw12, 1)) {
            return false;
        }
        uint8_t value = bcdToDecimal(raw12);
        if (value < 1 || value > 12) {
            return false;
        }
        const bool pm = (rawHour & 0x20U) != 0;
        if (value == 12) {
            value = 0;
        }
        hours = static_cast<uint8_t>(pm ? value + 12 : value);
    } else {
        const uint8_t raw24 = static_cast<uint8_t>(rawHour & 0x3FU);
        if (!validBcd(raw24, 2)) {
            return false;
        }
        hours = bcdToDecimal(raw24);
        if (hours > 23) {
            return false;
        }
    }

    voice_memo_time::UtcDateTime value;
    value.year = 2000 + bcdToDecimal(rawYear);
    value.month = bcdToDecimal(rawMonth);
    value.day = bcdToDecimal(rawDay);
    value.hour = hours;
    value.minute = bcdToDecimal(minutes);
    value.second = bcdToDecimal(seconds);

    if (!voice_memo_time::isValidUtcDateTime(value)) {
        return false;
    }

    *out = value;
    return true;
}

bool RtcPcf85063::writeUtc(const voice_memo_time::UtcDateTime& value) {
    if (!available_) {
        return false;
    }
    if (!voice_memo_time::isValidUtcDateTime(value) || value.year < 2000 || value.year > 2099) {
        return false;
    }

    uint8_t registers[kCalendarRegisters];
    // Writing bit 7 of the seconds register as 0 clears the OS flag, which is
    // exactly what "this clock is now valid" means.
    registers[0] = static_cast<uint8_t>(decimalToBcd(static_cast<uint8_t>(value.second)) & 0x7FU);
    registers[1] = decimalToBcd(static_cast<uint8_t>(value.minute));
    registers[2] = decimalToBcd(static_cast<uint8_t>(value.hour));  // 24-hour mode
    registers[3] = decimalToBcd(static_cast<uint8_t>(value.day));
    registers[4] = weekdayIndex(value.year, value.month, value.day);
    registers[5] = decimalToBcd(static_cast<uint8_t>(value.month));
    registers[6] = decimalToBcd(static_cast<uint8_t>(value.year % 100));

    if (!voice_memo_firmware::vm_i2c_write_regs(
            VM_RTC_I2C_ADDRESS, kRegSeconds, registers, kCalendarRegisters)) {
        return false;
    }

    oscillatorStopped_ = false;
    return true;
}

}  // namespace voice_memo_ui
