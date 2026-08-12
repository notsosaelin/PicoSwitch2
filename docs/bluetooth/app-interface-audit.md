# Phone-App Firmware Interface Audit

**Status:** 🔵 audit (2026-08-12). Purpose: define the complete firmware-side interface the phone
apps consume, so the apps are thin clients and nothing is discovered missing *after* the app is
built. Two app clients share this firmware surface:

1. **Management app / portal** — BLE GATT + newline-JSON command protocol (the in-band management
   channel; see [in-band-management-plan.md](in-band-management-plan.md)). Manages config, amiibo,
   pairing, personality. The Web Bluetooth portal in `web/index.html` already implements this.
2. **Controller-bridge app** — an Android handheld acting as a **Classic Bluetooth HID gamepad**
   (see [android-controller-bridge.md](android-controller-bridge.md)). Sends HID input reports the
   firmware's generic parser consumes.

The two are independent transports; a phone could be both (management central + HID device), but
that is out of first-version scope.

---

## 1. Management interface — command surface

The app-callable set is the **wireless allowlist** (`config_wireless_command_allowed`,
`config_wireless_bridge.c`). Diagnostics (`state/raw/imu/audiostat/sw2cap/btid`) are intentionally
CDC/UART-only and stay off the wireless path.

| Need | Command | Exposed to app? |
|---|---|---|
| Firmware id/version | `info` | ✅ |
| Keepalive | `ping` | ✅ |
| Read config (remap/colors) | `get` | ✅ |
| Set colors | `body` / `jcl` / `jcr` / `lb` | ✅ |
| Connected controller identity + battery | `device` (name, vid, pid, battery, charging) | ✅ |
| Amiibo status | `amiibo status` | ✅ (see §3 for a missing field) |
| **Amiibo import (upload a .bin)** | `amiibo begin` / `chunk` / `commit` | ✅ |
| **Amiibo export / back up the *virtual* tag** | `amiibo read <off> <len>` | ✅ |
| Amiibo slot select / present / eject / clear | `amiibo select/present/eject/clear` | ✅ |
| Flash persist | `save` / `amiibo persist` | ✅ (deferred-path timing per §C6 of the plan) |

### Gaps (management)
| Gap | Impact | Fix | Risk |
|---|---|---|---|
| **G1 — current output personality is not queryable** | App can't show the mode, gate amiibo controls to Pro2, or know the switch target | add `personality` query (read `g_usb_personality`) | **low — read-only** |
| **G2 — output personality switch is not a command** | App can't change output type | `personality <pro2\|gc\|jcl\|jcr>` → target-request flag (mechanism de-risked & owner-hardware-confirmed; §9 of plan) | medium — re-enumeration |
| **G3 — saved-pairing management not implemented** | App can't list/remove bonded phones (only triple-tap wipe-all) | `bonds list` / `bonds remove <n>` (grammar specced in `tools/test_bonds_command.c`) | medium — flash + le_device_db |
| **G4 — real physical amiibo backup is UART-only** | App **cannot back up a real amiibo** — the NFC mirror/initiator (`ns2_nfc_mirror`, incl. `set_initiator`/`initiator_submit`) is reachable only from `ns2_uart_diag.c`, not `config.c` | bridge the mirror to config commands (`amiibo mirror status`/`export`, allowlisted, bonded-only) | **higher — NFC path, real controller timing; validate on HW** |

---

## 2. Controller-bridge interface — the Classic HID contract

Fixed 10-byte input report (report ID 1: 6 axes @1–6, 14 buttons @7–8, hat @9), parsed by the
production generic gamepad driver. Verified green: `test_bthid_android_controller.c`,
`test_bthid_late_identity.c` (phone-CoD binding survives connect + late re-eval).

| Need | Exposed? |
|---|---|
| Buttons (14), two sticks, analog triggers, D-pad | ✅ fixed contract; two devices (Retroid, AYN Thor) map onto it unchanged |
| Phone Class-of-Device → generic gamepad binding | ✅ (no-match fallback, tested) |
| Neutral-on-disconnect (no stuck input) | ✅ (seam publishes neutral) |
| Connected identity surfaced to the management app | ✅ (`device` shows the phone's HID name) |

### Gaps (controller bridge)
| Gap | Impact | Fix | Risk |
|---|---|---|---|
| **G5 — no motion in the contract** | A handheld's built-in **gyro/accel does not reach the Switch 2** — no gyro aiming (Splatoon/Zelda). The generic parser has no IMU path; v1 doc excludes motion. | v2: add a motion block to the HID contract + a generic-parser motion path routing to the seam's `accel/gyro` (which already translates to Switch 2 motion). Biggest single value-add for the bridge. | medium — new descriptor + parser + calibration |
| **G6 — no rumble/output to the phone** | Phone can't vibrate as feedback | v2: HID output report to the Android HID Device | low-med |
| **G7 — 8-bit stick precision** | 16-bit handheld sticks downsample to 0–255 | acceptable for gameplay; note only | none |

---

## 3. Amiibo interface — completeness (your specific asks)

- **Imports — ✅ fully wired.** `amiibo begin/chunk/commit` uploads any 540/572-byte NTAG215 or
  2048-byte figure-v3 image; the portal's IndexedDB library feeds it. Confirmed on hardware
  (compatibility-matrix "ordinary write / v3 write" rows).
- **Back up the *virtual* (currently-stored) amiibo — ✅ wired.** `amiibo read <off> <len>` streams
  the stored image out for the app to save. Live UART export also exists.
- **Back up a *real physical* amiibo — ❌ gap (G4).** The firmware *can* mirror/relay a genuine
  controller's NFC (and even initiate reads via `ns2_nfc_mirror_set_initiator`), but only through the
  **UART diagnostic bridge** — it is **not** on the app-facing command surface. So an end user with a
  real amiibo cannot back it up from the app today.
- **State completeness — mostly, one gap.** `amiibo status` reports loaded/dirty/presented/v3loaded/
  persisted/size/signature/hasSave2/usingSave2/generation/payloadCrc/uid + upload progress. **Missing:
  the figure/character identity** (head/tail or character ID). For an *imported* amiibo the app has
  the .bin so it can derive the name; for a *real-amiibo backup* (G4) the app would need the firmware
  to surface the identity. Add a `figureId` (bytes 0x54–0x5B of the image, or head/tail) to `status`
  when G4 lands.

---

## 4. Prioritized recommendation

| # | Item | Value | Risk | Recommendation |
|---|---|---|---|---|
| G1 | `personality` query | High (app can't gate/display without it) | Low (read-only) | **Implement now** |
| G2 | `personality <target>` switch | High | Medium | Implement after G1; mechanism de-risked |
| G3 | `bonds list/remove` | High (pairing UX) | Medium | Implement with the in-band-management build |
| G4 | Real-amiibo backup over config (+ `figureId` in status) | High (a headline feature) | Higher (NFC path) | Plan + HW-validate; bridge the existing mirror/initiator, don't rebuild |
| G5 | Android-bridge motion (handheld gyro) | High (gyro aiming) | Medium | v2 feature; biggest bridge upgrade |
| G6 | Rumble to phone | Medium | Low-med | v2 |

**Only G1 is implemented in this pass** (read-only, zero-risk, unblocks the app's mode display and
the Pro2-gating recommendation). Everything else is documented here so the app design accounts for it
and nothing is discovered missing later.
