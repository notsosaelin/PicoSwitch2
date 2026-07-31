---
name: picoswitch2-motion-lab
description: Capture and analyze PicoSwitch2 native motion, DualSense translation, length-0x1E/0x28 PDUs, G6/G7/G8 reference lanes, magnetic perturbation experiments, gyro drift, axis mapping, and motion regressions. Use whenever working on Switch 2 motion semantics or testing a magnet against a genuine Pro Controller 2.
---

# PicoSwitch2 motion lab

## Preserve the production boundary

Read `AGENTS.md`, `docs/switch2/report-0x09-motion.md`,
`docs/switch2/uart-magprobe.md`, and
`docs/experiments/refuted-hypotheses.md`.

Production DualSense/Edge motion uses length `0x1E`. Do not restore the
hardware-refuted static-template `0x28` generator. A future `0x28` generator
must model every console-relevant changing lane coherently.

Call G6/G7/G8 a reference vector unless the experiment proves stronger
semantics. Their exact 22/22/20-bit codec does not prove they are raw
magnetometer samples.

## Capture one stationary condition

Use:

```powershell
./tools/motion_lab.ps1 -Scenario <slug> `
  -Action '<prepare one stationary condition>' `
  -Stimulus <none|sham|north|south|unknown> `
  -DistanceMm <distance> `
  -Hypothesis '<claim>' -Variable '<one change>' `
  -Expect '<wire discriminator>' `
  -Baseline <baseline-motion.raw.jsonl>
```

The retained ring is deliberately short. Capture separate baseline, sham,
distance, and polarity bundles instead of one long multi-phase motion.
`motion_lab` requires zero drops, at least one length-`0x28` PDU, runs
`ns2_magprobe`, emits CSV/JSON, compares the baseline, generates C/JSON
fixtures, and hashes the bundle.

Keep the controller stationary when testing a magnet. Move the stimulus, not
the controller, unless controller rotation is the single named variable.

## Run the magnet matrix

Run these as separate captures:

1. no magnet baseline;
2. nonmagnetic sham at the same location;
3. fixed-distance north pole;
4. same distance south pole;
5. additional fixed distances;
6. removed stimulus/recovery.

Log sticks, triggers, raw gyro, and acceleration. A strong magnet can disturb
other magnetic sensors; a G6/G7/G8 change is not attributable if input or IMU
state changed at the same time.

Evidence for a magnetic source requires controller-stationary response,
polarity sensitivity, distance response, and a sham control. A delayed
relationship to the primary quaternion supports fused-reference semantics
instead. No response is a valid negative result.

## Validate any model offline

Require an inverse codec, captured-fixture replay, signed endpoint tests,
timing invariants, lane covariance checks, and zero unexplained static lanes
before a UART-gated hardware trial. Never promote a diagnostic encoder into
production from one successful motion gesture.

Run:

```powershell
python tools/test_ns2_magprobe.py
./build/host-tests/build-host-test-ns2-motion-pdu.exe
./build/host-tests/build-host-test-ns2-ds5-motion.exe
```

State whether results are offline, build-only, or hardware-confirmed.
