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

## Important: this Touch model has one hardware version

The V1/V2 discussion applies to the non-touch `ESP32-S3-ePaper-1.54` family.
Do not select V1/V2 settings for this Touch board unless an official source
proves otherwise.

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
INGEST_URL
INGEST_TOKEN
DEVICE_ID
```

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
8 MB partition scheme so the app image has room (TEST B is ~935 KB, which is
78% of the 1.2 MB app partition in the default 4 MB layout):

```bash
ARDUINO_CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"

"$ARDUINO_CLI" compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=8M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=default_8MB" \
  --build-path /tmp/vm_build_path \
  --output-dir /tmp/vm_build_out \
  .
```

Use `--build-path` inside a writable directory so the Arduino cache is not
touched. The same sketch also compiles with `PartitionScheme=default` (4 MB
layout); it fits, but with much less headroom.

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

Then set these six values. Nothing else needs editing.

| Setting | Where | Value for TEST B |
| --- | --- | --- |
| `WIFI_SSID` | `secrets.h` | Your 2.4 GHz Wi-Fi SSID |
| `WIFI_PASSWORD` | `secrets.h` | Your Wi-Fi password |
| `INGEST_URL` | `secrets.h` | `"http://<NUC-LAN-IP>:8090"` |
| `INGEST_TOKEN` | `secrets.h` | Same as ingress `--token`, or `""` if auth is off |
| `DEVICE_ID` | `secrets.h` | `"bel-esp32-01"` for the first integration test |
| `VM_ENABLE_UPLOAD` | `config.h` | `1` |

Concrete `secrets.h` for TEST B (example address; use your real NUC LAN IP):

```cpp
#define WIFI_SSID "my-wifi"
#define WIFI_PASSWORD "my-password"
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
[wifi] connecting
[wifi] connected
[wifi] ESP32 local IP: 192.168.1.77
[app] BOOT press
[app] recording started id=...
[app] BOOT release
[app] recording stopped ...
[app] audio_peak=... audio_rms=...
[app] WAV finalized ...
[app] upload attempt id=... attempt=1
[upload] recording_id=... bytes=... duration_ms=...
[upload] target http://192.168.1.50:8090/api/v1/audio
[upload] started
[upload] HTTP 201
[app] upload accepted id=... http=201
```

A replay of the same `recording_id` instead reports:

```text
[upload] HTTP 200
[app] upload already known id=... http=200
```

A failed upload keeps the WAV in PSRAM, keeps the same `recording_id`, and
retries without generating a new one:

```text
[upload] connect failed: 192.168.1.50:8090
[upload] check that the ingress binds the NUC LAN address and that Wi-Fi is on the same network
[app] upload failed id=... next_attempt=2; keeping recording for retry
[app] retry pending id=... seconds_until_retry=5
[app] retrying previous upload id=...
[app] upload attempt id=... attempt=2
```

If the ingress answers with an error status:

```text
[upload] HTTP 500
[upload] response=<short body>
[app] upload failed id=... next_attempt=2; keeping recording for retry
```

If no HTTP response arrives at all:

```text
[upload] no HTTP response from ingress (connection dropped or timed out)
[app] upload failed id=... next_attempt=2; keeping recording for retry
```

If Wi-Fi never connects, the recording is retained and the firmware keeps
trying:

```text
[wifi] connecting
[wifi] not connected yet; retrying
[app] upload skipped id=... attempt=1 reason=wifi_not_connected; keeping recording for retry
[app] retry pending id=... reason=wifi
```

Serial output never prints the Wi-Fi password or `INGEST_TOKEN`. The only
network details shown are the target host, port and path.

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
| `[wifi] not connected yet; retrying` | Wrong SSID/password, or 5 GHz-only SSID (ESP32-S3 is 2.4 GHz) |


## Host-side URL/path test

`ingest_target.h` parses `INGEST_URL` and normalizes the request path. It has no
Arduino or Wi-Fi dependency, so it can be tested on the Mac without a board. The
`tests/` directory is not part of the sketch build (Arduino only compiles the
sketch root and `src/`).

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

## External Arduino libraries

None required beyond the Arduino ESP32 core built-ins:

```text
WiFi
Wire
Preferences
driver/i2s_std.h
```

## ES8311 attribution

The ES8311 register sequence is adapted from the Espressif ESP-ADF ES8311
driver. Attribution is preserved in `THIRD_PARTY_NOTICES.md`.
