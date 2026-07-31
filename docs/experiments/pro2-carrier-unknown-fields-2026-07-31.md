# Pro Controller 2 motion carrier — remaining unknown fields — 2026-07-31

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
