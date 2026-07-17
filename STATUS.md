# PicoSwitch2 Status

> Current-state snapshot. Historical implementation narratives are archived in
> [`docs/archive/status-through-2026-07-15.md`](docs/archive/status-through-2026-07-15.md).
> Planned work belongs in [`PLAN.md`](PLAN.md); evidence and protocol details belong under
> [`docs/`](docs/README.md).

Last verified: 2026-07-16
Branch: `ns2-testing`

## Current release

[`v1.2.0`](https://github.com/notsosaelin/PicoSwitch2/releases/tag/v1.2.0) was published on
2026-07-16 with Pico W and Pico 2 W UF2 assets. All 13 host-test executables pass. Pro Controller 2,
NSO GameCube, both individual Joy-Con 2 personalities, appearance controls, DualSense/Edge,
rumble, pairing, wake, and BOOTSEL have recent physical validation.

## Hardware-confirmed behavior

| Area | Status | Evidence |
|---|---|---|
| Switch 2 Pro Controller 2 USB identity and input | ✅ Confirmed | Real Switch 2 and PC/Steam |
| NSO GameCube USB identity and input | ✅ Confirmed | Real Switch 2 |
| NSO GameCube rumble | ✅ Confirmed | Real Switch 2; genuine-capture decoder |
| Real Pro Controller 2 input in NSO GameCube mode | ✅ Confirmed | L/R full-pull detents; ZL/ZR become GC ZL/Z |
| Joy-Con 2 Left and Right enumeration/input streaming | ✅ Confirmed | Real Switch 2 |
| Joy-Con 2 Left and Right sideways mappings | ✅ Confirmed | Real Switch 2; face/shoulder/trigger/stick profile |
| Joy-Con 2 rumble and STOP/reconnect behavior | ✅ Confirmed | Real Switch 2 |
| DualSense and DualSense Edge input | ✅ Confirmed | Real Switch 2 and Steam |
| Edge paddles, Fn buttons, and mute mapping | ✅ Confirmed | Real hardware |
| DualSense/Edge LEDs and rumble | ✅ Confirmed | Real hardware after report-boundary scheduler fix |
| Pro2 body/Joy-Con accents, Sony lightbar matching, and DualSense player-slot dots | ✅ Confirmed | Real Switch 2 and DualSense; config v8 hardware pass |
| BOOTSEL double-tap, triple-tap, and five-second hold with DualSense paired | ✅ Confirmed | Real hardware after report-boundary gesture service |
| Triple-tap post-wipe admission lock | ✅ Confirmed for the reported workflow | Wipe disconnects and requires an explicit new pairing window |
| Explicit re-pair after triple-tap wipe | ✅ Confirmed | Real hardware |
| Switch 2 wake from sleep | ✅ Confirmed | First real post-sleep controller input on real Switch 2 hardware |
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
| P2 | Joy-Con 2 Left is shown by Steam/Windows as a generic controller while Right is recognized | 🔵 PC-only compatibility issue; real-console enumeration works |
| P2 | Let reconnecting BLE controllers sleep with the console without touching bonds or admission | 🔵 Research; current automatic wake behavior is preserved |
| P3 | Console-native report `0x09` motion semantics | 🔴 Blocked on better primary evidence |
| P3 | NFC/amiibo transactions | 🔴 Blocked on a genuine console-side capture |

## Validation

Current automated coverage includes:

- DualSense Bluetooth output report layout and CRC
- Switch 2 player-LED command-mask decoding
- Xbox rumble payload construction and STOP semantics
- Genuine-capture NSO GameCube rumble decoding
- GameCube and Joy-Con 2 input report encoders
- Joy-Con 2 per-side identity and configurable accent placement
- HID output normalization
- Switch 2 pairing cryptography
- Switch 2 wake identity parsing and byte-exact advertisement construction
- USB personality cycling
- `gcusb` safety and protocol helpers

The firmware builds under the Pico SDK 2.2.0 toolchain for `pico_w` and `pico2_w`; the legacy
`NS2_PRO=OFF` Pico W configuration also passes its compile gate.

Config v8 stores a Pro Controller 2 body color plus independent Joy-Con 2 Left/Right accent colors.
Existing v5/v6 users retain their effective slot-0 color and remap/wake data; v7 body, remap, and
wake fields migrate intact. Joy-Con accents default to genuine retail values (`9B E1 E6` Left,
`FF 8C 5F` Right). Each personality advertises its configured appearance during enumeration, and
the active Pro2/Joy-Con color drives supported DualShock 4/DualSense lightbars independently of
player-indicator LEDs. Pro2 body rendering, Joy-Con accents, DualSense lightbar matching, live
player-dot reordering, and the prior wake/input/rumble baseline are hardware-confirmed.

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

1. Classify the Joy-Con 2 Left Steam/Windows behavior separately from real-console compatibility.
2. Add host coverage for BOOTSEL gesture progression under report-driven scheduling.
3. Research controller-specific sleep behavior without changing the confirmed console-wake path.
4. Build a reproducible console-side capture path before resuming NFC or report `0x09` motion work.
