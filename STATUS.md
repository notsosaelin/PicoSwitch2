# STATUS.md

> Living snapshot of the repository's current state. Not a roadmap (see `PLAN.md`),
> not project guidance (see `CLAUDE.md`). Update whenever significant work lands.

---

# Project Summary

**Project:** PicoSwitch2 — bridge Bluetooth controllers to a Nintendo Switch 2 as a **native Switch 2 Pro Controller**.

**Emulated device:** Switch 2 Pro Controller — USB VID `0x057E` / PID `0x2069`, bcdDevice `0x0210`.

**Boards:** Pico W (RP2040) and Pico 2 W (RP2350), one source tree.

**Current Branch:** `ns2-testing`

**Latest milestone:** Two hardware passes on the same day. **Test 1** found report-0x05 axes
pitch/roll-swapped (yaw correct) and report-0x09 drifting while stationary; both were fixed
(axis order re-derived from the genuine controller's own capture; a stillness-gated bias tracker
added). **Test 2** confirmed the report-0x05 axis fix (Steam calibration now works) but found the
report-0x09 bias-tracker fix made **no observable difference** — root-caused to a self-defeating
stillness gate (tested raw magnitude, which a MEMS gyro's own bias can exceed, so the gate likely
never opened). Redesigned the gate around steadiness (frame-to-frame derivative) instead of
magnitude, and added a live bias/gate debug readout so the next test can confirm the mechanism
directly — **pending hardware test 3.**

**Last Updated:** 2026-07-10 — **Strategic pivot: `anom=0` result narrows, doesn't solve, the
report-0x09 symptom; investigation shifted to systematic BLE reverse-engineering of the genuine
controller.** The mathematically-derived local-discontinuity detector (previous update) was run:
`anom` stayed 0 while the console still showed abrupt camera jumps. This weakens (does not
eliminate — see the outcome table it was designed against) the theory that our own phase
computation has a local defect, but it does **not** validate this repo's report-0x09 value
semantics (accumulator vs. bounded sample, still open). Per explicit direction: stopped treating
report 0x09 as a mostly-understood encoder needing another mathematical fix. **New work this
pass:** a non-invasive, timestamped raw-capture facility for the genuine controller's **BLE**
traffic (`src/bt_hid/sw2_capture.c`, config-mode `sw2cap on/off/stat` commands) — closes a
concrete, named blind spot (`switch2_ble.c` receives but never reads report bytes 16-59, where
motion data lives per third-party decodes) without changing any parsing/routing behavior. Full
field-level inventory (confirmed/strong-evidence/hypothesis/unknown), a controlled experiment
matrix, a BLE-vs-USB evidence comparison, and a concrete assessment of `Dycool/Usb-relay-for-NS`
for the still-missing console-side USB capture: new doc
[`docs/switch2/ble-controller-protocol-inventory.md`](docs/switch2/ble-controller-protocol-inventory.md).
Both boards build clean. **No BLE capture has been run yet** — this pass built the instrument,
not the data.

**Practical-usability fix, same day:** the CDC-terminal capture facility above produced no
usable output in practice — `sw2cap on` gave no visible result. Root cause was structural, not
incidental: the original design auto-streamed capture lines onto CDC unprompted, which the config
web UI's `sendCmd()` can't tolerate (it matches replies to requests by strict arrival order; an
unsolicited line between a command and its reply is indistinguishable from that reply). Redesigned
as a pull-based `sw2cap drain` command (batched JSON, up to 16 entries/call) and added a full
**"Switch 2 BLE Capture" panel to the config web UI** — Start/Stop/Clear/Download NDJSON, live
status polled automatically, per-kind and per-handle/length tallies, a bounded live view with
unbounded in-browser session retention for export, and filters (including one-click `0x000A`/
`0x000E`). Both boards build clean; embedded web filesystem regenerated. See
[`docs/switch2/ble-controller-protocol-inventory.md`](docs/switch2/ble-controller-protocol-inventory.md)
§2.3-2.5. **This fix worked** — see below.

**First real BLE captures + full byte-level analysis, same day, next.** Four sessions captured
through the new panel (`STILL_CAPTURE` + three fixed orientations, ~530 s total, `dropped=0`
throughout — confirming the pull-based redesign works end-to-end). Programmatic analysis (new
tools `tools/analyze_sw2_capture.py`/`analyze_sw2_fields.py`) of the **complete** files, not
samples, found: every one of `0x000A`'s 63 report bytes is accounted for as a free-running
counter, one of two analog-stick 12-bit pairs, or constant zero — **none respond to orientation**,
and this held under a tested (not assumed) ±1-byte alignment shift too. This refutes "motion is
present in `0x000A` at a different offset," but can't yet distinguish "motion needs an
explicit enable command" (this repo's BLE init never sends one, unlike the reference
`switch2_input_viewer.py` tool, confirmed by exhaustive grep) from "this report never carries
motion." Per the task's explicit trigger condition, that ambiguity produced a smallest-possible
**opt-in, off-by-default experiment**: `sw2cap experiment on` arms a one-shot, post-init attempt
to subscribe to the unverified `0x000E` handle and send an evidence-supported feature-enable
command, logging everything. Both boards build clean; embedded web filesystem regenerated.

**Experiment run, same day, next: `0x000E` reachable, still no orientation data.** The armed
experiment fired on a controller power-cycle (`dumps/sw2_capture_2026-07-10-EXPERIMENTAL.ndjson`,
2,371 records). Positive result: `0x000E` is real and responds — the `configure`/`enable` command
pair was ACK'd and a continuous ~33 Hz notification stream (2,331 records) began arriving on a
handle that had never produced anything before. Negative result: that stream's 63-byte content is
a byte-shifted **duplicate** of `0x000A`'s buttons+sticks payload (confirmed byte-identical stick
fields at a 5-byte offset) — offsets past the stick data are still constant zero. Diffing our
command sequence against the reference tool's actual working init flow
(`tools/switch2_input_viewer.py`) found three untested differences that could plausibly matter: a
`0xFF` (not `0x07`) `configure` value, six intervening SPI calibration reads, and a
previously-undocumented descriptor write ("report rate") after `enable`. Full findings:
`docs/switch2/ble-controller-protocol-inventory.md` §3.6.

**v2 experiment matrix + GATT discovery tool built, same day, next.** Rather than combine the
three untested differences into one opaque v2 attempt, each is now an independently-selectable,
one-shot variant (`sw2cap variant 1-6`: control, mask-0xFF-only, handle-write-only, mask+write,
calibration-sequence, full-sequence — the last also deferring the `0x000E` CCC subscribe to the
end, matching a previously-unnoticed ordering difference in the reference tool's actual connection
flow). Before writing that code, re-derived the "report rate" descriptor's raw handle exactly as
asked: found and corrected a real arithmetic error from the prior pass (it's `0x000C`, not
`0x000D` — the earlier value double-applied an offset that only applies to characteristic
value/declaration pairs, not descriptors) via careful reasoning cross-checked against this repo's
own confirmed handles — then, since that's still reasoning on paper, built a proper **one-shot GATT
discovery tool** (`sw2cap gattdisc on`) that walks BTstack's own live discovery of every service,
characteristic, and descriptor on the connection, for ground truth instead of more arithmetic.
Both boards build clean; embedded web filesystem regenerated. Full design, the handle-numbering
correction, the variant table, and the ordered capture procedure:
`docs/switch2/ble-controller-protocol-inventory.md` §2.7, §3.7, §9.

**v2 run on hardware, same day, next — major result: first independent, motion-consistent data
this project has ever observed from a genuine controller.** All 8 captures (GATT discovery + all
6 variants + baseline) analyzed completely with a new tool
(`tools/analyze_sw2_v2_captures.py`). **GATT discovery resolved `0x000C`** with real ground truth:
it's a vendor-specific descriptor of the `0x000A` characteristic (confirming the pre-hardware
paper correction), not `0x000E`'s — which has its own untested equivalent at `0x0010`; several more
undocumented characteristics were found (`0x0018`/`0x0022`/`0x0026`/`0x002A`/`0x002C`/`0x002E`/
`0x0032`), unexplored. **Every one of the six variants — including the plain control — produced a
real, independent 40-byte data block on `0x000E`** (offsets 14-54): absent from every earlier
capture, independent of buttons/sticks, structurally self-describing (a constant length-prefix
byte reading exactly 40, matching the reference tool's documented "Pro/GCN 40-byte motion block"
length), and behaviorally distinct from every counter/duplicate pattern already characterized
(smooth monotonic drift, not noise). **Central unresolved question**: because every variant
succeeded — including the control, whose commands are byte-identical to the earlier standalone v1
run that produced *zero* activity across 70+ seconds — none of the six tested differences (mask
value, calibration reads, the handle write, subscribe-timing) is the actual cause. No orientation/
gyro/accelerometer semantics assigned to any byte. Full report with the causal table, ranked
hypotheses, and the proposed next experiment (a stationary-only re-run of variant 1, no new code):
`docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md`; summary in the inventory doc §3.8.

**Continuation, same day, next — block interpretation + a concrete timing-difference candidate.**
User clarified variant 1 (all six, in fact) were already captured stationary — resolving the prior
follow-up proposal without a new run and ruling out incidental handling as the explanation. Priority
shifted to "what does the block encode": a new tool (`tools/analyze_sw2_motion_block.py`) ran an
exhaustive alignment/width/endianness/signedness scan across all six variants and found two
genuinely accumulator-like 2-byte fields — one (raw `0x13`-`0x14`) wraps through the full int16
range every ~4 seconds (~16,000 counts/s implied rate, structurally matching this project's own
report-0x09 phase-accumulator architecture), the other (raw `0x19`-`0x1A`) drifts smoothly across
the entire session with no wraparound. No semantics assigned to either. Separately, comparing the
prior (negative) session against this session's variant 1 operation-by-operation found a **real,
measured difference**: the old v1 code fired its CCC-subscribe/configure/enable writes within
1.2ms of each other with no ACK-gating (the device's `configure` ACK arrived ~39ms *after* `enable`
had already been sent); the current v2 code waits for each step's real confirmation. Leading
candidate explanation, not confirmed. Implemented (both boards build clean): a capture-annotation
marker (`sw2cap mark <text>`, new web-panel control) and a fix for a real instrumentation gap
(CCC/descriptor-write completion status is now captured, not just printed) — both pure logging,
verified not to change any BLE timing/behavior. A controlled-motion capture protocol (pitch/yaw/
roll, isolated rotations + fixed holds + baselines, marked phase boundaries) is designed and ready
to run — not yet executed. Full detail: `docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md`
§8-§12; summary in the inventory doc §3.9.

**Controlled-motion capture analyzed, same day, next — block-wide activity signature confirmed.**
User supplied `sw2_capture_2026-07-10_MOTION-CLASSIFICATION.ndjson` (all 44 phase markers present).
Direction-correlation test on the two candidate accumulator fields (raw `0x13`-`0x14`, `0x19`-`0x1A`)
was **negative** — neither reproducibly tracks labeled pitch/yaw/roll rotation direction; a
systematic scan of every possible 2-byte field found nothing better than chance-level effect sizes.
**But reframing the question found a strong, cross-validated positive result**: ~11 distinct byte
offsets spanning nearly the whole 40-byte block show a 100-1000× jump in residual noise / byte-
transition-rate the instant the controller is handled at all — deliberate rotation *or* a
nominally-still "hold" — vs. near-total silence during confirmed genuine rest (one byte frozen
99.7% of the time at rest, changing 70% of the time during rotation). The nominally-still hold
phases show inconsistent, intermediate noise levels across offsets/axes — the signature of
variable involuntary hand tremor, not a fixed motion/still flag. Classified as a new, empirically-
derived category ("activity/vibration-responsive noise floor") rather than forced into rate-like/
orientation-like/acceleration-like — **no axis, unit, or scale assigned to any byte.** Strengthens
the case the block is genuinely IMU-derived; does not identify which bytes are which sensor/axis.
Full detail: same report §13; inventory doc §3.10.

**Structural decomposition against a documented IMU FIFO layout, same day, next — no candidate
survives.** Per explicit direction, paused new hardware and investigated the block's internal
packet structure before requesting the hand-held test above. The controller's IMU is identified
elsewhere in this repo as an ICM-42670-P; tested whether the 40-byte block matches that chip's
documented FIFO packet format (2×20, 4×10, 5×8, 8×5 candidate splits) using a new tool
(`tools/analyze_sw2_block_structure.py`). **Confidence caveat stated explicitly**: the byte-level
layout tested is from general/trained knowledge of the shared ICM-42xxx family, not a directly-
consulted ICM-42670-P datasheet (no document access this session). An entropy-periodicity test
(the signature a repeating packet header would leave) found **no meaningful match at any tested
period** — negative, not merely absent evidence. A physically-grounded orientation-invariant-
magnitude test found one promising-looking candidate, but it's confounded by an already-known
non-physical accumulator byte, so it's inconclusive rather than confirmatory. The block-wide
activity/noise-floor finding (previous entry) remains the strongest evidence something real is
present; its exact internal packing is now confirmed not to match the layouts tested. Proposed
(not requested from the user yet): a mechanically-supported, non-handheld experiment — table taps
and a vibration source with zero skin contact — that separates "responds to genuine vibration"
from "responds to hand contact for some other reason," with expected results defined per model in
advance. Trigger investigation kept explicitly separate, not touched this pass. Full detail: same
report §14-§15; inventory doc §3.11.

**🔴 BLE 40-byte-block semantic decoding PAUSED, same day, next.** Every passive-statistical
approach tried across three passes (direction correlation, orientation-invariant scalar/vector
interpretation, periodic native-FIFO packet structure) failed to converge — per explicit
direction, no further indirect physical tests will be requested against this dataset. **What
remains true, not retracted**: the block is independently framed, responds strongly and
reproducibly to physical handling, and its internal encoding/semantics are genuinely unknown.
Captures and tooling preserved (`docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md`,
now carrying a pause banner). **This does not pause gyro or controller RE as a whole** — re-centered
on the actual project target instead (next entry).

**Re-centered on genuine console-side USB report 0x09, same day, next.** Full implementation-level
feasibility audit of `Dycool/Usb-relay-for-NS` — the one candidate that could finally close this
project's oldest, most consequential evidence gap (every existing report-0x09 fact traces back to
one static, unrepeatable third-party capture). Read the actual repository source this pass (not a
prior summary): the relay's feature-enable commands (`0x0C` configure/enable, mask `0x27`) match
this project's own documented bytes **exactly** — a genuine cross-validation. But the Pi-side
gadget uses a generic Linux `configfs` HID function with **no EP0/control-transfer relay at all** —
a real, code-confirmed gap, and a serious risk given this project's own hard-won knowledge that the
console requires a byte-exact vendor handshake to accept a device. **Recommended path: skip the
full 3-node relay for now** — a much smaller, lower-risk alternative (running the Windows-side
`hidapi` capture logic directly against the genuine controller, no Pi or console needed) can
already test whether the documented feature-enable command unlocks motion data on its own. Full
go/no-go report, validation ladder, and required fixes:
`docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`. A ranked inventory of remaining
unknown controller surfaces (prioritization only): `PLAN.md` "Controller surface inventory."

**Strategic priority change, same day, next: gyro formally PAUSED; shifted to systematic RE of
remaining genuine Pro Controller 2 features, starting with NFC/amiibo.** Not abandoned — deferred
until broader controller RE or new primary evidence changes the situation (explicit direction).
First pass on the new bounded subsystem: re-mined this repo's own `usbpcaptures/genuine_procon_2.pcapng`
(164,242 packets) with a new reusable tool (`tools/extract_nfc_traffic.py`) for command-`0x01`
(NFC) traffic, and traced two real, genuine-Pro-Controller-2 request/response exchanges to exact
packets — subcommand `0x0C` (response `61 12 50 10`, confirming this is the exact provenance of
`switch_pro2.c`'s already-hardcoded value) and subcommand `0x01` (a previously undocumented bare
acknowledgment, `dir=0x04`, not in `ndeadly/switch2_controller_research`'s own subcommand table).
Found one real, precise self-consistency gap worth a future validation pass: `switch_pro2.c`
hardcodes response `dir=0x01` for every command unconditionally, but the one genuine bare-ack NFC
response observed used `dir=0x04` — not fixed this pass (analysis/documentation only, no NFC
emulation changes, per explicit constraint). Full confidence-qualified inventory (six claims kept
separate: official Pro-Controller-2 NFC confirmation vs. Joy-Con-2/Pro-Controller-2 hardware ID vs.
protocol behavior per controller type vs. Switch-1 carryover, the last explicitly refuted at the
command-ID level): `docs/switch2/nfc-protocol-inventory.md`. No NFC IC identified in either
controller; no real amiibo tag transaction observed in any capture this project holds.

---

# Current Objective

**NFC/Amiibo reverse engineering — new active objective, 2026-07-10.** Gyro/report-0x09 is
formally **paused** (not abandoned) pending new primary evidence — see "Deferred / Blocked" below
for its full preserved status. Active work has shifted to systematic RE of remaining genuine
Switch 2 Pro Controller features, using NFC as the first bounded subsystem (feature existence
officially confirmed by Nintendo; protocol/hardware/init/Switch-1-compatibility treated as unknown
until evidenced). First pass: two real command-`0x01` exchanges traced to exact packets in this
repo's own genuine-controller USB capture (§ above). Full inventory, evidence-per-claim table, and
the next recommended capture/analysis task: `docs/switch2/nfc-protocol-inventory.md`.

### Gyro (v1.1) — 🔴 paused, preserved for context

History: the mathematically-derived anomaly
detector reported `anom=0` on real hardware while the console still showed abrupt jumps (rules out
a local computation defect, doesn't validate report-0x09's value semantics) → pivoted to
genuine-controller BLE reverse-engineering, which found and characterized a real, independent,
40-byte activity-responsive data block on BLE handle `0x000E` but could not decode its internal
semantics after three full analysis passes (direction correlation, orientation-invariant vector
interpretation, periodic native-FIFO packet structure all failed to converge) → BLE-block decoding
paused → re-centered on a genuine console-side USB report-0x09 observation path via a feasibility
audit of `Dycool/Usb-relay-for-NS` → **now formally paused in full** (this section) pending new
primary evidence or broader controller RE (i.e. NFC or other subsystems) surfacing something that
changes the picture. See
[`docs/switch2/ble-controller-protocol-inventory.md`](docs/switch2/ble-controller-protocol-inventory.md)
for the full BLE field-level inventory (still accurate, just not active work) and
[`docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md`](docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md)
for the paused block-decoding work. The narrative below (two hardware passes, the
debug-instrumentation bug, the take-2 gate confirmation, the anomaly detector) is preserved as-is
for context on how this conclusion was reached — it remains accurate, just no longer the active
work.

---

### Historical narrative (report-0x09 encoder investigation, superseded as the active objective)

report-0x09 is rewritten to the corrected int32 format (three phase accumulators + three Q16.16
accel) with the `0x0C` enable-gate, and report-0x05's frozen-timestamp/scale bugs are fixed
(Experiment A).

**Test 1** confirmed the transport/pipeline works (Steam detects gyro; Splatoon accepts the
negotiated feature) but found two math bugs: report-0x05's axes pitch/roll-swapped (yaw already
correct), and report-0x09 drifting while stationary (gyro bias integrating unbounded). Fixed:
axis order re-derived from the genuine controller's own report-0x05 capture (`ns2_seam.c`); a
stillness-gated bias tracker added before phase integration (`ns2_build_report()`).

**Test 2** confirmed the report-0x05 fix (Steam calibration now produces usable motion) but found
the report-0x09 bias-tracker fix changed **nothing** — Splatoon/Zelda still unusable. Root cause:
the stillness gate tested raw gyro *magnitude*, which a MEMS gyro's own constant bias can plausibly
exceed on its own, making the gate self-defeating (never opens → bias never adapts → identical to
no fix). Fixed: gate redesigned around *steadiness* (frame-to-frame derivative) instead of
magnitude, decoupling it from the bias's absolute size; the bias estimate and gate state are now
exposed live via the config-mode `imu` debug command so the mechanism can be confirmed directly
rather than inferred from in-game symptoms. Also refuted a magnetometer/9-axis-IMU research lead:
the format's 30-byte budget is fully accounted for with zero spare bytes, so there is no room for
extra sensor lanes in this report. Build-clean.

**Debug-readout audit (same day, after take 2):** the user tested the take-2 fix's debug output in
config mode — `bias=[0,0,0] still=0`, unchanged, on a live motion-feeding DualSense. Root cause was
**not** the gate itself: `ns2_build_report()` (the only code driving the tracker) never runs in
config mode (`usb.c`'s main loop skips it unconditionally there), so the debug fields were dead
static memory, not a real measurement. Fixed by extracting the tracker into `ns2_motion_tick()`
(now independent of `ns2_imu_enabled`/`ns2_streaming`) and adding a rate-limited
`ns2_motion_debug_tick()` called from config mode — the gate is now directly observable by
connecting a DualSense in config mode alone, **no console needed**.

**Gate check (same day, next): CONFIRMED WORKING.** DualSense read `still=1` stationary / `still=0`
moving — exactly correct. (Genuine Pro Controller 2 read `still=0` always — the already-documented
`switch2_ble`-discards-its-own-motion gap, confirmed by code inspection, not new.) The gate is no
longer a suspect.

**Symptom reclassified (same day, next): abrupt multidirectional jumps, not gradual drift.** A
confirmed-working stillness gate can't explain discontinuous jumps (bias drift is definitionally
smooth) — this rules out "just needs the gate to work" as the full story and **rules out jumping
straight to an accel-based complementary filter**, which targets gradual drift specifically. New
evidence: a carefully validated third-party decode of the Switch 2's *native BLE* motion format
(`docs/experiments/switch2_native_motion_map_DyCOOL.md`) shows that sibling report uses raw,
individually-clamped gyro **samples**, not an unbounded accumulator — a concrete alternative to
this repo's phase model, though its own author leaves report 0x09 itself unverified, and report
0x09's own prior worked-example evidence still favors the accumulator model on its own terms.
Gyro-scale mismatch was checked and ruled out (`event->gyro_range = 2000` for DS4/DS5 already
matches the `16.384 LSB/dps` constant in use). New instrumentation exposes the phase accumulator
itself (`phase=[...]` on the config-mode `imu` line) so it can be watched directly for
discontinuities, with no console — **this is the next hardware step, before the in-game
Splatoon/Zelda re-test**, since it distinguishes "our own math jumps" from "our math is smooth but
the console expects something else."
See [`docs/experiments/gyro-hardware-validation-2026-07-10.md`](docs/experiments/gyro-hardware-validation-2026-07-10.md) §9-12
and [`docs/switch2/report-0x09-motion.md`](docs/switch2/report-0x09-motion.md).

**Anomaly detector built and run (same day, next).** A per-tick phase-delta bound was
mathematically derived (not chosen) from `ns2_motion_tick()`'s own explicit clamps — ~763.5M raw
units (~64.0°), the largest increment the arithmetic can produce without a computation defect —
and checked every tick, with full-context capture on a trip (new `imuanom` config command).
**Run on hardware: `anom` stayed 0** while the console still showed abrupt jumps. Per the
detector's own outcome table (`gyro-hardware-validation-2026-07-10.md` §14.5), this rules out
outcome 1 (local phase discontinuity) for the observed session and points toward outcome 3
(the generated report is internally continuous; the discontinuity, if real, is in console
interpretation of a correctly-computed-but-wrong-semantic value, or in USB transit this
instrumentation cannot see) — **not** proof of either, since outcome 2 was already structurally
excluded by code inspection and outcome 3 isn't directly observable from this device alone. This
result is what motivated the strategic pivot at the top of this section: further encoder changes
have no discriminating evidence to act on, so the active work moved to genuine-controller BLE
capture instead. See `docs/switch2/ble-controller-protocol-inventory.md`.

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
| Gyro (report 0x09) | 🔴 | **Paused 2026-07-10** (not abandoned) pending new evidence. int32 phase + Q16.16 accel, `0x0C`-gated — format itself well-validated. **HW test 1:** drift found → bias tracker added → **HW test 2: no improvement** → root-caused to a self-defeating (magnitude-based) stillness gate → **fixed (take 2, derivative-based gate + live bias/still debug readout)**. `anom=0` on hardware ruled out a local computation defect without validating value semantics. See `docs/switch2/report-0x09-motion.md`. |
| NFC / Amiibo | 🔵 | **New, 2026-07-10 — active RE target.** Command `0x01` confirmed real on genuine Pro Controller 2 (two exchanges traced to exact packets in this repo's own USB capture); no tag transaction ever observed; no NFC IC identified in either controller. See `docs/switch2/nfc-protocol-inventory.md`. |
| Gyro (report 0x05 / Steam) | 🟢 | Experiment A fix (timestamp + scale) holds on HW: Steam detects gyro. **HW test 1:** pitch/roll swapped → **fixed** (axis order re-derived from genuine capture, `ns2_seam.c`). **HW test 2: confirmed working** (Steam calibration now usable). Roll sign still unverified independently. `docs/experiments/gyro-hardware-validation-2026-07-10.md`. |
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

- **Gyro (🔴 0x09 encoder-repair paused — reframed as genuine-controller BLE RE / 🟢 0x05
  working).** Report 0x05 (Steam) is confirmed working on hardware after the axis-order fix.
  Report 0x09 (console): after two fix attempts, a debug-instrumentation bug, a confirmed-working
  stillness gate, and a mathematically-derived local-anomaly detector that reported `anom=0` on
  hardware (while the console still jumped) — **no further report-0x09 encoder changes are
  planned without new evidence.** The `anom=0` result rules out a local computation defect as the
  explanation (previously the leading theory) but does not validate this repo's report-0x09
  value semantics (accumulator vs. bounded sample — still genuinely open). **Active work shifted
  2026-07-10 to systematic BLE reverse-engineering of the genuine controller**, closing the
  concrete blind spot where `switch2_ble.c` receives but never reads report bytes 16-59 (where
  third-party decodes place motion data): a new non-invasive, timestamped raw-capture facility
  (`src/bt_hid/sw2_capture.c`, config-mode `sw2cap on/off/stat`) plus a full field-level inventory,
  controlled experiment matrix, and BLE-vs-USB evidence comparison — see
  `docs/switch2/ble-controller-protocol-inventory.md`. **No BLE capture has been run yet.**
  A promising but unproven (v0) USB relay tool, `Dycool/Usb-relay-for-NS`, remains the best
  candidate for finally getting a repeatable, controllable genuine report-0x09 (USB) capture —
  see that doc §6 for the full assessment. Full report-0x09 encoder history:
  `docs/experiments/gyro-hardware-validation-2026-07-10.md`.
- **BT pairing reliability (🟡).** Pro Controller 2 reconnect can need a triple-tap and remains flaky.
  Not yet root-caused on our own BTstack path. External research (2026-07-10,
  `Dycool/NS-PC-Control`, a different Switch-2-bridge project using BlueZ) surfaced three concrete
  candidate mechanisms our stack hasn't been audited against: a per-device reconnect cooldown (they
  use 5s, to avoid hammering a device mid-reconnect), an explicit "fast-connectable"-equivalent
  policy for the HID service specifically, and an explicit disconnect-on-console-suspend instead of
  waiting out a generic link-loss timeout. Not yet verified applicable here. See
  `docs/bluetooth/btstack-implementation.md` "Reconnect reliability".
- **BLE DIS VID/PID (🟡).** The PnP VID/PID frequently arrive as 0; detection is worked around by device
  name + report length. A proper DIS/SDP resolution fix would be cleaner.

# Out of Scope (confirmed)

Wake-console-over-USB (Switch 2 wakes only on a BLE advert from a bonded controller; our dongle is
USB). Sharpened 2026-07-10 (external research, `docs/bluetooth/btstack-implementation.md` "BLE
wake-from-sleep"): the real mechanism requires raw-HCI-level MAC spoofing to impersonate a
specific, already-bonded controller's exact identity — not just "any" BLE advert — making this a
meaningfully bigger lift than the one-line summary suggests, if ever revisited.
4-player (single-controller milestone for now); HD-rumble → DualSense haptics; audio over BT controllers.

---

# Reverse-Engineering Progress

- **EP0 vendor identity handshake** — byte-exact vs a real PC2 (validated by MITM capture).
- **report 0x09 (console)** — buttons/sticks/GL/GR/C confirmed on hardware; **motion format corrected**
  to int32 phase + Q16.16 accel and verified on our capture (`docs/switch2/report-0x09-motion.md`).
  Motion is a **negotiated feature** — enabled by the `0x0C`/`0x27` handshake, not always-on
  (`docs/experiments/gyro-experiment-c-results.md`). The `0x0C` enable-mask bits are now individually
  named (buttons/sticks/**IMU**/mouse/battery-current/**magnetometer**), confirmed by
  `tools/switch2_input_viewer.py`'s `FeatureFlagWidget.BIT_FLAGS` and matching
  `TommyWabg/Switch2Connect`'s `FEATURE_MAGNOMETER = 0x80` independently.
- **report 0x05 (PC/common)** — buttons + accel@0x30 / gyro@0x36 int16 (matches TommyWabg's reader).
- **Factory motion calibration** (`0x13040` temp+gyro-bias, `0x13100` mag-bias+accel-bias — all
  `float32`) — decoded 2026-07-10 using `tools/switch2_input_viewer.py`'s field offsets against our
  own SPI dump; values are physically plausible (accel Z-bias ≈ gravity, gyro bias ≈ 0.02, matching
  this doc's "genuine ~0.03 dps" figure). See `docs/switch2/report-0x09-motion.md`.
- **Xbox Elite 2** — 4 paddles captured in report byte 19 (R4=0x01 R5=0x02 L4=0x04 L5=0x08); byte 17 =
  active profile (0=base); paddles only report raw when unmapped in the active profile.
- **DualSense Edge** — Fn/paddle bits in the 3rd button byte (0x10 FnL, 0x20 FnR, 0x40/0x80 paddles), per SDL.
- **IMU chip** = ICM-42670-P (from ndeadly datasheets).
- **New RE asset (2026-07-10):** `tools/switch2_input_viewer.py` — a working third-party PyQt/bleak
  BLE client that pairs with a genuine controller directly (GATT handles `0x000A`/`0x000E` input,
  `0x0014`/`0x0016` command, memory read/write, feature flags, vibration test, live motion plot).
  Confirms the command protocol is byte-identical across USB and BLE transports (only framing
  differs) and reveals a genuine Pro Controller 2's BLE motion reports come in two shapes: **14
  bytes** (handle `0x000A`, all device types — likely one raw `temp+accelXYZ+gyroXYZ` int16 sample)
  and **40 bytes** (handle `0x000E`, Pro/GCN device types — this repo's still-undecoded "length-40
  variant," now at least located; even this tool's own author left it undecoded).

---

# Technical Debt / Notes

- Config-mode `info` still reports `"version":"2.0"` (cosmetic; the web page keys off `id` only).
- `docs/` is being filled out (see PLAN.md documentation milestone).
- The report-0x09 phase-integration scale (`0.72818`, from 16.384 LSB/dps) and initial Z phase
  (`0x80000000`) are still first-cut values, unverified against a moving on-console reference.
- Gyro-bias-tracker constants (`>>8` adaptation rate, ~6 LSB/report jitter threshold for the
  stillness gate) are a reasonable first cut, not tuned against DualSense's actual bias/noise
  characteristics (uncharacterized in this repo). The take-1 constant (±40 LSB *magnitude*
  threshold) was replaced 2026-07-10 after test 2 showed it was likely self-defeating — see
  `docs/experiments/gyro-hardware-validation-2026-07-10.md` §7.

---

# Next Recommended Tasks

1. **NFC — proper report-0x09 endpoint-filtered time series (active objective).** Re-mine
   `usbpcaptures/genuine_procon_2.pcapng` filtering by USBPcap endpoint/transfer-type fields (not
   first-payload-byte, which produced a false-positive collision against USB Configuration
   Descriptors — see `docs/switch2/nfc-protocol-inventory.md` §5) to build a real report-`0x09`
   series across all 164,242 packets and check whether the NFC-state byte (offset `0x0C`) ever
   leaves `0x00` anywhere in the session. Zero new hardware; extends `tools/extract_nfc_traffic.py`.
2. **Genuine-controller USB report 0x09 — run the relay audit's recommended Phase 0 (gyro,
   paused, resume only with new evidence).** Per the full feasibility audit
   (`docs/experiments/usb-relay-feasibility-audit-2026-07-10.md` §7/§9): run
   `Dycool/Usb-relay-for-NS`'s Windows-side `hidapi` capture logic directly against the genuine
   controller over USB — **no Raspberry Pi, no console needed for this step** — to test whether
   its already-cross-validated feature-enable command (`0x0C` configure/enable, mask `0x27`,
   confirmed byte-identical to this repo's own documented command) unlocks report-0x09 motion data
   on its own. Only if that succeeds (or once console hardware access allows) move to the audit's
   Phase 1 (Pi+console enumeration test, no genuine controller needed). **Not the active
   objective** (see "Current Objective" above) — preserved as the documented path to resume gyro
   when new evidence or bandwidth allows. A ranked inventory of other unknown controller surfaces
   (prioritization only, not a queue of new work): `PLAN.md` "Controller surface inventory."
3. **Gyro — report 0x05 roll-sign verification (paused with gyro).** Axis order is confirmed
   working; roll's sign was inferred from a determinant constraint, not measured.
   `gyro-hardware-validation-2026-07-10.md` §4 has a tighter recapture protocol if/when resumed.
4. **BT pairing reliability** — audit `ns2_bt_host.c`'s reconnect path against the three concrete
   candidate mechanisms in `docs/bluetooth/btstack-implementation.md` "Reconnect reliability"
   (per-device cooldown, HID-specific fast-reconnect policy, explicit suspend-time disconnect) —
   Tier 2 in the controller-surface inventory (pure code audit, no new hardware needed).
5. **BLE DIS VID/PID** — resolve the PnP query so detection doesn't rely on names.
6. **Docs** — finish `/docs` (architecture, protocol, RE methodology) per CLAUDE.md.
7. **Gyro — report-0x09 encoder, paused.** Do not resume without new evidence. BLE-derived
   value-semantics hypotheses are no longer expected from item 2 (BLE-block decoding itself is
   paused) — the console-side USB relay audit (item 2) remains the documented path to real
   evidence whenever gyro work resumes; apply anything it eventually yields as a *hypothesis* to
   test, not a fact to encode directly.
