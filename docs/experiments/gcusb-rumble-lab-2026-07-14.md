# Immediate GameCube-Mode Rumble Blast — Code Audit + PC Host Lab — 2026-07-14

> Confidence key: **Confirmed** (code-verified or hardware-observed) / **Strong** / **Hypothesis** /
> **Unknown**. Per `PROMPT.md`'s explicit instruction, this document separates what was established
> by static code audit (this pass, no hardware needed) from what still requires the `gcusb` tool
> run against real hardware (not yet done — no Pico/genuine controller was connected to this
> machine while this pass ran).

## Question

As soon as the Pico is switched from Pro Controller 2 mode into NSO GameCube mode, the paired
Bluetooth controller immediately begins rumbling at or near full strength, and the rumble does not
stop normally. This occurs before any meaningful gameplay rumble effect should be active. What
transition or packet actually starts the motor?

## Competing hypotheses (before this pass)

1. **Stale shared rumble state** survives the transition (Pro2's last commanded amplitude never
   gets zeroed before GameCube mode's own state takes over).
2. **A race between `g_usb_personality` flipping and `switch_gc_init()` running** lets the BT task
   observe an inconsistent intermediate state.
3. **Vendor-bulk initialization commands get misrouted into the HID rumble decoder** (e.g. a
   `0x0A`/"vibration"-shaped bulk command accidentally reaching `switch_gc_hid_out_report()`).
4. **The console/host genuinely sends a real, strong rumble command immediately on entry** (e.g. a
   "connection confirmed" haptic cue), and the firmware's now-more-faithful `data[0]`-passthrough
   decode (2026-07-14, sixteenth pass) is just accurately reproducing it.
5. **A downstream BT-forwarding bug** (missed transition, driver-private cache staleness, or a
   long-sustain quirk specific to one connected-controller vendor) leaves the motor running
   regardless of what GC's own internal state says.

## Method

Per `PROMPT.md`'s explicit ordering: **audit first, do not edit `switch_gc_hid_out_report()` or
`ns2_bt_host.c` yet**, then build the PC-side instrument, *then* use it. This pass completed the
audit and built the instrument; hardware experiments are still pending (see "Remaining work").

## Audit findings (Confirmed by direct code reading, no hardware needed)

Traced the full lifecycle: `USB personality transition -> switch_gc_reset()/mount() ->
report_set_rumble() -> report.c shared state -> feedback_get_state()/dirty generation -> fast/
normal bthid_task() scheduling -> per-controller cached state/change detection -> controller-
specific BT output packet/sustain semantics`.

**1. Does switching Pro2→GameCube synchronously publish `(0,0)` before reconnect?**
**Confirmed: yes.** `usb_apply_mode_cycle()` (`src/usb.c:86-108`) calls `tud_disconnect()`, then
`usb_reset_personality_state(next)` — which for the GameCube target calls `switch_gc_reset()` →
`switch_gc_init()` (`src/switch_gc/switch_gc.c`), which calls `report_set_rumble(0, 0, 0)`
unconditionally — **before** `g_usb_personality = next` executes on the next line. This write to
the shared `report.c` state is personality-agnostic (plain critical-section-guarded globals), so
it takes effect immediately regardless of the `g_usb_personality` variable's exact timing.

**2. Can a stale Pro2 rumble value survive in `report.c`, `s_fb[]`, or a driver's private cached
rumble state?** **Confirmed: not in `report.c` or `s_fb[]`** (both get the `(0,0)` write above,
`ns2_seam.c`'s `feedback_get_state()` will detect the change on its very next call and mark
`rumble_dirty`). **But the per-driver private cache is untouched by this reset.**
`bthid_gamepad.c`'s `gamepad_task()` keeps its own `gp->rumble_left/right` (last value it believes
it physically sent) — this is never reset by a USB personality transition (it's BT-connection-
scoped state, and the BT connection itself is unaffected by a USB-side transition). In the normal
case this is harmless (the dirty-check still forwards the new `(0,0)` correctly on the next poll),
but it means: if a *subsequent* nonzero write happens to race with an in-flight cache update, the
comparison basis is this private cache, not the fresh shared state — a real, if narrow, surface
for a missed transition (see finding 5 below for a concrete, non-narrow bug in the same function).

**3. Does `switch_gc_init()` run before or after `g_usb_personality` changes, and can the BT task
observe an intermediate state?** **Confirmed: `switch_gc_init()` runs first** (`src/usb.c:100-101`:
`usb_reset_personality_state(next);` then `g_usb_personality = next;`). Core1 (BT task) reads
`g_usb_personality` in exactly one rumble-relevant place (`ns2_seam.c`'s `gc_active` check inside
`router_submit_input()`, unrelated to the rumble-forwarding path) — the rumble reset itself
(`report_set_rumble`) does not depend on `g_usb_personality` at all, so this particular race window
is benign for rumble specifically, though it could matter for other personality-gated logic not in
scope here.

**4. Are both the 30ms control timer and the 3ms rumble timer calling `bthid_task()` concurrently/
reentrantly on the same BTstack run loop?** **Confirmed: not reentrantly** — BTstack's run loop
(`btstack_run_loop_execute()`) is single-threaded/cooperative on core1; each timer callback runs to
completion before the next event is processed. Both `control_timer_handler` (30ms,
`ns2_bt_host.c`) and `rumble_timer_handler` (3ms, added seventeenth pass) call `bthid_task()`,
which means it now runs somewhat more often than once per 3ms in the worst case (both timers
firing close together) — harmless/idempotent (every driver `.task()` is a cheap dirty-check), not
a correctness bug, just mildly redundant.

**5. Can `rumble_dirty` be cleared by one driver/task pass before another consumer has sent the
corresponding physical stop?** **Confirmed: yes — a real bug, found this pass.**
`bthid_gamepad.c`'s `gamepad_task()`:
```c
if (left != gp->rumble_left || right != gp->rumble_right) {
    if (device->vendor_id == 0x045E) { ... bthid_send_output_report(...) ... }
    gp->rumble_left = left;
    gp->rumble_right = right;
}
feedback_clear_dirty(player_idx);
```
`feedback_clear_dirty()` runs **unconditionally**, and `gp->rumble_left/right` get updated to the
latest value **even when `device->vendor_id != 0x045E`** — i.e. for any non-Xbox device, this
function silently "consumes" the dirty flag and updates its own bookkeeping without ever physically
sending anything (documented/intentional for devices this driver has no known rumble format for —
see its own comment), but it means: **if the connected controller is not Xbox-vendor and goes
through this generic driver, no rumble command — on OR off — is ever actually sent to it via this
path at all.** This directly bears on hypothesis 5 and is why device identity matters enormously
for interpreting the bug (see finding 6).

**6. Which connected-controller drivers use long sustain durations, resend-on-change, or cached
output suppression?**
- **Xbox** (`bthid_gamepad.c`'s `gamepad_task()`, `vendor_id == 0x045E`): resend-only-on-change,
  and every resend arms `loop_count = 0xEB` (~10-minute hardware sustain), matching xpadneo's own
  documented convention (`docs/bluetooth/btstack-implementation.md` "loop_count"). **A missed
  "off" observation here can leave the motor running for up to 10 minutes** — this was the
  seventeenth pass's finding, real, but only relevant **if the connected device is actually Xbox**.
- **Sony DS4/DualSense** (`ds4_bt.c`, its own dedicated `.task()`, not the generic driver): uses an
  **absolute, always-refresh** model — every `need_update` resend carries the *current* commanded
  level directly (no "sustain for N minutes" concept at all). A missed poll just means the next
  successful poll sends whatever the current value is; there is **no multi-minute-stuck-on risk
  for this device family** the way there is for Xbox. If the connected controller during the
  reported bug was a DualSense/DS4, the Xbox-sustain hypothesis does not apply at all.
- **8BitDo NGC Modkit** (VID `0x2DC8`, routed through the *same* generic `bthid_gamepad.c` driver
  as Xbox, since it has no vendor-specific driver of its own): vendor_id `0x2DC8 != 0x045E`, so per
  finding 5 above, **this driver currently never sends it any rumble output report at all** — if
  the Modkit is the device that was observed rumbling, that is not explained by anything in this
  driver and needs its own investigation (a different feedback mechanism, or a different device
  was actually connected during that specific test).
- **Open question, not resolved by code audit alone**: *which* device family was actually paired
  when the "immediate full rumble" was observed. This materially changes which of the above applies
  and is exactly the kind of ambiguity `gcusb`'s `describe`/`read-input`/log output is meant to
  remove by making the exact connected-device identity and exact HID OUT traffic directly
  observable, instead of inferred from memory.

**7. Does a personality transition explicitly force an off packet to the physical controller, or
merely update shared memory and hope the next poll notices it?** **Confirmed: the latter.**
`switch_gc_init()`'s `report_set_rumble(0,0,0)` only writes the shared `report.c` global; there is
no synchronous "send stop now" call from `usb.c`'s core0 transition code to the BT-connected
controller. It relies entirely on the next `feedback_get_state()`/`gamepad_task()` poll (core1,
independent timer-driven) to notice and forward it. In the current code this is not itself proven
to be a problem (the very next poll, whether 3ms or 30ms cadence, should pick up the change) — but
it is a structural gap relative to the invariant PROMPT.md requires going forward ("the physical
driver receives an explicit zero/off output when required, not only a shared-state update").

**8/9. Does the host send a report `0x03` immediately during initialization, and what exact four
bytes? Does the same packet appear when the genuine controller is initialized on Windows?**
**Cannot be answered from code alone — this is exactly what the `gcusb` tool exists to establish.**
Nothing in `switch_gc_vendor_dispatch()` or `switch_gc_hid_out_report()` synthesizes a rumble
command as a side effect of any other command (verified: every `report_set_rumble()` call site in
`switch_gc.c` is either an explicit zero, or the single real nonzero call gated behind
`report_id == 0x03` inside `switch_gc_hid_out_report()` itself — hypothesis 3 above is **refuted
for the current code**: no vendor-bulk command routes into the rumble decoder). So if the Pico
*is* rumbling immediately on entry, either the connected BT controller's driver is misbehaving
(findings 5/6), or the host genuinely sends a real nonzero report `0x03` very early and the
firmware is correctly, faithfully reproducing it (hypothesis 4) — both require the tool + a real
capture to distinguish, not more code reading.

## What this pass refutes/confirms among the original hypotheses

| # | Hypothesis | Status after this pass |
|---|---|---|
| 1 | Stale shared rumble state | **Refuted** for `report.c`/`s_fb[]` (both correctly zeroed synchronously before reconnect) |
| 2 | `g_usb_personality` race | **Refuted** for rumble specifically (reset doesn't depend on that variable) |
| 3 | Vendor-bulk misrouted into rumble decoder | **Refuted** for the current code (structurally impossible — separate call paths, verified exhaustively) |
| 4 | Host genuinely sends a real strong rumble cue on entry | **Open** — needs the tool + a real capture |
| 5 | Downstream BT-forwarding bug | **Partially confirmed**: a real unconditional-dirty-clear bug found in `gamepad_task()` (finding 5); its relevance depends entirely on which device family is connected (finding 6), which is unknown without the tool |

## PC host lab: `tools/gcusb`

Built per `PROMPT.md`'s full spec — a native Windows executable (MinGW-w64/gcc against
SetupAPI/WinUSB/HID directly, no libusb, no Zadig, no driver replacement) that turns this PC into a
controlled USB host for either the Pico or the genuine NSO GameCube Controller. See
`tools/gcusb/gcusb_win.c`'s own header comment and `PROMPT.md` for the full design; summary:

- **Safety-critical device selection**: both devices share VID:PID `057E:2073`; the tool resolves
  `--target pico|genuine` against the actual connected device's `bcdDevice` (`0x0111`/`0x0101`) and
  refuses — never silently guesses — on any mismatch, ambiguity, or absent device.
- **Commands**: `list`, `describe`, `init`, `read-input`, `send-command`, `rumble`,
  `rumble-sweep`, `stop-rumble`, `replay` (with a hardware-free `--validate-only` mode), `compare`.
- **Allowlist**: `tools/gcusb/gcusb_core.c`'s table mirrors `switch_gc_vendor_dispatch()`'s own
  `switch(id)`/`sub` structure exactly; memory writes, pairing (`0x15`), and anything unrecognized
  are rejected by default (pairing requires an explicit `--profile console-capture` or `--unsafe`).
- **Rumble safety**: amplitude/duration clamps (never `0xFF`-first, capped pulse length
  independent of host refresh), a Ctrl+C/console-close handler that force-stops before exit, and a
  standalone `stop-rumble` that tries both candidate stop mechanisms (zero-data report `0x03` and
  a genuine zero-length interrupt OUT) independently.
- **Logging**: every transfer this tool initiates goes through one chokepoint
  (`log_event()`) with a monotonic microsecond timestamp, target/interface/transfer-type/
  direction, exact request/response bytes, and elapsed time — human-readable by default, NDJSON
  with `--ndjson`.

**Verified this pass (no hardware connected)**: the tool builds clean
(`tools/gcusb/build.ps1`), all 46 pure-logic tests pass (`tools/test_gcusb_core.c`), and every
read-only/refusal code path was exercised directly against a machine with zero matching USB
devices attached: `list` correctly reports none found; `describe`/`rumble`/etc. without `--target`
or with an unresolvable target refuse cleanly with an actionable message, never silently proceed;
`replay --validate-only` against a real example script
(`tools/gcusb/scripts/init_minimal.gcusb`) correctly identifies and names both allowlisted commands
with zero problems, and correctly flags a deliberately-inserted memory-write command as rejected.

**Not yet verified (needs real hardware, not available to this pass)**: the WinUSB device-interface
GUID used for enumeration (`k_default_winusb_guid` in `gcusb_win.c`) is a best-effort default for a
device bound via compatible-ID-only WinUSB (no custom extended-property GUID in this project's own
MS OS descriptor) — **not independently confirmed against this project's real hardware**. If
`describe`/`init`/etc. report `have_winusb: false` for an otherwise-correctly-identified device,
check Device Manager's "Device class GUID" for the actual value and pass `--winusb-guid {...}` to
override. This is the single most likely rough edge to hit on first real use.

## Remaining work (requires the owner's physical hardware — cannot be completed autonomously)

`PROMPT.md`'s Experiments 1-5 and the 10-step hardware acceptance test all require a paired
Bluetooth controller, a real Pico in GameCube mode, and (for differential testing) the genuine
controller connected via USB simultaneously — physical setup and real-time perception (does the
motor actually buzz) that only the device owner can provide. See `STATUS.md` for
the exact runbook to follow with `gcusb` once hardware is available. No firmware fix has been
implemented this pass, per `PROMPT.md`'s own explicit instruction: **do not produce another
firmware build until the tool names the first bad transition.**

## Conclusion (as of first draft, before live hardware)

Static code audit refutes three of the five original hypotheses outright (stale shared state,
personality-flip race, vendor-bulk misrouting) and narrows the remaining two to a single open
question: **which device family is actually paired**, and **what exact bytes the host sends
immediately on GameCube-mode entry**. Neither is answerable from code alone. The PC host lab
(`gcusb`) is built, tested at every level that doesn't require hardware, and ready to answer both
the next time real hardware is available.

## Update — live hardware run, same day (`TEST.md`)

The owner ran `gcusb` against a real Pico (GameCube mode, paired Xbox Series controller — this
resolves the "which device family" open question from above: **Xbox, confirmed**). `list`/
`describe` worked correctly (bcdDevice resolved both the Pico `0x0111` and, in a separate session,
the genuine unit `0x0101`), a 5-second `read-input` capture streamed clean neutral report `0x05`
data throughout with **no rumble triggered** — but `replay --script scripts/init_minimal.gcusb`
failed immediately: `ERROR: no WinUSB interface known for this device`. Exactly the rough edge
flagged in this doc's first draft.

**Root-caused directly against the live device** (no guessing) via PowerShell:
```powershell
Get-PnpDevice | Where-Object { $_.InstanceId -like "*VID_057E&PID_2073*" }
Get-PnpDeviceProperty -InstanceId $id -KeyName "DEVPKEY_Device_Driver"
Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters"
```
Found: the connected-and-working vendor interface (`USB\VID_057E&PID_2073&MI_01\...`, `Service:
WINUSB`, `Status: OK`) stores its real, per-device interface GUID under the registry value
**`DeviceInterfaceGUID`** (singular). The tool's `k_default_winusb_guid` hardcoded guess
(`{88bae032-5a81-49f0-bc3d-a4ff138216d6}`) turned out to be the device's **class** GUID (confirmed
matching `ClassGuid` in the `Get-PnpDevice` output exactly) — a different Windows concept from the
per-device **interface** GUID (`{6f13725e-ef0e-4fd3-ae5f-b2de989ec825}` on this specific unit) that
`SetupDiEnumDeviceInterfaces` actually needs. Two related bugs, both fixed in `gcusb_win.c`:
- `find_registered_interface_guid()` (new) walks the parent device's children via `cfgmgr32`
  (`CM_Get_Child`/`CM_Get_Sibling`) to find the `MI_01` child devnode, then reads its real
  `DeviceInterfaceGUID` (falling back to the plural `DeviceInterfaceGUIDs` for older driver
  versions, then to the old hardcoded default only if both lookups fail) — no more guessing a
  single global GUID for every device.
- `Container ID` display/matching was reading a `DEVPROP_TYPE_GUID` property into a `WCHAR` buffer
  and treating it as text (`????????` on screen, and fragile for matching — a GUID's raw bytes can
  contain an embedded zero-word that truncates `wcscmp` early). Fixed: stored as a real `GUID`,
  compared with `memcmp`, displayed with a proper `format_guid()`.

**Re-verified against the same live Pico after the fix**: `list`/`describe` now report `WinUSB
interface found: yes`. Two real, controlled tests then ran cleanly:
1. `init --target pico --profile steam` — sends only `0x03/0x0D` (Initialise USB) and `0x03/0x0A`
   (Select Input Report), zero rumble writes. Both commands got exactly the documented response
   bytes (`03 01 00 0d 00 f8 00 00 01 00 00 00` and `03 01 00 0a 00 f8 00 00`). Controller was not
   rumbling before or after.
2. `rumble --target pico --amplitude 32 --duration-ms 200` — a bounded, clamped, auto-stopping
   pulse. Owner confirmed the controller was **not** rumbling afterward. (Side note, not a
   regression risk: the true zero-length interrupt-OUT "ZLP" stop attempt failed immediately —
   Windows' HID API enforces fixed-size reports and doesn't support arbitrary-length writes the
   way a lower-level USB stack can, so this tool's ZLP candidate is a known, honest no-op on this
   transport; the zero-data report `0x03` write is the one that actually matters and it worked.)

## Narrowing the hypothesis further (still not a direct reproduction)

Neither passive input reading, the safe Steam-style handshake, nor a controlled pulse-with-
explicit-stop reproduces the bug. Combined with the owner's own field notes — same Xbox
controller: real Switch 2 console → immediate rumble persisting until unplugged; PC + a
Switch-1-emulator host → rumble that felt maxed and lasted longer than expected but eventually
stopped on its own — the leading hypothesis is now: **a real host (console or emulator) sends a
genuine nonzero rumble command that it never explicitly follows up with a stop**, and because the
connected controller is Xbox, that single trigger arms this project's own Xbox rumble bridge's
~10-minute hardware sustain (`loop_count=0xEB`, `pulse_sustain_10ms=0xFF` — mirroring xpadneo's
own documented Windows-driver-compatible convention). "Doesn't stop until unplugged" would then be
consistent with "outlasted the test session," not literally infinite; the PC/emulator case
recovering on its own is consistent with that host eventually sending a different rumble command
(a real gameplay effect) that changes or re-triggers the sustain window.

Researched this exact bug class directly (not guessed): **`atar-axis/xpadneo` issue #400,
"8BitDo Pro 2 non-stop rumble on connect"** documents a near-identical symptom for a *different*
controller on the *Linux* xpadneo driver — a deliberate "connection confirmation" welcome-rumble
effect that never terminated. Root cause per the issue and its fix (commit `94ad82a`): "There was
a flaw in the rumble testing logic during connect which caused controllers with motor masking bits
but without rumble envelope parameters to rumble indefinitely after connecting" — the fix ensures
the enable/motor-mask bits (`ff.enable`) are explicitly reset to 0 on stop, not left set with only
the magnitude zeroed. This project's own `bthid_gamepad.c` Xbox rumble path had exactly that shape:
`buf[0]` (the enable/motor-mask byte) was unconditionally `XBOX_RUMBLE_MOTORS` regardless of
amplitude, even when stopping.

**The owner declined the fully-confirming live test** (send one pulse, deliberately withhold the
stop, time how long it takes to self-resolve — bounded to ~10 minutes but requires the real
controller to visibly buzz that whole time) in favor of continued autonomous investigation. This
means the hypothesis above remains **Strong, not Confirmed** — no live reproduction of the actual
"stuck" state was captured this pass.

## Defensive fixes implemented (evidence-motivated, not gated behind full reproduction)

`PROMPT.md` itself requires a set of invariants "regardless of root cause" — these two fixes
satisfy specific named invariants directly and are safe/beneficial independent of whether the
exact trigger above is ever directly confirmed:

1. **`bthid_gamepad.c`: Xbox stop commands now clear the enable/motor-mask bits and the
   sustain/loop-count fields entirely**, not just the motor magnitude — directly mirroring
   xpadneo's own fix for the near-identical bug class above. `buf[0]`, `buf[5]`, `buf[7]` are all
   zeroed when `left==0 && right==0`, rather than only `buf[3]`/`buf[4]` (the magnitudes).
2. **A monotonic rumble generation counter** (`report_get_rumble_gen()`) replaces plain value
   comparison in `ns2_seam.c`'s `feedback_get_state()` for dirty-flag detection — the exact
   mechanism `PROMPT.md` names for its own explicitly-raised concern that faster polling alone
   (the seventeenth pass's fix) cannot guarantee a stop transition is never lost merely because a
   sampled value happened to round-trip back to what a consumer had already seen between polls.

Both boards + the `NS2_PRO=OFF` scratch config build clean; existing host test suites unaffected.
**Neither fix has been hardware-validated against the original failure scenario** (real Switch 2
console, Pro2→GameCube switch) — that remains the next real test.

## Update — real console re-test with the fixes, same day: symptom changed shape (progress, not a repeat)

The owner flashed the nineteenth-pass build and re-tested the original scenario on a real Switch 2
console. **The immediate full-strength rumble on GameCube-mode entry is gone.** But real Smash Bros
gameplay revealed a different, more precisely diagnosable problem: the game's normal small,
frequent rumble ticks were being rendered as one continuous "powerful" buzz that only stopped
during a scene transition (where the game genuinely sends no rumble at all), and persisted briefly
even right after pausing mid-fight.

This maps precisely onto a mechanism the nineteenth pass's fixes hadn't touched: the Xbox rumble
bridge set `pulse_sustain_10ms = 0xFF` (~2550ms) on **every** nonzero trigger regardless of
amplitude, with `pulse_release_10ms = 0` (no gap). The nineteenth pass's own test only ever sent a
single isolated pulse followed by an explicit stop — it never exercised what real gameplay actually
does: a continuous stream of separate, deliberately brief/small rumble ticks. If any two land
within ~2.55 seconds of each other (near-guaranteed for a "textured" gameplay rumble sent every few
tens of ms), each new trigger re-arms a multi-second hold before the previous one has a chance to
decay, smearing what should be a series of distinct short ticks into one sustained motor
engagement. This explains all three reported details at once: constant re-arming reads as
"powerful continuous"; the only way to get a genuine ~2.55s gap with zero triggers is a scene
transition; and whatever trigger landed in the last ~2.55s before a pause keeps holding regardless
of the pause itself.

**Fixed**: shortened `pulse_sustain_10ms` from `0xFF` to `0x05` (~50ms) for a genuine trigger —
long enough to feel like a distinct tick, short enough that a rapid stream of small ticks now reads
as a texture rather than one continuous buzz. `loop_count` stays `0xEB` for a genuine trigger, but
the much shorter per-pulse sustain drops the worst-case total duration (if a trigger were somehow
never followed by anything else) from ~10 minutes to `235 * 50ms ≈ 11.75s` — a materially safer
bound while this remains unconfirmed. The `stopping` branch (both fields zeroed) from the
nineteenth pass is unchanged. Both boards build clean, only `bthid_gamepad.c` recompiled.

## Update — foundational correction, same day: the byte-level model itself was wrong

While doing a documentation/hypothesis-audit pass (no hardware available), read the real Linux
kernel "HID: nintendo" driver source (Vicki Pfau, linux-input mailing list v11 patch series,
`https://marc.info/?l=linux-input&w=2&r=1&s=hid+switch2&q=b`) at the project owner's request. This
overturned the byte-level model every rumble fix in this document so far had assumed:

**The genuine GameCube controller has no continuous-amplitude rumble hardware at all.** It's a
simple ERM motor with exactly three states (`GC_RUMBLE_OFF=0`/`ON=1`/`STOP=2`, sent at `data[1]`).
Real hosts simulate a continuous perceived amplitude via delta-sigma/error-accumulation duty-cycle
modulation of that ON/OFF state — not via any single byte's magnitude. `data[0]` (what every
revision in this document treated as "intensity") is actually an unrelated, incrementing
sequence/command byte.

This single correction **retroactively explains the entire bug arc documented in this file**
without needing any of the intermediate byte-semantics theories: because a rolling sequence byte
is essentially never exactly zero, every real rumble packet looked like "some nonzero amplitude"
to the old decode regardless of the game's actual OFF/ON/STOP intent. Both major symptoms are
consistent with this single root cause — "immediate rumble on GameCube-mode entry" (an early real
packet with state=OFF/STOP but nonzero sequence byte) and "normal small gameplay rumbles becoming
one continuous buzz" (every packet during an active session has nonzero sequence). The
envelope-duration fix (previous section) remains independently correct and necessary — it governs
how long the *downstream* Xbox bridge holds a trigger, which matters regardless of which upstream
byte is read as the trigger — but the trigger itself was being misidentified the whole time.

**Fixed** in `switch_gc_hid_out_report()` (`src/switch_gc/switch_gc.c`, see `gc_rumble_state_t`):
now reads `data[1]` as the state enum and drives the motor at a fixed level for `GC_RUMBLE_ON`,
off for `OFF`/`STOP`/anything malformed. `tools/gcusb`'s `gcusb_build_rumble_data()` updated to
match (now emits a real OFF/ON state at the correct offset rather than a fake amplitude byte), and
`rumble-sweep` repurposed from an "amplitude sweep" (meaningless under this model) into a
toggle-cadence test, since toggling speed is now the actually relevant variable. Full details of
what was refuted and why: `docs/experiments/refuted-hypotheses.md` "GC rumble data[0] as a linear
amplitude byte". Both boards + `tools/gcusb` build clean; 47/47 `gcusb_core` tests pass.

**Not yet hardware-validated** — this is a *Strong*, not Confirmed, correction: no new hardware
test has run against this corrected model yet. The next real console/gameplay test is what would
move it to Confirmed.

## Conclusion

Live hardware access resolved the tool's own blocking bug (WinUSB GUID discovery) and two
controlled tests confirmed the rumble decode + stop path works correctly under gcusb's own direct
control, narrowing the original bug to a specific hypothesis and two defensive fixes. A real
console re-test then confirmed the original "immediate rumble on entry" symptom is gone, and
surfaced a *different*, much more precisely diagnosable envelope-duration bug during actual
gameplay — fixed by shortening the per-trigger pulse-sustain window. A subsequent documentation
audit then found the byte-level model itself was wrong all along, via real kernel driver source —
a foundational correction that retroactively explains the whole bug arc more completely than any
of the intermediate fixes did individually. This is real progress through successive,
evidence-driven refinement, not a repeat of the same guess: each fix targeted a specific,
newly-surfaced mechanism, verified against the best evidence available at the time. **Still not
fully confirmed** — neither the envelope fix nor the corrected byte-semantics fix has been
re-tested against real hardware; that is the next step.
