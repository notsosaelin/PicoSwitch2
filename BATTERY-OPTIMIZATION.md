# Battery Passthrough — Accuracy Audit & Optimization

> Deep audit of the controller→dongle→Switch battery path, aimed at making the reported
> state **as close to native as possible**. Research/documentation only — **NO CODE WAS
> CHANGED.** Verified against the live code and authoritative references (Linux HID drivers,
> dekuNukem Switch RE, ndeadly's Switch 2 RE `@ d1c5a7f`).
>
> Status: 🟡 core scaling is sound; **two concrete decode bugs found** (Switch Pro, Wii U Pro)
> plus several accuracy refinements. Prioritized in §7.

## 0. Corrected framing (important)

The Switch 2 shows controller battery as an **icon**, not a percentage. On hardware it **tracks
the controller's charge and reflects charging state**. Therefore the console is displaying from
the **dynamic Power Info byte** (`[1]=charging`, `[2:5]=level 0–9`), **not** from the battery
voltage. This **demotes** the earlier fixed-voltage finding (now §6.2) and makes the real work
the **per-controller decode accuracy** and the **level→0–9 mapping** below.

## 1. Pipeline

```
BT report ─▶ controller_battery_decode_<pad>(raw)  ─▶ {level 0..100, charging}   (controller_battery.c)
          ─▶ input_event_set_native_battery(level, charging)                     (input_event.h)
     OR   ─▶ input_event_set_bas_battery(level)   [BLE Battery Service, true 0..100%]
          ─▶ controller_battery_switch2_power_info(valid, level, charging)        (power byte, offset 0x01)
```
Native HID wins permanently over BAS (`input_event_set_bas_battery` no-ops once a native reading
exists). Encoder call sites: `switch_pro2.c:803`, `switch_gc_encode.c:13`, `switch_joycon2_encode.c:65`.

## 2. The Switch 2 battery model (ndeadly)

- **Power Info byte** (every console-native report `0x07..0x0A`, offset `0x01`): bitfield
  `[0]`=external power, `[1]`=charging, `[2:5]`=level **0–9**, `[6:7]`=reserved. **This drives the icon.**
- Battery **voltage (mV)** — report `0x05:0x1F` and command `0x0B/0x03`; **charge status** —
  `0x05:0x21` and `0x0B/0x04`. Present but (per §0) not the icon source.

The exact 0–9 → icon-segment thresholds are console-internal and unknown to us. A genuine Pro
Controller 2 derives its 0–9 from a Li-ion voltage curve. Best we can do: emit the **same 0–9 a
genuine pad would emit at the same true charge** — i.e. an accurate % and a faithful %→0–9 map.

## 3. Per-controller decode analysis

Legend: ✅ matches authority · ⚠️ inaccuracy · 🔴 bug.

### 3.1 DualSense / DualSense Edge — ✅ (source: Linux `hid-playstation.c`)
Raw at BT `report_data[52]`: `capacity = status[0] & 0x0F` (0–10), `charging = status[0] >> 4`.
Linux: case `0x0` discharging / `0x1` charging → `min(cap*10+5,100)`; `0x2` full → 100; `0xa/0xb/0xf`
→ 0/unknown. **PicoSwitch2 matches exactly** for 0x0/0x1/0x2 (`level*10+5`, clamp 100). Differences,
both defensible: full (0x2) is reported **not charging** (genuine "full on cable" ≈ charging-complete);
error states (0xa/0xb/0xf) `return false` → **retain previous** (avoids flicker to 0%). Native
resolution is **11 buckets** with a `+5` midpoint estimate — this is the dominant DualSense limit,
not a bug.

### 3.2 DualShock 4 — ✅ (source: Linux `hid-playstation.c`)
Raw at `report_data[29]`: `cap = raw & 0x0F`, `cable = raw & 0x10`. PicoSwitch2 mirrors Linux:
no-cable `raw<10→raw*10+5, ==10→100` (discharging); cable `<10→*10+5`, `==10→100` (charging),
`==11→100` (full, not charging), else retain. **Correct.**

### 3.3 DualShock 3 — ✅ (source: Linux `hid-sony` `sixaxis_battery_capacity[]`)
Raw at `data[29]`: table `{0,1,25,50,75,100}` for 0–5, `0xEE`→charging/100, `0xEF`→100.
**Exactly the Linux table.** Coarse by nature (6 states).

### 3.4 Switch Pro / Joy-Con / Switch-format 8BitDo — 🔴 BUG (source: dekuNukem RE)
Authoritative byte-2 layout: **high nibble** = battery `8=full, 6, 4, 2, 0=empty`, **LSB of that
nibble = charging** (byte mask `0x10`); low nibble = connection info.

Current `controller_battery_decode_switch_pro(battery_conn)`:
```c
uint8_t raw = battery_conn >> 4;                 // includes the charging LSB in the level
uint16_t level = raw > 8 ? 100 : raw * 12 + 5;   // charging inflates the level by ~one half-step
return store(out, level, (battery_conn & 0x08) != 0);  // 0x08 is a *connection-info* bit, not charging
```
Two defects:
1. **Level includes the charging LSB.** When charging, the nibble is odd (9/7/5/3/1); e.g. medium+charging
   = 7 → `7*12+5 = 89%` instead of ~77% for medium (6). Charging states read ~12% too high.
2. **Charging read from the wrong bit** (`0x08` = connection-info bit 3; the real charging bit is `0x10`).
   → Switch Pro/8BitDo charging indication is effectively wrong.

**Native-faithful mapping:** `level3 = (battery_conn >> 5) & 0x07` (0–4), `%= level3*25`
(`0/25/50/75/100`); `charging = (battery_conn & 0x10) != 0`. (This is the one Nintendo-native source —
it should round-trip to the console almost perfectly.)

### 3.5 Wii U Pro — 🔴 BUG (source: Linux `hid-wiimote-modules.c`)
Authoritative extension byte 10 layout (MSB→LSB): `BATTERY[bits 5–7] | USB[bit4] | CHARG[bit3] |
LTHUM[bit2] | RTHUM[bit1]`; BATTERY 0–4 (000 empty…100 full); USB active-low; **CHARG active-low
(0=charging)**.

Current `controller_battery_decode_wii_u_pro(status)`:
```c
uint8_t raw = (status >> 4) & 0x07;         // reads bits 4–6, should be bits 5–7 (>> 5)
uint16_t level = raw >= 4 ? 100 : raw * 25;
return store(out, level, (status & 0x04) == 0); // reads bit 2 (LTHUM), CHARG is bit 3 (0x08)
```
Both fields are shifted **one bit low**: the level mixes in the USB bit and drops the MSB, and
charging reads the left-thumb-button bit. A full, unplugged pad can decode as ~25%. The `*25`
scaling and active-low polarity are otherwise right.
**Native-faithful mapping:** `bat = (status >> 5) & 0x07` (0–4), `% = bat*25`;
`charging = (status & 0x08) == 0`.
> ⚠️ Verify against a real Wii U Pro extension capture before acting — the bug is inferred from the
> Linux doc diagram; the consistent one-bit offset strongly implies it, but a capture confirms it.

### 3.6 Wiimote — ✅ formula, ⚠️ inherently rough (source: Linux `hid-wiimote-modules.c`)
Raw at status-report `data[6]`: `% = raw * 100 / 255` — matches Linux exactly. But the Wiimote's
raw byte vs. AA-cell charge is **markedly non-linear**, so any linear scale is a coarse estimate;
no charging (AA cells) — correctly `false`.

### 3.7 BLE Battery Service pads — ✅ best source (Xbox, Switch 2, Stadia, MouthPad, generic)
Standard BAS characteristic delivers a **true 0–100%** (`bas_client_handler` → `bthid_set_battery_level`).
This is the **most accurate** input and maps cleanly to 0–9. Caveat: BAS carries **no charging
state**, so these always report not-charging; and the *controller's own* % may itself be coarse
(e.g. Xbox firmware reports few internal levels). Native-HID readings correctly override BAS.

### 3.8 No battery (Classic Xbox, BattlerGC Pro, generic Classic HID)
No telemetry → the encoder's safe default (`0x25`: external power, not charging, level 9). Shows a
full/wired icon. Acceptable, but see §6.1 (it also asserts external power).

## 4. The %→0–9 encode

`switch2_power_info`: `level09 = (pct*9 + 50)/100` — **correct linear rounding**. Two accuracy notes:
- **Double quantization** for coarse native sources: DualSense 11 buckets → % (midpoint) → 10
  buckets. Perceptually minor for a coarse icon, but a **direct native-bucket→0–9 map** (skipping the
  % waypoint) would be strictly more faithful for DualSense/DS4/DS3/SwitchPro/WiiUPro.
- **Endpoint bias:** the `+5` midpoint makes "empty" read as 5% and "full-ish" as 95%; combined with
  rounding, a nearly-empty pad can land on level 0 (empty icon) and levels 9/10 both on 9. If the
  console's low-battery warning triggers at a specific level, this endpoint handling is where a
  visible mismatch would come from.

## 5. Summary matrix

| Source | Native res. | Decode vs authority | Charging | Notes |
|---|---|---|---|---|
| DualSense/Edge | 11 (0–10) | ✅ Linux | ✅ (full→not charging) | midpoint `+5`; dominant coarseness |
| DualShock 4 | 11 + cable | ✅ Linux | ✅ | — |
| DualShock 3 | 6 | ✅ Linux table | ✅ (EE/EF) | very coarse |
| **Switch Pro / 8BitDo** | 5 (0/2/4/6/8) | 🔴 level+charging bit wrong | 🔴 wrong bit | Nintendo-native; should be near-perfect once fixed |
| **Wii U Pro** | 5 (0–4) | 🔴 one-bit-low on both fields | 🔴 wrong bit | verify vs capture |
| Wiimote | 256 (non-linear) | ✅ formula | n/a | inherently rough |
| Xbox/Switch2/Stadia/MouthPad/generic (BAS) | 0–100% | ✅ true % | ❌ none in BAS | **best source** |
| Classic Xbox / BattlerGC / generic Classic | none | default 0x25 | — | full/wired fallback |

## 6. Secondary encode issues

### 6.1 `external power` bit hardcoded to 1 — 🟠
`switch2_power_info` always ORs `0x01`. Since the icon still tracks level (§0), it isn't hiding the
level, but a battery-powered wireless pad is not externally powered; the console may render a
persistent plug/AC hint. Consider driving it from the pad's actual cable/charge state (0 for a
discharging wireless pad), and test whether the icon then matches native more closely.

### 6.2 Fixed voltage / charge status (`0x0B`, report `0x05`) — 🟡 (demoted)
`switch_pro2.c:744` replays constants (`0xA5,0x0E` = 3749 mV; `0x34,0x00,0x83,0x00`). Harmless for
the **icon** (§0), but wrong if any Switch surface (e.g. a detailed/settings view) reads voltage.
Low priority; derive from the normalized % if ever needed.

### 6.3 Legacy Switch 1 encoder drops charging — 🟡
`switch1_connection_info` maps %→`{0,2,4,6,8}` and never sets the Switch 1 charging nibble; `*12`
slightly under-scales vs `*12.5`. Legacy path, low priority.

## 7. Recommendations, prioritized (no code changed here)

1. **Fix Switch Pro decode (§3.4)** — mask the charging LSB out of the level and read charging from
   `0x10`. Highest value: it is the Nintendo-native source, so a correct decode should round-trip to
   the console almost exactly. Also fixes Switch-format 8BitDo pads.
2. **Fix Wii U Pro bit alignment (§3.5)** — battery `>>5`, charging `& 0x08` — after confirming with a
   real extension capture.
3. **Re-evaluate the `external power` bit (§6.1)** — set from real state; test icon fidelity at `0`.
4. **Consider direct native-bucket→0–9 maps (§4)** for coarse sources to drop the double quantization,
   and align the empty/low/full endpoints with the console's icon thresholds.
5. **Prefer/照 keep BAS as the high-accuracy path** where available (§3.7); it needs no scaling and only
   lacks charging.
6. Low priority: Switch 1 charging + `*12.5` (§6.3); voltage derivation (§6.2).

## 8. Verification experiments (hardware)
- **Localize decode vs encode:** compare config mode's normalized % (pre-encode) against the Switch
  icon at several real charge points. Divergence with a *correct* normalized % ⇒ encode/console side;
  a wrong normalized % ⇒ decode side.
- **Switch Pro / Wii U Pro:** capture the raw battery byte at known charge/charging states and confirm
  the §3.4/§3.5 bit layouts before/after any change.
- **`external power`:** A/B `0`-vs-`1` and observe the icon/plug rendering.
- **Charging fidelity:** plug/unplug each pad; confirm the console's charging indicator follows (this
  is where the Switch Pro/Wii U Pro charging-bit bugs would show).

## 9. Sources
- `src/controller_battery.c`, `include/controller_battery.h`; decoders' call sites in
  `src/bt_hid/bt/bthid/devices/vendors/{sony,nintendo}/*.c`; encoder in `src/switch_pro2/switch_pro2.c`
  (incl. `0x0B` at ~L744) and the per-personality encoders.
- Linux: `drivers/hid/hid-playstation.c` (DS4/DS5), `hid-sony` (`sixaxis_battery_capacity`),
  `hid-wiimote-modules.c` (Wiimote + Wii U Pro).
- dekuNukem `Nintendo_Switch_Reverse_Engineering/bluetooth_hid_notes.md` (Switch Pro battery byte).
- ndeadly `switch2_controller_research/{hid_reports,commands,bluetooth_interface}.md` @ `d1c5a7f`.

---

# 10. Output fidelity — are we outputting in a Switch 2-native fashion?

Method: compared our encoder output to ndeadly's genuine report spec and to the genuine captures
in the repo (`usbpcaptures/genuine_procon_2.pcapng`, `dumps/BLE CAPTURE/*.ndjson`). Note on
evidence: the BLE captures are **report `0x05`** (32-bit counter + battery **voltage** at `0x1F` +
charge-state at `0x21`), and the genuine USB capture is the **command/setup exchange** — so the
dump set contains **no genuine report `0x09`/`0x0A` streaming capture** (the one carrying the
0–9 Power Info byte). Format is therefore verified against the spec; live power-info **values** are
inferred from the related report-`0x05` battery fields.

## 10.1 Power Info byte — ✅ native in format
Offset `0x1` of every console-native report; bits `[0]`=ext power, `[1]`=charging, `[2:5]`=level
0–9, `[6:7]`=reserved. Our `switch2_power_info()` emits `0x01 | (charging?0x02:0) | (level<<2)`:
level occupies bits 2–5, **reserved bits 6–7 stay 0**, level is clamped 0–9. All four personalities
fill offset `0x1` identically (`switch_pro2.c:803`, `switch_gc_encode.c:13`,
`switch_joycon2_encode.c:65`) and ndeadly shows Power Info at `0x1` for `0x07/0x08/0x09/0x0A` alike.
**The byte's shape, position, and range are native.**

## 10.2 `external power` bit — ⚠️ non-native hybrid
Hardcoded to `1`. Genuine evidence from the report-`0x05` **charge-state byte (`0x21`)**: it reads
**`0x00` while wireless/discharging** (our BLE "still" capture: every sample `0x00`), rising toward
`0x34` when charging and `0x20` when full. So a genuine **wireless** pad presents as on-battery
(ext = 0), and a genuine **wired** pad presents as charging (ext = 1, charging = 1, level climbing
to full). Our dongle is USB-wired to the console, so `ext = 1` imitates a wired pad — but we pair
it with the **wireless** controller's *discharging* level and `charging = 0`. That trio
(wired + discharging + not charging) is a state a genuine controller never emits. Since the intent
is to surface the *wireless* pad's battery, the more native fiction is **`ext = 0`** (a
battery-powered wireless controller). Worth an A/B on hardware (§8).

## 10.3 Voltage & charge-state — 🟡 fixed, non-native (low impact for the icon)
Genuine report-`0x05` voltage (`0x1F`) is **dynamic** — the capture shows **3669–3670 mV** at that
charge and it drifts; charge-state (`0x21`) is `0x00`/`0x34`/`0x20` as above. We replay a **fixed
3749 mV** and a **fixed `0x34`** ("charging") for command `0x0B`, and leave report `0x05`
placeholders. Two consequences: (a) any voltage-based surface reads a constant; (b) our fixed
`0x34` **claims charging** even for a discharging pad — inconsistent with our own Power Info
charging bit. Per §0 the *icon* uses Power Info, so this is low priority, but the internal
inconsistency is worth removing.

## 10.4 Level span / endpoints — ⬜ unconfirmed
We emit the full 0–9 span via linear rounding. Whether a genuine Pro2 actually uses all of 0–9 (or,
say, never emits 0 and tops at 8, and where its low-battery warning trips) is **not confirmed** —
no genuine report-`0x09` discharge capture exists in the dump set. **Recommended capture:** a
genuine Pro Controller 2 streaming report `0x09` across a full charge→empty discharge, to calibrate
the level curve and the empty/low/full endpoints (§11.2).

**Verdict:** the level+charging output is **byte-native in format and position**; the only real
deviations are the hardcoded `external power` bit (10.2) and the fixed voltage/charge-state (10.3),
the latter largely cosmetic for the icon.

---

# 11. Getting precise readings from coarse-battery controllers

**Fundamental limit:** you cannot recover more resolution than the controller reports —
DualSense/DS4 = 11 buckets (0–10), DS3 = 6, Switch Pro / Wii U Pro = 5, Wiimote = a single
non-linear byte. The Switch 2 *icon* is itself coarse, so the achievable wins are, in order:
**(a) stability**, **(b) correct bucket→icon placement**, **(c) sub-bucket estimation from
temporal behavior**. These are presentation/estimation strategies, not new controller data.

## 11.1 Stability — highest value, low complexity
- **Hysteresis / dead-band at bucket edges.** Change the shown 0–9 only after a *sustained* move
  (e.g. N consecutive agreeing reports, or a ≥1.5-bucket delta). A coarse gauge that dithers between
  two adjacent buckets otherwise makes the icon flicker — a large chunk of "looks inaccurate."
- **EWMA / long-time-constant low-pass.** Battery moves over hours, so a τ of minutes removes noise
  with no meaningful lag. Apply on the normalized % before the 0–9 encode.
- **Monotonic-while-discharging clamp.** When `charging == 0`, never let the estimate rise; only a
  charging state may raise it. Matches real cell physics and stops upward bounces from noisy reads.

## 11.2 Correct bucket→icon placement
- **Skip the 0–100% waypoint for coarse native sources** (§4): map native buckets **directly** to
  0–9 with a fixed table (e.g. DualSense 0–10 → 0–9) instead of `bucket → % → 0–9`, removing the
  double quantization.
- **Endpoint calibration** against the §10.4 genuine capture, so "empty"/"low warning"/"full" land
  where the console expects them (today's `+5` midpoint shows empty at 5% and full at 95%).
- **Per-controller discharge curve.** Li-ion voltage→charge is non-linear; where a source's buckets
  are voltage-derived (DualSense fuel gauge), map with a matching curve so "half charge" lands on
  the icon's half rather than at a linear midpoint.

## 11.3 Sub-bucket estimation — real added precision, higher complexity
- **Drain-rate interpolation ("gas gauge").** Track wall-clock dwell time in each bucket and the
  observed drain rate; between transitions, interpolate a finer % from elapsed time. Example: a
  DualSense held at level 5 (~55%) for 25 min, historically draining ~1 bucket per ~45 min, is
  ~44% — sub-bucket resolution derived purely from temporal data.
- **Activity-weighted coulomb-ish counting.** Integrate estimated drain between coarse updates
  (optionally weighting for rumble/audio load, which draw more), and **re-anchor to the coarse
  reading on every bucket transition** to bound drift.
- **State handling.** Keep per-controller estimator state (last bucket, dwell, rate) keyed by
  controller identity so a reconnect resumes; **reset on disconnect and on any charge-state change**
  (charging invalidates the discharge model).

## 11.4 Per-source strategy

| Source | Native resolution | Recommended precision strategy |
|---|---|---|
| DualSense / DS4 | 11 (0–10) | direct 11→0–9 table + hysteresis; optional drain-rate interpolation (§11.3) |
| DualShock 3 | 6 | curve-map 6→0–9 + hysteresis |
| Switch Pro / 8BitDo | 5 | after the §3.4 bug fix: direct 5→0–9 + hysteresis |
| Wii U Pro | 5 | after the §3.5 fix: direct 5→0–9 + hysteresis |
| Wiimote | non-linear byte | non-linearity LUT + heavy EWMA; drain-rate interpolation viable |
| BAS (Xbox / Switch 2 / Stadia / MouthPad / generic) | true 0–100% | already fine — light smoothing only; supply charging from a secondary source if available |

## 11.5 Optional: a precise voltage path
If any Switch surface ever shows a **numeric %** (it would read voltage — report `0x05:0x1F` or
command `0x0B/0x03`, currently fixed per §10.3), the sub-bucket estimate from §11.3 could be emitted
as a **derived voltage** via a Li-ion %→mV curve, giving a precise numeric readout even from a
coarse source. Only worthwhile if such a surface exists; the battery **icon** does not use it.
