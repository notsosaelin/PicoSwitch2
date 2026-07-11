# Experiment Report: v2 Feature-Enable Matrix — GATT Ground Truth + First Observed Independent Motion-Correlated Data (2026-07-10)

> **🔴 PAUSED (2026-07-10) — BLE 40-byte block semantic decoding is on hold, insufficient
> discriminating evidence to continue productively.** Every tested passive-statistical approach
> (direction correlation, orientation-invariant scalar/vector interpretation, periodic native-FIFO
> packet structure) failed to converge, and per explicit direction, this repo will not chase
> another indirect physical test against the same opaque dataset. **What remains true and durable,
> not retracted by this pause**: the block is independently framed (raw offsets 15-54, a
> length-prefix byte, a constant-zero tail); it responds strongly and reproducibly to physical
> activity (§13); its internal encoding and semantics remain genuinely unknown. Captures and
> tooling are preserved for whenever a better observation point (see
> `docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`) makes renewed analysis worthwhile.
> This does **not** pause gyro or Switch 2 controller reverse-engineering as a whole — see that
> audit for the active work.

**Status:** 🔵 Major positive result. The controlled-motion capture (§13) found strong, cross-
validated, whole-block evidence that the 40-byte block responds to physical handling of the
controller — not via the clean directional rate/orientation signal originally hypothesized for
Fields A/B (§8), but via a dramatic, block-wide noise-floor increase (2-3 orders of magnitude)
whenever the controller is being moved or even just held in a strained position, vs. near-silence
at genuine rest. No axis or physical-unit semantics assigned. Trigger cause (§6/§9) still narrowed
to a concrete, measured candidate but not confirmed.
**Confidence legend:** ✅ Confirmed (directly demonstrated by the cited capture records) /
🔵 Strong evidence (consistent, reproducible pattern across multiple independent captures) /
🟡 Hypothesis (plausible, not independently verified) / ⬜ Unknown.
**Continuation, same day**: §8-§12 were added in a follow-up pass, prioritizing "what does the
block encode" over "what triggers it" per explicit instruction, using a new companion tool
(`tools/analyze_sw2_motion_block.py`) and a new capture-annotation marker mechanism
(`sw2cap mark <text>`, described in §11). §13 analyzes the resulting controlled-motion capture,
supplied by the user the same day. **§14 (same day, second continuation)**: investigated whether
the 40-byte block matches a documented native IMU FIFO packet structure (the controller's IMU is
identified elsewhere in this repo as an ICM-42670-P). **No candidate periodic packet layout
(2×20, 4×10, 5×8, 8×5) survived structural testing** — no repeating low-entropy header byte or
bit-field was found at any tested spacing. The block-wide activity/noise-floor finding (§13)
remains the strongest evidence that real IMU data is present; its internal packing remains
unresolved. Ends with a proposed mechanically-supported (non-handheld) experiment to separate
"responds to genuine vibration" from "responds to hand contact" as the next step, per explicit
instruction not to request another handheld test yet.

---

## 1. Question

Two questions, per the task that produced this pass:

1. What is the genuine live GATT structure of a Pro Controller 2, and what does raw handle
   `0x000C` (the "report rate" write target used by v2 experiment variants 3/4/6) actually
   represent — resolving `docs/switch2/ble-controller-protocol-inventory.md` §3.7.1's
   paper-derived, explicitly-unconfirmed hypothesis?
2. Did any of the six v2 experiment variants (`docs/switch2/ble-controller-protocol-inventory.md`
   §2.7.2) produce a notification stream containing data that is independent of buttons/sticks/
   counters/shifted-duplication — i.e. genuinely new, potentially motion-bearing content?

## 2. Method

Eight hardware captures, run through the config web UI's "Switch 2 BLE Capture" panel
(`dumps/sw2_capture_2026-07-10_{BASE-NO-VARIANT,VARIANT-1..6,NO-VARIANT-GATT-DISCOVERY}.ndjson`):
a passive baseline, a one-shot GATT discovery run, and one capture per experiment variant, each
requiring a fresh controller power-cycle (per §9's procedure). Analyzed completely and
programmatically with a new tool, `tools/analyze_sw2_v2_captures.py` (integrity + full ordered
timeline reconstruction, GATT-hierarchy reconstruction from the discovery capture, per-offset
byte statistics, cross-handle/cross-variant equality checks) — read-only, source NDJSON
untouched. Follow-up ad hoc `python3 -c` checks (reproduced inline below) filled in specific
numeric questions (drift rates, byte-boundary transition-rate signatures, structural byte
constants) once the tool's output identified where to look.

## 3. Results

### 3.1 Integrity (all 8 files) — ✅ Confirmed

Zero parse errors in any file. All records well-formed. **`dropped`-ring-overrun counts are not
present in the exported NDJSON** — that field is only ever reported live via `sw2cap stat`/`drain`
during the session itself, not persisted per capture entry; this is a real gap in what can be
verified retroactively from a file alone (see §6 for a note on hardening this later, not
implemented this pass).

Reconnect-attempt visibility: `NO-VARIANT-GATT-DISCOVERY` and each `VARIANT-*` file shows exactly
one `ccc_write` to `0x001B` (the ACK-notify CCC, written once per connection attempt at the very
start of init) — i.e. every file represents exactly one clean connection attempt, no stalled/
retried connections this round (unlike the earlier `EXPERIMENTAL.ndjson` session, which showed
one stalled attempt before a second succeeded).

### 3.2 Every variant executed exactly as designed — ✅ Confirmed

Cross-checked each file's `cmd_out` sequence against `SW2_V2_VARIANTS[]` (`btstack_host.c`):
variant marker present and correct; configure/enable flag bytes match design exactly (0x07 or
0xFF as specified); calibration-read variants (5, 6) sent all six reads in the exact designed
order and address/size (verified against `SW2_V2_CAL_READS[]` — after excluding the *normal init
sequence's* `READ_INFO` SPI read, which shares the identical `cmd=0x02/subcmd=0x04` framing at
address `0x13000`, a value that does not appear in the calibration list and is unambiguous to
exclude); the descriptor write fired only for variants 3/4/6, always with bytes `85 00`; variant
6's `0x000E` CCC subscribe was measurably the *last* v2 operation in its sequence (after configure,
all six cal reads, enable, and the handle write), while variants 1-5 subscribed first — exactly the
deferred-vs-first design distinction. **No missing, duplicated, truncated, rejected, or reordered
operation was found in any variant.** Every `ack` entry across all six files begins `byte1=0x01`,
the same success-shaped pattern every previously-confirmed ACK in this project has shown; no error
status was observed anywhere. **Gap found, not a rejection**: the CCC-write and descriptor-write
*completion* callbacks (`switch2_v2_ccc_write_callback`, `switch2_v2_handle_write_callback` in
`btstack_host.c`) only `printf()` their ATT status — they never call `sw2_capture_record()` — so
whether the `0x000C` write specifically was accepted or rejected at the ATT layer **cannot be
determined from these capture files**; only its *effect* (whether new data appeared) is
observable. See §6 for the fix, not implemented this pass per the task's scope.

### 3.3 GATT ground truth resolves `0x000C` — ✅ Confirmed, corrects prior paper-only estimate

`NO-VARIANT-GATT-DISCOVERY` captured a complete walk: 4 services, 19 characteristics, 14
descriptors. Full table in the tool's output; the relevant part:

```
decl=0x0009  value=0x000A  end=0x000C  props=READ|NOTIFY  uuid=ab7de9be89fe49ad828f118f09df7fd2
    desc=0x000B  uuid=0x2902                                              (standard CCC)
    desc=0x000C  uuid=679d55105a244dee955795df80486ecb                    (vendor-specific)
decl=0x000D  value=0x000E  end=0x0010  props=READ|NOTIFY  uuid=7492866cec3e4619825832755ffcc0f8
    desc=0x000F  uuid=0x2902                                              (standard CCC)
    desc=0x0010  uuid=679d55105a244dee955795df80486ecb                    (SAME vendor UUID)
```

`0x000C` **is** a descriptor of the `0x000A` characteristic (declaration `0x0009`) — **confirming**
the inventory doc §3.7.1 correction made before this pass ("0x000C, not 0x000D; a descriptor of
`0x000A`, not `0x000E`"), and refuting the *original* (now-doubly-superseded) `0x000D` guess.
`SW2_REPORT_RATE_HANDLE_HYPOTHESIS` should be considered **resolved for its raw-handle identity**.
Its UUID (`679d5510-5a24-4dee-9557-95df80486ecb`) is **not** a standard Bluetooth SIG descriptor
(not Report Reference `0x2908`, not Characteristic User Description `0x2901`) — it's vendor-defined,
consistent with "report rate" or similar being a real but proprietary control, not a misreading.

**New finding, not previously documented anywhere:** `0x000E` has its **own** copy of this exact
same vendor descriptor, at handle `0x0010`. The v2 variants' handle-write (`85 00` to `0x000C`)
targets `0x000A`'s copy — a faithful reproduction of `switch2_input_viewer.py`'s literal behavior
(its `input_handle` is fixed to the `0x000A` pairing at connection time, regardless of which
handle is later read from) — but this means **the write variants never touched `0x000E`'s own
report-rate descriptor (`0x0010`) at all**. If per-characteristic report rate is real and matters,
`0x0010` — not `0x000C` — is the one to test for `0x000E` specifically. Not tested this pass.

**Also newly discovered, undocumented anywhere in this repo or the reference tool's static
analysis:** two more `READ|NOTIFY` characteristics sharing the same vendor-UUID-family pattern as
`0x000A`/`0x000E` — `0x0026` (uuid `ab7de9be89fe49ad828f118f09df7fde`, same base as `0x000A`'s
`...fd2`, differing only in the trailing index byte) and `0x002E` (uuid
`7492866cec3e4619825832755ffcc0f9`, same base as `0x000E`'s `...cc0f8`) — each with their own
`0x2902` CCC and vendor report-rate descriptor, structured identically to `0x000A`/`0x000E`. Plus
two additional `NOTIFY`-only characteristics (`0x0022`, distinct UUID family from `0x001A`/`0x001E`)
and two additional `WRITE_NO_RESPONSE` characteristics (`0x0018`, `0x002A`) not matching any
handle this repo or the reference tool's `INPUT_HANDLES`/`COMMAND_HANDLES`/`COMMAND_RESPONSE_HANDLES`
constants. **None of these were subscribed to or exercised this pass** — flagged as a concrete,
high-value lead for future GATT exploration (§6), not investigated further here per the task's
explicit scope (exhaust the current capture set before building anything new).

Full corrected handle map (raw, BTstack/ATT-numbered — this repo's own `#define`s already use raw
numbers, no bleak adjustment needed anywhere in this repo's own code):

| Handle | Role | Source |
|---|---|---|
| `0x0009` | decl(`0x000A`) | GATT discovery ✅ |
| `0x000A` | value, input "Common" | GATT discovery ✅ + this repo's `#define` |
| `0x000B` | CCC(`0x000A`) | GATT discovery ✅ + this repo's `#define` |
| `0x000C` | vendor descriptor of `0x000A` ("report rate"?) | GATT discovery ✅ — corrects prior `0x000D` guess |
| `0x000D` | decl(`0x000E`) | GATT discovery ✅ |
| `0x000E` | value, input "Pro/GCN" | GATT discovery ✅ + reference tool + this repo's v1/v2 experiments |
| `0x000F` | CCC(`0x000E`) | GATT discovery ✅ + this repo's v1/v2 experiments (worked) |
| `0x0010` | vendor descriptor of `0x000E` ("report rate"?) | GATT discovery ✅ — **new, untested** |
| `0x0012` | value, rumble/output | GATT discovery ✅ + this repo's `#define` |
| `0x0014` | value, cmd | GATT discovery ✅ + this repo's `#define` |
| `0x0016` | value, cmd (secondary) | GATT discovery ✅ + reference tool |
| `0x0018` | value, WRITE_NO_RESPONSE | GATT discovery ✅ — **new, undocumented, unexplored** |
| `0x001A` | value, ack | GATT discovery ✅ + this repo's `#define` |
| `0x001E` | value, ack (secondary) | GATT discovery ✅ + reference tool |
| `0x0022` | value, NOTIFY-only | GATT discovery ✅ — **new, undocumented, unexplored** |
| `0x0026` | value, input? (READ\|NOTIFY, `0x000A`'s UUID family) | GATT discovery ✅ — **new, undocumented, unexplored** |
| `0x002A` | value, WRITE_NO_RESPONSE (`0x000A`'s UUID family) | GATT discovery ✅ — **new, undocumented, unexplored** |
| `0x002C` | value, WRITE_NO_RESPONSE (`0x0012`'s UUID family) | GATT discovery ✅ — **new, undocumented, unexplored** |
| `0x002E` | value, input? (READ\|NOTIFY, `0x000E`'s UUID family) | GATT discovery ✅ — **new, undocumented, unexplored** |
| `0x0032` | value, WRITE_NO_RESPONSE (`0x0016`'s UUID family) | GATT discovery ✅ — **new, undocumented, unexplored** |

### 3.4 Every variant produced a genuinely independent, motion-consistent 40-byte data block on `0x000E` — 🔵 Strong evidence

This is the headline result. Byte-level analysis of every variant's `0x000E` stream shows **46-47
of 63 offsets varying** (vs. only 8/63 for `0x000A`'s baseline pattern) — specifically, in
addition to the already-understood shifted stick-duplicate at offsets 5-10 (unchanged, still
tracks `0x000A`'s stick fields exactly — confirmed byte-identical again this session), **offsets
14 through 54 (a 41-byte span) are now active in every single variant, including variant 1 (the
plain control)**. This never happened in the earlier standalone v1 run (`EXPERIMENTAL.ndjson`,
prior session): a full scan confirms **0 of 2,331 records** in that file had any nonzero byte in
that range, over 70+ seconds, using the byte-identical command sequence.

Evidence this is genuinely new, independent, physically-plausible streaming data, not a counter/
timestamp/ACK/duplicate artifact — checked directly against the task's own stated criteria:

- **Absent from baseline**: ✅ confirmed (`BASE-NO-VARIANT` never subscribes `0x000E` at all).
- **Changes independently of buttons/sticks**: ✅ confirmed — `0x000A`'s own varying offsets in
  every one of these same files stay confined to its usual 7-8 offsets (counter + sticks); nothing
  unusual appears there. The new activity is confined to `0x000E`'s offsets 14-54 only.
- **Not explained by a counter**: the counter already identified at offset 0 (full entropy,
  wraps, +1/notification) has a completely different signature from offsets 14-54. Byte-boundary
  transition-rate analysis of one candidate 16-bit field (offsets 22-23) shows the low byte (22)
  changing on 99.8% of records while the high byte (23) changes on only 43.0% — exactly the
  signature of a genuinely incrementing/accumulating 16-bit value (the high byte only rolls over
  occasionally), not random noise and not a second independent counter.
- **Not explained by shifted duplication**: the already-known duplicate (stick data, offsets 5-10)
  is untouched and separately verified still present in the same files; the new activity occupies
  a disjoint byte range (14-54) that has no counterpart anywhere in `0x000A`'s own report.
- **Smooth, monotonic, non-random progression** (a signature no counter or noise source in this
  report family shows): decoding offsets 19-20, 22-23, and 25-26 as signed 16-bit little-endian
  and sampling 15 consecutive ~30ms-spaced records shows **steady, monotonic drift** with small,
  consistent per-step deltas (e.g. offset 25-26: `46, 56, 72, 90, 97, 104, 119, 138, 150, 160,
  173, 188, 201, 213, 230` — climbing smoothly, never reversing, never jumping erratically).
  Offset 16-17 stays near-constant (`384, 384, 384, 368, 384, ...`) over the same window — a
  qualitatively different behavior from the drifting fields, consistent with a field that isn't
  driven by the same accumulating process.
- **Quantified drift, all 6 variants**: every variant's candidate fields start near zero right at
  activation (`-157` to `-221`, or `2` to `6`, depending on the field — consistent with an
  accumulator that resets at the moment the feature is truly enabled) except variant 6, which
  starts already offset (`-2141`, `-479`, `35`) — consistent with its deferred-subscribe design:
  by the time its first `0x000E` notification is captured, `enable` (and the handle write) had
  already completed measurably earlier, so the accumulator had a head start before observation
  began. Drift rates vary in **both magnitude and sign** across the six independent connections
  (offset 19-20: `-305.8, +378.5, +357.6, +260.6, -520.1, +187.2` counts/s) — inconsistent with a
  fixed protocol artifact, consistent with each connection integrating whatever small physical
  perturbation (or sensor bias) was present during that particular ~60-90s window. Offset 25-26's
  rate, notably, stayed **positive and in a consistent 313-373 counts/s range across all six**
  independent connections — a strikingly repeatable bias-like signature.
- **Self-describing length prefix**: offset 14 is constant `0x28` (= **40** decimal) across every
  variant and every record — exactly the length of the block immediately following it (offsets
  15-54 inclusive = 40 bytes) — matching, byte for byte, the reference tool's documented
  **"Pro/GCN 40-byte motion block"** length for this exact handle, for the first time actually
  observed on real hardware rather than only asserted from static analysis of that tool's source.

**What this evidence supports, stated at the confidence level it earns**: `0x000E`'s report
contains a real, live, per-connection streaming data channel, structurally matching the
documented 40-byte motion block length, exhibiting behavior consistent with an integrating
(rate → accumulated value) physical channel. **What it does not yet establish**: which bytes are
which axis, what physical unit or scale applies, or that this is specifically gyroscope data as
opposed to some other integrating/accumulating quantity — per the task's explicit instruction, no
such semantic label is assigned here. That is squarely the next investigation's job (§6), once
this pass's causal-attribution work (§4) is complete.

## 4. Causal result table

| Variant | Operations verified | ACK result | Notification change | Independent payload data | Conclusion |
|---|---|---|---|---|---|
| 1 (control) | ✅ exact match to design | ✅ all success | `0x000E` stream begins | ✅ 40-byte block active | **Sufficient by itself** — the minimal skeleton (subscribe + configure(0x07) + enable(0x07), no extras) alone reproduces the result |
| 2 (mask_ff) | ✅ exact match | ✅ all success | `0x000E` stream begins | ✅ 40-byte block active | Works, but not proven *necessary* — variant 1 already works without it |
| 3 (handle_write_only) | ✅ exact match (write's own ACK unverifiable, §3.2) | ✅ all success (except unverified write) | `0x000E` stream begins | ✅ 40-byte block active | Works, but not proven *necessary* |
| 4 (mask_ff + handle_write) | ✅ exact match (write's own ACK unverifiable) | ✅ all success (except unverified write) | `0x000E` stream begins | ✅ 40-byte block active | Works, but not proven *necessary* |
| 5 (calibration_seq) | ✅ exact match, all 6 reads correct order/address/size | ✅ all success | `0x000E` stream begins | ✅ 40-byte block active | Works, but not proven *necessary* |
| 6 (full_sequence) | ✅ exact match incl. deferred CCC timing (write's own ACK unverifiable) | ✅ all success (except unverified write) | `0x000E` stream begins, active from the very first captured record | ✅ 40-byte block active, already mid-drift at first observation | Works; uniquely shows the accumulator was already running *before* the CCC subscribe, confirming the trigger is `enable`-time-scoped, not subscribe-time-scoped |

**Cannot be answered by this data alone: why did variant 1 (byte-identical commands to the prior
session's standalone experiment) succeed this time when it failed before?** Every one of the six
deliberately-varied dimensions this task asked to isolate (mask value, calibration reads, the
handle write, subscribe-timing) is **independently non-necessary**, since the plain control
variant — which has none of the other five variants' extra steps — already reproduces the full
result. The true differentiating factor between "prior session: never activates" and "this
session: activates in under 1s on all six independent connections" lies **outside** the six-variant
matrix entirely.

## 5. Assessment of the task's intended hypotheses

- **0xFF feature mask is sufficient**: ✅ true (variant 2 works) but not exclusively — 0x07
  already suffices (variant 1).
- **The raw `0x000C` write is sufficient**: ✅ true in the sense that variants with it (3, 4, 6)
  work, but not exclusively necessary — variant 1 lacks it and still works.
- **Their combination is sufficient**: ✅ true (variant 4 works) but likewise not exclusively
  necessary.
- **Calibration reads cause or unlock a state change**: ✅ true in the sense variant 5 works, but
  not exclusively necessary.
- **Subscription ordering matters**: 🟡 partially — it doesn't gate *whether* the block appears
  (variant 6 isn't uniquely successful), but it does measurably change *when relative to first
  observation* the accumulator has already been running, which is real, useful evidence about the
  underlying mechanism's timing (§3.4).
- **Only the full reference sequence works**: ❌ refuted — variant 1, the opposite of the full
  sequence, already works.
- **None of these differences enables motion under the tested conditions**: ❌ refuted at the
  surface level (independent data did appear in all six) — but true in the deeper sense that
  *none of the tested differences was what actually flipped the outcome*, since the untouched
  control alone reproduces it. The enabling factor is real but was not one of the six tested
  variables.

Per the task's branch for "if variant 6 differs in multiple inseparable ways, propose the
smallest follow-up A/B test; do not implement it yet" — variant 6 does not, in fact, uniquely
produce the result (all six do), so that branch doesn't strictly apply; the analogous situation
here is "all six variants succeeded because of something the matrix didn't vary" — see §6 for the
proposed follow-up.

## 6. Unresolved hypotheses (updated 2026-07-10, continuation pass)

**User-supplied ground truth, resolving the proposed follow-up from the first version of this
section without needing to run it**: variant 1 (and all six variants) *were* held genuinely
motionless throughout, and pairing itself happened while stationary. This directly answers what
§6 originally proposed testing — **the 40-byte block's drift is present and reproducible even
under confirmed-stationary conditions** — and rules out "the prior session's negative result and
this session's positive result differ because this session had incidental physical handling" as
an explanation on its own. Re-ranked accordingly, with one new, directly-measured candidate found
this pass (§9):

1. **🔵 Strong evidence, directly measured, not inferred**: the old (now-removed)
   `switch2_run_motion_experiment()` fired its CCC-subscribe write, `configure` command, and
   `enable` command **essentially simultaneously** — all three GATT writes issued within 1.2ms of
   each other, with **no wait for the CCC write's own ATT completion and no wait for `configure`'s
   application-layer ACK before `enable` was sent**. The device's ACK for `configure` didn't
   arrive until ~39ms *after* `enable` had already been transmitted. The new `switch2_v2_*` state
   machine (variant 1 included) explicitly waits for the CCC write's completion (41.6ms) before
   sending `configure`, and waits for `configure`'s ACK (150ms round trip) before sending `enable`.
   This is a real, verified difference in this repository's own code between the two sessions —
   see §9 for the exact timestamps. **Mechanistically plausible**: if the genuine controller's
   firmware needs the notification subscription to be truly active, or needs to finish processing
   `configure` before it can meaningfully act on `enable`, receiving `enable` before either has
   happened could cause it to silently ignore or mishandle the request — exactly the kind of
   difference that would flip a result from "nothing happens" to "it works," independent of any
   assumption about persistent controller-side state. **Not yet proven to be the cause** — a
   one-variable experiment could confirm it directly (proposed, not implemented, at the end of
   this section).
2. **🟡 Still plausible, unweakened by the stationary clarification**: a per-boot or per-bond
   state on the genuine controller — e.g. "this is the first time this exact command sequence has
   ever been sent to this specific bonded device" (the prior, all-negative session) vs. "it has
   succeeded at least once before" (every capture in this session). Would require a second
   physical controller, or a full forget/re-pair cycle on the existing one, to test — not done
   this pass. Not mutually exclusive with #1 — both could be simultaneously true (e.g. the
   controller might only "learn" to trust a properly-ACK-gated sequence, conflating the two).
3. **❌ Weakened**: incidental physical handling during capture — directly contradicted by the
   user's clarification that every capture, including variant 1, was taken with the controller
   stationary and paired-while-stationary.
4. **⬜ Not supported or refuted by current evidence**: incorrect command framing / feature-mask
   semantics, wrong subscription target, controller mode/transport/firmware differences beyond
   the timing difference already found, reference-tool behavior misunderstood from static code.

A structured comparison of every other difference between the two sessions the task asked for is
in §9 — most are inconclusive or don't apply; the ACK-gating timing difference (#1 above) is the
one with actual measured evidence behind it.

**Proposed, not implemented, one-variable trigger experiment** (per the task's explicit
instruction not to claim a trigger without one): a future "variant 7" that deliberately
reproduces the *old* unsequenced timing — send the CCC write, `configure`, and `enable` back to
back with no wait for any completion/ACK, using otherwise-identical bytes to variant 1 — run once,
stationary, fresh connection. If the 40-byte block fails to activate (or is measurably different),
that's a direct, single-variable confirmation of hypothesis 1. This is **not** today's experiment
(see §10/§11 for what today's controlled-motion capture actually is) and was not built this pass.

## 7. What this does not establish (explicit, per the task's constraints)

No gyro, accelerometer, quaternion, orientation, or calibration semantics are assigned to any byte
in this report. No claim is made about which axis (if any) any offset represents, what physical
unit or scale applies, or whether this data is fit for any particular use. `report 0x09`, normal
BLE initialization, and the v2 variants' own protocol behavior were not touched. No root cause is
claimed for why this session differs from the prior one — §6 states the leading hypothesis and its
confidence level explicitly, not as fact. **Update (continuation pass, §8-§12 below)**: two small,
additive code changes *were* made in the continuation pass — a capture-annotation marker
(`sw2cap mark`) and closing the write-completion-status logging gap §3.2 identified — both pure
logging additions verified not to change any GATT operation's timing or behavior (§11.1).

---

## 8. Block interpretation: ranked candidate layouts (continuation pass)

Tool: `tools/analyze_sw2_motion_block.py` (new, read-only, reusable) — per-byte statistics, an
exhaustive scan of every (start offset, width ∈ {1,2,3,4}, endianness, signedness) combination
fully inside the 40-byte payload, derivative/linearity/wraparound analysis, cross-field
correlation, a repeated-lane/interleave check, and a timing-relationship check, run across all six
variants' full active `0x000E` datasets (pooled where useful for statistical power). Full output
retained; this section reports the results that survived scrutiny.

### 8.1 Framing, confirmed

Raw report offset 14 is a constant byte, value `0x28` = **40 decimal**, in every record of every
variant — a self-describing length prefix for the 40-byte span immediately following it (raw
offsets 15-54, called `PAYLOAD[0..39]` below). Raw offsets 55-62 remain constant zero in every
variant (re-confirmed). This framing was assumed in the prior pass's report; this pass verified it
positively rather than merely re-asserting it.

### 8.2 Two fields survive as genuinely accumulator-like; nothing else does

The exhaustive scan tested every alignment/width/endianness/signedness combination and ranked them
by (linear-fit R² against sample index) × (fraction of steps moving in a consistent direction).
Across all six variants, independently, the same two regions dominate every ranked list — every
other high-scoring candidate the scanner found is explainable as a wider read that merely *includes*
one of these two 2-byte cores plus adjacent, slower-moving bytes (i.e. not independently meaningful):

- **Field A — payload offset 4-5 (raw `0x13`-`0x14`, decimal 19-20), int16 LE.** R²≈1.000 and
  ~100% directionally-consistent *within any unwrapped segment*, but — checked explicitly, not
  assumed — **this field wraps repeatedly within a single session**: 22-23 two's-complement
  wraparounds (jumps from near `-32768` to near `+32767` or vice versa) across ~2,700-2,950 active
  records, roughly one wrap every ~130 records ≈ **every ~4 seconds** at the ~33 Hz notification
  rate. This is why a naive linear fit over the *entire* uncapped session measures R²≈0.0001 (§8's
  companion `analyze_sw2_v2_captures.py` timing check) despite the field being almost perfectly
  linear between wraps — the two tools' different sample windows (a truncated 1,200-sample cap vs.
  the full session) is what first surfaced this, and it was confirmed directly by scanning for
  large single-step deltas. A field that wraps this cleanly and this regularly, at a roughly
  constant traversal rate, is a strong structural match for an **integrated angular/phase-style
  accumulator experiencing a small, roughly constant bias** — the same architecture already
  established for this project's own report 0x09 phase accumulator — though, per the task's
  constraint, this is stated as a structural resemblance, not a semantic claim.
- **Field B — payload offset 10-11 (raw `0x19`-`0x1A`, decimal 25-26), int16 LE.** R²≈0.9998-1.0000
  over the **entire, uncapped** active session in every one of the six variants — no wraparound
  observed at all. Slower than Field A (roughly an order of magnitude, based on the earlier pass's
  drift-rate table: ~300-500 counts/s for Field B vs. Field A's implied ~16,000 counts/s full-scale
  traversal rate). Also consistent with an accumulator, but evidently either a much smaller
  effective rate or a wider underlying counter that hasn't wrapped within this session's duration.
- **Possible wider (24-bit+) extension of each field**: the byte immediately above each 2-byte
  core (raw `0x15` for Field A, raw `0x1B` for Field B) shows a narrow-range, low-transition-rate
  pattern consistent with an occasional-carry high byte, rather than being independent noise —
  weak evidence each field may really be ≥24 bits wide with only the low 16 bits behaving cleanly
  linear on their own. Not confirmed; flagged for the next analysis pass once more data (or a
  longer single session) is available to observe the high byte incrementing more than a few times.

No 1-, 3-, or 4-byte-wide candidate at any other alignment, and no big-endian interpretation,
scored above the noise floor. No bit-level (sub-byte) field search was completed this pass beyond
noting which whole bytes have suspiciously small ranges (e.g. payload offsets 2, 15, 26, 37 sit at
range ≤1, essentially constant — candidates for flag/mode bits, not decoded further here).

### 8.3 A correlated, non-drifting pair

Raw offsets `0x26` (38 decimal, payload offset 23) and `0x31` (49 decimal, payload offset 34),
decoded as int16 LE, show a **consistent Pearson correlation of +0.44 to +0.55 across all six
independent variant sessions** — not itself drift-like (neither scores as a linear accumulator),
but clearly not independent of each other either, and far above the near-zero correlation every
other pair of candidate offsets shows (typically |r| < 0.2, mostly < 0.1). This is flagged as a
structurally linked pair worth revisiting once the controlled-motion capture (§10) provides ground
truth to correlate against — a consistent positive correlation between two non-drifting fields is
consistent with (among other explanations) two components of the same bounded physical quantity
that happen to move together under whatever incidental conditions were present, or a shared
dependency on a third field (e.g. both derived from the same underlying counter).

### 8.4 What controlled motion would distinguish, per field

Applying the task's own outcome mapping (§10.2) specifically to Fields A and B:

- If Field A/B's drift **rate (not just value) changes measurably during deliberate rotation**,
  and reverts toward its baseline rate when motion stops (even if that baseline rate is itself
  nonzero, consistent with §6's stationary-drift finding) → **rate-like** (e.g. gyro-style angular
  rate, integrated).
- If Field A/B **settle to a new, tilt-correlated resting rate or offset during a held nonzero
  tilt**, distinguishable from the baseline resting rate, and **return toward the original resting
  behavior** when returned to baseline orientation → **orientation/gravity-like**.
- If neither field's behavior changes in any detectable way across the entire pitch/yaw/roll
  protocol (§10.1) → strong evidence *against* either being motion-responsive at all — would argue
  for a non-motion accumulator (e.g. a free-running timer/tick unrelated to the IMU) despite the
  structural resemblance noted in §8.2.
- The correlated pair (§8.3) should be watched for whether its correlation *changes* under
  different motion axes (e.g. correlated during pitch but not during roll) — informative about
  whether it represents two components of a single rotating/projecting vector.

### 8.5 Repeated-lane / interleaving — not found

Tested 2, 4, 5, and 8-way lane splits of the 40-byte payload (comparing each lane's leading int16
value's correlation against lane 0's, across the full session). All correlations near zero
(|r| < 0.06) in every variant, at every lane count tested — **no evidence of multiple interleaved
samples per notification**. This somewhat weakens (does not eliminate) the a priori expectation,
based on report 0x09's own architecture, that a higher-rate physical sensor might be packed as
multiple samples per lower-rate BLE notification; if that's happening here, it isn't via a simple
fixed-stride lane split.

### 8.6 Timing relationship — inconclusive by design, not by finding

`corr(inter-sample dt, per-step delta)` is near zero for both Field A and Field B in every variant
(|r| < 0.03) — consistent with *either* a fixed per-notification increment (a rate integrated once
per BLE tick, not scaled by actual elapsed wall-clock time) *or* a genuinely time-scaled
integration running against a BLE cadence stable enough (~30ms, low jitter) that the two models are
statistically hard to separate from passive data alone. Not resolved this pass; the controlled
motion protocol doesn't specifically target this either, since it requires either deliberately
irregular polling (not supported by this repo's fixed notification path) or a much longer passive
capture with more natural cadence variance than this dataset has. Left as an explicit open item,
not investigated further.

---

## 9. Differential comparison: old standalone v1 vs. new v2 variant 1

Per the task's explicit list, every candidate difference between the prior session
(`EXPERIMENTAL.ndjson`, negative result) and this session's variant 1 (positive result) *outside*
the six variant actions themselves (which are identical between the two — variant 1 reproduces v1
exactly by design):

| Candidate | Finding |
|---|---|
| **Firmware build** | ✅ **Different, necessarily.** The entire v1 experiment code path (`switch2_run_motion_experiment()`) was deleted between the two sessions and replaced by the v2 state machine (`switch2_run_v2_experiment()` + `SW2_V2_VARIANTS[]`, this pass's own prior turn). The binary was rebuilt and reflashed. This alone doesn't explain *why* behavior changed, but it means "byte-identical commands" does not mean "byte-identical firmware" — see the next row for the concrete behavioral consequence found. |
| **Subscription (CCC) timing — order** | Same for variant 1 specifically: CCC-subscribe first in both sessions (variant 6 is the only one that defers it, not relevant to variant 1). |
| **Subscription/command timing — *gating*, not just order** | ✅ **Different, directly measured — see below.** This is the strongest concrete finding of this comparison. |
| **Capture start time** | Minor, likely not causal: the prior session's capture spanned a stalled-then-successful connection attempt (~13s gap between two `ccc_write`s to `0x001B`); this session's variant-1 file shows one clean connection with no stall. |
| **Reconnect vs. true power cycle** | Not distinguishable from available data — both sessions' connections went through the *identical* full pairing state sequence (`READ_INFO`→`PAIR_STEP1`→`PAIR_STEP2`→`PAIR_STEP3`→`PAIR_STEP4`→`SET_LED`→`DONE`), i.e. this repo's host code appears to always re-run full pairing on every connection regardless of any prior bond, in both sessions alike. |
| **Bond/session persistence** | The same physical controller was used continuously across both sessions; nothing in this project's Switch 2 code path ever calls a "forget device" equivalent for it. Whatever bond state exists persisted across the gap in both directions — doesn't distinguish the sessions, but doesn't rule out an independent, non-BLE-standard, controller-firmware-side "learned" flag either (§6 hypothesis 2). |
| **Config-mode entry order** | Not independently verifiable from capture data; both sessions necessarily had the Pico already in config mode (CDC) before any `sw2cap` command could be issued at all. |
| **Physical handling** | ❌ Ruled out this pass — user confirmed all captures, including variant 1, were taken with the controller stationary and paired while stationary. |
| **Common v2 initialization/instrumentation behavior** | ✅ **This is where the concrete difference lives** — detailed below. |

### 9.1 The measured timing difference

Exact timestamps, same three operations (CCC-subscribe write to `0x000F`, `configure` command,
`enable` command), directly compared:

**Old (`EXPERIMENTAL.ndjson`, v1, negative result):**
```
us=285854996  ccc_write  0x000F              0100
us=285855721  cmd_out    0x0014 (configure)  0c9101020004000007000000    <- +725us after CCC write
us=285856191  cmd_out    0x0014 (enable)     0c9101040004000007000000    <- +470us after configure
us=285895716  ack        0x001A (=configure) 0c0101041078...             <- device's configure-ACK
                                                                              arrives 39ms AFTER
                                                                              enable was already sent
```

**New (`sw2_capture_2026-07-10_VARIANT-1.ndjson`, positive result):**
```
us=196008850  ccc_write  0x000F              0100
us=196050468  cmd_out    0x0014 (configure)  0c9101020004000007000000    <- +41.6ms (waited for the
                                                                              CCC write's own ATT
                                                                              completion first)
us=196200449  ack        0x001A (=configure) 0c0101021078...             <- configure ACK arrives
                                                                              +150ms after configure
                                                                              was sent
us=196200543  cmd_out    0x0014 (enable)     0c9101040004000007000000    <- +94us after that ACK
                                                                              (i.e. sent immediately
                                                                              upon confirmation, not
                                                                              before it)
us=196260447  ack        0x001A (=enable)    0c0101041078...
```

**The old code fired all three GATT writes within 1.2ms of each other, unconditionally, never
waiting for the CCC write's ATT completion or for `configure`'s application-layer ACK — `enable`
was in flight roughly 39ms *before the device had even acknowledged `configure`*. The new v2 state
machine explicitly waits for each step's real confirmation before sending the next.** This is a
verified fact about this repository's own two code versions, not an inference about the
controller. It is mechanistically plausible as the actual cause (§6 hypothesis 1): if the genuine
controller's firmware needs its notification subscription to be truly active, or needs to finish
processing `configure` before `enable` is meaningful, receiving `enable` this early could cause it
to silently ignore or mishandle the sequence. **Not proven** — see §6 for the proposed (not
implemented) one-variable confirmation experiment.

---

## 10. Controlled hardware experiment design

One continuous BLE connection, variant 1 (already proven sufficient — no new experiment code),
using the new marker mechanism (§11.1) to mark phase boundaries in the exported NDJSON. Per the
task's exact required physical sequence.

### 10.1 Physical sequence

1. Stationary baseline, ≥30 seconds.
2. Several isolated positive pitch rotations, then stop; several isolated negative pitch
   rotations, then stop.
3. Hold a fixed nonzero pitch, ≥20 seconds.
4. Return to the original baseline orientation and hold.
5. Repeat 2-4 for yaw.
6. Repeat 2-4 for roll.
7. Finish with another stationary baseline.

Precise laboratory angles are not required for this first classification pass — approximate,
clearly isolated, single-axis motions are sufficient. Record (in the session notes, not
necessarily in the NDJSON) the controller's physical orientation convention used (e.g. "pitch =
tilting the face of the controller toward/away from the user, positive = face down") and the
approximate rotation direction called "positive," so a future pass can map sign conventions
correctly — this pass does not require or attempt that mapping.

### 10.2 Outcome mapping (defined before the test, per the task's instruction)

| Pattern observed in a field | Classification |
|---|---|
| Changes only during rotation, returns near its baseline *rate* when motion stops (baseline rate may itself be nonzero, per §6's stationary-drift finding) | Rate-like |
| Settles to a new resting value/rate during a fixed tilt, correlated with the held orientation | Orientation- or gravity-like |
| Brief transient at the start/stop of a movement, otherwise flat | Acceleration-like |
| Changes the same way regardless of what motion is happening | Timing/counter/state (already the working model for the already-characterized bytes outside Fields A/B) |
| Different subgroups of bytes show different behaviors from the list above | Composite/interleaved motion data |

Applied specifically to Fields A and B (§8.2) and the correlated pair (§8.3) once the capture is
in hand — not applied to any byte prematurely, per the task's explicit instruction not to select
an interpretation because it looks plausible.

---

## 11. Implementation: capture-annotation marker + the write-status logging fix

Two small, additive changes this pass, both verified not to alter any BLE operation's timing or
behavior — both boards build clean.

### 11.1 Capture-annotation marker (new)

`sw2_capture_mark(const uint8_t *label, uint16_t len)` (`src/bt_hid/sw2_capture.c`/`.h`) inserts a
single new `SW2_CAP_MARKER` capture entry carrying the label text, timestamped exactly like every
other entry. It is a **pure logging call** — `sw2_capture_record()` was already cross-core-safe
and side-effect-free with respect to the BLE connection; calling it with a text label from
`config.c` (core0, in response to the new `sw2cap mark <text>` command) touches nothing on core1,
the GATT client, or any part of the init/report/variant state machines. **This satisfies the
task's explicit constraint that a marker must not alter controller initialization or report
behavior** — it doesn't call into any BTstack API at all.

Web UI: a text input + "Add Marker" button in the capture panel (Enter key also submits). Sends
`sw2cap mark <label>`, logs confirmation, clears the input for the next label. Rendered in the
live capture view as the literal label text (decoded from the entry's hex bytes), not raw hex, so
markers are readable at a glance during a session.

### 11.2 Write-completion status logging (gap fix)

§3.2 found that `switch2_v2_ccc_write_callback()` and `switch2_v2_handle_write_callback()`
(`btstack_host.c`) computed their ATT completion status but only `printf()`'d it, never captured
it. Fixed by adding one `sw2_capture_record(SW2_CAP_WRITE_STATUS, handle, &status, 1)` call in
each, **placed immediately after the status is read and before any of the callback's existing
control-flow logic** — the status was already known at that point from the same
`gatt_event_query_complete_get_att_status()` call the `printf()` used; logging it doesn't delay,
reorder, or change what the callback does next. Verified by inspection (the added line has no
side effects beyond appending to the capture ring) rather than by a new hardware run — this pass's
constraint was "only if it can be done without changing timing or behavior," which a same-line
logging addition structurally satisfies. Future v2 captures will show a `write_status` entry after
each CCC/descriptor write, closing the "was `0x000C`'s write actually accepted?" gap.

---

## 12. Exact browser procedure for the controlled-motion capture

Requires a genuine Pro Controller 2, the config web UI (regenerated into `src/web_disk.h` this
pass — reflash before running), and about 5 minutes for the full sequence.

1. Open the config page, connect to the Pico, open the **Switch 2 BLE Capture** panel.
2. Select **`1 — control`** in the v2 experiment variant dropdown (already proven sufficient —
   §3.4/§6).
3. Click **Start Capture**.
4. Power-cycle the genuine controller to force a fresh connection (no config-mode pair/disconnect
   command exists or is needed — see the prior pass's §9 for why). Wait for it to settle (state
   `8`/`DONE` in the live view, then the variant firing — normally well under a second once
   connected).
5. As soon as the controller is stationary and connected, type `baseline1_start` into the marker
   field and click **Add Marker** (or press Enter). Hold the controller motionless for **at least
   30 seconds**, then mark `baseline1_end`.
6. **Pitch**: mark `pitch_pos_start`, perform one clear positive pitch rotation, mark
   `pitch_pos_end`the moment it stops. Repeat this start/rotate/end pair **2-3 times**. Then the
   same for negative pitch: `pitch_neg_start` / rotate / `pitch_neg_end`, 2-3 times.
7. Mark `pitch_hold_start`, tilt to a fixed nonzero pitch and hold it motionless for **at least 20
   seconds**, mark `pitch_hold_end`.
8. Mark `pitch_return_start`, return to the original baseline orientation, mark `pitch_return_end`,
   and hold there briefly.
9. Repeat steps 6-8 for **yaw**, using labels `yaw_pos_start`/`yaw_pos_end`,
   `yaw_neg_start`/`yaw_neg_end`, `yaw_hold_start`/`yaw_hold_end`, `yaw_return_start`/`yaw_return_end`.
10. Repeat steps 6-8 for **roll**, using labels `roll_pos_start`/`roll_pos_end`,
    `roll_neg_start`/`roll_neg_end`, `roll_hold_start`/`roll_hold_end`, `roll_return_start`/`roll_return_end`.
11. Mark `baseline2_start`, hold motionless for **at least 30 seconds**, mark `baseline2_end`.
12. Click **Stop Capture**, then **Download NDJSON**. Confirm the panel's `dropped` count reads
    `0` before trusting the session.
13. Before uploading/handing off the file: note in a message (not necessarily in the file itself)
    which physical direction was used as "positive" for each axis, and the controller's resting
    orientation convention (e.g. flat on a table, face up) — needed to map sign conventions later,
    not needed to run the analysis itself.

**Expected file name**: `sw2_capture_2026-07-10_MOTION-CLASSIFICATION.ndjson` (matching this
session's existing naming convention — `sw2_capture_<date>_<LABEL>.ndjson`). Place it in `dumps/`
alongside the existing v2 captures. This is the one file the next analysis pass needs — no other
capture is required to proceed with axis classification (§10.2's outcome mapping) for Fields A and
B.

---

## 13. Controlled-motion capture: results

Received `dumps/sw2_capture_2026-07-10_MOTION-CLASSIFICATION.ndjson`. User-supplied physical
convention: **positive = left, negative = right, for all three axes; resting orientation = flat
on a table, face up.**

### 13.1 Integrity

24,158 records, 0 parse errors. 24,083 `input` (all `0x000E`, all active — the block was live from
the first record, consistent with this being a continuation of an already-armed variant-1
connection), 44 `marker` entries — all 22 phase pairs from §12's procedure present and correctly
paired (`_start`/`_end` matched cleanly for every phase). One gap from the procedure: no
`yaw_hold_start/end` or `yaw_return_start/end`, and no `roll_return_start/end`, were recorded
(pitch alone has the complete 4-phase set: pos, neg, hold, return) — still ample data for the
analysis below. Session duration ~732s (~12.2 minutes).

### 13.2 Direction-correlation test on Fields A/B: negative result

Per §10.2's outcome mapping, tested first: does Field A's or Field B's *slope* (rate of change)
during `_pos`-labeled phases consistently differ from during `_neg`-labeled phases, per axis?

Computed per-phase linear-fit slope (Field A unwrapped across its ~4s wrap period first) for every
one of the 22 marked phases. Result: **no reproducible pattern**. Field A's slope during repeated
`pitch_pos` reps was `+23985.7`, `+37539.3`, `+819.5` counts/s — wildly different in magnitude for
supposedly-identical repeated motions, and its slope during `baseline1` (`-17482.9`, over 111.5s of
confirmed stillness) is comparable in magnitude to slopes during active rotation. Field B shows the
same pattern (`pitch_pos` reps: `+983.9`, `-2008.3`, `-2168.5` — inconsistent sign even within one
labeled direction). A systematic scan (Cohen's-d effect size between all `_pos` and all `_neg`
phase slopes, computed for every one of the 39 possible 2-byte-LE start offsets) found the largest
effect at raw offset `0x25` (d≈1.52) — but with massive within-group scatter (`_neg` slopes at that
offset ranged from `+189` to `-990`) that is plausibly explained by chance alone given 39 offsets
tested and only 8-9 samples per group. **Conclusion: neither Field A, Field B, nor any other
2-byte-LE field shows a slope or level that reproducibly tracks the labeled rotation direction.**
This refutes a simple "clean integrated rate, readable at this offset/width/endianness" model for
Fields A/B specifically — it does not refute the block being motion-related in a different way
(§13.3).

### 13.3 The actual finding: a block-wide, cross-validated activity/noise-floor signature

Reframed the question from "does a field's *value* track rotation direction" to "does the block's
*behavior* (not direction, just presence of variation) distinguish genuine stillness from
handling at all" — computed, for every 2-byte-LE offset, the residual noise remaining after
subtracting a local linear trend, separately for confirmed-stationary phases (`baseline1`,
`baseline2`) vs. any phase involving handling (`_pos`/`_neg`/`_hold`).

**Result: dramatic and widespread.** The ratio of active-phase to baseline-phase residual noise
exceeds 100× at eleven distinct offsets spanning nearly the entire block width (`0x14`, `0x1b`,
`0x20`, `0x22`, `0x24`, `0x26`, `0x2b`, `0x2d`, `0x2f`, `0x31`, `0x33`) — not one lucky byte, a
systemic pattern. The single cleanest example, offset `0x1e`, cross-validated with two independent
metrics:

- **Residual-noise-from-trend**: `baseline1`=1413/601, `baseline2` not separately shown but
  consistent, vs. every active-rotation phase in the 16,690-21,175 range — a ~20-30× jump.
- **Raw byte-level transition rate** (simplest possible metric — does the byte change at all
  between consecutive notifications): **0.3%** of records at baseline (i.e. this byte is
  essentially frozen while the controller is still) vs. **70.0%** during active rotation and
  **36.6%** during the nominally-stationary `_hold` phases.

The `_hold` phases are the most informative detail: they were meant to be motionless (§10.1's
"hold a fixed nonzero pitch"), yet show *intermediate* noise levels between baseline and active
rotation — and inconsistently across offsets (offset `0x14`: `pitch_hold` residual 18,382, almost
as high as active rotation, but `roll_hold` residual only 4,075; offset `0x1b`: both holds
comparatively low, 597 and 1,565). This inconsistency, rather than undermining the finding, *is*
the finding: it is not distinguishing a fixed "moving" vs. "still" state flag (which would be
uniform across all nominally-still `_hold` phases) — it tracks something closer to how physically
strained/hard-to-hold-perfectly-still each specific tilt was, which is exactly what involuntary
hand tremor while holding an awkward grip for 20-40 seconds would produce, and exactly what genuine
stillness (controller flat on a table, no hand contact required to stay in position) would not.

`pitch_return` phases (motion just stopped, controller settling back toward baseline) show
residual noise at an intermediate level between active and baseline (e.g. offset `0x14`: 6,869,
vs. baseline's 10-37 and active rotation's 13,600-18,400) — consistent with a brief settling
transient after motion stops, not an instantaneous return to silence.

### 13.4 Classification against the outcome mapping (§10.2)

None of the five predefined categories fits cleanly:

- Not **rate-like** (§13.2 — no direction-dependent slope).
- Not **orientation-/gravity-like** (`_hold` phases don't settle to a stable, tilt-correlated
  level — they stay noisy, just less consistently so than active rotation).
- Not simple **acceleration-like** in the "brief transient at start/stop only" sense — the
  elevated noise persists for the *entire* duration of a rotation or hold phase, not just at its
  boundaries (though `pitch_return`'s intermediate, decaying-looking value is consistent with a
  transient *settling* component layered on top).
- Not **timing/counter/state** — behavior is not uniform regardless of motion; it changes
  dramatically and specifically with whether the controller is being handled.
- Closest fit is **composite**, but the specific pattern found — a near-silent noise floor at
  genuine rest that jumps 2-3 orders of magnitude the instant the controller is handled at all,
  reproducible across ~11 distinct byte offsets spanning the block, without a clean per-axis
  directional readout — is not fully captured by any of the five categories as originally framed.
  Recorded here as a sixth, empirically-derived category: **activity/vibration-responsive noise
  floor** — consistent with the block carrying raw, high-bandwidth (unsmoothed/unintegrated)
  sensor samples sensitive enough to alias real mechanical vibration (including involuntary hand
  tremor) into large sample-to-sample swings, as opposed to a filtered or slowly-integrated
  channel. **This is a structural characterization, not a semantic one** — it does not identify
  which bytes are accelerometer vs. gyroscope, which axis, or what scale, and none of that is
  claimed here.

### 13.5 What this changes and what it doesn't

**Strengthens**: the case that the 40-byte block is genuinely IMU-derived data, not a coincidental
artifact — a coincidental/non-sensor field would have no reason to correlate this cleanly and
broadly with "is a human hand currently applying force/rotation to this object," across eleven
independent byte positions, cross-validated by two different statistical measures.

**Does not establish**: which offset is which physical axis or sensor; whether Fields A/B (§8,
the two fields that looked cleanly accumulator-like in the earlier *passive* dataset) play any
specific role in this activity signature, given they did not individually show the sharpest
baseline/active contrast in this dataset; any scale, unit, or calibration; or whether a real,
slower directional signal is present underneath and simply swamped by amplitude at the rotation
speeds/durations used in this test (a plausible next question, not answered here).

**Constraint compliance**: no gyro/accelerometer/quaternion/orientation semantics are assigned to
any byte. `report 0x09`, normal BLE init, and the v2 variants' protocol behavior remain untouched.

### 13.6 Recommended next step (proposed, not implemented)

Given the noise-floor signature swamps any attempt to read a clean per-axis DC level or slope at
the rotation speeds used here, the next highest-information test is **not** a repeat of this same
protocol — it's one that separates "is this amplitude-sensitive to motion intensity" (testable
immediately with existing tooling) from "is there a directional/orientation component underneath
the noise" (would need averaging/filtering approaches this pass didn't attempt). Concretely:
capture the controller completely undisturbed on a table (a clean re-baseline, several minutes) vs.
a deliberately *held-in-the-hand-but-not-moved* baseline (hand contact, no deliberate rotation) —
if the noise floor is already elevated by mere hand contact (without any deliberate motion at all),
that would further confirm the "responds to any physical handling, not specifically rotation"
reading and would be a cheap, single-variable way to test the tremor hypothesis directly (§13.3)
without needing a new capture-analysis tool.

**Superseded by §14.6**: explicit direction was given not to request this specific test yet, and
to investigate the block's internal packet structure more deeply first — §14 does that, and its
own proposed experiment (§14.6) supersedes this one with a mechanically-supported (non-handheld)
design that cleanly separates "responds to vibration" from "responds to hand contact," which this
section's proposal could not.

---

## 14. Structural decomposition: does the block match a documented IMU FIFO packet format?

**Question**: the direction-correlation and activity-noise-floor tests (§13) only tested clean,
byte-aligned scalar interpretations. They don't establish that the block *lacks* directional
gyro/accelerometer data — a native IMU FIFO packet structure (packed sub-fields, multiple samples
per notification, shared high-bits, delta-coding) could produce exactly the observed "block-wide
activity, no clean per-axis slope" signature while defeating a direct scalar slope test. This
section tests specific candidate packet structures against the complete dataset before requesting
more hardware.

### 14.1 What's known about the controller's IMU, and at what confidence

This repo's own documentation (`docs/switch2/report-0x09-motion.md`, sourced from `ndeadly`'s
public teardown research) identifies the genuine controller's IMU as an **ICM-42670-P**
(TDK InvenSense), a 6-axis (3-axis accel + 3-axis gyro, no magnetometer) MEMS IMU. That chip
identification itself is 🔵 strong evidence (an independent third-party hardware teardown), not
something re-verified this pass.

**This pass's FIFO packet layout knowledge is 🟡 general/trained knowledge, not a directly-
consulted datasheet** — this session has no internet or document-fetch access, so the specific
byte-level layouts tested below are reconstructed from general familiarity with the well-
documented, largely-shared FIFO packet architecture across TDK's ICM-42xxx/ICM-4x6xx IMU family
(extensively described in public datasheets, TDK's InvenSense eMD driver source, and open-source
drivers such as Zephyr's `icm42670p`), **not from the ICM-42670-P datasheet itself, open in this
session**. This is stated explicitly so any negative result below is read correctly: it means
"the specific layout tested didn't match," not "no documented layout could possibly match." The
general, high-confidence structural elements recalled: a 1-byte packet header (status/type bits),
6 bytes of accelerometer X/Y/Z (int16 each), 6 bytes of gyroscope X/Y/Z (int16 each), 1 byte of
temperature, and — in "16-byte" packets — 2 bytes of timestamp; a documented "high-resolution"
packet variant extends this with additional bytes carrying extra low-order precision bits for
each axis, widely described as reaching a 20-byte total packet size. The *exact* byte offset of
each of these fields, and precisely how the high-resolution extension bytes are packed, is the
part held at lower confidence here.

### 14.2 Framing re-confirmed, one ambiguity resolved

New tool `tools/analyze_sw2_block_structure.py` (`check_framing()`) re-confirms raw offset 14 is
the length-prefix byte and re-examines this session's occasional `0` value there (7 of 24,069
records) — **resolved as the already-known pre-activation transient** (the first 7 records of the
session, before the block "switches on," matching prior sessions' behavior), not a new mid-session
validity flag. Not a new finding; recorded here because it briefly looked like one before checking.

### 14.3 Periodicity test: no repeating packet-header signature found

`periodicity_test()` computes the block's full 40-position Shannon-entropy profile, then checks
whether that profile correlates with itself shifted by each candidate packet period — the
signature a genuine repeating packet *with a stable per-packet header/field structure* would leave
(a header byte would recur as a low-entropy position at multiples of the true packet size).

**Result: no period tested shows meaningful positive correlation.** `P=2`: `+0.174`; `P=4`:
`-0.178`; `P=5`: `-0.190`; `P=8`: `-0.321`; `P=10`: `-0.219`; `P=16`: `+0.107`; `P=20`: `+0.189` —
all near zero or negative, none suggesting a genuine repeating structure at 2×20, 4×10, 5×8, or
8×5 byte packets. `subbyte_header_scan()` additionally checked whether header bits might share a
byte with otherwise-busy data (a real pattern in some FIFO conventions) — found a handful of
low-entropy top-bit positions (raw `0x10`, `0x1c`, `0x1e`, `0x29`, `0x36`), but **none recur at a
consistent spacing from each other**, so this doesn't rescue any of the tested periodic layouts
either. ❌ **No candidate periodic FIFO-packet layout at the tested spacings is compatible with
this block's entropy structure.**

### 14.4 Magnitude-stability scan: physically-grounded, inconclusive

Independent of any assumed byte map: a genuine 3-axis accelerometer's vector magnitude
(`√(x²+y²+z²)`) should stay roughly orientation-invariant — only the per-axis *distribution*
should change with tilt, not the magnitude. `magnitude_stability_scan()` tests every
3-consecutive-int16 window (both endiannesses) against confirmed-stationary (`baseline1`/
`baseline2`) vs. fixed-tilt (`pitch_hold`/`roll_hold`) phases.

Best candidate: raw offset `0x10` (payload offset 1), big-endian — `baseline` mean magnitude
36,051 (CV 0.13), `pitch_hold`/`roll_hold` ratios `1.01`/`1.01`, CVs `0.12`/`0.12`. On the surface
this looks like a strong match. **Explicitly checked for and ruled a likely artifact**: this
window's own bytes don't overlap the confirmed counter at raw `0x0f`, but its Z-component (raw
`0x14`) sits adjacent to Field A (raw `0x13`-`0x14`, §8.2's wrapping accumulator) — a slowly-
drifting or wrapping quantity sampled over multi-second windows at *different points in an ~800
second session* will tend to show *similar typical magnitudes* across those windows for purely
statistical reasons (its long-run average doesn't depend on physical orientation), which can
produce exactly this kind of spurious "orientation-invariant" appearance. No candidate scored
well *and* was clearly free of overlap with an already-confirmed non-physical accumulator/counter
byte. 🟡 **Inconclusive** — this test could not confirm or confidently rule out any specific
accelerometer-triplet candidate given the confounds already established elsewhere in the block.

### 14.5 Confidence-qualified summary

| Claim | Confidence |
|---|---|
| Block is bounded raw offset 15-54, preceded by a length-prefix byte, followed by a constant-zero tail | ✅ Confirmed |
| Block responds to physical handling with a widespread (~11 offset), 100-1000× noise-floor increase | 🔵 Strong evidence (§13, cross-validated by two metrics) |
| Block matches a repeating native ICM-42670-P FIFO packet layout at 2×20, 4×10, 5×8, or 8×5 spacing, headers included | ❌ Not compatible, at the confidence level of this pass's reconstructed layout (§14.3) — not a claim that no documented layout could ever fit, given the datasheet itself wasn't directly consulted |
| Any specific 3-int16 window is "the" accelerometer triplet | 🟡 Hypothesis, unconfirmed — best candidate confounded by a known non-physical accumulator (§14.4) |
| Fields A/B (§8.2) are genuine gyro-phase/rate data specifically | ⬜ Unresolved ambiguity — behaviorally accumulator-like, but showed no reproducible rotation-direction correlation (§13.2) |
| The block contains *some* form of genuine, unfiltered IMU-derived data | 🔵 Strong evidence (the §13 activity signature is difficult to explain otherwise) though the exact internal packing remains unresolved |

No byte/bit-level packet map is being produced this pass, and no decoder was written for a
specific layout — per the task's own branching instruction, since no candidate layout survived
structural testing.

### 14.6 Proposed next experiment: mechanically-supported, not handheld

Per explicit instruction to prefer mechanically-supported tests over another handheld one, and to
define expected results per model *before* asking for a capture. Two competing models, both
consistent with everything confirmed so far:

- **Model A (IMU-driven)**: the block's activity signature is genuinely driven by inertial
  motion/vibration sensed by the accelerometer and/or gyroscope, packed in a form this pass's
  tests haven't correctly decoded yet (possibly firmware-repacked rather than a raw native FIFO
  dump, delta-coded, or using an untested bit-packing convention).
- **Model B (non-inertial handling artifact)**: the block's activity correlates with a hand being
  on or near the controller for a reason *other* than inertial sensing — e.g. capacitive/thermal
  contact sensing, or RF/power-supply coupling from proximity to the antenna/circuitry — and would
  not necessarily respond to genuine mechanical vibration/motion applied without skin contact.

**Experiment** (no hand contact with the controller itself during the "test" steps; use a rigid
tool — e.g. a pen or similarly inert object — for any tapping):

1. **True rest**: controller flat on the table, completely undisturbed, no one touching the table
   either — capture ~30s. (Re-establishes a hand-tremor-free, contact-free baseline; the existing
   `baseline1`/`baseline2` phases already approximate this but did follow a marker click, which
   involves brief hand proximity — this step is a cleaner control.)
2. **Mechanical tap, no contact**: with the controller still untouched on the table, tap the
   *table* firmly near the controller several times with a rigid object (not a hand on the
   controller) — capture the response.
3. **Direct mechanical vibration, minimal contact**: rest the controller against or on top of a
   controlled vibration source (e.g. a phone set to vibrate, placed under or beside it) for ~20s,
   touching the controller only briefly to position it, not while vibration is active — capture.
4. **Rigid-tool translation**: using the same rigid tool, slide the controller a few centimeters
   across the table without rotating it and without hand contact — capture.

**Expected results, defined in advance:**

| Step | Model A (IMU-driven) predicts | Model B (non-inertial artifact) predicts |
|---|---|---|
| 1. True rest | Noise floor stays at the already-established quiet baseline level | Same — both models agree here |
| 2. Table tap, no hand contact | Noise floor rises (real vibration reaches the IMU through the table/case) | Noise floor stays quiet (no skin contact with the controller) |
| 3. Vibration source, minimal contact | Noise floor rises, likely more than step 2 | Noise floor stays mostly quiet, rising only during the brief positioning contact |
| 4. Rigid-tool translation, no rotation | Noise floor may rise somewhat (real linear acceleration) but likely less dramatically than the handheld rotations in §13 | Noise floor stays quiet (no contact at all) |

A result matching Model A's column across steps 2-4 would be strong, mechanically-clean
confirmation that the block is genuinely inertial-sensor-driven, independent of the hand-tremor
confound that made §13's `_hold` phases ambiguous — and would justify returning to structural
decomposition with renewed confidence that *something* real is being decoded, just not the layout
tested so far. A result matching Model B (quiet under tapping/vibration without contact, active
only when touched) would be a significant, surprising finding worth escalating carefully rather
than assumed away.

**Not requested from the user in this pass** — per the task's instruction to first exhaust
structural analysis, this is presented as the defined next step, not an immediate ask.

---

## 15. Trigger investigation: explicitly deferred, not touched this pass

Per explicit instruction not to mix trigger investigation into this pass's block-decoding work:
§6/§9's finding (the old v1 code fired CCC-subscribe/configure/enable within 1.2ms with no
ACK-gating; the current v2 code waits for real confirmation at each step) remains the leading,
unconfirmed candidate explanation for why this session's variant 1 succeeds where the earlier
standalone v1 attempt found nothing. The proposed one-variable confirmation experiment (a future
"variant 7" reproducing the old unsequenced timing) remains proposed, not implemented, and is not
advanced further this pass. No new evidence on the trigger question was sought or found this turn.
