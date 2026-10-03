#pragma once

// VoiceMemo firmware for the Waveshare ESP32-S3-Touch-ePaper-1.54.
// Arduino IDE board selection: ESP32-S3 Dev Module.
//
// This Touch model has only one hardware version (stated in the official
// Waveshare repository README for ESP32-S3-ePaper-1.54: the V1/V2 distinction is
// for the non-touch ePaper family and is intentionally not used here).

// 0.5.0: microSD persistent queue for recordings and offline uploads.
// 0.4.1: battery power latch (GPIO17) asserted at the top of setup().
#define VM_FIRMWARE_VERSION "0.5.0"

// Set to 1 only after TEST A confirms microphone capture on the physical board.
// The three feature switches below can also be overridden from the command line
// (`--build-property compiler.cpp.extra_flags=-DVM_ENABLE_SD=0`), which is how
// the build variants are smoke-tested.
#ifndef VM_ENABLE_UPLOAD
#define VM_ENABLE_UPLOAD 1
#endif

// Set to 0 to build the pre-UI firmware (audio + upload only). The e-paper,
// touch, battery and RTC code still compiles; nothing drives the panel and the
// BOOT push-to-talk flow behaves exactly as before.
#ifndef VM_ENABLE_UI
#define VM_ENABLE_UI 1
#endif

// Set to 0 to build without the microSD queue. The firmware then behaves
// exactly as it did before this feature existed: one PSRAM WAV buffer that must
// be uploaded (or retried) before another recording can start. This is also the
// automatic runtime fallback whenever the card is absent, full or unhealthy.
#ifndef VM_ENABLE_SD
#define VM_ENABLE_SD 1
#endif

#define VM_SAMPLE_RATE 16000
#define VM_CHANNELS 1
#define VM_BITS_PER_SAMPLE 16
#define VM_BYTES_PER_SAMPLE (VM_BITS_PER_SAMPLE / 8)
#define VM_BYTES_PER_SECOND (VM_SAMPLE_RATE * VM_CHANNELS * VM_BYTES_PER_SAMPLE)

// Safety cap, not a feature limit. Recording is toggle-to-record, so this is
// only the net that catches a capture the user forgot to stop: reaching it
// finalizes the WAV and hands it to the normal store/upload pipeline exactly
// like a manual stop (nothing is ever discarded).
//
// The real ceiling is PSRAM, not this number: the whole note is captured into
// one PSRAM buffer before it is committed to the card, and the board's 8 MB
// hold (8 * 1024 * 1024 - VM_WAV_HEADER_BYTES) / VM_BYTES_PER_SECOND = 262 s
// of 16 kHz / 16-bit mono. 240 s needs 7 680 044 B and leaves ~700 KB of PSRAM
// free for the panel framebuffer and the rest of the runtime. Do not raise this
// without either checking `ESP.getPsramSize()` on the actual board or moving
// capture to a streaming write (see README, "PSRAM is a temporary capture
// buffer").
#define VM_MAX_RECORDING_SECONDS 240
#define VM_MAX_RECORDING_MS (VM_MAX_RECORDING_SECONDS * 1000UL)
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
// The upload/persist job runs on its own FreeRTOS task so RecordingApp::tick()
// keeps running and the BOOT button stays responsive while HTTP is in flight.
// 12 KB covers WiFiClient + String multipart framing over plain HTTP (no TLS),
// the VM_UPLOAD_CHUNK_BYTES streaming buffer (4 KB) and the deepest String
// temporaries, while the WAV (up to VM_MAX_WAV_BYTES, 7.7 MB) is never copied
// onto this stack: it is either in PSRAM or streamed from the card in chunks.
// Priority 1 matches the Arduino loop task: the worker must never outrank I2S
// audio capture. The task is deliberately NOT pinned to a core; it is almost
// always blocked on the socket, so it does not compete with audio.
#define VM_UPLOAD_TASK_STACK_BYTES 12288
#define VM_UPLOAD_TASK_PRIORITY 1

// ============================================================================
// microSD card - Waveshare ESP32-S3-Touch-ePaper-1.54 (SKU 34211/34212)
// ============================================================================
// The card is wired as **1-bit SDMMC**, not SPI. Official sources for THIS
// board (there is no separate touch-only repository):
//
//   waveshareteam/ESP32-S3-ePaper-1.54 @ 9957d0f4
//     02_Example/Arduino/04_SD_Card/sdcard_bsp.cpp
//         #define SDMMC_D0_PIN  GPIO_NUM_40
//         #define SDMMC_CLK_PIN GPIO_NUM_39
//         #define SDMMC_CMD_PIN GPIO_NUM_41
//         sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
//         slot_config.width = 1;                 // 1-wire SDMMC
//         slot_config.clk = SDMMC_CLK_PIN;
//         slot_config.cmd = SDMMC_CMD_PIN;
//         slot_config.d0  = SDMMC_D0_PIN;
//         host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
//     02_Example/ESP-IDF/V{1,2}/04_SD_Card/.../sdcard_bsp.c   (identical)
//     02_Example/ESP-IDF/V2/11_FactoryProgram/components/port_bsp/epaper_config.h
//         #define SD_MISO_D0_PIN  GPIO_NUM_40
//         #define SD_MOSI_CMD_PIN GPIO_NUM_41
//         #define SD_CLK_PIN      GPIO_NUM_39
//         #define SDlist "/sdcard"
//
// The official schematic (ESP32-S3-Touch-ePaper-1.54-Schematic.pdf) confirms
// the same three nets - IO39 SD_CLK, IO40 SD_MISO, IO41 SD_MOSI - and shows
// that DATA1/DATA2 are only routed to unpopulated resistors, DATA3/CS has a
// pull-up but no GPIO, card-detect is tied to GND, and the socket's VDD is on
// the always-on 3V3 rail. There is therefore NO chip-select, NO card-detect and
// NO SD power-enable GPIO to drive, and no pin overlaps with the e-paper (SPI2
// 6/8/9/10/11/12/13), touch/I2C (7/21/47/48) or audio (14/15/16/38/42/45/46).
//
// The Arduino `SD_MMC` wrapper drives exactly this ESP-IDF SDMMC driver, so
// `SD_MMC.setPins(39, 41, 40)` + `SD_MMC.begin("/sdcard", true)` is used. The
// official example calls `esp_vfs_fat_sdmmc_mount()` directly; the pin numbers,
// 1-bit width, mount point and clock are the official ones either way.
//
// NOTE: 39/40/41 are also the ESP32-S3 external-JTAG pins (MTCK/MTDO/MTDI).
// That is irrelevant unless the JTAG eFuses are burned; this board debugs over
// the built-in USB_SERIAL_JTAG on IO19/IO20.
#define VM_SD_MOUNT_POINT "/sdcard"
#define VM_SD_CLK_PIN 39
#define VM_SD_CMD_PIN 41
#define VM_SD_D0_PIN 40
#define VM_SD_FREQ_KHZ 40000  // SDMMC_FREQ_HIGHSPEED, as in the official example
#define VM_SD_MAX_OPEN_FILES 8

// Root of the persistent layout (see recording_paths.h). Never the mount point
// itself: the card may contain unrelated files.
#define VM_SD_ROOT "/voice_memo"

// Backpressure. A commit is refused (and the recording kept in PSRAM for the
// legacy path) when either limit would be crossed. VM_SD_MIN_FREE_BYTES is the
// reserve that must remain *after* the write, so the card always keeps room for
// its filesystem metadata; it is not a per-file limit.
#define VM_SD_MAX_PENDING 64
#define VM_SD_MIN_FREE_BYTES (1024ULL * 1024ULL)

// After an I/O error the volume is considered unhealthy and a full
// unmount+remount is retried at most this often. Deliberately slow: no polling
// storm while the card is out.
#define VM_SD_RETRY_MOUNT_MS 30000UL

// 0: after a confirmed upload, delete WAV + metadata (default policy).
// 1: move them to <root>/sent/ instead, which keeps the retention hook alive.
#define VM_SD_KEEP_SENT 0

// 1: re-hash every recovered pending WAV at boot and quarantine a CRC mismatch.
// Off by default: a max-length recording is ~7.7 MB, and hashing a full queue would
// add seconds to the boot. The cheap structural checks always run.
#define VM_SD_VERIFY_CRC_ON_BOOT 0

// 1: run the write/read/verify/remove self test in <root>/tmp on every boot.
// Bring-up only; leave 0 in production.
#define VM_SD_SELF_TEST 0

// Chunk size used to stream a WAV from the card into the multipart body. The
// whole file is never copied into RAM: the body is written chunk by chunk.
#define VM_UPLOAD_CHUNK_BYTES 4096

// Retry cadence for the persistent queue. Separate from the volatile
// VM_UPLOAD_RETRY_INTERVAL_MS so a slow link cannot couple the two paths.
#define VM_SD_QUEUE_RETRY_INTERVAL_MS 5000

// Attempts after which one queued recording stops being retried at the normal
// cadence: it is skipped so it cannot starve newer notes, and retried only at
// the slow backoff below. Nothing is ever deleted or quarantined for failing to
// upload - the recording stays on the card and keeps its id.
#define VM_SD_QUEUE_MAX_ATTEMPTS 10
#define VM_SD_QUEUE_MAX_BACKOFF_MS 600000UL  // 10 minutes

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

// ============================================================================
// PWR key polarity - CONFIRMED, not assumed
// ============================================================================
// GPIO18 (net BAT_KEY) is ACTIVE LOW: the key shorts the net to GND and the net
// is held up by a 10K pull-up (R58 to 3V3 on the official schematic,
// ESP32-S3-Touch-ePaper-1.54-Schematic.pdf) plus the ESP32-S3's internal
// pull-up. Pressed = LOW, released = HIGH.
//
// Two independent official sources agree:
//   1. 02_Example/Arduino/07_BATT_PWR_Test/src/button_bsp/button_bsp.c
//        #define button2_active 0        // PWR: active level = 0 (LOW)
//        gpio_conf.mode = GPIO_MODE_INPUT;
//        gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
//        gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
//      and the same file reads the key with gpio_get_level(PWR_BUTTON_PIN).
//   2. 02_Example/Arduino/11_RTC_Sleep_Test/src/power/board_power_bsp.cpp
//        const uint64_t ext_wakeup_pwr_mask = 1ULL << GPIO_NUM_18;
//        esp_sleep_enable_ext1_wakeup_io(..., ESP_EXT1_WAKEUP_ANY_LOW);
//      i.e. Waveshare itself wakes the chip when GPIO18 goes LOW.
//
// NOTE (recorded so nobody re-derives it wrongly): 07_BATT_PWR_Test's
// user_app.cpp contains `vbat_scanStatus()` which sets its arming flag when
// gpio_get_level(PWR_BUTTON_PIN) is non-zero, and `button_power_task()` which
// powers off when that flag is set. That pair is inverted relative to the
// button polarity declared in the very same example, so it is self-
// contradictory; the schematic, the active-low declaration and the EXT1
// ANY_LOW wake source are the consistent evidence and are what this firmware
// follows. Validate on hardware with TEST B (README).
//
// Guarded so the polarity can be flipped from the command line during physical
// validation instead of editing this file:
//     --build-property compiler.cpp.extra_flags=-DVM_PWR_KEY_ACTIVE_LOW=0
// The #ifndef is also what keeps "macro redefined" out of a -Warnings all build
// when that override is used.
#ifndef VM_PWR_KEY_ACTIVE_LOW
#define VM_PWR_KEY_ACTIVE_LOW 1
#endif

// Debounce window for the PWR key. The official example runs the multi_button
// library from a 5 ms esp_timer tick with its default 50 ms debounce window, so
// 50 ms is the vendor-consistent value rather than a guess. It is comfortably
// above the mechanical bounce of a tactile switch (a few ms) and well below any
// deliberate human press (>= 150 ms).
#define VM_PWR_DEBOUNCE_MS 50

// ============================================================================
// Automatic power-off after inactivity
// ============================================================================
// "Inactivity" means no *user* interaction. Background work (Wi-Fi, NTP, RTC,
// uploads, e-paper refreshes, SD remounts, log lines) never resets the timer;
// see power_policy.h for the single place that classification lives.
// The #ifndef guard is load-bearing: `--build-property
// compiler.cpp.extra_flags=-DVM_ENABLE_AUTO_POWER_OFF=0` overrides it, exactly
// like the VM_ENABLE_SD / VM_ENABLE_UI / VM_ENABLE_UPLOAD switches above.
#ifndef VM_ENABLE_AUTO_POWER_OFF
#define VM_ENABLE_AUTO_POWER_OFF 1
#endif
#define VM_AUTO_POWER_OFF_MS 120000UL

// Manual PWR short press always requests a graceful shutdown, independent of
// the inactivity timeout.
#ifndef VM_ENABLE_MANUAL_POWER_OFF
#define VM_ENABLE_MANUAL_POWER_OFF 1
#endif

// Bounded waits in the shutdown path. Every one is a deadline, never a delay()
// in normal operation; the shutdown path is allowed to block briefly because
// the device is going away and nothing else needs the CPU.
//   * how long a freshly painted POWERED OFF screen may take
#define VM_PWR_SHUTDOWN_EPD_TIMEOUT_MS 6000UL
//   * how long an interrupted background upload may take to notice the abort
//     flag and hand the worker back. The flag is polled between 4 KB chunks, so
//     this only has to cover one in-flight HTTP write.
#define VM_PWR_UPLOAD_ABORT_TIMEOUT_MS 500UL
//   * how long the SD volume may take to become releasable at shutdown. A commit
//     is never interrupted, so a slower-than-expected one is waited out here;
//     the bound only exists so a wedged card cannot hang the power-off.
#define VM_PWR_SHUTDOWN_CARD_RELEASE_TIMEOUT_MS 2000UL
//   * how long the main loop is serviced (button, outcome drain) between
//     shutdown polls.
#define VM_PWR_SHUTDOWN_POLL_MS 10UL

// Deep-sleep wake source for the PWR key. When 1, powerOff() releases the
// battery latch and then enters deep sleep with GPIO18 (an ESP32-S3 RTC GPIO)
// armed for EXT1 wake on LOW. On battery the latch release removes power before
// the sleep even starts; on USB, where VBUS keeps the board alive, deep sleep
// is what makes the device actually behave as "off" while the e-paper keeps its
// last image. This is exactly what the official 11_RTC_Sleep_Test example does
// with the same pin (see the polarity note above).
//
// The #ifndef guard is load-bearing, like the other feature switches: with
// `-DVM_PWR_WAKE_ON_PWR=0` the deep sleep is skipped and only the latch is
// released, which leaves the device running on USB. Without the guard that
// override produced a "macro redefined" warning and the override silently lost.
#ifndef VM_PWR_WAKE_ON_PWR
#define VM_PWR_WAKE_ON_PWR 1
#endif

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

// Minimum time a power notice ("STOP RECORDING FIRST") stays on the panel after
// its condition has cleared. The notice is otherwise event-driven: it is shown
// because something happened, not because a timer asked for it.
#define VM_UI_POWER_NOTICE_MIN_HOLD_MS 1200

// Battery is sampled at most this often, and re-rendered only on a change of at
// least VM_UI_BATTERY_RENDER_DELTA percent.
#define VM_UI_BATTERY_INTERVAL_MS 30000
#define VM_UI_BATTERY_RENDER_DELTA 5

// RTC is re-read at most this often (the panel cannot show seconds anyway).
#define VM_UI_CLOCK_INTERVAL_MS 10000

// Upper bound for waiting on the panel BUSY line before declaring a fault, so a
// disconnected or unpowered panel can never hang the firmware forever.
#define VM_EPD_BUSY_TIMEOUT_MS 5000

