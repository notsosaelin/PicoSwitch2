# Experiment A — Differential USBPcap capture on the Steam PC (genuine vs. ours)

**Status:** ✅ **executed — see [gyro-experiment-a-results.md](gyro-experiment-a-results.md)** (root
cause found + fixed: frozen report-0x05 IMU timestamp + 60× gyro under-scale). This doc is the
capture *method*, kept for reproducing the A/B capture in future.
**Parent plan:** [gyro-differential-re.md](gyro-differential-re.md) (Experiment A).
**Depends on / complements:** [Experiment C](gyro-experiment-c-results.md) (console path, done),
[report-0x09 motion format](../switch2/report-0x09-motion.md) (int32 layout, confirmed).

---

## Why this experiment (what's still unknown after C + other-findings)

Experiment C + `other-findings.md` fully resolved the **console / report-0x09** side: the motion
format (int32 phase + Q16.16 accel) and the enable handshake (`0x0C` feature mask `0x27`). But
the failure the user actually observed was on **Steam/Windows**, and that path is different and
unmeasured:

- Steam may select **report 0x05** (PC profile), not 0x09 — a different motion layout entirely.
- We don't know whether Steam even sends the `0x0C` IMU-enable, or whether it *gates* motion
  parsing on that enable.
- We don't know whether Steam reads Switch 2 Pro motion **at all** over USB.

We hold known-good (a genuine controller) and known-bad (our dongle) on the **same, fully
capturable** PC. The bug is their difference. Capture both, diff, done.

## Tooling (verified present)

- **USBPcap** — installed (`C:\Program Files\USBPcap\`, and Wireshark extcap `USBPcapCMD.exe`).
- **Wireshark / tshark** — installed; dissects `LINKTYPE_USBPCAP` with full `usb.*` fields.
- **Analysis harness** — a session-local analysis script (not retained) (tshark-based; ready, validate on first file).

---

## Protocol

### A0 — Feasibility gate (2 min, no capture) — DO THIS FIRST
Wire the **genuine** Switch 2 Pro Controller to the PC with a **USB-C data cable**. Open Steam →
Settings → Controller (or a gyro-aware game), and rotate the controller.

- **Gyro responds** → PC/Steam *can* read Switch 2 Pro motion over USB. Proceed to A1; the genuine
  session is our golden trace.
- **Gyro does NOT respond** (buttons work, gyro dead) → **PC/Steam does not support Switch 2 Pro
  gyro.** Then the user's Steam-gyro goal is impossible on that path regardless of our firmware;
  pivot the whole effort to **console-only** gyro (implement the int32 format + `0x0C` enable and
  validate on the console). This single 2-minute test can retire the entire Steam sub-problem.

### A1 — Golden trace: genuine controller, wired, in Steam
1. Wireshark → select the **USBPcapN** interface for the hub/root the controller is on
   (if unsure, capture on all USBPcap interfaces; we filter later).
2. Start capture, then: let it sit still ~3 s, then rotate **slowly about each physical axis in
   turn** (pitch, then yaw, then roll), ~3 s each. Stop. Save as `genuine_wired_steam.pcapng`.
   - The still→single-axis motions also help lock down axis order/sign for the console work.

### A2 — Our device: dongle emulating, in Steam
1. Pair a **DualSense** to our dongle (DualSense feeds real motion through `ds5_bt`; this tests
   our *motion pipeline*, not the known `switch2_ble` gap). Plug the dongle into the PC.
2. Same capture + same still/rotate sequence. Save as `ours_dongle_steam.pcapng`.

### A3 — Analysis (Claude, from the two files)
Run `usb_a_analyze.py` on each, then diff:
1. **Enumeration** — idVendor/idProduct/**bcdDevice**, config + HID report descriptors. (Does ours
   match the genuine device the way Steam keys on?)
2. **Control transfers** — any `SET_REPORT`/feature writes Steam sends the genuine one but not us
   (or that we STALL/NAK).
3. **Bulk EP2 commands** — does Steam send the `0x0C` feature-enable (mask `0x27`)? To both? Do we
   reply the same?
4. **Selected report id** — which report Steam polls (0x09 vs 0x05) and from which endpoint.
5. **Motion in the reports** — does the genuine device's motion field change as it rotates, and
   does ours? At what offset/format?
6. **Cadence** — inter-report Δt and the motion timing word progression.

---

## Expected outcomes → how each changes our understanding

| Finding in the diff | Conclusion | Action |
|---|---|---|
| A0 gyro dead on genuine wired | Steam can't do Switch2-Pro gyro | Drop Steam path; console-only |
| Steam polls report **0x09** + sends `0x0C 0x27` | Same mechanism as console | Fix = int32 format + enable-gate + feed motion; done for both paths |
| Steam polls report **0x05** | Separate PC motion path | Decode/fix report-0x05 motion (DualSense-style @0x2A); enable-gate may differ |
| Steam sends an enable/feature we NAK or mis-reply | Missing handshake step | Implement + ACK it byte-identically |
| Descriptors differ (bcdDevice/strings/HID desc) | Steam keys capability on a descriptor field | Match the golden descriptor (⚠ bcdDevice must stay 0x0210 for PC enum — reconcile) |
| Bytes/format differ only | Pure format bug | Match golden bytes |
| Everything matches, gyro still dead on ours | Steam DB quirk on a subtle field | Narrow to that field |

## Constraints / notes
- Do **not** change `bcdDevice` (0x0210) casually — every prior change broke PC enumeration. If the
  diff implicates it, flag it, don't silently change it.
- Capture genuine and ours **separately** (both enumerate as PID 0x2069; simultaneous is confusing).
- A2 uses a **DualSense**, not a genuine Pro 2 over BT — the BT Pro 2 path discards motion
  (`switch2_ble`), which would confound the test.

## Remaining questions this does NOT answer
- Console-side gyro validation (needs the int32 firmware + a console test; see Experiment C §7).
- Exact coordinate signs/axis order (needs the controlled single-axis rotation analysis).
