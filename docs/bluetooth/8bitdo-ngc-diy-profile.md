# 8BitDo NGC DIY Controller (NGC Modkit) — Confirmed Profile

> Confidence key: **Confirmed** (hardware-observed, cited capture) / **Strong Evidence** /
> **Hypothesis** / **Unknown**.

## Status

**Implemented and owner-confirmed working on real hardware, 2026-07-12, Switch 2 Pro Controller
mode.** Identity captured via `btid dump`/`btid desc`, button/trigger mapping derived from live
interactive captures (button-by-button, trigger partial/pre-click/full-click), and a dedicated
PID-specific profile (`NGC_MODKIT_BUTTON_MAP` in `bthid_gamepad.c`) implemented, reachability
verified on hardware (`is_ngc_modkit:true` in `btid desc`), and the final mapping confirmed working
end-to-end by the controller's owner. Full raw evidence and the two design iterations that were
tried and rejected before landing on this mapping: `docs/experiments/gate2-identity-log-hardware-captures-2026-07-12.md`.

**Not covered**: a second "Android/D-Input" BLE pairing mode this controller is reported to also
support — unconfirmed whether it uses the same PID or report shape; needs its own capture and
likely its own profile before this mapping can be assumed to apply there. The eventual NSO
GameCube USB output personality (Gate 3, not started) — see "Future NSO mode" below for what's
expected to change once that exists.

## Identity — Confirmed from `btid dump`/`btid desc`

| Field | Value |
|---|---|
| Advertised name | `8BitDo NGC Modkit` |
| Transport | Classic BT (not BLE) |
| VID:PID | `0x2DC8:0x286A` |
| VID/PID provenance | Classic SDP (resolves ~150-200ms after initial connect; `0x0000:0x0000` at the very first bind is expected/healthy per Gate 2's documented timing — see `docs/bluetooth/btstack-implementation.md` "Gate 2") |
| Class of Device | `0x082500` → major class `0x05` (Peripheral), minor class `0x02` (Gamepad) |
| HID report descriptor | 86 bytes, fingerprint `0x1F98`; Report ID 3; 16 flat Button-page usages (no semantic tagging in the descriptor itself) |
| Driver | `Generic BT Gamepad` with `is_ngc_modkit:true` (PID-specific table, not the shared 8BitDo paddle-controller table) — confirmed selected on real hardware |

The pairing UI the owner initially read VID from displayed `0x2DC9` — one off from this project's
own captured `0x2DC8`. `0x2DC8` matches 8BitDo's well-known registered USB vendor ID (already
referenced elsewhere in this codebase's `is_8bitdo` checks); trust `0x2DC8`.

**Not yet captured**: firmware version, whether VID/PID resolution is consistent across reconnects
(upstream's own 8BitDo M30 experience — see `docs/bluetooth/joypad-os-upstream-comparison-2026-07-12.md`
— found some 8BitDo units report different PIDs across firmware/mode variants; this unit resolved
cleanly, which is good news but not yet proof of consistency across sessions), and the second
pairing mode noted above.

## Rumble output — Implemented and hardware-confirmed

BlueRetro commit `e1a9831a875f5313a923160a1379a7ebbfaa2b11` contains an explicit
`BT_QUIRK_8BITDO_GC` output path for this advertised model. It sends Classic HID output report
`0xA5` with a three-byte payload:

```text
DB <low-frequency power> <high-frequency power>
```

PicoSwitch2 now reproduces that exact framing through the Modkit's existing PID-specific generic
gamepad quirk. The shared output task authorizes the callback only after VID `0x2DC8` resolves;
PID `0x286A` is already required to select the quirk. This does not widen output access to other
8BitDo or generic controllers, and the separate Microsoft-VID gate for Xbox-family quirks remains
intact. The host contract test pins ON intensity bytes, report ID, payload length, wrong/unresolved
VID rejection, and transport-send result propagation. Pico W, Pico 2 W, and legacy Switch 1 builds
pass. Physical ON/OFF behavior and real-console rumble were subsequently hardware-confirmed
without mapping regressions. Keep intensity and reconnect in the normal release regression matrix.

## Final confirmed mapping — Switch 2 Pro Controller 2 mode

| Physical control | HID usage | Output | Notes |
|---|---|---|---|
| A | 1 | Switch **A** | Direct, not the Xbox-style rotated mapping other pads use |
| B | 2 | Switch **B** | |
| X | 4 | Switch **X** | |
| Y | 5 | Switch **Y** | |
| D-pad | (hat, byte 1) | Standard | Unchanged, already worked |
| Both sticks | bytes 2-5 | Standard | Unchanged, already worked |
| L trigger, any real press | byte 7 analog | **ZL** | Via the existing generic seam-level analog fold (`ns2_seam.c`), not a button-table entry — see "Architecture" below |
| R trigger, any real press | byte 6 analog | **ZR** | Same mechanism, mirrored |
| Z | 11 | **R** (plain shoulder, not ZR) | Deliberately a different output than the trigger fold uses, so Z can never collide with a simultaneous real trigger press |
| — | — | **L** (plain shoulder) | Never fires — nothing maps to it; there's no left-side equivalent of Z on a real GameCube controller |
| — | — | **C** (Gamechat) | Never fires — nothing maps to it |
| Start | 12 | Switch **+** (Start) | Unchanged, already worked |
| Physical L3 (stick click) | 14 | **Capture** | Repurposed — a real GameCube controller has no clickable sticks at all |
| Physical R3 (stick click) | 15 | **Home** | Repurposed |
| Usages 3, 6, 13, 16 | — | *(unused)* | Confirmed not wired to any physical control on this unit |
| Usages 7, 8, 9, 10 | — | *(suppressed)* | See "Architecture" — these are the raw HID button bits for the trigger area, all deliberately mapped to nothing; the analog fold handles ZL/ZR instead |

### Architecture: why triggers route through the analog fold, not the button table

The raw HID descriptor declares 16 flat, undifferentiated Button-page usages with no semantic
tagging — usages 7/8 turned out to fire during *partial* trigger travel (a coarse "trigger mostly
pressed" echo of the analog value, not real distinct switches), and usages 9/10 fire only at the
true mechanical click. Two design iterations were tried before landing on the current one:

1. Mapping the true click (9/10) to `JP_BUTTON_L1`/`R1` and Z to `JP_BUTTON_R2` — broke because
   `ns2_seam.c`'s `router_submit_input()` has an existing, separate fallback that unconditionally
   derives `JP_BUTTON_R2` from `ANALOG_R2` crossing a threshold (a sensible default for
   controllers like Xbox/DualSense whose driver never reports a discrete click bit at all) — this
   collided with Z, producing "R and ZR both fire" on any real trigger press.
2. Suppressing that analog fold for this device (a new `suppress_l2r2_analog_fold` flag) fixed the
   collision but broke UX: the true-click-only mapping meant a *light* trigger press no longer
   registered as anything, when the owner specifically wanted "any real press" to register as
   ZL/ZR (matching how Pro Controller 2 mode necessarily approximates "trigger touched" with no
   real analog output available).

**Final design**: let the existing analog fold do what it already does well (ZL/ZR from real
analog trigger values, "any real press" not just full click), and move Z to `JP_BUTTON_R1`
instead of `R2` — a bit the analog fold never touches, so the collision can't recur. Usages 7/8/9/10
are all suppressed in the button table; the fold is the only thing driving ZL/ZR now.
`suppress_l2r2_analog_fold` itself (`core/input_event.h`) was left in place as available
infrastructure, unused by this device, for a genuinely different future collision.

Face buttons needed correcting too: the initial implementation assumed the standard Xbox-style
rotated convention (physical "A" position → Switch B slot) that every other Xbox/PlayStation-style
driver in this codebase uses by default — wrong for a GameCube-shaped controller, where physical
A/B/X/Y should land directly on Switch A/B/X/Y. Fixed by choosing the `JP_BUTTON_B1..B4` source
whose *default* remap destination matches the desired letter, rather than the source whose name
matches (see `NGC_MODKIT_BUTTON_MAP`'s code comment for the exact source→destination table used).

## NSO GameCube mode — Implemented 2026-07-13 (superseded speculation below kept for history)

**This section's original speculation ("Z → ZR") was superseded once Stage C actually shipped a
native Z destination** — GameCube's report `0x0A` has its own real Z bit, distinct from ZR/ZL
entirely, so there was no need to approximate Z as anything else. The authoritative, current
mapping now lives in `docs/switch2-gc/mapping.md` "8BitDo NGC Modkit specifically" (implemented in
`bthid_gamepad.c`/`ns2_seam.c`/`switch_gc_encode.c`, host-tested by
`tools/test_switch_gc_report.c`) — do not duplicate it here. Summary of what actually shipped,
for quick reference:

- L/R trigger → real continuous analog output (`left_trigger`/`right_trigger`), not the ZL/ZR
  digital approximation Pro Controller 2 mode still uses.
- Trigger true-click (usages 9/10) → real independent digital L/R detent bits, previously
  suppressed entirely in Pro2 mode.
- Z (usage 11) → **native GameCube Z**, a real, distinct bit — not ZR, and not a repurposing of the
  existing Pro2-mode "Z → plain R" mapping (that mapping is left unchanged and still fires
  alongside the new native Z bit, since GameCube mode reads them through entirely separate fields).
- Everything else (A/B/X/Y direct, Start→Plus, L3→Capture, R3→Home, D-pad/sticks standard) carried
  over unchanged, exactly as this section originally expected.

**Hardware-confirmed**: real hardware validation (the Modkit connected while the Pico streams
GameCube-mode reports to an actual host) — see `docs/switch2-gc/protocol.md`'s Stage D section for
the streaming-gate status this depends on.

## Raw hardware observations (preserved verbatim, now confirmed live)

### Zero-based 10-byte report layout — Confirmed via live `raw` polling 2026-07-12

```text
index:    00 01 02 03 04 05 06 07 08 09
neutral:  03 0F 7F 80 80 80 00 00 00 00
A held:   03 0F 7F 80 80 80 00 00 01 00
B held:   03 0F 7F 80 80 80 00 00 02 00
X held:   03 0F 7F 80 80 80 00 00 08 00
Y held:   03 0F 7F 80 80 80 00 00 10 00
Z held:   03 0F 7F 80 80 80 00 00 00 04
Start held: 03 0F 7F 80 80 80 00 00 00 08
L3 held (physical, usage 14): 03 0F 66 64 80 80 00 00 00 20
R3 held (physical, usage 15): 03 0F 7F 80 7F 77 00 00 00 40
R2 full/click: 03 0F 7F 80 80 80 FF 00 80 02
L2 full/click: 03 0F 7F 80 80 80 00 FF 40 01
Both L2+R2 full/click: 03 0F 7F 80 80 80 FF FF C0 03
```

**Correction**: an earlier pass of this table (sourced from a prior session's `DATA.md`) had L3/R3
transposed relative to what live `raw` polling actually shows. The rows above are the corrected,
live-confirmed values (physical L3 → byte9 `0x20`/usage 14; physical R3 → byte9 `0x40`/usage 15).
Full trigger partial-travel/pre-click data: see the experiment capture doc.

### Confirmed via live testing

- D-pad and both analog sticks: correct via the generic driver, unchanged.
- Byte 6 = R2 analog (`0x00` rest → `0xFF` full), byte 7 = L2 analog — Confirmed, matches the
  decoded HID descriptor's Simulation-Controls Accelerator(byte6)/Brake(byte7) declaration exactly.
- Usages 7/8 (byte8 `0x40`/`0x80`) are **not** distinct physical switches — they fire during
  partial trigger travel, independent of the real click. Confirmed via three-point sampling
  (rest / ~50-75% / full) for both triggers.
- Usages 9/10 (byte9 `0x01`/`0x02`) are the true mechanical click, confirmed to fire only at full
  press. Both triggers together compose cleanly with no bit aliasing.
- Every remaining physical control accounted for: no Home/Menu button, no back paddles, no other
  switches beyond what's in the table above — confirmed by interactively pressing everything on
  the unit and observing no other bits change.
