# Switch 2 Pro Controller — Protocol Research (ns2-testing)

**Goal of this branch:** make the PicoSwitch2 dongle enumerate to a Nintendo Switch 2 as a
**native Switch 2 Pro Controller** (rather than the Switch 1 Pro Controller the `main`
firmware currently emulates).

> **Source.** Distilled from
> [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research)
> (the MissionControl author's RE repo) and
> [alexvnesta/switch2controller](https://github.com/alexvnesta/switch2controller). The
> hex values here have since been **cross-checked byte-for-byte against the raw docs**
> (repo cloned and read directly); the exact, implementation-ready bytes now live in
> **[usb-spec.md](usb-spec.md)** — use that for coding. The one item still unverified is the
> USB command-endpoint transport (needs the USB pcap; see usb-spec §4). This page is the
> overview / feasibility; usb-spec.md is the reference.

---

## 1. USB identity

| Field | Switch 1 Pro (current) | **Switch 2 Pro (target)** |
|---|---|---|
| VID | `0x057E` | `0x057E` |
| PID | `0x2009` | **`0x2069`** |
| bcdDevice | — | `0x0200` (doc comment says 4.00, but bytes are `00 02`) |
| bDeviceClass | HID (single iface) | **`0xEF` / `0x02` / `0x01`** (Misc, IAD composite) |

Nintendo BLE Company ID: `0x0553`. Joy-Con 2 PIDs `0x2066`/`0x2067` (not our target).

### Configuration (composite, ~268 bytes, 5 interfaces)
- **IF0 — HID**: interrupt IN + OUT. Carries the controller input reports + command channel.
- **IF1 — Vendor-specific**: bulk IN + OUT.
- **IF2–IF4 — USB Audio Class**: stereo speaker (48 kHz) + mic. The PC2's new audio feature.

**Good news on the audio interfaces:** the **Joy-Con 2** and **NSO GameCube Controller** are
genuine Switch 2 controllers whose descriptors are just **HID + vendor-bulk, no audio** — so
the console definitely accepts Switch-2 controllers without audio interfaces. The open risk is
only whether it rejects `PID 0x2069` *specifically* unless audio is present. Plan: try a
minimal HID+vendor descriptor first (usb-spec Option B), fall back to the full audio descriptor
(Option A) only if needed.

---

## 2. Command protocol (over the HID channel)

New 8-byte command header (replaces the Switch 1 subcommand scheme):

| Off | Size | Field | Notes |
|---|---|---|---|
| 0x0 | 1 | Command ID | `0x01`–`0x18` |
| 0x1 | 1 | Direction | `0x91` host→device, `0x01` device→host |
| 0x2 | 1 | Transport | **`0x00` USB**, `0x01` Bluetooth |
| 0x3 | 1 | Subcommand | — |
| 0x4 | 1 | (unknown) | — |
| 0x5 | 1 | Length / ACK | request: data len; response: ACK byte |
| 0x6 | 2 | Reserved | `00 00` |

### USB initialization sequence (what the console does after enumeration)
1. **`0x07 / 0x01`** — first handshake. `07 91 01 01 00 00 00 00` → `07 01 01 01 10 78 00 00` + 1 flag byte.
2. **`0x03 / 0x0D`** — USB init; console sends its host BT address (reversed).
3. **`0x0C / 0x02` (+ `0x04`)** — feature mask: bit0 buttons, bit1 sticks, bit2 IMU, bit5 rumble, bit7 magnetometer (bit4 mouse = Joy-Con only).
4. **`0x03 / 0x0A`** — select input report (report ID `0x05` or `0x09`).
5. **`0x10 / 0x01`** — firmware version query; response type byte `0x02` = Pro Controller.
6. **`0x02 / 0x04`** — memory reads for calibration (see §4).

### Authentication — the key feasibility question
- The **AES-128-ECB challenge/response + XOR-derived LTK** (command `0x15`) is part of the
  **Bluetooth LE pairing** flow, used to derive the encrypted-link LTK. Standard BLE SMP is
  *rejected* by the controller — Nintendo uses a pseudo-OOB exchange over the HID command
  interface instead.
- Crucially, the controller's device key is a **fixed public constant**
  `5CF6EE792CDF05E1BA2B6325C41A5F10`, so `LTK = hostKeyA1 XOR B1` is computable and the
  challenge response `AES128_ECB(LTK, A2)` is fully replicable. **The crypto is not a real
  secret.**
- **Capture-verified update:** the console actually runs the `0x15` pairing (including the AES
  challenge) **over the USB link** during init — so there *is* crypto on the wire, but it's the
  replicable kind above (public key, mbedtls is in the Pico SDK). Confirmed too: commands ride
  the **vendor-bulk EP2** (`0x02`/`0x82`), input+rumble ride **HID EP1** (`0x81`/`0x01`), and USB
  responses use `transport=0x00` + ACK `00 f8`. Remaining unknown: whether wired input *requires*
  a correct AES reply (test on hardware; implement it to be safe). Exact bytes + the full observed
  handshake are in **[usb-spec.md](usb-spec.md) §4–5**.

---

## 3. Input report `0x09` (63 bytes) — the normal-operation report

| Off | Size | Field |
|---|---|---|
| 0x0 | 1 | 8-bit report counter (increments each report) |
| 0x1 | 1 | Power: [0]=source, [1]=charging, [2:5]=battery 0–9 |
| 0x2 | 3 | Buttons (bitmap below) |
| 0x5 | 3 | Left stick — packed 12-bit X/Y, uncalibrated |
| 0x8 | 3 | Right stick — packed 12-bit X/Y, uncalibrated |
| 0xB | 1 | `0x30` (or `0x38` if feature bit5 set) |
| 0xC | 1 | NFC state (`0x00` idle) |
| 0xD | 1 | Headset/audio state (`0x00` when none) |
| 0xE | 1 | Motion data length (observed {0,30,40}) |
| 0xF | 40 | Motion data (feature bit2; packed format not fully known) |
| 0x37 | 8 | Reserved |

### Button bitmap (3 bytes)
- **Byte0:** `80` RStick · `40` Plus · `20` ZR · `10` R · `08` X · `04` Y · `02` A · `01` B
- **Byte1:** `80` LStick · `40` Minus · `20` ZL · `10` L · `08` Up · `04` Left · `02` Right · `01` Down
- **Byte2:** `10` C · `08` GL (left grip) · `04` GR (right grip) · `02` Capture · `01` Home

New buttons vs Switch 1: **C, GL, GR** (grip buttons). bluepad32 does **not** expose these
(known project limitation) → they stay unmapped, which is fine.

## Output report `0x02` (42 bytes) — rumble
`00` report id · 16 B HD-rumble left LRA · 16 B HD-rumble right LRA · 9 B reserved.
(HD rumble is packed LRA data, richer than the Switch 1 rumble frames.)

---

## 4. Memory / calibration the console reads during init

Console issues `0x02/0x04` memory reads; we must answer with plausible blocks:

| Address | Size | Content |
|---|---|---|
| `0x13002` | 16 | Serial number |
| `0x13012` / `0x13014` | 2 / 2 | VID / PID |
| `0x13019`–`0x13022` | 3 each | Body / button / highlight / grip colours |
| `0x130A8` | 9 | Primary (left) stick factory calibration |
| `0x130E8` | 9 | Secondary (right) stick factory calibration |
| `0x1FC000` | 0x40 | Motion (IMU) calibration |
| `0x1FC040` / `0x1FC060` | 0xB each | User stick calibration |
| `0x1FA000` | — | BT pairing records (count + `count*0x28` entries) |

For emulation we synthesize a static factory block (neutral stick calibration, a made-up
serial/colours) and serve it from a table rather than real 2 MB SPI flash.

**Safe mode** (`ZR+PLUS+SYNC` on a real PC2 → enumerates as "Nintendo Safe Mode Device",
vendor-only USB) is a diagnostic/DFU channel, **not** a console-accepted controller mode —
not useful for us.

---

## 5. Feasibility verdict

**Emulating a wired-USB Switch 2 Pro Controller is plausible and probably does not require
defeating cryptography.** Reasons:
1. Identity is descriptor values (`PID 0x2069` + composite/IAD).
2. The USB handshake is a fixed, documented command sequence with **no challenge/response**.
3. Even the BLE crypto uses a **public** device key, so it's replicable if ever needed.
4. Input/rumble report formats are documented.
5. Our firmware **already** does USB Pro Controller (Switch 1) — same TinyUSB device model.

**Hard parts / risks:**
- Byte-exact descriptors + handshake responses (summaries are lossy — need raw captures).
- Composite descriptor with audio interfaces (does the console require them?).
- Answering the calibration memory reads convincingly.
- **Only testable on a real Switch 2**, and Claude can't flash/observe — slow iterate loop.
- 12-bit packed sticks + 3-byte button map + motion format are all new packing code.

---

## 6. Proposed roadmap

1. **Ground-truth capture** — pull ndeadly's raw `descriptors.md` hex + any USB pcap in
   `/captures`; produce an exact descriptor + handshake byte spec. *(no console needed)*
2. **Descriptor + enumeration** — new `usb_descriptors.c` variant (PID `0x2069`, IAD, HID +
   vendor + stub audio). Goal: Switch 2 enumerates it without rejecting. *(needs Switch 2)*
3. **Command handshake** — new `src/switch_pro2/` module answering `0x07`/`0x03`/`0x0C`/
   `0x10`/`0x02`. Goal: console completes init and shows a connected PC2. *(needs Switch 2)*
4. **Input reports** — map bluepad32 input → report `0x09` (12-bit sticks, 3-byte buttons).
   Goal: buttons/sticks work in-game. *(needs Switch 2)*
5. **IMU + rumble** — motion passthrough into the 40-byte motion block; HD-rumble output.
6. **Scope decision** — start with a **single** PC2 (a real PC2 is one device; 4× composite
   would be a huge descriptor). Multi-controller is a later question.

### Files this will touch (vs `main`)
- `src/usb_descriptors.c` — the big one (new composite descriptor, mode-switched).
- `src/switch_pro2/*` — new protocol module (parallel to `src/switch_pro/`).
- `src/usb.c` — drive the new command channel + report cadence.
- `src/pico_switch_platform.c` — map bluepad32 input into the new report format.
