#!/usr/bin/env python3
"""Measure the beep/gap structure of a recorded audio clip.

Purpose
-------
PicoSwitch2's DualSense audio bridge is debugged by ear (no bench
instrumentation, no UART on the test rig). "It beeps" is not a measurement.
Record the controller speaker with any microphone, hand the file to this tool,
and it turns the acoustic envelope into hard numbers: how long each beep lasts,
how long the gaps are, how steady the period is, the duty cycle, and the tone's
fundamental frequency.

That distinguishes the failure modes we actually care about:
  * a *consistent* ~50% duty cycle  -> the link delivers ~half the reports it
    should (sustained rate deficit, e.g. radio contention), not random loss;
  * a stable gap at a specific period -> a periodic scheduling anchor;
  * high variance in gap length       -> jitter / starvation bursts;
  * continuous tone (duty ~100%)      -> fixed! transport is keeping up.

Input
-----
Any audio format (wav/m4a/mp3/...). ffmpeg is used to decode to mono 16-bit PCM
first, so the recorder's native format does not matter. Pure Python stdlib
otherwise (no numpy/scipy on this rig).

Usage
-----
    python tools/analyze_audio_capture.py <recording> [--rate 48000]
        [--window-ms 1.0] [--min-segment-ms 4.0]
"""

import argparse
import math
import os
import statistics
import subprocess
import sys
import tempfile
import wave


def decode_to_wav(src, rate):
    """Decode any input to a temp mono s16 WAV at `rate` Hz via ffmpeg."""
    fd, tmp = tempfile.mkstemp(suffix=".wav")
    os.close(fd)
    cmd = ["ffmpeg", "-y", "-i", src, "-ac", "1", "-ar", str(rate),
           "-f", "wav", "-acodec", "pcm_s16le", tmp]
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode(errors="replace"))
        raise SystemExit(f"ffmpeg failed to decode {src!r}")
    return tmp


def read_samples(path):
    """Return (samples list of float in [-1,1], sample_rate)."""
    with wave.open(path, "rb") as w:
        n_ch = w.getnchannels()
        width = w.getsampwidth()
        sr = w.getframerate()
        raw = w.readframes(w.getnframes())
    if width != 2:
        raise SystemExit(f"expected 16-bit PCM after decode, got {width*8}-bit")
    import array
    a = array.array("h")
    a.frombytes(raw)
    if sys.byteorder == "big":
        a.byteswap()
    if n_ch > 1:  # decode step forces mono, but stay safe
        a = a[0::n_ch]
    return [s / 32768.0 for s in a], sr


def rms_envelope(samples, sr, window_ms):
    """RMS per non-overlapping window. Returns (env list, window_ms_actual)."""
    win = max(1, int(sr * window_ms / 1000.0))
    env = []
    for i in range(0, len(samples) - win + 1, win):
        acc = 0.0
        for j in range(i, i + win):
            acc += samples[j] * samples[j]
        env.append(math.sqrt(acc / win))
    return env, 1000.0 * win / sr


def segment(env, win_ms, min_seg_ms):
    """Hysteresis threshold -> list of (is_on, dur_ms, start_ms)."""
    ordered = sorted(env)
    floor = ordered[max(0, int(0.05 * len(ordered)))]
    peak = ordered[min(len(ordered) - 1, int(0.98 * len(ordered)))]
    span = max(1e-9, peak - floor)
    hi = floor + 0.25 * span
    lo = floor + 0.12 * span

    segs = []
    on = env[0] > hi
    start = 0
    for i, e in enumerate(env):
        if on and e < lo:
            segs.append((True, (i - start) * win_ms, start * win_ms))
            on, start = False, i
        elif not on and e > hi:
            segs.append((False, (i - start) * win_ms, start * win_ms))
            on, start = True, i
    segs.append((on, (len(env) - start) * win_ms, start * win_ms))

    # Drop sub-min segments by merging into the previous run (debounce).
    merged = []
    for s in segs:
        if merged and s[1] < min_seg_ms:
            p = merged[-1]
            merged[-1] = (p[0], p[1] + s[1], p[2])
        elif merged and merged[-1][0] == s[0]:
            p = merged[-1]
            merged[-1] = (p[0], p[1] + s[1], p[2])
        else:
            merged.append(list(s))
    return [tuple(s) for s in merged], hi, lo, floor, peak


def stats(label, xs):
    if not xs:
        return f"  {label:<12} (none)"
    mean = statistics.mean(xs)
    med = statistics.median(xs)
    sd = statistics.pstdev(xs) if len(xs) > 1 else 0.0
    return (f"  {label:<12} n={len(xs):<4} mean={mean:7.2f}ms "
            f"median={med:7.2f}ms sd={sd:6.2f} min={min(xs):6.2f} "
            f"max={max(xs):6.2f}")


def estimate_freq(samples, sr, seg_start_ms, seg_dur_ms):
    """Fundamental estimate over one on-segment: Goertzel peak + zero-crossings."""
    a = int(seg_start_ms / 1000.0 * sr)
    b = min(len(samples), a + int(seg_dur_ms / 1000.0 * sr))
    chunk = samples[a:b]
    if len(chunk) < 64:
        return None, None
    # Zero-crossing rate
    zc = sum(1 for i in range(1, len(chunk))
             if (chunk[i - 1] < 0) != (chunk[i] < 0))
    zc_hz = zc * sr / (2.0 * len(chunk))
    # Goertzel peak over a coarse grid
    best_f, best_p = None, -1.0
    f = 200.0
    while f <= 3000.0:
        w = 2.0 * math.pi * f / sr
        coeff = 2.0 * math.cos(w)
        s0 = s1 = s2 = 0.0
        for x in chunk:
            s0 = x + coeff * s1 - s2
            s2, s1 = s1, s0
        power = s1 * s1 + s2 * s2 - coeff * s1 * s2
        if power > best_p:
            best_p, best_f = power, f
        f += 25.0
    return best_f, zc_hz


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording")
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--window-ms", type=float, default=1.0)
    ap.add_argument("--min-segment-ms", type=float, default=4.0)
    args = ap.parse_args()

    tmp = decode_to_wav(args.recording, args.rate)
    try:
        samples, sr = read_samples(tmp)
    finally:
        os.remove(tmp)

    dur_s = len(samples) / sr
    env, win_ms = rms_envelope(samples, sr, args.window_ms)
    segs, hi, lo, floor, peak = segment(env, win_ms, args.min_segment_ms)

    ons = [d for (o, d, _) in segs if o]
    offs = [d for (o, d, _) in segs if not o]
    on_starts = [s for (o, _, s) in segs if o]
    periods = [on_starts[i + 1] - on_starts[i] for i in range(len(on_starts) - 1)]

    print(f"File          : {args.recording}")
    print(f"Decoded       : {dur_s:.2f}s mono @ {sr} Hz, "
          f"envelope window {win_ms:.2f}ms")
    print(f"Env floor/peak: {floor:.4f} / {peak:.4f}  "
          f"(on>{hi:.4f}, off<{lo:.4f})")
    print()
    print("Segment durations:")
    print(stats("BEEP (on)", ons))
    print(stats("GAP (off)", offs))
    print(stats("PERIOD", periods))
    if periods and ons:
        duty = 100.0 * statistics.mean(ons) / statistics.mean(periods)
        rate = 1000.0 / statistics.mean(periods)
        print(f"  duty cycle   {duty:5.1f}%   beep rate {rate:5.1f}/s")
    print()

    if ons:
        longest = max((s for s in segs if s[0]), key=lambda s: s[1])
        gf, zc = estimate_freq(samples, sr, longest[2], longest[1])
        if gf:
            print(f"Tone (longest beep {longest[1]:.1f}ms): "
                  f"~{gf:.0f} Hz (Goertzel), ~{zc:.0f} Hz (zero-cross)")
        print()

    print("First segments (ms):")
    for o, d, s in segs[:28]:
        print(f"  t={s:8.1f}  {'BEEP' if o else 'gap '}  {d:7.2f}ms")


if __name__ == "__main__":
    main()
