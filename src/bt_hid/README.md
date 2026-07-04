# `src/bt_hid/` — vendored joypad-os Bluetooth HID stack (WIP)

The native BTstack HID host that will replace bluepad32, exposing every button
(GL/GR/C, Xbox Elite paddles, DualSense Edge) + gyro + per-vendor rumble. See the
design in [`docs/bluetooth/bt-stack-migration.md`](../../docs/bluetooth/bt-stack-migration.md)
and the controller catalog in [`docs/bluetooth/controller-research.md`](../../docs/bluetooth/controller-research.md).

## Provenance & license
Vendored from **[joypad-ai/joypad-os](https://github.com/joypad-ai/joypad-os)** v2.2.0
@ `970656e`, **Apache-2.0** (see [`LICENSE`](LICENSE)). Ported files keep their upstream
paths and get a `// Ported from joypad-os @970656e` header when modified. Compatible with
PicoSwitch2's own Apache-2.0.

## Build gating
Compiled **only** under `-DBT_STACK_JOYPAD=ON` (default OFF → bluepad32 ships). `CMakeLists.txt`
excludes `src/bt_hid/` from the default source glob and adds it (with `src/bt_hid` on the include
path) only when the flag is on, so a work-in-progress port never breaks the shipping firmware.
Upstream layout is preserved under here (e.g. `core/`, `bt/bthid/`, `bt/transport/`) so the
vendored files' `#include "core/..."` / `#include "bt/..."` paths resolve against `src/bt_hid`.

## Vendored so far (Phase 0)
- `LICENSE` — joypad-os Apache-2.0.
- `core/buttons.h` — the unified `JP_BUTTON_*` model (W3C order; 22 buttons incl. `L4`/`R4`=paddles,
  `A2`=capture/touchpad). **Zero deps.**
- `core/input_event.h` — analog axes + event structs. **Zero internal deps** (stdlib only).

These two define the seam-side input model and dropped in unmodified.

## Vendored now (Phase 0)
- `bt/bthid/` — the full HID host: `bthid.{c,h}`, `bthid_registry.{c,h}`, and every driver
  (`devices/generic/` + `devices/vendors/{sony,nintendo,google,augmental}/`). 16 `.c` / 17 `.h`.
- `bt/transport/` — `bt_transport.{c,h}` + `bt_transport_cyw43.{c,h}` (the Pico W radio path).
- `core/{buttons,input_event}.h` — unified model (unmodified).

## Dependency surface (from the trial build) — vendor vs shim vs already-have
The vendored files reference these non-local headers. Strategy per group:

**REPLACE with thin adapters to our seam** (joypad-os's app framework — we don't want it; our
`switch_pro_input_t` + config already do this job):
- `core/router/router.h` — where drivers submit parsed input. Shim → write `switch_pro_input_t`
  (this IS the `jp_input → switch_pro_input_t` seam adapter, replacing bluepad32's `fill_input()`).
- `core/services/players/{manager,feedback}.h` — player assignment + rumble feedback. Shim →
  single player 0; feedback bridged to our `report_get_rumble()`.
- `core/services/keymap/keymap.h` — remap layer. Shim → passthrough (our config does remap).

**VENDOR (still to copy)**:
- `usb/usbh/hid/devices/generic/hid_parser.h` (+ its `.c`) — the BlueRetro-style HID descriptor
  parser the **generic (Xbox) driver** needs. Standalone-ish; bring it.
- `bt/btstack/btstack_host.h` (+ impl) — joypad-os's BTstack HID-host glue. Vendor + adapt onto the
  **SDK BTstack we already link** (don't double up BTstack).

**SHIM (small, ours)**:
- `core/services/storage/flash.h` — bond storage → back with `hardware_flash` (a spare sector).
- `platform/platform.h` — timers/logging/etc. → thin adapter over `pico_stdlib`.

**ALREADY HAVE (Pico SDK / SDK BTstack — just need them on the include path)**:
- `pico/{cyw43_arch,async_context,btstack_cyw43,btstack_hci_transport_cyw43}.h`,
  `btstack_run_loop.h`, `hci.h`, `hci_transport.h`.

## Build-order for the next passes
1. Vendor `hid_parser` + `btstack_host`; write the `flash`/`platform` shims.
2. Write the `router`/`players`/`keymap` adapter shims (the seam — start minimal: generic driver →
   `switch_pro_input_t`). Add `uint8_t extra` to `switch_pro_input_t` for GL/GR/C.
3. Merge joypad-os `btstack_config.h` needs (HID host, L2CAP channels) into our `src/btstack_config.h`.
4. Iterate `-DBT_STACK_JOYPAD=ON` to a clean build (generic/Xbox first; other vendor drivers follow).

## Not vendored (out of scope)
`ble_output`, non-CYW43 transports (`esp`/`nrf`/`usb`), and the `apps/` — PicoSwitch2 only needs the
HID *host* side on the Pico W radio.
