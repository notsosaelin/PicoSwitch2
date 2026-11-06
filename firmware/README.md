# Firmware

The PicoSwitch2 adapter firmware for the Raspberry Pi Pico W (RP2040) and
Pico 2 W (RP2350). It hosts Bluetooth controllers on the CYW43 radio (core 1)
and presents them to a Nintendo Switch 2 over USB as a genuine-looking
controller personality (core 0).

## Build

```powershell
./build.ps1              # both boards -> build/<board>/PicoSwitchWGA-<board>.uf2
./build.ps1 pico2_w      # one board
./build.ps1 -Clean       # wipe the build directories first
```

`build.ps1` uses the pinned toolchain installed by the Raspberry Pi Pico VS Code
extension under `~/.pico-sdk` (SDK 2.3.0, Arm GCC 15.2.Rel1, CMake 4.3.4,
Ninja 1.13.2, picotool 2.3.0). Its header lists every diagnostic and audio
variant. The Opus codec is a git submodule: run
`git submodule update --init` once after cloning.

## Layout

```text
firmware/
├── CMakeLists.txt         target, options, include paths (PICOSWITCH_MODULE_DIRS)
├── build.ps1              pinned-toolchain build for both boards
├── config/                tusb_config.h, btstack_config.h (on the global include path)
├── cmake/                 the BTstack hid_host.c substitution (long-report support)
├── third_party/opus/      Opus codec, git submodule (DualSense speaker audio)
└── src/                   firmware modules, one directory each
```

Every `.c` under `src/` is compiled (`GLOB_RECURSE`). Each module keeps its
sources and headers side by side, and headers are included by basename, so
moving a file between modules needs no `#include` edits, only the module list
in `CMakeLists.txt` and `tests/run_host_tests.ps1` when a module is added.

| Module | Responsibility |
| --- | --- |
| `app/` | `main`, the USB device stack and descriptors, the report loop, USB personality cycling, BOOTSEL gesture handling, owner LED |
| `bluetooth/` | Core-1 Bluetooth host lifecycle: pairing crypto, reconnect, wake, health and recovery |
| `hid_host/` | Vendored joypad-os Bluetooth HID host and per-vendor controller drivers (see [its README](src/hid_host/README.md)) |
| `input/` | The seam from source controllers into the shared `switch_pro_input_t` state: arbitration, remapping, keyboard/mouse, battery |
| `motion/` | Motion pipeline: genuine Pro Controller 2 passthrough, DualSense translation, motion PDU codecs, probes (see [its README](src/motion/README.md)) |
| `audio/` | DualSense speaker audio bridge, resampling, haptics, Pro Controller 2 audio transport |
| `nfc/` | Virtual amiibo storage and serving, NFC command handling, genuine tag passthrough |
| `console/pro2/` | Pro Controller 2 personality: vendor protocol RX/TX, report encoding |
| `console/pro/` | Shared Switch controller report definitions (`switch_pro_input_t`) |
| `console/gamecube/` | GameCube controller personality |
| `console/joycon2/` | Joy-Con 2 personality |
| `console/common/` | State shared by every personality: firmware identity profile, player LEDs |
| `settings/` | Persistent configuration and its flash storage |
| `management/` | The companion-app management channel: access control, bonds, peers, pairing |
| `diagnostics/` | UART diagnostics, protocol and rumble traces, capture buffers |

## Tests

Pure firmware logic is compiled and run on the host by
`tests/run_host_tests.ps1` (see [`tests/`](../tests/README.md)). Behavior that
depends on the console or a controller is validated on hardware, and the
evidence is recorded under [`docs/`](../docs/README.md).
