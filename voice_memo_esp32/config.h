#pragma once

// VoiceMemo firmware for the Waveshare ESP32-S3-Touch-ePaper-1.54.
// Arduino IDE board selection: ESP32-S3 Dev Module.
//
// This Touch model has only one hardware version (stated in the official
// Waveshare repository README for ESP32-S3-ePaper-1.54: the V1/V2 distinction is
// for the non-touch ePaper family and is intentionally not used here).

// 0.4.1: battery power latch (GPIO17) asserted at the top of setup().
#define VM_FIRMWARE_VERSION "0.4.1"

// Set to 1 only after TEST A confirms microphone capture on the physical board.
#define VM_ENABLE_UPLOAD 1

// Set to 0 to build the pre-UI firmware (audio + upload only). The e-paper,
// touch, battery and RTC code still compiles; nothing drives the panel and the
// BOOT push-to-talk flow behaves exactly as before.
#define VM_ENABLE_UI 1

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

// ============================================================================
// I2S RX slot selection (the ES8311 ADC lives in the LEFT slot)
// ============================================================================
// On ESP32-S3, `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(..., I2S_SLOT_MODE_MONO)`
// does NOT mean "one slot per frame". The macro has three branches and only the
// ESP32 / ESP32-S2 ones contain the `MONO ? I2S_STD_SLOT_LEFT : BOTH` logic;
// ESP32-S3 falls into the `#else` (SOC_I2S_HW_VERSION_2) branch, which hard
// codes `.slot_mask = I2S_STD_SLOT_BOTH` and ignores the mono/stereo argument.
// Verified against the header shipped with core 3.3.11 and with upstream
// ESP-IDF v5.5.5 (byte-identical):
//
//   components/esp_driver_i2s/include/driver/i2s_std.h
//     #if CONFIG_IDF_TARGET_ESP32      -> slot_mask = MONO ? LEFT : BOTH
//     #elif CONFIG_IDF_TARGET_ESP32S2  -> slot_mask = MONO ? LEFT : BOTH
//     #else                            -> slot_mask = I2S_STD_SLOT_BOTH
//
// The ES8311 is a mono codec whose ADC is routed to the LEFT slot: reg 0x44 is
// written 0x58, i.e. ADCDAT_SEL = 0b101 = "ADC + DACR" (left = microphone,
// right = DAC reference; the DAC is muted here, so the right slot is exactly
// zero). Because the macro left slot_mask at BOTH, the RX peripheral stored
// both slots of every 16 kHz LRCK frame, so the DMA stream became
// [ADC, 0, ADC, 0, ...] and the mono WAV ended up with two samples per real
// frame: 32 000 samples/s written as if they were 16 000, hence half speed.
//
// Selecting the slot explicitly makes the driver deliver exactly one sample per
// frame at VM_SAMPLE_RATE. Ordering note (I2S Philips, ws_pol = false): the
// first slot of a frame is the LEFT one, and it is the low 16 bits of the
// little-endian RX word, which is why the LEFT choice is also what the measured
// recording shows (audio in the even int16 positions, every odd one zero).
#define VM_I2S_RX_SLOT_LEFT 1
#define VM_I2S_RX_SLOT_RIGHT 0
#if (VM_I2S_RX_SLOT_LEFT + VM_I2S_RX_SLOT_RIGHT) != 1
#error "select exactly one I2S RX slot: VM_I2S_RX_SLOT_LEFT or VM_I2S_RX_SLOT_RIGHT"
#endif

// Temporary bring-up probe. When 1, AudioCapture::begin() grabs a short stereo
// burst before the first recording and prints, per slot, the zero ratio, RMS and
// peak plus the correlation and MAE between the two slots. It runs once, from
// the main loop, and does not touch the recording path. Keep at 0 for normal
// firmware; see "Audio capture: the I2S RX slot is pinned to LEFT" in README.md
// for the procedure.
#define VM_I2S_SLOT_PROBE 0

// ES8311 ADC register value for microphone gain. The official driver exposes
// gain enum values; 0x04 corresponds to the 24 dB setting in the ESP-ADF table.
#define VM_ES8311_MIC_GAIN_REG 0x04

// Wi-Fi fail-over timing for the ordered list of known networks in secrets.h
// (primary first, then the fallbacks). Every value is a deadline checked
// against millis() from loop(); none of them introduces a delay().
//
// IMPORTANT: an attempt is ended ONLY by WL_CONNECTED or by running out this
// full window. A driver failure status must never shorten it: the ESP32 core
// maps WIFI_REASON_NO_AP_FOUND / ASSOC_FAIL / AUTH_FAIL to WL_NO_SSID_AVAIL /
// WL_CONNECT_FAILED and retries the same network itself.
//
// The core's autoReconnect (enabled by default) and the multi-SSID selector are
// two reconnect machines fighting for the same station: the core re-runs
// esp_wifi_connect() for the old SSID while the selector wants to move on.
// WifiManager::begin() therefore calls WiFi.setAutoReconnect(false) and the
// selector is the single reconnect authority.
//
// That also means a switch is an explicit two-phase operation, never
// "timeout -> WiFi.begin(otherSsid)": the STA is disconnected first
// (WiFi.disconnect(false, false, 0) - radio stays on, credentials/NVS are never
// erased, no blocking wait), and the next WiFi.begin() is deferred until the
// driver has actually left the connecting state. Until then
// esp_wifi_set_config() rejects the new configuration with
// "sta is connecting, cannot set config", which is exactly why the old
// timeout -> begin(other) path never came up.
//
// A single network attempt (primary or fallback) gets this long. It has to
// cover scan + association + DHCP on a busy or distant AP.
#define VM_WIFI_CONNECT_TIMEOUT_MS 10000
// Safety deadline for the explicit disconnect that precedes every switch. The
// ARDUINO_EVENT_WIFI_STA_DISCONNECTED event proving the driver left the
// connecting state normally arrives within tens of milliseconds; this bound
// only covers a station that has nothing to disconnect (for example one that
// was scanning and never associated). It is a deadline like every other value
// here - never a delay().
#define VM_WIFI_DISCONNECT_TIMEOUT_MS 1500
// Offline pause after every known network timed out, before the cycle restarts
// at the primary, so a network that comes back is rejoined automatically
// without hammering the radio.
#define VM_WIFI_RETRY_INTERVAL_MS 10000
#define VM_UPLOAD_RETRY_INTERVAL_MS 5000
#define VM_UPLOAD_TIMEOUT_MS 15000

// Background upload task (VM_ENABLE_UPLOAD=1 only).
// The upload runs on its own FreeRTOS task so RecordingApp::tick() keeps
// running and the BOOT button stays responsive while HTTP is in flight.
// 8 KB is conservative for WiFiClient + String multipart framing over plain
// HTTP (no TLS) and keeps the 1.44 MB WAV in PSRAM, never on this stack.
// Priority 1 matches the Arduino loop task: the uploader must never outrank
// I2S audio capture. The task is deliberately NOT pinned to a core; it is
// almost always blocked on the socket, so it does not compete with audio.
#define VM_UPLOAD_TASK_STACK_BYTES 8192
#define VM_UPLOAD_TASK_PRIORITY 1

// ============================================================================
// UI hardware - every value below comes from the official Waveshare sources for
// the ESP32-S3-Touch-ePaper-1.54 and is not guessed:
//
//   waveshareteam/ESP32-S3-ePaper-1.54 @ main
//     02_Example/Arduino/*/user_config.h        -> EPD / touch / power pins
//     02_Example/Arduino/12_FT6336_Test/        -> FT6336 I2C address + reset
//     02_Example/Arduino/01_ADC_Test/adc_bsp.cpp-> battery ADC channel + /2
//     01_Arduino_Libraries/SensorLib/src/REG/PCF85063Constants.h -> RTC
//     02_Example/Arduino/08_Audio_Test/src/codec_board/board_cfg.h
//                                               -> the audio/I2C pins already
//                                                  used above ("S3_ePaper_1_54")
//
// The official repo README states that ESP32-S3-ePaper-1.54 has two hardware
// versions (V1/V2) while ESP32-S3-Touch-ePaper-1.54 - the board used here - has
// only one. There is therefore no V2 pin map to switch to.
// ============================================================================

// 1.54 inch 200x200 monochrome e-paper on SPI2 (EPD_SPI_NUM = SPI2_HOST).
#define VM_EPD_SPI_HOST 2
#define VM_EPD_DC_PIN 10
#define VM_EPD_CS_PIN 11
#define VM_EPD_SCK_PIN 12
#define VM_EPD_MOSI_PIN 13
#define VM_EPD_RST_PIN 9
#define VM_EPD_BUSY_PIN 8
// Panel power enable, active LOW (POWEER_EPD_ON() drives it to 0).
#define VM_EPD_PWR_PIN 6
#define VM_EPD_SPI_CLOCK_HZ 40000000

// FT6336 capacitive touch: shares the ES8311 I2C bus (SDA 47 / SCL 48).
#define VM_TOUCH_I2C_ADDRESS 0x38
#define VM_TOUCH_RST_PIN 7
#define VM_TOUCH_INT_PIN 21

// ============================================================================
// Battery power latch (BAT_Control / VBAT_PWR) - GPIO17
// ============================================================================
// On battery the board is alive only while the PWR key is held DOWN or the
// latch is asserted by the firmware. The latch is a MOSFET whose gate is
// GPIO17; the PWR key (BAT_KEY, GPIO18) is a momentary switch that powers the
// rails just long enough for the firmware to take over. If GPIO17 is never
// driven HIGH the rail collapses the instant the key is released, which looks
// exactly like "works on USB, dead on battery": USB feeds the rails
// independently, so the latch is only ever exercised on battery.
//
// Official Waveshare sources for this product, followed verbatim:
//
//   waveshareteam/ESP32-S3-ePaper-1.54 @ main
//     02_Example/Arduino/07_BATT_PWR_Test/user_config.h
//         #define VBAT_PWR_PIN    GPIO_NUM_17
//         #define PWR_BUTTON_PIN  GPIO_NUM_18
//     02_Example/Arduino/07_BATT_PWR_Test/src/power/board_power_bsp.cpp
//         VBAT_POWER_ON()  -> gpio_set_level(vbat_power_pin, 1)
//         VBAT_POWER_OFF() -> gpio_set_level(vbat_power_pin, 0)
//         (its constructor enables the pin's internal pull-up as well)
//
// GPIO17 HIGH therefore *maintains* battery power; GPIO17 LOW releases it.
// GPIO18 is the PWR key itself: it is an input and must NEVER be driven to hold
// the rail. The actual latch handling lives in board_power.{h,cpp}.
#define VM_VBAT_PWR_PIN 17
#define VM_PWR_KEY_PIN 18

// Battery rail: ADC1 channel 3 = GPIO4, 12 dB attenuation, /2 divider.
#define VM_BATTERY_ADC_CHANNEL 3
#define VM_BATTERY_DIVIDER_RATIO 2

// PCF85063 real-time clock on the same I2C bus.
#define VM_RTC_I2C_ADDRESS 0x51

// ============================================================================
// Clock: the PCF85063 holds UTC, the UI shows Europe/Lisbon local time
// ============================================================================
//
// The RTC chip has no notion of a timezone, an offset or daylight saving, so it
// is used strictly as a UTC source. This POSIX TZ string is the mainland-Portugal
// rule (Europe/Lisbon):
//
//   WET0WEST       std name WET, UTC+0; dst name WEST, UTC+1 (POSIX default +1)
//   ,M3.5.0/1      DST starts the last Sunday of March at 01:00 std (UTC)
//   ,M10.5.0/2     DST ends   the last Sunday of October at 02:00 dst (= 01:00 UTC)
//
// It is the same rule tzdata applies to Europe/Lisbon
// (https://github.com/nayarsystems/posix_tz_db, row "Europe/Lisbon"), so the
// WET/WEST switch happens automatically on the official dates. Nothing in the
// firmware stores a fixed +1 hour.
#define VM_TIME_TZ "WET0WEST,M3.5.0/1,M10.5.0/2"

// Both servers are only contacted through SNTP, which configTzTime() starts
// without blocking: the firmware never waits for an answer.
#define VM_TIME_NTP_SERVER_1 "pool.ntp.org"
#define VM_TIME_NTP_SERVER_2 "time.google.com"

// While the system clock is still the 1970 cold start the NTP request is
// restarted at most this often; once NTP is valid it is left alone and only
// restarted every VM_TIME_NTP_RESYNC_INTERVAL_MS (6 h).
#define VM_TIME_NTP_RETRY_INTERVAL_MS 60000UL
#define VM_TIME_NTP_RESYNC_INTERVAL_MS 21600000UL

// A healthy PCF85063 is only rewritten when it disagrees with the
// NTP-disciplined system clock by more than this many seconds, so the I2C bus
// stays quiet and the RTC is still corrected after long offline periods.
#define VM_TIME_RTC_SYNC_TOLERANCE_S 2

// ============================================================================
// UI timing / refresh policy (engineering choices, documented in README.md)
// ============================================================================

// Touch is polled from the main loop, never from an ISR or a new task.
#define VM_UI_TOUCH_POLL_INTERVAL_MS 50

// Recording timer: at most one partial refresh per second, and only while the
// panel is idle. Partials are asynchronous, so audio capture is never stalled.
#define VM_UI_RECORDING_TIMER_INTERVAL_MS 1000

// Ghosting bound: after this many partial refreshes the next refresh that is
// allowed to block (state != Recording) is promoted to a full refresh. The
// Waveshare demo never schedules a periodic full refresh; 20 is our choice,
// balancing visible ghosting against the flashing cost of a full update.
#define VM_UI_FULL_REFRESH_EVERY_PARTIALS 20

// SENT confirmation and BUSY hint durations. Measured from the moment the
// partial refresh was *started*, because the panel needs ~0.3-0.5 s to show it.
#define VM_UI_SENT_HOLD_MS 1500
#define VM_UI_BUSY_HINT_MS 1000

// Battery is sampled at most this often, and re-rendered only on a change of at
// least VM_UI_BATTERY_RENDER_DELTA percent.
#define VM_UI_BATTERY_INTERVAL_MS 30000
#define VM_UI_BATTERY_RENDER_DELTA 5

// RTC is re-read at most this often (the panel cannot show seconds anyway).
#define VM_UI_CLOCK_INTERVAL_MS 10000

// Upper bound for waiting on the panel BUSY line before declaring a fault, so a
// disconnected or unpowered panel can never hang the firmware forever.
#define VM_EPD_BUSY_TIMEOUT_MS 5000

