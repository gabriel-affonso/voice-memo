#include "touch_ft6336.h"

#include "config.h"
#include "i2c_bus.h"

namespace voice_memo_ui {
namespace {

// FT6336 register map, from the official Waveshare example.
constexpr uint8_t kRegTouchCount = 0x02;
constexpr uint8_t kRegPoint1 = 0x03;
constexpr uint8_t kPointBytes = 4;

// Panel edge; the official example clamps to the panel size the same way.
constexpr uint16_t kMaxCoordinate = 199;

}  // namespace

void Ft6336Touch::resetController() {
    gpio_config_t config = {};
    config.intr_type = GPIO_INTR_DISABLE;
    config.pin_bit_mask = (1ULL << VM_TOUCH_RST_PIN);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&config);

    // Official Ft6336_Reset() timing.
    gpio_set_level(static_cast<gpio_num_t>(VM_TOUCH_RST_PIN), 1);
    delay(100);
    gpio_set_level(static_cast<gpio_num_t>(VM_TOUCH_RST_PIN), 0);
    delay(100);
    gpio_set_level(static_cast<gpio_num_t>(VM_TOUCH_RST_PIN), 1);
    delay(100);
}

bool Ft6336Touch::begin() {
    if (!voice_memo_firmware::vm_i2c_begin()) {
        Serial.println("[ui] touch: shared I2C bus unavailable");
        return false;
    }

    // INT is configured but only sampled for diagnostics; polling keeps the
    // touch path out of an ISR that could disturb audio capture.
    gpio_config_t config = {};
    config.intr_type = GPIO_INTR_DISABLE;
    config.pin_bit_mask = (1ULL << VM_TOUCH_INT_PIN);
    config.mode = GPIO_MODE_INPUT;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&config);

    resetController();

    if (!voice_memo_firmware::vm_i2c_probe(VM_TOUCH_I2C_ADDRESS)) {
        Serial.printf("[ui] touch: FT6336 not answering at 0x%02X\n", VM_TOUCH_I2C_ADDRESS);
        return false;
    }

    interruptIdleLevel_ = gpio_get_level(static_cast<gpio_num_t>(VM_TOUCH_INT_PIN));
    ready_ = true;
    Serial.printf("[ui] touch ready: FT6336 addr=0x%02X rst=%d int=%d (idle level=%d)\n",
                  VM_TOUCH_I2C_ADDRESS,
                  VM_TOUCH_RST_PIN,
                  VM_TOUCH_INT_PIN,
                  interruptIdleLevel_);
    return true;
}

bool Ft6336Touch::readTouch(uint8_t* pointCount, uint16_t* x, uint16_t* y) {
    uint8_t count = 0;
    if (!voice_memo_firmware::vm_i2c_read_reg(VM_TOUCH_I2C_ADDRESS, kRegTouchCount, &count)) {
        return false;
    }
    *pointCount = count;
    if (count == 0) {
        return true;
    }

    uint8_t point[kPointBytes] = {0};
    if (!voice_memo_firmware::vm_i2c_read_regs(VM_TOUCH_I2C_ADDRESS, kRegPoint1, point, kPointBytes)) {
        return false;
    }

    // Same bit packing as the official GetTouchPoint().
    uint16_t rawX = static_cast<uint16_t>(((static_cast<uint16_t>(point[0]) & 0x0F) << 8) | point[1]);
    uint16_t rawY = static_cast<uint16_t>(((static_cast<uint16_t>(point[2]) & 0x0F) << 8) | point[3]);
    if (rawX > kMaxCoordinate) {
        rawX = kMaxCoordinate;
    }
    if (rawY > kMaxCoordinate) {
        rawY = kMaxCoordinate;
    }
    *x = rawX;
    *y = rawY;
    return true;
}

bool Ft6336Touch::pollTap(uint16_t* x, uint16_t* y) {
    if (!ready_) {
        return false;
    }

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextPollMs_) < 0) {
        return false;
    }
    nextPollMs_ = now + VM_UI_TOUCH_POLL_INTERVAL_MS;

    uint8_t count = 0;
    uint16_t pointX = 0;
    uint16_t pointY = 0;
    if (!readTouch(&count, &pointX, &pointY)) {
        // A single failed read is not fatal; keep the previous edge state so a
        // glitch cannot synthesize a tap.
        return false;
    }
    lastPointCount_ = count;

    if (count == 0) {
        touching_ = false;
        return false;
    }

    if (touching_) {
        // Finger already down: no repeat events until it is lifted.
        return false;
    }

    touching_ = true;
    if (x != nullptr) {
        *x = pointX;
    }
    if (y != nullptr) {
        *y = pointY;
    }
    return true;
}

}  // namespace voice_memo_ui
