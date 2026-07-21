# Joy-Con 2 Mouse Mode — Research & Implementation Design

> How the Switch 2 Joy-Con 2 acts as an optical mouse, how the console enables it, the on-wire data
> format (validated against a genuine capture), what PicoSwitch2 already has, and a feasibility/design
> path to emulating it from a paired Bluetooth mouse.
>
> **Implementation update (2026-07-21):** generic Bluetooth HID mouse classification/parsing,
> relative-motion delivery, Joy-Con feature negotiation, pointer posture, click mapping, and
> wheel-to-local-stick scrolling are implemented. Pointer activation and buttons are
> hardware-validated; wheel direction/cadence awaits the next hardware check. Mouse output is
> gated by both a structurally identified mouse source and console feature bit `0x10`, and mouse
> disconnect clears the shared state and held scroll pulse. Companion to
> [`COMMAND-IMPLEMENTATION.md`](COMMAND-IMPLEMENTATION.md) §6 (command surface) and
> [`docs/experiments/2026-07-19-usb-command-ab-diff.md`](docs/experiments/2026-07-19-usb-command-ab-diff.md)
> Exp 2 (where the enable mechanism was first found).
>
> Evidence: `nso-gc-refs/switch2_controller_research/{hid_reports,commands}.md` (ndeadly),
> the decrypted capture
`nso-gc-refs/switch2_controller_research/captures/nrf52840/btle_joycon2_mouse_mode_decrypted.pcapng`
(**decoded here**),
> `src/switch_joycon2/*`, `src/bt_hid/core/input_event.h`. External reference: **Dycool /
> NS-PC-Control**, which implements Joy-Con 2 mouse output (treated as structured hypothesis — not
> read line-by-line in this repo). Confidence: **Confirmed** (byte-verified here) · **Strong Evidence**
> · **Hypothesis** · **Unknown**.

## 0. Summary up front

- **What it is:** each Joy-Con 2 has an **optical sensor on its flat inner (rail) face**. Laid flat on
  a desk and slid around, the Joy-Con reports **mouse motion** to the console — the Switch 2's
  headline "mouse mode."
- **How it's enabled: Confirmed.** Not a dedicated command — it is **feature bit 4 (`0x10`, "Mouse
  data")** set via the ordinary `0x0C` feature-select command. In the genuine capture the mask is
  **`0x37`** (`0x27` standard + `0x10` mouse). (This *refuted* the earlier "command `0x13` = mouse
  mode" idea — Exp 2.)
- **Data format: Confirmed against a real capture.** Joy-Con 2 native reports (`0x07` L / `0x08` R)
  carry **relative** ΔX/ΔY + a lift-off byte at offset `0x9`; report `0x05` carries an **absolute**
  X/Y variant at offset `0x10`.
- **PicoSwitch2 feasibility: HIGH for the output format; MEDIUM end-to-end.** The wire format is
  understood and the Joy-Con 2 encoder already **reserves `0x9..0xD` for mouse**, and the input
  *data model* exists (`INPUT_TYPE_MOUSE`, int16 `delta_x/delta_y`, the MouthPad precedent). **But
  the input path for a real Bluetooth mouse does not exist yet** (audit, §6): there is **no generic
  BT/BLE mouse driver** — a plain paired mouse is *classified* but never *parsed* — and
  **`ns2_seam.c` carries no mouse motion to the Switch 2 personality at all** (even the MouthPad's
  motion only reaches the SInput path, never a Joy-Con 2). Those two pieces are the actual feature,
  designed in §7 as **"pair a Bluetooth mouse and use it as a Joy-Con 2 in mouse mode."**

## 1. The physical mechanism

The Joy-Con 2 mouse sensor is an optical flow sensor (mouse-grade) on the side that faces the rail
when attached. It measures **relative surface motion** (like any optical mouse) plus a **lift-off /
proximity** signal (how far the sensor is from the surface). This is why the native report is
**relative deltas**, not an absolute screen position — the console integrates the deltas into a
cursor, exactly like a desktop mouse.

Both Joy-Con 2 (L and R) have the sensor; the Pro Controller 2 and NSO GameCube do **not** (the mouse
field is documented "Only present on Joycon controllers", `hid_reports.md:43`).

## 2. Enabling mouse mode (Confirmed)

From the feature-flag table (`commands.md` §"Command 0x0C"):

| Bit | Mask | Feature |
|---|---|---|
| 0 | 0x01 | Button state |
| 1 | 0x02 | Analog sticks |
| 2 | 0x04 | IMU |
| **4** | **0x10** | **Mouse data (JoyCon only)** |
| 5 | 0x20 | Rumble |
| 7 | 0x80 | Magnetometer |

The genuine Joy-Con 2 mouse session negotiates features with the standard commands, mask **`0x37`**:

```
0x0C/02  37 00 00 00   Set feature mask = 0x37   (0x27 base + 0x10 mouse)
0x0C/04  37 00 00 00   Enable features  = 0x37
```

So mouse mode is a **declarative feature toggle**: the console sets bit 4, and the controller then
populates the mouse field in whatever input report is selected. There is **no separate "mouse"
command**. (Full timestamped sequence in Exp 2.)

## 3. On-wire data format

### 3a. Relative — Joy-Con 2 reports `0x07` (L) / `0x08` (R), offset `0x9`, 5 bytes (Confirmed)

| Offset (in report) | Size | Field | Type |
|---|---|---|---|
| 0x9 | 2 | **Delta X** | int16 LE (signed) |
| 0xB | 2 | **Delta Y** | int16 LE (signed) |
| 0xD | 1 | **Lift-off / proximity** | uint8 |

**Validated by decoding
`nso-gc-refs/switch2_controller_research/captures/nrf52840/btle_joycon2_mouse_mode_decrypted.pcapng`**
(4222 notifications on GATT handle `0x000E`). Real consecutive samples (8-bit report counter shown):

```
ctr=72  dX=  0  dY=  2  liftoff=0x1d    bytes@0x9: 00 00 02 00 1d
ctr=73  dX= -6  dY=  9  liftoff=0x1b    bytes@0x9: fa ff 09 00 1b
ctr=74  dX= -9  dY= 15  liftoff=0x14    bytes@0x9: f7 ff 0f 00 14
ctr=75  dX= -3  dY= 17  liftoff=0x19    bytes@0x9: fd ff 11 00 19
ctr=77  dX=  0  dY= 23  liftoff=0x13    bytes@0x9: 00 00 17 00 13
```

Observations: ΔX/ΔY are small signed per-report deltas (a smooth slide left-and-up here), consistent
with a high-rate mouse. The lift-off byte hovers `0x10`–`0x1e` while the sensor is on the surface (its
exact scale/units are unconfirmed — §8).

### 3b. Absolute — report `0x05`, offset `0x10`, 8 bytes (Strong Evidence, not seen in this capture)

| Offset | Size | Field | Note |
|---|---|---|---|
| 0x10 | 2 | **Position X** | absolute, uint16 |
| 0x12 | 2 | **Position Y** | absolute, uint16 |
| 0x14 | 2 | Unknown — "Surface quality?" | uint16 |
| 0x16 | 2 | Unknown — "Lift-off distance?" | uint16 |

`hid_reports.md:65-72`. The genuine mouse capture streamed the **native relative** report (`0x07/08`),
not `0x05`, so the absolute form is documented but **unobserved here**. It likely exists for hosts
that select report `0x05`; whether the console ever uses the absolute path for mouse games is unknown.

## 4. Reference implementation — Dycool / NS-PC-Control

The user notes **Dycool's repo (NS-PC-Control) implements Joy-Con 2 mouse mode**. It is the external
proof-of-concept that a host/controller pair can drive the mouse path end to end. **Not read
line-by-line in this repo** — treat its specifics as structured hypothesis until cross-checked, the
same standard applied to Dycool's NFC work (`nfc-protocol-inventory.md` §4). Concretely, studying it
would settle: the exact enable ordering, any per-axis scaling/inversion, how it fills the lift-off
byte, and whether it drives the relative (`0x07/08`) or absolute (`0x05`) path. **Suggested action:**
a focused audit of NS-PC-Control's mouse code, logged as an experiment, mirroring the NFC audit.

## 5. What the console consumes vs. what we'd emit

PicoSwitch2 mouse mode would be an **output** feature: present an emulated Joy-Con 2 that, when the
console sets feature bit 4, streams mouse deltas in report `0x07`/`0x08`. The input has to come from a
**paired pointing device** (there is no optical sensor on the dongle).

## 6. Current PicoSwitch2 state (audited)

**Already present (the data model + one precedent):**
- **Mouse data model.** `input_event.h` defines `INPUT_TYPE_MOUSE`, int16 `delta_x`/`delta_y`
  (+`delta_wheel`), and an `as_gamepad` flag ("present a MOUSE-type device ALSO as a gamepad …
  plain mice leave this false"). The vocabulary to carry mouse motion exists.
- **MouthPad precedent — but a narrow one.** The **MouthPad** driver submits `INPUT_TYPE_MOUSE`
  deltas (`mouthpad_ble.c:103,257-258`), proving a pointing device *can* drive an emulated output.
  **Caveats the earlier draft glossed over:** it is matched **by name** (a device-specific driver,
  not a generic mouse), and its mouse motion drives the **SInput** mouse interface — **not** a
  Joy-Con 2.
- **Classification only.** The BT-HID stack classifies a paired pointing device as
  `BTHID_DEVICE_MOUSE` from its Class-of-Device (`bthid.c:354`) — but classification is *not*
  parsing (see gap 0).
- **Reserved output field.** The Joy-Con 2 encoder reserves the mouse bytes
  (`switch_joycon2_encode.c:112` — *"0x9..0xD mouse … no source for them"*), currently zero-filled.

**Missing (the actual work — larger than the first draft claimed):**
0. **No generic BT/BLE mouse driver.** A plain paired Bluetooth mouse is *classified*
   `BTHID_DEVICE_MOUSE` but **never parsed** — nothing turns its standard HID mouse report (buttons +
   relative X/Y + wheel) into `INPUT_TYPE_MOUSE` deltas. Only the MouthPad's bespoke driver does.
   **This is new work, not "already flows."**
1. **`ns2_seam.c` drops mouse entirely.** The Switch 2 seam (`router_submit_input`) fills only
   gamepad fields; it has **no `INPUT_TYPE_MOUSE`/`delta_x`/`delta_y` handling**, so mouse motion
   never reaches the NS2 global input or a Joy-Con 2 — even from the MouthPad. This is the concrete
   form of the `switch_pro_input_t` gap below, and it **corrects the earlier claim** that "Bluetooth
   mouse motion already flows into the system" (it flows only to the SInput path).
2. **Feature-bit-4 gating.** The Joy-Con 2 `0x0C` handler (`switch_joycon2.c:436-453`) does not yet
   track a "mouse enabled" flag on `0x0C/04` when mask & `0x10` (as Pro2 gates the IMU on `0x04`).
3. **Input-seam carry + populate.** `switch_pro_input_t` (`include/switch_pro.h`) has **no
   mouse-delta fields**; add them (+ a `has_mouse` flag), carry them through `ns2_seam`, and populate
   `0x9..0xD` (int16 ΔX @ `0x9`, ΔY @ `0xB`, lift-off @ `0xD`) when mouse mode is enabled.
4. **Routing/UX.** A mouse is `INPUT_TYPE_MOUSE`, not a gamepad, so decide how it maps to the active
   Joy-Con 2 slot — mouse-only, or mouse + a paired gamepad (the `as_gamepad` precedent). See §7.

## 7. Proposed architecture — pair a Bluetooth mouse as a Joy-Con 2 mouse (design, not implemented)

The requested feature end to end: **pair an ordinary Bluetooth mouse to the dongle; the console sees
a Joy-Con 2 whose optical mouse it can enable, and the mouse drives it.** Four new links, from the
radio to the wire:

```
BT/BLE mouse ──▶ (NEW) generic mouse driver ──INPUT_TYPE_MOUSE──▶ input_event{delta_x/y, buttons}
   (or MouthPad, existing)                                              │
                                                                        ▼
                                     (NEW) ns2_seam mouse handling: carry delta_x/y (+has_mouse,
                                     +click→button) into switch_pro_input_t via set_global_gamepad_input
                                                                        │
                                                                        ▼
                              switch_joycon2_encode_report07/08()  — if console set feature bit 4:
                                   out[0x9..0xA] = scale·Δx     (DPI, §8)
                                   out[0xB..0xC] = scale·Δy
                                   out[0xD]      = lift-off (synthesized on-surface const, §8)
                                                                        │
                                                                        ▼
                                              Joy-Con 2 report 0x07/0x08 → console
```

### 7.1 The new generic mouse driver
A plain mouse has no bespoke driver today (§6 gap 0). Two report shapes to cover:
- **BT-Classic HID mouse:** request **boot protocol** (`SET_PROTOCOL` boot) → fixed
  `[buttons][Δx:i8][Δy:i8]( [wheel:i8] )`. Simplest MVP — trivial fixed parse, works for the vast
  majority of mice. **Ceiling:** boot protocol exposes only **3 buttons** (L/R/M); **side/extra
  buttons require report protocol** and report-map parsing — see §7.5.
- **BLE HID mouse (HOGP):** report-protocol only; parse the device's **HID report map** for the
  Generic-Desktop X/Y (usages `0x30`/`0x31`), buttons, and wheel — the project already has a HID
  report-descriptor parser (`usb/usbh/hid/devices/generic/hid_parser.c`) and the MouthPad shows the
  hand-rolled BLE pattern. Deltas may be 8/12/16-bit; sign-extend and normalize to the int16
  `delta_x/delta_y`.
- Output `INPUT_TYPE_MOUSE` with `delta_x/delta_y`, and map **L/R/M clicks + wheel** into the event
  (as buttons and `delta_wheel`).

### 7.2 ns2_seam mouse handling (the missing bridge)
`ns2_seam.c` must gain an `INPUT_TYPE_MOUSE` path (it has none, §6 gap 1): accumulate `delta_x/y`
into the NS2 input state and set `has_mouse`, so `switch_joycon2_build_report()` sees fresh deltas
each poll. Mouse motion is **per-poll relative** — accumulate between the encoder's report ticks and
zero after emitting, so no motion is dropped or double-counted at differing BT vs USB rates.

### 7.3 Assignment model (mouse alone vs mouse + gamepad)
A bare mouse can't press a stick or face buttons, so decide the slot policy:
- **Mouse-only Joy-Con** — motion + clicks only. Enough for the console's mouse-mode titles (the
  whole point of feature bit 4); clicks map to a couple of Joy-Con buttons.
- **Mouse + gamepad combo** — a paired gamepad drives stick/buttons while the mouse fills the mouse
  field, mirroring the `as_gamepad` precedent (one logical Joy-Con from two devices). Best "full
  controller + mouse" experience.

### 7.4 Click, lift-off, and scaling
- **Clicks:** mapped per the base map in §7.5.2 (Left→shoulder, Right→trigger, Middle→Home,
  Back→B, Forward→A); user-remappable.
- **Lift-off (`0xD`):** a desktop mouse has no proximity sensor, so **synthesize a constant
  on-surface value** (observed genuine range `0x10`–`0x1e`, §3a) — a mouse only reports while on a
  surface, so "always on-surface" is correct.
- **DPI/scaling (§8):** genuine sensor counts ≠ mouse counts; apply a scale (config-tunable) so feel
  matches. MVP 1:1 then tune.

### 7.5 Mouse buttons — variable count and side buttons (incl. 8BitDo Retro R8)

Different mice expose different button counts, so **buttons cannot be a fixed layout** — they must
be **discovered** from the mouse and mapped by *index*. This is the same problem the generic gamepad
path already solves, and the solution reuses those exact mechanisms.

#### 7.5.1 The protocol: how HID reports mouse buttons
- **Boot protocol = 3 buttons, hard ceiling.** Byte 0 is `[bit0=L][bit1=R][bit2=M]`; there is no room
  for more. Any mouse with side buttons **must** be driven in **report protocol** (§7.1).
- **Report protocol = declared, variable count.** The mouse's HID report map declares a button field
  as **Usage Page `0x09` (Button), Usage Minimum `0x01`, Usage Maximum `N`**, with `ReportCount = N`
  one-bit fields (often padded to a byte). **`N` is the exact button count** — read it from the
  report map (`hid_parser.c` already extracts Usage Min/Max, `ReportCount`, `ReportSize`). This is
  how "different mice, different counts" is resolved by *discovery*, not assumption.
- **Standard button numbering (HID convention):** `1`=Left (primary), `2`=Right, `3`=Middle/wheel,
  `4`=**Back** (side), `5`=**Forward** (side), `6+`=vendor-defined extras. Motion/wheel follow as
  Generic-Desktop `X 0x30` / `Y 0x31` / `Wheel 0x38`, plus optional Consumer **AC Pan** (`0x0C`,
  usage `0x0238`) for a horizontal/tilt wheel.

| Mouse class | Buttons | Usages |
|---|---|---|
| Boot / basic | 3 | L, R, M |
| Standard 5-button | 5 | + Back(4), Forward(5) |
| **8BitDo Retro R8** | core + **4 programmable** | needs capture — §7.5.3 |
| Gaming mice | 6–12+ | vendor extras (Buttons 6…N) |

#### 7.5.2 The design: discover-then-map by index (reuse existing machinery)
The mouse driver runs the **same usage-number button loop** the generic gamepad parser already uses
(`bthid_gamepad.c`): for each declared button *i*, set one normalized `JP_BUTTON_*` by a fixed
**index → button** table. Because it's driven by the parsed count `N`, one code path adapts to any
mouse — 3-button, 5-button, or 12-button — with no per-device code.

- **Core three** land on primary controls; **side buttons and extras land in the existing extended
  `JP_BUTTON_*` space** — `buttons.h:83` already reserves "extra controller-specific buttons"
  (`L4/R4/L5/R5/A3/A4/A5/F1/F2`), which is exactly where mouse buttons 4…N go.
- Once in `JP_BUTTON_*`, mouse buttons flow through the **normal Joy-Con button pipeline** and are
  **user-remappable like any controller** — the mouse driver never hard-codes a Joy-Con output; the
  `JP_BUTTON_* → Joy-Con` step is the encoder's job and is subject to the mapping profiles in
  [`JOYCON2-AUDIT.md`](JOYCON2-AUDIT.md) §7/§12.

**Default (base) map.** Keyed off the near-universal side-button convention — **button 4 = Back,
button 5 = Forward** (browser navigation) — so the two side buttons are reliably present and
meaningful on essentially any 5-button mouse. Fully remappable; this is the shipped starting point:

| Mouse button | Joy-Con output (target) | Normalized `JP_BUTTON_*` emitted |
|---|---|---|
| **1 — Left click** | **L / R** (shoulder, by side) | `JP_BUTTON_L1` |
| **2 — Right click** | **ZL / ZR** (trigger, by side) | `JP_BUTTON_L2` |
| **3 — Middle click** | **Home** | `JP_BUTTON_A1` |
| **4 — Back** (side) | **B** | `JP_BUTTON_B1` |
| **5 — Forward** (side) | **A** | `JP_BUTTON_B2` |
| 6…N — extras | extended (remap) | `JP_BUTTON_L4 / R4 / A3 …` |

Wheel → `delta_wheel`; AC Pan (horizontal wheel) → a second wheel/button if present.

Two things this base map depends on:

- **"L or R" / "ZL or ZR" = the active Joy-Con side.** The driver emits one normalized `JP_BUTTON_*`;
  the encoder resolves it to the single Joy-Con's actual button by side (Left JC → `L`/`ZL`, Right JC
  → `R`/`ZR`). Mouse-mode buttons take a **direct** route — they are *not* run through the sideways
  stick rotation (that transform is motion-only; a mouse has no stick), so this map does not inherit
  the sideways face-button remap. See [`JOYCON2-AUDIT.md`](JOYCON2-AUDIT.md) §7/§12.
- **Right-Joy-Con-oriented, with Left substitutions.** `A`, `B`, and `Home` exist on the **Right**
  Joy-Con (and Pro); the **Left** Joy-Con has directional buttons + `Capture`/`Minus` instead. So the
  base map targets the Right Joy-Con's set cleanly; on a Left personality, `A/B → ` two direction
  buttons and `Home → Capture` (documented substitutions, remappable). Mouse-in-right-hand ≈ Right
  Joy-Con is the expected common case.

**Side buttons that arrive as Consumer usages.** Some mice report Back/Forward not as mouse Buttons
4/5 but as **Consumer Control `AC Back` (`0x0224`) / `AC Forward` (`0x0225`)** (HID usage page
`0x0C`). Route those via the existing `consumer_usage` field / the quirks path (§7.5.2) to the same
`B`/`A` outputs, so the base map holds regardless of which representation a given mouse uses.

#### 7.5.3 Worked example — 8BitDo Retro R8 Mouse (4 programmable buttons)
The **Retro R8** is a real 8BitDo product (editions: Xbox/N/C64/Forest; ships with a "Retro R8
Adapter" 2.4 GHz receiver and Bluetooth), programmed by 8BitDo's Ultimate/Advance software
(`8Bitdo/research/.../LanguageTools.cs:256+`). Its 4 programmable buttons are the concrete
"side-button" target — with **one 8BitDo-specific caveat (Hypothesis, needs capture):**

- 8BitDo programmables can be assigned to **mouse buttons, keyboard keys, or macros**. So the R8's 4
  extra buttons may appear as **mouse Buttons 4–7** *or*, depending on how they're programmed, arrive
  in a **separate keyboard/consumer HID report** entirely — the same dual-nature behavior the repo
  already handles for the 8BitDo **Ultimate controller** (`switch_pro_8bitdo_ultimate_*`,
  `switch_pro_bt.c:190,215`).
- **Resolution path:** capture the R8's **report map** (which fields/usages it declares) *and* a
  **live press of each of the 4 buttons in its default mode** to learn whether they are mouse buttons
  or remapped keys. If mouse buttons → the §7.5.2 index loop handles them for free. If keyboard/
  consumer → handle them via the **quirks mechanism** (`bthid_gamepad_quirks.h` — "optional per-report
  extraction beyond the standard usage-number button loop"), mapping those usages into the same
  extended `JP_BUTTON_*` slots. Either way, **no new core path** — this is why the discover-then-map
  design scales from a 3-button mouse to the R8 to a 12-button gaming mouse.

**Phasing (each hardware-testable):**
- **Phase 1 — enable + boot-mouse plumbing:** generic BT-Classic boot-protocol mouse driver;
  `ns2_seam` mouse path; feature-bit-4 gating; emit `0x9..0xC` with a fixed on-surface lift-off.
  **Test: a paired USB/BT mouse moves the Switch 2 cursor via the Joy-Con 2 personality.**
- **Phase 2 — BLE mice + accuracy:** BLE HOGP report-map parsing; per-axis DPI scaling; lift-off
  refinement (§8).
- **Phase 3 — polish:** click/wheel remapping, mouse + gamepad combo (§7.3), absolute (`0x05`) path
  if any host needs it.

## 8. Unknowns & hypotheses

| Unknown | Hypothesis | Test |
|---|---|---|
| **Lift-off byte (`0xD`) semantics/units** | Sensor-to-surface proximity or "surface confidence"; `0x10`–`0x1e` = on-surface. Console may ignore deltas when it reads "lifted." | Diff lift-off across on-surface vs lifted segments in the capture; sweep constant values on hardware and watch cursor gating. |
| **Delta units / scaling / DPI** | Raw sensor counts per report at the native poll rate; console applies its own sensitivity. | Move a known physical distance and integrate ΔX/ΔY from the capture; compare to genuine mouse counts. |
| **Report rate** | Native `0x07/08` streams fast (thousands of notifications); deltas are per-report. | Timestamp-diff the `0x000E` notifications in the capture. |
| **Absolute (`0x05`) X/Y + "surface quality"** | Alternate representation for `0x05`-selecting hosts; surface-quality = sensor SNR. | Capture a session that selects report `0x05` with mouse enabled (none in-repo yet). |
| **Sensor calibration in SPI** | A mouse/optical calibration block may exist in flash (like stick/motion calib). | Scan Joy-Con 2 SPI dumps (`dumps/SWITCH2_JOYCON_L_1.bin`, `_R_1.bin`) near the known calib regions. |
| **Simultaneous mouse + stick/motion** | Mask `0x37` keeps buttons/sticks/IMU/rumble alongside mouse; all fields coexist in one report. | Confirmed by mask decomposition; verify our encoder can fill mouse without disturbing stick/motion. |

## 9. Experiments (ranked; analysis-first, no code)

1. **Audit NS-PC-Control's mouse implementation** (§4) — the reference; settles scaling, lift-off,
   enable ordering. Log as an experiment like the NFC audit.
2. **Timestamp + integrate the capture deltas** — derive the native report rate and a counts-per-mm
   estimate to calibrate our scaling before writing code.
3. **Scan Joy-Con 2 SPI dumps** for a mouse/optical calibration block (§8).
4. **(Needs hardware)** capture a mouse session that selects report `0x05` to observe the absolute
   form; and an on-vs-off-surface sequence to decode the lift-off byte.
5. **Survey real mice for boot-protocol support** (§7.1) — confirm the pragmatic MVP: do common
   BT-Classic mice honor `SET_PROTOCOL` boot (fixed `[buttons][Δx][Δy][wheel]`)? Determines whether
   Phase 1 can skip report-map parsing entirely.
6. **BLE HOGP report-map parse test** (§7.1) — validate the existing HID parser
   (`hid_parser.c`) against a BLE mouse's report map to extract X/Y/buttons/wheel offsets.
7. **8BitDo Retro R8 button classification** (§7.5.3) — capture the R8's report map + a live press of
   each of its 4 programmable buttons in default mode; determine whether they are HID mouse Buttons
   4–7 or a separate keyboard/consumer report. Settles whether the R8 needs a quirk or is handled by
   the plain index loop.

## 10. Risks & notes

- **Output-only, needs a pointing device.** No dongle sensor; value depends on the user having a
  Bluetooth mouse (or MouthPad) paired. The plumbing precedent exists, so this is low-risk to add.
- **Personality scope.** Mouse mode only makes sense in the **Joy-Con 2** personalities (the console
  gates the field to Joy-Con). Pro2/GC must never emit it.
- **Indistinguishability.** Getting the lift-off/scaling right matters for feel; Phase-1 with a fixed
  lift-off will likely move the cursor but may not match genuine sensitivity until Phase 2.
- **No firmware change here** — this document scopes the work; nothing is implemented.

## 11. References

- `nso-gc-refs/switch2_controller_research/hid_reports.md:43,65-72` (report `0x05` absolute mouse),
  `:109,122-128` / `:144,157-163` (reports `0x07`/`0x08` relative mouse).
- `nso-gc-refs/switch2_controller_research/commands.md` §"Command 0x0C" (feature bit 4 = mouse).
- `nso-gc-refs/switch2_controller_research/captures/nrf52840/btle_joycon2_mouse_mode_decrypted.pcapng`
  — decoded in §3a (handle `0x000E`).
- `src/switch_joycon2/switch_joycon2_encode.c:112` (reserved `0x9..0xD`),
  `src/switch_joycon2/switch_joycon2.c:436-453` (`0x0C` handler).
- `src/bt_hid/core/input_event.h:159-177` (mouse deltas + `as_gamepad` flag),
  `src/bt_hid/bt/bthid/devices/vendors/augmental/mouthpad_ble.c:103,257-258` (the only
  `INPUT_TYPE_MOUSE` producer — device-specific, SInput-path only).
- `src/bt_hid/bt/bthid/bthid.c:354` (mouse *classified* but not parsed),
  `src/bt_hid/usb/usbh/hid/devices/generic/hid_parser.c` (reusable HID report-map parser for §7.1).
- `src/bt_hid/ns2_seam.c` `router_submit_input` (no `INPUT_TYPE_MOUSE` path today — §6 gap 1);
  `src/report.c` `set_global_gamepad_input` (where carried deltas would land).
- Button-mapping machinery to reuse (§7.5): `src/bt_hid/bt/bthid/devices/generic/bthid_gamepad.c`
  (usage-number button loop), `bthid_gamepad_quirks.h:39-61` (per-report extraction beyond that loop),
  `src/bt_hid/core/buttons.h:83` (extended `JP_BUTTON_*` slots for extra buttons),
  `src/bt_hid/bt/bthid/devices/vendors/nintendo/switch_pro_bt.c:190,215`
  (`switch_pro_8bitdo_ultimate_*` — programmable-button dual-nature precedent).
- 8BitDo Retro R8 Mouse: `8Bitdo/research/mixed-software/8bitdo-v2-decompiled/AdvanceSuper.Data/LanguageTools.cs:256+`
  (product/editions + Retro R8 Adapter), programmed by 8BitDo Ultimate/Advance software.
- `docs/experiments/2026-07-19-usb-command-ab-diff.md` Exp 2 (mask `0x37` discovery).
- External: **Dycool / NS-PC-Control** — reference Joy-Con 2 mouse implementation (audit pending).
