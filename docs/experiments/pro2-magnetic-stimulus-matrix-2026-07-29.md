# Genuine Pro Controller 2 magnetic-stimulus matrix — 2026-07-29

## Result

No tested external magnetic condition produced a polarity-dependent or distance-scaling response
in the genuine Pro Controller 2 native length-`0x28` stream.

This is direct hardware evidence against interpreting G6/G7/G8 as a simple externally responsive
magnetic-field vector.

**Post-campaign correction:** exact reference-PCAP analysis later the same day established that
G6/G7/G8 are not independent lanes. Their bit ranges cross packed catch-up gyro and acceleration
samples. This explains the negative result and supersedes the former
“controller-processed reference vector / candidate second quaternion” label. See
[`pro2-raw-native-motion-pcap-2026-07-29.md`](pro2-raw-native-motion-pcap-2026-07-29.md).

The experiment does not prove that the controller lacks a physical magnetometer. Only one
controller, one top-center field geometry, approximate distances, and uncalibrated consumer magnets
were tested.

## Hardware and capture path

- Genuine Switch 2 Pro Controller, Bluetooth PID `057E:2069`
- PicoSwitch2 Pico 2 W adapter connected to a real Switch 2
- UART bridge on `COM11`
- Passive `motionpair capture`; production motion transport was not changed
- One Harbor Freight `8 × 3.2 mm` neodymium disc
- One Harbor Freight `47.6 × 22.2 × 9.5 mm` ceramic block
- Controller remained on a fixed support; the interpolated genuine `0x1E` acceleration supplied the
  physical-movement gate

Every usable capture had zero ring drops. One empty capture was excluded: the controller battery
had expired before its preflight. That event motivated a runner preflight that now rejects an
inactive native-motion source before consuming an experiment condition.

## Method

Each attributable condition was stored under `dumps/experiments/` as a
`picoswitch2-lab/v1` bundle containing:

- before/after UART diagnostics;
- raw JSONL;
- text, JSON, and CSV analysis;
- deterministic JSON/C fixtures;
- Git provenance and SHA-256 hashes.

The controller was never moved intentionally. Stimulus captures were replicated, followed by
no-magnet recovery. `ns2_magprobe.py aba` linearly interpolated between the no-magnet A/A-prime
anchors at the stimulus's actual time fraction. It then measured the stimulus residual against
that expected state. This avoids treating the native quaternion's substantial time drift as
physical fixture movement or magnetic response.

The acceptance criteria were:

1. zero capture loss;
2. acceleration pose gate passes;
3. response exceeds a matched no-magnet A/B/A residual;
4. response reproduces under unchanged stimulus;
5. response increases at shorter distance; and
6. opposite magnet faces reverse or materially redirect the response.

## Conditions

### Retained A/B/A anchors

Folder names below are relative to `dumps/experiments/`. Fractions are the stimulus capture time
between baseline `0` and recovery `1`, calculated from the mean Pico `t_us` of each retained
capture with 32-bit rollover handling. They are the exact values passed to
`ns2_magprobe.py aba --fraction`.

| Sequence | Baseline | Stimulus | Recovery | Fraction |
|---|---|---|---|---:|
| Ceramic Face A 100 mm #1 | `20260729-122701-pro2-mag-ceramic-confirmation-immediate-baseline` | `20260729-122748-pro2-mag-ceramic-confirmation-face-a-100mm` | `20260729-122909-pro2-mag-ceramic-confirmation-immediate-recovery` | `0.3690921671` |
| Ceramic Face A 100 mm #2 | same | `20260729-122817-pro2-mag-ceramic-confirmation-face-a-100mm-replicate` | same | `0.5918102408` |
| Ceramic Face B 100 mm #1 | `20260729-122909-pro2-mag-ceramic-confirmation-immediate-recovery` | `20260729-123029-pro2-mag-ceramic-polarity-face-b-100mm` | `20260729-123136-pro2-mag-ceramic-polarity-face-b-immediate-recovery` | `0.5475743687` |
| Ceramic Face B 100 mm #2 | same | `20260729-123057-pro2-mag-ceramic-polarity-face-b-100mm-replicate` | same | `0.7325945742` |
| No-magnet sham | `20260729-123322-pro2-mag-no-magnet-sham-a` | `20260729-123339-pro2-mag-no-magnet-sham-b` | `20260729-123357-pro2-mag-no-magnet-sham-recovery` | `0.4956136140` |
| Ceramic Face A 50 mm #1 | `20260729-123357-pro2-mag-no-magnet-sham-recovery` | `20260729-123611-pro2-mag-ceramic-face-a-top-center-50mm` | `20260729-123715-pro2-mag-ceramic-face-a-50mm-immediate-recovery` | `0.6753299918` |
| Ceramic Face A 50 mm #2 | same | `20260729-123637-pro2-mag-ceramic-face-a-top-center-50mm-replicate` | same | `0.8058839872` |

The earlier neodymium and initial 100 mm ceramic runs are retained alongside these bundles. They
were useful for developing the method, but the compact matrix above carries the attributable
polarity, distance and matched-control result.

### Neodymium disc

- Face A at 50 mm: no detectable change; removal and the original control differed by only
  `0.0041 degrees` in G6/G7/G8 direction.
- Face A at an estimated 20–25 mm: unchanged-stimulus variation was `0.0124 degrees`; removal
  changed direction by only `0.0087 degrees`.
- Face A at the closest stable non-contact point, estimated at or below 10 mm: time-adjusted
  stimulus residuals were `0.0283 degrees` and `0.0133 degrees`. They did not form a repeatable
  offset above later matched no-magnet variation.
- The disc briefly snapped to the controller during setup of the closest condition. This was
  recorded as a confound; it was not touching during capture, and acceleration remained stable.

### Ceramic block, Face A, 100 mm

The first long-anchor sequence appeared promising: two time-adjusted G6/G7/G8 residuals were
approximately `0.0391 degrees`. A tighter confirmation sequence then produced:

| Capture | G6/G7/G8 residual | World-frame residual | Accel residual | Gates |
|---|---:|---:|---:|---|
| Face A 100 mm #1 | `0.0620 deg` | `0.5602 deg` | `0.0422 deg` | pass |
| Face A 100 mm #2 | `0.0560 deg` | `0.5890 deg` | `0.0487 deg` | pass |

Unresolved byte `p17` moved by `1.13–1.77` pooled standard deviations in those two captures. This
was retained as a candidate, not promoted to a field decode.

### Ceramic block, Face B, 100 mm

Flipping the broad face did not reverse the result:

| Capture | G6/G7/G8 residual | World-frame residual | Accel residual | Gates |
|---|---:|---:|---:|---|
| Face B 100 mm #1 | `0.0563 deg` | `0.4596 deg` | `0.0354 deg` | pass |
| Face B 100 mm #2 | `0.0357 deg` | `0.3646 deg` | `0.0297 deg` | pass |

The mean Face-A and Face-B residual vectors were only `3.18 degrees` apart rather than oppositely
directed. `p17` remained positive instead of reversing.

### Matched no-magnet sham

Three untouched, back-to-back no-magnet captures produced a drift-adjusted G6/G7/G8 residual of
`0.0652 degrees`, exceeding every 100 mm ceramic result. Acceleration residual was
`0.0153 degrees`, and both pose and fusion gates passed. Thus the apparent 100 mm result was native
nonlinear variation, not evidence of magnetic response.

### Ceramic block, Face A, 50 mm

Reducing distance should have greatly increased a true field-dependent effect. It did not:

| Capture | G6/G7/G8 residual | World-frame residual | Accel residual | `p17` effect |
|---|---:|---:|---:|---:|
| Face A 50 mm #1 | `0.0481 deg` | `0.6164 deg` | `0.0090 deg` | `0.10 sigma` |
| Face A 50 mm #2 | `0.0544 deg` | `0.5189 deg` | `0.0179 deg` | `0.34 sigma` |

The stronger/closer field produced no distance scaling. `p17` also fell below the no-magnet sham's
ordinary variation.

## Interpretation

The complete matrix rejects these working hypotheses for the tested controller and geometry:

- G6/G7/G8 is a simple body-frame magnetic-field vector;
- `p17` is an externally responsive magnetic lane;
- the repeatable 100 mm residual was caused by magnet polarity.

The matrix independently established:

- the live `0x1E` quaternion has substantial native time drift even with gravity/pose unchanged;
- the historical G6/G7/G8 aliases have low within-capture spread but nonlinear inter-capture
  variation;
- pairwise A/B comparison alone can create a false magnetic result;
- the source image's “Magneto Quaternion” label is not proof of raw or directly observable
  magnetometer output.

Future length-`0x28` work should use the packed multi-sample layout and the handle-`0x000A` raw IMU
reference. Repeating stronger uncontrolled magnet tests has lower value unless a separate
magnetometer lane, calibrated field source, second controller unit, or independently identified
sensor location becomes available.

## Tooling improvements made during the campaign

- `motion_lab.ps1 -Ready` supports an already confirmed physical condition.
- `motion_lab.ps1 -ExistingCapture` finishes interrupted packaging without another hardware run.
- The runner rejects an inactive native-motion source before capture.
- Host text output preserves intentional blank lines.
- Single-line UART JSON replies are handled as arrays under PowerShell strict mode.
- The correct read-only `btversion` command replaced the nonexistent `btversion status`.
- `ns2_magprobe.py aba` performs time-fraction-weighted A/B/A drift subtraction across quaternion,
  acceleration, G6/G7/G8, every unexplained byte, and every unexplained bit.
- Host coverage includes A/B/A drift cancellation and unknown-lane residual calculation.
