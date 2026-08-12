---
name: picoswitch2-motion-lab
description: Capture and analyze PicoSwitch2 native motion, DualSense translation, length-0x1E/0x28 PDUs, packed IMU lanes, live genuine/generated group substitution, gyro drift, axis mapping, and motion regressions.
---

# PicoSwitch2 motion lab

## Preserve the production boundary

Read `AGENTS.md`, `docs/switch2/report-0x09-motion.md`,
`docs/experiments/ds5-motion-hybrid-harness-2026-08-01.md`, and
`docs/experiments/refuted-hypotheses.md`.

Production DualSense/Edge motion uses length `0x1E`. The length-`0x28` PDU is
a packed multi-sample IMU payload plus a mode-3 orientation carrier; there is
no magnetometer lane. Never restore the hardware-refuted static-template
generator or describe the former G6/G7/G8 alias as a physical sensor.

The translated `0x28` campaign was deliberately deferred on 2026-08-01. The
live hybrid remains a forensic fitment tool, not active production work. Do not
request its remaining prefix retest or resume generator tuning unless the
maintainer explicitly reopens the campaign because `0x1E` has a measured
shortcoming or a new observation point can answer the missing semantic.

## Choose the observation point

Use passive `motionpair` when the question is about genuine layout, timing, or
source correlation. It records exact genuine PDUs beside the latest DS5 sample
on the Pico clock and cannot change console output.

If the deferred campaign is explicitly reopened, use the live hybrid only when
the hypothesis names exactly one substituted physical group. The genuine
Nintendo `057E:2069` packet remains the immutable base; timing, status,
packing, and tail stay genuine. Every failed donor gate must emit the
byte-identical base.

## Passive capture

```powershell
.\tools\motion_lab.ps1 -Scenario <slug> `
  -Action '<prepare one controlled condition>' `
  -Hypothesis '<claim>' -Variable '<one change>' `
  -Expect '<wire discriminator>'
```

The runner requires a live native source, zero drops, and at least one
length-`0x28` PDU; it analyzes the corpus, generates C/JSON fixtures, and hashes
the complete bundle.

## Live fitment bisection

The maintainer must explicitly confirm the genuine Pro Controller 2,
DualSense, console, and UART are connected and stable. Run:

```powershell
.\tools\motion_lab.ps1 -Scenario <slug> `
  -Action '<one stationary or named movement condition>' `
  -Hypothesis '<claim>' -Variable '<one substituted group>' `
  -Expect '<console and capture discriminator>' `
  -HybridMode <genuine|accel|gyro|prefix|imu|all> -Ready
```

Always begin a new image with `genuine`. It is the instrumented positive
control and must remain byte-identical with correct input and motion. Then run
`accel`, `gyro`, and `prefix` separately. Do not use `imu` or `all` until every
smaller constituent has passed independently.

The PC reader keeps one UART open for mode, capture, drain, and disable. It
always requests `motionhybrid off` at the end. `ns2_motion_hybrid.py
audit-capture` reconstructs `output = base XOR output_xor`, rejects drops,
proves fallbacks stayed exact, and rejects any applied bit outside the selected
semantic group. A capture that fails the audit cannot become a fixture.

Useful direct diagnostics:

```text
motionhybrid status
motionhybrid genuine|accel|gyro|prefix|imu|all|off
motionhybrid capture start|dump|read
```

## Validate before hardware

Require the independent inverse codec, captured-fixture replay, signed endpoint
tests, timing invariants, physical-coherence tests, and fail-closed live
projector coverage. Never promote a diagnostic encoder into production from one
successful gesture.

Run:

```powershell
.\build\host-tests\build-host-test-ns2-motion-pdu.exe
.\build\host-tests\build-host-test-ns2-motion-pdu40.exe
.\build\host-tests\build-host-test-ns2-ds5-motion.exe
.\build\host-tests\build-host-test-ns2-ds5-motion40.exe
.\build\host-tests\test_ns2_motion_hybrid.exe
.\build\host-tests\test_ns2_motion_hybrid_projector.exe
python tools\test_ns2_motion_packet.py
python tools\test_ns2_motion_carrier.py
python tools\test_ns2_motion40_coherence.py
python tools\test_ns2_motion_hybrid.py
```

State whether results are offline, build-only, or hardware-confirmed. Record a
negative hardware result in `docs/experiments/refuted-hypotheses.md`; do not
retune known fields or request another movement capture without first changing
the observation point.
