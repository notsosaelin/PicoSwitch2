# Experiment Plan — Gyro RE by Differential Analysis

**Status:** 🔵 partially executed. **Done:** Experiment C (console/report-0x09 — motion is a negotiated
feature; format is int32 phase + Q16.16) → [gyro-experiment-c-results.md](gyro-experiment-c-results.md);
Experiment A (Steam/report-0x05 — frozen timestamp + gyro scale, **fixed**) →
[gyro-experiment-a-results.md](gyro-experiment-a-results.md); first hardware test of both + two
follow-on fixes (axis-order swap, stationary-drift bias tracker) →
[gyro-hardware-validation-2026-07-10.md](gyro-hardware-validation-2026-07-10.md). **Open:** B (canned
real-stream replay), D/E (descriptor/timing), F (mutator/fuzzer), G (Pico host/MITM) — only if
console-side validation needs them. **Goal:** stop guessing the report format; remove unknowns until
the fix is obvious.

---

## 1. The reframe: diff a working reference against the broken unit

We have been treating our device as a black box and *reconstructing* the report-0x09 format from an
old third-party capture, then guessing and hardware-testing. That is the slow way. The load-bearing
fact in the new evidence is:

> **A genuine controller → PC → gyro works. Our dongle → PC → no gyro. Same, fully-capturable host.**

In RE terms we now hold **known-good and known-bad side by side on an observable bus.** The bug is
*their difference*. The entire strategy should shift from "decode the format in the dark" to
"**capture both and diff them until they're identical.**"

### Evidence realignment (what's actually load-bearing)

| Observation | Weight | Why |
|---|---|---|
| Genuine → PC → gyro works; ours → PC → none | **Decisive** | Same host, capturable → the difference is the bug. |
| Our internal state has live IMU (`has_motion=1`, live accel/gyro) | High | The break is **downstream of our state** — in the USB output or how the host consumes it, not the BT parse. |
| Frozen value survives a controller swap | High | Our per-poll state resets on disconnect, so a surviving value is **cached host-side** (Steam), keyed on a field we get wrong (timestamp/cadence/descriptor). |
| Genuine controller *through our dongle* → no gyro | **Low / discard** | `switch2_ble` never parses motion, so that's expected — not evidence of anything deeper. |

**Conclusion:** the question is not "what are the report bytes" (we have a strong decode). It is
**"why does the host engage a genuine device's motion but not ours?"** — descriptors, handshake,
report-id selection, or timing. All of those are visible in a USB capture.

---

## 2. Instrument choice — the Pico's USB superpowers vs. free tooling

The Pico *can* be an analyzer / proxy / recorder / replayer / mutator. But for the PC comparison,
**Windows USBPcap (bundled with Wireshark) does it for free with zero firmware.** Reserve the Pico's
USB tricks for the one bus a PC can't tap: the console.

| Pico-as-X role | Where it wins | Verdict |
|---|---|---|
| USB protocol analyzer | Console↔controller bus (not PC-tappable) | Later; USBPcap covers PC now |
| Programmable USB proxy (MITM) | Console-side handshake + live real reports | Ultimate tool; very high effort — last resort |
| Report recorder | Controlled capture of the real controller | Redundant with USBPcap on PC |
| **Report replayer** | **Bisect content vs emulation with no capture tools** | **High value, low effort — do this** |
| Report mutator / feature fuzzer | Find which fields the host actually cares about | Only *after* a working baseline exists |
| Timing analyzer | Precise console-side cadence | Mostly derivable from the capture |
| Automated A/B validator | Regression-proof "our bytes == real bytes" | Nice-to-have follow-up |

---

## 3. Experiment catalog (priority-ordered)

### P0 — Experiment A: Differential USBPcap capture on the PC (genuine vs. ours)
- **Question:** What differs between a *working* (genuine) and *broken* (ours) USB gyro session on the
  same PC — enumeration/descriptors, control transfers/commands, the reports the host reads, and timing?
- **Why it matters:** This is the whole bug, made observable. It subsumes the descriptor, handshake,
  report-format, and timing sub-questions in one capture, with no firmware.
- **Method:** Wireshark + USBPcap. (1) Plug the genuine Pro 2 into the PC via USB, open Steam gyro view,
  rotate it, capture ~10 s. (2) Plug our dongle in (with a DualSense feeding motion), same steps.
  Diff: device/config/HID-report descriptors; all control (SETUP) transfers; which endpoint/report the
  host polls; inter-report Δt; the motion bytes.
- **Outcomes → understanding:**
  - *HID report descriptor differs* (ours doesn't declare the motion fields / a usage the host needs) →
    the host can't locate gyro → fix the descriptor.
  - *Host issues control transfers / feature writes to the genuine one it doesn't send us (or we NAK)* →
    a missing IMU-enable/handshake step → implement + ACK it.
  - *Host reads a different report / report-id / a GET_REPORT for motion* → we emit motion in the wrong
    place → redirect it.
  - *Report bytes/structure differ from ours* → format bug, now against a **fresh, first-party reference**.
  - *Everything matches but gyro still fails* → a Steam-DB quirk keyed on a subtle field (bcdDevice,
    serial, strings) → narrow to that field.
- **Effort:** Minimal (~1–2 h, free). **Priority: P0 — do first; it likely ends the search.**

### P0 — Experiment C: Re-mine the capture we already own (host→device side) ✅ DONE
- **Result:** ✅ **Motion is a negotiated feature, not always-on.** Full writeup:
  [gyro-experiment-c-results.md](gyro-experiment-c-results.md). The genuine controller streams
  report 0x09 with `motion-len = 0` from power-up (251 reports) and only emits the 30-byte
  motion block after a `0x0C/0x06` + `0x0C/0x04` enable handshake (packets 9483→9814); our
  firmware emits motion always-on (the inverse). Also corrected the capture's identity: it is a
  raw USB-2.0 **wire** capture (Packetry/Cynthion) of a **console** wired-USB session — so the
  Steam / report-0x05 path still needs Experiment A.
- **Question:** In ndeadly's USB capture, what does the *host* send the controller (enumeration, feature
  reports, any IMU-enable), and does motion turn on only after a specific command?
- **Why:** We already parsed only the *input* reports. The command side may reveal an enable step we
  never implement — and it costs nothing (we own the file + a parser).
- **Method:** Re-parse for SETUP/OUT transfers and control endpoints; correlate the first `len=30`
  motion report with any preceding command.
- **Outcome (realized):** a command *does* precede motion-on (`0x0C/0x04`), and the controller
  withholds motion until then. Our replies already match; our *behavior* (always-on) does not.
  **Caveat confirmed:** capture host is a console, not Steam — a lead for the console path,
  proof pending for Steam.
- **Effort:** Minimal (analysis only, no hardware). **Priority: P0 — done.**

### P1 — Experiment B: Canned real-stream **replay** through our dongle
- **Question:** Is the failure in our report **content** or our USB **emulation** (enumeration /
  descriptors / handshake / report-id / timing)?
- **Why it matters:** One test splits the search space in half and needs **no external capture tools.**
- **Method:** Embed a captured real report-0x09 motion stream in firmware; add a build/flag that emits
  those exact bytes (looping, with a progressing counter/timestamp) as report 0x09, ignoring the live
  controller. Test on PC (and console).
- **Outcomes → understanding:**
  - *Replayed real stream → gyro works* → our **emulation is correct**; the bug is purely our report
    content/values/scale. Focus there and A/B our generated bytes vs the canned bytes.
  - *Replayed real stream → still no gyro* → our **emulation is the bug** (descriptors/handshake/
    report-id/timing); report formatting is moot until fixed. **Biggest possible finding.**
- **Effort:** Low–medium (embed ~1 KB + a replay branch in `ns2_task`). **Priority: P1 (do if A/C are
  ambiguous, or in parallel — it's independent of PC tooling).**

### P1 — Experiment D/E: Descriptor + timing sub-analyses (subset of A)
- **D — Descriptors:** USBView on both devices; byte-diff device/config/HID-report descriptors.
  Answers whether the host parses motion via the descriptor. Effort: ~15 min.
- **E — Timing:** From the A capture, compare inter-report Δt and the motion **timestamp** progression.
  The *frozen* symptom makes a timestamp/cadence mismatch a strong suspect (host treats motion as
  stale). Effort: ~30 min analysis.
- **Priority: P1 (fall out of A for free).**

### P2 — Experiment F: Report **mutator / feature fuzzer** (after a baseline)
- **Question:** Which report fields does the host actually require (length byte value, timestamp
  semantics, sample count 30 vs 40, the mystery mag lanes)?
- **Why:** Once *something* produces gyro (from A or B), mutate one field at a time and watch the host —
  this maps the format's real constraints instead of guessing.
- **Effort:** Low per mutation (config flag / build) but N iterations. **Priority: P2 — needs a baseline first.**

### P3 — Experiment G1: Pico as USB-**host** report recorder
- **Question:** What exact bytes does the genuine controller emit for *known, controlled* motions?
- **Method:** TinyUSB host (native or PIO-USB) on the Pico; enumerate the genuine Pro 2; log reports
  over the config CDC.
- **Why lower priority:** USBPcap on the PC yields the same data far more cheaply. **Effort: High.**

### P4 — Experiment G2: Full Pico USB **MITM proxy** (console side)
- **Question:** What does the *console* (not PC) send/expect, captured live between it and a genuine
  controller? This is the only way to see the console-side handshake if the PC fix doesn't transfer.
- **Method:** Pico simultaneously USB host (to controller) + device (to console), forwarding + logging
  + optionally mutating. **Effort: Very high (dual-role USB).** **Priority: P4 — last resort / future infra.**

---

## 4. Shortest path

1. **C now** (free): re-mine the existing capture's command side — rule the enable-command theory in/out.
2. **A (+D+E)**: USBPcap the genuine-vs-ours A/B on the PC. This is the decisive step; it very likely
   names the root cause (descriptor, missing command, report-id, or timing).
3. **B** only if A/C leave content-vs-emulation ambiguous (or run in parallel — it's independent).
4. **F** to nail exact field constraints once a baseline works.
5. **G2** only if the PC fix doesn't transfer to the console.

**Biggest unknown eliminated per unit effort:** A and B. A is free and subsumes four sub-questions; B is
a small firmware change that halves the search space. Everything past B is refinement or console-only.

## 5. Strategy recommendation

Adopt **differential analysis against a live, working, first-party reference** as the standing method
for this project (it *is* meant to become the definitive reference implementation). Concretely: keep a
USBPcap capture of a genuine controller as the **golden trace**, and treat "make our USB session
byte-and-timing-identical to the golden trace" as the acceptance test — for gyro now, and for every
future unknown (rumble, wake, audio). The Pico's programmable-USB (replay → mutate → proxy) is the
escalation ladder for the console bus a PC can't see, used only when the free PC diff runs out.
