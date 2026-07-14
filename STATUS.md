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

**Last Updated:** 2026-07-14 — **NSO GameCube Controller: button mapping fully confirmed working on
real hardware. Rumble's original P0 bug (immediate full-strength rumble on GameCube-mode entry) is
Confirmed fixed by real hardware re-test; a follow-up gameplay bug (small rumble ticks smearing
into one continuous buzz) is fixed in code pending re-test; and a foundational correction to the
entire GC rumble byte model — found by reading the real Linux kernel "HID: nintendo" driver source
— is implemented but not yet hardware-tested at all.** The kernel source revealed the genuine
GameCube controller has no continuous-amplitude rumble hardware: it's a simple ON/OFF/STOP ERM
motor, and real hosts fake continuous intensity via rapid duty-cycle toggling, not a single
amplitude byte — this project had been reading the wrong byte as "intensity" since 2026-07-13,
which retroactively explains the entire rumble bug arc across four revisions. Full account:
`docs/experiments/gcusb-rumble-lab-2026-07-14.md` and the new `docs/experiments/
refuted-hypotheses.md` archive. `tools/gcusb` (the PC-side USB protocol lab `PROMPT.md` asked for)
is built, fixed, and confirmed working end-to-end against real hardware — see its own dated
entries below for the full build-out and debugging story. Current next step: flash and re-test
(`DATA.md` §9). Earlier the same week: a real Switch 2 console fully recognizes the Pico as a
genuine GameCube Controller and
streams live input via report `0x0A`, reached after an eleven-pass same-day hardware-debugging arc
(EP0 identity handshake,
real pairing crypto, calibration/feature-flag data, five missing vendor-bulk command families —
see the dated entries below for the full account). The sole known remaining issue — a real 4-way
button-bit rotation affecting Z/L-detent/R-detent/`SWITCH_MASK_ZL` in `switch_gc_encode_report()`
— was root-caused from the console's own Test Input screen feedback and fixed (see the
"eleventh pass" entry below); both boards rebuilt clean, 76/76 golden-test checks pass. **Not yet
re-tested against a real console** — this is the next test. Earlier same-day: Stage B (USB
personality/enumeration) hardware-validated — confirmed on real hardware, genuine controller
connected simultaneously for comparison: clean Windows enumeration as `Nintendo GameCube
Controller` (`057E:2073`), no driver error, real controller unaffected. Two real bugs found and
fixed via that hardware round — missing WinUSB auto-bind (Code 28), and a `bcdDevice` collision
with the genuine controller in Windows' driver cache that was corrupting the *real* controller's
own Steam mapping — see "NSO GameCube Controller output personality" below for the full account. A
runtime `usb_personality_t` (Switch 2 Pro Controller 2 / NSO GameCube / reserved Joy-Con 2 / CDC
config), volatile and BOOTSEL-hold-cycled (not persisted), lets the Pico enumerate as the genuine
`Nintendo GameCube Controller` (VID `057E`/PID `2073`) using Confirmed byte-exact descriptors. Full
four-combination build matrix green; exact hardware test procedure in `DATA.md`. See "NSO GameCube
Controller output personality" further down for the complete implementation narrative and evidence
trail. Also this date, earlier in the same work stream: Stage A (research/architecture) completed —
genuine controller
identity/USB-topology cross-checked live, both reference repos cloned+audited (one critical finding:
SoulCalDan's repo does **not** implement the native NSO protocol at all, it emulates the older WiiU
GC Adapter — re-scoped to ndeadly's repo as the sole primary protocol source), SPI dump analyzed and
cross-validated against documented factory-data addresses. Separately: genuine Switch 2 Pro Controller
BLE identity confirmed clean on real hardware (the exact scenario the transport-mask fix was built
for); a real pairing-window bug found and fixed (explicit pairing mode could silently corrupt an
in-flight connection attempt's timeout bookkeeping, disarming its recovery watchdog). See the
paragraphs at the end of this section (before "Current Objective") for full detail on all three; the
historical narrative below (2026-07-10) is preserved as-is.

**2026-07-10 — Strategic pivot: `anom=0` result narrows, doesn't solve, the
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

**Stub-driver polish pass, 2026-07-12 — NFC `dir` fix, audio-stub hardening, poll-rate bump.**
Gyro/report-0x09 stays paused (unchanged); this pass swept other documented "unknown/stub"
surfaces instead of leaving them idle. Three independent, build-clean changes to
`src/switch_pro2/switch_pro2.c`:
1. **NFC response `dir` byte fixed for bare acks** — closes the validation target identified
   2026-07-10 (`nfc-protocol-inventory.md` §2.3): `ns2_dispatch()` now sends `dir=0x04` (not the
   previous unconditional `0x01`) for NFC (`cmd=0x01`) responses with no payload, matching the
   genuine capture's packet #30532 exactly. Scoped strictly to NFC — not generalized to other
   commands, which are already working on hardware and untouched.
2. **Audio stub hardened (spec-compliance only, still no functional audio).** Read TinyUSB's
   vendored `usbd.c` source directly and confirmed `SET_INTERFACE`/`GET_INTERFACE` on the audio
   interfaces were never actually at risk — the framework provides a mandatory ACK fallback for
   those two requests even when a class driver's `control_xfer_cb` stalls (closes a real "how does
   this behave" unknown that was never written down). The stub now additionally answers Feature
   Unit Mute/Volume `GET_CUR` (UAC1 opcode `0x81`) with static unmuted/0 dB values, purely so a
   host's USB Audio class driver doesn't see failed control transfers if it ever binds — explicitly
   not a functional-audio claim; `SET_CUR`/`RANGE`/streaming still stall.
3. **USB poll rate raised 250 Hz → 1000 Hz** (`bInterval 4→1` on IF0's HID IN/OUT endpoints, both
   `NS2_AUDIO` descriptor variants) — a deliberate, labeled latency-over-fidelity deviation from the
   genuine controller's own cadence (same category as the existing tracked `bcdDevice` deviation).
   **Untested on console hardware** — `bInterval` has never been implicated in any past console
   compatibility gate in this project, but nothing about a descriptor field has been safe to assume
   without an actual hardware pass here, so this is not yet promoted to fact. Verified in the actual
   flashable `.uf2` for both boards (not just the ELF — a prior pass found the UF2 generation step
   can go stale on an incremental build). **Side-fix required and applied:** the report-0x09 motion
   tracker's bias/low-pass/jitter EMAs are tuned per-call, not per-elapsed-time, so simply raising
   the poll rate would have silently sped up the paused gyro tracker's (already first-cut,
   unvalidated) adaptation constants too. Both callers of `ns2_motion_tick()` now go through a
   shared `ns2_motion_tick_gated()` that caps the tracker's own cadence to ~250 Hz regardless of USB
   poll rate — buttons/sticks get the full latency benefit, gyro's paused tuning is unaffected.
   Full detail and the genuine-reference bytes: `docs/switch2/usb-spec.md` §2, §11, §13.
Both boards build clean (default `NS2_AUDIO=ON` and explicit `NS2_AUDIO=OFF`, both verified).
**Needs a hardware pass:** does the console still detect/init the dongle at 1000 Hz poll (no known
reason it wouldn't, but unconfirmed); does perceived input latency improve.

**NFC report-0x09 time series, same day, next — conclusive negative result + two doc-error
fixes.** Ran the analysis task queued since 2026-07-10: built `tools/extract_report09_timeseries.py`,
which parses the real 27-byte USBPcap packet header (endpoint/transfer-type/dataLength fields)
instead of scanning payload bytes, and filters strictly on that. Result: `genuine_procon_2.pcapng`
contains **zero** report-`0x09` records anywhere in its 164,242 packets — it's a PC/Windows session
with the real Pro Controller 2 (USBPcap device 38), which streamed report `0x05` exclusively
(19,554 real samples). Verified by cross-checking the tool's parsed `device`/payload fields against
the four packets already known (from the 2026-07-10 pass) to carry the two real NFC command
exchanges — all four are device 38, and the payload bytes match this doc's own previously-recorded
hex exactly, confirming both the finding and that the new parser is correct. Found and fixed two
incidental documentation errors surfaced in the process: (1) a device-number mix-up — "Pro
Controller 2 = device 7" is a fact about a *different* file (`ndeadly`'s own capture), mistakenly
cross-applied to this repo's own capture in a 2026-07-10 session log entry and from there into
`nfc-protocol-inventory.md`; (2) a mis-read report-select command payload, previously described as
selecting report `0x09` when it actually selects `0x05`. **Consequence, not a dead end:** answering
whether NFC state ever changes on the real console needs an actual console-side USB capture, which
this project has never obtained for *any* purpose — the same evidence gap already blocking
report-0x09 gyro ground truth (`docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`).
Full detail: `docs/switch2/nfc-protocol-inventory.md` §2.5.

**DualSense audio passthrough — new research doc, same day, next.** Per user direction, researched
`awalol/DS5Dongle` (MIT-licensed Pico 2 W firmware) as a candidate reference for a future feature:
bridging the Switch 2's (currently stubbed) USB audio interfaces to a connected DualSense's own
onboard speaker/jack/mic. Finding: DS5Dongle does the structurally identical bridge for a PC host
instead of a Switch 2, using Opus-encoded audio framed inside DualSense BT report `0x39` (speaker +
haptics, 547 bytes) and `0x32` (mic status) — report IDs this repo's own `ds5_bt.c` driver does not
currently parse at all (confirmed by reading it; it only handles report `0x31`, standard
buttons/sticks/rumble/LED/touch). Documented the full protocol shape, Opus settings, USB-side
plumbing, and a concrete work breakdown, plus dependency license status (Opus: BSD-3-Clause,
confirmed, safe to vendor; WDL, used for resampling: **unconfirmed**, needs direct verification
before any vendoring). Flagged the real open design question: DS5Dongle runs its audio codec on a
dedicated core1 loop, but this repo's core1 is already fully committed to the joypad-os BTstack
input stack — whether both fit on one RP2350 core is unresolved and is the load-bearing question
for whether/how this feature is even feasible. **Research only, not started, not the active
objective** — see `docs/switch2/audio-passthrough-research.md`.

**Stereo HD-rumble amplitude, same day, next — real capability gap closed, no new hardware
needed.** Surveying `unmapped-features.md` for the next bounded polish item after the audio
research found that joypad-os's `feedback_rumble_t` (`src/bt_hid/core/services/players/feedback.h`)
already carries independent `left`/`right` motor amplitudes — and even adaptive-trigger fields —
but this repo's cross-core `report.c` seam collapsed HD rumble to one shared scalar before it ever
reached that richer API, so every joypad-os driver with true per-motor output (DualSense, Xbox)
received identical left/right values regardless. Root cause was in **three** places, all with the
same shape: `switch_pro2.c`'s `ns2_hid_out_report()` took the peak of the left/right LRA blocks
instead of keeping them separate; `switch_pro.c`'s `decode_rumble()` (Switch 1 path) did the
identical collapse independently; `ns2_seam.c`'s `feedback_get_state()` duplicated one value into
both `rumble.left`/`rumble.right`. Fixed all three together: `report_set_rumble()`/
`report_get_rumble()` now carry a left/right pair (signature change, all 3 call sites updated, no
compatibility shim kept per this project's own convention). Frequency data is still lost — real
HD-rumble fidelity is a separate, larger "Advanced Haptics" backlog item — this only restores
amplitude stereo separation. Both boards build clean.

**Real, unrelated build regression found and fixed while verifying the rumble change, same day,
next.** Explicitly building `-DNS2_PRO=OFF` (the plain Switch-1 configuration, not exercised by
default `build.ps1`) to confirm the `switch_pro.c` rumble edit was sound surfaced a genuine,
two-day-old **link failure**: `config.c`'s gyro debug commands (`cmd_imu`/`cmd_imuanom`, added
2026-07-10) call `switch_pro2.c`'s `ns2_dbg_*` functions with no `#ifdef NS2_PRO` guard anywhere —
harmless under the default `NS2_PRO=ON` build (those functions exist), but `-DNS2_PRO=OFF` links
against nothing, since `switch_pro2.c`'s entire body is `#ifdef NS2_PRO`. Confirmed via
`git show HEAD:src/config.c` that this predates this session by two days — `build.ps1` never
exercises `NS2_PRO=OFF`, so it went unnoticed. Fixed with matching guards on the two command
functions and their dispatch-table entries; both configurations now verified building clean.

**Command-framing audit against the raw capture, same day, next — real gap found and fixed, plus
a documentation-table ambiguity resolved.** Per the highest-return, no-hardware-needed item in
`PLAN.md`'s "Controller surface inventory" (Tier 1, "checkable directly against `switch_pro2.c`
with no new hardware"): re-cloned `ndeadly/switch2_controller_research` for its raw USB capture
(`captures/usb/rumble-procon-gccon.pcapng.gz`, Great Scott Gadgets USB-LL format — a genuine
console-side session, PC2 = USBPcap-equivalent address 7) and used `tshark`'s frame reassembly to
extract every EP2 vendor-bulk command byte-exact from the wire, superseding the markdown summary
table this repo had been treating as ground truth. **Findings:** three real subcommands confirmed
for the first time (`0x03/0x0C`, `0x0A/0x02`, `0x0A/0x08` — all correctly bare-acked already by
this repo's existing fallback, so no functional bug, just newly-closed "Unknown" gaps); one
suspected bug in `0x0C/0x04`'s response length **ruled out** (the summary table had silently
truncated trailing zero bytes; the raw capture confirms this repo's existing 4-byte response is
correct); one real, previously-undiscovered response byte in `0x0C/0x06` flagged but not fixed
(inconsistent across the only 4 real instances captured — not enough evidence to act on); one real
value conflict at `0x13060` flagged but not fixed (this repo's hardcoded value vs. all-`0xFF` on
the real capture's unit — ambiguous whether that's genuine per-unit variation or a transcription
error, and the existing value is already hardware-validated as non-blocking, so left alone pending
more evidence). **One real gap found and fixed:** `0x13100` (magnetometer + accelerometer factory
calibration, float32×6) was fully decoded back on 2026-07-10 but never actually added to
`ns2_factory_init()` — any real console read there got silent zero-fill instead of the
already-known-correct data. Fixed using this repo's own SPI-dump bytes, cross-validated by this
pass's fresh capture decode (a different physical unit) confirming the console does read exactly
this address and expects real, non-zero, physically-plausible data (near-gravity accel bias on
both units independently). Both boards build clean. Full detail:
`docs/switch2/usb-spec.md` §14.

**`Dycool/NS-PC-Control` freshly re-audited, same day, next — one more real fix, NFC's biggest
capability gap mapped as hypothesis, BT-reconnect/wake evidence refined.** Per explicit direction,
re-cloned this external reference project fresh (commit `a422f4b`, 2026-07-12; the
`joycon-usb-experiments` branch previously audited 2026-07-10 has since merged into `main`) rather
than trusting the prior checkout, and read its current native-Switch-2 implementation
(`server/src/switch2_native.cpp`, `s2_nfc_codec.cpp`, `bluetooth_manager.cpp`) end to end. **One
more real fix, now doubly-sourced**: `0x13060` — left unchanged in the same-day earlier pass
pending a second source — turned out to have one: NS-PC-Control's own factory table carries the
identical finding ("reads back erased (0xFF) on the real unit... addr=0x13060 len=0x20") from a
*different* physical unit's capture. Two independent captures now agree against this repo's
previously-unannotated `4C 09 00 00`; fixed to explicit `0xFF`-fill. **Biggest capability gap
found, not ported**: NS-PC-Control has a complete, working amiibo read/write emulation (NFC
subcommands `0x03`-`0x15`, a 622-byte read-buffer format, a 454-byte write-staging format) that
this project has zero primary evidence of its own for — recorded as a detailed **hypothesis** in
the NFC inventory (not code), since NS-PC-Control's captures aren't bundled/re-checkable, per
explicit instruction not to promote another implementation's constants to facts. **BT
reconnect/wake evidence refined** with more precise values (exact BlueZ policy keys, exact 5s
cooldown, a real exponential backoff schedule; a simpler-than-previously-described wake mechanism)
— materially informs but doesn't yet implement the queued BT-reconnect-reliability audit, since
BlueZ's mechanism has no BTstack equivalent. Firmware-version and calibration-byte differences
found against this repo's already-doubly-confirmed values were correctly *not* copied (treated as
per-unit/per-firmware variance, not errors). Both boards build clean. Full evidence matrix:
`docs/experiments/ns-pc-control-audit-2026-07-12.md`.

**BT pairing reliability — root cause found and fixed, same day, next.** Per explicit, detailed
direction: traced the complete BLE connect/disconnect/reconnect lifecycle in `btstack_host.c` end
to end before changing anything (state transition table, Phase-1 required-questions answers, and
a BTstack-capability audit all written up in full — `docs/bluetooth/btstack-implementation.md`
"Reconnect reliability"). **Root cause, Confirmed from code, not inferred:** the BLE
`HCI_EVENT_DISCONNECTION_COMPLETE` handler captured the HCI disconnect `reason` byte but never
consulted it — every BLE disconnect, including ones the peer initiated on purpose (controller
powered off / explicitly disconnected), triggered the same up-to-5× blind direct-`gap_connect()`
reconnect cascade to the single last-known address (`BLE_CONNECT_TIMEOUT_MS`=10s per attempt, ~50s
worst case), during which scanning is stopped the entire time, so the host cannot discover the
controller (or anything else) reconnecting under a fresh session until the cascade exhausts. Traced
the parallel Classic BR/EDR path (`HID_SUBEVENT_CONNECTION_CLOSED`, used by DualSense/Xbox/8BitDo/
etc.) and confirmed it has **no** such cascade — clean evidence the defect is BLE-specific, matching
the documented "**Pro 2**" symptom naming exactly (the real Switch 2 Pro Controller/Joy-Con 2 use
the BLE path; nothing else does). **Fix:** gate the reconnect cascade on the disconnect reason —
`ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION`/`..._DUE_TO_POWER_OFF` now skip straight to resuming
scan; every other reason (e.g. genuine `CONNECTION_TIMEOUT`) keeps the existing 5-attempt cascade
unchanged. Smallest possible change: one boolean gate + one log line, no new timers or per-device
state, doesn't touch the (separately identified, real, but out-of-scope-this-pass) ~3.6× cadence
error in the unrelated `switch2_retry_init_if_needed()` GATT-init retry, and doesn't touch the
already-correct Classic path. All three configurations build clean
(`NS2_AUDIO=on`, `NS2_AUDIO=off`, `NS2_PRO=off`). **Not yet hardware-validated** — exact 3-step
test procedure (deliberately power off a paired controller, confirm the log shows the new skip
path, confirm prompt reconnect on power-back-on; then confirm the still-should-retry path is
unaffected for a genuine range-loss disconnect) is written up in the same doc section.

**Switch 2 GATT init retry timing — second, distinct BT defect found and fixed, same day, next.**
Per explicit follow-up direction, audited `switch2_retry_init_if_needed()` (the post-link
vendor/GATT pairing-command retry — a different mechanism from the link-layer reconnect fixed
above) before touching it, tracing the complete scheduler call chain rather than trusting the
prior pass's Hz estimate as fact. **Confirmed, not inferred:** there is no literal
`btstack_host_task()`; the real chain is `ns2_bt_host.c`'s self-rescheduling 30ms
`control_timer_handler()` → `bt_task()` → `cyw43_transport_task()` → `btstack_host_process()` →
`switch2_retry_init_if_needed()`, a single confirmed call path with no alternates compiled into
this project. This confirmed the prior pass's ~33Hz-not-~120Hz cadence lead, **and** found a
*second*, independent defect while tracing the full ACK/state-transition surface: the retry
counter was a `static` that was never reset — not on a state transition, not on disconnect, not on
a new connection — so each new pairing step inherited an essentially random remaining budget
(anywhere from ~0 to the full ~1.8s) depending on the counter's phase when the previous step
completed, and retries were **unbounded** with **no recovery path** if a step never got acknowledged
at all (a real, previously-undiagnosable permanently-stuck-connection failure mode, distinct from
the link-layer reconnect case since no disconnect ever occurs here to trigger that fix). **Fixed:**
replaced the call-count modulo with a real `btstack_run_loop_get_time_ms()` deadline that resets on
every command send (fresh or retry) and on disconnect, bounded at 10 retries (~5s), with an
explicit `gap_disconnect()` recovery transition on exhaustion that composes with the reconnect fix
above (a locally-initiated disconnect isn't one of the two "peer disconnected on purpose" reason
codes that fix excludes, so the existing reconnect cascade picks it up automatically). The 500ms
interval itself has **no primary-source evidence** (searched captures, code comments, and the
`NS-PC-Control` audit — none establish it) — preserved as existing project policy per explicit
instruction, labeled 🔵 Hypothesis, not promoted to fact. Also fixed an accidental full duplication
of the "BLE wake-from-sleep" doc section found while editing (copy-paste artifact from the previous
pass, content unchanged). Full state-machine trace, old-vs-new semantics table, and hardware test
plan: `docs/bluetooth/btstack-implementation.md` "Switch 2 GATT init retry timing". All three
configurations build clean. **Not yet hardware-validated.**

**Rumble regression — two root causes fixed, BLE Xbox still broken on hardware, cause not yet
isolated (Gate 1 of a new priority program, still open).** User hardware-reported BT rumble broken
across DualSense, Xbox, and generic/XInput controllers. The leading hypothesis (this session's
earlier stereo L/R rumble refactor) **did not hold up** under full line-by-line diff review — that
change is internally consistent, and the per-side decode offsets are byte-identical to what the old
single-scalar code computed before collapsing to a peak. Tracing the real 10-step path (USB decode
→ `report_set_rumble` → `feedback_get_state` → driver task → send) found two real causes: (1)
**shared, cross-family**: `find_player_index()` (`ns2_seam.c`) returned a raw BTstack
connection-slot index instead of always 0, contradicting its own "single controller; no player
manager" comment — `SWITCH_PRO_MAX_CONTROLLERS` is hardcoded to 4 regardless of `NS2_PRO`, so a
Classic BT device landing in connection slot 1-3 (not slot 0) read a feedback slot that
`ns2_hid_out_report()`'s hardcoded `report_set_rumble(0,...)` never wrote to — silently, identically
across every Classic-BT driver, and unrelated to mono-vs-stereo. Fixed: always returns 0 now,
matching what the comment already claimed. **Note: BLE connections always had `conn_index >=
NS2_SLOTS` and already fell through to the same `: 0` fallback before this fix — this bug never
affected BLE devices, which matters for what's below.** (2) **Independent second defect**: Xbox
Classic BT (`xbox_bt.c`) — `xbox_task()` was a complete no-op with a misleading comment; zero
feedback/rumble code anywhere in the file (confirmed by grep before touching it). Fixed by porting
the sibling `xbox_ble.c`'s logic (same report format assumption, same hardware family per this
project's own header comment) onto Classic BT — **however, see below, that assumption is now in
question.** (3) **User hardware-tested a real Xbox Series controller over BLE after fix (1) and
rumble is still broken.** Since BLE was never touched by fix (1) (see the note above), this is a
genuinely separate, unresolved defect — not a validation gap. Retraced the full BLE output-report
path end to end (previously never actually read, only assumed correct): `bthid_send_output_report()`
→ `bt_send_interrupt()` → `cyw43_transport_send_interrupt()` → `btstack_classic_send_report()`,
which — despite its name — does correctly branch to BTstack's `hids_client_send_write_report()` for
BLE conn_index values. **That routing is confirmed correct, not the defect.** The leading remaining
hypothesis: `hids_client_send_write_report()` only succeeds if the device's own GATT HID report map
declares report ID `0x03` (copied from Classic-BT Xbox tribal knowledge, never independently
verified) as an *Output* usage — this project has never actually inspected a real Xbox BLE report
map. **Not fixed this pass** — guessing a different report ID without evidence would repeat the
exact mistake this task's brief warned against. Instead added two diagnostics for the next hardware
test: `bthid_send_output_report()` now logs every send attempt + result, and the BLE
`HID_SERVICE_CONNECTED` handler now hex-dumps the raw GATT HID report descriptor once per
connection. (4) **Known, deliberately not fixed**: the generic driver (`bthid_gamepad.c`) only
sends rumble for Xbox's vendor ID — documented in place as a real capability gap needing a hardware
HID-descriptor capture before extending, not guessed at blind. Full trace, evidence, and the
fix-requirements checklist: `docs/bluetooth/btstack-implementation.md` "Rumble regression". All
three configurations build clean (including the new diagnostics). **DualSense, Xbox Classic BT, and
generic/XInput remain untested on hardware; Xbox BLE has one confirmed-still-broken data point with
root cause not yet isolated.** Per explicit instruction, Gates 2 (BT identity/VID-PID hardening) and
3 (GameCube-class support) remain **not started** — this priority program requires Gate 1 actually
resolved (not just build-verified) before proceeding.

**Correction, same day, immediately after the above:** researched the real Xbox-BLE HID rumble
format via the Linux `xpadneo` reference driver instead of requiring another hardware round-trip
(byte-for-byte match confirmed report ID/layout correct; found and fixed the real `loop_count` gap,
see item 3 in the docs section). **But a follow-up audit then found item (2)'s and this fix had both
landed in `xbox_bt.c`/`xbox_ble.c` — dead code, never registered** (`bthid_registry.c` explicitly
routes all Xbox controllers through the generic driver instead). Neither fix ever ran. The real,
reachable Xbox rumble path (`bthid_gamepad.c`'s `gamepad_task()`) had the identical unfixed bug —
now fixed there. **User decision: re-register `xbox_bt.c`/`xbox_ble.c` in `bthid_registry.c` rather
than delete them**, retiring the generic driver's role for Xbox (except Elite Series 2, still
excluded by both files, still falls back to the generic driver's already-fixed code). `xbox_ble.c`'s
input parsing is evidence-backed ("verified from testing"); `xbox_bt.c`'s Classic BT parsing is a
length-based guess between two report formats with no such evidence — flagged as this pass's top
hardware-validation risk (possible input regression on Classic BT Xbox specifically, not just a
rumble question). The same audit pass also found and fixed a duplicated `+64` bug in
`switch_pro_bt.c`'s rumble encoder and a missing CRC32/wrong hw_control flag in `ds4_bt.c` (verified
against current Linux `hid-playstation.c` source), and flagged (not fixed) a channel-choice
inconsistency in `wiimote_bt.c`. All build-verified; **nothing from this correction is
hardware-tested yet.** Full detail: `docs/bluetooth/btstack-implementation.md` "Rumble regression",
items 4 and "Additional findings."

**Hardware validation update, same day:** the user flashed the latest combined build and confirmed
rumble now works on Xbox, DualSense, Switch 1 Pro Controller, and Wiimote both standalone and with
its joystick attachment. This closes the cross-family "no rumble at all" regression on real
hardware and confirms the fixes reached live registered paths. Validation was functional/presence
only: it did not yet establish left/right motor separation, amplitude curves, stop/reconnect
behavior, Xbox transport or Classic button/stick parsing, generic non-Microsoft XInput rumble,
DS4 output, or the flagged Wiimote control-vs-interrupt channel distinction. Gate 2 (identity and
VID/PID hardening) may now proceed while those narrower cases remain tracked.

**Gate 2 — driver reachability audit, fresh upstream comparison, identity architecture fixes,
2026-07-12.** Full detail across four new/updated docs:
`docs/bluetooth/driver-reachability-audit.md`,
`docs/bluetooth/joypad-os-upstream-comparison-2026-07-12.md`,
`docs/bluetooth/btstack-implementation.md` ("Gate 2" section), and
`docs/bluetooth/8bitdo-ngc-diy-profile.md`.

**Reachability audit**: confirmed all 11 BT HID drivers are compiled, registered, and reachable (no
repeat of the earlier dead-code Xbox mistake). Found and fixed a real shadowing bug:
`switch_pro_bt.c` (Switch 1, Classic-only hardware, registered before `switch2_ble.c`) had a
name-based fallback with no transport guard that could claim a BLE-connecting Switch 2 Pro
Controller before the correct driver ran — and the existing re-evaluation safety net wouldn't have
self-corrected it, since the wrong driver's own `match()` kept returning `true` via the name path
even once accurate identity arrived. **Fixed structurally, not with a one-line patch**: added
`bthid_transport_mask_t` to every driver (`bthid.h`/`bthid.c`), checked centrally before any
`match()` call, closing this entire bug class for present and future drivers rather than just this
one instance. Also found and fixed the same root pattern in `ds4_bt.c` (its deliberately generic
advertised name, "Wireless Controller," could permanently misclaim a non-Sony clone).

**Fresh upstream audit** (`joypad-ai/joypad-os`, freshly cloned, SHA `b292005...`, 2026-07-11):
confirmed upstream *still* deliberately keeps Xbox on the generic driver — new evidence worth
weighing against this project's 2026-07-12 decision to re-register `xbox_bt.c`/`xbox_ble.c` (made
explicitly, with the risk already explained, not silently reverted). Found and preserved two
hardware-validated PicoSwitch2-only improvements in `bthid_gamepad.c` that upstream lacks (Xbox
Elite Series 2 paddle support; a name-based Xbox VID fallback) — flagged so a future upstream sync
doesn't silently overwrite them. Confirmed this project's Xbox `loop_count` rumble fix and its
`bthid_transport_mask_t` guard are both ahead of upstream, not behind it.

**Root cause of `0000:0000` VID/PID — Confirmed, not a parsing bug.** BTstack's own PnP-ID (`0x2A50`)
accessors are spec-correct (verified against the actual generated code); this project's DIS handler
is byte-for-byte identical to upstream's. The real cause is timing: the DIS query is deliberately
deferred until after HID notifications are enabled (avoiding a real prior GATT-contention
regression), but driver binding happens earlier — so VID/PID is correctly, unavoidably `0` at first
bind for any BLE device without Switch 2's pre-connection manufacturer-data shortcut. Reordering
was considered and rejected (would reintroduce the regression the ordering was built to avoid); the
fix targets the re-evaluation path instead (above).

**New infrastructure**: `bt_identity_log.c`/`.h` — a bounded, pull-based log (same proven pattern as
`sw2_capture.c`) recording one event per driver-binding decision (transport, name, VID:PID +
inferred provenance, class of device, descriptor length/fingerprint, selected driver, match reason,
player slot). Drained via the new `btid dump`/`stat`/`clear` config-mode commands. Not yet exercised
against real hardware. A stable per-device profile key (model identity vs. physical-device identity,
explicit degradation for randomized BLE addresses) was designed and documented but not implemented —
no consumer exists yet (no per-device mapping storage currently exists to migrate).

**8BitDo NGC DIY** hardware observations moved out of `DATA.md` into a durable doc
(`docs/bluetooth/8bitdo-ngc-diy-profile.md`) so they survive handoff-document replacement.
Implementation still blocked on hardware identity capture via the new `btid dump` facility, per
DATA.md's explicit sequencing.

All shared-code changes build-verified across `NS2_AUDIO=on` (default, both boards),
`NS2_AUDIO=off`, and `NS2_PRO=off`. **Nothing from this pass is hardware-tested yet** — the identity
log, the transport-mask fix, and the ds4_bt VID-reject fix all need a real multi-controller session
to validate.

**First real hardware validation, same day: 8BitDo NGC Modkit end-to-end.** Ran the exact `btid
dump`/`btid stat` workflow above against real hardware for the first time — no power cycle needed
(BOOTSEL-hold live-re-enumerates into config mode without losing the RAM-resident identity log,
confirmed by directly accessing the resulting `PICOSWITCH` COM port). Captured a clean identity
(Classic BT, `8BitDo NGC Modkit`, VID:PID `0x2DC8:0x286A` via SDP, 86-byte descriptor, correctly
landing on `Generic BT Gamepad`), which also caught three real logging-completeness bugs in the
identity log itself (wrong provenance/COD on the descriptor-arrival event; a missing event for
"VID resolved but driver stayed generic"; a duplicated provenance-inference pattern that let bug
one happen) — all fixed and re-verified live. Decoding the raw HID descriptor by hand found the
actual root cause of the controller's mismapped buttons: `is_8bitdo && buttonCnt > 14` was routing
this device into the paddle-controller table (`BITDO_BUTTON_MAP`, built for 8BitDo Ultimate/Pro 2),
since this device also happens to have 16 buttons despite a completely different physical layout —
fixed with a PID-specific (`0x286A`, not the shared `0x2DC8` 8BitDo VID) dedicated profile.

Interactive live capture (pressing every remaining physical control, sampling triggers at partial/
pre-click/full-click) found the trigger's "duplicate shoulder+trigger" symptom was real hardware
behavior, not a parsing bug: two of the 16 raw button bits fire during partial trigger travel,
independent of the actual mechanical click — a coarse analog-threshold echo, not real switches.
Building the fix surfaced a genuine architectural conflict: `ns2_seam.c`'s `router_submit_input()`
has a long-standing, driver-independent fallback that derives `JP_BUTTON_L2`/`R2` from the analog
trigger value crossing a threshold (correct default behavior for controllers like Xbox/DualSense
that never report a discrete click bit) — this collided with an early design that repurposed
`JP_BUTTON_R2` for the controller's Z button. Two iterations were needed (see the experiment doc for
what was tried and rejected) before landing on the final design: let the existing analog fold keep
driving ZL/ZR from real trigger values (matching what the owner actually wanted — "any real press,"
not just the discrete click), and move Z to a bit (`JP_BUTTON_R1`) the fold never touches. Face
buttons also needed correcting from an assumed Xbox-style rotated mapping to a direct A→A/B→B/X→X/
Y→Y one, appropriate for a GameCube-shaped controller. **Confirmed working end-to-end by the
controller's owner on real hardware, Switch 2 Pro Controller mode.** All changes (including the new
`suppress_l2r2_analog_fold` opt-out added to `input_event_t`, unused by the final design but kept as
infrastructure) build-verified across the full three-config matrix. Full raw evidence, the two
rejected design iterations, and the final confirmed mapping table:
`docs/experiments/gate2-identity-log-hardware-captures-2026-07-12.md` and
`docs/bluetooth/8bitdo-ngc-diy-profile.md`. **This validates the 8BitDo identity/profile and logging
fixes specifically — the rest of Gate 2 (Switch 2 Pro, Switch 1 Pro, Xbox, DualSense, Wiimote,
generic/XInput) remains hardware-untested.**

**2026-07-13 — Genuine Switch 2 Pro Controller BLE captured; the transport-mask fix's actual target
scenario is now hardware-confirmed clean.** `btid dump` on a real Switch 2 Pro Controller over BLE
shows a single `initial-bind` event, correct `Nintendo Switch 2 Contr...` driver, `provenance:
"ble_adv_mfr_data"`, and no re-evaluation/rebind — no shadowing occurred. The `device` command
independently agrees (`vid:1406 pid:8297` = `0x057E:0x2069`). One real finding: the controller's
actual advertised name is `"Switch 2 Pro"`, which does **not** contain the `"Pro Controller"`
substring the transport-mask fix's originally-hypothesized Switch-1-shadowing collision depended on
— so while the fix itself checks out as harmless defense-in-depth (doesn't break the legitimate
path), that specific collision was never reachable on real Nintendo naming and remains unreproduced.
Full raw JSON (verbatim, not paraphrased): `docs/experiments/gate2-identity-log-hardware-captures-2026-07-12.md`
"Capture 2".

Also owner-reported from the same session: genuine Switch 2 Pro Controller BLE pairing was
unreliable specifically when using the explicit BOOTSEL pairing gesture (worked fine via normal
auto-reconnect). Traced to a real bug: `ns2_bt_host.c`'s pairing window closed unconditionally on
expiry, and `btstack_host_stop_scan()` unconditionally resets `hid_state.state` to `IDLE` — which,
if a BLE `gap_connect()` was genuinely in flight at that moment, silently disarms the
`BLE_CONNECT_TIMEOUT_MS` (10s) watchdog for that specific attempt, since the watchdog requires
`state==CONNECTING` to fire. No other phase (GATT discovery, SM pairing, HID setup, Switch 2 GATT
init) was actually at risk — all key off per-connection state instead, confirmed by an exhaustive
trace of every `hid_state.state` read in the file. Classic BT was traced too and found not
vulnerable (its own connect-timeout watchdog is independent of this state). **Fixed**: pairing-window
closure now defers (via a new `btstack_host_close_pairing_window()`) if a connect is genuinely
in-flight, resolving the moment that attempt's raw connection concludes — bounded by the existing
`BLE_CONNECT_TIMEOUT_MS`, not a new invented constant. `PAIRING_WINDOW_MS` also widened 10s → 30s for
usability (secondary; not the actual fix). Build-verified across the full four-combination matrix
(`pico2_w`/`pico_w` default, plus `NS2_AUDIO=OFF`/`NS2_PRO=OFF` scratch builds on `pico2_w`); **not
yet hardware-validated** — exact test procedure in `docs/bluetooth/btstack-implementation.md`
"Pairing window vs in-flight connect".

**2026-07-13 — NSO GameCube Controller output personality: Stage A complete, Stage B evidence-ready
and now in progress.** Per explicit new priority (`NSO-GC.md`), moved directly from the pairing fix
into researching a native NSO GameCube Controller (`057E:2073`) USB output personality — a new
capability separate from Bluetooth input detection and from the existing Switch 2 Pro Controller 2
output personality, not exclusively coupled to any one input controller. Genuine hardware
(`USB\VID_057E&PID_2073`) confirmed attached and enumerating cleanly; live, non-destructive
`pywinusb`-based HID capability query (manufacturer/product/serial strings, usage page/usage, report
lengths, link-collection count) independently cross-checked against `ndeadly/switch2_controller_research`'s
documented descriptor topology — all matching. Both reference repos fresh-cloned to `E:\nso-gc-refs\`
(outside the tracked worktree) and fully audited: **critical finding** — `SoulCalDan`'s repo does
**not** implement the native NSO protocol at all; it hard-codes the older Wii U GameCube Adapter
identity (`057E:0337`) and reads a real physical GC controller over its original SI-bus protocol,
structurally incapable of producing any Switch-2-exclusive button (ZL/C/Home/Capture). Re-scoped:
`ndeadly`'s repo is the sole primary protocol source; SoulCalDan's is secondary evidence only for
genuine-GC-hardware trigger/stick electrical behavior. Both provided SPI dumps
(`dumps/NSO_GC_SPI_DUMP_1.bin`/`_2.bin`, byte-identical to each other, SHA-256 `4aee5a7c...`)
structurally analyzed: 6 of 8 addresses ndeadly's documented BLE factory-read sequence uses land
exactly on real (non-erased) data in this independently-generated dump — genuine cross-validation
promoting the factory-data region's existence from Hypothesis to Confirmed. Found and correctly
flagged-for-exclusion: a per-unit serial string, a possible lot code, and a BT-pairing-shaped record
(none to be reused in firmware); found a calibration-shaped record family using Nintendo's known
Joy-Con 12-bit packing convention (Strong, semantics not yet confirmed) and an unidentified numeric
lookup table (Hypothesis). Full architecture doc written covering: personality-selection design
(reusing the existing CDC-config-mode live-re-enumeration mechanism as proven prior art — no reflash
needed), a from-scratch codebase audit of exactly what's Pro2-coupled vs. already-shareable (rumble
routing and the cross-core input struct are already fully personality-agnostic; report
scheduling/command dispatch are not and shouldn't be forced to share), and a mapping policy
(GameCube output exposes ZL/C/Home/Capture natively and excludes L3/R3 entirely — confirmed physically
absent — with a concrete design-intent table for the 8BitDo NGC Modkit's eventual second mapping
mode). **Update, same session**: the earlier privilege-level blocker on raw descriptor capture was
resolved — a UAC-elevated `USBPcap --inject-descriptors` capture (replays descriptor-fetch
transactions the driver had already cached from the device's real enumeration; no physical replug, no
driver changes to the controller) got the **complete, byte-exact device descriptor (18 bytes) and
configuration descriptor tree (80 bytes, matching `wTotalLength` exactly, down to every
interface/endpoint/HID-descriptor-header byte)**. This promotes the device/config-descriptor half of
Stage B's evidence requirement from Strong to Confirmed and is the single biggest evidence upgrade of
this session — preserved at
`docs/experiments/nso-gc-captures/genuine-controller-descriptors-2026-07-13.pcap`, fully decoded in
`docs/switch2-gc/protocol.md`. New previously-undocumented fact found: `bcdUSB 0x0200` and
`bmAttributes 0xC0` (self-powered, no remote-wakeup — a real point of difference from SoulCalDan's
unrelated WiiU-adapter descriptor, which uses `0xE0`).

**Same session, follow-up — decoded ndeadly's own unanalyzed `rumble-procon-gccon.pcapng.gz` (a real
Cynthion USB-analyzer capture of a *different* genuine GC controller): major further evidence
upgrade, no hardware or elevation needed at all.** Found: (1) that capture's device descriptor is
byte-for-byte identical to this project's own — second independent unit confirming the same bytes;
(2) a neutral-state live report `0x0A` that decodes correctly against every documented field with
zero contradictions, including two exact matches against specific enumerated values — **promotes the
entire report `0x0A` layout from Strong to Confirmed**; (3) 8 real rumble-test samples of output
report `0x03`, confirming the report's framing (report ID `0x03` literally on the wire for USB,
4-byte data field, 37-byte zero padding) though the byte-level intensity/mode encoding itself remains
Hypothesis; (4) a previously-undocumented architecture fact — part of the init handshake happens over
plain **EP0 vendor control transfers**, not exclusively the bulk interface, including a factory-read
command (`bRequest=3`) that independently cross-validates against this project's own SPI dump (same
source address, same record structure, a *different* unit's serial number confirming the
`HHW5000xxxxxxx` format is genuinely per-unit). The raw 97-byte HID **Report** descriptor body and
string-descriptor text remain uncaptured but are now lower-priority, since the report's *effective*
structure is independently proven correct. **Still deliberately no firmware code written** — Stage B
(device/config descriptors) and the descriptor-construction half of Stage C (report `0x0A` structure)
are now fully evidence-ready; report `0x03`'s exact byte semantics and the button bitfield are the
two gaps most worth closing before Stage C/E are complete, but neither blocks starting Stage B. Full
detail, all citations, and the exact evidence-gap list: `docs/switch2-gc/protocol.md`,
`docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`,
`docs/switch2-gc/usb-personality.md`, `docs/switch2-gc/mapping.md`,
`docs/experiments/nso-gc-spi-dump-analysis-2026-07-13.md`,
`docs/experiments/nso-gc-reference-repo-audit-2026-07-13.md`.

**2026-07-13 — NSO GameCube Controller: Stage B (USB personality/enumeration) implemented and
build-verified, not yet hardware-validated.** Per explicit new priority (updated `NSO-GC.md`), moved
directly from Stage A's evidence-gathering into implementation. New runtime `usb_personality_t` enum
(`SWITCH2_PRO2` / `NSO_GAMECUBE` / reserved `JOYCON2` / `CDC_CONFIG`), owned exclusively by core0 and
requested (not written) by core1's BOOTSEL-hold gesture — the gesture itself needed zero code changes,
since `bootsel.c`'s existing `hold_fired` latch already satisfied every "fires once, release isn't a
tap" requirement. **Product decision, correcting Stage A's own earlier recommendation**: personality
selection is volatile (not persisted to flash, no `CONFIG_VERSION` bump) — every boot starts as Pro
Controller 2, and a ~5s BOOTSEL hold live-cycles Pro2 → GameCube → CDC config (terminal), reusing the
existing disconnect/reconnect re-enumeration mechanism verbatim. New `src/switch_gc/switch_gc.c`
module: Confirmed byte-exact device/config descriptors (promoted from Strong to Confirmed this same
pass — see below), Stage-B-scope stubs everywhere else, each explicitly documented with its own
evidence tier so nothing Strong-or-weaker gets silently treated as Confirmed. Centralized every
TinyUSB callback that can only have one link-time definition (descriptor callbacks, EP0 vendor
control, mount, HID get/set report) into a personality dispatcher in `usb_descriptors.c`; found and
fixed a real latent bug in the process (the MS-OS-1.0 WinUSB string was gated only on "not config
mode," so it would have incorrectly handed GameCube mode Pro2's WinUSB compat-ID binding). Two new
pure-logic modules (`usb_mode_cycle.c`, no pico-sdk dependency) are host-compilable and
host-tested (`tools/test_usb_mode_cycle.c`, `gcc -DNS2_PRO=1`) — confirms the cycle order, that the
reserved Joy-Con 2 slot is transparently skipped, and that config mode is a genuine fixed point.
`tools/verify_gc_descriptors.py` independently re-transcribes the expected descriptor bytes (not
copied from the C source) and diffs them against `switch_gc.c` — passes. Also this pass: decoding
`rumble-procon-gccon.pcapng.gz` further promoted the GameCube HID report descriptor's structure and a
second unit's device/config descriptor bytes to Confirmed, closing what had been the last real Stage B
evidence gap. Doc corrections made per explicit instruction: `CLAUDE.md`'s stale "documentation only,
wait for Pro2 maturity" policy superseded; the project owner's direct physical confirmation that ZL is
a real control on the genuine controller corrected an earlier wrong "physically absent" statement in
both `docs/switch2-gc/mapping.md` and `docs/switch2-gc/protocol.md`. Build-verified across the full
four-combination matrix (both boards' default `NS2_PRO=ON`, plus `NS2_AUDIO=OFF`/`NS2_PRO=OFF` scratch
builds) — `NS2_PRO=OFF` (Switch 1) confirmed unchanged by code inspection (its own branch of every
touched file is byte-for-byte the pre-existing implementation).

**2026-07-13, same day — hardware-validated, two real bugs found and fixed via the owner's own
comparative testing against a genuine controller.** (1) **Code 28** ("no compatible drivers") on the
whole composite device — the vendor interface (IF1) has no standard Windows driver and Stage B hadn't
implemented the same Microsoft OS 1.0 WinUSB auto-bind mechanism the existing Pro Controller 2
personality already relies on for its own vendor interface; fixed by mirroring it exactly for
GameCube. (2) **The real bug**: fixing (1) alone wasn't sufficient — re-testing with a genuine
controller connected simultaneously found intermittent misrecognition and, critically, the *genuine*
controller's own cached Steam mapping breaking too. Root cause: `bcdDevice` used the exact
raw-captured value, making the Pico and the genuine controller byte-for-byte identical to Windows
across VID, PID, bcdDevice, *and* serial (the serial string is literally `"00"` on both, not a real
per-unit value) — Windows keys its WinUSB driver cache on VID+PID+bcdDevice, so the two devices became
indistinguishable to it, corrupting cached state for both (a pure Windows-side bookkeeping issue, no
write ever touched the real controller). **This is the identical problem `switch_pro2.c`'s own
descriptor already solved** — its `bcdDevice` is deliberately `2.10`, not the genuine `2.00`, for
exactly this reason. Fixed the same way: GameCube's `bcdDevice` changed to `1.11` (was the real
`1.01`). **Confirmed working after both fixes**: clean enumeration under "Universal Serial Bus
devices," no driver error, genuine controller unaffected. Steam shows the Pico as "Nintendo GameCube
Controller" with a "Begin Setup" prompt rather than full native recognition — expected, not a bug:
Stage B intentionally never responds to any command beyond the WinUSB probe (that's Stage D's
init-handshake job), so Steam falls back to generic setup. Full detail:
`docs/switch2-gc/usb-personality.md` "Implementation status".

**2026-07-13, same day — HID Report descriptor promoted to Confirmed; `bRequest=2` promoted to
Hypothesis; both closed evidence gaps, one from a fresh live capture and one from re-mining data
already in the repo.** A live USBPcap replug capture on the project owner's genuine unit (hub
`USBPcap1`, this machine's only hub with a device attached; the other two are decoys/empty) caught a
standalone `GET_DESCRIPTOR(HID Report)` control transfer and recovered the full 97-byte body
byte-exact — closes the one real remaining Stage B/C descriptor gap. Archived:
`docs/experiments/nso-gc-captures/genuine-controller-hid-report-descriptor-2026-07-13.pcap`. Full
decode in `docs/switch2-gc/protocol.md` "HID report descriptor" — also **corrects a real error** in
the existing Output Report `0x03` byte table: the descriptor declares 63 bytes of output data
(`Report Count 0x3F`), not the 41 implied by ndeadly's table (`1+4+37`); the reserved region is 59
bytes, not 37. Separately, **re-analyzed `rumble-procon-gccon.pcapng.gz` exhaustively** (the whole
762s/1.3M-frame file, not just the known rumble-burst window) per explicit direction to mine existing
captures harder before requesting more hardware time: confirmed via strict per-endpoint device
correlation that the file's 8 already-documented GC rumble samples are the *complete* population (44
total writes to that endpoint system-wide, 36 of them zero-length packets — a new Confirmed finding
that idle/no-rumble state is a ZLP, not an all-zero report) — this was a real verification, not
assumed. Also found that the Pro Controller 2's own enumeration in the same file issues the identical
EP0 vendor `bRequest=2`/`wLength=16` command the GC controller's decode had flagged Unknown; byte-for-
byte comparison across the two device types promotes it to **Hypothesis** (byte 2 is a plausible
device-type discriminant: `0x05` Pro2 vs `0x02` GC; bytes 0-1/3-9/13-15 are constant across both).
Full detail: `docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`. **Rumble byte-semantics
remain Hypothesis** — this is a genuine data-scarcity limit of the only USB rumble capture that
exists, not a research gap; closing it further needs either a new deliberate multi-value capture or
accepting the existing "opaque intensity, fixed safe default" Stage E recommendation permanently.

**2026-07-13, same day — Stage C started (report `0x0A` construction); Stage E given a provisional,
explicitly-non-final implementation.** Per explicit project-owner direction ("proceed with Stage B
[i.e. treat it as closed] now; further rumble research would be premature"): `switch_gc_build_report()`
(new, `src/switch_gc/switch_gc.c`) constructs the full 63-byte report `0x0A` body for every field the
shared `switch_pro_input_t` struct already carries — mirrors `switch_pro2.c`'s own `ns2_build_report()`
conventions exactly (same source struct, same packed-12-bit stick passthrough, same
`tud_hid_n_report(instance, id, buf, 63)` call shape). Correctly hardcodes the Right/Left-Stick-click
bits to always 0 (genuine hardware has no L3/R3). Deliberately does **not** map native GameCube Z,
digital L/R trigger detent, or continuous analog L/R trigger — the shared input struct has no field
for them (Pro Controller 2 has no analog-trigger concept to reuse), and mapping.md's own per-device
tables (8BitDo NGC Modkit first) aren't built yet; extending the struct and those tables is the next
GC-specific increment. Report construction is implemented but **not yet streamed** — `switch_gc_task()`
still stays silent pending Stage D's "Select Input Report" command gate, matching genuine hardware's
own documented requirement (mirrors Pro2's `ns2_streaming` flag). Separately, gave `switch_gc_hid_out_report()`
a **provisional** Stage E implementation per the owner's explicit instruction: preserves the Confirmed
zero-length-packet idle behavior exactly, corrects the report to 4 rumble-data bytes + 59 reserved
bytes (was 37, see above), and maps any nonzero rumble data to one conservative fixed motor amplitude
— intensity byte semantics stay explicitly unresolved, not silently promoted to a decoded fact. Both
boards build clean. Also fixed the last stale Stage B hedge: `switch_gc.c`'s HID report descriptor
comment now cites today's live capture instead of "never independently captured."

**2026-07-13, same day, third pass (`PROMPT.md`) — Stage C completed (native Z/detents/analog
trigger), 8BitDo NGC Modkit GameCube-mode mapping implemented, a real TinyUSB report-ID contract
bug fixed, and a minimum Stage D streaming gate implemented — Confirmed against real USB bulk
evidence, not fabricated.** Five phases, all host-tested and build-verified, **not yet
hardware-validated**:

- **Encoder extracted + completed.** `switch_gc_encode_report()` moved to its own zero-dependency
  file (`src/switch_gc/switch_gc_encode.c`, mirroring `usb_mode_cycle.c`'s pattern) and now encodes
  native Z, independent L/R trigger detents, and continuous analog L/R trigger — closing every gap
  the previous pass had left. Report counter is now module-static and reset by
  `switch_gc_init()/reset()/mount()` instead of living forever as a function-static. Ten golden
  tests (`tools/test_switch_gc_report.c`) all pass.
- **Shared input model extended**: `switch_pro_input_t` gained `left_trigger`/`right_trigger`
  (continuous 0-255) and `gc_extra` (`GC_MASK_Z`/`L_DETENT`/`R_DETENT`) — always present (not
  `#ifdef NS2_PRO`-gated) so the struct layout is identical across build configs; Pro2/Switch 1
  encoders simply never read them. `input_event_t` gained matching `gc_has_native_layout`/
  `gc_native_z`/`gc_l_detent`/`gc_r_detent` fields, populated only by the 8BitDo NGC Modkit
  (`bthid_gamepad.c`'s `process_report_dynamic()`, gated on `is_ngc_modkit`) from usage 9/10/11 —
  usage 9/10 audited directly against `docs/bluetooth/8bitdo-ngc-diy-profile.md`'s raw hardware
  observations first (confirmed independent, non-aliasing true-clicks; usages 7/8's partial-travel
  echo deliberately excluded).
- **Analog-fold collision found and fixed**: the existing generic `analog[ANALOG_L2/R2]`→
  `SWITCH_MASK_ZL/ZR` fallback fold in `ns2_seam.c` would have silently synthesized a ZL/ZR the
  Modkit doesn't physically have, the instant GameCube mode read the same shared bits Stage C
  already forwards. Fixed by suppressing the fold specifically when
  `g_usb_personality == NSO_GAMECUBE && event->gc_has_native_layout` — proven not to affect Pro2 (the
  personality check alone excludes it) or any other device (the capability flag alone excludes
  them). Pro2's own already-validated Modkit mapping is provably unchanged.
- **8BitDo NGC Modkit GameCube-mode mapping implemented**: A/B/X/Y, D-pad, sticks, Start→Plus,
  L3-label→Capture, R3-label→Home all already flowed correctly (unchanged Pro2-era code, same
  shared struct fields GameCube's encoder also reads); Z, detents, and continuous analog L/R are
  the new pieces, added alongside (not replacing) the existing Pro2-mode mapping.
- **Real TinyUSB report-ID bug found and fixed**: `tud_hid_set_report_cb()`'s centralized
  dispatcher ignored TinyUSB's separate `report_id` parameter and always treated `buffer[0]` as the
  ID — audited directly against the installed TinyUSB 2.2.0 source
  (`lib/tinyusb/src/class/hid/hid_device.c`): correct by coincidence for interrupt OUT (report_id
  always 0, real ID inline at buffer[0] — the transport this device's descriptors actually declare
  an endpoint for) but wrong for control `SET_REPORT` (report_id carries the real value separately,
  with a matching leading buffer byte already stripped). Fixed via a new pure `hid_out_normalize()`
  helper (`src/hid_out_normalize.c`, 7 host tests, all pass), shared by both Pro2's and GameCube's
  rumble handlers — both `ns2_hid_out_report()`'s and `switch_gc_hid_out_report()`'s signatures
  changed to take an already-normalized `(report_id, data, len)` triple.
- **Minimum Stage D streaming gate implemented, Confirmed (not Strong) evidence.** Re-mining this
  project's own `rumble-procon-gccon.pcapng.gz` for bulk vendor-interface (IF1) traffic — 225 OUT
  transactions that had never been analyzed, only the file's EP0 control requests had been —
  resolved protocol.md's own flagged uncertainty about whether USB's command framing matches BLE's:
  it does, byte-for-byte, for the two commands that matter. Command 0x03/sub 0x0D ("Initialise
  USB") and 0x03/sub 0x0A ("Select Input Report", value 0x0A) are now Confirmed for USB specifically
  (promoted from BLE-derived Strong), with exact request AND response bytes captured directly —
  including proving 0x03/sub 0x03 ("Enable USB HID Reports") was never actually sent in that real
  session. `switch_gc_vendor_dispatch()` implements exactly this minimum pair (0x03 ACked
  defensively, everything else logged at 1/sec and ignored) using the identical response-envelope
  shape `switch_pro2.c`'s own already-shipped `ns2_dispatch()` uses for the same command family.
  `switch_gc_task()` now actually streams report `0x0A` once armed.
- **Full regression**: existing descriptor verifier and mode-cycle host test still pass; all four
  build configs (`pico_w`/`pico2_w` default, `pico2_w` `NS2_AUDIO=OFF`, `pico2_w` `NS2_PRO=OFF`)
  green after every phase.
- **Honest gap, not glossed over**: none of this has been hardware-validated. Worse, the Stage D
  gate this pass just built **cannot be exercised by the owner's existing Windows/Steam test setup
  at all** — that's Nintendo-console-specific vendor protocol a generic PC host has no reason to
  send. Exercising it needs either a real Switch 2 console (never available to this project so far)
  or a new raw-USB-bulk-write test tool (not built this pass — `pyusb` isn't installed on this
  analysis machine and no working backend was verified, so no half-working tool was added instead).
  Full narrative, evidence, and the exact next test: `DATA.md`.

**2026-07-13, same day, fourth pass — hardware test found no input reaching Steam; root-caused and
fixed without needing the planned Windows WinUSB tool.** The owner's hardware test confirmed clean
enumeration but zero input. Rather than build the tool, launched Steam directly against the
already-connected Pico and captured the real traffic with USBPcap. **Found**: Steam sends the
complete bulk command sequence and the Pico's firmware answers every command correctly — but Steam
requests input report **`0x05`**, never `0x0A`. The previous pass had only implemented streaming
for `0x0A` (assumed to be what any host would request); `0x05` is actually the shared,
cross-controller format PC/Steam hosts use for every Switch-family device, `switch_pro2.c`'s own
Pro2 already streams it the same way. Not a parser bug, not a missing EP0 prerequisite (also
confirmed live: `bRequest` 2/3/4 all stall on this project's own Pico, and Steam completed the bulk
handshake anyway). **Fixed**: `switch_gc_encode_report05()` (new, independent encoder — GC's own
analog-trigger tail at offsets `0x3C`/`0x3D`, not shared code with Pro2's module) and
`switch_gc_select_report()` (new pure decision function, host-tested) — `switch_gc_task()` now
streams whichever of `0x05`/`0x0A` the host actually selected. 33 new report-0x05 golden tests + 8
new selection-logic tests, all pass; full four-config matrix green. Full diagnosis, evidence, and
fix: `docs/experiments/gc-stage-d-steam-diagnosis-2026-07-13.md`. **Not yet re-tested on real
hardware** — see `DATA.md` for the exact next step.

**2026-07-13, same day, fifth pass — real hardware re-test: report 0x05 fix confirmed working
(all buttons except Z), but GC mode still not recognized by a real Switch 2 console.** The owner
correctly recalled hitting an analogous problem during Pro Controller 2's own development.
Checked `switch_pro2.c`'s `ns2_vendor_control_xfer()` and its own comment: a real Switch 2 console
issues three EP0 vendor control requests (`bRequest` 0x02/0x03/0x04) immediately after
`SET_CONFIGURATION` and refuses to proceed to the bulk command channel until they're answered —
"stalling these was why the console configured us then went silent... console-specific; Windows/
Steam never send them." GC's own `switch_gc_vendor_control_xfer()` only ever handled the WinUSB
MS-OS request and stalled all three of these — exactly reproducing the symptom. **Fixed**:
implemented the same three responses for GC, mirroring Pro2's shape (64-byte identity block,
16-byte info block, bare ACK), using this project's own already-captured Confirmed byte layout for
GC's identity/info fields (fictitious serial, per NSO-GC.md's exclusion rule — same policy Pro2's
own shipped code already follows for its own serial). Separately: the "Z button doesn't work" part
of the same test report is expected, not a new bug — report `0x05` (what Steam actually streams)
has no bit position for native Z at all, only report `0x0A` can carry it, and no console
currently exercises `0x0A` yet to test that path. Both boards build clean;
`tools/verify_gc_descriptors.py` still passes. **Not yet re-tested against a real console.**

**2026-07-13, same day, sixth pass — real console re-test still showed no recognition; EP0
identity alone was insufficient, extended to a fuller vendor-bulk command set matching Pro2's own
breadth.** After the EP0 identity fix, the owner re-tested and GC mode was still not recognized by
a real Switch 2. Read `switch_pro2.c`'s complete `ns2_dispatch()` for comparison: it answers
**every** command with at least a bare 8-byte ACK (`default:` case — "0x06 shutdown, 0x0A
vibration, and anything else"), whereas GC's dispatcher stayed completely silent for anything
outside the 0x03 family. Hypothesis: a real console may wait for a response to every command
before treating a device as recognized, while Steam (which sends a much smaller subset) doesn't
care. **Fixed**: `switch_gc_vendor_dispatch()` (`src/switch_gc/switch_gc.c`) extended to cover the
same command families Pro2 handles — SPI/memory reads (`0x02`, sourced from a new
`switch_gc_mem_read()` using the same identity bytes already served over EP0, 0xFF elsewhere),
feature flags (`0x0C`, GC's own documented value `0x27`), Bluetooth-pairing-shaped commands
(`0x15`, safe structural placeholders, no real crypto — GC has no real BLE bond to establish over
USB), `0x07`/`0x09`/`0x16`, and critically a `default:` case that always sends a bare ACK instead
of staying silent. Also live-verified (genuine controller reconnected, fresh USBPcap capture) that
the genuine controller currently streams report `0x05` too (not `0x0A`) under Steam, and confirmed
the Z-shows-as-ZR observation matches ndeadly's own documented bit table exactly (report `0x0A`
byte0 bit `0x10` is literally labeled "Z"; report `0x09`'s identical bit position is labeled "ZR" —
the console's test-input UI most likely just displays whichever report populates that bit slot
using its own "ZR" glyph) — no mapping change needed there. Both boards + both scratch configs
build clean. **Not yet re-tested against a real console** — this is the next thing to try.

**2026-07-13, same day, seventh pass — extended Pro2's existing LED handshake-progress diagnostic
(NS2_DIAG) to GameCube mode, since real-console re-test still showed no recognition and this
project's own `printf()` diagnostics go nowhere useful** (`pico_enable_stdio_usb`/`uart` are both
OFF in `CMakeLists.txt` — a real, previously-unnoticed gap: the rate-limited log line added two
passes ago was never actually visible to anyone without a UART cable). `switch_pro2.c` already has
exactly the right tool for this (`g_ns2_stage`, blinked as N rapid flashes by `ns2_bt_host.c`,
gated on `NS2_DIAG`) — it was just Pro2-only. Added a **separate** `g_gc_stage` variable
(`switch_gc.c`/`.h`) with its own stage numbers (1 device desc read / 2 config desc read / 3
SET_CONFIGURATION / 4 first EP0 vendor identity request / 5 first vendor bulk command / 6 report
selected, streaming armed), wired at the analogous hooks in `usb_descriptors.c` and
`switch_gc.c`'s own dispatch/mount functions. Per explicit instruction, scoped the blink condition
in `ns2_bt_host.c` to fire **only** while GameCube personality is active (`g_usb_personality ==
USB_PERSONALITY_NSO_GAMECUBE`) — Pro2 mode shows its normal non-diagnostic LED behavior
unchanged even with `NS2_DIAG` on, so the two personalities' flash counts can never be confused.
New diagnostic UF2s built (`NS2_DIAG=ON`, both boards) for the owner to flash and count LED
flashes in GameCube mode — the count directly identifies which USB handshake stage a real console
actually reaches, replacing guesswork with a concrete signal for the next fix.

**2026-07-13, same day, eighth pass — real console testing reached "14 then 20, never further";
diagnosed via a further stage split as specifically "attempts 0x03-family commands, gets stuck
before Select Input Report"; root-caused to missing pairing crypto and fixed.** The owner's
question ("is the genuine GC controller even a Bluetooth device?") prompted checking this
project's own reference research directly rather than continuing to add blink-code dimensions.
Finding: ndeadly's docs explicitly confirm wired USB is a legitimate, console-native transport for
this controller (not merely a PC/charging interface) — but also state **"the Switch 2 console
requires successful pairing... [which] can be performed via both Bluetooth and USB
connections."** `switch_gc_vendor_dispatch()`'s `0x15` pairing-shaped command family had shipped
**placeholder bytes, not real cryptography** — reasoning (incorrectly) that a wired connection
wouldn't need it. This is very likely the actual reason the console's handshake stalled after
~9+ commands including this exact family: pairing fails silently, so the console never proceeds
to select an input report. **Fixed**: extracted `switch_pro2.c`'s already-hardware-validated
AES-128/LTK-derivation pairing crypto (previously private/static) into a shared, host-tested
module (`src/ns2_pairing_crypto.c`, `include/ns2_pairing_crypto.h` — verified against the
standard FIPS-197 AES test vector, independent of anything Nintendo-specific), refactored Pro2 to
use it (byte-for-byte identical behavior, zero risk to its own proven pairing), and wired real
crypto into GC's `0x15` dispatch. GC's own "public device key" constant is an explicit
**assumption** (reuses Pro2's, since no GC-specific capture of this exchange exists yet) — flagged
as the next thing to suspect if pairing still fails. Also refined the LED diagnostic: split stage
20 ("any `0x03`-family command") from a new stage 21 ("specifically attempted Select Input
Report") with an exact-value two-digit readout for an unrecognized report ID, since the coarser
signal couldn't distinguish "never even tries the final step" from "tries it with a bad value."
All host tests (7 new pairing-crypto checks + all previous suites) pass; full build matrix +
both diagnostic UF2s green. **Not yet re-tested against a real console** — this is the next test.

**2026-07-13, same day, ninth pass — real console re-test: pairing crypto fix confirmed working
(console now shows a "Paired" GameCube icon), still stalls at stage 20 with no input.** Real
validation that the pairing fix landed correctly. Two more suspects fixed based on what the
documented post-pairing command sequence does next: (1) `switch_gc_mem_read()` was returning
`0xFF` (uninitialised) for the stick/trigger calibration addresses (`0x13080`/`0x130C0`) the
console reads right after pairing — added `switch_pro2.c`'s own already-proven-working synthetic
calibration block (reused byte-for-byte, not this project's own real-but-unsafe-to-reuse SPI dump
data) at both addresses, plus safe all-zero placeholders for the motion-bias region (`0x13100`/
`0x13140`) instead of `0xFF` (which can read as a garbage/NaN float). (2) The feature-flags
response (`0x0C`/sub `0x01`) was unconditionally returning all-zero capability bits regardless of
what the console asked about — "this controller supports none of the requested features" is a
plausible reason a console would stop rather than proceed. Fixed to echo real per-bit capability
levels for the requested mask, mirroring `switch_pro2.c`'s own already-working logic exactly.
Both boards + both diagnostic UF2s build clean; `tools/verify_gc_descriptors.py` still passes.
**Not yet re-tested against a real console** — this is the next test.

**2026-07-13, same day, tenth pass — real console re-test: no further progress after the
calibration/feature-flag fixes; systematic diff against Pro2's complete dispatcher instead of
more piecemeal guessing.** With Pro2 mode confirmed working perfectly on this exact console/
hardware, and GC mode still stalling at stage 20 with the "Paired" icon showing, did a full
command-by-command diff of `switch_pro2.c`'s complete `ns2_dispatch()` against GC's dispatcher
rather than continuing to guess one command at a time. Found real gaps: command families `0x10`
(firmware/type info, a specific 12-byte reply), `0x0B` (battery), `0x11` (a specific 4-byte reply
for one sub-command and a **29-byte structured reply** for another), `0x01` (NFC-shaped bare-ack
convention), and `0x18` were all previously falling through to GC's generic bare-ACK default (0
data bytes) instead of the structured replies Pro2 actually sends for the identical command IDs.
A console requesting a 12- or 29-byte structured block and receiving 0 bytes back is a much larger
protocol deviation than a wrong calibration value or feature-flag bit, and is now the leading
suspect. Added all five, mirroring Pro2's exact byte layout as a working assumption where GC-
specific values aren't independently confirmed (flagged Hypothesis in comments) — most notably
`0x10`'s "type" byte, which uses `0x04` as an evidence-adjacent guess (matching the EP0 identity
block's own Confirmed GC-specific `01 04 01` marker) rather than blindly copying Pro2's `0x02`.
Both boards + both diagnostic UF2s build clean. **Not yet re-tested against a real console.**

**2026-07-13, same day, eleventh pass — MILESTONE: real console re-test confirmed full recognition
and streaming input via report `0x0A`; a real 4-way button-bit rotation bug found and fixed.**
The command-family diff (tenth pass) closed the gap: the owner's real Switch 2 console now fully
recognizes the Pico as a genuine GameCube Controller and streams live input — the LED diagnostic is
no longer needed to make progress. Feedback from the console's own Test Input screen: pressing the
Modkit's physical L/R trigger detents lit up "ZL"/"ZR" (should read "L"/"R"), and the native Z
button — confirmed via genuine hardware to read "ZR" — instead lit up "L". Mapping all three
observations (plus the untested `SWITCH_MASK_ZL` bit inferred by elimination) onto
`switch_gc_encode_report()`'s byte0/byte1 bit positions revealed a clean 4-way rotation across
exactly the four Z/L-detent/R-detent/ZL slots — not a simple pair-swap, and not explained by
ndeadly's documented report-`0x0A` button table, which this encoder had followed byte-for-byte.
**Fixed** by applying the inverse rotation: Z now written to byte0 `0x20` (was `0x10`), L-detent
to byte0 `0x10` (was byte1 `0x20`), R-detent to byte1 `0x10` (was byte0 `0x20`), and
`SWITCH_MASK_ZL` to byte1 `0x20` (was byte1 `0x10`) — see the comment block in
`src/switch_gc/switch_gc_encode.c` for the full derivation. All 12 golden-test assertions touching
these four bits updated to match (76/76 checks still pass); report `0x05`'s encoder is untouched
(it has no bit position for these GC-native controls at all, so it was never in scope). Both boards
rebuilt clean, only `switch_gc_encode.c` recompiled. **Not yet re-tested against a real console** —
this is the next test, and the first one expected to validate a nearly-complete GameCube personality
rather than diagnose a stall.

**2026-07-13, same day, twelfth pass — real console re-test: Z and ZL confirmed correct, but L/R
detents still swapped with each other; fixed with a simple pairwise swap.** The eleventh pass's
4-way rotation fix was partially right: Z now correctly shows "ZR" and (implicitly) `ZL`'s slot is
correct, but the L/R detent pair was still crossed — pressing the physical L trigger's detent
showed "R" and vice versa. Since Z/ZL are now confirmed correct, this narrows to just the
L-detent/R-detent pair rather than requiring another full rotation. **Fixed**: swapped only those
two bit assignments in `switch_gc_encode_report()` — L-detent now byte1 `0x10` (was byte0 `0x10`),
R-detent now byte0 `0x10` (was byte1 `0x10`); Z (byte0 `0x20`) and `SWITCH_MASK_ZL` (byte1 `0x20`)
are unchanged. All 12 affected golden-test assertions updated (76/76 checks still pass); both
boards rebuilt clean, only `switch_gc_encode.c` recompiled. **Not yet re-tested against a real
console** — this is the next test, and per the owner's own assessment, expected to be the last
button-mapping fix needed before moving on to fidelity checks (analog trigger feel, D-pad, stick
ranges).

**2026-07-13, same day, thirteenth pass — real console re-test confirmed the twelfth pass's fix:
button mapping for the 8BitDo NGC Modkit is now correct.** Direct owner confirmation ("it works!").
Separately, tested Xbox and DualSense controllers (not the Modkit) in GameCube mode: face buttons,
D-pad, and sticks worked (shared generic code path), but shoulder buttons and triggers were wrong
— **Confirmed** the Pro2-style pairing (shoulders→plain L/R, trigger analog fold→ZL/ZR) is exactly
backwards for GameCube mode, which has no plain-L/R output bit at all (dead) and no ZR bit either
(only Z, which displays as "ZR"). **Fixed** in `router_submit_input()`
(`src/bt_hid/ns2_seam.c`): for any device *without* `gc_has_native_layout` (i.e. every controller
except the Modkit, whose own real per-button signals are untouched), shoulder LB/RB now map to
`SWITCH_MASK_ZL`/`GC_MASK_Z`, and analog trigger passthrough (already unconditional) gets a new
digital-detent synthesis at a high press threshold (`>224/255`, flagged Hypothesis — no real
GameCube trigger-to-detent curve exists for a substitute controller). Also broadened the existing
analog-fold suppression from "GameCube mode + `gc_has_native_layout`" to "GameCube mode,
unconditionally" — the old Pro2-style fold had no correct meaning for any device in this
personality. Full account: `docs/switch2-gc/mapping.md` "Generic controllers". Both boards + the
`NS2_PRO=OFF` scratch config build clean (this file is shared with the Switch-1 path). **Not yet
re-tested against a real console with a generic controller** — this is the next test.

**2026-07-13, same day, fourteenth pass — two real bugs found via hardware feedback: a stuck-on
rumble motor, and an asymmetric spurious "ZL" from generic controllers' own native trigger-click
bit; both fixed.** (1) **Rumble stuck on** (PC/Steam): once a nonzero rumble command arrived, the
motor stayed on forever with no path back to off if the host's actual "stop" packet happened to
still carry a nonzero framing byte — an inherent risk of this personality's deliberately-opaque
"any nonzero byte = vibrate" rumble decode (see the thirteenth-pass-adjacent Stage E note), which
cannot distinguish that case from Pro2's own real per-motor byte decode. **Fixed** with a watchdog,
not new byte decoding: a nonzero rumble command now latches the motor on for at most 500 ms
(`GC_RUMBLE_WATCHDOG_MS`, Hypothesis threshold — not derived from a measured host refresh cadence),
re-armed by each subsequent nonzero command, checked every `switch_gc_task()` tick; reset alongside
the rest of the personality's session-scoped state on init/reset/mount. (2) **Asymmetric spurious
"ZL"** (real console, generic controllers): confirmed the thirteenth pass's shoulder/trigger fix
mostly worked (right trigger correct), but the left trigger showed "ZL" partway through its travel
before correctly showing "L" at full plunge. Root cause: some pads (DualSense, several 8BitDo
tables — not Xbox, whose own button map has no L2/R2 destination) expose a genuine native digital
click bit for the trigger, distinct from both its analog axis and the seam's now-suppressed analog
fold; that native click bit was still flowing through the *old* per-family remap table into its
Pro2-appropriate legacy destination (`NS2_DST_ZL`/`NS2_DST_ZR`), stacking a second path on top of
the thirteenth pass's new shoulder mapping. Only the left side was visible because
`SWITCH_MASK_ZL` has a live bit in the GC encoder while `SWITCH_MASK_ZR` does not. **Fixed** by
clearing all four physical sources (`JP_BUTTON_L1/R1/L2/R2`) from the per-family remap loop's input
whenever the new generic-controller GC block already owns them, so nothing routes them a second
time. Both boards + the `NS2_PRO=OFF` scratch config build clean. **Not yet re-tested against real
hardware** — this is the next test for both fixes.

**2026-07-14, fifteenth pass — trigger fix confirmed working; rumble watchdog alone did not fix
PC/Steam's stuck-on motor, root-caused to an overly broad on/off check and narrowed.** The
fourteenth pass's watchdog didn't help because it can't distinguish "host stopped sending
anything" from "host keeps refreshing a nonzero-byte packet forever" — and the latter is likely
what Steam does. Re-examined the capture doc's own field breakdown
(`docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`): `data[0]` (its 1-indexed "byte1") is
the intensity-like value; `data[1]`/`data[2]` ("byte2"/"byte3") are a separate mode-selector field
that was nonzero in **every** captured sample regardless of intensity. The previous "off" check
OR'd all 4 bytes together, so a host whose real "stop" packet zeroes intensity but leaves its own
mode tag set would have been permanently misread as "still vibrating." **Fixed**: narrowed the
on/off decision in `switch_gc_hid_out_report()` to `data[0]` alone (the intensity byte
specifically) — not a new byte-semantics guess, a more precise reading of the same already-
Hypothesis field breakdown. The 500ms watchdog stays as a backstop for the case where a host stops
sending OUT reports entirely without an explicit off. Both boards build clean, only
`switch_gc.c` recompiled. **Not yet re-tested against real hardware** — this is the next test.

**2026-07-14, sixteenth pass — PC/Steam rumble confirmed fixed, but a real Switch 2 console still
showed erratic behavior; root-caused to a different problem and fixed.** The owner's follow-up
test: Steam no longer sticks on, but an actual Switch 2 console in-game reported the motor
"starts hardcore, stops, starts again, keeps going." Also asked directly whether GC shares Pro2's
HD-rumble format — no: this project's own capture analysis already found GC's rumble report is a
much simpler 4-byte field (vs. Pro2/Joy-Con2's 16-32-byte packed dual-LRA format), consistent with
a single basic ERM motor rather than two precision actuators, so it was never going to reuse
Pro2's decoder. The real bug: a real console driving actual gameplay haptics legitimately sends a
continuously *varying* intensity value — that's the entire point of dynamic rumble — and the
fifteenth pass's "any nonzero `data[0]` -> ONE fixed conservative amplitude" behavior collapsed
every one of those varying values to the same hard blast, then snapped instantly to 0 the moment
intensity dipped, turning genuine dynamic haptics into a jarring on/off switch riding on data that
was never binary. **Fixed** in `switch_gc_hid_out_report()`: pass `data[0]` straight through as the
motor amplitude (`report_set_rumble(0, data[0], data[0])`) instead of collapsing it to a constant —
uses the same byte the capture doc's field breakdown already calls intensity-like, just more
faithfully. Still Hypothesis-level (the 0-255 range's linearity as a true amplitude scale is
unconfirmed; the one existing capture only spans `0x50`-`0x68`), flagged as such in code and docs.
Both boards build clean, only `switch_gc.c` recompiled. **Not yet re-tested against real
hardware** — this is the next test.

**2026-07-14, seventeenth pass — real console (not Steam) rumble still erratic during actual
gameplay ("fires nonstop, then randomly stops"); re-analyzed the reference capture's timing (not
just byte content) and found the real bug in a shared BT-forwarding poll rate, not in the GC
decode itself.** The owner's report that Steam now behaves correctly but a real Smash Bros session
doesn't pointed at something specific to real console/game traffic. Re-opened
`rumble-procon-gccon.pcapng.gz` (previously only analyzed for byte content) and checked exact
frame timing for the first time: **the 8 "rumble test" samples are all isolated manual-test blips
(gaps of ~11s, then a ~200ms sweep through 6 values, then a ~20s gap to one final value), each
immediately surrounded by ZLPs** — meaning this capture has never contained real gameplay rumble
traffic at all, only a developer stepping through test values by hand. Separately, and more
useful: the raw OUT-token cadence on this endpoint (independent of payload) showed the console
polls it roughly every **4ms continuously**. Cross-checking that against `bthid_task()` (the
function whose per-device `.task()` forwards the current rumble state to the connected Bluetooth
pad) found it was only invoked once per **30ms** (`control_timer_handler`'s `CONTROL_TICK_MS`) —
almost 8x slower than the console's own polling rate. A real game driving fast, bursty rumble
(short repeated pulses, plausible for a simple ERM motor) could toggle state faster than this 30ms
sampling could observe; combined with the Xbox/generic BT rumble bridge's existing intentional
design (resend only on detected change, arm a ~10-minute hardware sustain per trigger, matching
xpadneo's own convention), a skipped "off" observation leaves the physical motor coasting on
whatever "on" trigger was last actually observed — a well-evidenced explanation for "fires
nonstop, then randomly stops" that has nothing to do with the rumble byte decode itself. **Fixed**
by adding a dedicated, much faster timer (`RUMBLE_TICK_MS = 3`, `src/bt_hid/ns2_bt_host.c`) purely
to poll `bthid_task()` more often — deliberately not by shortening `CONTROL_TICK_MS` itself, since
`control_tick`'s value is baked into every LED blink-rate calculation in that same handler; a
separate timer avoids any risk of regressing LED behavior for a fix that's specific to rumble
sampling. Also confirmed device address 8 in that capture really is the genuine GC controller (not
an address-reuse mixup with the file's other Pro Controller 2 traffic). Full account:
`docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md` "Timing re-analysis". Both boards + the
`NS2_PRO=OFF` scratch config build clean. **Not yet re-tested against real hardware** — this is
the next test, and the project still has zero real capture evidence of what sustained gameplay
rumble traffic actually looks like on the wire (a genuinely useful future capture target).

**2026-07-14, eighteenth pass — a new, more serious regression report (motor starts immediately on
GameCube-mode entry, before any gameplay effect) superseded the seventeenth pass's fix as
insufficient; stopped iterating on the decode blind and built a PC-side USB protocol lab instead
(`PROMPT.md`).** Per explicit project-owner direction, ceased speculative rumble tuning and did a
full static code audit of the rumble lifecycle *before* touching `switch_gc_hid_out_report()` or
`ns2_bt_host.c` again. **Audit results** (full detail, all 9 required questions answered with exact
code references: `docs/experiments/gcusb-rumble-lab-2026-07-14.md`): three of five candidate
hypotheses **refuted outright** by direct code reading — stale shared rumble state (refuted;
`switch_gc_init()`'s `report_set_rumble(0,0,0)` runs synchronously before every personality
reconnect, verified against `usb.c`'s exact transition sequence), a `g_usb_personality` race
(refuted; the reset doesn't depend on that variable's timing), and vendor-bulk commands being
misrouted into the HID rumble decoder (refuted; exhaustively verified every `report_set_rumble()`
call site in `switch_gc.c` — only one is gated behind an actual `report_id==0x03` HID OUT report).
**One real bug found** (relevance unconfirmed): `bthid_gamepad.c`'s `gamepad_task()` clears
`rumble_dirty` unconditionally even for non-Xbox devices, meaning this driver never sends ANY
rumble command — on or off — to a non-Xbox-vendor connected controller at all; which device family
was actually paired during the reported bug is unknown and unresolvable from code alone. Also
confirmed Sony DS4/DualSense (`ds4_bt.c`) uses a completely different always-refresh rumble model
with no multi-minute-sustain risk, unlike Xbox's — meaning the seventeenth pass's whole "10-minute
Xbox sustain" theory may not even apply to whichever device was actually connected.

**Main deliverable — `tools/gcusb`, a native Windows USB protocol lab.** Built per `PROMPT.md`'s
full spec: MinGW-w64/gcc directly against SetupAPI/WinUSB/HID (no libusb, no Zadig, no driver
changes), safety-critical `--target pico|genuine` resolution against actual `bcdDevice`
(`0x0111`/`0x0101`, refuses on any mismatch/ambiguity), ten subcommands (`list`/`describe`/`init`/
`read-input`/`send-command`/`rumble`/`rumble-sweep`/`stop-rumble`/`replay`/`compare`), an
allowlist mirroring `switch_gc_vendor_dispatch()`'s own command table exactly (memory writes,
pairing, and unknowns rejected by default), full rumble-experiment safety guarantees (amplitude/
duration clamps, Ctrl+C/exit force-stop, both candidate stop mechanisms tried independently,
explicit confirmation required to drive the genuine controller's real motor), and NDJSON +
human-readable transport logging. Pure allowlist/bcdDevice/NDJSON/replay-script logic lives in
`tools/gcusb/gcusb_core.c` (zero Windows-API dependency, 46 host-testable checks, all passing) —
mirrors this project's own established pure/impure split. **Verified this pass** (no hardware
connected): builds clean, every read-only/refusal CLI path exercised directly against a machine
with no matching USB devices attached (all correctly refuse rather than guess), and
`replay --validate-only` — a new hardware-free script-validation mode added specifically to satisfy
this requirement — correctly validates a real allowlisted init sequence
(`tools/gcusb/scripts/init_minimal.gcusb`) and correctly flags a deliberately-inserted
memory-write command as rejected. **Not yet verified against real hardware** (none connected while
building) — the WinUSB device-interface GUID used for enumeration is a documented best-effort
default with a `--winusb-guid` override escape hatch, flagged honestly as the most likely first
rough edge. **No firmware changes this pass**, per explicit instruction: do not produce another
build until the tool names the first bad transition. `DATA.md` replaced with a concise 9-section
handoff (previous 17-pass accumulated version was no longer efficient) — full chronological detail
stays here in `STATUS.md` and in the dated experiment docs. **Next step is entirely the owner's**:
run `PROMPT.md`'s deterministic experiment sequence with `tools/gcusb` against real hardware — see
`DATA.md` §9 for the exact runbook.

**2026-07-14, nineteenth pass — `gcusb` finally ran against real, currently-connected hardware
(Pico, GameCube mode, paired Xbox Series controller): fixed the WinUSB enumeration bug the owner
hit immediately, then two real, controlled tests (safe init handshake; a bounded rumble pulse with
an explicit stop) both completed cleanly with no stuck rumble — narrowing the bug meaningfully
without a full reproduction, and motivating two defensive fixes implemented from converging
evidence rather than a direct catch-in-the-act.** The owner's first live run
(`TEST.md`) hit exactly the rough edge flagged in the eighteenth pass: `describe`/`list` worked
(bcdDevice resolution correct for both Pico `0x0111` and, separately, the genuine unit `0x0101`),
but `replay`/any vendor-bulk command failed with "no WinUSB interface known" — the hardcoded
default GUID guess (`k_default_winusb_guid`) didn't match. **Root-caused directly against the live
device** via `Get-PnpDeviceProperty`/registry inspection: Windows stores the per-device WinUSB
interface GUID under `Device Parameters\DeviceInterfaceGUID` (**singular** — the code queried the
plural `DeviceInterfaceGUIDs`, a reasonable-sounding guess that was simply wrong), and the
hardcoded default was actually the device *class* GUID, not a device *interface* GUID — two
different Windows concepts conflated. **Fixed**: `find_registered_interface_guid()`
(`gcusb_win.c`) now walks the parent device's children via `cfgmgr32` (`CM_Get_Child`/
`CM_Get_Sibling`) to find the `MI_01` (vendor interface) child devnode and reads its real,
per-device-registered GUID directly from the registry, trying both the singular and plural value
names for robustness, falling back to the old hardcoded guess only if that lookup fails. Also fixed
a related latent bug found in the same area: `Container ID` was being read as a GUID-typed device
property but treated as wide-character text (`WideCharToMultiByte` on raw GUID bytes), printing as
`????????` and fragile for cross-referencing (a GUID's raw bytes can contain an embedded
` ` that would truncate a `wcscmp` early) — now stored as a proper `GUID` and compared with
`memcmp`, with a real `format_guid()` for display. **Confirmed working end-to-end against the real
Pico**: `list`/`describe` now report `WinUSB interface found: yes`; `init --target pico --profile
steam` (Initialise USB + Select Input Report, zero rumble writes) completed with exactly the
documented response bytes; the controller was not rumbling before or after. A bounded rumble test
(`rumble --target pico --amplitude 32 --duration-ms 200`, clamped, auto-stop) also completed
cleanly — the owner confirmed the controller was not rumbling afterward.

**These two clean results, combined with the owner's own baseline notes** (same Xbox controller:
real Switch 2 console → immediate rumble that persisted until unplugged; PC + a Switch-1-emulator
host → maxed-feeling rumble that eventually stopped on its own) **now converge on a specific,
though still not directly reproduced, hypothesis**: neither passive input reading, the safe
Steam-style handshake, nor a controlled pulse-with-explicit-stop reproduces the bug — so the real
trigger is very likely a genuine nonzero rumble command that the *host itself* (real console, or
the emulator) never follows up with an explicit stop, combined with the Xbox-specific ~10-minute
hardware sustain this project's own rumble bridge already relies on (`loop_count=0xEB`, matching
xpadneo's documented Windows-driver-compatible convention). Researched this class of bug directly:
`atar-axis/xpadneo` issue #400 ("8BitDo Pro 2 non-stop rumble on connect") documents a near-identical
symptom — a deliberate "connection confirmation" rumble effect that never stopped for controllers
whose stop command didn't actually clear the enable/motor-mask bits, only the magnitude (fixed in
commit `94ad82a`). The owner declined the fully-confirming live test (send one pulse, deliberately
withhold the stop, time how long it takes to self-resolve — bounded but requires several minutes
of the real controller buzzing) in favor of continued autonomous investigation, so this remains
**Strong, not Confirmed** evidence — no direct reproduction was captured.

**Implemented two defensive fixes from this converging evidence, both satisfying explicit
`PROMPT.md` invariants regardless of the exact trigger** (not gated behind a full reproduction,
since `PROMPT.md` itself separately requires these invariants "regardless of root cause"):
1. **`bthid_gamepad.c`'s Xbox rumble path now clears the enable/motor-mask bits AND the
   sustain/loop-count fields entirely on a genuine stop** (`left==0 && right==0`), rather than
   only zeroing the motor magnitude while leaving `pulse_sustain_10ms=0xFF`/`loop_count=0xEB`
   armed — directly mirroring xpadneo's own fix for the same bug class. Costs nothing and is
   strictly more correct regardless of whether it's confirmed to be *this* project's exact trigger.
2. **A monotonic rumble generation counter** (`report_get_rumble_gen()`, `include/report.h`/
   `src/report.c`; consumed in `ns2_seam.c`'s `feedback_get_state()`) replaces plain left/right
   value comparison for dirty-flag detection — this is the exact mechanism `PROMPT.md` itself
   names ("a monotonic rumble generation counter consumed exactly once per change") for its
   explicitly-flagged concern that faster polling alone (the seventeenth pass's fix) cannot
   guarantee a stop transition is never lost merely because the sampled value happened to
   round-trip back to what a consumer had already seen. `report_set_rumble()` now increments
   the generation on every call unconditionally, so a same-value read can never be mistaken for
   "no new event happened."
3. Also captured for the audit record but **not changed this pass** (out of scope, no new
   evidence): `bthid_gamepad.c`'s per-family non-Xbox `feedback_clear_dirty()` behavior (found
   eighteenth pass) remains as documented/intentional — an honest capability gap for non-Xbox
   devices, not itself implicated now that the connected device is confirmed Xbox.

Both boards + the `NS2_PRO=OFF` scratch config build clean; existing host test suites
(`test_hid_out_normalize`, `test_switch_gc_report`) unaffected, still passing. **Full account,
including the exact `Get-PnpDeviceProperty` commands used for live diagnosis and the xpadneo
research trail**: `docs/experiments/gcusb-rumble-lab-2026-07-14.md`. **Honestly still not a
confirmed root-cause fix** — no live reproduction was captured, only a well-evidenced hypothesis
and defensive hardening. **Not yet re-tested against real hardware with these firmware changes** —
this is the next test, ideally re-running the original failure scenario (real Switch 2 console,
Pro2→GameCube switch) now that both fixes are in place.

**2026-07-14, twentieth pass — real console re-test with the nineteenth-pass fixes: the immediate
full-strength rumble on GameCube-mode entry is gone, but real Smash Bros gameplay revealed a
different, more precisely-diagnosable bug — a rapid stream of small, legitimate rumble ticks was
smearing into one continuous "powerful" buzz that only stopped on scene transitions where nothing
sends any rumble at all, and persisted briefly even right after pausing.** This is a materially
different symptom shape from the original report, and each detail maps precisely onto a specific
mechanism: the Xbox rumble bridge (`bthid_gamepad.c`) set `pulse_sustain_10ms = 0xFF` (~2550ms) on
**every** nonzero trigger regardless of amplitude, with no release gap (`pulse_release_10ms = 0`).
Real gameplay rumble (unlike the nineteenth pass's own single-pulse-with-explicit-stop test) is a
continuous stream of separate, deliberately brief/small ticks — if any two land within ~2.55s of
each other (near-guaranteed for a "textured" rumble sent every few tens of ms), each new trigger
re-arms a multi-second hold before the previous one can decay, smearing the intended pulsing
texture into one sustained motor engagement. This precisely explains all three reported details:
"powerful continuous" (constant re-arming, never allowed to decay), "only stops on a transition
screen where normally nothing sends any rumble" (the only way to get a true ~2.55s gap with zero
triggers), and "still rumbles right after pausing" (whatever trigger landed in the last ~2.55s
before pause keeps holding regardless). **Fixed**: shortened `pulse_sustain_10ms` from `0xFF` to
`0x05` (~50ms) for a genuine (non-stopping) trigger — long enough to feel like a distinct tick,
short enough that a stream of separate small ticks now reads as a texture rather than one sustained
buzz. `loop_count` stays `0xEB` for a genuine trigger, but with the much shorter per-pulse sustain
the worst-case total duration if a trigger were somehow never followed by anything else drops from
~10 minutes to ~11.75s (`235 * 50ms`) — a much safer bound while this remains unconfirmed. Both
`stopping` fields (from the nineteenth pass's fix) are unchanged. Both boards build clean, only
`bthid_gamepad.c` recompiled. **Not yet re-tested against real Smash Bros gameplay** — this is the
next test.

**2026-07-14, twenty-first pass — documentation/hypothesis audit (no hardware available this
pass, per explicit instruction): found and fixed a foundational error in the GC rumble byte model
by reading the real Linux kernel "HID: nintendo" driver source, and built a "refuted hypotheses"
archive.** Per the project owner's request, read
`https://marc.info/?l=linux-input&w=2&r=1&s=hid+switch2&q=b` — a real Linux kernel patch series
(Vicki Pfau, v11: "Add preliminary Switch 2 support" / "Add rumble support for Switch 2" / "Add
unified report") — for insight into this project's own implementation. **Finding, Strong (not yet
independently hardware-confirmed): the genuine GameCube controller has no continuous-amplitude
rumble hardware at all.** It's a simple ERM motor with exactly three states
(`GC_RUMBLE_OFF=0`/`ON=1`/`STOP=2`, at `data[1]`, per the kernel's own `enum gc_rumble` and
`switch2_rumble_work()`); real hosts simulate continuous perceived amplitude via delta-sigma/
error-accumulation duty-cycle modulation of that ON/OFF state, not via any single byte's magnitude.
`data[0]` — what every rumble revision since 2026-07-13 had read as "intensity" — is actually an
unrelated, incrementing sequence/command byte. This single correction **retroactively explains the
entire multi-revision rumble bug arc**: a rolling sequence byte is essentially never exactly zero,
so every real rumble packet looked like "some nonzero amplitude" regardless of the game's actual
OFF/ON/STOP intent, fully consistent with both major symptoms observed (immediate rumble on entry;
small gameplay rumbles becoming one continuous buzz). The nineteenth/twentieth passes' downstream
fixes (Xbox stop-bit clearing, generation counter, shortened envelope) remain independently correct
and necessary — they govern how the *trigger* is delivered and held once identified, which matters
regardless of which upstream byte the trigger comes from — but the trigger itself was being
misidentified this whole time. **Fixed**: `switch_gc_hid_out_report()` (`src/switch_gc/switch_gc.c`)
now reads `data[1]` as a proper state enum (`gc_rumble_state_t`) instead of `data[0]` as amplitude;
`tools/gcusb`'s `gcusb_build_rumble_data()` updated to emit the corrected wire format, and
`rumble-sweep` repurposed from a meaningless "amplitude sweep" into a toggle-cadence test (the
actually relevant variable under this model). **New**: `docs/experiments/refuted-hypotheses.md` —
an archive of hypotheses this project has confirmed wrong, created per explicit instruction so
this exact byte-model mistake (and several others: the OR-all-4-bytes heuristic, three
personality-transition hypotheses already refuted by the eighteenth pass's audit, and the Xbox
`pulse_sustain_10ms=0xFF`-is-fine assumption) don't get re-investigated blind in a future session.
`docs/switch2-gc/protocol.md`'s "Output Report `0x03`" section rewritten with the corrected model
and full kernel-source citation; `docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`'s own
now-superseded byte-grouping analysis marked with a clear banner (raw samples kept as valid ground
truth, only the interpretation is struck). Both boards + `tools/gcusb` build clean; all 47
`gcusb_core` tests pass (one new check added for the corrected wire format). **Not
hardware-validated this pass** (none available) — this is Strong evidence from an authoritative
external source, not yet Confirmed by this project's own hardware test.

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
| Build system | ✅ | Both boards build clean from one tree; `build.ps1`. bluepad32 removed from CMake. **2026-07-12:** the `-DNS2_PRO=OFF` (plain Switch-1) configuration had silently failed to *link* since 2026-07-10 (`config.c`'s gyro debug commands called NS2-only functions with no guard) — `build.ps1` only exercises the default `NS2_PRO=ON` config, so this went unnoticed for two days. Fixed (guarded); now explicitly re-verified alongside every default build going forward. |
| Bluetooth stack | ✅ | Vendored joypad-os bthid (`src/bt_hid`) + BTstack/CYW43. All vendor drivers enabled. |
| Switch 2 USB identity | ✅ | Composite IAD device; EP0 vendor handshake byte-exact vs a real PC2; console accepts it. |
| Button/stick output | ✅ | report 0x09 (console) + report 0x05 (PC). GL/GR/C confirmed on-console. 1000 Hz poll (bInterval 1, 2026-07-12 — 🔵 deviation from genuine 250 Hz, untested on console). |
| Controllers | ✅ | DualSense/DualSense Edge, Xbox Series/Elite 2, Switch/8BitDo/etc. via joypad-os drivers. |
| Extended buttons | ✅ | Edge back paddles/Fn, Elite 4 paddles → GL/GR + Capture/C (name/report-based detection). |
| Rumble | 🟢 | **2026-07-12: restored on real hardware** for Xbox, DualSense, Switch 1 Pro Controller, and Wiimote both standalone and with joystick attachment. The audit found live-path/registry mistakes, corrected Xbox output behavior, Switch Pro encoding, and related driver defects; see `docs/bluetooth/btstack-implementation.md` "Rumble regression". Validation confirms vibration is present, not yet per-motor fidelity, curves, stop/reconnect, Xbox Classic input, generic non-Microsoft XInput, DS4, or Wiimote channel semantics. |
| Config web UI | ✅ | Live input↔output view, dynamic per-controller menu, per-device remapping, lightbar, raw-report debug. |
| PC / Steam | ✅ | Enumerates as a Switch 2 Pro; report 0x05 incl. gyro works. |
| Gyro (report 0x09) | 🔴 | **Paused 2026-07-10** (not abandoned) pending new evidence. int32 phase + Q16.16 accel, `0x0C`-gated — format itself well-validated. **HW test 1:** drift found → bias tracker added → **HW test 2: no improvement** → root-caused to a self-defeating (magnitude-based) stillness gate → **fixed (take 2, derivative-based gate + live bias/still debug readout)**. `anom=0` on hardware ruled out a local computation defect without validating value semantics. See `docs/switch2/report-0x09-motion.md`. |
| NFC / Amiibo | 🔵 | **Active RE target since 2026-07-10.** Command `0x01` confirmed real on genuine Pro Controller 2 (two exchanges traced to exact packets in this repo's own USB capture); no tag transaction ever observed; no NFC IC identified in either controller. **2026-07-12:** the response `dir` byte self-consistency gap is fixed (`dir=0x04` for bare acks, matching the genuine capture). See `docs/switch2/nfc-protocol-inventory.md`. |
| Gyro (report 0x05 / Steam) | 🟢 | Experiment A fix (timestamp + scale) holds on HW: Steam detects gyro. **HW test 1:** pitch/roll swapped → **fixed** (axis order re-derived from genuine capture, `ns2_seam.c`). **HW test 2: confirmed working** (Steam calibration now usable). Roll sign still unverified independently. `docs/experiments/gyro-hardware-validation-2026-07-10.md`. |
| BT pairing reliability | 🟢 | **2026-07-12: two distinct root causes found and fixed** — (1) BLE reconnect cascade fired on intentional disconnects too, starving scan for up to 50s; (2) GATT init-command retry used a never-reset call-count timer (~33Hz actual vs. assumed ~120Hz), unbounded, no recovery. Both build-verified, **hardware validation pending**. See `docs/bluetooth/btstack-implementation.md`. |
| BLE VID/PID (DIS) | 🟡 | Often resolves to 0; worked around by name + report-length detection. |

---

# Working Features (hardware-validated)

- **Console:** detected as a native Switch 2 Pro Controller; all buttons (incl. **GL/GR/C**), sticks,
  D-pad, and rumble output correctly. Behaves as a wired controller (the dongle is USB-wired).
- **1000 Hz USB poll** (bInterval 1, raised from the genuine-matching 250 Hz on 2026-07-12) — a
  deliberate latency-over-fidelity deviation, 🔵 untested on console hardware (see
  `docs/switch2/usb-spec.md` §13).
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
- **BT pairing reliability (🟢 root cause found and fixed, 2026-07-12 — hardware validation
  pending).** Traced the full BLE connect/disconnect lifecycle in `btstack_host.c` end to end
  (not inferred from names): every BLE disconnect, regardless of reason, triggered an up-to-5×
  blind direct-`gap_connect()` reconnect cascade to the single last-known address
  (`BLE_CONNECT_TIMEOUT_MS`=10s each, ~50s worst case) — including disconnects the peer initiated
  on purpose (controller powered off / explicitly disconnected). During that whole window,
  scanning is stopped, so the host can't discover the controller (or anything else) coming back
  under a fresh connection. Confirmed this exists **only** on the BLE path (used by the real
  Switch 2 Pro Controller/Joy-Con 2, matching the documented "Pro 2" symptom exactly) — the
  parallel Classic BR/EDR path (DualSense/Xbox/8BitDo/etc.) has no such cascade and needed no
  change. **Fix:** the disconnect-reason byte (captured but previously never consulted) now gates
  the cascade — `ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION`/`..._DUE_TO_POWER_OFF` skip straight
  to resuming scan instead of burning the retry budget on a device that isn't coming back;
  every other reason (e.g. `CONNECTION_TIMEOUT`) keeps the existing 5-attempt cascade unchanged.
  Full lifecycle trace, Phase-1/2 findings, and the exact hardware validation procedure:
  `docs/bluetooth/btstack-implementation.md` "Reconnect reliability". Both boards build clean
  (`NS2_AUDIO` on/off, `NS2_PRO` off) — **not yet tested on real hardware.**
  **Second, distinct defect found and fixed the same day**: `switch2_retry_init_if_needed()` (the
  post-link GATT pairing-command retry, unrelated to the link-layer reconnect above) used a
  never-reset call-count modulo instead of real elapsed time — confirmed ~33Hz actual caller
  (not the assumed ~120Hz), unbounded retries, no recovery path if a step never got acknowledged.
  Replaced with a real monotonic deadline, bounded at 10 retries with an explicit
  disconnect-and-let-the-other-fix-reconnect recovery transition. See
  `docs/bluetooth/btstack-implementation.md` "Switch 2 GATT init retry timing". Also build-verified,
  also **not yet tested on real hardware.**
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
  this doc's "genuine ~0.03 dps" figure). See `docs/switch2/report-0x09-motion.md`. **2026-07-12:**
  `0x13100` was decoded but never actually served (`ns2_factory_init()` had no entry for it — any
  real read there got zero-fill) — fixed, and independently cross-validated by a fresh decode of
  the console reading this exact address in `ndeadly`'s raw USB capture (a different physical
  unit, also showing near-gravity accel bias). See `docs/switch2/usb-spec.md` §14.
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
  `docs/experiments/gyro-hardware-validation-2026-07-10.md` §7. These constants are tuned assuming
  a ~250 Hz caller; since 2026-07-12 the USB poll rate is 1000 Hz, so `ns2_motion_tick()` is now
  called through a shared `ns2_motion_tick_gated()` rate limiter to keep this assumption intact
  (`switch_pro2.c`) — noted here so a future resumption of gyro work doesn't need to rediscover why
  that gate exists.
- USB poll rate (1000 Hz, `bInterval 1`) is a fidelity deviation from the genuine controller's
  250 Hz, **untested on console hardware** — see `docs/switch2/usb-spec.md` §13.

---

# Next Recommended Tasks

1. **Hardware-validate Gate 2's 2026-07-12 identity/driver-binding work — still highest priority for
   the identity/logging system itself (fix (c) in item 7 above takes priority for actual pairing
   reliability).** Reachability audit, fresh upstream comparison, the `bthid_transport_mask_t`
   structural fix, the `ds4_bt.c` VID-reject fix, and the new `bt_identity_log.c`/`btid dump` facility
   are build-verified; hardware coverage so far: 8BitDo NGC Modkit (Classic, full profile
   confirmed), **Switch 2 Pro Controller (BLE, identity/driver-binding confirmed clean, 2026-07-13
   — see item 7(c) and the experiment doc "Capture 2")**, and unconfirmed-but-reported-pairing-
   successful passes for Xbox/DualSense/a generic Switch Pro Controller (no `btid dump` pulled for
   those yet). Still fully untested: Switch 1 Pro Controller, Wiimote +/- attachment, generic/XInput
   with a captured `btid dump`, 8BitDo NGC DIY's second "Android/D-Input" mode. Specifically still
   watch: (a) does Classic BT Xbox input still parse correctly now that `xbox_bt.c` is live (flagged
   as this pass's top risk — its report-format detection was never hardware-verified); (b) does
   `ds4_bt.c`'s new VID-reject guard leave DS4 itself unaffected. Full detail:
   `docs/bluetooth/driver-reachability-audit.md`,
   `docs/bluetooth/joypad-os-upstream-comparison-2026-07-12.md`,
   `docs/bluetooth/btstack-implementation.md` "Gate 2" section.
2. **Continue Gate 2**: audit the remaining drivers (`ds3_bt`, `ds5_bt`, `wii_u_pro_bt`,
   `wiimote_bt`, `xbox_bt`, `xbox_ble`, `stadia_bt`) for the same "known-contradicting-VID should
   reject before name fallback" pattern already fixed in `ds4_bt.c` — not yet done for the rest.
   Preserve narrower rumble follow-ups: left/right fidelity, stop/reconnect, generic non-Microsoft
   XInput, and the flagged Wiimote channel question.
3. ~~8BitDo NGC DIY: once `btid dump` is validated on hardware, use it to capture the controller's
   real identity, then decide whether a small profile layered on the generic driver is safer than
   a standalone driver~~ **DONE 2026-07-12.** `btid dump`/`btid desc` validated end-to-end on real
   hardware in the same session (see the "First real hardware validation" narrative above); a
   dedicated PID-specific profile (`0x2DC8:0x286A`, not the shared 8BitDo VID) was implemented,
   found and fixed a real seam-level architecture conflict (`ns2_seam.c`'s analog-to-digital
   trigger fold), and was confirmed working end-to-end by the controller's owner in Switch 2 Pro
   Controller mode. **New follow-up promoted from this item**: the second "Android/D-Input" BLE
   pairing mode this controller reportedly also supports has not been captured — needs its own
   `btid dump` session and likely its own profile before assuming this mapping covers it. The NSO
   GameCube USB output personality (Gate 3) remains not started; do not begin it until Gate 2's
   remaining controllers are validated, per DATA.md's explicit sequencing. See
   `docs/bluetooth/8bitdo-ngc-diy-profile.md` "Future NSO mode" for what's expected to change.
4. **Hardware test the 2026-07-12 poll-rate bump (cheap, fast, blocks nothing else — can run
   alongside item 1).** Flash the current build and confirm on a real Switch 2 console: does the
   dongle still get detected/initialised at `bInterval 1` (1000 Hz)? bInterval has never been
   implicated in a past console compatibility gate, but is unconfirmed at this value specifically.
   If it regresses detection, the fallback is a partial step-down (e.g. `bInterval 2` = 500 Hz)
   rather than a full revert to 250 Hz. See `docs/switch2/usb-spec.md` §13.
5. ~~NFC — proper report-0x09 endpoint-filtered time series~~ **DONE 2026-07-12, conclusive
   negative result.** `tools/extract_report09_timeseries.py` (new, parses the real USBPcap header
   struct) proves `genuine_procon_2.pcapng` contains **zero** report-`0x09` records anywhere — it's
   a PC/Windows session with the real controller, which only ever streams report `0x05`
   (19,554 real samples, confirmed device 38). The NFC-state question needs an actual console-side
   USB capture, which this project has never obtained for any purpose (same gap as report-0x09
   gyro). Two incidental doc errors fixed in the process: a device-number mix-up (this file's real
   Pro Controller 2 is device 38, not 7 — "device 7" was a fact about a different, external capture
   file that had been mistakenly cross-applied) and a mis-read report-select target (selects report
   `0x05`, not `0x09`, in this capture). See `docs/switch2/nfc-protocol-inventory.md` §2.5. **New
   next task, promoted from this one:** acquiring a genuine console-side USB capture at all is now
   the shared blocker for both NFC-state and report-0x09-gyro ground truth — see item 6 below (the
   USB-relay feasibility audit) for the most-developed path toward getting one.
6. **Genuine-controller USB report 0x09 — run the relay audit's recommended Phase 0 (gyro,
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
7. **BT pairing reliability — hardware validation (promoted 2026-07-12; three distinct root causes
   found and fixed, now highest priority — this is the specific symptom the controller's owner has
   reported twice).** (a) The BLE link-layer reconnect path fired an unconditional up-to-50s
   blind-reconnect cascade on *every* disconnect, including intentional ones, starving scanning the
   whole time — present only on the BLE path (the real Switch 2 Pro Controller/Joy-Con 2), matching
   the documented "Pro 2" symptom exactly; fixed by consulting the already-captured-but-unused
   disconnect reason byte. (b) The separate GATT pairing-command retry
   (`switch2_retry_init_if_needed()`) used a never-reset call-count timer assuming ~120Hz but
   actually driven at ~33Hz, was unbounded, and had no recovery path if a step never got
   acknowledged; fixed with a real elapsed-time deadline, bounded retries, and an explicit recovery
   disconnect that composes with fix (a). (c) **2026-07-13, root-caused directly from the owner's
   report of unreliable explicit-pairing-mode connects**: the pairing window's expiry unconditionally
   stopped scanning even mid-connect, silently disarming the `BLE_CONNECT_TIMEOUT_MS` watchdog for
   whatever attempt was in flight; fixed by deferring the close until that attempt resolves, bounded
   by the existing 10s watchdog rather than a new constant. All three build-verified across the full
   four-combination matrix; **need a real hardware pairing/reconnect test** — exact procedures in
   `docs/bluetooth/btstack-implementation.md` "Reconnect reliability", "Switch 2 GATT init retry
   timing", and (new) "Pairing window vs in-flight connect". The genuine Switch 2 Pro Controller used
   for fix (c)'s root-cause report is the natural test unit — its identity already independently
   confirmed clean via `btid dump` (see item 1 below and the experiment doc's "Capture 2").
8. **Gyro — report 0x05 roll-sign verification (paused with gyro).** Axis order is confirmed
   working; roll's sign was inferred from a determinant constraint, not measured.
   `gyro-hardware-validation-2026-07-10.md` §4 has a tighter recapture protocol if/when resumed.
9. ~~BLE DIS VID/PID — resolve the PnP query so detection doesn't rely on names~~ **Substantially
   addressed 2026-07-12 by Gate 2** — the PnP-ID parsing itself was never the problem (confirmed
   spec-correct); the real cause (DIS query timing relative to driver binding) is an inherent,
   shared-with-upstream constraint that can't be "resolved" by reordering without reintroducing a
   prior GATT-contention regression, so the fix targets re-evaluation robustness instead (now
   hardened for `switch_pro_bt`/`ds4_bt`; `ds3_bt`/`ds5_bt`/`wii_u_pro_bt`/`wiimote_bt`/`xbox_bt`/
   `xbox_ble`/`stadia_bt` still need the same audit pass — see item 2 above). Detection no longer
   needs to "not rely on names" as a goal in itself — name-based matching is legitimate,
   evidence-ranked fallback behavior now that it correctly yields to contradicting VID/PID. Full
   detail: `docs/bluetooth/btstack-implementation.md` "Gate 2" section.
10. **Docs** — finish `/docs` (architecture, protocol, RE methodology) per CLAUDE.md.
11. **Gyro — report-0x09 encoder, paused.** Do not resume without new evidence. BLE-derived
   value-semantics hypotheses are no longer expected from item 6 (BLE-block decoding itself is
   paused) — the console-side USB relay audit (item 6) remains the documented path to real
   evidence whenever gyro work resumes; apply anything it eventually yields as a *hypothesis* to
   test, not a fact to encode directly.
