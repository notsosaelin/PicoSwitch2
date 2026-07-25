# Archived Development Log — through 2026-07-15

> Historical chronological record. For the current repository state, see
> [`STATUS.md`](../../STATUS.md). Claims marked pending here may have been resolved later.

> Living snapshot of the repository's current state. Not a roadmap (see `PLAN.md`),
> not project guidance (see `CLAUDE.md`). Update whenever significant work lands.

---

# Project Summary

**Project:** PicoSwitch2 — bridge Bluetooth controllers to a Nintendo Switch 2 as a **native Switch 2 Pro Controller**.

**Emulated device:** Switch 2 Pro Controller — USB VID `0x057E` / PID `0x2069`, bcdDevice `0x0210`.

**Boards:** Pico W (RP2040) and Pico 2 W (RP2350), one source tree.

**Current Branch:** `ns2-testing`

**Update 2026-07-15 — DualSense input/output scheduler hardware-confirmed; BOOTSEL gesture dispatch
correction pending hardware validation.**

- Real-hardware testing confirmed the report-boundary device-task fix: standard DualSense and Edge
  input, Edge-specific mappings, LEDs, and rumble all work, with none of the earlier hanging. This
  confirms the dedicated DS5 binding/output path and the decision to leave `device_quirks` out of it.
- All three BOOTSEL gestures still failed only while a DualSense was paired. The remaining split was
  exact: report boundaries serviced the cooperative raw-button sample, but `bootsel_poll()` and its
  double/triple/hold dispatch still ran exclusively in the delayed 30 ms control timer.
- Gesture polling/dispatch is now shared by both paths. Each inbound report services the raw sample,
  advances the existing gesture state machine, then runs the already hardware-confirmed device tasks;
  the 30 ms timer remains the quiet/disconnected fallback. Rumble code, DS5 packets/mappings, and the
  GameCube decoder are unchanged. Both firmware targets build and all host tests pass.

**Update 2026-07-15 — DualSense identity theory falsified and rolled back; scheduler correction
pending hardware validation.**

- Hardware testing of the attempted DS4-to-DS5 bootstrap caused a direct regression: DualSense and
  DualSense Edge stopped producing input on the real console, while BOOTSEL still stopped working
  under their traffic and rumble still did nothing. The bootstrap, direct-connection identity edits,
  and generic-quirk integration from that attempt have been removed.
- The prior working Edge paddles, Fn buttons, and mute button are decisive evidence that the dedicated
  `ds5_bt.c` parser was already bound: those controls are parsed only there. They could not have
  "happened to work" through DS4 or the generic quirk layer. Consequently, `device_quirks` is not in
  the DualSense input or output path and was not the cause of the Sony failure.
- One shared mechanism does explain the still-correlated LED/output and BOOTSEL failures. This port
  runs BTstack's non-returning run loop and drove both per-device `.task()` callbacks (including DS5's
  initial LED/output packet and rumble forwarding) and the cooperative BOOTSEL service from timers.
  Sustained Classic HID report handling can delay those timers. Incoming report boundaries now also
  service BOOTSEL and call `bthid_task()`: controller traffic drives the work it previously starved,
  while the existing 3 ms timer remains the quiet/idle fallback.
- No DS5 report parser, Edge mapping, USB personality, GameCube rumble decoder, or generic controller
  mapping changed in this correction. All ten host tests pass and Pico W/Pico 2 W release builds
  succeed. DualSense/Edge input, LED, rumble, and BOOTSEL behavior require physical validation.

**Update 2026-07-15 — DualSense output packet experiment (hardware-falsified as a complete fix).**

- The owner proved the same physical DualSense/Edge actuators work through `ds.daidr.me` even though
  Steam USB and every PicoSwitch2 personality produced no rumble. Direct source comparison against
  `daidr/dualsense-tester` commit `6e90cd150598ad1e70dc3974c13e8eea2cb59e06` found a concrete
  configuration mismatch rather than a failed motor or GC translation issue.
- The working browser tester sends an initial common output state with `validFlag0=0xF7` and
  `validFlag1=0xF7`; later rumble writes use `validFlag0=0x03`, retain the `0xF7` flag1 baseline,
  and place right/left motor magnitudes in common bytes 2/3. PicoSwitch2 retained its separately
  documented Bluetooth wrapper (report 0x31, sequence nibble, tag 0x10, common state, and CRC).
- PicoSwitch2 previously sent LIGHT_OUT setup instead of the `F7/F7` initialization and selected
  vibration-v2 (`flag0=0x02`, `flag2=0x04`) for every standard/Edge controller. It now mirrors the
  tester's initialization and legacy-compatible `0x03` rumble selector. Because flag1 remains
  `0xF7`, cached LEDs are included in every report rather than being zeroed by a rumble-only write.
- Packet tests cover initialization, active rumble, zero-magnitude STOP, persistent LEDs, sequence,
  report/tag bytes, and independent CRC golden values. Both firmware targets build clean. The
  already hardware-confirmed GameCube decoder was not changed. A subsequent Bluetooth hardware test
  still produced neither LED nor rumble, so packet-byte parity alone was not the missing mechanism;
  see the scheduler correction above.

**Update 2026-07-15 — genuine-capture GC rumble decoder, pending hardware validation.**

- The recovery build below produced no rumble at all on a real Switch 2. That falsifies the
  assumption that restoring the earlier `data[1]`-only decoder was sufficient in the current tree.
- The project's own eight genuine USB samples expose the concrete mismatch: four packets from the
  deliberate rumble burst are `03 sequence 00 01 00`, but the old decoder read only the first byte
  after sequence and classified all four as OFF. The decoder now recognizes both captured
  `00 01` and documented `01 00` active forms; `02` in either candidate byte is immediate STOP.
- Each ON becomes a bounded 40 ms downstream pulse. ON refreshes it; OFF and ZLP never extend it;
  explicit STOP ends it immediately. This is deliberately long enough to cross core1's 3 ms
  feedback poll while remaining incapable of the previous indefinite/full-blast latch.
- Added a host test that feeds all eight byte-exact genuine capture samples through the decoder.
  Both firmware targets build clean. No Bluetooth file was edited in this correction.

**Update 2026-07-15 — recovery attempt (failed on subsequent real-hardware test).**

- The work was never uploaded to remote Git, but Claude's local JSONL transcript preserved every
  edit and timestamp. The owner's 20:40 hardware report explicitly confirmed NSO GameCube rumble
  working for non-Sony controllers after the 13:13 Xbox envelope fix and 14:13 GC decoder edit.
- Restored `src/switch_gc/switch_gc.c` byte-for-byte to that confirmed decoder: report `0x03`
  byte 1 is OFF/ON/STOP, ON maps to fixed `0xB0`, OFF/STOP/ZLP stop, and the 500 ms watchdog remains
  the stuck-motor backstop. The next real-hardware test produced no rumble, so this restoration was
  not retained as the final decoder.
- No Bluetooth, pairing, BOOTSEL, controller-driver, or Xbox-output file was changed in this
  recovery pass.

**Update 2026-07-15 — second hardware-correction pass, pending validation.**

- Current-firmware DualSense units require the vibration-v2 selector in the tested host path. The
  driver now uses v2 for every DualSense-family device rather than only Edge; packet layout, CRC,
  zero-magnitude STOP, and both flag variants remain covered by the host test.
- In GameCube output mode, a real Pro Controller 2 now preserves its named controls: physical L/R
  synthesize the GC L/R full pull plus detent, and physical ZL/ZR map to GC ZL/Z. The separately
  hardware-derived Xbox/DualSense GC mapping remains unchanged.
- Switch 2 BLE Joy-Con L/R PID constants were still reversed in the input driver and discovery text
  (`2067` is Left, `2066` Right); they now agree with the genuine USB descriptors, SPI dumps, and
  the already-correct USB personality.
- All ten host-test executables pass; Pico W and Pico 2 W Release UF2 builds succeed.

**Update 2026-07-15 — GameCube rumble and triple-tap wipe fixes pending hardware validation.**

- GameCube USB output decoding was already correct: report `0x03` byte 1 is the official
  controller's OFF/ON/STOP state. The remaining failure was downstream. Standard Xbox pads bind
  to the dedicated `xbox_bt.c` / `xbox_ble.c` drivers, whose rumble envelope had diverged from the
  hardware-tested generic fallback and used a 2.55 s pulse. All Xbox paths now use one shared
  encoder (main-motor update mask plus zero magnitudes for STOP, 50 ms active pulse), and failed
  sends retain the dirty transition so an OFF cannot be lost permanently. A first implementation
  incorrectly made STOP an all-zero packet; hardware testing immediately exposed that byte 0 is
  an update mask (zero means "change nothing"), and the regression was corrected on 2026-07-15.
- Triple-tap now installs a global persistent admission lock before disconnecting/erasing state.
  It stops BLE/Classic discovery, disables Classic connectability, rejects late incoming or
  already-queued connections, clears the persistent last-connected record, and survives reboot.
  Only an explicit double-tap pairing window clears it. This is intentionally global rather than
  a MAC denylist: a wipe must also cover powered-off controllers, rotating BLE addresses, and
  Switch 2's custom ATT pairing path, which never creates a BTstack SM bond.
- CYW43 LE bond deletion now removes every device-database slot explicitly. The previous
  `le_device_db_init()` call does not erase the SDK 2.2.0 TLV backend and therefore left real LE
  keys in flash.
- DualSense/DualSense Edge rumble was also reported non-functional. Its 78-byte report framing and
  CRC were correct, but its state/flag behavior did not match Sony's Linux driver: it repeated the
  one-time lightbar-setup command in every rumble report, never selected vibration-v2 for Edge, sent
  setup and initial LEDs close enough for BTstack's deferred queue to replace the first packet, and
  cleared dirty feedback even when queuing failed. `ds5_output.c` now builds the packed report from
  explicit update state; setup is sent once and spaced from LED programming, Edge uses the v2 flag,
  rumble STOP keeps the vibration selectors while writing zero magnitudes, and failed commands are
  retried. This is source- and host-test-validated but still requires physical DualSense/Edge tests.
- New host tests `tools/test_xbox_rumble.c` and `tools/test_ds5_output.c` pass, as do the complete
  existing host-test set and both `pico_w` and `pico2_w` release builds. Physical GameCube gameplay,
  Xbox stop, DualSense/Edge rumble, and controller re-pairing tests remain required.

**Hardware correction, same day:** that physical pass failed in three specific ways: Xbox had no
rumble in NSO GameCube mode on a real console despite working from Steam; three DualSense-family
controllers had no rumble and the new staged setup left their LEDs dark; and every BOOTSEL gesture
stopped working while a DualSense was connected. All three now have code-proven mechanisms and new
fixes, pending another physical pass:

- CYW43's direct Classic-L2CAP output path (used by DualSense to avoid its unsafe SDP path) called
  `l2cap_send()` opportunistically with no can-send queue. Reports could be rejected under input
  traffic. It now preserves the promised current report, coalesces later state into one latest
  successor, and drains only from `L2CAP_EVENT_CAN_SEND_NOW`. DualSense setup+initial LED is atomic
  again, so a delivered LIGHT_OUT can no longer be separated from/darken a dropped color command.
- BOOTSEL's core0 sampler still depended on core1 acknowledging a multicore-lockout FIFO IRQ inside
  200 us. DualSense Classic traffic can miss that window indefinitely. Sampling now uses an
  asynchronous cooperative SRAM handshake: core0 requests without blocking USB, core1 parks from
  its 3 ms timer with interrupts disabled, and core0 samples on a later loop iteration. There is no
  short deadline that can silently discard all observations.

**Latest milestone:** Two hardware passes on the same day. **Test 1** found report-0x05 axes
pitch/roll-swapped (yaw correct) and report-0x09 drifting while stationary; both were fixed
(axis order re-derived from the genuine controller's own capture; a stillness-gated bias tracker
added). **Test 2** confirmed the report-0x05 axis fix (Steam calibration now works) but found the
report-0x09 bias-tracker fix made **no observable difference** — root-caused to a self-defeating
stillness gate (tested raw magnitude, which a MEMS gyro's own bias can exceed, so the gate likely
never opened). Redesigned the gate around steadiness (frame-to-frame derivative) instead of
magnitude, and added a live bias/gate debug readout so the next test can confirm the mechanism
directly — **pending hardware test 3.**

**Last Updated:** 2026-07-14 — **Three real bugs reported directly by the project owner after
testing the Joy-Con2/mapping/mode-cycle work above, all found and fixed same-day:**

1. **BOOTSEL gestures (mode-cycle, pairing window, wipe) stopped registering while a controller
   was actively connected and being used, only working again after powering the controller off.**
   Root cause: `src/bt_hid/bt/bthid/devices/vendors/nintendo/switch2_ble.c`'s "print when buttons
   change" debug block had no rate limit or one-time gate (unlike every sibling driver's own debug
   prints), so it fired on essentially every button edge during active use — the exact printf-flood
   failure class this project already root-caused once before (see the "Same day, earlier pass"
   Sony-pairing regression entry below), here stalling the single-threaded core1 run loop badly
   enough that `control_timer_handler`'s 30ms tick — the only thing that calls `bootsel_poll()`,
   which all three BOOTSEL gestures depend on — effectively stopped running with useful regularity
   while a controller was connected and streaming. **Fixed**: rate-limited to 1/sec, matching
   `switch_gc.c`'s own established `last_unknown_log_ms` convention.
2. **Joy-Con 2 (L) and (R) not enumerating/being recognized on a real Switch 2 console** — the
   same class of bug GC and Pro2 originally hit ("it took matching all the system's expected
   calls"). Root cause: `switch_joycon2.c`'s vendor bulk command dispatcher was missing `case 0x11:`
   and `case 0x18:` entirely (both fall through to a bare 0-byte ACK instead of the specific
   structured replies), even though GC's own 2026-07-13 fix for the identical symptom explicitly
   documents these two command families as the leading suspect. Joy-Con2 was templated from GC's
   pattern *before* that fix existed, so it never got carried over. **Fixed**: added both cases,
   byte-for-byte identical to GC's own values (Hypothesis-tier for Joy-Con2 specifically, same
   evidence tier as the rest of this dispatcher — not independently confirmed).
3. **Stick mapping slightly off in Steam.** Root cause: `ns2_seam.c`'s `ns2_to12()` used a single
   `v*4095/255` linear scale, but the source convention (every bthid driver) treats 128 as the
   nominal rest/center value — and 128 isn't the exact midpoint of 0..255 (127.5 is), so
   `128*4095/255` truncated to 2055, not 2048, with the inverted Y axis resting at 2040 — X and Y
   were 7-8 units off center in opposite directions, for every device and every personality. Exactly
   the kind of small, asymmetric bias Steam's calibration screen is sensitive to. **Fixed**:
   `ns2_to12()` now scales the 0..128 and 128..255 halves of the range independently, so
   0/128/255 map to exactly 0/2048/4095.

All three fixed in code, both boards build clean, existing host tests re-run with no regressions
(`test_usb_mode_cycle` 11/11, `test_switch_joycon2_report` 43/43, `test_switch_gc_report` all
passing). **None of the three have been hardware-re-tested yet** — that's the immediate next step.

**Update, same day — item 1 above recurred; the printf-rate-limit fix was real but not the whole
story.** The project owner re-tested and confirmed the BOOTSEL-gesture failure specifically while
the Pico is plugged into a real Switch 2 console with a controller (DualSense) paired — but
explicitly **not** while plugged into a PC with the same controller paired. That rules out anything
Bluetooth-side (core1 doesn't know or care what's on the other end of the USB cable), and points at
core0/USB-side behavior that differs specifically against a genuine console. The concrete
architectural difference: `ns2_streaming` (`switch_pro2.c`, and the equivalent gates for
GameCube/Joy-Con2) only ever goes true against a real console, since only a genuine console
completes the full EP0/vendor identity handshake before selecting a report and starting to poll — a
PC/Steam session typically never reaches that state. Once streaming, core0's main loop
(`usb_core_task()`) becomes a continuous, tight HID-report-push loop (`tud_hid_n_report()` +
`get_global_gamepad_input()`'s `critical_section_enter_blocking()`, every cycle) — much busier than
an idle/PC-connected core0. `bootsel.c`'s `read_bootsel_locked()` was using
`multicore_lockout_start_blocking()`/`_end_blocking()`, which have **no timeout**: if core0 can't
reach a safe point to grant the lockout promptly under that load, the call — and with it core1's
*entire* cooperative run loop (LED, rumble, the whole BTstack event loop, not just BOOTSEL
sampling) — simply waits, however long that takes. **Fixed**: switched to
`multicore_lockout_start_timeout_us()`/`_end_timeout_us()` with a 2ms bound
(`BOOTSEL_LOCKOUT_TIMEOUT_US`); a timed-out sample is treated as "no observation this tick" in
`bootsel_poll()` rather than corrupting the press/release edge tracker with a guessed value — the
hold/tap timers are wall-clock-based, so they still resolve correctly across skipped samples as
long as *some* sample succeeds while the button is actually held/tapped, and a bounded 2ms cost per
tick means core0 being briefly busy can no longer stall core1 altogether. Confirmed via SDK source
read (`multicore.c`) that a timed-out `_end_` call still asynchronously signals core0's release
(fire-and-forget event, not waiting for acknowledgment) — no scenario leaves core0 stuck parked.
Both boards build clean, host tests unaffected (pure C-level logic untouched). **Not yet
hardware-re-tested** — the next test should specifically repeat the failing scenario (console +
paired controller, hold BOOTSEL) to confirm this resolves it.

**Update, same day — the BOOTSEL fix worked; confirmed Joy-Con2's real remaining bug.** The
project owner confirmed the bounded-lockout fix resolved mode-cycling on console. Joy-Con 2 (L)/(R)
still don't enumerate on a real console, and critically: **no "Paired" notification appears at all**
when switching to either Joy-Con2 personality, unlike Pro2/GameCube (which both show it) — a much
earlier failure point than the vendor-bulk-command gap (`case 0x11`/`0x18`) already fixed, which
only affects the *streaming* phase after pairing succeeds. Comparing `switch_joycon2.c`'s EP0
identity block (`switch_joycon2_ctrl_identity_l[64]`, returned for `bRequest=3` and for SPI reads at
`0x13000`) against the project's own genuine-unit SPI dump analysis
(`docs/experiments/joycon2-spi-dump-analysis-2026-07-14.md` §3.3/3.4, which documents the real,
Confirmed layout: header(2) + type code(2) at `0x13002` + **serial (12 bytes: "W"+11 digits) at
`0x13004`-`0x1300F`** + 2 reserved zero bytes + VID/PID at `0x13012`-`0x13015`) found a real,
concrete bug: the array's fictitious serial used only **9** digits after `'W'` (a 10-byte field)
instead of the Confirmed 11-digit/12-byte shape — 2 bytes short, silently shifting VID, PID, and
every byte after them 2 positions earlier than the real layout (VID landing at offset 16 instead of
the Confirmed 18). A misaligned identity block corrupts the very data (VID/PID) a console's initial
recognition handshake reads — a much more plausible explanation for never reaching "Paired" at all
than the streaming-phase gap fixed earlier. **Fixed**: extended the fictitious serial to the full
11 digits (matching the doc this file already cited), which pushes VID/PID/the trailing "01 08"
fixed bytes/colour placeholder to their Confirmed offsets; the Right-side derivation's PID-patch
indices were updated from 18/19 to the now-correct 20/21 to match. Both boards build clean, host
tests unaffected. **Not yet hardware-re-tested** — this is the leading candidate for why Joy-Con2
never showed "Paired"; worth testing before further investigating the streaming phase.

**Update, same day — Confirmed on real hardware: Joy-Con 2 (L) and (R) both enumerate, pair, and
stream on a genuine Switch 2 console.** The identity-block offset fix above was the actual root
cause — both personalities now show "Paired" and function, closing out the enumeration gap.
**Joy-Con2 output personalities move from Hypothesis-tier ("templated from GC, not independently
confirmed") to Confirmed for USB enumeration, EP0 identity handshake, and input streaming.** Button
mapping is reported as "off" (exact symptom not yet detailed) — next task, expected to be a small,
targeted fix rather than a re-architecture given the underlying report construction
(`switch_joycon2_encode.c`) already passes its full host-side golden-test suite. Remaining
Hypothesis-tier items for Joy-Con2: rumble byte semantics, motion/gyro (no source yet), NFC (Right
only, not emulated).

**Update, same day (side task) — 8BitDo Ultimate 2 "MG" (VID `0x2DC8`/PID `0x200B`) back-paddle
mapping added.** This controller was already correctly identified (VID/PID/name resolve, shown
in config mode's device panel) via the generic gamepad driver, but its 2 back paddles were
unmapped. The project owner captured raw input-report bytes directly (config mode's raw-report
debug view) for both paddles: byte 8 differs (`0x20` = left paddle, `0x04` = right paddle), every
other byte identical between the two captures. Rather than assume this model's back-paddle usage
numbers match `BITDO_BUTTON_MAP`'s existing convention (usage 3/6, established for a *different*
8BitDo paddle model) — the same "don't assume one 8BitDo model matches another" lesson the NGC
Modkit's own `is_ngc_modkit` special-case already established — added a new `is_ultimate_mg` flag
(`bthid_gamepad.c`, VID+PID gated) and a fixed-byte-offset check mirroring the existing Xbox Elite
Series 2 back-paddle pattern exactly: `data[8] & 0x20` → `JP_BUTTON_L4`, `data[8] & 0x04` →
`JP_BUTTON_R4`. These already have a working default destination with zero additional remap-table
work: `NS2_DEFAULT_MAP` routes L4/R4 to GL/GR in Pro2 mode, and `ns2_seam.c`'s Joy-Con2
`joycon2_active` reinterpretation (added earlier this session) routes them to SL/SR in Joy-Con2
mode. Both boards build clean, existing host tests unaffected. **Not yet hardware-tested.**

**Last Updated:** 2026-07-15 — **`bthid_gamepad.c` split into a shared parsing engine + one
quirk file per exact controller model/mechanism**, per explicit project owner direction after
noticing it had grown to 1016 lines of accumulating inline special cases (5 identity booleans,
5 button-usage tables, several raw-byte quirk blocks, a vendor-gated rumble block) while adding
the 8BitDo Ultimate MG paddle mapping above. New shared type `gamepad_quirk_t`
(`bthid_gamepad_quirks.h`) bundles a quirk's button-usage table (or a `select_button_map`
override for Xbox's BLE-vs-Classic and 8BitDo's paddle-vs-not distinctions), an optional
`extract_extra()` for raw-byte reads beyond the standard table (paddles, GC-native Z/detent
bits), and an optional `send_rumble()`. One ordered, most-specific-first match table
(`bthid_gamepad_quirks.c`) replaces the old implicit if/else-if priority. Seven quirks, one file
each under `quirks/{xbox,bitdo}/`: `xbox`, `xbox_elite2`, `bitdo_paddle` (the "Ultimate/Pro 2,
etc." usage-3/6 convention — named for the *mechanism*, since no exact PID list was ever
confirmed), `bitdo_ngc_modkit`, `bitdo_ultimate_mg`, `bitdo_m30`, plus `generic` as the
fallback. Naming rule going forward: name a quirk after the exact model when PID-confirmed
(so a future Xbox Elite 1/3 gets its own file, never folded into `xbox`/`elite2`), after the
mechanism when it's a deliberate cross-model fallback. Real correctness catch made during the
move (not just a mechanical extraction): Elite Series 2 controllers previously got Xbox rumble
because the old code checked `device->vendor_id == 0x045E` directly, ignoring identity
booleans entirely — naively giving `QUIRK_XBOX_ELITE2` a NULL `send_rumble` would have silently
dropped rumble for Elite 2 controllers, so both quirks now explicitly share the same
`xbox_send_rumble()`. `gamepad_task()`'s rumble dispatch re-resolves the quirk fresh (rather than
trusting the cached one) specifically so a Classic BT device that never went through descriptor
parsing still gets rumble exactly as before. Both boards build clean, all existing host tests
re-run with no regressions. Behavior-preserving refactor, not independently hardware-tested
(no protocol bytes changed) — pending routine re-confirmation next time each affected controller
(Xbox, Xbox Elite 2, 8BitDo Ultimate MG/NGC Modkit/M30, any generic pad) is on hand.

**Update, same day — comment-accuracy sweep, triggered by a project owner question about why
Xbox isn't under `vendors/microsoft/`.** Answering it surfaced a real, confirmed stale comment:
`bthid_gamepad_quirk_xbox.c`'s rumble comment (copied verbatim from the pre-refactor
`bthid_gamepad.c` during the split above) claimed `vendors/microsoft/xbox_bt.c`/`xbox_ble.c` were
"never registered... dead code... since removed," making the generic driver "the only Xbox
rumble implementation in the tree." **Confirmed false by direct inspection**: both files exist,
are registered (`bthid_registry.c`'s `xbox_bt_register()`/`xbox_ble_register()`), are compiled in
(`NS2_BT_ALL_DRIVERS` is defined for the real build, `CMakeLists.txt`), and each has its own
independently-working rumble implementation. The claim was true at an earlier point in the
project (see `docs/bluetooth/driver-reachability-audit.md`/`btstack-implementation.md` for the
real, accurately-historicized bug hunt and its "re-register as the primary Xbox path" resolution)
but was never updated afterward, and got propagated into new code during today's refactor simply
by being copied verbatim. **Fixed**: rewrote that comment to state the actual current architecture
(dedicated files own standard Xbox/Series traffic; the generic driver's Xbox quirks are reachable
only for Elite Series 2, which both dedicated files explicitly exclude by PID, plus any edge case
where a standard Xbox device fails to match them). Also found and fixed one more instance of the
same underlying staleness: `bthid_registry.c`'s own top comment still described a "Phase 0, vendor
drivers not compiled yet" bring-up state that hasn't matched the real build since `NS2_BT_ALL_DRIVERS`
was defined — rewritten to describe the current state. Swept the wider `src/bt_hid` tree and
`docs/bluetooth/` for the same class of claim (dead code / never registered / "the only X in the
tree") — the docs already correctly historicize the original bug and its fix; no other stale
instances found. Both boards build clean.

**Last Updated:** 2026-07-15 (late) — **🟡 BOOTSEL sampling moved from core1 to core0. This is the
real fix for a conflict that had been mis-diagnosed all day; both prior "fixes" were opposite
halves of the same dead end.**

**Root cause (hardware-confirmed from both directions).** BOOTSEL's raw sample must tri-state the
flash CS pin with the *other* core parked. Sampling lived on core1, which meant parking **core0** —
but core0 runs TinyUSB in a tight, unbounded loop and **cannot grant a lockout promptly while
streaming to a host**. That gave two mutually exclusive failures, both observed on real hardware:
- `multicore_lockout_start_blocking()` (committed version): core1 waits however long core0 takes →
  **BOOTSEL works, rumble breaks.** Core1's whole run loop — including rumble forwarding on its
  3 ms timer — stalls with it, so rumble stop commands land late and Xbox motors run on (loop_count
  `0xEB` keeps pulsing ~11.75 s per trigger unless superseded) → *"rumbling uncontrollably"*.
- 2 ms bounded timeout (uncommitted 2026-07-14 22:25): the sample never succeeds while a controller
  is connected → `bootsel_poll()` returns `BOOTSEL_NONE` forever → **rumble works, BOOTSEL silently
  dead.** Worst on DualSense (Classic BT), which loads core0 hardest — the project owner's own
  observation that BOOTSEL worked on Xbox/Switch 2 (BLE) but not DualSense is what pinned this down.

**Fix: invert the direction.** Core0 samples BOOTSEL itself (`bootsel_sample_core0()`, called from
`usb_core_task()`, self-rate-limited to ~5 ms) and parks **core1**, which is a cooperative BTstack
run loop that yields almost immediately (~20 µs park, <1% of core1's time). Core1's `bootsel_poll()`
now only *reads* the published sample — it never parks anyone and cannot stall, so rumble stays on
cadence. A missed sample is harmless here (retry in 5 ms, previous value retained), which is why the
same "bounded timeout" idea that broke BOOTSEL before is safe on this side of the split. Core0 stays
a lockout victim because `config_service_save()` still parks it for flash writes.

**Both boards build clean. NOT yet hardware-tested.** Expected: BOOTSEL works on all controllers
incl. DualSense, *and* Xbox/GC rumble behave (no more late stop commands). Sony rumble was reported
still absent and is **not** explained by this — the DS5 output construction was audited (wire format
`0xA2`+`0x31`+77 B payload, CRC span, buffer bounds all correct) with no defect found; treat it as a
separate open issue.

**Damage done this session, recorded honestly:** the refactored `bthid_gamepad.c` (quirk split) was
destroyed by a `git checkout HEAD --` with no backup — never committed, no editor history, not
recoverable. Lost: the architectural split and the 8BitDo Ultimate MG paddle mapping (**project
owner reports MG mappings still work on the current build**, so the loss may be limited to the
architecture). NOT lost: the Xbox rumble tuning (committed at 11:35, verified byte-identical to the
quirk version) and every `quirks/*.c` file, preserved under `_reverted_quirk_split/`.

**Process lesson that caused most of today's damage:** the working tree held a pile of
**never-flashed** changes from the prior night. The build the owner was running at 7-8am predated
them, so the first build of the day was the first time any of it ran on hardware — and I repeatedly
blamed my own edits instead of checking `ls -la` / mtimes against the last commit. **Check what has
actually been flashed before diagnosing a "regression."**

---

**Superseded:** 🔴 THE TREE WAS REVERTED TO ITS PRE-2026-07-15 STATE.
Two attempts at the triple-tap wipe fix were made; both were removed. `btstack_host.c` carries only
the 2026-07-14 stale-bond fix (28 lines) that predates this work. Read the "BOOTSEL still broken"
entry below BEFORE touching this area again — the root-cause chain this file previously asserted
is now known to rest on a false premise.**

## 🔴 BOOTSEL dead once a controller is paired — UNRESOLVED, cause unknown

Project owner, 2026-07-15 evening, on the reverted-plus-relanded build: **no BOOTSEL gesture
registers once a controller is paired — no mode swap, no pairing, no wipe.** Reported on Xbox
**and DualSense**, on **PC and Switch 2**. This blocks launch.

**Critical correction — the "printf flood starves core1" theory is almost certainly WRONG.**
`CMakeLists.txt` lines 106-107 set `pico_enable_stdio_usb(PicoSwitchWGA 0)` and
`pico_enable_stdio_uart(PicoSwitchWGA 0)`: **both stdio backends are disabled, so `printf` has no
output driver.** It costs some formatting CPU and writes nowhere — it cannot block on UART/CDC I/O.
That undermines the stated root cause of the 2026-07-14 `switch2_ble.c` fix, the 2026-07-15
pairing-gate diagnosis, and the six rate-limits added during the stack audit (all now reverted).
Those `printf`s may still be worth bounding on CPU grounds, but **"logging flood" should not be
assumed to be the mechanism again without measuring it.** Verify this claim against the build
before relying on it either way.

**What is actually known:**
- The symptom is `bootsel_poll()` (`bootsel.c`) returning `BOOTSEL_NONE`. It does that in exactly
  two places: `!usb_lockout_ready`, or `multicore_lockout_start_timeout_us(2000)` /
  `multicore_lockout_end_timeout_us(2000)` failing (`BOOTSEL_LOCKOUT_TIMEOUT_US`).
- **The 2026-07-14 bounded-lockout change converts "core0 too busy to grant the lockout within
  2 ms" directly into "BOOTSEL silently does nothing"** — which is precisely this symptom. That
  change predates the 2026-07-15 work and is uncommitted in the working tree. It is the leading
  suspect and was never ruled out.
- DualSense reproducing it matters: DualSense is **Classic BT**, so any BLE-only explanation
  (advertising handler, GATT, Switch 2 paths, connection parameters) cannot be the whole story.
  Whatever it is, it is on a **shared** path or on core0.
- **A clean baseline has never actually been tested.** Both 2026-07-15 attempts were reverted and
  relanded without an intervening hardware run, so "the revert restores BOOTSEL" is an assumption,
  not an observation. The build now in `build/` is that untested baseline.

**Next step must be measurement, not another theory.** Suggested order: (1) flash the reverted
build and confirm whether BOOTSEL works with a controller paired — this alone splits the problem
in half; (2) if still broken, instrument `bootsel_poll()`'s two failure exits (a counter surfaced
via LED blink or config mode — **not** `printf`, which goes nowhere) to learn whether it is
`usb_lockout_ready` or the lockout timeout; (3) only then decide whether core0's USB/streaming
loop is genuinely unable to grant a 2 ms lockout, and if so whether BOOTSEL sampling belongs on
core0 instead of core1.

---

**Historical (2026-07-15, both attempts now REVERTED — kept for the analysis, not as current
state).** Root cause of the *wipe* bug, traced through `btstack_host.c`:
none of the **four** admission points that decide whether a controller gets connected — BLE
advertising auto-connect (`GAP_EVENT_ADVERTISING_REPORT`), Classic inquiry auto-connect
(`GAP_EVENT_INQUIRY_RESULT`), the device-initiated `HCI_EVENT_CONNECTION_REQUEST`/
`HCI_EVENT_CONNECTION_COMPLETE` path (DS3/DS4/DS5-style), and `HID_SUBEVENT_INCOMING_CONNECTION`'s
unconditional `hid_host_accept_connection()` — ever consulted bond state. Each only
checks "is this a recognized controller type" (advertised manufacturer ID/name/class-of-device),
and background BLE scanning + Classic inquiry run continuously (see the entry below this one).
So `btstack_host_delete_all_bonds()` deleted the *old* crypto bond, but nothing stopped the
always-on auto-connect/auto-accept loop from silently rebuilding a *new* one on the very next
advertisement/inquiry result — or, for Switch 2's custom `SW2_CMD_PAIRING` handshake, completing
a handshake that never used BTstack bonding to begin with (`btstack_host.c`'s
`profile->ble == BT_BLE_CUSTOM` branch skips SM pairing entirely). Separately, the wipe never
cleared `hid_state.has_last_connected`, so the periodic 20s bonded-reconnect loop kept calling
`gap_connect()` back to the wiped device's own address regardless of any of the above.

**First attempt (reverted, do not resurrect).** It added a `pairing_required` flag with
`!pairing_required` gates on three of the four admission points, rejecting path 3 by
`gap_disconnect()`-ing the ACL after BTstack had already auto-accepted it. It built clean on both
boards and on `-DNS2_PRO=OFF`. **On real hardware (project owner, 2026-07-15, Xbox + DualSense, on
both PC and a Switch 2) it broke two things that worked before it: BOOTSEL gestures stopped
registering while a controller was connected, and rumble stopped working entirely.** Two defects,
both found by code trace afterward:

1. **There are FOUR admission paths, not three.** `HID_SUBEVENT_INCOMING_CONNECTION` calls
   `hid_host_accept_connection()` unconditionally (`btstack_host.c`, ~line 4979) and was never
   gated. So the `HCI_EVENT_CONNECTION_COMPLETE` reject fought the HID layer's accept, producing
   connect/reject churn plus a printf per cycle. That starves core1's cooperative run loop, so
   `bootsel_poll()`'s 2 ms budget (`BOOTSEL_LOCKOUT_TIMEOUT_US`, `bootsel.c`) times out every tick
   and returns `BOOTSEL_NONE` forever — the same core1-starvation class already documented twice in
   this file. Input still trickles through, so the device *looks* paired and working.
2. **The reject path set `classic_state.pending_valid = false`**, which skips the name/COD copy at
   `HID_SUBEVENT_INCOMING_CONNECTION` (~line 4993). The device is then accepted with no name →
   driver matching falls through to the generic gamepad driver → `send_rumble` is NULL for
   DualSense → silent rumble loss. This is the reported rumble regression.

**Correction to an earlier claim in this file:** that attempt also added a flash write
(`btstack_host_save_last_connected()`) inside `btstack_host_delete_all_bonds()`, and this file
previously called that the "likely reason" the flag got stuck. **That claim was speculation and is
withdrawn** — `btstack_host_delete_all_bonds()` *already* does TLV flash I/O from that same core1
timer context via `gap_delete_all_link_keys()`/`le_device_db_init()`, and `config_service_save()`
does an explicit `multicore_lockout_start_blocking()` erase+program from the very same
`control_timer_handler`, both long-established and working. Flash I/O there is a real hazard class
worth respecting, but it was never demonstrated to be a cause here. Defects 1 and 2 above are the
code-proven ones.

## Post-wipe pairing lockout (⬜ REVERTED 2026-07-15 — design kept for a future reland only)

**This code is NOT in the tree.** It was written, build-verified, and removed the same day when
BOOTSEL remained broken on hardware. The design is recorded because it is sound and worth
resurrecting *after* the BOOTSEL problem above is genuinely understood — but note it was never
validated, and its safety argument leaned partly on the printf-flood theory that the stdio finding
above now casts serious doubt on. Contract, per explicit project-owner direction: *triple-tap =
forget everything and reject every controller until the user explicitly opens a pairing window.*
The mechanism was deliberately the opposite shape of the first attempt: **never reject anything
after the fact — stop the dongle from being reachable at all.** A `pairing_lockout` flag set by
`btstack_host_delete_all_bonds()`, cleared only by `btstack_host_clear_pairing_lockout()` (wired
into `cyw43_transport_set_pairing_mode(true)`, the double-tap path), closing all four paths at the
two choke points upstream of them:

- `btstack_host_start_scan()` returns early → no scan, no inquiry → **paths 1 and 2** can never
  fire (their handlers require `BLE_STATE_SCANNING` / `inquiry_active`).
- `gap_connectable_control(0)` + `gap_discoverable_control(0)` → no incoming ACL is ever
  established → **paths 3 and 4** can never fire. No `gap_disconnect()` churn, because there is no
  connection to reject.

**The safety argument made at the time** was: when `pairing_lockout` is false, every path is
byte-for-byte the baseline; the flag is read in only four places (`start_scan()`, the recovery
watchdog, the idle safety net, its accessors), all cheap bool reads; nothing is added to the
connected/streaming path or to any of the four admission handlers. **That argument appears to have
been correct as far as it went — and BOOTSEL was still broken with this code in the tree.** Which
is the strongest available evidence that the BOOTSEL fault is independent of all of it, and was
likely never caused by the 2026-07-15 work at all. Do not read this section as "the lockout broke
BOOTSEL"; read it as "the lockout did not fix it and did not obviously cause it."

Two defects were found by tracing during implementation and fixed before landing (both real, and
both worth re-applying in any reland):

1. **The idle safety net would have printf-flooded every tick.** During a lockout *every* condition
   it tests is satisfied (state `IDLE`, nothing connected, not scanning), and its
   `"Safety: idle with no connections, resuming scan"` log fires *before* the `start_scan()` call —
   so guarding `start_scan()` alone was **not** sufficient. Now guarded on `!pairing_lockout`.
2. **The classic recovery watchdog would have rebooted the board after 10s**, silently discarding
   the lockout (it is session state) and letting every wiped controller back in. It infers "BT
   transport is dead" from absent inquiry activity, which a lockout guarantees;
   `recovery_start_time` is cleared *only* by `GAP_EVENT_INQUIRY_COMPLETE`. Reachable in practice
   because the connection-timeout loop can set it *after* a wipe (connection slots are cleared
   asynchronously on `HCI_EVENT_DISCONNECTION_COMPLETE`). Now guarded on `!pairing_lockout` and
   explicitly disarmed in the wipe.

Also handled: `pairing_close_deferred` is cleared by the wipe, so `ns2_bt_host.c`'s
`open_pairing_window()` early-return can never strand the lockout with no way to clear it. The
in-memory last-connected record is cleared (no flash write — not needed, see above). Both boards
build clean; `-DNS2_PRO=OFF` (shared Switch-1 path) verified clean.

**Reland preconditions:** do not restore this until (a) the BOOTSEL fault above is understood and
fixed, and (b) the wipe bug is re-confirmed to still exist on a known-good build. Also note the
lockout creates a **trap** if BOOTSEL is unreliable: a triple-tap engages it, and *only* a
double-tap clears it — if BOOTSEL is dead, no controller can ever connect again until reflash.
Any reland should carry a non-BOOTSEL escape hatch (timeout or config-mode clear).

**Known limitation (deliberate, documented in code):** the lockout is session state — a power-cycle
clears it, after which a wiped controller can be auto-admitted and silently re-bonded (SSP
auto-accept). Closing that needs a persistent per-device trust record, which is required for
Switch 2 anyway (it never creates a BTstack bond to delete) and remains the tracked follow-up.

## Bluetooth stack audit (2026-07-15) — findings kept, code REVERTED

Full read-only audit of `src/bt_hid/**`. **The six rate-limits described below were reverted along
with everything else; the tree does not contain them.** They are recorded as findings, not fixes.

⚠️ **Read the stdio correction in the BOOTSEL entry above first.** This audit was framed around
"unbounded `printf` starves core1", but `printf` has **no stdio backend** in this build
(`CMakeLists.txt` 106-107), so it writes nowhere and cannot block on I/O. These sites are still
unbounded formatting work on a per-report path and are defensible to bound on CPU grounds — but the
severity claimed here was inherited from an unverified mechanism, and **the fault this audit was
meant to explain persisted with all six rate-limits applied.** Treat the table as "unbounded work
in a hot path, priority unknown", not as a root cause. Each site fires on **every report** at the
controller's full rate once its condition holds:

| Site | Condition that triggers it |
|---|---|
| `bthid.c` (default transaction case) | **Any** BT device sending a non-DATA/non-HANDSHAKE transaction — e.g. DATC (`0xB0`) continuation fragments. Shared path for every device. |
| `switch2_ble.c` (`!sw2` no driver data) | Init failed / no free device slot — floods exactly when something is already wrong |
| `switch2_ble.c` (report too short) | Any short notification routed here; SW2 notifies on more than one characteristic |
| `ds4_bt.c` (unknown report) | Any `0x11` < 12 B, any `0x01` < 10 B, or unknown ID — DS4 **clones** commonly deviate |
| `ds5_bt.c` (unknown report) | Same shape as DS4 |
| `stadia_bt.c` (report too short) | Any persistently short report |

The reverted patch used the convention already established in `wiimote_bt.c` (~line 582) and
`switch_gc.c` (`last_unknown_log_ms`): a 1–2 s window. It was logging-frequency only, altering no
control flow. Still worth noting regardless of mechanism: `switch2_ble.c`'s 2026-07-14 fix
rate-limited one `printf` in `switch2_ble_process_report()` and left two others (`!sw2` no-driver-
data, and report-too-short) unguarded in the same function.

**Audited and found correct (no change made):** output-report buffers are bounds-checked before
`memcpy` (`btstack_host.c` ~5585/5595, incl. the 79-byte DS5 BT case); `bthid_set_hid_descriptor()`
bounds-checks against `BTHID_MAX_DESC_LEN`; DS4/DS5 report structs are length-checked before cast;
`wiimote_bt.c` and `xbox_ble.c`'s diagnostics are already guarded (the latter swaps drivers, so it
is effectively one-shot); `bthid.c`'s unknown-device log is capped at 3.

### 🔵 Latency: BLE connection parameters are left at BTstack's defaults — NOT changed, needs a hardware A/B

`gap_set_connection_parameters()` and `gap_request_connection_parameter_update()` are **never
called anywhere in this repo**, so every BLE connection we initiate (we are always central) uses
BTstack's defaults from `hci.c` ~5032:

| Parameter | BTstack default | Appropriate for a gamepad |
|---|---|---|
| `le_connection_interval_min` | `0x0008` = 10 ms | 6 (7.5 ms) |
| `le_connection_interval_max` | `0x0018` = **30 ms** | `0x000C` = 15 ms |
| `le_connection_latency` | **4** | **0** |
| `le_supervision_timeout` | `0x0048` = 720 ms | unchanged |

These are BTstack's power-saving defaults aimed at sensors. `latency = 4` is simply wrong for HID:
slave latency lets the peripheral skip up to 4 connection events, which mainly delays
**central→peripheral** traffic — i.e. **rumble** — by up to `latency × interval` (≈120 ms at the
default 30 ms). Input (peripheral→central) is less affected, since a peripheral with data may
transmit at the next event, but the 30 ms interval still caps input rate.

**Why this was NOT applied despite being a real finding:** (a) the benefit is device-dependent and
unmeasurable without hardware — BTstack *auto-accepts* peripheral-requested parameter updates
within `le_connection_parameter_range` (`l2cap.c` ~4346), so controllers that request their own
preferred interval already get it, and these defaults only bind devices that never ask; (b) more
importantly, **Switch 2's GATT init retry timing is hardware-tuned** (see the 2026-07-12 entry) and
plausibly interacts with connection-interval changes — shipping that unvalidated alongside an
already-untested lockout fix would confound the next hardware session, which is exactly the mistake
that produced the regression above.

**Proposed patch, ready to apply once the lockout is validated** — one call in `packet_handler()`'s
`HCI_STATE_WORKING` block, before `btstack_host_start_scan()`:
```c
// conn_scan_interval, conn_scan_window (BTstack defaults), interval_min, interval_max,
// latency, supervision_timeout, min_ce_len, max_ce_len
gap_set_connection_parameters(0x0060, 0x0030, 0x0006, 0x000C, 0, 0x0048, 0, 0);
```
Validity check: supervision timeout must exceed `(1 + latency) × interval_max × 2` = 30 ms; 720 ms
passes with wide margin. Test as its own A/B: confirm BLE controllers still connect and stay
connected, then compare rumble responsiveness (the parameter with the clearest predicted effect).

**Also worth noting:** the software input path itself is already tight and offers little to gain —
reports are event-driven (BTstack callback → driver → `router_submit_input()`), not polled, and
rumble forwarding already runs on a dedicated 3 ms timer (`RUMBLE_TICK_MS`). The remaining latency
is dominated by the BLE connection interval above and the USB poll rate, not by our own code.

**Previous update:** 2026-07-15 — **Real bug found and fixed: stale BLE bonds weren't
self-healing, forcing a manual BOOTSEL triple-tap (wipe bonds) to re-pair a genuine Pro
Controller 2 / GameCube controller.** The project owner reported a "small regression" —
needing to triple-tap instead of just entering the controller's own pairing mode — and clarified
no double-tap step was involved at all, which ruled out gesture detection (`bootsel.c`, changed
this session) as the cause; confirmed via `git diff` that `btstack_host.c` (the actual bonding/
reconnect logic) had zero changes this session. Root cause, traced through the code: background
BLE scanning and periodic bonded-device reconnect run continuously (`btstack_host_start_scan()`'s
`scan_start_time`/`BLE_RECONNECT_INTERVAL_MS` tracking), not just during an explicit pairing
window. A genuine controller's BLE key relationship gets re-established whenever it's re-paired
elsewhere (e.g. back to the owner's actual Switch 2 between test sessions with this dongle),
making this dongle's stored bond stale. `SM_EVENT_REENCRYPTION_COMPLETE`'s failure branch already
auto-deletes a stale bond and re-pairs — but only if the failed attempt reaches a clean SM
completion event. If the peer instead just drops the link outright (an HCI-level authentication
failure or missing-key condition, the more likely real behavior for a genuine key mismatch),
`HCI_EVENT_DISCONNECTION_COMPLETE`'s handler was blindly retrying the *same* stale bond up to 5
times before giving up — never touching the bond itself. **Fixed**: that handler now checks for
`ERROR_CODE_AUTHENTICATION_FAILURE`/`ERROR_CODE_PIN_OR_KEY_MISSING` specifically and deletes the
local bond before retrying, so the next connect attempt performs a genuinely fresh pairing
instead of repeating the same doomed handshake — scoped narrowly to those two reasons so a real
supervision timeout/link-quality disconnect still retries with the existing bond unchanged, as
before. Both boards build clean, host tests unaffected (file has no host-testable pure-logic
surface). **Not yet hardware-tested** — the next real-hardware session should confirm a
Pro Controller 2/GameCube controller re-pairs automatically after this fix, without needing a
manual bond wipe, the next time its key gets rotated by pairing to another host.

**Previous update:** 2026-07-14 — **Joy-Con 2 moves from "complete blank slate" to real hardware
evidence.** The project owner obtained genuine Joy-Con 2 L and R hardware plus full SPI flash dumps
of each (`dumps/SWITCH2_JOYCON_L_1.bin`/`_R_1.bin`). Full analysis:
`docs/experiments/joycon2-spi-dump-analysis-2026-07-14.md`; new Stage A doc:
`docs/switch2-joycon2/protocol.md`. Highlights: Joy-Con 2 L/R USB PIDs (`0x2067`/`0x2066`) now
**Confirmed** from a second, independent source (the genuine unit's own flash, not just the kernel
driver); a previously-undocumented per-model hardware type code (`HB`/`HC`, vs `HE` Pro2/`HH` GC); a
genuinely new finding that the factory-calibrated accelerometer bias lands on a different axis
position for Joy-Con than for Pro Controller 2 (different IMU mounting orientation — a real
implementation can't reuse Pro2's axis mapping as-is); confirmation that a single Joy-Con's stick
calibration only populates one of the two available calibration slots (hardware has one stick, not
two); and confirmation the Pro2-only "DSPH" audio DSP blob is absent on both Joy-Con and GameCube,
consistent with it being tied to Pro2's headphone jack specifically.

**Update, same day — USB device + configuration descriptors now Confirmed byte-exact for both
sides.** Both Joy-Cons were already connected to this machine and enumerated in Windows; a USBPcap
`--inject-descriptors` capture (same non-destructive method as GameCube's own Stage B evidence)
replayed their cached descriptor-fetch transactions with zero replug needed. Result: byte-exact
device descriptors (PID-only difference between L/R, `bcdDevice 0x0100` both — a *third* independent
source now agreeing with the kernel driver and the SPI flash on the PID assignment) and a
byte-exact, 80-byte configuration descriptor **structurally identical to GameCube's own** (same
IAD+HID+vendor-bulk shape, same endpoint addresses/types/sizes) — strong direct confirmation that
GameCube, not Pro2, is the right implementation template. Also found: a third Nintendo-VID USB
device on the same hub, PID `0x2068`, hub-class — the owner's own USB adapter, not a controller.
Not yet captured (at that point): the HID Report descriptor body and string descriptor text — both
need a live capture spanning a replug, not just the injected-cache method. Raw capture:
`docs/experiments/joycon2-captures/genuine-controller-descriptors-2026-07-14.pcap`.

**Update, same day — HID Report descriptor also now Confirmed byte-exact.** A software
disable/enable cycle (tried first, to avoid asking for a physical replug) doesn't trigger a real USB
bus reset — no lower-level reset tool was available either. The project owner did a real
unplug/replug of Joy-Con 2 Left while a live capture ran, catching the full enumeration: the 100-byte
HID Report descriptor (three report IDs — `5` input flat-vendor, `7` input structured [16 buttons +
one 12-bit X/Y stick + vendor data], `1` output — confirming the single-stick/fewer-buttons shape
already inferred from the SPI factory data, this time from the wire format itself), plus interface
strings `"If_Hid"` (index 5, same as GameCube's) and `"Joy-Con 2 (L)"` (index 6). **Report ID 7 as
the console-facing extended report is new information** — Pro2/GameCube both use `0x0A` for this
role; a future implementation must not assume that carries over. Raw capture:
`docs/experiments/joycon2-captures/genuine-controller-full-enumeration-replug-2026-07-14.pcap`. Full
writeup: `docs/switch2-joycon2/protocol.md`.

**Update, same day — full field-level button/report mapping found in already-cloned reference
material.** `ndeadly/switch2_controller_research` (cloned earlier for GameCube work, at
`E:\nso-gc-refs\switch2_controller_research`) turned out to already have complete Joy-Con 2
sections in `descriptors.md` (byte-for-byte matching this project's own fresh USB capture — good
cross-validation), `hid_reports.md` (full field-level button bitmaps and report layout for both L
and R, backed by real decrypted nRF52840 BLE captures), and `bluetooth_interface.md`. Confirms R's
USB HID report ID is `8` (matching the BLE-side numbering already documented there, so no separate
R-side USB capture was needed), gives complete button bitmaps for both sides, confirms NFC exists
only on the Right Joy-Con 2, and reveals a "mouse mode" (relative delta-X/Y sensor data) neither this
project nor its docs had previously known about. Also found: real component datasheets (a TDK
ICM-42670-P IMU, an NXP PN7160/7161 NFC controller, a MAX98388/98389 audio amp consistent with the
"Pro2-only DSPH blob = headphone jack" theory) — noted but not analyzed in depth this pass.

**Also same day — wake-from-sleep design substantially strengthened.** The same reference repo's
`bluetooth_interface.md`/`commands.md` document the exact byte-exact wake advertisement format: it's
the ordinary reconnection advertisement with **one flag bit changed** (`0x81` vs `0x00` at a fixed
offset), plus the target's VID/PID (already known) and the bonded console's own BD_ADDR (already
structurally decoded from this project's own SPI dump analysis). There's even a documented command
(`0x03`/`0x01`, "Bluetooth Wake") that a genuine controller responds to by broadcasting this. This
moves the feature from "unknown payload, needs a capture" to "known payload, needs a BLE
advertising-transmit implementation" — `docs/bluetooth/wake-from-sleep-design.md` fully rewritten.

**Remaining open architectural question**: the project owner has decided Joy-Con 2 L/R should be
**one runtime-selectable personality**, configurable in the existing CDC config UI to present as L,
R, or **both sides merged into a single virtual controller identity** (Switch 1 "Joy-Con Grip"-style
pairing) — a three-way choice, not a simple side-select toggle. The merged-input policy (how two
physical source controllers' — or one controller's — worth of input folds into one Joy-Con2 report
stream) is not designed yet; flagged as the next real design question, not blocking further evidence
gathering. See `docs/switch2-joycon2/protocol.md` "Open questions."

**Update, same day — Joy-Con 2 Stage B+C implemented, both boards build clean, 32/32 host-side
golden tests pass.** New `include/switch_joycon2.h`/`switch_joycon2_encode.h` +
`src/switch_joycon2/switch_joycon2.c`/`switch_joycon2_encode.c`, templated closely from
`switch_gc.h`/`.c`. `USB_PERSONALITY_JOYCON2_L`/`_R` are now `true` in `usb_mode_cycle.c` — the BOOTSEL
mode-cycle is Pro2 → GameCube → Joy-Con2 Left → Joy-Con2 Right → Config. What's solid: USB device/config/HID Report
descriptors for both sides are the real Confirmed bytes from this session's captures; the input
report encoders (report `0x07`/`0x08` structured, report `0x05` shared) implement the full
Confirmed field/button layout from `ndeadly`'s docs, verified by `tools/test_switch_joycon2_report.c`
(32 checks, all passing) the same way `switch_gc_encode.c` has its own golden tests. What's
Hypothesis, matching exactly the evidence tier GameCube's own Stage D started at before hardware
testing corrected it: the EP0 identity handshake bytes, the vendor bulk command responses beyond
what's structurally shared across the whole controller family, and the rumble byte semantics
(deliberately implemented as a conservative "any nonzero = on at a fixed amplitude" model, *not*
assuming GameCube's own "not a linear amplitude byte" lesson transfers without evidence — see the
file's own comment). Side is never user-facing: `USB_PERSONALITY_JOYCON2_L` and `_R` are two
separate, always-available personalities, and `usb.c`'s `usb_reset_personality_state()` calls
`switch_joycon2_set_side()` automatically based on which one the BOOTSEL cycle just selected —
there is no config-UI toggle and none is planned.

**Final architecture correction (project owner, 2026-07-14): no merged/paired L+R mode, ever, on
one Pico.** An earlier same-day pass explored making both Joy-Con identities appear concurrently
from a single Pico (driven by one paired controller, e.g. Xbox, as a "complete Joy-Con 2 set");
that was investigated and conclusively ruled infeasible on current hardware — RP2040/RP2350 can
only hold one USB device address at a time (register-level limit, not a TinyUSB gap), a genuine
Charging Grip is a real 3-device USB hub topology (hub + independently-addressed L + R, confirmed
via live capture), and `Dycool/NS-PC-Control` (a comparable Linux-gadget project) independently
rejects L+R pair mode for the same reason. Settled final shape: Pro Controller 2
(`USB_PERSONALITY_SWITCH2_PRO2`) is the default, primary, production-quality personality for using
one paired controller as a complete Switch 2 controller. Joy-Con 2 Left and Right remain two
separate, individually-selectable **experimental/test** personalities for hardware validation only
— never presented as the recommended full-controller mode, and named `Joy-Con 2 Left
(Experimental)` / `Joy-Con 2 Right (Experimental)` everywhere a personality name is shown to a
human. See `docs/switch2-joycon2/protocol.md` "Why not simultaneous L+R" for the full evidence
writeup. LED mode-cycle acknowledgement was generalized to `flashes = personality_ordinal + 1`
(1 Pro2 / 2 GameCube / 3 Joy-Con2 Left / 4 Joy-Con2 Right / 5 Config) — no personality gets
special-cased "experimental" LED behavior, Joy-Con2 L/R just occupy their ordinary slot in the same
counting scheme every other personality already uses.

**Update, same day — Joy-Con2 button mapping: the SL/SR gap fixed, everything else already
worked.** Auditing what a generic bridged controller (Xbox/DualSense) actually needs mapped onto
each Joy-Con2 personality found that A/B/X/Y, D-pad, L/R, ZL/ZR, stick+click, Plus/Minus, Home/
Capture, and C all already worked correctly with zero new code — Joy-Con2's encoder reads the same
`SWITCH_MASK_*`/`SWITCH_EXTRA_*` fields Pro2 already populates via the existing per-family remap
pipeline (`ns2_seam.c`'s `router_submit_input()`), just side-gated. The one genuine gap: **SL/SR**,
real physical rail buttons on both Joy-Con units with no Pro2/GameCube equivalent, hardcoded to 0
and explicitly marked "not sourced" in the encoder. Fixed: new `SWITCH_EXTRA_SL`/`SR` bits
(`include/switch_pro.h`), wired into both the structured report 7/8 encoder and the shared report
`0x05` encoder (`switch_joycon2_encode.c`), sourced by reinterpreting the existing
`NS2_DST_GL`/`NS2_DST_GR` destinations as SL/SR whenever a Joy-Con2 personality is active
(`ns2_seam.c`'s `ns2_apply_dst()`, gated the same way GameCube's own `gc_active` analog-fold
suppression already is) — GL/GR mean "grip button," which a lone Joy-Con2 physically doesn't have,
so the same generic-controller paddle/extra source buttons that default to GL/GR in Pro2 mode
instead drive a real Joy-Con control here. Pro2/GameCube mode unaffected (gated on
`joycon2_active` alone). Also clarified and documented (project owner framing, 2026-07-14): the
Switch's "sideways" single-Joy-Con reinterpretation is entirely console-side software — this
project only needs to report physical button positions correctly, exactly as a genuine Joy-Con
does, with no rotation/remap layer of its own. Full writeup:
`docs/switch2-joycon2/mapping.md`. Host-tested: `tools/test_switch_joycon2_report.c`, 43/43 checks
(8 new SL/SR cases added). Both boards build clean. **Not yet hardware-tested.**

**Update, same day — Config mode is no longer terminal; BOOTSEL hold now exits it live back to
Pro2.** Raised by the project owner as a usability gap: previously, once in CDC Config mode, the
only way out was unplug/replug or reset (deliberate initial scope limit from `NSO-GC.md`, "do not
add a live Config-to-Pro2 exit path in this pass" — not a technical constraint). Fixed:
`usb_mode_cycle.c`'s `usb_next_personality()` now wraps `CDC_CONFIG` back to `SWITCH2_PRO2` instead
of returning itself unchanged, and `ns2_bt_host.c`'s `control_timer_handler()` no longer suppresses
BOOTSEL_HOLD detection while `g_usb_config_mode` is true — only the pairing/wipe gestures
(double-tap/triple-tap) stay suppressed there, since those still make no sense mid-config-session.
`!NS2_PRO` (Switch-1-only) builds are unaffected — no cycle exists there, so this doesn't change
that build's behavior at all. Full cycle is now `Pro2 → GameCube → Joy-Con2 Left → Joy-Con2 Right →
Config → (BOOTSEL hold) → Pro2`, a closed loop; power-cycle/reset still also returns to Pro2
unconditionally, as an independent recovery path. Host-tested: `tools/test_usb_mode_cycle.c`,
11/11 checks (wrap assertion rewritten). Both boards build clean. **Not yet hardware-tested.**

**Update, same day — first hardware test, and a real bug found + fixed.** The Joy-Con2(L)
personality enumerated under Windows "Other devices" with **Code 28** ("no compatible drivers") —
the exact symptom Pro2 and GameCube each already hit and fixed via a deliberate `bcdDevice`
deviation (`docs/switch2-gc/usb-personality.md` "`bcdDevice` WinUSB-cache collision"). Root cause
here: this implementation used the real captured `bcdDevice` (`0x0100`) verbatim instead of
applying that same fix — and this exact machine has genuine Joy-Con 2 L/R hardware whose own real
binding was already cached from this session's earlier captures, so the collision was closer to
guaranteed than hypothetical. **Fixed**: both device descriptors now use `0x0110` (1.10), matching
the established pattern. **Not yet re-tested after this fix** — next thing to flash and check.

**Same day, earlier pass — REGRESSION found and fixed, after the Gate 2 driver-audit
pass below broke Sony pairing entirely (DualSense, DualSense Edge, DualShock 4 all stopped pairing
— reported directly by the owner while testing).** Root cause: `HCI_EVENT_CONNECTION_COMPLETE`'s
direct-L2CAP outgoing-connect branch (the path all Sony devices are forced through on CYW43,
`btstack_host.c` ~line 1799) issues a narrow SDP PnP-ID query using the *shared*
`classic_state.pending_vid`/`pending_pid` scratch fields — but, unlike the `hid_host_connect()` path
used by Xbox/etc. (which explicitly zeroes these two fields immediately before its own equivalent
query, ~line 5172), this direct-L2CAP path never did. A stale VID left over from a previous
connection attempt (or an unanswered/failed query for the current device) could sit in those fields
and then get handed to `bthid_update_device_info()` as if it were freshly resolved. This was
harmless before today, because nothing downstream ever rejected a driver over a wrong VID — but the
"Gate 2" VID-reject guards (`ds4_bt.c`'s original 2026-07-12 one, and today's newly-added ds3/ds5/
wiimote/wii_u_pro ones) now DO reject on exactly that signal, so a stale non-Sony VID would cause a
correctly-bound Sony driver to be discarded mid-pairing. **This is very likely also the actual root
cause of the original "DS4 won't pair" complaint that started this whole investigation** — DS4's own
guard, landed 2026-07-12, could have been silently breaking DS4 via this same mechanism the whole
time, which fits "really weird"/intermittent far better than any of the hypotheses considered
earlier that day.

**First fix attempt (partial, not sufficient on its own):** reset
`classic_state.pending_vid = 0; classic_state.pending_pid = 0;` immediately before the
direct-L2CAP path's SDP query, mirroring what the `hid_host_connect()` path already does. Both
boards built clean, but **hardware re-test showed all three (DualSense, DualSense Edge, DualShock
4) still would not pair** — the scratch-field staleness was real and worth fixing regardless, but
it was not the whole story (or not the dominant factor) behind this specific regression.

**Second fix attempt (also not sufficient on its own): removed the VID-reject guards entirely**
from `ds3_bt.c`, `ds4_bt.c`, `ds5_bt.c`, `wii_u_pro_bt.c`, and `wiimote_bt.c` — reverted today's
Gate-2-consistency additions in the last four back to byte-identical-with-pre-session (confirmed via
`git diff`), and removed `ds4_bt.c`'s original 2026-07-12 guard too. Both boards built clean, but
**hardware re-test showed DualSense/DualSense Edge/DualShock 4 still would not pair** — confirmed
directly by the project owner, who also confirmed they perform a fresh triple-tap (wipe all bonds)
before every pairing attempt, ruling out stale bond/link-key data as an explanation. Since
`ds3_bt.c`/`ds5_bt.c`/`wii_u_pro_bt.c`/`wiimote_bt.c` were at this point byte-identical to
pre-session and DualSense (handled by `ds5_bt.c`) still failed, the cause had to be in
`btstack_host.c`, the one Classic-BT-relevant file still carrying session changes.

**Third fix: removed the passive "wake-from-sleep capture" diagnostic** added earlier this session
to the BLE `GAP_EVENT_ADVERTISING_REPORT` handler — it logged every unrecognized BLE advertisement's
full raw payload via a byte-by-byte `printf()` loop, which could plausibly flood the single-threaded
cooperative run loop shared with the Classic BT connection state machine. **Hardware re-test
confirmed this fixed pairing** — DualSense/DualSense Edge/DualShock 4 all paired again. But the same
test surfaced two NEW symptoms: DualSense/Edge had no rumble, and BOOTSEL gestures (including the
mode-cycle hold, blocking the NSO GameCube personality switch) and the pairing LED were misbehaving.

**Fourth fix: reverted the `pending_vid`/`pending_pid` reset from the first fix attempt.** That
reset was specifically added to protect against the (now-removed) VID-reject guards acting on stale
data — with the guards gone, the reset no longer serves that purpose, and was reconsidered as a
likely cause of the new rumble symptom instead: `bthid_update_device_info()` (which re-confirms or
re-binds a device's driver, by name as well as VID) is only called when
`classic_state.pending_vid || classic_state.pending_pid` is nonzero. Forcing both to zero right
before a PnP-ID SDP query means that if the query gets no response — plausible for Sony devices
given this whole code path exists because their SDP responses are unreliable on this chip — that
re-confirmation call is silently skipped, leaving the device on whatever driver it was initially
bound to instead of being properly reconfirmed. Reverting this leaves `btstack_host.c` with only two
changes versus its pre-session state: the direct-L2CAP-for-Sony consolidation (verified
behavior-identical to the original triplicated code via `git diff`) and a cosmetic Joy-Con 2 L/R
display-label fix scoped to an unrelated BLE codepath. Both boards build clean.
**Not yet hardware-retested.**

**On the BOOTSEL/LED/mode-switch reports specifically:** verified via `git status`/`git diff` that
`ns2_bt_host.c` (BOOTSEL gesture handling, LED rendering, mode-cycle request), `usb.c` (mode-cycle
handling on core0), `bootsel.c` (gesture/tap detection), and `config.c` are **completely untouched**
by any change this session — zero diff against pre-session HEAD. Nothing currently in this session's
changeset touches those files or any state they read. If these symptoms persist on the next test,
they are very unlikely to be caused by anything in this session's work, and are worth checking
against whether they predate today entirely.

**Same day, earlier passes:** NSO GameCube Controller: button mapping fully confirmed working on
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
(`DATA.md` §9).

**Same day, later pass (2026-07-14):** investigated a user report that a DS4 (DualShock 4) won't
pair over Bluetooth. Code audit of `btstack_host.c`'s Classic BT connection layer found the
routing itself is **not** DS4-specific — DS4 and DualSense (DS5) share the exact same
`BT_PROFILE_SONY` profile and the same CYW43 direct-L2CAP-forced connection path (`bt_device_db.c`),
so if DS5 pairs, the shared BT-layer machinery is very unlikely to be the DS4-specific cause. Along
the way, found and fixed a real DRY violation: the "force direct L2CAP for Sony on CYW43" check was
duplicated inline at 3 outgoing-connect call sites (with an identical `printf`) — consolidated into
one `bt_cyw43_force_sony_direct_l2cap()` helper so future changes to the condition can't drift out
of sync between sites again. Behavior-preserving, not yet hardware-tested. The leading remaining
suspect for the actual "can't pair" symptom is downstream of pairing: `ds4_bt.c`'s CRC32/`hw_control`
output-report fix (2026-07-12) is still marked not-hardware-tested, and `ds4_task()` sends that
report unconditionally ~100ms after connect to trigger enhanced report mode — if malformed, this
could cause the DS4 to drop the link right after connecting, which would look like a failed pairing
rather than an output/rumble bug. Recommended next diagnostic when hardware is available: check
whether disabling that first `ds4_send_output()` call changes the symptom (isolates BT-layer pairing
from the output-report handshake).

**Stronger finding, same pass, from tracing the fallback path directly (not yet fixed — see
reasoning below for why):** confirmed DS4 and DS5 share the exact same `BT_PROFILE_SONY` profile
struct, so the routing itself can't differ between them by device type — but there's a real,
confirmed logic gap in `HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE`'s failure branch (name-request-failed
fallback). If a device's name isn't in the inquiry EIR *and* the subsequent remote name request
times out or fails, `classic_state.pending_profile` is guaranteed to still be `BT_PROFILE_DEFAULT`
(it can only become Sony via a name match, and there was never a name) — so
`bt_cyw43_force_sony_direct_l2cap()` can never return true here, and the connection falls through to
plain `hid_host_connect()`, the exact SDP path this whole direct-L2CAP detour exists to avoid ("SDP
responses from DS4/DS5 crash the CYW43 SPI bus"). The comment directly above this code says it
"handles DS4, DS3, and other controllers that may not respond to name requests" — but by the time
this branch runs, there is no longer any information left to actually detect Sony. This would present
as intermittent, seemingly random pairing failures specifically when a controller's BT radio doesn't
answer a name request promptly — a good match for "really weird" behavior reported by a user, more so
than a hard, always-reproducing failure.

**Deliberately not fixed this pass.** The one available fallback signal at this point is Class of
Device (`is_gamepad`/`is_joystick`, already parsed from `cod`), but COD alone can't safely
disambiguate Sony (needs direct L2CAP) from other Gamepad-COD devices like Xbox controllers (which
correctly want `hid_host_connect()` and have no SDP-crash problem) — defaulting name-resolution
failures to direct L2CAP would fix this DS4 gap but could misroute an Xbox controller hitting the
exact same rare edge case. This is a real trade-off, not a mechanical bug fix, so it's documented
here for a deliberate decision rather than silently coded.

Also scoped (design-only, no firmware changes) a wake-from-sleep
feature request: cloning an already-console-bonded genuine controller's BLE identity to wake the
Switch 2, informed by `Dycool/NS-PC-Control`'s capture-and-replay precedent — see
`docs/bluetooth/wake-from-sleep-design.md` (new).

Earlier the same week: a real Switch 2 console fully recognizes the Pico as a
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
[`docs/switch2/ble-controller-protocol-inventory.md`](../switch2/ble-controller-protocol-inventory.md).
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
[`docs/switch2/ble-controller-protocol-inventory.md`](../switch2/ble-controller-protocol-inventory.md)
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
NUL that would truncate a `wcscmp` early) — now stored as a proper `GUID` and compared with
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
[`docs/switch2/ble-controller-protocol-inventory.md`](../switch2/ble-controller-protocol-inventory.md)
for the full BLE field-level inventory (still accurate, just not active work) and
[`docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md`](../experiments/sw2-v2-motion-block-discovery-2026-07-10.md)
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
See [`docs/experiments/gyro-hardware-validation-2026-07-10.md`](../experiments/gyro-hardware-validation-2026-07-10.md) §9-12
and [`docs/switch2/report-0x09-motion.md`](../switch2/report-0x09-motion.md).

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

**Immediate, added 2026-07-14, updated same day after a regression:**

- **DS4/DS5/DualSense Edge pairing — hardware re-test needed NOW, top priority.** The investigation
  into the original "DS4 won't pair" complaint led to adding VID-reject guards across the Sony and
  Wiimote-family drivers (Gate 2 consistency pass) — which then caused a confirmed regression
  (DualSense, DualSense Edge, and DualShock 4 all stopped pairing entirely, reported directly during
  hardware testing). A first fix (resetting stale `classic_state.pending_vid`/`pending_pid` scratch
  fields before the direct-L2CAP path's SDP query) did not resolve it on retest. Current state: **all
  the VID-reject guards have been removed** from `ds3_bt.c`, `ds4_bt.c` (including its original
  2026-07-12 one), `ds5_bt.c`, `wii_u_pro_bt.c`, and `wiimote_bt.c` — a deliberate step back to the
  simpler, previously-working name-based matching for this whole family, since the guard mechanism
  itself is now the leading suspect rather than just needing a smaller fix. Both boards build clean.
  **Needs an immediate hardware re-test** of DualSense/DualSense Edge/DualShock 4 pairing before
  anything else in this file. Full account, including the two fix attempts and why the first wasn't
  enough: see the dated entry at the top of this file.
- If pairing is *still* broken after this, the guards were not the (sole) cause and the next step is
  a fresh hardware capture of the debug printfs already present in `bthid_update_device_info()`/
  `find_driver()` during a live failed attempt — don't re-guess blind again.
- **Wake-from-sleep — design doc only, no firmware yet.** `docs/bluetooth/wake-from-sleep-design.md`
  scopes a capture-and-replay approach (clone an already-bonded genuine controller's BLE identity),
  revising the prior flat "out of scope" verdict now that a genuine bonded controller is available to
  capture from. Next concrete step needs hardware: add the passive advertisement-capture diagnostic
  described there, then bring a genuine controller into range and see what it logs.

**Longer-standing backlog:**

1. **Hardware-validate Gate 2's 2026-07-12 identity/driver-binding work — still highest priority for
   the identity/logging system itself (fix (c) in item 7 above takes priority for actual pairing
   reliability).** Reachability audit, fresh upstream comparison, the `bthid_transport_mask_t`
   structural fix, and the new `bt_identity_log.c`/`btid dump` facility are build-verified; hardware
   coverage so far: 8BitDo NGC Modkit (Classic, full profile confirmed), **Switch 2 Pro Controller
   (BLE, identity/driver-binding confirmed clean, 2026-07-13 — see item 7(c) and the experiment doc
   "Capture 2")**, and unconfirmed-but-reported-pairing-successful passes for Xbox/DualSense/a
   generic Switch Pro Controller (no `btid dump` pulled for those yet). Still fully untested: Switch
   1 Pro Controller, Wiimote +/- attachment, generic/XInput with a captured `btid dump`, 8BitDo NGC
   DIY's second "Android/D-Input" mode. **Note: the `ds4_bt.c` VID-reject fix mentioned in earlier
   versions of this entry was removed 2026-07-14** — it caused a pairing regression across the whole
   Sony family; see the dated entry at the top of this file. Specifically still watch: does Classic
   BT Xbox input still parse correctly now that `xbox_bt.c` is live (flagged as this pass's top risk
   — its report-format detection was never hardware-verified). Full detail:
   `docs/bluetooth/driver-reachability-audit.md`,
   `docs/bluetooth/joypad-os-upstream-comparison-2026-07-12.md`,
   `docs/bluetooth/btstack-implementation.md` "Gate 2" section.
2. **Continue Gate 2 — REVERSED 2026-07-14, do not re-add without hardware evidence.** The
   "known-contradicting-VID should reject before name fallback" pattern was extended from `ds4_bt.c`
   to `ds3_bt`/`ds5_bt`/`wii_u_pro_bt`/`wiimote_bt`/`switch_pro_bt`, then found to cause (or at least
   correlate strongly with) a real pairing regression across the whole Sony family — all five
   additions were reverted the same day, along with `ds4_bt.c`'s original guard. `xbox_bt.c`/
   `xbox_ble.c`/`stadia_bt.c` guards were LEFT IN (different connection path, not implicated). Any
   future attempt to re-add VID-reject guards to the Sony/Wiimote-family drivers needs a real
   hardware pairing test in the loop, not just a code-consistency argument — see the dated entry at
   the top of this file for the full account of why this seemed safe and wasn't. Preserve narrower
   rumble follow-ups: left/right fidelity, stop/reconnect, generic non-Microsoft XInput, and the flagged
   Wiimote channel question.
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
