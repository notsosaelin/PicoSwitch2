# Pro Controller 2 motion carrier — remaining unknown fields — 2026-07-31

> **Correction, 2026-08-01:** This report correctly found the high-rate signed22 gyro field, but
> its `/256` physical conversion copied the adjacent acceleration field's binary point. The Pro2
> sensor/common stream remains `16.4 counts/dps`; high-rate gyro uses **seven fractional bits
> (`/128`)**. High-rate acceleration remains `/256`, and normal/catch-up scaling is unchanged.
> Historical tables below retain the original interpretation. See
> [`pro2-imu-constants-audit-2026-08-01.md`](pro2-imu-constants-audit-2026-08-01.md).

## Question

After the length-`0x1E`/`0x28` decode, is anything left undecoded? A byte-exact
re-encoding test answers this directly: fields that can be reproduced but not
explained are exactly the ones a synthesizer would get wrong.

## Method

`tools/ns2_motion_packet.py` builds complete PDUs; `tools/test_ns2_motion_packet.py`
decodes every genuine PDU in the repository corpus, re-encodes it from the
decoded fields, and compares bytes.

```powershell
python tools\test_ns2_motion_packet.py
python tools\test_ns2_motion_carrier.py
```

Corpus: 52 captures under `dumps/BLE CAPTURE/` and `dumps/motion/`.

## Result: the decode is byte-exact

| Form | Records | Re-encoded byte-for-byte |
|---|---:|---|
| `0x28` high-rate | 858 | ✅ |
| `0x28` normal | 149 | ✅ |
| `0x28` catch-up | 981 | ✅ |
| `0x1E` carrier | 2,070 | ✅ |

Zero mismatches. Every field position, width, sign convention, and the LSB-first
packing are confirmed against genuine hardware.

**Byte-exactness is reproduction, not comprehension.** Two fields are reproduced
without being understood, and the test is what exposed them — an earlier pass
silently zeroed one of them and still produced "plausible" packets.

## Unknown 1: length-`0x1E` byte 12, bit 7

Set in **280 of 2,070** genuine carriers (13.5%).

**It tracks motion.** Median inter-record carrier change:

| Flag | Median max abs change |
|---|---:|
| set | `0.001280` |
| clear | `0.000012` |

A 100× separation. It is 0% across stationary captures and 23–48% across moving
ones (`sw2_native_passthrough_live` 37%, `variant9_fast_link` 39%,
`chart-face-forward` 48%, the three Splatoon boundary captures 23–29%).

It is **not** a simple threshold on that change — thresholding at the median
gives only 21% precision against a 13.4% base rate — and it alternates rapidly
inside moving captures (85 runs across 185 records).

Ruled out, none beating the 86.5% trivial-zero baseline:

| Candidate | Agreement |
|---|---:|
| sign of retained lane 0 / 1 / 2 | 28.1% / 39.4% / 81.1% |
| lane-1 bit 24 / bit 25 | 60.6% / 86.5% (bit 25 is always zero) |
| tick parity | 50.0% |
| previous record's flag | 84.5% |
| retained energy > 0.75 | 20.8% precision |
| retained energy > 1.0 | 14.3% precision |
| max abs retained > 0.95 / 0.99 of limit | 27.0% / 25.9% precision |
| second-largest magnitude > 0.5 | 19.9% precision |

### ❌ Refuted: whole-quaternion sign canonicalization

The obvious candidate was that the bit records whether the omitted component was
negated before packing. Nintendo's Switch 1 DScale packer performs exactly that
negation and does not transmit the result; if Switch 2 transmitted it, this is
where it would live. It also predicted the observed behaviour — a stationary
controller holds one sign, a rotating one crosses zero repeatedly.

**No hardware was needed to test it.** A whole-quaternion negation flips all
three transmitted lanes together, so negating the later record must restore
continuity exactly when the flag toggles. Over 160 adjacent same-chart toggles
across six captures, including the 94-record all-state-3
`pro2-chart-face-forward-no-transition` capture:

| Adjacent same-chart pairs | n | median `d_raw` | median `d_neg` | negation helps |
|---|---:|---:|---:|---:|
| flag toggles | 160 | `0.002511` | `1.318595` | **0 / 160** |
| flag unchanged | 375 | `0.003473` | `1.304876` | 0 / 375 |

The trajectory is already smooth without negation in every single case.
Refuted.

### ❌ Refuted: interleaving or cadence structure

The flag rate is flat regardless of packet context — 12% when the next PDU is a
`0x28` versus 14% when it is not, 13% either way for the preceding PDU, and
13–14% across every tick-gap bucket (5, 6, 11, 12, 13). No relation to the
`0x1E`/`0x28` interleave or to the connection interval.

### Status

Characterized but unexplained. It is **not** relied upon anywhere, and
`tools/ns2_motion_packet.py` preserves it verbatim.

What is established: it tracks motion strongly (100× separation), it is
per-sample rather than modal, and it is none of the twelve candidates tested.
A useful next probe would target what changes *within* motion rather than
between motion and rest, since the toggle rate does not track motion magnitude
(`d_raw` 0.0025 on toggle versus 0.0035 on hold — if anything slightly lower).

### Adjacent bits, for completeness

Bits 2–6 of byte 12, and bits 2–7 of bytes 4 and 8, are zero in **every** genuine
record. Byte 12 bit 1 is never set, which independently confirms carrier lane 1
is **25 bits**, not the 26 the decoder's mask would allow.

## Unknown 2: length-`0x28` status `0x00`

Five packets out of 1,988 carry sensor status `0x00` instead of `0x0D`/`0x0E`/
`0x0F`. All five are in `pro2-imuref-15ms-raw-native-raw-2026-07-29.jsonl`, a
raw↔native handoff capture.

The remaining 1,983 packets agree with layout selection exactly: `0x0D` ⇔
elapsed 0–10, `0x0E` ⇔ 11–14, `0x0F` ⇔ 15+.

`docs/switch2/ble-controller-protocol-inventory.md` records `0x04` as "no data
was read from the IMU" for a different report length, so a "no new IMU data"
meaning for `0x00` is plausible but unproven. Their concentration in a handoff
capture also admits a transitional explanation.

`tools/ns2_motion_packet.py` never emits status `0x00`: a synthesizer must not
claim a condition it does not model. The test counts these rather than folding
them into a layout, and fails if they stop being rare.

## What byte-exactness did not catch

Byte-exactness proves encode and decode agree on the bits. It cannot prove they
agree with physics. Building a generator on top of the byte-exact map exposed
two defects that every round-trip test passes straight through.

### Defect: wire values are not a single unit

Each layout packs its slots at a different fixed-point scale, and **slot width
does not determine it**. A generator supplying ordinary ICM counts to the
high-rate layout is wrong by 256x; the packet is well-formed, decodes cleanly,
and reads back as 1/256 g.

`ns2_motion_reference.WIRE_TO_COUNTS` is now the single authority:

| Layout | accel slots | gyro slots |
|---|---|---|
| high-rate | ×1/256, ×1/256 | ×1/256 |
| normal | ×1, ×2, ×1 | ×2, ×1 |
| catch-up | ×1, ×2, ×1 | ×1/4, ×1/4 |

Verified across the corpus: all **eight** acceleration slots in all three
layouts agree on **1.051–1.052 g** once normalized, despite raw medians
spanning 2,153 to 1,102,983 — a 512× range collapsing to 0.1% agreement. Gyro
noise floors likewise agree at 0.15–0.19 dps.

### Defect: comparing unnormalized magnitudes

Pooling raw slots describes the layout mix, not the motion. The first synthetic
comparison reported a genuine acceleration median of 1,101,581 counts; the real
figure is 1.05 g.

## Chart hysteresis, measured against hardware

Replaying each genuine carrier's own orientation through `select_chart`, and
deciding from the **genuine** previous chart so that one divergence cannot
cascade: **2,059 / 2,070 decisions agree (99.47%)**.

Read honestly, holds outnumber swaps ~180:1, so that figure is carried by
correctly holding. We reproduce **1 of the hardware's 11 swaps**. All 11
disagreements share one shape: the hardware swapped while the held chart was
still representable, at margins 0.667–0.994 of the limit.

### ❌ Refuted: one-sample lookahead

Only **1 of 11** genuine swaps has a next-sample margin reaching the limit —
and it is the one already over at the current sample. Lookahead predicts
nothing the current sample does not.

### ❌ Refuted: an earlier fixed threshold

Genuine *holds* reach **0.9998** of the limit, so the classes are not
separable by margin. A threshold sweep:

| Threshold | Genuine swaps missed | Spurious swaps |
|---|---:|---:|
| 0.90 | 3 | 600 |
| 0.95 | 6 | 151 |
| 0.99 | 8 | 19 |
| 1.00 | 10 | **0** |

Swap timing is not a function of the carrier alone.

### Why this does not block a synthesizer

Both charts at a swap carry the same three values, so chart choice is a
**lossless** representational choice — a decoder recovers the same orientation
either way. The property that must hold exactly is that no lane is ever asked
to leave the field, and swapping at the limit guarantees it by construction
with zero spurious swaps. `test_we_never_ask_a_lane_to_leave_the_field` pins
that down; the 99.47% figure is context, not the bar.

## Two emission modes, and which one to translate into

An adapter cannot emit `0x28` without knowing when to send it and what to put
in the elapsed field. Both are measurable, and the corpus splits **perfectly** —
no capture is mixed.

| Mode | Captures | `0x28` packets | `elapsed` == tick delta since previous `0x28` |
|---|---:|---:|---:|
| `0x28`-only (no `0x1E` at all) | 14 | 1,196 | **100.0%** |
| interleaved with `0x1E` | 24 | — | **~0%** |

In `0x28`-only mode the elapsed count is simply the inter-packet tick gap. The
`pro2-native-interval-*` captures sweep this mode from 8 to 24 ticks and agree
exactly at every interval.

In interleaved mode elapsed counts back only to the most recent PDU of **any**
length. It sits near a constant 7 while the `0x28`→`0x28` delta ranges over
11–30. `elapsed × (intervening carriers + 1)` predicts that delta but
consistently overshoots by 3–5 ticks; **the exact interleaved relation is not
resolved.**

### Policy: translate into `0x28`-only mode

It is a genuine hardware mode, its elapsed rule is exactly verified over 1,196
packets, and choosing it removes the unresolved interleaved semantics from the
problem entirely. The generator sets elapsed from its own emit interval.

### Slot budget

The layouts need real samples, and `ns2_motion_packet.py` refuses to invent
them. At the 800 Hz tick, for a source reporting near 250 Hz:

| Interval | Layout | Needs | Available |
|---|---|---|---:|
| 8 ticks (10.0 ms) | high-rate | 2 accel + 1 gyro | ~2.5 |
| 12 ticks (15.0 ms) | normal | 3 accel + 2 gyro | ~3.8 |
| 24 ticks (30.0 ms) | catch-up | 3 accel + 2 gyro | ~7.5 |

The longer intervals carry comfortable margin. High-rate is marginal, and a
source slower than 200 Hz cannot fill it without inventing samples.

### Target the catch-up layout

Catch-up's tail is a **single bit, zero in all 981 corpus packets**. The other
two layouts carry a 16-bit tail holding two Q3 temperature samples, which a
translated source has no way to produce — a DualSense exposes no die
temperature. Choosing catch-up removes that fabrication entirely.

| Layout | Tail | Must synthesize temperature? |
|---|---|---|
| catch-up | 1 bit, always 0 | **no** |
| normal | 16 bit, Q3 pair | yes |
| high-rate | 16 bit, Q3 pair | yes |

Catch-up also raises the sample rate rather than lowering it. The shipping
`0x1E` path carries one orientation per packet at 133 Hz. A `0x28` catch-up
packet every 20 ms carries 3 acceleration and 2 gyro samples, so **50 packets/s
delivers ~250 IMU samples/s** — the point of the multi-sample layouts, and a
fidelity gain over the current path rather than a trade.

`pro2-native-interval-16` through `-24` are genuine hardware captures in exactly
this configuration.

## The three translation design decisions, audited

Wiring the translator required three decisions that the byte-exact packer does
not settle, because a packet can be perfectly well-formed and still describe
the wrong physical timeline. Each was checked against the corpus before any
hardware test.

### ✅ Decision 1: saturation clamps, never wraps — the wire limit *is* the
sensor limit

Every layout and every slot converges on the same physical full-scale range.
Twelve independent `(width, scale)` pairs:

| layout | field | width | wire→counts | full scale |
|---|---|---|---|---|
| high-rate | accel ×2 | 22 | 1/256 | ±8192 counts = **±2.00 g** |
| high-rate | gyro | 22 | 1/256 | ±8192 counts = **±499.51 dps** |
| normal | accel | 14 / 13 / 14 | 1 / 2 / 1 | ±8192 counts = **±2.00 g** |
| normal | gyro | 13 / 14 | 2 / 1 | ±8192 counts = **±499.51 dps** |
| catch-up | accel | 14 / 13 / 14 | 1 / 2 / 1 | ±8192 counts = **±2.00 g** |
| catch-up | gyro | 16 / 16 | 1/4 | ±8192 counts = **±499.51 dps** |

±2 g and ±500 dps are stock ICM full-scale settings. Two consequences:

1. The `WIRE_TO_COUNTS` factors were originally derived empirically, by
   median-matching acceleration magnitude across layouts. They now fall out of
   a hardware constraint independently — a genuine cross-check, not a
   restatement of how they were found.
2. Clamping at the wire limit **is** clamping at the sensor's own limit.
   Genuine hardware cannot report beyond ±2 g / ±499.5 dps either, so
   saturating there is what the real controller does. Wrapping would invert
   the reported direction of motion, which is far worse than clipping it.

The corpus does not independently exercise the limit — the largest genuine
gyro wire value observed is 669, i.e. 10.2 dps of the 499 available — because
every catch-up capture is low-motion. The convergence argument is what carries
this, not the observed range.

### ✅ Decision 2: slots span the emit window — this was a real defect

**The first implementation was wrong.** It filled the three acceleration slots
from the first three samples to arrive and dropped the rest, so a packet
covered only the head of its window and discarded the freshest ~40% of the
data outright.

Genuine packets place the oldest sample in slot 0 and the **newest** in the
last slot. Measured across 973 catch-up packets, mean-square difference
between slots, computed *within capture* and corrected for slot 1's coarser
quantization, as a fraction of the full-window value:

| pair | mean \|Δ\|² | / asymptote |
|---|---|---|
| seam `a2[N]`→`a0[N+1]` | 13.80 | **0.572** ← smallest gap in the stream |
| `a0`→`a1` | 14.64 | 0.607 |
| `a1`→`a2` | 16.39 | 0.680 |
| `a0`→`a2` | 20.87 | 0.866 |
| one whole window | 24.10 | 1.000 ← saturated asymptote |

The ordering is strictly monotone in slot index, and the decisive fact is the
seam. **If a packet held three consecutive samples taken at the start of its
window, the step from its last slot to the next packet's first slot would be
the largest gap in the stream — not the smallest.** A paired sign test over
894 tick-contiguous packet pairs puts the seam below the within-packet
`a0`→`a2` gap in 67.1% of pairs (z = +10.2).

What is **not** resolved is each slot's exact fractional position. The
accelerometer structure function saturates before one window elapses, so the
map from mean-square difference back to elapsed time is compressive; and the
corpus is stationary (per-axis noise σ ≈ 2.0 counts, flat structure function
at every lag from 20 ms to 150 ms). The gaps can be **ordered** but not
**measured**.

Two analysis errors are worth recording, because both produced confident wrong
numbers:

* An earlier reading — "intra-packet 3.7 counts vs inter-packet 3.0 counts
  proves spanning" — was **not supported**. Raw difference magnitudes in a
  stationary corpus are noise-dominated; that comparison had no power. The
  conclusion happened to survive, on entirely different evidence.
* Pooling the statistic **across** captures inflated `a0`→`a2` to 1.43 of the
  asymptote — an impossible value for a saturating structure function, and the
  signal that something was wrong. Different captures rest at different
  orientations; pooling mixed populations. It appeared as a y-axis-only
  anomaly and briefly looked like a slot-2 scale error. Within capture it
  vanishes entirely. Note that byte-exact re-encoding proves the bit layout is
  a correct bijection but **not** that each lane is assigned to the right
  (slot, axis) — a permutation would round-trip perfectly too.

Gyro placement is genuinely unresolved. Its paired sign test came out weak and
with the *opposite* sign to acceleration (z = −4.0), which is what quarter-point
spacing would produce, since that makes the within-packet and seam gaps equal.
A stationary gyro is pure noise, so this is near the floor either way.

Implementation: `ns2_ds5_motion40` buffers timestamped samples in a ring and
selects at build time, when the window length is finally known. Acceleration
anchors the ends — oldest sample first, newest last — and spaces the interior
evenly. Gyro takes the quarter points, which also give the unbiased
trapezoidal estimate of the window's integral; the console integrates gyro, so
the mean matters more than the freshness of either endpoint.

A second defect surfaced while fixing this: the emit window and the
console-visible tick timeline are **two different clocks**. Reporting `elapsed`
from the poll time rather than from the newest sample sent would claim a span
wider than the samples cover, and the error would accumulate against the
console's clock. `last_emit_us` now advances by exactly the elapsed reported
(so truncation remainders carry forward instead of drifting), while
`last_sample_us` separately bounds the next selection window so no sample can
ever appear in two packets.

### ✅ Decision 3: enabling replaces the motion block

Confirmed by a corpus-wide correlation with **zero exceptions across 32
captures**: the emission mode follows the BLE notification interval, not a host
feature mask.

| notification interval | captures | mode |
|---|---|---|
| 6.0 ticks (7.5 ms) | 15 | always interleaved `0x1E` + `0x28` |
| ≥ 8.0 ticks (10 ms) | 17 | always `0x28`-only |

The interval captures were checked to confirm they are not filtered — they
genuinely contain only `0x28`. Since only the `0x28`-only mode has a resolved
elapsed relation (tick delta since the previous `0x28`, 1196/1196), sending
`0x1E` alongside would put us in the mode whose elapsed semantics are still
unknown.

**Residual unverified risk:** our USB poll rate is ~1 ms, which corresponds to
no genuine BLE interval in the table. Mitigating precedent: the `0x1E` path
already repeats its latest carrier across polls and is console-validated, and
the `0x28` path repeats identically. This is the main thing the hardware A/B
is for.

## ❌ First hardware A/B failed — and why the offline tests could not have caught it

**Result (2026-07-31):** the console *accepted* the packets and acted on them.
Counters were healthy — 50.6 packets/s, 19.75 ms cadence, `emitted_len` 40,
zero saturation — and in-game motion was violent and erratic. Accepted but
wrong, which is a different failure from the 250 Hz experiment's absent gyro.

Two root causes, both invisible to every test that existed.

### The prefix describes the wrong instant

A 0x28 prefix is a modular slice of the same orientation carrier that 0x1E
sends outright, so a generator must decide *which instant* the sliced
orientation describes. Nothing in the byte layout asks that question, so
byte-exactness, field-range checks and encoder round-trips all pass regardless
of the answer. `ns2_ds5_motion40_build()` passed the CURRENT carrier — the
orientation at the packet's own tick.

Measured against 24 interleaved captures and 773 paired packets
(`tools/ns2_motion40_prefix_epoch.py`), that is wrong:

| model | best constant | pooled median error |
|---|---|---|
| A: `coordinate = tick − elapsed + c` | c = +3.9 ticks | 0.00023° |
| B: `coordinate = tick + c` | c = −3.1 ticks | 0.00023° |

At the correct epoch the prefix reproduces the genuine orientation almost
exactly. At `coordinate = tick` — what the firmware did — the moving captures
degrade by 20–80×: `chart-transition-splatoon-3-to-1` goes 0.039° → 0.901°,
`lazy-susan-return` 0.005° → 0.390°. Stationary captures barely move, which is
why every stationary comparison in this document passed.

The prefix lags the packet tick by **at least ~3 ticks**, and under model A by
`elapsed − 4` — 12.1 ticks (15 ms) at our catch-up cadence. If the console
anchors on the prefix and integrates the packet's own IMU samples forward, an
end-of-window prefix double-counts the whole window's rotation.

🔵 **Which model is right is unresolved.** They differ only when the window
length varies, and elapsed is 7 in almost every paired packet. Separating them
needs interleaved traffic captured at a long notification interval — see
"Suggested experiments" below.

### Catch-up was the wrong layout to target

Of the 773 paired packets — the only 0x28s with a genuine 0x1E alongside to
validate against — the layouts are:

| layout | paired packets |
|---|---|
| high-rate | **768** |
| normal | 3 |
| catch-up | **2** |

Catch-up appears almost exclusively in 0x28-**only** captures, which by
definition carry no 0x1E. So the layout this translator targets is the one
layout with essentially no paired orientation ground truth, and the epoch above
is measured on a layout we do not emit.

Catch-up was chosen because its tail is a single always-zero bit while
high-rate carries a 16-bit Q3 temperature pair a DualSense cannot supply. That
optimised for *ease of filling* over *strength of evidence*, which is the wrong
trade and the reason offline validation kept passing while hardware failed.

The tail is a weak objection anyway: across 771 genuine high-rate packets the
tail takes 35 distinct values dominated by `0x01C0` (155 packets), and the
0x1E builder already ships a genuine nominal temperature constant.

### Consequences

- Prefer the layout with paired ground truth (**high-rate**) over the layout
  that is convenient to fill.
- Emitting at elapsed ≈ 7–8 also collapses the model-A/model-B ambiguity: the
  two disagree by 1 tick there, against 9 ticks at elapsed 16.
- An "offline-validated" claim must state *which relationship* was validated.
  Byte-exactness validated the layout. It never validated the semantics of a
  single field, and this document previously blurred that.

## Consequence for a synthesizer

The carrier, chart hysteresis, saturation trigger, prefix slice, epoch, all three
cadence layouts, their packed IMU fields, and the Q3 temperature tail are
decoded and byte-exact.

Two fields are reproduced but not understood. Neither blocks generation — both
are carried through verbatim — but the byte-12 flag should be resolved before
claiming the format is fully understood, because if the sign hypothesis holds it
changes what a decoder can recover.

## Reproduce

```powershell
python tools\test_ns2_motion_packet.py    # 14 tests: byte-exact replay, chart
                                          # hysteresis, scale normalization
python tools\test_ns2_motion_carrier.py   # 20 tests: codec + firmware parity

# Where in the emit window each IMU slot sits -- the evidence behind the
# slot-placement policy in ns2_ds5_motion40.c (Decision 2 above).
python tools\ns2_motion40_slot_timing.py

# Structural comparison against an input-matched genuine stream.
python tools\ns2_motion_synth.py dumps\motion\2026-07-24\ds5-pro2-paired-pitch-2026-07-22.jsonl
```

The paired captures carry both sides at the same instant — the genuine `native`
PDU and the DualSense `cal_g`/`cal_a` that produced motion at that moment — so
the comparison is input-matched rather than two unrelated sessions.

Current result on `ds5-pro2-paired-pitch`: acceleration 1.054 g genuine against
1.009 g synthetic, gyro 9.99 dps against 5.83 dps. Both physical quantities are
now the right size and the right unit.

Note the report's three sections. Only **PHYSICAL** is a pass criterion.
Absolute lane occupancy differs by design — our orientation is gyro-integrated
from identity exactly as the shipping `0x1E` path is (`ns2_ds5_motion.c`),
while the genuine controller carries gravity-referenced attitude, and the
console demonstrably accepts the former. Layout mix in this harness follows the
capture's record spacing, not the adapter's emit timing, so it is
informational.
# Correction — high-rate gyro binary point (2026-08-01)

This report correctly found the high-rate signed22 gyro field and exposed its raw values, but its
`/256` physical conversion copied the adjacent acceleration field's binary point. Existing evidence
now separates the factors: the Pro2 sensor/common stream remains `16.4 counts/dps`, while high-rate
gyro uses **seven fractional bits (`/128`)**. High-rate acceleration remains `/256`; normal and
catch-up scaling is unchanged. Historical tables below retain the original interpretation so the
investigation remains auditable. The corrected authority is
[`../switch2/report-0x09-motion.md`](../switch2/report-0x09-motion.md) and the resolution is in
[`pro2-imu-constants-audit-2026-08-01.md`](pro2-imu-constants-audit-2026-08-01.md).
