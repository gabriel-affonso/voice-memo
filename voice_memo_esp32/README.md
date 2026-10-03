# VoiceMemo ESP32 Arduino project

Self-contained Arduino IDE project for:

```text
Waveshare ESP32-S3-Touch-ePaper-1.54
```

Board selection in Arduino IDE:

```text
ESP32-S3 Dev Module
```

This is the real-hardware microphone validation firmware. It uses the same
ESP32 -> ES8311 -> NUC audio path as the server project. Since `v0.5.0` it also
turns the board's microSD slot into a persistent queue: a finalized recording is
committed to the card before the PSRAM buffer is reused, so a Wi-Fi loss, a
reboot or a failed upload cannot lose it, and new notes can be recorded while
older ones are still waiting to be sent. See
[microSD persistence and the offline queue](#microsd-persistence-and-the-offline-queue-v050).

## Hardware version: this Touch model has only one

The V1/V2 discussion applies to the **non-touch** `ESP32-S3-ePaper-1.54` family.
The Touch board has a single hardware revision, so there is no V2 pin map to
select. Source, the official repository for this product
(`waveshareteam/ESP32-S3-ePaper-1.54`, README.md):

```text
- The ESP32-S3-ePaper-1.54 has two versions, namely V1 and V2. Starting from
  November 1, 2025, the V2 version will be replaced and the V1 version will no
  longer be sold.
- ESP32-S3-Touch-ePaper-1.54 has only one version.
```

All pin numbers used by the firmware (audio, I2C, e-paper, touch, battery) come
from that repository's official examples and board definition, never from a
guess. The mapping and its provenance are listed in the
[Hardware map](#hardware-map-official-sources) section below.

## Required files

Arduino IDE needs the `.ino` file and all local `.h` / `.cpp` files in the
same sketch directory. Keep them together when copying.

Local-only ignored file:

```text
secrets.h
```

It must not be committed.

## Mac / Arduino IDE setup

1. Copy the whole directory to the Mac:

```bash
cp -R /opt/voice-memo/firmware/voice_memo_esp32 ~/Desktop/voice_memo_esp32
```

2. Open Arduino IDE.

3. Open:

```text
voice_memo_esp32/voice_memo_esp32.ino
```

4. Select board:

```text
Tools -> Board -> ESP32 Arduino -> ESP32-S3 Dev Module
```

5. Recommended Tools settings:

```text
USB CDC On Boot: Enabled
Flash Size: 8MB (or match the installed board)
PSRAM: OPI PSRAM
Upload Speed: 921600
USB Mode: Hardware CDC and JTAG
```

If the installed board is the 8MB/8MB model, use:

```text
Flash Size: 8MB
PSRAM: OPI PSRAM
```

If the exact installed flash size is unknown, start with the Arduino IDE
default for `ESP32-S3 Dev Module` and check Serial output after upload.

6. Create local secrets:

```bash
cp secrets.example.h secrets.h
```

Then edit `secrets.h`:

```text
WIFI_SSID
WIFI_PASSWORD
WIFI_FALLBACK_SSID
WIFI_FALLBACK_PASSWORD
INGEST_URL
INGEST_TOKEN
DEVICE_ID
```

`WIFI_FALLBACK_SSID` / `WIFI_FALLBACK_PASSWORD` are optional: they describe a
second known network the firmware falls back to when the primary one is
unavailable. Leave the password empty (`""`) for an open network, and see
[Wi-Fi fail-over](#wi-fi-fail-over-primary--fallback) below.

For TEST A, Wi-Fi credentials can remain empty because upload is disabled by
default in `config.h` with:

```cpp
#define VM_ENABLE_UPLOAD 0
```

`secrets.h` is ignored via `.gitignore`. Only `secrets.example.h` (placeholders)
belongs in the repository. Verify with:

```bash
git check-ignore -v secrets.h
```

7. Connect the board over USB.

8. Select the USB port under `Tools -> Port`.

9. Compile:

```text
Sketch -> Verify/Compile
```

10. Upload:

```text
Sketch -> Upload
```

11. Open Serial Monitor.

12. Set baud rate to:

```text
115200
```

## TEST A: microphone only

1. Keep this default in `config.h`:

```cpp
#define VM_ENABLE_UPLOAD 0
```

2. Compile and upload.

3. Open Serial Monitor at `115200`.

4. Press and HOLD the BOOT button.

5. Speak normally while holding BOOT.

6. Release BOOT.

7. Expected useful output:

```text
[boot] VoiceMemo ESP32 firmware ...
[boot] PSRAM detected=yes total=... free=...
[es8311] detected id1=... id2=...
[es8311] initialized for microphone capture
[boot] I2S/ES8311 initialization success
[boot] TEST_A mode: microphone-only; upload disabled
[app] BOOT press
[app] recording started id=...
[app] recording bytes=...
[app] BOOT release
[app] recording stopped ...
[app] audio_peak=<non-zero> audio_rms=<non-zero>
[app] WAV finalized ...
```

A working microphone should produce an `audio_peak` clearly above the silent
threshold. If the peak is near zero, the firmware prints:

```text
[app] MIC AUDIO APPEARS SILENT
```

TEST A has no Wi-Fi or NUC dependency. With `VM_ENABLE_UPLOAD 0` the firmware
never calls `WiFi.begin()` and never contacts the ingress. What happens to the
WAV after the serial summary depends on the card:

```text
# card present (v0.5.0 default): persisted, and nothing is uploaded
[store] saving id=... bytes=...
[store] committed id=... bytes=... crc=...
[queue] pending=1

# no card: the pre-microSD behaviour, the WAV is dropped
[app] TEST_A upload disabled; recording retained only for serial summary
```

Because TEST A never uploads, a card left in the board accumulates pending notes
until `VM_SD_MAX_PENDING` refuses more. That is intentional (nothing is deleted
silently), but set `VM_ENABLE_SD 0` for a pure microphone bring-up, or clear
`/voice_memo/pending` on a computer afterwards.

Wi-Fi credentials and `INGEST_URL` may stay empty or unset for TEST A.

### Compile from the command line

Board selection is `ESP32-S3 Dev Module` with 8 MB flash and OPI PSRAM. Use the
8 MB partition scheme so the app image has room (with the UI, TEST B is ~978 KB,
which is 81% of the 1.2 MB app partition in the default 4 MB layout):

```bash
ARDUINO_CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"

"$ARDUINO_CLI" compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=8M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=default_8MB" \
  --build-path /tmp/vm_build_path \
  --output-dir /tmp/vm_build_out \
  --warnings all \
  .
```

Use `--build-path` inside a writable directory so the Arduino cache is not
touched. The same sketch also compiles with `PartitionScheme=default` (4 MB
layout); it fits, but with much less headroom.

Measured sizes with arduino-cli 1.5.1 and core 3.3.11 (v0.6.0, Frank included):

| Build | Flash | Static RAM |
| --- | --- | --- |
| `VM_ENABLE_UPLOAD 1`, `VM_ENABLE_UI 1`, `VM_ENABLE_SD 1` (default) | 1 164 439 B (34%) | 50 624 B (15%) |
| `VM_ENABLE_UPLOAD 1`, `VM_ENABLE_UI 1`, `VM_ENABLE_SD 0` | 1 067 927 B (31%) | 49 128 B (14%) |
| `VM_ENABLE_UPLOAD 1`, `VM_ENABLE_UI 0`, `VM_ENABLE_SD 1` | 1 070 855 B (32%) | 50 076 B (15%) |
| `VM_ENABLE_UPLOAD 0`, `VM_ENABLE_UI 1`, `VM_ENABLE_SD 1` | 678 556 B (20%) | 34 584 B (10%) |
| `VM_ENABLE_UPLOAD 0`, `VM_ENABLE_UI 1`, `VM_ENABLE_SD 0` (TEST A) | 587 268 B (17%) | 33 104 B (10%) |

Every row is measured with `--warnings all` and zero warnings.

The Frank asset pack (v0.6.0) is 44 992 B of `const` data - eight 200x200 frames at
5000 B plus eight 96x52 overlays at 624 B - plus ~1 KB of code, so it adds about
45 KB of flash (~1.4%) and **no** static RAM: the frames are read from flash in
place and the only framebuffer stays the driver's 5000-byte one. Note the
`VM_ENABLE_UI 0` row: with the UI switched off nothing references the face module,
so `--gc-sections` discards the whole pack and the feature costs nothing there.
Earlier v0.5.0 numbers were 1 118 647 / 1 020 963 / 1 072 055 / 632 748 / 540 264 B
for the same five rows.

The v0.5.0 power management feature added about 15 KB of
flash (~1.4%) and ~230 B of static RAM over the microSD-only build: one debounced
key, one activity timer, the inhibit policy and three extra screens.

The microSD feature adds ~97 KB of flash over v0.4.1 (almost all of it the
SDMMC + FAT VFS driver) and ~1.6 KB of static RAM. The background worker's stack
grew from 8 KB to 12 KB to cover the 4 KB streaming chunk; the WAV itself is
never copied onto it.

## Audio capture: the I2S RX slot is pinned to LEFT

A recording that sounded roughly 2x slower and one octave lower than the speaker
was traced to the I2S receive configuration, not to the WAV, the ES8311 gain or
the sample rate.

**Root cause.** `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_MONO)`
does not mean "one slot per frame" on ESP32-S3. The macro has three branches and
only the ESP32 / ESP32-S2 ones contain the mono logic:

```c
#if CONFIG_IDF_TARGET_ESP32        /* and #elif CONFIG_IDF_TARGET_ESP32S2 */
    .slot_mask = (mono_or_stereo == I2S_SLOT_MODE_MONO) ?
                I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH,
#else                              /* ESP32-S3, ESP32-C3/C6, ... */
    .slot_mask = I2S_STD_SLOT_BOTH,
#endif
```

The header is byte-identical to upstream ESP-IDF v5.5.5 in
`components/esp_driver_i2s/include/driver/i2s_std.h` (lines 31-36 and 140-150).
So on this board the old code silently produced `slot_mask = I2S_STD_SLOT_BOTH`,
and the RX peripheral stored **both** slots of every 16 kHz LRCK frame. The
driver meanwhile sizes the DMA buffer and counts samples for one slot
(`i2s_std.c`: `active_slot = slot_mode == MONO ? 1 : 2`, `i2s_common.c`:
`bytes_per_frame = bytes_per_sample * active_slot`), so the stream carried twice
as many samples per second as the 16 kHz WAV header claimed.

**Which slot carries the microphone: LEFT.** Three independent sources agree.

* The ES8311 ADC is routed to the left slot. `audio_codec_init()` writes
  `REG 0x44 = 0x58` (same as ESP-ADF's `es8311.c`, comment
  `/* set internal reference signal (ADCL + DACR) */`). Bits[6:4] are
  `ADCDAT_SEL`, and `0b101 = 5` is the Linux `es8311` AIF1TX source
  `"ADC + DACR"`: left = microphone, right = DAC reference. The DAC is muted in
  this firmware, so the right slot is exactly zero.
* Waveshare's official `08_Audio_Test` for this board uses `esp_codec_dev`, whose
  mono default is `ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0)` == channel 0 ==
  `I2S_STD_SLOT_LEFT`; the stock app reads stereo 16-bit frames and keeps bytes
  0-1 (the left word) as the microphone.
* Measured on the real 18.08 s recording: **every odd 16-bit sample was exactly
  0** (144 640 of 144 640 RX words had a zero high half) and the audio sat in the
  even positions. The left slot is the first slot of an I2S Philips frame
  (`ws_pol = false`) and the low 16 bits of the little-endian RX word.

`audio_capture.cpp` therefore builds the slot configuration explicitly and pins
`.slot_mask` to `I2S_STD_SLOT_LEFT` (configurable through
`VM_I2S_RX_SLOT_LEFT` / `VM_I2S_RX_SLOT_RIGHT` in `config.h`), with a
`static_assert` that fails the build if the mask is ever `BOTH` again. The
Arduino-ESP32 core applies the same workaround in its own `ESP_I2S` library:

```c
  // Newer targets default to SLOT_BOTH even in mono, which doubles each
  // sample in the DMA buffer.  Force SLOT_LEFT for true mono ...
```

Nothing else changed: the sample rate is still 16000 Hz, the WAV is still mono
PCM 16-bit, the ES8311 gain, upload, UI, Wi-Fi and the PSRAM buffer are
untouched.

### Host-side check on an existing WAV

`tools/wav_slot_diagnostic.py` applies the same test that identified the bug:
per-slot zero ratio / RMS / peak, the correlation and MAE between `x[0::2]` and
`x[1::2]`, and the fraction of frames where `x[2i] == x[2i+1]`.

```bash
python3 tools/wav_slot_diagnostic.py recording.wav
python3 tools/wav_slot_diagnostic.py recording.wav --decimate fixed.wav
```

For the buggy 18.08 s recording it prints `slot2 zeros=100.0000%`,
`correlation = 0.000000` and the verdict *"slot 1 carries the audio ... this is
`I2S_STD_SLOT_LEFT`"*. A healthy post-fix recording must show a non-zero RMS on
**both** halves unless the two halves are simply adjacent samples of a
band-limited signal.

### Physical test script (post-fix)

1. Record with BOOT held for ~10 s and note `duration_ms` from
   `[app] recording stopped ...`.
2. `duration_ms` must be within a few hundred ms of the real press time (it used
   to be ~2x), and the uploaded WAV must be ~10 s, not ~20 s.
3. Play the WAV as-is. It must sound normal with no `x[::2]` decimation and no
   32 kHz header edit.
4. Check `[app] audio_peak` / `audio_rms` are still non-zero, and that a 45 s
   hold still reports `duration_ms = 45000`.
5. To settle LEFT vs RIGHT on the bench, build once with `VM_I2S_SLOT_PROBE 1`.
   At boot it captures a 4096-frame stereo burst before the first recording and
   prints per slot:

   ```text
   [i2s-probe] frames=4096 slotA(left)=zeros 0.00% rms=... peak=... | slotB(right)=zeros 100.00% rms=0.0 peak=0
   [i2s-probe] correlation(slotA,slotB)=... MAE=...
   [i2s-probe] slotA (LEFT) carries the ES8311 ADC; keep VM_I2S_RX_SLOT_LEFT
   ```

   The probe restores the production single-slot configuration before returning,
   so a recording made right after boot is unaffected. If the log instead names
   `slotB (RIGHT)`, build with `VM_I2S_RX_SLOT_RIGHT 1` (`VM_I2S_RX_SLOT_LEFT 0`)
   as Build B and re-test; the wrong slot yields silence or the muted DAC
   reference.

## Interpreting hardware problems

Likely ES8311/I2C problem if you see:

```text
[es8311] no ACK on I2C bus
[boot] I2S/ES8311 initialization failed
```

Likely I2S/data-direction problem if ES8311 initializes but audio remains
silent:

```text
[es8311] detected ...
[es8311] initialized for microphone capture
[app] audio_peak=0 audio_rms=0
[app] MIC AUDIO APPEARS SILENT
```

In that case verify GPIO16 is used for ESP32 I2S RX and GPIO45 is used for
ESP32 I2S TX. Do not swap them.

## Battery power latch and boot order (v0.4.1)

On battery the board does **not** stay on by itself. The PWR key (`BAT_KEY`,
`GPIO18`) is a momentary switch: it connects the battery just long enough for the
firmware to run, and the firmware must then drive the latch gate - `GPIO17`
(`BAT_Control` / `VBAT_PWR`) - HIGH. Official Waveshare semantics, followed
verbatim (`02_Example/Arduino/07_BATT_PWR_Test/{user_config.h,src/power/board_power_bsp.cpp}`):

| Official call | Effect |
| --- | --- |
| `VBAT_POWER_ON()` | `gpio_set_level(vbat_power_pin, 1)` - **holds** battery power |
| `VBAT_POWER_OFF()` | `gpio_set_level(vbat_power_pin, 0)` - releases it |

`BoardPower` (`board_power.{h,cpp}`) wraps those operations and nothing else:

| Method | Role |
| --- | --- |
| `keepBatteryPowerOn()` | **the battery boot critical path**: drops a stale RTC pad hold on `GPIO17`, configures it as an output and drives it HIGH. First statement of `setup()`; no logging, no `delay()`, no bus access |
| `releaseSleepPadsIfNeeded()` | hands `GPIO18` back from the RTC wake pad to the digital GPIO matrix and reports whether this reset was a deep-sleep wake. Runs immediately after the latch, before `Serial.begin()` |
| `begin()` | re-asserts the latch and prints the one-shot `[power]` lines |
| `powerOff()` | drives `GPIO17` LOW, holds it LOW through sleep, arms the PWR deep-sleep wake source and enters deep sleep; called once by the shutdown sequence (v0.5.0) |
| `latchOn()` | true once the latch has been asserted |

`GPIO18` is the PWR key itself. It is an input and is never driven as an output;
the firmware reads it and, since v0.5.0, arms it as the deep-sleep wake source.

### Boot order

`keepBatteryPowerOn()` is the **first instruction of `setup()`** - the same
property v0.4.1 had - and nothing is allowed before it. `keepBatteryPowerOn()`
also drops a stale `GPIO17` pad hold internally, so no separate "release first"
step is needed:

```text
reset -> ROM bootloader -> second stage bootloader -> setup()
  |
  +- 1. BoardPower::keepBatteryPowerOn()      GPIO17 = HIGH   <- latch asserted here
  |                                            (drops a stale hold first)
  +- 2. BoardPower::releaseSleepPadsIfNeeded() free GPIO18 from the RTC wake pad
  +- 3. Serial.begin(115200) + delay(200)     first diagnostic output
  +- 4. BoardPower::begin()                   the one-shot [power] lines
  +- 5. microSD mount (VM_ENABLE_SD)
  +- 6. Button, DeviceId, RecordingApp, audio (ES8311 + I2S)
  +- 7. UI: e-paper -> touch -> battery ADC -> RTC -> TimeManager
  |      -> Frank's boot animation (CLOSED -> AWAKE, two partial frames)
  +- 8. Wi-Fi (VM_ENABLE_UPLOAD)
  +- 9. PowerManager::begin()                 inactivity starts; PWR is DISARMED
```

There is deliberately **one** application-level call between reset and the latch,
and it is the latch itself. The comment in `setup()` says exactly that:

```cpp
// Battery boot critical path:
// assert VBAT latch before any non-essential initialization.
```

Four details matter:

* **A stale `GPIO17` hold cannot be deferred.** An engaged pad hold *overrides*
  the output, so `gpio_set_level(17, 1)` would be silently ignored and the latch
  could never be asserted. Releasing the hold therefore has to be part of the
  assertion, not a step after it. `keepBatteryPowerOn()` does both, in the order
  the ESP-IDF contract requires:
  1. write `1` to the output register *while the hold is still active* (so the
     pad is already being driven where we want it);
  2. `gpio_hold_dis(17)` + `gpio_deep_sleep_hold_dis()`;
  3. `gpio_config()` the pin as an output and write `1` again.

  `esp_driver_gpio/include/driver/gpio.h` states the reason for that order:
  *"the gpio will be set to the default mode, so, the gpio will output the
  default level if this function is called. If you don't want the level changes,
  the gpio should be configured to a known state before this function is
  called."* Releasing the hold first would instead let the pad fall back to its
  default level before we drive it. On a cold boot the hold is not engaged and
  `gpio_hold_dis()` is a harmless no-op, so paying for it on every boot costs two
  register operations and no time.
* The pin is written HIGH **before** it is switched from high-Z to output.
  `gpio_set_level()` updates the output register without requiring the pin to be
  an output yet, so the pad goes from high-Z straight to HIGH and never emits the
  LOW pulse that a config-then-set order would produce. The internal pull-up is
  enabled exactly as in the official constructor.
* Step 2 exists for the USB-attached "off" state: `rtc_gpio_deinit(18)` restores
  `GPIO18` as a normal digital input instead of an RTC wake pad. It touches a pin
  the latch does not depend on, so it is kept *after* the latch on purpose - the
  set of things that happen before the rail is safe stays exactly one call long.
* Nothing else writes `GPIO17` during normal operation, so after step 1 the latch
  stays asserted for the whole run: no periodic re-write and no `[power]` line per
  `loop()` iteration.

The window from reset to step 1 (ROM + second stage bootloader) is not covered by
firmware, so a PWR press must last long enough to reach `setup()`; a press shorter
than that cannot latch the rail by any firmware means. Deep sleep,
`esp_sleep_enable_ext1_wakeup_io()` and `gpio_hold_en()` are no longer absent -
they are the shutdown path now, documented in
[Power management](#power-management-pwr-key-auto-power-off-and-shutdown-v050).

### An e-paper screen is not a heartbeat

The panel is bistable and keeps its last image with **no power at all**. A visible
UI therefore proves nothing about the MCU, the touch controller or Wi-Fi. The
battery failure mode looks exactly like that: the image stays, while touch and
Wi-Fi are dead because the rails already dropped. Diagnose the latch from the
serial `[power]` / `[boot]` lines and from touch and Wi-Fi actually responding -
never from the picture.

Expected at boot (once, never continuously):

```text
[power] battery latch gpio=17 level=HIGH
[power] PWR key gpio=18 active_low=1 debounce_ms=50
[power] auto power-off enabled timeout_ms=120000
[power] deep-sleep wake on PWR enabled (USB-attached off state)
```

### Battery latch physical test

The latch itself has one pass condition: with **USB disconnected**, start from an
off board and press-and-**hold** PWR until
`[power] battery latch gpio=17 level=HIGH` appears; releasing PWR afterwards must
leave the board running. That is the actual latch test - it fails if the board
dies on release. A press shorter than the bootloader + `setup()` time cannot latch
the rail on any firmware, so "one quick tap" is not a valid pass condition.

Everything else the PWR key now does - the post-boot arming release, the short
press, auto power-off, the shutdown screen and the USB-attached case - is in
[Power management](#power-management-pwr-key-auto-power-off-and-shutdown-v050),
including the full physical script (A-H plus USB). **REQUIRES PHYSICAL
VALIDATION.**

## Power management: PWR key, auto power-off and shutdown (v0.5.0)

v0.4.1 only ever *asserted* the battery latch. v0.5.0 adds the other half: the PWR
key is debounced and armed, an inactivity timer switches the device off by itself,
and a graceful shutdown sequence concludes the work in flight before the latch is
released. Two rules shape every decision below:

* the inactivity timer measures how long since the **user** interacted, never how
  long since the firmware did something;
* a shutdown request is **refused** (and the user told why) while a note exists
  only in PSRAM, and **deferred** while a commit is in flight. The device
  deliberately stays on rather than lose a note silently.

### Hardware and confirmed polarity

| Net | GPIO | Role | Level |
| --- | --- | --- | --- |
| `BAT_Control` / `VBAT_PWR` | 17 | battery power latch | HIGH = hold battery power, LOW = release |
| `BAT_KEY` | 18 | PWR button | **ACTIVE LOW**: pressed = LOW, released = HIGH |

The polarity is not guessed. It is confirmed from the official sources for this
board, which are the evidence the firmware follows:

```text
ESP32-S3-Touch-ePaper-1.54-Schematic.pdf
  (linked from the Waveshare docs "Resources-And-Documents" page)
      net legend:  BAT_Control GP17, BAT_KEY GP18
      R58 = 10K pull-up from BAT_KEY to 3V3
      switch Key2 shorts BAT_KEY to GND when pressed
      GPIO17 -> R64 1K -> T2 (S8050) base;
      T2 collector pulls Q4 (AO3401 P-FET) gate low to conduct VBAT to VSYS

waveshareteam/ESP32-S3-ePaper-1.54
  02_Example/Arduino/07_BATT_PWR_Test/src/button_bsp/button_bsp.c
      #define button2_active 0                     // PWR: valid/active level = 0 = LOW
      gpio_conf.mode = GPIO_MODE_INPUT;
      gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
      gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
      (read with gpio_get_level(PWR_BUTTON_PIN), polled from a 5 ms esp_timer)
  02_Example/Arduino/11_RTC_Sleep_Test/src/power/board_power_bsp.cpp
      const uint64_t ext_wakeup_pwr_mask = 1ULL << GPIO_NUM_18;
      esp_sleep_enable_ext1_wakeup_io(..., ESP_EXT1_WAKEUP_ANY_LOW);
```

`07_BATT_PWR_Test/user_app.cpp` contains `vbat_scanStatus()` and
`button_power_task()` whose arming flag is set on a **non-zero**
`gpio_get_level(PWR_BUTTON_PIN)` and which then powers off - the opposite of the
`button2_active 0` declared in the very same example. That pair is
self-contradictory, so it is **not** followed. The schematic, the active-low
declaration and the EXT1 `ANY_LOW` wake source are the three consistent pieces of
evidence, and they are what `VM_PWR_KEY_ACTIVE_LOW 1` encodes.

The polarity is confirmed from official sources; it is still to be confirmed
physically on the board by tests A and B in the power script below.

### Boot arming: the power-on press is not a shutdown request

The dangerous failure mode is obvious once stated: on battery the user powers the
board on by *holding* PWR. A naive `if (pressed) powerOff()` therefore reads the
power-on press as a request to power off, and the device switches straight back
off the instant the user lets go. The firmware cannot tell the two presses apart
from the pin alone - at `t = 0` the key is simply down - so it does not try. It
arms the button instead.

`PowerButton` is a two-state machine:

```text
DISARMED_AFTER_BOOT   the only state at reset
      |  key observed RELEASED and stable for the full debounce window
      v
ARMED                 one clean press -> release cycle emits exactly one request
```

* While **disarmed**, a press is ignored - and remembered, so it can never be
  counted twice.
* The first debounced **release** arms the button. Arming emits `Armed`, never a
  shutdown request, so the release that ends the power-on press cannot switch the
  device off.
* Only a **later** press (held for the full debounce window, while armed) emits
  `ShortPress`; releasing it closes the cycle without adding a second request.
* If the user holds PWR for a very long time at boot and then lets go, the press
  is still ignored and the release only arms.

`VM_PWR_DEBOUNCE_MS` is **50 ms**, and it is the vendor-consistent value rather
than a guess: the official example runs the `multi_button` library from a 5 ms
`esp_timer` with that library's 50 ms default debounce window. It is comfortably
above the mechanical bounce of a tactile switch (a few ms) and well below a
deliberate human press (>= 150 ms).

The key is **polled** from `loop()` - `digitalRead(VM_PWR_KEY_PIN)` fed to
`PowerManager::tick()` - never from an ISR. There is no interrupt handler, no edge
counter and no task for the button.

### Short press: one press, one request

After arming, one deliberate press produces exactly one shutdown request. A
contact bounce is a run of opposite samples shorter than the debounce window, so
it never produces an edge at all and one gesture cannot emit two requests. Holding
the key down produces exactly one event (the press) and nothing else until it is
actually released - it is not a stream of requests - and a press shorter than the
window is not a press at all. No ISR is involved, so there is no interrupt-storm
path either.

### Auto power-off after inactivity

With `VM_ENABLE_AUTO_POWER_OFF 1`, `PowerManager` powers the device off after
`VM_AUTO_POWER_OFF_MS` (120000 ms = 2 minutes) with no user interaction. Both
values are centralised in `config.h`, and the timeout is never spelled out in
code - `power_manager.h`, `power_policy.h` and `recording_app.cpp` all use the
macro.

The elapsed-time test is millis()-safe:

```cpp
static_cast<uint32_t>(nowMs - lastActivityMs) >= timeoutMs
```

The subtraction is unsigned, so the comparison stays correct when the 32-bit
counter wraps `0xFFFFFFFF -> 0`. The forbidden form `now > last + timeout` is not
used anywhere.

### What counts as activity

The timer measures "how long since the user interacted", not "how long since the
firmware did something". Background work must never keep the device awake
forever, so the classification is explicit:

| Resets the inactivity timer (user interaction) | Does **not** reset it (background work) |
| --- | --- |
| a valid FT6336 touch press (`TouchTap`) | Wi-Fi connect / disconnect |
| tag selection (`TagSelected`) | NTP sync |
| BOOT press or release (`BootButton`) | RTC read |
| recording start (`RecordingStarted`) | upload start / finish |
| recording end, by release or the 45 s limit (`RecordingStopped`) | a scheduled retry |
| a PWR interaction the user can perceive (`PwrButton`) | SD remount |
| any future explicit UI interaction (`UiInteraction`) | e-paper refresh |
| | pending-count change |
| | a log line |
| | an internal timer |

The switch decides, not the caller: `activity_is_user_interaction()` in
`power_policy.h` is a `switch` over the whole `ActivityEvent` enum with **no
`default:`**, so `-Wswitch` makes adding an event without classifying it a warning
in every build. `PowerManager::markActivity()` also re-checks the classification
and refuses a background event, so a mislabelled caller cannot silently disable
the automatic power-off. The gestures that currently occur are published where
they happen (touch in `UiController`, BOOT and the recording edges in
`RecordingApp`, the PWR press in `PowerManager`); `RecordingStarted` and
`TagSelected` are classified as interactions for their own call sites, and the
physical gesture that causes each one already resets the timer through the event
that does get published.

### Shutdown inhibit reasons

One enum answers "may the device power off right now?", so the log and the UI can
name the reason instead of re-deriving it from booleans:

| `ShutdownInhibit` | Class | Meaning |
| --- | --- | --- |
| `None` | - | nothing holds data that only exists in RAM; powering off is safe |
| `Recording` | soft | audio is being captured (including `MaxReachedWaitingRelease`); finalize it first |
| `Saving` | soft | a commit is in flight (`WAV`/metadata -> `tmp/` -> rename -> `pending/`); the request is deferred and completes as soon as the commit returns |
| `FilesystemCritical` | soft, reserved | short-lived maintenance (a remount, a chunk read) is touching the volume; deferred like `Saving` |
| `VolatileUnsavedRecording` | **hard** | the only copy of a recording lives in PSRAM (`Uploading` / `RetryWait`); powering off would destroy a note |

The soft/hard distinction is the whole decision:

* **soft** = the request is **deferred**. It is remembered, the work in progress
  finishes - a commit is never abandoned halfway - and the shutdown proceeds by
  itself the moment the inhibit clears.
* **hard** = the request is **refused**. Nothing is queued; the device stays on and
  keeps retrying, because powering off would destroy the only copy of a note. The
  user is told `UNSENT NOTE` once per episode - not silently dropped, and not once
  per `loop()` iteration.

`shutdown_inhibit_for_state()` maps the recording state machine onto the enum, and
`shutdown_inhibit_is_hard()` / `shutdown_inhibit_is_soft()` are the only places
the distinction is made. `Recording` is classified soft because it is a transient
"not yet" - but a request that arrives during a capture is **refused rather than
remembered** (see the next sub-section): the end of the recording is itself a user
interaction, so remembering the request would power the device off under the
user's hand. `Saving` and `FilesystemCritical` are the soft cases that are
genuinely deferred and completed.

**A non-empty `pending/` queue is not an inhibit.** Committed recordings are
already durable on the card and are recovered on the next boot, so `Idle` maps to
`None` no matter how many notes are waiting.

### PWR during recording, and during Saving

| State | Outcome |
| --- | --- |
| `Recording`, `MaxReachedWaitingRelease` | **refused**: the recording continues, the panel shows `STOP RECORDING FIRST`, and PWR is never a stop control - BOOT is |
| `Saving` | **deferred**: the commit is completed (`WAV`/metadata -> `tmp/` -> rename -> `pending/`) and only then does the device power off |

A refused press during a capture is deliberately not remembered: if it were, the
device would power itself off the moment the recording was committed, which the
user never asked for. The notice stays on the panel until the condition clears,
with `VM_UI_POWER_NOTICE_MIN_HOLD_MS` as the minimum.

### The queue on the microSD

Recordings already committed to the card are not a reason to stay awake. Whether
`pending/` holds 1, 5 or 20 notes, the device powers off normally; on the next
boot `recoverOnBoot()` rebuilds the queue from the card and the uploads continue
where they left off. The final image is Frank's dead face (`FRANK_DEAD`, eyes
`X X`); how many notes are still waiting is written to the log
(`[power] SD queue safe pending=N`) and is shown on the panel only when the
animation could not be flushed and the `POWERED OFF` fallback screen was painted
instead (`N notes saved`), so the user is not left guessing whether the waiting
audio survived. This is the whole point of
[microSD persistence](#microsd-persistence-and-the-offline-queue-v050): the queue,
not the PSRAM buffer, is what makes an unattended power-off safe.

### Shutdown during an upload from the card

If an upload from the card is in flight, the shutdown **may interrupt it** - the
audio is already durable, so this is recoverable. Nothing is deleted: the item
stays in `uploading/` (or is returned to `pending/`), boot recovery makes it
pending again, and no HTTP 2xx is ever invented for a POST that did not finish.

The interruption is bounded on purpose. The uploader polls a cooperative
`abortFlag` between 4 KB chunks, but a worker already blocked inside a socket
write would not see it; `Esp32IngressUploader::abortActiveTransfer()` therefore
closes that socket from the main loop, so the blocked write fails immediately
instead of holding the device for the 15 s HTTP timeout. The abort and the worker
drain are bounded by `VM_PWR_UPLOAD_ABORT_TIMEOUT_MS` (500 ms), the main loop is
serviced between polls every `VM_PWR_SHUTDOWN_POLL_MS` (10 ms), and the radio is
stopped before any further waiting - so nothing in the shutdown path waits on the
network.

### Fallback without a microSD card: never lose a note

If no card is mounted - or the card refused the commit, is full or is unhealthy -
a finalized recording exists **only in PSRAM**, in `Uploading` or `RetryWait`.
There is exactly one such buffer and no durable copy, so the firmware will not
power off:

* automatic power-off is blocked **indefinitely** - `VolatileUnsavedRecording` is
  a hard block, so the timeout keeps its real age but never fires while the note
  is at risk;
* a PWR short press is **refused** and the panel owns the screen with `UNSENT
  NOTE` / `Shutdown blocked` / `Waiting for upload`; the device stays on until the
  ingress confirms the upload or the user power-cycles it by another means (for
  example removing the battery or USB).

No forced shutdown is implemented, and there is no override. Losing a note
silently is deliberately not an option: the device prefers to stay on and keep
retrying. This is otherwise the v0.4.1 path, where it was the only one.

### The shutdown sequence

`enterGracefulShutdown()` in `voice_memo_esp32.ino` runs the sequence once, from
`loop()`, and never returns on success. The order is the safety argument:

1. **Re-check the inhibit.** The request already came from `PowerManager`, which
   refuses to emit one while data is at risk; this is a last, cheap line of
   defence. If any inhibit is active the shutdown aborts and the device stays on.
2. **Bounded wait for an in-flight commit.** A commit may still be running even
   though the state machine just cleared; it is waited out (never interrupted),
   bounded, so a wedged card cannot hang the power-off.
3. **Interrupt an upload and stop the radio.** The cooperative abort flag is set
   and the socket closed, then `esp_wifi_disconnect()` + `esp_wifi_stop()` run
   *before* any further waiting, so a worker blocked on the network cannot turn
   into a 15 s stall. No reconnect is ever started during shutdown.
4. **Release the SD volume.** `RecordingStore::closeVolume()` unmounts the card.
   Every write was already `flush()`ed and `close()`d by the store, and the commit
   ends in a rename, so there is no dirty page to sync; the unmount is what
   guarantees no handle survives the power cut. The release is refused while the
   worker still owns a job, and bounded by
   `VM_PWR_SHUTDOWN_CARD_RELEASE_TIMEOUT_MS` (2000 ms).
5. **Play Frank's shutdown animation** (`AWAKE -> BLINK -> CLOSED -> DEAD`). Its
   last frame is `FRANK_DEAD`, painted as a complete sprite with a **full**
   refresh and waited for, so the eyes are already `X X` on the glass before the
   latch is touched; that is the image the bistable panel keeps. The sequence
   continues regardless of the outcome - the screen is the last thing that may
   block a power-off, never the first. Every BUSY wait on this path
   (`EpdDisplay::waitIdleFor()`) uses `VM_PWR_SHUTDOWN_EPD_TIMEOUT_MS`, not the
   normal UI bound, and every failure is reported and survived. If not even one
   Frank frame can be flushed (a panel that stopped answering), the pre-Frank
   `POWERED OFF` screen is painted instead, on the same bounded full refresh, so
   the user is still told how many notes are safe.
6. **`audio.stop()`** - the codec stops driving I2S and the amplifier stays
   disabled (playback was never enabled in this firmware).
7. **Release the battery latch** (`BoardPower::powerOff()`) and, on USB, enter
   deep sleep (see below).

The display rail is deliberately left powered and the panel idle, which is the
state the official Waveshare examples leave the panel in when they release the
latch. Cutting the panel rail during or just after an update would risk corrupting
the one image the user is left looking at; the ~30 uA it costs is accepted. Long
`delay()`s are not used in normal operation: the periodic work is deadline-driven
and `loop()` never blocks on the network, the card or the panel. The shutdown path
is the one place allowed to block briefly, because the device is going away and
nothing else needs the CPU.

### USB connected: the latch alone is not enough

Releasing GPIO17 is a real power-off only on battery. The schematic gives the
board an independent `VBUS -> VSYS` path (`Q5` AO3401 with its gate on `VBUS` and
`R68` 100K, and `U4` ETA6098), so with USB attached the board keeps running after
the latch is released. Waveshare's own wiki also requires USB to be disconnected
for its PWR-button power test.

When `VM_PWR_WAKE_ON_PWR` is 1, `BoardPower::powerOff()` therefore does both
halves of what the official RTC-sleep example does:

1. drives `GPIO17` LOW and calls `gpio_hold_en(GPIO17)` so the released level is
   held through sleep (otherwise the board's 100K `R63` would pull the gate back
   up and the device would power itself on again);
2. arms `GPIO18` as an EXT1 deep-sleep wake source:
   `esp_sleep_enable_ext1_wakeup_io(1ULL << 18, ESP_EXT1_WAKEUP_ANY_LOW)`, plus an
   explicit RTC pull-up, because with the RTC peripherals powered down the
   internal digital pull-up no longer applies;
3. calls `esp_deep_sleep_start()`.

On battery the rail collapses before the sleep even matters; on USB the deep sleep
is what makes "off" real, and pressing PWR wakes the device. At boot the other
half is folded into the latch assertion: `keepBatteryPowerOn()` releases the RTC
pad hold on `GPIO17` in the same call that drives it HIGH (a stale LOW hold would
otherwise override the output and make the assertion impossible), and
`releaseSleepPadsIfNeeded()` then calls `rtc_gpio_deinit(18)` immediately after.
On a cold boot both are harmless no-ops.

**REQUIRES PHYSICAL VALIDATION.** The USB-attached off state - whether it enters
deep sleep, whether a PWR press wakes it, and whether the board really keeps
running after the latch release on USB - has not been verified; it is documented
here from the schematic, the official example and the code.

### The e-paper keeps the last image

The panel is bistable: it holds its image with **no power at all**, so the last
image the firmware painted - `FRANK_DEAD`, the dead Frank with `X X` eyes, or the
`POWERED OFF` fallback screen when the animation could not be flushed - remains
visible after the device is off. That is also why a visible screen is **not**
proof that the MCU is running - a stale image with dead touch and dead Wi-Fi is
exactly what a released battery latch looks like. Both shutdown images
deliberately have no live value (no clock, no battery, no Wi-Fi) because a frozen
reading would be a permanent lie.

### Serial log reference

One line per event, never per `loop()` iteration:

| Line | Emitted when |
| --- | --- |
| `[power] battery latch gpio=17 level=HIGH` | `BoardPower::begin()`, once at boot |
| `[power] PWR key gpio=18 active_low=1 debounce_ms=50` | `BoardPower::begin()`, once at boot |
| `[power] auto power-off enabled timeout_ms=120000` | `BoardPower::begin()` (or `auto power-off disabled`) |
| `[power] deep-sleep wake on PWR enabled (USB-attached off state)` | `BoardPower::begin()` |
| `[power] PWR disarmed after boot; waiting for a stable release` | `PowerManager::begin()`, once |
| `[power] PWR release confirmed; shutdown button armed` | the first debounced release after boot |
| `[power] PWR short press` | a short press while armed |
| `[power] shutdown requested reason=user` | an allowed PWR request |
| `[power] shutdown requested reason=inactivity` | an allowed inactivity timeout |
| `[power] capture in progress: shutdown requests are refused` | once per recording episode |
| `[power] commit in progress: shutdown requests are deferred` | once per commit episode |
| `[power] shutdown deferred: recording` | a request during a capture (refused, never queued) |
| `[power] shutdown deferred: saving` | a request during a commit (deferred, then completed) |
| `[power] shutdown deferred: filesystem_critical` | the same, while maintenance holds the volume |
| `[power] shutdown blocked: volatile recording not persisted` | a hard block: the note exists only in PSRAM |
| `[power] shutdown blocked: volatile_unsaved_recording` | the same block, named by the policy layer |
| `[power] inactivity timeout 120000 ms` | the timeout expires (announced once per interval) |
| `[power] deferred shutdown proceeding reason=...` | the deferred request is granted |
| `[power] preparing shutdown reason=...` | `enterGracefulShutdown()` starts |
| `[power] SD release deferred: upload worker still active` | the volume could not be released yet |
| `[power] SD queue safe pending=3` | the volume was released; also repeated as the sequence summary |
| `[power] wifi stopped` | the radio was stopped |
| `[power] shutdown screen on panel (N ms)` | the `POWERED OFF` full refresh finished (fallback path) |
| `[frank] boot animation` | `UiController::playBootAnimation()`, once per boot (two frames: `closed`, `awake`) |
| `[frank] frame <state> full\|partial N ms` | one animation frame finished (state, waveform, duration) |
| `[frank] boot animation done in N ms` | the panel is awake; the home screen is painted next |
| `[frank] shutdown animation` | the shutdown sequence reached the last image |
| `[frank] shutdown animation done in N ms` | `FRANK_DEAD` is on the glass and waited for |
| `[frank] ... aborted ...` / `frame ... failed` | the panel refused or lost a frame; the caller carries on |
| `[power] battery latch OFF gpio=17 level=LOW` | `BoardPower::powerOff()` |
| `[power] entering deep sleep; wake source=PWR gpio=18 level=LOW` | `BoardPower::powerOff()`, before `esp_deep_sleep_start()` |
| `[boot] woke from deep sleep; PWR pressed=yes` | the next boot, when the reset was an EXT1 wake (`no` for another EXT1 pin) |

Nothing is logged per loop iteration, and a host test asserts exactly that:
`power_manager_test` counts zero `Serial` lines over 10 s of idle ticking.

### Configuration

| Flag (`config.h`) | Default | Meaning |
| --- | --- | --- |
| `VM_PWR_KEY_ACTIVE_LOW` | 1 | confirmed GPIO18 polarity: pressed = LOW |
| `VM_PWR_DEBOUNCE_MS` | 50 | debounce window for the PWR key |
| `VM_ENABLE_AUTO_POWER_OFF` | 1 | build the inactivity power-off (0 = off) |
| `VM_AUTO_POWER_OFF_MS` | 120000 | inactivity timeout (2 minutes) |
| `VM_ENABLE_MANUAL_POWER_OFF` | 1 | a PWR short press requests a shutdown |
| `VM_PWR_WAKE_ON_PWR` | 1 | arm GPIO18 as an EXT1 (ANY_LOW) deep-sleep wake source |
| `VM_PWR_SHUTDOWN_EPD_TIMEOUT_MS` | 6000 | BUSY bound for the shutdown screen (`waitIdleFor()`); the normal UI path keeps `VM_EPD_BUSY_TIMEOUT_MS` (5000) |
| `VM_PWR_UPLOAD_ABORT_TIMEOUT_MS` | 500 | bound for an interrupted upload to hand the worker back |
| `VM_PWR_SHUTDOWN_CARD_RELEASE_TIMEOUT_MS` | 2000 | bound for the SD volume to become releasable |
| `VM_PWR_SHUTDOWN_POLL_MS` | 10 | main-loop servicing interval between shutdown polls |
| `VM_UI_POWER_NOTICE_MIN_HOLD_MS` | 1200 | minimum time a power notice stays on the panel |

Three of these are guarded with `#ifndef` so they can be overridden from the
command line instead of by editing this file — `VM_ENABLE_AUTO_POWER_OFF`,
`VM_PWR_WAKE_ON_PWR` and `VM_PWR_KEY_ACTIVE_LOW`:

```bash
# disable the inactivity power-off entirely
--build-property compiler.cpp.extra_flags=-DVM_ENABLE_AUTO_POWER_OFF=0

# release the latch only; do not deep sleep (USB keeps the board up)
--build-property compiler.cpp.extra_flags=-DVM_PWR_WAKE_ON_PWR=0

# flip the PWR polarity, if TEST B ever disproves the active-low reading
--build-property compiler.cpp.extra_flags=-DVM_PWR_KEY_ACTIVE_LOW=0
```

The polarity switch is guarded deliberately: it is the one setting a physical test
might force you to change, and being able to flip it from the build command keeps
that diagnosis a one-line experiment instead of a source edit.

Two BUSY bounds coexist on purpose. `EpdDisplay::waitIdle()` keeps the 5000 ms
`VM_EPD_BUSY_TIMEOUT_MS` that every normal refresh uses, while `waitIdleFor()` takes
an explicit deadline; the shutdown sequence passes the slightly more generous
`VM_PWR_SHUTDOWN_EPD_TIMEOUT_MS` because the final image matters more than the
milliseconds and the device is going away anyway. Neither wait can hang the
power-off: both are deadlines, and `showShutdownScreen()` reports and survives
their failure.

### Physical test script (A-H) and the USB case

**REQUIRES PHYSICAL VALIDATION.** Nothing below can be confirmed from code alone.
A is the power-on path, B/C the manual power-off cycle, and D-H the inhibitions
and fallbacks.

| # | Setup | Action | Expected |
| --- | --- | --- | --- |
| A | USB disconnected, device off | press and **hold** PWR until the firmware asserts GPIO17, then release | the device stays on; the release does **not** power it off; the panel plays Frank's boot animation and then shows the READY home screen; log shows `PWR release confirmed; shutdown button armed` and no `shutdown requested` |
| B | device on, at least 1 s after the initial release | single press PWR, release | log `PWR short press` then `shutdown requested reason=user`; panel plays `AWAKE -> BLINK -> CLOSED -> DEAD` and stays on `FRANK_DEAD`; the device powers off; touch and Wi-Fi stop responding; the e-paper keeps the dead face |
| C | device off (after B) | press PWR again | normal boot |
| D | device on | start a recording with BOOT, press PWR | the device does **not** power off; the panel shows `STOP RECORDING FIRST`; the recording is not corrupted; release BOOT to finish normally |
| E | device on | do not interact | at ~120 s the log shows `inactivity timeout 120000 ms` then `shutdown requested reason=inactivity`, and the device powers off |
| F | device on | wait ~100 s, tap a tag, wait 30 s | still on; it powers off ~120 s after the last interaction |
| G | Wi-Fi off, card inserted | record a note, confirm `[store] committed`, wait 2 min | the device may power off (log `[power] SD queue safe pending=N`); power on again and the pending queue reappears and uploads |
| H | card removed, Wi-Fi off | record a note, wait > 2 min | the device must **not** auto-power-off and must not lose the WAV; log `shutdown blocked: volatile recording not persisted`; pressing PWR shows `UNSENT NOTE` |

With USB attached the same firmware is expected to behave differently, because
VBUS keeps the rail up. Every expectation in this table is **to be confirmed
physically** and is not asserted as verified:

| # | Setup | Action | Expected |
| --- | --- | --- | --- |
| USB-1 | USB connected, device on | press PWR | `shutdown requested reason=user`; the e-paper plays Frank's shutdown animation and stays on `FRANK_DEAD`; the device enters deep sleep instead of stopping |
| USB-2 | USB connected, device on | leave it idle | auto power-off fires at ~120 s and the device enters deep sleep |
| USB-3 | USB connected, device asleep | press PWR | the device wakes; log `[boot] woke from deep sleep; PWR pressed=yes`; the latch is re-asserted and the device boots normally |

## TEST B: complete upload (ESP32 -> NUC ingress)

TEST B sends a real WAV to the existing ingress contract:

```text
POST /api/v1/audio
```

`INGEST_URL` is a **base URL only** (`http://<host>:<port>`). The firmware
appends `/api/v1/audio` itself, so never put the endpoint in `secrets.h`. Any
path present in `INGEST_URL` is ignored on purpose and normalized to the single
ingress endpoint, so the firmware cannot POST to `/` and cannot build a doubled
path such as `/api/v1/audio/api/v1/audio`.

For example, all of these produce the same request target:

```text
http://192.168.1.50:8090
http://192.168.1.50:8090/
http://192.168.1.50:8090/api/v1/audio
        -> POST http://192.168.1.50:8090/api/v1/audio
```

### What you must change locally (only `secrets.h` + one line in `config.h`)

Copy the template if you have not already:

```bash
cp secrets.example.h secrets.h
```

Then set these values. Nothing else needs editing.

| Setting | Where | Value for TEST B |
| --- | --- | --- |
| `WIFI_SSID` | `secrets.h` | Your 2.4 GHz Wi-Fi SSID (primary) |
| `WIFI_PASSWORD` | `secrets.h` | Your Wi-Fi password |
| `WIFI_FALLBACK_SSID` | `secrets.h` | Fallback SSID (for example a phone hotspot), or `""` to disable |
| `WIFI_FALLBACK_PASSWORD` | `secrets.h` | Fallback password; `""` for an open network |
| `INGEST_URL` | `secrets.h` | `"http://<NUC-LAN-IP>:8090"` |
| `INGEST_TOKEN` | `secrets.h` | Same as ingress `--token`, or `""` if auth is off |
| `DEVICE_ID` | `secrets.h` | `"bel-esp32-01"` for the first integration test |
| `VM_ENABLE_UPLOAD` | `config.h` | `1` |

Concrete `secrets.h` for TEST B (example address; use your real NUC LAN IP):

```cpp
#define WIFI_SSID "my-wifi"
#define WIFI_PASSWORD "my-password"
#define WIFI_FALLBACK_SSID "salvação"
#define WIFI_FALLBACK_PASSWORD "my-hotspot-password"
#define INGEST_URL "http://192.168.1.50:8090"
#define INGEST_TOKEN ""
#define DEVICE_ID "bel-esp32-01"
```

The single line to enable TEST B is in `config.h`:

```cpp
#define VM_ENABLE_UPLOAD 1
```

Upload stays disabled in tracked source (`VM_ENABLE_UPLOAD 0`) on purpose, so
TEST A remains the default and works with no Wi-Fi and no NUC.

### Record only 3-5 seconds for the first upload test

The recording limit stays at the normal 45 seconds
(`VM_MAX_RECORDING_SECONDS` in `config.h`). For the first end-to-end upload,
press and HOLD BOOT, speak for about 3-5 seconds, then release. A short clip
keeps the multipart upload quick and makes the ingress log easy to read.
Push-to-talk behavior is unchanged.

### Step 1: discover the NUC LAN IPv4 address

Run on the NUC (not over Tailscale):

```bash
hostname -I
```

Or, if you want the interface and address explicitly:

```bash
ip -4 addr show scope global
```

Pick the private LAN address, typically `192.168.x.x` or `10.x.x.x`. The ESP32
must use that address: it is on the same Wi-Fi network, so only the LAN IPv4
address is reachable.

Do NOT use the Tailscale `100.x.y.z` address in `INGEST_URL`. The ESP32 has no
Tailscale client, so a `100.x` address is normally unreachable from the board.

### Step 2: start the ingress manually for the LAN test

The ingress defaults to localhost, so TEST B needs an explicit bind that the
ESP32 can reach. Run it in the foreground, with a disposable test database and
a disposable audio directory, so production data is untouched:

```bash
cd /opt/voice-memo

# disposable test data (delete afterwards)
mkdir -p /tmp/vm-test-audio
rm -f /tmp/vm-test.sqlite

python3 -m venv /tmp/vm-test-venv
/tmp/vm-test-venv/bin/pip install -r requirements.txt

/tmp/vm-test-venv/bin/python -m <ingress_module> \
  --host 0.0.0.0 \
  --port 8090 \
  --db /tmp/vm-test.sqlite \
  --audio-dir /tmp/vm-test-audio \
  --log-level debug
```

Replace `<ingress_module>` with the actual ingress entry point in the
repository (for example the module that exposes `POST /api/v1/audio`); check
`--help` first, since exact flag names live in the server repo:

```bash
/tmp/vm-test-venv/bin/python -m <ingress_module> --help
```

Notes:

- `--host 0.0.0.0` is required for TEST B. The default localhost bind is not
  reachable from the ESP32.
- `--db /tmp/vm-test.sqlite` keeps the production SQLite file untouched.
- `--audio-dir /tmp/vm-test-audio` keeps production audio untouched.
- Do not change the default binding permanently, do not add a systemd unit and
  do not touch firewall rules.
- If the ingress has auth enabled, start it with `--token` and put the same
  value in `INGEST_TOKEN`. Otherwise leave auth off and `INGEST_TOKEN` empty.

### Step 3: watch the NUC side

Keep the manual ingress running in the foreground and watch its stdout. It is
the primary evidence for TEST B. A successful upload logs the request for:

```text
POST /api/v1/audio
```

with `device_id=bel-esp32-01` and a `recording_id`. In parallel you can confirm
the row and the file were created:

```bash
sqlite3 /tmp/vm-test.sqlite 'select device_id, recording_id, bytes, created_at from recordings order by created_at desc limit 5;'
ls -l /tmp/vm-test-audio
```

`201 Created` on the first delivery and `200 already_known` on a replayed
`recording_id` are both recorded as successful delivery.

### Step 4: watch the ESP32 side

Open Serial Monitor at `115200`. Expected success output:

```text
[boot] TEST_B mode: upload enabled
[boot] known wifi networks=2
[wifi] sta ready autoReconnect=0
[wifi] state=trying_primary ssid=<primary-ssid> status=6
[wifi] connected ssid=<primary-ssid> ip=192.168.1.77 status=3
[app] BOOT press
[app] recording started id=...
[app] BOOT release
[app] recording stopped ...
[app] audio_peak=... audio_rms=...
[app] WAV finalized ...
[app] async upload started id=... attempt=1
[upload] recording_id=... bytes=... duration_ms=...
[upload] target http://192.168.1.50:8090/api/v1/audio
[upload] started
[upload] HTTP 201
[app] async upload accepted id=... http=201
```

`[app] async upload started` is printed by the main loop and returns
immediately: the POST itself runs on a background FreeRTOS task, so `loop()`
keeps calling `tick()` while the WAV is in flight.

A replay of the same `recording_id` instead reports:

```text
[upload] HTTP 200
[app] async upload already known id=... http=200
```

A failed upload keeps the WAV in PSRAM, keeps the same `recording_id`, and
retries without generating a new one. The retry also runs in the background:

```text
[upload] connect failed: 192.168.1.50:8090
[upload] check that the ingress binds the NUC LAN address and that Wi-Fi is on the same network
[app] async upload failed id=... next_attempt=2; keeping recording for retry
[app] retry pending id=... seconds_until_retry=5
[app] retrying previous upload id=...
[app] async upload started id=... attempt=2
```

If the ingress answers with an error status:

```text
[upload] HTTP 500
[upload] response=<short body>
[app] async upload failed id=... next_attempt=2; keeping recording for retry
```

If no HTTP response arrives at all:

```text
[upload] no HTTP response from ingress (connection dropped or timed out)
[app] async upload failed id=... next_attempt=2; keeping recording for retry
```

If Wi-Fi never connects, the recording is retained and the firmware keeps
trying (see [Wi-Fi fail-over](#wi-fi-fail-over-primary--fallback) for the exact
cadence):

```text
[wifi] state=trying_primary ssid=<primary-ssid> status=6
[wifi] disconnected ssid=<primary-ssid> reason=201 name=NO_AP_FOUND
[wifi] primary timeout status=6
[wifi] switching: disconnecting primary
[wifi] station idle; starting fallback
[wifi] state=trying_fallback ssid=salvação status=6
[wifi] fallback timeout status=6
[wifi] switching: disconnecting fallback
[wifi] station idle
[wifi] no known network available status=6
[wifi] state=retry_wait
[wifi] retrying known networks status=6
[wifi] state=trying_primary ssid=<primary-ssid> status=6
[app] upload skipped id=... attempt=1 reason=wifi_not_connected; keeping recording for retry
[app] retry pending id=... reason=wifi
```

Serial output never prints the Wi-Fi password or `INGEST_TOKEN`. The only
network details shown are the SSID being attempted, the numeric
`WiFi.status()`, and the target host, port and path.

### Wi-Fi fail-over: primary + fallback

`WifiManager` owns an ordered list of known networks instead of a single pair of
credentials. The list is built in `voice_memo_esp32.ino` from `secrets.h`:

```cpp
static const WifiNetwork kKnownWifiNetworks[] = {
    {WIFI_SSID, WIFI_PASSWORD},
    {WIFI_FALLBACK_SSID, WIFI_FALLBACK_PASSWORD},
};
```

Usable entry 0 is the primary network, every later entry is a fallback tried in
order. Empty SSIDs are skipped, so leaving `WIFI_FALLBACK_SSID` empty disables
the fallback and keeps the single-network behaviour. Adding a third network is a
one-line change in that array (plus its defines in `secrets.h`); no Wi-Fi logic
is duplicated.

The decision rules live in `wifi_network_selector.h` as an explicit connect
state machine (`Idle`, `TryingPrimary`, `TryingFallback`,
`DisconnectingForSwitch`, `RetryWait`), which has no Arduino/Wi-Fi dependency and
is covered by a host test. The timing is entirely deadline-based - `loop()` is
never blocked and there is no `delay()`:

| Constant (`config.h`) | Value | Meaning |
| --- | --- | --- |
| `VM_WIFI_CONNECT_TIMEOUT_MS` | 10000 | Full window given to one network attempt before the next entry is tried |
| `VM_WIFI_DISCONNECT_TIMEOUT_MS` | 1500 | Safety deadline for the explicit disconnect that precedes every switch (the driver normally confirms in tens of ms) |
| `VM_WIFI_RETRY_INTERVAL_MS` | 10000 | Offline pause (`RetryWait`) after the whole list timed out, before restarting at the primary |

`WifiManager::begin()` (called once from `setup()`) selects `WIFI_STA`, calls
`WiFi.setAutoReconnect(false)` and subscribes to
`ARDUINO_EVENT_WIFI_STA_DISCONNECTED`. **The selector is the only reconnect
authority**: the ESP32 core has its own retry loop (`STAClass::_autoReconnect`
defaults to `true`) and, while that loop runs, it keeps the station in the
connecting state for the *old* SSID. Two reconnect machines cannot share one
station.

`WiFi.begin()` is called **exactly once per state transition** into
`TryingPrimary` / `TryingFallback`, never once per `loop()`, and never while the
selector is in `DisconnectingForSwitch`. While `WiFi.status() == WL_CONNECTED`
no other network is ever started. The active state, the numeric
`WiFi.status()` and the SSID are logged on every transition (the SSID only;
never a password or a token).

An attempt is ended **only** by `WL_CONNECTED` or by running out the full
10 s window - a driver failure status never shortens it. In arduino-esp32 3.3.11
`WIFI_REASON_NO_AP_FOUND` / `ASSOC_FAIL` / `AUTH_FAIL` map to `WL_NO_SSID_AVAIL`
/ `WL_CONNECT_FAILED`, and the core also performs one `disconnect()` +
`connect()` retry of its own on the first failure. An earlier revision treated
those statuses as terminal and re-issued `WiFi.begin(otherSsid)` after 1 s,
which overwrote the stored credentials mid-flight.

Switching networks is two explicit phases, never `timeout -> WiFi.begin(other)`:

1. the timeout issues `WiFi.disconnect(false, false, 0)` once - the radio and the
   netif stay up, credentials/NVS are **never** erased, and the call does not
   block (`timeoutLength = 0`; the 3-argument form of `WiFiSTAClass::disconnect`
   in core 3.3.11 would otherwise spin for up to 100 ms);
2. `DisconnectingForSwitch` waits, still without blocking, for the driver to
   confirm it left the connecting state (`ARDUINO_EVENT_WIFI_STA_DISCONNECTED`,
   with `VM_WIFI_DISCONNECT_TIMEOUT_MS` as a safety deadline for a station that
   had nothing to disconnect);
3. only then is the next `WiFi.begin()` issued, exactly once.

Without phase 2 the new SSID was rejected by `esp_wifi_set_config()` with
`wifi:sta is connecting, cannot set config`, and the station stayed on the old,
unreachable network. The `[wifi] disconnected ssid=... reason=N name=...` line
comes from the same event and is the only place the real disconnect reason code
(`wifi_err_reason_t`, e.g. `ASSOC_FAIL` / `NO_AP_FOUND` / `ASSOC_LEAVE`) and the
SSID that was being attempted are available.

Behaviour:

* primary available -> connects and uses it normally;
* primary unavailable (full 10 s window, then timeout) -> disconnects, waits for
  the driver, then tries the fallback; the upload path is unchanged: it still
  POSTs to the same `INGEST_URL`;
* nothing available -> stays offline (`RetryWait`), the firmware keeps running,
  and the whole list is tried again after `VM_WIFI_RETRY_INTERVAL_MS`;
* a known network that comes back is joined automatically on the next cycle;
* while connected the manager does not rescan and does not switch back to the
  primary, so an upload over the fallback is never interrupted.

Logs (passwords and tokens are never printed; `status=` is `WiFi.status()`):

```text
[wifi] sta ready autoReconnect=0
[wifi] state=trying_primary ssid=... status=6
[wifi] disconnected ssid=... reason=203 name=ASSOC_FAIL
[wifi] primary timeout status=6
[wifi] switching: disconnecting primary
[wifi] station idle; starting fallback
[wifi] state=trying_fallback ssid=salvação status=6
[wifi] fallback timeout status=6
[wifi] switching: disconnecting fallback
[wifi] station idle
[wifi] no known network available status=6
[wifi] state=retry_wait
[wifi] retrying known networks status=6
[wifi] state=trying_primary ssid=... status=6
[wifi] connected ssid=... ip=192.168.1.77 status=3
[wifi] disconnected status=6
```

`reason=` is the `wifi_err_reason_t` code carried by
`ARDUINO_EVENT_WIFI_STA_DISCONNECTED` and `name=` is
`WiFi.STA.disconnectReasonName()` for it. A burst of
`wifi:Association refused too many times, max allowed 1` from the driver is the
AP refusing the association (or a Wi-Fi/AP configuration problem on the AP
side); the firmware's answer is the `[wifi] disconnected ... reason=...` line
that names it, followed by the controlled switch above - never a `WiFi.begin()`
while the station is still connecting.


### Non-blocking uploads (background task)

The upload never runs on the Arduino loop task. `RecordingApp::begin()` creates a
single FreeRTOS task (`vm_upload`, 8 KB stack, priority 1, not pinned to a core)
that sleeps on a task notification. When a recording is finalized, the main loop
freezes it, notifies the task and returns to `tick()` immediately, so the BOOT
button keeps being debounced while the POST is in flight.

| State | Meaning | New recording? |
| --- | --- | --- |
| `Idle` | buffer free; a persistent queue may be pending | yes |
| `Recording` | capturing into PSRAM | already recording |
| `MaxReachedWaitingRelease` | 45 s hit, waiting for the release | no |
| `Saving` | the frozen WAV is being committed to the microSD card | no |
| `Uploading` | fallback path: background POST owns the PSRAM WAV | no |
| `RetryWait` | fallback path: attempt failed, same WAV kept for retry | no |

With a healthy card the firmware never enters `Uploading`/`RetryWait`: a
recording is committed to the card and uploaded from there, so `Idle` stays
available and the next recording can start immediately. Those two states are the
volatile fallback used when the card is absent, full or unhealthy - in that mode
there is exactly one WAV buffer, so a BOOT press is refused instead of
overwriting the audio:

```text
[app] BOOT press ignored: buffer busy id=... state=uploading
```

That line appearing immediately after `[upload] started` is the proof that
`tick()` kept running during the upload. After `accepted`/`already known` the
buffer is released and the next press starts a normal recording.

A BOOT release belonging to a press that was refused during an upload is
discarded, so it cannot stop the next recording the instant it starts.

While `Uploading`/`RetryWait` hold the buffer, `recording_id`, `duration_ms` and
the PSRAM WAV are frozen: the task only reads them, `clearRecording()` is never
called before the outcome is drained, and there is exactly one upload task, so
two uploads can never race for the same audio. The result travels back through a
one-slot FreeRTOS queue, which also provides the memory barrier that publishes
it safely to the main loop.

#### Optional: verify the endpoint without the board

Useful when the ESP32 cannot reach the NUC. Run on the NUC itself:

```bash
curl -i -X POST http://127.0.0.1:8090/api/v1/audio \
  -F device_id=bel-esp32-01 \
  -F recording_id=curl-selftest-1 \
  -F duration_ms=1000 \
  -F sample_rate=16000 \
  -F format=wav \
  -F audio=@/tmp/vm-test-audio/../test.wav
```

Then from the Mac (same Wi-Fi as the ESP32), which proves LAN reachability:

```bash
curl -i -m 5 http://<NUC-LAN-IP>:8090/api/v1/audio
```

An HTTP response of any kind proves the LAN path and bind are correct; a
timeout or connection refused means the ESP32 would fail too.

### Retry identity

- Success: HTTP `201` (accepted) and HTTP `200` (already_known).
- Failure: anything else, including connect failure and timeout.
- On failure the WAV is kept, `recording_id` is preserved and the retry reuses
  it. A new `recording_id` is generated only when a new recording starts.
- With a microSD card the retry reads the same file from the card, so it also
  survives a reboot; with no card the WAV stays in PSRAM.
- The WAV is removed (or moved to `sent/`) only after a valid HTTP confirmation.
- Retry cadence is `VM_UPLOAD_RETRY_INTERVAL_MS` (5000 ms) for the volatile
  fallback and `VM_SD_QUEUE_RETRY_INTERVAL_MS` (5000 ms) for the persistent
  queue, both in `config.h`.

### Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `[upload] connect failed` | Ingress bound to localhost, wrong `INGEST_URL`, or different network |
| `[upload] no HTTP response` | Firewall/drop between Wi-Fi LAN and NUC, or very large payload |
| `[upload] HTTP 401` / `403` | `INGEST_TOKEN` does not match ingress `--token` |
| `[upload] HTTP 404` | Route mismatch on the ingress; the firmware always POSTs to `/api/v1/audio`, so verify that route exists and that host/port in `INGEST_URL` are correct |
| `[upload] HTTP 400` | Missing/incorrect multipart fields |
| `[upload] HTTP 200` | Replay of a known `device_id` + `recording_id`; success |
| `[wifi] state=trying_primary` never followed by `connected` | Wrong SSID/password for the primary, or 5 GHz-only SSID (ESP32-S3 is 2.4 GHz) |
| `[wifi] sta ready autoReconnect=0` | The core's own reconnect machine is off; the multi-SSID selector is the only reconnect authority. `autoReconnect=1` means `WiFi.setAutoReconnect(false)` did not take effect |
| `[wifi] disconnected ... reason=` / `name=` | Real `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` reason and the SSID that was being attempted. A burst of `wifi:Association refused too many times, max allowed 1` before it is the AP refusing the association (e.g. MAC filter, band steering to 5 GHz, or too many stations); `reason=` says which one |
| `[wifi] switching: disconnecting primary` / `... fallback` | The attempt timed out. The station is being explicitly disconnected (radio on, no NVS erase, non-blocking) before any other network is configured |
| `[wifi] station idle; starting primary` / `... fallback` | The driver confirmed it left the connecting state; the next `WiFi.begin()` is issued once, right after this line |
| `[wifi] begin refused ...` | `esp_wifi_set_config()` rejected the new configuration (`WL_CONNECT_FAILED`), e.g. an invalid SSID/passphrase. If it is followed by `sta is connecting, cannot set config`, the disconnect wait in `DisconnectingForSwitch` did not complete - check `VM_WIFI_DISCONNECT_TIMEOUT_MS` and the event listener |
| `[wifi] primary timeout` | Primary did not reach `WL_CONNECTED` inside `VM_WIFI_CONNECT_TIMEOUT_MS`; the fallback is attempted next |
| `[wifi] fallback timeout` | Fallback did not reach `WL_CONNECTED` in its window either (wrong/placeholder fallback password, or hotspot off) |
| `[wifi] no known network available` | Neither the primary nor the fallback is reachable; the firmware stays offline in `RetryWait` and retries after `VM_WIFI_RETRY_INTERVAL_MS` |
| `status=` in the `[wifi]` lines | Numeric `WiFi.status()`: `1` = `WL_NO_SSID_AVAIL`, `3` = `WL_CONNECTED`, `4` = `WL_CONNECT_FAILED`, `5` = `WL_CONNECTION_LOST`, `6` = `WL_DISCONNECTED`. Values `1`/`4`/`5` are transient while associating; a new `WiFi.begin()` must not be forced on them |
| `[sd] card not present` | No card, a card not formatted FAT32, or an unreadable card. The firmware keeps working and shows `NO SD`; format the card as FAT32 (the vendor requires it) and reboot |
| `[sd] write failed ... (io_error)` | The card was removed or failed mid-write. Nothing was lost: the recording stays in RAM and the queue is paused until `[sd] remounted` |
| `[sd] storage full (free space reserve)` | Fewer than `VM_SD_MIN_FREE_BYTES` would remain. Delete uploaded files, or raise the limit only if you know the card has room; pending notes are never deleted automatically |
| `[sd] storage full (queue limit reached)` | `VM_SD_MAX_PENDING` recordings are already waiting. Connect Wi-Fi so the queue drains; nothing is discarded |
| `[store] quarantined corrupt audio id=...` | A `pending/*.wav` failed header/CRC validation. Inspect it under `corrupt/` on a computer; it is never uploaded |
| `[store] quarantined metadata without audio id=...` | A sidecar with no WAV (and nothing in `tmp/`). The audio was lost before the commit; the sidecar is kept under `corrupt/` for diagnosis |
| `[store] cleanup deferred id=...` | The ingress accepted the note but the card could not be updated (it was removed). The file stays and is re-delivered after a remount; the ingress answers `200 already_known` |
| `[queue] pending=` never falls | Wi-Fi is down or the ingress is unreachable. The `[upload] failed ... source=sd` line above it says why |


## e-paper UI (v0.4.0)

The firmware drives the black/white 1.54 inch panel and the capacitive touch
screen. The UI is a pure observer of `RecordingApp`: it renders the state the
firmware is really in, and the only thing it may change is the tag selected for
the *next* recording, and only while the buffer is free.

BOOT remains the only way to record. There is no touch record button.

### Hardware map (official sources)

Every value below was read from the official Waveshare repository for this
product, `waveshareteam/ESP32-S3-ePaper-1.54` (commit `9957d0f4`). Nothing is
guessed, and the audio/I2C pins that were already validated on hardware match the
official board definition exactly.

| Function | Value | Official source |
| --- | --- | --- |
| E-paper SPI | `SPI2_HOST`, 40 MHz | `09_LVGL_V8_Test/user_config.h` |
| E-paper DC / CS / SCK / MOSI | `10` / `11` / `12` / `13` | `09_LVGL_V8_Test/user_config.h` |
| E-paper RST / BUSY | `9` / `8` | `09_LVGL_V8_Test/user_config.h` |
| E-paper power enable | `GPIO6`, active LOW | `09_LVGL_V8_Test/src/power/board_power_bsp.cpp` |
| Touch FT6336 | I2C `0x38`, RST `GPIO7`, INT `GPIO21` | `12_FT6336_Test/{ft6336_bsp.cpp,user_config.h}` |
| Battery ADC | `ADC1` channel 3 (`GPIO4`), 12 dB, x2 divider | `01_ADC_Test/adc_bsp.cpp` |
| RTC PCF85063 | I2C `0x51` | `01_Arduino_Libraries/SensorLib/src/REG/PCF85063Constants.h` |
| I2C bus | SDA `47`, SCL `48`, 100 kHz | `codec_board/board_cfg.h` (`Board: S3_ePaper_1_54`) |
| Audio I2S | MCLK `14`, BCLK `15`, WS `38`, DIN `16`, DOUT `45` | `codec_board/board_cfg.h` |
| Speaker amp / audio power | `GPIO46` (PA), `GPIO42` (power enable, active LOW) | `codec_board/board_cfg.h`, `user_config.h` |
| BOOT / PWR button | `GPIO0` / `GPIO18` | `09_LVGL_V8_Test/user_config.h` |
| Battery power latch (`BAT_Control`) | `GPIO17`, HIGH holds the battery rail | `07_BATT_PWR_Test/{user_config.h,src/power/board_power_bsp.cpp}` |
| microSD | **1-bit SDMMC**: CLK `GPIO39`, CMD `GPIO41`, D0 `GPIO40`; no CS, no card-detect, no power-enable GPIO | `04_SD_Card/sdcard_bsp.cpp`, `11_FactoryProgram/.../epaper_config.h`, official schematic |

The pin macros live in `config.h`; nothing about recording, upload, the ingress
protocol or the multipart body changed.

Two extra checks were made while verifying the version question:

* The official V1 and V2 example trees were diffed: their `user_config.h` pin
  maps are **identical**, so the V1/V2 difference is not a pin-mapping difference.
* The FT6336 example exists only in the V2 tree (`13_FT6336_Test` for ESP-IDF,
  `12_FT6336_Test` for Arduino), i.e. the Touch board is described by the V2
  examples. Its touch and ADC values were cross-checked in both the Arduino and
  the ESP-IDF examples and agree (`0x38` / RST `7` / INT `21`, ADC1 channel 3 with
  a x2 divider).

### Architecture

```text
RecordingApp ──uiSnapshot()──► UiController ──UiView──► ui_draw_screen() ──► GfxCanvas
     │                              │                                            │
     │                              ├── Ft6336Touch (tap → tag, Idle only)        │
     │                              ├── BatteryMonitor (30 s)                     │
     │                              └── TimeManager ──► RtcPcf85063 (UTC)         │
     │                                       │      └─ SNTP (Wi-Fi only)          │
     │                                       └─ time_zone.cpp: UTC → Europe/Lisbon│
     │                                                                           ▼
     └── never renders, never blocks the panel ◄──────────── EpdDisplay (SPI2, full/partial)
```

| File | Role |
| --- | --- |
| `board_power.{h,cpp}` | battery power latch (`GPIO17`); asserted as the **first** statement of `setup()` (v0.4.1 property restored) |
| `voice_tags.h` | the four tags and their labels - the only place the strings exist |
| `tag_selection.h` | selected tag vs tag frozen at `startRecording()` |
| `ui_layout.h` | 200x200 geometry, tag rectangles, hit test |
| `ui_model.h` | state -> screen, refresh policy, transient deadlines (pure) |
| `ui_screens.{h,cpp}` | the painter: view model -> pixels (pure, host-tested) |
| `gfx_canvas.{h,cpp}` | 1bpp framebuffer primitives + 5x7 font renderer (pure) |
| `font5x7.h` | generated glyph table |
| `frank_sprites.h` | generated Frank asset pack: eight 200x200 frames + eight 96x52 face overlays, 1bpp `PROGMEM` |
| `frank_face.{h,cpp}` | Frank's boot/shutdown animations and the sprite -> framebuffer copy |
| `epaper_display.{h,cpp}` | SPI/GPIO/LUT/refresh driver (official sequence) |
| `touch_ft6336.{h,cpp}` | FT6336 polling driver |
| `battery_monitor.{h,cpp}` | ADC1 battery monitor |
| `rtc_pcf85063.{h,cpp}` | PCF85063 driver: full UTC calendar read/write |
| `time_zone.{h,cpp}` | POSIX-TZ timezone install + UTC -> local conversion (pure) |
| `time_manager.{h,cpp}` | timezone + NTP + RTC discipline + `HH:MM` for the UI |
| `i2c_bus.{h,cpp}` | the single shared `Wire` instance |
| `ui_controller.{h,cpp}` | event-driven decisions and flushing |

No LVGL and no extra Arduino library: four static screens on a monochrome panel
are cheaper and far more predictable as a small direct framebuffer layer. The
sketch still links only core libraries.

### Frank: the boot and shutdown animations

Frank is the device's character - pixelated Frankenstein - and he is drawn only by
`frank_face.{h,cpp}`, from the generated pack in `frank_sprites.h`. Two sequences
exist, and nothing else in the firmware knows a sprite state:

```text
power on    (persisted FRANK_DEAD) -> CLOSED -> AWAKE -> READY (home)
power off   (home) -> AWAKE -> BLINK -> CLOSED -> DEAD -> deep sleep / latch off
```

* **Boot** runs in `setup()` through `UiController::playBootAnimation()`, right
  after the panel is up and before Wi-Fi. It is deliberately only two frames -
  "he opened his eyes" - because it sits on the critical path to the home screen:
  no dead frame, no blink, no mouth animation. It is a *transition*: as soon as it
  finishes, the existing `UiController::update()` paints the normal home screen
  from `loop()`, and the animation is never a screen of its own. The first home
  screen is forced onto the full waveform (by setting the existing
  "partials since the last full refresh" counter to its limit), because a partial
  paint over Frank's dark head would ghost behind the tag grid.
* **The dead image is not redrawn at boot.** The panel is bistable, so the
  `FRANK_DEAD` face the previous shutdown left is what the user sees while the
  device is off. The driver's own init (`EpdDisplay::begin()`) does clear the panel
  to white before the first frame - that is the mandatory base-image/full-refresh
  sync the SSD1681 needs before any partial update - so the visible sequence at
  power-on is `X X` (persisted, until the first driver command) -> white (init) ->
  `— —` -> `• •` -> home. Two partial updates, no extra full refresh.
* **Shutdown** runs inside `UiController::showShutdownScreen()`, i.e. from the one
  graceful-shutdown sequence, after every inhibition check and after the card and
  the radio are safe, immediately before `BoardPower::powerOff()`. A refused or
  inhibited shutdown never reaches it, so no animation can play when the device
  stays on.
* **Assets.** Each state has a complete 200x200 frame (5000 B) and a 96x52 face
  overlay (624 B). `frankBlitPacked()` copies a packed bitmap straight into the
  panel framebuffer, one bit per pixel, **bit 7 = leftmost, set bit = ink**; the
  panel wants the opposite polarity, so the copy inverts - and that is the only
  place the two conventions meet. Every full frame is a `const` array, read from
  flash in place: no frame is ever copied into RAM, and the only framebuffer is
  the driver's existing 5000-byte one.
* **Refresh.** Both boot frames use the **partial** waveform. The first one must be
  a *complete* sprite (the framebuffer holds the driver's white boot picture, so a
  face-only patch would leave two eyes floating with no head) and it is safe with
  the fast waveform because `begin()` has just written a base image that matches
  what the panel displays - exactly what a partial update needs. The second frame
  copies only the face overlay, which is exactly equivalent because the animated
  states differ from each other *only* inside that 96x52 rectangle -
  `tests/frank_face_test.cpp` asserts that, and it is also why `FRANK_RECORDING`
  (which also changes pixels elsewhere) is never animated face-only. The shutdown
  animation uses the partial waveform for its three opening frames and ends on
  `FRANK_DEAD` as a complete sprite with a **full** refresh that is waited for.
  Expect a boot to cost two partial updates plus the home screen's one full
  refresh (~0.4 s each frame, ~2.4 s for the home), and a shutdown about
  `3 partials + 1 full refresh`; the panel, not the animation code, sets that pace.
  `min_visible_ms` in the frame tables is a floor, never a delay for looks.
* **Failure.** Every frame is bounded by the panel BUSY deadline
  (`VM_EPD_BUSY_TIMEOUT_MS`) and the frame lists are compile-time arrays, so an
  animation can neither run forever nor hang a boot or a shutdown. A panel that
  fails sets the driver's fault flag and the firmware carries on without a face;
  a shutdown whose animation fails falls back to the `POWERED OFF` screen.
* **Unused states stay available.** `FRANK_HALF_OPEN`, `FRANK_BLINK`,
  `FRANK_HAPPY`, `FRANK_RECORDING` and `FRANK_ERROR` are still in the pack and
  still reachable on demand through `frankShow(display, state)` (the entry point
  for a future RECORDING face, a HAPPY confirmation or an ERROR screen); the boot
  simply no longer visits them.

### Screens

| Screen | Shown when | Content |
| --- | --- | --- |
| READY | `Idle` + Wi-Fi up | time, Wi-Fi, battery, `READY`, a storage status line, 2x2 tag grid, `Hold BOOT to record` |
| READY OFFLINE | `Idle` + Wi-Fi down | `OFFLINE`; recording stays allowed and the note is queued on the card |
| RECORDING | `Recording` | `● REC`, `MM:SS`, the frozen tag, `Release to finish` |
| MAX 45s | `MaxReachedWaitingRelease` | `MAX 45s`, `Release BOOT` |
| SAVING | `Saving` | `SAVING`, `to microSD`, the frozen tag, `Do not remove card` |
| UPLOADING | `Uploading` (fallback path) | `UPLOADING`, static arrow, `Tag · Ns`, `Please wait` |
| RETRY WAIT | `RetryWait` (fallback path) | `WAITING FOR WIFI`, `Note preserved`, `RETRYING` |
| SENT | upload accepted (`Idle` only) | check mark, `SENT`, `Voice saved` for 1.5 s, then READY |
| BUSY hint | BOOT pressed while the buffer is busy | small inverted `BUSY` box in the footer, 1 s |

The READY screen carries one line of storage truth between the heading and the
tag grid - the only place the persistent queue is visible:

| Condition | Line |
| --- | --- |
| storage I/O failure, card gone after a failure | `SD ERROR` |
| last commit refused for space | `SD FULL` |
| no card mounted | `NO SD` |
| notes waiting, an upload is in flight | `up 3 pending` |
| notes waiting, idle link | `3 pending` |
| healthy card, empty queue | (nothing is drawn) |

Background uploads deliberately do **not** take over the screen: the panel shows
READY with the pending count so the user can start the next recording, which is
the whole point of the queue. The SENT confirmation is still shown when the
ingress accepts any queued note.

No upload percentage is shown: the uploader reports no progress, so inventing one
would be a lie. The arrow is static because an e-paper must not pretend to
animate.

### Layout and touch hitboxes

200x200 panel, 8 px outer margin, top status bar with a rule at `y = 20`, the tag
grid between `y = 100` and `y = 165`, footer text at `y = 180`.

| Tag | Hitbox (x, y, w, h) | Pixels covered |
| --- | --- | --- |
| Work | `8, 100, 89, 30` | x 8..96, y 100..129 |
| Idea | `103, 100, 89, 30` | x 103..191, y 100..129 |
| Todo | `8, 136, 89, 30` | x 8..96, y 136..165 |
| Personal | `103, 136, 89, 30` | x 103..191, y 136..165 |

The selected tag is drawn inverted (black box, white label); exactly one tag is
ever inverted. A tap outside those four rectangles changes nothing. The same
numbers are printed at boot:

```text
[ui] hitbox Work x=8 y=100 w=89 h=30
[ui] hitbox Idea x=103 y=100 w=89 h=30
[ui] hitbox Todo x=8 y=136 w=89 h=30
[ui] hitbox Personal x=103 y=136 w=89 h=30
```

### Refresh strategy

The panel is never redrawn per `loop()`. A render happens only when something
visible changed: screen, tag, Wi-Fi, battery (>= 5 %), clock minute, the recording
second, or one of the two transient hints.

| Situation | Waveform | Why |
| --- | --- | --- |
| Boot | full, once | official init: clear, base image, then partial mode |
| Any screen change while `Idle` | full | large visual change, and `Idle` is the only state where blocking costs nothing |
| Return to READY after a cycle | full | also clears the ghosting accumulated during the cycle |
| Screen change during `Recording` / `Uploading` / `RetryWait` / `MaxReached` | partial | blocking would cost audio samples, a BOOT press, or the release that starts the upload |
| SENT and BUSY overlays | partial | a multi-second flashing refresh would outlive their own display window |
| Recording timer | partial, at most 1/s | keeps the timer live without touching the audio path |
| Every 20 partials while `Idle` | full | documented ghosting bound (`VM_UI_FULL_REFRESH_EVERY_PARTIALS`) |

Two deliberate deviations from the official demo, both required here:

1. **Partial refreshes are asynchronous.** The official `EPD_DisplayPart()`
   waits for the panel BUSY line; this firmware issues the update and returns, so
   the main loop keeps feeding I2S. If the panel is still busy when a new update
   is due, the update is deferred to a later `loop()` iteration instead of being
   waited for. Blocking waits still use `vTaskDelay()`, so the idle task is never
   starved and the task watchdog cannot fire.
2. **BUSY waits are bounded** (`VM_EPD_BUSY_TIMEOUT_MS`). The official driver
   loops forever; a disconnected panel here degrades to "UI disabled" and the
   voice memo keeps working.

Partial refresh is the panel's own partial waveform driving the whole
framebuffer (`0x22, 0xCF`), exactly as the official demo does for LVGL, so it is
fast and flash-free but not a sub-window update. Ghosting is bounded by the rule
above.

### Touch

Polled from the main loop at 50 ms; no ISR, no LVGL indev. A tap is reported once
per press edge, so a resting finger cannot repeat a selection. I2C is not touched
at all while `Recording` or `MaxReachedWaitingRelease`; in `Uploading`/`RetryWait`
taps are still polled so the refusal can be logged:

```text
[ui] touch x=61 y=118
[ui] tag selected=Idea
[ui] touch x=120 y=118
[ui] tag selection ignored in state=uploading
[ui] touch outside the tag grid; selection unchanged
```

Coordinates are used exactly as the official FT6336 example reports them (raw
x/y, clamped to 200, no rotation or swap). The official examples never combine
touch with the display, so this is the only orientation the vendor documents; see
"Residual risks" below.

### Tags, and what is (not) sent to the server

`VoiceTag` is a central enum with a central `voiceTagLabel()`; no label string is
hardcoded anywhere else. `TagSelection` keeps two values apart:

* `selected` - the choice for the next recording, changed by touch in `Idle`;
* `recording` - frozen by `startRecording()`.

So a tap during Recording, Uploading or RetryWait can never rewrite the metadata
of audio that already exists, and the next recording picks up whatever is
selected by then. This stage does **not** transmit the tag: the ingress contract,
the multipart body, QueueDB and Notion are untouched. The snapshot field is the
hook for a later stage.

### Battery

`BatteryMonitor` uses the officially confirmed mapping (ADC1 channel 3 / GPIO4,
12 dB, curve-fitting calibration, x2 divider, 8-sample average) and maps the
voltage to a percentage with a documented, monotonic Li-ion curve
(`battery_level.h`). It is not a fuel gauge.

Sampled every 30 s and re-rendered only on a change of at least 5 %. If the rail
reads below 2.5 V - USB only, no battery, or the `-EN` model without a battery -
the UI shows `--%` instead of a misleading `0%`.

### Date and time

The PCF85063 only counts seconds: it has no concept of a timezone, a UTC offset
or daylight saving. The firmware therefore uses it strictly as a **UTC** source
and converts to **Europe/Lisbon** local time only when the top bar is painted:

```text
PCF85063 UTC ──► TimeManager ──► time_zone.cpp ──► localtime_r() ──► "HH:MM"
   (or NTP)         │              (setenv TZ + tzset)
                    └── writes UTC back into the RTC after a valid NTP answer
```

* `rtc_pcf85063.{h,cpp}` reads and writes the full calendar (0x04..0x0A) as UTC,
  forces 24-hour mode and starts the oscillator exactly like the vendor
  `SensorPCF85063::initImpl()`. Its OS (oscillator stop) flag is respected: a
  never-set clock is invalid, not "00:00".
* `time_zone.{h,cpp}` installs the mainland-Portugal POSIX rule
  `WET0WEST,M3.5.0/1,M10.5.0/2` with `setenv("TZ", ...)` + `tzset()` and then
  converts with `localtime_r()`. That is the same rule tzdata applies to
  `Europe/Lisbon`, so the offset is **UTC+0 in winter (WET)** and **UTC+1 in
  summer (WEST)**, switching automatically on the official dates - last Sunday of
  March at 01:00 UTC and last Sunday of October at 01:00 UTC. No fixed offset and
  no stored "+1 hour" exists anywhere in the firmware.
* `time_manager.{h,cpp}` owns the policy: it applies the timezone at boot, reads
  the RTC, and - only while Wi-Fi is up - calls `configTzTime()` once, which
  starts SNTP **without blocking**. Once `time()` reports a real time, the UTC
  instant is written back into the PCF85063. A healthy RTC is only rewritten when
  it disagrees with the NTP clock by more than 2 s, and SNTP is re-armed at most
  every 6 h; there is no continuous server polling and no per-`loop()` redraw.

Offline behaviour is deliberate: **RTC UTC → local conversion → local time**,
with no network involved. If the RTC is absent, invalid, or its OS flag is set
*and* NTP has not synchronised yet, the top bar shows `--:--` rather than a
plausible-looking wrong clock. If the RTC is dead but NTP is available, the
NTP-disciplined system clock is displayed instead. The UI only reads
`TimeManager::timeText()`; it contains no timezone logic.

The RTC is *defined* as UTC from this version on. A device whose PCF85063 was
left holding something else (it was never written to by the previous firmware -
there was no write path) is repaired automatically by the first NTP sync, which
rewrites the whole calendar as UTC; offline it simply shows what the chip holds,
or `--:--` when the chip says it was never set.

Both event-driven serial lines are printed once, not per loop:

```text
[time] timezone: WET0WEST,M3.5.0/1,M10.5.0/2 (Europe/Lisbon, WET/WEST)
[time] RTC UTC: 2026-01-15 12:00:00
[time] Lisbon local: 2026-01-15 12:00:00 (WET, UTC+0)
[time] NTP request sent to pool.ntp.org / time.google.com
[time] NTP synchronized
[time] RTC updated from NTP (UTC 2026-09-15 12:00:00)
[time] RTC UTC: 2026-09-15 12:00:00
[time] Lisbon local: 2026-09-15 13:00:00 (WEST, UTC+1)
```

and at a cold boot with a dead RTC and no Wi-Fi:

```text
[time] no trustworthy time (RTC invalid and NTP not synchronized); UI shows --:--
```

### Shared I2C

ES8311, FT6336 and the PCF85063 share SDA 47 / SCL 48. `i2c_bus.cpp` owns the
single `Wire` instance (100 kHz, the clock the audio path was validated with) and
every peripheral reuses it; `audio_codec_es8311.cpp` now calls `vm_i2c_begin()`
instead of `Wire.begin()`. No second bus, no second `Wire.begin()`. Audio capture
stays on I2S and is untouched.

### Concurrency

The display is driven from the Arduino loop task only, after
`RecordingApp::tick()`, so the BOOT edge is always sampled before the panel is
touched. The `vm_upload` task still does nothing but network I/O, its priority is
unchanged, and it never calls into the UI. All UI cross-checks read a plain
snapshot that the upload task influences only through the existing result queue.

### UI serial reference

```text
[power] battery latch gpio=17 level=HIGH
[power] PWR key gpio=18 active_low=1 debounce_ms=50
[power] auto power-off enabled timeout_ms=120000
[power] deep-sleep wake on PWR enabled (USB-attached off state)
[epd] panel ready in 2134 ms (official Waveshare driver, SPI2 40000000 Hz)
[frank] boot animation
[frank] frame closed partial 402 ms
[frank] frame awake partial 371 ms
[frank] boot animation done in 773 ms
[ui] touch ready: FT6336 addr=0x38 rst=7 int=21 (idle level=1)
[ui] ready: touch=yes battery=yes rtc=yes
[battery] ADC1 channel 3 (GPIO4) ready; divider ratio x2; update every 30000 ms
[rtc] PCF85063 detected at 0x51 (24-hour mode, oscillator running)
[time] timezone: WET0WEST,M3.5.0/1,M10.5.0/2 (Europe/Lisbon, WET/WEST)
[time] RTC UTC: 2026-01-15 12:00:00
[time] Lisbon local: 2026-01-15 12:00:00 (WET, UTC+0)
[time] NTP synchronized
[time] RTC updated from NTP (UTC 2026-01-15 12:00:01)
[ui] touch x=61 y=118
[ui] tag selected=Idea
[ui] partial refresh screen=recording 1 ms
[ui] full refresh screen=ready 2408 ms
[ui] showing SENT (uploads accepted=1)
[ui] SENT visible for 1500 ms
```

Set `VM_ENABLE_UI 0` in `config.h` to build the pre-UI firmware (audio + upload
only); the UI translation units are then dropped by the linker.

The `[frank] frame ... ms` values are the real refresh time of each frame, which
is what sets the pace of the animation: the illustrative numbers above assume
~0.4 s per partial boot frame. They are **to be confirmed on the bench** - the log
is the measurement.

### Residual risks (not verifiable without the board)

* **Touch orientation.** The vendor example reports raw x/y clamped to the panel
  size and applies no transform, which is what this firmware assumes. If a tap on
  Work selects a different tag, the fix is the single coordinate assignment in
  `Ft6336Touch::readTouch()` - the boot log prints the tap coordinates so the
  mapping can be identified immediately.
* **Panel timing.** Partial update time, ghosting rate and the 20-partial bound
  are engineering estimates; the serial log reports the measured refresh time so
  the number can be tuned. That now includes Frank's animation: how the partial
  waveform renders the large black areas of a complete head (the boot's first
  `closed` frame and the shutdown's frames) and how much of the home screen ghosts
  behind the shutdown frames cannot be seen without the board. The two safety
  valves are one-line changes in `frank_face.cpp`: promote a frame's
  `full_refresh` to `true` (costs ~2.4 s, guarantees the image - it is the whole
  cost the boot used to pay) or raise its `min_visible_ms` floor.
* **Battery curve.** The voltage -> percentage curve is an approximation, not a
  datasheet characteristic.
* **Power latch.** `GPIO17` HIGH is the official latch semantics, but the fix can
  only be confirmed on battery: the bootloader window before `setup()` is not
  covered by firmware, so a PWR press shorter than the boot time cannot latch the
  rail. See "Battery latch physical test" above.

## microSD persistence and the offline queue (v0.5.0)

**PSRAM is a temporary capture buffer. microSD is persistence and the offline
queue.** That one sentence is the whole design:

| Storage | Role | Lifetime of the data |
| --- | --- | --- |
| PSRAM (1.44 MB, OPI) | the single capture buffer: I2S fills it and the WAV header is finalized in place | from `BOOT` press until the recording is committed to the card (or dropped in TEST A) |
| microSD (FAT32) | durable copy of every committed recording plus its metadata, and the queue the background uploader drains | from the atomic commit until the ingress confirms delivery |

Once a recording is committed, the firmware no longer depends on PSRAM for it.
Losing Wi-Fi, rebooting, removing and reinserting the card, or a failed upload
cannot lose it. While older notes are queued or uploading the user can keep
recording: capture is the only thing serialized, not upload.

### microSD hardware map (official sources)

The card is wired as **1-bit SDMMC**, not SPI. That is the vendor's own
implementation, not a preference:

```text
waveshareteam/ESP32-S3-ePaper-1.54 @ 9957d0f4
  02_Example/Arduino/04_SD_Card/sdcard_bsp.cpp
      #define SDMMC_D0_PIN  GPIO_NUM_40
      #define SDMMC_CLK_PIN GPIO_NUM_39
      #define SDMMC_CMD_PIN GPIO_NUM_41
      sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
      slot_config.width = 1;              // 1-wire SDMMC
      slot_config.clk = SDMMC_CLK_PIN;
      slot_config.cmd = SDMMC_CMD_PIN;
      slot_config.d0  = SDMMC_D0_PIN;
      host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
  02_Example/ESP-IDF/V{1,2}/04_SD_Card/components/sdcard_bsp/sdcard_bsp.c   (identical)
  02_Example/ESP-IDF/V2/11_FactoryProgram/components/port_bsp/epaper_config.h
      #define SD_MISO_D0_PIN  GPIO_NUM_40
      #define SD_MOSI_CMD_PIN GPIO_NUM_41
      #define SD_CLK_PIN      GPIO_NUM_39
      #define SDlist          "/sdcard"

ESP32-S3-Touch-ePaper-1.54-Schematic.pdf (linked from the official
Resources-And-Documents page), SD_Card block / GPIO table:
      IO39 = SD_CLK, IO40 = SD_MISO, IO41 = SD_MOSI
```

| Item | Value | Source |
| --- | --- | --- |
| Interface | **1-bit SDMMC** (`slot_config.width = 1`) | `04_SD_Card/sdcard_bsp.cpp` |
| CLK | `GPIO39` | schematic net `SD_CLK` + official example |
| CMD / MOSI net name | `GPIO41` | schematic net `SD_MOSI` + official example |
| DATA0 / MISO net name | `GPIO40` | schematic net `SD_MISO` + official example |
| DATA1 / DATA2 | routed only to `NC` resistors, no ESP32 pin | schematic |
| DATA3 / CS | 10 kOhm pull-up only, no ESP32 pin | schematic |
| Card detect | hardwired to GND, no GPIO | schematic |
| SD power enable | none; socket VDD is on the always-on 3V3 rail | schematic (U7 MP1605, EN tied to VSYS) |
| Mount point | `/sdcard`, FAT32, `format_if_mount_failed = false` | official example |
| Clock | `SDMMC_FREQ_HIGHSPEED` (40 MHz) | official example |

There is no pin overlap with the e-paper (SPI2: DC 10, CS 11, SCK 12, MOSI 13,
RST 9, BUSY 8, PWR 6), touch/I2C (RST 7, INT 21, SDA 47, SCL 48) or audio
(MCLK 14, BCLK 15, WS 38, DIN 16, DOUT 45, PA 42/46). The only caveat is
silicon-level: 39/40/41 are also the ESP32-S3 external-JTAG pins, which is
irrelevant unless the JTAG eFuses are burned; this board debugs over the built-in
USB_SERIAL_JTAG on IO19/IO20.

The firmware uses the Arduino `SD_MMC` wrapper, which drives exactly that ESP-IDF
SDMMC driver: `SD_MMC.setPins(39, 41, 40)` + `SD_MMC.begin("/sdcard", true)`
selects slot 1, 1-bit width, the official pins and the official mount point (the
core sets `slot_config.width = 1` when `mode1bit` is true). The mount is never
formatted; a card that cannot be mounted simply disables persistence.

### Modules

| File | Role | Arduino free? |
| --- | --- | --- |
| `vm_fs.h` | abstract volume + read handle, and the RAII volume lock | yes |
| `sd_storage.{h,cpp}` | the only file that includes `SD_MMC`: mount, pins, health, remount, self-test | no |
| `recording_paths.h` | the whole on-card layout in one place | yes |
| `recording_meta.{h,cpp}` | metadata struct + canonical flat JSON + strict parser | yes |
| `record_crc32.{h,cpp}` | CRC-32 (IEEE) over a byte range | yes |
| `recording_store.{h,cpp}` | semantic queue: atomic commit, boot recovery, item lifecycle, backpressure, ordered cache | yes |
| `upload_source.h` | `ByteSource` + `MemoryByteSource` for the multipart body | yes |
| `uploader.{h,cpp}` | streaming multipart upload (one framing for RAM and card) | no |
| `tests/memory_file_system.h` | in-memory volume with fault injection for the host tests | yes |

Nothing outside `sd_storage.cpp` calls `SD_MMC`, and nothing outside
`recording_store.cpp` builds a path.

### Card layout

```text
/voice_memo/
    pending/     <recording_id>.wav + <recording_id>.json   committed, not yet delivered
    uploading/   <recording_id>.wav + <recording_id>.json   an attempt was in flight
    tmp/         <recording_id>.wav.tmp / .json.tmp         never a valid recording
    corrupt/     quarantined files that failed validation
    sent/        optional retention (VM_SD_KEEP_SENT = 1), otherwise unused
```

Nothing else on the card is touched. The mount point is `/sdcard`; the firmware
only ever writes under `<root>` (`VM_SD_ROOT`, default `/voice_memo`).

### Commit protocol (atomicity)

A finalized WAV becomes durable through exactly one rename:

```text
1. write /voice_memo/tmp/<id>.json.tmp     metadata first: it is the intent record
2. write /voice_memo/tmp/<id>.wav.tmp      the audio, still not a recording
3. rename tmp/<id>.json.tmp -> pending/<id>.json
4. rename tmp/<id>.wav.tmp  -> pending/<id>.wav     <-- the commit point
```

Only after step 4 does the firmware consider the recording persisted and free
the PSRAM buffer. A crash before step 4 leaves only `.tmp` files, which are
discarded at boot (nothing was ever confirmed). A crash *between* steps 3 and 4
leaves a committed sidecar plus a tmp WAV, and recovery finishes the commit
because the sidecar proves the intent. Either way no partially written file can
ever look like a valid recording, and a recording never disappears between
"finalize" and "enqueue".

### Metadata schema

`<recording_id>.json`, a flat, human-readable object, one key per line:

```json
{
  "schema_version": 1,
  "recording_id": "rec-00000001-0000ABCD",
  "device_id": "bel-esp32-01",
  "tag_index": 1,
  "duration_ms": 5230,
  "wav_bytes": 209244,
  "sample_rate": 16000,
  "created_utc": 1768478400,
  "crc32": 2914185633,
  "attempt_count": 0,
  "sequence": 7
}
```

| Field | Meaning |
| --- | --- |
| `schema_version` | bumped on an incompatible change; a newer file is rejected rather than guessed at |
| `recording_id` | idempotency key shared with the ingress, **never regenerated on retry** |
| `device_id` | same value the multipart field carries |
| `tag_index` | `VoiceTag` index frozen at `startRecording()` (the tag survives a reboot) |
| `duration_ms` | advertised to the ingress |
| `wav_bytes` | complete WAV size (44-byte header + PCM), re-checked against the card at boot |
| `sample_rate` | 16000, stored so recovery can re-validate the header |
| `created_utc` | Unix UTC seconds, or `0` when no trustworthy clock existed (offline boot with a never-set RTC) |
| `crc32` | CRC-32 (IEEE) of the complete WAV; its presence is the "computed" flag, so a legitimate 0 is not confused with "absent" |
| `attempt_count` | failed attempts so far; diagnostics only, never used for correctness |
| `sequence` | persistent FIFO order assigned by the store (`0` only for a WAV recovered without a sidecar) |

The parser accepts exactly one flat object of string/number values and rejects
anything else, so a truncated or corrupt sidecar is detected instead of being
half-applied. Unknown keys are ignored, so a newer writer cannot break an older
reader.

JSON was chosen over SQLite deliberately: the queue is bounded to a few dozen
items on a FAT32 card, and a one-parse, inspectable, trivially recoverable file
is the cheapest thing that survives a power cut.

### Recording lifecycle (what changed)

```text
before (v0.4.1)                          now (v0.5.0, card present)

BOOT -> Idle                             BOOT -> recover persistent queue -> Idle
  |                                        |
press -> Recording                       press -> Recording
  |                                        |
release -> finalize WAV in PSRAM         release -> finalize WAV in PSRAM
  |                                        |
  +-> Uploading (buffer frozen)            +-> Saving (buffer frozen; SD commit)
  |     |                                  |     |
  |     +- 2xx -> Idle (buffer freed)      |     +- Ok -> Idle (buffer freed, item queued)
  |     +- err -> RetryWait (frozen)       |     +- full/error -> Uploading/RetryWait (fallback)
  |                                        |
  +- RetryWait blocks a new recording      +-> uploader drains the queue in background,
                                                 in any state; a new recording is allowed
```

`Idle` is now the normal state after a recording even while uploads are pending.
A queued upload is started while `Idle` and keeps running while the next note is
captured; only the single PSRAM capture buffer is serialized, and no new card
work is started during capture (see [Concurrency](#concurrency)).

| State | Meaning | New recording? |
| --- | --- | --- |
| `Idle` | buffer free; the queue may be non-empty | yes |
| `Recording` | capturing into PSRAM | already recording |
| `MaxReachedWaitingRelease` | 45 s hit, waiting for the release | no |
| `Saving` | the frozen WAV is being committed to the card | no (the PSRAM copy is still the only one) |
| `Uploading` | volatile fallback: the PSRAM WAV is being POSTed (no card) | no |
| `RetryWait` | volatile fallback: the POST failed, same WAV kept | no |

`Uploading`/`RetryWait` are now the **fallback** path, used only when the card is
absent, full or unhealthy. With a healthy card the firmware never enters them.

### Uploading from the card

The worker opens the WAV through a locked handle and copies it into the
multipart body in `VM_UPLOAD_CHUNK_BYTES` (4 KB) chunks. The whole file is
**never** copied into RAM: it is either in PSRAM (volatile fallback) or streamed
from the card chunk by chunk. `Content-Length` comes from the metadata, so the
body is still a single multipart POST to the same endpoint.

Preserved unchanged:

* `recording_id` and `device_id` (idempotency keys, identical on every retry);
* the tag (in the metadata; not yet a multipart field, exactly as before);
* the endpoint `POST /api/v1/audio` and the base-URL normalization;
* the multipart field set and `firmware_version`;
* `201` = accepted, `200` = already known, anything else = failure.

The WAV is deleted (or moved to `sent/`) **only** after a valid HTTP
confirmation. A failed attempt keeps the file and the id and retries every
`VM_SD_QUEUE_RETRY_INTERVAL_MS`. Nothing is ever left "stuck" in `uploading/`: an
attempt moves the item there before the POST, and recovery returns it to
`pending/` after a reboot.

A recording the ingress keeps rejecting does **not** starve the queue: once an
item reaches `VM_SD_QUEUE_MAX_ATTEMPTS` failed attempts it is skipped in favour
of newer notes and retried only at `VM_SD_QUEUE_MAX_BACKOFF_MS`. It is never
deleted or quarantined for failing to upload - it stays on the card with its id
and is delivered as soon as the server accepts it.

`recording_id` is minted once per recording and reused on every retry. It mixes
in a hardware random nonce, so an id can no longer collide with one from an
earlier boot; if the card nevertheless already holds a recording with the same
id, the commit is refused, a fresh id is minted and the commit is retried (the
audio has not been uploaded yet, so this is safe), which prevents the ingress
from silently deduplicating a brand-new note as `already_known`.

### Recovery on boot

`RecordingStore::begin()` mounts the card, creates the layout and rebuilds the
queue without trusting any previous RAM state:

| Case | Result |
| --- | --- |
| A. reboot during capture, before saving | PSRAM audio is lost - accepted, documented |
| B. reboot during the `.tmp` writes | tmp files are discarded; they never become `pending/` |
| C. reboot between the two commit renames | the tmp WAV is promoted because the sidecar proves the intent; if that promotion cannot be done, the tmp WAV and sidecar are **kept** for the next boot, never deleted |
| D. reboot during an upload | `uploading/` items are moved back to `pending/` and retried; an item that cannot be moved is still queued from `uploading/` |
| E. upload confirmed, reboot before cleanup | the ingress is idempotent on `recording_id`; the replay returns `200 already_known` and the item is removed |
| F. `pending/*.json` with no audio | quarantined to `corrupt/` (never guessed at), unless a `tmp/<id>.wav.tmp` is still there because the commit can still be completed |
| G. `pending/*.wav` with no sidecar | validated from its header, hashed, given a fresh sidecar carrying the real CRC and kept - the audio survives |
| H. corrupt or structurally invalid audio | quarantined to `corrupt/`, never uploaded |

Recovery reads only a 44-byte header per file (cheap); a WAV found without a
sidecar is the one case that is hashed in full, so its rebuilt sidecar is
truthful rather than a sentinel. Re-hashing every *other* pending WAV at boot is
opt-in with `VM_SD_VERIFY_CRC_ON_BOOT 1`.

### Working without a card / card removed / card full

* **No card at boot.** The mount fails without formatting, the boot continues,
  the UI shows `NO SD`, and the firmware behaves exactly like v0.4.1: one PSRAM
  buffer that must be uploaded (or retried) before the next recording. This is
  the deliberately lowest-regression fallback.
* **Card removed during operation.** Every filesystem call returns `false`
  instead of aborting; no `File` is ever used after a failure. A failed rename
  or read on the upload path also marks the volume unhealthy, so the remount and
  the recovery scan are triggered even when no new recording is made. The volume
  is remounted at most every `VM_SD_RETRY_MOUNT_MS` (30 s) - never while an
  upload handle is open, and never in a tight loop. A recording already
  committed is never "forgotten": it is still on the card and reappears in the
  queue after the remount. The UI shows `SD ERROR`. A recording being finalized
  while the card is gone falls back to the volatile path so it is not lost
  either. The queue head can never wedge: an item whose WAV is *provably* gone
  (checked only on a healthy, mounted volume) is dropped from the queue, and a
  recording the ingress keeps rejecting is skipped, not deleted.
* **SD full.** Before writing, the store checks both the free-space reserve and
  the queue length. When either would be crossed the commit is refused, nothing
  on the card is touched, no pending recording is deleted, the UI shows
  `SD FULL`, and the new recording is kept in PSRAM and sent through the volatile
  fallback so it can still be delivered. Space is reclaimed only by confirmed
  uploads.

### Retention policy

Default: after a confirmed upload the WAV and its sidecar are deleted. That is
`VM_SD_KEEP_SENT 0`. Setting it to `1` moves both files to `sent/` instead, which
is the hook for a future retention window. `sent/` is never scanned at boot and
never blocks the queue.

### CRC and integrity

CRC-32 (IEEE) is computed once, when the recording is committed, and stored in
the sidecar. Boot recovery always runs the cheap structural checks:

* file exists and is at least 44 bytes;
* `RIFF` / `WAVE` / `fmt ` / `data` markers, 16-byte PCM fmt chunk, format 1;
* one channel, 16 bits, 16000 Hz;
* the `data` chunk exactly fills the file and the `RIFF` size matches it;
* an even payload (whole 16-bit samples).

The full CRC re-hash is `VM_SD_VERIFY_CRC_ON_BOOT` (default `0`, because hashing
a full queue would add seconds to the boot). The CRC is always written, so it is
available for diagnostics and can be verified on demand.

### Concurrency

The volume is shared by the Arduino loop task (commit, mark, remove) and the
`vm_upload` task (sequential reads of a pending WAV). All access goes through a
single **recursive** FreeRTOS mutex owned by the storage backend:

```text
lock -> open / rename / read chunk -> unlock      (short filesystem operations)
HTTP write                                        (no lock held)
lock -> mark uploaded / mark pending -> unlock
```

A recursive mutex is required because `RecordingStore` holds the lock across a
multi-step atomic commit while the per-operation backend calls take it again; a
plain mutex would deadlock the task against itself. The mutex is never held
across HTTP. In practice contention is nil: the main loop only issues a new job
after the previous outcome was drained (`jobInFlight_`), so the two tasks
alternate rather than overlap.

Nothing filesystem-heavy ever runs while audio is being captured:

* the remount + recovery scan is deferred out of
  `Recording`/`MaxReachedWaitingRelease`/`Saving`;
* a queued upload is *started* only in `Idle` (an upload already in flight still
  finishes during a recording - that is the point of the queue);
* a queued upload's outcome is held back while capture is running
  (`markUploaded` removes a ~1.4 MB file, `markPendingAgain` renames and rewrites
  a sidecar), then applied as soon as capture ends.

A newly finalized recording takes priority over a queued upload: the loop sets an
abort flag, the worker stops at the next chunk, the item goes back to `pending/`,
and the commit runs immediately.

### Backpressure

Two independent limits, both checked before the card is touched:

| Limit | Default | Meaning |
| --- | --- | --- |
| `VM_SD_MAX_PENDING` | 64 | maximum recordings in the queue (~92 MB of 45 s notes) |
| `VM_SD_MIN_FREE_BYTES` | 1 MiB | space that must remain *after* the write, so the filesystem never fills up |

The queue is bounded on purpose: an unbounded offline queue would fill the card
silently.

### Configuration

| Flag (`config.h`) | Default | Meaning |
| --- | --- | --- |
| `VM_ENABLE_SD` | 1 | build the microSD queue (0 = exactly the v0.4.1 behaviour) |
| `VM_SD_ROOT` | `/voice_memo` | root of the layout on the card |
| `VM_SD_MOUNT_POINT` | `/sdcard` | SDMMC mount point |
| `VM_SD_CLK_PIN` / `VM_SD_CMD_PIN` / `VM_SD_D0_PIN` | 39 / 41 / 40 | official 1-bit SDMMC pins |
| `VM_SD_FREQ_KHZ` | 40000 | SDMMC clock (`SDMMC_FREQ_HIGHSPEED`) |
| `VM_SD_MAX_OPEN_FILES` | 8 | VFS file handle budget |
| `VM_SD_MAX_PENDING` | 64 | queue length limit |
| `VM_SD_MIN_FREE_BYTES` | 1 MiB | free-space reserve |
| `VM_SD_RETRY_MOUNT_MS` | 30000 | remount cadence after an I/O error |
| `VM_SD_KEEP_SENT` | 0 | delete after upload, or keep in `sent/` |
| `VM_SD_VERIFY_CRC_ON_BOOT` | 0 | full CRC re-hash during recovery |
| `VM_SD_SELF_TEST` | 0 | write/read/verify/remove a test file under `tmp/` on every boot |
| `VM_UPLOAD_CHUNK_BYTES` | 4096 | streaming chunk size for the multipart body |
| `VM_SD_QUEUE_RETRY_INTERVAL_MS` | 5000 | retry cadence for the persistent queue |
| `VM_SD_QUEUE_MAX_ATTEMPTS` | 10 | after this many failed uploads an item is skipped (not dropped) so it cannot starve newer notes |
| `VM_SD_QUEUE_MAX_BACKOFF_MS` | 600000 | slow retry interval for items at the attempt cap (10 min) |

### Serial log reference

```text
[sd] initializing...
[sd] mounted type=sdhc total=31914983424 free=31869104128
[sd] root=/voice_memo
[store] recovered=2 discarded_tmp=1 corrupt=0
[queue] pending=2
[store] saving id=rec-00000003-0000ABCD bytes=209244
[store] committed id=rec-00000003-0000ABCD bytes=209244 crc=ADBEEF12
[queue] pending=3
[upload] start id=rec-00000001-0000ABCD source=sd bytes=209244 duration_ms=6500 attempt=1
[upload] accepted id=rec-00000001-0000ABCD http=201 source=sd
[store] removed id=rec-00000001-0000ABCD
[queue] pending=2
[store] saving id=... (interrupted commit)          # recovery of case C
[store] interrupted commit not completed id=... (kept for the next boot)
[store] recovered id=... (interrupted upload)       # recovery of case D
[store] quarantined corrupt audio id=...
[store] dropped missing recording id=...
[store] recording_id already on the card; retrying as id=...
[queue] all items reached 10 attempts; retrying the oldest slowly
[sd] storage full (free space reserve); recording kept in RAM
[sd] write failed id=... bytes=... (io_error)
[sd] volume marked unhealthy; will retry mount
[sd] remounted; pending=3
[sd] card not present
```

Never printed: Wi-Fi passwords, `INGEST_TOKEN`, or audio content.

### microSD physical test script

**REQUIRES PHYSICAL VALIDATION.** Nothing below can be confirmed from code alone.

**Bring-up (once, before connecting the app):**

1. Set `VM_SD_SELF_TEST 1` in `config.h` and flash. With a FAT32 card inserted
   (16 MB-32 GB; the vendor states FAT32 is required) expect:

   ```text
   [sd] initializing...
   [sd] mounted type=sdhc total=... free=...
   [sd] self-test ok (write/read/verify/remove)
   ```

2. Confirm `/voice_memo/{pending,uploading,tmp,corrupt,sent}` exist on the card
   and that no other file was touched.
3. Set `VM_SD_SELF_TEST 0` again for production.

**End-to-end scenarios:**

| # | Scenario | Steps | Expected |
| --- | --- | --- | --- |
| 1 | Online | Wi-Fi up, ingress up, record A | `[store] committed`, `[queue] pending=1`, `[upload] start ... source=sd`, `[upload] accepted`, `[store] removed`, `pending=0`; no WAV left on the card |
| 2 | Offline queue | Wi-Fi down, record A, B, C | all three record back-to-back; `pending=3`; the panel shows `3 pending` in READY; the third press is *not* refused |
| 3 | Reboot offline | with 3 pending, power-cycle | `[store] recovered=0` (already committed), `[queue] pending=3`; the three notes still on the card |
| 4 | Reconnect | bring Wi-Fi up | `[queue] pending` falls 3 -> 2 -> 1 -> 0 in `sequence` order; the ingress log shows the three `recording_id`s |
| 5 | Reboot during upload | pull power while `[upload] started ... source=sd` is in flight | after boot: `[store] recovered id=...(interrupted upload)`, `[queue] pending=1`, the same `recording_id` re-uploaded; the ingress answers `201` or `200 already_known` |
| 6 | Card removed while Idle | pull the card | no crash, no reset; UI shows `SD ERROR` after the first failed access; uploads pause; reinsert -> `[sd] remounted` within 30 s and the queue is rebuilt |
| 7 | No card at boot | boot with the slot empty | boot completes; `[sd] card not present`; UI shows `NO SD`; recording still works via the PSRAM fallback |
| 8 | Full card | fill the card (or set `VM_SD_MIN_FREE_BYTES` very high in a test build) and record | `[sd] storage full ...; recording kept in RAM`; UI shows `SD FULL`; no pending file was deleted; the note is still uploaded when Wi-Fi returns |
| 9 | Recording while uploading | with a slow link, start recording B right after A was committed | `UPLOADING` is not shown for A (background), the panel stays usable, B records normally, both end up on the ingress |
| 10 | Bad card | insert a card the board cannot read | `[sd] card not present`; the firmware keeps working; no repeated reset |

### Performance notes

**REQUIRES PHYSICAL VALIDATION** for the numbers; what follows are the design
guarantees, which are testable from code:

* The queue is an **in-memory ordered cache**. `pendingCount()`, `oldestPending()`
  and the UI all read it, so the main loop never scans the card. The only scan is
  `recoverOnBoot()` (and a remount, which rebuilds it), where per file the cost is
  one 44-byte header read.
* The loop never blocks on the card: the commit and every upload run on the
  `vm_upload` worker. The only main-loop filesystem work is a rename/remove per
  attempt, and it happens after the outcome was drained.
* The atomic commit writes the WAV once (1.44 MB maximum, ~0.4-1.5 s at typical
  SDMMC speeds) and never copies it: `writeFile` streams the PSRAM buffer straight
  to the card. `SAVING` is visible on the panel for exactly that window.
* The uploader streams in 4 KB chunks; the peak added RAM is the chunk buffer on
  the worker stack plus the metadata String, never the WAV size. The worker stack
  is 12 KB for that reason.
* The persistent `sequence` makes ordering O(n log n) at recovery and O(1) per
  dequeue; a retry never reorders the queue.
* Measured on the bench, record: time to commit a 45 s note (`[store] saving` ->
  `[store] committed`), free heap/PSRAM before and after a commit and an upload,
  `uxTaskGetStackHighWaterMark` for `vm_upload`, and whether the BOOT button and
  audio stay clean while a background upload runs (scenario 9).

### Known limitations

* **REQUIRES PHYSICAL VALIDATION.** All of the above assumes the 1-bit SDMMC
  pins, the 40 MHz clock and the board's pull-ups behave as the vendor code and
  schematic describe. Nothing here was measured on hardware.
* The uploader does not report progress, so neither the UI nor the log invents a
  percentage (unchanged from v0.4.1).
* If the worker is blocked inside `WiFiClient::connect()` when a recording is
  finalized, the commit waits for that call to time out (bounded by
  `VM_UPLOAD_TIMEOUT_MS`, 15 s). The item is safe on the card and the SAVING
  screen is shown meanwhile; only starting a *new* recording is delayed. The
  response read has its own `VM_UPLOAD_TIMEOUT_MS` deadline, so a peer that
  leaves the connection open without sending anything can no longer spin the
  worker forever and hold the single job slot.
* Background uploads now run while audio is captured, which is new in v0.5.0.
  The worker is priority 1 and unpinned, so on the dual-core ESP32-S3 it should
  not disturb I2S; confirm audio quality with scenario 9 on the bench.
* FAT rename atomicity is the strongest guarantee FAT32 offers; a power cut
  exactly during the rename is handled by recovery but is not provable from code.
* Time on the card (`created_utc`) is 0 until the RTC or NTP is trustworthy;
  ordering uses the persistent `sequence`, so a missing clock never reorders the
  queue.

## Host-side tests

Ten pieces of firmware logic have no Arduino/Wi-Fi/FreeRTOS dependency and are
tested on the Mac without a board, including the whole persistent-store policy
(paths, metadata, atomic commit, recovery, queueing, retention), the whole power
policy (PWR arming, activity classification, inhibition and the shutdown
decision) and Frank's sprite pipeline (bit order, polarity, asset geometry). The
`tests/` directory is not part of the sketch build (Arduino only compiles the
sketch root and `src/`).

### Run every suite at once

```bash
cd voice_memo_esp32
./tests/run_host_tests.sh
```

It compiles and runs all ten compiled suites with `-Wall -Wextra` into
`$TMPDIR`, runs one source-level guard, and exits non-zero if anything fails. All
eleven steps pass: `ingest_target`, `recording_state`, `wifi_fallback`,
`time_zone`, `firmware_calc`, `recording_store`, `ui_model`, `frank_face`
(78 checks), `power_button` (41 checks), `power_manager` (74 checks) and
`power_critical_path_check`. The individual commands are listed below.

Ten of the eleven are compiled C++ suites. The eleventh,
`tests/power_critical_path_check.sh`, is a **source-level guard** and is
deliberately different: the battery boot critical path is a property of statement
order inside `setup()` — the VBAT latch must be asserted before any non-essential
initialization — and no executable host test can observe that, because it is not
runtime behaviour. The guard parses `setup()` and fails the run if:

* the first executable statement is not `boardPower.keepBatteryPowerOn();`
* `releaseSleepPadsIfNeeded()` is not the second
* a `Serial` call or a `delay()` appears before statement 3
* `GPIO17` is driven from anywhere other than `board_power.cpp`
* the `// Battery boot critical path:` comment is removed or reworded
* `keepBatteryPowerOn()` stops dropping the pad hold internally

It was verified to fail against an intentionally reintroduced regression (moving
`Serial.begin()` + `delay(200)` back above the latch makes five of its checks
fail).

### Persistent store test

`vm_fs.h`, `recording_paths.h`, `recording_meta.{h,cpp}`, `record_crc32.{h,cpp}`
and `recording_store.{h,cpp}` are Arduino free, so the exact on-card rules run
against an in-memory volume (`tests/memory_file_system.h`) that also injects
faults (unmountable card, failed writes, failed renames, full volume):

```bash
cd voice_memo_esp32
c++ -std=c++11 -Wall -Wextra -I. -o /tmp/recording_store_test \
    tests/recording_store_test.cpp recording_store.cpp recording_meta.cpp \
    record_crc32.cpp wav_format.cpp identifier_utils.cpp
/tmp/recording_store_test
```

262 assertions, grouped by rule:

1. path construction, including rejecting `../` and `/` in a recording id;
2. CRC-32 against the standard `"123456789" -> 0xCBF43926` vector, plus chained
   (streaming) equality with the one-shot computation;
3. WAV structural validation: valid, truncated, short, wrong sample rate, stereo;
4. metadata round trip, and rejection of truncated JSON, nested values, wrong
   value types, negative numbers, invalid ids, a future `schema_version` and
   trailing garbage;
5. commit + sidecar contents (tag, duration, size, CRC, sequence) and streamed
   read-back;
6. `.tmp` files never become `pending/` (commit atomicity);
7. an interrupted commit (sidecar committed, WAV still in `tmp/`) is completed by
   recovery;
8. a reboot during `uploading/` returns the item to `pending/` with the same
   `recording_id`;
9. recovery of a WAV with no sidecar (kept, sidecar rebuilt) and of corrupt audio
   or an orphan sidecar (quarantined to `corrupt/`);
10. card absent (mount refused, saves rejected) and card full (both the
    free-space reserve and the queue limit, with existing items untouched);
11. the upload lifecycle: pending -> uploading -> pending on failure with the
    same id and an incremented attempt count, then removal on success;
12. a confirmed upload deletes only its own recording;
13. oldest-first ordering survives a reboot (persistent `sequence`) and a retry;
14. the optional `sent/` retention policy;
15. card removed mid-attempt: no crash, no forgotten item, re-delivery after the
    card returns - including an item that recovery could not move out of
    `uploading/`, which must remain queued and readable from there;
16. queue count consistency, including items in `uploading/` and quarantined
    items;
17. the volume lock is balanced on every path (and the backend's mutex is
    recursive by design, because a commit nests per-operation calls);
18. an interrupted commit whose promotion rename fails keeps the tmp WAV and its
    sidecar (never deletes the only copy) and completes on a later boot;
19. a half-moved pair (WAV in `uploading/`, sidecar in `pending/`) is read from
    both directories instead of being quarantined as metadata-less;
20. the per-item attempt cap skips a rejected recording instead of starving the
    queue, and drops nothing;
21. a provably missing file is dropped from the queue, while an unhealthy volume
    never assumes absence;
22. a pre-existing on-card file is treated as a duplicate and never overwritten;
23. a WAV recovered without a sidecar gets a truthful CRC, so
    `VM_SD_VERIFY_CRC_ON_BOOT 1` does not quarantine it on the next boot.

### URL/path test

`ingest_target.h` parses `INGEST_URL` and normalizes the request path.

```bash
cd voice_memo_esp32
c++ -std=c++11 -Wall -Wextra -o /tmp/ingest_target_test tests/ingest_target_test.cpp
/tmp/ingest_target_test
```

The test asserts the contract, including:

```text
base URL http://192.168.1.244:8090
  -> host = 192.168.1.244
  -> port = 8090
  -> request path = /api/v1/audio
```

It also checks that a trailing slash and any explicit path normalize to the same
endpoint, and that invalid base URLs (missing scheme, empty host, zero port) are
rejected.

### Recording/upload state machine test

`recording_state.h` holds the transitions `RecordingApp` uses for the single
PSRAM WAV buffer, including `State::Uploading` and `State::RetryWait`.

```bash
cd voice_memo_esp32
c++ -std=c++11 -Wall -Wextra -o /tmp/recording_state_test tests/recording_state_test.cpp
/tmp/recording_state_test
```

It covers: `Idle -> press -> Recording`, `Recording -> release -> Uploading`,
`Uploading + press -> still Uploading` (new recording refused),
`Uploading -> Accepted/AlreadyKnown -> Idle`, `Uploading -> Failed ->
RetryWait`, `RetryWait -> retry -> Uploading`, that `Saving` freezes the buffer
and commits to `Idle`, and that `Saving`/`Uploading`/`RetryWait` never allow the
buffer to be overwritten.

### Wi-Fi fail-over test

`wifi_network_selector.h` holds the explicit connect state machine
(`Idle` / `TryingPrimary` / `TryingFallback` / `DisconnectingForSwitch` /
`RetryWait`), the attempt window, the disconnect-before-switch phase and the
offline retry cadence that `WifiManager` runs on the board.

```bash
cd voice_memo_esp32
c++ -std=c++11 -Wall -Wextra -o /tmp/wifi_fallback_test tests/wifi_fallback_test.cpp
/tmp/wifi_fallback_test
```

It covers (177 assertions), one section per regression:

1. no `WiFi.begin()` while the selector is in `DisconnectingForSwitch` (no
   command at all before the completion signal or the safety deadline);
2. exactly one begin per transition into `TryingPrimary` / `TryingFallback`
   (asserted as an invariant on every step of every case, not only by counting);
3. primary timeout -> `DisconnectingForSwitch` -> only after the driver is idle
   -> `TryingFallback`, plus the no-event variant that completes at
   `VM_WIFI_DISCONNECT_TIMEOUT_MS`;
4. fallback timeout -> `DisconnectingForSwitch` -> `RetryWait`;
5. the adapter calls `WiFi.setAutoReconnect(false)`, verifies it with
   `getAutoReconnect()`, has a single `WiFi.begin()` call site, disconnects with
   `WiFi.disconnect(false, false, 0)`, never erases NVS, never turns the radio
   off and never uses `delay()` (checked against `wifi_manager.cpp`);
6. primary available -> connects and never starts the fallback;
7. primary absent, fallback available -> connects over the fallback;
8. a 5-minute offline soak: controlled cycles with zero invariant violations,
   so `sta is connecting, cannot set config` has no path to happen;
9. a network that appears during `RetryWait` is joined on the next cycle;
10. while connected, no begin and no disconnect is issued for 120 s - only a real
    link drop restarts the list, once.

Plus: no command inside an attempt window (including at 1 s / 5 s / 9.9 s, the
guard for the revision that cut an attempt short and reconfigured the radio); a
third network reached through `TryingFallback`; an empty list doing nothing; a
single network going straight to `RetryWait` through a disconnect; `millis()`
wrap-around; and the state names used in the logs.

### UI logic and layout test

`ui_model.h`, `ui_layout.h`, `tag_selection.h`, `battery_level.h`, `gfx_canvas.*`
and `ui_screens.*` are Arduino free, so the screen mapping, the hitboxes and the
whole painter can be verified without a board:

```bash
cd voice_memo_esp32
c++ -std=c++11 -Wall -Wextra -I. -o /tmp/ui_model_test \
    tests/ui_model_test.cpp gfx_canvas.cpp ui_screens.cpp
/tmp/ui_model_test          # assertions only
/tmp/ui_model_test --dump   # + ASCII art of every screen (200x200, 1:1 pixels)
```

It covers: every state -> screen mapping including `Idle + no Wi-Fi -> OFFLINE`
and `Saving -> SAVING`, the SENT overlay (and that it never masks a real state,
including `Saving`), the BUSY hint, tag selection refused outside `Idle`, the
storage status line (`N pending`, `up N pending`, `SD FULL`, `SD ERROR`,
`NO SD`) being painted only when there is something to say, `selected` vs frozen
`recording` tag, the four hitboxes (centres, exact edges, gutters, margins - 109
assertions), the full/partial refresh policy for every state (including that
`Saving` can never block), the transient timeout arithmetic across the
`millis()` wrap, the battery curve, layout sanity (cells inside the panel, no
overlap, nothing clipped, every new screen unclipped) and the inverted selected
cell.

`--dump` prints each screen as ASCII art, which is how the 200x200 layout was
reviewed without the physical panel.

### Frank sprite test

The sprite copy in `frank_face.h` is pure, and `frank_sprites.h` is generated
data, so the two things that could silently ruin the animation - a wrong bit
convention and a wrong asset geometry - are checked on the Mac:

```bash
cd voice_memo_esp32
c++ -std=c++11 -Wall -Wextra -I. -Itests -o /tmp/frank_face_test \
    tests/frank_face_test.cpp gfx_canvas.cpp
/tmp/frank_face_test          # assertions only
/tmp/frank_face_test --dump   # + ASCII art of the boot frames' faces
```

78 assertions cover: every array being exactly `W*H/8` bytes (5000 for a full
frame, 624 for a face overlay); the bit order (bit 7 = leftmost, checked with
`0x80`, `0x01` and `0x1E`) and the polarity (a set bit is ink, and an all-zero
bitmap must *paint* background rather than leave it alone); every one of the
40000 pixels of all eight frames agreeing with an independently written decoder,
with no ink pixel lost or invented (a corner-pixel check also catches a negative
image); the 96x52 overlay being pixel-identical to the full sprite's face
rectangle for all eight states - which is what makes the partial-refresh frames
valid; every animated state differing from `AWAKE` only *inside* that rectangle
while `FRANK_RECORDING` deliberately does not; and the five animated faces being
pairwise distinct, so no frame is a stall.

`-Itests` is only needed because the generated pack does `#include <Arduino.h>`;
the test defines `PROGMEM` itself (it is empty on the ESP32 - `const` data is
already in flash) and never links the panel driver, so no ESP32 core is involved.

### Timezone / clock test

`time_zone.{h,cpp}` is Arduino free, so the exact UTC -> Europe/Lisbon conversion
the board performs can be checked on the Mac, DST transitions included:

```bash
cd voice_memo_esp32
c++ -std=c++11 -Wall -Wextra -o /tmp/time_zone_test tests/time_zone_test.cpp time_zone.cpp
/tmp/time_zone_test
```

It covers (68 assertions): the POSIX TZ install, `2026-01-15 12:00 UTC -> 12:00`
(WET, UTC+0), `2026-09-15 12:00 UTC -> 13:00` (WEST, UTC+1), the exact second of
both transitions for 2026/2027/2028 (last Sunday of March 01:00 UTC, last Sunday
of October 01:00 UTC), the day-rollover near midnight, the
"RTC first, NTP system clock second, nothing otherwise" policy, an invalid /
never-set RTC mapping to `--:--`, and the pure Gregorian arithmetic (leap day,
round trips, calendar validation).

The DST rule itself was verified against newlib-esp32 rather than assumed:
`_tzset_r()` parses the `M3.5.0/1` `M10.5.0/2` rules and `__tzcalc_limits()`
converts each rule's local time to GMT by adding that rule's own offset, which is
exactly POSIX semantics (start in standard time, end in DST) and matches the
tzdata `Europe/Lisbon` line `WET0WEST,M3.5.0/1,M10.5.0`.

### Mono sample budget / WAV duration test

`firmware_calc.h` and `wav_format.cpp` are Arduino free, so the two quantities
that were wrong while the RX returned two slots per frame are pinned on the Mac:

```bash
cd voice_memo_esp32
c++ -std=c++11 -Wall -Wextra -o /tmp/firmware_calc_test \
    tests/firmware_calc_test.cpp wav_format.cpp
/tmp/firmware_calc_test
```

It asserts the capture format (16000 Hz, 1 channel, 16-bit, 32 000 B/s), that
`duration_ms_for_samples()` is elapsed time for real mono samples
(`160000 -> 10000 ms`), that `max_samples()` is 720 000 and the payload budget is
1 440 000 bytes so the 45 s cap is 45 s of real audio, that a 10 s and a 45 s
payload both make the WAV header advertise exactly that duration, and that
exactly one RX slot (`VM_I2S_RX_SLOT_LEFT`) is selected.

### PWR key / boot arming test

`power_button.h` is Arduino free, so the debounce and the mandatory post-boot
arming run on the Mac, driven millisecond by millisecond. It covers cases 1-8 and
11-14 and 27-28 of the specification:

```bash
cd voice_memo_esp32
c++ -std=c++11 -Wall -Wextra -I. -Itests -o /tmp/power_button_test tests/power_button_test.cpp
/tmp/power_button_test
```

41 checks: boot with PWR released; boot with PWR held; a power-on press held
through `setup()` and then released (it arms, it never requests); exactly one
request per press/release cycle; contact bounce on both edges not duplicating a
request; a press shorter than the window being ignored; a press held for ten
seconds emitting exactly one event and no release; two presses producing two
requests; a long press that started while disarmed being ignored entirely; noise
not arming the button; the `millis()` wrap across a debounce window; polarity
parameterisation (an active-high build behaves consistently if the polarity were
ever disproved on hardware); and the zero-debounce clamp.

### Power policy / PowerManager test

`power_policy.h` is fully Arduino free and `power_manager.h` needs only `Serial`,
so the whole activity / inhibition / shutdown decision is driven here on a fake
clock. It covers cases 9-28 plus the logging check:

```bash
cd voice_memo_esp32
c++ -std=c++11 -Wall -Wextra -I. -Itests -o /tmp/power_manager_test tests/power_manager_test.cpp
/tmp/power_manager_test
```

74 checks: 119999 ms is not 120000 ms and the timeout fires exactly once; a touch,
a BOOT press or a tag selection restarts the interval while Wi-Fi, NTP, an RTC
read, an upload, a retry, a remount, an e-paper refresh, a pending-count change
and a log line never do; `Recording` and `MaxReachedWaitingRelease` suspend
auto-off; `Saving` defers and then completes; a persisted queue allows the
shutdown; a PSRAM-only note blocks it indefinitely with one `UNSENT NOTE`; the
upload completing releases the shutdown without another two minutes; a PWR press
during a capture is refused and not remembered; the `millis()` wrap-around; and
zero log lines over 10 s of idle ticking.

Neither power suite uses the ESP32 core. `power_manager.h` needs nothing from the
board but `Serial`, so `tests/power_manager_test.cpp` includes
`tests/arduino_shim.h` directly - a small (~60-line) `Serial` shim (`println` /
`print` / `printf`, plus the line counter the logging check reads). The runner
also passes `-Itests`, which puts `tests/Arduino.h` (a one-line facade over that
shim) on the include path, so any `#include <Arduino.h>` in the power headers
resolves to the shim rather than the ESP32 core. `power_button.h` and
`power_policy.h` are fully Arduino-free. Nothing under `tests/` is compiled into
the firmware - Arduino only builds the sketch root and `src/`.

### WAV slot diagnostic

`tools/wav_slot_diagnostic.py` checks an existing recording for the I2S slot
mistake (per-slot zero ratio / RMS / peak, correlation and MAE between
`x[0::2]` and `x[1::2]`, adjacent-frame equality). See "Audio capture: the I2S
RX slot is pinned to LEFT" above.

```bash
python3 tools/wav_slot_diagnostic.py recording.wav
```

### UI physical test script

With `VM_ENABLE_UPLOAD 1` and the ingress reachable, open Serial at 115200 and
follow the table. Each step lists what must appear on the panel and in the log.

| # | Action | Panel | Serial |
| --- | --- | --- | --- |
| 1 | power on | Frank wakes (`X X` persisted, then the driver's init clears the panel, then `— —` -> `• •`), then `READY`, tag grid, footer | `[epd] panel ready`, `[frank] boot animation`, two `[frank] frame ...` lines, `[frank] boot animation done in ... ms`, `[ui] ready:`, the four `[ui] hitbox` lines |
| 2 | tap Work | Work inverted | `[ui] touch x=.. y=..` then `[ui] tag selected=Work` |
| 3 | tap Idea | Idea inverted, Work normal | `[ui] tag selected=Idea` |
| 4 | hold BOOT | `● REC`, `00:00` | `[app] BOOT press`, `[app] recording started ... tag=Idea` |
| 5 | keep holding ~5 s | timer advances once per second, nothing else flashes | `[ui] partial refresh screen=recording <n> ms` |
| 6 | release BOOT | `UPLOADING`, `Idea · 5s` | `[app] BOOT release`, `[app] async upload started` |
| 7 | tap any tag during the upload | nothing changes | `[app] BOOT press ignored` only if BOOT is pressed; `[ui] tag selection ignored in state=uploading` |
| 8 | ingress answers 201 | `SENT` for ~1.5 s, then full refresh to `READY` | `[app] async upload accepted ... http=201`, `[ui] showing SENT`, `[ui] full refresh screen=ready` |
| 9 | power the ingress/Wi-Fi off, then record | after release `WAITING FOR WIFI` / `RETRYING`; in `Idle` the panel shows `OFFLINE` | `[app] retry pending ... reason=wifi` |
| 10 | hold BOOT for 45 s | `MAX 45s` / `Release BOOT`, then `UPLOADING` on release | `[app] MAX_RECORDING_DURATION_REACHED` |
| 11 | press BOOT while `UPLOADING` | small `BUSY` box for ~1 s | `[app] BOOT press ignored: upload busy` |
| 12 | read the clock at boot | top bar shows the current Lisbon time (`HH:MM`), or `--:--` if the RTC was never set | `[time] timezone:`, `[time] RTC UTC:`, `[time] Lisbon local:` |
| 13 | boot with Wi-Fi up | time corrects itself within seconds | `[time] NTP synchronized`, `[time] RTC updated from NTP (UTC ...)`, then the refreshed `[time] Lisbon local:` |
| 14 | boot with Wi-Fi off | top bar still shows the local time from the RTC alone | no `[time] NTP` line; `[time] Lisbon local: ... (WET/WEST, UTC+0/+1)` |

Also confirm: the microphone still records (non-zero `audio_peak`), BOOT stays
responsive, the upload stays asynchronous (`tick()` keeps logging during the
POST), no watchdog reset appears, and a tap does not disturb the ES8311. The
`[time]` lines appear once at boot and once per NTP correction - a continuous
stream of them would mean the clock is being polled or printed per `loop()`.

## External Arduino libraries

None required beyond the Arduino ESP32 core built-ins:

```text
WiFi
Wire
Preferences
SD_MMC            (v0.5.0: microSD persistence, 1-bit SDMMC)
FS
driver/i2s_std.h
driver/spi_master.h
driver/gpio.h
esp_adc/adc_oneshot.h
esp_adc/adc_cali.h
freertos/FreeRTOS.h, freertos/task.h, freertos/queue.h, freertos/semphr.h
```

## ES8311 attribution

The ES8311 register sequence is adapted from the Espressif ESP-ADF ES8311
driver. Attribution is preserved in `THIRD_PARTY_NOTICES.md`.
