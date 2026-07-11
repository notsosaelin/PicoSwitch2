## Hardware Testing - 2026-07-10

### Current state

0x05
- Steam detects gyro.
- Pitch/roll appear incorrect.
- Yaw appears mostly correct.

0x09
- Splatoon clearly accepts gyro mode.
- Stationary controller still causes camera movement.
- Console is consuming motion data but interpreting it incorrectly.

### Conclusions

- Motion pipeline exists.
- Problem appears mathematical rather than transport-related.
- Do not continue heuristic filter tuning.
- Focus on coordinate transforms, timing, calibration, and encoder reconstruction.

### Questions

- Does report 0x09 require additional mathematical encoding?
- Is there an IMU encoder or quaternion transform?
- Are we sending raw rate when the console expects integrated orientation?
- How is this repository, https://github.com/TommyWabg/Switch2Connect, parsing switch 2 input differently from ours? And outputting? Is there something worth exploring here?

---

## Resolution (same day, follow-up)

Both findings above were root-caused without new hardware, then fixed and build-verified. Full
writeup: `docs/experiments/gyro-hardware-validation-2026-07-10.md`.

- **0x05 pitch/roll wrong, yaw correct** → the seam's DualSense→Switch axis transform was never
  checked against a real device. Re-mined the genuine controller's own report-0x05 capture
  (`usbpcaptures/genuine_procon_2.pcapng`, its known "still, then pitch/yaw/roll" protocol) to read
  the true axis order off real hardware: X=pitch, Z=yaw, Y=roll. Fixed `ns2_seam.c` (🔵 medium
  confidence — roll's sign is inferred from a rotation-properness constraint, not independently
  measured; a cleaner recapture would close this).
- **0x09 stationary drift** → pure gyro-rate integration with no bias removal; a DualSense's bias,
  unlike the genuine ~0.03 dps, drifts once integrated. Added a stillness-gated per-axis bias tracker
  in `ns2_build_report()` before phase integration.
- Not yet re-tested on hardware — next step is re-flashing and repeating today's Steam + Splatoon test.

The `TommyWabg/Switch2Connect` question is still open (not investigated this pass).

# Hardware Testing — 2026-07-10

## 14:07 — Post-Fix Hardware Validation

### Changes Under Test

Claude root-caused and implemented two fixes based on the previous hardware results.

#### Report `0x05` — Axis Transform

Previous symptom:

* Steam detected live gyro.
* Yaw appeared mostly correct.
* Pitch and roll appeared incorrect.

Claude re-analyzed the genuine Pro Controller 2 USB capture:

`usbpcaptures/genuine_procon_2.pcapng`

The capture used a known motion sequence:

* Still
* Pitch
* Yaw
* Roll

Derived genuine axis convention:

* X = pitch
* Z = yaw
* Y = roll

The DualSense → Switch transform in `src/bt_hid/ns2_seam.c` was changed accordingly.

Current confidence:

🔵 Medium confidence.

The axis order is supported by the capture, but the roll sign was inferred from a determinant-preserving rigid-body transform rather than independently confirmed by a clean isolated roll capture.

#### Report `0x09` — Stationary Drift

Previous symptom:

* Splatoon and Zelda clearly reacted to motion output.
* The camera moved wildly even while the controller was stationary.

Claude's working diagnosis was uncorrected gyro bias accumulating through phase integration.

A stillness-gated per-axis bias tracker was added in `ns2_build_report()` before phase integration.

---

## Hardware Results

### Report `0x05` — Steam

Observed:

* Gyro is working more smoothly.
* Axes now appear correct.
* Controller calibration was required in Steam.
* Calibration had also been attempted during the previous test and did not fix the old behavior.
* After the latest axis-transform fix, calibration now results in usable motion.

Current interpretation:

* The axis-transform fix appears to have materially improved report `0x05`.
* The previous pitch/roll problem appears substantially resolved.
* Steam calibration now having an effect is meaningful evidence that the generated motion is closer to the expected coordinate model.

Do not mark the transform fully confirmed yet.

A clean isolated-axis capture may still be useful to independently validate roll polarity.

---

### Report `0x09` — Switch 2

Tested in:

* Splatoon
* Zelda

Observed:

* Gyro remains wildly unstable.
* Motion is still not usable.
* The stillness-gated bias tracker did not solve the fundamental console-side behavior.

This weakens the previous theory that ordinary DualSense gyro bias accumulation was the primary cause of report `0x09` instability.

Do not continue increasing smoothing or bias correction heuristically without new evidence.

The console is clearly reacting to the motion path, but our generated report `0x09` data is still not mathematically equivalent to genuine controller output.

---

## New Research Leads

### Genuine Pro Controller 2 IMU Capabilities

I have read that the Switch 2 Pro Controller may use a 9-axis IMU.

This is currently unverified.

If true, investigate whether report `0x09` depends on data beyond raw 3-axis gyro and 3-axis accelerometer input, such as:

* Magnetometer data
* Sensor fusion
* Orientation estimation
* Phase or attitude state
* Calibration state
* An encoder using multiple sensor groups

This is a research lead, not a confirmed explanation.

Important existing clue:

The genuine report `0x09` capture contains a second lane group at offsets previously described as `0x15/0x19/0x1D`, which appeared magnetometer-like but was not understood.

Our implementation currently fills uncertain lanes without a proven semantic model.

Reassess whether those fields are actually required for valid console motion interpretation.

---

### Additional Repository to Investigate

Repository:

`https://github.com/Dycool/NS-PC-Control/tree/joycon-usb-experiments`

Investigate specifically for evidence relevant to:

* Switch 2 motion input
* USB motion reports
* IMU encoding
* Sensor fusion
* Magnetometer handling
* 9-axis motion
* Report `0x09`
* Timing or counter reconstruction
* Any mathematical transform between raw sensor values and host-consumed motion

Do not broadly summarize the repository.

Extract only findings that can change or accelerate the current report `0x09` investigation.

---

### Previously Open Repository Lead

The following question remains unresolved:

`https://github.com/TommyWabg/Switch2Connect`

Investigate:

* How Switch 2 input is parsed
* How motion is interpreted
* How output differs from our implementation
* Whether it contains evidence relevant to report `0x09`
* Whether any assumptions in our current implementation conflict with its findings

Again, extract actionable evidence rather than producing a general repository review.

---

## Current Assessment

### Report `0x05`

🟡 Strong evidence of major improvement.

* Live motion works.
* Axis behavior appears correct.
* Steam calibration now produces usable results.

Remaining validation:

* Independently verify roll sign if needed.

### Report `0x09`

🔴 Still unresolved.

The latest hardware test weakens the theory that the primary problem was simply:

* DualSense sensor noise
* Stationary gyro bias
* Lack of smoothing

The remaining problem is more likely related to the actual semantic or mathematical model of genuine report `0x09`.

Priority areas now include:

* Meaning of every motion field
* Second lane-group semantics
* Possible 9-axis input
* Sensor fusion
* Coordinate transforms
* Integration model
* Timing model
* Calibration
* Phase/orientation encoding

---

## Immediate Objective

Do not make another heuristic report `0x09` adjustment yet.

First:

1. Reassess the latest hardware evidence.
2. Determine exactly which previous theories are weakened or eliminated.
3. Investigate the two external repository leads for directly relevant evidence.
4. Revisit the genuine report `0x09` capture, especially all fields currently treated as unknown or approximated.
5. Verify whether the claimed 9-axis IMU is real and whether it changes the interpretation of the second lane group.
6. Identify the smallest experiment or analysis that can distinguish between:

   * raw gyro integration,
   * multi-sensor encoding,
   * sensor fusion/orientation state,
   * incorrect field semantics,
   * incorrect timing reconstruction.

Optimize for recovering the genuine mathematical model.

Do not optimize for making the current incorrect model feel less bad.

---

## Resolution (continuation, same day)

Worked the "Immediate Objective" checklist above without new hardware.

**External repo leads (item 3): investigated, findings folded into docs.**
`Dycool/NS-PC-Control` (`joycon-usb-experiments`) is a legacy Switch-1-format wrapper (fixed
12-byte int16 `MotionReport`) — nothing relevant. `TommyWabg/Switch2Connect` is relevant: it's a
PC-side BLE host for a **genuine** Pro Controller 2, and (a) confirms the real controller streams
raw int16 angular *rate* over BLE, not integrated phase — something downstream always has to
integrate/fuse it; (b) does NOT trust naive integration itself — runs a full Mahony/Madgwick AHRS
(`imufusion`) for its own gyro-mouse feature, gated on "accel near 1g AND gyro actively moving"
(the opposite shape of gate from both our take-1 and take-2 attempts) — a concrete, working
fallback design if take-2's derivative gate turns out insufficient; (c) confirms magnetometer is a
real but *separately negotiated* BLE feature bit (`0x80`) with its own offset range, corroborating
(not contradicting) the report-0x09 magnetometer refutation below. Full detail:
`docs/experiments/gyro-hardware-validation-2026-07-10.md` §7.5.

**9-axis IMU / magnetometer lead (item 5): refuted, not confirmed or denied by new capture —
refuted by arithmetic on evidence already in hand.** The report-0x09 motion block's byte layout
was independently proven (half-variance discriminator + temperature-constant + 800Hz-timing
checks, `report-0x09-motion.md`) to be exactly 30 bytes with **zero spare bytes**: timing(2) +
temp(2) + phase×3(12) + accel×3(12) + tail(2) = 30. There is no room for a magnetometer lane in
this wire format regardless of what the physical IMU chip supports internally. The "second lane
group at 0x15/0x19/0x1D" that originally motivated this lead was from the **already-refuted
int16 model** — those offsets are inside the Q16.16 accel fields' fractional bytes (see "Why the
int16 model looked partially right" in that doc), not a separate sensor. Closed without new
hardware or captures.

**Root cause found for "bias tracker didn't fix 0x09" (items 2, 6):** the take-1 stillness gate
tested raw gyro **magnitude** (`|raw| < 40 LSB`) to decide when to adapt the bias estimate. This
is self-defeating: a MEMS gyro's constant zero-rate bias is *part of* its magnitude reading, and
a DualSense's bias (uncharacterized here, but consumer MEMS commonly runs several dps) can
plausibly exceed 40 LSB (~2.4 dps) on its own — in which case the gate never opens, the bias
estimate never adapts away from its power-on `0`, and the "fix" degrades to exactly the un-fixed
behavior. This reproduces test 2's symptom (zero observable change) exactly. **Fix (take 2):**
gate on the gyro's frame-to-frame *derivative* (steadiness) instead of magnitude — a constant
bias held still has ~zero derivative regardless of its absolute size, so the gate now opens
correctly no matter how large that bias turns out to be. Also added a live debug readout
(`bias=[...] still=0|1` on the config-mode `imu` line) so hardware test 3 can confirm the gate
is actually opening, instead of inferring it indirectly from in-game camera behavior a third time.
Both boards build clean. Full writeup:
`docs/experiments/gyro-hardware-validation-2026-07-10.md` §6-8.

**Not yet done / explicitly deferred:** no new hardware test (division of labor — Claude builds,
user tests); DualSense's actual gyro bias/noise magnitude is still uncharacterized (the new debug
readout can capture it on the next test); an accel-based drift-correction (complementary filter)
model was scoped as the *next* step only if test 3 shows `still=1` but drift persists — not
implemented this pass, to avoid stacking an unvalidated second layer on an unvalidated first fix.

---

# New User Direction — 2026-07-10

## New Evidence and Research Assets

The repository now contains two Pro Controller 2 SPI dump files under `dumps/`.

These dumps have not yet been analyzed. Do not assume they contain executable firmware or that
their contents are directly usable until their structure, provenance, and relationship are
established.

## External Repository Correction

Revisit:

`https://github.com/Dycool/NS-PC-Control/tree/joycon-usb-experiments`

The earlier review may have dismissed this repository too broadly. Inspect the
`joycon-usb-experiments` branch/tree specifically. It reportedly contains Switch-related
Bluetooth, gyro, and wake material.

Do not produce a general repository summary. Extract only concrete evidence relevant to current
Switch 2 protocol gaps, including initialization, wake behavior, motion handling, report formats,
or reusable reverse-engineering techniques. Keep Switch 1 behavior clearly separated from Switch
2 evidence.

## Unverified Motion-Encoding Notes

The following are user-supplied research notes, not confirmed conclusions:

* Report `0x09` motion may involve some form of compressed quaternion plus additional metadata.
* Some fields may not be aligned to 8-bit boundaries.
* Motion-data length may be variable.
* The input report in use may determine the output format; the Switch 2 console may not request
  separate motion modes by default.
* Faithful motion generation may require understanding the complete encoding behavior even if
  every field's physical meaning is not known.
* Partial approximations may appear responsive while still producing unstable or incorrect
  behavior.

These notes challenge any assumption that the current byte-aligned Q16.16 interpretation is a
complete model. They do not disprove that model without discriminating evidence.

## Strategic Direction

Shift the immediate emphasis toward reverse engineering an unresolved protocol feature rather
than continuing heuristic motion tuning. Report `0x09` remains the leading candidate because the
console reacts to it but current output is not faithful.

The SPI dumps may offer a new observation path. A controller-firmware-side implementation would
be especially valuable for recovering the encoding, but do not assume the external SPI contains
the controller's executable firmware. If it is encrypted, compressed, configuration-only, or an
update image, characterize that first.

More invasive firmware-extraction ideas—such as a boot-ROM exploit, fault injection, or a hidden
memory-read command—remain speculative and should not be pursued before evaluating the dumps and
lower-cost observation paths already available.

## Immediate Objective for Claude Code

1. Analyze both files in `dumps/` conservatively: record sizes and hashes, determine whether they
   differ, map populated and erased regions, identify headers, strings, signatures, repeated
   structures, entropy changes, and plausible firmware versus configuration regions.
2. Preserve possible controller-unique or security-sensitive material; do not publish raw keys,
   identifiers, or pairing data in documentation.
3. Inspect the exact `joycon-usb-experiments` branch/tree for actionable Bluetooth, gyro, and wake
   evidence, correcting the earlier conclusion if warranted.
4. Reassess the current report `0x09` model against the unverified variable-length and
   sub-byte-field hypotheses. Identify what existing captures support or contradict each claim.
5. Select the smallest next analysis or experiment that best distinguishes the current Q16.16
   model from a packed quaternion/metadata model.
6. Update durable documentation with confirmed findings and clearly label strong evidence,
   hypotheses, and unknowns. End with a concise hardware test request only if one is needed.

## Constraints

* Do not make another heuristic filter, bias, or smoothing change during this investigation.
* Do not treat the supplied motion-encoding notes as authoritative.
* Do not infer field semantics from host reaction alone.
* Do not assume an external project's Switch 1 behavior applies unchanged to Switch 2.
* Do not write to either SPI dump or expose potentially unique secrets.

---

## Resolution — SPI dump analysis + Dycool re-check (2026-07-10, continuation)

Worked the "Immediate Objective for Claude Code" checklist above. No code changes this pass —
docs-only, per "do not make another heuristic filter/bias/smoothing change during this
investigation."

**1-2. SPI dumps analyzed, conservatively.** Both files: `0x200000` bytes, SHA-256
`bb5c0fb4...9a0fb5` (**identical hash** — the two captures, 4 days apart, are byte-for-byte the
same; nothing on this unit's flash changed with use in that window). Entropy-mapped, hex-dumped at
every offset our firmware already synthesizes (exact match, confirming prior RE), searched for
strings/magics/signatures. **Confirmed:** the motion-cal region (`0x1FC000`) is unprogrammed
(`0xFF`) on this unit — consistent with, not contradicting, existing docs. **New, unrelated to
report 0x09:** a real two-record Bluetooth bond table at `0x1FA000` (raw address/key bytes
deliberately redacted from the written report per the constraint above — see
`docs/experiments/spi-dump-analysis-2026-07-10.md` §3.4), a real battery discharge-curve table at
`0x1FB000`, and a self-describing, correctly length-prefixed `"DSPH"` firmware/coefficient blob at
`0x175000` (almost certainly audio/haptics DSP, not motion — no evidence ties it to the IMU
pipeline). **The bulk of the 2MB image (~1.7MB) is high-entropy (7.9+ bits/byte), consistent with
encrypted/compiled firmware** — no plaintext motion/quaternion logic recoverable this way. Full
writeup: `docs/experiments/spi-dump-analysis-2026-07-10.md`.

**3. Dycool re-check, corrected.** The earlier pass only checked `shared/src/` (legacy Switch-1
motion struct, correctly judged irrelevant) and missed `server/src/` entirely. That directory has
real Switch-2-specific content: `switch2_native.cpp` independently confirms `0x1FC000` as the real
motion-cal write address but for a 72-byte (not 64-byte) payload — an open, unresolved
discrepancy, not a contradiction (folded into `report-0x09-motion.md`). Separately,
`bluetooth_manager.cpp`/`gadget_wakeup.cpp`/`docs/wakeup.md` contain substantial, concrete BT
wake and reconnect-reliability material — off-topic for report 0x09, filed in
`docs/bluetooth/btstack-implementation.md` instead (new sections: "Reconnect reliability" — three
concrete candidate mechanisms for the standing triple-tap bug — and "BLE wake-from-sleep").

**4-5. Reassessed the compressed-quaternion / non-byte-aligned claim.** The non-byte-aligned part
**conflicts with hard evidence already in hand** (the half-variance discriminator's clean split at
exactly the 16-bit boundary, independently on 3 axes and 2 captures, would not happen by chance in
a genuinely bit-packed format) and is not adopted — field *boundaries* stay confirmed. What
survives and sharpens: whether each byte-aligned phase field's *value* is raw integrated rate
(current assumption) or a fused-orientation/quaternion-component representation in that same
slot — this is a semantics question, not a layout question, and is now the sharpest open item.
**Smallest next experiment identified:** hold the controller at a fixed non-zero tilt (not moving)
and check whether the phase value matches a static orientation reading (fusion — value would be
stable and match the tilt) or drifts/has no defined "current orientation" without a known
integration start (pure rate integration). Folded into `report-0x09-motion.md` remaining-unknowns
item 2.

**6. Hardware test request: none new from this pass.** This pass was docs/analysis-only. The
standing ask from the prior session is unchanged and still open: **hardware test 3** — flash the
take-2 derivative-gate fix and re-run Splatoon/Zelda, reading the config-mode `imu` debug line
(`bias=[…] still=…`) with the controller motionless before judging the in-game result. The
fixed-tilt phase-semantics experiment above (item 4-5) is a **second, independent** test worth
running in the same session if test 3 leaves the drift question open.

---

## Resolution — `switch2_input_viewer.py` cross-reference (2026-07-10, continuation)

A new tool, `tools/switch2_input_viewer.py`, was added: a working third-party PyQt/bleak BLE client
that pairs directly with a genuine Switch 2 controller (GATT, not USB) and decodes its reports and
calibration live. Per instruction, `CORTEX.md`/`CORTEX_PARSE.md` were **not** consulted this pass.
No code changes — this was a documentation/cross-reference pass using this tool's *source code*
(read, not run — no hardware/BLE access here) against the SPI dumps already in hand.

**Concrete, high-confidence findings, applied to our own SPI dump:**
- **Factory motion-calibration decoded.** `0x13040` = `temperature` (float32) + `gyro_bias` (3×
  float32); `0x13100` = `magnetometer_bias` (3× float32) + `accelerometer_bias` (3× float32).
  Applied to our dump: temp 27.09, gyro_bias ≈ (-0.021, -0.0006, 0.003) — tiny, matching this repo's
  already-documented "genuine ~0.03 dps" figure — magnetometer_bias = (0,0,0) (never calibrated,
  consistent with the unprogrammed user-cal region), accelerometer_bias ≈ (0.16, -0.07, **10.4**) —
  the Z value sitting close to standard gravity **confirms these are physical SI-unit floats
  (m/s²), not raw ADC counts.** This is genuinely new and previously unknown — the prior SPI-dump
  pass had only checked the *user* cal region (`0x1FC000`, unprogrammed) and missed this
  always-populated *factory* block entirely. Folded into `report-0x09-motion.md`.
- **Feature-flag bits now named**, third-independent-source confirmation: bit2=IMU, bit7=magnetometer,
  matching this repo's own `0x27` capture and `TommyWabg/Switch2Connect`'s `FEATURE_MAGNOMETER=0x80`.
- **Command protocol confirmed identical across USB and BLE** — same `[id][0x91][transport][sub]...`
  shape, same `0x02/0x04` memory-read layout as this repo's own `ns2_dispatch()`. Strong
  cross-validation of the existing protocol model, not just one field.
- **BLE motion reports located in two shapes**: 14 bytes (handle `0x000A`, all types — one raw
  sample) and **40 bytes** (handle `0x000E`, Pro/GCN types) — the first concrete sighting of this
  repo's long-flagged "length-40 variant," though still undecoded (even by this tool's own author).
- **BT bond-table read independently confirmed** field-for-field against the prior SPI-dump
  analysis's structural-only read (same offsets for both host addresses and the LTK, including the
  byte-reversal convention this repo's own pairing code already uses).
- **Reasoning update, not a fact:** USB's high poll rate vs. BLE's lower notification rate gives a
  mundane explanation for why USB carries one "integrated phase" value per report rather than
  multiple raw samples — this *weakens* (doesn't kill) the case that fusion/quaternion encoding is
  *necessary*, and mildly supports this repo's existing rate-integration approach being
  fundamentally the right shape, with the open bug more likely in constants/gate logic than
  architecture. The fixed-tilt experiment (already on the books) remains the way to settle this for
  real.

**Immediate objective / hardware test request: unchanged.** Still hardware test 3 (take-2 gate fix)
as the standing ask; nothing in this pass changes what to test next, only what we understand about
*why* the current model looks the shape it does.

---

## Hardware Debug Results — 2026-07-10 (continuation)

### Test Conditions

Both controllers were connected to the Pico dongle while it was in config mode. The `imu` debug
line was observed for each controller.

### DualSense Observation

Observed output:

```text
imu: has_motion=1 rid=0x9 stream=0 mlen=0 | accel=[0, 674, 4093] gyro=[6, 4, 12] | bias=[0, 0, 0] still=0
```

The displayed sensor values changed substantially over time.

What this establishes:

* The dongle is receiving and exposing live DualSense motion samples.
* At the sampled moment, the derivative-gate instrumentation reported `still=0`.
* The bias estimate remained zero at the sampled moment.

What this does not establish:

* Whether the controller was physically stationary long enough for the stillness gate to open.
* Whether `still` ever changes to `1` during a longer stationary observation.
* Whether the derivative threshold is appropriate for the DualSense's actual sample noise.
* Whether report `0x09` phase semantics are integration, fused orientation, or quaternion-related.

### Genuine Pro Controller 2 Observation

Observed output:

```text
imu: has_motion=0 rid=0x9 stream=0 mlen=0 | accel=[0, 0, 0] gyro=[0, 0, 0] | bias=[0, 0, 0] still=0
```

What this establishes:

* The current config-mode instrumentation did not expose motion samples from the genuine Pro
  Controller 2 through this path.

Do not interpret the zero values as proof that the genuine controller transmitted zero motion.
`has_motion=0` instead indicates that the current pipeline did not recognize or populate a motion
sample, so this output cannot yet be used to compare genuine and generated report `0x09` values.

### Effect on Current Theories

The compressed-quaternion or fused-orientation hypothesis remains unresolved. These debug lines
neither strengthen nor refute it because the genuine controller's motion payload is not reaching
the observable fields, and the DualSense line shows source sensor data rather than proving the
semantics of the encoded report sent to the console.

The DualSense result does raise a narrower implementation question: whether the take-2
derivative-based stillness gate ever opens under real stationary noise. A single `still=0` sample
is insufficient to conclude that it fails.

### Immediate Objective for Claude Code

Prioritize reverse-engineering visibility before changing the report `0x09` mathematical model:

1. Trace why a genuine Pro Controller 2 connected through the tested path reports
   `has_motion=0`, `stream=0`, and `mlen=0` despite `rid=0x9`.
2. Determine whether this is an unsupported parser/transport path, an instrumentation limitation,
   or evidence that the relevant motion data is located elsewhere.
3. Add the minimum instrumentation needed to expose the genuine controller's raw report bytes
   and decoded motion fields without altering controller behavior.
4. For the DualSense, establish whether `still` ever becomes `1` during a controlled stationary
   interval and record the observed derivative/noise range. Adjust no threshold until that data
   is available.
5. Once genuine report values are observable, design the fixed-tilt comparison already identified
   to distinguish integrated rate from stable fused-orientation semantics.

### Constraints

* Do not treat the zeroed genuine-controller debug line as a valid zero-motion sample.
* Do not claim the quaternion hypothesis has gained support from these observations alone.
* Do not make another smoothing, bias, or encoder-model change before establishing what the
  instrumentation is actually observing.
* Keep the next work focused on gaining a discriminating genuine-controller observation path.

---

## Resolution — debug-instrumentation bug found from the hardware debug results above (2026-07-10, continuation)

**Correction:** an earlier pass this session summarized this file's content without reading past
the "Constraints" section above, so it missed the "Hardware Debug Results" data entirely and
wrongly reported "no new hardware test data." That data was real and is processed now.

**Root cause of `bias=[0,0,0] still=0` on both controllers: an instrumentation bug, not a gate
failure.** `stream=0`/`mlen=0` were the tell — those debug fields are only readable in config mode,
and `usb.c`'s main loop takes an unconditional `if (g_usb_config_mode) { config_cdc_task();
continue; }` branch that skips `ns2_task()` — and therefore `ns2_build_report()`, the *only* code
that ran the take-2 bias/stillness tracker — entirely. The tracker's static state was frozen at
its power-on-zero value in the only place it was ever read. The DualSense line's live, changing
`accel`/`gyro` (fed independently by core1's BT stack regardless of USB mode) is what exposed the
mismatch: motion pipeline alive, tracker output frozen. Item 1 of the "Immediate Objective" above
("trace why a genuine Pro Controller 2 reports has_motion=0...") is a separate, already-understood
issue — the `switch2_ble` driver discarding a genuine Pro 2's own motion, a pre-existing documented
gap, not new.

**Fixed:** extracted the tracker (`ns2_motion_tick()`) so it runs whenever `in.has_motion`,
independent of `ns2_imu_enabled`/`ns2_streaming` (those now only gate whether the computed phase
bytes get written into a transmitted report, not whether the tracker itself advances). Added
`ns2_motion_debug_tick()`, called from `usb.c`'s config-mode branch, rate-limited to ~250 Hz to
match real HID cadence. **Practical effect:** the gate is now checkable by connecting a DualSense
in config mode alone — no console, no game needed for this specific check. Both boards build
clean. Full writeup: `docs/experiments/gyro-hardware-validation-2026-07-10.md` §9-10.

**Take-2's actual correctness remains unverified** — this pass fixed the *observability* of the
gate, not the gate itself. Revised immediate ask: connect a DualSense, enter config mode, let it
sit still, and report whether `still` transitions to `1` within a few seconds. That result is a
precondition for the in-game Splatoon/Zelda test (items 4-5 of the prior Immediate Objective) being
informative about the phase math at all — if `still` never reaches `1`, an in-game drift result
can't distinguish "gate broken" from "gate fine, drift is elsewhere," which is exactly the ambiguity
that caused two rounds of inconclusive full hardware tests already.

---

## Resolution — config-mode gate check: CONFIRMED WORKING (2026-07-10, continuation)

**User result:** DualSense — `still=1` when stationary, `still=0` when moving (exactly correct).
Genuine Pro Controller 2 — `still=0` always, even stationary.

**DualSense: the take-2 derivative gate is confirmed working on real hardware.** This is the first
successful hardware validation of any part of the bias-tracking mechanism, across three attempts
(take-1's magnitude gate, take-2's derivative gate whose own readout turned out unreachable, and
now this). The gate is no longer a suspect in the stationary-drift investigation.

**Genuine Pro Controller 2's `still=0` is expected, confirmed by code inspection, not a new bug.**
`switch2_ble.c` (`src/bt_hid/bt/bthid/devices/vendors/nintendo/`) contains zero references to
`has_motion`/`gyro`/`accel`/`motion` anywhere — this driver has never populated motion fields for a
genuine Pro Controller 2. `ns2_motion_debug_tick()`'s `if (in.has_motion)` guard therefore never
runs the tracker for this source, so `still` never updates away from its default. Same
already-documented gap as `STATUS.md`'s "`switch2_ble` discards a genuine Pro 2's own motion,"
confirmed through a new symptom, not a new discovery.

**No code changes this pass** — purely confirmatory. Full writeup:
`docs/experiments/gyro-hardware-validation-2026-07-10.md` §11.

**Immediate objective, revised:** proceed to the in-game Splatoon/Zelda re-test with the current
build (already flashed — same build the config-mode check used). If stationary drift is gone,
take-2 is confirmed and this investigation closes pending the report-0x05 roll-sign item. If drift
persists despite a confirmed-working gate, the bias tracker is ruled out as the (sole) cause and
the next step is the accel-based drift-correction (complementary filter) model already flagged —
not further gate/threshold tuning, which this result rules out as the explanation.

---

## Hardware Symptom Correction and RE Direction — 2026-07-10 (continuation)

### Corrected Hardware Observation

The report `0x09` failure on real Switch 2 hardware should not be described as ordinary drift.

With a DualSense connected through the Pico dongle, both Splatoon and Zelda exhibit abrupt camera
movement or jumping in multiple directions even while the controller is stationary.

This is qualitatively different from a slowly accumulating orientation error. The previous use of
“drift” was misleading and may have biased the investigation toward gyro bias and gradual
integration error.

### What This Changes

The confirmed-working derivative stillness gate does not explain the fundamental symptom. Bias
tracking may still affect long-term stability, but abrupt multidirectional jumps are stronger
evidence of an incorrect report `0x09` representation, field semantic, state transition, scale,
counter/timing relationship, or encoder model.

Do not proceed directly to an accelerometer-based complementary filter merely because that was
previously nominated as the next response to “drift.” That proposal targeted gradual integration
drift and has not been shown to address discontinuous camera jumps.

The corrected symptom does not prove a quaternion model. It does strengthen the strategic case
for decoding genuine report `0x09` behavior before adding further stabilization layers.

### Local Evidence to Reassess

Read:

`docs/experiments/switch2_native_motion_map_DyCOOL.md`

Determine whether its mapped fields, assumptions, or unresolved regions provide evidence for the
jumping symptom or expose a conflict with the current report builder. Separate confirmed layout
facts from DyCOOL-derived interpretations and from this repository's own hypotheses.

### New External Research Leads

#### USB relay and packet-sniffing tooling

Repository:

`https://github.com/Dycool/Usb-relay-for-NS`

Evaluate it specifically as a possible reverse-engineering observation, relay, replay, mutation,
or packet-capture tool. Identify the exact transports and packet paths it can observe and whether
it can expose genuine Switch 2 report `0x09` traffic or enable controlled comparisons. Do not
adopt it merely because it is related to Nintendo Switch USB.

#### Steam controller support

Repository:

`https://github.com/ValveSoftware/steam-devices`

Steam supports Switch 2 controller gyro, so inspect this repository for concrete Switch 2 report
descriptors, device identification, hidraw permissions, driver routing, or references to the code
that actually parses motion. If this repository only contains device metadata or permissions,
follow precise upstream references rather than treating support declarations as protocol
evidence.

### Immediate Objective for Claude Code

Shift the primary effort from bias correction to report `0x09` reverse engineering:

1. Reclassify the real-hardware symptom as abrupt multidirectional jumping, not ordinary drift,
   and update any affected documentation or active hypotheses.
2. Compare `switch2_native_motion_map_DyCOOL.md`, genuine captures, and the current `0x09` builder
   field by field. Identify every generated value that is inferred, approximated, discontinuous,
   or not validated against genuine hardware.
3. Determine which candidate fault classes can produce abrupt stationary jumps: incorrect field
   semantics, scaling/overflow, sign or axis discontinuity, packed state, timing/counter errors,
   invalid initialization, or a missing relationship between fields.
4. Investigate `Dycool/Usb-relay-for-NS` only for actionable capture/replay/mutation capability
   relevant to distinguishing those candidates.
5. Investigate `ValveSoftware/steam-devices` and any concrete upstream parser it identifies for
   Switch 2 gyro evidence.
6. Select the smallest experiment or capture analysis that eliminates the most `0x09` encoding
   hypotheses. Prefer direct comparison of stationary genuine reports against generated reports
   over another mathematical correction by intuition.

### Constraints

* Do not call the observed behavior drift without preserving the corrected jumping symptom.
* Do not implement a complementary filter until evidence shows gradual integration error is the
  remaining failure.
* Do not treat Steam recognizing gyro as proof that its metadata repository contains the parser.
* Do not treat external Switch or Switch 1 tooling as applicable to Switch 2 without verifying the
  report path.
* Do not make another heuristic `0x09` encoder change before identifying a discriminating test.

---

## Resolution — symptom reclassification worked, no encoder change made (2026-07-10, continuation)

Worked all six items of the "Immediate Objective" above; respected every constraint (no
complementary filter, no heuristic `0x09` change, symptom kept as "jumping" throughout, Steam/Switch-1
generalizations avoided).

**1. Symptom reclassified in all affected docs** (`STATUS.md`, `PLAN.md`,
`report-0x09-motion.md`, `gyro-hardware-validation-2026-07-10.md`): abrupt multidirectional jumps
while stationary, not gradual drift.

**2. `switch2_native_motion_map_DyCOOL.md` compared field-by-field against report 0x09.** Point of
agreement: the timing-word structure (low-12 = 800Hz tick, high-4 = elapsed ticks) is identical
between the native BLE format and report 0x09 — a real structural link. Point of tension: native
BLE encodes raw, individually-clamped (±500°/s) gyro *samples*; report 0x09's established model is
an unbounded accumulator. This tension isn't new — it was already report-0x09's lowest-confidence
element before this document arrived — and report 0x09's own prior worked-example evidence (smooth
values, physically-plausible differentiated rates in `report-0x09-motion-analysis.md`) still favors
the accumulator model on its own terms. Verdict: sharpens an existing open question, doesn't
overturn the working model. Full reconciliation: `gyro-hardware-validation-2026-07-10.md` §12.2-12.3.

**3. Candidate fault classes ranked** (§12.5 of that doc): value semantics (open), a discontinuity
in our own phase computation (now directly testable, see item 6), sign/axis permutation (lower
priority — no mechanism for discrete jumps from smooth data), timing/dt edge cases, **a real
self-introduced discrepancy**: this session's earlier `ns2_motion_tick()` refactor made phase
accumulate before `ns2_imu_enabled`, unlike the genuine controller's captured clean startup state —
flagged, not fixed (predates the jumping symptom, so not primary, but worth a deliberate call).

**4. `Dycool/Usb-relay-for-NS` investigated.** Real, promising, unproven (v0): a live USB relay
(genuine Switch 2 ↔ Pi USB gadget ↔ Windows PC ↔ genuine Pro Controller 2, PID `0x2069` confirmed
in its setup script) that could produce repeatable genuine report-0x09 captures — modest hardware
needs, no Cynthion/Packetry required — but the console-side USB gadget classification is explicitly
unverified by its own author, no sample captures exist yet. Filed in `PLAN.md` as the
highest-value RE-capability upgrade if it works, not adopted as ready-to-use.

**5. `ValveSoftware/steam-devices` investigated.** Confirmed dead end: pure udev permission rules,
Switch 2 identifiers present only as access-grant entries, zero parsing logic or byte offsets, no
link to Steam's actual (closed-source) parser. No follow-up warranted.

**6. Smallest discriminating experiment selected and implemented as pure instrumentation** (no
encoder change): `ns2_dbg_motion_phase()` exposes the live `phase[3]` accumulator on the
config-mode `imu` debug line. This directly answers "is the discontinuity in our own math, or in
how the console reads an otherwise-smooth value" — with no console needed, reusing the exact debug
path already validated working for `still`/`bias`. Both boards build clean.

**Immediate ask:** connect a DualSense, enter config mode, let it sit motionless for a minute or
more, and report whether the `imu` line's `phase=[...]` field stays smooth or jumps. That result
picks the next branch: smooth → the bug is in value semantics or console interpretation (fixed-tilt
experiment, or a genuine capture via the Dycool relay tool if it ever works); jumps → the bug is
local to `ns2_motion_tick()`'s own computation (check the `dt_us` clamp and the pre-enable
accumulation flagged in item 3).

---

## Resolution — mathematically-derived anomaly detector implemented (2026-07-10, continuation)

Responded to the debug output showing DualSense `phase=[116423320, 112172745, -2034314527]`
(~9.76°, 9.40°, -170.51°) drifting visibly while `still=1`. Per the explicit task: no filter, bias
correction, coordinate transform, quaternion model, or speculative fix implemented — audit and
instrumentation only.

**Full path audited** (source IMU sample → seam clamp → stillness gate → bias tracker → dt/timing
→ phase integration → enable-state gating → 30-byte serialization), table in
`gyro-hardware-validation-2026-07-10.md` §14.1. One self-introduced discrepancy already flagged
last session (pre-enable phase accumulation) noted, not re-litigated or fixed. No other
discontinuity source found by inspection alone — the open question is empirical, which the new
instrumentation now answers rigorously instead of by eyeballing a slowly-polled snapshot.

**Anomaly bound derived, not chosen** (§14.2): every value feeding a phase increment is
independently bounded — `int16` gyro range, the seam's explicit clamp, EMA convex-combination
bounds on the low-pass and bias trackers (proven by induction), and the `dt_us` clamp already in
the code. Combining the worst provable case of each gives `NS2_MAX_PHASE_DELTA ≈ 763,552,071` raw
units (~64.0°) — the largest single-tick increment this arithmetic can produce without a
computation defect. Deliberately the loosest defensible bound, so a trip is unambiguous.

**Instrumentation implemented** (§14.3): `ns2_motion_tick()` checks every tick against the bound;
on a trip, captures full context (raw gyro/accel, corrected gyro, bias, stillness, `dt_us`,
phase before/after, the tripping delta(s), timing counters, enable state, and the would-be 30-byte
encoding) plus a 4-tick preceding trail, into `ns2_last_anom`. The phase→bytes serialization was
extracted into one shared function (`ns2_encode_motion30()`) used by both the real report and the
capture — not a redundant addition, since the prior `phase=[...]` field (a ~70ms-polled snapshot)
could neither catch a transient single-tick spike reliably nor prove impossibility, only "looks
big." Exposed via a new `imuanom` config command (and `anom=<count>` on the existing `imu` line);
web UI auto-fetches on change. `out[]`/`CFG_TUD_CDC_TX_BUFSIZE` raised 256→1024 to carry it. Both
boards build clean. **No generated-motion behavior changed** — phase integration, the bias
tracker, the stillness gate, and the transmitted bytes are unchanged; only new reads/captures were
added.

**Test procedure and outcome mapping** written (§14.4-14.5): stationary config-mode observation,
watching `anom`. Outcome 2 (continuous phase, discontinuous encoding) is now **structurally ruled
out by code inspection** — one shared serialization function can't diverge from itself — narrowing
the live question to outcome 1 (local defect, would show as `anom` incrementing) vs. outcome 3
(everything on this device is continuous; the discontinuity is elsewhere — console interpretation
or USB transit). **No anomaly has been captured yet** — this is a fresh build, not yet run on
hardware with this instrumentation. No root cause claimed.

---

## Resolution — strategic pivot: genuine-controller BLE reverse-engineering (2026-07-10, continuation)

**The hardware test ran: `anom=0`, console still jumps.** Per instruction, this is read correctly
as narrowing (rules out a local computation defect as *the* explanation) not validating (says
nothing about whether this repo's report-0x09 value semantics — accumulator vs. bounded sample —
are right). No filter, bias correction, coordinate transform, or quaternion/integration model was
implemented or claimed this pass, per the explicit constraint. report-0x09 encoder work is now
**paused without new evidence to act on** — this is a deliberate stop, not an oversight.

**First bounded objective completed:** the named blind spot (`switch2_ble.c` receives genuine
Pro Controller 2 input but discards its motion data — bytes 16-59 of every report, where
third-party decodes place motion) is closed via a new **non-invasive, timestamped raw-capture
facility**, not a parsing fix:
- `src/bt_hid/sw2_capture.c`/`.h` — a lock-free-adjacent (cross-core `critical_section_t`, matching
  `report.c`'s existing pattern) SPSC ring buffer, off by default, that never blocks the BT stack
  (drops and counts on overrun rather than waiting).
- Wired into `btstack_host.c` at every point this host's own code already sees Switch 2 BLE
  traffic: input notifications (full raw bytes, ahead of the existing handle filter), ACK
  notifications (ahead of *its* filter — the pre-existing debug log implies other handles are
  sometimes seen there), every outgoing init/pairing/LED command, both CCC (notify-enable)
  writes, and every `sw2_init_state` transition. Deliberately excludes high-frequency rumble
  writes (would crowd the buffer without adding RE value).
  **No existing parsing, routing, or connection behavior was changed** — only new reads/captures.
- Exported live as NDJSON over the existing config-mode CDC link (`sw2cap on/off/stat` commands),
  draining continuously rather than needing a dump command — suited to multi-minute experiment
  sessions logged by simply capturing serial output.
- Both boards build clean. **No capture has been run on hardware yet.**

**Sources treated as leads, not specifications, per instruction** — `report-0x09-motion-analysis.md`'s
worked example, `switch2_input_viewer.py`, `switch2_native_motion_map_DyCOOL.md`, and
`ndeadly/switch2_controller_research` were each read for what they *specifically* demonstrate
(named function/offset/behavior) vs. what they merely assert or leave unverified, and labeled
accordingly in the new inventory doc's confidence tiers. Two concrete, previously-undocumented
gaps surfaced this way: (1) `switch2_input_viewer.py` shows a **second parallel GATT handle
triple** (`0x000E`/`0x0016`/`0x001E`) this repo's BLE host never subscribes to, alongside the one
it does use (`0x000A`/`0x0014`/`0x001A`) — the richer 40-byte motion format lives on the unused
one; (2) this repo's BLE init sequence **never sends a feature-configure/enable command**
(confirmed by exhaustive grep for `0x0C`-family bytes — none exist), unlike the reference tool's
own init sequence, which does. Neither gap was closed this pass (both require live protocol
changes untestable without hardware access) — both are documented as prioritized, low-risk,
reversible experiments (7 and 8) in the new matrix, explicitly framed as "try as an isolated
manual command," not "add to the permanent init sequence."

**Deliverables** (all in `docs/switch2/ble-controller-protocol-inventory.md`):
1. Field-level inventory — Confirmed (this repo's own code/behavior) / Strong Evidence (named,
   checkable third-party source) / Hypothesis / Unknown.
2. An 11-experiment controlled matrix (stationary, isolated pitch/yaw/roll, fixed tilt,
   known-angle movement, feature-mask replication, handle variants, init capture, reconnect,
   wake), prioritized by cost/value.
3. An explicit table of what BLE evidence can and cannot establish about USB-only report 0x09 —
   the headline conclusion: BLE work is genuinely valuable RE in its own right and can inform
   hypotheses, but cannot substitute for a genuine console-side USB capture.
4. A concrete assessment of `Dycool/Usb-relay-for-NS` (researched earlier this session, written
   up formally here): promising (modest hardware needs, targets the right PID, would yield
   unmodified genuine report-0x09 bytes if it works) but an unproven "v0" with an unverified
   console-side USB gadget layer — the recommended next step if BLE-only progress stalls.

**STATUS.md, PLAN.md updated** to reflect the pivot as the active objective while preserving the
full report-0x09 encoder history as accurate-but-superseded context, not deleted. Unresolved
interpretations (value semantics, whether either open handle-triple carries anything different,
whether feature-negotiation is required over BLE) are stated as explicitly unresolved throughout
— no premature claims.

---

## Resolution — capture facility rebuilt as a web UI panel (2026-07-10, continuation)

`sw2cap on` produced no usable output. Diagnosed and fixed the underlying design, not just the
symptom: the original module auto-streamed NDJSON lines to CDC unprompted, which cannot coexist
with the config web UI's request/response protocol (`sendCmd()` matches replies to requests by
strict arrival order — an unsolicited line arriving between a command and its reply corrupts the
queue). No analysis or byte-inspection was attempted per the explicit instruction not to; this
pass was entirely about making the facility usable.

**Redesigned `src/bt_hid/sw2_capture.c`/`.h`** around a pull-based `sw2_capture_drain_one()` API
(pop-one-entry, called in a bounded loop by the command handler) instead of push-streaming.
Ring buffer deepened 96→256 entries to give a pull-based, Web-Serial-round-trip-latency consumer
more burst headroom. New `sw2cap drain` command returns up to 16 entries plus `capturing`/
`dropped` in one JSON reply — consolidates what would otherwise be a separate `stat` poll.
`out[]`/`CFG_TUD_CDC_TX_BUFSIZE` raised 1024→4096 to fit a full batch.

**Built the requested "Switch 2 BLE Capture" panel** in `web/index.html`: Start/Stop/Clear/
Download NDJSON, a live status pill, captured/dropped counts, per-kind counts, client-side-tallied
observed handles and report lengths (no extra firmware state needed — the browser already sees
every drained entry), a bounded (60-entry) live view with unbounded full-session retention for
export, kind/handle filters (with `0x000A`/`0x000E` one-click buttons) that affect only the live
view, and a clearly-labeled non-semantic highlight over byte offsets 16-29 of `input` entries —
documented in the UI itself as a viewing aid, not a decoded field. Drain is polled every tick of
the existing `pollLoop()` (unconditionally, so status stays live regardless of what
started/stopped a session), integrated as one more step in that single sequential await chain —
deliberately not a second concurrent loop, to avoid racing two independent command streams against
the same strict-FIFO reply matcher. Initial connect now also syncs capture state immediately, so
the page reflects a session already running (e.g. from a prior page load) rather than assuming idle.

**No BLE initialization, feature negotiation, motion decoding, or report-0x09 change was made.**
Both boards build clean; embedded web filesystem regenerated. `docs/switch2/ble-controller-protocol-inventory.md`
§2 rewritten to match (was describing the removed auto-push design); the experiment matrix's
procedure now points at the panel instead of raw-terminal logging. STATUS.md/PLAN.md updated with
a concise pointer to this fix rather than re-narrating the whole pivot.

**Not yet run:** still no genuine BLE capture session exists. The panel is what makes running the
experiment matrix (item 1 of STATUS.md's "Next Recommended Tasks") actually practical.

---

## Resolution — first real BLE captures analyzed; opt-in motion-enable experiment implemented (2026-07-10, continuation)

**The panel worked.** Four full sessions captured through it and supplied for analysis:
`dumps/sw2_capture_2026-07-10-{STILL_CAPTURE,ANGLE1,ANGLE2,ANGLE3}.ndjson` (stationary baseline +
three separate fixed orientations, ~530 s combined, `dropped=0` reported throughout every session
per the user). Per instruction, exact physical axes/signs were **not** inferred from the
filenames — only relative "changed vs. did not change" comparisons were drawn.

**Analyzed completely and programmatically, not from sampled lines**, via two new reusable,
read-only tools: `tools/analyze_sw2_capture.py` (NDJSON integrity, timestamp/cadence validation,
per-offset entropy/variance/transition-rate, between-capture mean comparison) and
`tools/analyze_sw2_fields.py` (tests concrete field-decode hypotheses — the reference tool's
12-bit stick unpack vs. a raw-int16 alternative — plus a ±1-byte alignment-shift test). Source
NDJSON left untouched throughout.

**Integrity:** 0 parse errors, 0 duplicates, 0 non-monotonic timestamps across all four files;
every record is `kind=input, handle=0x000A, len=63`, steady ~33.3 Hz; no `ack`/`cmd_out`/
`ccc_write`/`state` records appear anywhere (these sessions only ran during an already-established
connection, not across a fresh init — item 9 of the experiment matrix remains open).

**Byte-level result: only 8 of 63 offsets ever vary, and every one of them is explained by
something other than orientation.** Offsets 0-1 are a free-running ~33 Hz tick counter. Offsets
10-11/13-14, decoded as the reference tool's 12-bit stick pairs, read near mid-scale and are
**near-identical between the stationary baseline and two of the three orientation captures**
(means match to within 0.1 of 4096) — the signature of an untouched analog stick, not an
orientation field; the third orientation shows a small shift consistent with incidental hand
pressure. Offsets 2 and 31 both drift **monotonically with elapsed session time** (not with which
orientation was held) — a slow secondary counter, not a motion sample. The remaining 55 offsets,
including the entire 16-59 range where a reference tool's *sibling* BLE format places a 14-byte
motion block, are **constant zero in every one of ~23,500 records across four physical
orientations**. A ±1-byte alignment-shift test — run specifically because the task warned against
concluding "no gyro" from one assumed offset convention — produces the *same* few exposed offsets
regardless of which file or shift is checked, i.e. reframing does not surface hidden
orientation-responsive data either. Full table: `docs/switch2/ble-controller-protocol-inventory.md`
§3.5.

**Conclusion against the four posed hypotheses:** "motion present in `0x000A` at a different
offset/encoding" is refuted (all 63 bytes accounted for). "Motion exists but wasn't enabled" and
"this report carries no motion at all" remain indistinguishable from passive observation — and
this repo's own BLE init sequence has never sent the feature-configure/enable command a reference
tool (`switch2_input_viewer.py`) does send, confirmed by exhaustive grep. That ambiguity is exactly
the task's stated trigger condition for building an experiment, not for declaring an answer.

**Implemented: an off-by-default, opt-in, one-shot motion-enable experiment** — the smallest
change that can resolve the remaining ambiguity, per the explicit constraint not to modify report
0x09, add filtering, or assign orientation semantics. `sw2_capture.h` gained
`sw2_set/get_motion_experiment_enabled()`; `btstack_host.c` gained `switch2_run_motion_experiment()`
(subscribes the reference tool's second, **unverified-for-this-device** handle `0x000E`, writes its
CCC at the assumed `0x000F`, sends a `0x0C`-family configure+enable command pair with
`flags=0x07`), hooked to fire exactly once per connection from the existing
`switch2_handle_feedback()` tick, after `SW2_INIT_DONE` — the normal init sequence itself is
untouched. `config.c` gained `sw2cap experiment on|off|stat`; the web panel gained a matching,
unchecked-by-default toggle with an inline warning that it performs real, unverified protocol
actions. Every CCC write, command, ACK, and notification this produces is captured through the
existing `sw2_capture_record()` path exactly like everything else — nothing is decoded, nothing
feeds input routing. Both boards build clean; embedded web filesystem regenerated.
**Implemented, not yet run on hardware.**

`docs/switch2/ble-controller-protocol-inventory.md` updated throughout (§2.6 the experiment, §3.5
the findings, §5 the experiment matrix marked against what's now done, §8 the precise next capture
procedure). STATUS.md/PLAN.md updated to match.

**Immediate ask:** check the web panel's "Experimental: attempt motion-enable (handle 0x000E)"
box, reconnect the genuine controller so the armed experiment fires, capture for ~30+ seconds,
download the NDJSON, and confirm `dropped=0`. Look specifically for any `input` entry on handle
`0x000E` — that single fact (present or absent) is what the next analysis pass needs. Exact steps:
`ble-controller-protocol-inventory.md` §8.

---

## Resolution — experiment run: `0x000E` reachable, still no orientation data; concrete v2 lead found (2026-07-10, continuation)

User supplied `dumps/sw2_capture_2026-07-10-EXPERIMENTAL.ndjson` (2,371 records, 84.46 s) from
running the immediate ask above. Full byte-level analysis performed (ad hoc extension of the
existing per-offset approach, filtered to `handle=0x000E`; source NDJSON untouched).

**The armed experiment fired correctly and produced a real, previously-impossible result.** The
capture contains, for the first time in any session this pass, the full non-`input` traffic: the
normal init sequence (`ccc_write`→`0x001B`, then a **second** `ccc_write`→`0x001B` ~13s later —
i.e. one connection attempt appears to have stalled before a second succeeded, a live data point
for the standing "BT pairing reliability / triple-tap" backlog item, not analyzed further this
pass), `state` 01→03→04→05→06→07→08 (confirming the `READ_INFO→PAIR_STEP1` branch fires on this
reconnect, *not* `READ_LTK`), then the experiment's own `ccc_write`→`0x000F`, `cmd_out` configure
(`0x0c`, subcmd `0x02`, flags `0x07`) and enable (subcmd `0x04`, flags `0x07`), both ACK'd with a
success-shaped response. Immediately after: **2,331 notifications began arriving on `0x000E`** at
the normal ~33 Hz cadence — a handle that produced literally nothing in any of the four earlier
passive captures.

**But the content is not new motion data — it's the same buttons+sticks report, shifted.**
Byte-compared `0x000E`'s 63-byte records against `0x000A`'s 13 records captured in the same
window: the 12-bit-packed stick1/stick2 pair is **byte-identical** between the two (`0x000A`
offsets 10-15 == `0x000E` offsets 5-10, confirmed on sampled records), just living 5 bytes earlier
in `0x000E`'s framing. `0x000E`'s full layout: an 8-bit per-notification counter (offset 0, +1 each
record — a different shape from `0x000A`'s 16-bit +30/record one), a constant tag byte (`0x20`),
two small near-zero bytes of unknown meaning, the shifted stick payload, another constant tag byte
(`0x30`), then **offsets 12-62 constant zero across all 2,331 records** — the identical "nothing
here" result already established for `0x000A`, now demonstrated on the second handle too. Also
notable: the report is 63 bytes, not the reference tool's documented 40-byte "Pro/GCN" length.

**This closes hypothesis 4 as stated (reachability) while opening a sharper one.** `0x000E` isn't
unreachable — it responds normally to the exact command bytes the reference tool uses — but
`configure(0x07)`/`enable(0x07)` alone doesn't turn on anything beyond a duplicate report. Diffed
our command sequence against `tools/switch2_input_viewer.py`'s actual working init flow (read, not
run) and found three concrete, untested differences: (1) it calls `configure_features(0xFF)` — all
8 flag bits, not `0x07` — before six SPI calibration reads (stick cal ×2, user cal, gyro cal,
accel/mag cal, pairing data) that this experiment skipped entirely, then `enable_features(0x07)`;
(2) a **previously-undocumented handle**, raw ATT `0x000D` (three past the `0x000A`/`0x000E` value
handle — resolved from the tool's bleak-indexed `input_handle+3`), gets a write of bytes `85 00`
after `enable`, labeled "report rate" in the tool's own comment — not in this repo's handle map
until now. Any of these three could plausibly be the real trigger; none were exercised, since this
pass deliberately sent only the literal `configure`/`enable` commands per the original task's
"smallest opt-in experiment" instruction.

**No code changes this pass** — analysis and documentation only, per the standing constraint
against modifying report 0x09 or claiming orientation semantics. `docs/switch2/ble-controller-protocol-inventory.md`
updated (new §3.6 with the full findings, experiment-matrix items 7/8 marked done, new item 12 for
the v2 replication, §8 rewritten to describe the as-run procedure). STATUS.md/PLAN.md updated to
match.

**Immediate ask, revised:** implement experiment 12 — a v2 of the opt-in toggle that replicates
the reference tool's complete sequence (`0xFF` configure, the six cal reads in order, `0x07`
enable, then the `0x000D` write) — then repeat the same arm/power-cycle/capture/download procedure.
Not yet built; asked the user whether to proceed before writing another live-hardware protocol
experiment.

---

## Resolution — v2 experiment matrix + GATT discovery tool implemented (2026-07-10, continuation)

User approved building v2, with explicit constraints: don't combine the three untested differences
(mask value, calibration reads, the extra handle write) into one opaque sequence — isolate each as
its own selectable variant; resolve the "0x000D" handle numbering exactly, mapping documented vs.
Bleak-visible vs. BTstack-visible vs. characteristic-value vs. CCC-descriptor handles, before
writing any code that depends on it; six specific variants (control / mask-only / handle-write-only
/ mask+write / calibration-sequence / full-sequence); each variant must log its identifier,
complete ordered operation sequence, exact bytes, target handle, ACK/reply bytes, and timing; a
fresh reconnect required between variants; make the active variant visible in the panel and NDJSON;
do not decode motion, do not modify report 0x09/normal init/synthesized gyro; do not record any
variant successful until hardware demonstrates motion-correlated bytes.

**Handle-numbering resolution done first, as instructed — and it caught a real mistake.**
Re-read `switch2_input_viewer.py`'s exact handle usage (three independent `X - 1` call sites plus
one inline literal `write_gatt_char(0x0005 - 1, ...)`, cross-checked against this repo's own three
independently-confirmed value handles) and concluded `bleak_handle = documented_value_handle - 1`
consistently — i.e. bleak reports a characteristic's *declaration* handle, not its value handle
(GATT guarantees declaration = value − 1, always). Applying that properly to the "write
`input_handle+3`" step (previously concluded to be raw handle `0x000D` in the prior pass's
analysis) revealed that conclusion was wrong: it had applied the same `-1`/`+1` correction to a
*descriptor* write, which isn't subject to the declaration/value split that motivates the
correction for characteristics in the first place. Redone correctly: given confirmed
`0x0009=decl(0x000A)`, `0x000A=value`, `0x000B=CCC`, and v1-confirmed `0x000E=value`/`0x000F=CCC`,
`0x000E`'s declaration must be `0x000D` (GATT-mandatory), leaving `0x000C` — not `0x000D` — as the
only unassigned handle the write could target, and most likely a third descriptor on `0x000A`
(not `0x000E`). Documented as a corrected, explicitly-labeled hypothesis, not fact — and, since
it's still reasoning on paper, backed by a genuinely new instrument: a one-shot **GATT discovery
tool** (`sw2cap gattdisc on`) that walks BTstack's own live service→characteristic→descriptor
discovery on the real connection and captures every handle+UUID through the existing NDJSON
pipeline (three new capture kinds: `gatt_svc`/`gatt_char`/`gatt_desc`) — ground truth, not another
arithmetic pass. This is exactly the kind of durable RE infrastructure CLAUDE.md asks to prioritize
over one-off answers.

**v2 variant matrix implemented as a single table-driven state machine** (`btstack_host.c`): a
`sw2_v2_variant_t` struct (configure/enable flag values, whether to insert the six calibration
reads, whether to add the handle write, whether to defer the CCC subscribe to the end) drives one
shared sequential state machine instead of six near-duplicate functions. Variant 6 (full sequence)
uniquely defers the `0x000E` CCC subscribe until after configure/calibration/enable/handle-write —
found while re-reading the reference tool's connection code for this task: it doesn't call
`start_notify()` for input reports until *after* all of that setup, an ordering difference the
original v1 experiment and DATA.md's variants 1-5 don't share, and which DATA.md's own phrasing
("exact ordering of subscriptions... [for the] full reference sequence") specifically called out
as belonging only to the full-sequence variant. SPI calibration-read commands are built via a
`switch2_build_spi_read_cmd()` helper that reproduces `switch2_input_viewer.py`'s
`read_spi_memory()` byte layout exactly (verified against its source, not re-derived). Per-variant
logging requirements are satisfied by the existing capture pipeline without new machinery: a new
`variant` capture kind marks which one is running, and the ordered operation sequence, exact bytes,
target handles, ACK bytes, and timing between steps are all already implicit in the timestamped
`cmd_out`/`ccc_write`/`ack` entries every step already produces. A fresh reconnect between variants
falls out of the existing fired-once-per-connection guard (reused from v1) without extra code. The
old `sw2cap experiment on/off/stat` command and its single checkbox were replaced by
`sw2cap variant <0-6>`/`variant stat` and `sw2cap gattdisc on/off/stat`, with matching web-panel
controls (a variant dropdown + a separate GATT-discovery checkbox, both off by default, both with
status pills) and a `0x000C` quick-filter button alongside the existing `0x000A`/`0x000E` ones.

**Both boards build clean; web filesystem regenerated.** No hardware run yet, per the explicit
constraint not to record any variant as successful without one.
`docs/switch2/ble-controller-protocol-inventory.md` updated: §2.6 marked superseded-but-historical,
new §2.7 (v2 design), new §3.7 (the handle-numbering resolution, written up including the
correction of the prior pass's mistake), §5's old item 12 replaced with 12a (GATT discovery) +
12b-12g (the six variants), and a new §9 with the DATA.md-specified hardware run order (control →
mask-only → handle-write-only → mask+write → full-sequence → calibration-only, the last
conditional on the others' results, GATT discovery run alone first). STATUS.md/PLAN.md updated to
match.

**Immediate ask:** run §9's procedure in order, starting with `sw2cap gattdisc on` alone to confirm
or correct the `0x000C` hypothesis, then variants 1→2→3→4→6→(5 if needed), power-cycling the
controller between each and downloading/checking `dropped=0` after every capture.

---

## Resolution — v2 hardware run analyzed: independent motion-consistent data found on every variant (2026-07-10, continuation)

User supplied the complete 8-file v2 capture set (GATT discovery + all 6 variants + a fresh
baseline). Per the task's explicit scope, this pass was analysis-and-documentation only — no new
experiment implemented, no init/report-0x09/filter/motion-encoding code touched — and built a new
reusable tool rather than doing an undocumented one-off: `tools/analyze_sw2_v2_captures.py`
(integrity + full ordered timeline reconstruction for every file, GATT-hierarchy reconstruction
from raw discovery records, per-offset byte statistics, cross-handle/cross-variant equality
checks), followed by targeted ad hoc `python3 -c` checks once the tool's output identified
specific questions worth answering precisely (drift rates, byte-boundary transition signatures,
structural byte constants). Full write-up, with the causal table and hypothesis assessment DATA.md
specified: `docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md`.

**GATT discovery gave real ground truth for `0x000C`, confirming the pre-hardware paper
correction.** `0x000C` is a vendor-specific descriptor (UUID `679d5510-...`) of the `0x000A`
characteristic (declaration `0x0009`) — not `0x000E`'s, and not the originally-guessed `0x000D`.
`0x000E` turns out to have its own identical-role descriptor at `0x0010`, never tested. The
discovery capture also surfaced several previously-undocumented characteristics
(`0x0018`/`0x0022`/`0x0026`/`0x002A`/`0x002C`/`0x002E`/`0x0032`) not present in this repo's own
handle map or the reference tool's static constants — flagged as a lead, not investigated further
this pass (explicit scope: exhaust the current capture set first).

**Design-fidelity check: every variant executed exactly as its table specifies, no exceptions.**
Cross-checked cmd bytes, flag values, calibration-read addresses/sizes/order (after fixing a false
positive in the analysis tool itself — the normal init sequence's `READ_INFO` SPI read shares the
same cmd/subcmd bytes as a calibration read and was initially miscounted as one), the handle-write,
and CCC-subscribe timing (confirming variant 6's deferred-to-last design) against
`SW2_V2_VARIANTS[]`. All ACKs success-shaped, no rejections observed. **One real instrumentation
gap found**: the CCC-write and handle-write completion callbacks in `btstack_host.c` only
`printf()` their ATT status, never `sw2_capture_record()` it — so whether `0x000C`'s write was
itself accepted or rejected is not recoverable from these capture files, only its downstream effect
is observable. Documented as a fix for later, not implemented this pass.

**The major result: every one of the six variants — including the plain control — produced a real,
independent 40-byte data block on `0x000E` that was never present in any earlier capture.**
Offsets 14-54 activate within under a second of the feature-enable sequence completing, in all six
files, and stay active for the rest of each session. This satisfies every criterion the task set
for "independent payload data," checked directly rather than assumed: absent from baseline (0/2,331
records active in the original standalone v1 capture, re-verified with a full scan this pass);
independent of buttons/sticks (`0x000A`'s own varying offsets stay at their usual boring 7-8 in
every one of these same files); not a counter (byte-boundary transition-rate analysis of a
candidate 16-bit field shows the classic low-byte/high-byte incrementing signature, not random
noise, and is structurally distinct from the known counter at offset 0); not the already-known
shifted stick-duplicate (that duplicate, at offsets 5-10, is separately re-confirmed still present
and unchanged in the same files — the new activity is a disjoint byte range). Beyond the task's own
checklist, three additional lines of evidence: consecutive samples of several offset-pairs show
smooth, monotonic, non-random drift (not the jump-around pattern noise or a second counter would
produce); the drift rate varies in both magnitude *and sign* across the six independent
connections (arguing against a fixed protocol artifact) while one field's rate stayed positive and
in a tight 313-373 counts/s band across all six (a bias-like signature); and offset 14 is a
constant byte reading exactly 40 (decimal) — a self-describing length prefix for the 40-byte span
that follows it, matching the reference tool's documented "Pro/GCN 40-byte motion block" length
for the first time on real hardware rather than only in that tool's static source.

**The central open question, stated precisely per the task's causal-table requirement**: because
*every* variant succeeded — including the control, whose commands are byte-identical to the
original standalone v1 experiment that produced zero activity across 70+ seconds — **none of the
six deliberately-tested differences (feature-mask value, calibration reads, the handle write,
subscribe-timing) is the actual cause**. All of the task's posed hypotheses ("0xFF mask is
sufficient," "the `0x000C` write is sufficient," etc.) are technically true in isolation but none
is *necessary*, since the untouched control alone reproduces the full result. The real
differentiator between the two sessions lies outside the six-variant matrix — ranked candidate
explanations (session/bond-state persistence being the best-supported by the timing pattern;
incidental physical handling being plausible but weaker, given 100%-reproducible near-instant
activation across six independent connections; several other candidates from the task's own list
noted as neither supported nor refuted by current evidence) are in the full report §6, explicitly
labeled by confidence, not asserted as fact.

**No orientation/gyro/accelerometer/quaternion semantics were assigned to any byte**, per the
task's explicit constraint — the report states only what's measurable (presence, independence,
drift behavior, structural framing) and is careful to say what it does *not* establish.

**Docs updated**: `docs/switch2/ble-controller-protocol-inventory.md` (new §3.8 summarizing the
result and linking the full report, §5's matrix rows 12a/12b-12g marked done, a new §10 with the
proposed follow-up procedure, §7 updated to reflect the run happened), STATUS.md, PLAN.md.

**Immediate ask (the single highest-information follow-up, proposed not implemented):** re-arm
variant 1 — already implemented, already proven sufficient, no new code needed — and capture again
with the controller held genuinely motionless for the entire session, explicitly re-stating the
discipline the original standalone v1 test's procedure had but this six-variant testing round's
procedure didn't repeat as explicitly. If the block still activates and drifts while motionless,
that argues for a session/bond-state explanation (and against reading the drift as tracking real
physical motion); if it stays inactive or near-zero, that argues physical handling during this
round's captures was real and shifts the next step toward a controlled, deliberate-motion protocol
instead.

---

## Resolution — block interpretation + a measured timing-difference candidate; controlled-motion protocol designed (2026-07-10, continuation)

User clarified, without needing a new capture: variant 1 (and all six variants) were already held
genuinely motionless throughout, paired and captured while stationary. This directly answered the
prior turn's proposed follow-up in place — the block's drift is confirmed present under
stationary conditions — and per explicit strategic direction, shifted this pass's priority from
"what triggers the block" (deprioritized, not dropped) to "what does the block encode," while
keeping report 0x09, normal BLE init, and the v2 variants' own protocol behavior untouched.

**Block interpretation, exhaustively, before picking any semantics.** Built a new companion tool,
`tools/analyze_sw2_v2_captures.py` → superseded for this purpose by a purpose-built
`tools/analyze_sw2_motion_block.py` (per-byte stats, an exhaustive scan of every alignment × width
{1,2,3,4} × endianness × signedness combination inside the 40-byte block, derivative/wraparound
analysis, cross-field correlation, a repeated-lane/interleave check, and a timing-relationship
check, run across all six variants' full active datasets). Two fields survive as genuinely
accumulator-like, ranked by objective linear-fit/monotonicity scores rather than by eyeballing:
raw offsets `0x13`-`0x14` (int16 LE) — found, on closer inspection prompted by a suspicious
near-zero full-session R² that contradicted an earlier near-perfect short-window R², to wrap
through the entire signed 16-bit range roughly every 4 seconds (~22-23 wraps across ~2,700-2,950
records) — a clean, textbook two's-complement wraparound signature, structurally matching this
project's own report-0x09 phase-accumulator architecture (small constant bias, integrated). Raw
offsets `0x19`-`0x1A` drift smoothly across the *entire* uncapped session with no wraparound at
all, at a much slower rate. A separate, non-drifting pair (raw `0x26`/`0x31`) shows a consistent
+0.44 to +0.55 correlation across all six independent variant sessions — flagged as structurally
linked, not yet explained. No interleaved-lane structure was found at 2/4/5/8-way splits. No
semantics were assigned to any byte — the report states only what would distinguish rate-like from
orientation-like from counter-like behavior, per the task's own outcome-mapping framework, deferred
to the controlled-motion capture.

**Enumerating every non-variant difference between the two sessions turned up a real, measured
one, not just hypotheses.** Per the task's explicit list (firmware build, subscription timing,
capture start time, reconnect-vs-power-cycle, bond/session persistence, config-mode entry order,
physical handling, common v2 init/instrumentation behavior), most were inconclusive or ruled out
(physical handling, directly, by the user's clarification) — but comparing the exact operation
timestamps between the old standalone v1 capture and this session's variant 1 found the old code
fired its CCC-subscribe write, `configure` command, and `enable` command within **1.2ms of each
other**, never waiting for the CCC write's own ATT completion or for `configure`'s application-layer
ACK — the device's `configure` ACK didn't arrive until **~39ms after `enable` had already been
sent**. The current v2 state machine, by design (built two turns ago, not this pass), explicitly
waits for each step's real confirmation before advancing. This is a verified fact about this
repo's own two code versions across the two sessions, not speculation about the controller — the
leading, best-evidenced candidate explanation for the session-to-session difference, though not
yet proven causal. Per the task's explicit instruction not to claim a trigger without one, this is
recorded as the leading hypothesis with a proposed (not implemented) one-variable confirmation
experiment — a future "variant 7" reproducing the old unsequenced timing on otherwise-identical
bytes — for later, separate from today's actionable work.

**Implemented, both boards build clean**: a capture-annotation marker mechanism
(`sw2_capture_mark()`/`sw2cap mark <text>`/a new web-panel text input + button, rendering the
label text directly in the live view rather than raw hex) — pure logging, calls no BTstack API at
all, satisfying the explicit constraint that a marker must not alter controller initialization or
report behavior; and a fix for a real instrumentation gap found analyzing the prior hardware run
(the CCC-write and handle-write completion callbacks only ever `printf()`'d their ATT status,
never captured it) — one `sw2_capture_record()` call added to each, placed after the status was
already computed and before any existing control flow, verified by inspection (not a new hardware
run) to change nothing about either callback's timing or behavior, satisfying the task's
"only if it can be done without changing their timing or behavior" constraint.

**Designed the controlled hardware experiment**, using existing variant 1 unchanged, one continuous
connection, the new marker mechanism to mark phase boundaries: stationary baseline (≥30s) → several
isolated positive/negative pitch rotations → fixed pitch hold (≥20s) → return to baseline → repeat
for yaw → repeat for roll → final stationary baseline, exact marker label strings specified for
each phase. Outcome mapping defined before the test (rate-like / orientation-like / acceleration-
like / timing-like / composite), per the task's own framework, applied to Fields A/B once the
capture exists — not before. Not yet run.

**Docs updated**: `docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md` extended with
§8 (block interpretation), §9 (differential comparison with exact timestamps), §10 (experiment
design), §11 (marker/instrumentation-fix implementation), §12 (exact browser procedure); §6
rewritten to reflect the stationary clarification and the new timing-difference candidate. Inventory
doc gained §3.9. STATUS.md/PLAN.md updated to match.

**Immediate ask:** run §12's exact procedure — variant 1, one continuous connection, marked
pitch/yaw/roll sequence per the physical protocol above — and supply
`sw2_capture_2026-07-10_MOTION-CLASSIFICATION.ndjson`. That is the one file needed to classify
Fields A and B (and the correlated pair) against the outcome mapping; no other capture is required
to proceed.

---

## Resolution — controlled-motion capture analyzed: block-wide activity signature found; direction test negative (2026-07-10, continuation)

User ran the procedure and supplied `sw2_capture_2026-07-10_MOTION-CLASSIFICATION.ndjson` (24,158
records, 0 parse errors, all 44 phase markers present and cleanly paired), plus the two remaining
inputs the procedure asked for outside the file itself: positive = left (all three axes), resting
orientation flat on a table face up.

**Direction-correlation test on Fields A/B (the two candidates flagged in the passive-data pass):
negative.** Computed per-phase linear-fit slope for both fields across all 22 marked phases.
Neither shows a slope that reproducibly tracks the labeled `_pos`/`_neg` direction — repeated
`pitch_pos` reps produced wildly different Field A slopes (`+23985.7`, `+37539.3`, `+819.5`
counts/s), and slopes during confirmed-stationary `baseline1` were comparable in magnitude to
slopes during active rotation. Broadened to a systematic scan (Cohen's-d effect size between all
`_pos` and all `_neg` phase slopes) across all 39 possible 2-byte-LE start offsets in the block —
the best candidate (raw `0x25`, d≈1.52) has effect-size magnitude plausibly explained by chance
given the number of offsets tested and the small per-group sample count, and its own within-group
scatter is enormous. **This refutes "Field A/B is a clean rate-like accumulator readable at this
offset/width/endianness" as tested** — it does not refute the block being motion-related some
other way, which the next check found.

**Reframed the question — not "does a field's value track direction" but "does the block's
behavior distinguish genuine stillness from handling at all" — and found a strong, reproducible,
cross-validated positive result.** Computed residual noise (deviation from a local linear trend)
separately for confirmed-stationary phases vs. any handling phase, across every 2-byte offset in
the block. Found the effect at **eleven distinct byte offsets spanning nearly the entire 40-byte
width**, not one lucky position — active-phase residual noise exceeds baseline by 100-1000× at
each. Cross-validated the cleanest example (offset `0x1e`) with a second, independent, much
simpler metric (raw byte-level transition rate): frozen 99.7% of the time at genuine rest, but
changing on 70.0% of records during active rotation and 36.6% during the nominally-still `_hold`
phases. The `_hold` phases' inconsistent, intermediate, per-offset-and-per-axis-varying noise
levels (not uniform, as a fixed motion/still flag would produce) read as a genuine tell: this
looks like variable involuntary hand tremor while holding an awkward tilt for 20-40 seconds, not
a discrete state bit.

**Classified honestly against the pre-defined outcome mapping rather than forcing a fit**: doesn't
match rate-like (§ direction test, negative), orientation-/gravity-like (holds don't settle to a
stable level), simple acceleration-like (elevated noise persists for the whole phase duration, not
just at boundaries), or timing/counter-like (behavior isn't uniform regardless of motion). Recorded
as a new, empirically-derived sixth category: **activity/vibration-responsive noise floor** —
consistent with raw, unfiltered/unintegrated sensor samples sensitive enough to alias real
mechanical vibration into large sample-to-sample swings, stated explicitly as a *structural*
characterization, not a semantic one. **No gyro/accelerometer/axis/unit/scale semantics were
assigned to any byte** — the report is explicit about what this does and doesn't establish (§13.5),
per the standing constraint.

**No code changes this pass** — analysis and documentation only, continuing the prior pass's
tooling (ad hoc `python3` analysis using the same block-framing already established;
`tools/analyze_sw2_motion_block.py` was not modified, since its exhaustive scan infrastructure was
reusable as-is via direct interactive queries against the new file). `report 0x09`, normal BLE
init, and the v2 variants' protocol behavior remain untouched.

**Docs updated**: `docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md` gained §13 (full
results) and an updated top-of-file status line; inventory doc gained §3.10; STATUS.md/PLAN.md
updated to match, including a revised "Next Recommended Tasks" item.

**Immediate ask, revised:** the proposed next step is a hand-held-but-not-moved baseline — pick the
controller up and hold it still (no deliberate rotation) for ~30 seconds under variant 1, compare
its noise floor against the existing table-flat baseline and active-rotation data already in hand.
This isolates "responds to any physical handling" from "responds to rotation specifically" using
only data already captured plus one cheap new capture — no new tooling or firmware needed.

---

## Resolution — structural decomposition against ICM-42670-P FIFO packet formats: no candidate survives; mechanically-supported experiment proposed (2026-07-10, continuation)

User redirected before the hand-held test above: don't request it yet, since the existing
controlled-motion capture's fixed-hold phases already show tremor-consistent intermediate
activity, and a hand-held-still capture would likely just reproduce that without distinguishing
remaining hypotheses. Explicit instruction: pause new hardware, investigate the block's *internal*
packet structure more deeply first — the negative direction-correlation result only refutes
clean byte-aligned scalar interpretations, not directional gyro/accel data packed some other way
(sub-byte fields, multiple samples per notification, delta-coding, shared high bits, native IMU
FIFO packets). Specifically asked to test whether 40 bytes decomposes as two 20-byte
ICM-42670-P-style high-resolution FIFO packets (the controller's IMU, per this repo's existing
teardown-sourced identification), or another documented exact multiple.

**Built and ran a new structural-analysis tool**, `tools/analyze_sw2_block_structure.py`
(framing re-check, an entropy-profile periodicity test across candidate packet sizes, a sub-byte
header-bit scan, and a physically-grounded orientation-invariant vector-magnitude scan) — kept as
permanent, reusable RE tooling per the project's standing "build infrastructure, not one-off
answers" philosophy, even though (per below) it didn't resolve the block's structure this pass.

**Stated my own confidence honestly before testing anything**: the specific FIFO byte-level
layouts I could test come from general/trained familiarity with the shared ICM-42xxx family's
well-documented packet architecture (accel XYZ + gyro XYZ + temp + timestamp, plus a documented
"high-resolution" 20-byte extension), not from directly consulting the ICM-42670-P datasheet
itself — no internet/document-fetch access exists in this session. Said explicitly, before
reporting results, so a negative finding reads correctly as "the layout I could test didn't
match," not "no documented layout could ever match."

**Periodicity test: negative, cleanly.** An entropy-profile self-correlation test across every
candidate packet period (2, 4, 5, 8, 10, 16, 20 bytes) — the signature a genuine repeating packet
with a stable header field would leave — found no meaningful positive correlation at *any* tested
period (all near zero or negative, best was `P=20` at only `+0.189`). A supplementary check for
header bits hiding in the TOP 2-3 bits of otherwise-busy bytes (a real pattern in some FIFO
conventions) found a few low-entropy candidates, but none recur at a consistent spacing from each
other, so they don't rescue any tested layout either. This refutes 2×20, 4×10, 5×8, and 8×5 as
tested, under my reconstructed layout's confidence level.

**Magnitude-stability test: a promising-looking result, explicitly checked and downgraded.**
Independent of any assumed byte map: scanned every 3-consecutive-int16 window for whether its
vector magnitude stays orientation-invariant (as a real accelerometer's must) between confirmed
rest and the fixed-tilt hold phases. The top candidate (raw offset `0x10`, big-endian) showed a
striking match — ratios of `1.01`/`1.01` between baseline and both holds. Rather than accept this
at face value, checked it against the block's already-known structure and found its Z-component
byte sits adjacent to Field A (the confirmed wrapping accumulator from the prior pass) — a slowly
drifting quantity sampled over multi-second windows scattered across an ~800-second session will
tend to show similar *typical* magnitudes regardless of physical orientation, for purely
statistical reasons unrelated to gravity. No candidate was both a strong match and clearly free of
overlap with an already-confirmed non-physical byte. Recorded as inconclusive, not confirmed — a
direct application of the task's "do not label a layout correct because its numbers look
plausible" instruction, applied against my own most tempting result.

**Net result**: no documented periodic FIFO packet layout survives structural testing at any of
the tested spacings, under the confidence level available this session. The block-wide activity/
noise-floor signature from the prior pass remains the strongest evidence that genuine sensor data
is present — that finding is unaffected by this pass's negative results, since it never depended
on any specific byte-level decode. What's now additionally confirmed is that the block's *internal
packing* doesn't match the specific FIFO structures tested, narrowing (not closing) the hypothesis
space: firmware repacking before BLE transmission, delta-coding, or an untested bit-packing
convention remain open, alongside the possibility that my reconstructed layout itself needs
correcting against the actual datasheet.

**Proposed, not requested, a mechanically-supported follow-up** per the explicit instruction to
prefer non-handheld tests: table taps and a vibration source with zero skin contact, versus a
brief-contact control, testing two named competing models (genuine IMU/vibration-driven vs. a
non-inertial handling artifact such as capacitive or thermal contact sensing) with expected
results defined per model *before* any capture, per the task's explicit requirement. This cleanly
removes the hand-tremor confound that made the existing `_hold` phases ambiguous.

**Trigger investigation kept explicitly separate**, per instruction not to mix the two threads —
§6/§9's ACK-gating timing candidate and its proposed one-variable confirmation experiment are
untouched this pass, recorded as still-deferred in a new §15.

**No firmware/BLE/report-0x09/capture-behavior changes this pass** — analysis, one new reusable
tool, and documentation only, per the explicit constraint. Docs updated: the v2 motion-block report
gained §14 (structural decomposition) and §15 (trigger-deferral note); inventory doc gained §3.11;
STATUS.md/PLAN.md updated, including the "Next Recommended Tasks" item.

**Immediate ask, revised again:** the mechanically-supported experiment above (report §14.6) is
proposed, not requested yet — presented as the defined next step rather than an immediate ask, per
this turn's task structure ("identify the smallest supported-motion experiment... only request the
subset needed").

---

## Resolution — BLE block decoding paused; re-centered on genuine USB report 0x09 via a full `Dycool/Usb-relay-for-NS` feasibility audit (2026-07-10, continuation)

User called a stop: no more table taps, vibration tests, hand-contact controls, or further
physical experiments against the opaque BLE 40-byte block. Three full passes (direction
correlation, orientation-invariant vector interpretation, periodic native-FIFO packet structure)
had already exhausted what passive statistics against one dataset could establish — continuing
would mean inventing another indirect physical test purely because the prior ones failed, not
because new information would result. Explicit instruction: record the narrow, durable conclusion
(independently framed; responds strongly to physical activity; internal encoding/semantics
unknown; no tested interpretation validated), preserve captures/tooling, pause semantic decoding of
this specific block — but **not** gyro or Switch 2 RE as a whole — and re-center on the project's
actual target: reproducing genuine console-side USB report 0x09, starting with an
implementation-level feasibility audit of `Dycool/Usb-relay-for-NS`.

**Read the actual repository this pass, not a prior summary.** Earlier assessments of this repo in
this project (`ble-controller-protocol-inventory.md` §6, `PLAN.md`'s old Dycool bullet) were
summary-level, produced without directly inspecting the code. This pass used `WebFetch` to pull
and read the real source: `README.md`, `setup_pro_gadget.sh`, `pi_pro_proxy.cpp`,
`relay_protocol.hpp`, `win_pro_relay_capture.cpp`, `CMakeLists.txt` — quoting exact code where it
mattered rather than re-stating impressions.

**Traced the complete topology**: genuine Pro Controller 2 (USB) → Windows PC (`hidapi`,
`win_pro_relay_capture`) → UDP → Raspberry Pi (`pi_pro_proxy`) → `/dev/hidg0` (Linux `configfs`
HID gadget) → Switch 2 console. Confirmed bidirectional, confirmed the gadget's VID/PID/bcdDevice
match this project's own established genuine identity.

**Found a genuine positive**: `SendSwitch2Init()`'s feature-configure/enable commands
(`0C 91 00 02 00 04 00 00 27 00 00 00` and `0C 91 00 04 00 04 00 00 27 00 00 00`) match this
project's own independently-documented USB report-0x09 enable command **exactly**, byte for byte,
including the `0x27` mask (`report-0x09-motion.md` line 83) — real cross-validation between two
independent sources, not a coincidence worth dismissing.

**Found the audit's central concrete concern, evidence-backed not inferred**: the Pi-side gadget
uses Linux's generic `configfs`/`libcomposite` HID function, confirmed via an explicit in-code
comment (`// - This forwards HID reports. It does not implement a lower-level FunctionFS USB
device.`) to have **no mechanism for relaying EP0/control transfers at all** — it can only ever
answer control requests with the kernel's own static, descriptor-driven responses. Cross-referenced
against this project's own hard-won, already-documented fact (`STATUS.md`: *"EP0 vendor handshake
byte-exact vs a real PC2; console accepts it"*) that the Switch 2 console performs a vendor-specific
EP0 handshake as part of device classification, which this project's own PicoSwitch2 emulation had
to replicate byte-exact to be accepted. The relay, as committed, has no way to do this — a real,
specific, checkable reason (not just the repo's own vague "v0 may still fail" disclaimer) that
console classification is the likely failure point, before any report-0x09 data would ever be
exchanged with a real console.

**Identified a materially smaller, lower-risk alternative that sidesteps that risk entirely**: the
Windows-side `hidapi` + `SendSwitch2Init()` + timestamped-capture logic can be run directly against
the genuine controller over USB — no Pi, no console, no relay. Since the feature-enable command is
already cross-validated correct, this tests directly and cheaply whether report-0x09 motion data
becomes observable once the documented command is sent, independent of whether the harder
Pi/console classification problem can ever be solved. Proposed as the actual recommended first
step (not the full 3-node relay), per the task's explicit request for "the smallest isolated path."

**Produced the requested go/no-go report** with all specified sections (confirmed/unverified
components, concrete defects, required hardware, exact setup procedure, minimal fixes ranked by
leverage, a 4-phase validation ladder beginning with enumeration/buttons before motion, and the
eventual capture matrix, explicitly not analyzed since no captures exist yet):
`docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`.

**Produced the parallel controller-surface inventory** (commands, descriptors, input variants,
output reports, init, calibration, memory regions, wake, reconnect, mouse, audio, haptics,
firmware-update), ranked by relevance/evidence/tractability/hardware-dependency/information value,
explicitly labeled prioritization work rather than a queue of new implementations — added to
`PLAN.md`.

**No changes to `report 0x09`, BLE initialization, filters, or the BLE block's interpretation** —
this pass was research (one `WebFetch`-based repository read) and documentation only. No claim was
made that BLE and USB motion formats are equivalent — the audit is explicitly a separate, USB-only
path, orthogonal to the now-paused BLE work.

**Docs updated**: new pause banners at the top of `sw2-v2-motion-block-discovery-2026-07-10.md` and
`ble-controller-protocol-inventory.md`; new `usb-relay-feasibility-audit-2026-07-10.md`; `PLAN.md`
gained the controller-surface inventory and a rewritten Dycool assessment; `STATUS.md`'s "Current
Objective" and "Next Recommended Tasks" rewritten to reflect the second pivot.

**Ends with outcome 1** (per the task's required two-outcome structure): a concrete, executable
relay bring-up task — run the Windows-only `hidapi` capture (audit §7/§9 Phase 0) as the actual
next step, with Phase 1 (Pi+console enumeration, no genuine controller needed) as the cheap
follow-up that resolves the classification risk before investing in the full chain.

---

## Resolution — strategic pivot to NFC/Amiibo RE; gyro formally paused (2026-07-10, continuation)

**Instruction:** pause gyro investigation (not abandon — deferred until broader controller RE or
new primary evidence changes the situation); shift to systematic RE of remaining genuine Pro
Controller 2 features, starting with NFC/amiibo as a bounded subsystem. Treat Nintendo's official
NFC confirmation as fact, everything else (hardware, protocol, init, Switch-1 compatibility) as
unknown until evidenced. Six claims kept strictly separate throughout (official confirmation vs.
Joy-Con-2/Pro-Controller-2 physical hardware ID vs. protocol behavior per controller type vs.
Switch-1 carryover). Analysis, reusable read-only tooling, and documentation only — no NFC
emulation implemented, no exploratory writes, no behavior changes.

**External research:** Nintendo's own amiibo-support page confirms the Pro Controller 2's NFC
touchpoint location (over the Switch logo, top-center) — claim 1, officially confirmed.
`ndeadly/switch2_controller_research` (found via GitHub search; default branch is `master`, not
`main`) independently documents command `0x01` = NFC with a subcommand table (`0x01`-`0x04`
"Unknown", `0x05` "Get status", `0x06` "Read device", `0x08` "Write device", `0x0C` "Unknown",
`0x14` "Write buffer", `0x15` "Read buffer") and a GATT service/characteristic map that
cross-validates almost UUID-for-UUID against this project's own GATT discovery from an earlier
session pass — including resolving several previously-unexplained handles (`0x0018` firmware
update, `0x002C`/`0x002E` headset audio with an NFC-state byte at report offset `0xC`) and
confirming `0x0022`/`0x0026`/`0x002A` are genuinely still unknown to both projects. Switch 1's own
NFC/IR scheme (`src/switch_pro/switch_pro.c`, MCU subcommands `0x21`/`0x22` inside an entirely
different protocol) confirms claim 6 — Switch 1 carryover — is refuted at the command-ID level.

**Repo-internal dormant evidence found:** `docs/switch2/unmapped-features.md` already explicitly
acknowledged command `0x01`/NFC traffic is real and stubbed (the single most direct piece of
"dormant NFC evidence" asked for). `switch_pro2.c`'s `ns2_dispatch()` already hardcodes a real,
capture-derived 4-byte response (`61 12 50 10`) for subcommand `0x0C`, with a header comment
tracing it to "captures/usb, Pro Controller 2 = device 7" — i.e. `usbpcaptures/genuine_procon_2.pcapng`.

**New tooling and findings:** wrote `tools/extract_nfc_traffic.py` (reusable, read-only) to scan
that capture (164,242 packets, USBPcap format) for the command-`0x01` envelope signature. Found 7
raw byte matches; ruled 5 out as coincidental collisions (subcommand bytes not in any documented
table) and traced 2 to real command frames: subcommand `0x0C` (packets #30520/#30526, response
`61 12 50 10` — confirms this capture is the exact source of `switch_pro2.c`'s hardcoded value)
and subcommand `0x01` (packets #30528/#30532, a bare `dir=0x04` acknowledgment — new evidence
beyond ndeadly's own "Unknown" entry for this subcommand). Found a real self-consistency gap:
`switch_pro2.c` hardcodes response `dir=0x01` unconditionally, but the one genuine bare-ack
response observed used `dir=0x04` — documented as a validation target, not fixed (out of scope
this pass). A follow-up attempt to scan report-`0x09` traffic for a live NFC-state transition hit
a methodology dead end (the `payload[0]==0x09` filter matched USB Configuration Descriptors, not
HID reports — `bLength=0x09` collides with the report ID) — documented so it isn't repeated; the
correct fix (filter by USBPcap endpoint/transfer-type fields) is the recommended next task.

**Docs:** new `docs/switch2/nfc-protocol-inventory.md` (confidence-qualified inventory: Confirmed/
Strong Evidence/Hypothesis/Unknown, six-claim evidence separation, conflicts-between-sources
section, closing 4-part summary per the task's required format). `docs/switch2/unmapped-features.md`
§3 updated to point at it and note the `dir`-byte discrepancy. `STATUS.md`/`PLAN.md` updated:
gyro formally marked 🔴 paused (not the active objective, full history preserved for context); NFC
added as the new active objective with its own `PLAN.md` milestone (v1.1.5) and `STATUS.md` table
row/task-list entries.

**Chosen next step (branch 1 of 3, per the task's decision tree):** an existing Pro Controller 2
NFC capture already existed in this repo and has now been re-mined — selects "reproduce its exact
protocol map offline and identify what this repository can validate" over the Joy-Con-2-only or
no-evidence branches. Concrete next task: filter the same capture by USBPcap endpoint/transfer-type
(not first-payload-byte) to build a real report-`0x09` series and check whether the NFC-state byte
ever leaves idle anywhere in the 164,242-packet session — zero new hardware, extends the same tool.
