# Length-`0x28` generation — evidence audit — 2026-07-31

## Why this document exists

The `0x28` translator was reported as "offline-validated" three times and
failed on hardware. Each failure was then diagnosed from data already in this
repository. That is not three unlucky bugs; it is one methodological error
producing repeat symptoms.

**The error:** every test built for this feature compared one of our
implementations against another of our implementations. Encoder against
decoder. C against Python. Firmware against reference. Those are *consistency*
tests. A consistency test cannot detect a wrong semantic choice, because both
sides of the comparison share the assumption being tested.

Byte-exactness on 981 genuine packets proves exactly one thing: **given the
fields decoded from a genuine packet, the encoder reproduces that packet.** It
is a bijection test on the *layout*. It says nothing about whether the fields
*we generate from DualSense input* are the right fields. The number 981/981 is
persuasive and measures the wrong axis.

The only test that can catch a semantic error is: **drive the translator from
real input and compare its output against the genuine output captured from the
same instant.** The paired captures that permit exactly this
(`ds5-pro2-paired-*`) existed the whole time and were used only for a scalar
magnitude comparison (1.05 g vs 1.01 g), never for a packet comparison.

This document therefore classifies every generated field by **what evidence
supports the value we put there**, separately from whether it lands in the
right bits.

## The distinction that was blurred

| claim | what it means | what it does NOT mean |
|---|---|---|
| "byte-exact on 981 packets" | the layout is a correct bijection | that any generated value is correct |
| "matches the reference on 30 vectors" | C matches Python | that the Python formula is right |
| "all host tests pass" | internal consistency holds | that hardware agrees |
| "1.05 g vs 1.01 g" | magnitudes are the right size | that axes, order, frame or epoch are right |

A byte-exact packet can carry an orientation from the wrong moment, samples
from the wrong instants, axes in the wrong order, and a layout the console
never uses in that mode — and every test above still passes.

## Field-by-field audit of a generated catch-up packet

Status legend: ✅ Complete · 🟡 In Progress · 🔵 Partial · 🔴 Blocked · ⬜ Not Started

| field | what we generate | evidence for the VALUE | status |
|---|---|---|---|
| `packing_mode` = 3 | constant | 3 in all 981 genuine catch-up packets | ✅ |
| `status` = `0x0F` | constant | `0x0F` in all genuine catch-up | ✅ |
| `tail_bit` = 0 | constant | 0 in all 981 | ✅ |
| `tick` | our own accumulator from 0 | genuine tick is an 800 Hz counter; our absolute origin is arbitrary and its acceptability is untested | 🔵 |
| `elapsed` | our emit window in ticks | the *relation* (tick delta in `0x28`-only) is validated 1196/1196; our *choice* of ~16 is not | 🔵 |
| prefix carrier lanes | modular slice of the **current** `0x1E` carrier | **wrong instant.** Genuine lags the packet tick by ≥3 ticks, up to `elapsed − 4` | 🔴 |
| accel slot scaling | raw ÷2, per-slot wire factor | all 12 (width, scale) pairs converge on ±2 g — strong and independent | ✅ |
| gyro slot scaling | raw ×4 | same convergence argument, ±499.5 dps | ✅ |
| accel/gyro **axis order and sign** | inherited from the `0x1E` path | validated for `0x1E` by working gameplay; **never checked** that `0x28` slots use the same convention | 🔴 |
| accel/gyro **slot instants** | oldest→newest across the window | ordering confirmed on 973 packets; exact fractional positions unresolved | 🔵 |
| gyro slot instants | quarter points | **not resolved** — a stationary gyro is pure noise | 🔴 |
| layout choice (catch-up) | chosen for its 1-bit tail | 768 of 773 paired packets are **high-rate**; catch-up has 2 | 🔴 |

## Structural unknowns that block generation

These are not field values. They are questions about the stream that no
byte-level work can answer.

1. **🔴 Chart-state bootstrap in `0x28`-only mode.** Unwrapping the modular
   prefix requires a known chart state. 17 captures carry `0x28` with **zero**
   `0x1E` (one has 255 packets), so in steady state the console receives no
   `0x1E` to take that state from. Where it comes from is unknown. Our decoder
   cannot decode these captures without a `0x1E`, which means *we cannot
   currently verify our own output in the mode we chose to emit.*
   Caveat: these captures may begin mid-session, so a `0x1E` at connection
   setup is not excluded.
2. **🔴 Prefix epoch model.** Fixed lag (~3 ticks) or window-relative
   (`elapsed − 4`)? Indistinguishable on this corpus because elapsed is 7 in
   almost every paired packet. They diverge by 9 ticks at our cadence.
3. **🔵 Repeat tolerance.** A 1 kHz USB poll against a ~50 Hz packet cadence
   sends each `0x28` ~20 times. Genuine never repeats one. Whether the console
   deduplicates by tick is untested — this was the hypothesis behind the fill
   modes and it remains unverified either way.

## What "nearly absolutely certain" requires

In dependency order. Nothing below needs hardware.

1. **Build the golden replay harness.** Drive the translator from the paired
   captures' DualSense samples; compare the generated packet against the
   genuine `0x28` from the same instant, field by field. This is the test that
   would have caught the epoch error, the layout error, and any axis error, and
   it is the only one that can. Ground truth: 768 paired high-rate packets.
2. **Retarget to high-rate**, the layout with paired ground truth, at elapsed
   ≈ 7–8 — which also collapses the epoch ambiguity from 9 ticks to 1.
3. **Resolve the axis convention** for `0x28` slots against paired data rather
   than assuming the `0x1E` mapping transfers.
4. **Resolve chart bootstrap**, or emit interleaved so a `0x1E` supplies it.
5. Only then flash — and only with a stated, falsifiable prediction of what
   the console should do.

The bar for asking for a hardware test is: **the harness reproduces genuine
packets from genuine input**, with any divergence explained. Not "tests pass."

## Reproduce

```powershell
python tools\ns2_motion40_prefix_epoch.py      # epoch, both models
python tools\ns2_motion40_slot_timing.py       # slot ordering + layout bands
```
