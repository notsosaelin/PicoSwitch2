# Compatibility Matrix

Last updated: 2026-07-15

This matrix records observed behavior, not inferred support. "Source-tested" means host tests or
code inspection only; it is weaker than physical hardware confirmation.

## Output personalities

| Personality | Switch 2 enumeration | Input streaming | Rumble | Notes |
|---|---|---|---|---|
| Pro Controller 2 | ✅ Confirmed | ✅ Confirmed | ✅ Confirmed | Primary/default mode; 1000 Hz USB interval is a deliberate deviation |
| NSO GameCube | ✅ Confirmed | ✅ Confirmed | ✅ Confirmed | Genuine-capture state decoder; native Z and trigger detents supported |
| Joy-Con 2 Left | ✅ Confirmed | ✅ Confirmed | 🔵 Provisional | Button mapping needs a complete real-console pass |
| Joy-Con 2 Right | ✅ Confirmed | ✅ Confirmed | 🔵 Provisional | Button mapping needs a complete real-console pass |
| CDC/config | ✅ Confirmed | N/A | N/A | Web Serial configuration and read-only MSC page |

## Bluetooth controllers

| Controller | Pair/input | Extra controls | LED/output | Rumble | Open validation |
|---|---|---|---|---|---|
| DualSense | ✅ Confirmed | N/A | ✅ Confirmed | ✅ Confirmed | Regression matrix across every output personality |
| DualSense Edge | ✅ Confirmed | ✅ Paddles/Fn/mute | ✅ Confirmed | ✅ Confirmed | Regression matrix across every output personality |
| Xbox family | ✅ Previously confirmed | Elite paddles supported | N/A | ✅ Pro2 and GC paths observed during development | Explicit STOP/reconnect matrix |
| Switch 2 Pro Controller | ✅ Confirmed | C/GL/GR supported | Player LED path present | 🔵 Source-tested | Physical L/R mapping correction |
| Joy-Con 2 input | ✅ Confirmed | Side-specific controls | Player LED path present | 🔵 Source-tested | Mapping and rumble fidelity |
| Switch 1 Pro Controller | ✅ Previously confirmed | Standard controls | Player LED path present | ✅ Previously confirmed | Fresh release regression pass |
| 8BitDo NGC Modkit | ✅ Confirmed | Native GC capability mapping | N/A | 🔵 Source-tested | Second Android/D-input pairing mode |
| 8BitDo Ultimate 2 MG | ✅ Identity captured | Paddles implemented | N/A | 🔵 Source-tested | Paddle hardware re-test |
| Wiimote family | ✅ Previously confirmed | Attachment parsing present | Player LEDs | ✅ Previously confirmed | Channel/fidelity regression pass |

## BOOTSEL and pairing

| Scenario | Status |
|---|---|
| Double-tap with DualSense/Edge connected | ✅ Confirmed |
| Triple-tap with DualSense/Edge connected | ✅ Confirmed |
| Five-second mode hold with DualSense/Edge connected | ✅ Confirmed |
| DualSense/Edge rumble while gestures remain responsive | ✅ Confirmed |
| Post-wipe automatic readmission remains blocked | ✅ Confirmed for reported workflow; include in release matrix |
| Re-pair after explicit new pairing window | 🟡 Include in release matrix |

## PC-specific behavior

- Pro Controller 2 and NSO GameCube enumerate on Windows/Steam.
- Joy-Con 2 Right is recognized by Steam.
- Joy-Con 2 Left currently appears as a generic `Nintendo Joy-Con 2 (L)` with a setup prompt.
  Real-console enumeration works, so this is tracked as a Windows/Steam identity/cache issue rather
  than a Switch 2 protocol failure.

## Release test record template

For each physical pass, record:

```text
Firmware commit:
Board: pico_w | pico2_w
Console firmware:
Controller model and firmware:
Output personality:
Enumeration: pass/fail
Buttons/axes: pass/fail + exceptions
Rumble ON/STOP: pass/fail
Reconnect: pass/fail
Double/triple/hold: pass/fail
Notes:
```

Do not upgrade a row to Confirmed without recording the physical result in this file or a linked
experiment report.
