#!/usr/bin/env python3
"""Host-side I2S slot diagnostic for VoiceMemo WAV recordings.

The ESP32-S3 I2S RX hardware always frames two slots per LRCK period. If the
receive slot mask is wrong, the mono ES8311 ADC data lands in the DMA stream
together with the codec's idle second slot, so a WAV that claims 16 kHz holds
two samples per real 16 kHz frame. Three shapes are possible:

  duplicate : A A B B C C ...   the same sample twice per frame
  even-live : A 0 B 0 C 0 ...   audio in slot 1, slot 2 idle
  odd-live  : 0 A 0 B 0 C 0 ... audio in slot 2, slot 1 idle

For the first shape a perfect duplicate produces correlation(x[0::2], x[1::2])
of +1.0 and an MAE of 0. For the other two one of the two sub-sequences is
identically zero, which is an even stronger signal.

Usage:
    python3 tools/wav_slot_diagnostic.py recording.wav [--decimate out.wav]
                                                       [--skip-seconds N]
"""

import argparse
import array
import math
import struct
import sys
import wave


def load_mono16(path):
    with wave.open(path, "rb") as handle:
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        rate = handle.getframerate()
        frames = handle.getnframes()
        raw = handle.readframes(frames)
    if width != 2:
        raise SystemExit(f"{path}: only 16-bit PCM is supported (got {width * 8}-bit)")
    samples = array.array("h")
    samples.frombytes(raw)
    if sys.byteorder == "big":
        samples.byteswap()
    return {
        "channels": channels,
        "rate": rate,
        "frames": frames,
        "samples": samples,
        "raw": raw,
    }


def pearson(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return float("nan")
    a = a[:n]
    b = b[:n]
    mean_a = sum(a) / n
    mean_b = sum(b) / n
    var_a = sum((value - mean_a) ** 2 for value in a) / n
    var_b = sum((value - mean_b) ** 2 for value in b) / n
    if var_a == 0.0 and var_b == 0.0:
        return float("nan")
    if var_a == 0.0 or var_b == 0.0:
        return 0.0
    cov = sum((a[i] - mean_a) * (b[i] - mean_b) for i in range(n)) / n
    return cov / math.sqrt(var_a * var_b)


def rms(values):
    if not len(values):
        return 0.0
    return math.sqrt(sum(float(value) * value for value in values) / len(values))


def describe(name, values):
    zeros = sum(1 for value in values if value == 0)
    return (
        f"  {name:<10} n={len(values):<8} zeros={zeros / max(len(values), 1):8.4%} "
        f"rms={rms(values):9.2f} peak={max((abs(v) for v in values), default=0):6d}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("wav", help="WAV file recorded by the firmware")
    parser.add_argument("--skip-seconds", type=float, default=0.0, help="ignore this much audio at the start")
    parser.add_argument("--decimate", metavar="OUT", help="write x[::2] as a 16-bit mono WAV")
    args = parser.parse_args()

    info = load_mono16(args.wav)
    rate = info["rate"]
    samples = info["samples"]
    start = int(args.skip_seconds * rate)
    samples = samples[start:]

    print(f"file            : {args.wav}")
    print(f"channels        : {info['channels']}")
    print(f"sample rate     : {rate} Hz")
    print(f"bit depth       : 16-bit")
    print(f"duration        : {info['frames'] / rate:.3f} s ({info['frames']} frames)")
    print(f"analysed        : {len(samples)} samples")
    print()

    even = samples[0::2]
    odd = samples[1::2]
    print("per-slot statistics (slot 1 = even indices, slot 2 = odd indices)")
    print(describe("slot1", even))
    print(describe("slot2", odd))
    print()

    corr = pearson(even, odd)
    n = min(len(even), len(odd))
    mae = sum(abs(even[i] - odd[i]) for i in range(n)) / max(n, 1)
    scale = rms(even) + rms(odd)
    print("even/odd comparison")
    print(f"  correlation(x[0::2], x[1::2]) = {corr:.6f}")
    print(f"  MAE(x[0::2], x[1::2])         = {mae:.3f}")
    print(f"  MAE / rms                     = {mae / scale:.4f}" if scale else "  MAE / rms = n/a")
    print()

    pairs = len(samples) // 2
    equal = sum(1 for i in range(pairs) if samples[2 * i] == samples[2 * i + 1])
    print("adjacent-frame duplication check")
    print(f"  exact x[2i] == x[2i+1]        = {equal / max(pairs, 1):.4%}")
    print()

    zeros_even = sum(1 for value in even if value == 0) / max(len(even), 1)
    zeros_odd = sum(1 for value in odd if value == 0) / max(len(odd), 1)

    print("verdict")
    if zeros_odd > 0.999 and zeros_even < 0.999:
        print("  slot 2 is idle and slot 1 carries the audio.")
        print("  -> the capture is 2x too long; the RX slot mask must select slot 1 only.")
        print("  -> on ESP32-S3/ES8311 this is I2S_STD_SLOT_LEFT.")
    elif zeros_even > 0.999 and zeros_odd < 0.999:
        print("  slot 1 is idle and slot 2 carries the audio.")
        print("  -> the capture is 2x too long; the RX slot mask must select slot 2 only.")
        print("  -> on ESP32-S3/ES8311 this is I2S_STD_SLOT_RIGHT.")
    elif abs(corr) > 0.99 and mae / scale < 0.02:
        print("  the two slots are duplicates of each other (A A B B ...).")
        print("  -> the capture is 2x too long; select a single RX slot.")
    else:
        print("  both slots carry audio, so this is a genuine 2-channel or already mono stream.")
        print("  if the file sounds 2x slow, check the I2S clock/rate configuration instead.")

    if args.decimate:
        keep = samples[0::2]
        out = array.array("h", keep)
        if sys.byteorder == "big":
            out.byteswap()
        with wave.open(args.decimate, "wb") as handle:
            handle.setnchannels(1)
            handle.setsampwidth(2)
            handle.setframerate(rate)
            handle.writeframes(out.tobytes())
        print()
        print(f"wrote {args.decimate}: {len(keep)} samples at {rate} Hz")


if __name__ == "__main__":
    main()
