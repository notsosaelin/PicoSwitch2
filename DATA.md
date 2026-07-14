# Handoff: NSO GameCube Controller — rumble byte-model correction found via kernel source; needs re-test

Worktree preserved, nothing committed. Full chronological history of every pass lives in
`STATUS.md` (dated entries) and in `docs/experiments/`; this file is current-state-only.

## 1. Current working state

GameCube USB personality is fully recognized by a real Switch 2 console and streams input
correctly (button mapping confirmed working). Rumble has been an iterative, evidence-driven fix
process across four firmware revisions (2026-07-13 through -14). The original P0 bug (immediate
full-strength rumble on GameCube-mode entry) is **confirmed fixed** by hardware re-test. A
follow-up gameplay bug (small rumble ticks smearing into one continuous buzz) was root-caused and
fixed in code, **pending re-test**. Then, during a documentation/hypothesis-audit pass (no
hardware available), reading the real Linux kernel "HID: nintendo" driver source revealed the
entire byte-level model this project had been using for GC rumble was wrong — a foundational
correction, now implemented, that retroactively explains the whole bug arc more completely than
any single earlier fix. **Nothing from this latest correction has been hardware-tested yet.**

## 2. Root cause — corrected understanding (Strong, not yet Confirmed)

**The genuine GameCube controller has no continuous-amplitude rumble hardware at all.** Per the
real Linux kernel driver source (Vicki Pfau, linux-input mailing list v11 patch series,
`https://marc.info/?l=linux-input&w=2&r=1&s=hid+switch2&q=b`, read 2026-07-14 at the project
owner's request): it's a simple ERM motor with exactly three states (`GC_RUMBLE_OFF=0`/`ON=1`/
`STOP=2`, sent at report `0x03`'s `data[1]`). Real hosts simulate a continuous perceived amplitude
via delta-sigma/error-accumulation duty-cycle modulation of that ON/OFF state — not via any single
byte's magnitude. `data[0]` — what every rumble revision since 2026-07-13 read as "intensity" — is
actually an unrelated, incrementing sequence/command byte.

**This single correction retroactively explains the entire bug arc**: a rolling sequence byte is
essentially never exactly zero, so every real rumble packet looked like "some nonzero amplitude"
to the old decode regardless of the game's actual OFF/ON/STOP intent. Both major symptoms are
consistent with just this one root cause — immediate rumble on entry (an early packet with
state=OFF/STOP but nonzero sequence byte), and small gameplay rumbles becoming one continuous buzz
(every packet during an active session has nonzero sequence). Full account, including what
specifically was refuted and why: `docs/experiments/refuted-hypotheses.md`.

**The two downstream fixes from earlier passes remain independently correct and necessary** — they
govern how a *trigger* is delivered and held once identified (reliable stop delivery, short enough
envelope for rapid toggling to read as texture), which matters regardless of which upstream byte
is read as the trigger. Only the trigger-identification itself was wrong.

## 3. PC host-lab tool: `tools/gcusb`

Native Windows executable (MinGW-w64/gcc directly against SetupAPI/WinUSB/HID — no libusb, no
Zadig, no driver changes). Confirmed working end-to-end against real hardware in an earlier pass
(WinUSB enumeration bug fixed; `list`/`describe`/`init`/`rumble`/`stop-rumble` all verified).
**Updated this pass** to match the corrected rumble model: `gcusb_build_rumble_data()` now emits a
real OFF/ON state at the correct byte offset instead of a fake amplitude value;
`rumble-sweep` repurposed from a meaningless "amplitude sweep" (GC has no amplitude to sweep) into
a bounded toggle-cadence test, since toggling speed is the actually relevant variable under this
model.

**Build**: `cd tools/gcusb && ./build.ps1` (MSYS2 ucrt64 gcc). **Usage**: `gcusb.exe` with no args
prints full usage. 47/47 pure-logic tests pass (`gcc -I tools/gcusb -o test_gcusb_core
tools/test_gcusb_core.c tools/gcusb/gcusb_core.c`).

## 4. Genuine-vs-Pico differential result

Not yet run — the genuine controller was connected in a separate session, not simultaneously with
the Pico. Still pending: `gcusb compare --script tools/gcusb/scripts/init_minimal.gcusb` with both
devices connected at once.

## 5. Firmware changes

**Nineteenth pass** (confirmed fixed the original entry-rumble bug): Xbox stop commands
(`bthid_gamepad.c`) clear the enable/motor-mask byte and sustain/loop-count fields entirely on stop,
not just magnitude; a monotonic rumble generation counter (`report_get_rumble_gen()`,
`include/report.h`/`src/report.c`/`src/bt_hid/ns2_seam.c`) replaces plain value comparison for
change-detection, closing a "stop transition lost to polling" gap.

**Twentieth pass** (fixed in code, pending re-test): shortened `bthid_gamepad.c`'s
`pulse_sustain_10ms` from `0xFF` (~2550ms) to `0x05` (~50ms) for a genuine trigger, so rapid
legitimate rumble ticks stop smearing into one continuous hold.

**Twenty-first pass** (this pass — foundational correction, Strong evidence, not yet
hardware-tested): `switch_gc_hid_out_report()` (`src/switch_gc/switch_gc.c`) now reads `data[1]` as
a proper 3-value state enum (`gc_rumble_state_t`: OFF/ON/STOP) instead of `data[0]` as a linear
amplitude — see §2. Drives the motor at a fixed level (`GC_RUMBLE_ON_AMPLITUDE = 0xB0`, tunable,
not measured against real hardware feel) when ON, off otherwise.

## 6. Tests/builds

- `tools/test_gcusb_core.c` — 47/47 pass (one new check added for the corrected wire format).
- `test_hid_out_normalize.c`, `test_switch_gc_report.c` — re-run, still pass (unaffected).
- Both boards (`pico_w`, `pico2_w`) build clean with this pass's change; only `switch_gc.c`
  recompiled. `tools/gcusb/gcusb.exe` rebuilt clean.
- New UF2s: `build/pico_w/PicoSwitchWGA-pico_w.uf2`, `build/pico2_w/PicoSwitchWGA-pico2_w.uf2`.

## 7. Hardware validation

- Original entry-rumble bug (nineteenth pass): **Confirmed fixed** (real console re-test).
- Envelope-duration fix (twentieth pass): fixed in code, **not yet re-tested**.
- Byte-model correction (twenty-first pass, this pass): fixed in code, **not yet tested at all** —
  no hardware was available this pass; this is the most important untested change.
- `gcusb` tool: validated end-to-end against real hardware in an earlier pass; the corrected
  rumble-building code (this pass) has not itself been re-run against hardware.

## 8. Remaining unknowns

- Whether `GC_RUMBLE_ON_AMPLITUDE = 0xB0` (the fixed "on" level driven downstream) feels right —
  chosen as a reasonable firm-but-not-max default, not measured. The genuine motor has no amplitude
  control of its own, so this is inherently a translation choice for the downstream (Xbox, etc.)
  motor, not something with a single "correct" value to discover.
- Whether the shortened `0x05` (~50ms) pulse-sustain value (twentieth pass) is right, now compounded
  with the corrected state-based triggering — may need retuning together, not independently.
- `data[2]`/`data[3]` (always `0x00` in every real sample seen) remain Unknown.
- Whether the genuine controller's own rumble behavior differs from the Pico's (§4, not yet run).

## 9. Single highest-value next action

**Flash the new UF2 and re-test both the original scenario and real gameplay.** Specifically:
1. Confirm the entry-rumble fix still holds (regression check — switch Pro2→GameCube, motor
   should stay off).
2. Play Smash Bros (or similar) and check whether rumble now reads as a responsive texture of
   distinct small/large pulses matching the game's actual output, rather than one continuous buzz.
3. If it now feels *too weak* or *choppy*: `GC_RUMBLE_ON_AMPLITUDE` (`switch_gc.c`) is probably too
   low, or `pulse_sustain_10ms` (`bthid_gamepad.c`) too short — report specifically which.
4. If it *still* smears into one continuous buzz: the byte-model correction itself may need
   re-examination (possible that `data[1]`'s offset or enum values don't match this specific
   revision, or that a real console's actual toggling rate exceeds what our polling/forwarding
   chain can track) — report the exact symptom shape (same as before, or different) so the next
   fix targets the right layer.
