# Genuine Switch 2 Controller — BLE Protocol Inventory, Capture Facility, and RE Plan

> **✅ Production resolution, 2026-07-21:** the UART console bridge and a new live GATT discovery
> capture resolved the native Pro2 path. Report `0x000E` value/CCC/descriptor handles are
> `0x000E/0x000F/0x0010`; the console uses feature mask `0x27`; and a 7.5 ms central interval
> yields an interleaved 133 Hz `0x1E`/`0x28` stream. The dongle now transports those opaque PDUs
> into USB report `0x09`, hardware-confirmed in Splatoon 3. Sections describing the earlier v1-v6
> search and handle hypotheses are retained as experiment history, not current instructions. See
> [`native-pro2-motion-passthrough-2026-07-21.md`](../experiments/native-pro2-motion-passthrough-2026-07-21.md).

> **✅ Bonded HOME reconnect resolution, 2026-07-22:** the custom ATT `0x15` exchange stores the
> controller's normalized LTK locally and in BTstack's LE device database with RAND/EDIV zero.
> Subsequent HOME connections must call `sm_request_pairing()` and wait for
> `SM_EVENT_REENCRYPTION_COMPLETE`; a raw HCI Start Encryption command produced encrypted ACL
> traffic but did not restore BTstack/controller session security, leaving the controller in its
> running-LED state with no active input. After SM success, the host restores ACK/input CCCs,
> reasserts P1, and runs the existing native-motion feature setup. A genuine Pro Controller 2
> passed 20 consecutive controller-off/HOME cycles without SYNC, with input, LED, and gyro intact.

> **🔴 Semantic decoding of the `0x000E` 40-byte motion block is PAUSED (2026-07-10)** — three full
> analysis passes (direction correlation, orientation-invariant vector interpretation, periodic
> native-FIFO packet structure) failed to converge; see
> `docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md`'s pause banner for the durable
> conclusion and what remains true. **The rest of this document (capture facility, field-level
> inventory, GATT handle map, experiment tooling) remains active and accurate** — only the specific
> "what does the 40-byte block encode" thread is paused. The project's active focus has re-centered
> on genuine console-side USB report 0x09 — see
> `docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`.

**Status:** 🔵 In progress (updated 2026-07-10). The raw-capture facility (§2) is
**hardware-validated**: four full sessions (~530 s total, one stationary + three distinct fixed
orientations, `dropped=0` throughout) have been captured and analyzed programmatically (§3.5,
`tools/analyze_sw2_capture.py`, `tools/analyze_sw2_fields.py`). **Finding: on this connection, as
currently initialized, handle `0x000A`'s 63-byte input report contains no orientation-responsive
data anywhere in it** — every one of its 63 bytes is accounted for as either a free-running
counter, one of two analog-stick 12-bit pairs, or constant zero, across all four captures. This
refutes "motion is already present in `0x000A` at a different offset" (§3.5.1) but does **not**
determine *why* — feature-not-enabled and no-motion-on-this-report are both still consistent with
the data (§3.5.3). Per this pass's task, that ambiguity triggered building a smallest-possible,
**off-by-default** opt-in experiment (§2.6) that attempts the evidence-supported next step
(subscribe to `0x000E`, send a feature-enable command).

**The v1 experiment has been run** (`dumps/sw2_capture_2026-07-10-EXPERIMENTAL.ndjson`, §3.6):
`0x000E` is confirmed **reachable** — the CCC write and `configure`/`enable` command pair were
accepted, and a real, continuous ~33 Hz notification stream (2,331 records over 70.8 s) began
arriving on a handle that had never produced anything before. But its content is a byte-shifted
**duplicate of the same buttons+sticks payload** already seen on `0x000A`, with the same
"everything past the stick fields is constant zero" result — no orientation-responsive data
appeared on either handle. This is a genuine negative result, not an inconclusive one: reachability
and command-acceptance are now Confirmed; "our exact `configure(0x07)`/`enable(0x07)` call turns on
IMU content" is refuted.

**A v2 experiment matrix + a GATT-discovery tool have since been implemented** (§2.7, §3.7),
replacing the single v1 toggle: instead of guessing which of three untested differences from the
reference tool's working sequence matters (mask value, calibration reads, an extra descriptor
write), each is isolated as its own one-shot, independently-selectable variant (`sw2cap variant
1-6`), plus a separate one-shot GATT discovery tool (`sw2cap gattdisc on`) to establish real
ground-truth handle numbers rather than trust arithmetic on a third-party tool's bleak-indexed
handles (§3.7.1 found and corrected a real error in that arithmetic before any v2 code was
written). Both boards build clean.

**Both have now been run on hardware — major result (§3.8, full report
`docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md`):**
1. **`0x000C` is resolved from real GATT discovery, not paper arithmetic**: it's a vendor-specific
   descriptor of the `0x000A` characteristic (not `0x000E`) — confirming §3.7.1's pre-hardware
   correction. `0x000E` has its own equivalent descriptor at `0x0010`, untested. Several more
   previously-undocumented characteristics were discovered (`0x0018`, `0x0022`, `0x0026`, `0x002A`,
   `0x002C`, `0x002E`, `0x0032`) — unexplored, flagged as a lead.
2. **Every one of the six variants — including the plain control — produced a real, independent,
   40-byte data block on `0x000E` (offsets 14-54) that was never present in any earlier capture**:
   smoothly drifting, non-random, absent from baseline, independent of buttons/sticks, structurally
   self-describing (a length-prefix byte reading exactly 40), matching the reference tool's
   documented "Pro/GCN 40-byte motion block" length for the first time on real hardware. **This is
   the first independent, motion-consistent data this project has ever observed from a genuine
   Switch 2 controller.** Because *every* variant succeeded — including the one with none of the
   six tested differences — **none of the deliberately-varied dimensions (mask value, calibration
   reads, the handle write, subscribe-timing) was the actual cause**; something outside the tested
   matrix differs between this session (100% activation) and the prior standalone v1 session (0%
   activation using byte-identical commands). Root cause unresolved; no orientation/gyro/
   accelerometer semantics assigned to any byte yet — see the full report for the ranked
   hypotheses and the proposed next experiment.

**Strategic context:** after two report-0x09 hardware passes, a debug-instrumentation fix, and a
mathematically-derived local-anomaly detector (`docs/experiments/gyro-hardware-validation-2026-07-10.md`
§7-14) that reported `anom=0` while the console still showed abrupt camera jumps, the local
computation is no longer the leading suspect. This document reframes the investigation: from
"fix our synthesized report-0x09 encoder" to "systematically observe what the genuine controller
actually sends," starting with the BLE transport this project already has a live host connection
to (unlike the console-only USB report 0x09, which needs new capture hardware — §4).

---

## 1. The blind spot this closes

`switch2_ble_process_report()` (`src/bt_hid/bt/bthid/devices/vendors/nintendo/switch2_ble.c`)
receives a complete, already-decrypted 63-64 byte BLE input report and reads only:
- bytes 0-15 (an "Unknown" 4-byte header, then 4 bytes buttons, then 6 bytes of packed stick
  axes — see the function's own layout comment),
- bytes 60-61 (GameCube trigger bytes, GC controller only).

Bytes 16-59 (44 bytes) are received, sit in the callback's `data`/`len` arguments, and are never
read by anything — not logged, not stored, not passed anywhere. Per
`docs/experiments/switch2_native_motion_map_DyCOOL.md` (a third-party decode of this same report
family), this is exactly the byte range where motion data lives. The BT connection/pairing
machinery in `src/bt_hid/bt/btstack/btstack_host.c` has the same property one layer up: only one
GATT input-notification handle (`SW2_INPUT_REPORT_HANDLE = 0x000A`) is subscribed to at all, and
`switch2_ack_notification_handler()`'s own pre-existing debug log implies it sometimes sees
notifications on handles other than the one it filters for (0x001A) — meaning some traffic may
already be arriving and being silently dropped by a handle check, not just an unread byte range.

**This pass does not fix parsing.** It makes every byte of every notification/command/state
transition this code already touches inspectable, unmodified, with a timestamp — see §2.

---

## 2. Raw-capture facility (implemented, hardware-validated 2026-07-10)

### 2.1 What it captures

New module `src/bt_hid/sw2_capture.c`/`.h`. Five event kinds, each with an exact timestamp
(`time_us_64()`), the GATT handle involved, and the **complete, unmodified byte payload** (up to
64 bytes — the largest Switch 2 BLE payload observed anywhere in this codebase or the reference
tools, so no truncation is expected in practice; if it ever happens, the exported `orig_len` vs
`len` fields make it visible rather than silent):

| Kind | Captured at | What it is |
|---|---|---|
| `input` | `switch2_hid_notification_handler()`, before any handle filtering | Every raw input-report notification byte |
| `ack` | `switch2_ack_notification_handler()`, before the 0x001A filter | Every raw command-response/ACK notification byte, **on whatever handle it actually arrives on** |
| `cmd_out` | Immediately before each `gatt_client_write_value_of_characteristic_without_response()` call in the init/pairing sequence, the player-LED command, and the input/ACK CCC completions | Every raw command byte this host sends |
| `ccc_write` | Immediately before each notification-enable (CCC) write | Which handle was told to start notifying, and with what value |
| `state` | Top of `switch2_send_init_cmd()` (covers every transition but the terminal one) + the `SW2_INIT_DONE` assignment site | The `sw2_init_state_t` value being acted on, so captured packets can be correlated to init/pairing phase |

Deliberately **not** captured: rumble/haptic writes (`switch2_send_rumble()`) — these can fire at
high frequency during active feedback and would crowd out the input/command traffic that matters
for this investigation; noted here so the omission is a documented decision, not an oversight.

### 2.2 Design guarantees

- **Off by default.** Disabled, `sw2_capture_record()` is a single `if` check — no behavior
  change from normal operation.
- **Never blocks the BT stack.** The ring buffer (256 entries, ~20 KB) is written by core1 (BT
  stack callbacks) under a `critical_section_t` (the same primitive `report.c` already uses for
  cross-core state) held only long enough to copy one entry in or out — never across CDC/USB I/O.
  A full ring drops the new entry and increments a counter rather than waiting for the consumer.
- **No interpretation.** The module stores bytes, a kind tag, and a handle — it does not decode
  anything. All semantic analysis happens offline from the exported log.

### 2.3 Pull-based drain, revised 2026-07-10 (v1 auto-push didn't work through the web UI)

The first version auto-streamed NDJSON lines to CDC unprompted, on every `config_cdc_task()`
iteration once enabled. **This produced no usable output through the web UI** — and the reason is
structural, not incidental: the UI's `sendCmd()` matches replies to requests by strict
arrival-order (a FIFO queue of pending promises, one per command sent). An unsolicited capture
line arriving between a command and its reply is indistinguishable from that reply — it either
corrupts the next queued command's result or is silently swallowed. A raw terminal *would* show
the lines, but only if a genuine controller happened to be actively generating traffic at the
time, which is easy to not have set up correctly, and there was no way to confirm the ring wasn't
silently overrunning without a separate `stat` call raced against the same stream.

**Redesigned: drain is now explicitly pulled**, one entry at a time internally
(`sw2_capture_drain_one()`), batched into a normal bounded JSON reply by a config command —
fitting the existing request/response protocol instead of fighting it.

### 2.4 Commands (config mode, CDC serial)

```
sw2cap on             start a fresh capture session (clears the ring + drop counter)
sw2cap off            stop capturing
sw2cap stat           {"capturing":bool,"dropped":N}
sw2cap drain          {"capturing":bool,"dropped":N,"entries":[...up to 16...],"more":bool}
sw2cap gattdisc on|off|stat   one-shot GATT discovery tool (§2.7.1) -- {"gattdisc":bool}
sw2cap variant <0-6>          arm a v2 experiment variant, 0=off (§2.7.2) -- {"ok":true,"variant":N}
sw2cap variant stat           {"variant":N}
sw2cap mark <text>            insert a timestamped annotation label (§3.9) -- {"ok":true,"marked":"..."}
```
Each entry: `{"us":<time_us_64>,"kind":"input"|"ack"|"cmd_out"|"ccc_write"|"state"|"gatt_svc"|
"gatt_char"|"gatt_desc"|"variant"|"write_status"|"marker","handle":"0x0000","len":N,"orig_len":N,
"bytes":"<hex>"}`. `write_status` (added §3.9) captures the ATT completion status of a CCC or
descriptor write — previously only reached printf(), see §3.9 for the fix. `marker` (added §3.9)
carries raw ASCII bytes of a user-supplied label, not a protocol event.

### 2.5 Web UI panel (config page, "Switch 2 BLE Capture" section)

The primary way to use this facility. Polls `sw2cap drain` on every tick of the config page's
existing live-status loop (~70 ms, alongside the controller-mapping/IMU-debug polling it already
does) — continuously, not just while a session is active, so the panel reflects `capturing`/
`dropped` state even if a session was started by something else (e.g. a raw terminal) or left
running from a previous page load.

- **Start / Stop / Clear / Download NDJSON** buttons. Start/Stop show the firmware's `ok`/`error`
  reply directly in the page log. Clear resets the browser-side view only (the firmware ring is
  independent and keeps running if a session is active).
- **Live status pill**, captured/dropped counts, and a count per event kind (`input`/`ack`/
  `cmd_out`/`ccc_write`/`state`/`gatt_svc`/`gatt_char`/`gatt_desc`/`variant`) — all computed from
  what's actually been drained into the browser, updated every poll.
- **v2 experiment variant selector** (§2.7.2) — a dropdown (off / 1-6, each named) that arms one
  variant at a time via `sw2cap variant <n>`, with a status pill showing which is armed. **GATT
  discovery toggle** (§2.7.1), separate from the variant selector, with its own status pill and an
  inline note not to run it together with a variant.
- **Observed GATT handles and report lengths** — tallied client-side from drained entries (no
  extra firmware-side tracking needed; the browser already sees every captured entry).
- **Bounded live view** (most recent 60 entries) in the page, separate from **full-session
  retention** in a plain JS array (no cap) used for NDJSON export — so a multi-minute session
  doesn't grow the visible DOM without bound while still producing a complete export.
- **Filters** (kind — including the four new GATT-discovery/variant kinds — and GATT handle, with
  one-click `0x000A`/`0x000E`/`0x000C` buttons, the last added for jumping straight to the
  report-rate descriptor hypothesis) affect only the live view's rendering, never what's retained
  for export.
- For `input` entries, the raw hex is shown with byte offsets 16-29 visually set apart — **a
  viewing convenience only**, explicitly labeled as such in the UI (not a decoded field, not a
  claim about this report's semantics); it exists because that's the range this report's own
  `process_report()` never reads (§1) and where a *sibling* BLE report's third-party decode places
  motion, not because anything here has confirmed what these bytes mean.
- **NDJSON download** re-serializes the full retained session (`JSON.stringify` per entry,
  newline-joined) via a `Blob` + object-URL download — value-preserving (every field survives the
  parse this page already had to do to render the live view), not a raw-byte-identical copy of
  the wire text.

**Validated 2026-07-10:** four sessions captured through this exact panel
(`dumps/sw2_capture_2026-07-10-{STILL_CAPTURE,ANGLE1,ANGLE2,ANGLE3}.ndjson`, 6196/6159/6007/5167
records, `dropped=0` reported throughout every session). This confirms the pull-based redesign
(§2.3) works end-to-end through the browser, closing the loop on the earlier "`sw2cap on` produced
no visible output" failure.

### 2.6 v1 motion-enable experiment (implemented + run 2026-07-10 — superseded by §2.7, kept for history)

Triggered by §3.5's finding that this connection never receives orientation-responsive data on
`0x000A` and by the fact (confirmed, §3.1) that this repo's own init sequence never sends a
feature-configure/enable command at all — unlike `switch2_input_viewer.py` (§3.2), which does. Ran
once (§3.6): confirmed `0x000E` is reachable and accepts a `configure(0x07)`/`enable(0x07)` pair,
but the resulting report is a byte-shifted duplicate of `0x000A`'s buttons+sticks payload — no
orientation data. This single fixed sequence has been **replaced** by §2.7's variant matrix
(the v1 sequence is now exactly variant 1, "control") — the code described below no longer exists
standalone in `btstack_host.c`; this section is retained as an accurate record of what ran and
what it found, per CLAUDE.md's "documentation over conversation history" principle.

- Command was `sw2cap experiment on|off|stat`. Web UI toggle was "Experimental: attempt
  motion-enable (handle 0x000E)". Both have been replaced by §2.7's variant selector.
- Behavior: subscribed `SW2_MOTION_HANDLE = 0x000E` + `SW2_MOTION_CCC_HANDLE = 0x000F`, then sent
  a `0x0C`-family configure (subcmd `0x02`) then enable (subcmd `0x04`) command pair, both with
  `flags=0x07` (buttons|sticks|IMU), mirroring `switch2_input_viewer.py`'s
  `configure_features()`/`enable_features()` shape (§3.2) — fired once per connection, after
  `SW2_INIT_DONE`, all traffic captured via the existing `sw2_capture_record()` pipeline.
- Result: §3.6. Motivated §2.7's three-variable breakdown of what the reference tool's working
  sequence does differently.

### 2.7 v2 feature-enable experiment matrix + GATT discovery tool (implemented 2026-07-10, not yet run on hardware)

Built in response to §3.6's result: rather than guess which of three untested differences (mask
value, calibration reads, an extra descriptor write) matters, each is isolated as its own one-shot,
independently-selectable variant, so a positive result is attributable to a specific cause instead
of one opaque combined change. Implemented in `src/bt_hid/sw2_capture.h` (public API),
`src/bt_hid/bt/btstack/btstack_host.c` (the discovery + variant state machines), and wired into
`config.c`/the web panel.

#### 2.7.1 GATT discovery tool

Before trusting *any* handle beyond this repo's own confirmed set (§3.1) or the reference tool's
documented value handles (§3.2), §3.7.1 below shows the "write report rate" handle used by
variants 3/4/6 rests on an ambiguous derivation (0x000C vs 0x000D, resolved on paper to 0x000C but
not independently confirmed). Rather than add a fourth guess, this tool walks BTstack's own live
GATT discovery on the current connection — every primary service, then every characteristic in it
(declaration handle, value handle, properties, UUID), then every descriptor of each characteristic
(handle, UUID) — and captures each result via the existing pipeline (new kinds `gatt_svc`/
`gatt_char`/`gatt_desc`). This is ground truth from this exact device's own GATT server, not
arithmetic on a third-party tool's bleak-indexed numbers.

- **Command:** `sw2cap gattdisc on|off|stat`. Web UI: an unchecked-by-default "GATT discovery"
  checkbox, separate from the variant selector.
- **One-shot**, fires once per connection after `SW2_INIT_DONE`, same trigger point as the v2
  variants. **Do not arm together with a variant** — BTstack allows only one outstanding GATT
  query per connection at a time, so the two would contend; run discovery alone first.
- Every service/characteristic/descriptor is captured unconditionally (not filtered to handles
  this repo already cares about) — the standard GAP/GATT/DIS/Battery services a real BLE device
  usually exposes come along too, which is useful cross-validation, not noise to filter out.

#### 2.7.2 v2 variant matrix

All six variants target the same `0x000E`/`0x000F` pair (§2.6); they differ only in what's sent
before/after/around the configure+enable commands. `sw2cap variant <n>` arms variant `n` (0 = off)
for the next connection's one-shot firing; `sw2cap variant stat` reports which is armed. Exactly
one variant is armed at a time; each fires once per connection (guard resets on disconnect), so a
fresh reconnect/power-cycle is required between attempts — including to re-run the same variant.

| # | Name | Configure flags | Enable flags | Calibration reads | Handle write | CCC subscribe timing | Isolates |
|---|---|---|---|---|---|---|---|
| 1 | `control` | 0x07 | 0x07 | no | no | first (v1 order) | Reproduces v1 exactly — the baseline every other variant is compared against |
| 2 | `mask_ff` | 0xFF | 0xFF | no | no | first | Whether the flags *value* alone (0xFF vs 0x07) matters |
| 3 | `handle_write_only` | 0x07 | 0x07 | no | yes | first | Whether the descriptor write alone matters, independent of flags |
| 4 | `mask_ff_handle_write` | 0xFF | 0xFF | no | yes | first | Whether mask+write together suffice, without calibration |
| 5 | `calibration_seq` | 0x07 | 0x07 | yes | no | first | Whether having read calibration data matters, independent of flags/handle-write |
| 6 | `full_sequence` | 0xFF | 0x07 | yes | yes | **last** (deferred) | The complete reference-tool sequence, including subscribing to 0x000E *last* — not just its command bytes but its actual operation order |

Notes on the design, not just the table:

- **Variant 6's deferred CCC subscribe is a distinct, previously-unnoticed variable.**
  `switch2_input_viewer.py`'s own connection flow doesn't call `start_notify()` for input reports
  until *after* configure, all six calibration reads, enable, and the handle write are already
  done (`tools/switch2_input_viewer.py` lines ~1240-1340) — meaning the reference tool's proven
  sequence subscribes to notifications *last*, right before first use, not first. Variants 1-5 all
  keep v1's original "subscribe first" order for simplicity and to isolate one variable at a time;
  only variant 6 tests the ordering itself, exactly as this task's instructions singled out
  "exact ordering of subscriptions" for the full-sequence variant specifically.
- **The six calibration reads** (variants 5 and 6) are issued in `switch2_input_viewer.py`'s exact
  order and byte format (`read_spi_memory(address, size)`, reproduced verbatim in
  `switch2_build_spi_read_cmd()`): primary stick cal (`0x13080`/`0x40`), secondary stick cal
  (`0x130C0`/`0x40`), user cal (`0x1FC040`/`0x40`), gyro cal (`0x13040`/`0x10`), accel/mag cal
  (`0x13100`/`0x18`), pairing data (`0x1FA000`/`0x40`) — sent sequentially, each waiting for its
  own ACK before the next is sent (matching the reference tool's `await`-per-read behavior; also
  the only practical way to know *which* cal read's ACK just arrived, since all six share the same
  cmd/subcmd bytes and are distinguished only by send order).
- **No variant was found protocol-invalid.** Every variant nests on top of the same minimum
  skeleton (subscribe + configure + enable, in some order) that v1 already proved works; none
  requires an ordering DATA.md's task flagged as a possible dependency risk that this design
  doesn't already account for (the one real ordering dependency found — subscribe-timing — became
  variant 6's defining feature rather than an obstacle).
- **Logging**, per variant, satisfies the task's list without new capture machinery: a
  `SW2_CAP_VARIANT` entry marks which variant started; every CCC write, command byte, and ACK is
  already captured with an exact timestamp via the existing pipeline, so "complete ordered
  operation sequence," "exact outgoing bytes," "ACK/reply bytes," and "timing between operations"
  are all directly readable from a session's exported NDJSON without extra decoding. "Notification
  handles and lengths before and after each step" and "first appearance of any new or changing
  report region" are likewise derivable by comparing entries' timestamps against each variant's
  own logged steps — no separate before/after bookkeeping was added, since the full session is
  already a complete, timestamped record.

---

## 3. Field-level inventory

Confidence tiers, applied strictly: **Confirmed** = directly demonstrated by this repo's own
code behavior or a capture we hold. **Strong Evidence** = a specific, checkable claim from a
named third-party source (code we can read, not just an assertion). **Hypothesis** = plausible,
not yet checked. **Unknown** = no basis yet either way.

### 3.1 Confirmed (this repo's own code/behavior)

- GATT handle map this host actually uses: input notify `0x000A` (CCC `0x000B`), output/rumble
  `0x0012`, command `0x0014`, ACK notify `0x001A` (CCC `0x001B`). (`btstack_host.c` `#define`s.)
- Init/pairing is a fixed sequence: `READ_INFO` (SPI read, addr encoded as `0x00,0x30,0x01,0x00`)
  → `PAIR_STEP1` (send local BD address, and BD address − 1, for reasons not documented in our
  own code) → `PAIR_STEP2`/`PAIR_STEP3` (fixed 16-byte "magic" payloads, sourced from "BlueRetro"
  per a code comment — **not independently derived by this repo**) → `PAIR_STEP4` (fixed
  9-byte completion) → `SET_LED` → `DONE`.
- **This host never sends a feature-configure/enable command of any kind during BLE init** —
  confirmed by exhaustive search (`grep`) for `0x0C`-family bytes in the entire switch2 BLE
  section of `btstack_host.c`: none exist. Compare §3.2.
- `switch2_ble_process_report()` reads only report bytes 0-15 and (GC only) 60-61; bytes 16-59
  are received but never read anywhere in this codebase (§1).
- Report length gating: reports shorter than 16 bytes are dropped with no capture; the
  `SW2_INPUT_REPORT_HANDLE` notification path additionally requires `value_length <= 64`.
- The genuine controller's own SPI-backed factory calibration (`0x13040` temp+gyro-bias,
  `0x13100` mag-bias+accel-bias, both `float32`) is independently confirmed by decoding this
  project's own SPI dump with a third-party tool's field offsets — see
  `docs/experiments/spi-dump-analysis-2026-07-10.md` and `report-0x09-motion.md`'s "Factory
  motion calibration" section. Not a BLE-protocol fact, but a genuine-hardware fact this repo
  holds primary evidence for.

### 3.2 Strong evidence (named third-party source, specific and checkable)

- **`tools/switch2_input_viewer.py`** (a working PyQt/bleak BLE client, added to this repo,
  confirmed to correctly decode this project's own SPI dump — see `report-0x09-motion.md`) shows:
  - A **second parallel handle triple** this repo's code never uses: input `0x000E`, command
    `0x0016`, response `0x001E` — alongside the primary triple (`0x000A`/`0x0014`/`0x001A`) this
    repo's code does use. The tool's own comment ("all the handles are one less than these in
    bleak") is a bleak-library indexing quirk, not evidence the raw ATT numbers differ from what
    this repo's code already uses for the primary triple — the primary-triple numbers match
    exactly between the two independent implementations, which is itself a meaningful
    cross-check of the fixed handle table both assume.
  - Handle `0x0016` (the secondary command handle) requires a **33-byte zero prefix** before the
    actual command bytes — different framing from `0x0014`'s direct framing. Not exercised or
    replicated by this repo's code.
  - The **"Common" format (handle `0x000A`) carries a 14-byte motion block** per report (that
    tool's own commented-out dtype: `temperature, accel_x/y/z, gyro_x/y/z`, all `int16` — one raw
    sample, not an accumulator) — i.e. the exact handle this repo already subscribes to already
    carries *some* motion data in the bytes this repo currently discards.
  - The **"Pro/GCN" format (handle `0x000E`) carries a 40-byte motion block**, richer than the
    14-byte one, structure undecoded even by that tool's own author.
  - That tool's own init sequence **does** send feature-configure/enable commands
    (`configure_features(0xFF)` then `enable_features(flags)`, default `flags = 0x03` — buttons +
    sticks only, bit 2 = IMU not set unless the UI's IMU checkbox is checked) as part of getting
    a working session — a step this repo's own init sequence skips entirely (§3.1).
- **`docs/experiments/switch2_native_motion_map_DyCOOL.md`** (bit-exact decode, cross-correlated
  against a reference sensor and gravity integration) describes the "Pro/GCN" 40-byte format's
  internal bit-packed layout in detail (multi-sample accel/gyro, ±500°/s clamp, on-chip bias
  correction) — but its own confidence table explicitly marks **Pro Controller 2's report 0x09
  (USB) "TO BE VERIFIED... likely a variant,"** and by extension does not claim the BLE decode
  transfers to what this repo's already-subscribed `0x000A` handle sends (which per the *other*
  third-party source above is a *different*, 14-byte format, not the 40-byte one this document
  decodes).
- **`ndeadly/switch2_controller_research`** (referenced throughout this repo's existing docs,
  e.g. `usb-spec.md`, `report-0x09-motion-analysis.md`) is the source for report-0x09's USB byte
  layout and the console-side `0x0C` feature-negotiation model — a **different transport**
  (console USB) from everything else in this document (genuine-controller BLE). Treat as a lead
  for what feature negotiation *might* look like on BLE by analogy, not as BLE-specific evidence.

### 3.3 Hypothesis (plausible, not yet checked against a capture)

- The richer 40-byte format on `0x000E` may require an explicit subscription (CCC write) this
  host never performs, meaning it may never arrive regardless of what the device is capable of —
  or the device may not send it unsolicited even if subscribed, requiring the same kind of
  feature-enable command §3.2 shows the reference tool sending. **Now the subject of an
  implemented, off-by-default experiment (§2.6) — not yet run.**
- The pairing "magic bytes" (`PAIR_STEP2`/`STEP3` fixed payloads) may be per-model constants
  (safe to hardcode, as this repo already does) or could be per-unit/session-derived on genuine
  hardware in ways that happen to work with fixed values in practice but aren't actually
  supposed to be fixed. Not evaluated this pass.

### 3.4 Unknown

- Whether report-0x09-style "angular phase" semantics (accumulator) or DyCOOL's "raw clamped
  sample" semantics (or something else again) describes what `0x000E`'s 40-byte block actually
  contains for a Pro Controller 2 specifically, **if** it turns out to be reachable at all
  (§2.6/§3.5) — genuinely unknown pending that experiment's result. Do not adopt either model as
  fact. (Resolved for `0x000A`: see §3.5 — this handle carries no such block on this connection.)
- Whether the abrupt camera jumps observed on real console hardware (report 0x09, USB) share any
  root cause with anything observable over BLE at all — BLE and USB are different transports
  carrying different report formats (§4). A BLE capture can inform hypotheses about the *general*
  sensor/encoding model but cannot, by itself, prove or disprove anything about report 0x09's
  specific bytes.
- Whether the ACK-handler's "notification on a handle other than 0x001A" debug log (§1) has ever
  actually fired, and if so what handle it saw — not answered by the four §3.5 captures, since
  those only recorded `kind=input` on `0x000A` (no `ack`/`cmd_out`/`ccc_write`/`state` entries
  appear in any of the four files — see §3.5's integrity notes).

### 3.5 Confirmed via captured data (2026-07-10) — four full sessions, `dropped=0`

Analysis performed programmatically over the complete files (not sampled lines) by
`tools/analyze_sw2_capture.py` and `tools/analyze_sw2_fields.py` (both read-only, both reusable —
rerun them against any future capture). Source data:
`dumps/sw2_capture_2026-07-10-{STILL_CAPTURE,ANGLE1,ANGLE2,ANGLE3}.ndjson`. Per the task that
produced these, exact physical axes/signs are **not** inferred from the filenames — only relative
"changed vs. did not change" comparisons are drawn.

**Integrity (all four files):** 0 JSON parse errors, 0 exact duplicate records, 0 repeated-payload
input records, 0 non-monotonic timestamps. Every record in every file is `kind=input,
handle=0x000A, len=orig_len=63`, at a steady ~33.3 Hz (median inter-record delta ≈30 ms). No
`ack`/`cmd_out`/`ccc_write`/`state` records appear anywhere — these captures only ran during
already-established connections, not across a fresh init (see §5 experiment 9, still open). Record
counts/durations: STILL_CAPTURE 6196 records/186.09 s, ANGLE1 6159/184.74 s, ANGLE2 6007/180.36 s,
ANGLE3 5167/155.01 s.

**Byte-level, all four files agree:** of 63 offsets, exactly **8 ever vary at all**
(`{0,1,2,10,11,13,14,31}`); the remaining **55 are constant zero in every single record of every
file**. Accounting for all 8:

| Offset(s) | Behavior | Interpretation |
|---|---|---|
| 0-1 | LE u16, near-full entropy, +30/record steady, wraps every ~2184 records | Free-running report/tick counter — consistent with the ~33 Hz cadence itself (`65536/30 ≈ 2185` records per wrap, matches observed wrap spacing) |
| 2 | 4 distinct values per file, but the *range itself* shifts monotonically across the whole ~9-minute session (STILL 1-4 → ANGLE1 4-7 → ANGLE2 8-11 → ANGLE3 11-14, captured in that chronological order) | A slow counter advancing with **session time, not orientation** — the shift tracks capture order, not which file is which orientation |
| 10-11, 13-14 | Two 12-bit little-endian-nibble-packed pairs (the reference tool's stick1/stick2 unpack, `unpack_12bit_triplet`) | **Analog stick X/Y**, not motion: all four files decode to near-mid-scale values (stick1 X/Y ≈2018/2189, stick2 X/Y ≈2145/2084 out of a 4096 range, i.e. within ~3% of center) and are **near-identical between STILL_CAPTURE and ANGLE1/ANGLE2** (means match to within 0.1 unit) despite those being different physical orientations — a stick reads the same regardless of controller orientation unless someone is pressing it, which is exactly what's observed. ANGLE3 shows a small (~2-5%) shift, consistent with incidental hand pressure while holding a tilted controller, not an orientation-encoding field. (A competing raw-int16-LE hypothesis over the same bytes was also tested and produces values that look like uninterpretable noise by comparison — e.g. sign flips between files with no session-time or orientation pattern — weakening it relative to the 12-bit-stick reading.) |
| 31 | 3 distinct values per file, also shifting monotonically with **session time** (STILL 84-86 → ANGLE1 83-85 → ANGLE2 82-84 → ANGLE3 81-83) | Same slow-counter signature as offset 2 — moves with elapsed time across the whole session, not with which orientation was held |

**Alignment-shift test** (±1 byte framing shift applied to all 63 bytes, checking whether a
shifted view reveals non-zero data hidden in the 16-59 zero region): identical result in **all
four files** — shift −1 exposes {16,32,33,42}, shift 0 exposes {31,32,41}, shift +1 exposes
{30,31,40}, and in every case the exposed offsets are the *same* few positions regardless of which
physical orientation was captured (they're bleed from the always-varying offsets above, not new
information). **No framing shift reveals orientation-responsive data anywhere in bytes 16-59.**
This directly closes the concern (raised as an explicit warning for this task) that a "no motion"
conclusion might rest on an unverified offset convention: the convention was tested, not assumed,
and shifting it does not change the outcome.

**Conclusion against the four hypotheses posed for this task:**
1. *"Motion is already present in 0x000A at a different offset/encoding"* — **refuted**. All 63
   bytes are accounted for as counters, sticks, or constant zero; none show orientation-responsive
   behavior under the tested offset, an inverted 12-bit read, or ±1-byte reframing.
2. *"Motion fields exist but remain constant because the feature wasn't enabled"* and
   3. *"These notifications contain no live motion data (on this report)"* — **both remain
   consistent with the data and cannot be distinguished by passive observation alone**, since this
   repo's own init sequence never sends the feature-enable command the reference tool sends
   (§3.1) — a genuinely untested condition, not evidence either way.
4. *"Motion is on the unused `0x000E` path or another report variant"* — **plausible, untested**,
   and the best-supported next step given (2) and (3)'s ambiguity and (1)'s refutation — motivated
   the §2.6 experiment. **Now run once, negative result — see §3.6.**

### 3.6 §2.6 experiment run: `0x000E` reachable, still no orientation data (2026-07-10)

Source: `dumps/sw2_capture_2026-07-10-EXPERIMENTAL.ndjson`, one session, `sw2cap experiment on`
armed then the controller power-cycled per §8's procedure. 2,371 records, 84.46 s, **all
non-`input` kinds present for the first time** (unlike the four earlier files, which only ever
captured `kind=input` — see §3.5's integrity notes): `ccc_write`×4, `state`×7, `cmd_out`×8,
`ack`×8, `input`×2344 (13 on `0x000A`, 2331 on `0x000E`).

**Confirmed by this capture (previously Hypothesis/Unknown):**
- **`0x000E` is reachable and does carry live notification traffic once the experiment's
  CCC-write + `configure`/`enable` sequence runs** — a real, continuous ~33 Hz stream (2,331
  records over 70.8 s) began arriving on a handle that produced nothing in any of the four earlier
  captures. This was §3.3's leading open hypothesis; it's no longer merely hypothetical.
- **The `0x0C`-family `configure`(subcmd `0x02`)/`enable`(subcmd `0x04`) command pair is accepted
  by this device over BLE** — both were ACK'd on `0x001A` with the same success-shaped response
  pattern (leading `0x01` status byte) every other successful init/pairing ACK in this repo's
  captures shows. §3.1's confirmed gap ("this repo never sends this command") is now paired with
  direct confirmation that when it *is* sent, the device responds normally, not with an error.
- **`0x000F`'s CCC write succeeded implicitly** (notifications began arriving right after) —
  strengthens §2.6's `handle+1` CCC guess for `0x000E` from ASSUMED toward Confirmed-in-practice,
  though still not independently verified via a GATT discovery dump.
- **`0x000E`'s report, once flowing, is 63 bytes — not the reference tool's documented 40-byte
  "Pro/GCN" length** — and byte-for-byte, it's a **shifted duplicate of `0x000A`'s buttons+sticks
  payload**: the same 12-bit-packed stick1/stick2 pair (`0x000A` offsets 10-15 == `0x000E` offsets
  5-10, confirmed byte-identical across sampled records) sits 5 bytes earlier, framed by an 8-bit
  per-notification sequence counter (offset 0, +1/record — a *different* counter shape from
  `0x000A`'s 16-bit, +30/record one), two small near-zero bytes (offsets 2-3, low entropy, meaning
  unknown — too small a range to be a sensor sample), and constant tag bytes at offsets 1 (`0x20`)
  and 11 (`0x30`). **Offsets 12-62 are constant zero across all 2,331 records** — the same
  "nothing here" result as `0x000A`, now demonstrated on a second, previously-unreachable path.

**Refuted:** "reachability is the only blocker — once `0x000E` is subscribed and features are
enabled with a plausible flags value, orientation data will appear." It doesn't, at least not with
exactly the flags/command bytes this experiment sent.

**New lead for a v2 experiment**, found by diffing this experiment's command sequence against the
reference tool's actual working init flow (`tools/switch2_input_viewer.py` lines ~1286-1326,
between its own `configure_features`/`enable_features` calls) — three concrete differences, any of
which could plausibly be the real trigger:
1. The reference tool calls **`configure_features(0xFF)`** (all 8 flag bits) — this experiment
   used `0x07` for *both* configure and enable. `configure` may be a capability-declaration step
   that needs the full mask regardless of what gets streamed, with `enable`'s smaller mask being
   the actual on/off switch — untested here.
2. The reference tool issues **six SPI calibration reads between `configure` and `enable`**:
   primary/secondary stick cal (`0x13080`, `0x130C0`), user cal (`0x1FC040`), gyro cal (`0x13040`),
   accel/mag cal (`0x13100`), and pairing data (`0x1FA000`) — none of which this experiment
   performed. If the device's firmware expects its calibration state read before it starts
   streaming a calibrated IMU sample, skipping this could plausibly explain silence.
3. **A previously-undocumented handle**: the reference tool writes bytes `85 00` to
   bleak-indexed `input_handle+3` *after* `enable_features`, labeled "report rate" in that tool's
   own comment. This handle does not appear anywhere else in this document's confirmed handle map
   (§3.1) — genuinely new. Not written by this experiment at all. **Correction (2026-07-10,
   revisited before v2 implementation): an earlier version of this section resolved this to raw
   ATT handle `0x000D` via a single arithmetic pass. That derivation double-applied bleak's
   value-handle offset to a descriptor operation, which isn't subject to the same offset a
   characteristic value/declaration split is. Re-derived properly in §3.7.1: the best-supported
   raw handle is actually `0x000C`, not `0x000D` — and it's most likely a descriptor on the
   `0x000A` characteristic, not `0x000E`. Left uncorrected until now specifically so this mistake
   — and the fact that it went unnoticed until asked to "resolve handle numbering exactly" — is
   visible as an example of why guessed offsets need independent verification (§3.7.1's GATT
   discovery tool), not just a second round of arithmetic.**

None of these three were exercised this pass — this experiment deliberately sent only the
smallest, most literally evidence-supported command (the exact `configure`/`enable` byte shape),
per the task that specified it. The result narrows what "evidence-supported" should mean for a v2
attempt: replicate the reference tool's *full* sequence (order, `0xFF` configure value, cal reads,
and the descriptor write), not just its two feature-flag commands in isolation — done in §2.7/§3.7.

### 3.7 Handle-numbering resolution and the v2 experiment design (2026-07-10)

Before writing any v2 protocol code, this pass re-derived the raw ATT handle §3.6 item 3 targets,
since acting on a wrong handle would produce a meaningless result (a write to the wrong attribute,
or an ATT error mistaken for "the real handle rejects this").

#### 3.7.1 Bleak-vs-raw handle numbering — reasoning, not yet independently confirmed

`switch2_input_viewer.py` consistently computes `bleak_handle = documented_value_handle - 1` for
every characteristic it uses — not just as a general comment, but demonstrated at three
independent call sites: `self.input_handle = INPUT_HANDLES[0] - 1` (`INPUT_HANDLES = [0x000A,
0x000E]`), `self.command_handle = COMMAND_HANDLES[0] - 1` (`COMMAND_HANDLES = [0x0014, 0x0016]`),
`self.command_response_handle = COMMAND_RESPONSE_HANDLES[0] - 1` (`COMMAND_RESPONSE_HANDLES =
[0x001A, 0x001E]`), and an inline literal `write_gatt_char(0x0005 - 1, data, True)`. A later check,
`if self.command_response_handle+1 == 0x001E`, confirms the inverse holds too (bleak + 1 =
documented). All of this is internally consistent with **this repo's own three independently
confirmed value handles** (`0x000A`, `0x0014`, `0x001A` — `btstack_host.c` `#define`s, exercised
successfully on real hardware) and their secondary-triple counterparts — a real cross-check, not
just trusting the comment.

Per the GATT specification, a characteristic's **declaration** attribute is always exactly one
handle *before* its **value** attribute (this is structural, not implementation-specific — the
value is always the next allocated handle after its declaration). `bleak_handle = value - 1` is
therefore consistent with bleak reporting the **declaration handle**, not the value handle, as
`characteristic.handle` — plausible on some bleak backends, and not something this repo needs to
fully explain, only to correctly account for.

Applying this to the known layout: `0x0009 = decl(0x000A)`, `0x000A = value` (**confirmed**,
`SW2_INPUT_REPORT_HANDLE`), `0x000B = CCC` (**confirmed**, `SW2_CCC_HANDLE`). The v1 experiment
(§2.6/§3.6) independently **confirmed** `0x000E = value` and `0x000F = CCC` work (real
notifications arrived after subscribing there). Since a characteristic's value is always exactly
one past its own declaration, `0x000E`'s declaration **must** be `0x000D` — which leaves `0x000C`
as the only unassigned handle between `0x000A`'s CCC (`0x000B`) and `0x000E`'s declaration
(`0x000D`). The reference tool's write target, `input_handle(0x0009) + 3`, lands on `0x000C` —
**if** descriptor-handle arguments to bleak's `write_gatt_descriptor()` are raw/unadjusted (no
further `-1`), which is the simpler and better-supported reading, since only *characteristics*
have the declaration/value split that motivates the `-1` rule in the first place; a descriptor is
a single attribute with no such split.

**Historical conclusion (now refuted): `0x000C` was the best-supported estimate**, correcting §3.6's earlier `0x000D`
guess — and it most likely names a **third descriptor on the `0x000A` characteristic** (report
rate for the *base* "Common" format), not anything belonging to `0x000E`. If that's right, this
write may be unrelated to unlocking `0x000E`'s content at all. **This entire derivation is
reasoning on paper, not an independent measurement** — it has not been checked against this
device's actual GATT table. §2.7.1's GATT discovery tool exists specifically to replace this
paragraph with ground truth; until a discovery capture exists, `SW2_REPORT_RATE_HANDLE_HYPOTHESIS`
(`btstack_host.c`) is labeled exactly that — a hypothesis.

#### 3.7.2 Why a six-way matrix instead of one combined v2 attempt

§3.6 identified three untested differences between v1 and the reference tool's working sequence.
Combining all three into one "v2" attempt would answer *whether* something works but not *why* —
if it did work, there'd be no way to tell whether the mask, the calibration reads, the handle
write, or the subscribe-timing was the actual cause (or some combination). The task that requested
this explicitly ruled that out ("do not replicate all three differences as one opaque sequence"),
so §2.7.2's matrix isolates each variable, with variant 6 as a positive-control replication of the
complete reference sequence (including the subscribe-timing difference found while re-reading the
reference tool's code for this task, not previously documented) to check against once the isolated
variants are in.

### 3.8 v2 hardware run: independent motion-consistent data confirmed on all six variants (2026-07-10)

Full report: `docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md` (methods, per-variant
causal table, hypothesis assessment, ranked unresolved causes, proposed follow-up). Summary of the
confidence-qualified findings:

- **✅ Confirmed, from live GATT discovery** (`dumps/sw2_capture_2026-07-10_NO-VARIANT-GATT-DISCOVERY.ndjson`):
  `0x000C` is a vendor-specific descriptor (UUID `679d5510-5a24-4dee-9557-95df80486ecb`) of the
  `0x000A` characteristic — not `0x000E`, and not `0x000D`. This corrects §3.7.1's paper-derived
  estimate with real hardware ground truth (the estimate itself, made *before* seeing hardware
  data, turned out correct on the handle number; the doubly-superseded original `0x000D` guess
  from §3.6 remains wrong). `0x000E` has its own equivalent descriptor at `0x0010`, not yet tested.
  Several more previously undocumented characteristics exist on this device
  (`0x0018`/`0x0022`/`0x0026`/`0x002A`/`0x002C`/`0x002E`/`0x0032`) — see the full report §3.3 for
  the complete table; none explored this pass.
- **✅ Confirmed, all six variants**: every variant's `cmd_out`/`ack`/`ccc_write` sequence matches
  its design exactly — no missing, duplicated, rejected, or reordered operations, all ACKs
  success-shaped. One instrumentation gap found (not a rejection): the descriptor-write and
  CCC-write *completion* callbacks never call `sw2_capture_record()`, so the `0x000C` write's own
  ATT-level accept/reject status is not recoverable from these captures — only its downstream
  effect is observable.
- **🔵 Strong evidence, all six variants**: `0x000E`'s report gained a **40-byte active data block**
  at offsets 14-54 — absent from every earlier capture (0/2,331 records in the original v1 run),
  independent of buttons/sticks (`0x000A` stays at its usual 7-8 varying offsets throughout),
  structurally self-describing (offset 14 is a constant length-prefix byte reading exactly 40 —
  matching the reference tool's documented "Pro/GCN 40-byte motion block" length), and behaviorally
  distinct from every counter/duplicate/ACK pattern already characterized in this report family
  (smooth monotonic drift with byte-boundary transition-rate signatures consistent with a genuine
  accumulating value, not noise). **No orientation/gyro/accelerometer semantics are assigned** —
  see the full report §3.4/§7 for exactly what is and isn't established.
- **Causal attribution — the central open question**: because *every* variant succeeded, including
  the plain control (identical to the exact commands that produced *zero* activity in the prior
  standalone session), **none of the six deliberately-tested differences (mask value, calibration
  reads, the handle write, subscribe-timing) is the actual cause**. The real differentiator between
  "prior session: never activates" and "this session: activates on all six independent connections"
  lies outside the tested matrix. Ranked hypotheses and the proposed single follow-up experiment
  (a stationary-only re-run of variant 1, to test whether the drift persists without physical
  motion) are in the full report §6 — not implemented this pass.

This changes §3.3/§3.4's tables' bottom line for `0x000E` specifically: hypothesis 4 from the
original four-hypothesis framing ("motion is on the unused `0x000E` path") now has **direct,
reproducible, positive hardware support** — a first for this entire investigation.

### 3.9 Continuation: block interpretation + a concrete timing-difference candidate (2026-07-10)

User clarified the follow-up §3.8 proposed doesn't need to be run: **variant 1 (and all six
variants) were already held genuinely motionless, paired and captured while stationary** — the
40-byte block's drift is confirmed present under stationary conditions, ruling out incidental
physical handling as the explanation for *this* session's result. Per explicit direction, the
active priority shifted from "what triggers the block" to "what does the block encode," with the
trigger question deprioritized but not dropped.

**Block interpretation** (new tool, `tools/analyze_sw2_motion_block.py`, exhaustive
alignment/width/endianness/signedness scan + derivative/wraparound/correlation/lane/timing
analysis across all six variants): two 2-byte fields survive as genuinely accumulator-like —
**Field A** (raw offsets `0x13`-`0x14`) wraps through the full int16 range roughly every 4 seconds
(~16,000 counts/s implied rate) — a strong structural match for an integrated angular/phase
accumulator under a small constant bias, the same architecture already established for this
project's own report-0x09 phase model. **Field B** (raw offsets `0x19`-`0x1A`) drifts smoothly
across the *entire* session with no wraparound observed, at a much slower rate. A separate,
non-drifting pair (raw `0x26`/`0x31`) shows a consistent +0.44 to +0.55 correlation across all six
variants — structurally linked, not yet explained. No interleaved/repeated-lane structure was
found. **No semantics assigned to any byte** — full ranked-candidate detail and what a controlled
motion capture would need to show to distinguish rate-like from orientation-like from
counter-like behavior: full report §8.

**A concrete, directly-measured candidate trigger difference was found** while enumerating every
difference between the prior (negative) session and this session's variant 1: the *original* v1
experiment code fired its CCC-subscribe write, `configure` command, and `enable` command within
1.2ms of each other, unconditionally — the device's `configure` ACK didn't arrive until ~39ms
*after* `enable` had already been sent. The current v2 state machine explicitly waits for the CCC
write's completion and for `configure`'s ACK before sending `enable`. This is a verified fact
about this repo's own two code versions (exact timestamps in the full report §9), not a claim
about the controller — it is the leading candidate explanation, not a confirmed one. A
one-variable follow-up experiment (deliberately reproducing the old unsequenced timing) is
proposed, not implemented.

**New capture-annotation marker mechanism** (`sw2cap mark <text>`, a new web-panel input/button)
and a **fix for a real instrumentation gap** (CCC-write/handle-write completion status is now
captured, not just printed) were implemented this pass — both pure additive logging, verified not
to change any BLE operation's timing or behavior. Both boards build clean. Full design, the
ranked block-interpretation tables, the differential comparison, the controlled-motion experiment
design (physical sequence + outcome mapping), and the exact browser procedure:
`docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md` §8-§12.

### 3.10 Controlled-motion capture analyzed: block-wide activity signature confirmed (2026-07-10)

`sw2_capture_2026-07-10_MOTION-CLASSIFICATION.ndjson` received and analyzed (44/44 phase markers
present and correctly paired; user-supplied convention: positive = left for all three axes,
resting orientation flat on a table face up). Full detail: full report §13.

- **Direction-correlation test on Fields A/B (§3.9's two candidate accumulators): negative.**
  Neither field's slope reproducibly tracks labeled rotation direction — repeated identical
  motions produced wildly different, even sign-inconsistent, slopes. A systematic scan of every
  possible 2-byte field found no offset with a defensible direction-correlated signal (the
  strongest candidate's effect size is plausibly explained by chance given the number of offsets
  tested).
- **🔵 Strong evidence, cross-validated by two independent metrics, reproduced across ~11 distinct
  byte offsets spanning nearly the whole 40-byte block**: a dramatic (100-1000×) increase in
  residual noise/byte-transition-rate the instant the controller is handled at all (deliberate
  rotation *or* a nominally-still `_hold`), vs. near-total silence during confirmed genuine rest
  (`baseline1`/`baseline2`, one byte frozen 99.7% of the time at rest vs. changing 70% of the time
  during rotation). The nominally-still `_hold` phases show *inconsistent, intermediate* noise
  levels across different offsets and axes — the signature of variable involuntary hand tremor
  while holding an awkward grip, not a fixed motion/still state flag.
- **Classification**: doesn't cleanly fit rate-like, orientation-like, acceleration-like, or
  timing/counter-like as originally framed — recorded as a new, empirically-derived category,
  **activity/vibration-responsive noise floor**, consistent with raw/unfiltered sensor samples
  sensitive enough to alias real mechanical vibration, but this is a structural characterization,
  not a semantic one. **No axis, physical unit, or scale is assigned to any byte.**
- **Next step, superseded**: see §3.11 — explicit direction was given to investigate the block's
  internal packet structure more deeply before requesting the hand-held test proposed here.

### 3.11 Structural decomposition against a documented IMU FIFO layout: no candidate survives (2026-07-10)

The controller's IMU is identified elsewhere in this repo as an **ICM-42670-P** (🔵 strong
evidence, third-party teardown) — this pass tested whether the 40-byte block matches that chip's
documented FIFO packet architecture (candidate 2×20, 4×10, 5×8, 8×5 byte splits) using a new tool,
`tools/analyze_sw2_block_structure.py`. **Confidence caveat, stated explicitly**: the exact
byte-level FIFO layouts tested come from general/trained knowledge of the shared ICM-42xxx family
architecture, not a directly-consulted ICM-42670-P datasheet (no internet/document access this
session) — a negative result below means "the specific layout tested didn't match," not "no
documented layout could possibly match."

- **❌ No periodic packet structure found.** An entropy-profile self-correlation test across every
  candidate packet period (2, 4, 5, 8, 10, 16, 20 bytes) — the signature a repeating packet with a
  stable header field would leave — found no meaningful positive correlation at any tested period
  (all near zero or negative). A supplementary sub-byte scan (checking whether header bits might
  share a byte with otherwise-busy data) found a few low-entropy candidates that don't recur at
  any consistent spacing, so they don't rescue any tested layout either.
- **🟡 Inconclusive**, not confirmatory: a physically-grounded test (does any 3-consecutive-int16
  window's vector magnitude stay orientation-invariant, as a real accelerometer's must, between
  confirmed rest and fixed-tilt holds?) found one candidate with a promising-looking match, but it
  sits adjacent to an already-confirmed non-physical accumulator byte (§3.9's Field A) whose own
  slow drift can produce a similar-looking spurious "stability" — no candidate was both a strong
  match and clear of this confound.
- **🔵 What remains strong**: the block-wide activity/noise-floor signature (§3.10) itself —
  difficult to explain as anything other than genuine, unfiltered sensor data, even though its
  exact internal packing is now confirmed *not* to match the periodic FIFO layouts tested.
- **Proposed next experiment (not requested from the user yet)**: a mechanically-supported,
  non-handheld test — table taps and a vibration source with no skin contact, vs. a hand-contact
  control — designed to separate "responds to genuine vibration" (supports real IMU data) from
  "responds to hand contact for some other reason" (would argue against it), with expected results
  defined per model before any capture. Full design, the two competing models, and the exact
  expected-result table: full report §14.6.
- **Trigger investigation (§3.9) explicitly not touched this pass** — kept separate, per explicit
  instruction not to mix the two threads. Full detail: full report §14, §15.

---

## 4. What BLE evidence can and cannot establish, vs. USB report 0x09

| Question | BLE capture (this facility) | USB report 0x09 (console) |
|---|---|---|
| Genuine motion byte *layout* for **some** Switch 2 report | Can establish directly, for whatever handle(s) are captured | Already independently established (`report-0x09-motion.md`, very high confidence) — different report |
| Whether that layout/semantics **transfers to report 0x09** | Cannot establish — different transport, different report, DyCOOL's own document explicitly declines to claim this | Only a genuine console-side USB capture can answer this directly |
| Whether feature/motion negotiation is required, and how | Can establish for BLE specifically, by comparing `sw2cap on` sessions with vs. without sending the reference tool's `configure_features`/`enable_features` commands (§5, experiment 5) | Already established for USB (`0x0C`/`0x27`, negotiated) — a different negotiation, not proof BLE works the same way |
| The abrupt-jump symptom's root cause | **Cannot** directly explain a USB-transport, console-consumed symptom | Requires a genuine console-side USB capture — no substitute |
| General sensor/scale/bias characteristics of genuine hardware | Can contribute (e.g. cross-check gyro noise/bias figures against `switch2_native_motion_map_DyCOOL.md`'s numbers) | N/A |

**Bottom line:** this capability closes the "we're throwing away bytes we already receive" gap
and can build a rigorous, first-party BLE inventory — genuinely valuable RE work in its own
right, and CLAUDE.md's standing objective either way. It is **not** a substitute for a genuine
report-0x09 USB capture, which remains the only direct way to resolve the console-side symptom.

---

## 5. Controlled experiment matrix

Each experiment: connect a genuine controller, open the config web UI's "Switch 2 BLE Capture"
panel (§2.5), click **Start Capture**, perform the described action, click **Stop Capture**, then
**Download NDJSON** and rename the file to note which experiment it was. Keep experiments short
and single-variable — don't combine multiple actions in one capture unless the matrix says to.
Check the panel's `dropped` count is `0` before trusting a session's completeness.

| # | Experiment | Method | What to look for | Status |
|---|---|---|---|---|
| 1 | **Stationary baseline** | Controller flat, motionless, 2+ minutes immediately after connect | Does the `0x000A` report change at all beyond the counter bytes? | ✅ **Done** (`STILL_CAPTURE`, 6196 records) — no orientation-responsive change anywhere; see §3.5 |
| 2 | **Isolated pitch** | From stationary, rotate slowly about the physical pitch axis only, ~90°, pause, return | Which byte range changes, and how | 🔵 Partial — `ANGLE1`/`ANGLE2`/`ANGLE3` are fixed-orientation, not axis-isolated single-motion captures, and filenames don't establish which physical axis each is; see §3.5 for what *did* change (nothing orientation-responsive) |
| 3 | **Isolated yaw** | Same, yaw axis only | Same comparison, yaw axis | ⬜ Not run as an isolated case |
| 4 | **Isolated roll** | Same, roll axis only | Same comparison | ⬜ Not run as an isolated case |
| 5 | **Fixed non-zero tilt** | Hold at a constant tilt, motionless, 1+ minute | Does any field settle to a stable non-baseline value at a held tilt? | ✅ **Done** (`ANGLE1`/`ANGLE2`/`ANGLE3`, 3 separate fixed tilts) — none found; see §3.5 |
| 6 | **Known-angle movement** | Rotate a controlled, known amount at a controlled rate | Scale-factor estimate (counts/degree) | ⬜ Blocked — no field yet identified to measure a scale factor against (§3.5) |
| 7 | **Feature-mask replication** | `sw2cap experiment on` (§2.6) — subscribes `0x000E` + sends `configure_features`/`enable_features` (flags=0x07) | Does anything change on `0x000A`, or does anything new arrive on `0x000E`, after this command? | ✅ **Done, negative** (`EXPERIMENTAL`, §3.6) — command accepted (ACK'd), but no orientation data resulted; concrete v2 lead identified (§3.6) |
| 8 | **Report-length / handle variants** | Same `sw2cap experiment on` toggle (§2.6) | Whether any notification ever arrives on `0x000E` | ✅ **Done, positive-but-limited** — `0x000E` *is* reachable (2,331 notifications), but its content is a duplicate of `0x000A`'s buttons+sticks, still 0 bytes of orientation data (§3.6) |
| 9 | **Initialization capture** | `sw2cap on` *before* connecting; capture power-up through `SW2_INIT_DONE` | Full command/ACK/state sequence with real timing | 🔵 **Partially done, incidentally** — `EXPERIMENTAL` captured a full `READ_INFO→PAIR_STEP1→…→SET_LED→DONE` sequence (the `READ_LTK` branch was *not* taken, confirming this device reconnected via the "already known" path) preceded by a second, ~13s-earlier, incomplete `ccc_write` to `0x001B` with no follow-up — a real data point for the "BT pairing reliability" backlog item (a connection attempt that stalled before a second one succeeded), not yet analyzed in depth |
| 10 | **Reconnect** | Disconnect and let it reconnect, `sw2cap on` throughout | Does reconnect skip any init steps? | 🔵 Same data point as #9 above touches this; not a dedicated, analyzed reconnect experiment yet |
| 11 | **Wake** | If reachable, capture across sleep/wake | Any commands/notifications specific to the transition | ⬜ Not run |
| 12a | **GATT discovery** | `sw2cap gattdisc on` (§2.7.1) | Ground truth for `0x000C` | ✅ **Done** — `0x000C` confirmed as `0x000A`'s vendor descriptor; several new undocumented characteristics found; see §3.8, full report |
| 12b-12g | **v2 variant matrix** | `sw2cap variant <1-6>` (§2.7.2) | Whether any variant produces independent, motion-consistent data; which variable causes it | ✅ **Done, major positive result** — all six variants produced a real 40-byte independent data block on `0x000E`; the cause is **not** any single tested variable (the plain control alone reproduces it) — see §3.8, full report `docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md` |

**Priority order, updated (post-hardware-run):** the proposed stationary-only re-run of variant 1
(full report §6 — the single highest-information follow-up: does the 40-byte block's drift
persist with the controller genuinely motionless, or does it reflect real handling?) → 9/10 (worth
a dedicated, single-variable reconnect capture given the incidental stalled-connection data point
already seen) → 2-4, 6
(axis/scale characterization, only meaningful once/if the variant matrix finds a live field to
characterize) → 11 (exploratory / lower priority).

---

## 6. `Dycool/Usb-relay-for-NS` — concrete assessment for console-side USB capture

(Research conducted earlier this session; recorded here as the authoritative writeup per this
task's request. No new investigation performed in this pass.)

**What it is:** a live USB relay chain — `real Switch 2 console (USB) ↔ Raspberry Pi (configfs
USB gadget, emulating a Pro Controller) ↔ UDP ↔ Windows PC (hidapi) ↔ real Pro Controller 2
(USB)`. The Pi's gadget setup script explicitly sets `idProduct=0x2069` (Pro Controller 2),
confirming it targets this exact device. The Windows-side relay (`win_pro_relay_capture.cpp`)
opens the genuine controller directly via `hidapi`, performs the Switch2 USB init handshake, and
relays both directions while writing a timestamped capture file — `ControllerToSwitch` direction
is exactly the controller→console input stream that would carry report 0x09 motion. Its
`Switch2Protocol` class is currently pure byte pass-through (`// TODO: Switch 2 protocol
translation if needed`), so a working relay would yield **unmodified genuine report 0x09 bytes**.

**What's required:** a Raspberry Pi with USB gadget/OTG capability (`configfs`/`libcomposite` —
a Pi Zero or a Pi 4 in `dwc2` mode both qualify) plus a Windows PC. **No specialized USB analyzer
hardware** (no Cynthion/Packetry/FaceDancer) — meaningfully more accessible than the capture
method that originally decoded report 0x09's format.

**What's unverified:** this is an explicit "v0." A code comment in `pi_pro_proxy.cpp` states it
only forwards HID reports and does **not** implement a lower-level FunctionFS USB device — if the
Switch 2 console's device classification depends on lower-level USB behavior the gadget doesn't
replicate, this may simply fail to enumerate as a real console-facing device. No committed sample
captures exist, and a referenced decode script (`decode_relay_capture.py`) is mentioned in a
comment but not present in the repo.

**Assessment:** this is the **single highest-value candidate** for finally closing the
console-side report-0x09 evidence gap that has constrained this entire investigation (every
existing report-0x09 fact traces back to one static third-party capture this repo cannot repeat
or control). It is not yet a tool to build on — it's a tool to **try**, next time Raspberry Pi +
console hardware access coincides, with the explicit expectation that the v0 gadget layer may
need real debugging before it produces anything. Recommended as the top item once this session's
BLE-capture work (§2-5) has run its course or stalls for lack of further BLE-only progress.

---

## 7. Explicitly not done this pass

Per the task's constraints: the report-0x09 encoder (`ns2_build_report()`, `ns2_motion_tick()`)
was not touched; no filter, bias-correction, coordinate-transform, or quaternion/integration model
was implemented or claimed; no root cause is claimed for the console-side jumping symptom; nothing
captured or analyzed this pass (or any experiment run so far) is decoded into physical units or
assigned orientation semantics — every finding stays at the byte level; the normal BLE init
sequence (`switch2_send_init_cmd()`, the `sw2_init_state_t` machine) was not modified by either the
v1 experiment (§2.6) or the v2 matrix (§2.7) — both run strictly *after* `SW2_INIT_DONE`, as
additional one-shot actions, never a change to init itself; **the v2 matrix has now been run on
hardware, and — per this task's explicit instruction — a variant is recorded as successful only
because a hardware capture demonstrated motion-correlated bytes (§3.8, full report), not on
confidence alone**; no orientation/gyro/accelerometer/quaternion semantics were assigned to any of
the newly-active bytes; no root cause is claimed for why this session's result differs from the
prior all-negative session (ranked as explicit hypotheses, not facts, in the full report §6); no
new code was written this pass — analysis, a new analysis tool, and documentation only.

---

## 8. Capture procedure used for the §3.6 experiment run (completed 2026-07-10)

Recorded as-run, for reproducibility. Requires a genuine Pro Controller 2 and the config web UI
(`web/index.html`, served locally by `tools/run_config_portal.ps1`).

1. Open the config page, connect to the Pico over serial, open the **Switch 2 BLE Capture** panel.
2. Check the **Experimental: attempt motion-enable (handle 0x000E)** checkbox (pill goes
   `off` → `armed`). This does nothing yet — it only arms the *next* connection.
3. Click **Start Capture**.
4. Force a fresh BLE connection at the **link layer** — there is no config-mode pair/unpair/
   disconnect command, and none is needed: `btstack_host.c`'s connection code never references
   config mode at all, so the BT stack behaves identically whether the Pico is in normal or config
   mode. **Power the genuine Pro Controller 2 off, then back on.** Powering off drops the BLE ACL
   (a real disconnect, seen by `switch2_cleanup_on_disconnect()`, which also resets the
   experiment's one-shot guard); powering back on lets the Pico's existing periodic
   reconnect-to-bonded-device scan pick it up and run it through the normal init sequence to
   `SW2_INIT_DONE` — which is what fires the armed experiment. (Moving the controller out of BLE
   range and back achieves the same disconnect/reconnect if powering off isn't convenient.) Known
   flakiness applies here too (see `STATUS.md`'s "BT pairing reliability" item) — if it doesn't
   reconnect on the first attempt, power-cycle it again rather than assuming the experiment failed.
   Leave it stationary once reconnected.
5. Wait at least 30 seconds after the controller reconnects (covers normal init plus the one-shot
   experiment firing on the first feedback tick after `SW2_INIT_DONE`).
6. Click **Stop Capture**, then **Download NDJSON**. Confirm the panel's `dropped` count reads `0`
   before trusting the session.
7. In the downloaded file, look specifically for: any `ccc_write` entry with `handle=0x000F`, any
   `cmd_out` entry with the `0c9101...` prefix (the configure/enable commands), and — the key
   question — **any `input` entry with `handle=0x000E`** (there will be none if the handle guess
   is wrong or the device ignores the command; there may also be `ack` entries worth checking for
   an error status). Uncheck the experiment toggle afterward so future normal-capture sessions
   don't re-arm it accidentally.
8. **Result (recorded here, not hypothetical):** `0x000E` input entries did appear — 2,331 of them
   (`dumps/sw2_capture_2026-07-10-EXPERIMENTAL.ndjson`). Analyzed with an ad hoc extension of
   `tools/analyze_sw2_capture.py`'s per-offset approach, filtered to `handle=0x000E`. Findings in
   §3.6: reachable, command accepted, but the content is a shifted duplicate of `0x000A`'s
   buttons+sticks payload with no orientation-responsive bytes.

---

## 9. v2 capture procedure — ordered by information value (not yet run)

Same connection mechanics as §8 (no config-mode pair/unpair/disconnect command exists or is
needed; power-cycle the genuine controller to force a fresh connection between attempts; expect to
retry a power-cycle once or twice given this stack's known reconnect flakiness). Differences:
select a variant from the **v2 experiment variant** dropdown instead of a plain checkbox, and run
GATT discovery first, alone, per this task's explicit instruction to resolve handle numbering
before trusting a variant that depends on it.

**Order** (per this task's explicit recommendation, "may find the enabling action quickly while
retaining the full reference sequence as a positive-control attempt" — do not skip ahead to save
time, since each step's value depends on having the prior ones' results):

0. **GATT discovery, alone.** Check the **GATT discovery** checkbox (not the variant dropdown),
   Start Capture, power-cycle the controller, wait ~10s (discovery is a fast burst of small
   queries, not a ~30s command sequence), Stop, Download. Confirm `dropped=0`. In the NDJSON, find
   the `gatt_char` entry whose `handle` is `0x000A` and note its declaration handle (data bytes
   0-1, LE u16) — this should read `0x0009`, confirming the whole §3.7.1 derivation's foundation.
   Then find whichever `gatt_desc`/`gatt_char` entry actually sits at `0x000C` (if any) and its
   UUID — this either confirms or corrects `SW2_REPORT_RATE_HANDLE_HYPOTHESIS` before variants
   3/4/6 spend a connection attempt writing to a possibly-wrong handle. Uncheck GATT discovery
   before moving on.
1. **Variant 1 (control).** Select `1 — control`, Start Capture, power-cycle, wait ~15s, Stop,
   Download. Expected (per §3.6, already run once as the standalone v1 experiment): `0x000E`
   duplicates `0x000A`'s buttons+sticks, nothing past the stick fields. Running it again here is a
   sanity check that the refactor into the variant table didn't change v1's behavior — not
   expected to find anything new by itself.
2. **Variant 2 (mask 0xFF).** Same procedure, `2 — mask_ff`. Tests whether the flags value alone
   matters.
3. **Variant 3 (handle-write only).** `3 — handle_write_only`. Watch specifically for a `cmd_out`
   entry with `handle=0x000C` (or whatever §step 0 corrected it to) and whether an `ack` follows it
   with a non-error status — a rejected write (ATT error) is itself informative: it would mean this
   isn't a writable attribute at all under the current mask, undermining the "unlock via this
   write" hypothesis regardless of variant 4/6's results.
4. **Variant 4 (mask 0xFF + handle-write).** `4 — mask_ff_handle_write`.
5. **Variant 6 (full sequence).** `6 — full_sequence`, run *before* variant 5 per the task's
   explicit recommended order — this is the closest replica of the reference tool's actual proven
   sequence (including the deferred CCC-subscribe timing), so it's the best single test of whether
   *anything* in this design space works at all. Wait longer here (~45-60s) — six calibration
   reads, each awaiting its own ACK, plus enable and the handle write, take measurably longer than
   the two-command variants.
6. **Variant 5 (calibration sequence), conditionally.** Only run `5 — calibration_seq` if variant 6
   produced orientation-responsive bytes but variants 2-4 did not — that pattern would implicate
   the calibration reads specifically, and variant 5 isolates them (flags stay at `0x07`, no
   handle write) to confirm.

**After each variant:** check `dropped=0`, then search the downloaded NDJSON for the `variant`
entry (confirms which variant actually ran — cross-check against what was selected), the sequence
of `cmd_out`/`ack` pairs (confirms each step's ACK arrived with a non-error status before the next
step fired — a stall partway through is visible as the sequence simply stopping), and — the
success criterion this task specifies — **any byte region in any `input` entry (on `0x000A` or
`0x000E`) that changes under physical motion and differs from the stationary baseline already
established in §3.5**. A new report *shape* (different length, new nonzero region, even if not yet
understood) counts; do not decode it in the field, just confirm it exists and note which variant
and which offsets, then bring it back for the same rigorous per-offset analysis §3.5/§3.6 already
demonstrated.

**§9 has now been run in full** (all 8 captures: GATT discovery + variants 1-6, plus the base
baseline) — results in §3.8 and the full report,
`docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md`.

---

## 10. Next capture procedure (proposed, not yet run)

Per the full v2 report's §6: the single highest-information follow-up is a **stationary-only
re-run of variant 1** — no new code, the exact already-implemented, already-verified control
variant, with one added discipline: hold the controller genuinely motionless for the entire
session this time (unlike this pass's back-to-back six-variant testing run, whose procedure didn't
re-state that requirement as explicitly as the original standalone v1 test did).

1. Select **`1 — control`** in the variant dropdown, Start Capture, power-cycle the controller,
   and **do not touch or move it for the whole session** (~90s, matching variant 1's typical
   duration this pass).
2. Stop Capture, Download, confirm `dropped=0`.
3. Check whether the 40-byte block at offsets 14-54 on `0x000E` still activates and drifts. Two
   informative outcomes either way — see the full report §6 for what each implies. This is a
   read-only confirmation pass; do not decode axis semantics from it in the field.
