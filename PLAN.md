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
- [x] Implement and host-test the revised BOOTSEL policy: paired single-tap cycles only controller
  personalities, double-tap opens pairing, triple-tap wipes/disconnects, and a two-second hold
  enters Config directly; Config keeps only triple-tap wipe and two-second direct exit.
- [ ] Hardware-validate the revised BOOTSEL matrix both with no controller and with a paired
  controller, including bond-preserving disconnect before paired double-tap pairing.
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
length-`0x28` form remains only partially decoded: a controlled template-derived generator caused
random motion and was removed, proving its unresolved lanes are semantically active. A
software-generated reference/magnetometer solution is the accepted direction for controllers that
lack the hardware; the rejected static-template construction is not that solution. See
[`docs/bluetooth/dualsense-motion.md`](docs/bluetooth/dualsense-motion.md),
[`docs/switch2/report-0x09-motion.md`](docs/switch2/report-0x09-motion.md), and
[`docs/switch2/uart-magprobe.md`](docs/switch2/uart-magprobe.md).

Do not generalize the DualSense result blindly to other IMUs. Reuse the calibrated quaternion
translator only after each controller family has a verified sensor layout, axis map, timestamps,
scale, and stationary-bias behavior.

- [x] Choose software generation—not a required physical magnetometer—as the direction for missing
  length-`0x28` reference data.
- [ ] Decode/model every dynamic console-relevant `0x28` lane before enabling a production
  generator.

### NFC / amiibo

Status: 🟡 genuine Pro2 physical passthrough and feature-gated Virtual Amiibo reads are recognized
by a real Switch 2. Transactional write staging/commit, logical eject, next-scan re-presentation,
same-session updated readback, and validated UART export are hardware-confirmed. The exported
540-byte image differs from the unique matching original only within permitted writable ranges.
Automatic write-before-eject persistence, clean/used selection, power-cycle recovery, offline
library use, and full-library backup restore are hardware/browser-confirmed. Manual
present/remove controls and all native write paths remain open.

The offline implementation validates and transactionally uploads 540/572-byte images and retains
two lossless copies: immutable **Unused** imported data and mutable console-written **Used** data.
The selected copy, both images, dirty state, and optional signature are stored in alternating
CRC-verified flash banks at sectors `-3` and `-5`. A successful console commit requests this
snapshot automatically, and logical TagRemoved waits until it verifies. The browser-local library
works without Web Serial, accepts a single file or recursively scans a directory, caches both
copies and catalog details in IndexedDB, and can export/import the complete library as versioned
JSON. **Save current Amiibo** retrieves both adapter copies before clearing dirty protection.
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
- [x] Add alternating-bank persistence outside `pico_config_t`, including version-1 migration.
- [x] Add recursive directory import, browser-local library caching, parsed identity, and optional
  cached friendly catalog metadata.
- [x] Replace the long library selector with a nine-position artwork carousel and automatically
  center/mark the adapter's active tag.
- [x] Keep the production library available without Web Serial, retain separate Unused/Used
  copies, and add versioned full-library export/import.
- [ ] Add separate portal controls for **Eject** (present=false while retaining the active image)
  and **Remove active amiibo** (clear the adapter slot only after dirty-write protection).
- [x] Hardware-test automatic snapshot recovery and Used/Unused selection.
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

## Longer-term

- Capability-based haptic translation, including DualSense adaptive triggers where useful.
- Declarative controller profiles where report formats permit them.
- Complete NFC and console-native motion only when evidence supports indistinguishable behavior.

## Out of scope for the current release

- Multi-controller / local 4-player: the scope is now one dongle to one controller. Background
  discovery idles once a controller is connected (hardware-confirmed; see STATUS.md). Actively
  rejecting a 2nd bonded controller that pages in is a possible future increment, not done here.
- Pretending a paired Joy-Con 2 L/R pair is one combined USB identity
- Persisting the volatile USB personality across power cycles
- Shipping speculative NFC or motion packet semantics
- Rewriting the vendored joypad-os stack wholesale
