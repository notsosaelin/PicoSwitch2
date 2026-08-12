# Genuine/generated motion hybrid harness — 2026-08-01

## Purpose

The fully synthetic DualSense length-`0x28` recipe is internally coherent but
hardware-rejected. The next observation point begins with a controller-authored
packet that the Switch already accepts and replaces exactly one semantic group
at a time. This is a live protocol bisection, not another full-packet guess.

The analogy is mechanical fitment: byte widths prove the flange bolts line up;
source epoch, physical pose, cadence, and controller-private state prove the
rest of the exhaust occupies the same space while the engine is running.

## Existing capture harness

No new raw transport recorder is required:

- `blecap` retains the complete genuine 63-byte GATT report, handle, direction,
  original length, and Pico timestamp.
- `motionpair` retains the exact controller-authored `0x1E` or `0x28` motion
  PDU beside the latest raw and calibrated DualSense gyro/acceleration sample,
  controller sensor timestamp, Pico timestamp, sequence, and calibration state.
- `tools/motion_lab.ps1` discovers UART through `PicoSwitch2Lab.psm1`, requires
  a live genuine motion source, rejects drops, analyzes the capture, generates
  C/JSON fixtures, hashes the bundle, and now emits `motion.fitment.json`.
- `tools/ns2_motion_reference.py` independently decodes all 320 bits of every
  observed mode-3 `0x28` cadence layout. The original bytes remain the primary
  evidence.

`motionpair` contains every motion byte. The remaining bytes in the complete
63-byte report are buttons, sticks, battery, and other ordinary input state;
use `blecap` when those are relevant. The two capture modes intentionally share
one retained ring and are mutually exclusive to preserve RAM and zero-loss
behavior.

## Offline fitment jig

`tools/ns2_motion_hybrid.py` defines a complete, non-overlapping bit partition:

| PDU | Groups |
|---|---|
| `0x1E` | timing, temperature, prefix, flags/reserved, acceleration, tail |
| mode-3 `0x28` | timing, status, packing, prefix, acceleration history, gyro history, tail |

Commands:

```powershell
python tools/ns2_motion_hybrid.py capture <motionpair-or-blecap.jsonl> `
  --output motion.fitment.json
python tools/ns2_motion_hybrid.py inspect <PDU_HEX>
python tools/ns2_motion_hybrid.py fit <GENUINE_HEX> <DONOR_HEX>
python tools/ns2_motion_hybrid.py splice <GENUINE_HEX> <DONOR_HEX> `
  --group accel
```

Structural fit requires equal packet length and, for `0x28`, equal mode-3
layout and status. Splicing copies only named masks and verifies that every
unselected bit remains identical to the genuine base. It explicitly reports
`physical_alignment_proven=false`: structural fit alone cannot establish that
two controllers describe the same instant and pose.

`src/bt_hid/motion/ns2_motion_hybrid.c` is the matching transport-independent
firmware primitive. On any bad length, mode, layout, status, or group it leaves
the destination untouched.

## Implemented live fitment jig

The live path is built and its first three bisections are hardware-observed:
the byte-identical control, acceleration-only, and gyro-only modes preserved a
stationary Splatoon camera. The first prefix-only run exposed a harness defect
rather than a large numeric prefix error; that result and correction are
recorded below.

- `ns2_motion_hybrid_projector.c` owns an independent DualSense translator and
  a 32-sample timestamped donor ring on BT/core 1.
- A strict-unit-valid genuine length-`0x1E` packet supplies the diagnostic
  chart anchor through the already validated translated-source approximation.
  This is not promoted to an exact model of Nintendo's private chart: genuine
  transition packets with retained energy above one are rejected and alignment
  waits for another carrier. Alignment also requires calibrated DualSense data,
  acquired stationary bias, fresh source, and matching gravity
  direction/magnitude. Acceleration/gyro modes keep the genuine `0x1E`.
  Prefix mode instead owns orientation across both interleaved lengths after
  alignment, because a `0x28` prefix and the surrounding `0x1E` carriers are
  one stateful orientation sequence.
- Only genuine high-rate, mode-3 `0x28` packets are eligible. Normal/catch-up
  layouts remain byte-identical genuine fallbacks because paired evidence for
  their donor projection is insufficient.
- The donor window is anchored to the genuine packet's Pico arrival time and
  genuine elapsed count. It does not slide backward to the latest DualSense
  sample.
- The projector retains genuine tick, elapsed, status, packing, and temperature
  tail, builds all donor fields coherently, then the splicer copies only the
  requested physical group.
- `ns2_native_motion.c` stores base and candidate in separate fields of the
  cross-core snapshot. USB/core 0 selects the candidate only while its exact
  mode is still requested; `off` therefore restores opaque genuine bytes
  immediately.
- Source identity is hard-gated to Nintendo `057E:2069`. Physical accel/gyro
  groups remain freshness/sequence gated and fail to untouched genuine data.
  Once prefix ownership is aligned, short donor scheduling gaps hold the latest
  donor orientation instead of alternating back to a moving genuine history.
  Disconnect/reset drops ownership and requires a new genuine anchor.

The shared retained capture ring has an exclusive `motion_hybrid` mode. Each
record contains the complete genuine base, emitted XOR, selected mask, source
age/sequence, calibration/pose state, changed-bit count, and fallback reason.
`read_uart_diag.ps1` drains this framing losslessly and always requests
`motionhybrid off` before closing a one-shot capture.

## Live harness design

The first live image must retain genuine timing and ownership. Do not run the
synthetic scheduler beside it. At each controller-authored native PDU:

1. genuine Pro Controller 2 PDU is the immutable base;
2. latest calibrated DualSense source is snapshotted on the same Pico clock;
3. a donor group is projected into the **base PDU's** layout, tick window and
   epoch — never into an independent cadence;
4. the C splicer copies exactly the selected group;
5. invalid physical-group alignment outputs the untouched genuine packet and
   counts the fallback; an acquired orientation group is held across short
   donor gaps so its source cannot alternate packet by packet;
6. capture records the genuine base, emitted result, selected mask, source age,
   calibration state and failure reason. Donor-selected bits are recoverable
   from `base XOR result`, avoiding a larger capture-ring allocation.

Initial UART surface, all default-off:

```text
motionhybrid status
motionhybrid off             # ordinary production ownership
motionhybrid genuine         # instrumented byte-identical positive control
motionhybrid accel           # replace only packed acceleration samples
motionhybrid gyro            # replace only packed gyro samples
motionhybrid prefix          # replace the coherent 0x1E + 0x28 orientation path
motionhybrid imu             # acceleration + gyro, genuine prefix
motionhybrid all             # only after every smaller group passes
motionhybrid capture start
motionhybrid capture dump
motionhybrid capture read
```

The mode commands also accept `motionhybrid mode <name>`. A complete bounded
experiment is normally run through:

```powershell
.\tools\motion_lab.ps1 -Scenario <slug> -Action '<one condition>' `
  -HybridMode genuine -Ready
```

The runner records before/after diagnostics, uses one already-open UART for
mode/capture/disable, rejects drops, runs `ns2_motion_hybrid.py audit-capture`,
generates JSON/C fixtures, hashes every artifact, and records Git/build
provenance. The auditor reconstructs `output = base XOR output_xor`, proves
fallbacks are byte-identical, and rejects any applied bit outside the selected
semantic mask.

Timing/status/packing/tail remain genuine during the first bisection. They are
not useful first variables: native passthrough already proves them, and changing
timing simultaneously changes the cadence layout. Length-`0x1E` remains
entirely genuine for acceleration, gyro, and IMU modes; prefix/all modes must
replace its orientation mask as part of the same carrier history.

## Mandatory live gates

- genuine source identity is Nintendo `057E:2069` and owns output slot 0;
- donor provenance is the DualSense decoder, never late SDP identity alone;
- both sources are present and the DualSense calibration state is ready;
- donor age is within the source-rate budget and sensor sequence advances for
  physical accel/gyro replacement; an already-owned orientation may hold its
  last donor sample through a short scheduling gap;
- genuine PDU is mode 3 and its status agrees with its elapsed-selected layout;
- source pose/alignment preflight passes before replacing prefix or gravity;
- output is captured byte-exactly with zero dropped/overwritten records;
- before orientation ownership, any failure emits the untouched genuine PDU;
  after ownership, a short source gap holds donor orientation rather than
  mixing histories;
- UART `off` restores ordinary opaque passthrough immediately.

The first hardware control is `motionhybrid genuine`. It must be byte-identical
to ordinary native passthrough and preserve stationary aim. Only then does one
group change per run. No controller spinning is required for the first
stationary localization: the known failure occurs at rest.

## 2026-08-01 live bisection result

The genuine control produced 95 byte-identical records and no console change.
Acceleration-only produced 14 applied high-rate records with zero saturation or
drops and remained stable. Gyro-only produced two clean runs; synchronized
Display 3 video measured less than one pixel of coherent displacement through
the applied window.

The original prefix mode applied 19 generated `0x28` prefixes while passing 68
genuine `0x1E` carriers through. Display 3 moved violently during the exact
650 ms window. Offline history decode showed the generated prefix was only
`0.001..0.072` degrees from the genuine one. The large response therefore did
not localize a gross prefix projection error: it localized repeated switching
between two absolute-orientation histories. Five stale-donor `0x28` fallbacks
introduced the same discontinuity inside the window.

The corrected harness treats orientation as a sequence-wide group. Prefix mode
now splices the aligned donor prefix into both `0x1E` and high-rate `0x28`, and
holds the most recent donor orientation through a short report gap. A new host
regression proves that both lengths change only their prefix masks and never
fall back to a second orientation source after ownership. This correction is
host/build validated but was not flashed before the campaign closed.

## Campaign closure — 2026-08-01

The maintainer deliberately stopped translated-`0x28` work after reviewing its
likely product value. This is not an unresolved release blocker:

- production DualSense/Edge `0x1E` motion is hardware-validated and feels
  correct in real gameplay;
- genuine Pro Controller 2 `0x1E` and `0x28` already pass through opaquely;
- both genuine lengths share one native clock and orientation trajectory;
  `0x28` adds cadence-dependent sample history rather than a distinct or
  intrinsically more accurate motion mode;
- the remaining unknown is controller-private sequence/filter/FIFO behavior,
  not a missing bit range that another ordinary movement capture will reveal.

The harness, zero-loss captures, fixture generator, and corrected prefix owner
are preserved because they are a reusable genuine/generated fitment instrument.
They remain default-off. No prefix retest, generator tuning, or new physical
motion capture should be requested unless a concrete `0x1E` deficiency appears
or a genuinely new observation point can answer the private state semantic.

## Evidence boundary

The current host gate proves exact field ownership, structural fit,
untouched-bit preservation, deterministic bias/pose alignment, genuine-clock
windowing, immutable status/timing/tail, physical-group stale/repeated/layout
fallbacks, coherent cross-length prefix ownership, and capture/fixture framing.
Hardware has validated the genuine, acceleration, and gyro bisections. The
corrected coherent prefix path is not hardware-validated and is intentionally
not queued for testing. No result here promotes synthetic `0x28` to production.
