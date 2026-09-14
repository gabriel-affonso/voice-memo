#include "audio_capture.h"

#include "audio_codec_es8311.h"
#include "config.h"

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
    stdConfig.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_MONO
    );
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
