# Frank — Cardputer ADV v0.1

Hardware bring-up + visual identity test for the **M5Stack Cardputer ADV**.

This is a standalone application firmware, meant to be installed through
**M5Launcher** (or any other launcher) and to leave the bootloader, the
partition table and every other firmware untouched. It contains no partition,
OTA or flash-writing code of its own.

## What v0.1 does

- initializes the Cardputer ADV (LCD + physical keyboard) via `M5Cardputer`
- draws a blocky pixel-art Frank on the 240x135 landscape screen
- toggles `READY` ⇄ `KEYBOARD OK` on SPACE — exactly once per physical press
- logs bring-up and key diagnostics on Serial at 115200

**Not in v0.1:** audio, microSD, Wi-Fi, NUC upload, Notion, tags, power
management, recording, the Waveshare `RecordingApp`.

The Waveshare firmware under `../voice_memo_esp32/` is untouched and shares no
code with this sketch.

## Arduino settings

| Setting | Value |
| --- | --- |
| Board | **M5Cardputer** (`esp32:esp32`) |
| Flash size | **8 MB (64Mb)** |
| Partition scheme | **8M with spiffs (3MB APP/1.5MB SPIFFS)** |
| Everything else | defaults |

Libraries (Arduino IDE → Library Manager):

- **M5Cardputer** ≥ 1.2.0 — pulls in M5Unified ≥ 0.2.25 and M5GFX ≥ 0.2.32

The Cardputer vs. Cardputer ADV difference (LCD panel probe + TCA8418 keyboard
controller) is resolved at runtime by M5GFX/M5Unified, so this binary also runs
on the original Cardputer.

## Build

Arduino IDE: open `frank_cardputer_adv.ino` and use *Sketch → Export Compiled
Binary*; the image lands in `build/esp32.esp32.m5stack_cardputer/`.

arduino-cli:

```sh
arduino-cli compile \
  --fqbn "esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB" \
  --warnings all \
  frank_cardputer_adv
```

## Artifact

`Frank-Cardputer-ADV-v0.1.bin` — a plain ESP32-S3 **application image**
(flashed at `0x10000`, the app partition).

- ≈ 483 KB, well inside the 1.5 MB app partition M5Launcher uses on the Cardputer
- copy it to the microSD card and install it from M5Launcher
- **never** flash the `*.merged.bin` by hand: that one contains bootloader and
  partition table and would overwrite the launcher's environment

## Known risk to check on hardware

The keyboard is read through `M5Cardputer.Keyboard` (TCA8418 over I²C on the
ADV). Library version 1.2.0 only drains the controller when the INT pin (GPIO11)
raises an interrupt. If SPACE is not detected on real hardware, the follow-up is
a polling reader injected through the official
`Keyboard.begin(std::unique_ptr<KeyboardReader>)` hook — no driver rewrite and
no library fork.
