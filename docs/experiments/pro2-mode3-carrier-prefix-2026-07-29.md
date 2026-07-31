# Pro Controller 2 mode-3 split-carrier correction — 2026-07-29

## Question

Why did the length-`0x28` prefix appear to need a different scale in a small pitch capture than in
a larger moving capture? The discriminators were:

1. a wrong sampling epoch;
2. a differential/adaptive state model; or
3. a wrong bit-boundary interpretation before either comparison.

This was answered offline before requesting another hardware action. A first pass fixed most of
the boundary but incorrectly named two bits between the second and third carrier pieces a separate
state. A second pass tested that proposed state-transition law and found the simpler exact layout:
those are carrier 2's low two bits. The existing dynamic corpus was sufficient; no UART action or
firmware change was needed.

## Method

[`tools/ns2_motion_reference.py`](../../tools/ns2_motion_reference.py) now:

- decodes the length-`0x1E` two-bit chart state and retained wire-carrier lanes;
- preserves the genuine sensor timer and rejects non-contiguous interpolation spans;
- compares constant quarter-tick offsets with the packet-derived
  `current tick - encoded elapsed + 4` epoch;
- reconstructs the split third carrier before fitting it;
- groups comparisons only by the length-`0x1E` carrier state;
- reports unconstrained affine slopes and a fixed power-of-two-scale fit separately;
- causally unwraps each modular prefix using only the preceding length-`0x1E` chart/history, then
  compares it with the interpolated reference at the packet-derived epoch.

The structural lead was Nintendo's earlier stateful motion packing, but the result below comes from
direct Switch 2 captures. Third-party source was not treated as Switch 2 protocol evidence.

Reproduce the two dynamic checks:

```powershell
python tools\ns2_motion_reference.py `
  --motionpair dumps\motion\2026-07-24\ds5-pro2-paired-pitch-2026-07-22.jsonl

python tools\ns2_motion_reference.py `
  --blecap "dumps\BLE CAPTURE\sw2_native_passthrough_live_2026-07-21.jsonl"
```

Run the decoder regression:

```powershell
python tools\test_ns2_motion_reference.py
```

## Corrected prefix layout

All bit positions below are relative to the 36-byte payload after the four-byte
timer/elapsed/status preamble.

### High-rate layout, elapsed `0..10`

| Payload bits | Width | Meaning established by this pass |
|---:|---:|---|
| `0..1` | 2 | packing mode; always `3` in the analyzed corpus |
| `2..25` | 24 | carrier lane 0, signed |
| `26..48` | 23 | carrier lane 1, signed |
| `49..50` | 2 | carrier lane 2 bits `1..0` |
| `51..73` | 23 | carrier lane 2 bits `24..2`, signed as one `s25` field with bits `49..50` |
| `74..` | — | previously validated high-rate packed IMU samples and tail |

The high-rate carrier is therefore `s24+s23+s25`. It has exactly two extra fractional bits in
every lane. Divide all three reconstructed wire values by four before comparing them with the
common carrier units.

### Normal and catch-up layouts, elapsed `11+`

| Payload bits | Width | Meaning established by this pass |
|---:|---:|---|
| `0..1` | 2 | packing mode; always `3` in the analyzed corpus |
| `2..23` | 22 | carrier lane 0, signed |
| `24..44` | 21 | carrier lane 1, signed |
| `45..46` | 2 | carrier lane 2 bits `1..0` |
| `47..67` | 21 | carrier lane 2 bits `22..2`, signed as one `s23` field with bits `45..46` |
| `68..` | — | previously validated normal/catch-up packed IMU samples and tail |

The normal/catch-up carrier is therefore `s22+s21+s23`. The previous
`flags2 + three equal-width signed lanes` interpretation was wrong, but the first correction also
stopped one step too early: it separated carrier 2's low bits and invented a state field. Treating
the contiguous `23` or `25` bits as one signed value removes that state without moving any
validated IMU boundary.

## Direct-capture result

Across 65 usable captures, all 2,592 decoded prefixes used packing mode `3`. The two carrier-2
low bits exercise all four values: `1` in 1,966 records, `2` in 402, `3` in 130, and `0` in 94.
That distribution is expected for data bits and is not evidence for a four-state machine.

After high-rate precision normalization, the carrier lanes align to the retained
length-`0x1E` carrier with these fixed slopes:

```text
lane 0 = sqrt(2) / 2^24
lane 1 = sqrt(2) / 2^23
lane 2 = sqrt(2) / 2^23
```

Only the length-`0x1E` carrier state needs separate affine windows. Every fitted intercept lies
within `0.001` quarter-`sqrt(2)` units of an integer window in the dynamic validation capture.
Those exact modular windows are what sign-extending/truncating a wider carrier produces; they do
not require a prefix-specific history state.

The earlier constant-offset search was only an approximation. The encoded elapsed field supplies
the missing relationship:

```text
prefix carrier epoch = current tick - encoded elapsed + 4
                     = preceding carrier tick + 4
```

| Capture | Aligned | Correlation | Packet-derived fixed NRMSE | Best constant offset / NRMSE | History decode median / max error |
|---|---:|---:|---:|---:|---:|
| paired pitch | 25 | `0.999996` | `0.002718` | `-3.75` / `0.008728` | `0.000968°` / `0.010508°` |
| retained moving window | 67 | `0.999996` | `0.002771` | `-3.00` / `0.002771` | `0.004682°` / `0.060233°` |

The pitch capture mixes seven-tick high-rate and thirteen-tick normal packets. A single constant
offset cannot represent both, while `preceding + 4` improves its fixed-scale error by `3.21×`.
The moving window is nearly all seven-tick high-rate traffic, where `preceding + 4` is exactly the
same coordinate as `current - 3`; it therefore preserves the prior result.

The retained moving window reports 2,029 global ring drops, but its 69 predecessor comparisons
all match the packet's self-contained elapsed field. The retained window is locally contiguous
and is valid for the gated epoch fit.

The causal decoder uses the most recent length-`0x1E` state and retained components only to choose
the nearest quarter-`sqrt(2)` modular windows. It had zero chart-state mismatches in both dynamic
sets and reproduced the interpolated reference within the angular errors above. This is a real
history decoder, but the reference at `preceding + 4` is still interpolated between surrounding
length-`0x1E` packets. It does not prove the final integer rounding rule.

A later reciprocal lazy-susan capture directly crossed the state-3/state-0 boundary. It established
that those two wire charts are cyclic lane assignments of one continuous three-value carrier and
that the prefix seam selected chart 0 in both directions. See
[`pro2-carrier-chart-transition-2026-07-29.md`](pro2-carrier-chart-transition-2026-07-29.md).

The third-lane correction is structurally independent of the fit: combining the two preceding
bits with the following signed field yields cadence widths that differ by exactly two fractional
bits in all three lanes. The fit then independently confirms the corrected third-lane scale.
Stationary captures remain correctly marked underconstrained instead of being used to choose a
scale.

## Tail temperature positive control

The same offline pass decoded the high-rate/normal tail as two adjacent Q3 temperature samples:

```text
integer = sign_extend_10(tail >> 6)
temperature_a_q3 = integer * 8 + tail[2:0]
temperature_b_q3 = integer * 8 + tail[5:3]
temperature_raw = temperature_q3 / 8
temperature_c = 25 + temperature_q3 / 1024
```

The initial moving-window pass found Q3 values `32..38`, only `0.041` correlation with time/tick,
and no decoded accel/gyro-axis correlation above `0.153`. The decisive evidence was already present
in two independent zero-drop raw/native/raw A/B/A captures:

| Capture | Raw before | Native tail pair | Raw after |
|---|---:|---:|---:|
| `pro2-imuref-raw-native-raw-2026-07-29.jsonl` | `4..5`, mean `4.357` | `4.0..4.625`, mean `4.28125` | `4..5`, mean `4.167` |
| `pro2-imuref-15ms-raw-native-raw-2026-07-29.jsonl` | `4`, mean `4` | `3.25..4.0`, mean `3.951923` | `4`, mean `4` |

The handle-`0x000A` path supplies the ICM's signed raw temperature directly. Both A/B/A runs
independently bracket the Q3 native result, including its fractional resolution. The two low
triplets matched in `993/1023` zero-drop records because adjacent temperature samples usually share
the same fractional eighth; the unequal 3% are expected sample variation rather than padding.

[`dumps/research/ndeadly-switch2-research.json`](../../dumps/research/ndeadly-switch2-research.json)
independently records the raw handle-`0x000A` temperature as signed 16-bit motion data. It also
preserves a July 2025 visual guess that the first two handle-`0x000E` byte pairs were timestamp and
temperature. Direct captures now refine that guess: the first four bytes are
timer/elapsed/status, the payload is non-byte-aligned as the same discussion suspected, and the
temperature pair is the final 16 bits of high-rate/normal payloads. The archive is corroborating
context, not the basis for the decode.

## Conclusion and evidence boundary

The old mismatch was a field-boundary error followed by a false state split and then an
over-simplified constant epoch. The length-`0x28` carrier represents the orientation four sensor
ticks after the preceding carrier, regardless of the current packet's elapsed count.

The prefix is a mode-3, cadence-dependent truncated projection of the length-`0x1E` orientation
carrier. It is **not** an independently decodable smallest-three quaternion:

- the length-`0x1E` chart states are cyclic lane reassignments of one continuous carrier;
- 16 genuine rapid-motion records exceed the strict retained-vector unit constraint, refuting a
  literal strict-smallest-three interpretation of the genuine carrier;
- the modular history decoder is capture-validated, but exact integer rounding has not been proven
  against a simultaneous full-resolution carrier sample;
- catch-up bit 287 is the sole byte-alignment remainder and is zero in all 1,066 catch-up packets
  across 14 repository captures; it is observed reserved-zero padding.

Chart-state coverage was completed later the same day by
[`pro2-carrier-chart-transition-2026-07-29.md`](pro2-carrier-chart-transition-2026-07-29.md):
all four states now have adjacent transition evidence across nine boundaries, and the prefix seam
choice is resolved for each. That document is authoritative for the chart law.

Production DualSense and DualSense Edge motion therefore remains on the hardware-validated
length-`0x1E` carrier. No firmware or UART mutation was made for this result, and this evidence
does not authorize a synthetic length-`0x28` packet.

## Highest-return next experiment

The former prefix-state and constant-epoch experiments are closed, and chart-state coverage is
complete. One target remains:

1. prove exact integer projection/rounding, and require the resulting model to predict held-out
   captures without per-capture tuning.

Only if that offline model leaves two live-distinguishable candidates should a short, passive UART
capture be requested. A new stationary magnet capture would not discriminate the remaining
models.
