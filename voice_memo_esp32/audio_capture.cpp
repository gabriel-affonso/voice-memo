#include "audio_capture.h"

#include "audio_codec_es8311.h"
#include "config.h"

namespace {

// The ES8311 ADC drives the LEFT slot of every I2S frame and the codec's DAC
// reference (muted in this firmware) occupies the RIGHT one, so the microphone
// is captured by selecting the LEFT slot and letting the ESP32-S3 RX store one
// sample per frame. See the "I2S RX slot selection" block in config.h for the
// full evidence chain; VM_I2S_RX_SLOT_RIGHT exists only for the controlled
// physical A/B test described in README.md.
#if VM_I2S_RX_SLOT_RIGHT
constexpr i2s_std_slot_mask_t kRxSlotMask = I2S_STD_SLOT_RIGHT;
#else
constexpr i2s_std_slot_mask_t kRxSlotMask = I2S_STD_SLOT_LEFT;
#endif

static_assert(kRxSlotMask != I2S_STD_SLOT_BOTH,
              "I2S RX must capture exactly one slot per frame; with both slots the "
              "idle right slot is stored too and the WAV ends up twice as long");

// I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG() cannot be used on its own here: on
// ESP32-S3 (its `#else` branch) it forces slot_mask = I2S_STD_SLOT_BOTH
// regardless of I2S_SLOT_MODE_MONO, which is precisely the capture bug this
// firmware used to have. Build the configuration explicitly and pin the slot.
i2s_std_slot_config_t rxSlotConfig() {
    i2s_std_slot_config_t slot_config = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_MONO
    );
    slot_config.slot_mask = kRxSlotMask;
    return slot_config;
}

#if VM_I2S_SLOT_PROBE
// Stereo variant used only by the bring-up probe, so that both slots of each
// frame are returned interleaved as [left, right, left, right, ...].
i2s_std_slot_config_t probeSlotConfig() {
    i2s_std_slot_config_t slot_config = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO
    );
    slot_config.slot_mask = I2S_STD_SLOT_BOTH;
    return slot_config;
}

constexpr size_t kProbeFrames = 4096;
constexpr uint32_t kProbeTimeoutMs = 200;
#endif  // VM_I2S_SLOT_PROBE

}  // namespace

bool AudioCapture::enableChannel(i2s_chan_handle_t handle, const char* label) {
    if (handle == nullptr) {
        return false;
    }

    const esp_err_t err = i2s_channel_enable(handle);
    if (err != ESP_OK) {
        log_e("i2s_channel_enable(%s) failed: %s", label, esp_err_to_name(err));
        return false;
    }
    return true;
}

void AudioCapture::disableChannel(i2s_chan_handle_t handle, bool& enabled, const char* label) {
    if (handle == nullptr || !enabled) {
        return;
    }

    const esp_err_t err = i2s_channel_disable(handle);
    if (err == ESP_OK) {
        enabled = false;
        return;
    }

    // ESP_ERR_INVALID_STATE means the driver has already released the channel.
    // Record that so a later stop() does not warn again.
    if (err == ESP_ERR_INVALID_STATE) {
        enabled = false;
    } else {
        log_e("i2s_channel_disable(%s) failed: %s", label, esp_err_to_name(err));
    }
}

bool AudioCapture::begin() {
    i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&channelConfig, &txHandle_, &rxHandle_);
    if (err != ESP_OK) {
        log_e("i2s_new_channel failed: %s", esp_err_to_name(err));
        return false;
    }

    i2s_std_config_t stdConfig = {};
    stdConfig.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VM_SAMPLE_RATE);
    // One 16-bit slot per frame, taken from the ES8311 ADC slot (LEFT).
    stdConfig.slot_cfg = rxSlotConfig();
    stdConfig.gpio_cfg = {
        .mclk = static_cast<gpio_num_t>(VM_I2S_MCLK_PIN),
        .bclk = static_cast<gpio_num_t>(VM_I2S_BCLK_PIN),
        .ws = static_cast<gpio_num_t>(VM_I2S_WS_PIN),
        .dout = static_cast<gpio_num_t>(VM_I2S_DOUT_PIN),
        .din = static_cast<gpio_num_t>(VM_I2S_DIN_PIN),
        .invert_flags = {
            .mclk_inv = false,
            .bclk_inv = false,
            .ws_inv = false,
        },
    };

    err = i2s_channel_init_std_mode(rxHandle_, &stdConfig);
    if (err != ESP_OK) {
        log_e("i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!audio_codec_init()) {
        log_e("ES8311 initialization failed");
        return false;
    }

    if (!enableChannel(rxHandle_, "rx")) {
        return false;
    }
    rxEnabled_ = true;

    if (!audio_codec_start_adc()) {
        log_e("ES8311 ADC start failed");
        return false;
    }

#if VM_I2S_SLOT_PROBE
    probeSlots();
#endif

    ready_ = true;
    return true;
}

bool AudioCapture::start() {
    if (ready_) {
        return true;
    }
    if (rxHandle_ == nullptr) {
        return false;
    }

    if (!enableChannel(rxHandle_, "rx")) {
        return false;
    }
    rxEnabled_ = true;

    if (!audio_codec_start_adc()) {
        log_e("ES8311 ADC start failed");
        return false;
    }

    ready_ = true;
    return true;
}

size_t AudioCapture::read(uint8_t* dest, size_t maxBytes, uint32_t timeoutMs) {
    if (!ready_ || dest == nullptr || maxBytes == 0) {
        return 0;
    }

    size_t bytesRead = 0;
    const esp_err_t err = i2s_channel_read(rxHandle_, dest, maxBytes, &bytesRead, timeoutMs);
    if (err == ESP_OK) {
        return bytesRead;
    }
    if (err == ESP_ERR_TIMEOUT) {
        return 0;
    }

    log_e("i2s_channel_read failed: %s", esp_err_to_name(err));
    return 0;
}

void AudioCapture::stop() {
    audio_codec_stop_adc();
    // Only disable what was actually enabled. txHandle_ is created by
    // i2s_new_channel but never enabled, so disabling it is what produced:
    //   i2s_common: i2s_channel_disable(): the channel has not been enabled yet
    disableChannel(rxHandle_, rxEnabled_, "rx");
    disableChannel(txHandle_, txEnabled_, "tx");
    ready_ = false;
}

bool AudioCapture::isReady() const {
    return ready_;
}

#if VM_I2S_SLOT_PROBE
void AudioCapture::probeSlots() {
    // Temporary bring-up probe (VM_I2S_SLOT_PROBE 1). It reconfigures the RX to
    // stereo (both slots, which is what the driver sizes its buffers for), grabs
    // one burst, reports which slot carries the ES8311 ADC, and then always
    // restores the production single-slot configuration. It never runs while a
    // recording is in progress and leaves rxEnabled_ consistent, so stop() keeps
    // working.
    //
    // The burst is heap allocated on purpose: begin() runs on the Arduino loop
    // task, whose stack is 8 KB, and one burst is 16 KB.
    constexpr size_t kBurstSamples = kProbeFrames * 2;
    constexpr size_t kBurstBytes = kBurstSamples * sizeof(int16_t);

    i2s_std_slot_config_t stereo = probeSlotConfig();
    i2s_std_slot_config_t single = rxSlotConfig();

    if (rxEnabled_) {
        disableChannel(rxHandle_, rxEnabled_, "rx");
    }

    int16_t* burst = static_cast<int16_t*>(malloc(kBurstBytes));
    size_t frames = 0;

    esp_err_t err = i2s_channel_reconfig_std_slot(rxHandle_, &stereo);
    if (err != ESP_OK) {
        log_e("[i2s-probe] stereo reconfig failed: %s", esp_err_to_name(err));
    } else if (enableChannel(rxHandle_, "rx")) {
        rxEnabled_ = true;
        if (burst == nullptr) {
            log_e("[i2s-probe] out of memory for the probe buffer");
        } else {
            size_t bytesRead = 0;
            err = i2s_channel_read(rxHandle_, burst, kBurstBytes, &bytesRead, kProbeTimeoutMs);
            if (err == ESP_OK) {
                frames = bytesRead / (2 * sizeof(int16_t));
            } else {
                log_e("[i2s-probe] read failed: %s", esp_err_to_name(err));
            }
        }
        disableChannel(rxHandle_, rxEnabled_, "rx");
    }

    if (frames > 0) {
        uint64_t sumLeft = 0;
        uint64_t sumRight = 0;
        uint64_t sumLeftSquares = 0;
        uint64_t sumRightSquares = 0;
        uint64_t sumProduct = 0;
        uint64_t sumAbsDiff = 0;
        uint32_t zerosLeft = 0;
        uint32_t zerosRight = 0;
        int32_t peakLeft = 0;
        int32_t peakRight = 0;

        for (size_t i = 0; i < frames; ++i) {
            const int32_t left = burst[i * 2];
            const int32_t right = burst[i * 2 + 1];
            sumLeft += static_cast<uint64_t>(left);
            sumRight += static_cast<uint64_t>(right);
            sumLeftSquares += static_cast<uint64_t>(left) * static_cast<uint64_t>(left);
            sumRightSquares += static_cast<uint64_t>(right) * static_cast<uint64_t>(right);
            sumProduct += static_cast<uint64_t>(left) * static_cast<uint64_t>(right);
            const int32_t diff = left - right;
            sumAbsDiff += static_cast<uint64_t>(diff < 0 ? -diff : diff);
            zerosLeft += (left == 0) ? 1 : 0;
            zerosRight += (right == 0) ? 1 : 0;
            const int32_t magnitudeLeft = left < 0 ? -left : left;
            const int32_t magnitudeRight = right < 0 ? -right : right;
            if (magnitudeLeft > peakLeft) {
                peakLeft = magnitudeLeft;
            }
            if (magnitudeRight > peakRight) {
                peakRight = magnitudeRight;
            }
        }

        const double n = static_cast<double>(frames);
        const double meanLeft = static_cast<double>(sumLeft) / n;
        const double meanRight = static_cast<double>(sumRight) / n;
        const double varLeft = static_cast<double>(sumLeftSquares) / n - meanLeft * meanLeft;
        const double varRight = static_cast<double>(sumRightSquares) / n - meanRight * meanRight;
        const double covariance = static_cast<double>(sumProduct) / n - meanLeft * meanRight;
        const double rmsLeft = sqrt(varLeft > 0.0 ? varLeft : 0.0);
        const double rmsRight = sqrt(varRight > 0.0 ? varRight : 0.0);
        const double denominator = sqrt((varLeft > 0.0 ? varLeft : 0.0) * (varRight > 0.0 ? varRight : 0.0));
        const double correlation = denominator > 0.0 ? covariance / denominator : 0.0;
        const double mae = static_cast<double>(sumAbsDiff) / n;

        Serial.printf("[i2s-probe] frames=%u slotA(left)=zeros %.2f%% rms=%.1f peak=%ld | "
                      "slotB(right)=zeros %.2f%% rms=%.1f peak=%ld\n",
                      static_cast<unsigned int>(frames),
                      100.0 * static_cast<double>(zerosLeft) / n,
                      rmsLeft,
                      static_cast<long>(peakLeft),
                      100.0 * static_cast<double>(zerosRight) / n,
                      rmsRight,
                      static_cast<long>(peakRight));
        Serial.printf("[i2s-probe] correlation(slotA,slotB)=%.6f MAE=%.1f\n", correlation, mae);
        if (zerosRight == frames && zerosLeft != frames) {
            Serial.println("[i2s-probe] slotA (LEFT) carries the ES8311 ADC; keep VM_I2S_RX_SLOT_LEFT");
        } else if (zerosLeft == frames && zerosRight != frames) {
            Serial.println("[i2s-probe] slotB (RIGHT) carries the ES8311 ADC; build with VM_I2S_RX_SLOT_RIGHT");
        } else if (rmsLeft == 0.0 && rmsRight == 0.0) {
            Serial.println("[i2s-probe] both slots silent; check the microphone and the ES8311 gain");
        } else {
            Serial.println("[i2s-probe] both slots carry audio; re-check the ES8311 routing (reg 0x44)");
        }
    } else {
        Serial.printf("[i2s-probe] no usable stereo burst from the RX channel (err=%s)\n",
                      esp_err_to_name(err));
    }

    free(burst);

    err = i2s_channel_reconfig_std_slot(rxHandle_, &single);
    if (err != ESP_OK) {
        log_e("[i2s-probe] restoring the configured slot failed: %s", esp_err_to_name(err));
    }
    if (enableChannel(rxHandle_, "rx")) {
        rxEnabled_ = true;
    }
}
#endif  // VM_I2S_SLOT_PROBE
