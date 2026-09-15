#pragma once

// E-paper driver for the Waveshare ESP32-S3-Touch-ePaper-1.54 panel.
//
// This is a direct port of the official Waveshare demo driver:
//
//   waveshareteam/ESP32-S3-ePaper-1.54 @ main
//   02_Example/Arduino/09_LVGL_V8_Test/src/display/epaper_driver_bsp.{h,cpp}
//
// Same SPI host, same pins, same init sequence, same waveform tables (copied
// byte for byte into epd_waveform_tables.h). The only deliberate deviations,
// both required by this firmware, are:
//
//  1. BUSY waits are bounded. The official read_busy() loops forever; here a
//     missing or unpowered panel degrades into "UI disabled" instead of a
//     firmware hang.
//  2. A partial refresh can be started *without* waiting for BUSY. During
//     Recording the main loop also feeds I2S, and the panel needs hundreds of
//     milliseconds to finish an update, so waiting would drop audio. The caller
//     must not start another update before busy() goes low; the UI defers.
//
// Framebuffer: rowBytes(25) * 200 = 5000 bytes, 1 bit per pixel, bit 7 =
// leftmost pixel, set bit = white - identical to the official driver so the
// GfxCanvas bit convention matches the panel.

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>

#include "gfx_canvas.h"

namespace voice_memo_ui {

class EpdDisplay {
public:
    // Powers the panel, initialises SPI/GPIO, loads the full waveform, clears
    // the panel and switches it to partial mode. Blocking on purpose: it runs
    // once in setup(), before audio capture exists. Returns false when the
    // panel does not come up, in which case the caller must keep the firmware
    // running without a UI.
    bool begin();

    bool ready() const { return ready_; }
    // True once a BUSY wait timed out. The UI stops touching the panel.
    bool faulted() const { return faulted_; }

    GfxCanvas& canvas() { return canvas_; }

    // Full refresh: hardware reset, full waveform, base image, then partial mode
    // again. Flashes and takes seconds, so it must only be called while no
    // audio is being captured (UiController guarantees this).
    bool refreshFull();

    // Partial refresh: pushes the framebuffer with the partial waveform and
    // returns immediately. Returns false when the panel is still busy with the
    // previous update (the caller keeps its pending render and retries) or when
    // the panel is faulted.
    bool startPartial();

    // BUSY line: HIGH means the panel is still updating.
    bool busy() const;

    // Waits until the panel can accept a new command. Bounded by
    // VM_EPD_BUSY_TIMEOUT_MS.
    bool waitIdle();

    // Diagnostics for the serial log / physical test.
    uint32_t lastRefreshMs() const { return lastRefreshMs_; }
    uint32_t partialRefreshCount() const { return partialRefreshCount_; }
    uint32_t fullRefreshCount() const { return fullRefreshCount_; }

private:
    void powerOn();
    void initGpio();
    void initSpi();

    void setLevel(int pin, bool high);

    void sendCommand(uint8_t command);
    void sendData(uint8_t data);
    void sendBytes(const uint8_t* data, size_t length);

    void setLut(const uint8_t* lut);
    void setWindows(uint16_t xStart, uint16_t yStart, uint16_t xEnd, uint16_t yEnd);
    void setCursor(uint16_t xStart, uint16_t yStart);
    void turnOnDisplay();
    void turnOnDisplayPartial();

    void hardwareReset();
    void loadFullWaveform();
    void loadPartialWaveform();
    void writeFramebufferToRam();
    void writeFramebufferAsBaseImage();

    bool waitBusy(const char* stage);

    spi_device_handle_t spi_ = nullptr;
    GfxCanvas canvas_;
    uint8_t* buffer_ = nullptr;
    bool ready_ = false;
    bool faulted_ = false;
    bool spiReady_ = false;
    uint32_t lastRefreshMs_ = 0;
    uint32_t partialRefreshCount_ = 0;
    uint32_t fullRefreshCount_ = 0;
};

}  // namespace voice_memo_ui
