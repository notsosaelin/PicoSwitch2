# PicoSwitch2 Roadmap

> Forward-looking work only. See [`STATUS.md`](STATUS.md) for current behavior and
> [`docs/archive/roadmap-through-2026-07-15.archived.md`](docs/archive/roadmap-through-2026-07-15.archived.md) for
> the completed milestone narrative.

## Current objective: post-v1.5 protocol work

Version `v1.5.0` adds genuine Pro Controller 2 native-motion passthrough, UART protocol tracing,
current Switch 2 controller firmware identities, and hardware-confirmed bonded HOME reconnect with
input, P1 LED, and gyro restoration. Pico 2 W retains live DualSense audio/native haptics; Pico W
retains its validated non-audio behavior.

Post-release work on `ns2-testing` adds hardware-confirmed genuine Pro Controller 2 headphone audio
and calibrated DualSense/DualSense Edge translation to the Switch 2 length-`0x1E` motion carrier.
Those changes are checkpoints, not part of the published `v1.5.0` artifacts.

### Immediate priority: genuine-controller protocol discovery

- [x] Extend `ns2_command_atlas.py` across the controller-side `blecap` schema before requesting
  new hardware activity. The current offline audit admits 46 zero-loss console-side traces and
  30 command/subcommand pairs, but that corpus mixes emulated and relayed transactions. There are
  30 zero-loss BLE captures; only two contain command traffic, both covering the same
  initialization sequence. Keep transport/provenance explicit and separate observed wire shapes
  from inferred names.
- [x] Rank the resulting gaps by user value and captureability. Start with passive or reversible
  genuine-controller surfaces: initialization/state transitions, headset/audio control, rumble,
  LED/player state, reconnect/power, and native NFC. Keep firmware-update capture prepared for the
  next real update opportunity rather than trying to manufacture one.
- [x] Confirm that no additional generic protocol runner is currently justified: the completed
  atlas exposes evidence gaps, but the existing tracer, BLE capture path, and
  `PicoSwitch2Lab.psm1` can package each ranked experiment. Do not create another UART selector
  or a second artifact format unless a later bounded experiment proves that packaging gap.
- [ ] Promote a discovery only after a zero-loss capture, semantic A/B discriminator, active
  protocol-document update, and replay fixture where the transaction is deterministic.

### Deferred: translated DualSense length-`0x28`

The campaign is intentionally paused as of 2026-08-01. Production DualSense/Edge motion remains
the hardware-validated `0x1E` carrier; genuine Pro Controller 2 `0x1E`/`0x28` remains opaque
passthrough. The work still produced complete mode-3 field maps, byte-exact packers, shared-clock
and sample-window rules, a physical-coherence test, and a fail-closed genuine/donor hybrid harness.
Hardware validated the byte-identical, acceleration-only, and gyro-only hybrid modes. The first
prefix run was invalidated by alternating genuine and donor orientation histories; its corrected
sequence-wide ownership is host/build validated but deliberately untested.

Do not spend another flash or physical motion test on this path unless the maintainer explicitly
reopens it because `0x1E` shows a concrete gameplay defect or a new observation point can answer
the missing controller-private filter/state semantic. The detailed record remains in
[`docs/experiments/ds5-pdu40-interleaved-hardware-2026-08-01.md`](docs/experiments/ds5-pdu40-interleaved-hardware-2026-08-01.md)
and
[`docs/experiments/ds5-motion-hybrid-harness-2026-08-01.md`](docs/experiments/ds5-motion-hybrid-harness-2026-08-01.md).

### Release gate

- [x] Pico W release build
- [x] Pico 2 W release build
- [x] Thirty-five host-test executables
- [x] DualSense/Edge input, extra buttons, LEDs, and rumble on hardware
- [x] BOOTSEL double-tap, triple-tap, and hold while DualSense is connected
- [x] NSO GameCube input and rumble on a real Switch 2
- [x] Joy-Con 2 Left/Right sideways mappings and rumble on a real Switch 2
- [x] Match genuine current Pro Controller 2, NSO GameCube, and Joy-Con 2 Left/Right firmware
  identities; hardware confirms all four emulated personalities are up to date
- [x] Configurable Pro2/Joy-Con colors, Sony lightbar matching, and DualSense player dots
- [x] Reconcile documentation and archive superseded development logs
- [x] Complete source-comment evidence cleanup
- [x] Hardware-validate live audio, headset removal/reinsert, native rumble, and bonded reconnect
- [x] Push the release commit and publish both UF2 artifacts (`v1.5.0`, 2026-07-22)

## Next: compatibility closure

Use [`docs/status/compatibility-matrix.md`](docs/status/compatibility-matrix.md) as the single test
matrix. Do not combine mapping changes with unrelated Bluetooth or USB protocol changes.

1. [x] Classify and fix the Joy-Con 2 Left Steam/Windows behavior separately from real-console
   compatibility. The missing Microsoft OS 1.0 Extended Properties descriptor prevented libusb
   from discovering WinUSB interface 1; fresh Windows and Steam UI tests confirm the fix.

Completed closure checks: rumble ON/STOP/reconnect across the tested controller/personality
matrix, individual Joy-Con 2 rumble, triple-tap admission blocking, and explicit re-pairing after
opening a new pairing window are hardware-confirmed. The real Pro Controller 2's physical
L/R/ZL/ZR translation in NSO GameCube output mode is also confirmed.

2. [x] Reverse-engineer the first-generation 8BitDo Ultimate Bluetooth P1/P2 path. Stock
   Bluetooth reports suppress the rear switches; a reversible physical-profile encoding now emits
   independent held signatures, and an OUI-scoped Switch-driver module converts them to GL/GR.
3. [x] Hardware-validate the encoded 8BitDo profile on Pico: P1->GL, P2->GR, simultaneous
   paddles, no injected-button leakage, ordinary input, and console wake. Its inherently slow
   Classic reconnect remains controller-specific; the rejected timing experiment is retained but
   blocked from flashing because it did not improve speed and broke wake.
4. [x] Hardware-validate the Retro Fighters BattlerGC Pro Classic-XInput mapping: direct A/B/X/Y
   labels, centered sticks, D-pad, Start/Select/Home, distinct ZL/Z shoulders, continuous analog
   triggers, click-gated `HOME+B` detents, and GameCube-mode L3/R3 suppression.
5. [ ] Run targeted BattlerGC Pro rumble and reconnect/wake regression checks.
6. [x] Hardware-validate Bluetooth battery passthrough across native-HID, BLE BAS, and unsupported
   Classic fallback families. Software coverage pins DualShock 3/4, DualSense, Switch Pro,
   Wii U Pro, Wiimote, recurring BAS updates/source priority, and every console-native USB
   personality; use [`docs/bluetooth/battery-passthrough.md`](docs/bluetooth/battery-passthrough.md)
   as the physical test matrix.

## Audio and controller-update investigation

- [x] Replace the descriptor-only Pro Controller 2 audio stub with a UAC1 driver that owns both
  isochronous endpoints, consumes 48 kHz speaker PCM, emits continuous silent microphone PCM, and
  implements writable mute/volume controls.
- [x] Confirm on Windows that the audio function starts without Device Manager Code 10 and that
  existing controller behavior remains regression-free.
- [x] Isolate and redesign the Pico 2 W DualSense speaker bridge. The standard Pico 2 W
  configuration uses the controller's effective 45 kHz clock, whole 512-frame PCM
  blocks, SRAM-resident Opus/hot memory routines, and a producer-paced core1
  foreground worker at 300 MHz/1.20 V. Hardware playback is continuous with zero
  PCM drops/errors; LED/BOOTSEL, config persistence, cold boot, and wake regressions
  pass. The 150/200 MHz controls remain below the real-time threshold. See
  [`docs/switch2/audio-passthrough-research.md`](docs/switch2/audio-passthrough-research.md).
- [x] Merge the validated 300 MHz/live-audio configuration into the normal
  `PicoSwitchWGA-pico2_w.uf2` artifact.
- [x] Evaluate a Pico W port at 300 MHz. The fixed-point/XIP image passed build
  and memory gates but barely played audio on hardware, so the experiment was
  rejected and Pico W was restored to its validated non-audio configuration.
- [x] Implement bonded-reconnect audio without requiring a fresh pair. The Pico 2 W
  path extends HID Host's internal 16-bit report-length state only for exact
  DualSense `0x32`/`0x39` shapes; it does not depend on late VID/name resolution
  or inaccessible private L2CAP channel events.
- [x] Implement conditional Pro Controller 2 headset presence from the physical
  DualSense jack; no jack continues to advertise no headset.
- [x] Hardware-validate audio after bonded reconnect; native haptic coexistence
  and repeated headset removal/reinsert are confirmed.
  The latched audible activation and persistent ISO endpoint lifecycle now pass
  repeated jack removal/reinsert plus ordinary controller/dongle reconnect without
  input or rumble regressions. A transient build produced audio with lighter native
  haptics; the recent-flow-gated build restored full legacy rumble but starved audio
  startup. Retest pre-stream `0x39` ownership, continuous audio during rumble, legacy
  Rumble restoration after unplug and audio/native-haptic restoration after
  replug, bonded reconnect audio, and bonded reconnect native rumble are confirmed.
  The capture-derived interval-peak accumulator and waveform-preserving 13/4×
  (3.25×) curve are hardware-confirmed and checkpointed at `2930c90`. The
  standard Pico 2 W build also uses this native renderer without a headset,
  with valid Opus silence and a bounded two-packet STOP tail.
- [x] Run an extended playback and thermal soak of the Pico 2 W 300 MHz audio build. An eight-hour
  Smash session completed with no observed thermal or stability issue (temperature not instrumented).
- [x] Decode and hardware-validate genuine Pro Controller 2 headphone output. The controller consumes
  one 240-byte, 48 kHz stereo Opus/CELT packet per 20 ms interval, split into ordered 120-byte `0x04`
  and `0x02` GATT writes. Live console audio is clean and input, gyro, rumble, headset lifecycle,
  LED, and BOOTSEL behavior remain regression-free. See
  [`docs/switch2/pro2-headset-audio.md`](docs/switch2/pro2-headset-audio.md).
- [ ] Add DualSense microphone report decoding and Opus-to-USB return after speaker playback is
  physically stable.
- [x] Eliminate the Switch 2 “Update this controller” prompt by querying genuine controllers through
  the UART↔BLE bridge and matching their exact coherent tuples. Pro Controller 2 and both Joy-Con 2
  personalities are hardware-confirmed up to date.

## Reliability and maintainability

- [x] Extract the pure BOOTSEL gesture recognizer and cover timer-only, report-only starvation,
  mixed scheduling, one-shot hold, unknown-sample, and timestamp-wrap behavior on the host.
- [x] Keep report-boundary maintenance and timer fallback behavior documented together.
- [x] Resolve late BLE Device Information Service VID/PID on the consuming side: retain immediate
  HID/name binding and notification-first GATT setup, then re-evaluate Xbox BLE, Stadia, and
  MouthPad provisional matches when authoritative VID/PID arrives. Host coverage pins input before
  and after corrective rebinds; the resulting build is hardware-confirmed with Xbox Series BLE.
- [x] Convert config mode from CDC+MSC to CDC-only: remove the `PICOSWITCH` read-only drive,
  embedded FAT image, MSC descriptors/callbacks, and web-disk generator while retaining the
  existing config VID/PID, CDC command protocol, and BOOTSEL personality lifecycle.
- [x] Implement and host/build-test a Config-personality-only BLE management transport: pause
  controller discovery before advertising, classify the incoming Peripheral-role link before HID,
  reuse the existing core-0 parser through a bounded cross-core bridge, and expose only production
  settings/Amiibo commands in the local Web Bluetooth portal.
- [ ] Hardware-validate Config BLE from desktop Chromium and Android if available, including a
  pre-connected Classic controller and BLE controller, settings/Amiibo persistence, clean Config
  exit, normal reconnect, and non-discoverability/no audio stutter in controller personalities.
- [x] Make Virtual Amiibo always available with a blank/no-tag initial state; replace the
  original/modified portal model with one mutable browser record per imported dump and one
  loaded-slot pointer.
- [x] Add a one-shot install marker so every UF2 flash erases settings, virtual-tag journals, wake
  identity, and Bluetooth bonds while ordinary reboot persistence remains unchanged.
- [x] Retire per-controller-family **button** mapping storage and UI in favor of one locked base
  button map plus Switch-side remapping on the stable emulated controller identity; retain all
  body/accent/Sony-lightbar color controls.
- [ ] Hardware-validate the v10 first-boot erase exactly once, blank NFC behavior, browser
  loaded-slot state and mutable-dump transfer over Config BLE, settings save/readback, and re-pair after the
  bond wipe.
- [x] Implement and host-test the revised BOOTSEL policy: paired single-tap cycles only controller
  personalities, double-tap opens pairing, triple-tap wipes/disconnects, and a two-second hold
  enters Config directly; Config keeps only triple-tap wipe and two-second direct exit.
- [ ] Hardware-validate the revised BOOTSEL matrix both with no controller and with a paired
  controller, including bond-preserving disconnect before paired double-tap pairing.
- [x] Require a stored LE bond and active 16-byte encryption for management RX/CCC writes, and
  accept a new management Just-Works bond only during the existing double-tap pairing window.
- [x] Make bonded/encrypted management production-default-on without adding persistent config:
  `mgmt off` is a RAM-only escape hatch and an ordinary reboot restores management availability.
- [x] Remove the remaining wireless core-0 bond-operation wait: defer list/remove replies across
  task ticks, bind completion to the originating BLE session, and hold bridge back-pressure until
  the reply is published. Keep CDC Config's USB-pumping synchronous behavior unchanged.
- [ ] Hardware-validate the management security/default matrix: new bond rejected outside the
  window, accepted inside it, bonded reconnect outside it, unbonded/plaintext writes rejected,
  `mgmt off` disconnect/silence for the current boot, and reboot restoring advertising.
- Add a release checklist that records board, firmware revision, controller firmware, console
  firmware, and test result.

## Controller appearance customization

- [x] Replace the unused per-player lightbar array with one persistent `body_color` source.
- [x] Preserve the previously effective slot-0 color and all v6 remap/wake data during migration.
- [x] Apply `body_color` to Pro Controller 2 factory offset `0x13019`.
- [x] Drive DualShock 4 and DualSense lightbars from the same RGB value, including black/off,
  without coupling the lightbar hue to player assignment.
- [x] Hardware-validate that the Switch 2 configuration menu renders the configured body color.
- [x] Hardware-validate Sony lightbar matching and persistence after the revised two-stage
  DualSense lightbar initialization.
- [x] Add independently configurable Joy-Con 2 Left/Right accents, defaulting to their genuine
  retail highlight colors and driving the Sony lightbar in the matching active personality.
- [x] Hardware-validate both configurable Joy-Con accents in the Switch UI and Sony lightbar.
- [x] Add an explicit bonded management/portal/app action that safely re-enumerates the current USB
  personality after saving host-visible identity colors.
- [ ] Hardware-validate that same-personality apply refreshes the Switch 2 color and restores input,
  motion, rumble, audio, wake, and the management connection after the brief USB reconnect.
- [x] Parse console command `0x09` and drive physical player-indicator LEDs from the real console
  slot while keeping this state independent from `body_color`.
- [x] Hardware-validate that DualSense player dots follow Switch menu reordering.

## Individual Joy-Con sideways mapping

- [x] Translate both Joy-Con personalities from normalized physical paired-controller inputs,
  retaining configurable Pro2 semantics specifically for Capture and C/GameChat.
- [x] Map L1/R1 to SL/SR and L2/R2 to each side's requested shoulder/trigger pair.
- [x] Rotate face buttons into the Left D-pad and Right face cluster.
- [x] Feed the lone stick from both the paired left stick and D-pad, with per-side axis rotation.
- [x] Reuse Pro2 Capture, Home, and C/GameChat sources.
- [x] Lock both report formats with host-side golden tests.
- [x] Complete a physical button, D-pad-to-stick, and analog-direction pass on Switch 2.
- [x] Pair generic Bluetooth HID mice, gate native mouse output to structurally identified mouse
  sources, activate the Joy-Con pointer, map buttons, and translate wheel notches into local-stick
  menu navigation.

## Protocol research

### Console-native motion (`0x09`)

Status: ✅ genuine Pro Controller 2 passthrough and DualSense translation confirmed; 🔵 additional
controller families remain open.

The UART↔console bridge and live GATT discovery established the real controller sequence and
attribute handles. A genuine PID `0x2069` source now supplies its native length-30/length-40 motion
PDUs directly to Pro2 USB report `0x09` at a verified 7.5 ms BLE interval. Splatoon 3 confirms
correct axes/aim, no stationary drift, reconnect recovery, and a stable power-off hold. See
[`docs/experiments/native-pro2-motion-passthrough-2026-07-21.md`](docs/experiments/native-pro2-motion-passthrough-2026-07-21.md).

DualSense and DualSense Edge now translate their calibrated accel/gyro stream into the
hardware-validated length-`0x1E` Switch 2 quaternion carrier. Splatoon 3 confirms correct
direction, scale, rapid movement, stationary behavior, and reconnect recovery. The genuine
length-`0x28` form is now identified as a packed multi-sample IMU payload. Exact reference-PCAP
analysis transfers the catch-up accel/gyro field map to the Pro2 and proves the former
G6/G7/G8 “reference” aliases cross real packed samples. A controlled template-derived generator
caused random motion because it corrupted those samples and was removed. A future software
generator must synthesize the complete coherent multi-sample payload; no magnetometer-specific
lane is currently established. See
[`docs/bluetooth/dualsense-motion.md`](docs/bluetooth/dualsense-motion.md),
[`docs/switch2/report-0x09-motion.md`](docs/switch2/report-0x09-motion.md), and
[`docs/switch2/uart-magprobe.md`](docs/switch2/uart-magprobe.md).

Do not generalize the DualSense result blindly to other IMUs. Reuse the calibrated quaternion
translator only after each controller family has a verified sensor layout, axis map, timestamps,
scale, and stationary-bias behavior.

- [x] Reject a required physical-magnetometer premise: no independent magnetic/reference lane has
  been established, and the former aliases are packed IMU bit slices.
- [x] Decode the main normal/catch-up multi-sample accel/gyro field map offline from reference
  PCAPs and the exact ICM-42670-P datasheet.
- [x] Hardware-validate UART `imuref`: raw handle-`0x000A` sample delivery, notification ownership,
  and clean `imuref off` restoration of input and native gyro.
- [x] Correlate live raw `0x000A` samples against every accel/gyro lane in `0x000E`. A zero-drop
  7.5–30 ms cadence matrix proves exact layout boundaries at tick 11 and tick 15. The high-rate,
  normal, and corrected catch-up maps all agree with raw stationary axes and scale.
- [x] Bound ordinary catch-up behavior through the maximum accepted 30 ms interval. No fourth
  layout appears. Bit 287 is the single byte-alignment remainder and is zero across 1,066 catch-up
  packets in 14 captures; treat it as observed reserved-zero padding.
- [x] Resolve the native preamble and layout discriminator. The encoded 12-bit elapsed count
  selects the layout without a captured predecessor, and byte 3 maps exactly to high-rate
  (`0x0D`), normal (`0x0E`), or catch-up (`0x0F`) in the zero-drop corpus.
- [x] Decode the high-rate/normal tail as two Q3 IMU-temperature samples sharing a signed ten-bit
  integer part. Two independent zero-drop raw/native/raw A/B/A captures bracket the native
  temperature with handle-`0x000A`; retain the 3% unequal low fractions as two real samples.
- [x] Correct the prefix field boundaries and discriminate sampling-epoch versus state-collapse
  error. The prefix is mode `3` plus `s24+s23+s25` in high-rate and `s22+s21+s23` otherwise.
  Carrier 2 is split around its low two bits; the former “separate state” was a false boundary.
  Carrier-state-grouped fits use exact power-of-two scales.
- [x] Resolve the high-rate gyro fixed-point scale without another physical capture. The genuine
  sensor/common stream is `±2000 dps / 16.4 counts/dps`; high-rate gyro uses seven fractional bits
  (`/128`), not high-rate acceleration's eight (`/256`). Existing carrier integration improves from
  a `0.554` median recovered rotation to `1.108`, with two captures at `1.000` and `0.994`.
- [x] Resolve the carrier epoch and implement a diagnostic history decoder. The prefix is sampled
  at `current tick - encoded elapsed + 4`, or four ticks after the preceding carrier. This improves
  the mixed-cadence pitch fixed NRMSE from `0.008728` to `0.002718`; causal modular unwrapping has
  sub-`0.005°` median error in both dynamic captures with zero observed chart mismatch.
- [x] Capture reciprocal genuine chart transitions. State `0` wire `(G0,G1,G2)` and state `3`'s
  local state-0-boundary projection `(G1,G2,G0)` form one continuous carrier across both
  `3 → 0` and `0 → 3`.
  Prefix epochs straddling both boundaries select chart 0. Sixteen genuine rapid-motion records
  exceed the strict retained-vector unit constraint, so strict smallest-three is not an exact
  genuine model.
- [x] Capture a transition involving state 2 and verify the stateful chart-handoff
  law across all four states. A zero-drop `0 → 1` boundary selected local state-0 projection
  `(G2,G0,G1)`, but a
  held-out zero-drop `3 → 1 → 0` capture refuted one globally composable unsigned permutation
  per state. Its `1 → 0` edge has minimum unsigned residual `1.185389`; the cyclic topology's
  opposite-sign branch reaches `0.024716`. Across all five boundaries, that structured model has
  RMS/max `0.025302/0.047878` and minimum branch margin `0.324174`, covering both state-1 sign
  branches without per-edge tuning.
  A later zero-drop state-2 trigger captured reciprocal `3 → 2 → 3` seams. Both select topology
  `(G2,G0,G1)` and opposite-branch signs `(+,−,−)`, at residuals `0.036162` and `0.011824`.
  The nine-boundary corpus now covers every chart state at RMS/max `0.023541/0.047878`.
  Its interleaved prefix selects chart 3 across `3 → 2`; the stateful local-frame audit also
  resolves the direct `3 → 1` prefix as chart 1.
- [x] Prove exact integer projection/rounding against the generated full corpus fixtures.
- [x] Hardware-reject the sequence-coherent acceleration fix behind the default-off high-rate
  gate. LIVE matched the established `0x1E` gain and passed every offline coherence check, but the
  console still produced chaotic uncommanded motion. Disabling the gate returned immediately to
  validated `0x1E`. The readiness tool now fails closed for this exact recipe.

### NFC / amiibo

Status: 🟡 genuine Pro2 physical passthrough and feature-gated Virtual Amiibo reads are recognized
by a real Switch 2. Transactional write staging/commit, logical eject, next-scan re-presentation,
same-session updated readback, and validated UART export are hardware-confirmed. The exported
540-byte image differs from the unique matching original only within permitted writable ranges.
Automatic write-before-eject persistence, power-cycle recovery, offline library use, and
full-library backup restore are hardware/browser-confirmed. Manual Eject/Present
is implemented and awaiting real-console validation; destructive slot removal and all native write
paths remain open.

The adapter validates and transactionally uploads 540/572-byte images and internally retains an
imported baseline plus the optional latest console-written image for recovery. Both images, dirty
state, and optional signature are stored in alternating
CRC-verified flash banks at sectors `-3` and `-5`. A successful console commit requests this
snapshot automatically, and logical TagRemoved waits until it verifies. The browser-local library
works without an adapter, accepts a single file or recursively scans a directory, caches mutable
dumps in IndexedDB (content-keyed for v3 combinations), and retains one loaded-slot pointer.
**Sync Amiibo from Adapter** overwrites the matching browser copy before clearing dirty protection.
Primary capture corrected the read model to a 600-byte reader buffer served in 70-byte offset
chunks. The command-driven runtime handles the confirmed read flow plus an evidence-reconstructed
write flow: exact-UID `0x06` selection, a 64-byte preparation buffer, bounded `0x14` staging, and
atomic `0x08` commit. It performs no NFC idle polling.

The UART-gated native Pro2 bridge has completed one genuine physical-tag read that the Switch
recognized. It writes the extended `0x0016` command path, keeps `0x001E` subscribed, and receives
the matching ordinary NFC replies on `0x001A`. Production gating, reconnect/removal, Joy-Con 2
Right, and physical writes remain unvalidated.
Switch 1 Pro/Joy-Con Right requires protocol translation through report `0x31` and the NFC MCU; it
is not raw passthrough. See
[`docs/switch2/nfc-implementation.md`](docs/switch2/nfc-implementation.md).

- [x] Audit protocol sources, current transport limits, RAM, flash, CPU, and config-mode upload
  feasibility.
- [x] Add the host-tested non-blocking USB vendor-IN response pump and transport-neutral NFC codec.
- [x] Add virtual upload/read/write/download and validate it on a real Switch 2.
  - [x] Config-mode transactional upload and full-image download.
  - [x] RAM image and atomic console-write codec.
  - [x] Command `0x01` read state-machine integration using the primary 600-byte/chunk capture.
  - [x] Real-console virtual read validation with a non-NFC source controller.
  - [x] Fail-closed virtual write dispatch reconstructed from existing command/capture evidence.
  - [x] Reassemble vendor OUT commands across USB packets; host-test the exact 64+24-byte `0x14`
    split which caused the first hardware attempt's `2168-0002` crash.
  - [x] Real-console game-owned write reaches complete `0x14`, `0x08`, and `05 00` without a crash.
  - [x] Add live UART image export so console-written RAM state can be downloaded without moving
    the console-facing USB cable.
  - [x] Validate dirty state and generation-safe UART download of a mutated 540-byte image.
  - [x] Validate post-write logical auto-eject, next-scan re-presentation of the updated image, and
    ordinary-read continued presentation.
  - [x] Automatically queue a power-loss-safe snapshot before the post-write removal edge.
  - [x] Power-cycle after a console write and validate that the Used image and dirty state recover.
- [ ] Complete 2048-byte figure-v3 Virtual Amiibo write support.
  - [x] Hardware-confirm the descriptor-driven read and v3-only `0x14`/`0x21` device-command path.
  - [x] Implement and host-test exact device-command/data-write classification, six-chunk staging,
    bounded three-record commit, generation-safe update, dirty state, persistence gating, and
    Config/UART/portal readback.
  - [x] Hardware-validate owner/format write, `05 00`, durable Stop/eject, next-scan updated
    readback, and power-cycle recovery.
  - [x] Replace the captured fixed `7A C4` trailer with the dump's complete 64-byte SRAM response;
    hardware-confirm an untouched downloaded dump with no signature override.
  - [x] Capture and structurally separate the Air Riders 355-byte/`0x20` game-data transaction from
    the validated 454-byte/`0x08` owner/format write.
  - [x] Hardware-test the non-mutating `0x20` completion gate: it reaches `05 00` with zero write
    errors, but proved that leaving the tag presented causes three retries and `2115-0088`.
  - [x] Hardware-test the corrected Stop-to-TagRemoved lifecycle and three-second re-presentation
    cooldown: the immediate scan receives absent `07 41`; the unchanged image is later retried
    three times and ends at `2115-0096`.
  - [x] Mirror one successful Air Riders write through a genuine Pro Controller 2 and a physical
    Kirby & Warp Star tag; capture both `0x20` shapes, genuine state `0x16`, the follow-up writes,
    and a matching sector-0 before/after diff.
  - [x] Implement and host-test the capture-derived 355-byte clear and 167-byte update records,
    generation-safe persistence without intermediate ejection, and genuine `0x16` status.
  - [x] Hardware-isolate the first prepared-build failure: the 355-byte stage and ordinary
    checkpoint commit successfully, but intermediate auto-eject blocks the 167-byte stage and
    causes `2115-0096`; retain presentation across only that bounded inter-stage window.
  - [x] Hardware-validate the complete two-stage Air Riders write: both extended envelopes, both
    `0x20` completions, all three ordinary checkpoints, persistence, and final Stop complete with
    zero write errors.
  - [x] Capture genuine written-tag reuse and implement the missing sector-aware `0x1E` result:
    bare ACK, empty status `0x15`, one report-state edge, and a 196-byte result served through
    `0x15`. Host-test all captured bytes including read-only sector-1 page 0.
  - [x] Hardware-validate the prepared `0x1E` path: status `0x15`, all three data chunks, and Stop
    match genuine hardware.
  - [x] Hardware-validate the subsequent reused-tag 167-byte update transport and persistence.
    It completed with zero errors; the next read isolated a separate implicit sector-1 page-0
    state transition.
  - [x] Hardware-validate retained dynamic sector-1 page 0 (`A5 00 01 00` →
    `A5 00 02 00`) across the update and immediate second reuse. Air Riders accepted the second
    read and loaded the previously saved custom color.
  - [x] Power-cycle the adapter and repeat that read. The exact generation-4 image/CRC recovered
    from flash, `0x1E` served `A5 00 02 00`, and Air Riders accepted the retained save.
  - [x] Hardware-capture a non-cosmetic learned gameplay-state save after completing a level.
    It reused the known 167-byte extended plus ordinary-write sequence, touched no new region,
    and produced an HMAC-valid generation-9 image with zero write errors.
  - [x] Capture one failing untouched King Dedede or Bandana Waddle Dee write on the current build.
    King Dedede & Tank Star uses dynamic record pages: sector-0 `0xB2` and sector-1 capability/data
    `0x64/0x65`, versus Kirby's `0x92` and `0x00/0x01`. The first header-only fix proved staging
    succeeds but fixed-page commit validation still caused `2115-0096`.
  - [x] Hardware-validate the allocation-relative codec across all 16 available Air Riders v3
    dumps. Every image completed real-console read and write. The implementation remains
    identity-agnostic and fails closed outside the captured three-record schema and safe memory
    bounds.
  - [ ] Complete the production-portal Sync test against the intentionally dirty v3 generation.
- [x] Add alternating-bank persistence outside `pico_config_t`, including version-1 migration.
- [x] Add recursive directory import, browser-local library caching, parsed identity, and optional
  cached friendly catalog metadata.
- [x] Replace the long library selector with an imported-files-only artwork carousel that
  progressively fills during directory scanning, enlarges/centers the selected tag with four
  progressively smaller neighbors per side, animates navigation, and filters imported entries
  without disturbing AmiiboAPI source order.
- [x] Collapse the production manager into one carousel-centered surface: compact search and
  tap-to-cycle filters, one Load/Select/Import/Sync action, active-tag selection on connection,
  centered save metadata/write badge, and a non-modal details/action drawer.
- [x] Replace the redundant carousel arrows/count with smooth hovered-wheel, touch-swipe, and
  keyboard navigation; group the three responsive filter chips below the compact primary action.
- [x] Make Initialize discoverable and usable offline. It requests user-owned keys only when
  needed, self-verifies the re-signed copy, and clears both ordinary and Air Riders v3 save ranges.
- [ ] Expose physical-tag scanning in the production portal only after firmware provides a
  capability-reported Config-transport API; the UART-only initiator is not sufficient.
- [x] Keep the production library available without an adapter connection, store mutable dumps in
  browser-local IndexedDB (content-keyed for distinct v3 rider/machine combinations), retain one
  loaded-slot pointer, and add versioned full-library export/import.
- [x] Merge unload and adapter removal into one scope-aware **Eject amiibo** action with dirty-write
  confirmation.
- [ ] Hardware-validate manual Eject, adapter loading, validated 540/v3 adapter-to-browser sync,
  and re-presentation on a real Switch 2.
- [x] Hardware-test automatic snapshot recovery and the former Unused/Used selection mechanics.
- [ ] Regression-test the unchanged internal two-image/two-bank recovery mechanics through the
  simplified one-dump browser UI.
- [ ] Hardware-test interrupted-upload preservation.
- [ ] Complete native Switch 2 reader relay.
  - [x] Capture and relay one genuine Pro2 physical read recognized by the console.
  - [ ] Validate production selection, reconnect/removal, Joy-Con 2 Right, and physical writes.
- [ ] Capture and implement Switch 1 reader translation.

### Console-side capture infrastructure

Status: ✅ retained command tracer implemented, hardware-validated, and used to unlock native motion.

A bounded, opt-in UART trace ring now records EP0 setup/replies, vendor bulk commands/replies, and
HID output reports across all native personalities. It is disabled by default, performs no UART or
formatting work inside USB callbacks, and has host coverage for truncation and overflow behavior.
The full `trace clear/start/reenumerate/stop/dump` workflow is hardware-validated on a real Switch 2:
the pull transport delivered a complete 63-record Pro2 initialization capture with zero overwrites
or framing loss while a genuine Pro Controller 2 was paired to the dongle. Broader input, rumble,
wake, audio, GC, and Joy-Con trace regression coverage remains before adding sampled input or
audio-control events. The first PC-side decoder/differ now validates JSONL, renders known protocol
fields, redacts session material by default, and aligns semantic A/B differences; a live UI and
high-rate trace support remain future increments.

The full tooling wishlist and build order — the on-device tracer, the out-of-band trace channel
(the dongle's single USB-C port is occupied by the console), the fault-injection harness, and the
capture/analysis tools — live in
[`docs/switch2/uart-trace-tooling.md`](docs/switch2/uart-trace-tooling.md); the superseded wishlist
is archived at
[`docs/archive/tooling-plan-through-2026-07-21.archived.md`](docs/archive/tooling-plan-through-2026-07-21.archived.md)
planning.

### NFC investigation laboratory

Status: 🟡 core offline lab implemented; remaining hardware and capture gaps are listed below
(2026-07-29).

The build order and rationale come from
[`docs/LLM/amiibo-v3-investigation-retrospective.md`](docs/LLM/amiibo-v3-investigation-retrospective.md);
the current workflow is
[`docs/re-methodology/nfc-investigation-workflow.md`](docs/re-methodology/nfc-investigation-workflow.md).

- [x] v3 corpus analyzer (`tools/amiibo_corpus.py`)
- [x] Shared NFC layout module (`tools/ns2_nfc_semantics.py`)
- [x] Semantic transaction decoder and comparator (`ns2_trace.py nfc` / `nfc-diff`)
- [x] Experiment runner with a hashed artifact bundle (`tools/nfc_lab.ps1`)
- [x] `picoswitch2-nfc-lab` skill enforcing the phase order
- [x] **Host-replayable v3 NFC core** (2026-07-29). `ns2_v3_serve()` and its twenty file-scope
  statics moved to `src/nfc/ns2_amiibo_v3_runtime.c` behind
  `ns2_amiibo_v3_runtime_step(state, host, now_ms, generation, sub, request, image, effects)`,
  matching the 540 path's shape. Durable side effects go through `ns2_amiibo_v3_host_t` so a test
  can inject an apply failure or a pending flash write. `tools/test_ns2_amiibo_v3_runtime.c`
  replays the recognition read, the Air Riders write lifecycle, the `0x1E` reuse read, the
  persistence-gated eject with its 3 s cooldown, and the generation edge — all with a fake clock.
- [x] Structured internal error telemetry (2026-07-29). `ns2_amiibo_v3_error_t` distinguishes eight
  causes behind the single console-facing `07 41`, with the specific
  `ns2_virtual_nfc_result_t` and `0x14` stage offset, reported by `amiibo v3diag`.
- [ ] Trace the report NFC-state field so state edges are observed rather than inferred from `0x05`.
- [ ] Capture-to-fixture generator: turn a selected transaction into C/JSON fixtures plus expected
  state transitions, replacing hand-transcribed arrays.
- [ ] Persistence fault injector: interrupt erase/program at every journal step and verify recovery
  without risky physical testing.
- [ ] Portal browser integration harness with a mock adapter implementing the USB CDC / Config BLE
  command contract, so portal and protocol work stop sharing a change surface.
- [ ] Optional: an ISO14443A RF capture/emulation instrument for a second observation point at the
  tag boundary. It complements, never replaces, console/controller captures — PicoSwitch2 emulates
  the controller, so RF evidence must be translated across that boundary.

### Shared controller protocol laboratory

Status: 🟡 infrastructure active 2026-07-29; motion campaign complete, audio and firmware-tap
hardware campaigns pending.

- [x] Shared `picoswitch2-lab/v1` manifest, Git provenance, UART port discovery, UTF-8 artifact
  writing and SHA-256 hashing (`tools/PicoSwitch2Lab.psm1`).
- [x] Zero-loss capture-to-JSON/C fixture generator (`tools/capture_to_fixture.py`).
- [x] Capture-derived command/subcommand atlas (`tools/ns2_command_atlas.py`).
- [x] Magnet-ready stationary native-motion runner with `ns2_magprobe` analysis, baseline
  comparison and fixtures (`tools/motion_lab.ps1`).
- [x] Non-mutating audio continuity runner and delta analyzer (`tools/audio_lab.ps1`);
  counter reset remains explicit.
- [x] Read-only normalized `audio headset` UART diagnostic.
- [x] Host-only command-`0x0D` state model/reassembler and offline packaging runner.
- [x] Fail-closed current-image flash-space auditor for a candidate research capture bank; this
  does not substitute for a linker reservation.
- [x] Repository-local Codex protocol, motion, firmware and audio workflow skills.
- [x] Run the controlled no-magnet/sham/polarity/distance/recovery matrix with a genuine Pro
  Controller 2. No external-field response was resolved; see
  [`docs/experiments/pro2-magnetic-stimulus-matrix-2026-07-29.md`](docs/experiments/pro2-magnetic-stimulus-matrix-2026-07-29.md).
- [ ] Physically validate DualSense `none`/TRS-headphones/TRRS-headset classification.
- [ ] Design and prove the non-overlapping on-device firmware-capture partition, then implement the
  research-only progressive flash sink.

Workflow: [`docs/re-methodology/controller-protocol-lab.md`](docs/re-methodology/controller-protocol-lab.md).

### Wake from sleep

Status: ✅ implemented and hardware-confirmed in Pro Controller 2 mode.

The console identity is learned from the completed USB `0x15` pairing exchange and persisted after
the timing-sensitive handshake. The first non-neutral controller report after stable USB
inactivity provides automatic wake. See
[`docs/bluetooth/wake-from-sleep-design.md`](docs/bluetooth/wake-from-sleep-design.md).

Do not conflate console wake with controller sleep. Some controllers naturally power down during
the dock's brief VBUS outage (confirmed for DualSense/Edge), while Xbox Series BLE can reconnect
before its own search timeout. The failed generic ACL-disconnect experiment stranded the existing
controller relationship and was fully reverted. The 2026-07-17 follow-up found no safe generic
host-only solution: disconnecting does not command power-off, BLE low-power parameters retain the
link, and preventing the central from acting on advertisements necessarily delays reconnect and
therefore automatic wake. Keep current controller-managed idle sleep until a verified per-family
sleep command or distinguishable wake advertisement is captured. See
[`docs/bluetooth/controller-sleep-research.md`](docs/bluetooth/controller-sleep-research.md).

## Android handheld controller bridge

Status: Pico-side contract and regressions implemented. A no-root Android 13 AYN Thor has
hardware-validated built-in input capture, public HID Device registration, app-led bonding, Pico
receipt, and working in-game console input. The observed face-label mismatch is corrected in the
next APK; lifecycle/latency and the new unified relationship UX remain open hardware gates.
Architecture and gates:
[`docs/bluetooth/android-controller-bridge.md`](docs/bluetooth/android-controller-bridge.md).

**Current ownership:** the maintainer returned the completed Android/HID work to the main repository
workflow on 2026-08-13. Preserve the hardware-confirmed v1 input path and byte-compatible v2
extension while closing the remaining gates.

- [x] Prove on one target handheld that an ordinary foreground API-28+ app can acquire
  `BluetoothProfile.HID_DEVICE`, read the built-in controls, and register the fixed generic-gamepad
  descriptor. Confirmed on AYN Thor (Android 13) with `Odin Controller`; the app reaches Ready.
- [x] Audit the connected Retroid Pocket Classic read-only over ADB. Its API-34 OEM stack has HID
  Device enabled and its service running; the built-in `0x2022:0x3001` controller exposes the
  required controls. The ordinary-app proxy/register callback and labeled live inputs remain part
  of the unchecked feasibility item above.
- [x] Check in the canonical descriptor/neutral fixture and compile it through the production
  generic parser. Host coverage pins the exact 10-byte wire layout, complete-state mapping,
  wrong-ID/truncation rejection, disconnect cleanup, and Android-initiated fallback even when the
  OEM retains a phone Class of Device.
- [x] Complete the first pair from an app-launched system chooser/bond flow and reach working
  in-game console input through PicoSwitch2's generic gamepad parser.
- [x] Add persisted `Auto` / `Nintendo` / `Xbox` face-label normalization without changing the
  proven HID descriptor or firmware base map. `Auto` has bounded Thor/Retroid evidence and an
  explicit positional fallback; changing it neutralizes held state.
- [x] Collapse management and controller setup into one saved adapter relationship: first-use
  **Pair Adapter**, direct known-address GATT reconnect with scan fallback, and controller-mode HID
  connection to the same saved Classic bond without another chooser.
- [x] Implement the Android bridge as an independent Gradle project under
  `android/companion/`, with golden report-encoder tests and no root, Shizuku,
  accessibility service, hidden API, or controller-family impersonation.
- [x] Extend the native Android Amiibo page with portal-derived plaintext identity fields and
  optional phone-local, HMAC-verified owner/nickname/date/write metadata. Keep import, adapter
  operations, and diagnostics independent of the user’s 160-byte retail key.
- [x] Add separately tested Android-local cache-first catalog/artwork enrichment without making it
  a prerequisite for local import or adapter operations. The cache matches portal-style figure IDs,
  exposes friendly metadata/title-ID game labels, bounds network/image work, and falls back offline.
- [x] Complete the handheld UI hierarchy pass: Home-only connection status, focused Home tiles,
  collapsed Settings categories, a full-height artwork-led Amiibo library, deterministic
  Name/Series/Recently added sorting, and shared adapter-only catalog identity on Home/Amiibo.
- [x] Add separately tested Android-local Amiibo initialization/re-signing and ZIP exchange without
  making either a prerequisite for local import or adapter operations.
- [x] Add a strict foreground phone-NFC backup path for ordinary NTAG215 figures: exact version/page
  reads, optional real originality signature, fail-closed BCC/shape validation, no writes/auth/NDEF,
  and deliberate figure-v3 rejection until direct phone-RF evidence exists.
- [ ] Hardware-validate phone NFC against an independently dumped ordinary Amiibo, including
  signature stability, mid-read tag removal, duplicate handling, and byte-exact 540/572 output.
- [ ] Add structured owner-Mii attributes only after a user-owned full-feature `Ver3StoreData`
  fixture pins the field map offline. Defer graphical Mii rendering: the repository has no licensed
  renderer or FFL resource, and Nintendo's proprietary `FFLResHigh.dat` must never be bundled.
- [ ] Hardware-validate corrected face labels, full controls, foreground/lifecycle neutralization,
  the rebuilt saved-relationship reconnect, latency, and return to a known physical controller
  before making a broader compatibility claim.
- [x] Add the firmware-side bounded source registry/arbiter: multiple HID-ready sources may be
  enumerated, exactly one explicit source owns console slot 0, source changes neutralize and gate
  on a fresh report, and stale lifecycle events cannot clear a recycled connection. Host coverage
  is complete; physical coexistence, latency, audio/motion, and repeated Android/physical switching
  remain hardware gates.
- [x] Allow bonded/encrypted wireless management to select the active source and expose a clean
  **Active controller** chooser in the Android Input page. Selection uses the existing neutral/fresh
  arbiter boundary; it does not merge reports or automatically fall through on disconnect.
- [ ] Add a custom BLE GATT source driver only if a captured target-OEM failure proves the public
  Classic HID Device path unavailable; do not pre-emptively add a second protocol.

## Longer-term

- Capability-based haptic translation, including DualSense adaptive triggers where useful.
- Declarative controller profiles where report formats permit them.
- Complete NFC and console-native motion only when evidence supports indistinguishable behavior.

## Out of scope for the current release

- Multi-controller / local 4-player console output remains out of scope: the firmware registry can
  describe several available HID sources, but one explicit source owns the single console stream.
  Background discovery still idles once a controller is connected; simultaneous radio links and
  repeated physical/Android switching require the hardware gates in STATUS.md.
- Pretending a paired Joy-Con 2 L/R pair is one combined USB identity
- Persisting the volatile USB personality across power cycles
- Shipping speculative NFC or motion packet semantics
- Rewriting the vendored joypad-os stack wholesale
