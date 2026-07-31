# Experiment A — Results: Steam-path gyro root cause (report 0x05)

**Status:** ✅ complete — **root cause found and fixed** (pending hardware re-validation).
**Date:** 2026-07-10. **Parent:** [gyro-experiment-a-plan.md](gyro-experiment-a-plan.md).
**Captures:** `E:\PicoSwitch2\usbpcaptures\{genuine_procon_2,picoswitch_2_dongle}.pcapng`
(USBPcap, ~10–11 MB each). Harness: session-local analysis scripts, not retained in the repository.

---

## A0 — Feasibility gate (passed)
Genuine Pro 2 wired to the PC via USB-C: **Steam reads and interprets its gyro.** So the Steam
path is viable and a golden trace exists — proceed to the differential capture.

## What Steam does (from the genuine capture)
- Device: `VID 057E / PID 2069`, **bcdDevice 0x0201** (retail), addr 38. Ours: 0x0210 (our
  deliberate WinUSB-cache offset), addr 41. **bcdDevice is not implicated** — Steam drives gyro
  on the genuine 0x0201 unit, and our 0x0210 unit still gets enabled + selected + streams motion.
- Steam sends the **IMU-enable over bulk EP2** — same as the console: `0c 91 00 02 …27` and
  `0c 91 00 04 …27` (feature mask **0x27**). So the `0x0C` enable is used on the Steam path too.
- Steam **selects report `0x05`** (not 0x09): `03 91 00 0a 00 04 00 00 **05** 00 00 00`.
- Both devices then stream report **0x05** (~20k packets) on HID EP `0x81`. Report 0x05 is where
  Steam reads gyro.

## Report 0x05 motion layout (decoded from the genuine stream)
Capture offsets (capdata[0] = report id 0x05; firmware buffer `p[k]` = capdata[k+1]):

| capture off | firmware `p[]` | field | genuine behavior |
|---|---|---|---|
| 0x2A | p[0x29] | const `0x01` | constant |
| 0x2B–0x2E | **p[0x2A]** | **IMU timestamp, u32 LE** | **increments every report** |
| 0x2F | p[0x2E] | const `0x01` | ~constant |
| 0x31/0x33/0x35 | p[0x30]/p[0x32]/p[0x34] | accel X/Y/Z int16 LE | moves (±5000–7000) |
| 0x37/0x39/0x3B | p[0x36]/p[0x38]/p[0x3A] | gyro X/Y/Z int16 LE | moves (peak 7401) |

(Firmware positions for accel `p[0x30]` and gyro `p[0x36]` already matched — only the timestamp
and the scale were wrong.)

## Differential result — genuine vs ours (report 0x05)

| Field | Genuine | Ours | Verdict |
|---|---|---|---|
| **Timestamp u32 @0x2B** | `0x7e1a→0x87de→0x8cc0…` — changes **19553/19553** | **`0x0` always — 0/20330** | 🔴 **frozen** |
| **Gyro peak \|value\|** | **7401** | **122** (~60× too small) | 🔴 **scale** |
| Accel range | X ±~5000, Y ±~6000, Z −4578..6989 | X/Y ±~5000 (live), Z briefly 16383 | ✅ ok (watch Z) |
| Const byte p[0x2E] | `0x01` | `0x00` | minor |

**Two root causes, both in report 0x05:**
1. **Frozen IMU timestamp.** `ns2_build_report_05()` `memset`s the buffer, writes accel and gyro,
   but **never writes the 4-byte timestamp at `p[0x2A]`** (the `static uint32_t counter` there was
   dead code). Steam integrates motion against this timestamp; a frozen (all-zero) timestamp makes
   gyro **show one value then freeze** — exactly the user's reported symptom.
2. **Gyro ~60× under-scaled.** The seam divided DualSense raw gyro by **64** (`ns2_seam.c`),
   collapsing peak rotation from ~7808 to ~122 LSB — imperceptible in Steam even when not frozen.
   The DualSense raw gyro is already close to the Switch int16 gyro scale (≈ pass-through).

Accel was already correct (`/2` matches the genuine range). Note the transient Z=16383 on ours —
a momentary near-full-scale accel reading, not a scaling bug; monitor after the fix.

## Fix applied
- `src/switch_pro2/switch_pro2.c` `ns2_build_report_05()`: write `time_us_32()` as the 4-byte
  timestamp at `p[0x2A]` (gated on `has_motion`) + `p[0x2E]=0x01`. `time_us_32()`'s ~1 MHz cadence
  matches the genuine ~0.8 MHz closely; only monotonic advance matters.
- `src/bt_hid/ns2_seam.c`: gyro scaling `/64` → `/1` (pass-through), keeping the axis transform.

## Validation plan (hardware)
Re-flash, pair a DualSense, re-capture `ours_dongle_steam` and re-run `usb_a_confirm.py`:
- **timestamp** should now change ~every report (≈20000/20000), and
- **gyro peak** should reach the low thousands (comparable to genuine ~7401), and
- **Steam** should show live, continuous gyro that tracks the DualSense (not frozen).
If gyro moves but is off-scale, adjust the seam divisor; if it still freezes, the timestamp
cadence/width needs tuning (unlikely — genuine only needs monotonic advance).

## Scope boundary
This resolves the **Steam / report-0x05** path (the symptom the user reported). It is **independent**
of the **console / report-0x09** work, where the motion block is the corrected **int32 phase +
Q16.16** format and must additionally be **gated behind the `0x0C` enable**
([report-0x09-motion.md](../switch2/report-0x09-motion.md), [Experiment C](gyro-experiment-c-results.md)).
The genuine controller also streams a second report `0x01` (~60k pkts) on EP 0x81 that Steam does
not select for motion — not investigated.
