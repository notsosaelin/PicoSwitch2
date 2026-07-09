# STATUS.md

> Living snapshot of the repository's current state. Not a roadmap (see `PLAN.md`),
> not project guidance (see `CLAUDE.md`). Update whenever significant work lands.

---

# Project Summary

**Project:** PicoSwitch2 — bridge Bluetooth controllers to a Nintendo Switch 2 as a **native Switch 2 Pro Controller**.

**Emulated device:** Switch 2 Pro Controller — USB VID `0x057E` / PID `0x2069`, bcdDevice `0x0210`.

**Boards:** Pico W (RP2040) and Pico 2 W (RP2350), one source tree.

**Current Branch:** `ns2-testing`

**Latest Significant Commit:** `571739b` — Config: drop dead bluepad32 remap, finalize schema v5

**Last Updated:** 2026-07-05 — first-release cleanup (bluepad32 retired)

---

# Current Objective

**First release.** bluepad32 is fully retired; joypad-os is the sole Bluetooth stack. Console
gyro is deferred to the next sprint (its report-0x09 motion format is undocumented and the
reference captures are encrypted — see below).

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
| Console gyro | 🔴 | Deferred. report-0x09 motion format undocumented; reference captures encrypted. |
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

- **Console gyro (🔴 next sprint).** report 0x09's 40-byte motion block (offset 0x0F, length byte @0x0E,
  observed {0,30,40}) is an undocumented packed format. ndeadly's procon2 motion captures are BLE
  link-encrypted with **no in-capture handshake** → undecryptable without a fresh capture. Two blind
  hypotheses tried on hardware = no motion. PC gyro (report 0x05) already works. **Definitive path:** a
  decryptable BLE sniff of a real PC2↔console session (with the pairing/encryption handshake), then decrypt
  with the LTK to read the exact bytes.
- **BT pairing reliability (🟡).** Pro Controller 2 reconnect can need a triple-tap and remains flaky.
- **BLE DIS VID/PID (🟡).** The PnP VID/PID frequently arrive as 0; detection is worked around by device
  name + report length. A proper DIS/SDP resolution fix would be cleaner.

# Out of Scope (confirmed)

Wake-console-over-USB (Switch 2 wakes only on a BLE advert from a bonded controller; our dongle is USB);
4-player (single-controller milestone for now); HD-rumble → DualSense haptics; audio over BT controllers.

---

# Reverse-Engineering Progress

- **EP0 vendor identity handshake** — byte-exact vs a real PC2 (validated by MITM capture).
- **report 0x09 (console)** — buttons/sticks/GL/GR/C layout confirmed on hardware; motion still unknown.
- **report 0x05 (PC/common)** — buttons + accel@0x30 / gyro@0x36 int16 (matches TommyWabg's reader).
- **Xbox Elite 2** — 4 paddles captured in report byte 19 (R4=0x01 R5=0x02 L4=0x04 L5=0x08); byte 17 =
  active profile (0=base); paddles only report raw when unmapped in the active profile.
- **DualSense Edge** — Fn/paddle bits in the 3rd button byte (0x10 FnL, 0x20 FnR, 0x40/0x80 paddles), per SDL.
- **IMU chip** = ICM-42670-P (from ndeadly datasheets).

---

# Technical Debt / Notes

- Config-mode `info` still reports `"version":"2.0"` (cosmetic; the web page keys off `id` only).
- `docs/` is being filled out (see PLAN.md documentation milestone).
- Gyro emission is left in report 0x09 behind the `has_motion` gate as hypothesis 2 (harmless — the console
  ignores an unrecognized block); revisit when the format is known.

---

# Next Recommended Tasks

1. **Console gyro** — obtain a decryptable BLE capture, decode the report-0x09 motion packing, implement.
2. **BT pairing reliability** — investigate the Pro 2 reconnect flakiness in the joypad-os BLE bond path.
3. **BLE DIS VID/PID** — resolve the PnP query so detection doesn't rely on names.
4. **Docs** — finish `/docs` (architecture, protocol, RE methodology) per CLAUDE.md.
