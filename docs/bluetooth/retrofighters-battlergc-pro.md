# Retro Fighters BattlerGC Pro Bluetooth Profile

Status: ✅ mapping hardware-confirmed; rumble/reconnect targeted checks remain
Last updated: 2026-07-17

## Scope

This profile covers the first-generation Retro Fighters BattlerGC Pro in Bluetooth XInput mode.
The controller presents the Classic Bluetooth name `Xbox Wireless Controller`, Class of Device
`0x002508`, and no usable SDP PnP VID/PID. It is therefore not identifiable through a normal
manufacturer/product pair.

The profile is intentionally restricted to:

- Classic Bluetooth, not BLE
- exact name `Xbox Wireless Controller`
- unresolved VID and PID
- the 16-byte report-ID-`0x01` XInput layout below

Current first-party Xbox Bluetooth controllers use the BLE driver and/or Microsoft's VID. If a
Classic controller with the same exact unresolved identity but a different physical layout is
found, this heuristic must be replaced with a descriptor or pairing-fingerprint model hint.

## Pairing behavior

### Hardware observation

The controller originally reached a successful outgoing ACL connection and then stalled before
authentication, encryption, SDP, or HID setup. BlueRetro's successful trace explicitly requested
SSP security immediately after ACL establishment. PicoSwitch2 now does the same only for the exact
Classic name above.

The successful Pico trace is:

1. outgoing ACL succeeds;
2. Pico requests security level 2;
3. authentication and encryption succeed;
4. the controller closes the first outgoing HID/ACL attempt;
5. it reconnects inbound;
6. authentication/encryption repeat and HID opens successfully.

Pairing and the main input layout are hardware-confirmed. The distinct shoulders, click-gated
trigger curve, GameCube-mode L3/R3 suppression, and separate Home report have also passed the
focused physical checks.

## Input report

Report length is 16 bytes including report ID:

| Offset | Size | Meaning |
|---|---:|---|
| `0x00` | 1 | Report ID `0x01` |
| `0x01` | 2 | Left X, unsigned little-endian `0..65535` |
| `0x03` | 2 | Left Y, unsigned little-endian `0..65535` |
| `0x05` | 2 | Right X, unsigned little-endian `0..65535` |
| `0x07` | 2 | Right Y, unsigned little-endian `0..65535` |
| `0x09` | 2 | Left analog trigger, `0..1023` |
| `0x0B` | 2 | Right analog trigger, `0..1023` |
| `0x0D` | 1 | Hat: neutral `0`, N/NE/E/SE/S/SW/W/NW = `1..8` |
| `0x0E` | 2 | Consecutive button bitfield |

Captured neutral:

```text
01 00 80 ff 7f 00 80 ff 7f 00 00 00 00 00 00 00
```

The old Classic Xbox parser treated the stick words as signed values. That interpreted neutral
`0x8000` as full negative deflection. The BattlerGC profile and corrected base Classic-Xbox
decoder both use the observed unsigned range, matching the already-correct Xbox BLE decoder.

## Button mapping

The BlueRetro issue trace pressed controls in a declared order and produced clean consecutive bits
for A/B/X/Y, both shoulders, Select, and Start. Firmware `HOME+B` exposes the trigger clicks in the
next two sequential positions; Home uses the separate report-ID-`0x02` event documented below.

| Physical control | Raw bit | Pro2 normalized source | NSO GameCube meaning |
|---|---:|---|---|
| A | `0x0001` | A (direct label) | A |
| B | `0x0002` | B (direct label) | B |
| X | `0x0004` | X (direct label) | X |
| Y | `0x0008` | Y (direct label) | Y |
| Left upper shoulder | `0x0010` | L | native ZL |
| Right upper shoulder | `0x0020` | R | native Z (displayed as ZR) |
| Select/View | `0x0040` | Minus | Minus |
| Start/Menu | `0x0080` | Plus | Plus |
| Left trigger click | `0x0100` | L3 + full trigger | full analog L + L detent; transported L3 discarded |
| Right trigger click | `0x0200` | R3 + full trigger | full analog R + R detent; transported R3 discarded |
| Home | report `02 01` pressed / `02 00` released | Home | Home |
| Left analog trigger | bytes `0x09..0x0A` | ZL via normal Pro2 fold | continuous analog L |
| Right analog trigger | bytes `0x0B..0x0C` | ZR via normal Pro2 fold | continuous analog R |

Face buttons are mapped by their printed GameCube labels, not Xbox cluster position. The left and
right upper shoulders remain separate: left is native NSO-GC ZL and right is native Z (the console
labels that slot ZR). They remain ordinary separate L/R sources in Pro Controller output mode.

Trigger analog values are not converted into L2/R2 inside the Battler-specific decoder. The shared
router performs its existing analog-to-ZL/ZR fold in Pro2 mode and suppresses it in NSO GameCube
mode, where the continuous values and independent detent bits are emitted instead.

Hardware capture shows the physical analog value reaches raw `1020` around half the trigger's
mechanical travel and then provides no more position information before the click. The adapter
cannot reconstruct that missing portion. The Battler decoder linearly maps the available raw
`0..1020` range into output `0..223`; the matching real click emits `255` and the independent
detent bit. Pro2 mode preserves the controller's transported L3/R3 destinations. Because the NSO
GameCube output has no physical L3/R3 controls, that personality discards those two transported
meanings. Only the analog trigger and its native detent remain, preventing a full trigger click
from firing an unrelated Capture or C/GameChat action.

To expose the physical trigger clicks on the tested hardware, connect in Bluetooth XInput mode and
use `HOME+B`.

Home is not present in the streaming report-ID-`0x01` button field. Live capture on 2026-07-17
showed a separate two-byte event report: `02 01` while pressed and `02 00` when released. The
driver latches that state across subsequent `0x01` reports so the normal input stream cannot erase
the Home press immediately. The top screenshot/circle button produced no distinguishable input
report in this Bluetooth XInput mode and therefore remains unavailable.

## P1/P2

P1 and P2 are programmable macro buttons. Live config captures of P1 alone, P2 alone, and both
together all remained byte-for-byte neutral. They are not independent host-visible controls in
this Bluetooth profile, so PicoSwitch2 cannot map them unless controller firmware/profile
programming is changed to emit a distinguishable report.

## Output and unknowns

- Pairing: ✅ hardware-confirmed
- Ordinary input connection: ✅ hardware-confirmed
- Main raw button/axis layout: ✅ captured / source-tested
- Main face, stick, D-pad, trigger, and center mapping: ✅ hardware-confirmed
- Distinct ZL/Z shoulder correction and `0..223` click-gated curve: ✅ hardware-confirmed
- Separate Home report and GameCube-mode L3/R3 suppression: ✅ hardware-confirmed
- P1/P2 independent raw controls: 🔴 unavailable in captured Bluetooth reports
- Rumble: 🔵 existing Xbox Classic output path retained; Battler-specific hardware validation
  pending
- Reconnect and wake: 🟡 targeted regression pending

## References

- Retro Fighters, BattlerGC Pro product page and firmware instructions:
  <https://retrofighters.com/our-collection/battlergc-pro/>
- Laser Bear, BattlerGC Pro BlueRetro configuration:
  <https://www.laserbear.net/blogs/documentation/retro-fighters-battler-gc-pro-connection-guide-for-blueretro>
- BlueRetro issue #1348 and attached XInput/Switch traces:
  <https://github.com/darthcloud/BlueRetro/issues/1348>
- Dedicated driver, decoder, and regression test:
  `src/bt_hid/bt/bthid/devices/vendors/retrofighters/battlergc_pro.c`,
  `src/bt_hid/bt/bthid/devices/vendors/retrofighters/battlergc_pro_report.c`,
  `tools/test_battlergc_pro_report.c`
