# BTStack Implementation (`joypad-os`)

> Current-status note (2026-07-24): this document contains the detailed, dated implementation
> history. The authoritative runtime architecture is `docs/architecture/overview.md`; current
> controller and BOOTSEL results are in `STATUS.md` and
> `docs/status/compatibility-matrix.md`. DualSense/Edge input, LEDs, rumble, and BOOTSEL gestures
> are now hardware-confirmed. Xbox BLE output, Switch 2 pairing/reconnect, and normal GATT
> initialization are also hardware-confirmed. Treat older validation checklists below as snapshots
> from the date of each change; only items still open in the compatibility matrix remain current.
> References to `DATA.md` in those snapshots mean the deleted session brief catalogued in
> `docs/archive/ephemeral-handoff-index.archived.md`, not a current authority.

This project implements a native Bluetooth HID host stack based on [joypad-os](https://github.com/joypad-ai/joypad-os) to handle controller connections and inputs. **Bluepad32 has been completely removed** from this project in favor of this implementation, which provides much finer-grained control and parses extra controller features (e.g., paddles, capture buttons).

## Architecture & Submodules

The implementation lives primarily under `src/bt_hid/` and is tied to the standard Pico SDK `btstack` (specifically `pico_btstack_ble` and `pico_btstack_classic`).

### Core Host (`ns2_bt_host.c`)
- **Initialization:** Responsible for setting up the BTstack environment (`sm_init`, `l2cap_init`, `gatt_client_init`, etc.) and the various GATT/HIDS clients.
- **Connection Management:** Handles device scanning, connecting, and GATT discovery (for BLE HID). It also handles Classic BT HID device connection (`hid_host`).
- **Storage:** Persists link keys and bonding information to flash using BTstack's TLV flash storage.

### Drivers & Registry (`bthid.c`, `bthid_registry.c`, `devices/`)
- A registry-based system maps connected vendor/product IDs to a specific driver (`switch2_ble`, `ds5_bt`, `generic`, etc.).
- The driver parses the raw HID report from the controller and outputs a unified `input_event_t`.
- **Unified Button Model:** Converts all controller inputs into W3C standard bits (e.g., `JP_BUTTON_B1`, `JP_BUTTON_L4`), which enables agnostic handling of Switch, PlayStation, Xbox, and Generic controllers.

### The Seam (`ns2_seam.c`)
- Connects the isolated `joypad-os` domain to the PicoSwitch2 `switch_pro_input_t` format used by the USB core.
- **Router Submit:** When a driver completes parsing an input report, it calls `router_submit_input(const input_event_t *e)`. 
- **Remapping:** `ns2_seam` maps the generic `JP_BUTTON_*` bits to Switch 1 buttons and Switch 2 extra buttons (C, GL, GR) based on a configurable mapping.
- **Analog Packing:** Translates 8-bit `joypad-os` analog values into the packed 12-bit format required by the Switch Pro Controller.
- **Feedback:** Serves as a bridge for rumble and LED states. `report_get_rumble()` pulls the USB-requested rumble amplitude, which is pushed to the controller via its driver.

---

## Rumble regression (🟢 core families restored on hardware 2026-07-12; edge cases remain)

> **Hardware validation update (2026-07-12):** the user flashed the latest combined build and
> confirmed rumble is working again on an Xbox controller, DualSense, Switch 1 Pro Controller,
> and Wiimote both standalone and with its joystick attachment. This validates that the repaired
> shared feedback path and the now-reachable driver output paths produce real vibration across
> several independent controller families. It does **not** yet prove left/right motor fidelity,
> amplitude curves, stop/reconnect behavior, Xbox transport or Classic-input parsing, generic
> non-Microsoft XInput rumble, DS4 CRC behavior, or the flagged Wiimote channel distinction.
> Those remain explicit follow-ups rather than reasons to keep the whole regression open.

> **2026-07-12 correction:** this section originally claimed `xbox_ble.c` "already has a complete,
> correct implementation" and used it, unverified, as the template for the `xbox_bt.c` fix below.
> **That claim was never actually tested — it was a static-review assumption** ("the code reads
> correctly"), not a Confirmed fact. The user then hardware-tested a real Xbox Series controller
> over BLE and reported rumble still does not work. The claim has been struck through/corrected
> throughout this section rather than silently edited, per this project's evidence-discipline rule:
> don't promote assumptions into facts, and don't erase a wrong claim without recording that it was
> wrong. See the new "BLE Xbox: still broken, cause not yet isolated" subsection below.

**Reported:** BT rumble stopped working across DualSense, Xbox, and generic/XInput-class
controllers. The leading hypothesis going in was a recent uncommitted refactor that changed the
shared `report_set_rumble()`/`report_get_rumble()`/`feedback_get_state()` seam from one scalar
amplitude (duplicated to both motors) to independent left/right values. **That hypothesis did not
hold up** — full line-by-line review of that diff (`include/report.h`, `src/report.c`,
`src/bt_hid/ns2_seam.c`, `src/switch_pro/switch_pro.c`, `src/switch_pro2/switch_pro2.c`) found it
internally consistent: every signature change has a matching call site, the per-side decode
offsets in both `switch_pro.c`'s `decode_rumble_motor()` and `switch_pro2.c`'s
`ns2_rumble_motor_amp()` calls are byte-identical to what the old single-scalar code computed
per side before collapsing to a peak, and the dirty-flag comparison in `feedback_get_state()` is a
straightforward two-field generalization of the old one-field check. **No defect found in the
stereo-rumble diff itself.**

### The real root causes (Confirmed from code, traced end to end per the requested 10-step path)

**1. Shared, cross-family root cause: `find_player_index()` didn't actually enforce single-player
routing, contradicting its own comment.**

```c
// src/bt_hid/ns2_seam.c, BEFORE this fix
int find_player_index(int dev_addr, int instance) {
    (void)instance;
    return (dev_addr >= 0 && dev_addr < NS2_SLOTS) ? dev_addr : 0;
}
```

`dev_addr` here is never a real Bluetooth address — every one of the 13 call sites (Sony, Microsoft,
Nintendo, generic, Stadia drivers, plus `switch2_handle_feedback()` in `btstack_host.c`) passes
`event.dev_addr`/`conn_index`, which is BTstack's own connection-slot index. `NS2_SLOTS` is
`SWITCH_PRO_MAX_CONTROLLERS`, hardcoded to `4` in `CMakeLists.txt` **unconditionally, regardless of
`NS2_PRO`** — not narrowed for the single-controller NS2 milestone. `btstack_host.c`'s
`find_free_classic_connection()` is a real 4-slot allocator for Classic BT connections (indices
0-3); BLE connections use `BLE_CONN_INDEX_OFFSET = MAX_CLASSIC_CONNECTIONS = 4` added to their
index, so they're always `>= NS2_SLOTS` and always fell through to the `: 0` fallback — accidentally
safe. **A Classic BT device landing in connection slot 1, 2, or 3 got that nonzero index back
verbatim as its "player index."** Every driver's `feedback_get_state(player_idx)`/
`feedback_clear_dirty(player_idx)` then read/cleared `s_fb[1]`/`s_fb[2]`/`s_fb[3]` — but
`ns2_hid_out_report()` (`switch_pro2.c`) **always** publishes rumble to hardcoded slot 0
(`report_set_rumble(0, left, right)` — the single-controller NS2 milestone's own documented
design). Any device outside classic slot 0 was reading a feedback slot that never received
anything: `rumble_dirty` always false, `rumble.left/right` always `(0,0)`, silently, driver-agnostic
— explaining a regression reported across independent controller families identically, and
**unrelated to mono-vs-stereo**: a single scalar published to slot 0 would have been exactly as
invisible to a driver resolved to a different slot.

Whether a given device actually lands outside slot 0 in practice depends on Classic connection-slot
allocation/reuse across a test session (e.g. testing multiple controllers sequentially without a
full dongle power-cycle between them) — plausible, not independently reproduced without hardware,
but the code path is unambiguous once traced.

**Fix:** `find_player_index()` now always returns `0`, matching its own pre-existing comment
("single controller; no player manager") — the previous implementation just didn't actually deliver
on that claim. This is not a workaround; it makes routing match this project's stated single-output
architecture exactly.

**2. Independent, second real defect: Xbox Classic BT (`xbox_bt.c`) never sent rumble at all.**

```c
static void xbox_task(bthid_device_t* device)
{
    (void)device;
    // Xbox BT controllers don't need periodic maintenance
    // Rumble is handled through HID output reports when needed
}
```

The comment claims rumble is handled elsewhere; nothing in the file ever called
`feedback_get_state()` or sent an output report — confirmed by an exhaustive grep for
`feedback`/`rumble` across the file (zero hits) before touching anything. The sibling BLE driver,
`xbox_ble.c`, *appeared* on static review to have a complete implementation (Report ID `0x03`,
8 bytes: `enable_actuators`, unused trigger bytes, strong/weak motor 0-100, duration/delay/repeat)
— this project's own header comment for `xbox_bt.c` states "Same layout as Xbox BLE (same hardware,
same HID report descriptor)," so the same format was assumed to apply over Classic BT too. **This
assumption about `xbox_ble.c` was never hardware-verified and has since been contradicted — see
below.** **Fix (still believed correct in isolation):** ported `xbox_ble.c`'s logic into
`xbox_task()` (adapted to `xbox_bt_data_t`, which needed `rumble_left`/`rumble_right` tracking
fields added — it had none), and added the missing `core/services/players/feedback.h` include the
file never had. This fix addresses a real, confirmed gap (Classic BT Xbox sent nothing at all), but
its Report-ID-`0x03` assumption inherits whatever is wrong with `xbox_ble.c`'s same assumption below
— Classic BT Xbox rumble has not been hardware-tested since this fix landed.

**3. Was open/unconfirmed; a real, evidence-backed defect found via reference-driver research
instead of a hardware round-trip — `loop_count` (byte 7) was `0x00` instead of a sustain value.**

User hardware test (2026-07-12, Xbox Series controller over BLE): rumble still doesn't work after
item 1's fix. Traced end to end and ruled out several layers before finding the actual defect:

- `find_free_classic_connection()` only allocates slots 0-3 for **Classic** BT. BLE connections get
  `conn_index = BLE_CONN_INDEX_OFFSET + ble_index` (`BLE_CONN_INDEX_OFFSET = MAX_CLASSIC_CONNECTIONS
  = 4`), always `>= NS2_SLOTS (4)`. `find_player_index()`'s *old*, buggy body
  (`(dev_addr >= 0 && dev_addr < NS2_SLOTS) ? dev_addr : 0`) already fell through to the `: 0`
  fallback for every BLE conn_index, before this pass's fix. **A BLE Xbox controller was already
  being routed to slot 0, both before and after item 1's change** — so item 1 cannot explain why it
  was still silent. **Ruled out, not the defect.**
- The output-report transport path was re-traced from scratch (previously never actually read,
  only assumed correct because DS5/DS4 use it): `bthid_send_output_report()` (`bthid.c`) →
  `bt_send_interrupt()` (`bt_transport.h`) → `cyw43_transport_send_interrupt()`
  (`bt_transport_cyw43.c`) → `btstack_classic_send_report()` (`btstack_host.c`). Despite its name,
  the last function **does** correctly branch on `conn_index >= BLE_CONN_INDEX_OFFSET` and calls
  BTstack's `hids_client_send_write_report(conn->hids_cid, report_id, HID_REPORT_TYPE_OUTPUT, data,
  len)` for BLE, which internally calls `gatt_client_write_value_of_characteristic()` — an
  **acknowledged GATT Write Request**, not an unacknowledged Write Command
  (`hids_client.c:867`). **Confirmed correct, not the defect** — and notably matches what real Xbox
  BLE drivers require (see below), so this project's transport layer was already doing the right
  thing here.
- Rather than guess at the report ID or payload format blind (or require another hardware round
  trip for something well-documented elsewhere — Xbox BLE controllers are one of the most
  thoroughly reverse-engineered HID devices in the Linux ecosystem), fetched the actual reference
  implementation: [atar-axis/xpadneo](https://github.com/atar-axis/xpadneo), the mature,
  actively-maintained Linux driver for Xbox One S/Series controllers over Bluetooth, including BLE
  (HOGP). Its `xpadneo.h` defines the exact wire format:
  ```c
  #define XPADNEO_XBOX_RUMBLE_REPORT 0x03
  struct xpadneo_rumble_data {
      enum xpadneo_rumble_motors enable;   // bit0=weak,bit1=strong,bit2=right_trig,bit3=left_trig
      u8 magnitude_left;    u8 magnitude_right;    // trigger motors
      u8 magnitude_strong;  u8 magnitude_weak;      // main motors
      u8 pulse_sustain_10ms; u8 pulse_release_10ms; u8 loop_count;
  } __packed;  // + report_id prefix = 9 bytes total
  ```
  Compared byte-for-byte against `xbox_ble.c`/`xbox_bt.c`: **report ID `0x03` and every byte
  position matched exactly** — the enable-bit convention, trigger-vs-main motor split, and duration
  fields are all correct. This effectively rules out the report-ID/GATT-descriptor-mismatch
  hypothesis from the previous pass (still theoretically possible for some model variant — xpadneo's
  own `XPADNEO_QUIRK_*` flags show a handful of Xbox controller clones/firmware revisions use
  reversed or swapped motor-mask bits — but no longer the leading explanation).
  **One real difference found:** xpadneo's `rumble_worker()` (`rumble.c:76-77`) sets
  `pulse_sustain_10ms = 0xFF` **and** `loop_count = 0xEB` (235) — explicitly commented "we pulse the
  motors for 60 minutes as the Windows driver does" — so a single command sustains the effect for
  roughly 10 minutes without needing to be resent. This project's driver set `pulse_sustain_10ms =
  0xFF` correctly but **`loop_count = 0x00`** ("repeat: none"). On real hardware this most likely
  means the motor stops after a single ~2.55-second `pulse_sustain_10ms` burst and then goes silent
  — indistinguishable from "rumble doesn't work" to a tester, especially since this driver (correctly,
  matching xpadneo's own design) only resends when the amplitude actually *changes*, so nothing
  would re-trigger the motor for a sustained, unchanging rumble effect.

**Fixed in `xbox_ble.c`/`xbox_bt.c` (see item 4 — this was originally applied to dead, unregistered
code and had to be redone in the generic driver instead; both files are now re-registered as the
live Xbox path, so this fix applies here too):** `loop_count` (buf[7]) changed from `0x00` to
`0xEB`, matching xpadneo's value and rationale exactly. This directly answers one of the task brief's required trace questions — "Does
unchanged amplitude need periodic resend/keepalive for any controller family?" — yes for Xbox
BLE/Classic BT, and the fix is to set a long sustain window in the report itself (matching the
reference driver) rather than adding a periodic-resend mechanism.

**4. Process failure caught by a follow-up audit: items 2 and 3's fixes were applied to
`xbox_bt.c`/`xbox_ble.c`, which are dead code — never registered, never run.**

`src/bt_hid/bt/bthid/bthid_registry.c:16` states outright: *"xbox_bt.h and xbox_ble.h no longer
registered — generic driver handles all Xbox."* `xbox_bt_register()`/`xbox_ble_register()` are
defined (`xbox_bt.c:389`, `xbox_ble.c:296`) but **never called anywhere in the tree** — confirmed by
grepping the whole repo. Both files still compile (CMake globs `src/**/*.c`) and their driver
structs, comments, and — until this was caught — bug fixes all look completely legitimate on a
normal read. **Neither the original xbox_task() no-op fix (item 2) nor the loop_count fix (item 3)
ever ran on real hardware**, because no Xbox controller is ever routed to those drivers. This means
item 2's "Classic BT Xbox rumble sent nothing" fix was pointless from the start (the real Classic BT
Xbox path was never `xbox_task()`), and the BLE Xbox hardware test that motivated item 3's
investigation was testing a completely different code path than the one that got fixed.

The actually-reachable Xbox rumble implementation *at the time of the hardware test* was inside the
monolithic generic HID-descriptor driver and had an independent copy of the same Xbox rumble
format, including the live `loop_count = 0x00` bug. That was fixed using the shared
`xbox_rumble_build_payload()` implementation. The later generic-gamepad quirk split moved the
generic fallback's dispatch into the `xbox`/`xbox_elite2` profiles while deliberately retaining
the same Microsoft-VID gate and failed-send retry behavior.

**Resolution (user decision): re-register `xbox_bt.c`/`xbox_ble.c` as the primary Xbox path rather
than delete them**, retiring the generic driver's Xbox-specific code path in favor of the
purpose-built ones (`bthid_registry.c`, both now registered ahead of the generic fallback). Before
doing this, checked input-parsing confidence for each, since the registry's own prior comment
justified staying on the generic driver as "covers all Xbox variants without layout assumptions" —
worth taking seriously given `NS2_BT_ALL_DRIVERS` is unconditionally defined
(`CMakeLists.txt:78`), meaning Xbox was the *one* deliberately-held-back vendor family while every
other (Sony, Nintendo, Google, MouthPad) was already fully re-enabled — this reads as a considered
choice, not leftover Phase-0 staging:
- `xbox_ble.c`'s BLE button/stick parsing is a single, fixed 16-byte format, explicitly commented
  "verified from testing." **Low risk**, re-registered with confidence.
- `xbox_bt.c`'s Classic BT parsing guesses between two different report struct layouts based on
  report length (`xbox_bt_input_report_t` vs `xbox_bt_input_alt_t`), with no "verified" comment or
  equivalent evidence either format has actually been exercised against real hardware. **Higher
  risk** — re-registered per explicit instruction, but this is a real, acknowledged possibility of
  regressing currently-working Classic BT Xbox button/stick input (which the user has not reported
  as broken — only rumble was). Both files still exclude Xbox Elite Series 2 (`product_id 0x0B05`/
  `0x0B22`), which continues to fall through to the generic driver as before. Its resolved
  `xbox_elite2` profile owns the same validated rumble sender and paddle extractor.

**If a hardware test shows Classic BT Xbox input regressed** (wrong buttons, dead sticks, etc.),
the fix is either to add an `xbox_bt_match()` exclusion for the affected model (same pattern as the
existing Elite Series 2 exclusion) so it falls back to the generic driver, or to revert this
re-registration for Classic BT specifically while keeping BLE. This is now explicitly the top
hardware-validation risk introduced by this pass, on top of the actual rumble fix.

**Still not fully closed — needs one hardware retest, now with a specific, well-evidenced fix in the
correct file to verify.** If it's still silent, the remaining candidates in descending likelihood
are: a model/firmware variant needing one of xpadneo's known quirks (reversed/swapped motor-mask
bits, or no motor-mask support at all — see `XPADNEO_QUIRK_REVERSE_MASK`/`XPADNEO_QUIRK_SWAPPED_MASK`/
`XPADNEO_QUIRK_NO_MOTOR_MASK` in `xpadneo.h`), or a genuine report-map mismatch specific to this
device. Two diagnostics from an earlier pass remain in place as a fallback for that case (not the
primary path anymore):
- `bthid_send_output_report()` (`bthid.c`) logs `conn=/report_id=/len=` and whether
  `bt_send_interrupt()` queued or failed, for every call.
- The BLE `HID_SERVICE_CONNECTED` handler (`btstack_host.c`) hex-dumps the full raw GATT HID report
  descriptor once per connection.

**5. Known, deliberately NOT fixed: the generic driver only sends rumble for Xbox's vendor ID
(`0x045E`), through a resolved profile with a validated sender.** This driver matches *any*
unrecognized BLE HID gamepad or any
Classic device whose Class-of-Device says Peripheral/Joystick/Gamepad — a wide, output-format-
unknown net. It computes `left`/`right` and tracks "last sent" state as if committed to sending,
but for any vendor ID other than `0x045E` it clears `rumble_dirty` without ever transmitting — a
real, confirmed, honest capability gap. **Not fixed**: blindly sending the Xbox-format report to an
arbitrary matched device with an unknown, unverified output-report shape is a real hardware risk
this project cannot validate without a physical device of that exact type — documented in place
(see the code comment above `gamepad_task()`) as a known gap requiring a hardware HID-descriptor
capture before extending, not a guess to ship blind. Item 1's fix likely resolves the "generic/
XInput" symptom on its own for devices that were only failing due to the slot-routing bug; this
item 5 gap only bites a device whose vendor ID genuinely isn't `0x045E` *and* whose output format
is genuinely unknown to this driver.

### Additional findings from a follow-up subagent audit of the whole BT HID driver tree (2026-07-12)

Triggered by the dead-code discovery in item 4 — if one driver could have a well-documented fix
silently land in unreachable code, other drivers likely have analogous "claims vs. reality"
mismatches. A dedicated audit of every vendor/generic driver plus the feedback seam found:

- **`switch_pro_bt.c`'s `encode_rumble()`, Confirmed arithmetic bug, fixed:** `scaled` already had
  `+64` folded in, then `amplitude` added a second `+64` on top — an apparent copy/refactor
  duplication. Made even the smallest nonzero intensity jump straight to amplitude `128` with no
  headroom near the bottom of the range, topping out at `230` instead of the intended `~166`. Fixed
  by removing the duplicate offset. **Separately flagged, not fixed:** the real Joy-Con/Pro
  Controller HD-rumble amplitude curve is log2-based per
  [dekuNukem's reverse-engineering docs](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/rumble_data_table.md),
  not linear — this function remains a linear approximation even after the arithmetic fix, marked
  Hypothesis not Confirmed for perceived-strength accuracy. A faithful port of the real curve/table
  is separate, larger work, out of scope for this pass.
- **`ds4_bt.c`'s BT output report, Confirmed against the current Linux kernel source, fixed:**
  fetched `hid-playstation.c` directly (`torvalds/linux`, `drivers/hid/hid-playstation.c`) and
  compared `struct dualshock4_output_report_bt` field-for-field against this driver's hand-built
  buffer. Two real gaps: `hw_control` (buffer offset 2) was `0x80` (`DS4_OUTPUT_HWCTL_HID` only),
  missing `DS4_OUTPUT_HWCTL_CRC32` (`0x40`) — so even if a CRC were present, the controller wasn't
  told to expect one; and no CRC32 was ever computed at all (bytes 74-77 of the report were left
  zero). Modern DS4 firmware requires this CRC (`PS_OUTPUT_CRC32_SEED = 0xA2`, same algorithm
  `ds5_bt.c` already implements for DualSense) to accept a BT output report. Fixed: `hw_control =
  0xC0`, `valid_flag0` corrected to the real `MOTOR|LED` bits (`0x03`, was `0xFF`), and the CRC32
  computed and appended (duplicated `ds5_bt.c`'s CRC function locally rather than sharing across two
  files). **Not yet hardware-tested.** `ds3_bt.c` was checked for the same class of issue and is
  clean — the PS3-era protocol has no CRC requirement in any reference.
- **`xbox_bt.c`/`xbox_ble.c` dead code** — see item 4 above.
- **Not fixed, flagged for a decision:** `wiimote_bt.c` sends `wiimote_request_status()`/
  `wiimote_write_data()`/`wiimote_read_data()` over the BT **control** channel, while its sibling
  `wii_u_pro_bt.c` explicitly comments *"Send on INTERRUPT channel (some controllers reject commands
  on CONTROL channel)"* and uses the interrupt channel for the equivalent calls — an inconsistency
  within `wiimote_bt.c` itself (its rumble/LED/report-mode calls already use the interrupt channel)
  and a direct contradiction of the sibling driver's documented hardware finding for the same
  Wiimote-family protocol. Mitigating factor: `btstack_wiimote_send_control()` rewrites the header
  to a proper `SET_REPORT` before sending, so it's not a raw protocol violation — but on a real
  Wiimote (as opposed to Wii U Pro) that rejects control-channel writes, extension detection
  (Nunchuk/Classic Controller/Guitar Hero) could silently never complete since the 5-retry/1s-timeout
  state machine fails quietly rather than hanging. **Not fixed this pass** — lower confidence than
  the other findings (hardware impact genuinely unverified, and it's plausible Wiimote vs. Wii U Pro
  really do differ here), flagged for a hardware test with a real Wiimote before touching it.
- **Architectural, not a bug:** `find_player_index()` always returning slot 0 means every
  simultaneously-connected BT controller reads the *same* shared feedback slot — if BT-side
  multi-controller input is ever exercised, every connected controller would rumble in lockstep to
  whatever the console commands for player 1, rather than each controller rumbling only for its own
  slot. Consistent with this project's documented single-controller milestone; the concrete
  architectural blocker for per-controller rumble once BT multi-controller support is built.
- **Audit scope note:** covered every file in `devices/`, the registry, `ns2_seam.c`, and the
  feedback seam in depth; did not reach the bulk of `btstack_host.c`'s Classic/BLE connection state
  machines (5000+ lines), `mouthpad_ble.c`, or `sw2_capture.c`'s ring buffer — flagged as where a
  follow-up pass should continue.

### Fix requirements checklist (from the task brief)

| Requirement | Status |
|---|---|
| Fix the shared root cause first | ✅ Item 1 |
| Preserve independent L/R values | ✅ Untouched — stereo refactor was never the defect |
| Explicit mono fallback only where genuinely single-motor | N/A — no mono fallback needed; the bug was routing, not encoding |
| Never clear `rumble_dirty` until the correct consumer accepted the update | ✅ `ds5_bt.c`/`xbox_bt.c`/`xbox_ble.c` all clear only alongside an actual send-or-genuinely-nothing-changed; the generic driver's Xbox-only gate clears dirty without sending for non-0x045E devices — documented, not fixed (item 4) |
| Preserve reliable zero/stop delivery | ✅ Same dirty-flag/compare-and-send pattern handles `(0,0)` identically to any other value — no special-cased "nonzero only" path exists anywhere in the traced code |
| Avoid indefinite dirty-state spam when disconnected | ✅ Unchanged — `feedback_get_state`/`feedback_clear_dirty` are per-slot and only polled by an active driver's task, which stops running once the device disconnects |
| Keep per-player state isolated | 🔵 By design, this project has exactly one player (slot 0) — "isolated" now correctly means "always the same slot," matching the single-controller milestone, not literal multi-player isolation (out of scope until multi-controller is implemented) |
| No family-specific patches without evidence of a second independent defect | ✅ Item 2 (Xbox Classic) is exactly that — proven by direct code absence, not inferred. Item 3 (BLE Xbox) is still open — diagnostics added, not yet a confirmed second/third defect |

### Deliberately not built: a host-side decoder test

The task brief suggested a small host-side decoder/seam test. Not built this pass: the actual
defect (`find_player_index()`) has no meaningful "input bytes" to fixture — it's a pure integer
routing function, and its bug (returning something other than a hardcoded constant) is more
directly caught by reading the two-line implementation than by a test harness. The *decode* logic
(`decode_rumble_motor()`/`ns2_rumble_motor_amp()`) was already verified correct by direct diff
comparison against the prior working version (above) — no behavior differs there, so a new test
would be validating already-unchanged code. Revisit if a future rumble regression *is* traced to
the decode layer specifically.

### Historical validation state at implementation time

The checklist below records what was still open on 2026-07-12. Subsequent hardware passes confirmed
Xbox BLE ON/STOP/reconnect, DualSense/Edge stereo/native haptics and STOP behavior, and the shared
Switch output seam. Use the current compatibility matrix for release decisions.

**Performed:** all three configurations (`NS2_AUDIO=on`, `NS2_AUDIO=off`, `NS2_PRO=off`) rebuilt
clean after each change, including the `loop_count` fix in `xbox_ble.c`/`xbox_bt.c`.

**Hardware-tested so far:** Xbox Series controller over BLE — **still no rumble** after item 1's
fix (2026-07-12 user report, before the `loop_count` finding). This disproved the original
"`xbox_ble.c` already works" assumption. The `loop_count` fix has not yet been hardware-tested —
it was found via reference-driver research (xpadneo), not a new capture.

**Required before calling this closed** (per the task's own explicit validation matrix — builds
alone are insufficient):
- Xbox BLE: re-test rumble with the `loop_count = 0xEB` fix. If still silent, capture the
  diagnostics below (hex-dumped GATT HID report descriptor + send-status log) to check for one of
  xpadneo's known motor-mask quirks or a genuine report-map mismatch on this specific unit.
- Xbox Classic BT (`xbox_bt.c`, item 2): not yet hardware-tested at all — carries the same
  `loop_count` fix now, but treat as open until tested, not as done.
- DualSense: left-only, right-only, both, amplitude change, stop. Not yet tested since item 1
  landed.
- Generic/XInput or 8BitDo: rumble on (if the device's vendor ID happens to be `0x045E`-compatible
  or it's otherwise claimed by a driver that sends), amplitude change, stop. **If the specific
  device's vendor ID isn't `0x045E` and no other driver claims it, item 4's gap means this will
  still be silent** — that's the expected, documented remaining limitation, not a fix failure.
- Switch 1 output config and Switch 2 Pro output config, both through this same seam.
- Disconnect while rumbling, reconnect, then rumble again (exercises whether a stale slot/dirty
  state from the previous session could interfere — `find_player_index()` always returning 0 means
  every reconnect uses the exact same slot every time, by construction).
- No rumble command → no spurious vibration (unchanged code path, not expected to regress, worth
  confirming anyway).

---

## Gate 2: identity and driver-binding architecture (✅ BLE closure hardware-confirmed — 2026-07-16)

> Confidence key: **Confirmed** (read directly in code, traced to the call site) / **Strong
> Evidence** (real mechanism, impact not independently hardware-measured) / **Hypothesis** /
> **Unknown**.

Full driver reachability inventory: `docs/bluetooth/driver-reachability-audit.md`. Full upstream
comparison: `docs/bluetooth/joypad-os-upstream-comparison-2026-07-12.md`. This section covers the
identity-acquisition trace and the resulting architecture fixes.

### Why VID/PID reads `0000:0000` — Confirmed, traced end to end

**Not a parsing bug.** The BLE PnP ID characteristic (`0x2A50`) is a 7-byte structure per the
Bluetooth GATT spec: 1 byte `vendor_id_source` + 2 bytes `vendor_id` (LE) + 2 bytes `product_id`
(LE) + 2 bytes `product_version` (LE). Verified directly in BTstack's own generated accessors
(`btstack_event.h`): `vendor_source_id` at event byte 6, `vendor_id`/`product_id`/`product_version`
as little-endian `uint16` at bytes 7/9/11 respectively — spec-correct. This project's
`dis_client_handler()` (`btstack_host.c`) uses those generated accessors directly; the extraction
is spec-correct. **The 7-byte structure has never been the problem.**

**The actual cause is timing, confirmed from the real event sequence:**
`device_information_service_client_query()` — the call that starts the DIS/PnP-ID GATT read — is
only issued inside the `GATTSERVICE_SUBEVENT_HID_SERVICE_REPORTS_NOTIFICATION` handler, i.e. *after*
HID protocol mode is set to REPORT and report notifications are already enabled
(`btstack_host.c`, comment: "Mode is already REPORT and notifications are now enabled. Start the
remaining GATT clients one at a time: DIS -> BAS"). This ordering is deliberate, not an oversight —
an existing comment traces it to a real prior regression: running DIS concurrently with HID
notification setup previously starved the notification-enable GATT procedure on devices with many
report characteristics, and the device dropped the link. **Driver binding happens earlier**:
`bt_on_hid_ready()` fires at `GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED`, which precedes
`REPORTS_NOTIFICATION` in the real BTstack event sequence. So for any BLE device that doesn't supply
identity pre-connection (i.e., anything without Switch 2's manufacturer-data company-ID shortcut,
which is read straight off the raw advertisement before a GATT connection even exists), **VID/PID
is correctly, unavoidably `0` at the moment the driver is first selected** — this is not a defect to
patch at the parsing layer, it's an inherent ordering constraint this project shares with upstream.

**Reordering DIS earlier was considered and rejected.** It would directly reintroduce the
notification-starvation regression the current ordering was built to avoid — DATA.md's own caution
against "another name exception" applies equally to "just move DIS earlier" as a shortcut; the fix
has to be on the *consuming* side (re-evaluation), not by fighting the GATT contention constraint.

### The real, evidenced bug: re-evaluation doesn't always self-correct — found and fixed

Because identity legitimately arrives late, this project already has a re-evaluation mechanism
(`bthid_update_device_info()`, `bthid.c`) that re-checks driver selection once accurate VID/PID
arrives. Tracing it against registered drivers found several concrete gaps from the same root
pattern: **a driver's `match()` returning `true` from a name-based fallback
that never gets invalidated once contradicting VID/PID evidence arrives**, because the re-eval logic
only asks "does the *current* driver's own `match()` still return true?" — and a name fallback that
doesn't check VID/PID at all will always say yes, regardless of what VID/PID later turns out to be.

1. **`switch_pro_bt.c` (Switch 1, Classic-only hardware) could shadow `switch2_ble.c` (Switch 2,
   BLE-only hardware) over BLE** — Switch 1's driver is registered first, its name fallback matches
   any name containing "Pro Controller," and it has no transport guard. **Fixed structurally**: added
   `bthid_transport_mask_t transports` to `bthid_driver_t` (`bthid.h`), checked centrally in
   `find_driver()` and the re-eval loop *before* any `match()` call, so a driver whose declared
   transport doesn't match the connection's actual transport can never claim it — independent of
   VID/PID or name at all. Every driver now declares its real hardware's transport(s) (see the
   reachability audit doc for the full table); this closes the whole bug class, not just this one
   instance, and also hardens the previously-order-dependent-only `xbox_ble.c`/`xbox_bt.c` split.
2. **`ds4_bt.c` could permanently misclaim a non-Sony device** — DS4 deliberately advertises the
   generic name "Wireless Controller" (own comment), which a cheap generic/XInput-class clone could
   plausibly also use. The VID/PID check excluded DualSense explicitly but didn't reject *other*
   known-non-Sony VIDs before falling through to the name check, so even after a later SDP query
   resolved a clearly-non-Sony VID, `ds4_match()` would still return `true` via the name path on
   re-evaluation. **Fixed**: added an explicit `if (vendor_id != 0 && vendor_id != 0x054C) return
   false;` guard before the name fallback — VID/PID being simply *unknown* (`0`) still allows the
   name-based provisional match, unchanged from before; only a *known, contradicting* VID now
   forecloses it. Confirmed upstream shares this exact gap (byte-for-byte identical `ds4_match()`) —
   not a PicoSwitch2 regression, but a real bug worth fixing locally regardless.

3. **The 2026-07-16 BLE closure audit covered every registered BLE name fallback.** `xbox_ble.c`,
   `stadia_bt.c`, and `mouthpad_ble.c` now share the pure
   `bthid_name_fallback_allowed()` policy. An unknown VID still permits the immediate pre-DIS name
   bind. A known non-Microsoft VID invalidates Xbox's vendor-wide name fallback; Stadia and
   MouthPad additionally reject a known wrong PID for their expected vendor. Their exact identity
   paths are unchanged. `switch2_ble.c` already requires an exact Nintendo VID/PID and needed no
   name correction.
4. **A valid DIS result is always delivered to BTHID.** The connection cache is still updated first,
   but equality with advertisement-derived IDs no longer suppresses `bthid_update_device_info()`.
   Repeating the same identity is intentionally idempotent, while generic parsing can refresh its
   VID-dependent quirk state and the identity log can record DIS provenance.
5. **Host regression coverage exercises the production rebind state machine.**
   `tools/test_bthid_late_identity.c` proves immediate input on provisional and generic drivers,
   correction after authoritative VID/PID, transport-mask enforcement, safe fallback, repeated-DIS
   idempotence, and delivery of the first notification after rebind. The DIS query remains after
   notification setup; no GATT operation was moved earlier.

> **Hardware validation update, 2026-07-16:** the user flashed the resulting build and confirmed
> the Xbox Series BLE regression pass works correctly. Pair/input continuity and the previously
> confirmed output behavior remain intact with the notification-first DIS ordering and late
> identity handoff in place.

Classic-only name fallbacks (`ds3_bt.c`, `ds5_bt.c`, `wii_u_pro_bt.c`, `wiimote_bt.c`, and
`xbox_bt.c`) are outside this BLE DIS timing closure and remain candidates for a separate late-SDP
identity audit. The already-fixed DS4 guard and central transport masks continue to protect the
known concrete Classic cases.

### `vendor_source_id` is parsed correctly but currently discarded — flagged, not fixed

The PnP ID characteristic's `vendor_source_id` byte (1 = Bluetooth SIG-assigned company ID, 2 = USB
Implementer's Forum-assigned VID — genuinely different ID spaces, per DATA.md's explicit warning not
to confuse them) is correctly extracted and logged (`printf` in `dis_client_handler`) but never
stored or checked anywhere downstream — every driver's `match()` treats `vendor_id` as if it were
always a USB VID regardless of source. **No concrete failure observed for this** — every currently
supported vendor (Sony `0x054C`, Microsoft `0x045E`, Nintendo `0x057E`) conventionally reports
`vendor_source_id == 2` (USB-IF) for their BT accessories, a well-established real-world pattern —
but this is a real, if currently dormant, correctness gap: a future device reporting
`vendor_source_id == 1` (Bluetooth SIG ID) could numerically collide with an expected USB VID and be
silently misidentified. Not fixed this pass (would require threading an additional field through
`bthid_update_device_info()`'s API for a currently-hypothetical risk); flagged for the next driver
that reports unusual identity data.

### Evidence-ranked provenance (documented, not yet enforced as a runtime priority order)

Per DATA.md's requested ranking, strongest to weakest, as actually observed in this codebase today:

1. **BLE manufacturer-specific advertisement data** (Switch 2 only) — available pre-connection, from
   the raw advertising report itself; company ID `0x0553` (Nintendo) checked in
   `btstack_host.c`'s advertising-report handler before any GATT connection exists. Most reliable:
   no GATT round-trip, no timing race with driver binding.
2. **BLE GATT DIS PnP ID (`0x2A50`)** — spec-correct once it arrives, but arrives *after* initial
   driver binding for every device that doesn't have path 1 above (see timing section). Strongest
   *available* evidence once present; requires the re-evaluation path to actually act on it (now
   hardened across every registered BLE name-fallback driver per the fixes above).
3. **Classic BT SDP PnP information** — not separately re-audited this pass; existing code paths for
   Wiimote-family devices explicitly default VID/PID (`device->vendor_id = 0x057E` etc. in
   `wii_u_pro_bt.c`'s `wii_u_init()`) when SDP doesn't supply it, since "Wiimote-family lacks PnP
   SDP" per that file's own comment — a real, documented hardware limitation, not a bug.
4. **HID report descriptor / report-map signature** — used implicitly by the generic driver's report
   parsing (report length/field-layout heuristics, e.g. Xbox Elite Series 2 detection by 20-byte
   report length) but not yet formalized as a distinct provenance tier with its own confidence
   ranking.
5. **Exact hardware-tested name string** — the fallback every driver already has; now correctly
   demoted below known-contradicting VID/PID per the DS4/Switch-1 fixes above, but not below an
   *unknown* (zero) VID/PID, which is the correct behavior (name is still the best evidence
   available when nothing stronger exists yet). All registered BLE name-fallback drivers now apply
   this rule; the separate Classic late-SDP audit remains open.
6. **Class of Device / generic HID fallback** — `bthid_gamepad_driver`, lowest priority by
   registration order, matches last per `find_driver()`'s first-match loop.

This ranking is not currently enforced as an explicit runtime priority mechanism (e.g., a
provenance-tagged identity struct) — it emerges from the combination of registration order,
transport gating, and each driver's own internal VID/PID-before-name ordering. Formalizing it as
data (a `bt_identity_t` with an explicit provenance enum, checked before driver search) is a larger
change than this pass attempted; deferred as a candidate follow-up if a future driver's matching
logic proves too fragile to keep extending case-by-case.

### Stable per-device profile key (design, not implemented — no consumer exists yet)

DATA.md asks for a stable identity key design to support future per-device mapping profiles and
controller-specific quirks. **Important baseline fact**: this project currently has no per-device
mapping *storage* to migrate — `pico_config_t` (`config.c`'s flash-backed settings) holds only
global settings (the output-personality appearance/Sony-lightbar colors and the NS2 button remap table), not anything keyed per controller
model or per physical unit. Today's controller-specific behavior is resolved into a compiled-in
`gamepad_quirk_t` profile every connection from live VID/PID/name/report-shape evidence — it is not stored
configuration, so there's nothing to migrate yet. This design is infrastructure for *when* a
per-device mapping feature is actually built, not a retrofit of an existing one.

**Two separate identities, composed, per DATA.md's explicit instruction not to conflate them:**

1. **Model identity** — "what quirks/mappings apply to this kind of controller," shared across
   every physical unit of that model. Derived from the strongest available evidence, same
   provenance ranking as driver matching above:
   - `vidpid:{VID:04X}:{PID:04X}` if VID/PID is known (nonzero) — most stable, survives firmware
     updates and reconnects.
   - else `descfp:{fingerprint:04X}:{len}` (the same cheap checksum `bt_identity_log.c` already
     computes) if a HID report descriptor was captured — catches devices that never resolve
     VID/PID (8BitDo's own documented real-world unreliability here, per the upstream comparison
     doc) but have a consistent descriptor.
   - else `name:{matched-substring}` (the specific string a driver's `match()` matched on, not the
     full possibly-inconsistent advertised name) — least stable, but still better than nothing.
2. **Physical-device identity** — "which specific unit of that model is this," for per-unit
   customization (e.g. two DualSenses with different assigned LED colors). Classic BT's `bd_addr`
   is a stable, public, permanent address — usable directly. **BLE degrades honestly, not
   silently**: a resolved/public BLE address is usable the same way; a device only ever seen via a
   rotating resolvable-private-address with no IRK-based resolution available at this layer cannot
   be reliably distinguished from another unit of the same model across sessions — this is a real
   hardware/protocol limitation (per DATA.md: "randomized BLE addresses" is explicitly one of the
   cases this design must handle), not a bug to paper over with a false sense of precision. In that
   degraded case, physical identity falls back to `session:{conn_index}` — stable only for the
   current connection, explicitly not persisted as if it were a real per-unit key.

**Combined key** = `{model_key}#{physical_id}`. A profile store built on this should treat any key
containing a `name:` model component or a `session:` physical component as **best-effort** —
correctly shared/applied this session, but not guaranteed to re-match the same physical unit (or
even the same model, for a `name:` match against an inconsistent advertised name) on a future
reconnect. This is an explicit, documented property of the design, not an edge case to special-case
away — DATA.md's own instruction is to document collision/degradation behavior, not eliminate it
where the underlying Bluetooth identity data genuinely doesn't support better.

**Not implemented this pass**: no code currently computes or stores this key — `bt_identity_log.c`'s
`desc_fingerprint()` is the one piece of shared machinery this design already depends on and that
already exists. Building the actual profile store (flash-backed, keyed lookup, a migration/eviction
policy for collisions) is deferred until a real feature needs it, per CLAUDE.md's guidance against
designing for hypothetical requirements — this section exists so that feature, when built, doesn't
have to re-derive the identity-composition problem from scratch.

---

## Reconnect reliability (🟢 root cause found and fixed for the BLE path — 2026-07-12)

> Confidence key used throughout this section: **Confirmed** (read directly in this project's own
> code, traced to the actual call site) / **Strong Evidence** (a real mechanism exists but its
> real-world impact on the reported symptom isn't independently measured) / **Hypothesis** (plausible,
> not traced) / **Unknown**.

### Root cause (Confirmed): the BLE reconnect path retries blindly regardless of why the link dropped

`btstack_host.c`'s `HCI_EVENT_DISCONNECTION_COMPLETE` handler (the BLE branch, reached whenever
`find_connection_by_handle()` finds an active BLE slot for the disconnected handle) captures the
HCI disconnect `reason` byte but — before this fix — never inspected it. **Every** BLE disconnect,
regardless of cause, triggered the same "reconnect to last-known device" cascade:

```c
if (hid_state.has_last_connected && hid_state.reconnect_attempts < 5) {
    hid_state.reconnect_attempts++;
    btstack_host_connect_ble(hid_state.last_connected_addr, hid_state.last_connected_addr_type);
}
```

`btstack_host_connect_ble()` calls `btstack_host_stop_scan()` at its top, then issues a direct
`gap_connect()` to that one specific address. If that attempt fails or times out
(`BLE_CONNECT_TIMEOUT_MS = 10000`, i.e. 10 s — `gap_connect()` has no built-in timeout, so this
project's own code has to cancel it), `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`'s failure branch
retries the same way, up to 5 times total, before finally falling back to
`btstack_host_start_scan()`. **Worst case: up to 5 × 10 s = ~50 s where the host is committed to
directly connecting one specific address, with scanning stopped for the entire window**, before it
gives up and starts scanning again (which is what would let it discover the controller advertising
under a *fresh* connection attempt on its own).

This cascade fired identically whether the disconnect reason was an accidental link loss
(`ERROR_CODE_CONNECTION_TIMEOUT` = supervision timeout, genuinely worth chasing) **or** an
intentional one — `ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION` (0x13) or
`ERROR_CODE_REMOTE_DEVICE_TERMINATED_CONNECTION_DUE_TO_POWER_OFF` (0x15) — i.e. the controller
telling us on the wire that it disconnected on purpose (powered off, or the user explicitly
disconnected it). In that case a same-address direct reconnect is guaranteed to burn the full ~50 s
window on a device that has no intention of reconnecting right then, **and stop the host from
scanning for anything else — including that same controller coming back later under a fresh BLE
connection — until the cascade exhausts.**

This is specific to the **BLE path** — confirmed by reading the parallel Classic (BR/EDR) path,
`HID_SUBEVENT_CONNECTION_CLOSED` (used by DualSense/DualSense Edge/Xbox/8BitDo/etc.): it has **no**
such cascade — it frees the connection slot and, if no devices remain connected, just resumes
scanning/inquiry. No blind retry, no reason-code gap, no scan-starvation window. This line-of-code
absence is itself evidence: the architectural defect exists in exactly the one path this project's
own documentation names — **"Pro 2 reconnect"** (the real Switch 2 Pro Controller / Joy-Con 2,
which use `BT_BLE_CUSTOM` and this project's own GATT-based handshake, not Classic BT at all) — and
nowhere else.

### The fix (Confirmed, build-verified, not yet hardware-validated)

`btstack_host.c`, `HCI_EVENT_DISCONNECTION_COMPLETE`'s BLE branch now gates the reconnect cascade
on the disconnect reason:

```c
bool reason_warrants_reconnect =
    reason != ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION &&
    reason != ERROR_CODE_REMOTE_DEVICE_TERMINATED_CONNECTION_DUE_TO_POWER_OFF;

if (hid_state.has_last_connected && hid_state.reconnect_attempts < 5 &&
    reason_warrants_reconnect) {
    /* ...unchanged... */
} else if (btstack_classic_get_connection_count() == 0) {
    /* now also reached for a peer-initiated disconnect: resumes scanning immediately
       instead of burning ~50s on a doomed direct reconnect */
    btstack_host_start_scan();
}
```

This is the smallest change that removes a real, provable defect without touching anything already
working: `CONNECTION_TIMEOUT`/`CONNECTION_FAILED_TO_BE_ESTABLISHED`/every other reason still gets
the existing 5-attempt cascade unchanged (accidental link loss is still worth chasing directly);
only the two reasons that mean "the peer told us it's leaving on purpose" skip straight to
resuming scan. No new timers, no new per-device state, no change to the existing
`reconnect_attempts` reset-on-success behavior (already correct — set to 0 in three separate
success paths, confirmed by reading each one). Added one `printf` so the skip path is observable
during a hardware test.

### Full lifecycle trace (Phase 1 — read end to end, not inferred from names)

| # | Transition | Trigger / event | Owning layer | Key state | Initiator | Cleanup | Next |
|---|---|---|---|---|---|---|---|
| 1 | Cold boot, no bond | `BTSTACK_EVENT_STATE` → `HCI_STATE_WORKING` | `btstack_host.c` `packet_handler()` | `hid_state.powered_on=true`; Classic made discoverable+connectable; `btstack_host_restore_last_connected()` (no-op, nothing stored); `btstack_host_start_scan()` | — | — | 2 |
| 2 | Discovery + first pairing (BLE) | `GAP_EVENT_ADVERTISING_REPORT` matches a known profile or generic-HID heuristic | `packet_handler()` | `hid_state.pending_*` set; `btstack_host_connect_ble()` called | **Pico-initiated** (direct `gap_connect()` to the advertised address) | scan stopped | 3 |
| 2b | Discovery + first pairing (Classic) | `GAP_EVENT_INQUIRY_RESULT` matches gamepad/joystick/Wiimote COD or profile | `packet_handler()` | `classic_state.pending_*` set; `hid_host_connect()` or direct L2CAP | **Pico-initiated** | inquiry stopped | 3 |
| 3 | Connected, HID readiness | `HCI_SUBEVENT_LE_CONNECTION_COMPLETE` (status 0) → `register_switch2_hid_listener()`/`sm_request_pairing()`/`start_hids_client()`; or `HID_SUBEVENT_CONNECTION_OPENED` for Classic | `packet_handler()` / `hid_host_packet_handler()` | `ble_connection_t`/`classic_connection_t` slot filled; for `BT_BLE_CUSTOM` (Switch2), `switch2_send_next_init_cmd()` starts the GATT init state machine (`SW2_INIT_READ_INFO → PAIR_STEP1-4 → SET_LED → DONE`) | — | `hid_state.reconnect_attempts = 0`, `has_last_connected = true` (3 separate confirmed reset sites) | 4/7 |
| 4 | Controller-initiated disconnect / radio loss | `HCI_EVENT_DISCONNECTION_COMPLETE` | `packet_handler()` | `reason` byte captured (now consulted, see fix above); `switch2_cleanup_on_disconnect()` unconditionally resets `sw2_init_state=IDLE` before the reconnect decision | Either | GATT listeners unregistered, HIDS/BAS clients disconnected, connection slot memset | 5 or "resume scan" |
| 5 | Controller attempts to reconnect | Controller re-advertises (BLE) or re-pages (Classic) | Radio/controller-side, outside this project's control | — | **Controller-initiated** | — | 2 (BLE: only if scanning is active — see root cause) / 2b |
| 6 | Pico initiates a connection | Same as row 2 — the *only* Pico-initiated paths are scan-triggered auto-connect and the post-disconnect reconnect cascade (row 4→2) | `packet_handler()` | — | **Pico-initiated** | — | 3 or failure→7 |
| 7 | Failed auth / timeout / cancel / retry | `HCI_SUBEVENT_LE_CONNECTION_COMPLETE` (status≠0), or the `BLE_CONNECT_TIMEOUT_MS` safety-net in `btstack_host_process()` calling `gap_connect_cancel()` | `packet_handler()` / `btstack_host_process()` | `reconnect_attempts` incremented (capped at 5); `reconnect_attempt_time` cleared on cancel | — | — | retry (4→2) or fallback scan |
| 8 | Intentional shutdown/unpair/forget | BOOTSEL triple-tap → `wipe_all_devices()` (`ns2_bt_host.c`) → `btstack_host_disconnect_all_devices()` + `btstack_host_delete_all_bonds()` | `ns2_bt_host.c` control-tick handler | `has_last_connected=false`/`reconnect_attempts=0` set **synchronously** inside `delete_all_bonds()`, which runs before the async `HCI_EVENT_DISCONNECTION_COMPLETE` for each torn-down link arrives — so the reconnect cascade never fires for a wipe, by call-order, not by an explicit guard | User (BOOTSEL) | `le_device_db` bonds removed, link keys cleared | 1 |

### Answers to the required questions (Phase 1)

- **Does PicoSwitch2 actively reconnect, or only wait?** Both, depending on path: BLE does an
  **active, Pico-initiated** direct reconnect cascade after every qualifying disconnect (the one
  just fixed); Classic BT only becomes connectable/discoverable and waits, plus resumes inquiry —
  no owned outbound retry loop there. **Confirmed** (row 2/2b/4 above).
- **Is there a repeated connection-attempt loop this project owns?** **Yes, BLE only** — up to 5
  sequential `gap_connect()` attempts, each independently bounded by `BLE_CONNECT_TIMEOUT_MS`
  (10 s). **Confirmed.**
- **Could the symptom come from scan/page-scan availability rather than a "cooldown" issue?**
  **Yes — this was the actual finding.** It's not that retries were too *frequent* (they're
  serialized, 10 s apart); it's that the retry cascade **starves scanning** for up to 50 s per
  disconnect, and fired even when scanning (not direct-connecting) was the only thing that could
  have worked. **Confirmed**, and it's why a naive "add a cooldown timer" would have been the
  wrong shape of fix — cooldown throttles *frequency*; this defect was about *unconditional
  engagement* and *scan exclusivity*, not frequency.
- **Global or per-device reconnect state?** **Global, confirmed** —
  `hid_state.last_connected_addr`/`reconnect_attempts`/`has_last_connected` are single fields, not
  a per-device table. With more than one BLE device ever paired, only the most-recently-connected
  one gets the owned reconnect cascade; others rely on passive scan discovery only. **Not changed
  this pass** — real, but a materially larger change (a per-device table) than the evidence
  currently justifies; this project's BLE-capable roster (Switch2 Pro/Joy-Con2, plus whichever
  generic BLE HID devices match) is typically small, and DATA.md's explicit guidance is to prefer
  one well-supported fix over speculative expansion.
- **Idempotent disconnect/failure callbacks; can a stale callback tear down a newer connection?**
  Reviewed, not exhaustively proven: `find_connection_by_handle(handle)` keys off BTstack's own
  `hci_con_handle_t`, which BTstack does not reuse until a slot is fully torn down — standard
  BTstack invariant, not re-verified from first principles in the vendored HCI source this pass.
  **Strong Evidence**, not **Confirmed**.
- **Timers wrap-safe, units/cadences explicit?** Mixed at the time of the previous pass. The
  connect-timeout/scan-timeout checks use `btstack_run_loop_get_time_ms()` deltas (`uint32_t`
  subtraction — wraps safely at ~49.7 days). `switch2_retry_init_if_needed()` had a genuine cadence
  defect, since fixed — see "Switch 2 GATT init retry timing" below.
- **Does success reset all failure/backoff state?** **Confirmed, yes** — `reconnect_attempts = 0`
  at three independent, correctly-placed sites (`btstack_host_save_last_connected()` and two
  further success paths in `packet_handler()`), all reached only on an actual successful
  connection/pairing outcome.
- **Are intentional disconnects distinguished from failures?** **This was the defect — now fixed**
  (root cause above).
- **Are stored link keys/identity preserved correctly across reboot?** `btstack_host_restore_last_connected()`
  runs on every `HCI_STATE_WORKING` (i.e. every boot) and `le_device_db`/BTstack's TLV flash
  storage persists bonds independent of this project's own `last_connected_*` fields (which are a
  separate, in-RAM "who to proactively reconnect to" hint, not the bond store itself). **Confirmed**
  structurally; not independently re-verified against a real flash read this pass.
- **Is the symptom connection failure, or successful-connection-but-no-input?** This pass's
  evidence supports **connection failure** (the link itself doesn't come back promptly) as the
  primary, code-provable mechanism. A "connected but the GATT init handshake never completes"
  failure mode was considered and **structurally ruled out** as a *separate* contributor:
  `switch2_cleanup_on_disconnect()` unconditionally resets `sw2_init_state = SW2_INIT_IDLE` on
  every BLE disconnect (row 4 above, before the reconnect decision), and every fresh
  `HCI_SUBEVENT_LE_CONNECTION_COMPLETE` success for a `BT_BLE_CUSTOM` profile unconditionally calls
  `register_switch2_hid_listener()` → the init state machine restarts cleanly every time. No
  evidence of a stale/stuck GATT-handshake state surviving across a reconnect.

### Phase 2 — BTstack capability findings (from the vendored source, not assumed)

- **Outbound connection:** `gap_connect()` (BLE) / `hid_host_connect()` + `gap_connect()` (Classic,
  direct-L2CAP path) create connections; `gap_connect_cancel()` cancels a pending one — both
  already used by this project's own timeout safety-nets.
- **Connection/disconnection events:** `HCI_SUBEVENT_LE_CONNECTION_COMPLETE` (BLE) and
  `HCI_EVENT_DISCONNECTION_COMPLETE` (both transports) carry a `status`/`reason` byte respectively
  — both already parsed by this project; only the latter was previously left unused for BLE, now
  fixed.
- **BR/EDR-specific:** `gap_discoverable_control()`/`gap_connectable_control()` (already used, set
  at `HCI_STATE_WORKING`); page-scan type/interval/window are **not** touched anywhere in this
  project's own code — no evidence gathered this pass on whether BTstack's Pico-SDK CYW43 build
  even exposes a page-scan-tuning API distinct from `hci_send_cmd()` with raw HCI opcodes (would
  need a dedicated follow-up if the Classic path is ever found to need it — it currently shows no
  equivalent defect).
- **Do not conflate BLE scan parameters with BR/EDR page scan** (explicit caution from the task
  brief) — this project's fix touches neither; it only gates an existing BLE `gap_connect()` call
  behind a reason check, no scan-parameter APIs of any kind were touched.
- **Bonding:** `le_device_db`/`le_device_db_tlv` (BLE) and BTstack's classic link-key TLV storage
  are both wired up (`setup_tlv_storage()`); `gap_set_bondable_mode(1)` and
  `gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT)` are set at boot.

### Comparison evidence from `Dycool/NS-PC-Control` (Strong Evidence only — informed the search, did not determine the fix)

Re-audited 2026-07-12 against a fresh clone (commit `a422f4b`; the `joycon-usb-experiments` branch
cited 2026-07-10 has since merged into `main` — full detail:
`docs/experiments/ns-pc-control-audit-2026-07-12.md` §5). Their `server/src/bluetooth_manager.cpp`
(BlueZ on Linux, a structurally different stack) uses:

```
[General] FastConnectable = true
[BR]      PageScanType = 1 ; PageScanInterval = 128 ; PageScanWindow = 48
[Policy]  ReconnectUUIDs = 00001124-...,00001812-...
          ReconnectAttempts = 15
          ReconnectIntervals = 1,1,1,2,2,2,4,4,8,8,16,16,32
          AutoEnable = true
```

plus an application-level per-device proactive-reconnect cooldown of exactly 5 seconds
(`RECONNECT_COOLDOWN = std::chrono::seconds(5)`), and an explicit disconnect-on-console-suspend.
**None of this was portable or determined this pass's fix** — BlueZ's `main.conf` mechanism has no
BTstack equivalent, and this project's actual defect (unconditional retry regardless of disconnect
reason, and the resulting scan-starvation) is a different shape of problem than "attempts too
frequent," which is what a cooldown timer would address. Kept here as comparison context per
DATA.md's instruction not to treat another implementation's policy as proof of this project's wire
behavior. If BTstack is ever found to expose a genuine fast-reconnect/page-scan-tuning equivalent,
NS-PC-Control's values are a reasonable starting point to test against, not to copy blind.

### Current validation status

The user-facing bonded reconnect path is now hardware-confirmed across genuine Pro Controller 2,
Joy-Con 2, DualSense, Xbox, and other tested families. The exact intentional-disconnect reason log
and an induced supervision-timeout branch were not separately captured. The original targeted
procedure is retained below for future fault-injection work:

1. **Direct fix validation:** pair a genuine Switch 2 Pro Controller (or Joy-Con 2). Deliberately
   power it off via its own controls (not BOOTSEL triple-tap — that's this project's own wipe
   gesture and is a different code path, already unaffected either way). Confirm the dongle's
   serial/debug log shows `"Disconnect reason 0x13/0x15 looks intentional... resuming scan
   instead"` rather than 5 rounds of `"Attempting BLE reconnection..."`. Then power the controller
   back on and confirm it reconnects promptly (should no longer need multiple tries).
2. **Regression check on the "should still retry" path:** move the same controller out of BT range
   momentarily (or otherwise induce a supervision timeout) rather than powering it off; confirm the
   existing 5-attempt direct-reconnect cascade still engages (reason should be
   `CONNECTION_TIMEOUT`, not one of the two intentional-disconnect codes) — this path must remain
   unchanged.
3. **Broader symptom check:** repeat the original informal test that produced the "sometimes needs
   a triple-tap" report, several times, and note whether it's now consistently one-shot. This is
   the only step that can actually confirm the user-facing symptom improved, not just that the code
   path traced correctly.

### Not changed this pass, and why

- **Per-device (not global) reconnect state** — real limitation, but a larger change than this
  evidence justifies; not the mechanism behind the reported single-controller "Pro 2" symptom.
- **BlueZ-style page-scan/fast-connectable tuning for BTstack** — no evidence yet that BTstack's
  Pico-SDK build even exposes an equivalent API; would need dedicated research before any code
  change, and the Classic path shows no corresponding defect to justify it right now.
- **Any change to the Classic BR/EDR reconnect path** — traced and found to already behave
  correctly (no blind cascade, resumes scanning on empty connection count); no defect found, so
  nothing to fix.

## Switch 2 GATT init retry timing (🟢 fixed; normal path hardware-confirmed)

> A *different* mechanism from "Reconnect reliability" above: this section is about the
> **post-link vendor/GATT command sequence** that pairs and configures a Switch 2 Pro Controller /
> Joy-Con 2 after the BLE link is already connected — `switch2_send_init_cmd()`/
> `switch2_retry_init_if_needed()`/`sw2_init_state`, not `gap_connect()`.

### Proven scheduling/call-frequency result (Phase 1)

There is no function literally named `btstack_host_task()` in this codebase — the actual chain,
traced call site by call site (not inferred):

`ns2_bt_host.c`'s `control_timer_handler()` (a BTstack run-loop timer, `CONTROL_TICK_MS = 30`,
**self-rescheduling**: it re-arms its own next firing 30ms after each invocation *starts*, so 30ms
is a floor, not a guaranteed period — a busy run loop can only push the real interval slower, never
faster) → calls `bt_task()` (inline wrapper in `bt_transport.h`) → `bt_transport->task()`, which for
this project's only compiled transport is `cyw43_transport_task()` (`bt_transport_cyw43.c`) →
calls `btstack_host_process()` → which unconditionally calls `switch2_retry_init_if_needed()` as one
of several periodic tasks. **Confirmed single call path** — grepped for every other possible caller
of `switch2_retry_init_if_needed()`, `btstack_host_process()`, and `cyw43_transport_task()`; each
has exactly one call site. `BTSTACK_USE_ESP32`/`BTSTACK_USE_NRF` transport branches exist in the
vendored source but are not compiled by this project's `CMakeLists.txt` (`pico_cyw43_arch_none`
only) — dead code for this build, not a real alternate path.

**Answering the numbered questions:**
1. **What determines call frequency?** The self-rescheduling `control_timer`, nominal 30ms.
2. **Fixed, event-driven, transport-, or load-dependent?** Nominally fixed with a floor (can't fire
   faster than 30ms apart); not event-driven (doesn't fire in response to BT traffic); not
   meaningfully transport-dependent for this project (one transport compiled in); can run slower
   under a busy run loop, never faster.
3. **Differ across build configs/transports?** No — `CONTROL_TICK_MS` is a single unconditional
   `#define`; `NS2_AUDIO`/`NS2_PRO` don't touch `src/bt_hid/**`'s scheduling.
4. **Is "500ms" evidence-backed?** **No primary source found** (Phase 2, below) — treated as an
   existing, unverified project policy, not a fact.
5. **Idempotent retries for every state?** The retry re-sends whatever `switch2_send_init_cmd()`
   builds for the *current* `sw2_init_state` — since transport/GATT event processing happens before
   the retry check within each `btstack_host_process()` call (confirmed by reading the function
   body in order), a genuine ACK that already arrived is processed and advances state *before* the
   retry check runs in the same tick, so a retry (old or new code) sends the command matching
   whatever state is current, not a stale one. **Strong Evidence** the retries are effectively
   idempotent in practice; not proven for every controller-side edge case (e.g. whether the
   controller itself tolerates an unexpected duplicate of an already-acked step) since that's
   real-controller behavior, not something this project's own code can determine offline.
6. **Can ACK arrival race with a retry?** Structurally narrow: `btstack_host_process()` processes
   transport/GATT events *before* calling `switch2_retry_init_if_needed()` in the same call, and
   this project uses `pico_cyw43_arch_none` (explicit `cyw43_arch_poll()`, not an interrupt-driven
   background/threadsafe arch) — no evidence of true concurrent re-entrancy into these functions.
   **Strong Evidence**, not exhaustively proven against BTstack's deepest internals.
7. **Does the counter persist across disconnects/sessions?** **Confirmed, yes, before this fix** —
   `retry_counter` was a `static` inside the function, incremented every call, never reset anywhere
   (not on a state transition, not on disconnect, not on a new connection). This is a *second*,
   independent defect from the Hz-assumption error (see "Old vs. new semantics" below).
8. **Are retries infinite? What recovers usability?** **Confirmed, before this fix: infinite, and
   nothing recovered it.** No cap existed; a controller that never advanced past one init step would
   retry that step forever, with no fallback disconnect/rescan — a real, previously-undiagnosable
   permanently-stuck-connection failure mode, structurally different from (and not covered by) the
   separately-fixed link-layer reconnect cascade, which only engages on an actual disconnect event
   that would never happen here.
9. **Can a stale `sw2_init_handle` target a closed/reused handle?** `sw2_init_handle` is set in
   `register_switch2_hid_listener()` (on connect) and zeroed in `switch2_cleanup_on_disconnect()`,
   called synchronously and unconditionally within the same `HCI_EVENT_DISCONNECTION_COMPLETE`
   handling this project's own code runs — no separate call context where a disconnect could be
   processed without also zeroing this. `switch2_retry_init_if_needed()` already checked
   `sw2_init_handle != 0` before this fix. **Confirmed no meaningful staleness window.**
10. **Does a successful response reset the retry epoch, or can a new state inherit a
    near-expired counter?** **Confirmed, before this fix: no reset — a new state inherited whatever
    phase the global call-counter happened to be at**, from a fresh ~1.8s runway down to
    effectively zero, depending on when in its 60-call cycle the previous state happened to
    complete. This is the second, structurally distinct defect this fix addresses, not just the
    Hz-assumption error.

### Old vs. new retry semantics

| | Before | After |
|---|---|---|
| Timing basis | Raw call-count modulo 60, assumed ~120Hz caller | `btstack_run_loop_get_time_ms()` deadline, `SW2_INIT_RETRY_INTERVAL_MS = 500` |
| Real interval | ~1.8s (actual ~33Hz caller ÷ intended 60 counts) | ~500ms, wall-clock, regardless of caller frequency |
| Deadline scope | Global counter, never reset — a new state could inherit anywhere from ~0 to the full ~1.8s runway depending on caller-count phase | Reset every time a command is sent (fresh or retry) — set inside `switch2_send_init_cmd()` itself, so both the *first* send for a state and every retry refresh it identically |
| Cross-session | Counter never reset on disconnect/new connection — phase carried over indefinitely | Explicitly reset in `switch2_cleanup_on_disconnect()` (belt-and-suspenders with the state-change check below) |
| Retry bound | None — infinite | `SW2_INIT_MAX_RETRIES = 10` (~5s of retries) |
| Recovery on exhaustion | None — permanently stuck, no diagnosis | `gap_disconnect()` on the stuck handle — composes with the reconnect-reliability fix above (a locally-initiated disconnect isn't one of the two "peer disconnected on purpose" reason codes that fix excludes, so the existing reconnect cascade engages automatically) |
| Observability | One `printf` per retry, attempt number only | `printf` on every retry (state, attempt/max, elapsed ms) and on recovery-disconnect (state, retry count, total elapsed) |

### Interval provenance (Phase 2) — 🔵 Hypothesis / project policy, not evidence

Searched this repository for a primary source establishing 500ms specifically: raw captures, code
comments, and the `Dycool/NS-PC-Control` audit from the same day (no equivalent GATT-init retry
timer documented there — their pairing flow is over BlueZ/a different transport entirely and
wasn't examined at this level of detail). **No source found.** Every "BlueRetro" reference in this
codebase (the cited origin of the *state sequence* and the pairing "magic bytes" payloads) is about
*what* to send, never *how often to retry if unacknowledged* — BlueRetro's own source was not
available to check this pass (external project, not vendored, not re-cloned this session). Per
DATA.md's explicit fallback: **treated as an existing, unverified project policy** — the interval
is preserved at its original ~500ms intent (`SW2_INIT_RETRY_INTERVAL_MS`), now actually delivered
at that real-world value instead of ~1.8s, but the *value itself* remains 🔵 Hypothesis-tier, not
Confirmed. If a real capture ever shows the genuine console/host's own retry timing for this
handshake, that should supersede this constant.

### The fix (Confirmed, build-verified, not yet hardware-validated)

`src/bt_hid/bt/btstack/btstack_host.c`: `switch2_send_init_cmd()` now records
`sw2_init_cmd_sent_ms = btstack_run_loop_get_time_ms()` on every send and resets
`sw2_init_retry_count` when `sw2_init_state` differs from the last state a command was sent for.
`switch2_retry_init_if_needed()` now checks `now - sw2_init_cmd_sent_ms >=
SW2_INIT_RETRY_INTERVAL_MS` (a real elapsed-time deadline, wrap-safe via unsigned subtraction —
correct up to a ~24.8-day true elapsed time, which no single init attempt can approach) before
retrying, bounded at `SW2_INIT_MAX_RETRIES = 10`, with an explicit `gap_disconnect()` recovery
transition on exhaustion. `switch2_cleanup_on_disconnect()` explicitly resets the new state too.
No unit-test seam exists for this timer/state logic in this project (no host-side test harness for
BTstack-dependent code) — validated by code-path reasoning (above) and the build matrix below;
constructing a fake-BTstack framework solely for this change was judged disproportionate per
DATA.md's own guidance.

### Current validation status

Normal genuine Pro Controller 2 and Joy-Con 2 pairing, initialization, input, motion, and reconnect
are hardware-confirmed. The deliberately stalled-ACK/retry-exhaustion backstop has not been forced
on hardware. The original fault-injection procedure remains:
1. Pair a genuine Switch 2 Pro Controller / Joy-Con 2 and watch the debug log during the init
   handshake — confirm each state's command is sent once promptly, and (if a step is ever slow)
   that a retry fires close to 500ms later, not ~1.8s.
2. If feasible, artificially stall one step (e.g. briefly interrupt the link at the BTstack level,
   if this project has a way to do that, or observe a naturally slow real-world pairing) and confirm
   the retry count increments correctly and recovery-disconnects after 10 attempts (~5s) rather than
   retrying forever.
3. Confirm normal pairing is not slowed or made less reliable by this change — the common case
   (ACK arrives promptly) should behave identically to before, just with correct backstop timing.

### Not changed this pass, and why

- **The 500ms value itself** — no evidence found either way; preserved as project policy, not
  promoted to fact (see Phase 2 above).
- **A deterministic host-side unit test** — no proportional test seam exists for BTstack-coupled
  timer/state logic in this project; validated by reasoning + build + a planned hardware pass
  instead, per DATA.md's own explicit allowance for this case.
- **The global-single-BLE-device architecture** — not touched; this fix operates entirely within
  the existing per-connection `sw2_init_state`/`sw2_init_handle` scope, which DATA.md's own scope
  boundary said not to expand without evidence this task specifically required it (it didn't).

## Pairing window vs in-flight connect (🟢 fixed; normal path hardware-confirmed)

**Reported symptom**: a genuine Switch 2 Pro Controller paired reliably when the dongle was left in
its default auto-reconnect/scan state, but was "weird" — frequently failed to finish — specifically
when the owner used the explicit BOOTSEL-double-tap pairing gesture. Xbox, DualSense, and a generic
(non-Nintendo) Switch Pro Controller all paired normally either way; only the Switch 2 Pro
Controller over BLE, only in explicit pairing mode, was unreliable. See
`docs/experiments/gate2-identity-log-hardware-captures-2026-07-12.md` "Capture 2" for the
`btid dump` capture that independently confirmed this controller's identity/driver resolution is
otherwise clean — the bug traced here is a pairing-window timing defect, not an identity or
driver-binding problem.

### Root cause (Confirmed, traced end to end in `btstack_host.c`)

`ns2_bt_host.c`'s pairing window (`PAIRING_WINDOW_MS`, was 10000) closed unconditionally on expiry:
`bt_set_pairing_mode(false)` → `cyw43_transport_set_pairing_mode(false)` →
`btstack_host_stop_scan()`, regardless of what the BT host was doing at that instant.
`btstack_host_stop_scan()` unconditionally executes `hid_state.state = BLE_STATE_IDLE;` — it does
**not** call `gap_connect_cancel()` and does not touch any pending-connection or per-connection
state, so it does not literally cancel an outstanding `gap_connect()`. What it corrupts instead is
bookkeeping: `hid_state.state` is read in exactly six places in the whole file (grep-verified) — the
`BLE_CONNECT_TIMEOUT_MS` watchdog, the idle safety-net, a scan/state resync check, the periodic
bonded-reconnect trigger, and the two new-device admission gates (BLE advertising handler, Classic
inquiry-restart handler). None of the six read past the raw `gap_connect()` phase — GATT discovery,
SM pairing/bonding, HID setup, and Switch 2's own GATT init (`switch2_retry_init_if_needed()`, see
"Switch 2 GATT init retry timing" above) all key off **per-connection** state
(`ble_connection_t.state`, `conn->hid_ready`) instead, and are already unaffected by scan/inquiry
being stopped mid-setup.

The one phase that *is* broken: while `hid_state.state == BLE_STATE_CONNECTING` (a `gap_connect()`
genuinely outstanding), an external `stop_scan()` call resets state to `IDLE` — and the
`BLE_CONNECT_TIMEOUT_MS` watchdog (10s) requires `state==CONNECTING` to fire. Once disarmed, a
stuck or slow-to-respond connection attempt has no software-side recovery left; whatever happens
next depends entirely on the CYW43/BTstack HCI layer's own (unknown, and not necessarily bounded to
anything close to 10s) internal handling. This is a plausible, well-evidenced explanation for
"pairing mode sometimes just doesn't finish" that wouldn't reproduce outside explicit pairing mode,
since only the pairing window's expiry timer calls `stop_scan()` at an arbitrary moment mid-connect
— the app's own default reconnect/scan logic never externally interrupts its own in-flight attempt
this way.

**Classic BT was traced too and found not vulnerable.** Its connection-establishment watchdog
(`CLASSIC_CONNECT_TIMEOUT_MS`, 15s) is keyed on each `classic_connection_t`'s own `connect_time`,
set the instant `hid_host_connect()` succeeds — completely independent of `hid_state.state`,
`scan_active`, or `inquiry_active`. Stopping Classic inquiry mid-connect doesn't touch it. No
Classic-side defer logic was needed.

### The fix (Confirmed, build-verified, not yet hardware-validated)

New `btstack_host_close_pairing_window()` (`btstack_host.c`) replaces the direct
`btstack_host_stop_scan()` call in `cyw43_transport_set_pairing_mode(false)`. If
`hid_state.state == BLE_STATE_CONNECTING`, it defers — sets a `pairing_close_deferred` flag and
returns without touching scan/inquiry state — instead of closing immediately. The defer resolves
(performs the real `btstack_host_stop_scan()`) at the two points the raw connect attempt concludes,
both inside `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`:

- **Success** (`status == 0`): resolved right after `hid_state.state = BLE_STATE_CONNECTED`. From
  this point on `stop_scan()`'s state reset is inert (per the six-read-site trace above), so it's
  safe to close immediately — GATT discovery, bonding, HID setup, and Switch 2 GATT init all proceed
  independently regardless of when exactly scan/inquiry stop.
- **Failure** (`status != 0`, including the watchdog's own `gap_connect_cancel()`, which the existing
  design already generates a failure event for): resolved at the top of the failure branch. If the
  window really had expired (`pairing_close_deferred` was set) and this wasn't a reconnect to an
  already-bonded device (`hid_state.pending_addr != hid_state.last_connected_addr`), the handler
  closes cleanly and skips the generic "resume scanning" retry path below it — otherwise (bonded
  reconnect, or the window hadn't actually expired) today's existing retry/resume-scan behavior runs
  completely unmodified. This distinction exists because the generic failure handler *unconditionally
  resumes scanning* for any new (non-bonded) candidate that fails — correct today only because that
  branch was previously unreachable after a genuinely-expired window; the defer mechanism makes it
  reachable, so it needs this explicit guard to honor "close cleanly if expired" (point 6 below)
  without changing bonded-reconnect behavior (point 8).

**Grace-period bound**: not an invented constant. The defer can only outlive the connection by, at
most, the already-existing `BLE_CONNECT_TIMEOUT_MS` (10s) — the watchdog that disarms if
`hid_state.state` gets corrupted is exactly the mechanism this fix keeps armed, so the bound is
whatever that watchdog already enforces. Once past raw-connect, closure is immediate (not deferred
further), so GATT/HID setup and Switch 2's own init retry bound (`SW2_INIT_MAX_RETRIES=10 ×
SW2_INIT_RETRY_INTERVAL_MS=500ms` ≈ 5s worst case, see "Switch 2 GATT init retry timing" above) were
never actually at risk from this bug and don't need their own defer logic.

`open_pairing_window()` (`ns2_bt_host.c`) also now declines to re-arm scanning
(`btstack_host_start_scan()` would itself overwrite `hid_state.state`) while
`btstack_host_pairing_close_deferred()` is true — guards the edge case of a second BOOTSEL
double-tap landing in the few-second grace window right after a previous window's deadline, which
would otherwise reintroduce the identical class of bug for whichever attempt is still finishing.

`PAIRING_WINDOW_MS` was also widened 10000 → 30000 for usability, per explicit instruction — this is
secondary; it does not fix the bug (the defer mechanism does) and was validated by design, not by
being the load-bearing part of this change.

### Semantic checklist (traced against the 8-point spec that drove this fix)

1. No candidate selected at expiry → `hid_state.state` isn't `CONNECTING` → closes immediately,
   unchanged from before. ✅
2. Candidate admitted, connect begins → scan/inquiry already stopped by `btstack_host_connect_ble()`
   itself at attempt start (pre-existing), so no unrelated device can be admitted while one is in
   flight — nothing new needed here. ✅
3. GATT/HID/Switch 2 init proceed independently of the discovery deadline → true by construction,
   confirmed via the six-read-site trace (none of them gate post-CONNECTED progress). ✅
4. Bounded by existing timeouts, not left open indefinitely → bounded by `BLE_CONNECT_TIMEOUT_MS`
   (pre-CONNECTED) and closes immediately once CONNECTED (post-CONNECTED phases were never at risk).
   ✅
5. Success closes pairing mode/state immediately → `resolve_deferred_pairing_close()` fires the
   instant raw-connect succeeds. ✅
6. Failure resumes for the unused remainder, or closes cleanly if expired → handled by the
   `pairing_window_expired && !is_bonded_reconnect` branch in the failure handler. ✅
7. No stale-callback leakage into a later session → `pairing_close_deferred` is a single flag,
   always resolved (never left set) by the success/failure paths that are the only ways out of
   `BLE_STATE_CONNECTING`. ✅
8. Bonded-reconnect behavior preserved → explicitly exempted via the `is_bonded_reconnect` address
   comparison in the failure handler, and Classic BT (used by no bonded-reconnect logic touched here)
   was confirmed unaffected by tracing, not by adding unneeded code to it. ✅

### Current validation status

Explicit pairing windows, new pairing, bonded reconnect, and the 30-second UX are hardware-confirmed
with genuine Switch 2 controllers and the broader controller matrix. The exact old-boundary and
forced-failure cases below were not isolated as dedicated fault-injection tests:

1. If the Switch 2 Pro Controller is already bonded from a prior session, wipe only that bond
   (BOOTSEL triple-tap wipes *all* bonds — there's no per-device wipe command yet, so either accept
   re-pairing everything, or skip straight to step 3 to test reconnect-of-an-existing-bond instead of
   fresh pairing).
2. Double-tap BOOTSEL to open the (now 30s) pairing window, then power on/wake the Switch 2 Pro
   Controller so it starts advertising. Repeat several times across separate windows — this is the
   scenario that was previously unreliable.
3. Time one attempt to land near the *old* 10s boundary specifically (e.g. delay powering on the
   controller until ~8-9s into the window) — this is the exact case that used to corrupt
   `hid_state.state` mid-connect; confirm it now completes instead of hanging.
4. Open a pairing window and let it expire with **no controller** present — confirm it still closes
   at the (new, 30s) deadline with no hang and the LED returns to idle/connected display correctly.
5. If feasible, force a failed candidate (e.g. briefly power off the controller mid-connect) —
   confirm the window either resumes scanning (if time remains) or closes cleanly (if expired)
   without an infinite retry loop, per the log lines `"Pairing candidate failed after window
   expiry -- closing cleanly, not resuming scan"` / `"Deferred pairing-window close: candidate
   resolved, closing now"`.
6. With the Switch 2 Pro Controller already bonded, confirm normal automatic reconnect (no BOOTSEL
   gesture at all) still behaves exactly as before — this path was deliberately left untouched.
7. Re-capture `btid dump`/`btid stat` after a successful pairing-mode connection and confirm the
   identity resolution is still clean (single `initial-bind`, `dropped:0`), matching Capture 2.

### Not changed this pass, and why

- **Classic BT admission latching** — traced and found not vulnerable to this specific bug (its own
  watchdog is independent of `hid_state.state`); adding defer logic there would be unneeded
  complexity for a problem that doesn't exist. If a real Classic-side symptom ever surfaces, retrace
  first rather than assuming this fix's BLE-side reasoning transfers directly.
- **Per-device bond wipe** — BOOTSEL triple-tap still wipes every bond; the hardware validation
  procedure above works around this rather than expanding scope to add one.
- **`PAIRING_WINDOW_MS`'s exact value (30s)** — chosen for usability per explicit instruction, not
  derived from any protocol constant; treat it as adjustable UX, not load-bearing.

## BLE wake-from-sleep (implemented and hardware-confirmed 2026-07-16)

`docs/wakeup.md` in the same external repo documents how the Switch 2 actually wakes from BLE: a
setup wizard captures a **real, already-paired Joy-Con 2's own HOME-button BLE advertisement**
(its MAC address + the full raw advertising payload) into a config file, then replays that exact
advertisement later via raw HCI on the chosen adapter. **Re-checked 2026-07-12 against the current
shipped feature** (previously described from an older/different source): the actual implementation
is **simpler than previously characterized here** — no mention of stopping `bluetoothd`, disabling
BlueZ, or spoofing the adapter's own public address via `btmgmt public-addr`; it's a captured
MAC + advertising payload replayed via a chosen `hci` device, apparently alongside normal BlueZ
operation. Full detail: `docs/experiments/ns-pc-control-audit-2026-07-12.md` §6.

Previously filed flatly out of scope here on the reasoning that "this project's dongle has no such
device to capture from." That assumption was wrong. The implementation now learns the console
address from the completed USB `0x15` pairing exchange, persists it after the timing-sensitive
handshake, and uses a timer-driven BTstack advertiser that temporarily pauses discovery without
disconnecting controllers or blocking the run loop. Automatic wake from the first real post-sleep
controller input is confirmed on a real Switch 2. See
**`docs/bluetooth/wake-from-sleep-design.md`** for the byte-exact payload, state machine, safety
gates, and validation record.

## Genuine Pro Controller 2 native motion (hardware-confirmed 2026-07-21)

The console/UART bridge, live GATT discovery, and controlled BLE captures resolved the production
sequence for PID `057E:2069`: configure features `0x27`, perform the console's six calibration
reads, enable `0x27`, write `{0x85,0x00}` to descriptor `0x0010`, subscribe to report `0x000E`, and
request a 7.5 ms central-side connection interval. The resulting native stream runs at about
133 Hz and interleaves length-30 and length-40 motion PDUs.

This path starts automatically only in the Pro Controller 2 USB personality, after a 250 ms
post-init guard. UART-selected experiment variants and GATT discovery suppress it. Because
enabling `0x000E` stops the controller's ordinary `0x000A` report, the host normalizes buttons and
sticks from the new report while transporting its motion block opaquely across cores. Each motion
snapshot carries its Bluetooth source slot, and the USB side also requires output slot 0 to retain
the genuine Pro2 VID/PID before consuming it. Splatoon 3 validates axes, stationary behavior,
power-cycle, reconnect, and source-off hold. Full evidence:
[`native-pro2-motion-passthrough-2026-07-21.md`](../experiments/native-pro2-motion-passthrough-2026-07-21.md).
