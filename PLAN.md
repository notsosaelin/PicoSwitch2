# PicoSwitch2 Roadmap

This file describes accepted future work and project direction.

For current implementation state, validated behavior, open hardware gates, and recent discoveries, see
[`STATUS.md`](STATUS.md).

For durable protocol and architecture details, see the relevant documents under [`docs/`](docs/).

Do not use this file as a historical changelog. Completed work should normally be removed or reduced
to the minimum context necessary to explain future work.

Do not add speculative ideas to this roadmap merely because they were discussed. Add work here when
it has become an accepted project direction, a concrete follow-up to an existing implementation, or
a known technical requirement.

---

# Current Baseline

PicoSwitch2 v2.0.0 is released.

The v2.0 generation established the current product architecture:

- native Switch 2 controller personalities
- Bluetooth physical-controller input
- Android companion
- Android handheld-controller bridge
- platform-neutral bridge core
- motion translation
- rumble translation
- battery passthrough
- C/GameChat support
- encrypted wireless management
- Virtual Amiibo
- runtime firmware/application contract validation
- durable diagnostics and compatibility tooling

Current working architecture should be treated as the baseline.

Future feature work should consume existing abstractions rather than reopening stable controller,
motion, rumble, battery, descriptor, or bridge behavior without evidence of a concrete limitation.

---

# Current Development Priority

## Bluetooth Keyboard / Keyboard + Mouse Input — Complete

Delivered and hardware validated. Controller / Keyboard / Keyboard + Mouse input modes, the
composite logical-source model, structural HID classification, the remapping-capable configuration
foundation with canonical defaults and sparse user overrides, persistence with schema migration, the
management and UART surface, diagnostics, and host coverage.

Reference: [`docs/bluetooth/keyboard-mouse-input.md`](docs/bluetooth/keyboard-mouse-input.md).
Current state and the confirmed reconnect/discovery architecture are recorded in
[`STATUS.md`](STATUS.md).

### Deferred follow-ups

Neither is in progress. Both need product decisions or hardware evidence, not more implementation.

- **Bounded partial-KB/M discovery policy.** Current hardware-correct behavior keeps discovery
  active while only one KB/M role is present, which is what lets a returning role rejoin without
  re-pairing. A future bounded completion-window pass would allow intentional keyboard-only or
  mouse-only use without indefinite active discovery. Not specified here.
- **Classic pointing-device admission in Controller mode.** Classic discovery admits only
  gamepad-class Class-of-Device peripherals in Controller mode, so a Classic Bluetooth mouse driving
  Joy-Con 2 mouse mode must initiate the connection itself. The KB/M pass opened that gate for its
  own roles only. Whether Controller mode should also admit a pointing device from inquiry is a
  product decision, not a defect, and needs hardware evidence about which mice actually page the
  host.

### Follow-on: graphical remapping editor

UX_PASS owns the polished Keyboard / Keyboard + Mouse remapping editor. It consumes the
`kbm map` / `kbm bind` / `kbm reset` / `kbm mouse` configuration surface already shipped by this
pass and must not need to restructure the Bluetooth input path, edit firmware constants, or
reconstruct defaults from source.

---

# Near-Term Product Work

These items are accepted directions but should be performed as separate coherent engineering passes.

## Touch Gamepad

Add an optional on-device virtual controller to the companion application.

The Touch Gamepad should use the existing normalized controller/bridge architecture rather than
creating an independent controller pipeline.

Initial implementation should focus on the currently supported companion platform.

### Core requirements

- [ ] Optional Touch Gamepad input source.
- [ ] Landscape/full-screen controller layout.
- [ ] D-pad.
- [ ] Face buttons.
- [ ] L/R and ZL/ZR.
- [ ] Dual analog sticks.
- [ ] + / -.
- [ ] Home.
- [ ] Capture.
- [ ] C/GameChat.
- [ ] Real multitouch with stable pointer ownership.
- [ ] Reliable release of all held state on lifecycle/source/disconnect transitions.
- [ ] Low-latency input path.
- [ ] Responsive safe-area-aware layout.
- [ ] Preserve the existing bridge/core implementation.

Do not combine this pass with unrelated controller, protocol, or platform work.

---

## Android Companion Polish

Perform a dedicated Android UI/UX and reliability pass after major functionality is stable.

Potential work should be based on the actual current application rather than historical UI plans.

Focus areas may include:

- clearer connection state
- clearer active-source state
- input configuration discoverability
- adapter/firmware compatibility presentation
- Amiibo workflow polish
- diagnostics accessibility
- lifecycle/reconnection behavior
- handheld-sized layout consistency

Do not use this as an excuse to refactor bridge-core or firmware behavior.

---

# Platform Backend Expansion

The platform-neutral bridge architecture intentionally allows additional host platforms.

No additional platform backend should begin until it is selected as an active task.

## Linux

A Linux companion/backend is an accepted future direction.

The first implementation should prove the minimum transport and input path required to use Linux
devices as PicoSwitch2 controller sources.

Potential Linux input sources include:

- standard evdev gamepads
- built-in handheld controls

The Pico remains responsible for Switch-facing controller emulation.

A direct Linux implementation should consume the shared bridge architecture rather than duplicate
Android behavior.

### ROCKNIX / H700 target

A small-screen ROCKNIX companion for supported H700 handhelds is a desired Linux use case.

The initial target should be a handheld-oriented interface suitable for 720x480-class devices,
using the device's built-in controls as the controller source through the Pico.

Do not begin this work as part of unrelated firmware or Android tasks.

---

## Windows

A Windows backend remains an accepted future platform direction.

Before implementation, verify the transport approach required by the current Pico bridge protocol.

Reuse shared bridge/session/protocol logic where practical.

Do not choose a universal cross-platform UI framework merely to support this port.

Platform portability of protocol and controller logic is more important than forcing every frontend
into the same toolkit.

---

# Input Architecture Expansion

## Reserved Physical Inputs

Unknown or additional physical buttons should remain unassigned until they can be represented
correctly.

Currently reserved Android physical inputs include:

- `KEYCODE_BUTTON_C`
- `KEYCODE_BUTTON_Z`

Do not silently bind these to unrelated Switch actions.

These controls should eventually be available to a deliberate mapping/input system.

---

## Input Mapping

A broader configurable input-mapping system is a future capability, not a prerequisite for every
new input source.

Current features should avoid hardcoding mappings so deeply that future customization requires
rewriting input backends.

Potential future mapping capabilities include:

- per-source mappings
- named presets
- keyboard layouts
- Keyboard + Mouse layouts
- additional physical controls
- source-specific convenience mappings

Do not build a large generic mapping framework until current requirements justify it.

---

# Firmware Input Sources

PicoSwitch2 should continue to support input sources directly on the Pico where doing so produces a
useful standalone adapter.

Current/future source categories may include:

- Bluetooth gamepads
- Bluetooth keyboards
- Bluetooth mice
- companion-provided normalized controller state

Each source should ultimately feed a coherent normalized controller state or personality-specific
input representation without duplicating Switch-facing output behavior.

Do not weaken the single-active-source ownership model merely to support additional HID peers.

---

# Compatibility Closure

Use [`docs/status/compatibility-matrix.md`](docs/status/compatibility-matrix.md) as the authoritative
compatibility test matrix.

Outstanding hardware validation should be completed when relevant hardware is available, but should
not automatically block unrelated development.

Known follow-ups include:

- [ ] Targeted BattlerGC Pro rumble regression.
- [ ] Targeted BattlerGC Pro reconnect/wake regression.
- [ ] Remaining controller-family compatibility checks listed in the compatibility matrix.
- [ ] Revalidate previously working controller families after changes that materially touch their
      shared input path.

Do not combine unrelated mapping, Bluetooth, motion, USB, or compatibility changes in one pass.

---

# Android Bridge Hardware Closure

The Android controller bridge is implemented and has already passed substantial hardware
validation.

Remaining hardware checks should be maintained in STATUS.md and the compatibility documentation.

When performing a dedicated regression pass, validate as applicable:

- connection
- buttons
- sticks
- triggers
- battery
- rumble
- motion
- active-source switching
- lifecycle neutralization
- disconnect neutralization
- saved adapter relationship/reconnect

Do not reopen the bridge architecture unless a current failure demonstrates a real architectural
problem.

---

# Firmware / App Compatibility

Maintain explicit runtime compatibility between firmware and companion applications.

Preserve:

- firmware build identity
- bridge contract version
- full descriptor guard
- contract parity tests
- exact descriptor identification diagnostics
- firmware/application compatibility reporting

Any peer-observable change to:

- descriptor bytes
- wire layout
- field widths
- units
- semantics
- capabilities
- management protocol

must be evaluated against the existing contract/version rules.

Internal implementation changes do not require a contract bump merely because code changed.

---

# Reliability and Maintainability

## NS2 motion encoder rename

Rename the misleading `ns2_ds5_motion.*` implementation to a neutral name.

It is the shared translated Switch 2 motion encoder and should no longer appear DualSense-specific.

This must be performed as its own mechanical task.

Requirements:

- [ ] Choose a neutral name that reflects current ownership.
- [ ] Rename code, tests, tools, and documentation consistently.
- [ ] Preserve behavior exactly.
- [ ] Run all existing motion regression tests.
- [ ] Do not combine this rename with motion algorithm changes.

---

## `NS2_PRO=OFF` build

Repair the currently known non-NS2 build failure involving
`g_usb_reenumerate_request_pending` being referenced outside the appropriate guard.

Requirements:

- [ ] Restore clean `NS2_PRO=OFF` build.
- [ ] Add or preserve build validation so the configuration does not silently regress again.
- [ ] Keep the fix narrowly scoped.

---

## Handheld motion frame

Current handheld motion orientation uses display rotation to establish the held frame.

This is acceptable unless hardware demonstrates a failure.

Do not redesign this preemptively.

If a handheld with fixed controls produces incorrect motion because display orientation and physical
held-frame orientation diverge, investigate a source-specific fixed-frame model.

---

# Motion Research

Current production motion behavior should remain frozen unless concrete hardware evidence exposes a
defect.

Translated DualSense/DualSense Edge motion uses the validated Switch 2 length-`0x1E` carrier.

Genuine Pro Controller 2 native motion may pass through its native forms.

## Deferred translated `0x28`

Do not resume translated length-`0x28` generation unless:

- production `0x1E` exhibits a concrete gameplay deficiency, or
- a materially better observation point becomes available for the missing private state/filter
  semantics.

Do not restore the previously rejected static/template-derived generator.

The existing experiment documentation remains the authoritative evidence base.

---

## Additional motion sources

New controller families may use the shared translated motion encoder only after validating:

- sensor layout
- axis orientation
- units
- scale
- timestamps
- held-frame transform
- stationary bias behavior

Do not generalize calibration from DualSense to unrelated devices without evidence.

---

# NFC / Virtual Amiibo

Virtual Amiibo is an established production subsystem.

Future NFC work should be evidence-driven and should preserve the existing working virtual-tag path.

Remaining work is tracked primarily in STATUS.md and the dedicated NFC documentation.

Accepted future directions include:

- production validation of remaining manual Eject/Present behavior
- remaining adapter/browser sync validation
- interrupted-operation recovery validation
- native Switch 2 reader relay closure
- physical write research
- Joy-Con 2 Right native-reader validation
- Switch 1 NFC translation
- additional figure-v3 compatibility only when supported by captures

Do not replace validated Virtual Amiibo behavior with speculative protocol semantics.

Use the existing NFC laboratory, fixtures, persistence tests, and capture workflows.

---

# Native NFC Relay

Native physical NFC reader relay remains incomplete.

Current future work:

- [ ] Validate production selection behavior.
- [ ] Validate reconnect/removal behavior.
- [ ] Validate Joy-Con 2 Right.
- [ ] Research physical write behavior.
- [ ] Preserve genuine Pro Controller 2 physical-read behavior.

Switch 1 NFC support requires protocol translation and should remain separate from native Switch 2
relay work.

---

# Phone NFC

Ordinary NTAG215 backup support exists in the Android companion.

Remaining work includes:

- [ ] Hardware-validation against independently dumped ordinary Amiibo.
- [ ] Tag-removal failure handling.
- [ ] Duplicate behavior.
- [ ] Byte-exact output validation.

Do not add figure-v3 phone-RF behavior without direct evidence.

Do not bundle proprietary Nintendo resources.

---

# Audio

Current validated audio behavior should remain stable.

Future audio work should be narrowly evidence-driven.

## DualSense microphone return

Potential future work:

- decode DualSense microphone reports
- determine required audio format conversion
- provide microphone return through the existing USB audio path

Do not begin this work merely as cleanup around existing speaker support.

Existing speaker playback and haptic coexistence should remain regression-protected.

---

# Haptics

Current rumble behavior should remain frozen unless a concrete compatibility issue is found.

A more capability-based haptic translation layer remains a longer-term possibility.

Potential future research may include:

- richer DualSense haptics
- DualSense Edge
- Xbox-family haptics
- adaptive-trigger translation where it provides meaningful Switch-side behavior

Do not generalize this work until a concrete output capability or user-facing benefit is established.

---

# Protocol Research

Protocol research should continue only when it answers useful unknowns or improves fidelity.

Use the existing shared protocol laboratory rather than creating duplicate capture infrastructure.

Current reusable infrastructure includes:

- UART trace capture
- capture-to-fixture generation
- semantic transaction decoding
- command/subcommand atlas
- motion tooling
- audio tooling
- NFC tooling
- experiment runners
- provenance and artifact hashing

When a new experiment exposes an actual tooling gap, extend the shared laboratory rather than
creating a parallel ad-hoc workflow.

---

# Genuine Controller Protocol Discovery

Promote protocol discoveries only after sufficient evidence.

Where practical, require:

- zero-loss capture
- clear provenance
- semantic discriminator
- documentation update
- replay fixture for deterministic transactions

Keep observed wire shapes separate from inferred meaning.

Do not manufacture firmware-update experiments merely to satisfy an unknown. Capture genuine update
behavior when a real update opportunity occurs.

---

# Capture Infrastructure

The existing console-side UART trace infrastructure should remain available for future research.

Potential future additions should be driven by a demonstrated need, such as:

- additional sampled input tracing
- additional audio-control tracing
- fault injection
- richer semantic comparison
- high-rate trace support

Do not build a second tracing system when the existing one can be extended.

---

# Release Engineering

Each future release should be reproducible and attributable to a clean source revision.

Maintain release checks for:

- Pico W build
- Pico 2 W build
- relevant host tests
- JVM/companion tests where applicable
- descriptor parity
- contract/version guards
- signing verification
- firmware build identity
- clean repository state
- hardware sanity checks appropriate to changed subsystems

Record relevant:

- source revision
- board
- firmware build
- console firmware
- controller firmware
- companion version
- test result

Do not require unrelated exhaustive hardware testing for every small release. Match validation scope
to the changed subsystem and known regression risk.

---

# Longer-Term Direction

PicoSwitch2 is evolving from a controller adapter into a controller-input and translation platform.

The long-term architecture should support multiple kinds of input source while preserving one
coherent Switch-facing controller stream.

The preferred conceptual boundary is:

    Input source
        |
        v
    normalized / personality-aware input state
        |
        v
    translation and capability handling
        |
        v
    selected Switch controller personality
        |
        v
    PicoSwitch2 USB output
        |
        v
    Nintendo Switch 2

Future work should strengthen this architecture only when real product requirements justify it.

Avoid speculative abstractions.

---

# Explicit Non-Goals

Unless a future task deliberately changes scope, do not assume the project is attempting to support:

- multiple independent local players through one console-facing controller stream
- automatic merging of arbitrary controllers
- paired Joy-Con 2 L/R pretending to be one combined USB identity
- speculative controller protocol behavior without evidence
- wholesale replacement of vendored Joypad-OS infrastructure
- direct OS-to-Switch emulation that bypasses the Pico
- a universal cross-platform UI framework
- undocumented proprietary Nintendo assets or resources

---

# Roadmap Maintenance

When a milestone ships:

1. Update STATUS.md with the new current state.
2. Update durable feature/protocol documentation.
3. Remove completed implementation detail from this roadmap unless it explains remaining work.
4. Keep only genuinely open tasks.
5. Move historically useful milestone narratives to the archive if they remain worth preserving.

PLAN.md should stay short enough that a fresh contributor can understand the project's intended
direction without reading a history of everything PicoSwitch2 has already accomplished.

It answers one question:

> Where are we going next?