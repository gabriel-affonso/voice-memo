#pragma once

// Minimal ES8311 microphone-only adapter for the Waveshare
// ESP32-S3-Touch-ePaper-1.54 board.
//
// Register sequence is adapted from the Espressif ESP-ADF ES8311 driver,
// which is distributed under the Espressif MIT license.
bool audio_codec_init();
bool audio_codec_start_adc();
bool es8311_microphone_config(bool analog);
bool audio_codec_stop_adc();
bool audio_codec_is_detected();
