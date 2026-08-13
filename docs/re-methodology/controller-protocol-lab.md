# PicoSwitch2 controller protocol laboratory

Status: 🟢 Infrastructure active 2026-08-01; the genuine-Pro2 motion campaign is closed and the
next phase is evidence-first genuine-controller command discovery. Audio and firmware-tap hardware
campaigns remain pending.

This is the shared workflow for protocol work that is not specific to NFC.
The NFC laboratory remains authoritative for amiibo. The common rule is the
same: one physical action, one intended variable, a complete artifact bundle,
and permanent replay coverage.

## Shared components

| Component | Purpose |
|---|---|
| `tools/PicoSwitch2Lab.psm1` | Port discovery, UTF-8 output, Git provenance, hashes and `picoswitch2-lab/v1` manifests |
| `tools/capture_to_fixture.py` | Convert a zero-loss UART trace, BLE capture or paired-motion capture to deterministic JSON and C fixtures |
| `tools/ns2_command_atlas.py` | Aggregate observed command/subcommand shapes across lossless console `trace` and controller `blecap` files with explicit provenance |
| `tools/motion_lab.ps1` | Capture one short stationary native-motion condition, analyze it and compare it with a baseline |
| `tools/audio_lab.ps1` | Sample audio diagnostics without changing codec, route or stream state |
| `tools/ns2_firmware_update.py` | Fail-closed host model and reassembler for the command-`0x0D` update sequence |
| `tools/firmware_lab.ps1` | Package a complete update trace into a verified local image, metadata and fixtures |

Repository-local Codex skills under `.agents/skills/` enforce the protocol,
motion, audio and firmware workflows. The existing Claude NFC skill remains
separate because NFC has additional image, crypto and persistence rules.

## Artifact contract

Every domain runner creates a manifest and the artifacts applicable to its
evidence source:

```text
dumps/experiments/<timestamp>-<scenario>/
  experiment.json          picoswitch2-lab/v1 provenance and hashes
  diagnostics.before.json  for live diagnostic runners
  diagnostics.after.json   for live diagnostic runners
  <domain>.raw.jsonl       when the domain has a wire capture
  <domain>.analysis.txt
  <domain>.analysis.json
  <domain>.fixture.json
  <domain>.fixture.h
```

The manifest records the dirty worktree explicitly. A dirty tree is permitted
during research, but it prevents a later reader from incorrectly attributing
the flashed binary to the last commit.

`capture_to_fixture.py` rejects a capture whose final record reports drops or
overwrites. It preserves declared and captured lengths separately. A generic
trace can therefore remain useful for command-shape inventory even when its
24-byte retained prefix is insufficient for payload replay.

## Command atlas

Build a capture-derived inventory without assigning semantics to unknown
payloads:

```powershell
python tools/ns2_command_atlas.py <trace-a.jsonl> <blecap-a.jsonl> `
  --output build/command-atlas.md

python tools/ns2_command_atlas.py <captures...> --json `
  --output build/command-atlas.json
```

The atlas records command/subcommand, request and response counts, declared
lengths, retained lengths, payload hashes, personalities, source hashes,
capture boundary, link/header transport, and GATT handles. It rejects missing
loss metadata instead of assuming a legacy capture is complete. Names come
from the current command audit. A name such as `audio_candidate` remains a
hypothesis until a controlled experiment resolves it.

## Historical magnetic-stimulus campaign

The former G6/G7/G8 aliases are not a reference vector. Their bit ranges cross
packed gyro and acceleration samples in length-`0x28`, and the controlled
magnet campaign found no polarity or distance response. The commands below are
retained only as a worked example of a one-variable A/B/A experiment; do not
repeat it as a motion hypothesis.

```powershell
./tools/motion_lab.ps1 -Scenario pro2-mag-baseline `
  -Action 'Controller stationary; no magnet or magnetic tool nearby' `
  -Stimulus none

./tools/motion_lab.ps1 -Scenario pro2-mag-sham-50mm `
  -Action 'Controller stationary; nonmagnetic sham fixed 50 mm from the marked face' `
  -Stimulus sham -DistanceMm 50 `
  -Baseline dumps/experiments/<baseline>/motion.raw.jsonl

./tools/motion_lab.ps1 -Scenario pro2-mag-ceramic-face-a-50mm `
  -Action 'Controller stationary; ceramic Face A fixed 50 mm from the marked top-center point' `
  -Stimulus ceramic-face-a -DistanceMm 50 `
  -Hypothesis 'G6/G7/G8 respond to a magnetic field while IMU and input remain fixed' `
  -Variable 'replace the sham with ceramic Face A at the same 50 mm position' `
  -Expect 'reference-vector direction or magnitude changes beyond baseline spread' `
  -Baseline dumps/experiments/<baseline>/motion.raw.jsonl
```

Repeat with the opposite face at the same position, additional fixed distances,
and a removal/recovery capture. Do not touch the controller during a stationary
condition. Log input/gyro/acceleration before and after so a Hall-effect stick
or trigger disturbance cannot be mistaken for a reference-vector response.

The runner limits capture to the retained ring, requires zero drops and at
least one length-`0x28` record, emits one decoded CSV row per `0x28`, invokes
`ns2_magprobe compare`, and generates replay fixtures. It also refuses to start
unless the native-motion preflight reports an active genuine PID and can resume
offline packaging after an interrupted capture:

```powershell
./tools/motion_lab.ps1 -Scenario <same-scenario> `
  -ExistingCapture dumps/experiments/<run>/motion.raw.jsonl
```

For a stimulus captured at actual time fraction `F` between the baseline
(`0`) and recovery (`1`), use the A/B/A analyzer:

```powershell
python tools/ns2_magprobe.py aba `
  dumps/experiments/<baseline>/motion.raw.jsonl `
  dumps/experiments/<stimulus>/motion.raw.jsonl `
  dumps/experiments/<recovery>/motion.raw.jsonl `
  --fraction F --json
```

It interpolates quaternion pose with SLERP and linearly interpolates
acceleration, the historical alias ranges, and every retained unknown byte/bit
before reporting the drift-adjusted stimulus residual. The completed matrix
resolved no external magnetic response; exact packed-IMU decoding later
explained why the aliases were not independent lanes. Methods, commands,
results and limitations are in
[`../experiments/pro2-magnetic-stimulus-matrix-2026-07-29.md`](../experiments/pro2-magnetic-stimulus-matrix-2026-07-29.md).

## Next genuine-controller discovery pass

Start offline. Run `ns2_command_atlas.py` over existing complete, zero-loss
genuine-controller traces and record capture provenance for every observed
command/subcommand pair. Use the atlas to select one missing behavior, not to
assign semantics from payload shape alone. Prefer passive or reversible
boundaries already exposed by the adapter: initialization, reconnect/power,
player LED, rumble, headset/audio control, and native NFC.

The 2026-08-13 offline audit closes the schema boundary without inventing a
protocol answer. The atlas accepts console-side `trace` and controller-side
`blecap` JSONL, rejects captures without explicit zero-loss terminal metadata,
and retains capture boundary, link/header transport, handle, completeness, and
source hashes. The current corpus contains 46 admissible `trace` files and 30
admissible `blecap` files. It still finds only two BLE files with framed command
traffic, both the same variant-8 initialization sequence. See
[`../switch2/controller-command-atlas.md`](../switch2/controller-command-atlas.md)
for exact coverage and the ranked gaps. Capture boundary does not by itself
prove genuine-hardware authorship; use the associated experiment provenance.

Only request a new hardware action when the atlas identifies one exact missing
wire/state discriminator. Use the existing tracer and
`PicoSwitch2Lab.psm1`; add a generic runner only if repeated experiments expose
a packaging gap. Firmware update remains opportunity-driven: prepare the
research-only full-payload sink, then wait for a real console update rather
than fabricating or replaying proprietary firmware traffic.

## Audio regression and microphone prerequisite

Create the immutable speaker baseline before microphone work:

```powershell
./tools/audio_lab.ps1 -Scenario ds5-speaker-golden `
  -Action 'Play the controlled reference signal through the DualSense headset' `
  -DurationSeconds 60 -SampleSeconds 5
```

The default path is observational. It does not send `audio clear`,
`ds5codec lock`, `pro2audio`, route, microphone or stream commands.
`-ResetCounters` is explicit and must be named as the experiment's variable.
Use `-Baseline <audio.samples.json>` for later comparisons.

`audio headset` is a read-only UART diagnostic reporting the current normalized
state (`none`, `headphones`, or `headset`) and controller VID/PID. Physically
validate it with known TRS headphones and a known TRRS mic headset before
allowing it to gate microphone traffic.

The analyzer treats new PCM drops or Opus errors as failures. Timing maxima are
reported as investigation evidence, not automatically called audible failure.

## Firmware update capture

The host model implements:

```text
0x0D/01 → /02 → /03 → /04* → /05 → /06 → /07
```

It validates the known failsafe addresses, declared size, length-prefixed USB
chunks, observed byte count and CRC32. Its USB envelope and request layouts are
fixture-checked against the examples mirrored in
`nso-gc-refs/switch2_controller_research/commands.md`. Analyze only a full
retained trace:

```powershell
python tools/test_ns2_firmware_update.py -v
./tools/firmware_lab.ps1 -Trace <full-command-0x0d-trace.jsonl>
```

The current generic protocol trace retains only the first 24 bytes of a long
command. The reassembler deliberately rejects a truncated `0x0D/04` and
requires the future dedicated on-device sink.

The on-device sink remains a staged research task:

1. audit the current image's candidate space with
   `tools/audit_firmware_capture_space.py`, then reserve that region in the
   research build's linker layout with compile-time overlap assertions;
2. compile it only in a research build;
3. journal and progressively flush 4 KiB sectors;
4. never use Nintendo's target address as a Pico address;
5. export over CDC only after the console session;
6. keep raw proprietary images local and commit only hashes/metadata.

## Offline verification

```powershell
./tools/test_PicoSwitch2Lab.ps1
python tools/test_capture_to_fixture.py -v
python tools/test_ns2_command_atlas.py -v
python tools/test_audio_lab_analyze.py -v
python tools/test_audit_firmware_capture_space.py -v
python tools/test_ns2_firmware_update.py -v
python tools/test_repo_skills.py -v
python tools/test_ns2_magprobe.py
```

These tests validate the laboratory and parsers. They are not controller or
console hardware validation.
