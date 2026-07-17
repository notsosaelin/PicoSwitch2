# PicoSwitch2 Roadmap

> Forward-looking work only. See [`STATUS.md`](STATUS.md) for current behavior and
> [`docs/archive/roadmap-through-2026-07-15.md`](docs/archive/roadmap-through-2026-07-15.md) for
> the completed milestone narrative.

## Current objective: post-release compatibility closure

Version `v1.1.0` now preserves the hardware-confirmed Pro Controller 2, NSO GameCube,
DualSense/Edge, rumble, pairing, and BOOTSEL baseline. Close the remaining mapping and regression
matrix gaps before beginning new protocol work.

### Release gate

- [x] Pico W release build
- [x] Pico 2 W release build
- [x] Ten host-test executables
- [x] DualSense/Edge input, extra buttons, LEDs, and rumble on hardware
- [x] BOOTSEL double-tap, triple-tap, and hold while DualSense is connected
- [x] NSO GameCube input and rumble on a real Switch 2
- [x] Reconcile documentation and archive superseded development logs
- [x] Complete source-comment evidence cleanup
- [x] Push the release commit and publish both UF2 artifacts (`v1.1.0`, 2026-07-15)

## Next: compatibility closure

Use [`docs/status/compatibility-matrix.md`](docs/status/compatibility-matrix.md) as the single test
matrix. Do not combine mapping changes with unrelated Bluetooth or USB protocol changes.

1. Classify the Joy-Con 2 Left Steam/Windows behavior separately from real-console compatibility.

Completed closure checks: rumble ON/STOP/reconnect across the tested controller/personality
matrix, individual Joy-Con 2 rumble, triple-tap admission blocking, and explicit re-pairing after
opening a new pairing window are hardware-confirmed. The real Pro Controller 2's physical
L/R/ZL/ZR translation in NSO GameCube output mode is also confirmed.

## Reliability and maintainability

- Extract or wrap the BOOTSEL gesture state machine for a host-side starvation regression test.
- Keep report-boundary maintenance and timer fallback behavior documented together.
- Finish the generic-gamepad quirk split only after a behavior-equivalence test exists; do not
  reintroduce the previously reverted refactor from `_reverted_quirk_split/` blindly.
- Resolve BLE Device Information Service VID/PID without delaying driver binding or starving input
  notifications.
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

## Protocol research

### Console-native motion (`0x09`)

Status: 🔴 paused pending new primary evidence.

The byte layout is documented, but generated value semantics remain unresolved. Resume only after
obtaining a genuine console-side trace or another experiment capable of distinguishing competing
models. Do not tune the encoder against symptoms alone.

### NFC / amiibo

Status: 🔴 blocked on a genuine transaction capture.

The command inventory is documented. Do not promote third-party implementations to Confirmed until
this project captures and validates a real read/write exchange.

### Console-side capture infrastructure

Status: 🟡 recommended next research investment.

A reproducible control/bulk/interrupt capture path would unblock both motion and NFC. Validate it in
stages: enumeration, identity handshake, buttons, timing fidelity, then feature-specific traffic.

### Wake from sleep

Status: ✅ implemented and hardware-confirmed in Pro Controller 2 mode.

The console identity is learned from the completed USB `0x15` pairing exchange and persisted after
the timing-sensitive handshake. The first non-neutral controller report after stable USB
inactivity provides automatic wake. See
[`docs/bluetooth/wake-from-sleep-design.md`](docs/bluetooth/wake-from-sleep-design.md).

Do not conflate console wake with controller sleep. Some controllers naturally power down during
the dock's brief VBUS outage (confirmed for DualSense/Edge), while Xbox Series BLE can reconnect
before its own search timeout. The failed generic ACL-disconnect experiment stranded the existing
controller relationship and was fully reverted. Continue researching a controller-side solution;
do not delete bonds, install an admission gate, or suppress incoming connections for this feature.

## Longer-term

- Capability-based haptic translation, including DualSense adaptive triggers where useful.
- Multi-controller output architecture after single-controller behavior is release-stable.
- Declarative controller profiles where report formats permit them.
- Complete NFC and console-native motion only when evidence supports indistinguishable behavior.

## Out of scope for the current release

- Pretending a paired Joy-Con 2 L/R pair is one combined USB identity
- Persisting the volatile USB personality across power cycles
- Shipping speculative NFC or motion packet semantics
- Rewriting the vendored joypad-os stack wholesale
