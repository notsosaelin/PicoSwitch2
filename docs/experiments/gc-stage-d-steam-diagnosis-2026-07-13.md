# NSO GameCube Stage D — Steam Diagnosis and Report-0x05 Fix — 2026-07-13

> Confidence key: **Confirmed** (hardware-observed) / **Strong** / **Hypothesis** / **Unknown**.

## Question

After implementing the minimum Stage D streaming gate (0x03/0x0D + 0x03/0x0A, arming report
`0x0A`), the project owner's hardware test found: Windows/Steam detect the Pico correctly as a
Nintendo GameCube Controller, but no button/stick/trigger input reaches Windows/Steam at all.
Was streaming ever armed?

## Method

Launched Steam directly against the Pico's already-connected GameCube-personality USB interface
and captured the resulting traffic with USBPcap (`--inject-descriptors`, same method used
throughout this project). Confirmed via `DEVPKEY_Device_HardwareIds` (`REV_0111` = `bcdDevice`
`0x0111`) that only the Pico, not the genuine controller, was connected during this capture — no
risk of confusing the two.

## Result — Confirmed

Steam sends the complete real bulk command sequence and the Pico's firmware responds correctly to
every command it recognizes:

```
03 91 00 01 00 08 00 00 00 00 00 00 00 00 30 01 00   (SPI/memory read, x8, addresses matching
                                                        the previously-documented calibration table)
07 91 00 01 00 00 00 00
0c 91 00 02 00 04 00 00 27 00 00 00                   (feature flags = 0x27, matches doc)
11 91 00 01 00 00 00 00
0a 91 00 08 00 14 00 00 01 ff ff ff ff ff ff ff ff
   35 00 46 00 00 00 00 00 00 00 00                   (vibration test sample, matches doc)
0c 91 00 04 00 04 00 00 27 00 00 00                   (confirm feature flags)
01 91 00 0c 00 00 00 00
01 91 00 01 00 00 00 00
08 91 00 02 00 04 00 00 01 00 00 00
03 91 00 0a 00 04 00 00 05 00 00 00   -> 03 01 00 0a 00 f8 00 00   *** Select Input Report = 0x05 ***
03 91 00 0d 00 08 00 00 01 00 ff ff ff ff ff ff -> 03 01 00 0d 00 f8 00 00 01 00 00 00
09 91 00 07 00 08 00 00 01 00 00 00 00 00 00 00
```

**Steam requests report ID `0x05`, never `0x0A`.** The Pico's `switch_gc_vendor_dispatch()` only
armed streaming for `c[8] == 0x0A` (matching ndeadly's documented "invalid report IDs are ignored"
semantics) — so it correctly did nothing for `0x05`, and streaming never armed. This is not a
parser bug, not a missing prerequisite, and not evidence of a broken mapping: it is Steam
exercising the *other* valid report ID this device's own HID descriptor declares (Report ID 5, the
"63-byte flat vendor array... common to all Switch 2 controller types" — the same format
`switch_pro2.c`'s `ns2_build_report_05()` already streams for Pro Controller 2 on PC/Steam).

**Also resolved, same evidence**: whether EP0 `bRequest` 2/3/4 gate the bulk sequence. The Pico
currently stalls all three (only the WinUSB MS-OS request is implemented) — and Steam still
completed the entire bulk exchange successfully. **Confirmed: they are not a prerequisite for a
PC/Steam host to reach the bulk select-report sequence.**

**New, previously-undocumented finding**: a third EP0 vendor request, `bRequest=4`,
`wValue=0x0276`, OUT direction, `wLength=0`, occurs (in the *reference* ndeadly capture) between
`bRequest=2` and the first bulk command — the genuine controller ACKs its status stage normally.
Purpose still Unknown; not investigated further since it's proven not to matter for reaching
streaming from a PC host.

## Fix — Implemented

- `switch_gc_encode_report05()` (`src/switch_gc/switch_gc_encode.c`): new, independent encoder for
  report `0x05`'s common layout (mirrors `switch_pro2.c`'s `ns2_build_report_05()` bit-for-bit, not
  shared code — deliberately not coupled to Pro2's module/state) plus the GC-specific analog
  trigger tail at `0x3C`/`0x3D` (documented in `docs/switch2-gc/protocol.md` "Input Report 0x05").
  Native Z and the independent L/R detents have no bit position in this format and are omitted.
- `switch_gc_select_report()` (`src/switch_gc/switch_gc_report_select.c`, new pure/host-tested
  file): decision logic for which report ID a `0x03/0x0A` command arms — accepts `0x05` or `0x0A`,
  ignores anything else (leaves the current selection, if any, unchanged).
- `switch_gc_task()` now streams whichever of the two the host actually selected, defaulting to
  silent if neither has been selected — matching genuine hardware absent a supported command.

## Tests

- `tools/test_switch_gc_report.c`: 33 new report-0x05 checks (neutral, 32-bit little-endian
  counter, each button, sticks, analog tail at 0x3C/0x3D, no L3/R3/GL/GR/ZR/plain-shoulder
  synthesis) — all pass, alongside the existing 43 report-0x0A checks.
- `tools/test_switch_gc_report_select.c` (new): 8 checks (select 0x05, select 0x0A, unsupported ID
  ignored, switching between the two, an unsupported ID doesn't clear an existing selection, short
  command doesn't corrupt state) — all pass.
- Full four-config build matrix green after reconfiguring (new source files required a `cmake`
  re-run, not just `ninja`, for the two scratch configs — `GLOB_RECURSE` is evaluated at configure
  time).

## Remaining questions

- Whether a real Switch 2 console would ever request `0x0A` at all is still untested (Stage G) —
  report `0x0A`'s implementation is preserved unchanged for that case, just no longer assumed to be
  what a PC host uses.
- `bRequest=2`/`bRequest=4`'s actual purpose remains Unknown (confirmed non-blocking, not confirmed
  meaningless).
- Not yet re-tested on real hardware after this fix — see `STATUS.md` for the current next step.
