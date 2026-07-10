# STATUS.md

> Living snapshot of the repository's current state. Not a roadmap (see `PLAN.md`),
> not project guidance (see `CLAUDE.md`). Update whenever significant work lands.

---

# Project Summary

**Project:** PicoSwitch2 — bridge Bluetooth controllers to a Nintendo Switch 2 as a **native Switch 2 Pro Controller**.

**Emulated device:** Switch 2 Pro Controller — USB VID `0x057E` / PID `0x2069`, bcdDevice `0x0210`.

**Boards:** Pico W (RP2040) and Pico 2 W (RP2350), one source tree.

**Current Branch:** `ns2-testing`

**Latest milestone:** PC/Steam gyro working — the dongle is a native Switch 2 Pro Controller with live
gyro on Steam (report 0x05 timestamp + scale fix). Console gyro (report 0x09 int32) rewrite is next.

**Last Updated:** 2026-07-10 — Experiment A (Steam gyro root-caused + fixed); report-0x09 motion format
corrected to int32; documentation cleanup pass.

---

# Current Objective

**Gyro (v1.1) — active reverse-engineering.** The report-0x09 motion format has been **re-decoded and
corrected**: it is **three int32 angular-phase accumulators + three int32 Q16.16 accelerometer values**
(not the interleaved-int16 model we shipped — that was refuted; the "gyro" bytes were the fractional
halves of the Q16.16 accel). Verified on our own capture (accel |g|=1.0005, CV 0.007; timing
high-nibble = 800 Hz tick-delta 299/299). Motion is also a **negotiated feature** (streams `len=0`
until the `0x0C` feature-enable, mask `0x27`), which our always-on firmware inverts. Firmware **not yet
rewritten**. Current step: **Experiment A** (USBPcap genuine-vs-ours on the Steam PC) to resolve the
Steam/report-0x05 path before implementing. First release (bluepad32 retired) is complete.
See [`docs/experiments/`](docs/experiments/) and [`docs/switch2/report-0x09-motion.md`](docs/switch2/report-0x09-motion.md).

---

# Overall Progress

| Area | Status | Notes |
|---|---|---|
| Build system | ✅ | Both boards build clean from one tree; `build.ps1`. bluepad32 removed from CMake. |
| Bluetooth stack | ✅ | Vendored joypad-os bthid (`src/bt_hid`) + BTstack/CYW43. All vendor drivers enabled. |
| Switch 2 USB identity | ✅ | Composite IAD device; EP0 vendor handshake byte-exact vs a real PC2; console accepts it. |
| Button/stick output | ✅ | report 0x09 (console) + report 0x05 (PC). GL/GR/C confirmed on-console. 250 Hz poll (genuine). |
| Controllers | ✅ | DualSense/DualSense Edge, Xbox Series/Elite 2, Switch/8BitDo/etc. via joypad-os drivers. |
| Extended buttons | ✅ | Edge back paddles/Fn, Elite 4 paddles → GL/GR + Capture/C (name/report-based detection). |
| Rumble | ✅ | Console rumble decoded (report 0x02) and forwarded to the pad. |
| Config web UI | ✅ | Live input↔output view, dynamic per-controller menu, per-device remapping, lightbar, raw-report debug. |
| PC / Steam | ✅ | Enumerates as a Switch 2 Pro; report 0x05 incl. gyro works. |
| Gyro (report 0x09) | 🟡 | Format **re-decoded + verified**: int32 phase + Q16.16 accel (old int16 model refuted). Motion is a negotiated feature (`0x0C` enable). Firmware not yet rewritten. See `docs/switch2/report-0x09-motion.md`. |
| Gyro (report 0x05 / Steam) | 🔵 | **Root-caused + fixed** (Experiment A): frozen IMU timestamp @p[0x2A] (now written) + gyro 60× under-scaled (`/64`→`/1`). Built; pending HW re-capture. `docs/experiments/gyro-experiment-a-results.md`. |
| BT pairing reliability | 🟡 | Pro 2 reconnect sometimes needs a triple-tap; works but flaky. |
| BLE VID/PID (DIS) | 🟡 | Often resolves to 0; worked around by name + report-length detection. |

---

# Working Features (hardware-validated)

- **Console:** detected as a native Switch 2 Pro Controller; all buttons (incl. **GL/GR/C**), sticks,
  D-pad, and rumble output correctly. Behaves as a wired controller (the dongle is USB-wired).
- **250 Hz USB poll** (bInterval 4) — matches the genuine PC2; halved latency vs the old 125 Hz.
- **Config mode:** hold BOOTSEL ~5 s → re-enumerate as CDC + read-only MSC serving the config page.
  The BT stack keeps running, so input streams live.
- **Config UI** (Chrome/Edge, Web Serial): 4-panel layout — current input type (auto-detected), output
  type, live **Detected → Will-Output-As** table with per-button remap dropdowns, and lightbar colour.
  Also a live raw-HID-report hex row for reverse-engineering new controllers, and an Elite active-profile
  warning.
- **Per-device remapping:** each controller family (Sony / Xbox / Nintendo / Generic) has its own stored
  JP→Switch map; defaults reproduce the built-in behaviour exactly.

---

# Deferred / Blocked

- **Gyro (🟡 active RE).** The report-0x09 motion format is **re-decoded and corrected** (int32 phase +
  Q16.16 accel; the shipped int16 model is refuted — see `docs/switch2/report-0x09-motion.md`) and the
  activation is now understood (**negotiated feature** via the `0x0C` enable, mask `0x27`; motion streams
  `len=0` until then — proven in `docs/experiments/gyro-experiment-c-results.md`). Three stacked issues
  remain to fix: (1) firmware motion format (int16→int32), (2) enable-gate (we're always-on), (3) the
  `switch2_ble` BT driver discards the genuine Pro 2's motion. **Current step: Experiment A** (USBPcap
  genuine-vs-ours on Steam) to resolve whether Steam uses report 0x09 or 0x05 before the firmware rewrite.
- **BT pairing reliability (🟡).** Pro Controller 2 reconnect can need a triple-tap and remains flaky.
- **BLE DIS VID/PID (🟡).** The PnP VID/PID frequently arrive as 0; detection is worked around by device
  name + report length. A proper DIS/SDP resolution fix would be cleaner.

# Out of Scope (confirmed)

Wake-console-over-USB (Switch 2 wakes only on a BLE advert from a bonded controller; our dongle is USB);
4-player (single-controller milestone for now); HD-rumble → DualSense haptics; audio over BT controllers.

---

# Reverse-Engineering Progress

- **EP0 vendor identity handshake** — byte-exact vs a real PC2 (validated by MITM capture).
- **report 0x09 (console)** — buttons/sticks/GL/GR/C confirmed on hardware; **motion format corrected**
  to int32 phase + Q16.16 accel and verified on our capture (`docs/switch2/report-0x09-motion.md`).
  Motion is a **negotiated feature** — enabled by the `0x0C`/`0x27` handshake, not always-on
  (`docs/experiments/gyro-experiment-c-results.md`).
- **report 0x05 (PC/common)** — buttons + accel@0x30 / gyro@0x36 int16 (matches TommyWabg's reader).
- **Xbox Elite 2** — 4 paddles captured in report byte 19 (R4=0x01 R5=0x02 L4=0x04 L5=0x08); byte 17 =
  active profile (0=base); paddles only report raw when unmapped in the active profile.
- **DualSense Edge** — Fn/paddle bits in the 3rd button byte (0x10 FnL, 0x20 FnR, 0x40/0x80 paddles), per SDL.
- **IMU chip** = ICM-42670-P (from ndeadly datasheets).

---

# Technical Debt / Notes

- Config-mode `info` still reports `"version":"2.0"` (cosmetic; the web page keys off `id` only).
- `docs/` is being filled out (see PLAN.md documentation milestone).
- Gyro emission in report 0x09 currently uses the **refuted int16 layout** and is **always-on**; both are
  wrong (see the corrected format doc + Experiment C). Rewrite pending Experiment A.

---

# Next Recommended Tasks

1. **Gyro — Experiment A** (USBPcap genuine-vs-ours on Steam): resolve the report-0x05 vs 0x09 Steam path,
   then rewrite `ns2_build_report()` to the corrected int32 format + `0x0C` enable-gate. `docs/experiments/gyro-experiment-a-plan.md`.
2. **BT pairing reliability** — investigate the Pro 2 reconnect flakiness in the joypad-os BLE bond path.
3. **BLE DIS VID/PID** — resolve the PnP query so detection doesn't rely on names.
4. **Docs** — finish `/docs` (architecture, protocol, RE methodology) per CLAUDE.md.
