# PicoSwitch2

PicoSwitch2 turns a Raspberry Pi Pico W or Pico 2 W into a Bluetooth-to-USB
controller adapter for Nintendo Switch 2. It can present five USB personalities:

1. Switch 2 Pro Controller (`057E:2069`)
2. Nintendo GameCube Controller (`057E:2073`)
3. Nintendo Joy-Con 2 (L) (`057E:2067`)
4. Nintendo Joy-Con 2 (R) (`057E:2066`)
5. Local configuration device (`CAFE:4012`)

The project is based on
[PicoSwitch-WirelessGamepadAdapter](https://github.com/juan518munoz/PicoSwitch-WirelessGamepadAdapter),
with a vendored [joypad-os](https://github.com/joypad-ai/joypad-os) Bluetooth HID stack,
[BTstack](https://github.com/bluekitchen/btstack), and
[TinyUSB](https://github.com/hathach/tinyusb). Switch 2 protocol work is informed by this
project's hardware captures and
[ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research).

## Current capabilities

- Switch 2 Pro Controller output with buttons, sticks, C/GL/GR, and rumble.
- Native NSO GameCube output with buttons, sticks, analog triggers, digital trigger
  detents, and hardware-validated rumble.
- Experimental Joy-Con 2 Left and Right output personalities.
- Native Joy-Con 2 mouse mode from a paired Bluetooth HID mouse, including pointer movement,
  buttons, and wheel-to-stick menu navigation.
- DualSense and DualSense Edge input, including Edge paddles, Fn and mute buttons,
  LEDs, and rumble.
- Live Windows-audio output through a paired DualSense internal speaker on
  Pico 2 W. Its standard artifact uses the hardware-confirmed
  floating-point/SRAM codec path at 300 MHz/1.20 V. Pico W remains on its
  validated non-audio configuration.
- Xbox-family, Switch-family, 8BitDo, and other supported Bluetooth HID controllers.
- A local configuration page for remapping buttons and independently setting the Pro Controller 2
  body and Joy-Con 2 Left/Right accent colors. The active personality's appearance is shared with
  supported DualShock/DualSense lightbars.
- Builds for both RP2040 Pico W and RP2350 Pico 2 W.

The Pro Controller 2 personality deliberately uses a 1 ms USB interrupt interval
(1000 Hz); the genuine controller uses 4 ms (250 Hz). The GameCube and Joy-Con 2
personalities retain their genuine descriptor timing.

See [STATUS.md](STATUS.md) for hardware-confirmed behavior and known limitations, and
[docs/README.md](docs/README.md) for the documentation map. A fresh coding-agent session should
start with [AGENTS.md](AGENTS.md).

## Hardware

- Raspberry Pi Pico W or Pico 2 W. Non-wireless Pico boards cannot host Bluetooth.
- A data-capable USB cable, optionally through a powered USB hub.

## Install

1. Download the UF2 for your board from
   [GitHub Releases](https://github.com/notsosaelin/PicoSwitch2/releases):
   `PicoSwitchWGA-pico_w.uf2` or `PicoSwitchWGA-pico2_w.uf2`.
2. Hold BOOTSEL while plugging the Pico into a PC.
3. Copy the UF2 to the mounted `RPI-RP2` or `RP2350` drive.
4. Connect the Pico to the Switch 2 or PC.

## Pairing and BOOTSEL

| Gesture | No controller paired | Controller paired | Config mode |
|---|---|---|---|
| Single-tap | No action | Cycle Pro2 → GameCube → Joy-Con 2 Left → Joy-Con 2 Right → Pro2 | No action |
| Double-tap | Open a 30-second pairing window | Disconnect the active controller without deleting its bond, then open a 30-second pairing window | No action |
| Triple-tap | Erase stored pairings and lock admission | Disconnect, erase stored pairings, and lock admission | Same wipe/disconnect action |
| Hold for 2 seconds | Enter Config directly | Enter Config directly | Exit directly to Pro Controller 2 |

Personality selection is volatile and every cold boot begins in Pro Controller 2 mode. Config is
deliberately excluded from the single-tap controller cycle.

| LED pattern | Meaning |
|---|---|
| Brief flash about every 2 seconds | Idle and locked to previously admitted controllers. |
| Fast blink | Pairing window open. |
| Solid | Controller connected. |
| Very fast burst | Pairing data is being cleared. |
| Slow steady blink | Config mode active. |

BOOTSEL processing is serviced at Bluetooth HID report boundaries as well as during idle
polling. This is required so a high-rate controller such as DualSense cannot starve button
gestures or output scheduling.

## Configuration

Hold BOOTSEL for two seconds from any controller personality. The device re-enumerates directly as
the **PicoSwitch Config** serial device; firmware no longer exposes a storage drive or embeds the
web portal.

1. Start the local portal from the repository root:

```powershell
.\tools\run_config_portal.ps1
```

2. In desktop Chrome or Edge, select the PicoSwitch Config serial port.
3. Inspect live input, change mappings or Pro2/Joy-Con appearance colors, and save.
4. Hold BOOTSEL for two seconds to return directly to Pro Controller 2, or power-cycle to boot
   directly into Pro Controller 2 mode.

The launcher serves `web/index.html` from localhost, giving browser storage a stable origin for the
amiibo library while keeping the changing portal entirely off the microcontroller.

The amiibo library remains available when no serial device is connected. It retains separate
Unused and console-written Used copies, can select either copy, and can export/import a versioned
library backup in case browser storage is cleared. Adapter-only controls remain disabled offline.

The separate Virtual Amiibo browser diagnostic does not require a Pico or serial port:

```powershell
.\tools\run_amiibo_portal_test.ps1
```

It serves `web/diagnostic.html` locally and simulates upload, persistence, console writes,
cache-first save-back, the recursive browser library, and AmiiboAPI metadata.

## Building

On Windows, `build.ps1` uses the toolchain installed by the official Raspberry Pi Pico
VS Code extension. Its expected versions are Pico SDK 2.2.0, ARM GCC 14_2_Rel1,
CMake 3.31.5, Ninja 1.12.1, and picotool 2.2.0-a4.

```powershell
./build.ps1            # both boards
./build.ps1 pico_w     # RP2040 Pico W, validated non-audio build
./build.ps1 pico2_w    # RP2350 Pico 2 W, live audio at 300 MHz
./build.ps1 -Clean     # clean, then build both
./build.ps1 -Tone      # Pico 2 W fixed DualSense diagnostic tone
./build.ps1 -Audio     # Pico 2 W live-audio 150 MHz comparison
./build.ps1 -AudioOverclock # Pico 2 W live-audio 200 MHz comparison
```

Artifacts are written to:

- `build/pico_w/PicoSwitchWGA-pico_w.uf2`
- `build/pico2_w/PicoSwitchWGA-pico2_w.uf2`

This is also a standard Pico SDK CMake project for environments with `PICO_SDK_PATH` set.

## Architecture

Core 0 owns TinyUSB and the active USB personality. Core 1 owns Bluetooth, controller
drivers, pairing, LEDs, BOOTSEL sampling, and controller output such as rumble. Shared input
and output state crosses the cores through `src/report.c` under a hardware critical section.

The architectural overview is in [docs/architecture/overview.md](docs/architecture/overview.md).
Protocol facts and unresolved research are deliberately kept in `docs/`, while source comments
retain only wire layouts, invariants, and implementation constraints.

## Known limitations

- One Bluetooth controller is routed at a time.
- Genuine Pro Controller 2 native motion passthrough and calibrated DualSense/DualSense Edge
  translation to the Switch 2 length-`0x1E` carrier are hardware-confirmed. The genuine
  length-`0x28` form remains only partially decoded and is never synthesized for Sony controllers.
- Joy-Con 2 Left and Right are separate sideways controller personalities; they are not combined
  into one paired-controller USB identity.
- The first-generation 8BitDo Ultimate Bluetooth controller requires the guarded custom controller
  firmware under `8Bitdo/` to expose P1/P2 independently.
- Switch 2 wake-from-sleep is implemented in Pro Controller 2 mode. After the console has entered
  sleep, the first real controller input sends the wake request automatically. Neutral reconnect
  reports never trigger wake.
- Live DualSense speaker output is Pico 2 W-only. A saved DualSense bond is
  sufficient: every reconnect resets the audio session and resends the
  controller's audio-enable transaction, so a fresh pair is not required. The
  bonded HID Host path extends its report length only for the two exact
  oversized Sony audio shapes; controller and dongle power-cycle reconnects are
  hardware-confirmed.
  The Pico W fixed-point/XIP experiment barely produced audio on hardware and
  was removed from its standard build. On Pico 2 W, the physical DualSense jack
  conditionally drives Switch 2 headset presence so a bare controller does not
  capture console audio. Real-console insertion, headphone output, repeated
  unplug/replug recovery, native haptic rumble, and an eight-hour stability soak are confirmed.
  The DualSense microphone return remains open.

See [PLAN.md](PLAN.md) for the prioritized roadmap and
[docs/status/compatibility-matrix.md](docs/status/compatibility-matrix.md) for the test matrix.

## Acknowledgements

- [juan518munoz](https://github.com/juan518munoz) — original PicoSwitch wireless adapter.
- [ndeadly](https://github.com/ndeadly) — Switch 2 controller research and captures.
- [Vicki Pfau](https://github.com/endrift) — Linux HID Nintendo protocol work used as one
  corroborating source for GameCube rumble framing.
- [joypad-os](https://github.com/joypad-ai/joypad-os) — Bluetooth HID host stack and drivers.
- [hathach](https://github.com/hathach) — TinyUSB.
- [bmelanman](https://github.com/bmelanman) — retro-pico-switch protocol implementation.

## License

[Apache License 2.0](LICENSE)
