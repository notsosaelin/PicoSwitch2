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
