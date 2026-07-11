# BTStack Implementation (`joypad-os`)

This project implements a native Bluetooth HID host stack based on [joypad-os](https://github.com/joypad-ai/joypad-os) to handle controller connections and inputs. **Bluepad32 has been completely removed** from this project in favor of this implementation, which provides much finer-grained control and parses extra controller features (e.g., paddles, capture buttons).

## Architecture & Submodules

The implementation lives primarily under `src/bt_hid/` and is tied to the standard Pico SDK `btstack` (specifically `pico_btstack_ble` and `pico_btstack_classic`).

### Core Host (`ns2_bt_host.c`)
- **Initialization:** Responsible for setting up the BTstack environment (`sm_init`, `l2cap_init`, `gatt_client_init`, etc.) and the various GATT/HIDS clients.
- **Connection Management:** Handles device scanning, connecting, and GATT discovery (for BLE HID). It also handles Classic BT HID device connection (`hid_host`).
- **Storage:** Persists link keys and bonding information to flash using BTstack's TLV flash storage.

### Drivers & Registry (`bthid.c`, `bthid_registry.c`, `devices/`)
- A registry-based system maps connected vendor/product IDs to a specific driver (`switch2_ble`, `ds5_bt`, `generic`, etc.).
- The driver parses the raw HID report from the controller and outputs a unified `input_event_t`.
- **Unified Button Model:** Converts all controller inputs into W3C standard bits (e.g., `JP_BUTTON_B1`, `JP_BUTTON_L4`), which enables agnostic handling of Switch, PlayStation, Xbox, and Generic controllers.

### The Seam (`ns2_seam.c`)
- Connects the isolated `joypad-os` domain to the PicoSwitch2 `switch_pro_input_t` format used by the USB core.
- **Router Submit:** When a driver completes parsing an input report, it calls `router_submit_input(const input_event_t *e)`. 
- **Remapping:** `ns2_seam` maps the generic `JP_BUTTON_*` bits to Switch 1 buttons and Switch 2 extra buttons (C, GL, GR) based on a configurable mapping.
- **Analog Packing:** Translates 8-bit `joypad-os` analog values into the packed 12-bit format required by the Switch Pro Controller.
- **Feedback:** Serves as a bridge for rumble and LED states. `report_get_rumble()` pulls the USB-requested rumble amplitude, which is pushed to the controller via its driver.

---

## Reconnect reliability (🟡 open — "Pro 2 reconnect sometimes needs a triple-tap")

Not yet root-caused on this project's own stack. External research (2026-07-10) into
`Dycool/NS-PC-Control` (`joycon-usb-experiments` branch, `server/src/bluetooth_manager.cpp`) — a
different project that also bridges controllers to a Switch 2, via BlueZ on Linux rather than
BTstack — surfaced concrete mechanisms worth checking against our own reconnect path as candidate
root causes, **not yet verified as applicable here**:

- **Per-device reconnect cooldown** (their `tick()`: 5 s between reconnect attempts per device) —
  avoids hammering a device mid-reconnect, which can itself prevent a clean connection.
- **Explicit exponential-backoff reconnect policy** (their `configure_bluez_reconnect_policy()`:
  15 attempts) specifically enabling BlueZ "fast-connectable" mode for the HID UUID
  (`00001124-...`) and HOGP device classes — i.e. treating HID reconnect as a distinct, faster
  path rather than falling back to generic BT reconnect timing.
- **Explicit disconnect on console suspend**, rather than leaving the link in a stale state —
  their `disconnect_gamepads()` tears down all BT links when the Switch sleeps, and reconnection is
  only attempted after a wake is positively observed, not on a fixed timer.

Our own `ns2_bt_host.c`/BTstack reconnect path has not been audited against these three mechanisms
specifically. **Suggested next step** for this backlog item: check whether BTstack's default
reconnect timing hammers the link without a cooldown, whether "fast-connectable"-equivalent
behavior exists/is enabled for our HID service, and whether we currently do anything special when
the console's own suspend/resume is detected (vs. just waiting for a generic link-loss timeout).

## BLE wake-from-sleep (research only — feature remains out of scope)

`docs/wakeup.md` in the same external repo documents how the Switch 2 actually wakes from BLE: it
requires a raw HCI-level replay of a **captured, real Joy-Con 2's exact advertisement payload**
(mimicking a HOME-button press) — including spoofing the BT adapter's public address to match the
already-bonded controller's real MAC. This is done by stopping `bluetoothd`, disabling LE privacy,
setting the adapter's public address via `btmgmt public-addr`, then issuing raw `hcitool cmd`
advertising commands — deliberately bypassing the normal BlueZ D-Bus API so existing links aren't
dropped. This confirms and sharpens (rather than changes) `PLAN.md`'s "out of scope" call: waking
the console isn't just "send *a* BLE advert," it requires impersonating a specific, already-bonded
controller's identity at the radio-address level, which our dongle (USB-attached, not acting as a
second bonded BLE peripheral identity) doesn't currently attempt. Not pursued — filed as
context only, in case this project's Bluetooth radio capability is ever repurposed for it.
