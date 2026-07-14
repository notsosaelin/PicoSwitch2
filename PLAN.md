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
**rumble**, **1000 Hz** poll (raised from 250 Hz 2026-07-12 — 🔵 latency-over-fidelity deviation,
untested on console hardware, see `docs/switch2/usb-spec.md` §13). Every joypad-os controller
supported (DualSense/Edge, Xbox/Elite 2, Switch, 8BitDo, …) with extended buttons (Edge paddles/Fn,
Elite 4 paddles). Full **config web UI** (live view, dynamic per-controller menu, **per-device
remapping**, lightbar, raw-report debug). PC/Steam enumerates as a Switch 2 Pro with full
buttons/sticks (gyro added in v1.1). Builds on Pico W + Pico 2 W. bluepad32 removed.

**2026-07-12 stub-driver polish pass** (see `STATUS.md` "Last Updated" for full detail): NFC
response `dir` byte fixed for bare acks (closes a 2026-07-10 self-consistency gap against the
genuine capture); audio stub hardened (confirmed TinyUSB's core already ACKs `SET_INTERFACE` even
when the stub stalls; added static Mute/Volume `GET_CUR` answers — spec-compliance only, no
functional audio); USB poll rate 250→1000 Hz with a decoupling fix so the paused gyro tracker's
per-call-tuned EMA constants aren't silently invalidated by the faster call rate. All three changes
independent, build-clean both boards.

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

**Done 2026-07-12, conclusive negative result — capture acquisition is now the actual blocker, not
analysis technique.** Filtering `genuine_procon_2.pcapng` by real USBPcap header fields (endpoint/
transfer-type, via new `tools/extract_report09_timeseries.py`) proves it contains zero report-`0x09`
records: it's a PC/Windows session (the real controller only streams report `0x05` to a PC host,
19,554 real samples confirmed), never a console session. The NFC-state-byte question needs an
actual console-side USB capture — this project has never obtained one, for NFC or anything else
(the same gap already blocking report-0x09 gyro ground truth). Two incidental doc errors found and
fixed along the way: a device-number mix-up (this capture's real Pro Controller 2 is USBPcap
device 38, not 7 — "device 7" was a fact about a different, external capture file) and a mis-read
report-select target (selects report `0x05`, not `0x09`, in this file). Full detail:
`docs/switch2/nfc-protocol-inventory.md` §2.5.

**`Dycool/NS-PC-Control` audit, 2026-07-12 — biggest capability gap identified, deliberately not
ported.** A fresh clone (commit `a422f4b`) of this external reference project's current native
Switch 2 implementation has a complete, working amiibo read/write emulation (NFC subcommands
`0x03`-`0x15`: scan mode, status, begin-operation, a 622-byte read-buffer format, a 454-byte
write-staging format, commit) — this project's biggest NFC evidence gap, closed *as a hypothesis*
only. Not ported to code: their captures aren't bundled/re-checkable, so none of it is "Confirmed"
by this project's own standard. Recorded in `nfc-protocol-inventory.md` §4 as 🔵 Hypothesis, ready
to validate against if this project ever captures its own real amiibo transaction. Full audit
(also covers a second real factory-memory fix, `0x13060`, and refined BT-reconnect/wake evidence):
`docs/experiments/ns-pc-control-audit-2026-07-12.md`.

### 🟡 v1.2 — reliability & polish
- **BT pairing reliability (🟢 root cause found and fixed, 2026-07-12 — hardware validation
  pending)** — fix the Pro 2 reconnect flakiness (triple-tap sometimes needed). Full end-to-end
  lifecycle trace of `btstack_host.c` (not the `NS-PC-Control`/BlueZ comparison evidence from
  earlier the same day, which only informed the search) found the real defect: the BLE reconnect
  path fired an unconditional up-to-5×/~50s blind `gap_connect()` cascade on *every* disconnect —
  including ones the peer initiated on purpose — starving scanning the whole time; confirmed
  present only on the BLE path (real Switch 2 Pro Controller/Joy-Con 2), matching the documented
  "Pro 2" naming exactly, with the parallel Classic BR/EDR path (DualSense/Xbox/etc.) confirmed
  clean. Fixed by finally consulting the disconnect-reason byte (captured, never used before this).
  **Second, distinct defect found the same day**, following up per explicit direction: the separate
  post-link GATT pairing-command retry (`switch2_retry_init_if_needed()`) used a never-reset
  call-count timer (confirmed real caller ~33Hz, not the assumed ~120Hz — so retries fired every
  ~1.8s, not the intended ~500ms), was unbounded, and had no recovery path if a step never got
  acknowledged (a distinct permanently-stuck-connection failure mode the link-layer fix can't
  reach, since no disconnect ever happens to trigger it). Fixed with a real `btstack_run_loop_get_time_ms()`
  deadline, bounded at 10 retries, and an explicit recovery `gap_disconnect()` that composes with
  the reconnect fix above. Build-verified across all three configurations; both fixes need a real
  hardware test before closing — exact procedures in `docs/bluetooth/btstack-implementation.md`
  "Reconnect reliability" and "Switch 2 GATT init retry timing".
  **Third, distinct defect found 2026-07-13**, directly from the controller owner's report that a
  genuine Switch 2 Pro Controller paired unreliably specifically via explicit pairing mode (fine via
  normal auto-reconnect): the pairing window's expiry unconditionally called
  `btstack_host_stop_scan()`, which resets `hid_state.state` to `IDLE` regardless of whether a BLE
  `gap_connect()` was genuinely in flight — silently disarming the `BLE_CONNECT_TIMEOUT_MS` (10s)
  watchdog for that specific attempt (confirmed via an exhaustive trace: `hid_state.state` is read in
  exactly six places, none past the raw-connect phase — GATT/HID/Switch 2 init were never actually at
  risk). Fixed with a new `btstack_host_close_pairing_window()` that defers the close until an
  in-flight attempt resolves, bounded by the existing `BLE_CONNECT_TIMEOUT_MS` rather than a new
  constant; `PAIRING_WINDOW_MS` also widened 10s→30s for usability (secondary). Classic BT traced and
  confirmed not vulnerable (independent watchdog). Build-verified across the full four-combination
  matrix; needs a real hardware pairing test — exact procedure and an 8-point semantic checklist in
  `docs/bluetooth/btstack-implementation.md` "Pairing window vs in-flight connect".
- **BLE DIS VID/PID** — resolve the PnP query properly so detection doesn't lean on device names.
- **Docs** — complete `/docs` (architecture, bt, switch2, RE methodology, experiments) per CLAUDE.md.

### 🟡 Active priority program (2026-07-12): rumble → BT identity hardening → GameCube-class support

A strict 3-gate sequence — each gate must be validated (or blocked on a precise hardware
observation) before the next begins.

- **Gate 1 — BT rumble regression: 🟢 core regression restored on hardware; edge cases tracked.**
  The latest combined build has now produced real rumble on Xbox, DualSense, Switch 1 Pro
  Controller, and Wiimote both standalone and with its joystick attachment. User-reported
  regression across DualSense/Xbox/generic. Fixed: a
  `find_player_index()` routing bug (`ns2_seam.c` — returned a raw BTstack connection-slot index
  instead of always 0, so a Classic BT device outside slot 0 read a feedback slot never written to
  — **BLE was never affected**, its conn_index already fell through to the same fallback); a
  completely unimplemented Xbox Classic BT rumble path (`xbox_bt.c`'s `xbox_task()` was a no-op);
  and, found via reference-driver research rather than another hardware round-trip, a wrong
  `loop_count` byte. After the first two fixes, a hardware test of an Xbox Series controller over
  BLE still showed no rumble — confirmed unrelated to those fixes (BLE routing was never affected).
  Instead of guessing at the wire format or requiring a fresh capture, compared `xbox_ble.c`/
  `xbox_bt.c` byte-for-byte against the Linux `xpadneo` driver (atar-axis/xpadneo), the mature
  reference Xbox-BLE HID driver: report ID `0x03` and every byte position matched exactly, but
  `loop_count` (byte 7) was `0x00` where xpadneo sets `0xEB` to sustain an effect ~10 minutes from
  one command — `0x00` likely stopped the motor after a single ~2.55s pulse, invisible to a tester
  since this driver only resends on an amplitude change. **Initially fixed in `xbox_ble.c`/
  `xbox_bt.c` — a follow-up subagent audit then found those files are dead code, never registered
  (`bthid_registry.c`: "generic driver handles all Xbox"), so the fix never ran.** Re-applied to the
  actually-reachable path, `bthid_gamepad.c`'s `gamepad_task()`, which had the identical bug
  independently. **User decision: re-register `xbox_bt.c`/`xbox_ble.c` as the primary Xbox path**
  (retiring the generic driver's role for Xbox except Elite Series 2, which both files still
  exclude by product ID and which keeps using the generic driver's already-fixed code).
  `xbox_ble.c`'s input parsing is evidence-backed ("verified from testing"); `xbox_bt.c`'s Classic
  BT parsing guesses between two report formats with no such evidence — flagged as this pass's top
  hardware-validation risk. The same audit fixed a duplicated `+64` bug in `switch_pro_bt.c`'s
  rumble encoder and a missing CRC32/wrong `hw_control` flag in `ds4_bt.c` (verified against current
  Linux `hid-playstation.c` source), and flagged (not fixed) a channel-choice inconsistency in
  `wiimote_bt.c`. The earlier-suspected stereo L/R rumble refactor was **ruled out** by full diff review
  from the start of this investigation — never the cause. Full trace, evidence, fix-requirements
  checklist, and validation matrix: `docs/bluetooth/btstack-implementation.md` "Rumble regression".
  Build-verified and hardware-validated for presence of rumble on the controller families listed
  above. Per-motor fidelity, amplitude/stop/reconnect behavior, generic non-Microsoft XInput,
  DS4, Xbox transport/input details, and the Wiimote channel question remain follow-ups.
- **Gate 2 — BT/BLE identity hardening: 🟡 in progress, first real hardware validation passed
  (8BitDo NGC Modkit), remaining controllers untested.** Driver reachability audit: confirmed all
  11 BT HID drivers compiled+registered+reachable; found and structurally fixed a real shadowing
  bug (`switch_pro_bt.c`, Classic-only Switch 1 hardware, could claim a BLE-connecting Switch 2 Pro
  Controller via an unguarded name fallback, and the existing re-evaluation safety net wouldn't
  have self-corrected it) by adding `bthid_transport_mask_t` to every driver, checked centrally
  before `match()` — closes the whole bug class, not just this instance. Fresh `joypad-ai/joypad-os`
  audit (cloned SHA `b292005...`, 2026-07-11): confirmed upstream still deliberately routes Xbox
  through the generic driver (new evidence weighed against this project's re-registration decision,
  not silently reverted); preserved two hardware-validated PicoSwitch2-only improvements upstream
  lacks (Xbox Elite Series 2 paddles, Xbox name-based VID fallback). Root cause of `0000:0000`
  VID/PID: **Confirmed** not a parsing bug — BTstack's PnP-ID accessors are spec-correct; the real
  cause is that DIS query is deliberately deferred past driver binding (avoiding a real prior
  GATT-contention regression) — fix targets re-evaluation robustness instead (also fixed the same
  pattern in `ds4_bt.c`; `ds3_bt`/`ds5_bt`/`wii_u_pro_bt`/`wiimote_bt`/`xbox_bt`/`xbox_ble`/
  `stadia_bt` still need the same audit). New `bt_identity_log.c`/`btid dump` facility (bounded,
  pull-based, same proven pattern as `sw2_capture.c`) records one event per binding decision —
  **hardware-validated 2026-07-12** against a real 8BitDo NGC Modkit, which also caught and fixed
  three real logging-completeness bugs in the process. Stable per-device profile key designed and
  documented, not implemented (no consumer exists yet). Full detail:
  `docs/bluetooth/driver-reachability-audit.md`,
  `docs/bluetooth/joypad-os-upstream-comparison-2026-07-12.md`,
  `docs/bluetooth/btstack-implementation.md` "Gate 2" section,
  `docs/experiments/gate2-identity-log-hardware-captures-2026-07-12.md`. Build-verified across all
  three configs; **2026-07-13: genuine Switch 2 Pro Controller (BLE) also hardware-confirmed
  clean** — single `initial-bind`, correct driver, `provenance:"ble_adv_mfr_data"`, no
  shadowing/re-evaluation, `device` command independently agreeing (see experiment doc "Capture 2").
  This is the actual scenario `bthid_transport_mask_t` was built to protect; the specific
  hypothesized `"Pro Controller"` name-collision was confirmed **not reachable** on this hardware
  (real advertised name is `"Switch 2 Pro"`), so the fix is validated as defense-in-depth rather than
  a reproduced-failure fix. **Switch 1 Pro, Xbox, DualSense, Wiimote, and generic/XInput remain
  hardware-untested for identity capture** (though Xbox/DualSense/a generic Switch Pro Controller
  were reported to *pair* successfully — no `btid dump` pulled for those sessions yet). Separately,
  a real pairing-window bug was found and fixed the same day from a genuine Switch 2 Pro Controller
  report of unreliable explicit-pairing-mode connects — see "BT pairing reliability" above (v1.2).
- **Gate 3 — GameCube-class support: 🔵 both halves now active.** Two separable problems, kept
  separate per explicit instruction: (a) 8BitDo NGC DIY Bluetooth input support — **implemented and
  owner-confirmed working 2026-07-12** (`NGC_MODKIT_BUTTON_MAP` in `bthid_gamepad.c`, PID-specific
  `0x2DC8:0x286A`), full mapping and the two rejected design iterations documented in
  `docs/bluetooth/8bitdo-ngc-diy-profile.md`; the controller's reported second "Android/D-Input"
  BLE pairing mode is unconfirmed and not yet captured — needs its own session before assuming this
  profile covers it. (b) a native NSO GameCube Controller **USB output personality** — **explicitly
  promoted to highest project priority 2026-07-13** (`NSO-GC.md`), superseding the earlier
  "don't start until Gate 2's remaining controllers are validated" sequencing. **Stage A
  (research/architecture) substantially complete same day** — see "NSO GameCube Controller output
  personality" below for the dedicated status. The 8BitDo mapping's current Pro2-mode design (Z→R,
  triggers→ZL/ZR via analog fold) is confirmed as a Pro-Controller-2-specific approximation; a
  concrete second-mode mapping table (native Z, native analog L/R, native digital detent) is now
  designed in `docs/switch2-gc/mapping.md`, superseding the older "Future NSO mode" sketch in the
  profile doc (that doc should be pointed at the new mapping doc once the GameCube personality is
  actually implemented, rather than duplicated).

### 🟢 NSO GameCube Controller output personality (2026-07-13) — Stage B implemented and hardware-validated

New USB output personality emulating the genuine Nintendo GameCube Controller for Switch 2
(`057E:2073`), architected as a third `usb_personality_t` state alongside CDC-config-mode and the
existing Switch 2 Pro Controller 2 personality — not coupled into `switch_pro2.c`, not exclusive to
any one input controller. Full stage-by-stage plan and evidence base:
`docs/switch2-gc/protocol.md` (protocol reference, evidence-graded), `docs/switch2-gc/usb-personality.md`
(architecture — codebase audit of Pro2 coupling + the runtime personality-selection design, reusing
the existing CDC-mode live-re-enumeration mechanism as proven prior art), `docs/switch2-gc/mapping.md`
(mapping policy — native GameCube capability set, explicit L3/R3 exclusion, 8BitDo NGC Modkit
second-mode design intent). Supporting evidence:
`docs/experiments/nso-gc-reference-repo-audit-2026-07-13.md` (both reference repos cloned+audited —
critical finding: SoulCalDan's repo implements the unrelated older WiiU GC Adapter protocol, not the
native NSO protocol; re-scoped to ndeadly's repo as sole primary source),
`docs/experiments/nso-gc-spi-dump-analysis-2026-07-13.md` (both provided SPI dumps analyzed,
cross-validated against ndeadly's documented factory-data addresses — 6/8 match exactly — with
per-unit identity/pairing material correctly identified and flagged for exclusion).

**Update, same session — the privilege blocker is resolved.** Retried the elevated USBPcap capture
(UAC-approved this time) using `--inject-descriptors`, which replays descriptor-fetch transactions
already cached by USBPcap's driver from the device's real enumeration — no physical replug, no driver
changes. Result: **complete, byte-exact device descriptor (18B) and configuration descriptor tree
(80B, matching `wTotalLength` exactly)**, preserved at
`docs/experiments/nso-gc-captures/genuine-controller-descriptors-2026-07-13.pcap` and fully decoded in
`docs/switch2-gc/protocol.md`. This is Confirmed-tier evidence for everything Stage B needs from the
device/configuration descriptors. A follow-up 15s live capture (same elevated method) was attempted to
also catch the raw 97-byte HID **Report** descriptor body and string-descriptor text, but produced no
output this time — not yet resolved why (possibly a second UAC prompt wasn't approved).

**Update, same session — decoded ndeadly's `rumble-procon-gccon.pcapng.gz`, pure offline analysis, no
hardware or elevation needed.** Found: a second genuine unit's device descriptor byte-for-byte
matching this project's own (independent cross-hardware confirmation); a neutral-state live report
`0x0A` that decodes correctly against every documented field with zero contradictions — **promotes
report `0x0A`'s layout from Strong to Confirmed**; 8 real rumble-test samples of output report `0x03`
(framing Confirmed, byte-level intensity encoding still Hypothesis); and a new, previously-undocumented
architecture fact — part of the init handshake uses plain EP0 vendor control transfers (not only the
bulk interface), including a factory-read command that independently cross-validates against this
project's own SPI dump. Full detail: `docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`.

**Update, same session — Stage B implemented.** Per an updated `NSO-GC.md` handoff (superseding the
prior one), moved directly into implementation. **Product decision correcting this document's own
earlier Stage F recommendation**: personality selection is a *volatile, BOOTSEL-hold-cycled* runtime
state (Pro2 → GameCube → CDC config, terminal; power-cycle returns to Pro2), not persisted to flash —
Stage F as originally scoped (persisted selection, reboot-required switching) is superseded, not
merely deferred. New `usb_personality_t` enum owned exclusively by core0; core1's existing BOOTSEL-hold
gesture (`bootsel.c`, unmodified — its `hold_fired` latch already satisfied every requirement) now
requests a cycle instead of unconditionally entering config. New `src/switch_gc/switch_gc.c` module
carries Confirmed byte-exact descriptors (promoted from Strong this same pass — see below) and
explicit, individually-evidence-tagged Stage-B-scope stubs for everything else. Centralized every
TinyUSB callback that can only have one link-time definition (descriptor callbacks, EP0 vendor
control, mount, HID get/set report); found and fixed a real latent bug in the process (Pro2's
WinUSB/MS-OS-1.0 string descriptor was gated only on "not config mode," so it would have incorrectly
answered for GameCube mode too). Two new pure-logic files (`src/usb_mode_cycle.c`, zero pico-sdk
dependency) are host-compilable and host-tested
(`tools/test_usb_mode_cycle.c`); `tools/verify_gc_descriptors.py` independently re-derives the
expected descriptor bytes (not copied from the C source) and diffs them against `switch_gc.c`. Same
pass, decoding `rumble-procon-gccon.pcapng.gz` further promoted the GameCube HID report descriptor's
structure and a second physical unit's device/config descriptor bytes to Confirmed — this closed what
had been the last real Stage B evidence gap before implementation even started. Two real evidence gaps
remain, neither blocking Stage B and not fully blocking Stage C's *start* (per NSO-GC.md's explicit
scope note) but both worth closing before Stage C/E claim completeness: (1) report `0x03`'s exact
byte-level rumble encoding (8 real samples in hand, pattern not fully decoded); (2) a live
button-press sample for report `0x0A`'s bitfield. Build-verified across the full four-combination
matrix; `NS2_PRO=OFF` (Switch 1) confirmed unchanged by direct diff review of its own branch in every
touched file.

**Update, same day — hardware-validated via the owner's own comparative testing against a genuine
controller, two real bugs found and fixed.** Missing MS OS 1.0 WinUSB auto-bind (Code 28) fixed first;
insufficient alone. The real culprit: `bcdDevice` used the exact raw-captured value, making the Pico
byte-for-byte identical to a genuine controller across every field Windows' WinUSB driver cache keys
on (VID+PID+bcdDevice — serial doesn't help, it's `"00"` on both) — corrupting the *genuine*
controller's own cached Steam mapping too until fixed, the identical problem `switch_pro2.c`'s own
descriptor already solved the same way (deliberately non-real `bcdDevice`). Confirmed working after
both fixes: clean enumeration, no driver error, genuine controller unaffected. Steam's "Begin Setup"
prompt (vs. the genuine controller's full native recognition) is the expected Stage B boundary, not a
new bug — native recognition needs the init-handshake response Stage D will add. Full narrative:
`docs/switch2-gc/usb-personality.md` "Implementation status".

**Update, same day — both remaining Stage B evidence gaps closed (no new hardware, one live capture
+ one re-analysis); Stage C started.** The HID report descriptor gap closed via a live USBPcap replug
capture (promoted Strong→Confirmed); the rumble-encoding gap was re-scanned exhaustively across the
*entire* `rumble-procon-gccon.pcapng.gz` capture (not just the known burst window) and confirmed the 8
existing samples really are the complete population — also surfacing a real correction (Output Report
`0x03` is 63 data bytes, not 41) and a new Hypothesis-tier lead on the previously-Unknown `bRequest=2`
command. Per explicit direction, further rumble research is now deliberately paused (data-scarcity
limit, not effort). Stage C (report `0x0A` construction) is **started**: `switch_gc_build_report()`
implements every destination the shared input model currently supports; native GameCube Z, digital
L/R trigger detent, continuous analog L/R trigger, and all per-device mapping tables remain, gated on
a `switch_pro_input_t` extension. Stage E got a provisional (explicitly not final) opaque-intensity
rumble implementation. Full detail: `STATUS.md`'s matching 2026-07-13 entries.

**Update, same day, third pass (`PROMPT.md`) — Stage C completed, Modkit GameCube-mode mapping
implemented, a real TinyUSB report-ID bug fixed, minimum Stage D gate implemented on Confirmed (not
speculative) evidence.** The `switch_pro_input_t` extension flagged above is done (`gc_extra`/
`left_trigger`/`right_trigger`), populated only for the 8BitDo NGC Modkit and forwarded
unconditionally regardless of active personality; a real analog-fold collision (would have
synthesized a ZL/ZR the Modkit doesn't have) was found and fixed with a personality-and-device-gated
suppression. Re-mining the same rumble capture for previously-unanalyzed bulk vendor-interface (IF1)
traffic resolved protocol.md's own flagged USB-vs-BLE framing uncertainty and promoted the actual
streaming-trigger command sequence to Confirmed — `switch_gc_task()` now really streams once armed.
Ten new golden encoder tests plus seven report-ID-normalization tests all pass; full four-config
build matrix green. **Not hardware-validated** — see `DATA.md` for why the owner's existing
Windows/Steam setup can't exercise the new Stage D gate at all (it needs a real Switch 2 console or
a not-yet-built raw-bulk-write tool), and what the next concrete step is.

**Bringing this section current, 2026-07-14 (see `STATUS.md`'s own dated entries for the full
pass-by-pass account — this note exists so a reader stopping at PLAN.md alone isn't left with a
stale picture):** GameCube mode went on to reach full real-console recognition and confirmed-working
button mapping (Stage C/D substantially complete and hardware-validated). Rumble was NOT left
"deliberately paused" as the entry above says — it became the dominant multi-day investigation after
real hardware testing surfaced an actual P0 regression (immediate full-strength rumble on
GameCube-mode entry). That investigation is now itself substantially resolved: a PC-side USB
protocol lab (`tools/gcusb`) was built per an explicit handoff (`PROMPT.md`), a real downstream
Bluetooth-forwarding bug was found and fixed (Xbox rumble bridge), and — the actual headline
result — reading the real Linux kernel "HID: nintendo" driver source revealed the GC rumble
byte-model this project had used since 2026-07-13 was fundamentally wrong (the genuine controller
has no continuous-amplitude rumble hardware at all; it's a 3-state ON/OFF/STOP motor, not a linear
amplitude byte). All of this is fixed in code. **None of it is hardware-validated yet** — that is
the current single most important next step, not further rumble research. Full detail:
`docs/experiments/gcusb-rumble-lab-2026-07-14.md`, `docs/experiments/refuted-hypotheses.md`,
`DATA.md`.

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
| ~~Initialization / command framing (USB `0x03` family, unverified)~~ **DONE 2026-07-12** | M | H (re-verified byte-exact against the raw capture; 3 new subcommands confirmed, 1 real gap fixed — `docs/switch2/usb-spec.md` §14) | — | — | Closed | 4 |
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
- **DualSense audio passthrough** (new 2026-07-12, research only) — bridge the Switch 2's USB
  audio interfaces to a connected DualSense's own onboard speaker/jack/mic over its proprietary
  BT audio protocol (Opus-encoded, report `0x39`/`0x32` — not currently touched by `ds5_bt.c` at
  all). A working MIT-licensed reference exists for the structurally identical PC-facing bridge
  (`awalol/DS5Dongle`, same board family/SDK). Real feature-sized work (core1 budget conflict with
  the existing BTstack loop is the load-bearing open question) — not started. Full protocol
  detail, dependency license status, and a concrete work breakdown:
  `docs/switch2/audio-passthrough-research.md`.
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
