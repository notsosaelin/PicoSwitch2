# PicoSwitch2

Turn a **Raspberry Pi Pico W** or **Pico 2 W** into a wireless adapter that pairs a
**Bluetooth controller** and presents it to a **Nintendo Switch 2** as a **native Switch 2
Pro Controller** (USB VID `057E` / PID `2069`).

Because the console sees a genuine Pro Controller 2, it works with **no console-side setup** —
all buttons (including the Switch 2's **C, GL, and GR** grip buttons), sticks, and **rumble**.
A DualSense, DualSense Edge, Xbox, Xbox Elite Series 2, Switch, 8BitDo, and more are all
supported, with their **extra buttons** (Edge paddles/Fn, Elite paddles) mapped through.

> Forked from [juan518munoz/PicoSwitch-WirelessGamepadAdapter](https://github.com/juan518munoz/PicoSwitch-WirelessGamepadAdapter).
> The Switch 2 protocol is original work informed by
> [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research).
> Bluetooth is handled by a vendored [joypad-os](https://github.com/joypad-ai/joypad-os) bthid stack
> on [BTstack](https://github.com/bluekitchen/btstack); USB by [TinyUSB](https://github.com/hathach/tinyusb).

## Features

- **Native Switch 2 Pro Controller** — recognized by the system with no setup; all buttons incl.
  **C / GL / GR**, sticks, D-pad, and **rumble**.
- **Extended buttons mapped** — DualSense Edge back paddles → GL/GR and Fn → Capture/C; Xbox Elite
  Series 2 paddles → GL/GR. These are surfaced by dedicated driver parsing (a real gain over generic stacks).
- **250 Hz USB poll**, matching the genuine Pro Controller 2.
- **Self-contained config mode** — hold BOOTSEL ~5 s and the dongle re-enumerates as a serial +
  storage device that **serves its own configuration web page** (no app or internet needed).
- **Live config UI** — see the connected controller auto-detected, watch each input light up next to
  the Switch output it produces, and **remap any button per controller** — plus a lightbar colour picker.
- **Works on PC too** — enumerates as a Switch 2 Pro (Steam etc.), including **gyro** over the common report.
- **Builds for both boards** (RP2040 Pico W and RP2350 Pico 2 W) from one source tree.

> **Scope:** one controller at a time (multi-player is on the roadmap). **Console gyro** is not yet
> supported — its native motion-report format is undocumented and under investigation; **PC gyro works**.
> A Switch 1 Pro Controller target is still available via `-DNS2_PRO=OFF`.

## Hardware

- A **Raspberry Pi Pico W** (RP2040) **or Pico 2 W** (RP2350) — the wireless models; the non-W Picos
  have no Bluetooth and won't work.
- A USB cable to connect the Pico to the Switch 2 (or a powered USB hub), or to a PC for flashing.

## Install

1. Download the latest `.uf2` for your board from
   [Releases](https://github.com/notsosaelin/PicoSwitch2/releases):
   - `PicoSwitchWGA-pico_w.uf2` for the **Pico W**
   - `PicoSwitchWGA-pico2_w.uf2` for the **Pico 2 W**
2. Hold **BOOTSEL** while plugging the Pico into a PC. It mounts as a drive (`RPI-RP2` / `RP2350`).
3. Drag the `.uf2` onto that drive. The Pico reboots running PicoSwitch2.
4. Plug the Pico into your Switch 2 (or a powered USB hub). The LED flashes briefly every ~2 s while
   it waits for a controller.

## Usage

### First-time pairing

1. **Double-tap BOOTSEL** to open a **10-second pairing window** (LED blinks fast).
2. Put your controller into pairing mode (e.g. DualSense: hold **PS + Create** until the light pulses).
3. When it connects, the LED goes **solid**. The controller is bonded and reconnects automatically.

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
| Solid on | A controller is connected |
| Very fast flicker (~1 s burst) | Wiping saved controllers |
| Steady ~1 s blink | Config mode active |

## Configuration

Hold **BOOTSEL for ~5 seconds** to enter **config mode**. The dongle re-enumerates as a USB serial +
read-only storage device (VID `CAFE` / PID `4012`) named **PICOSWITCH**. Pair a controller *before*
entering config mode — it stays connected and streams live.

1. Open the **PICOSWITCH** drive and launch `CONFIG.HTM`. It uses the **Web Serial API**, so use
   **Chrome or Edge** (desktop).
2. Click **Connect** and select the PicoSwitch serial port.
3. You can:
   - **See the connected controller** auto-detected (name + type), with a live table of each input and
     the Switch 2 output it produces.
   - **Remap** any button per controller family via the dropdowns.
   - **Set the lightbar colour** for DualSense / DualShock pads (applies live).
4. **Save**. Unplug/replug (or power-cycle) to return to normal Pro Controller mode.

> Remaps and colours take effect immediately (colours) or on the controller's next reconnect (maps).

## Building

PicoSwitch2 builds on **Windows** using the toolchain bundled with the official **Raspberry Pi Pico
VS Code extension**, so there are no global SDK installs to manage.

### Prerequisites

1. **VS Code** with the
   **[Raspberry Pi Pico](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico)**
   extension. On first use it downloads a private toolchain to `%USERPROFILE%\.pico-sdk`
   (Pico SDK, ARM GCC, CMake, Ninja, picotool) — everything `build.ps1` needs.
2. **Git**, to clone the repo.
3. **Python 3** — only if you edit the config web page (see below).

`build.ps1` expects these toolchain versions under `%USERPROFILE%\.pico-sdk`:

| Component | Version |
|---|---|
| Pico SDK | 2.2.0 |
| ARM GCC  | 14_2_Rel1 |
| CMake    | 3.31.5 |
| Ninja    | 1.12.1 |
| picotool | 2.2.0-a4 |

If the extension installed different versions, update the matching variables at the top of `build.ps1`.

### Build

```powershell
git clone https://github.com/notsosaelin/PicoSwitch2.git
cd PicoSwitch2

./build.ps1            # build both boards
./build.ps1 pico_w     # only the Pico W  (RP2040)
./build.ps1 pico2_w    # only the Pico 2 W (RP2350)
./build.ps1 -Clean     # wipe build dirs first, then build
```

Build artifacts:

- `build/pico_w/PicoSwitchWGA-pico_w.uf2`
- `build/pico2_w/PicoSwitchWGA-pico2_w.uf2`

> **Not on Windows, or prefer your own SDK?** Standard pico-sdk CMake project. With `PICO_SDK_PATH`
> set: `cmake -B build/pico2_w -G Ninja -DPICO_BOARD=pico2_w && cmake --build build/pico2_w`.

### Editing the config web page

The config page is embedded as a FAT12 image (`src/web_disk.h`). After editing `web/index.html`,
regenerate that header before building:

```powershell
python tools/make_web_disk.py web/index.html src/web_disk.h
```

## Architecture

| Core | Responsibility |
|---|---|
| **core0** | TinyUSB device — the Switch 2 Pro Controller (or CDC + storage in config mode) |
| **core1** | joypad-os bthid + BTstack — Bluetooth host, LED, BOOTSEL gestures, pairing, rumble/lightbar, settings flash |

The cores share per-controller input and a rumble channel through `src/report.c` under a hardware
critical section. `src/bt_hid/ns2_seam.c` maps each controller's inputs to the Switch 2 wire format;
`src/switch_pro2/` implements the USB protocol. See [`PLAN.md`](PLAN.md) and [`STATUS.md`](STATUS.md).

## Known limitations & roadmap

- **One controller at a time.** Multi-player is on the roadmap (see [`PLAN.md`](PLAN.md)).
- **Console gyro not yet supported** — the native motion-report format is undocumented and under
  reverse engineering. PC gyro (over the common report) works.
- **Bluetooth reconnect can be flaky** for the Switch 2 Pro Controller (sometimes needs a re-pair).
- **Wake-from-sleep is not supported** — the Switch 2 wakes only on a BLE advertisement from a bonded
  controller, and this dongle is a USB device.

## Acknowledgements

- [juan518munoz](https://github.com/juan518munoz) — the original
  [PicoSwitch-WirelessGamepadAdapter](https://github.com/juan518munoz/PicoSwitch-WirelessGamepadAdapter).
- [ndeadly](https://github.com/ndeadly) —
  [switch2_controller_research](https://github.com/ndeadly/switch2_controller_research) and MissionControl.
- [joypad-os](https://github.com/joypad-ai/joypad-os) — the vendored Bluetooth HID host stack + drivers.
- [hathach](https://github.com/hathach) — [TinyUSB](https://github.com/hathach/tinyusb).
- [bmelanman](https://github.com/bmelanman) — [retro-pico-switch](https://github.com/bmelanman/retro-pico-switch),
  source of the original Pro Controller protocol implementation.

## License

[Apache License 2.0](LICENSE).
