# Hardware Validation (2026-07-10) — Steam gyro fix + console gyro rewrite, on-hardware tests

**Status:** 🔵 Report 0x05 (Steam): axis fix confirmed working on hardware (test 2), roll sign
still unverified. 🔴 Report 0x09 (console): **still unresolved after two hardware passes** — the
first fix (magnitude-gated bias tracker) did not survive contact with hardware; a second,
differently-designed fix (§6) is now in and **pending test 3.** **Date:** 2026-07-10.
**Parent:** [gyro-experiment-a-results.md](gyro-experiment-a-results.md) (Steam timestamp/scale fix),
[report-0x09-motion.md](../switch2/report-0x09-motion.md) (console int32 format + enable-gate).

---

## 1. What was tested

The Experiment-A fix (report-0x05 timestamp + gyro scale) and the report-0x09 int32-phase rewrite
(int32 format + `0x0C` enable-gate + EMA low-pass) were flashed and tested for the first time on
real hardware: a DualSense paired to the dongle, tested against **Steam** (report 0x05) and against
the **console** (report 0x09, Splatoon + both Zeldas for the negotiated-feature check).

### Results (as logged, see the now-superseded `SESSION.md`)

| Path | Result |
|---|---|
| **0x05 / Steam** | Steam detects gyro (no longer frozen — Experiment A's timestamp fix holds). **Pitch/roll appear incorrect. Yaw appears mostly correct.** |
| **0x09 / console** | Splatoon clearly accepts gyro mode (the negotiated-feature gate works — length flips 0→30 and the game responds). **Stationary controller still causes camera movement** — the console is consuming motion data but interpreting it incorrectly. |

### Conclusion drawn at the time
The motion **pipeline** (transport, negotiated-feature gate, report structure) works on both paths.
The remaining problems are **mathematical**: coordinate transform (0x05) and integration/calibration
(0x09) — not transport. This ruled out further heuristic filter tuning as the next step.

---

## 2. Root-causing "pitch/roll incorrect, yaw correct" (report 0x05 axis mapping)

### 2.1 The seam's axis transform was never validated against a real device
`ns2_seam.c`'s `router_submit_input()` maps DualSense IMU axes to the Switch 2 output axes with a
fixed permutation + sign flip. That transform was carried over from the *old* (60×-under-scaled)
gyro path and never checked against a genuine Switch 2 Pro Controller's own axis convention — only
its *scale* was validated (Experiment A). "Yaw correct, pitch/roll not" is the signature of a
**partial** permutation bug (one axis right, two swapped), not a scale or transport bug — consistent
with the conclusion above.

### 2.2 New analysis: read the axis convention off the genuine controller's own capture
Experiment A's golden trace (`usbpcaptures/genuine_procon_2.pcapng`, a genuine Pro Controller 2
wired to the PC, USBPcap) was captured with a **known protocol**
([gyro-experiment-a-plan.md](gyro-experiment-a-plan.md) §A1): "sit still ~3 s, then rotate slowly
about each physical axis in turn — **pitch, then yaw, then roll** — ~3 s each." That capture was
mined for Experiment A's *content* (timestamp/scale) but its *axis order* was never analyzed. It
directly answers "what does raw gyro X/Y/Z mean physically" — the question §"Remaining unknowns"
in `report-0x09-motion.md` flags as open. No existing tooling did this (the prior session's
`scratchpad/usb_a_*.py` scripts were session-local and gone); a new parser was written this session
(`pcapng_parse.py` — raw pcapng block reader + `USBPCAP_BUFFER_HEADER` decode, no tshark dependency)
to re-mine it.

**Method:** decode every USBPcap-wrapped report-0x05 HID frame (EP `0x81` IN, 64-byte payload,
report id `0x05`) — 19 554 samples, `~4 ms` cadence, 78.2 s total — into accel/gyro per axis
(offsets per [report-0x09-motion.md](../switch2/report-0x09-motion.md)'s report-0x05 table: accel
X/Y/Z @ payload `0x31/0x33/0x35`, gyro X/Y/Z @ `0x37/0x39/0x3B`, all `int16_le`). Bucket `|gyro|` by
0.25 s to find which axis dominates when, then cross-check with accel: **rotating about a body axis
leaves that axis's own gravity component ~constant while the other two swing** — a rotation-axis
detector independent of the gyro data itself.

**Result — three segments, matching a 3-phase rotation:**

| Segment | t (s) | Dominant raw gyro | Accel cross-check |
|---|---|---|---|
| Still 1 | 0–14 | none (`\|g\|` < 2 LSB) | gravity ≈ all on accel-Z (4279/4309, controller flat) |
| **Phase 1** | 14.5–34.5 | **X** (absmean 1365 vs Y 338, Z 322 — ~4:1) | accel-**X** std smallest (571 vs Y 2341, Z 3065) → **X is the rotation axis** |
| Phase 2 | 35.5–41.5 | Z (absmean 1806 vs X 423, Y 591) | accel-Z std smallest of the three (1135) — weaker separation, less clean |
| Messy | 42–54 | mixed, no single axis >2:1 | (repositioning grip, not a clean rotation) |
| Phase 3 | 55–68 | Y-ish (absmean 1092) but Z close behind (1164) | accel-X std smallest (852) — **doesn't match Y as the rotation axis** |
| Still 2 | 69–78 | none | gravity redistributed onto accel-X/Z (controller set down differently) |

Phase 1 is unambiguous (clean single-axis rotation, confirmed two independent ways). Phase 2 is
consistent but weaker. Phase 3 is genuinely messy (no clean dominance, and the accel cross-check
contradicts a naive Y-axis reading) — the capture's third rotation was not held to the same
cleanliness as the first two.

### 2.3 Reading the mapping (order-trusting, then physically constrained)

Trusting the plan's stated order (pitch, yaw, roll) against the segment order (X, Z, Y-ish) gives:
**genuine gyro X = pitch, Z = yaw, Y = roll.** This is corroborated by the hardware symptom itself:
our old seam already fed DS5 yaw (`e->gyro[1]`) to output Z — and yaw is the one axis the user
reported as correct. By elimination the bug is X↔Y: DS5 pitch (`gyro[0]`) was going to output Y
(labelled "roll" by the genuine convention) and DS5 roll (`gyro[2]`) was going to output X (labelled
"pitch") — swapped.

**The fix is not a bare index swap.** The old transform (`out0=-src2, out1=-src0, out2=src1`) is a
proper rotation (determinant +1 — physically required for a rigid IMU remount, no mirroring). Simply
swapping which source feeds output 0 vs 1 flips the determinant to −1 (a mirror), which would look
right on isolated single-axis motion but be wrong on any compound rotation (real aiming). The
implemented fix swaps the two rows **and** negates one to restore determinant +1:

```
out_X (pitch) = -src_pitch   (was: -src_roll)
out_Y (roll)  = +src_roll    (was: -src_pitch)   <- sign chosen to keep det = +1, not independently measured
out_Z (yaw)   = +src_yaw     (unchanged — already correct)
```

Applied identically to accel (same transform, physical consistency between the two sensors sharing
a body frame). Implemented in `ns2_seam.c` `router_submit_input()`.

**Confidence:** 🔵 Medium — X/pitch and Z/yaw are well-supported (two independent signals: order +
accel cross-check for X, order + hardware symptom for Z). Y/roll's *axis identity* is supported only
by elimination (phase 3 was too messy to independently confirm), and its **sign** is inferred from
the determinant constraint, not measured. **Needs a hardware re-test to confirm**, ideally preceded
by a cleaner recapture (see §4).

---

## 3. Root-causing "stationary controller still moves the camera" (report 0x09 drift)

This is a **different bug** from the axis mapping above — SESSION.md is explicit that this happens
with the controller sitting still, which an axis/permutation error cannot cause (a wrong axis would
mis-route *intentional* motion, not manufacture motion from nothing).

**Cause:** `ns2_build_report()`'s motion block integrates raw gyro rate into the int32 angular-phase
field every report, unconditionally. A **genuine** ICM-42670-P has ~0.03 dps bias — negligible even
integrated over minutes. A **DualSense**'s gyro bias is not characterized in this repo but is
almost certainly larger; any constant bias, once integrated continuously, accumulates **linearly and
without bound** — a textbook dead-reckoning drift, independent of how clean the noise-vs-signal
filtering is (the existing EMA low-pass reduces noise, not bias). This matches the symptom exactly.

**Fix:** a slow, gated per-axis bias tracker in `ns2_build_report()`. Each report, if **all three**
raw gyro axes read below a small threshold (±40 LSB ≈ ±2.4 dps — i.e. the controller looks
stationary this instant), each axis's bias estimate creeps toward its current low-passed value
(`>>8` shift ≈ seconds-scale time constant); the bias estimate is then subtracted before
integration. Gating on "all three near zero" (not per-axis) avoids absorbing real single-axis
rotation into that axis's own bias estimate. This is the same shape of fix commercial gyro-aim
pipelines use (a slow bias/zero-rate tracker gated on a stillness heuristic) — not a novel technique,
but this repo had none of it; the block was doing raw dead-reckoning.

**Confidence:** 🟡 High that this is *a* correct fix for *drift while stationary* specifically (the
physics is unconditional — any unremoved bias must drift under integration). Not yet validated that
DualSense bias is the *only* contributor, or that the ±40 LSB / `>>8` constants are well-tuned —
flag for hardware re-test.

---

## 4. Recommended next capture (for a clean axis-order re-validation)

Phase 3 above (intended "roll") was not clean enough to independently confirm. A tighter recapture
would remove the remaining 🔵: hold the genuine controller flat, then for each axis — **pause ~2 s
fully still, rotate ~90° over ~2 s, pause ~2 s still, back to flat ~2 s still** — before moving to
the next axis, with a clear stop between them (the "messy" 42–54 s window in this capture was
exactly a same-motion transition with no stop, which is why it couldn't be isolated). Note the
performed order out-of-band (verbally, or in a synced timestamp) so it doesn't have to be inferred.
The same `pcapng_parse.py` script works unmodified; the analysis in §2.2 is directly reusable.

## 5. Files touched (tests 1 → take-1 fixes)
- `src/bt_hid/ns2_seam.c` — axis permutation fix (§2.3).
- `src/switch_pro2/switch_pro2.c` `ns2_build_report()` — gyro bias tracker (§3).
- Both boards built clean (`build.ps1`).

---

## 6. Second hardware test (2026-07-10, 14:07) — results

Both take-1 fixes above were flashed and re-tested: Steam (report 0x05) and the console —
Splatoon + both Zeldas (report 0x09).

| Path | Result |
|---|---|
| **0x05 / Steam** | **Improved.** Gyro smoother, axes now appear correct. Steam calibration — which had no effect on the pre-fix build — now produces usable motion. Not yet independently confirmed for roll polarity (§4 recapture still outstanding). |
| **0x09 / console** | **Unchanged.** Gyro remains wildly unstable in both Splatoon and Zelda; motion is still not usable. The stillness-gated bias tracker had **no observable effect** on the stationary-drift symptom. |

**What this eliminates:** report 0x09's drift is not explained by *"ordinary DualSense gyro
bias, uncorrected."* That fix was implemented and made no difference — either the bias tracker
itself never engages (a mechanism bug, see §7), or bias was never the dominant term. It does
**not** eliminate integration/bias as a category — see §7's conclusion.

**What this does not touch:** the format itself (field boundaries, Q16.16 accel scale, 800 Hz
timing, the negotiated-feature gate) — none of that changed between test 1 and test 2, and test 1
already showed the console *accepting* motion and *reacting* to deliberate rotation (Splatoon
"clearly accepts gyro mode"). The bug is specifically in what the phase fields *contain* while
the controller is not moving, not in whether the console reads them at all.

---

## 7. Root-causing the persistent 0x09 drift (take 2)

### 7.1 The take-1 bias tracker's gate was self-defeating

Take 1's stillness gate (`ns2_build_report()`, pre-fix) was:

```c
bool still = true;
for (int ax = 0; ax < 3; ax++)
    if (in.gyro[ax] > 40 || in.gyro[ax] < -40) still = false;
```

This tests raw gyro **magnitude** against a fixed threshold (±40 LSB ≈ ±2.4 dps at the
16.384 LSB/dps scale used throughout this pipeline). The bug: a MEMS gyro's constant **zero-rate
bias is part of its magnitude reading** — a gyro sitting perfectly still does not read `0`, it
reads its bias. Consumer-grade MEMS gyros (the class the DualSense's IMU belongs to; the genuine
Pro Controller 2's ICM-42670-P is comparatively excellent at ~0.03 dps) commonly carry a
zero-rate bias of **several dps** unless factory-trimmed. If the DualSense's bias alone exceeds
~2.4 dps, `still` is **false on every single report, forever** — even with the controller bolted
to a table. The bias tracker's update line (`if (still) bias[ax] += …`) then never executes, the
bias estimate stays at its power-on value of `0`, and the "fix" degrades to exactly the take-1
un-fixed behavior: pure unbounded rate integration. **This reproduces the test-2 symptom exactly**
(zero observable change) and is a plausible, self-consistent explanation — not yet independently
confirmed by an on-hardware bias reading (see §7.3).

### 7.2 The fix: gate on steadiness, not smallness

Stillness should mean *"the reading isn't changing,"* not *"the reading is near zero."* A
constant bias, however large, produces a near-zero **frame-to-frame derivative** while the
controller is physically still — only genuine rotation produces a large derivative. Implemented
in `ns2_build_report()`:

```c
int32_t d = (int32_t)in.gyro[ax] - ns2_gyro_prev_raw[ax];
ns2_gyro_prev_raw[ax] = in.gyro[ax];
if (d < 0) d = -d;
ns2_gyro_jitter[ax] += ((d << 6) - ns2_gyro_jitter[ax]) >> 3;   // EMA of the derivative
if (ns2_gyro_jitter[ax] > (6 << 6)) still = false;               // > ~6 LSB/report of change
```

This gates correctly regardless of how large the DualSense's absolute bias turns out to be. The
bias tracker's adaptation logic (slow `>>8` EMA toward the low-passed reading, subtracted before
integration) is otherwise unchanged from take 1 — only the *gate* was broken.

### 7.3 Closing the loop: bias/gate state is now observable, not just inferred

Both hardware tests so far diagnosed the bias tracker **indirectly**, from in-game camera
behavior — slow and ambiguous (many different bugs produce "camera drifts"). To avoid a third
round of blind guessing, the bias estimate and live gate state are now exposed through the
existing config-mode IMU debug readout (`cmd_imu` in `src/config.c`, surfaced on the config web
UI's `imu:` line): `bias=[x,y,z] still=0|1`. **Before touching the bias-tracker constants again,
read this line with the controller sitting still** — if `still` never reads `1`, the gate is
still broken (a different mechanism than §7.1); if it does read `1` but drift continues anyway,
the bias tracker is not the dominant remaining error term and the investigation should shift
to the phase-integration constant, timing reconstruction, or a fusion-based (accel-corrected)
model instead of continuing to iterate on gating heuristics.

### 7.4 Confidence

🟡 Medium-high that §7.1 is *a* real mechanism bug (the reasoning is a direct reading of the
code + well-known MEMS gyro characteristics, not a guess) and that §7.2 fixes it. **Not yet
confirmed** that fixing the gate alone eliminates the drift — DualSense gyro noise (not just
bias) is also uncharacterized in this repo, and the genuine controller may additionally rely on
accelerometer-based tilt correction (a complementary/Kalman filter) that this implementation still
lacks entirely for yaw-axis drift, which no bias tracker can correct (no absolute reference for
yaw exists in a 3-axis IMU). Flag as the next research direction if §7.3's `still=1` diagnostic
comes back positive but drift persists.

### 7.5 External corroboration — `TommyWabg/Switch2Connect`

Per SESSION.md's research leads, investigated two external repos for report-0x09-relevant
evidence (no hardware, analysis only).

**`Dycool/NS-PC-Control` (`joycon-usb-experiments` branch): first pass incomplete, corrected
2026-07-10.** The initial check only looked at `shared/src/` and found a legacy Switch-1-format
`MotionReport` (fixed 12-byte int16 struct, 3 samples/report, classic 4096 counts/g /
16.384 counts/dps) — no AHRS, no bias/drift handling there. A deeper pass across the *whole* tree
(the repo owner flagged the branch mentions BT/gyro/wake material beyond `shared/src/`) found real
content in `server/src/`:
- `server/src/switch2_native.cpp` independently confirms `0x1FC000` as the real
  motion-calibration write address (matches this repo's documentation) via
  `mem_write_user_motion_cal()`, but for a **72-byte** payload — a discrepancy against this repo's
  documented 64-byte (`0x40`) region, not yet resolved (folded into
  `docs/switch2/report-0x09-motion.md`'s Calibration section). It also confirms a `0x02/0x05`
  memory-write command family and a `set_feature_state()` IMU-enable toggle — consistent with
  (not new evidence beyond) this repo's existing negotiated-feature model.
- `bluetooth_input.cpp` streams `pad.motion_samples[0..2]` — a 3-samples-per-report internal
  representation. Weak, not independent proof of the Switch 2's actual wire encoding (this is that
  project's own internal model, not a cited capture), but worth noting as a second project
  independently landing on "3 samples" — loosely consistent with this repo's still-undecoded
  length-40 variant ("likely 3 samples at a higher rate").
- **Bluetooth wake and reconnect-reliability findings** (`server/src/bluetooth_manager.cpp`,
  `gadget_wakeup.cpp`, `docs/wakeup.md`) are real and substantial but **off-topic for report
  0x09** — filed in `docs/bluetooth/btstack-implementation.md` instead (new "Reconnect
  reliability" and "BLE wake-from-sleep" sections), since they bear on the separate "BT pairing
  reliability" and "wake-over-BLE out-of-scope" items, not motion.

**`TommyWabg/Switch2Connect`: relevant, and it changes what "next step" should mean.** This
project is a PC-side host that connects to a **genuine** Pro Controller 2 over **BLE** (different
transport from our USB report 0x09, so it isn't direct proof of our field semantics, but it's
hard evidence about how Nintendo's own firmware and a working consumer of it behave):

- **The real controller's BLE motion report streams raw int16 angular *rate*, not integrated
  phase** (`src/controller.py`, Pro Controller 2 branch: gyro at one offset range, accel at
  another, ~14.29 LSB/dps). Nintendo's own hardware does not hand a consumer "orientation" —
  *something* downstream has to integrate/fuse it, exactly like we do.
- **Switch2Connect does not trust naive integration either.** It runs a full Mahony/Madgwick AHRS
  (the `imufusion` library) over the raw gyro+accel stream to get a stable orientation for its own
  gyro-mouse feature. This is strong, independent (non-hardware) support for this doc's standing
  worry in §7.4: pure rate integration, even bias-corrected, may not be enough — the genuine
  controller (or *something* in the pipeline) likely performs real sensor fusion, not just bias
  removal.
- **Concrete, working bias-correction design** (`_mahony_update()`): gate the correction term on
  `accel is near 1g AND gyro is actively moving` (specifically: accel error small *and*
  `gyro_mag ≥ 45`) — the **opposite** shape of gate from both our take-1 (magnitude-below-threshold)
  and take-2 (derivative-below-threshold) attempts. Instead of asking "is the controller still?",
  it asks "is the controller in steady, non-accelerating motion, so gravity is a trustworthy
  reference right now?" and derives the correction from a Mahony proportional-feedback term (cross
  product of measured vs. estimated gravity vector), not a simple EMA toward the raw reading.
  **This is the concrete design to reach for** if hardware test 3 shows `still=1` (take-2's gate
  is working) but drift still persists — it is a different, accel-referenced correction mechanism
  entirely, not a retuning of the current one. Not implemented this pass (a full AHRS is a
  materially larger change than a gate fix, and stacking it on an unvalidated take-2 would make a
  third hardware failure hard to attribute) but now has a named algorithm, a working reference
  implementation, and a citable gating condition instead of a vague "consider fusion" note.
- **Magnetometer confirmed as a real, separate BLE feature** (`FEATURE_MAGNOMETER = 0x80`,
  enabled via a `0x0c`-family command with mask bit `0x80` set). This **corroborates** §"Refuted:
  magnetometer / 9-axis IMU" in `report-0x09-motion.md`: our capture's enable mask was `0x27`
  (binary `0010 0111` — bits 0,1,2,5), which does **not** include bit `0x80`. Magnetometer was
  simply never requested in our capture, consistent with (not contradicting) the 30-byte block
  having zero spare room for it. If a magnetometer field exists at all in this protocol, it is a
  separate, independently-negotiated report field — not something to look for inside the phase or
  accel lanes.
- **Incidental find, filed for later:** the BLE gyro/accel/magnetometer byte offsets Switch2Connect
  uses are a ready-made reference if this repo ever implements passing through a **genuine Pro
  Controller 2's own motion** over BLE — currently a known gap (`switch2_ble` discards a real Pro
  2's motion; STATUS.md "Deferred / Blocked"). Out of scope for report 0x09 (USB/console), noted
  for the BLE work.

---

## 8. Files touched (test 2 → take-2 fix)
- `src/switch_pro2/switch_pro2.c` — stillness gate redesigned (derivative, not magnitude);
  bias-tracker state promoted to file scope; new `ns2_dbg_motion_bias()` debug getter (§7).
- `src/config.c` `cmd_imu()` — exposes live bias/still state.
- `web/index.html` (+ regenerated `src/web_disk.h`) — `imu:` debug line shows `bias=[...]
  still=0|1`.
- Both boards build clean (`build.ps1`); **pending hardware test 3.**

---

## 9. The take-2 debug readout was itself unreachable — found from real hardware output (2026-07-10)

### 9.1 What was actually tested, and what it showed

The user connected both a DualSense and a genuine Pro Controller 2 to the dongle **in config
mode** and read the `imu` debug line for each:

```
DualSense: imu: has_motion=1 rid=0x9 stream=0 mlen=0 | accel=[0, 674, 4093] gyro=[6, 4, 12] | bias=[0, 0, 0] still=0
Genuine:   imu: has_motion=0 rid=0x9 stream=0 mlen=0 | accel=[0, 0, 0] gyro=[0, 0, 0] | bias=[0, 0, 0] still=0
```

This is real, and it's the actual test-3-adjacent data this doc's §7.3 asked for — an earlier pass
in this session initially failed to read it (the file had grown past where that pass stopped
reading). Correcting that now.

### 9.2 `bias=[0,0,0] still=0` is not a measurement — the code path was unreachable

`stream=0` and `mlen=0` are the tell. Reading the `imu` CDC command is only possible in **config
mode**, and `usb.c`'s main loop takes an unconditional early-exit there:

```c
if (g_usb_config_mode) {
    config_cdc_task();
    continue;   // ns2_task() below is never reached
}
...
ns2_task();   // only place that used to call ns2_build_report() -> the bias/gate tracker
```

`ns2_build_report()` — the *only* code that ran the take-2 bias/stillness tracker — was **never
called** in config mode. `bias`/`still` are file-scope statics that stayed at their power-on-zero
values forever whenever read the only way they were reachable. The DualSense line's live,
changing `accel`/`gyro` values (which come from the cross-core state, updated independently by
core1's BT stack regardless of USB mode) proved the motion *pipeline* was alive while the
tracker's own output stayed frozen — that mismatch is what exposed the bug. **This says nothing
about whether the take-2 derivative gate works.** It never ran.

The genuine Pro Controller 2's `has_motion=0` is unrelated and already explained: the
`switch2_ble` driver still discards a genuine Pro 2's own motion (documented gap, `STATUS.md`
Deferred/Blocked) — expected, not new.

### 9.3 Fix: decouple the tracker from the streaming/enabled gates

`ns2_build_report()`'s motion block used to compute timing+phase+bias *inline*, gated on
`in.has_motion && ns2_imu_enabled` — so the tracker only ran when a real host had both connected
and completed the `0x0C/0x04` IMU-enable handshake, which never happens in config mode (no real
host is present). Extracted the tracker into `ns2_motion_tick()`, called whenever `in.has_motion`
regardless of `ns2_imu_enabled` (which now only gates whether the *computed* phase bytes get
*written into the transmitted report* — the internal state keeps advancing either way). Added
`ns2_motion_debug_tick()`, called from `usb.c`'s config-mode branch, rate-limited to ~250 Hz to
match real HID cadence (the low-pass/jitter EMA time constants are tuned per-call, not
per-elapsed-time, so calling faster than the real streaming rate would make the observed
stillness behavior unrepresentative of what happens on-console).

**Practical effect:** the `imu` debug line's `bias=[...] still=...` fields are now live and
meaningful *without a console at all* — plug a DualSense into the dongle, enter config mode, and
watch `still` directly to confirm the take-2 gate opens when the controller sits motionless. This
is strictly easier than the originally-planned test (no game, no console session needed) and
should be done **before** the in-game Splatoon/Zelda test, since it isolates the gate mechanism
from everything else in the pipeline.

### 9.4 Confidence and next step

🔴 Take-2's actual behavior is **still unverified** — this pass found and fixed a *different* bug
(the instrumentation was unreachable), not confirmation or refutation of the gate itself. The
`gyro-bias-tracker constants` technical-debt note in `STATUS.md` still applies. **Next hardware
step, revised:** read the `imu` line with a DualSense connected in config mode, sitting still,
and confirm `still` transitions to `1` within a few seconds. Only then does the in-game
Splatoon/Zelda drift test become informative about the *phase math*, rather than potentially
re-discovering the same "the instrumentation says nothing happened" symptom for an unrelated
reason.

## 10. Files touched (this section's fix)
- `src/switch_pro2/switch_pro2.c` — extracted `ns2_motion_tick()` (timing/phase/bias, independent
  of `ns2_imu_enabled`) from `ns2_build_report()`'s inline motion block; added
  `ns2_motion_debug_tick()` (rate-limited, config-mode-only entry point).
- `include/switch_pro2.h` — declares `ns2_motion_debug_tick()`.
- `src/usb.c` — calls `ns2_motion_debug_tick()` from the config-mode branch of the main loop.
- Both boards build clean (`build.ps1`); **pending a hardware check of the `imu` line in config
  mode** (§9.3) — no code changes to the gate/bias math itself this pass, only to what makes it
  observable.

---

## 11. Config-mode gate check — CONFIRMED WORKING on real hardware (2026-07-10)

### 11.1 Result

Once the debug readout was actually reachable (§9-10), the user connected each controller in
config mode and read the `imu` line:

| Controller | Result |
|---|---|
| **DualSense** | `still=1` when stationary, `still=0` when moving. |
| **Genuine Pro Controller 2** | `still=0` always, even stationary. |

### 11.2 DualSense: the take-2 derivative gate works exactly as designed

This is the first real hardware confirmation of *any* part of the bias-tracking mechanism across
three attempts (take-1 magnitude gate, take-2 derivative gate's first check which turned out to be
unreachable, and now this). `still` correctly follows physical stillness — opens at rest, closes
under motion — independent of the DualSense's absolute gyro bias, exactly the property §7.2's
redesign was for. **This closes the open question from §9.4**: the gate mechanism itself is sound;
whatever caused test 2's in-game drift is either (a) already fixed now that the bias tracker can
actually adapt, or (b) a different remaining error term (phase-integration constant, timing, or a
fusion/accel-correction need) if in-game drift persists on re-test. The gate is no longer a
suspect on its own.

### 11.3 Genuine Pro Controller 2: expected, not a new bug

Confirmed by code inspection (not inference): `switch2_ble.c` (`src/bt_hid/bt/bthid/devices/vendors/nintendo/`)
contains **zero** references to `has_motion`, `gyro`, `accel`, or `motion` anywhere in the file —
this driver has never populated the shared `switch_pro_input_t`'s motion fields for a genuine Pro
Controller 2 at all. `ns2_motion_debug_tick()`'s `if (in.has_motion) ns2_motion_tick(&in);` guard
therefore never calls the tracker for this source, so `ns2_dbg_still` never updates away from
whatever it was left at — `0` is the expected, uninformative result, not evidence of anything
wrong with report 0x09 or the gate. This is the same gap already tracked in `STATUS.md`
Deferred/Blocked ("the `switch2_ble` BT driver still discards a genuine Pro 2's own motion") —
now additionally confirmed through this specific symptom, not a new discovery.

### 11.4 Next step

The precondition set in §9.4 is met. **Proceed to the in-game Splatoon/Zelda re-test** with the
current build (no code changes needed — the fix under test is already flashed, since it's the same
build the config-mode check just validated). If stationary drift is gone: take-2 is confirmed and
this investigation is closed pending roll-sign verification (report 0x05) and the deferred items.
If drift persists despite a confirmed-working gate: the bias tracker is conclusively not the
(sole) remaining cause, and the next step is the accel-based drift-correction (complementary
filter) model flagged in §7.4/§9.4 and `report-0x09-motion.md`'s phase-semantics discussion — not
further gate or threshold tuning, which this result rules out as the explanation.

---

## 12. Symptom reclassified: abrupt multidirectional jumps, not gradual drift (2026-07-10)

### 12.1 Correction

The in-game report-0x09 symptom (§Report `0x09` — Switch 2, test 2) has been described throughout
this document and `STATUS.md`/`PLAN.md` as "drift." Corrected, from direct observation: with a
DualSense connected, both Splatoon and Zelda show **abrupt camera movement or jumping in multiple
directions, even while the controller is stationary** — qualitatively different from a slowly
accumulating orientation error. "Drift" implied gyro bias and gradual integration error, which
biased the investigation toward the bias-tracker line of work (§3-11 above). That work was not
wasted — the take-2 gate is now confirmed correct (§11) — but it was never going to explain
*discontinuous* jumps, since bias-driven drift is definitionally smooth.

**Practical effect:** don't let a confirmed-working stillness gate be read as "the fix is basically
done, just needs an accel-based correction layer for the rest." An abrupt-jump symptom is stronger
evidence of an incorrect report-0x09 representation, field semantic, state transition, scale, or
counter/timing relationship than of residual gradual drift. Do **not** proceed straight to a
complementary filter (§7.4/§11.4) merely because that was the previously-nominated next step for
"drift" — that proposal targets gradual integration error and has no particular reason to fix
discontinuous jumps.

### 12.2 New primary evidence: the native BLE motion format, independently decoded

A new local document, `docs/experiments/switch2_native_motion_map_DyCOOL.md`, provides a
carefully validated (cross-correlated against a reference sensor, gravity-integration-tested)
decode of the Switch 2's **native BLE** motion report — Joy-Con 2 (R), report `0x08`, GATT handle
`0x000E`, 40 bytes. Confidence markers in that document are unusually rigorous for third-party
material: stated correlations (0.996-0.9994 gyro, 0.999 accel) against an independent reference
sensor, gravity-vector cross-checks, and an explicit confidence table separating CONFIRMED from
STRONG from UNKNOWN per field — evaluated on its own terms, not taken on authority.

**Structure:** the 36-byte payload (after a 4-byte timing+temperature header matching report
0x09's own timing-word structure almost exactly — see §12.3) is **bit-packed, LSB-first**, and
contains **three accelerometer samples and two gyro samples per BLE notification**, at three
different sub-report timestamps (t−10ms, t−5ms, t), alternating between full (14-bit) and half
(13-bit) resolution to fit bandwidth. This is **raw sensor samples, not an integrated/accumulated
value** — the native format's gyro fields are individually clamped to ±500°/s (14-bit, ±8192
counts) with a "near clamp" status bit, explicitly **not** a free-running phase accumulator.

Also newly established by that document: genuine hardware performs **on-chip bias correction
before transmission** ("saturam com correção de bias" — clamped fields show bias-corrected
saturation, not raw-sensor saturation), and gives an actual noise/bias characterization for a
genuine Joy-Con 2 at rest: mean gyro (−1.2, −5.3, −2.3) counts, σ = (4.9, 9.2, 3.8) counts — the
first real per-axis noise numbers available in this repo for *any* genuine Switch 2 controller
(previously only a rough "~0.03 dps" figure existed). At 16.4 counts/dps that's roughly
−0.07 to −0.32 dps mean, σ 0.2-0.6 dps — small, but not the single "~0.03 dps" figure previously
cited; worth treating that older figure as approximate, not precise.

The document explicitly flags: **"Pro Controller 2 (0x09): TO BE VERIFIED (motion @0x0F, 30 bytes
USB — smaller payload, likely a variant)."** This is not a decode of report 0x09 — it's the
strongest available Rosetta stone for reasoning about it by analogy, and its own author does not
claim otherwise.

### 12.3 Reconciling the native format against report 0x09's established model

Point of **agreement**: report 0x09's timing word (`motion[0x00]`: low-12 bits = 800 Hz tick,
high-4 bits = ticks elapsed since previous report) has the **identical structure** to the native
BLE format's `motion[0:2]` field described in the DyCOOL document. This is a meaningful structural
link between the two reports — both plausibly draw from the same underlying 800 Hz IMU sampling
timeline and framing convention, even though the payloads that follow differ.

Point of **tension**: the native format encodes raw, bounded samples; report 0x09's established
model (independently derived from two prior captures, `report-0x09-motion-analysis.md`) is an
**unbounded 32-bit binary-angle accumulator**. These are different value semantics. However, this
tension is **not new** — it was already the single lowest-confidence part of the *existing*
report-0x09 model before this document arrived (`report-0x09-motion-analysis.md`'s own confidence
table rates the accumulator interpretation "High confidence, but still inferred," below the
"Very high confidence" tier given to field boundaries/timing/accel-scale). What this document adds
is a concrete, working *alternative* — not proof the existing model is wrong.

**Weighing the two:** the accumulator model is not merely convenient — `report-0x09-motion-analysis.md`'s
worked example (a real, captured report-0x09 payload) shows phase values that are small
(`X≈−0.006°, Y≈−0.002°, Z≈−180°` shortly after motion-enable, consistent with the documented
`{0,0,0x80000000}` startup state) and, across the full capture, differentiate into **physically
plausible bounded rates (≤~2000°/s)** using ordinary modular (wraparound-aware) subtraction — i.e.
genuine report-0x09 phase data, when read the way this repo's model expects, behaves smoothly and
sanely. That is real, if indirect, evidence *for* the accumulator model specifically for report
0x09, independent of what the sibling BLE report does. **Net assessment: field boundaries and the
accumulator-style semantic both remain the working model; this session's new evidence sharpens
what to check next (§12.4) rather than overturning either.**

### 12.4 Ruled out this pass: gyro scale mismatch

One candidate explanation considered: if the DualSense's actual raw gyro LSB/dps scale differs
from the `16.384 LSB/dps` (±2000°/s over a 16-bit signed range) this repo's phase-integration
constant assumes, residual sensor noise could be amplified into large, erratic phase swings that
would look exactly like multidirectional jumps. **Checked against the joypad-os framework's own
declaration, not a guess:** `src/bt_hid/core/input_event.h` declares
`event->gyro_range = 2000; // Default to DS4/DS5 range (±2000 dps)` — i.e. the framework itself
states the DualSense's gyro is ±2000°/s over (implicitly, given SInput's 16-bit convention used
elsewhere in the same file) a signed 16-bit range, which is exactly `32768/2000 = 16.384 LSB/dps`.
**This matches the constant already in use.** Scale mismatch is ruled out as the explanation for
*this* controller. (Separately, `gyro_range` is a real per-controller field the codebase already
carries but report-0x09's phase math never reads — a DS3, whose own driver comment states
`event->gyro_range = 100`, would be **20× off** if ever fed through the same hardcoded constant.
Filed as a real robustness gap, not the cause of the current DualSense-specific symptom.)

### 12.5 Candidate fault classes for abrupt jumps (unresolved, ranked)

1. **Value semantics** (accumulator vs. bounded raw sample) — §12.3's open question; leans
   toward accumulator per report-0x09's own prior evidence, not fully closed.
2. **A discontinuity in *our* phase computation itself** — untested until now; see §12.6, the new
   instrumentation this pass adds specifically to check this.
3. **Sign/axis permutation** — a wrong axis mapping produces visually wrong-direction motion, but
   has no obvious mechanism to produce *discrete jumps* from an otherwise smoothly-evolving signal;
   lower priority than 1-2 for explaining this specific symptom shape.
4. **Timing/dt edge cases** — the phase-integration `dt_us` clamp (500-16000 µs) bounds any single
   step, but a real gap (BT hiccup, mode transition) hitting the clamp ceiling repeatedly could
   still produce a visible step; the "always present" nature of the symptom argues against this
   being the *primary* cause, but it's cheap to rule out via §12.6's instrumentation too.
5. **Pre-enable accumulation** (self-introduced by this session's §9-10 refactor): `ns2_motion_tick()`
   now runs whenever `in.has_motion`, independent of `ns2_imu_enabled` — meaning phase accumulates
   during the ~174-251 zero-length reports *before* the console negotiates IMU, whereas genuine
   hardware's captured starting state was clean (`{0,0,-180°}`). This **cannot** be the primary
   cause (the jumping symptom predates this session's refactor), but it's a real, newly-introduced
   discrepancy against the captured reference starting state, worth a deliberate decision (reset
   phase on the `0→1` transition of `ns2_imu_enabled`, or not) rather than leaving it accidental.
   Not changed this pass — flagged, not fixed, pending the discriminating check in §12.6.
6. **Missing field/relationship** — report 0x09 has no equivalent of the native format's undecoded
   "header bits 5-67" region (DyCOOL's own document flags this as possibly a slow-drift/fusion
   state, unconfirmed); report 0x09's independently-derived byte layout has no unaccounted space
   for such a region (§"Refuted: magnetometer" in `report-0x09-motion.md` — the 30-byte budget is
   exact), so this is unlikely to apply directly, but is worth remembering if the byte-layout
   model itself is ever revisited.

### 12.6 New instrumentation: expose the phase accumulator directly (this pass's code change)

Per the standing constraint (don't make another heuristic `0x09` encoder change before identifying
a discriminating test), this pass adds **observability only**: `ns2_dbg_motion_phase()`
(`switch_pro2.c`) exposes the live `ns2_phase[3]` accumulator — the exact value report 0x09 would
transmit right now — through the same config-mode `imu` debug command as `bias`/`still`, now also
showing `phase=[x,y,z]` (raw int32) and an approximate degree conversion. Both boards build clean.

**What this directly tests (candidate #2 above):** connect a DualSense, enter config mode, let it
sit motionless, and watch `phase` over an extended period (a minute or more — long enough to catch
an infrequent event). Two outcomes:
- **Phase stays smooth/slowly-changing** (small monotonic creep from the now-confirmed-working
  bias tracker, no sudden large steps): our own math has no discontinuity. The jump must originate
  either in the console's interpretation of a mathematically-smooth-but-wrong-semantic value
  (favors candidate #1) or somewhere between our phase value and what's transmitted (worth then
  re-checking the report-assembly code path itself, not exercised by this config-mode check).
- **Phase itself jumps abruptly** while the input is stationary: the bug is in *our* computation,
  not in the console's interpretation — candidates #2/#4/#5 become the priority, and the fix is
  local to `ns2_motion_tick()`/`ns2_build_report()`, not a redesign of the value semantic.

This is the smallest available experiment: no new hardware, no new capture tooling, reuses the
exact debug path already validated working in §11.

### 12.7 External repo research this pass

**`Dycool/Usb-relay-for-NS`** (per SESSION.md's new lead) — a live USB relay chain (`real Switch 2
↔ Raspberry Pi USB gadget ↔ UDP ↔ Windows PC (hidapi) ↔ real Pro Controller 2`), explicitly
targeting PID `0x2069`. If it works, this is exactly the missing capability this repo has lacked
all session: **repeatable, timestamped, controlled captures of genuine console-side report 0x09
traffic** (the Windows-side relay does pure byte pass-through with a capture file, so unmodified
genuine bytes would be recorded). Hardware needs are modest (a Pi with USB gadget/OTG support, no
Cynthion/Packetry needed) — realistic for this project. **Caveat: unproven.** It's an explicit "v0"
with a code comment admitting the console-side USB gadget classification may not work at all, no
committed sample captures, and a referenced-but-not-committed decode script. **Not a tool to build
on top of yet — a candidate to try and see if it works, next time hardware access is available.**
Filed in `PLAN.md`.

**`ValveSoftware/steam-devices`** — confirmed dead end for protocol evidence. It is exactly
`udev` access-grant rules (`hidraw`/USB device-node permissions for Steam/SteamVR on Linux) — three
files total, no report-parsing logic, no byte offsets, nothing beyond a VID/PID allowlist
(Switch 2 Pro Controller `057e:2069` and siblings are present, confirming Steam *talks* to the
device, nothing about *how* it parses motion). Steam's actual parser is closed-source and not
observable from this repo. No changes made to `report-0x09-motion.md` based on this repo.

## 13. Files touched (this section)
- `src/switch_pro2/switch_pro2.c` — new `ns2_dbg_motion_phase()` getter.
- `src/config.c` `cmd_imu()` — exposes live `phase=[...]`.
- `web/index.html` (+ regenerated `src/web_disk.h`) — `imu:` line shows `phase=[...] (~[...]°)`.
- Both boards build clean (`build.ps1`); **pending a hardware check of the `phase` field's
  stability in config mode** (§12.6) — no change to the encoder/gate/bias math itself this pass.

---

## 14. Full-path audit + mathematically-derived anomaly detector (2026-07-10, continuation)

### 14.0 New hardware data received

```
DualSense: imu: has_motion=1 rid=0x5 stream=1 mlen=0 | accel=[-30, 688, 4091] gyro=[10, 5, 10]
           | bias=[6, 3, 6] still=1 | phase=[116423320, 112172745, -2034314527]
           (~[9.76, 9.40, -170.51]°)
```
`rid=0x5`/`stream=1` are **stale, harmless leftovers** from whatever host session (PC/Steam)
preceded this config-mode session — `ns2_report_id`/`ns2_streaming` are set once by a real host's
`0x03/0x0A` command and never reset on config-mode entry (only `ns2_init()` at boot resets them);
they aren't read by anything in config mode. Not a bug, just a display artifact worth naming so it
isn't mistaken for one. The substantive data: `still=1` (gate open, consistent with §11), `bias`
converged to nonzero (6,3,6 LSB), and **phase sitting at ~9-10° on X/Y** (not near 0°) while
reportedly stationary, described by the user as "moving a lot." This is exactly the observation
this section's instrumentation was built to make legible — see §14.4 for the test procedure.
**No anomaly has been captured with the current build yet** (fresh build, not yet re-flashed to
hardware) — nothing below claims a root cause; §14.4 explains how to get one.

### 14.1 Full-path audit (no behavior changes)

Traced source sample → transmitted bytes, per the task's explicit scope:

| Stage | Code | Bound / guarantee |
|---|---|---|
| Source IMU sample | DualSense driver (`ds5_bt.c`) → `input_event_t.gyro[3]` | `int16_t`, type-bounded to [-32768,32767] |
| Axis transform | `ns2_seam.c` `router_submit_input()`: permute + `ns2_clamp16()` | Explicitly re-clamped to [-32768,32767] regardless of input |
| Cross-core publish | `report.c` `set_global_gamepad_input()` | Existing infra, unaudited this pass (pre-existing, not touched by any recent change) |
| Stillness gate | `ns2_motion_tick()`: `ns2_gyro_prev_raw`/`ns2_gyro_jitter` | EMA of a bounded derivative; decides `still`, doesn't touch phase directly |
| Bias tracker | `ns2_gyro_lp`, `ns2_gyro_bias` (EMAs, weights 1/4 and 1/256) | Each independently bounded to the input's range by induction (§14.2) |
| dt / timing | `dt_us` clamped `[500,16000]`; `count`/`imu_tick` derived from it | Hard clamp in code, not advisory |
| Phase integration | `ns2_phase[ax] += g * dt_us * 72818 / 100000` | Now bound-checked every tick (§14.2-14.3) |
| Serialization | `ns2_encode_motion30()` | Lossless LE32 packing, **one function** used for both the transmitted report and the anomaly capture (§14.3) — cannot diverge between the two by construction |
| Enable-state gating | `ns2_imu_enabled`, toggled by `0x0C/0x04` | Gates whether bytes are written to the *transmitted* report; `ns2_motion_tick()` itself runs regardless (2026-07-10, §9) |
| Counters | `ns2_imu_tick` (12-bit, wraps `&0x0FFF`), `count` (4-bit, clamped 1-15) | Deterministic functions of `dt_us`; no independent path for them to diverge from it within a single-threaded, non-reentrant call |

**One self-introduced discrepancy already flagged, not re-litigated here:** §12.5 item 5 (phase
accumulates before `ns2_imu_enabled` goes true, unlike the genuine controller's captured clean
startup state). Still not fixed this pass — observability only, per the task's explicit constraint.
**No other discontinuity source was found by inspection** — the remaining unknowns are empirical
(does this arithmetic, on real hardware with real DualSense noise, ever actually hit the derived
bound?), which is exactly what §14.4's procedure tests.

### 14.2 Anomaly criterion — derived, not chosen

Every value feeding a single phase increment is independently bounded by the code's own explicit
clamps and C's type system:

- `in->gyro[ax]`: `int16_t`, so `|in->gyro[ax]| <= 32768` by type alone, independent of sensor
  behavior — and separately re-clamped to the same range by `ns2_clamp16()` upstream.
- `ns2_gyro_lp[ax]` (EMA, weight 1/4, of that input): by induction — `glp_next = glp + (input -
  glp) >> 2`; if `|glp| <= M` and `|input| <= M`, then `glp_next` stays within `[-M, M]` (shown at
  both extremes: `glp = M ⇒ glp_next ∈ [M/2, M]`; `glp = -M ⇒ glp_next ∈ [-M, -M/2]`), and `glp`
  starts at 0 (within bounds) — so `|ns2_gyro_lp[ax]| <= 32768<<6` **always**, not just typically.
- `ns2_gyro_bias[ax]` (EMA, weight 1/256, of `ns2_gyro_lp[ax]`): identical induction, same bound:
  `|ns2_gyro_bias[ax]| <= 32768<<6`.
- `g = (ns2_gyro_lp - ns2_gyro_bias) >> 6`: by the triangle inequality, `|g| <= 2 * 32768 = 65536`
  in the **worst provable case** (glp and bias at opposite extremes simultaneously) — not expected
  of real sensor data, but not excluded by the code's own logic, so used here rather than a
  tighter, guessed bound.
- `dt_us`: clamped to `[500, 16000]` by `ns2_motion_tick()` itself, a hard ceiling in the code.

```c
#define NS2_MAX_G_MAGNITUDE 65536
#define NS2_MAX_DT_US       16000
#define NS2_MAX_PHASE_DELTA \
    ((int32_t)((int64_t)NS2_MAX_G_MAGNITUDE * NS2_MAX_DT_US * 72818 / 100000))
    // = 763,552,071 raw units ≈ 64.0° — the largest |phase increment| a single tick's
    // arithmetic can produce without a computation defect (overflow, a bypassed clamp,
    // memory corruption). Not a heuristic threshold: exceeding it is proof of a defect in
    // this specific arithmetic, independent of what the phase VALUE is supposed to mean.
```

This is deliberately the **loosest defensible** bound (using the worst-case `|g|` rather than a
tighter "well-behaved" estimate) so a trip is unambiguous evidence of a real defect, not a false
positive from legitimately fast (if unusual) motion.

### 14.3 Instrumentation added (observability only — no generated-motion behavior changed)

- **`ns2_encode_motion30()`** — the phase/accel → 30-byte serialization, extracted out of
  `ns2_build_report()` into its own function, called from both the real report builder and the
  anomaly capture. Same inputs, same code path, every time — this is what makes "does the encoding
  step ever diverge from the transmitted bytes" answerable by inspection (§14.5, outcome 2) rather
  than requiring a separate runtime check that could itself have bugs.
- **`ns2_motion_tick()`** — now checks `|increment| > NS2_MAX_PHASE_DELTA` per axis, every tick.
  On a trip: increments `ns2_anom_seq`, and captures full context into `ns2_last_anom`
  (`ns2_anom_capture_t`, `switch_pro2.h`): raw gyro/accel, corrected `g`, bias, `still`, `dt_us`,
  phase before/after, the delta(s) that tripped it, the timing counters, `ns2_imu_enabled`, and
  the 30-byte encoding this tick *would* produce (via `ns2_encode_motion30()`, always computed,
  regardless of whether the IMU gate is actually open — `motion_len` separately records whether it
  was really transmitted).
- **`ns2_anom_trail[]`** — a 4-entry ring buffer of lightweight per-tick state (raw gyro, delta,
  `still`, `dt_us`), updated every tick regardless of anomaly status, so a capture always includes
  real preceding context ("surrounding" state per the task), not just the offending tick alone.
- **Exposure**: `imu` debug line gains `anom=<count>`; a new `imuanom` CDC command returns the
  full `ns2_last_anom` capture as JSON (large enough to need `out[]`/`CFG_TUD_CDC_TX_BUFSIZE`
  raised from 256 to 1024 bytes). The web UI auto-fetches and displays it whenever `anom`
  increases.
- **Nothing here alters `ns2_phase[]`, the bias tracker, the stillness gate, or what gets
  transmitted when the IMU is enabled** — every new read is of already-computed values; the only
  new writes are to the anomaly-capture structures themselves.

**Why new instrumentation was needed, not redundant:** the existing `phase=[...]` field (§12.6)
only shows a snapshot every ~70 ms poll — a single-tick transient spike between polls would never
be seen, and even if seen, "that number looks big" isn't a rigorous discontinuity proof. Nothing
in the prior instrumentation could distinguish "impossible per this arithmetic" from "just larger
than I expected."

### 14.4 Hardware test procedure (stationary)

1. Flash the current build (`build/pico_w/PicoSwitchWGA-pico_w.uf2` or the `pico2_w` equivalent).
2. Power-cycle the dongle (clears `ns2_anom_seq` and all tracker state to a clean baseline).
3. Pair/connect a DualSense as usual.
4. Enter config mode (hold BOOTSEL ~5 s), open the config web page.
5. Set the controller down, fully stationary, and leave it — **several minutes**, not seconds
   (the bias tracker's `>>8` time constant is on the order of a second to converge; a longer
   window gives a real chance to catch an infrequent event, not just the initial settling).
6. Watch the `imu` line's `anom` counter. If it increases at any point, the anomaly box
   auto-populates below it with the full capture — record/screenshot the JSON.

### 14.5 Outcome mapping — apply once real data exists, not before

| Observed | Interpretation |
|---|---|
| **`anom` increases; `imuanom` shows `\|delta\|` on some axis exceeding the derived ~763.5M (~64°) bound** | **Outcome 1 — local phase discontinuity.** Proven defect inside `ns2_motion_tick()`'s own arithmetic or its inputs. Next: read the `trail` for what preceded it (a raw `gyro` spike → upstream/BT noise; `dt_us` near 16000 → a timing gap/stall; `g`/`bias` inconsistent with the trail → overflow or corruption). Do not fix blind — correlate first. |
| **`anom` stays 0 throughout, but a separate in-game test still shows the console jumping** | **Outcome 2 — continuous phase, discontinuous final encoding.** With the current code, this is **structurally near-impossible to produce without memory corruption**, not merely unobserved: `ns2_encode_motion30()` is the one and only function that turns `phase[]`/`accel[]` into bytes, used identically for the transmitted report and the would-be anomaly capture, with no conditional logic that could diverge. If genuinely observed, it specifically implicates something *outside* this arithmetic (a stray write into `p[0x0F..0x2C]` elsewhere, stack corruption) rather than an encoding bug in this function. |
| **`anom` stays 0 throughout, and the console still jumps in-game** | **Outcome 3 — the generated report is continuous but the console still jumps.** With outcome 2 structurally excluded, this is the remaining, most informative result: the discontinuity is not in this device's own computation or serialization. It's either (a) the console correctly receiving a *smooth* value with the *wrong semantic* (the accumulator-vs-bounded-sample question, §12.3), or (b) something in USB transit this instrumentation cannot see. Distinguishing (a) from (b) needs either a genuine report-0x09 capture (`Dycool/Usb-relay-for-NS`, if it ever works, §12.7) or the fixed-tilt experiment (`report-0x09-motion.md` item 2). |

**No claim is made about which row applies** — this build has not yet run on hardware with this
instrumentation. §14.4 is how to find out.

## 15. Files touched (this section)
- `include/switch_pro2.h` — `ns2_anom_trail_t`/`ns2_anom_capture_t` types, `NS2_ANOM_TRAIL`,
  `ns2_dbg_motion_anomaly()`; also now the single home for the `ns2_dbg_*` getter prototypes
  (previously hand-duplicated as `extern` in `config.c`).
- `src/switch_pro2/switch_pro2.c` — `ns2_encode_motion30()` (extracted, shared, de-duplicated);
  `NS2_MAX_PHASE_DELTA` derivation; anomaly detection + capture in `ns2_motion_tick()`.
- `src/config.c` — includes `switch_pro2.h`; `imu` line gains `anom=<count>`; new `imuanom`
  command; `out[]` raised 256→1024 bytes.
- `include/tusb_config.h` — `CFG_TUD_CDC_TX_BUFSIZE` raised 256→1024 to carry the larger reply.
- `web/index.html` (+ regenerated `src/web_disk.h`) — anomaly box, auto-fetch on `anom` change.
- Both boards build clean (`build.ps1`). **No change to generated motion behavior** — phase
  integration, the bias tracker, the stillness gate, and the transmitted bytes are byte-for-byte
  identical to before this pass; only new observability was added.
