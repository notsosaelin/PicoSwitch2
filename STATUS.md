# PicoSwitch2 Status

> Current-state snapshot. Historical implementation narratives are archived in
> [`docs/archive/status-through-2026-07-15.md`](docs/archive/status-through-2026-07-15.md).
> Planned work belongs in [`PLAN.md`](PLAN.md); evidence and protocol details belong under
> [`docs/`](docs/README.md).

Last verified: 2026-07-15
Branch: `ns2-testing`

## Release readiness

PicoSwitch2 is in release-candidate stabilization. Both Pico W and Pico 2 W builds succeed, all
ten host-test executables pass, and the primary Pro Controller 2, NSO GameCube, DualSense, and
BOOTSEL paths have recent physical validation. The next release should preserve this baseline
before new protocol work begins.

## Hardware-confirmed behavior

| Area | Status | Evidence |
|---|---|---|
| Switch 2 Pro Controller 2 USB identity and input | ✅ Confirmed | Real Switch 2 and PC/Steam |
| NSO GameCube USB identity and input | ✅ Confirmed | Real Switch 2 |
| NSO GameCube rumble | ✅ Confirmed | Real Switch 2; genuine-capture decoder |
| Joy-Con 2 Left and Right enumeration/input streaming | ✅ Confirmed | Real Switch 2 |
| DualSense and DualSense Edge input | ✅ Confirmed | Real Switch 2 and Steam |
| Edge paddles, Fn buttons, and mute mapping | ✅ Confirmed | Real hardware |
| DualSense/Edge LEDs and rumble | ✅ Confirmed | Real hardware after report-boundary scheduler fix |
| BOOTSEL double-tap, triple-tap, and five-second hold with DualSense paired | ✅ Confirmed | Real hardware after report-boundary gesture service |
| Triple-tap post-wipe admission lock | ✅ Confirmed for the reported workflow | Wipe disconnects and requires an explicit new pairing window |
| Pico W and Pico 2 W builds | ✅ Confirmed | Local release builds |

## Current USB personalities

Every boot starts in Pro Controller 2 mode. A five-second BOOTSEL hold advances the volatile cycle:

1. Switch 2 Pro Controller 2 (`057E:2069`)
2. NSO GameCube Controller (`057E:2073`)
3. Joy-Con 2 Left (`057E:2067`, experimental)
4. Joy-Con 2 Right (`057E:2066`, experimental)
5. CDC/configuration mode (`CAFE:4012`)
6. Back to Pro Controller 2

The selection is not persisted across power cycles.

## Bluetooth and BOOTSEL architecture

- Core 1 runs BTstack plus the vendored joypad-os HID layer.
- A persistent global pairing lock is installed before triple-tap disconnect/erase begins. Only an
  explicit double-tap pairing window reopens admission.
- Switch 2 controllers use a custom ATT pairing handshake, so the wipe policy cannot depend only on
  BTstack's LE bond database.
- Core 0 samples BOOTSEL using a cooperative cross-core SRAM handshake at a 30 ms cadence.
- Incoming HID report boundaries service raw BOOTSEL sampling, gesture recognition, and
  `bthid_task()`. This prevents sustained DualSense Classic traffic from starving controller output
  or button gestures. The timers remain the quiet/disconnected fallback.

See [`docs/architecture/overview.md`](docs/architecture/overview.md) and
[`docs/bluetooth/btstack-implementation.md`](docs/bluetooth/btstack-implementation.md).

## Known open issues

| Priority | Issue | State |
|---|---|---|
| P1 | Joy-Con 2 Left/Right button mapping needs a complete real-console verification pass | 🟡 In progress |
| P1 | Confirm the Pro Controller 2 physical L/R mapping correction | 🟡 Needs hardware validation |
| P2 | Joy-Con 2 Left is shown by Steam/Windows as a generic controller while Right is recognized | 🔵 PC-only compatibility issue; real-console enumeration works |
| P2 | Complete the controller/personality regression matrix, including rumble STOP and reconnect | 🟡 In progress |
| P2 | Wake-from-sleep advertisement support | ⬜ Designed, not implemented |
| P3 | Console-native report `0x09` motion semantics | 🔴 Blocked on better primary evidence |
| P3 | NFC/amiibo transactions | 🔴 Blocked on a genuine console-side capture |

## Validation

Current automated coverage includes:

- DualSense Bluetooth output report layout and CRC
- Xbox rumble payload construction and STOP semantics
- Genuine-capture NSO GameCube rumble decoding
- GameCube and Joy-Con 2 input report encoders
- HID output normalization
- Switch 2 pairing cryptography
- USB personality cycling
- `gcusb` safety and protocol helpers

The firmware builds under the Pico SDK 2.2.0 toolchain for `pico_w` and `pico2_w`; the legacy
`NS2_PRO=OFF` Pico W configuration also passes its compile gate.

## Documentation map

- [`docs/README.md`](docs/README.md) — documentation index and authority rules
- [`docs/status/compatibility-matrix.md`](docs/status/compatibility-matrix.md) — controller/personality validation
- [`docs/architecture/overview.md`](docs/architecture/overview.md) — runtime architecture and data flow
- [`docs/re-methodology/evidence-standards.md`](docs/re-methodology/evidence-standards.md) — evidence tiers and experiment rules
- [`docs/switch2/`](docs/switch2/) — Pro Controller 2 protocol
- [`docs/switch2-gc/`](docs/switch2-gc/) — NSO GameCube protocol and mapping
- [`docs/switch2-joycon2/`](docs/switch2-joycon2/) — Joy-Con 2 protocol and mapping
- [`docs/bluetooth/`](docs/bluetooth/) — Bluetooth host, identity, pairing, and controller profiles
- [`docs/experiments/`](docs/experiments/) — immutable experiment records and refuted hypotheses

## Next recommended work

1. Tag and release the current hardware-confirmed baseline.
2. Execute the compatibility matrix without changing protocol code mid-pass.
3. Correct Joy-Con 2 mappings from explicit button-by-button observations.
4. Add host coverage for BOOTSEL gesture progression under report-driven scheduling.
5. Build a reproducible console-side capture path before resuming NFC or report `0x09` motion work.
