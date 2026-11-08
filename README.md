# PicoSwitch2

[![CI](https://github.com/notsosaelin/PicoSwitch2/actions/workflows/ci.yml/badge.svg)](https://github.com/notsosaelin/PicoSwitch2/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/notsosaelin/PicoSwitch2)](https://github.com/notsosaelin/PicoSwitch2/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

PicoSwitch2 turns a Raspberry Pi Pico W or Pico 2 W into a Bluetooth-to-USB
controller adapter for the Nintendo Switch 2. Pair almost any Bluetooth
controller (or a keyboard and mouse, or an Android or Windows device) and the
console sees a native Switch 2 controller, with motion, rumble, battery, NFC
and audio where the source supports them.

It is also an open technical reference: the protocol findings behind each
feature are documented with their evidence under [`docs/`](docs/README.md).

## Features

- **Five USB personalities**, switched at runtime:

  | Personality | USB ID |
  | --- | --- |
  | Switch 2 Pro Controller | `057E:2069` |
  | NSO GameCube Controller | `057E:2073` |
  | Joy-Con 2 (L) / (R), experimental | `057E:2067` / `057E:2066` |
  | Configuration device | `CAFE:4012` |

- **Bluetooth input** from DualSense and DualSense Edge (paddles, Fn, mute,
  lightbar), Xbox, Switch 1 and Switch 2, Wii, 8BitDo, Retro Fighters and other
  HID controllers, plus Bluetooth keyboards and mice.
- **Motion**: genuine Pro Controller 2 passthrough, and calibrated DualSense
  translation.
- **Rumble and haptics**, including native DualSense haptics and GameCube rumble.
- **Audio** (Pico 2 W): console audio through a DualSense speaker or headset jack.
- **Virtual Amiibo**, including the 2048-byte Kirby Air Riders figures.
- **Companion apps** for [Android](companions/android/) and
  [Windows](companions/windows/): manage the adapter wirelessly, use the device
  itself as a controller (Controller Link), and an editable on-screen gamepad.
  A [Linux](companions/linux/) companion (one executable for x64 and arm64,
  handhelds in Game Mode first) is implemented and awaiting hardware validation.

What has been validated on real hardware, and what has not, is tracked in
[`STATUS.md`](STATUS.md) and the
[compatibility matrix](docs/status/compatibility-matrix.md).

## Hardware

- A Raspberry Pi **Pico W** (RP2040) or **Pico 2 W** (RP2350). Boards without
  wireless cannot host Bluetooth. Audio needs a Pico 2 W.
- A data-capable USB cable.

## Install

1. Download the UF2 for your board from
   [Releases](https://github.com/notsosaelin/PicoSwitch2/releases):
   `PicoSwitchWGA-pico_w.uf2` or `PicoSwitchWGA-pico2_w.uf2`.
2. Hold BOOTSEL while plugging the Pico into a computer.
3. Copy the UF2 to the `RPI-RP2` or `RP2350` drive that appears.
4. Plug the Pico into the Switch 2.

Updating is the same steps. The standard UF2 keeps your pairings, settings
and amiibo. To start from a clean adapter instead, flash the `-wipe.uf2` for
your board: its first boot erases all of them (as the BOOTSEL triple-tap
does).

Firmware and companion apps must come from the same release.

## Use

The Pico's BOOTSEL button controls pairing and personalities:

| Gesture | No controller paired | Controller paired | Config mode |
| --- | --- | --- | --- |
| Tap | — | Next personality: Pro 2 → GameCube → Joy-Con 2 L → Joy-Con 2 R | — |
| Double-tap | Open a 30 s pairing window | Disconnect (keeping the pairing), then open a pairing window | — |
| Triple-tap | Erase all pairings | Disconnect and erase all pairings | Same |
| Hold 2 s | Enter Config mode | Enter Config mode | Return to Pro Controller 2 |

Every cold boot starts in Pro Controller 2 mode.

| LED | Meaning |
| --- | --- |
| Brief flash every ~2 s | Idle, only previously paired controllers may connect |
| Fast blink | Pairing window open |
| Solid | Controller connected |
| Very fast burst | Erasing pairings |
| Slow blink | Config mode |

### Configuration

Settings (controller colors, input source, amiibo library and more) are managed
from the web portal or a companion app. To use the portal, hold BOOTSEL for two
seconds, then from the repository:

```powershell
./tools/run_config_portal.ps1
```

and select the **PicoSwitch Config** serial port in desktop Chrome or Edge. The
portal is served locally; nothing is hosted on the microcontroller. It also
reaches the adapter over Bluetooth without leaving controller mode.

## Build from source

```powershell
git clone --recurse-submodules https://github.com/notsosaelin/PicoSwitch2.git
cd PicoSwitch2
./firmware/build.ps1            # both boards
./firmware/build.ps1 pico2_w    # one board
```

Output: `firmware/build/<board>/PicoSwitchWGA-<board>.uf2`.

`build.ps1` uses the toolchain installed by the Raspberry Pi Pico VS Code
extension (Pico SDK 2.3.0, Arm GCC 15.2.Rel1, CMake 4.3.4, Ninja 1.13.2,
picotool 2.3.0). `firmware/` is also a standard Pico SDK CMake project for any
environment with `PICO_SDK_PATH` set. See [`firmware/README.md`](firmware/README.md)
for the module layout and build variants, and [`tests/README.md`](tests/README.md)
for the test suites.

## Repository layout

```text
firmware/      adapter firmware (C, Pico SDK): src/ modules, config, build
companions/    android/ (Kotlin), windows/ (C#, WinUI 3) and linux/ (C#, Avalonia) apps
contracts/     wire contracts shared by firmware, companions and tests
web/           browser configuration portal
tests/         host tests, tool tests, fixtures and runners
tools/         UART, capture, lab and analysis tooling
docs/          architecture, protocol reference, experiments, archive
```

## Known limitations

- One Bluetooth controller drives the console at a time.
- Joy-Con 2 L and R are separate sideways controllers, not a combined pair.
- The Switch 2's length-`0x28` motion form is only partly decoded and is never
  synthesized for translated sources; the validated length-`0x1E` form is used.
- DualSense audio is Pico 2 W only, and its microphone is not passed through.
- The first-generation 8BitDo Ultimate works on its stock firmware, but its
  P1/P2 back paddles are not available.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). Protocol claims need evidence; the
standards are in [`docs/re-methodology/evidence-standards.md`](docs/re-methodology/evidence-standards.md).
Report security issues as described in [`SECURITY.md`](SECURITY.md).

## Acknowledgements

- [juan518munoz](https://github.com/juan518munoz): the original
  [PicoSwitch wireless adapter](https://github.com/juan518munoz/PicoSwitch-WirelessGamepadAdapter).
- [ndeadly](https://github.com/ndeadly): [Switch 2 controller research](https://github.com/ndeadly/switch2_controller_research) and captures.
- [joypad-os](https://github.com/joypad-ai/joypad-os): the vendored Bluetooth HID host stack and drivers.
- [BTstack](https://github.com/bluekitchen/btstack) and [TinyUSB](https://github.com/hathach/tinyusb) ([hathach](https://github.com/hathach)).
- [Vicki Pfau](https://github.com/endrift): Linux HID Nintendo work, one corroborating source for GameCube rumble framing.
- [bmelanman](https://github.com/bmelanman): the retro-pico-switch protocol implementation.

## License

[Apache License 2.0](LICENSE). Nintendo Switch is a trademark of Nintendo; this
project is not affiliated with or endorsed by Nintendo.
