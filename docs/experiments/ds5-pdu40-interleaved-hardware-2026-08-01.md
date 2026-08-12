# DualSense length-`0x28` interleaved hardware A/B — 2026-08-01

## Question

After correcting the high-rate gyro lane from eight fractional bits to seven
(`/128`), does the generated interleaved length-`0x28` path match or improve on
the hardware-validated length-`0x1E` DualSense translation?

## Method

- Pico 2 W, 300 MHz production audio profile.
- DualSense connected to the adapter; Splatoon camera left stationary on the
  console.
- Enable the default-off gate with `ds5motion pdu40 on`.
- Record a 12-second desktop timeline while bracketing it with
  `ds5motion pdu40 status`.
- Disable the gate with `ds5motion pdu40 off` and record an eight-second
  stationary control from the same live session.
- Compare the implementation against the retained genuine interleaved stream
  `dumps/BLE CAPTURE/sw2_native_passthrough_live_2026-07-21.jsonl`; do not ask
  for another physical motion capture.

## Hardware result: failed

The corrected gyro scale reduced the prior violence, but motion remained
severely degraded:

- camera jumps began immediately while the controller was stationary;
- jump amplitude grew with time at rest;
- movement reduced but did not remove the jitter;
- yaw was visibly delayed.

Across the recorded interval the diagnostic changed from:

```text
emitted 51708, starved 4481, overlong 1046
```

to:

```text
emitted 53517, starved 4583, overlong 1078
```

That is `+1809` generated packets, `+102` starvation events, and `+32`
overlong windows, with zero acceleration or gyro saturation. One-second video
samples show the camera occupying materially different poses despite no
controller movement. After `ds5motion pdu40 off`, eight consecutive one-second
samples of the proven `0x1E` path remain effectively fixed.

This rejects the generator as hardware-ready. The production gate remains
default off.

## Root cause: two timelines were being interleaved

The failure is not a remaining gyro gain error. The old mixed path violated
two directly captured properties:

1. `ns2_ds5_motion40_t` owned an independent 12-bit tick starting at zero,
   while the interleaved `0x1E` translator continued on its established tick.
   Enabling the gate mid-session therefore alternated incompatible epochs.
2. A generated `0x28` replaced exactly one ~1 ms USB poll, then a freshly
   advancing `0x1E` returned on the next poll. A genuine controller publishes
   one new native PDU near its BLE notification cadence and the USB bridge
   holds that snapshot until the next notification.

The retained moving genuine stream contains 255 consecutive motion PDUs over
1.905 seconds: 185 length-`0x1E` and 70 length-`0x28` (~133.9 total PDU/s).
Every comparable PDU has:

```text
current_tick - previous_PDU_tick == current_encoded_elapsed
```

(`254/254`). This is one shared timeline across both lengths. The stronger
44-file result was already documented as `1274/1274`; calling the interleaved
elapsed relation unresolved was a documentation and implementation error.

## First corrective implementation (offline green, hardware rejected)

The diagnostic scheduler now:

- stores the proven `0x1E` tick beside every source sample;
- derives `0x28.tick` and `0x28.elapsed` from the immediately preceding
  selected PDU rather than a generator-local clock;
- holds one selected native-rate PDU across intervening USB polls;
- rewrites a selected carrier's elapsed nibble to the same shared PDU
  boundary;
- emits three carrier frames followed by one high-rate frame, with the
  `0x28` selected at a seven-tick boundary matching the retained genuine
  high-rate packets;
- falls back to a coherent carrier instead of inventing or repeating missing
  samples.

`test_interleaved_scheduler_shares_one_tick_timeline` pins the shared tick,
elapsed, hold, cadence, and no-starvation invariants.

### Hardware result: shared timeline was necessary but not sufficient

The corrected scheduler was flashed and tested in the same stationary Splatoon
fixture. UART proved that the intended implementation was live:

```text
after 2 s:  79 0x28, 240 carriers, 2221 held polls,
            0 fallback, 0 starved, 0 overlong, 0 saturation
after 10 s: 358 0x28, 1088 carriers, 10140 held polls,
            1 fallback, 1 starved, 0 overlong, 0 saturation
```

The approximately 3:1 carrier/batch ratio, held-poll growth, shared tick, and
near-zero fallback counters are the predicted healthy scheduler signature.
Nevertheless, an eight-second screen timeline showed large repeated camera
rotations while the controller remained stationary. `ds5motion pdu40 off`
immediately returned to a stable six-second length-`0x1E` control. The corrected
scheduler is therefore also hardware-rejected; timing and USB ownership were
real defects, but not the last defect.

### Next defect: one noisy sample did not represent the carrier window

The generated `0x1E` carrier integrates every accepted DualSense IMU sample at
roughly 800 Hz. The high-rate `0x28` generator instead selected one midpoint
gyro sample and published it as the sole vector for the complete seven-tick
window. Passive UART sampling with the gate off measured stationary
`gyro_corrected` standard deviations of approximately `80/50/35` ordinary
counts over 50 observations, while the values averaged close to zero. Picking
one observation therefore discarded the cancellation already present in the
packet's own orientation trajectory. This matches the hardware symptom: rest
was worst, while real movement partly masked the mismatch.

The replacement computes the signed, tick-weighted mean over every
source sample in `(previous PDU, current PDU]`. The weights are the same shared
tick deltas used by the carrier integration and must sum exactly to encoded
elapsed; otherwise the builder fails closed to a carrier. A host test pins a
delayed-source example where the correct weighted mean is 10 counts and the
incorrect record mean is 40.

### Hardware result: weighted gyro was not the missing cause

The weighted build was flashed. Its scheduler remained healthy (no saturation,
near-zero fallback/starvation), but stationary aim still swept through large
rotations. Two UART-only controls then isolated the failure without asking for
another physical motion capture:

1. Forcing all three gyro axes to exactly zero did **not** stop the rotations in
   the interleaved carrier mode. This refutes gyro gain, bias, midpoint choice,
   and window averaging as the primary stationary cause.
2. Repeating the same zero-gyro test with `pdu40 fill empty` was stable for eight
   seconds. A later fixed-rate probe moved the camera, proving the isolated
   `0x28` packets were consumed rather than silently ignored. Mixing the two
   representations exposed a coherence defect.

Changing the preceding length-`0x1E` chart from the Switch 2 carrier to the
state-3 DScale diagnostic did not materially change the failure, so chart-state
selection alone was also not the cause.

### Causal candidate found at the live seam: acceleration was halved twice

`input status` on the flashed DualSense Edge reported stationary acceleration
`[-21, -669, 4059]`: the common input reaching `switch_pro2.c` is already in
the Pro Controller 2's 4096-count/g scale. That is intentional —
`ns2_motion_seam_apply()` converts native DualSense 8192-count/g acceleration
to the output frame and divides it by two.

`ns2_ds5_motion40_build()` nevertheless still assumed it received raw native
DualSense counts and divided by two a second time before applying the high-rate
`/256` wire binary point. Generated `0x28` packets therefore carried about
0.5 g while their interleaved `0x1E` carrier and the physical controller state
carried about 1 g. This is a real representation mismatch and can destabilize
the console's fused history with gyro fixed at zero, but it is **not yet proven
to cause the abrupt multidirectional jumps**. That symptom resembles the old
length-`0x1E` failure caused by a semantically wrong carrier, so prefix/carrier
coherence remains a co-equal fault class until hardware separates them.

The offline synthesizer missed it because its paired fixture feeds pre-seam
DualSense samples directly into the model; its reported synthetic ~1.01 g was
not the value the live firmware emitted. The fix treats the module boundary as
post-seam 4096-count/g and writes `input * 256` directly. The focused host test
now supplies 4096 counts for 1 g, so reinstating the second divide-by-two fails.
Both board builds, 55 compiled host tests, the Python protocol suites, and both
install-reset marker checks pass. Hardware validation is intentionally an A/B
inside one image: `ds5motion pdu40 accel live` sends the corrected 1 g value,
`accel half` recreates the former 0.5 g value, and `accel zero` is diagnostic
only. Switching modes resets only the experimental `0x28` timeline. If `live`
and `half` fail identically, acceleration magnitude is refuted as the primary
cause and the next target is the modular prefix/carrier transition.

### Offline closed-loop correction: physical 1 g still did not match `0x1E`

The first post-seam correction removed the large 0.5 g defect, but a new
sequence-level test caught a smaller mismatch that every per-field test had
missed. The hardware-validated translated `0x1E` path applies its established
output calibration as `source_count * 68963` in the Q16.16 acceleration lane.
Decoded back to ordinary counts, that is a gain of:

```text
68963 / 65536 = 1.0522918701171875
```

The first corrected `0x28` builder instead emitted `source_count * 256`, or
gain 1.0 in its Q8 acceleration lane. It therefore still changed the same
acceleration vector by 5.23% at every `0x1E`/`0x28` boundary. The earlier text
above correctly identified and removed the second divide-by-two, but its
`input * 256` endpoint was not yet cross-representation coherent.

`tools/test_ns2_motion40_coherence.py` now compiles the actual C translators,
drives them with a deterministic 800 Hz physical trajectory, and independently
decodes the resulting mixed stream in Python. It checks one shared PDU clock,
the prefix at its claimed epoch, gyro area against the source and carrier
rotation, both acceleration slots against gravity at their source poses, and
every complete `0x1E -> 0x28 -> 0x1E` transition. Its first run failed on this
exact 5.23% boundary.

LIVE now emits the same calibrated acceleration as `0x1E`:

```text
0x1E Q16.16: source_count * 68963
0x28 Q8:     round((source_count * 68963) / 256)
```

HALF remains the exact former `source_count * 128` double-normalization
control; ZERO remains diagnostic-only. After the correction, all 17 generated
closed loops pass. Maximum errors are 0.000011° at the modular prefix,
0.429 ordinary gyro count, and 0.0019 ordinary acceleration count. Six wire-
level negative controls are all detected: current-carrier-as-past-prefix,
half acceleration, half gyro, swapped gyro X/Y, detached elapsed, and a stale
following carrier.

**Evidence boundary:** this proves the generated representations are mutually
coherent for a known physical trajectory and that the test would catch the
specific recipe failures already encountered. It cannot prove Nintendo's
private filtering or exact console consumption rule because genuine hardware
does not transmit its complete 800 Hz inputs. Hardware validation remains
required, but it is now a justified single A/B rather than another field guess.

### Hardware result: closed-loop-coherent LIVE still rejected

The deliberate real-console A/B rejected that complete recipe. With a
DualSense Edge (`054C:0DF2`) connected and reporting plausible stationary IMU
input, UART explicitly selected `pdu40 accel live` and enabled the interleaved
high-rate gate. The console camera immediately moved chaotically in all
directions, never settled, and did not respond usefully to commanded controller
rotation. Disabling `pdu40` immediately restored the validated `0x1E` path.

The final pre-disable counters were:

```text
emitted=4850 carriers=14671 held_polls=136723
fallback_carriers=56 starved=56 overlong=0
saturated_accel=2 saturated_gyro=0
```

Those counters do not support a transport-collapse explanation. Fallbacks were
about 1.15% of high-rate batches and fall back to the safe carrier; there were
no overlong windows or gyro saturations. Earlier exact-zero-gyro hardware also
rotated, so neither gyro noise nor rate scale explains the stationary failure.

**Result:** the hypothesis that the decoded lanes, shared clock, reconstructed
prefix, and physically coherent IMU history are sufficient for a console-valid
generated `0x28` is refuted. The offline gate remains valuable because it
prevents internally contradictory recipes, but it is necessary rather than
sufficient. Do not request more arbitrary movement captures or tune the known
field scales. The next experiment needs a new observation point that can
isolate Nintendo-private state or filtering semantics, preferably a live
genuine/generated hybrid bisection rather than another full synthetic packet.

## Protocol interpretation

Length-`0x1E` is **not** Switch 1 compatibility. Both lengths are native Switch
2 Pro Controller PDUs:

- `0x1E`: fused orientation carrier plus acceleration;
- `0x28`: cadence-dependent packed IMU samples plus a modular projection of
  the same carrier.

Switch 1 compatibility motion is the separate report-`0x30` protocol. A
length-`0x28` packet can preserve additional sample history, but it is not an
intrinsically newer or more accurate replacement for `0x1E`.

## Disposition — 2026-08-01

The subsequent genuine-base hybrid harness validated byte-identical,
acceleration-only, and gyro-only substitution and showed that the first prefix
failure alternated two orientation histories. A corrected sequence-wide prefix
owner is host/build validated but was not flashed. The maintainer then deferred
translated-`0x28` work: production `0x1E` already behaves correctly, genuine
controllers already pass both native lengths through, and the likely remaining
gain does not justify more physical iterations without a concrete `0x1E`
defect or a materially better observation point. The default-off generator and
harness remain reproducible research infrastructure, not queued product work.
