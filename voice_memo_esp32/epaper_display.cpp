#include "epaper_display.h"

#include <cstring>
#include <esp_heap_caps.h>

#include "config.h"
#include "epd_waveform_tables.h"

namespace voice_memo_ui {
namespace {

constexpr uint16_t kWidth = 200;
constexpr uint16_t kHeight = 200;

// Column-major 1bpp stride for a 200 px wide panel: 200 / 8 = 25 bytes.
constexpr uint16_t kRowBytes = (kWidth + 7) / 8;
constexpr uint16_t kBufferBytes = kRowBytes * kHeight;  // 5000

// SSD1681-class command set, exactly as used by the official Waveshare driver.
constexpr uint8_t kCmdDriverOutputControl = 0x01;
constexpr uint8_t kCmdDataEntryMode = 0x11;
constexpr uint8_t kCmdSwReset = 0x12;
constexpr uint8_t kCmdTemperatureSensor = 0x18;
constexpr uint8_t kCmdMasterActivation = 0x20;
constexpr uint8_t kCmdDisplayUpdateControl2 = 0x22;
constexpr uint8_t kCmdWriteRam = 0x24;
constexpr uint8_t kCmdWriteRamBase = 0x26;
constexpr uint8_t kCmdWriteLut = 0x32;
constexpr uint8_t kCmdWriteRegisterForDisplayOption = 0x37;
constexpr uint8_t kCmdBorderWaveform = 0x3C;
constexpr uint8_t kCmdSetRamXStartEnd = 0x44;
constexpr uint8_t kCmdSetRamYStartEnd = 0x45;
constexpr uint8_t kCmdSetRamXCursor = 0x4E;
constexpr uint8_t kCmdSetRamYCursor = 0x4F;
constexpr uint8_t kCmdLutEndOption = 0x3F;
constexpr uint8_t kCmdLutFrameRate = 0x03;
constexpr uint8_t kCmdLutVoltage = 0x04;
constexpr uint8_t kCmdLutGateVoltage = 0x2C;

}  // namespace

void EpdDisplay::setLevel(int pin, bool high) {
    gpio_set_level(static_cast<gpio_num_t>(pin), high ? 1 : 0);
}

bool EpdDisplay::busy() const {
    // HIGH means busy (see the official read_busy()).
    return gpio_get_level(static_cast<gpio_num_t>(VM_EPD_BUSY_PIN)) == 1;
}

bool EpdDisplay::waitBusy(const char* stage) {
    const uint32_t startMs = millis();
    while (busy()) {
        if (static_cast<uint32_t>(millis() - startMs) > VM_EPD_BUSY_TIMEOUT_MS) {
            // The official driver waits forever here. A panel that never
            // releases BUSY is almost certainly unpowered or disconnected, and
            // hanging the voice memo is worse than losing the display.
            Serial.printf("[epd] FATAL: panel busy timeout (%s); disabling UI\n", stage);
            faulted_ = true;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

bool EpdDisplay::waitIdle() {
    if (faulted_) {
        return false;
    }
    return waitBusy("idle");
}

void EpdDisplay::powerOn() {
    gpio_config_t config = {};
    config.intr_type = GPIO_INTR_DISABLE;
    config.mode = GPIO_MODE_OUTPUT;
    config.pin_bit_mask = (0x1ULL << VM_EPD_PWR_PIN);
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&config);
    // POWEER_EPD_ON() in the official board power helper drives it to 0.
    setLevel(VM_EPD_PWR_PIN, false);
    // Let the panel rail settle before the reset sequence starts.
    vTaskDelay(pdMS_TO_TICKS(50));
}

void EpdDisplay::initGpio() {
    gpio_config_t config = {};
    config.intr_type = GPIO_INTR_DISABLE;
    config.mode = GPIO_MODE_OUTPUT;
    config.pin_bit_mask = (0x1ULL << VM_EPD_RST_PIN) | (0x1ULL << VM_EPD_DC_PIN) |
                          (0x1ULL << VM_EPD_CS_PIN);
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&config);

    config.mode = GPIO_MODE_INPUT;
    config.pin_bit_mask = (0x1ULL << VM_EPD_BUSY_PIN);
    gpio_config(&config);

    setLevel(VM_EPD_RST_PIN, true);
    setLevel(VM_EPD_CS_PIN, true);
}

void EpdDisplay::initSpi() {
    spi_bus_config_t busConfig = {};
    busConfig.miso_io_num = -1;
    busConfig.mosi_io_num = VM_EPD_MOSI_PIN;
    busConfig.sclk_io_num = VM_EPD_SCK_PIN;
    busConfig.quadwp_io_num = -1;
    busConfig.quadhd_io_num = -1;
    busConfig.max_transfer_sz = kWidth * kHeight;

    spi_device_interface_config_t deviceConfig = {};
    // Chip select is driven manually through GPIO, like the official driver.
    deviceConfig.spics_io_num = -1;
    deviceConfig.clock_speed_hz = VM_EPD_SPI_CLOCK_HZ;
    deviceConfig.mode = 0;
    deviceConfig.queue_size = 7;

    esp_err_t err = spi_bus_initialize(static_cast<spi_host_device_t>(VM_EPD_SPI_HOST), &busConfig, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        Serial.printf("[epd] spi_bus_initialize failed: %s\n", esp_err_to_name(err));
        return;
    }
    err = spi_bus_add_device(static_cast<spi_host_device_t>(VM_EPD_SPI_HOST), &deviceConfig, &spi_);
    if (err != ESP_OK) {
        Serial.printf("[epd] spi_bus_add_device failed: %s\n", esp_err_to_name(err));
        return;
    }
    spiReady_ = true;
}

void EpdDisplay::sendCommand(uint8_t command) {
    setLevel(VM_EPD_DC_PIN, false);
    setLevel(VM_EPD_CS_PIN, false);
    spi_transaction_t transaction;
    memset(&transaction, 0, sizeof(transaction));
    transaction.length = 8;
    transaction.tx_buffer = &command;
    const esp_err_t err = spi_device_polling_transmit(spi_, &transaction);
    setLevel(VM_EPD_CS_PIN, true);
    if (err != ESP_OK) {
        Serial.printf("[epd] FATAL: SPI command 0x%02X failed: %s\n", command, esp_err_to_name(err));
        faulted_ = true;
    }
}

void EpdDisplay::sendData(uint8_t data) {
    setLevel(VM_EPD_DC_PIN, true);
    setLevel(VM_EPD_CS_PIN, false);
    spi_transaction_t transaction;
    memset(&transaction, 0, sizeof(transaction));
    transaction.length = 8;
    transaction.tx_buffer = &data;
    const esp_err_t err = spi_device_polling_transmit(spi_, &transaction);
    setLevel(VM_EPD_CS_PIN, true);
    if (err != ESP_OK) {
        Serial.printf("[epd] FATAL: SPI data 0x%02X failed: %s\n", data, esp_err_to_name(err));
        faulted_ = true;
    }
}

void EpdDisplay::sendBytes(const uint8_t* data, size_t length) {
    setLevel(VM_EPD_DC_PIN, true);
    setLevel(VM_EPD_CS_PIN, false);
    spi_transaction_t transaction;
    memset(&transaction, 0, sizeof(transaction));
    transaction.length = 8 * length;
    transaction.tx_buffer = data;
    const esp_err_t err = spi_device_polling_transmit(spi_, &transaction);
    setLevel(VM_EPD_CS_PIN, true);
    if (err != ESP_OK) {
        Serial.printf("[epd] FATAL: SPI burst of %u bytes failed: %s\n",
                      static_cast<unsigned int>(length),
                      esp_err_to_name(err));
        faulted_ = true;
    }
}

void EpdDisplay::setWindows(uint16_t xStart, uint16_t yStart, uint16_t xEnd, uint16_t yEnd) {
    sendCommand(kCmdSetRamXStartEnd);
    sendData(static_cast<uint8_t>((xStart >> 3) & 0xFF));
    sendData(static_cast<uint8_t>((xEnd >> 3) & 0xFF));

    sendCommand(kCmdSetRamYStartEnd);
    sendData(static_cast<uint8_t>(yStart & 0xFF));
    sendData(static_cast<uint8_t>((yStart >> 8) & 0xFF));
    sendData(static_cast<uint8_t>(yEnd & 0xFF));
    sendData(static_cast<uint8_t>((yEnd >> 8) & 0xFF));
}

void EpdDisplay::setCursor(uint16_t xStart, uint16_t yStart) {
    sendCommand(kCmdSetRamXCursor);
    sendData(static_cast<uint8_t>(xStart & 0xFF));

    sendCommand(kCmdSetRamYCursor);
    sendData(static_cast<uint8_t>(yStart & 0xFF));
    sendData(static_cast<uint8_t>((yStart >> 8) & 0xFF));
}

void EpdDisplay::setLut(const uint8_t* lut) {
    sendCommand(kCmdWriteLut);
    sendBytes(lut, 153);
    if (!waitBusy("lut")) {
        return;
    }

    sendCommand(kCmdLutEndOption);
    sendData(lut[153]);

    sendCommand(kCmdLutFrameRate);
    sendData(lut[154]);

    sendCommand(kCmdLutVoltage);
    sendData(lut[155]);
    sendData(lut[156]);
    sendData(lut[157]);

    sendCommand(kCmdLutGateVoltage);
    sendData(lut[158]);
}

void EpdDisplay::turnOnDisplay() {
    sendCommand(kCmdDisplayUpdateControl2);
    sendData(0xC7);
    sendCommand(kCmdMasterActivation);
    waitBusy("full update");
}

void EpdDisplay::turnOnDisplayPartial() {
    sendCommand(kCmdDisplayUpdateControl2);
    sendData(0xCF);
    sendCommand(kCmdMasterActivation);
}

void EpdDisplay::hardwareReset() {
    setLevel(VM_EPD_RST_PIN, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    setLevel(VM_EPD_RST_PIN, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    setLevel(VM_EPD_RST_PIN, true);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void EpdDisplay::loadFullWaveform() {
    sendCommand(kCmdSwReset);
    if (!waitBusy("swreset")) {
        return;
    }

    sendCommand(kCmdDriverOutputControl);
    sendData(0xC7);
    sendData(0x00);
    sendData(0x01);

    sendCommand(kCmdDataEntryMode);
    sendData(0x01);

    // Official call is EPD_SetWindows(0, Width-1, Height-1, 0).
    setWindows(0, kWidth - 1, kHeight - 1, 0);

    sendCommand(kCmdBorderWaveform);
    sendData(0x01);

    sendCommand(kCmdTemperatureSensor);
    sendData(0x80);

    sendCommand(kCmdDisplayUpdateControl2);
    sendData(0xB1);
    sendCommand(kCmdMasterActivation);

    setCursor(0, kHeight - 1);
    if (!waitBusy("cursor")) {
        return;
    }

    setLut(kWaveformFull);
}

void EpdDisplay::loadPartialWaveform() {
    setLut(kWaveformPartial);

    sendCommand(kCmdWriteRegisterForDisplayOption);
    sendData(0x00);
    sendData(0x00);
    sendData(0x00);
    sendData(0x00);
    sendData(0x00);
    sendData(0x40);
    sendData(0x00);
    sendData(0x00);
    sendData(0x00);
    sendData(0x00);

    sendCommand(kCmdBorderWaveform);
    sendData(0x80);

    sendCommand(kCmdDisplayUpdateControl2);
    sendData(0xC0);
    sendCommand(kCmdMasterActivation);
    waitBusy("partial mode");
}

void EpdDisplay::writeFramebufferToRam() {
    sendCommand(kCmdWriteRam);
    sendBytes(buffer_, kBufferBytes);
}

void EpdDisplay::writeFramebufferAsBaseImage() {
    sendCommand(kCmdWriteRam);
    sendBytes(buffer_, kBufferBytes);
    sendCommand(kCmdWriteRamBase);
    sendBytes(buffer_, kBufferBytes);
    turnOnDisplay();
}

bool EpdDisplay::begin() {
    if (buffer_ == nullptr) {
        buffer_ = static_cast<uint8_t*>(heap_caps_malloc(kBufferBytes, MALLOC_CAP_SPIRAM));
        if (buffer_ == nullptr) {
            // 5000 bytes also fit comfortably in internal RAM.
            buffer_ = static_cast<uint8_t*>(malloc(kBufferBytes));
        }
        if (buffer_ == nullptr) {
            Serial.println("[epd] FATAL: no framebuffer memory");
            return false;
        }
    }
    canvas_.begin(buffer_, kWidth, kHeight);
    canvas_.clear(GfxColor::White);

    powerOn();
    initGpio();
    initSpi();
    if (!spiReady_) {
        return false;
    }

    const uint32_t startMs = millis();
    hardwareReset();
    loadFullWaveform();
    if (faulted_) {
        return false;
    }
    // First content: a cleared screen, promoted to the panel's base image so
    // later partial updates have a clean reference.
    writeFramebufferAsBaseImage();
    if (faulted_) {
        return false;
    }
    loadPartialWaveform();
    if (faulted_) {
        return false;
    }

    ready_ = true;
    lastRefreshMs_ = millis() - startMs;
    Serial.printf("[epd] panel ready in %u ms (official Waveshare driver, SPI%d %u Hz)\n",
                  static_cast<unsigned int>(lastRefreshMs_),
                  VM_EPD_SPI_HOST,
                  static_cast<unsigned int>(VM_EPD_SPI_CLOCK_HZ));
    return true;
}

bool EpdDisplay::refreshFull() {
    if (!ready_ || faulted_) {
        return false;
    }
    // A new command may only be issued once the previous update finished.
    if (!waitIdle()) {
        return false;
    }

    const uint32_t startMs = millis();
    hardwareReset();
    loadFullWaveform();
    if (faulted_) {
        return false;
    }
    writeFramebufferAsBaseImage();
    if (faulted_) {
        return false;
    }
    loadPartialWaveform();
    if (faulted_) {
        return false;
    }

    lastRefreshMs_ = millis() - startMs;
    ++fullRefreshCount_;
    return true;
}

bool EpdDisplay::startPartial() {
    if (!ready_ || faulted_) {
        return false;
    }
    // Never wait here: the caller is the main loop, which may be feeding I2S.
    if (busy()) {
        return false;
    }

    const uint32_t startMs = millis();
    writeFramebufferToRam();
    turnOnDisplayPartial();
    lastRefreshMs_ = millis() - startMs;
    ++partialRefreshCount_;
    return true;
}

}  // namespace voice_memo_ui
