# `src/bt_hid/` — Bluetooth HID stack (vendored joypad-os)

The native BTstack HID host that drives all Bluetooth controllers — the **sole BT stack**
(bluepad32 fully retired). Per-vendor HID drivers expose every button (Switch 2 GL/GR/C, DualSense
Edge paddles/Fn, Xbox Elite paddles) plus motion and per-vendor rumble. Controller catalog:
[`docs/bluetooth/controller-research.md`](../../docs/bluetooth/controller-research.md). The
migration that produced this (shipped 2026-07-04) is archived at
[`docs/archive/bt-stack-migration-2026-07.md`](../../docs/archive/bt-stack-migration-2026-07.md).

## Provenance & license
Vendored from **[joypad-ai/joypad-os](https://github.com/joypad-ai/joypad-os)** v2.2.0 @ `970656e`,
**Apache-2.0** (see [`LICENSE`](LICENSE)). Ported files keep their upstream paths and carry a
`// Ported from joypad-os @970656e` header when modified. Compatible with PicoSwitch2's Apache-2.0.
Upstream layout is preserved (`core/`, `bt/bthid/`, `bt/transport/`, `usb/usbh/hid/`) so vendored
`#include "core/..."` / `#include "bt/..."` paths resolve against `src/bt_hid/`.

## Layout
- **`bt/bthid/`** — the HID host (`bthid.{c,h}`, `bthid_registry.{c,h}`) + per-vendor drivers under
  `devices/vendors/{sony,nintendo,microsoft,google,augmental}/`, plus the generic gamepad driver
  (`devices/generic/bthid_gamepad.c`) that parses arbitrary HID descriptors via
  `usb/usbh/hid/devices/generic/hid_parser.{c,h}` — this is how Xbox / Elite are handled.
- **`bt/transport/bt_transport_cyw43.{c,h}`** — the Pico W CYW43 radio path, on the SDK's BTstack.
- **`bt/btstack/`** — joypad-os's BTstack HID-host glue, adapted onto the SDK BTstack we already link.
- **`core/{buttons,input_event}.h`** — the unified `JP_BUTTON_*` input model drivers emit.

## The seam — `ns2_seam.c`
`ns2_seam.c` implements the framework hooks the joypad-os drivers call (`router_submit_input`,
`feedback_get_state`, `router_device_disconnected`, …) as thin adapters onto **our** side:
`input_event_t` → `switch_pro_input_t` (per-family remap + DualSense→Switch IMU transform), single
player 0, rumble/LED bridged to `report_get_rumble()` / config lightbar. This replaced bluepad32's
`fill_input()` / `forward_rumble()`. Everything above the seam (the core0 USB/report path) is
unchanged by the BT stack.

## Build
Built into the default/shipping firmware — no flag. `CMakeLists.txt` compiles `src/bt_hid/` and puts
it (plus `src/bt_hid/bt/bthid`) on the include path.

## Not vendored (out of scope)
joypad-os's `apps/`, `ble_output`, and the non-CYW43 transports (`esp`/`nrf`/`usb`) — PicoSwitch2
only needs the HID *host* side on the Pico W radio.
