#pragma once

// VoiceMemo firmware for the Waveshare ESP32-S3-Touch-ePaper-1.54.
// Arduino IDE board selection: ESP32-S3 Dev Module.
//
// This Touch model has only one hardware version. The V1/V2 distinction is
// for the non-touch ePaper family and is intentionally not used here.

#define VM_FIRMWARE_VERSION "0.3.0"

// Set to 1 only after TEST A confirms microphone capture on the physical board.
#define VM_ENABLE_UPLOAD 1

#define VM_SAMPLE_RATE 16000
#define VM_CHANNELS 1
#define VM_BITS_PER_SAMPLE 16
#define VM_BYTES_PER_SAMPLE (VM_BITS_PER_SAMPLE / 8)
#define VM_BYTES_PER_SECOND (VM_SAMPLE_RATE * VM_CHANNELS * VM_BYTES_PER_SAMPLE)
#define VM_MAX_RECORDING_SECONDS 45
#define VM_MAX_PAYLOAD_BYTES (VM_BYTES_PER_SECOND * VM_MAX_RECORDING_SECONDS)
#define VM_WAV_HEADER_BYTES 44
#define VM_MAX_WAV_BYTES (VM_WAV_HEADER_BYTES + VM_MAX_PAYLOAD_BYTES)

#define VM_BOOT_BUTTON_PIN 0
#define VM_BUTTON_ACTIVE_LOW 1
#define VM_BUTTON_DEBOUNCE_MS 20

// ES8311 I2C control bus
#define VM_ES8311_I2C_SDA_PIN 47
#define VM_ES8311_I2C_SCL_PIN 48
#define VM_ES8311_I2C_ADDRESS 0x18

// I2S audio bus
#define VM_I2S_MCLK_PIN 14
#define VM_I2S_BCLK_PIN 15
#define VM_I2S_WS_PIN 38
#define VM_I2S_DIN_PIN 16   // ES8311 ADC output -> ESP32 I2S RX
#define VM_I2S_DOUT_PIN 45  // ESP32 I2S TX -> ES8311 DAC input

// Amplifier controls. Playback is intentionally not enabled.
#define VM_PA_EN_PIN 42
#define VM_PA_CTRL_PIN 46

// ES8311 ADC register value for microphone gain. The official driver exposes
// gain enum values; 0x04 corresponds to the 24 dB setting in the ESP-ADF table.
#define VM_ES8311_MIC_GAIN_REG 0x04

#define VM_WIFI_RETRY_INTERVAL_MS 10000
#define VM_UPLOAD_RETRY_INTERVAL_MS 5000
#define VM_UPLOAD_TIMEOUT_MS 15000
