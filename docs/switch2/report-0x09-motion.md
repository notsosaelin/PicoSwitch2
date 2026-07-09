# Switch 2 Pro Controller — Report 0x09 Motion (IMU) Format

**Status:** 🔵 Decoded from a real controller's clear USB capture; pending on-console validation.
**Confidence:** Strong (layout + scale from real bytes) for the accel/gyro sample; Hypothesis for
the second lane group (magnetometer vs. duplicate sample) and the exact trailer.

---

## Question

Report `0x09` (the console-facing input report) carries an IMU block whose packing ndeadly's
`hid_reports.md` calls an *"unknown packed format"* (length byte `{0,30,40}` at offset `0x0E`, data
at `0x0F`). Two blind hypotheses (3 samples, length 40) produced **no motion** on the console. We need
the real layout and scale.

## Method — decode from the unencrypted USB capture

ndeadly's BLE motion captures are link-encrypted (no in-capture handshake → undecryptable). But
`captures/usb/rumble-procon-gccon.pcapng.gz` is a **USB** capture (USB has no link encryption) of a
real Pro Controller 2. Parsed it directly:

- Container: **big-endian pcapng**, `LINKTYPE_USB_DARWIN` (288). ~190k 67-byte HID input packets.
- Each 67-byte packet = 4-byte header + `09` report-id + 62-byte report body.
- Per-byte variance over the body located: counter@0x00, power@0x01, buttons@0x02–0x04, sticks@0x05+,
  **motion-length@0x0E (values {0, 30})**, **motion@0x0F**.
- Decoded the 30-byte motion region as 15 `int16` LE lanes. Used *gravity magnitude invariance*
  (|accel| is constant regardless of orientation) across 50k reports to identify the accel triple.

Scripts in the session scratchpad: `usb_parse.py`, `usb_reports.py`, `usb_motion.py`, `usb_accel.py`,
`usb_still.py`.

## Format (length = 30)

Offsets are within report 0x09 (report-id stripped; `p[0x00]` = counter).

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x0E | 1 | Motion data length | `0` (IMU off) or `30` (this capture). ndeadly also saw `40`. |
| 0x0F | 2 | Timestamp | int16, **increments +4 per report**. |
| 0x11 | 2 | Temperature | constant `0x0C00` (3072) in the capture. |
| 0x13 | 12 | **Sample 0** | interleaved `int16`: `[g_x, a_x, g_y, a_y, g_z, a_z]`* |
| 0x1F | 12 | **Sample 1** | same interleave; **accel confirmed @0x21/0x25/0x29**, gyro @0x1F/0x23/0x27. |
| 0x2B | 2 | Trailer | small (~110 in capture); left 0 in our output. |

\* Sample-0's "accel" lanes (0x15/0x19/0x1D) read magnetometer-like on the real unit (large,
orientation-stable, ~±30000). In IMU-only mode (feature mask `0x27`, no magnetometer bit) the console
likely ignores them; we currently write our accel there too (harmless if ignored, correct if it is a
duplicate sample). **Remaining unknown — see below.**

### Scale (confirmed)

- **Accelerometer: 1g = 4096 LSB (±8g FSR)** — the accel triple's magnitude is a rock-steady ~4096
  (coefficient of variation 0.03), peak axis < 32k (no clipping). Matches the ICM-42670-P at ±8g.
- **Gyroscope: ~16 LSB/deg/s** (ICM-42670-P ±2000 dps).
- Our seam scales the DualSense IMU by **/2 (accel)** and **/64 (gyro)**, which yields exactly these
  units — the same `in.accel`/`in.gyro` report 0x05 emits (and report-0x05 gyro works on PC). So
  **report 0x09 reuses those values unchanged**; no rescaling.

## Implementation

`src/switch_pro2/switch_pro2.c` `ns2_build_report()` writes the above when `in.has_motion` is set
(gated so non-gyro pads keep length 0). Both samples carry our single per-report sample.

## Remaining unknowns / suggested experiments

1. **Sample-0 lane group (0x13–0x1E):** magnetometer, or a genuine second accel/gyro sample? Test on
   the console: if gyro works with our current fill, the console reads the confirmed sample (0x1F). If
   aiming feels doubled/odd, try zeroing 0x15/0x19/0x1D.
2. **Length 40 variant:** ndeadly observed 40 — likely 3 samples. Not seen in this capture; add if a
   game requests a higher rate and 30 is rejected.
3. **Timestamp semantics:** +4/report worked for the real unit; confirm the console doesn't require a
   specific epoch/rate.
4. **Durable RE infra:** our own dongle is a BT host that *decrypts* a real controller's reports — the
   config-mode raw-report view can capture any future Switch 2 controller's format directly (see
   `PLAN.md` "dongle-as-sniffer"). The USB capture answered this one, but that path removes the need
   for external captures next time.
