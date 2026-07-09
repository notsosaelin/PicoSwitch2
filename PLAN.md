# PLAN.md — PicoSwitch2 Roadmap

> **Where are we going?** (For *where we are*, see `STATUS.md`; for *how we work*, see `CLAUDE.md`.)

PicoSwitch2 turns a Raspberry Pi Pico W / Pico 2 W into a bridge that pairs Bluetooth
controllers and presents them to a **Nintendo Switch 2** as a **native Switch 2 Pro
Controller** (VID `0x057E` / PID `0x2069`) — aiming to be as close to indistinguishable
from first-party hardware as practical, and the definitive open-source technical reference
for Switch 2 controller emulation.

---

## Build & flash

```
./build.ps1                      # both boards
./build.ps1 pico_w               # one board
```
Pico VS Code toolchain under `~/.pico-sdk` (SDK 2.2.0, ARM GCC 14.2, CMake 3.31.5, Ninja).
Artifacts: `build/<board>/PicoSwitchWGA-<board>.uf2`. Flash by holding BOOTSEL while plugging
into a PC and dropping the `.uf2`.

After editing `web/index.html`, regenerate the embedded copy before building:
`python tools/make_web_disk.py web/index.html src/web_disk.h`.

**Division of labor:** Claude edits + builds both boards; the user flashes + tests on hardware
(console behaviour can't be verified by Claude).

---

## Architecture (current)

- **core0** — TinyUSB device. Normal mode = Switch 2 Pro Controller (HID + vendor-bulk +
  audio interfaces). Config mode = composite CDC + read-only MSC (serves the web page).
- **core1** — the vendored **joypad-os bthid** stack (`src/bt_hid`) on BTstack/CYW43: Bluetooth
  host to the controllers, plus LED, BOOTSEL gestures, pairing, rumble/lightbar, and settings flash.
- **Seam** — `src/bt_hid/ns2_seam.c` maps each driver's unified `input_event_t` (JP buttons) →
  the Switch 2 wire format via the per-family remap, and publishes it cross-core through `report.c`.
- **NS2 protocol** — `src/switch_pro2/` (descriptors, EP0 identity handshake, command channel,
  report 0x09/0x05, rumble). Spec: `docs/switch2/usb-spec.md`.

bluepad32 has been fully retired (joypad-os is the sole BT stack).

---

## Milestones

### ✅ v1.0 — first release (current)
Native Switch 2 Pro Controller: console detection, all buttons incl. **GL/GR/C**, sticks, D-pad,
**rumble**, **250 Hz** poll. Every joypad-os controller supported (DualSense/Edge, Xbox/Elite 2,
Switch, 8BitDo, …) with extended buttons (Edge paddles/Fn, Elite 4 paddles). Full **config web UI**
(live view, dynamic per-controller menu, **per-device remapping**, lightbar, raw-report debug).
PC/Steam works incl. gyro. Builds on Pico W + Pico 2 W. bluepad32 removed.

### 🔴 v1.1 — motion
- **Console gyro** — decode report-0x09's motion packing (undocumented) and emit it. Blocked on a
  decryptable BLE capture; see STATUS.md. PC gyro (report 0x05) already works.

### 🟡 v1.2 — reliability & polish
- **BT pairing reliability** — fix the Pro 2 reconnect flakiness (triple-tap sometimes needed).
- **BLE DIS VID/PID** — resolve the PnP query properly so detection doesn't lean on device names.
- **Docs** — complete `/docs` (architecture, bt, switch2, RE methodology, experiments) per CLAUDE.md.

### Backlog / longer-term
- **Multi-controller** — lift the single-controller milestone toward 4 players (USB hub confirmed to
  work on the Switch 2; determine the real per-player output path).
- **Advanced haptics** — capability-based translation (HD-rumble ⇄ DualSense) where practical.
- **Switch 2 GameCube controller** — documentation first (analog triggers, unique mapping), then support.
- **Firmware-update passthrough** — receive/store the console's controller update for community archival.

### Out of scope (confirmed)
Wake-console-over-USB (needs a BLE advert from a bonded controller; our link is USB); audio over BT
controllers; HD-rumble → DualSense haptics as a v1 feature.

---

## Reverse-engineering direction

Treat the Pico as a protocol-analysis platform. The config-mode **raw-HID-report** view already lets
us reverse new controllers live. Next instrument: a decryptable BLE sniff path for the console↔PC2
link so the report-0x09 motion (and any future console-side unknowns) can be read directly rather
than guessed. Every experiment belongs in `/docs/experiments` with question → hypothesis → method →
result → remaining unknowns.
