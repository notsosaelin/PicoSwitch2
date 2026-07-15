# Controller Research Catalog (bundled from joypad-os)

Definitive per-controller Bluetooth decoding reference for the
[BT stack migration](bt-stack-migration.md). Distilled from
[joypad-ai/joypad-os](https://github.com/joypad-ai/joypad-os) (**Apache-2.0**), the most complete
open-source controller-decoding project, cross-checked against ndeadly's Switch 2 research.

> **Attribution.** Button model, driver architecture, and per-vendor mappings below are from
> joypad-os `src/bt/bthid/` and `src/core/`. When the drivers are vendored into `src/bt_hid/` they
> carry the upstream Apache-2.0 `LICENSE` + provenance headers.

---

## 1. Unified button model (`JP_BUTTON_*`, W3C Gamepad ordering)

Every driver normalizes to this bitmask (bit index = W3C button index):

| Const | Bit | Xbox | Switch | PlayStation | → **Switch 2 emit** |
|---|---|---|---|---|---|
| `B1` | 0 | A | B | Cross | B |
| `B2` | 1 | B | A | Circle | A |
| `B3` | 2 | X | Y | Square | Y |
| `B4` | 3 | Y | X | Triangle | X |
| `L1` | 4 | LB | L | L1 | L |
| `R1` | 5 | RB | R | R1 | R |
| `L2` | 6 | LT | ZL | L2 | ZL |
| `R2` | 7 | RT | ZR | R2 | ZR |
| `S1` | 8 | Back | − | Select | Minus |
| `S2` | 9 | Start | + | Start | Plus |
| `L3` | 10 | LS | LS | L3 | L3 |
| `R3` | 11 | RS | RS | R3 | R3 |
| `DU/DD/DL/DR` | 12–15 | D-pad | D-pad | D-pad | D-pad |
| `A1` | 16 | Guide | Home | PS | Home |
| `A2` | 17 | — | Capture | Touchpad | **Capture / C** |
| `A3` | 18 | — | — | Mute | (→ C option) |
| `A4` | 19 | — | — | — | — |
| **`L4`** | 20 | **Paddle P1** | — | (Edge back L) | **GL** |
| **`R4`** | 21 | **Paddle P2** | — | (Edge back R) | **GR** |

Active-high. **Analog axes** (0–255, center 128; L2/R2 0=released): `LX,LY,RX,RY,L2,R2,RZ`.
**Y convention:** 0=up, 255=down (HID standard); Nintendo pads (inverted Y) invert before submitting.

The three columns that matter for us: `L4/R4/A2` are exactly the **GL/GR/C** the Switch 2 has —
now exposed by these per-vendor drivers (bluepad32 hid them). Confirmed on-console.

---

## 2. Driver interface (`bthid_driver_t`)

```c
typedef struct {
    const char* name;
    bool (*match)(name, class_of_device, vid, pid, is_ble);   // VID/PID > name > COD
    bool (*init)(bthid_device_t*);
    void (*process_report)(bthid_device_t*, const uint8_t* data, uint16_t len);  // raw HID → unified
    void (*task)(bthid_device_t*);                            // periodic: output reports / rumble
    void (*disconnect)(bthid_device_t*);
} bthid_driver_t;
```
Registry order = priority (first match wins); generic gamepad is the lowest-priority fallback.
Output: `bthid_send_output_report(conn, report_id, data, len)` for rumble/LEDs.

---

## 3. Supported controllers (joypad-os registry)

| Family | Driver | Match | Extras exposed | Rumble | Gyro |
|---|---|---|---|---|---|
| **Xbox** (One/Series/**Elite**) | *generic HID parser* (BlueRetro-style descriptor parse) | COD/HID | **Elite paddles → L4/R4** | motors | — |
| **DualShock 3** | `sony/ds3_bt` | VID 054C | — | motors | — |
| **DualShock 4** | `sony/ds4_bt` | VID 054C | touchpad→A2 | motors | yes |
| **DualSense / Edge** | `sony/ds5_bt` | VID 054C | touchpad→A2, mute→A3, **Edge back→L4/R4** | dual-actuator | yes |
| **Switch Pro** | `nintendo/switch_pro_bt` | VID 057E | capture→A2 | rumble | yes |
| **Switch 2 Pro 2 / Joy-Con 2 / NSO GC** | `nintendo/switch2_ble` | VID 057E BLE, PID 2066/2067/2069/2073 | **GL/GR/C** native | HD rumble | yes |
| **Wii U Pro** | `nintendo/wii_u_pro_bt` | name `-UC` | — | — | — |
| **Wiimote** | `nintendo/wiimote_bt` | name | — | rumble | (accel) |
| **Stadia** | `google/stadia_bt` | VID 18D1 | — | rumble | — |
| **Generic BT HID** | `generic/bthid_gamepad` | fallback, parses HID report descriptor | descriptor-defined | if present | — |

**Xbox is handled by the generic HID-descriptor parser, not a hardcoded map** — so all Xbox variants
(and the Elite paddles, which appear as extra buttons in the descriptor) work without per-model code.

---

## 4. Switch 2 native controllers (most relevant — VID `0x057E`)

From `switch2_ble.h` + ndeadly (`docs/switch2/`):

- PIDs: `0x2066` R-JoyCon2, `0x2067` L-JoyCon2, **`0x2069` Pro Controller 2**, `0x2073` NSO GC.
  BLE advertisement manufacturer company id **`0x0553`**. (L/R corrected 2026-07-14 — this
  ndeadly/BLE-sourced entry had them reversed; the real Linux kernel "HID: nintendo" driver source
  is authoritative here, see `docs/switch2-gc/usb-personality.md`.)
- Input report (0x09/0x05): sticks are 12-bit packed at report byte 10 (`raw_lx = b10 | ((b11&0x0F)<<8)`,
  `raw_ly = (b11>>4) | (b12<<4)`, then RX/RY at 13–15) — **identical packing to what we already emit**.
- **GL/GR/C** are native bits (report 0x09 byte 2: `0x10`=C, `0x08`=GL, `0x04`=GR — matches our
  emit side). A real Pro 2 over BLE passes them straight through.
- Per-stick factory calibration centers/ranges (`cal_*` in the driver) — Pro vs GC ranges differ.

This driver is the reference for **passing GL/GR/C through** when the *source* pad is itself a Switch 2
controller, and confirms our stick/button wire layout from the receive side.

---

## 5. Gyro / IMU

- **DualSense/DS4/Switch** expose gyro+accel; the driver decodes to signed rates. The seam
  (`ns2_seam.c`) applies the proven **DualSense→Switch** axis transform
  (`sw.x=−ds.z, sw.y=−ds.x, sw.z=+ds.y`) into `switch_pro_input_t.{accel,gyro}`.
- Emission: **report 0x05** carries accel/gyro and **works on Steam** (after the Experiment A
  timestamp + scale fix). **Report 0x09** (console) motion is decoded (int32 phase + Q16.16); the
  firmware rewrite is tracked separately ([../switch2/report-0x09-motion.md](../switch2/report-0x09-motion.md)).

## 6. Rumble

Per-vendor output reports (the reason to leave bluepad32): DS5 dual-actuator + optional trigger
haptics, Xbox motors, Switch HD rumble frames. joypad-os owns each format; we drive them via
`bthid_send_output_report` from `report_get_rumble()`. Start with simple amplitude → each driver's
basic rumble, refine to HD later.

---

## 7. Files to vendor (Phase 0), with provenance

```
joypad-os/src/bt/bthid/bthid.{c,h}                     → src/bt_hid/bthid.{c,h}
joypad-os/src/bt/bthid/bthid_registry.{c,h}            → src/bt_hid/bthid_registry.{c,h}
joypad-os/src/bt/bthid/devices/generic/*               → src/bt_hid/devices/generic/
joypad-os/src/bt/bthid/devices/vendors/{microsoft,sony,nintendo,google}/*
joypad-os/src/core/{buttons.h,input_event.h}           → src/bt_hid/core/
joypad-os/src/bt/transport/bt_transport_cyw43.*        → src/bt_hid/transport/ (adapt to pico_cyw43_arch)
+ joypad-os LICENSE + a THIRD_PARTY note; per-file "Ported from joypad-os @<commit>" headers.
```

Do **not** vendor `ble_output`, other transports (`esp/nrf/usb`), or the `apps/` — not needed.
