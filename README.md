# PicoSwitch2

Turn a **Raspberry Pi Pico W** or **Pico 2 W** into a wireless adapter that presents up
to **four Bluetooth controllers** to a Nintendo **Switch**, **Switch Lite**, or
**Switch 2** as **native Pro Controllers** (USB VID `057E` / PID `2009`).

Because the console sees genuine Pro Controllers, everything that works with a real Pro
Controller works here — the **native in-system remap UI**, **gyro/motion aiming**, and
**rumble** — with no console-side setup.

> Forked from [juan518munoz/PicoSwitch-WirelessGamepadAdapter](https://github.com/juan518munoz/PicoSwitch-WirelessGamepadAdapter).
> The Pro Controller protocol is ported from the MIT-licensed
> [bmelanman/retro-pico-switch](https://github.com/bmelanman/retro-pico-switch).
> Bluetooth is handled by [Bluepad32](https://github.com/ricardoquesada/bluepad32);
> USB by [TinyUSB](https://github.com/hathach/tinyusb).

## Features

- **Native Pro Controller** on Switch OG / Lite / **Switch 2** — recognized by the
  system, usable in the built-in **Change Grip/Order** and **controller remap** screens.
- **Up to 4 controllers at once**, mixed types supported (e.g. DualSense + Xbox together).
- **Gyro / accelerometer passthrough** for motion aiming.
- **Rumble** — console rumble is forwarded to the controller.
- **Pairing control** — bonds persist across power cycles, and an **allow-list is
  enforced by default**, so only controllers you've explicitly paired will reconnect.
  Open a pairing window or wipe everything with [BOOTSEL gestures](#pairing--led-guide).
- **Self-contained config mode** — hold BOOTSEL ~5 s and the dongle re-enumerates as a
  serial + storage device that **serves its own configuration web page** (no app or
  internet needed). See [Configuration](#configuration).
- **Per-player lightbar colors** and **per-platform button remapping** via the web page.
- **Builds for both boards** (RP2040 Pico W and RP2350 Pico 2 W) from one source tree.

[Full list of supported controllers →](https://bluepad32.readthedocs.io/en/latest/supported_gamepads/)

## Hardware

- A **Raspberry Pi Pico W** (RP2040) **or Pico 2 W** (RP2350) — the wireless models;
  the non-W Picos have no Bluetooth and won't work.
- A USB cable to connect the Pico to the Switch dock / Switch 2, or to a PC for flashing.

## Install

1. Download the latest `.uf2` for your board from
   [Releases](https://github.com/notsosaelin/PicoSwitch2/releases):
   - `PicoSwitchWGA-pico_w.uf2` for the **Pico W**
   - `PicoSwitchWGA-pico2_w.uf2` for the **Pico 2 W**
2. Hold the **BOOTSEL** button while plugging the Pico into a PC. It mounts as a drive
   named `RPI-RP2` (or `RP2350`).
3. Drag the `.uf2` onto that drive. The Pico reboots running PicoSwitch2.
4. Plug the Pico into your Switch dock (or the Switch 2 / a powered USB hub). The LED
   gives a brief flash every ~2 s to show it's running and waiting for a controller.

## Usage

### First-time pairing

1. **Double-tap BOOTSEL** to open a **10-second pairing window** (LED blinks fast).
2. Put your controller into pairing mode (e.g. DualSense: hold **PS + Create** until the
   light pulses).
3. When it connects, the LED goes **solid**. The controller is now bonded and on the
   allow-list, so it will reconnect automatically from now on — no need to re-pair.

Repeat for up to four controllers. Player order follows connection order.

### Pairing & LED guide

| BOOTSEL gesture | Action |
|---|---|
| **Double-tap** | Open a 10 s pairing window (admits any controller, then re-locks) |
| **Triple-tap** | Wipe all saved controllers and clear the allow-list |
| **Hold ~5 s**  | Enter [config mode](#configuration) |

| LED pattern | Meaning |
|---|---|
| Brief flash every ~2 s | Idle / locked — running, waiting for a known controller |
| Fast blink | Pairing window open |
| Solid on | At least one controller connected |
| Very fast flicker (~1 s burst) | Wiping saved controllers |
| Steady ~1 s blink | Config mode active |

## Configuration

Hold **BOOTSEL for ~5 seconds** to enter **config mode**. The dongle re-enumerates as a
USB serial + read-only storage device (VID `CAFE` / PID `4012`) named **PICOSWITCH**.

1. Open the **PICOSWITCH** drive and launch `index.html` in your browser. It uses the
   **Web Serial API**, so use **Chrome or Edge** (desktop).
2. Click **Connect** and select the PicoSwitch serial port.
3. Configure:
   - **Lightbar colors** — per-player RGB with live preview (for controllers with a
     lightbar, like DualSense / DualShock 4).
   - **Button remapping** — per-platform profiles (Generic / Nintendo / PlayStation /
     Xbox) are auto-selected by the controller's Bluetooth vendor ID, with
     platform-correct labels (e.g. PlayStation shows Cross / Mute / Create / PS).
4. Save. Unplug and replug (or power-cycle) to return to normal Pro Controller mode.

> Configuration changes take effect on the controller's **next reconnect**.

## Building

PicoSwitch2 builds on **Windows** using the toolchain bundled with the official
**Raspberry Pi Pico VS Code extension** — no global SDK install required. Installing that
extension lays down everything needed under `%USERPROFILE%\.pico-sdk`
(SDK 2.2.0, ARM GCC 14_2_Rel1, CMake 3.31.5, Ninja 1.12.1, picotool).

```powershell
git clone --recursive https://github.com/notsosaelin/PicoSwitch2.git
cd PicoSwitch2

./build.ps1            # build both boards
./build.ps1 pico_w     # build only the Pico W  (RP2040)
./build.ps1 pico2_w    # build only the Pico 2 W (RP2350)
./build.ps1 -Clean     # wipe build dirs first, then build
```

Artifacts land in `build/pico_w/PicoSwitchWGA-pico_w.uf2` and
`build/pico2_w/PicoSwitchWGA-pico2_w.uf2`.

This is a standard pico-sdk CMake project, so it can also be built with your own SDK by
selecting the board explicitly, e.g. `cmake -B build/pico_w -DPICO_BOARD=pico_w` (the
firmware presents 4 USB HID interfaces; this is set in `CMakeLists.txt` via
`-D SWITCH_PRO_MAX_CONTROLLERS=4`).

### Editing the config web page

The configuration page is embedded in the firmware as a FAT12 image. After editing
`web/index.html`, regenerate the embedded copy before building:

```powershell
python tools/make_web_disk.py web/index.html src/web_disk.h
```

## Architecture

| Core | Responsibility |
|---|---|
| **core0** | TinyUSB device — Pro Controller HID interfaces (or CDC + storage in config mode) |
| **core1** | Bluepad32 + BTstack — Bluetooth host, LED, BOOTSEL gestures, pairing, rumble/lightbar, settings flash |

The two cores share per-controller input and a rumble channel through `src/report.c`
under a hardware critical section. A deep-dive source map, key constants, and design
notes live in [`plan.md`](plan.md).

## Known limitations & roadmap

- **8BitDo DIY Modkit** controllers don't connect yet (likely Bluepad32's
  Class-of-Device filter rejecting them before our code runs). Experimental; not done.
- **Analog-stick remapping** (swap L/R sticks, invert axes) is **not** implemented —
  button remapping only.
- Controller-specific **extra buttons** (Xbox Elite paddles, DualSense Edge back buttons,
  Steam grips) aren't exposed by Bluepad32's fixed virtual-gamepad button set, so they
  can't be mapped.
- All four USB Pro Controller interfaces are always present; with fewer pads paired, the
  spare interfaces appear as idle "ghost" players. This only occurs on non Nintendo devices such as PC.
- **Wake-from-sleep is not supported** on Switch 2 — it wakes only on a BLE advertisement
  from a BLE-bonded controller, and this dongle is a USB device.

## Acknowledgements

- [juan518munoz](https://github.com/juan518munoz) — the original
  [PicoSwitch-WirelessGamepadAdapter](https://github.com/juan518munoz/PicoSwitch-WirelessGamepadAdapter) this is forked from.
- [ricardoquesada](https://github.com/ricardoquesada) — [Bluepad32](https://github.com/ricardoquesada/bluepad32).
- [hathach](https://github.com/hathach) — [TinyUSB](https://github.com/hathach/tinyusb).
- [bmelanman](https://github.com/bmelanman) — [retro-pico-switch](https://github.com/bmelanman/retro-pico-switch),
  source of the Pro Controller protocol implementation.
- [DavidPagels](https://github.com/DavidPagels/retro-pico-switch) and
  [splork](https://github.com/aveao/splork) — HID descriptors and TinyUSB usage examples.

## License

[Apache License 2.0](LICENSE).
