# Frank — Cardputer ADV

Standalone M5Stack Cardputer ADV firmware, installed through **M5Launcher**.
No partition, OTA or flash-writing code: bootloader, partition table, M5Launcher
and every other firmware stay untouched. `../voice_memo_esp32/` is untouched and
shares no code with this sketch.

## v0.2.1 — AUDIO DIAGNOSTIC (current)

v0.2 played back silence on hardware while display and keyboard worked. v0.2
could not tell *which* link was broken, because it played back whatever it had
captured — and a silent capture produces a silent playback. This build tests the
three links separately, keeping the validated v0.1/v0.2 display init, keyboard
handling and the Frank pixel-art identity.

```
FRANK
AUDIO TEST

1 : SPEAKER
2 : MIC LEVEL
3 : RECORD/PLAY
ESC : BACK

IDF 5.5.5  core 3.3.11      <- the running build identifies itself
SPK heard : ?
MIC peak  : 0
```

### Test 1 — SPEAKER ONLY

No microphone data involved. `Mic.end()`, `Speaker.begin()`, then two generated
tones (`Speaker.tone(440 Hz, 400 ms)`, `Speaker.tone(880 Hz, 400 ms)`) via the
official API. Afterwards the screen asks `HEARD TONES ? Y / N` and Serial records
the binary answer:

```
[audio-test] speaker begin=1
[audio-test] tone 1 (440 Hz, 400 ms) queued=1
[audio-test] tone 2 (880 Hz, 400 ms) queued=1
[audio-test] speaker test complete
[audio-test] speaker heard=YES
```

This is the only test that can prove the amplifier/DAC path works on its own.

### Test 2 — MIC LEVEL ONLY

No playback. `Speaker.end()`, `Mic.begin()`, **1200 ms warm-up**, then blocks of
320 samples (20 ms) are queued with `Mic.record(...)` and analysed as they
complete, giving a window every 100 ms:

- a blocky log-scale level meter (must react to speech, whistling, clapping)
- peak / RMS / min / max / zero-percentage on screen
- `CONSTANT SAMPLE <v>` when every sample in the window is identical — the
  signature of the known ES8311/I2S failure (e.g. `-8 -8 -8 -8` or `-1 -1 -1`)

```
[audio-test] min=-12
[audio-test] max=345
[audio-test] peak=345
[audio-test] rms=88.4
[audio-test] zero_pct=1.20
[audio-test] raw=3 -5 12 8 -2 0 6 11
[audio-test] constant_sample=-8
```

### Test 3 — RECORD / PLAYBACK

5 s target, 16000 Hz, PCM16 mono, captured through the v0.2 pipeline (two
requests in flight, re-armed from the buffer-release callback so the DMA is
never starved). Sequence — the warm-up is **never** part of the recording:

```
Mic.begin() -> 1200 ms warm-up -> READY TO RECORD -> SPACE -> capture
```

Stopping (SPACE or the 5 s limit) sets the pipeline to drain instead of
discarding in-flight requests, then `Mic.end()`, then `Speaker.begin()` and
`Speaker.playRaw(buffer, captured_samples, 16000)`. `ENTER` replays, `SPACE`
records again (with warm-up), `ESC` returns to the menu.

```
[audio-test] recording stopped reason=space
[audio-test] samples=42560
[audio-test] duration_ms=2660
[audio-test] wall_ms=2705
[audio-test] rate_measured=15733 (requested 16000)
[audio-test] peak=12482 rms=1834.2 zero_pct=0.01
[audio-test] min=-12900 max=12482
```

`rate_measured = samples / wall time` is the key number: if the I2S clock tree is
wrong, the real rate will not be ~16000 and `wall_ms` will not match
`duration_ms`.

### Why an IDF A/B pair

M5Unified 0.2.25 drives the ESP32-S3 I2S clock through a **raw divider override**
(`M5UNIFIED_I2S_DRIVER_MANAGED_CLK = 0` on the S3): it selects the PLL_D2 /
"PLL_240M" source with a raw register write (`rx_clk_sel = 1`, `tx_clk_sel = 1`)
and computes the divider from a **120 MHz** base. ESP-IDF changed exactly that
area for the ESP32-S3:

| | IDF 5.4.2 (core 3.2.1) | IDF 5.5.5 (core 3.3.11) |
| --- | --- | --- |
| `SOC_I2S_CLKS` (S3) | `{PLL_F160M, XTAL, EXTERNAL}` | `{PLL_F240M, PLL_F160M, XTAL, EXTERNAL}` |
| `I2S_CLK_SRC_PLL_240M` | not defined | defined (maps to PLL_D2, 240 MHz) |
| `i2s_ll_rx_set_clk_src()` → `rx_clk_sel` | `XTAL=0, PLL_160M=2, APLL=3` | `XTAL=0, **PLL_240M=1**, PLL_160M=2, APLL=3` |

In IDF 5.4.2 nothing in IDF assigned the register value `1`; in IDF 5.5.5 the
value `1` that M5Unified hardcodes was formally assigned to the **240 MHz**
PLL_D2 source, while M5Unified still divides it as 120 MHz. That is a concrete,
version-specific difference in the path the ES8311 clock depends on.

Two binaries are provided so this can be settled on hardware with everything
else held constant:

| Build | Toolchain | Reported IDF | Size |
| --- | --- | --- | --- |
| `Frank-Cardputer-ADV-v0.2.1-idf55.bin` | Arduino core 3.3.11 | v5.5.5 | 525,456 B |
| `Frank-Cardputer-ADV-v0.2.1-idf54.bin` | Arduino core 3.2.1 | v5.4.2-25-g858a988d6e | 565,328 B |

Both were compiled from the **same source revision** (sha256
`6281450105ffff6c7763a4a47d72f8356a1bf5671a88db4e09866c9d9fb15124`), 0 errors /
0 warnings each, with M5Cardputer 1.2.0 / M5Unified 0.2.25 / M5GFX 0.2.32.
The running build prints its own IDF and core version on the menu screen and on
Serial, so the flashed `.bin` is never in doubt.

### Recording buffer (and why v0.2 lasted ~2 s)

The diagnostic asks for 5 s (160,640 bytes including in-flight slack) and steps
down `5 → 4 → 3 → 2 s` only if the heap cannot afford it, with a 32 KB floor.
Every decision is logged, so a shortened buffer can never be a guess again:

```
[audio-test] free_heap_before=...
[audio-test] psram_free=...
[audio-test] buffer_duration_selected=5
[audio-test] buffer_bytes=160640
[audio-test] free_heap_after=...
```

v0.2 used a 3 → 2 → 1 s ladder with a 48 KB floor and only logged a warning when
it stepped down: with a free heap below ~146 KB it silently recorded 2 s
(`buffer=64000`), which is the most likely explanation for the ~2 s capture.
The second candidate is an I2S clock that is too fast, which would make a 3 s
buffer fill in ~2 s of wall time while the on-screen timer (derived from the
sample count) still reads 3.0 s. Test 3's `wall_ms` and `rate_measured`
distinguish the two cases directly.

## Controls

| Screen | Keys |
| --- | --- |
| Menu | `1` speaker · `2` mic level · `3` record/play · `ESC` back |
| Speaker test | `Y` / `N` to record whether the tones were heard |
| Mic level | `ESC` back |
| Record/play | `SPACE` start/stop/re-record · `ENTER` play · `ESC` menu |

## Arduino settings

| Setting | Value |
| --- | --- |
| Board | **M5Cardputer** (`esp32:esp32`) |
| Flash size | **8 MB (64Mb)** |
| Partition scheme | **8M with spiffs (3MB APP/1.5MB SPIFFS)** |
| Libraries | M5Cardputer 1.2.0 → M5Unified 0.2.25 → M5GFX 0.2.32 |

```sh
arduino-cli compile \
  --fqbn "esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB" \
  --warnings all frank_cardputer_adv
```

Install the other core with `arduino-cli core install esp32:esp32@3.2.1` (only
one esp32 core version can be installed at a time, so the two builds are made
one after the other).

## Artifacts

| File | Size | Note |
| --- | --- | --- |
| `Frank-Cardputer-ADV-v0.2.1-idf55.bin` | 525,456 B | diagnostic, IDF 5.5.5 |
| `Frank-Cardputer-ADV-v0.2.1-idf54.bin` | 565,328 B | diagnostic, IDF 5.4.2 |
| `Frank-Cardputer-ADV-v0.2.bin` | 518,512 B | previous milestone, preserved |
| `Frank-Cardputer-ADV-v0.1.bin` | 483,456 B | validated bring-up, preserved |

All are plain application images (app partition at `0x10000`), well inside the
1.5 MB app slot M5Launcher uses. **Never** flash the `*.merged.bin` from the same
build output — it carries a bootloader and partition table.

## Notes for the audio tests

- The ES8311 needs ~1 s to stabilise after `Mic.begin()`; the firmware waits
  1200 ms before measuring and before recording, and that wait is not recorded.
- Microphone and speaker cannot be active at once: the firmware always ends one
  before starting the other.
- Gain is the M5Unified default (`magnification = 16`, ES8311 ADC volume 0 dB),
  playback master volume 180/255. Neither is changed in this build.
- No arbitrary "good/bad audio" threshold: only obviously dead input (all zero /
  constant) is called out, and it is reported as a value, not a verdict.
