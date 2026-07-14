# Gate 2 identity-log hardware captures — 2026-07-12

Raw `btid dump`/`btid stat` output from real controller sessions, preserved verbatim (with the
retrieval procedure and the board/firmware context) so this evidence survives independent of
`DATA.md`, chat history, or terminal scrollback. Analysis lives in
`docs/bluetooth/btstack-implementation.md` "Gate 2" and the per-driver compatibility notes; this
file is the source-of-truth raw data those documents summarize.

## Test procedure (established this session)

1. Plug the Pico into a PC via USB. Cold boot defaults to switch-controller mode (HID-only).
2. Pair/reconnect the target controller. BT pairing works independent of the console — the CYW43
   radio doesn't care what's on the USB port.
3. Hold BOOTSEL ~5s on the same still-powered board — live USB re-enumeration into config mode
   (CDC), not a reboot. Core1 and the BT connection are untouched, so the RAM-resident
   `bt_identity_log` ring survives.
4. Open the resulting `PICOSWITCH`/`USB Serial Device` COM port (this session: opened directly via
   PowerShell's `System.IO.Ports.SerialPort`, 115200 8N1, `\r\n` line endings — matches
   `config.c`'s `config_cdc_task()` line reader and `reply()`'s framing exactly).
5. Send `btid dump` (repeat while `"more":true`), then `btid stat` to check `dropped`.

## Board / firmware context

- Board: Pico 2 W (`pico2_w`), built via `./build.ps1` at the commit state described in this
  session's `DATA.md` handoff (Gate 2 identity/transport-mask work, pre-hardware-validation).
- Config-mode USB identity confirmed via Windows device enumeration:
  `USB\VID_CAFE&PID_4012&MI_00` → `COM6`, matching the documented config-mode descriptor
  (`0xCAFE:0x4012`).

## Capture 1 — 8BitDo NGC Modkit, Classic BT

**Before the identity-log fix below** (three logging-completeness bugs found from this exact
capture, fixed same session — see `docs/bluetooth/btstack-implementation.md` "Gate 2" for the fix
detail): descriptor-arrived events hardcoded `provenance:"unknown"` and `cod:"000000"` regardless
of actual state, and the moment SDP VID/PID resolved while staying on the generic driver had no
log event of its own. None of this affects BT functionality — logging only. The raw capture below
is still valid data; only its `provenance`/`cod` fields for the second event should be read as
"not yet reliable" rather than "confirmed zero/unknown" — the actual VID/PID/descriptor
length/fingerprint/driver fields are accurate.

```json
{"dropped":0,"entries":[
{"ms":29755,"conn":0,"transport":"classic","name":"8BitDo NGC Modkit","vid":"0x0000","pid":"0x0000","provenance":"unknown","cod":"082500","desc_len":0,"desc_fp":"0x0000","driver":"Generic BT Gamepad","reason":"initial-bind","slot":-1},
{"ms":29924,"conn":0,"transport":"classic","name":"8BitDo NGC Modkit","vid":"0x2DC8","pid":"0x286A","provenance":"unknown","cod":"000000","desc_len":86,"desc_fp":"0x1F98","driver":"Generic BT Gamepad","reason":"descriptor-arrived","slot":-1}
],"more":false}
```

`btid stat` → `{"dropped":0}`. Immediate re-`dump` → `{"dropped":0,"entries":[],"more":false}`
(drains cleanly, no duplication).

**Reading**: `cod:082500` on event 1 decodes to major device class `0x05` (Peripheral) / minor
class `0x02` (Gamepad) — matches `bthid_gamepad.c`'s `gamepad_match()` Classic-COD fallback exactly,
confirming the *initial* bind (before VID/PID resolved) legitimately matched via COD, not a guess.
VID `0x2DC8` matches 8BitDo's well-known registered USB vendor ID (already referenced elsewhere in
this codebase, e.g. `bthid_gamepad.c`'s `is_8bitdo` check) — the pairing UI's reported `0x2DC9` for
this same session looks like a display/transcription discrepancy on the OS side, not a value this
project's own capture got wrong.

No dedicated 8BitDo driver is registered, so remaining on `Generic BT Gamepad` throughout is
correct, expected behavior — not a defect.

## Logging defects found and fixed from this capture (2026-07-12)

1. `bthid_set_hid_descriptor()`'s identity-log call hardcoded `BTID_PROV_UNKNOWN` and passed `NULL`
   for class-of-device unconditionally, instead of reflecting the device's actual
   already-resolved state at that point. **Fixed**: now calls the same
   `identity_provenance_guess()` helper the other log sites use, and looks up the connection's
   real COD via `bt_get_connection()`.
2. The "still generic, but VID/PID just resolved via SDP/DIS" case
   (`bthid_update_device_info()`'s `else if (current == &bthid_gamepad_driver)` branch) never
   logged an event at all — the resolution moment was only inferable after the fact from whatever
   event happened to log next. **Fixed**: added an explicit `"vid-resolved-stayed-generic"` event.
3. Two near-duplicate inline provenance-inference ternaries (initial-bind's and reeval-rebind's)
   existed separately, which is exactly how defect 1 happened (a third copy drifted). **Fixed**:
   extracted a single `identity_provenance_guess()` helper, used by every call site except
   initial-bind (which has genuinely different semantics — a nonzero BLE VID *at first bind*
   specifically means pre-connection manufacturer data, not DIS, since DIS cannot have completed
   yet at that exact point).

**Re-tested after the fix, same session**: identical connection, `btid dump` now shows the new
`vid-resolved-stayed-generic` event with correct `provenance:"classic_sdp"` and `cod:"082500"`
(matching event 1, no longer zeroed):

```json
{"dropped":0,"entries":[
{"ms":18171,"conn":0,"transport":"classic","name":"8BitDo NGC Modkit","vid":"0x0000","pid":"0x0000","provenance":"unknown","cod":"082500","desc_len":0,"desc_fp":"0x0000","driver":"Generic BT Gamepad","reason":"initial-bind","slot":-1},
{"ms":18309,"conn":0,"transport":"classic","name":"8BitDo NGC Modkit","vid":"0x2DC8","pid":"0x286A","provenance":"classic_sdp","cod":"082500","desc_len":0,"desc_fp":"0x0000","driver":"Generic BT Gamepad","reason":"vid-resolved-stayed-gen","slot":-1},
{"ms":18309,"conn":0,"transport":"classic","name":"8BitDo NGC Modkit","vid":"0x2DC8","pid":"0x286A","provenance":"classic_sdp","cod":"082500","desc_len":86,"desc_fp":"0x1F98","driver":"Generic BT Gamepad","reason":"descriptor-arrived","slot":-1}
],"more":false}
```

All three fixes confirmed on real hardware, `dropped:0`. One further cosmetic bug this same
capture caught: the `reason` string `"vid-resolved-stayed-generic"` (28 chars) silently truncated
to `"vid-resolved-stayed-gen"` — `BTID_REASON_LEN` (24) was too small for the strings actually in
use (`"initial-bind-generic-fallback"` has the same problem). **Fixed**: `BTID_REASON_LEN` raised
to 32, and `config.c`'s `%.24s` format specifier for the field corrected to `%.32s` to match.

## Raw HID report descriptor (via the new `btid desc` command)

Added specifically to inspect real descriptor bytes/parse results instead of guessing from input
symptoms — see `docs/bluetooth/btstack-implementation.md` "Gate 2" for the command's design.

```json
{"conn":0,"len":86,"bytes":"05010905A1018503050115002507463B0195017504651409398142750195048101150026FF0009300931093209359504750881020502150026FF0009C409C595027508810205091901291015002501750195108102C0","map":{"report_id":3,"button_cnt":16,"is_xbox":false,"is_8bitdo":true,"is_elite2":false,"is_ngc_modkit":true,"has_sim_triggers":true,"digital_shoulder_triggers":false,"hat":{"byte":1,"mask":"0x0F","min":0},"x":{"byte":2,"mask":"0xFF","max":255},"y":{"byte":3,"mask":"0xFF","max":255},"z":{"byte":4,"mask":"0xFF","max":255},"rz":{"byte":5,"mask":"0xFF","max":255},"rx":{"byte":7,"mask":"0xFF","max":255},"ry":{"byte":6,"mask":"0xFF","max":255},"buttons":[{"usage":1,"byte":8,"mask":"0x01"},{"usage":2,"byte":8,"mask":"0x02"},{"usage":3,"byte":8,"mask":"0x04"},{"usage":4,"byte":8,"mask":"0x08"},{"usage":5,"byte":8,"mask":"0x10"},{"usage":6,"byte":8,"mask":"0x20"},{"usage":7,"byte":8,"mask":"0x40"},{"usage":8,"byte":8,"mask":"0x80"},{"usage":9,"byte":9,"mask":"0x01"},{"usage":10,"byte":9,"mask":"0x02"},{"usage":11,"byte":9,"mask":"0x04"},{"usage":12,"byte":9,"mask":"0x08"},{"usage":13,"byte":9,"mask":"0x10"},{"usage":14,"byte":9,"mask":"0x20"},{"usage":15,"byte":9,"mask":"0x40"},{"usage":16,"byte":9,"mask":"0x80"}]}}
```

Decoded by hand: Report ID 3; hat switch (byte 1); Generic Desktop X/Y/Z/Rz → bytes 2-5 (both
analog sticks); Simulation-Controls Accelerator/Brake → bytes 6-7 (this driver's `rx`/`ry` slots,
repurposed for trigger analog when `has_sim_triggers` is set — byte 6 = Accelerator = R2, byte 7 =
Brake = L2, matching the confirmed hardware behavior below exactly); 16 flat, undifferentiated
Button-page usages 1-16, packed into bytes 8-9 with no semantic tagging at all — the descriptor
itself never distinguishes "shoulder" from "trigger click" from anything else; that's entirely up
to whichever lookup table the driver code applies.

`is_ngc_modkit:true` confirms the PID-specific detection and table selection both work correctly
on real hardware — the actual reachability proof DATA.md required, not just a code-reading claim.

## Interactive button-by-button capture (`raw` command, live polling)

Every physical control pressed individually while polling `raw` (the live BT controller's raw HID
report, exposed via `report.c`'s debug seam) immediately after. Neutral baseline first:

```
neutral: 03 0f 7f 80 80 80 00 00 00 00
```

| Control | Raw bytes | Decoded |
|---|---|---|
| Start | `03 0f 7f 80 80 80 00 00 00 08` | byte9=`0x08` → usage 12 |
| Home/Menu button | *(confirmed: does not exist on this unit)* | — |
| Any other button beyond the above | *(confirmed: none — every physical control accounted for)* | — |
| L3 (physical, left stick click) | `03 0f 66 64 80 80 00 00 00 20` | byte9=`0x20` → usage 14 (bytes 2/3 shift slightly — mechanical click deflects the stick a little, not a button signal) |
| R3 (physical, right stick click) | `03 0f 7f 80 7f 77 00 00 00 40` | byte9=`0x40` → usage 15 (same minor stick deflection on bytes 4/5) |

**Correction to the original hardware-observation table** (the one moved into
`docs/bluetooth/8bitdo-ngc-diy-profile.md` from a prior session's `DATA.md`): that table's "L3
held"/"R3 held" rows had the two transposed relative to this live, freshly-confirmed capture — live
data shows **physical L3 → usage 14, physical R3 → usage 15**, the reverse of what the old static
table implied. An earlier analysis pass in this same session, working from the old table, concluded
the button-mapping table had L3/R3 swapped — that conclusion was wrong; it was the *old table*
transposed, not the code. Trust this live capture, not the older static one, if they ever disagree
again.

### Trigger investigation: partial travel, pre-click, full click, both together

| Test | Raw bytes | byte6 (R2 analog) | byte7 (L2 analog) | byte8 | byte9 |
|---|---|---|---|---|---|
| R2 ~halfway (no click) | `03 0f 7f 808080 ba 00 80 00` | `0xBA` (~73%) | `0x00` | `0x80` (usage 8) | `0x00` |
| R2 just before click | `03 0f 7f 808080 de 00 80 00` | `0xDE` (~87%) | `0x00` | `0x80` (usage 8) | `0x00` |
| R2 full click | `03 0f 7f 808080 ff 00 80 02` | `0xFF` | `0x00` | `0x80` (usage 8) | `0x02` (usage 10) |
| L2 ~halfway (no click) | `03 0f 7f 808080 00 79 40 00` | `0x00` | `0x79` (~47%) | `0x40` (usage 7) | `0x00` |
| L2 full click | `03 0f 7f 808080 00 ff 40 01` | `0x00` | `0xFF` | `0x40` (usage 7) | `0x01` (usage 9) |
| Both L2+R2 full click together | `03 0f 7f 808080 ff ff c0 03` | `0xFF` | `0xFF` | `0xC0` (usage 7+8) | `0x03` (usage 9+10) |

**Key finding**: usage 7 (byte8 `0x40`) and usage 8 (byte8 `0x80`) fire during *partial* travel —
well before any mechanical click — and stay asserted through full press too. They are **not**
distinct physical switches; they're a coarse "trigger past some early threshold" echo of the same
analog value already carried in bytes 6/7. Usage 9/10 (byte9 `0x01`/`0x02`) fire **only** at the
true mechanical click, confirmed by their absence at both partial-travel samples. Both L2+R2
together compose cleanly with no bit aliasing between the two triggers.

## Design iteration: two attempts before the owner-confirmed final mapping

The button table for this device went through two real design iterations before landing on the
confirmed-working one — recorded here so the reasoning (including what was tried and rejected) is
preserved, not just the final answer.

**Iteration 1** (informed by the trigger investigation above, before owner review): mapped usage
9/10 (the true click) to `JP_BUTTON_L1`/`JP_BUTTON_R1` (→ NS2 "L"/"R" by default) and usage 11 (Z)
to `JP_BUTTON_R2` (→ NS2 "ZR" by default), reasoning that a genuine GameCube controller's L/R
triggers have exactly one digital click-through with no separate shoulder bumper. **Broke on real
hardware**: `ns2_seam.c`'s `router_submit_input()` has a *separate*, driver-independent fallback —
`if (analog[ANALOG_R2] > 64) buttons |= JP_BUTTON_R2` — that unconditionally re-derives
`JP_BUTTON_R2` from the analog trigger value regardless of what any driver's button table already
decided (a deliberate, sensible default for controllers like Xbox/DualSense whose driver never
reports a discrete click bit at all). Since this device's R2 analog is real and nonzero, that fold
fired independently of Z, producing "R1 (from the true click) + R2 (from the fold) both set" —
observed live as `raw:160 = JP_BUTTON_R1(0x20)|JP_BUTTON_R2(0x80)`.

**Iteration 2**: added a new `suppress_l2r2_analog_fold` flag to `input_event_t`
(`core/input_event.h`), set by this driver for `is_ngc_modkit`, checked in `router_submit_input()`
to skip the analog fold entirely for this device. Fixed the R1+R2 collision, but broke UX per
direct owner feedback: the true-click-only "L"/"R" mapping meant a *light* trigger press (which the
owner wanted to register as ZL/ZR, matching how Pro-Controller-2 mode necessarily approximates
"trigger touched" since it has no real analog output) no longer registered as anything at all until
full click.

**Final, owner-confirmed design**: reverted `suppress_l2r2_analog_fold` (removed the assignment;
the flag itself stays in `input_event.h` as available infrastructure for a genuinely different
future collision). Usage 7/8/9/10 (both the partial-travel echo bits and the true click) are now
**all** suppressed (mapped to 0) — the existing seam-level analog fold is the *only* thing driving
ZL/ZR now, exactly matching "any real press" rather than requiring the full click. Z moved to
`JP_BUTTON_R1` (→ NS2 "R", not ZR) specifically because R1 is untouched by the analog fold, so Z
can never collide with a simultaneous real trigger press again. Face buttons were also corrected
from the assumed Xbox-style rotation (physical A → Switch B, etc., which is right for Xbox/PS pads
but wrong for a GameCube-shaped one) to a direct A→A/B→B/X→X/Y→Y mapping, per explicit owner
instruction. Confirmed working end-to-end by the owner on real hardware in Switch 2 Pro Controller
mode. Full final mapping table: `docs/bluetooth/8bitdo-ngc-diy-profile.md`.

## Capture 2 — Genuine Switch 2 Pro Controller, BLE

**This is the scenario the `bthid_transport_mask_t` fix (Gate 2) was specifically built to
protect** — a genuine Switch 2 Pro Controller connecting over BLE, which Switch 1's Classic-only
`switch_pro_bt.c` could theoretically shadow via an unguarded name-substring fallback if its match
logic were ever reached for a BLE connection. Captured same session, same board/firmware/procedure
as Capture 1, via the same live `System.IO.Ports.SerialPort` connection to `COM6` (`btid dump`,
`btid stat`, `btid desc`, `device`, each read back directly, no manual transcription).

Raw output, verbatim:

```
DUMP1: {"dropped":0,"entries":[{"ms":4190,"conn":4,"transport":"ble","name":"Xbox Wireless Controller","vid":"0x0000","pid":"0x0000","provenance":"unknown","cod":"000000","desc_len":0,"desc_fp":"0x0000","driver":"Xbox Wireless Controlle","reason":"initial-bind","slot":-1},{"ms":4194,"conn":4,"transport":"ble","name":"Xbox Wireless Controller","vid":"0x0000","pid":"0x0000","provenance":"unknown","cod":"000000","desc_len":283,"desc_fp":"0xBCE8","driver":"Xbox Wireless Controlle","reason":"descriptor-arrived","slot":-1},{"ms":27134,"conn":4,"transport":"ble","name":"Switch 2 Pro","vid":"0x057E","pid":"0x2069","provenance":"ble_adv_mfr_data","cod":"000000","desc_len":0,"desc_fp":"0x0000","driver":"Nintendo Switch 2 Contr","reason":"initial-bind","slot":-1}],"more":false}
STAT: {"dropped":0}
DESC: {"error":"no descriptor cached"}
DEVICE: {"name":"Switch 2 Pro","vid":1406,"pid":8297}
```

### Reading

The dump's third entry is the Switch 2 Pro Controller (the first two are an earlier, already-known
Xbox BLE session that happened to still be in the 16-entry ring — included here unedited since the
raw log is preserved verbatim, not because it's new evidence):

| Field | Value | Reading |
|---|---|---|
| `transport` | `"ble"` | Confirmed BLE, matches design expectation |
| `name` | `"Switch 2 Pro"` | The actual advertised name — **not** `"Pro Controller"` or any substring of it (see below) |
| `vid`/`pid` | `0x057E:0x2069` | Nintendo's real Switch 2 Pro Controller identity |
| `provenance` | `"ble_adv_mfr_data"` | Resolved from pre-connection BLE advertisement manufacturer data — available immediately, before any GATT exchange (Switch 2's specific mechanism, documented in `btstack-implementation.md`) |
| `desc_len`/`desc_fp` | `0` / `0x0000` | **Expected, not a bug.** Switch 2 doesn't use a standard HID-over-GATT report descriptor — `switch2_ble.c` routes via `BT_BLE_CUSTOM`/`register_switch2_hid_listener()` instead of the generic HIDS-client path that populates this field. `btid desc`'s own `{"error":"no descriptor cached"}` response confirms the same thing independently: there is nothing to cache, by design. |
| `driver` | `"Nintendo Switch 2 Contr..."` (truncated at 24 display chars) | Correct dedicated Switch 2 driver |
| `reason` | `"initial-bind"` | **Only one event for this connection.** No `descriptor-arrived`, no `vid-resolved-stayed-generic`, no reclassification — it bound to the correct driver on the very first decision, no re-evaluation was ever needed. |

`btid stat` independently confirms `dropped:0` (no ring-buffer overflow masking other events), and
the `device` command cross-checks at a completely different layer (live input-routing state, not
the BT identity log): `vid:1406, pid:8297` decimal = `0x057E:0x2069` — same identity, agreeing
independently.

### What this does and doesn't validate

**Confirmed**: identity resolution, driver binding, and the re-evaluation path all work correctly
end-to-end for a real Switch 2 Pro Controller over BLE, with clean single-shot `initial-bind` and
no shadowing.

**Not confirmed — and this matters**: the specific hypothesized collision the transport-mask fix
was designed to prevent (Switch 1's `switch_pro_bt.c` claiming a BLE Switch 2 connection via a
`"Pro Controller"` name-substring fallback) was **never reachable on this hardware**, because the
real advertised name is `"Switch 2 Pro"`, which doesn't contain `"Pro Controller"` as a substring.
This capture proves the *transport-mask mechanism itself* doesn't break the legitimate path (defense
in depth, correctly inert when not triggered) — it does **not** prove the mechanism prevents the
collision it was built for, because that collision was never actually reproduced. The
`bthid_transport_mask_t` fix remains justified on code-reading grounds (the unguarded fallback
existed and was real), just not on a reproduced-failure basis.

## Pairing window vs. in-flight connect (2026-07-13)

Separate from identity-log captures: the owner reported genuine Switch 2 Pro Controller BLE pairing
was unreliable specifically *when explicit pairing mode was used* ("It'll only pair when i first
connect the dongle and DON'T put it in pairing mode"), while Xbox, DualSense, and a generic Switch
Pro Controller all paired normally either way. Root-caused by code trace (not reproduced via a
dedicated capture — the fix and its bounded-recovery reasoning are documented in
`docs/bluetooth/btstack-implementation.md` "Pairing window vs in-flight connect"): the 10s pairing
window's expiry unconditionally called `btstack_host_stop_scan()`, which resets `hid_state.state`
to `IDLE` regardless of whether a BLE `gap_connect()` was genuinely in flight — silently disarming
the `BLE_CONNECT_TIMEOUT_MS` watchdog for that specific attempt with no user-visible symptom besides
"pairing sometimes just doesn't finish." Fixed in `btstack_host.c`/`bt_transport_cyw43.c`/
`ns2_bt_host.c`; see that doc for the full trace and the fix's exact semantics. This specific
Switch 2 Pro Controller unit is the best candidate for the hardware verification procedure in that
section, since it's the controller the original report was about.
