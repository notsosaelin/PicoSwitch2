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
PC/Steam enumerates as a Switch 2 Pro with full buttons/sticks (gyro added in v1.1). Builds on
Pico W + Pico 2 W. bluepad32 removed.

### 🔴 v1.1 — motion (gyro) — PAUSED 2026-07-10, not abandoned

Deferred pending new primary evidence or broader controller RE (e.g. NFC, below) surfacing
something that changes the picture. Preserved in full for context; no longer the active target.
- **PC / Steam gyro (report 0x05) — 🟢 working, confirmed on hardware (test 2).**
  Experiment A fixed the frozen timestamp + 60× gyro under-scale — validated live, Steam detects
  gyro. Hardware test 1 then surfaced a second bug: **pitch/roll swapped, yaw correct**. Re-mining
  the genuine controller's own capture identified the true axis order (X=pitch, Z=yaw, Y=roll) and
  fixed `ns2_seam.c`'s transform accordingly. **Hardware test 2 confirmed it**: gyro is smoother,
  axes correct, and Steam calibration — inert before the fix — now produces usable motion. Remaining
  gap: roll's *sign* was inferred from a determinant constraint, not independently measured (🔵). See
  `docs/experiments/gyro-hardware-validation-2026-07-10.md`.
- **Console gyro (report 0x09) — 🔴 encoder work paused; re-centered on a genuine console-side USB
  observation path (second pivot, 2026-07-10) after BLE-block decoding itself was also paused.**
  Motion block is int32 angular-phase × 3 + Q16.16 accel × 3, gated on the `0x0C`/`0x27`
  enable-negotiation (`docs/switch2/report-0x09-motion.md`) — the byte *layout* is well-validated
  and not in question; the *value semantics* (accumulator vs. bounded sample) are the open
  question. After two hardware passes, a debug-instrumentation fix, a confirmed-working stillness
  gate, and a mathematically-derived local-anomaly detector that reported **`anom=0` on hardware
  while the console still jumped** (ruling out a local computation defect as *the* explanation,
  without validating the value-semantics model) — **no further encoder changes planned without
  new evidence.** Full history: `docs/experiments/gyro-hardware-validation-2026-07-10.md` §9-14.
- **Genuine-controller BLE reverse-engineering — 🟡 active, 2026-07-10.** New objective,
  replacing "fix the encoder": closes a concrete blind spot (`switch2_ble.c` receives but never
  reads report bytes 16-59, where third-party decodes place motion) with a non-invasive,
  timestamped raw-capture facility (`src/bt_hid/sw2_capture.c`, config-mode `sw2cap on/off/stat/
  drain`). **Revised same day:** the initial CDC-auto-streaming design produced no usable output
  in practice (structurally incompatible with the config web UI's request/response protocol) —
  redesigned as a pull-based batch-drain command with a proper **"Switch 2 BLE Capture" panel in
  the config web UI** (Start/Stop/Clear/Download NDJSON, live status, per-kind/handle/length
  tallies, filters). **First real captures, same day:** four full sessions (stationary + 3 fixed
  orientations, ~530s, `dropped=0`) analyzed completely and programmatically (new tools
  `tools/analyze_sw2_capture.py`/`analyze_sw2_fields.py`) — found every byte of `0x000A`'s 63-byte
  report is a counter, an analog-stick field, or constant zero; **no orientation-responsive data
  anywhere in it**, confirmed under a tested (not assumed) alignment-shift check too. Feature-not-
  enabled vs. no-motion-on-this-report remain indistinguishable from passive capture alone (this
  repo's BLE init never sends the feature-enable command a reference tool does) — which triggered
  building a smallest-possible **opt-in, off-by-default experiment** (`sw2cap experiment on`,
  §2.6 of the doc below): subscribes the unverified `0x000E` handle and sends an evidence-supported
  feature-enable command, once per connection, logging everything. Build-clean on both boards.
  **Run, same day: positive-but-limited result.** `0x000E` is confirmed reachable — the `0x0C`
  `configure`/`enable` command pair was accepted (ACK'd) and produced a real, continuous ~33 Hz
  notification stream (2,331 records, `dumps/sw2_capture_2026-07-10-EXPERIMENTAL.ndjson`) on a
  handle that had never produced anything before. But the content is a byte-shifted **duplicate**
  of `0x000A`'s buttons+sticks payload (stick fields confirmed byte-identical, just 5 bytes
  earlier) — still zero orientation-responsive bytes. Diffing our command sequence against the
  reference tool's actual working init flow found three untested differences: `configure_features`
  should use `0xFF` (we used `0x07`), six SPI calibration reads happen between `configure` and
  `enable` that we skipped, and a previously-undocumented descriptor write (`85 00`, "report rate")
  happens after `enable`.
  **v2 implemented, same day, next: a six-variant experiment matrix + a GATT discovery tool**,
  replacing a single combined "v2" attempt so a positive result is attributable to a specific
  cause (`sw2cap variant 1-6`: control / mask-0xFF-only / handle-write-only / mask+write /
  calibration-sequence / full-sequence — the last also deferring the `0x000E` CCC subscribe to the
  end, a previously-unnoticed ordering difference found while re-reading the reference tool's
  actual connection flow). Before writing that code, re-derived the "report rate" write's raw
  target handle exactly as instructed — and caught a real arithmetic mistake from the prior pass in
  doing so (it's `0x000C`, not the previously-stated `0x000D`; the error double-applied an offset
  that only applies to a characteristic's value/declaration split, not to a descriptor). Since that
  correction is still reasoning on paper, also built a **one-shot GATT discovery tool**
  (`sw2cap gattdisc on`) that walks BTstack's own live discovery of every service, characteristic,
  and descriptor on the connection — ground truth for handle numbers going forward, not more
  arithmetic. Build-clean on both boards. Full design, the handle-numbering correction, the
  variant table, and the ordered capture procedure:
  `docs/switch2/ble-controller-protocol-inventory.md` §2.6, §2.7, §3.5-§3.7, §5, §9.
  **Run on hardware, same day, next — major result.** All 8 captures analyzed completely with a
  new tool (`tools/analyze_sw2_v2_captures.py`). GATT discovery resolved `0x000C` from real ground
  truth (a vendor descriptor of `0x000A`, confirming the pre-hardware paper correction; `0x000E`
  has its own untested equivalent at `0x0010`; several more undocumented characteristics found).
  **Every one of the six variants — including the plain control — produced a real, independent
  40-byte data block on `0x000E`** (offsets 14-54): absent from every earlier capture, independent
  of buttons/sticks, self-describing (a constant length-prefix byte reading exactly 40, matching
  the reference tool's documented 40-byte "Pro/GCN" block length), behaviorally distinct from every
  counter/duplicate pattern already characterized (smooth monotonic drift, not noise) — **the
  first independent motion-consistent data this project has observed from a genuine controller.**
  Because every variant succeeded, including the control (byte-identical commands to the earlier
  standalone run that found nothing across 70+ seconds), **none of the six tested differences is
  the actual cause** — the real differentiator lies outside the tested matrix, unresolved. No
  orientation/gyro/accelerometer semantics assigned to any byte. Full report (methods, causal
  table, ranked hypotheses, proposed next experiment — a stationary-only variant-1 re-run, no new
  code): `docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md`; summary in the inventory
  doc §3.8.
  **Continuation, same day, next.** User confirmed variant 1 (all six) were already captured
  stationary — resolving the proposed follow-up without a new run, and shifting priority to "what
  does the block encode." A new tool (`tools/analyze_sw2_motion_block.py`) found two genuinely
  accumulator-like fields via an exhaustive alignment/width/endian/sign scan: raw `0x13`-`0x14`
  wraps through the full int16 range every ~4s (structurally matching this project's own
  report-0x09 phase-accumulator architecture), raw `0x19`-`0x1A` drifts smoothly all session with
  no wraparound — no semantics assigned to either. Separately, diffing the prior (negative) session
  against this session's variant 1 found a **measured, concrete** timing difference: the old v1
  code fired CCC-subscribe/configure/enable within 1.2ms of each other with no ACK-gating (the
  device's `configure` ACK arrived ~39ms *after* `enable` was already sent); the current v2 code
  waits for real confirmation at each step — the leading (not confirmed) candidate cause. Built (both
  boards build clean): a capture-annotation marker (`sw2cap mark <text>`) and a fix for a real
  instrumentation gap (write-completion ATT status is now captured, not just printed) — both pure
  logging, verified not to change BLE timing/behavior. A controlled pitch/yaw/roll motion-capture
  protocol with marked phase boundaries is designed, not yet run. Full detail:
  `docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md` §8-§12; inventory doc §3.9.
  **Run and analyzed, same day, next.** Direction-correlation test on the two candidate
  accumulator fields was **negative** (neither reproducibly tracks pitch/yaw/roll direction across
  repeated identical motions; a full 39-offset scan found nothing better than chance-level effect
  sizes). Reframing the question found a strong, cross-validated positive instead: **~11 distinct
  byte offsets spanning nearly the whole 40-byte block show a 100-1000× residual-noise/transition-
  rate jump the instant the controller is handled at all** (rotation or a nominally-still "hold"),
  vs. near-total silence at confirmed genuine rest — one byte frozen 99.7% of the time at rest,
  changing 70% of the time during rotation. The "hold" phases show inconsistent, intermediate
  noise levels across offsets/axes, matching variable involuntary hand tremor rather than a fixed
  motion-flag. Classified as a new category, "activity/vibration-responsive noise floor" — doesn't
  fit rate-like/orientation-like/acceleration-like/counter-like cleanly. **No axis, unit, or scale
  assigned to any byte.** Strengthens the case the block is genuinely IMU-derived; doesn't identify
  which bytes are which sensor. Full detail: same report §13; inventory doc §3.10.
  **Structural decomposition, same day, next — no documented FIFO layout survives.** Per explicit
  direction, paused new hardware to investigate the block's internal packet structure before
  requesting the hand-held test above. Tested whether the block matches the controller's identified
  IMU (ICM-42670-P)'s documented FIFO packet format — candidate 2×20, 4×10, 5×8, 8×5 byte splits —
  using a new tool (`tools/analyze_sw2_block_structure.py`). **Explicit confidence caveat**: the
  byte-level layout tested is reconstructed from general/trained knowledge of the shared ICM-42xxx
  family architecture, not a directly-consulted ICM-42670-P datasheet (no document access this
  session). An entropy-periodicity test (the signature a repeating packet header leaves) found
  **no meaningful match at any tested period** — a real negative result, not absence of evidence.
  A physically-grounded orientation-invariant-magnitude test found one promising candidate, but
  it's confounded by an already-known non-physical accumulator byte, so it's inconclusive. The
  block-wide activity signature remains the strongest evidence something real is present; its
  internal packing is now confirmed not to match the layouts tested. Proposed (not requested from
  the user yet): a mechanically-supported, non-handheld experiment — table taps and a vibration
  source with zero skin contact, vs. a brief-contact control — separating "responds to genuine
  vibration" (supports real IMU data) from "responds to hand contact" (would argue against it),
  expected results defined per model in advance. Trigger investigation (the ACK-gating timing
  difference) kept explicitly separate, not touched this pass. Full detail: same report §14-§15;
  inventory doc §3.11.
  **🔴 Paused, same day, next.** Per explicit direction: three full analysis passes exhausted what
  passive statistics against this one opaque dataset can establish; no further indirect physical
  tests will be requested. Durable, non-retracted conclusion: the block is independently framed,
  responds strongly and reproducibly to physical handling, and its internal encoding/semantics
  remain unknown. Captures and tooling preserved. **This does not pause gyro/controller RE as a
  whole** — re-centered on the project's actual target instead (next bullet).
- **`Dycool/Usb-relay-for-NS` — full implementation-level feasibility audit, 2026-07-10.** Read the
  actual repository source this pass (not the earlier summary-level note below, now superseded). A
  live USB relay chain (real Switch 2 console ↔ Pi USB gadget ↔ UDP ↔ Windows PC ↔ real Pro
  Controller 2 via `hidapi`), explicitly targeting PID `0x2069`. **Positive finding**: its
  feature-enable commands (`0x0C` configure/enable, mask `0x27`) match this project's own
  documented bytes *exactly* — real cross-validation, not just a plausible guess. **Concrete
  defect**: the Pi-side gadget is a generic Linux `configfs` HID function with **no EP0/
  control-transfer relay at all** (confirmed by an explicit in-code comment, not inferred) — given
  this project's own hard-won knowledge that the console requires a byte-exact vendor handshake to
  accept a device, this is a real risk to console classification, not yet resolved either way
  without hardware. **Recommended path**: skip the full 3-node relay for now — a much smaller,
  lower-risk alternative (the Windows-side `hidapi` capture logic run directly against the genuine
  controller, no Pi or console needed) can already test whether the cross-validated feature-enable
  command unlocks motion data on its own. Full go/no-go report (confirmed/unverified/defects,
  required hardware, exact setup, minimal fixes, a 4-phase validation ladder, the eventual capture
  matrix): `docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`. A ranked inventory of
  other unknown controller surfaces (prioritization only): this file's "Controller surface
  inventory" section, above.

### 🔵 v1.1.5 — NFC / Amiibo reverse engineering (new, 2026-07-10)
**Active objective, replacing gyro as the primary RE target (gyro paused, see above).** Nintendo's
own documentation confirms the Pro Controller 2 has an NFC touchpoint (over the Switch logo,
top-center); protocol/hardware/init/Switch-1-compatibility treated as unknown until evidenced.
Six claims kept strictly separate throughout (official confirmation vs. Joy-Con-2/Pro-Controller-2
physical hardware ID vs. protocol behavior demonstrated per controller type vs. Switch-1
carryover — the last explicitly refuted at the command-ID level: Switch 1 uses MCU subcommands
`0x21`/`0x22` inside a different protocol entirely; Switch 2 uses a dedicated top-level command
`0x01` with its own subcommand family).

**First pass, same day:** re-mined this repo's own `usbpcaptures/genuine_procon_2.pcapng`
(164,242 packets) with a new reusable tool (`tools/extract_nfc_traffic.py`) for the command-`0x01`
envelope signature. Found and traced **two real request/response exchanges to exact packets** on
a capture this repo's own code explicitly identifies as a genuine Pro Controller 2: subcommand
`0x0C` (`61 12 50 10` response — confirms the exact provenance of `switch_pro2.c`'s already-
hardcoded value) and subcommand `0x01` (a bare `dir=0x04` acknowledgment, previously undocumented
anywhere consulted, including `ndeadly/switch2_controller_research`'s own subcommand table, which
lists `0x01` as "Unknown" with no example). Both exchanges occur once, interleaved in the
console's mandatory init sequence — NFC status is queried once at connection time, not polled.
Cross-validated against `ndeadly/switch2_controller_research` (GATT map, command table) and
Nintendo's official amiibo-support documentation. **One real, precise self-consistency gap found**
(not fixed this pass, analysis-only per explicit constraint): `switch_pro2.c` hardcodes response
`dir=0x01` unconditionally; the one genuine bare-ack response observed used `dir=0x04`.
Full inventory, evidence table, and next task: `docs/switch2/nfc-protocol-inventory.md`.

**Not evidenced by any source consulted this pass:** NFC IC identity (either controller); any real
amiibo tag transaction (detect/read/write/mount/unmount); chunking/checksums/authentication for
the `0x14`/`0x15` (write/read buffer) subcommands.

**Next task:** filter the same capture by USBPcap endpoint/transfer-type fields (not
first-payload-byte, which produced a false-positive collision against USB Configuration
Descriptors) to build a real report-`0x09` time series and check whether the NFC-state byte
(offset `0x0C`) ever leaves idle anywhere in the 164,242-packet session.

### 🟡 v1.2 — reliability & polish
- **BT pairing reliability** — fix the Pro 2 reconnect flakiness (triple-tap sometimes needed).
- **BLE DIS VID/PID** — resolve the PnP query properly so detection doesn't lean on device names.
- **Docs** — complete `/docs` (architecture, bt, switch2, RE methodology, experiments) per CLAUDE.md.

### Controller surface inventory, ranked (2026-07-10)

Prioritization work only, per explicit instruction — **not** permission to start several unrelated
implementations. Produced alongside the USB-relay feasibility audit
(`docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`) to re-center on the project's actual
target after BLE 40-byte-block decoding was paused. Ranked by relevance to faithful emulation,
available primary evidence, experimental tractability, dependency on unavailable hardware, and
information value — qualitative (H/M/L), reflecting this repo's current state of knowledge, not a
precise score.

| Surface | Relevance | Evidence | Tractability | HW dependency | Info value | Tier |
|---|---|---|---|---|---|---|
| USB report 0x09 motion (genuine, console-driven) | H | L (one static 3rd-party capture, unrepeatable) | M (relay audit found a viable low-cost path, §7 of the audit) | M (PC+controller only for the recommended first step; full console relay for later phases) | **H — the actual project target** | **1** |
| Initialization / command framing (USB `0x03` family, unverified) | M | L (unverified against this repo's own working sequence) | H (checkable directly against `switch_pro2.c` with no new hardware) | None | M | **1** |
| USB descriptors / EP0 vendor handshake | H (blocks the relay and any future USB-side emulation work) | M (this repo already has a byte-exact working handshake for its own PicoSwitch2 identity) | M (Phase 1 of the relay ladder tests this cheaply) | L (Pi + console only, no genuine controller needed) | H | **1** |
| Commands (BLE + USB, general) | M | H (extensively documented this session — SPI read, LED, pairing, feature config, GATT-discovered handle map) | — | — | Already well-covered | 3 |
| Input variants (report 0x05/0x09 USB, `0x000A`/`0x000E` BLE) | H | H (0x05/0x09 well-documented; BLE `0x000A`/`0x000E` structurally mapped, semantics paused) | — | — | Already well-covered where tractable | 3 |
| Output reports (rumble) | M | H (decoded and forwarded, working) | — | — | Already covered | 4 |
| Calibration (factory SPI cal) | M | H (`0x13040`/`0x13100`/`0x13080`/`0x130C0` decoded and cross-validated) | — | — | Already covered | 4 |
| Memory regions (SPI dump) | L-M | H (bond table, battery curve, `"DSPH"` blob all characterized) | L for the DSP blob specifically (needs a second unit to diff against) | M (second physical unit for the DSP blob) | L for emulation fidelity, M for RE completeness | 3 |
| Reconnect reliability | M (user-facing flakiness) | M (three candidate mechanisms identified from external research, unaudited against this repo's own code) | H (pure code audit, no new hardware) | None | M | **2** |
| Wake-over-BLE | L (already out of scope, requires MAC spoofing) | M (mechanism understood, not attempted) | L | H (needs a bonded, sleeping console) | L | 5 |
| Mouse mode (feature-flag bit 4) | L (no known use case for this project) | L (bit identified, never exercised) | M | L | L | 5 |
| Audio | L (explicitly out of scope for BT controllers) | L | L | M | L | 5 |
| Haptics (HD-rumble fidelity) | M (backlog item, not blocking core function) | L-M (DSP blob is a candidate lead, unconfirmed) | L (needs a second SPI dump to diff) | M | M | 3 |
| Firmware-update passthrough | L (community-archival value, not emulation-blocking) | L | L | M | L | 5 |

**Tier 1 (do first, cheap, high-leverage)**: the relay's Phase 0 (§7 of the audit — no console or
Pi needed) and Phase 1 (§9's ladder — Pi + console, no genuine controller needed) can both run
independent of each other and independent of fixing anything; the `0x03`-family init-command check
against `switch_pro2.c` is pure code auditing with zero new hardware. **Tier 2**: reconnect-
reliability code audit, same reasoning. **Tiers 3-5**: already reasonably well-documented, lower
marginal value, or blocked on hardware/access this project doesn't currently have — not
recommended before Tier 1/2 items land.

### Backlog / longer-term
- **Multi-controller** — lift the single-controller milestone toward 4 players (USB hub confirmed to
  work on the Switch 2; determine the real per-player output path).
- **Advanced haptics** — capability-based translation (HD-rumble ⇄ DualSense) where practical.
- **Switch 2 GameCube controller** — documentation first (analog triggers, unique mapping), then support.
- **Firmware-update passthrough** — receive/store the console's controller update for community archival.
- **BT bond-table realism** (new 2026-07-10, from SPI dump RE) — our `0x1FA000` memory-read always
  claims zero stored bonds; a genuine unit's flash shows a real two-record bond table (BD_ADDR +
  shared 16-byte key, BR/EDR + LE pair for one host). Investigate whether presenting a plausible
  bond record improves the "Pro 2 reconnect sometimes needs a triple-tap" flakiness. See
  `docs/experiments/spi-dump-analysis-2026-07-10.md` §3.4.
- **Battery-curve fidelity** (new 2026-07-10) — a genuine unit's flash (`0x1FB000`) has a real
  per-unit discharge-curve table; our `0x0B` battery command currently returns fixed placeholder
  values. Low priority. See `spi-dump-analysis-2026-07-10.md` §3.5.
- **`"DSPH"` firmware blob** (new 2026-07-10) — a genuine unit's flash has a self-describing,
  correctly-length-prefixed DSP firmware/coefficient blob at `0x175000` (207,536 bytes), almost
  certainly audio or HD-rumble-waveform DSP (not motion — no evidence ties it to the IMU pipeline).
  Candidate future RE target for haptics/audio fidelity; needs a second unit's dump to diff against
  before it's tractable (this pass only had two dumps of the *same* unit, which were identical).
  See `spi-dump-analysis-2026-07-10.md` §3.6.

### Out of scope (confirmed)
Wake-console-over-USB (needs a BLE advert from a bonded controller; our link is USB); audio over BT
controllers; HD-rumble → DualSense haptics as a v1 feature.

---

## Reverse-engineering direction

Treat the Pico as a protocol-analysis platform. Four proven instruments so far: the config-mode
**raw-HID-report** view (reverse new controllers live), **direct capture parsing** — the report-0x09
motion format was cracked by parsing ndeadly's *unencrypted USB* capture (the BLE captures are encrypted;
USB is not — check the transport before assuming a wall) — **live BLE querying** via
`tools/switch2_input_viewer.py` (external, added to `tools/` for reference; a working reference
implementation of the same command protocol this repo's own USB path implements, useful for resolving
unknown SPI addresses/command bytes and for capabilities this repo's static analysis can't reach, e.g.
reading a *calibrated* unit's user motion-cal region) — and, as of 2026-07-10, this repo's own
**non-invasive BLE traffic capture** (`src/bt_hid/sw2_capture.c`, config-mode `sw2cap on/off/stat/
drain`, driven by a dedicated capture panel in the config web UI —
`docs/switch2/ble-controller-protocol-inventory.md`): the durable next step this section used to
describe as aspirational — "our dongle is itself a BT host that *decrypts* a real controller's reports,
so the raw-report view can read any future Switch 2 controller's format directly, with no external
captures" — is now implemented **and hardware-validated**: four full sessions captured and
completely, programmatically analyzed (`dropped=0` throughout), finding no orientation-responsive
data in the one report format captured so far — see `ble-controller-protocol-inventory.md` §3.5.
The natural next instrument this enabled: an opt-in, off-by-default one-shot experiment
(`sw2cap experiment on`, same doc §2.6) that tries the evidence-supported next step (an additional
notification handle + feature-enable command) and captures the result — implemented, not yet run.
Every experiment belongs in `/docs/experiments` with question → hypothesis → method → result →
remaining unknowns.
