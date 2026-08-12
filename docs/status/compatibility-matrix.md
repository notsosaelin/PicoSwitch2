# Compatibility Matrix

Last updated: 2026-08-12

This matrix records observed behavior, not inferred support. "Source-tested" means host tests or
code inspection only; it is weaker than physical hardware confirmation.

## Output personalities

| Personality | Switch 2 enumeration | Input streaming | Rumble | Notes |
|---|---|---|---|---|
| Pro Controller 2 | ✅ Confirmed | ✅ Confirmed, including native motion from a genuine Pro2 and translated motion from DualSense/Edge | ✅ Confirmed | Primary/default mode; 1000 Hz USB interval remains a required deviation for current motion behavior |
| NSO GameCube | ✅ Confirmed | ✅ Confirmed | ✅ Confirmed | Genuine `1.1.2 / 12.0.0` firmware identity and up-to-date status confirmed; native Z and trigger detents supported. **Motion capability present but not yet emitted** — the confirmed report `0x0A` carries the same motion block as Pro2's 0x09 (length @`0xE`, data @`0xF`, feature-bit-2 activated); implementable via the report-0x09 int32 path (see docs/switch2-gc/open-questions.md) |
| Joy-Con 2 Left | ✅ Confirmed | ✅ Confirmed | ✅ Confirmed | Sideways controls, mouse pointer/buttons/wheel, rumble, STOP, reconnect, and current firmware identity confirmed |
| Joy-Con 2 Right | ✅ Confirmed | ✅ Confirmed | ✅ Confirmed | Sideways controls, mouse pointer/buttons/wheel, rumble, STOP, reconnect, and current firmware identity confirmed |
| CDC/config | ✅ Confirmed with CDC-only descriptor and local portal | N/A | N/A | No mass-storage interface or embedded page |

## Bluetooth controllers

| Controller | Pair/input | Extra controls | LED/output | Rumble | Open validation |
|---|---|---|---|---|---|
| DualSense | ✅ Confirmed | ✅ Calibrated Switch 2 motion | ✅ Confirmed | ✅ Confirmed | Splatoon 3 direction, scale, rapid motion, stationary behavior, reconnect, and audio coexistence confirmed |
| DualSense Edge | ✅ Confirmed | ✅ Paddles/Fn/mute and calibrated Switch 2 motion | ✅ Confirmed | ✅ Confirmed | Motion and ordinary input/output regression pass complete |
| Xbox family | ✅ Confirmed | Elite paddles supported | N/A | ✅ Confirmed | Late BLE DIS identity plus ON/STOP/reconnect confirmed |
| Switch 2 Pro Controller | ✅ Confirmed | C/GL/GR plus native motion passthrough | ✅ P1 restores after HOME reconnect | ✅ Confirmed | Splatoon 3 axes/stationary hold plus 20 controller-off/HOME reconnect cycles confirmed without SYNC; input, LED, and gyro restore |
| Joy-Con 2 input | ✅ Confirmed | Side-specific controls | Player LED path present | 🔵 Source-tested | Mapping and rumble fidelity |
| Switch 1 Pro Controller | ✅ Confirmed | ✅ Standard controls | ✅ Player LEDs | ✅ Confirmed | Current regression pass complete; reconnect remains asleep and a later fresh press wakes |
| Retro Fighters BattlerGC Pro | ✅ Confirmed mapping | ✅ Dedicated native GC profile | N/A | 🔵 Xbox-compatible output | Labels, ZL/Z, click-gated triggers, Home report `02`, and GC L3/R3 suppression confirmed; screenshot and P1/P2 emit no independent Bluetooth state |
| 8BitDo NGC Modkit | ✅ Confirmed | Native GC capability mapping | N/A | ✅ Confirmed | BlueRetro-derived `0xA5 / DB LL RR` rumble works on hardware; second Android/D-input pairing mode |
| 8BitDo Ultimate 2 MG | ✅ Confirmed | ✅ Paddles confirmed | N/A | 🔵 Source-tested | Quirk-refactor hardware regression pass complete |
| 8BitDo Ultimate Bluetooth (first model) | ✅ Confirmed | ✅ P1/P2 custom transport to GL/GR | Player LED path present | 🔵 Existing Switch output | Custom firmware paddles and console wake confirmed; reconnect remains slower than other Classic controllers |
| Wiimote family | ✅ Confirmed | ✅ Standalone and attachment input | ✅ Player LEDs | ✅ Confirmed | Current regression pass complete |
| Generic Bluetooth HID mouse | ✅ Confirmed in Joy-Con 2 modes | Buttons and relative X/Y/wheel | N/A | N/A | Pointer activation, mouse-only gating, disconnect cleanup, and wheel navigation confirmed |
| Android Controller Bridge (Retroid Pocket Classic + AYN Thor) | 🔵 Pico host-tested + Android ADB-audited on two devices | 14-button contract, D-pad, two sticks, analog triggers | N/A | N/A | Retroid API-34 and AYN Thor API-33 both expose the HID Device service and map onto the fixed contract (Thor validates the BRAKE/GAS trigger fallback); ordinary APK registration, pairing, and end-to-end hardware validation pending |

## Switch 2 motion

| Source | Console carrier | Status |
|---|---|---|
| NSO GameCube output (motion) | Report `0x0A`, length @`0xE` + data @`0xF` (same block as Pro2 0x09) | 🔵 Format confirmed in the report (feature-bit-2 activated, values {0,30,40}); **not yet emitted** — adds via the report-0x09 int32 path once that lands |
| Genuine Pro Controller 2 | Native length-`0x1E` and length-`0x28` PDUs | ✅ Opaque passthrough, reconnect, P1 LED, and gyro confirmed |
| DualSense / DualSense Edge | Synthesized length-`0x1E` quaternion PDU | ✅ Calibration, direction, scale, rapid movement, stationary behavior, and reconnect confirmed |
| Other IMU controllers | None | 🔵 Requires a verified sensor layout, calibration, axes, scale, timestamp, and bias model per family |
| Synthetic length-`0x28` | Disabled and removed | ❌ Static-template experiment caused random motion; unresolved lanes are semantically active |

## BOOTSEL and pairing

| Scenario | Status |
|---|---|
| Former double/triple/hold scheduling with DualSense/Edge connected | ✅ Hardware-confirmed baseline |
| Revised paired/unpaired/Config action matrix | ✅ Confirmed (components validated on hardware — single-tap cycle, double-tap, triple-tap, and hold all confirmed; PLAN.md release gate) |
| Single-tap controller-only personality cycle | ✅ Confirmed (owner, 2026-08-12): routine BOOTSEL personality cycle on a live Switch 2 — the console detects the new controller and drops the old one |
| Two-second direct Config entry and Config→Pro2 exit | ✅ Confirmed during CDC-only Virtual Amiibo validation |
| Paired double-tap disconnect-without-bond-delete then pairing | ✅ Confirmed (PLAN.md: double-tap validated on hardware with DualSense connected) |
| Triple-tap wipe from normal and Config modes | ✅ Confirmed (PLAN.md: triple-tap validated on hardware; triple-tap admission blocking hardware-confirmed) |
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
| Config save/readback after reconnect | ✅ Confirmed for the former schema (colors); v10/reset-on-flash regression pending |
| Cold boot | ✅ Confirmed |
| Console wake | ✅ Ten attempts with every known controller |
| Real-console input/wake during audio | ✅ Confirmed |
| Real-console rumble during audio | ✅ Confirmed with peak-preserving 3.25× native PCM; judged close to HD Rumble |
| Extended playback/thermal soak | ✅ Eight-hour Smash session; no observed thermal or stability issue (temperature not instrumented) |

## Genuine Pro Controller 2 audio

| Scenario | Status |
|---|---|
| Physical headset insertion/removal | ✅ Confirmed; Switch 2 routes and withdraws audio automatically |
| Live console audio | ✅ Clean output through genuine controller headphone jack |
| Input, native gyro, and rumble during audio | ✅ Confirmed |
| LED and BOOTSEL during audio | ✅ Confirmed |
| Microphone return | 🔵 Not implemented/tested |

## NFC / Virtual Amiibo

| Scenario | Status |
|---|---|
| Genuine Pro2 physical amiibo read relay | ✅ Confirmed through the UART-gated diagnostic bridge |
| Virtual Amiibo read with a non-NFC source controller | ✅ Confirmed on a real Switch 2 |
| Virtual Amiibo game-owned write and completion | ✅ Complete 88-byte staging, `0x08`, and `05 00` confirmed without a crash |
| Post-write logical eject | ✅ Confirmed as absent `07 41`; former rescan loop eliminated |
| Next-scan re-presentation and updated read | ✅ Same selected UID and complete 600-byte-buffer read confirmed |
| Live UART export while USB remains console-attached | ✅ 540-byte generation-stable, UID/BCC-valid mutated image saved |
| Automatic write-before-eject flash snapshot | ✅ Live-console completion and power-cycle recovery confirmed |
| Single-slot image and alternating-bank persistence generations | ✅ Confirmed; the board holds exactly one amiibo, and the two flash banks are generations of it |
| Offline browser library and full-library backup | 🟡 Single-slot flow confirmed; catalog-ID dedupe and `.zip` backup schema await browser regression |
| Config-only Bluetooth library transfer | 🟡 Host/build/static confirmed; hardware pending |
| New-UF2 blank state and one-shot persistence/bond erase | 🟡 Build marker verified on all targets; hardware pending |
| Native Pro2/Joy-Con 2 physical-tag write | 🔵 Pending capture and implementation |
| Virtual Amiibo in the **Joy-Con 2 Right** output personality | 🔵 Feasibility only — Right's NFC hardware is confirmed (live NFC-state byte @`0xE`, PN7160/PN7161), but its NFC *command* protocol is undocumented. Candidate experiment: wire Pro2's NFC serving into JoyCon2 R's `0x01` handler and validate the console queries it. Left has no NFC. See docs/switch2-joycon2/open-questions.md |

### Figure-v3 (NTAG I2C Plus 2K / Kirby Air Riders, 2048-byte)

Full protocol reference: [`../Amiibo-v3.md`](../Amiibo-v3.md).

| Scenario | Status |
|---|---|
| 2048-byte v3 read path (`0x14`/`0x21` device command, 83-byte `0x18` result, descriptor-driven page ranges) | ✅ Confirmed on a real Switch 2 |
| Per-response SRAM CRC-16/MCRF4XX trailer | ✅ Confirmed; per-dump, not a fixed controller constant |
| Untouched downloaded dump accepted with no signature override | ✅ Confirmed (`Kirby & Warp Star.bin`) |
| Ordinary v3 write, `0x08` commit, `05 00`, Stop/eject | ✅ Confirmed, zero write errors |
| Air Riders two-stage extended write (`0x20`, 355-byte clear + 167-byte update) | ✅ Confirmed; 18 ordinary chunks, 3 × `0x08`, 8 extended chunks, 2 × `0x20` |
| Sector-aware extended read (`0x1E`, 196-byte result in three `0x15` chunks) | ✅ Confirmed |
| Dynamic sector-1 page-0 capability generation (`A5 00 0n 00`) | ✅ Confirmed; advances and survives a power cycle |
| Allocation-relative storage derived from the envelope (no figure/UID whitelist) | ✅ Confirmed against Kirby (`0x92`, `0x00/0x01`) and King Dedede (`0xB2`, `0x64/0x65`) |
| Full available dump set | ✅ All 16 Kirby Air Riders v3 dumps completed real-console read **and** write |
| Persisted export integrity | ✅ HMAC-valid, SRAM-valid, retained nickname/owner, both sector-0 and sector-1 game records |
| Non-cosmetic learned gameplay state round-trip | ✅ Confirmed; page 4 `05 → 06`, sector-1 page 0 `03 → 04` |
| Production-portal Sync of a retained dirty generation | 🟡 Remaining lifecycle check |

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
Motion: pass/fail/not applicable
Audio/headset lifecycle: pass/fail/not applicable
Reconnect: pass/fail
Double/triple/hold: pass/fail
Notes:
```

Do not upgrade a row to Confirmed without recording the physical result in this file or a linked
experiment report.
