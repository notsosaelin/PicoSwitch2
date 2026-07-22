# Compatibility Matrix

Last updated: 2026-07-21

This matrix records observed behavior, not inferred support. "Source-tested" means host tests or
code inspection only; it is weaker than physical hardware confirmation.

## Output personalities

| Personality | Switch 2 enumeration | Input streaming | Rumble | Notes |
|---|---|---|---|---|
| Pro Controller 2 | ✅ Confirmed | ✅ Confirmed, including native motion from a genuine Pro2 source | ✅ Confirmed | Primary/default mode; 1000 Hz USB interval remains a required deviation for current native-motion passthrough |
| NSO GameCube | ✅ Confirmed | ✅ Confirmed | ✅ Confirmed | Genuine `1.1.2 / 12.0.0` firmware identity and up-to-date status confirmed; native Z and trigger detents supported |
| Joy-Con 2 Left | ✅ Confirmed | ✅ Confirmed | ✅ Confirmed | Sideways buttons, stick mapping, rumble, STOP, reconnect, and current firmware identity confirmed; console reports up to date |
| Joy-Con 2 Right | ✅ Confirmed | ✅ Confirmed | ✅ Confirmed | Sideways buttons, stick mapping, rumble, STOP, reconnect, and current firmware identity confirmed; console reports up to date |
| CDC/config | ✅ Confirmed | N/A | N/A | Web Serial configuration and read-only MSC page |

## Bluetooth controllers

| Controller | Pair/input | Extra controls | LED/output | Rumble | Open validation |
|---|---|---|---|---|---|
| DualSense | ✅ Confirmed | N/A | ✅ Confirmed | ✅ Confirmed | Current regression pass complete |
| DualSense Edge | ✅ Confirmed | ✅ Paddles/Fn/mute | ✅ Confirmed | ✅ Confirmed | Current regression pass complete |
| Xbox family | ✅ Confirmed | Elite paddles supported | N/A | ✅ Confirmed | Late BLE DIS identity plus ON/STOP/reconnect confirmed |
| Switch 2 Pro Controller | ✅ Confirmed | C/GL/GR plus native motion passthrough | ✅ P1 restores after HOME reconnect | ✅ Confirmed | Splatoon 3 axes/stationary hold plus 20 controller-off/HOME reconnect cycles confirmed without SYNC; input, LED, and gyro restore |
| Joy-Con 2 input | ✅ Confirmed | Side-specific controls | Player LED path present | 🔵 Source-tested | Mapping and rumble fidelity |
| Switch 1 Pro Controller | ✅ Confirmed | ✅ Standard controls | ✅ Player LEDs | ✅ Confirmed | Current regression pass complete; reconnect remains asleep and a later fresh press wakes |
| Retro Fighters BattlerGC Pro | ✅ Confirmed mapping | ✅ Dedicated native GC profile | N/A | 🔵 Xbox-compatible output | Labels, ZL/Z, click-gated triggers, Home report `02`, and GC L3/R3 suppression confirmed; screenshot and P1/P2 emit no independent Bluetooth state |
| 8BitDo NGC Modkit | ✅ Confirmed | Native GC capability mapping | N/A | ✅ Confirmed | BlueRetro-derived `0xA5 / DB LL RR` rumble works on hardware; second Android/D-input pairing mode |
| 8BitDo Ultimate 2 MG | ✅ Confirmed | ✅ Paddles confirmed | N/A | 🔵 Source-tested | Quirk-refactor hardware regression pass complete |
| 8BitDo Ultimate Bluetooth (first model) | ✅ Confirmed | ✅ P1/P2 custom transport to GL/GR | Player LED path present | 🔵 Existing Switch output | Custom firmware paddles and console wake confirmed; reconnect remains slower than other Classic controllers |
| Wiimote family | ✅ Confirmed | ✅ Standalone and attachment input | ✅ Player LEDs | ✅ Confirmed | Current regression pass complete |

## BOOTSEL and pairing

| Scenario | Status |
|---|---|
| Double-tap with DualSense/Edge connected | ✅ Confirmed |
| Triple-tap with DualSense/Edge connected | ✅ Confirmed |
| Five-second mode hold with DualSense/Edge connected | ✅ Confirmed |
| DualSense/Edge rumble while gestures remain responsive | ✅ Confirmed |
| Post-wipe automatic readmission remains blocked | ✅ Confirmed for reported workflow; include in release matrix |
| Re-pair after explicit new pairing window | ✅ Confirmed |

## DualSense audio

| Scenario | Status |
|---|---|
| Pico 2 W: live Windows PCM → DualSense internal speaker at 300 MHz | ✅ Confirmed continuous; zero PCM drops/errors |
| Pico W: fixed-point/XIP live PCM at 300 MHz | ❌ Rejected; audio barely played on hardware and the standard build is non-audio |
| Existing-bond reconnect without a fresh pair | ✅ Confirmed after controller-only and dongle power cycles; audio and native rumble return |
| No physical headset connected | ✅ Confirmed: Switch does not route console audio to the bare DualSense speaker |
| Physical headset insertion and console output | ✅ Confirmed: Switch 2 recognizes the jack and plays through DualSense-connected headphones |
| Physical headset removal/reinsert | ✅ Input, audio, and native haptics restore through repeated cycles |
| LED and BOOTSEL during 300 MHz regression pass | ✅ Confirmed |
| Config save/readback after reconnect | ✅ Confirmed (mappings and colors) |
| Cold boot | ✅ Confirmed |
| Console wake | ✅ Ten attempts with every known controller |
| Real-console input/wake during audio | ✅ Confirmed |
| Real-console rumble during audio | ✅ Confirmed with peak-preserving 3.25× native PCM; judged close to HD Rumble |
| Extended playback/thermal soak | ✅ Eight-hour Smash session; no observed thermal or stability issue (temperature not instrumented) |

## PC-specific behavior

- Pro Controller 2 and NSO GameCube enumerate on Windows/Steam.
- Joy-Con 2 Right is recognized by Steam.
- Joy-Con 2 Left and Right are recognized after the Windows WinUSB interface-property fix;
  fresh Windows-node and Steam UI validation is complete.

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
