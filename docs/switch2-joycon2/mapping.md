# Joy-Con 2 Left/Right (Experimental) Output Personalities — Mapping Policy

> Status: 🟡 SL/SR wiring implemented 2026-07-14 (`include/switch_pro.h`'s `SWITCH_EXTRA_SL`/`SR`,
> `src/switch_joycon2/switch_joycon2_encode.c`, `src/bt_hid/ns2_seam.c`'s `ns2_apply_dst()`),
> host-tested (`tools/test_switch_joycon2_report.c`, 43/43 checks), build-verified on both boards.
> **Not yet hardware-tested.** Every other native button (A/B/X/Y, D-pad, L/R, ZL/ZR, stick +
> click, Plus/Minus, Home/Capture, C) was already wired and passing tests before this pass — see
> `docs/switch2-joycon2/protocol.md` "Wire input/output report contents" for the underlying wire
> format this section maps onto.

## Why "mapping" is a smaller problem here than it looks

Joy-Con 2 L and R reuse the exact same cross-core model (`switch_pro_input_t`,
`include/switch_pro.h`) and the exact same generic-controller remap pipeline
(`config_get_ns2_map()`/`NS2_DST_*`, `src/bt_hid/ns2_seam.c`'s `router_submit_input()`) that
Pro Controller 2 already uses. `switch_joycon2_encode_report()`/`_report05()`
(`src/switch_joycon2/switch_joycon2_encode.c`) just read a subset of the same
`SWITCH_MASK_*`/`SWITCH_EXTRA_*` bits Pro2's own encoder reads, side-gated by which of the two
personalities is active. Concretely, for a generic controller (Xbox, DualSense, etc.) used to
drive a Joy-Con2 personality, the default per-family remap already produces the physically
correct result with **zero new code**, because a lone Joy-Con's own physical controls line up
directly with a generic pad's:

| Generic source | Default destination (unchanged from Pro2) | Joy-Con2 (L) physical meaning | Joy-Con2 (R) physical meaning |
|---|---|---|---|
| Face buttons (B1-B4) | A/B/X/Y | (unused — L has no A/B/X/Y) | A/B/X/Y |
| D-pad | Up/Down/Left/Right | Up/Down/Left/Right | (unused — R has no D-pad) |
| Shoulder L1/R1 | plain L / R | L | R |
| Trigger L2/R2 (analog, folded past a threshold — unchanged Pro2 fold, **not** suppressed for Joy-Con2, see below) | ZL / ZR | ZL | ZR |
| Left/right stick click | L3 / R3 | Stick click (the one physical stick a lone Joy-Con has) | Stick click |
| Select/Start | Minus / Plus | Minus | Plus |
| Aux buttons (A1-A4 default) | Home / Capture / C / Capture | Capture | Home / C |

The one genuine gap was **SL/SR** — real physical rail buttons present on *both* Joy-Con units
(used as shoulder-equivalents when a lone Joy-Con is held sideways), which have no equivalent
concept on Pro Controller 2 or the GameCube controller and so had no field in
`switch_pro_input_t` at all. Fixed this pass — see below.

## Native capability set — Confirmed (`docs/switch2-joycon2/protocol.md`)

| Capability | Left | Right | Confidence |
|---|---|---|---|
| A, B, X, Y | — (physically absent) | Yes | Confirmed |
| D-pad (Up/Down/Left/Right) | Yes | — (physically absent) | Confirmed |
| Plus / Minus | Minus only | Plus only | Confirmed |
| L / ZL (plain shoulder + trigger) | Yes | — | Confirmed |
| R / ZR | — | Yes | Confirmed |
| SL / SR (rail buttons) | Yes (this unit's own) | Yes (this unit's own) | Confirmed |
| Stick + click | Yes (one stick, one click) | Yes | Confirmed |
| Home | — | Yes | Confirmed |
| Capture | Yes | — | Confirmed |
| C / GameChat | — (no physical control) | Yes | Confirmed |
| NFC | — (no hardware) | Yes (hardware present, not emulated) | Confirmed |
| Motion (gyro/accel) | Yes (hardware present, not emulated) | Yes | Confirmed |

Byte-level bit positions for all of the above: `docs/switch2-joycon2/protocol.md` "Report ID 7/8".

## SL/SR — the real gap, now fixed

**Problem**: SL/SR are genuine physical controls on a real Joy-Con 2, not a Pro2/GameCube concept,
so no field in `switch_pro_input_t` carried them — the encoder hardcoded both bits to 0
unconditionally, correctly documented in-code as "not sourced from anything yet," not a silent
omission.

**Fix**: added `SWITCH_EXTRA_SL`/`SWITCH_EXTRA_SR` bits to the existing `extra` field
(`include/switch_pro.h`) — generic, not per-side, since exactly one Joy-Con2 personality is ever
active at a time (same convention already used for `SWITCH_MASK_L3`/`R3`, "the stick that's
active"). `switch_joycon2_encode_report()` now reads them unconditionally into report 7/8's byte 1
(`0x80`/`0x40`, both sides — same bit position regardless of which side is active); the shared
report `0x05` encoder reads them side-gated into its own distinct SL/SR bit positions (byte 0
`0x20`/`0x10` for Right, byte 2 `0x20`/`0x10` for Left), matching how it already side-gates
ZL/L and ZR/R.

**Source**: `src/bt_hid/ns2_seam.c`'s `ns2_apply_dst()` reinterprets the existing
`NS2_DST_GL`/`NS2_DST_GR` destinations as SL/SR whenever a Joy-Con2 personality is active
(`joycon2_active`, computed from `g_usb_personality` the same way GameCube mode's own
`gc_active` already is). This is deliberate, not a workaround: GL/GR mean "grip button," and a
lone Joy-Con2 physically has no grips to read (it's not docked in a Charging Grip in this
project's scope), so the exact same generic-controller source buttons the per-family map already
assigns to GL/GR by default (`L4`, `R4`, `L5`, `R5` — paddle/extra slots, typically unused on a
standard Xbox/DualSense pad) instead drive a real control the personality *does* have. Pro2 and
GameCube mode are unaffected — the reinterpretation is gated on `joycon2_active` alone, and their
own GL/GR default mapping and behavior is provably unchanged (same pattern already established by
GameCube's own analog-fold suppression in the same function).

No new persisted config field, no new remap-table schema, no new source-button slot — this reuses
the existing per-family customizable map (`config_get_ns2_map()`) exactly as designed. A user who
wants SL/SR on a *different* physical button (e.g. an Xbox Elite paddle, if one is ever bound to
this project) can already reassign it via the existing config UI once a `NS2_DST_SL`/`NS2_DST_SR`-style
selector is exposed there — not yet done, since the config UI's destination list still reads
Pro2/GameCube-flavored names; flagged as a small follow-up if the paddle-default proves confusing
in practice, not a functional gap.

## "Sideways mode" — why no rotation/remapping is needed on this project's side

The project owner's own framing (2026-07-14): both individual Joy-Con2 output personalities will
be used in the Switch's "sideways" single-Joy-Con configuration once mapped. This does **not**
require any button-rotation or reinterpretation in this project's firmware. On genuine hardware, a
lone Joy-Con always reports its physical buttons in their fixed physical positions (Up/Down/
Left/Right for L; X/Y/A/B for R) regardless of how it's held — the Switch console's own system
software is what reinterprets "sideways" semantics (e.g. presenting the D-pad as a virtual
face-button cluster), not the controller. Since this project's whole design goal is to mimic
genuine hardware at the wire level (not to build a custom ergonomic remap), correctly populating
the physical bit positions documented above is the entire job; the console will apply the same
sideways reinterpretation it already applies to a real Joy-Con. No experiment or implementation
work is needed here beyond what's already done — flagged explicitly so a future contributor
doesn't invent an unnecessary rotation layer.

## Non-goals for this document

- A `NS2_DST_SL`/`NS2_DST_SR` destination enum surfaced in the config UI's per-family remap
  editor — the GL/GR-slot reinterpretation above is a working default; exposing it as an explicit,
  independently reassignable destination is future work if the default proves insufficient.
- Docked-in-Charging-Grip behavior (GL/GR reaching the host from a docked Joy-Con2, whether
  through a reserved bit or otherwise) — out of scope per `docs/switch2-joycon2/protocol.md`'s own
  open question; this project does not emulate a Charging Grip.
- Mouse mode, NFC, and motion data — no source exists in this project for any of them yet (see
  `protocol.md`); left zeroed exactly as before this pass.
