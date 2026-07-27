# Nintendo Wii Remote / Wii MotionPlus Motion / IMU — Definitive Technical Reference

**Scope.** Everything required to obtain calibrated angular velocity and linear acceleration from a
Nintendo Wii Remote (RVL-CNT-01), a Wii Remote Plus (RVL-CNT-01-TR), an external Wii MotionPlus
(RVL-035), and the Nunchuk's secondary accelerometer — over Bluetooth Classic HID, with no console
present.

**Non-scope.** IR camera pointing (referenced only where it interacts with motion fusion), speaker
audio, Balance Board, and the console-side gesture/pointer libraries.

**Audience.** Someone implementing Wii motion passthrough in this repository, or a future LLM /
contributor who has no access to this conversation.

| | |
|---|---|
| **Document status** | ✅ Complete for accelerometer and MotionPlus acquisition; 🔵 Partial on absolute nominal scale constants (see §6.10) |
| **Date** | 2026-07-23 |
| **Repo implementation status** | 🔵 Phases 1–3 implemented 2026-07-27 (accelerometer decode, MotionPlus detection/activation/decode, calibration, rumble-latch fix). Build-clean and host-tested; **not yet hardware-validated** — see §12.5 |

### Confidence legend

Per [`../re-methodology/evidence-standards.md`](../re-methodology/evidence-standards.md):

- **Confirmed** — corroborated by at least two independent implementations that talk to real
  hardware, or directly verified in this repository.
- **Strong Evidence** — one authoritative implementation plus consistent secondary documentation.
- **Hypothesis** — plausible, single-source, or derived but unvalidated.
- **Unknown** — explicitly open.

Nothing in this document has yet been validated against hardware *by this project*. Every claim's
tier reflects the strength of the external evidence base. Tier promotion to "verified here" requires
a capture under [`../experiments/`](../experiments/).

---

## 0. Summary up front

If you read nothing else:

1. The Wii Remote has **two independent motion sources**, and they are acquired completely
   differently:
   - a **3-axis accelerometer built into the remote**, delivered inline in the standard input
     report;
   - a **3-axis gyroscope** that is *not* part of the remote at all — it lives behind the
     **extension port** as an I²C device and must be detected, activated, and decoded separately.
2. **Motion is off by default.** The remote sends report `0x30` (buttons only) until you send output
   report `0x12` to select a Data Reporting Mode that includes accelerometer and/or extension bytes.
3. **The accelerometer is 10-bit, but split.** Eight bits sit in a dedicated byte; the low 2 bits are
   smuggled into unused bits of the *button* bytes. Reading only the byte gives you an 8-bit sensor
   and a subtly wrong zero point.
4. **MotionPlus is a shape-shifter.** Before activation it answers at I²C address `0x53`
   (register base `0xA60000`). After you write a passthrough-mode byte to `0xA600FE`, it *becomes*
   the extension at address `0x52` (`0xA40000`) and the real extension (if any) hides behind it.
5. **MotionPlus has a dual-range sensor with per-axis, per-sample range selection.** Each of the
   three axes independently reports a "slow" bit. Slow and fast modes have scale factors that differ
   by roughly 4.5×. Ignoring these bits produces motion that is correct at low speed and wildly wrong
   at high speed — the classic symptom of a broken MotionPlus implementation.
6. **Use the per-unit calibration block at `0xA60020`.** The published *nominal* full-scale figures
   disagree across sources by 10–35% (§6.10). The 32-byte calibration block is the only authoritative
   scale for a given physical unit.
7. **Bit 0 of the first parameter byte of *every* output report is the rumble motor.** Sending an LED
   or report-mode command without preserving that bit silently stops rumble. This repository
   currently has that bug (§12.3).

---

## 1. The sensors

### 1.1 Accelerometer — Wii Remote

| Property | Value | Confidence |
|---|---|---|
| Part | Analog Devices **ADXL330** (early units); later revisions use a pin-compatible successor | Strong Evidence |
| Axes | 3 (X, Y, Z) | Confirmed |
| Physical range | ±3 g minimum full-scale per datasheet | Strong Evidence |
| Digitized resolution | **10 bits** on X; **9 bits** on Y and Z, presented as 10-bit with LSB forced to 0 | Confirmed |
| Zero-g raw value | ≈ `0x200` (512) in 10-bit space; ≈ `0x80` in the truncated 8-bit view | Confirmed |
| Location | Top of the PCB, slightly left of the A button | Strong Evidence |

The Y and Z channels genuinely carry one bit less precision than X. WiiBrew's guidance — and the
Linux kernel's implementation — is to treat all three as 10-bit and accept that Y/Z bit 0 is always
zero. Do not "improve" this by rescaling Y/Z to 9 bits; the zero point and the 1 g reference in the
calibration block are both expressed in the 10-bit space.

### 1.2 Gyroscope — Wii MotionPlus

| Property | Value | Confidence |
|---|---|---|
| Pitch + roll | InvenSense **IDG-600** dual-axis MEMS gyro (later units: IDG-650) | Strong Evidence |
| Yaw | Epson Toyocom single-axis MEMS gyro (X3500W class) | Strong Evidence |
| Axes | 3 (yaw, roll, pitch) | Confirmed |
| Digitized resolution | **14 bits** per axis | Confirmed |
| Zero-rate raw value | ≈ `8192` (2¹³), i.e. mid-scale | Confirmed |
| Range selection | **Dual-range, per-axis, per-sample**, signalled by "slow" bits | Confirmed |
| Nominal full scale | ~±2000 °/s fast, ~±440 °/s slow — **disputed, see §6.10** | 🔵 Partial |
| Bus | I²C behind the Wii Remote extension port | Confirmed |

### 1.3 Wii Remote Plus (RVL-CNT-01-TR)

The Wii Remote Plus integrates the MotionPlus silicon inside the remote body. **From the protocol's
point of view it is identical**: the integrated MotionPlus still presents itself as an extension-port
I²C device at `0xA60000`, still requires activation, and still exposes an extension pass-through port
for a physical Nunchuk. An implementation written against the external MotionPlus works unchanged.

The externally visible differences:

- Bluetooth device name is `Nintendo RVL-CNT-01-TR` rather than `Nintendo RVL-CNT-01`.
- Pairing uses a different (SSP-capable) path on some firmware revisions.
- The `IS_INTEGRATED` byte — byte 0 of the MotionPlus identifier — is `0x01` on an integrated unit
  and `0x00` on an external MotionPlus (Strong Evidence; from Dolphin's emulation, which sets `0x00`
  and comments that it is modelling a detachable unit).

> ⚠️ **Repo note:** the current `wiimote_match()` matches `"Nintendo RVL-CNT-01"` via `strstr` and
> only excludes the `-UC` (Wii U Pro) suffix. `Nintendo RVL-CNT-01-TR` therefore *already matches*
> this driver as a substring. That is correct behaviour, but it is accidental rather than intended,
> and it means Wii Remote Plus units are already reaching this code path today.

---

## 2. Transport and report framing

The Wii Remote is a **Bluetooth Classic HID** device. It predates BLE entirely; there is no GATT
path and no BLE HID variant.

| Direction | HID transaction header | Meaning |
|---|---|---|
| Host → Remote | `0xA2` | `DATA` / `OUTPUT` |
| Remote → Host | `0xA1` | `DATA` / `INPUT` |

Frames are sent on the HID **interrupt** channel (PSM 0x13). The control channel (PSM 0x11) works
for output reports on most stacks but is slower and is not required.

```
Host → Remote:   A2 <report_id> <payload...>
Remote → Host:   A1 <report_id> <payload...>
```

Whether the `0xA1` prefix is visible to your callback depends on your Bluetooth stack. In this
repository, `wiimote_process_report()` receives the buffer with `data[0] == report_id` — the
transaction byte has already been stripped — while transmit paths build the `0xA2` byte explicitly
(`wiimote_bt.c:175`, `:190`, `:198`, `:213`, `:228`, `:237`). **Preserve this asymmetry when adding
new code**; it is a real and easily-missed trap.

### 2.1 Output reports (host → remote)

| ID | Size | Function |
|---|---|---|
| `0x10` | 1 | Rumble (rumble bit only) |
| `0x11` | 1 | Player LEDs |
| `0x12` | 2 | **Set Data Reporting Mode** |
| `0x13` | 1 | IR Camera enable |
| `0x14` | 1 | Speaker enable |
| `0x15` | 1 | Status information request |
| `0x16` | 21 | **Write memory / registers** |
| `0x17` | 6 | **Read memory / registers** |
| `0x18` | 21 | Speaker data |
| `0x19` | 1 | Speaker mute |
| `0x1A` | 1 | IR Camera enable 2 |

Confidence: **Confirmed** (WiiBrew, corroborated by `hid-wiimote-core.c` request constants).

### 2.2 The rumble bit — a global side effect

> **Bit 0 (`0x01`) of the first parameter byte of every output report sets the rumble motor state.**

This is not a per-report flag; it is a latch that every output report rewrites. The Linux kernel
handles this by routing *every* transmit through `wiiproto_keep_rumble()`, which ORs in the cached
rumble state before queueing (`hid-wiimote-core.c:126-127`, and its call sites at `:146`, `:254`,
`:266`, `:290`, `:302`, `:332`, `:357`).

Any implementation that sends `0x11` (LEDs), `0x12` (report mode), `0x15` (status), `0x16`/`0x17`
(memory) with a literal `0x00` in that byte will **turn rumble off** as a side effect. See §12.3.

Confidence: **Confirmed** (WiiBrew explicit; kernel implementation exists solely to work around it).

### 2.3 Input reports (remote → host)

| ID | Function |
|---|---|
| `0x20` | Status information |
| `0x21` | Read memory/register response |
| `0x22` | Acknowledge output report / result |
| `0x30`–`0x3F` | Data Reporting Modes (see §3.1) |

---

## 3. Turning motion on

### 3.1 Data Reporting Modes

Output report `0x12` selects which sensors appear in the streaming input report:

```
A2 12 <flags> <mode>
```

- `<flags>` bit 2 (`0x04`) = **continuous reporting**. When clear, the remote sends a report only
  when the data changes. When set, it reports at the fixed interval regardless.
- `<flags>` bit 0 (`0x01`) = rumble (§2.2).
- `<mode>` = one of the report IDs below.

| Mode | Payload after the two button bytes | Total payload | Accel? | Ext bytes |
|---|---|---|---|---|
| `0x30` | — | 2 | ✗ | 0 |
| `0x31` | `AA AA AA` | 5 | ✅ | 0 |
| `0x32` | `EE × 8` | 10 | ✗ | 8 |
| `0x33` | `AA AA AA` + `II × 12` | 17 | ✅ | 0 |
| `0x34` | `EE × 19` | 21 | ✗ | 19 |
| `0x35` | `AA AA AA` + `EE × 16` | 21 | ✅ | **16** |
| `0x36` | `II × 10` + `EE × 9` | 21 | ✗ | 9 |
| `0x37` | `AA AA AA` + `II × 10` + `EE × 6` | 21 | ✅ | **6** |
| `0x3D` | `EE × 21` | 21 | ✗ | 21 (no buttons) |
| `0x3E`/`0x3F` | interleaved accel + 36 IR bytes | 21 each | ✅ (split) | 0 |

Confidence: **Confirmed** (WiiBrew; the subset `0x30`–`0x37` is already encoded in
`wiimote_bt.c:93-103`).

**Recommended modes for this project:**

| Goal | Mode | Rationale |
|---|---|---|
| Accelerometer only, no extension | `0x31` | Smallest frame that carries accel |
| Accelerometer + MotionPlus (no passthrough) | `0x35` | 6 M+ bytes fit in the 16 ext bytes |
| Accelerometer + MotionPlus + Nunchuk passthrough | `0x35` | Alternating frames, still 6 ext bytes used |
| Accelerometer + IR + extension | `0x37` | Only 6 ext bytes — exactly enough for M+ |

`0x35` is the right default and is **already what this repository selects** when an extension is
present (`wiimote_bt.c:227`).

### 3.2 Status report `0x20` — extension presence

```
A1 20 BB BB LF 00 00 VV
```

| Offset (from report ID) | Field |
|---|---|
| 1–2 | Core buttons |
| 3 | `LF` — LED state in the high nibble, flags in the low nibble |
| 4–5 | Reserved / zero |
| 6 | `VV` — battery level |

Low-nibble flags in byte 3:

| Bit | Mask | Meaning |
|---|---|---|
| 0 | `0x01` | Battery is low |
| 1 | `0x02` | **Extension controller is connected** |
| 2 | `0x04` | Speaker enabled |
| 3 | `0x08` | IR camera enabled |

Confidence: **Confirmed** (WiiBrew; implemented at `wiimote_bt.c:609-621`).

Two behaviours matter:

1. The remote sends an **unsolicited** `0x20` whenever an extension is inserted or removed. This is
   the hot-plug signal. The repo already handles it (`wiimote_bt.c:633-654`).
2. **Receiving a `0x20` resets the Data Reporting Mode to `0x30`.** After any status report —
   solicited or not — you must re-send `0x12`, or the stream silently degrades to buttons-only.

> ⚠️ **Repo note:** point 2 is *not* currently handled. `wiimote_task()` sends a keep-alive status
> request every 30 s (`wiimote_bt.c:891-896`) while in `WII_STATE_READY`, and the `0x20` handler does
> not re-arm the report mode unless the extension-connected flag changed. If the "status resets DRM"
> behaviour is real, accelerometer streaming stops 30 seconds after connect. **This is the single
> highest-value hardware test to run** (§14, Q1). Confidence on the reset behaviour itself:
> **Strong Evidence** (widely documented on WiiBrew and reproduced by multiple userspace libraries);
> confidence that it affects this repo: **Hypothesis** until measured.

---

## 4. Accelerometer data

### 4.1 Byte layout and the split LSBs

For any accel-bearing mode, the payload is:

```
A1 RR BB BB XX YY ZZ [...]
      │  │  │  │  └── Z<9:2>
      │  │  │  └───── Y<9:2>
      │  │  └──────── X<9:2>
      └──┴─────────── core buttons, and the accelerometer low bits
```

The two button bytes do double duty. Their layout:

| Bit | `BB[0]` (first button byte) | `BB[1]` (second button byte) |
|---|---|---|
| 0 | D-Pad Left | Two |
| 1 | D-Pad Right | One |
| 2 | D-Pad Down | B |
| 3 | D-Pad Up | A |
| 4 | Plus | Minus |
| 5 | **X\<0\>** | **Y\<1\>** |
| 6 | **X\<1\>** | **Z\<1\>** |
| 7 | (unused) | Home |

So:

- **X** gets both of its low bits, from `BB[0]` bits 5 and 6 → full 10-bit range.
- **Y** gets only bit 1, from `BB[1]` bit 5. Bit 0 is unavailable and reads as 0.
- **Z** gets only bit 1, from `BB[1]` bit 6. Bit 0 is unavailable and reads as 0.

Confidence: **Confirmed** — this is the exact behaviour implemented in
`hid-wiimote-modules.c:445-451`, and it matches WiiBrew.

### 4.2 Reference decode

Verbatim from the Linux kernel (`hid-wiimote-modules.c:445-455`), where `accel` points at the two
button bytes (i.e. `accel[0]` = `BB[0]`, `accel[2]` = `XX`):

```c
x = accel[2] << 2;
y = accel[3] << 2;
z = accel[4] << 2;

x |= (accel[0] >> 5) & 0x3;
y |= (accel[1] >> 4) & 0x2;
z |= (accel[1] >> 5) & 0x2;

/* reported as signed, centred on zero */
input_report_abs(wdata->accel, ABS_RX, x - 0x200);
input_report_abs(wdata->accel, ABS_RY, y - 0x200);
input_report_abs(wdata->accel, ABS_RZ, z - 0x200);
```

Note the masks: `& 0x3` for X (two bits) but `& 0x2` for Y and Z (one bit, already in position 1).
This is the compact expression of "Y and Z have 9 bits presented as 10".

Translated to this repository's indexing, where `data[0]` is the report ID and `data[1..2]` are the
button bytes:

```c
/* modes 0x31 / 0x33 / 0x35 / 0x37: accel bytes begin at data[3] */
uint16_t ax = ((uint16_t)data[3] << 2) | ((data[1] >> 5) & 0x03);
uint16_t ay = ((uint16_t)data[4] << 2) | ((data[2] >> 4) & 0x02);
uint16_t az = ((uint16_t)data[5] << 2) | ((data[2] >> 5) & 0x02);
```

> ⚠️ **Repo note:** `wiimote_process_report()` masks the button bytes with `0x1F` and `0x9F`
> (`wiimote_bt.c:365`) *before* using them. That correctly discards the accel bits for button
> purposes — but any future accel decode must read `data[1]`/`data[2]` **raw**, before that mask is
> applied, or bits 5 and 6 are already gone.

### 4.3 Factory calibration (EEPROM)

The Wii Remote stores accelerometer calibration in its own EEPROM — **not** in a register — at
offset `0x0016`, duplicated at `0x0020` for redundancy. Read it with output report `0x17` using
address-space selector `0x00` (EEPROM), *not* `0x04`.

| Offset (block 1 / block 2) | Content |
|---|---|
| `0x16` / `0x20` | 0 g X \<9:2\> |
| `0x17` / `0x21` | 0 g Y \<9:2\> |
| `0x18` / `0x22` | 0 g Z \<9:2\> |
| `0x19` / `0x23` | bits 1:0 = 0 g X\<1:0\>; bits 3:2 = 0 g Y\<1:0\>; bits 5:4 = 0 g Z\<1:0\> |
| `0x1A` / `0x24` | 1 g X \<9:2\> |
| `0x1B` / `0x25` | 1 g Y \<9:2\> |
| `0x1C` / `0x26` | 1 g Z \<9:2\> |
| `0x1D` / `0x27` | bits 1:0 = 1 g X\<1:0\>; bits 3:2 = 1 g Y\<1:0\>; bits 5:4 = 1 g Z\<1:0\> |
| `0x1E` / `0x28` | Motor value (high nibble), speaker volume (low nibble) |
| `0x1F` / `0x29` | Checksum |

Confidence: **Confirmed** (WiiBrew, long-standing and corroborated by every userspace library).

Note that this is a **two-point calibration**: it gives you the raw reading at 0 g and the raw
reading at +1 g, per axis. It does not give a "sensitivity" in the Switch 1 sense.

### 4.4 Conversion to g

```c
/* zero[i] and one_g[i] are the assembled 10-bit values from §4.3 */
float accel_g[i] = (float)(raw[i] - zero[i]) / (float)(one_g[i] - zero[i]);
```

Uncalibrated fallback, when the EEPROM read fails or is not implemented:

```c
/* COUNTS_PER_G is unit-dependent — measure it, do not trust this default. */
#define WII_ACCEL_ZERO       512
#define WII_ACCEL_COUNTS_PER_G  102.0f
float accel_g[i] = (float)(raw[i] - WII_ACCEL_ZERO) / WII_ACCEL_COUNTS_PER_G;
```

The counts-per-g figure is **not** derivable from the ADXL330's ±3 g rating, because the digitizer's
input window is wider than the sensor's full scale — the 10-bit range does not map onto ±3 g. Widely
reported observed values cluster near 100–110 counts per g, but this varies per unit, which is
exactly what the two-point EEPROM block exists to capture.

Confidence on the uncalibrated constant: **Hypothesis** — see §14, Q2, which measures it directly as
a by-product of the six-position test. Confidence on the calibrated form: **Confirmed**.

### 4.5 Coordinate system

With the remote held **pointing at the screen, buttons facing up**:

| Axis | Direction | At rest, buttons up |
|---|---|---|
| **X** | Lateral — across the width of the remote (left/right) | ≈ 0 g |
| **Y** | Longitudinal — along the pointing axis (forward/back) | ≈ 0 g |
| **Z** | Normal to the button face (up/down) | ≈ **+1 g** |

Confidence: **Confirmed for the axis assignment.** This repository independently corroborates it:
`wiimote_detect_orientation()` (`wiimote_bt.c:244-264`) uses X-axis deviation from centre as the
sideways/upright discriminator, with the comment that X stays near centre when pointing at the
screen and deviates when held NES-style. That is only true if X is the lateral axis.

Confidence on the **sign** of each axis: **Strong Evidence**. Absolute polarity should be pinned by
the six-position test in §14, Q2 before being relied on for gesture work.

### 4.6 Interleaved modes `0x3E` / `0x3F`

Modes `0x3E` and `0x3F` split the accelerometer across two alternating reports to make room for 36
bytes of IR data:

```
A1 3e BB BB XX <36 IR bytes across the pair>
A1 3f BB BB YY <...>
```

The Z value is scattered across the button bits of both reports:

| Report | Byte | Bits carried |
|---|---|---|
| `0x3E` | `BB[0]` bits 5:6 | Z\<5:4\> |
| `0x3E` | `BB[1]` bits 5:6 | Z\<7:6\> |
| `0x3F` | `BB[0]` bits 5:6 | Z\<1:0\> |
| `0x3F` | `BB[1]` bits 5:6 | Z\<3:2\> |

Confidence: **Strong Evidence** (WiiBrew only; not implemented by the Linux kernel).

**Recommendation: do not implement these modes.** They halve the effective accelerometer rate,
require pairing state across two frames, and exist only to serve full-resolution IR — which is out
of scope. Mode `0x37` gives accel + IR + 6 extension bytes without any of this.

---

## 5. The extension port

Everything gyroscopic happens here, so the bus model has to be understood first.

### 5.1 Bus model

The extension port is an **I²C bus** exposed through the remote's memory-mapped register window.
Two device addresses matter:

| I²C address | Register window | Occupant |
|---|---|---|
| `0x52` | `0xA40000`–`0xA400FF` | The **active** extension |
| `0x53` | `0xA60000`–`0xA600FF` | The **inactive** MotionPlus |

The third byte of the register address encodes the I²C address (`0xA4` → `0x52`, `0xA6` → `0x53`);
the low byte is the register offset within that device's 256-byte space.

Confidence: **Confirmed** (Dolphin `MotionPlus.h:116-117` names these constants explicitly:
`INACTIVE_DEVICE_ADDR = 0x53`, `ACTIVE_DEVICE_ADDR = 0x52`).

### 5.2 Read and write

**Write — output report `0x16`:**

```
A2 16 MM FF FF FF SS DD DD ... (16 data bytes, zero-padded)
```

**Read — output report `0x17`:**

```
A2 17 MM FF FF FF SS SS
```

| Field | Meaning |
|---|---|
| `MM` | Address-space selector **and rumble bit**. `0x00` = EEPROM, `0x04` = control registers. Bit 0 = rumble. |
| `FF FF FF` | 24-bit big-endian offset |
| `SS` (write) | Byte count, max 16 |
| `SS SS` (read) | 16-bit big-endian byte count |

> ⚠️ The WiiBrew page's phrasing of `MM` is easy to misread as "bit 2 = EEPROM, bit 3 = registers".
> It is not. The kernel is unambiguous — `wiiproto_req_wmem()` and `wiiproto_req_rmem()` both do
> `if (!eeprom) cmd[1] |= 0x04;` (`hid-wiimote-core.c:329-330`, `:354-355`). **`0x04` means
> registers; `0x00` means EEPROM.** This repository's hardcoded `buf[2] = 0x04` for `0xA4xxxx`
> accesses (`wiimote_bt.c:200`, `:212`) is therefore **correct**.

**Read response — input report `0x21`:**

```
A1 21 BB BB SE FF FF DD × 16
```

| Field | Meaning |
|---|---|
| `S` (high nibble of `SE`) | Bytes returned, minus one |
| `E` (low nibble of `SE`) | Error: `0` = OK, `7` = write-only / no extension, `8` = bad address |
| `FF FF` | Low 16 bits of the source address |
| `DD` | Up to 16 bytes, zero-padded |

**Acknowledgement — input report `0x22`:**

```
A1 22 BB BB RR EE
```

`RR` is the output report being acknowledged; `EE` is `0x00` on success, `0x07` for "no extension".

Confidence: **Confirmed** (WiiBrew + kernel + this repo's working implementation at
`wiimote_bt.c:658-737`).

### 5.3 Unencrypted initialisation

Extensions power up expecting an encrypted handshake. The universally-used modern bypass is to write
two magic bytes, which puts the extension into plaintext mode:

```
write 0x55 → 0xA400F0     /* initialise */
write 0x00 → 0xA400FB     /* disable default encryption */
```

Confidence: **Confirmed** — `hid-wiimote-core.c:417-424` does exactly this, and this repository
already does it (`wiimote_bt.c:776`, `:790`).

After this, read the 6-byte identifier at `0xA400FA`.

### 5.4 Extension identifiers

Read 6 bytes from `0xA400FA`. Bytes 2–3 are always `A4 20` for a plaintext-initialised extension;
bytes 4–5 are the type, and byte 0 sub-classifies.

| ID bytes `[0] [1] [4] [5]` | Extension |
|---|---|
| `00 00 · 00 00` | Nunchuk |
| `00 00 · 01 01` | Classic Controller |
| `01 00 · 01 01` | Classic Controller Pro |
| `02 00 · 01 01` | NES Classic Controller |
| `03 00 · 01 01` | SNES Classic Controller |
| `00 00 · 01 03` | Guitar Hero guitar |
| `01 00 · 01 03` | Guitar Hero drums |
| `03 00 · 01 03` | DJ Hero turntable |
| `00 00 · 04 02` | Balance Board |
| `00 00 · 01 20` | Wii U Pro Controller |
| `·· ·· · 04 05` | **MotionPlus, active, no passthrough** |
| `·· ·· · 05 05` | **MotionPlus, active, Nunchuk passthrough** |
| `·· ·· · 07 05` | **MotionPlus, active, Classic passthrough** |
| `FF FF FF FF FF FF` | Nothing connected / read failed |

Confidence: **Confirmed** (`hid-wiimote-core.c:443-465` and `:546-557`; the non-MotionPlus rows are
independently implemented in `wiimote_bt.c:703-731`).

---

## 6. MotionPlus

### 6.1 Detection

An **inactive** MotionPlus lives at `0xA60000`. Detection is a read of its identifier:

```
read 6 bytes ← 0xA600FA
expect:  <II> 00 A6 20 00 05
```

where `<II>` is `0x00` for a detachable MotionPlus and `0x01` for one integrated into a Wii Remote
Plus.

The kernel's acceptance test is deliberately loose — it only checks the final byte:

```c
/* hid-wiimote-core.c:525-526 */
if (rmem[5] == 0x05)
    return true;
```

Confidence: **Confirmed**.

**Important ordering constraint.** MotionPlus detection must be attempted *before* concluding "no
extension", and it does **not** require the `0xA400F0`/`0xA400FB` initialisation — MotionPlus has its
own at `0xA600F0`/`0xA600FB`:

```c
/* hid-wiimote-core.c:474-486 */
write 0x55 → 0xA600F0
write 0x00 → 0xA600FB
```

### 6.2 Activation

Write a single byte to `0xA600FE`. The value selects the passthrough mode *and* performs the
activation:

| Value | Mode | Use when |
|---|---|---|
| `0x04` | MotionPlus only, no passthrough | Nothing else is plugged into the M+ |
| `0x05` | Nunchuk passthrough | A Nunchuk (or DJ Hero turntable) is attached |
| `0x07` | Classic passthrough | Classic Controller / guitar / drums attached |

Verbatim from the kernel (`hid-wiimote-core.c:490-511`):

```c
switch (exttype) {
case WIIMOTE_EXT_CLASSIC_CONTROLLER:
case WIIMOTE_EXT_DRUMS:
case WIIMOTE_EXT_GUITAR:
        wmem = 0x07;
        break;
case WIIMOTE_EXT_TURNTABLE:
case WIIMOTE_EXT_NUNCHUK:
        wmem = 0x05;
        break;
default:
        wmem = 0x04;
        break;
}
return wiimote_cmd_write(wdata, 0xa600fe, &wmem, sizeof(wmem));
```

Confidence: **Confirmed**.

On activation the MotionPlus **moves**. It stops responding at `0x53`/`0xA60000` and takes over
`0x52`/`0xA40000`, becoming the extension as far as the remote is concerned. Dolphin models this
exactly (`MotionPlus.cpp:204-213`: *"Motion plus does not respond to 0x53 when activated… No i2c
passthrough when activated"*).

Verify by re-reading `0xA400FA`, which should now return `·· ·· A4 20 04 05` (or `05 05` / `07 05`).

There is a settling period. Dolphin models `Activating` and `Deactivating` states during which the
**extension port is completely unresponsive** (`MotionPlus.cpp:215-220`). Poll the identifier with
retries rather than assuming the write took effect immediately. Recommended: 5 attempts at 50 ms
intervals. Confidence on the exact settling time: **Unknown** — see §14, Q3.

### 6.3 Deactivation

Write `0x55` to `0xA400F0`, then `0x00` to `0xA400FB` — i.e. the ordinary extension initialisation
sequence, sent to the *active* MotionPlus. Dolphin explains why this works
(`MotionPlus.cpp:262-268`): *"It seems a write of any value here triggers deactivation on a real M+.
The M+ deactivation signal is cleverly the same as EXT initialization."*

This is an important interaction: **the standard extension-init sequence deactivates a
MotionPlus.** If your init state machine unconditionally writes `0xA400F0 = 0x55` after activating
MotionPlus, you will immediately turn it back off. Order matters.

Confidence: **Confirmed**.

### 6.4 The 6-byte MotionPlus data frame

MotionPlus data appears in the extension bytes of the input report — offset 6 in mode `0x35`,
offset 3 in mode `0x32`, offset 16 in mode `0x37`.

```
      bit:  7    6    5    4    3    2  │  1   │  0
    ┌─────────────────────────────────────────────────┐
 [0]│              Yaw Speed <7:0>                    │
 [1]│              Roll Speed <7:0>                   │
 [2]│              Pitch Speed <7:0>                  │
    ├──────────────────────────────────┼──────┼───────┤
 [3]│        Yaw Speed <13:8>          │ Yaw  │ Pitch │   ← slow bits
    ├──────────────────────────────────┼──────┼───────┤
 [4]│        Roll Speed <13:8>         │ Roll │  Ext  │   ← slow bit, ext-connected
    ├──────────────────────────────────┼──────┼───────┤
 [5]│        Pitch Speed <13:8>        │  1   │   0   │   ← M+ data marker
    └─────────────────────────────────────────────────┘
```

| Bit | Location | Meaning |
|---|---|---|
| Pitch slow | `[3]` bit 0 | 1 = slow range, 0 = fast range |
| Yaw slow | `[3]` bit 1 | 1 = slow range, 0 = fast range |
| Ext connected | `[4]` bit 0 | 1 = an extension is plugged into the MotionPlus |
| Roll slow | `[4]` bit 1 | 1 = slow range, 0 = fast range |
| `zero` | `[5]` bit 0 | Always 0 |
| `is_mp_data` | `[5]` bit 1 | **1 = this frame is MotionPlus data**, 0 = passthrough extension data |

Confidence: **Confirmed** — two fully independent implementations agree bit-for-bit. The kernel's
ASCII table and parser (`hid-wiimote-modules.c:2716-2763`) and Dolphin's packed bitfield struct
(`MotionPlus.h:97-113`) are structurally identical:

```cpp
u8 yaw1;  u8 roll1;  u8 pitch1;
u8 pitch_slow : 1;          u8 yaw_slow : 1;   u8 yaw2   : 6;
u8 extension_connected : 1; u8 roll_slow : 1;  u8 roll2  : 6;
u8 zero : 1;                u8 is_mp_data : 1; u8 pitch2 : 6;
```

Note the **non-obvious cross-byte placement**: the *pitch* slow bit lives in byte `[3]` alongside
*yaw*'s high bits, not in byte `[5]` with pitch's own high bits. Getting this wrong produces an
implementation where two axes behave and one goes haywire only at speed.

### 6.5 Reference decode

Verbatim from `hid-wiimote-modules.c:2740-2763`, with `x` = yaw, `y` = roll, `z` = pitch:

```c
x = ext[0];
y = ext[1];
z = ext[2];

x |= (((__u16)ext[3]) << 6) & 0xff00;
y |= (((__u16)ext[4]) << 6) & 0xff00;
z |= (((__u16)ext[5]) << 6) & 0xff00;

x -= 8192;
y -= 8192;
z -= 8192;

if (!(ext[3] & 0x02))            /* yaw fast   */
        x = (x * 2000 * 9) / 440;
else
        x *= 9;
if (!(ext[4] & 0x02))            /* roll fast  */
        y = (y * 2000 * 9) / 440;
else
        y *= 9;
if (!(ext[3] & 0x01))            /* pitch fast */
        z = (z * 2000 * 9) / 440;
else
        z *= 9;
```

The `<< 6` combined with `& 0xff00` is a compact way of writing `(ext[n] >> 2) << 8` — it drops the
two flag bits and places the remaining 6 bits at positions 13:8.

The `* 9` on both branches is a kernel-specific fixed-point scale chosen for input-subsystem
compatibility, not a protocol constant. Ignore it; the meaningful part is the `2000/440` ratio
between fast and slow.

### 6.6 The slow/fast dual range — the crux

This is the single most important behaviour in the MotionPlus protocol and the one most often got
wrong.

- Each axis independently reports whether *that axis, in this sample* used the slow or fast range.
- The bit is `1` for **slow** (high sensitivity, small range) and `0` for **fast** (low sensitivity,
  large range).
- The two ranges differ in scale by roughly **4.5×**.

The kernel's own comment states the rule (`hid-wiimote-modules.c:2729-2735`):

> *"The single bits Yaw, Roll, Pitch in the lower right corner specify whether the wiimote is
> rotating fast (0) or slow (1). Speed for slow rotation is 8192/440 units / deg/s and for fast
> rotation 8192/2000 units / deg/s. To get a linear scale for fast rotation we multiply by
> 2000/440 = ~4.5454 … If the wiimote is not rotating the sensor reports 2^13 = 8192."*

The hardware switches ranges autonomously as you move. An implementation that ignores the bits will
look perfectly calibrated on a slow desk test and then under-report by ~4.5× the instant the user
makes a real gesture. **There is no way to lock the range**; you must honour the bits per sample.

Confidence: **Confirmed**.

### 6.7 Calibration block at `0xA60020`

32 bytes, read from the **inactive** MotionPlus register space. Dolphin's struct
(`MotionPlus.h:29-70`), with a compile-time assertion that the total is exactly `0x20` bytes:

```cpp
struct CalibrationBlock          // 13 bytes
{
  BigEndianValue<u16> yaw_zero;
  BigEndianValue<u16> roll_zero;
  BigEndianValue<u16> pitch_zero;
  BigEndianValue<u16> yaw_scale;
  BigEndianValue<u16> roll_scale;
  BigEndianValue<u16> pitch_scale;
  u8 degrees_div_6;
};

struct CalibrationData           // 32 bytes total
{
  CalibrationBlock    fast;      // 0x00 .. 0x0C
  u8                  uid_1;     // 0x0D
  BigEndianValue<u16> crc32_msb; // 0x0E .. 0x0F
  CalibrationBlock    slow;      // 0x10 .. 0x1C
  u8                  uid_2;     // 0x1D
  BigEndianValue<u16> crc32_lsb; // 0x1E .. 0x1F
};
static_assert(sizeof(CalibrationData) == 0x20, "Wrong size");
```

Absolute addresses:

| Address | Field |
|---|---|
| `0xA60020` | fast block begins |
| `0xA6002D` | `uid_1` |
| `0xA6002E`–`0xA6002F` | CRC32 high half |
| `0xA60030` | slow block begins |
| `0xA6003D` | `uid_2` |
| `0xA6003E`–`0xA6003F` | CRC32 low half |

All 16-bit values are **big-endian**. The `degrees_div_6` field means what it says: the angular
velocity that the `*_scale` value represents, in deg/s, divided by 6. A `degrees_div_6` of 45 means
the scale point corresponds to 270 deg/s.

**Checksum:** CRC32 over the two 14-byte spans that exclude the checksum fields themselves —
offsets `0x00`–`0x0D` and `0x10`–`0x1D` (`MotionPlus.cpp:145-154`). The result is split with the
high half stored at `0x0E` and the low half at `0x1E`. Use it to reject a corrupt read rather than
silently calibrating from garbage.

Confidence: **Confirmed** for the layout. **Strong Evidence** for the CRC span, which is
single-source (Dolphin) though it is emulation code written to satisfy real games.

> ⚠️ Read this block **before activating** MotionPlus, while it still answers at `0xA6xxxx`.

### 6.8 Calibrated conversion

The calibration is **two-point**: a `zero` and a `scale` per axis, expressed in a **16-bit** space,
while the data frame is **14-bit**. Dolphin declares this explicitly —
`TwoPointCalibration<GyroType, 16>` for the calibration versus `RawValue<GyroType, 14>` for the data
(`MotionPlus.h:47`, `:76`).

Bring them into a common space by shifting the calibration values right by 2:

```c
/* per axis, choosing the block indicated by that axis's slow bit */
int32_t zero14  = (int32_t)cal.zero  >> 2;
int32_t scale14 = (int32_t)cal.scale >> 2;
int32_t degrees = cal.degrees_div_6 * 6;

float dps = (float)(raw14 - zero14) * (float)degrees / (float)(scale14 - zero14);
```

Three details that are easy to miss:

1. **`scale` may be numerically *less* than `zero`.** Dolphin's synthetic calibration sets
   `YAW_SCALE = CALIBRATION_ZERO - 0x4400` and `PITCH_SCALE = CALIBRATION_ZERO - 0x4400`, but
   `ROLL_SCALE = CALIBRATION_ZERO + 0x4400` (`MotionPlus.cpp:113-115`). The **sign of
   `(scale - zero)` encodes the axis polarity** and the division above handles it automatically.
   Do not take an absolute value.

2. **The MotionPlus does not follow the right-hand rule.** After the division above, Dolphin applies
   `sign_fix = Vec3(-1, +1, -1)` on `(pitch, roll, yaw)` to obtain right-handed angular velocity
   (`MotionPlus.cpp:61-68`). Whether you need this depends on the convention of your consumer.

3. **The slow and fast blocks are selected per axis, independently.** From
   `MotionPlus.cpp:76-86`:

   ```cpp
   const auto& pitch_block = is_slow.x ? slow : fast;
   const auto& roll_block  = is_slow.y ? slow : fast;
   const auto& yaw_block   = is_slow.z ? slow : fast;
   ```

   You may legitimately be using the fast block for yaw and the slow block for pitch within the same
   6-byte frame.

Confidence: **Confirmed** for the mechanism; **Strong Evidence** for the exact 14↔16-bit
reconciliation, which is derived from Dolphin's type parameters rather than quoted from it.

### 6.9 Uncalibrated fallback

If the calibration block is unreadable:

```c
int32_t v = raw14 - 8192;
float dps = is_slow ? (v / 18.6f)      /* 8192 counts / 440 dps  */
                    : (v / 4.096f);    /* 8192 counts / 2000 dps */
```

Confidence: **Strong Evidence** — this is the kernel's model, but see the next section.

### 6.10 🔵 The nominal-scale disagreement

Three credible sources give three different answers for the MotionPlus full-scale range. This is a
genuine open discrepancy and is documented here rather than silently resolved.

| Source | Slow full scale | Fast full scale | Ratio | Basis |
|---|---|---|---|---|
| Linux `hid-wiimote` | **440 °/s** | **2000 °/s** | 4.545 | Driver constant, uncited |
| Dolphin (typical real unit) | **270 °/s** at scale point → ≈508 °/s full scale | **1200 °/s** at scale point → ≈2258 °/s full scale | 4.444 | Measured from author's physical MotionPlus |
| WiiBrew (datasheet math) | **595 °/s** | — | — | 1.35 V reference ÷ 2.27 mV/°/s |

Dolphin's constants are `CALIBRATION_SLOW_SCALE_DEGREES = 0x10E` (270) and
`CALIBRATION_FAST_SCALE_DEGREES = 0x4B0` (1200), against a `CALIBRATION_SCALE_OFFSET = 0x4400`
(17408 in 16-bit space = 4352 counts in 14-bit space) — with the source comment *"Values are similar
to that of a typical real M+"* (`MotionPlus.h:123-126`).

**What to conclude:**

- The *ratio* between fast and slow is consistent at ≈4.5× across all sources. **Confirmed.**
- The *absolute* scale differs by up to 35% depending on source. **Unknown** which is right in
  general — and the likely answer is that it varies per unit, which is precisely why Nintendo shipped
  a per-unit calibration block.
- **Therefore: always read `0xA60020`.** Treat §6.9 as a degraded fallback that will be off by tens
  of percent, acceptable for detecting motion but not for integrating orientation.

### 6.11 Coordinate system

| Frame field | Rotation about | Positive sense (before `sign_fix`) |
|---|---|---|
| **Yaw** | The vertical axis (Z) | "Yaw Down Speed" per WiiBrew naming |
| **Roll** | The longitudinal / pointing axis (Y) | "Roll Left Speed" |
| **Pitch** | The lateral axis (X) | "Pitch Left Speed" |

WiiBrew's field names ("Yaw Down", "Roll Left", "Pitch Left") are the raw sensor's own naming and are
**not** a right-handed convention — which is exactly what Dolphin's `sign_fix = (-1, +1, -1)`
compensates for. Confidence: **Strong Evidence**; the definitive sign convention should be pinned
empirically (§14, Q2).

Note the axis pairing between the two sensors: gyro **pitch** rotates about the accelerometer's
**X** axis, gyro **roll** about **Y**, gyro **yaw** about **Z**. This correspondence is what makes
complementary filtering possible (§11).

---

## 7. Passthrough modes

When MotionPlus is activated with `0x05` or `0x07`, both it and the downstream extension must share
one 6-byte window. They do so by **alternating frames**.

### 7.1 Telling the two apart

**Bit 1 of byte `[5]`** (`is_mp_data`): `1` = MotionPlus frame, `0` = passthrough extension frame.

This is the only discriminator. Check it on every frame; do not assume strict alternation, because a
failed extension read causes the MotionPlus to emit its own data instead (`MotionPlus.cpp:605-609`).

Confidence: **Confirmed**.

### 7.2 What the MotionPlus does to the passed-through data

The MotionPlus needs bits `[5]:1` and `[5]:0` for its own bookkeeping, and `[4]:0` for the
extension-connected flag. It steals them from the extension's payload, relocating the displaced bits
and dropping the least significant bit of some fields.

**Nunchuk passthrough (`0x05`)** — verbatim from `MotionPlus.cpp:651-668`, annotated as *"via
wiibrew.org / Verified on real hardware via a test of every bit"*:

```cpp
// Data passing through drops the least significant bit of the three accelerometer values.
SetBit<6>(data[5], ExtractBit<7>(data[5]));  // bit 7 of byte 5 → bit 6 of byte 5
SetBit<7>(data[5], ExtractBit<0>(data[4]));  // bit 0 of byte 4 → bit 7 of byte 5
SetBit<4>(data[5], ExtractBit<3>(data[5]));  // bit 3 of byte 5 → bit 4 of byte 5
SetBit<3>(data[5], ExtractBit<1>(data[5]));  // bit 1 of byte 5 → bit 3 of byte 5
SetBit<2>(data[5], ExtractBit<0>(data[5]));  // bit 0 of byte 5 → bit 2 of byte 5
```

The inverse, which is what a *reader* actually needs (`MotionPlus.cpp:681-696`):

```cpp
SetBit<0>(data[5], ExtractBit<2>(data[5]));
SetBit<1>(data[5], ExtractBit<3>(data[5]));
SetBit<3>(data[5], ExtractBit<4>(data[5]));
SetBit<0>(data[4], ExtractBit<7>(data[5]));
SetBit<7>(data[5], ExtractBit<6>(data[5]));
// Restore the destroyed LSBs from the next-least-significant bit:
SetBit<2>(data[5], ExtractBit<3>(data[5]));
SetBit<4>(data[5], ExtractBit<5>(data[5]));
SetBit<6>(data[5], ExtractBit<7>(data[5]));
```

Net effect for a consumer: the Nunchuk's **C and Z buttons are unaffected**, its **joystick is
unaffected**, and its **accelerometer loses its least significant bit** (10-bit → 9-bit).

**Classic passthrough (`0x07`)** — `MotionPlus.cpp:669-678`:

```cpp
// Drops the LSB of the axes of the left (or only) joystick.
// Bit 0 of byte 4 is overwritten by the 'extension_connected' flag.
SetBit<0>(data[0], ExtractBit<0>(data[5]));
SetBit<0>(data[1], ExtractBit<1>(data[5]));
```

Inverse (`MotionPlus.cpp:697-710`):

```cpp
SetBit<0>(data[5], ExtractBit<0>(data[0]));
SetBit<1>(data[5], ExtractBit<0>(data[1]));
SetBit<0>(data[0], ExtractBit<1>(data[0]));
SetBit<0>(data[1], ExtractBit<1>(data[1]));
SetBit<0>(data[4], true);   // an unused Classic button bit
```

Confidence: **Confirmed** — Dolphin's comment asserts per-bit hardware verification, and the
transformation is self-consistent with the frame layout in §6.4.

**DJ Hero turntable caveat:** the bit that Classic passthrough overwrites at `data[4]:0` is unused on
a Classic Controller but *significant* on the DJ Hero turntable. Dolphin notes *"passthrough not
feasible"* for that device, which is why the kernel routes the turntable through **Nunchuk**
passthrough (`0x05`) instead (`hid-wiimote-core.c:501-503`). If you ever support the turntable, copy
that choice.

### 7.2.1 Implementation status (2026-07-27)

✅ Implemented. `wii_mp_passthrough_restore()` in `src/bt_hid/motion/wii_motionplus.c` applies the
inverse sequence above; `wiimote_bt.c` calls it on every frame where
`wii_mp_is_motionplus_frame()` is false, then routes the frame to the ordinary Nunchuk or Classic
decoder.

Three things this fixed, all of which only appear once a MotionPlus is active:

1. **The passthrough extension was entirely dead.** The decoders were selected by the port's
   `ext_type`, which in passthrough is `WII_EXT_MOTIONPLUS_NUNCHUK`, not `WII_EXT_NUNCHUK` — so no
   branch matched and a Nunchuk's stick and C/Z buttons did nothing. Frame type and port type are
   now distinguished (`decode_type` vs `ext_type`).
2. **The bit relocation was never undone**, so even once routed, byte 5 was wrong.
3. **Extension buttons dropped out on every MotionPlus frame.** Because the two sources alternate
   (§7.3), a naive decoder reports C/Z as released half the time. Extension buttons are now latched
   in `ext_buttons_held` and re-asserted on MotionPlus frames. Analog axes needed no latch — they
   live in `event.analog[]`, which already persists between reports.

Host tests in `tools/test_wii_motionplus.c` check the restore against a forward-transform fixture:
Nunchuk stick X/Y and C/Z survive a round trip, the Classic left-stick LSBs land back in byte 5,
and `WII_MP_PASSTHROUGH_NONE` is a no-op. Accelerometer LSBs are deliberately not checked for
equality — the MotionPlus destroys them and they are refilled from the next bit up, as Dolphin does.

Not hardware validated.

### 7.2.2 Diagnosing "tilt works, turning does not" (2026-07-27)

Reported on hardware: slow up/down response, **no left/right turning at all**.

That symptom is diagnostic, and it is not an axis-mapping bug. **Gravity carries no yaw
information** — rotating about the gravity vector does not change the measured vector — so an
accelerometer alone can produce pitch and roll but never yaw. Slow tilt with dead horizontal
turning is precisely what the console produces from accel with the gyro at zero.

The axis mapping was checked against §4.5 and §6.8 and is structurally correct: Wii X/Y/Z are
lateral / longitudinal / face-normal, the DualSense frame is lateral / face-normal / longitudinal,
the remount `accel = [wiiX, wiiZ, wiiY]` matches type for type, and Dolphin's `sign_fix (-1, +1,
-1)` on (pitch, roll, yaw) is applied. So look at whether the gyro is live before touching signs.

**Check the UART log first.** `[WIIMOTE] MotionPlus present (integrated=N)` then `[WIIMOTE]
MotionPlus active (mode XX)` must both appear. If the second is missing, activation never
completed and the driver is publishing `has_motion = true` with `gyro = {0,0,0}` — accel-only
motion, exactly the reported symptom. Mode `0x04` = MotionPlus only, `0x05` = Nunchuk passthrough,
`0x07` = Classic passthrough.

**Fixed alongside this:** `mp_have_sample` was set once and never cleared, so a MotionPlus that
stopped answering (unplugged, activation lost, reconnect) left its final rate republished forever.
Because the console integrates rate, a stuck non-zero sample spins the camera without end. There
is now a 250 ms staleness gate (generous against the ~50 Hz per-source passthrough rate of §7.3)
that clears the gyro and logs `MotionPlus samples stopped`, plus a full reset of the motion state
on connect so nothing survives a reconnect.

### 7.3 Effective rates

In a passthrough mode, MotionPlus and extension frames alternate, so each source arrives at **half**
the report rate. At the nominal ~100 Hz report rate that is ~50 Hz per source. Budget for this: a
50 Hz gyro is adequate for orientation tracking but noticeably coarse for fast gesture recognition.

Confidence: **Strong Evidence** (the alternation is Confirmed; the resulting rate follows from the
report rate in §10, which is itself Strong Evidence).

---

## 8. The Nunchuk's accelerometer

The Nunchuk contains a **second, independent 3-axis accelerometer**. It is frequently overlooked and
is free data once the extension is already being parsed.

6-byte Nunchuk frame:

| Byte | Content |
|---|---|
| 0 | Joystick X (8-bit, centre ≈ 128) |
| 1 | Joystick Y (8-bit, centre ≈ 128) |
| 2 | Accel X \<9:2\> |
| 3 | Accel Y \<9:2\> |
| 4 | Accel Z \<9:2\> |
| 5 | `AZ<1:0>` `AY<1:0>` `AX<1:0>` `C` `Z` |

Byte 5 in detail:

| Bits | Field |
|---|---|
| 7:6 | Accel Z \<1:0\> |
| 5:4 | Accel Y \<1:0\> |
| 3:2 | Accel X \<1:0\> |
| 1 | Button C (**0 = pressed**) |
| 0 | Button Z (**0 = pressed**) |

Confidence: **Confirmed** — WiiBrew and Dolphin's `Nunchuk.h` bitfield agree exactly:

```cpp
u8 z : 1;  u8 c : 1;  u8 acc_x_lsb : 2;  u8 acc_y_lsb : 2;  u8 acc_z_lsb : 2;

u16 GetAccelX() const { return ax << 2 | bt.acc_x_lsb; }
u16 GetAccelY() const { return ay << 2 | bt.acc_y_lsb; }
u16 GetAccelZ() const { return az << 2 | bt.acc_z_lsb; }
```

Unlike the Wii Remote's accelerometer, **all three Nunchuk axes carry a full 10 bits** — the packing
is a clean 2 bits per axis. (Under MotionPlus Nunchuk passthrough, the LSB is dropped, reducing this
to 9 bits — §7.2.)

The Nunchuk's own two-point calibration block lives in its extension register space at `0xA40020`
(mirrored at `0xA40030`) and follows the same 0 g / 1 g structure as §4.3, followed by joystick
max/min/centre values for X and Y and a checksum. Confidence: **Hypothesis** — this layout is widely
reproduced in userspace libraries but was not verified against a primary source during the
preparation of this document. Do not implement it without confirming first (§14, Q4).

> ⚠️ **Repo note:** `wiimote_bt.c:463-476` already decodes the Nunchuk's joystick and buttons but
> explicitly skips bytes 2–4 with the comment `// Bytes 2-4: accelerometer`. The data is already in
> the buffer; only the decode is missing.

---

## 9. The MotionPlus "challenge"

Real Wii games perform a cryptographic challenge-response against the MotionPlus before trusting it.
Dolphin models the full sequence (`MotionPlus.h:170-233`): a 0x40-byte challenge block at register
`0x50`, a state machine visible through register `0xF7` progressing `0x00 → 0x02 → 0x0E → 0x14 →
0x1A`, and triggers at `0xF1` and `0xF2`.

**You do not need any of it.** The challenge is initiated by the *host software*, not required by
the MotionPlus to stream data. The Linux kernel implements none of it and reads gyro data
successfully. This section exists so a future reader who encounters `0xF7` transitions in a capture
knows what they are looking at and does not conclude their implementation is incomplete.

Confidence: **Confirmed** (kernel omits it entirely and works).

One incidental detail worth knowing: `uid_1` / `uid_2` in the calibration block appear to be a unit
identity that games cache, so that a previously-challenged MotionPlus is not re-challenged
(`MotionPlus.cpp:134-138`). Confidence: **Hypothesis** (Dolphin's author states it as an inference).

---

## 10. Timing and rates

| Property | Value | Confidence |
|---|---|---|
| Nominal input report interval | ~10 ms (≈100 Hz) | Strong Evidence |
| Effective per-source rate in passthrough | ≈50 Hz | Strong Evidence |
| Continuous reporting flag | `0x12` parameter byte bit 2 (`0x04`) | Confirmed |
| Timestamp in report | **None** | Confirmed |

**There is no timestamp field anywhere in the Wii protocol.** Unlike the DualSense (which carries a
32-bit sensor timestamp — see [`dualsense-motion.md`](dualsense-motion.md)) or the Switch 1 Joy-Con (which packs
three time-separated IMU frames per report — see [`switch1-motion.md`](switch1-motion.md)), the Wii
Remote gives you one sample with no time reference at all.

Consequences for integration:

- You must timestamp on arrival, using the host clock, and accept Bluetooth jitter as angular error.
- Dropped frames are **undetectable** — there is no sequence counter either.
- Both of these argue for using the accelerometer as a gravity reference to bound drift (§11) rather
  than relying on pure gyro integration.

Note that the kernel selects **non-continuous** reporting (`cmd[1] = 0` in `wiiproto_req_drm()`,
`hid-wiimote-core.c:249-251`), as does this repository (`wiimote_bt.c:228`). With accelerometer data
enabled this is effectively continuous anyway, because sensor noise guarantees the payload changes
every interval. Setting the continuous bit is nonetheless worth testing for motion work, since it
removes a data-dependent variable from timing analysis (§14, Q5).

---

## 11. Sensor fusion notes

The Wii Remote is unusual in offering three *complementary* references:

| Source | Gives | Drifts? | Absolute? |
|---|---|---|---|
| Accelerometer | Gravity vector → pitch & roll | No | Yes, but corrupted by linear acceleration |
| MotionPlus gyro | Angular velocity → all three axes | **Yes** | No |
| IR camera | Absolute pointing + roll vs. the sensor bar | No | Yes, but only when aimed at the bar |

The standard approach — and the one Nintendo's own middleware used — is a complementary filter: the
gyro provides high-frequency response, the accelerometer corrects low-frequency drift in pitch and
roll, and yaw drift is either accepted or corrected by IR when the sensor bar is visible.

**Yaw is the hard axis.** With no magnetometer, and with the accelerometer blind to rotation about
gravity, yaw drift is unbounded unless IR is available. Any implementation promising absolute
orientation without IR is promising something the hardware cannot deliver. This is worth stating
plainly in user-facing documentation before someone files a bug about it.

For this repository's purposes — translating Wii motion into a Switch 2 controller personality —
**angular velocity is very likely the right output, not orientation.** The Switch 2 Pro Controller's
own motion reports carry rates, not quaternions, so passing rates through avoids building a fusion
filter whose drift characteristics would then have to be matched against genuine hardware. Confidence
that rates are the correct interchange: **Strong Evidence**, pending the outcome of the native
motion work tracked in
[`../experiments/native-pro2-motion-passthrough-2026-07-21.md`](../experiments/native-pro2-motion-passthrough-2026-07-21.md).

---

## 12. PicoSwitch2 current state (audited)

Audited file: `src/bt_hid/bt/bthid/devices/vendors/nintendo/wiimote_bt.c` (975 lines), plus
`wiimote_bt.h`.

### 12.1 What already exists and works

| Capability | Location | Status |
|---|---|---|
| Device matching (VID `0x057E` / PID `0x0306`, name match) | `:303-321` | ✅ |
| Init state machine (14 states) | `:123-139`, `:740-902` | ✅ |
| Status request / `0x20` parse, incl. battery | `:188-192`, `:607-655` | ✅ |
| Extension unencrypted init (`0xF0=0x55`, `0xFB=0x00`) | `:776`, `:790` | ✅ |
| Extension ID read at `0xA400FA` and classification | `:804`, `:681-737` | ✅ |
| Register read/write helpers (`0x16`/`0x17`) | `:194-221` | ✅ |
| Report mode selection (`0x31` / `0x35`) | `:223-231` | ✅ |
| Extension hot-swap | `:632-654` | ✅ |
| Nunchuk / Classic / Classic Mini / Guitar decode | `:461-589` | ✅ |
| Rumble, player LEDs | `:233-239`, `:172-186` | ✅ |
| Orientation auto-detect from accel X, with hysteresis | `:244-264` | ✅ |

**The transport, the register protocol, and the extension state machine are all already correct.**
Adding motion is a decode-and-plumb exercise, not a protocol reverse-engineering exercise. This is a
much stronger starting position than either the DualSense or Switch 1 motion work.

### 12.2 What is missing for motion

| Gap | Evidence | Impact |
|---|---|---|
| **Accelerometer Y and Z are never read.** Only `data[3]` (X) is sampled. | `:432-435` | No usable accelerometer |
| **The 2-bit LSBs are never recovered.** Buttons are masked `0x1F`/`0x9F` at `:365`, destroying accel bits 5–6 before any accel decode could reach them. | `:365` | 8-bit sensor instead of 10-bit; wrong zero point |
| **EEPROM accel calibration is never read.** `wiimote_read_data()` hardcodes address space `0x04` (registers) and cannot address EEPROM at all. | `:194-221` | No calibration path exists |
| **No MotionPlus detection.** `0xA600FA` is never read. | — | Gyro invisible |
| **No MotionPlus activation.** `0xA600FE` is never written. | — | Gyro unreachable |
| **`wiimote_ext_type_t` has no MotionPlus member.** | `:71-77` | No state to represent it |
| **Extension IDs `04 05` / `05 05` / `07 05` are unhandled** — they fall through to `"Unknown extension"`. | `:726-730` | An active M+ would be misclassified |
| **Nunchuk accelerometer bytes 2–4 are explicitly skipped.** | `:463-476` | Second accelerometer unused |
| **`input_event_t` carries no motion fields on this path.** | `core/input_event.h` | Nowhere to put the data |

### 12.3 Two real bugs found during this audit

**Bug 1 — every LED, report-mode, and status command silently stops rumble.**

Per §2.2, bit 0 of the first parameter byte of *every* output report is the rumble latch. The repo
sends literal zeros there:

```c
/* :175  */ uint8_t buf[3] = { 0xA2, WII_CMD_LED, led_pattern };
/* :190  */ uint8_t buf[3] = { 0xA2, WII_CMD_STATUS_REQ, 0x00 };
/* :228  */ uint8_t buf[4] = { 0xA2, WII_CMD_REPORT_MODE, 0x00, mode };
/* :198+ */ buf[2] = 0x04;   /* write-data: register space, rumble bit clear */
```

`led_pattern` is built from `(1 << (player + 3))` — bits 4–7 only — so bit 0 is always clear.
Consequence: any player-LED update, keep-alive status request, or report-mode change during active
rumble **turns the motor off**. The 30-second keep-alive at `:891-896` makes this reachable in normal
use.

Fix: mirror the kernel's approach — cache the rumble state in `wiimote_data_t` and OR it into byte 2
of every outbound report, exactly as `wiiproto_keep_rumble()` does.

Confidence: **Confirmed** by code inspection against a Confirmed protocol fact. Not yet observed on
hardware.

**Bug 2 — `wiimote_write_data()` declares a 23-byte buffer for a 22-byte report.**

```c
/* :196 */ uint8_t buf[23];
```

Output report `0x16` is 21 bytes of payload plus the report ID, plus the `0xA2` transaction byte =
22. The extra byte is harmless but is sent, meaning one trailing zero byte beyond the defined report
length reaches the remote. It evidently works, but it is worth tightening to `buf[22]` and noting
the derivation, because the current value looks like a deliberate size and is not.

Confidence: **Confirmed** (arithmetic against the Confirmed report size in §2.1). **Impact: cosmetic.**

### 12.5 Implemented 2026-07-27 (build-clean + host-tested, hardware pending)

| Checklist item | State |
|---|---|
| §13.1 raw button bytes, 10-bit X/Y/Z assembly | ✅ `wiimote_decode_accel()` |
| §13.3 accelerometer emitted | ✅ published through `input_event_t`, not a debug channel |
| §13.6 `WII_EXT_MOTIONPLUS*` types | ✅ incl. the `04 05` / `05 05` / `07 05` identifiers that previously fell through to "Unknown extension" |
| §13.7 probe `0xA600FA` | ✅ `WII_STATE_MP_DETECT` |
| §13.8 calibration before activation + CRC | ✅ two 16-byte reads of `0xA60020`, CRC32-verified |
| §13.9 MotionPlus init pair | ✅ `0xA600F0`/`0xA600FB` |
| §13.10 activation `0xA600FE` | ✅ mode chosen from the downstream extension |
| §13.11 verify `0xA400FA` with retries | ✅ 5 × 50 ms |
| §13.12 deactivation hazard audit | ✅ probe ordered strictly after ext-init; nothing re-runs `0xA400F0` afterwards |
| §13.13 frame decode honouring `is_mp_data` | ✅ checked per frame |
| §13.14 per-axis slow/fast + calibrated conversion | ✅ `wii_mp_sample_centi_dps()` |
| §13.16 no second motion representation | ✅ reuses `input_event_t`; publishes the SInput convention (`±32767 = ±2000 dps` / `±4 g`) that `report-0x09-motion.md` shows the encoder consumes |
| §13.18 rumble-latch bug | ✅ fixed; all senders OR in the cached state |

**Units and axes were taken from documentation, not measurement.**
`docs/switch2/report-0x09-motion.md` fixes the chain end-to-end (`in.gyro` at
16.384 LSB/dps; `in.accel` × 65536 into Q16.16 where 4096 = 1 g, after the seam
halves it), and `ds3_bt.c` states the same convention. Motion is published in the
DualSense slot frame because `ns2_seam.c` remounts that frame into the Pro2 frame
for every source: `accel = [wiiX, wiiZ, wiiY]` (Wii is Z-up, DualSense Y-up) and
`gyro = [pitch, yaw, roll]` with Dolphin's `sign_fix (-1,+1,-1)`.

The Wii now has its own `SWITCH_MOTION_SOURCE_WII` provenance rather than
borrowing `GENERIC`. Verified safe first: the only consumer is a positive test for
`DUALSENSE`, and nothing tests `== GENERIC`, so the new id lands on the generic
encoder exactly as before while giving future IMU-bearing controllers a place for
per-family policy.

**Not implemented:** §13.4/§13.5 EEPROM accelerometer calibration (needs the
address-space byte parameterised; the two-point struct and fallback constants are
already in place), §13.15 passthrough bit-reversal for extension frames decoded
*behind* an active MotionPlus, and §13.17 re-expressing
`wiimote_detect_orientation()` on the calibrated vector.

**Still unvalidated on hardware**, so §14 Q1–Q7 all remain open — in particular Q2
(true axis signs). The decode is proven against the documented bit layout by
`tools/test_wii_motionplus.c`, which is a correctness proof of the parser, not of
the sign conventions.

### 12.4 Architectural observation

The orientation auto-detect at `:244-264` is, in effect, **a motion feature already shipping** — it
consumes accelerometer data to infer how the user is holding the device. It is currently implemented
as a one-off with magic thresholds (`WII_ACCEL_CENTER 128`, `THRESH_ON 20`, `THRESH_OFF 12`) applied
to a truncated 8-bit X reading.

When a proper accelerometer decode lands, this should be **re-expressed on top of it** — comparing a
calibrated gravity vector against the device's axes — rather than left as a parallel implementation.
That removes three magic numbers, makes the behaviour explainable, and would let the same code serve
the sideways/upright orientation problem documented in
[`../switch2-joycon2/mapping.md`](../switch2-joycon2/mapping.md). This is a concrete instance of
the consolidation that `CLAUDE.md` asks for.

---

## 13. Implementation checklist

Ordered so that each step is independently testable.

**Phase 1 — Accelerometer (no new protocol required)**

1. Read the button bytes **raw** before the `0x1F`/`0x9F` mask, and assemble 10-bit X/Y/Z per §4.2.
2. Ensure report mode `0x31` is selected even with no extension — already true (`:227`).
3. Emit raw counts to the UART diagnostic channel (`src/ns2_uart_diag.c`) and confirm ≈`0x200` at
   rest and ≈`0x200 + 1 g` on Z.
4. Add EEPROM read support: extend `wiimote_read_data()` to take the address-space byte as a
   parameter (`0x00` for EEPROM, `0x04` for registers) rather than hardcoding `0x04`.
5. Read `0x0016`, validate the checksum, fall back to `0x0020`, apply §4.4.

**Phase 2 — MotionPlus detection and activation**

6. Add `WII_EXT_MOTIONPLUS` and `WII_EXT_MOTIONPLUS_PASSTHROUGH_*` to `wiimote_ext_type_t`.
7. After the status report indicates no extension — *and also when one is present* — probe
   `0xA600FA` for `·· 00 A6 20 00 05`.
8. If found, read the 32-byte calibration block at `0xA60020` **before activating**, and verify its
   CRC32.
9. Send the MotionPlus init pair (`0xA600F0 = 0x55`, `0xA600FB = 0x00`).
10. Write the passthrough mode to `0xA600FE` based on what else was detected (§6.2).
11. Re-read `0xA400FA` with retries and confirm `04 05` / `05 05` / `07 05`.
12. **Audit the init state machine for the deactivation hazard in §6.3** — do not write `0xA400F0`
    after activation.

**Phase 3 — MotionPlus decode**

13. Parse the 6 extension bytes per §6.4, honouring `is_mp_data` at `[5]:1`.
14. Apply per-axis slow/fast block selection and the calibrated conversion of §6.8.
15. For passthrough modes, apply the inverse bit transformations of §7.2 before decoding the
    extension frame.

**Phase 4 — Plumbing**

16. Extend `input_event_t` with gyro and accel fields, or reuse whatever the DualSense motion work
    establishes — **do not invent a second motion representation.** Coordinate with `dualsense-motion.md`
    §13 and the native Pro Controller 2 motion experiment.
17. Re-express `wiimote_detect_orientation()` on the calibrated vector (§12.4).
18. Fix the rumble-latch bug (§12.3) — it will become far more visible once motion features
    encourage longer sessions.

**Phase 5 — Documentation**

19. Record a UART trace of a full MotionPlus activation as a capture under `docs/experiments/`.
20. Promote every Strong Evidence claim in this document that the capture confirms, and answer §14.

---

## 14. Open questions

| # | Question | Why it matters | Suggested experiment |
|---|---|---|---|
| **Q1** | Does an input report `0x20` really reset the Data Reporting Mode to `0x30`? | If yes, the repo's 30-second keep-alive kills motion streaming. | Connect, set mode `0x35`, send `0x15`, log subsequent report IDs over UART for 60 s. |
| **Q2** | What are the true signs of the three accelerometer axes and the three gyro axes? | Every consumer of this data depends on it; getting it wrong inverts gestures. | Six-position static test (each face down) for accel; three single-axis rotations at known sense for gyro. Record raw counts. |
| **Q3** | How long is the MotionPlus activation settling period, and does the port really go fully unresponsive? | Determines retry/timeout design in the init state machine. | Write `0xA600FE`, then poll `0xA400FA` every 10 ms and log the first successful read latency. |
| **Q4** | Is the Nunchuk calibration block at `0xA40020` laid out as assumed in §8? | It is the one Hypothesis-tier layout in this document. | Read 16 bytes from `0xA40020`, verify checksum, compare 0 g values against observed rest readings. |
| **Q5** | Does setting the continuous-reporting bit (`0x12` flags bit 2) change the observed interval or jitter? | Timing determinism matters because there is no timestamp (§10). | Capture inter-arrival times over 30 s with the bit clear and set. |
| **Q6** | Which nominal full-scale figure (§6.10) matches a real unit, once its own calibration block is read? | Resolves a genuine three-way source disagreement. | Read `0xA60020`, compute implied full scale, compare against 440/508/595 °/s. Publish the unit's serial. |
| **Q7** | Does the Wii Remote Plus (`RVL-CNT-01-TR`) behave identically, and is `IS_INTEGRATED` really byte 0? | Determines whether one code path serves both. | Repeat Q3 and Q6 on a `-TR` unit; log the full `0xA600FA` identifier. |

---

## 15. References

**Primary — protocol documentation**

- [WiiBrew: Wiimote](https://wiibrew.org/wiki/Wiimote) — report modes, accelerometer bit packing,
  EEPROM calibration, output report list, memory read/write.
- [WiiBrew: Wiimote/Extension Controllers/Wii Motion Plus](https://wiibrew.org/wiki/Wiimote/Extension_Controllers/Wii_Motion_Plus)
  — activation, identifiers, data format, passthrough.
- [WiiBrew: Wiimote/Extension Controllers/Nunchuck](https://wiibrew.org/wiki/Wiimote/Extension_Controllers/Nunchuck)
  — Nunchuk frame layout.

**Primary — implementations consulted directly (source read, not summarised)**

- Linux kernel `drivers/hid/hid-wiimote-core.c` — extension/MotionPlus detection and activation
  (`:417-558`), memory request framing (`:305-359`), rumble latch (`:126-128`).
  <https://github.com/torvalds/linux/blob/master/drivers/hid/hid-wiimote-core.c>
- Linux kernel `drivers/hid/hid-wiimote-modules.c` — accelerometer decode (`:425-457`), MotionPlus
  decode and scaling (`:2712-2769`).
  <https://github.com/torvalds/linux/blob/master/drivers/hid/hid-wiimote-modules.c>
- Dolphin `Source/Core/Core/HW/WiimoteEmu/MotionPlus.h` — calibration block structure, data-format
  bitfields, scale constants.
  <https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/WiimoteEmu/MotionPlus.h>
- Dolphin `Source/Core/Core/HW/WiimoteEmu/MotionPlus.cpp` — calibrated conversion (`:56-89`),
  activation/deactivation semantics (`:190-340`), passthrough bit transformations (`:651-711`).
  <https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/WiimoteEmu/MotionPlus.cpp>
- Dolphin `Source/Core/Core/HW/WiimoteEmu/Extension/Nunchuk.h` — Nunchuk accelerometer packing.
  <https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/WiimoteEmu/Extension/Nunchuk.h>

**Secondary**

- Analog Devices ADXL330 product page — accelerometer range.
  <https://www.analog.com/en/products/adxl330.html>
- USB_Host_Shield_2.0 `Wii.cpp` — cited by this repository's existing driver as its original
  reference.

**Related documents in this repository**

- [`dualsense-motion.md`](dualsense-motion.md) — DualSense IMU reference. Contrast: dedicated 16-bit gyro,
  hardware timestamp, single fixed range.
- [`switch1-motion.md`](switch1-motion.md) — Switch 1 Joy-Con / Pro IMU reference. Contrast: three
  time-separated frames per report, SPI-flash calibration.
- [`../switch2-joycon2/mapping.md`](../switch2-joycon2/mapping.md) — sideways/upright orientation
  policy; §12.4 above proposes consolidating the Wiimote's orientation detection with that work.
- [`../switch2/uart-trace-tooling.md`](../switch2/uart-trace-tooling.md) — the UART diagnostic channel used by the Phase 1 and Phase 5 steps.
- [`../re-methodology/evidence-standards.md`](../re-methodology/evidence-standards.md) —
  confidence tier definitions.

---

## Appendix A — Quick reference card

```
DETECT MOTIONPLUS
  read  6 ← 0xA600FA        expect ?? 00 A6 20 00 05

READ CALIBRATION  (before activating!)
  read 32 ← 0xA60020        fast block, uid1, crc_hi, slow block, uid2, crc_lo

ACTIVATE
  write 0x55 → 0xA600F0
  write 0x00 → 0xA600FB
  write MODE → 0xA600FE     0x04 none | 0x05 nunchuk | 0x07 classic
  read  6    ← 0xA400FA     expect ?? ?? A4 20 <MODE> 05

STREAM
  write 0x12, flags, 0x35   buttons + accel + 16 ext bytes
                            (flags bit0 = rumble, bit2 = continuous)

FRAME (mode 0x35, data[0] = report id)
  data[1..2]  buttons + accel LSBs   (read RAW, before masking)
  data[3..5]  accel X, Y, Z <9:2>
  data[6..11] extension / MotionPlus

ACCEL ASSEMBLY
  X = data[3]<<2 | (data[1]>>5)&0x3
  Y = data[4]<<2 | (data[2]>>4)&0x2
  Z = data[5]<<2 | (data[2]>>5)&0x2      zero ≈ 0x200

MOTIONPLUS FRAME  (ext[5] bit1 == 1)
  yaw   = ext[0] | ((ext[3]>>2)<<8)      slow bit = ext[3] & 0x02
  roll  = ext[1] | ((ext[4]>>2)<<8)      slow bit = ext[4] & 0x02
  pitch = ext[2] | ((ext[5]>>2)<<8)      slow bit = ext[3] & 0x01
  ext connected = ext[4] & 0x01          zero rate ≈ 8192

DEACTIVATE
  write 0x55 → 0xA400F0
  write 0x00 → 0xA400FB
```
