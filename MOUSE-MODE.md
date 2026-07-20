# Joy-Con 2 Mouse Mode — Research & Implementation Design

> How the Switch 2 Joy-Con 2 acts as an optical mouse, how the console enables it, the on-wire data
> format (validated against a genuine capture), what PicoSwitch2 already has, and a feasibility/design
> path to emulating it from a paired Bluetooth mouse.
>
> **Documentation only — no code changed.** Companion to
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
- **PicoSwitch2 feasibility: HIGH for output emulation.** We already ingest Bluetooth mouse motion
  (`INPUT_TYPE_MOUSE`, int16 `delta_x/delta_y`; MouthPad precedent) and the Joy-Con 2 encoder already
  **reserves `0x9..0xD` for mouse**. The gaps are small and enumerated in §6–§7.

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

**Already present:**
- **Mouse input ingestion.** `input_event.h` defines `INPUT_TYPE_MOUSE` and int16 `delta_x`/`delta_y`
  (+`delta_wheel`), explicitly for "high-resolution pointers." The **MouthPad** driver already submits
  as `INPUT_TYPE_MOUSE` at 12-bit precision (`mouthpad_ble.c:19,103`), and the generic BT-HID stack
  classifies mice as `BTHID_DEVICE_MOUSE` (`bthid.c:354`). So **Bluetooth mouse motion already flows
  into the system.**
- **Reserved output field.** The Joy-Con 2 encoder explicitly reserves the mouse bytes:
  `switch_joycon2_encode.c:112` — *"0x9..0xD mouse … This project currently has no source for them."*
  The field is currently zero-filled.
- **The report descriptor** advertises the Joy-Con 2 report length that includes this region.

**Missing (the actual work):**
1. **Feature-bit-4 gating.** The Joy-Con 2 `0x0C` handler (`switch_joycon2.c:436-453`) mirrors the
   Pro2/GC structure and does not yet track a "mouse enabled" flag on `0x0C/04` when mask & `0x10`
   (analogous to how Pro2 gates the IMU on bit `0x04`).
2. **Input-seam carry.** `switch_pro_input_t` (the encoder's input struct, `include/switch_pro.h`)
   has **no mouse-delta fields** — the BT-side `delta_x/delta_y` are not propagated to the personality
   encoder. This seam needs a small extension (carry the deltas + a "has_mouse" flag).
3. **Populate `0x9..0xD`** from the carried deltas when mouse mode is enabled (int16 ΔX @ `0x9`, ΔY @
   `0xB`, lift-off @ `0xD`), replacing the zero-fill.
4. **Routing/UX.** Decide how a paired mouse maps to the active Joy-Con 2 slot (a mouse is
   `INPUT_TYPE_MOUSE`, not a gamepad — the seam already handles "MOUSE-type device also as gamepad"
   for MouthPad, a useful precedent).

## 7. Proposed architecture (design, not implemented)

```
BT mouse / MouthPad ──INPUT_TYPE_MOUSE──▶ input_event (delta_x, delta_y int16)
        │                                        │
        │                       (seam: add mouse deltas + has_mouse to switch_pro_input_t)
        ▼                                        ▼
 report.c shared state ─────────────▶ switch_joycon2_encode_report07/08()
                                             │ if console set feature bit 4 (mouse_enabled):
                                             │   out[0x9..0xA] = clamp16(delta_x)
                                             │   out[0xB..0xC] = clamp16(delta_y)
                                             │   out[0xD]      = lift-off (see §8)
                                             ▼
                                     Joy-Con 2 report 0x07/0x08 → console
```

**Phasing (each hardware-testable):**
- **Phase 1 — enable + plumbing:** track mouse-enabled from `0x0C/04` mask bit `0x10`; extend the
  input seam to carry deltas; emit them in `0x9..0xC`. Fixed lift-off constant (e.g. mid-range
  `~0x18`, matching the observed on-surface range). Test: a paired BT mouse moves the Switch 2 cursor.
- **Phase 2 — lift-off / accuracy:** model lift-off (present ~`0x14` while "moving", higher when
  idle?) and per-axis scaling to match genuine feel (§8).
- **Phase 3 — polish:** wheel/click mapping, absolute (`0x05`) path if any host needs it, DPI scaling.

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
- `src/bt_hid/core/input_event.h:159-173` (mouse deltas), `mouthpad_ble.c` (mouse-as-gamepad seam).
- `docs/experiments/2026-07-19-usb-command-ab-diff.md` Exp 2 (mask `0x37` discovery).
- External: **Dycool / NS-PC-Control** — reference Joy-Con 2 mouse implementation (audit pending).
