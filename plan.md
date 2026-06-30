# PicoSwitch2 — Project Status & Handoff

Wireless gamepad adapter: pair up to 4 Bluetooth controllers and present them to a
Nintendo Switch / Switch 2 as **native Pro Controllers** (VID 057E / PID 2009),
with native in-system remapping, gyro, and rumble. Builds for **Pico W (RP2040)**
and **Pico 2 W (RP2350)** from one tree.

Forked from juan518munoz/PicoSwitch-WirelessGamepadAdapter. Pro Controller protocol
ported from the MIT-licensed bmelanman/retro-pico-switch.

---

## Build & flash

```
./build.ps1            # both boards   ./build.ps1 pico_w   ./build.ps1 -Clean
```
Uses the Pico VS Code extension's private toolchain under `C:\Users\notso\.pico-sdk`
(SDK 2.2.0, ARM GCC 14_2_Rel1, CMake 3.31.5, Ninja 1.12.1) — no global installs.
Artifacts: `build/pico_w/PicoSwitchWGA-pico_w.uf2`, `build/pico2_w/PicoSwitchWGA-pico2_w.uf2`.
Flash by holding BOOTSEL while plugging into a PC and dropping the `.uf2`.

**Division of labor:** Claude edits + compiles both boards; the user flashes and tests
on real hardware (Switch OG / Lite / Switch 2, Pico 2 W). Console behaviour can't be
verified by Claude.

**Gotchas:** in PowerShell, quote cmake `-D` args (`"-DPICO_BOARD=$b"`) or the var
isn't expanded; don't set `$ErrorActionPreference='Stop'` around native cmake calls.
After editing `web/index.html`, regenerate the embedded copy:
`python tools/make_web_disk.py web/index.html src/web_disk.h`, then build.

---

## Architecture

- **core0** — TinyUSB device. Normal mode = N Pro Controller HID interfaces to the
  console. Config mode = composite CDC + read-only MSC.
- **core1** — bluepad32 + btstack: Bluetooth host to the controllers; also owns the
  LED, BOOTSEL gestures, pairing, rumble/lightbar output, and the settings flash write.
- **Cross-core:** `report.c` shares per-controller input (BT→USB) and a rumble channel
  (USB→BT) under a hardware `critical_section`. The settings flash write runs on core1,
  parking core0 via `multicore_lockout` (the same mechanism used to read BOOTSEL).

### Source map
| File | Role |
|---|---|
| `src/main.c` | entry; `report_init()` + `config_load()`; launch core1; run USB on core0 |
| `src/usb.c` | core0 loop: HID reports, config-mode CDC, suspend remote-wakeup; mode switch |
| `src/usb_descriptors.c` | USB descriptors — HID Pro Controller **and** CDC+MSC config, mode-switched |
| `src/switch_pro/switch_pro.c` | Pro Controller protocol (handshake, subcommands, SPI calib, 0x30/0x21/0x81) |
| `src/pico_switch_platform.c` | bluepad32 callbacks; input→remap; LED+BOOTSEL+pairing; rumble+lightbar; family detect |
| `src/bootsel.c` | BOOTSEL gesture engine (double/triple tap, 5s hold), `multicore_lockout`-safe, RP2040/RP2350 |
| `src/report.c` | cross-core shared input + rumble + any-button-pressed |
| `src/config.c` | persistent settings (flash) + config-mode CDC command protocol |
| `src/msc.c` | read-only USB MSC disk that serves the config web page |
| `src/web_disk.h` | generated FAT12 image of `web/index.html` (do not hand-edit) |
| `web/index.html` | config page (Web Serial); source for `web_disk.h` |
| `tools/make_web_disk.py` | builds + self-validates the FAT12 disk image |
| `include/{switch_pro,report,config,remap,bootsel,usb,tusb_config}.h` | headers |
| `CMakeLists.txt` | extension header + dual-board; `-D SWITCH_PRO_MAX_CONTROLLERS=4`; links `hardware_flash` |

### Key constants / facts
- USB HID mode: `SWITCH_PRO_MAX_CONTROLLERS=4` (CMake `-D`) = 4 HID interfaces (IN 0x81-84 / OUT 0x01-04).
- Config-mode USB: VID `0xCAFE` / PID `0x4012`, CDC (serial) + MSC ("PICOSWITCH" drive).
- Settings flash sector: `PICO_FLASH_SIZE_BYTES - 4*FLASH_SECTOR_SIZE` (safely below btstack's bank).
- Config format v3: per-player `lightbar[4][3]` + per-family `button_map[FAMILY_COUNT][18]`; migrates v1/v2.
- bluepad32 submodule tracks `develop`; uses the SDK's bundled btstack (not external).

---

## Status — DONE (all hardware-validated unless noted)

- **Native Pro Controller** on Switch OG / Lite / **Switch 2**, with native remap UI.
- **Gyro/accel passthrough** — fixed drift (zeroed IMU calibration origin) + MissionControl
  DualSense→Switch axis transform (accel/gyro: x=−ds.z, y=−ds.x, z=+ds.y); scaled gyro/64, accel/2.
- **4 simultaneous controllers** (mixed DualSense + Xbox confirmed).
- **DualSense latency** — fixed by disabling Bluetooth sniff mode (it's BR/EDR Classic; no BLE input path).
- **Rumble** — console rumble forwarded via bluepad32 `play_dual_rumble` (throttled).
- **Pairing control** — bonds persist; allow-list enforced (locked by default, only known
  controllers reconnect); **double-tap BOOTSEL** = 10s pairing window; **triple-tap** = wipe
  (disconnect all + del_keys + clear allow-list); LED: slow flash idle / fast blink window /
  solid connected / fast burst on wipe / steady in config mode. BOOTSEL read is dual-core-safe.
- **Config mode** — hold BOOTSEL ~5s → re-enumerate as CDC + read-only MSC; the drive carries
  `index.html` so the dongle is fully self-contained (works offline, no external files).
- **Web config (Chrome/Edge, Web Serial):** per-player lightbar colours (RGB + preview) and
  **per-platform button remapping** with auto-selected profiles (Generic / Nintendo / PlayStation /
  Xbox, by BT vendor id) and platform-correct labels (e.g. PlayStation shows Cross/Mute/Create/PS).

---

## Status — REMAINING

- **8BitDo DIY Modkit won't connect.** Likely bluepad32's Class-of-Device filter
  (`uni_hid_device.c:347`) rejecting it *before* our callback runs — so it needs a bluepad32-
  internal CoD relaxation (e.g. accept any CoD while a pairing window is open) + hardware test.
  Offered as an experimental build; not yet done.
- **Analog-stick remapping** (swap L/R, invert axes) — not implemented (button remap only).
- **README / docs** — final pass (build, flashing, pairing gestures, config mode, Switch 2 notes).
- **Wake-from-sleep — SHELVED.** Switch 2 wakes only on a BLE advertisement from a *BLE-bonded*
  controller (per alexvnesta/switch2controller); our dongle is USB, so it can't. Inert USB
  remote-wakeup code remains in `usb.c` (may work on the *original* Switch — untested).

### Known limitations
- Controller-specific extra buttons (Xbox Elite paddles, DualSense Edge back buttons, Steam
  grips) are **not exposed by bluepad32** (fixed virtual-gamepad button set) → not mappable
  without custom per-controller report parsing.
- Config changes (colours/maps) take effect on the controller's next reconnect.
- All 4 USB Pro Controller interfaces are always present; with fewer pads paired the spare
  interfaces are idle "ghost" players (inherent to static USB interfaces).
