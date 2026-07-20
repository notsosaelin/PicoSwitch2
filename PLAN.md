# PicoSwitch2 Roadmap

> Forward-looking work only. See [`STATUS.md`](STATUS.md) for current behavior and
> [`docs/archive/roadmap-through-2026-07-15.md`](docs/archive/roadmap-through-2026-07-15.md) for
> the completed milestone narrative.

## Current objective: post-v1.4 protocol work

Version `v1.4.0` adds hardware-confirmed Pico 2 W live DualSense audio, conditional
Switch 2 headset routing, native PCM haptics with and without a headset, and saved-bond
audio/rumble reconnect recovery. Pico W retains its validated non-audio behavior.

### Release gate

- [x] Pico W release build
- [x] Pico 2 W release build
- [x] Twenty-eight host-test executables
- [x] DualSense/Edge input, extra buttons, LEDs, and rumble on hardware
- [x] BOOTSEL double-tap, triple-tap, and hold while DualSense is connected
- [x] NSO GameCube input and rumble on a real Switch 2
- [x] Joy-Con 2 Left/Right sideways mappings and rumble on a real Switch 2
- [x] Configurable Pro2/Joy-Con colors, Sony lightbar matching, and DualSense player dots
- [x] Reconcile documentation and archive superseded development logs
- [x] Complete source-comment evidence cleanup
- [x] Hardware-validate live audio, headset removal/reinsert, native rumble, and bonded reconnect
- [x] Push the release commit and publish both UF2 artifacts (`v1.4.0`, 2026-07-18)

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
  [`DS5-NS2_AUDIO.md`](DS5-NS2_AUDIO.md) and
  [`AUDIO-INVESTIGATION.md`](AUDIO-INVESTIGATION.md).
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
- [ ] Run an extended playback and thermal soak of the Pico 2 W 300 MHz audio build.
- [ ] Add DualSense microphone report decoding and Opus-to-USB return after speaker playback is
  physically stable.
- [ ] Re-test the Switch 2 “Update this controller” prompt after USB audio is healthy; if it
  remains, capture and isolate the firmware/DSP version or memory-read gate separately.

## Reliability and maintainability

- [x] Extract the pure BOOTSEL gesture recognizer and cover timer-only, report-only starvation,
  mixed scheduling, one-shot hold, unknown-sample, and timestamp-wrap behavior on the host.
- [x] Keep report-boundary maintenance and timer fallback behavior documented together.
- [x] Resolve late BLE Device Information Service VID/PID on the consuming side: retain immediate
  HID/name binding and notification-first GATT setup, then re-evaluate Xbox BLE, Stadia, and
  MouthPad provisional matches when authoritative VID/PID arrives. Host coverage pins input before
  and after corrective rebinds; the resulting build is hardware-confirmed with Xbox Series BLE.
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

The full tooling wishlist and build order — the on-device tracer, the out-of-band trace channel
(the dongle's single USB-C port is occupied by the console), the fault-injection harness, and the
capture/analysis tools — live in [TOOLING.md](TOOLING.md), the canonical home for all tooling
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
