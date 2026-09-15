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
ESP32 -> ES8311 -> NUC audio path as the server project.

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
never calls `WiFi.begin()`, never contacts the ingress, and a successful
recording ends with:

```text
[app] TEST_A upload disabled; recording retained only for serial summary
```

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

Measured sizes with arduino-cli 1.5.1 and core 3.3.11:

| Build | Flash | Static RAM |
| --- | --- | --- |
| `VM_ENABLE_UPLOAD 1`, `VM_ENABLE_UI 1` | 1 003 311 B (30%) | 48 760 B (14%) |
| `VM_ENABLE_UPLOAD 0`, `VM_ENABLE_UI 1` (TEST A) | 525 172 B (15%) | 32 736 B (9%) |
| `VM_ENABLE_UPLOAD 1`, `VM_ENABLE_UI 0` (pre-UI) | 944 831 B (28%) | 48 108 B (14%) |

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
| `keepBatteryPowerOn()` | configures `GPIO17` as an output and drives it HIGH; idempotent and safe before `Serial.begin()` |
| `begin()` | re-asserts the latch and prints the two `[power]` lines, once |
| `powerOff()` | drives `GPIO17` LOW; intentionally never called (no deep sleep yet) |
| `latchOn()` | true once the latch has been asserted |

`GPIO18` is the PWR key itself. It is an input and is never driven by the
firmware; it is only named in the log.

### Boot order

`keepBatteryPowerOn()` is the **first statement of `setup()`**, ahead of every
driver:

```text
reset -> ROM bootloader -> second stage bootloader -> setup()
  |
  +- 1. BoardPower::keepBatteryPowerOn()   GPIO17 = HIGH   <- latch asserted here
  +- 2. Serial.begin(115200) + delay(200)
  +- 3. BoardPower::begin()                the one-shot [power] lines
  +- 4. Button, DeviceId, RecordingApp, audio (ES8311 + I2S)
  +- 5. UI: e-paper -> touch -> battery ADC -> RTC -> TimeManager
  +- 6. Wi-Fi (VM_ENABLE_UPLOAD)
```

Two details matter:

* The pin is written HIGH **before** it is switched from high-Z to output.
  `gpio_set_level()` updates the output register without requiring the pin to be
  an output yet, so the pad goes from high-Z straight to HIGH and never emits the
  LOW pulse that a config-then-set order would produce. The internal pull-up is
  enabled exactly as in the official constructor.
* Nothing else in the firmware writes `GPIO17`, so after step 1 the latch stays
  asserted for the whole run: no periodic re-write, and no `[power]` line in
  `loop()`.

The window from reset to step 1 (ROM + second stage bootloader) is not covered by
firmware, so a PWR press must last long enough to reach `setup()`. Deep sleep,
`esp_sleep_enable_*` and `gpio_hold_en()` are deliberately absent: the board
never enters deep sleep, so a hold would have nothing to protect.

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
[power] PWR key gpio=18
```

### Battery latch physical test

| # | Setup | Expected |
| --- | --- | --- |
| A | USB connected | normal boot, touch works, Wi-Fi connects, both `[power]` lines present |
| B | unplug USB while running | board keeps running; touch keeps working; Wi-Fi stays up; the MCU does not freeze |
| C | power off, then boot on battery only | press and **hold** PWR until `[power] battery latch gpio=17 level=HIGH` appears; releasing PWR afterwards leaves the board running |
| D | after C | touch responds, tags change, Wi-Fi connects, a BOOT press records audio |

Step C is the actual latch test: it fails if the board dies on release. A press
shorter than the bootloader + `setup()` time cannot latch the rail on any
firmware, so "one quick tap" is not a valid pass condition.

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
| `Idle` | buffer free | yes |
| `Recording` | capturing into PSRAM | already recording |
| `MaxReachedWaitingRelease` | 45 s hit, waiting for the release | no |
| `Uploading` | background POST owns the WAV | no |
| `RetryWait` | attempt failed, same WAV kept for retry | no |

There is exactly one WAV buffer, so while `Uploading` or `RetryWait` owns it, a
BOOT press is refused instead of overwriting the audio:

```text
[app] BOOT press ignored: upload busy id=... state=uploading
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
- On failure the WAV stays in PSRAM, `recording_id` is preserved and the retry
  reuses it. A new `recording_id` is generated only when a new recording starts.
- Retry cadence is `VM_UPLOAD_RETRY_INTERVAL_MS` (5000 ms) in `config.h`.

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
| `board_power.{h,cpp}` | battery power latch (`GPIO17`); asserted first in `setup()` |
| `voice_tags.h` | the four tags and their labels - the only place the strings exist |
| `tag_selection.h` | selected tag vs tag frozen at `startRecording()` |
| `ui_layout.h` | 200x200 geometry, tag rectangles, hit test |
| `ui_model.h` | state -> screen, refresh policy, transient deadlines (pure) |
| `ui_screens.{h,cpp}` | the painter: view model -> pixels (pure, host-tested) |
| `gfx_canvas.{h,cpp}` | 1bpp framebuffer primitives + 5x7 font renderer (pure) |
| `font5x7.h` | generated glyph table |
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

### Screens

| Screen | Shown when | Content |
| --- | --- | --- |
| READY | `Idle` + Wi-Fi up | time, Wi-Fi, battery, `READY`, 2x2 tag grid, `Hold BOOT to record` |
| READY OFFLINE | `Idle` + Wi-Fi down | `OFFLINE`; recording stays allowed, the note will wait in the buffer |
| RECORDING | `Recording` | `● REC`, `MM:SS`, the frozen tag, `Release to finish` |
| MAX 45s | `MaxReachedWaitingRelease` | `MAX 45s`, `Release BOOT` |
| UPLOADING | `Uploading` | `UPLOADING`, static arrow, `Tag · Ns`, `Please wait` |
| RETRY WAIT | `RetryWait` | `WAITING FOR WIFI`, `Note preserved`, `RETRYING` |
| SENT | upload accepted (`Idle` only) | check mark, `SENT`, `Voice saved` for 1.5 s, then READY |
| BUSY hint | BOOT pressed while the buffer is busy | small inverted `BUSY` box in the footer, 1 s |

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
[power] PWR key gpio=18
[epd] panel ready in 2134 ms (official Waveshare driver, SPI2 40000000 Hz)
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

### Residual risks (not verifiable without the board)

* **Touch orientation.** The vendor example reports raw x/y clamped to the panel
  size and applies no transform, which is what this firmware assumes. If a tap on
  Work selects a different tag, the fix is the single coordinate assignment in
  `Ft6336Touch::readTouch()` - the boot log prints the tap coordinates so the
  mapping can be identified immediately.
* **Panel timing.** Partial update time, ghosting rate and the 20-partial bound
  are engineering estimates; the serial log reports the measured refresh time so
  the number can be tuned.
* **Battery curve.** The voltage -> percentage curve is an approximation, not a
  datasheet characteristic.
* **Power latch.** `GPIO17` HIGH is the official latch semantics, but the fix can
  only be confirmed on battery: the bootloader window before `setup()` is not
  covered by firmware, so a PWR press shorter than the boot time cannot latch the
  rail. See "Battery latch physical test" above.

## Host-side tests

Six pieces of firmware logic have no Arduino/Wi-Fi dependency and are tested on
the Mac without a board. The `tests/` directory is not part of the sketch build
(Arduino only compiles the sketch root and `src/`).

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
RetryWait`, `RetryWait -> retry -> Uploading`, and that
`Uploading`/`RetryWait` never allow the buffer to be overwritten.

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

It covers: every state -> screen mapping including `Idle + no Wi-Fi -> OFFLINE`,
the SENT overlay (and that it never masks a real state), the BUSY hint, tag
selection refused outside `Idle`, `selected` vs frozen `recording` tag, the four
hitboxes (centres, exact edges, gutters, margins - 109 assertions), the full/partial
refresh policy for every state, the transient timeout arithmetic across the
`millis()` wrap, the battery curve, layout sanity (cells inside the panel, no
overlap, nothing clipped) and the inverted selected cell.

`--dump` prints each screen as ASCII art, which is how the 200x200 layout was
reviewed without the physical panel.

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
| 1 | power on | `READY`, tag grid, footer | `[epd] panel ready`, `[ui] ready:`, the four `[ui] hitbox` lines |
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
driver/i2s_std.h
driver/spi_master.h
driver/gpio.h
esp_adc/adc_oneshot.h
esp_adc/adc_cali.h
```

## ES8311 attribution

The ES8311 register sequence is adapted from the Espressif ESP-ADF ES8311
driver. Attribution is preserved in `THIRD_PARTY_NOTICES.md`.
