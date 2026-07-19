# Battery Passthrough — Scaling Audit

> Audit of the controller→dongle→Switch battery reporting path, focused on **scaling
> accuracy**. Research/documentation only — **no code was changed.** Sources: the live
> code (`src/controller_battery.c` and its callers), ndeadly's genuine Switch 2 protocol
> docs (`nso-gc-refs/switch2_controller_research/` @ `d1c5a7f`), and standard controller
> battery references.
>
> Status: 🟡 several concrete scaling issues found; the single decisive unknown (which
> channel the console *displays* from) needs a hardware A/B test (§6).

## 1. The data flow

```
BT controller report ─▶ controller_battery_decode_*(raw)  ─▶ 0..100% + charging
                        (src/controller_battery.c)
   ─▶ input_event_set_native_battery(level, charging)      ─▶ event.battery_level (0..100)
   ─▶ controller_battery_switch2_power_info(valid,level,ch) ─▶ power byte in the
                        (fills report offset 0x01)             console-native report (0x07..0x0A)
```

Call sites: `switch_pro2.c:803`, `switch_gc_encode.c:13`, `switch_joycon2_encode.c:65`
(Switch 2 power byte); `switch_pro.c:109` (legacy Switch 1 nibble).

## 2. What the genuine Switch 2 actually consumes (ndeadly)

A real Pro Controller 2 exposes battery on **three** channels, and PicoSwitch2 handles them
very differently:

| Channel | Where | Genuine content | PicoSwitch2 |
|---|---|---|---|
| **Power Info byte** | every console-native report `0x07..0x0A`, offset `0x01` | bitfield: `[0]`=external power, `[1]`=charging, `[2:5]`=level **0–9**, `[6:7]`=reserved | **dynamic** — `switch2_power_info()` |
| **Battery Voltage** | input report `0x05` offset `0x1F`; also **command `0x0B/0x03`** | battery voltage in **mV** | **FIXED** — see F1 |
| **Charge Status** | input report `0x05` offset `0x21`; also **command `0x0B/0x04`** | charge status | **FIXED** — see F1 |

The console defaults to report **#2** (`0x09` for a Pro Controller), which carries the Power
Info byte. Report `0x05` (#1, with voltage) is "common to all controllers" but ndeadly notes
it is "not known if input report ID #1 is currently used officially." The console **does**
issue command `0x0B` (that is why PicoSwitch2 implements it), at least during init.

## 3. Findings (by severity × confidence)

### F1 — Battery voltage & charge status are hardcoded replay values 🔴 (confirmed code fact)
`switch_pro2.c:744-747`:
```c
case 0x0B:  // battery
    if (sub == 0x03) { memcpy(d, (const uint8_t[]){0xA5,0x0E,0x00,0x00}, 4); ... } // voltage
    else if (sub == 0x04) { memcpy(d, (const uint8_t[]){0x34,0x00,0x83,0x00}, 4); ... } // charge
```
`0xA5,0x0E` little-endian = `0x0EA5` = **3749 mV**, the exact value from ndeadly's capture. So
the queried **voltage is constant (~50% of a Li-ion 3.30–4.20 V range) and the charge status is
constant**, independent of the real controller. Report `0x05`'s voltage field (offset `0x1F`)
is likewise a placeholder (see the "battery voltage/current placeholders" note in
`docs/bluetooth/battery-passthrough.md`).
**Impact:** if the console's battery UI (or any "precise %") derives from voltage, it will read
a **fixed ~50% regardless of the real charge** — the most concrete candidate for "not accurate."
This is the #1 thing to verify on hardware (§6).

### F2 — `external power` bit is hardcoded to 1 on every report 🟠 (confirmed code fact)
`controller_battery_switch2_power_info()` returns `0x01 | (charging?0x02:0) | (level<<2)` — bit 0
(`external power`) is **always set**, and the invalid-default `0x25` sets it too. A wireless
controller running on its own battery is *not* externally powered; asserting it may make the
console render a plugged/charging state (and possibly de-prioritise or freeze the displayed
level). It is arguable that the *dongle* is USB-powered so "external power" is technically true —
but then the reported *level* (the wireless pad's battery, which the dongle is **not** charging)
is semantically inconsistent with it. Worth testing `external power = 0` to see if the console
then shows the wireless pad's level faithfully.

### F3 — The 0–9 level is coarse, and the %→level map assumes linearity 🟠 (design)
`switch2_power_info()`: `switch_level = (pct*9 + 50)/100` → correct **linear** rounding into 0–9.
But a genuine controller derives its 0–9 level from a **non-linear Li-ion voltage curve**
(flat through the mid-range). If the console interprets each level per that genuine curve, a
linear %→level map is skewed wherever the real curve is non-linear (e.g. our "50%→5" may not be
the console's idea of level 5). Only 10 display steps exist here regardless, so this channel is
inherently coarse.

### F4 — Double quantization from coarse controller sources 🟡 (design)
The DualSense reports battery in **11 buckets (0–10)**; the decoder expands to % via a
midpoint (`level*10+5`), then the encoder re-quantises to **10 buckets (0–9)**. Worked example
(DualSense → Switch level):

| DS raw level | decoded % | Switch 0–9 |
|---:|---:|---:|
| 0 | 5 | 0 |
| 1 | 15 | 1 |
| 5 | 55 | 5 |
| 9 | 95 | 9 |
| 10 | 100 | 9 |

Monotonic and roughly right, but two lossy quantizations plus a 100% waypoint shift some buckets
(e.g. DS levels 9 and 10 both land on Switch 9; DS level 0 lands on empty).

### F5 — Legacy Switch 1 encoder drops charging state 🟡 (minor, legacy path)
`switch1_connection_info()` maps % → `{0,2,4,6,8}` (5 levels) and ORs a fixed low nibble `0x01`;
the Switch 1 "charging" convention (odd battery nibble) is never emitted, so a charging pad on a
Switch 1 host shows as discharging. Legacy path; low priority.

### F6 — Per-decoder accuracy notes 🟢 (reference)

| Decoder | Raw resolution | Mapping | Notes |
|---|---|---|---|
| DS5 | 0–10 (+status nibble) | `level*10+5`, clamp 100 | midpoint estimate; standard hid-playstation-style |
| DS4 | 0–11 (+cable bit) | `level*10+5`; 10/11→100 | same family |
| DS3 | 0–5 discrete | `{0,1,25,50,75,100}`; `0xEE/0xEF`→100 | very coarse (6 states) |
| Switch Pro | 0–8 nibble | `raw*12+5`, clamp 100 | should be ~`*12.5`; `*12` slightly under-scales top |
| Wii U Pro | 0–4 | `raw*25`; ≥4→100 | 25% steps; charging active-low (correct) |
| Wiimote | 0–255 | `raw*100/255` | fine-grained byte but the raw value is **non-linear** vs charge; only a rough estimate |

## 4. Summary of scaling correctness

- **Power Info level (0–9):** the %→0–9 arithmetic is **correct** (linear round). Limits are (a)
  coarseness, (b) the linear-vs-genuine-curve assumption (F3), (c) upstream double-quantization
  (F4), (d) the `external power` bit (F2).
- **Voltage / charge status:** **not scaled at all — fixed replay values (F1).** This is the
  most likely source of a visibly wrong reading *if the console uses this channel.*

## 5. Recommendations (no code changed here)

1. **Decide the console's display source first (§6) — do not tune blind.** If it reads voltage,
   fixing the 0–9 map does nothing; if it reads the level, the fixed voltage is harmless.
2. **Make voltage/charge track the real battery (F1).** Derive a plausible mV from the normalised
   % via a Li-ion curve (or a simple linear 3300–4200 mV map) so command `0x0B/0x03`, `0x0B/0x04`,
   and report `0x05:0x1F/0x21` move with the pad instead of sitting at 3749 mV.
3. **Reconsider the `external power` bit (F2).** Test `0`; set it (and charging) from the
   controller's actual cable/charge state rather than a constant.
4. **If the console uses the level, consider a non-linear %↔level map (F3)** matched to the
   genuine voltage curve, and/or map the controller's native buckets straight to 0–9 to avoid the
   double quantization (F4).
5. **Low priority:** emit Switch 1 charging (F5); refine Switch Pro `*12`→`*12.5` (F6).

## 6. Decisive hardware experiment

Vary a controller's real charge (e.g. a DualSense at ~90% vs ~20%) and watch the Switch 2 UI:
- **Reading barely moves / sits near ~50%** → the console is using the **fixed voltage** (F1) —
  highest-value fix.
- **Reading tracks but is off by a level or shows plugged/charging** → the **Power Info** path
  (F2/F3/F4) — tune the level map and the `external power` bit.
- Cross-check by reading config mode's own battery readout (the normalised %, pre-encode) against
  what the Switch shows; a divergence localises the fault to the **encode/console** side vs the
  **decode** side.

## 7. Sources
- `src/controller_battery.c`, `include/controller_battery.h`; callers in
  `src/switch_pro2/switch_pro2.c` (incl. the `0x0B` handler ~L744), `src/switch_gc/…`,
  `src/switch_joycon2/…`, `src/switch_pro/…`.
- `nso-gc-refs/switch2_controller_research/{hid_reports,commands,bluetooth_interface}.md` @ `d1c5a7f`.
- `docs/bluetooth/battery-passthrough.md` (existing behaviour + test matrix).
