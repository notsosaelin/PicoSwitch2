# Bluetooth Stack Migration — bluepad32 → BTstack/joypad-os HID layer

**Status:** design / planning (no code yet). **Goal:** replace bluepad32's flattened
virtual-gamepad input with a native BTstack HID host (ported from
[joypad-ai/joypad-os](https://github.com/joypad-ai/joypad-os), Apache-2.0) that exposes **every**
button on every controller, so the emulated Switch 2 Pro Controller can forward GL/GR/C, Xbox Elite
paddles, DualSense Edge buttons, gyro, and rumble faithfully. Companion research catalog:
[controller-research.md](../bluetooth/controller-research.md).

> **Context.** The USB side is DONE — the dongle is a fully working native Switch 2 Pro Controller
> (`docs/switch2/`). This migration is purely on the **Bluetooth input side** (core1). The USB /
> report-builder side (core0, `src/switch_pro2/`, `src/switch_pro/`) is **not touched** by this work.

---

## 1. Why migrate

bluepad32 normalizes every pad into one `uni_gamepad_t` shaped like an Xbox pad. That is why, today:

- **Extra buttons are invisible.** GL/GR/C (Switch 2), Xbox **Elite paddles**, DualSense **Edge**
  back buttons, DualSense **touchpad/mute** — none reach us; `uni_gamepad_t` has no fields for them.
- **Input has a little lag** (bluepad32's parse + normalize + event pipeline).
- **Audio is impossible.** bluepad32 gives no low-level access to the BT link, so controller audio
  (Xbox/PS headset over BT) can never be routed.

joypad-os instead runs **BTstack directly** with **per-vendor HID parsers** that decode each pad's
raw report into a **unified 22-button model** (W3C ordering, `JP_BUTTON_*`) plus analog axes, gyro,
and touchpad. It is the definitive open-source controller-decoding reference and is Apache-2.0.

---

## 2. Architecture: current vs target

### Current (bluepad32)
```
CYW43 radio ─ BTstack ─ libbluepad32 ─ uni_gamepad_t ─┐
                                                      │  pico_switch_platform.c
                                        fill_input() ─┤    (family remap, 12-bit sticks,
                                                      │     DualSense→Switch IMU, rumble)
                              set_global_gamepad_input()
                                                      │
                          switch_pro_input_t  ◄── THE SEAM (shared, critical-section)
                                                      │
                       core0: switch_pro / switch_pro2 report builders → USB
```

### Target (joypad-os bthid)
```
CYW43 radio ─ BTstack ─ bthid host ─ vendor driver.process_report() ─┐
              (transport   (conn mgmt,   (DS5 / Switch2 / Xbox-generic  │
               _cyw43)      report route)  → unified buttons+axes+IMU)  │
                                                     jp_input_t (unified)│
                                             adapter: jp_input → ───────┤
                                             switch_pro_input_t (EXTENDED)
                                                     │
                          switch_pro_input_t  ◄── SAME SEAM (extended for extras)
                                                     │
                       core0: report builders (UNCHANGED) → USB
```

**Key idea:** keep `switch_pro_input_t` as the cross-core seam. Only what sits *below* it changes.
Extend the struct with the extra buttons + confirm IMU fields (already present), and the entire
core0 USB/report path keeps working untouched.

---

## 3. The seam: `switch_pro_input_t` extension

Today (`include/switch_pro.h`): `buttons[3]` (Switch-1 masks), `left/right_stick[3]` (12-bit packed),
`accel[3]`, `gyro[3]`. Add an explicit extras field so the report builders can emit GL/GR/C:

```c
typedef struct {
    uint8_t buttons[3];      // existing Switch-1 masks (A/B/X/Y/L/R/ZL/ZR/±/home/capture/dpad/L3/R3)
    uint8_t extra;           // NEW: bit0 C(chat) bit1 GL bit2 GR  (report 0x09 byte2 0x10/0x08/0x04)
    uint8_t left_stick[3];
    uint8_t right_stick[3];
    int16_t accel[3];        // already plumbed; report 0x05 emits it, 0x09 pending (see §7)
    int16_t gyro[3];
} switch_pro_input_t;
```

`ns2_build_report` (report 0x09) then sets `p[0x04] |= in.extra` (C=0x10, GL=0x08, GR=0x04 per
`docs/switch2/usb-spec.md §7`). No other report code changes.

---

## 4. What we port from joypad-os (scope)

| Area | joypad-os path | LOC | Needed? |
|---|---|---|---|
| HID host + conn mgmt + report routing | `src/bt/bthid/{bthid,bthid_registry}.c` | ~700 | **Yes** (core) |
| Vendor drivers | `src/bt/bthid/devices/vendors/**` + `generic/` | ~6.8K | **Yes** (the value) |
| Unified input model | `src/core/{buttons.h,input_event.h}` + router | ~small | **Yes** (adapt) |
| BTstack config + host glue | `src/bt/btstack/**` | ~5.8K | **Partial** — we already have a working BTstack (SDK) + `btstack_config.h`; reuse ours, port only the HID-host glue |
| CYW43 transport | `src/bt/transport/bt_transport_cyw43.c` | ~part of 1.4K | **Yes** (our radio) — likely close to what pico_cyw43_arch already provides |
| BLE output / other transports / apps | `ble_output`, `esp/nrf/usb` transports | — | **No** |

Rough first-cut port: **~8–9K LOC** brought in, of which the vendor drivers (~6.8K) are the payload.
Large, but self-contained and mostly mechanical (they're leaf parsers with a clean interface).

---

## 5. Migration phases (each phase leaves a working build)

The migration is gated behind a **CMake option** so `main` stays shippable throughout:
`option(BT_STACK_JOYPAD "Use the joypad-os bthid stack instead of bluepad32" OFF)`. bluepad32 remains
the default until the new stack reaches parity.

- **Phase 0 — Skeleton (no behavior change).** Add `src/bt_hid/` (vendored joypad-os bthid + core
  model + attribution/LICENSE). Compile it in but keep bluepad32 driving input. Get it *building* for
  pico2_w. *Deliverable: it links.*
- **Phase 1 — BTstack host up.** Bring up the bthid host on our existing SDK BTstack + CYW43 (replace
  bluepad32's `uni_*` init with `bthid_init` + `bthid_registry_init`). Wire pairing window / bond
  wipe / LED (currently bluepad32 callbacks in `pico_switch_platform.c`) to bthid connection events.
  *Deliverable: a controller connects and disconnects; LED states work.*
- **Phase 2 — Input parity.** Write the `jp_input_t → switch_pro_input_t` adapter (buttons via the
  `JP_BUTTON_*` table → Switch masks + `extra`; axes 0–255 → 12-bit; IMU passthrough). Register the
  generic + Xbox/DS/Switch2 drivers. *Deliverable: Xbox/DualSense/Switch pads drive the Switch 2
  emulation exactly like today, minus the lag.*
- **Phase 3 — Extra buttons.** Map Elite paddles / Edge back buttons / touchpad → GL/GR/C via a
  config profile (see §6). *Deliverable: GL/GR/C register on the console.*
- **Phase 4 — Rumble.** Route `report_get_rumble()` → `bthid_send_output_report()` per-vendor
  (DS5 haptics, Xbox motors). *Deliverable: rumble works, ideally richer than bluepad32's.*
- **Phase 5 — Flip default + retire bluepad32** once parity is verified on real hardware for the
  main controller families, then remove `libbluepad32` from the link.

Config-mode UI, the Switch-1 output path, and multi-controller support ride along unchanged because
they all sit above the seam.

---

## 6. Extra-button mapping (GL/GR/C)

`JP_BUTTON_*` already carries the extras (`controller-research.md`): `A2`=Capture/Touchpad/Mute,
`L4`/`R4`=paddles/back-buttons. Default profile (user-remappable in config mode):

| Switch 2 target | Source (unified) | Xbox Elite | DualSense Edge | Pro 2 |
|---|---|---|---|---|
| **GL** (left grip) | `JP_BUTTON_L4` | Paddle P3/P1 | Left back (Fn/paddle) | GL |
| **GR** (right grip) | `JP_BUTTON_R4` | Paddle P4/P2 | Right back | GR |
| **C** (chat) | `JP_BUTTON_A2` | *(none — leave 0 or remap)* | Touchpad-click / Mute | C |

Controllers without extras simply leave those bits 0 (today's behavior). A native Switch 2 Pro 2 over
BLE (via `switch2_ble` driver) passes GL/GR/C straight through.

---

## 7. Gyro & rumble

- **Rumble — solved by the port.** Each vendor driver owns its output-report format; we call
  `bthid_send_output_report`. This is strictly better than bluepad32's single `play_dual_rumble`.
- **Gyro — half solved.** `switch_pro_input_t.{accel,gyro}` is already populated (our `fill_input`
  does the DualSense→Switch transform; the vendor drivers will do the same per-pad) and **report 0x05
  already emits it** (works on PC/Steam). The **remaining gap is console-side**: report **0x09**'s
  40-byte motion block packing is still unknown, so the console gets no gyro yet. That is an
  **independent RE task** (not part of this migration) — candidate sources: the real report-0x09
  motion bytes in `captures/usb/rumble-procon-gccon.pcapng` (len `0x1E`=30) and ndeadly's
  `btle_procon2_motion_0x000A/0x000E.pcapng`. Track separately.

---

## 8. Risks & rollback

- **Biggest risk:** this replaces code that currently *works*. Mitigation: the `BT_STACK_JOYPAD`
  CMake flag keeps bluepad32 as the default and a one-flag rollback until parity is proven.
- **BTstack config drift:** joypad-os tunes `btstack_config.h`; we must merge its needs (HID host,
  more L2CAP channels) into ours without breaking the SDK BTstack we already use. Do this in Phase 1
  in isolation.
- **Bond storage / pairing gestures:** our BOOTSEL double-tap window + triple-tap wipe + allow-list
  live in `pico_switch_platform.c` against bluepad32 APIs; they must be re-implemented against bthid
  connection events in Phase 1.
- **Flash/RAM budget:** +~8K LOC of drivers; verify the pico2_w image still fits (it will; drivers
  are small). pico_w (RP2040) is tighter — validate.
- **Licensing:** joypad-os is Apache-2.0 (compatible with our Apache-2.0). Vendor the ported files
  under `src/bt_hid/` with the upstream `LICENSE` + a `THIRD_PARTY` note and per-file provenance
  headers. Keep bluepad32's licensing intact while it coexists.

---

## 9. Immediate next step

Phase 0: create `src/bt_hid/` with the joypad-os bthid host + core button model + one driver (start
with **Xbox via the generic HID parser**, since it needs no vendor quirks) vendored with attribution,
and get it **compiling** alongside bluepad32 behind `-DBT_STACK_JOYPAD=ON` (default OFF). No behavior
change yet — just prove the port builds on pico2_w. Then Phase 1 brings the host up.
