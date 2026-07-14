# NSO GameCube Output Personality — Mapping Policy

> Status: 🟢 Implemented 2026-07-13 (Stage C + the 8BitDo NGC Modkit's GameCube-mode mapping),
> pending its own hardware validation. `switch_gc_build_report()`/`switch_gc_encode_report()`
> (`src/switch_gc/switch_gc.c`, `src/switch_gc/switch_gc_encode.c`) implement the full report
> `0x0A` construction: A/B/X/Y, D-pad, Plus/Minus, Home/Capture, C, ZL, both analog sticks, **and**
> native Z, independent L/R trigger detents, and continuous analog L/R trigger — the latter three
> via new dedicated `switch_pro_input_t` fields (`gc_extra`/`left_trigger`/`right_trigger`, see
> include/switch_pro.h's `GC_MASK_*`), populated only for the 8BitDo NGC Modkit
> (`gc_has_native_layout`-gated, `src/bt_hid/bt/bthid/devices/generic/bthid_gamepad.c`). L3/R3
> remain hardcoded to 0 per this document's own constraint. Host-tested:
> `tools/test_switch_gc_report.c` (10-point golden coverage). The generic analog→ZL/ZR fold is
> suppressed specifically for GameCube-mode + Modkit (see "Do not repeat the existing analog-fold
> problem" below) so a device with no real ZL/ZR doesn't get one synthesized. Every *other*
> per-device mapping table below (DualSense/Xbox/genuine Switch Pro Controller as GameCube-output
> sources) remains design intent, not code.

## Principle (verbatim intent from the project brief, not to be diluted)

The NSO GameCube **output** personality exposes the complete native capability set of a genuine
NSO GameCube Controller. Each connected **input** controller supplies only the capabilities it
physically has and that its mapping intentionally uses. These are separate concerns: a
poorly-equipped input controller must never reduce what the USB output personality advertises, and
a well-equipped input controller must never be used to synthesize output capabilities the genuine
GameCube controller doesn't have (L3/R3, chiefly).

## Native GameCube output capability set — Confirmed/Strong (per `docs/switch2-gc/protocol.md`)

| Capability | Confidence | Notes |
|---|---|---|
| A, B, X, Y | Strong | Standard buttons, report `0x0A` byte 0 |
| D-pad (Up/Down/Left/Right) | Strong | Report `0x0A` byte 1 |
| Plus, Minus | Strong | Report `0x0A` byte 0/1 (genuine GC controller has no physical Start button in the NSO redesign — Plus/Minus occupy that role per the documented bitfield) |
| Native Z | Strong | Report `0x0A` byte 0, `0x10` |
| ZL (digital) | **Confirmed physically present** | **Correction, 2026-07-13**: the project owner has directly confirmed on genuine hardware that ZL is a real physical control on the NSO GameCube Controller — the earlier "physically absent, bit presumed always-0" statement in this table was wrong and is superseded. Report `0x0A` byte 1, `0x10`. Treat as a real, assertable native output alongside Z/C/Home/Capture, not a bit that only exists in the descriptor for shape-compatibility reasons. |
| L, R (digital detent) | Strong | Distinct from the continuous analog values below — real physical microswitches per SoulCalDan's GC-hardware evidence (secondary source, GC-general not NSO-specific) |
| Analog L, analog R | Strong | Report `0x0A` offsets `0xC`/`0xD`, continuous, "uncalibrated" per ndeadly |
| C / GameChat | Strong (bit exists) / Unknown (exact semantics) | Report `0x0A` byte 2, `0x10` |
| Home | Strong | Report `0x0A` byte 2, `0x01` |
| Capture | Strong | Report `0x0A` byte 2, `0x02` |
| Both analog sticks | Strong | Report `0x0A` bytes `0x5`-`0xA`, packed 12-bit |
| Rumble | Strong (exists) / Unknown (byte encoding) | Output report `0x03`, single ERM motor |
| **L3 / R3** | **Confirmed absent** | Per explicit physical correction: the genuine NSO GameCube Controller has no clickable sticks. **The output personality must never expose L3/R3 destinations.** |

## Internal normalized model requirements

The cross-core input struct (`switch_pro_input_t`, `include/switch_pro.h`) and the seam's
destination enum (`NS2_DST_*`, `src/bt_hid/ns2_seam.c`) already carry several of the fields a
GameCube personality needs — `NS2_DST_C`, `NS2_DST_HOME`, `NS2_DST_CAPTURE`, `NS2_DST_GL`/`NS2_DST_GR`
already exist and are wired to Pro2's `SWITCH_EXTRA_*`/`SWITCH_MASK_*` bits today. This is a
meaningfully smaller gap than a from-scratch model would face. **Implemented 2026-07-13** (added,
preserved independently per explicit instruction — not collapsed into existing fields that mean
something different):

- **Native GameCube Z** — `switch_pro_input_t.gc_extra`'s `GC_MASK_Z` bit (`include/switch_pro.h`),
  NOT `NS2_DST_GL`/`GR` (Pro2's own paddle-adjacent concept) or `SWITCH_MASK_R`/`SWITCH_MASK_ZR`
  (a different physical control entirely). Populated by `bthid_gamepad.c`'s
  `process_report_dynamic()` from the Modkit's usage 11, independent of (and in addition to) that
  same usage's existing `JP_BUTTON_R1`→Pro2-R mapping.
- **Digital L/R detent** — `GC_MASK_L_DETENT`/`GC_MASK_R_DETENT`, populated directly from the
  Modkit's usage 9/10 raw bits (the confirmed TRUE mechanical click, not usage 7/8's partial-travel
  echo — see `docs/bluetooth/8bitdo-ngc-diy-profile.md` "Raw hardware observations"). Independent
  of the *analog* L/R value end-to-end: neither is derived from the other, verified by
  `tools/test_switch_gc_report.c`'s "digital detents independent of analog value" and "analog
  full-scale without detent" cases. (This directly generalizes the lesson already learned and
  documented from the 8BitDo NGC Modkit input work — see `docs/bluetooth/8bitdo-ngc-diy-profile.md`
  "Architecture" — where collapsing a digital detent and an analog-derived digital fold onto the
  same output bit caused a real, previously-shipped bug. The GC output personality does not repeat
  that collapse in the other direction.)
- **Continuous analog L/R trigger** — `left_trigger`/`right_trigger` (`uint8_t`, 0..255), populated
  in `ns2_seam.c`'s `router_submit_input()` from `input_event_t.analog[ANALOG_L2/R2]`
  unconditionally, before the Pro2-specific analog-fold check — a raw passthrough, never
  thresholded or collapsed to a boolean.
- **ZL remains a distinct destination from Pro2's own `NS2_DST_ZL`/`SWITCH_MASK_ZL`** — the GC
  output encoder reads the same `SWITCH_MASK_ZL` bit (destination enum value reused, per the
  original plan below) but encodes it into report `0x0A`'s own, independent ZL bit — verified by
  `tools/test_switch_gc_report.c`'s "ZL alone... does NOT set... native Z bit" case. For the 8BitDo
  NGC Modkit specifically, this bit is now suppressed in GameCube mode (see "Do not repeat the
  existing analog-fold problem" below) since that device has no real ZL to report.
- **No new L3/R3 output destination.** `switch_gc_encode_report()` hardcodes both stick-click bits
  to 0 unconditionally and never reads `SWITCH_MASK_L3`/`SWITCH_MASK_R3` — verified by
  `tools/test_switch_gc_report.c`'s "L3/R3 source inputs cannot set native output stick-click bits"
  case.

### Do not repeat the existing analog-fold problem

`ns2_seam.c`'s `router_submit_input()` unconditionally folds `analog[ANALOG_L2]`/`[ANALOG_R2]`
crossing a threshold into `JP_BUTTON_L2`/`R2` (ultimately `SWITCH_MASK_ZL`/`ZR`) as a fallback for
controllers whose driver never reports a distinct digital trigger-click bit. This is real,
validated, and **must stay unchanged for Pro2** — including for the Modkit in Pro2 mode, which
deliberately relies on it (see `docs/bluetooth/8bitdo-ngc-diy-profile.md` "Architecture").

But for **GameCube mode**, this same fold would silently synthesize a Pro2-style ZL/ZR that has no
correct meaning at all in this personality — GameCube mode's L/R are a continuous analog trigger
with an independent digital detent, never a plain "any real press" digital button. **Implemented
fix, revised 2026-07-13**: the fold is suppressed whenever `g_usb_personality ==
USB_PERSONALITY_NSO_GAMECUBE`, for **every** device, not just ones with `gc_has_native_layout` (the
original version of this fix only suppressed it for the Modkit specifically — broadened after
hardware testing with Xbox/DualSense in GC mode showed shoulder buttons and triggers were mapped to
dead/wrong bits; see "Generic controllers" below for what replaced it). This is
personality-gated, device-independent:
- Pro2 mode: the personality check alone excludes it, so Pro2's behavior is provably unchanged
  regardless of which device is connected.
- GameCube mode, any device: fold suppressed. The Modkit's own real continuous-analog and
  independent-detent fields (above) carry its actual signal. Every other device gets the
  generic-controller policy below instead of a fabricated ZL/ZR.

## Mapping policy by input controller

### Genuine NSO GameCube Controller as input (Bluetooth-side, hypothetical — not this project's
current scope, but stated for completeness)

Would trivially validate and pass through its own complete native layout, correctly excluding L3/R3
because those controls don't exist to read in the first place. Not an active development target this
pass (this document covers the **USB output** personality; a genuine GC controller as a **Bluetooth
input** device is a separate, unstarted question).

### Generic controllers (DualSense, Xbox, etc.) — Confirmed 2026-07-13 by direct hardware feedback

Once GC mode reached full console recognition, the owner tested Xbox and DualSense controllers (not
the Modkit) while in GameCube output mode: face buttons, D-pad, and sticks worked correctly (they
already share the same generic `SWITCH_MASK_*` code path as every other mode), but the shoulder
buttons and triggers were wrong — the Pro2-style pairing (shoulders → plain L/R, trigger analog
fold → ZL/ZR) is the **reverse** of what GameCube mode needs, since GC has no plain L/R shoulder
bit at all (dead output) and no ZR bit either (only Z, which the console displays as "ZR" — see
`switch2-gc/protocol.md`).

**Fixed** in `router_submit_input()` (`src/bt_hid/ns2_seam.c`), gated on `g_usb_personality ==
USB_PERSONALITY_NSO_GAMECUBE && !event->gc_has_native_layout` (i.e. any device *except* the Modkit,
which keeps its own real per-button signals untouched by this synthesis):

| Generic source | Pro2-mode destination (unchanged) | GameCube-mode destination (fixed 2026-07-13) |
|---|---|---|
| Shoulder LB/L1 | `SWITCH_MASK_L` (plain L) | `SWITCH_MASK_ZL` — shows as "ZL" |
| Shoulder RB/R1 | `SWITCH_MASK_R` (plain R) | `GC_MASK_Z` — shows as "ZR" |
| Analog trigger LT/L2 | Continuous fold → `SWITCH_MASK_ZL` past a low threshold (`>64`) | Continuous passthrough to `left_trigger` (already unconditional) **+** `GC_MASK_L_DETENT` past a high threshold (`>224`, approximating the real mechanical detent's near-full-travel position) |
| Analog trigger RT/R2 | Continuous fold → `SWITCH_MASK_ZR` past a low threshold | Continuous passthrough to `right_trigger` **+** `GC_MASK_R_DETENT` past the same high threshold |

The `224`/255 detent threshold is a **Hypothesis**, not hardware-derived — no genuine GameCube
trigger-to-detent curve data exists for a substitute controller's analog range, and this is
expected to need retuning once real detent-feel feedback comes in. L3/R3 remain unmapped for
generic controllers in GC mode (same rule as the Modkit — no real output bit exists to alias them
to without an explicit per-device decision, not made yet).

### 8BitDo NGC Modkit specifically

This controller already has a confirmed, owner-validated Bluetooth **input** profile for Pro
Controller 2 output (`docs/bluetooth/8bitdo-ngc-diy-profile.md`) — that work is separate from and
must not be degraded by this output-personality work (per explicit instruction: "keep the Modkit
input work and NSO GameCube output implementation separately testable even though they will
eventually work together"). Once the GameCube output personality exists, the Modkit's *existing*
input profile should gain a **second** target mapping (GameCube-output-mode), reusing the same
identity detection (`is_ngc_modkit`, PID `0x2DC8:0x286A` exact match — never the broad shared
8BitDo button-count heuristic) already proven correct:

| Physical control | Pro2-mode mapping (existing, shipped) | GameCube-mode mapping (implemented 2026-07-13) |
|---|---|---|
| A/B/X/Y | Direct A→A/B→B/X→X/Y→Y | Same — direct (unchanged code path, both modes read the same `SWITCH_MASK_*` bits) |
| L analog trigger | ZL (via seam analog fold, "any real press") | **Native analog L** (`left_trigger`) — raw `ANALOG_L2` passthrough, undegraded; the fold is suppressed for this device in GameCube mode (see "Do not repeat the existing analog-fold problem" above) so it no longer also produces a synthesized ZL |
| R analog trigger | ZR (via seam analog fold) | **Native analog R** (`right_trigger`), same reasoning |
| Trigger digital detent (usages 9/10) | Currently suppressed/unused (fold handles it) | **Native digital L/R detent** (`GC_MASK_L_DETENT`/`GC_MASK_R_DETENT`) — read directly from usage 9/10's true-click bits in `bthid_gamepad.c`, independent of the analog value |
| Z (usage 11) | `JP_BUTTON_R1` → plain "R" (unchanged) | **Native GameCube Z** (`GC_MASK_Z`) — populated *in addition to*, not instead of, the existing Pro2-mode `JP_BUTTON_R1` mapping, so Pro2 mode is provably unaffected |
| Physical L3 (usage 14) | Capture | Capture (unchanged — Modkit has no true L3, this was already a documented alias, and Capture is a real GameCube output, so the alias remains valid) |
| Physical R3 (usage 15) | Home | Home (unchanged, same reasoning) |
| — | L (plain shoulder), never fires | Still never fires — Modkit has no left-side equivalent of Z; nothing invents one |
| — | C (Gamechat), never fires | Still never fires — Modkit does not physically provide a C/GameChat control |
| Start | Switch + (Start) | Plus (GameCube output's equivalent of Start) — unchanged code path |

**Implemented** in `bthid_gamepad.c`'s `process_report_dynamic()` (native Z/detent extraction,
`gc_has_native_layout` flag) and `ns2_seam.c`'s `router_submit_input()` (fold suppression,
unconditional trigger/gc_extra forwarding) — not a separate remap table. Verified end-to-end (from
an explicit `switch_pro_input_t` through the encoder) by `tools/test_switch_gc_report.c`'s "Modkit
Z plus one trigger simultaneously" and detent-independence cases. **Not yet verified**: real
hardware — the 8BitDo Modkit connected while the Pico is in GameCube mode and actually streaming
(gated on Stage D, also implemented this pass — see `docs/switch2-gc/protocol.md` "USB init command
sequence"). `docs/bluetooth/8bitdo-ngc-diy-profile.md` "Future NSO GameCube mode" has been updated
to point here rather than duplicating its own now-stale speculation.

### Devices lacking controls

Leave unsupported outputs unmapped unless an explicit documented mapping policy assigns them (same
principle as the sufficiently-equipped case, just with fewer available source controls). Do not
synthesize missing buttons — an input controller without a dedicated Home button, for example, simply
never asserts the GameCube output's Home bit; this must not be worked around by inventing a chord or
combo unless a future mapping explicitly documents one.

## Explicit non-goals for this document

- Concrete C enum/struct definitions for the new destinations (Stage B/C implementation work, once
  the output personality's own report-construction code exists to consume them).
- Per-device mapping tables beyond the 8BitDo NGC Modkit (design intent only, above) — DualSense,
  Xbox, genuine Switch Pro Controller, and any other sufficiently-equipped controller's GameCube-mode
  mapping is future work, sequenced after the output personality itself is validated.
- Any change to the Modkit's shipped Pro2-mode behavior — this document's Modkit table is additive
  (a second mode), not a replacement.
