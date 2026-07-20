# Joy-Con 2 Implementation Audit — Passthrough, Orientation & the Double-Translation Flaw

> A full audit of the Joy-Con 2 personality: how an input report is built today, the specific design
> flaw that a **real** Joy-Con 2 exposes (its inputs get translated twice and partly dropped), and a
> design for two goals — **(A)** real Joy-Con 2 controllers pass through faithfully, and **(B)** any
> other controller works in Joy-Con mode in **either** orientation (sideways *or* upright).
>
> **Documentation only — no code changed.** Companion to `SERIAL-GENERATION.md` / `MOUSE-MODE.md`.
> Primary sources read for this audit: `src/switch_joycon2/switch_joycon2_encode.c`,
> `src/switch_joycon2/switch_joycon2.c`, `src/bt_hid/bt/bthid/devices/vendors/nintendo/switch2_ble.c`,
> `src/bt_hid/ns2_seam.c`, `src/report.c`, `src/bt_hid/core/buttons.h`.
> Confidence: **Confirmed** (verified in source this pass) · **Strong Evidence** · **Hypothesis** ·
> **Unknown**.

## 0. Summary up front

- **The flaw is a *double translation* plus *data loss*.** Every input — including a genuine
  Joy-Con 2 — is first decoded by the BLE driver into a **Pro-controller-shaped normalized form**
  (`switch2_ble.c`), then the Joy-Con 2 encoder **unconditionally applies a sideways rotation +
  remap** (`switch_joycon2_encode.c`). For a generic pad that single translation is the whole point
  and works. For a real Joy-Con 2 the two stages **do not compose to identity**, and the decode
  stage **silently drops the rail SL/SR buttons and all motion**. (Confirmed.)
- **The encoder is device-type-blind.** `switch_joycon2_encode_report()` receives only a normalized
  `switch_pro_input_t` + `source_buttons` + side. It cannot tell "real Joy-Con 2" from "PS5 pad,"
  and the sideways policy is hard-coded — there is no orientation choice. (Confirmed.)
- **The fix is unusually cheap because the infrastructure already exists.** The global-input layer
  **already retains the source VID/PID** (`get_global_device`) and **the raw native report**
  (`get_global_raw_report`, fed by `bthid_on_raw_report`). So detecting a real Joy-Con 2 and
  forwarding its native fields need almost no new plumbing. (Confirmed.)
- **Central unknown that shapes everything:** because the generic sideways mapping "works fine," the
  console evidently consumes our `0x07/0x08` stick field **literally and does not re-rotate it**
  (Strong Evidence). Therefore the correct transform for a *real* Joy-Con 2 is **whatever maps the
  real Joy-Con's streamed frame onto that same literal frame** — which is *measurable*, not
  assumable. §5 makes deriving it the first experiment rather than guessing identity-vs-rotation.
- **Recommended shape:** replace the hard-coded sideways policy with a **mapping-profile selector**
  — `IDENTITY` (source is a real Joy-Con 2), `SIDEWAYS` (generic default), `UPRIGHT` (generic,
  user-selectable) — chosen per report build, with a raw-report fast-path for `IDENTITY`. §7.

## 1. The current pipeline (how a report is built)

```
 Real JC2 / Pro2 / PS5 / 8BitDo …
        │  (BLE or BT-Classic HID)
        ▼
 vendor driver  ── e.g. switch2_ble.c ─────────────────────────────────────┐
   • parses native report into input_event_t                               │
   • maps buttons → JP_BUTTON_* in a PRO-CONTROLLER (upright, 2-handed) frame│  ← lossy for a real JC2
   • DROPS rail SL/SR (never mapped); DROPS motion (never parsed)           │
        │ router_submit_input()                                            │
        ▼                                                                   │
 ns2_seam.c router_submit_input()                                          │
   • → switch_pro_input_t `in` (sticks, triggers, motion, battery)         │
   • set_global_gamepad_input(slot,in)  set_global_raw_buttons(slot,b)     │
   • set_global_device(slot, name, VID, PID)   ← identity retained ✓       │
 bthid_on_raw_report() → set_global_raw_report(slot, data, len)            │  ← native bytes retained ✓
        │                                                                   │
        ▼                                                                   │
 switch_joycon2.c switch_joycon2_build_report()                            │
   • get_global_gamepad_input(0,&in);  get_global_raw_buttons(0)           │
   • switch_joycon2_encode_report(&in, raw, s_side, …)                     │
        │                                                                   │
        ▼                                                                   ▼
 switch_joycon2_encode.c  ── ALWAYS applies the sideways profile: ──────────
   • 90° stick rotation (joycon2_pack_sideways_stick)
   • sideways button remap (face→directions, L1/R1→rail, …)
        │
        ▼
 Joy-Con 2 native report 0x07 (L) / 0x08 (R) / 0x05  → Switch 2 console
```

The encoder's own header comment states the intent plainly: *"The source-side translation implements
the project's deliberate single-Joy-Con sideways profile"* (`switch_joycon2_encode.c:57-60`). That
intent is correct for a generic pad; the bug is that it is **the only** path.

## 2. The flaw, precisely

A genuine Joy-Con 2 already emits Joy-Con-2-native reports. Feeding it through the pipeline does:

```
native JC2 report ──decode──▶ Pro-shaped normalized ──encode(sideways)──▶ JC2 report'
                     (lossy)                            (rotate + remap)
```

- **Not an identity.** `decode` maps the Joy-Con's buttons through a Pro-controller table
  (`switch2_ble.c:317-357`) and its stick into a generic left-stick frame; `encode` then rotates 90°
  and remaps for sideways hold (`switch_joycon2_encode.c:72-110`). The composition is a scrambled
  mapping, not the pass-through the user expects — this is exactly the "mappings thrown off" symptom.
- **Rail buttons vanish.** `switch2_ble.c` maps `SW2_L/R/ZL/ZR`, D-pad, face, ±, stick-clicks,
  Home/Capture/C, and grips — but **never** `SW2_L_SR`/`SW2_L_SL`/`SW2_R_SR`/`SW2_R_SL` (bits
  4,5,20,21). On a single Joy-Con held sideways, **SL/SR *are* the shoulder buttons**, so the real
  controller's most-used sideways buttons are dropped before the encoder even runs. (Confirmed.)
- **The normalized vocabulary has no home for SL/SR.** `buttons.h` defines B1–B4, L1/R1/L2/R2,
  S1/S2, L3/R3, D-pad, A1–A5, L4/R4/L5/R5 — but **no SL/SR concept**. So even if the driver wanted
  to forward them, the generic model can't represent them distinctly. (Confirmed — this is why a
  faithful path must bypass or extend the normalized form; §5, §7.)
- **Motion is dropped for real JC2.** `switch2_ble.c` parses only buttons + sticks (+ GC triggers);
  it never fills `has_motion`/`accel`/`gyro`. A real Joy-Con 2's gyro/accel never reach the encoder.
  (Confirmed.)

## 3. What already survives to the encoder (the enabling infrastructure)

The fix is cheap because three things are **already** available at the personality layer:

| Capability | Accessor | Set by | Use |
|---|---|---|---|
| Source **VID/PID** | `get_global_device(0,…,&vid,&pid)` | `ns2_seam.c:295` `set_global_device` | Detect a real Joy-Con 2: VID `0x057E`, PID `0x2067` (L) / `0x2066` (R) |
| **Raw native report** bytes | `get_global_raw_report(0,buf,n)` | `ns2_seam.c:318` via `bthid_on_raw_report` | Passthrough source for `IDENTITY` profile |
| Normalized input + raw buttons | `get_global_gamepad_input` / `get_global_raw_buttons` | `ns2_seam.c:275-276` | Existing generic path (sideways/upright) |

The PID even encodes the **side** directly (`0x2067`=Left, `0x2066`=Right — `switch2_ble.c:25-26`),
which matters for §5's side-matching constraint.

## 4. Defect list (ranked)

| # | Defect | Evidence | Severity |
|---|---|---|---|
| 1 | Real Joy-Con 2 double-translated → scrambled mapping | §2; `switch_joycon2_encode.c:61-110` | **High** — the reported bug |
| 2 | Rail SL/SR dropped by the BLE decoder | `switch2_ble.c:312-357` (no `SW2_*_S*` mapping) | **High** |
| 3 | Encoder cannot see source type or orientation | `switch_joycon2_encode.c:61` signature | **High** (root cause of #1) |
| 4 | No upright orientation for generic pads | only `joycon2_pack_sideways_stick` exists | **Medium** (goal B) |
| 5 | Real JC2 motion (gyro/accel) dropped | `switch2_ble.c` never sets `has_motion` | **Medium** |
| 6 | No SL/SR in the normalized vocabulary | `buttons.h:48-91` | **Medium** (blocks a clean non-bypass path) |
| 7 | Rumble back to a real JC2 unimplemented | `switch2_ble_task` TODO (`switch2_ble.c:372-377`) | **Low** (fidelity) |
| 8 | Personality side is fixed, not matched to the real JC2 | `s_side` set once via `switch_joycon2_set_side` | **Medium** (passthrough correctness) |

## 5. Goal A — real Joy-Con 2 passthrough

**The console consumes our stick field literally.** Because a generic pad's *pre-rotated* sideways
stick "works fine," the console is **not** applying its own single-Joy-Con rotation to our
`0x07/0x08` reports (Strong Evidence). So "passthrough" means: **transform the real Joy-Con's
streamed frame into the exact frame the console consumes literally** — and that transform is
*measurable*, not something to assume.

### Experiment first (do this before writing the mapping)

> **The single most valuable step in this whole audit.** Pair a real Joy-Con 2, log the BLE
> driver's `raw_lx/raw_ly` and `sw2_buttons` while pressing known physical inputs (push the
> sideways-"up", press SL, press SR, press the stick). Compare against what the *generic sideways
> profile* emits for the same intended in-game action. The delta **is** the correct passthrough
> transform. Likely outcomes:
> - If the real Joy-Con streams its **native/physical** frame → the correct transform is the *same*
>   rotation the generic profile already applies (so `IDENTITY` ≈ reuse the sideways stick rotation
>   but skip the lossy Pro-remap and keep SL/SR).
> - If the real Joy-Con streams an **already-sideways** frame → the correct transform is a true
>   identity/byte copy.
>
> We cannot tell which from source alone — hence measure. (Unknown → resolvable in one session.)

### Strategy A1 — native field passthrough (recommended)

When `get_global_device` reports a real Joy-Con 2, **bypass the normalized round-trip**: read the
native report via `get_global_raw_report`, copy the button word / stick / motion / mouse fields
straight into the emulated `0x07`/`0x08` report, and only **re-stamp** the counter (`out[0]`) and
battery (`out[1]`) to the dongle's own cadence. This preserves SL/SR and motion for free and is the
highest-fidelity option. Requirements/caveats:
- **Report-format alignment.** The dongle-as-BLE-host and the console-as-USB-host each negotiate a
  report ID independently. Verify the real Joy-Con streams the same field layout the console
  selected (0x07/0x08 vs 0x05); if they differ, transcode the fields rather than memcpy. (Hypothesis
  — needs the §5 capture to confirm offsets line up.)
- **Side match (defect #8).** A real Left Joy-Con (PID `0x2067`) must be presented as JOYCON2_L. If
  the personality side ≠ the real Joy-Con's side, refuse passthrough or auto-align the personality
  from the PID. Recommend auto-align (the PID is already known at connect).

### Strategy A2 — extended-normalized identity profile (fallback)

Keep the decode→encode architecture but (a) **extend** the seam/normalized form to carry the fields
the generic model lacks (SL/SR, un-rotated native stick, motion), and (b) add an `IDENTITY` profile
that copies them through with no rotation and no Pro-remap. More invasive (touches `buttons.h`,
`switch2_ble.c`, `ns2_seam.c`) but composes cleanly with goal B and benefits any future controller
that has SL/SR-like buttons. Prefer A1 for JC2 specifically; keep A2 in view if a raw-report
fast-path proves fragile.

## 6. Goal B — other controllers, either orientation

Generalize the *single* hard-coded sideways policy into **selectable orientation profiles** for
generic controllers:

| Profile | Stick | Buttons | Use |
|---|---|---|---|
| **SIDEWAYS** (current default) | 90° rotate into the sideways shell frame (`joycon2_pack_sideways_stick`) | face→directions, L1/R1→SL/SR, ± / ZL·ZR per side | one Joy-Con held horizontally |
| **UPRIGHT** (new) | **no rotation** — the paired stick maps straight to the single stick | face buttons stay as face buttons; L1/R1→SL/SR; single ± ; ZL/ZR from L2/R2 | one Joy-Con held vertically / as a standalone mini-pad |

Design notes:
- **UPRIGHT** is mostly the sideways table **minus the 90° rotation** and with the face-button remap
  reverted to a natural face layout. The rotation is already isolated in
  `joycon2_pack_sideways_stick`, so an `UPRIGHT` variant is a small, well-contained addition.
- Expose the choice as a **config setting** (per side, or global) next to the existing
  `joycon2_left_accent` / `joycon2_right_accent` settings in `config.c` — same pattern, add
  `joycon2_orientation`. Default **SIDEWAYS** (preserves today's proven behavior).
- Orientation applies **only to generic sources**; a real Joy-Con 2 always uses `IDENTITY`
  regardless of the setting (its physical orientation is the user's business, and its report already
  encodes the intended frame).

## 7. Proposed architecture

Introduce a **profile selector** and parameterize the encoder:

```c
typedef enum { JC2_MAP_IDENTITY, JC2_MAP_SIDEWAYS, JC2_MAP_UPRIGHT } joycon2_map_profile_t;

// in switch_joycon2_build_report():
uint16_t vid, pid; char name[…];
get_global_device(0, name, sizeof name, &vid, &pid);
bool real_jc2 = (vid == 0x057E) && (pid == 0x2067 || pid == 0x2066);

joycon2_map_profile_t profile =
    real_jc2 ? JC2_MAP_IDENTITY
             : config_get_joycon2_orientation();   // SIDEWAYS (default) | UPRIGHT

switch_joycon2_encode_report(&in, raw, s_side, profile, counter, p);
```

- `switch_joycon2_encode_report(..., profile, ...)` gains a `profile` argument. `SIDEWAYS` keeps the
  exact current body; `UPRIGHT` swaps the stick pack + button table; `IDENTITY` takes the
  raw-report fast-path (A1) — reading `get_global_raw_report` and copying native fields.
- **The stick rotation stays isolated** in a `joycon2_pack_stick(profile, …)` that dispatches to the
  rotated (sideways) or straight (upright) packer — a minimal, testable refactor of the existing
  `joycon2_pack_sideways_stick`.
- **Purity preserved.** The encoder stays a pure function of (input, raw, side, profile); only the
  *selection* of the profile lives in the runtime glue (`switch_joycon2.c`), matching the file's
  existing "pure encoders / runtime glue" split.
- **Side auto-alignment** (defect #8): when a real JC2 connects, set `s_side` from its PID so
  `IDENTITY` and the identity block agree with the physical unit.

This one seam fixes defects #1, #3, #4 and enables #2/#5 (via the A1 raw path) with a single, well-
contained change surface.

## 8. Mapping reference (current + proposed)

Current **sideways** button map (Confirmed from `switch_joycon2_encode.c:72-105`), Left side:

| Source (`JP_BUTTON_*`) | Emulated JC2 (L) bit | Meaning |
|---|---|---|
| L3 | b0 0x80 | stick click |
| S1 (Select) | b0 0x40 | Minus |
| R2 | b0 0x20 | ZL |
| L2 | b0 0x10 | L |
| B3/B1/B4/B2 | b0 0x08/04/02/01 | Up/Left/Right/Down (face→directions) |
| L1 / R1 | b1 0x80 / 0x40 | **SL / SR (rail)** |
| Capture | b1 0x01 | Capture |

Proposed additions:
- **UPRIGHT (L):** identical shoulder/SL/SR/± mapping, but face buttons stay face buttons and the
  stick is packed **without** the 90° rotation. (Hypothesis — validate feel on hardware.)
- **IDENTITY (real JC2):** native `sw2_buttons` bits copied to their same-named emulated bits,
  **including SL/SR**, with the stick transform fixed by the §5 experiment. No Pro-remap.

## 9. Unknowns & experiments (ranked)

| # | Unknown | Test |
|---|---|---|
| 1 | Real JC2 streamed stick frame vs the frame the console consumes literally (§5) | Log BLE `raw_lx/ly` + `sw2_buttons` under known physical inputs; diff against the generic sideways output |
| 2 | Does the real JC2 stream the same report ID/offsets the console selects from the dongle? | Capture both negotiations; compare field offsets (memcpy vs transcode) |
| 3 | Does the console ever apply its own single-Joy-Con rotation to our reports (i.e., in some pairing/registration state)? | Present un-rotated vs rotated stick to the console; observe in-game direction |
| 4 | Exact real-JC2 SL/SR bit → emulated report bit correspondence | Press SL/SR on a real JC2, read `sw2_buttons` bits 4/5/20/21, map to output b1 |
| 5 | Real JC2 motion report layout (offsets for gyro/accel) | Parse a motion-enabled JC2 stream; cross-ref `report-0x09-motion.md` |
| 6 | Cross-side use (real L on an R personality) — refuse or auto-align? | Try both; confirm console behavior when side mismatches |

## 10. Risks & notes

- **Preserve the working generic path.** `SIDEWAYS` must remain byte-identical to today's behavior;
  the refactor is purely additive (new profiles, new selector). Regression-test a generic pad first.
- **Passthrough is only as faithful as the decode.** A1's fidelity depends on the raw report
  actually carrying SL/SR + motion at known offsets — confirm before trusting memcpy (§9 #2/#4/#5).
- **Side matching is a correctness issue, not cosmetic.** A real Left Joy-Con presented as R will
  feel broken even with a perfect transform; auto-align `s_side` from PID.
- **Motion & rumble are follow-ons.** Full indistinguishability for a real JC2 also needs its
  gyro/accel forwarded (defect #5) and rumble routed back (defect #7); scope them after the
  button/stick passthrough lands.
- **No firmware change in this document** — this scopes the work; nothing is implemented.

## 11. References

- `src/switch_joycon2/switch_joycon2_encode.c:27-114` — `joycon2_pack_sideways_stick`,
  `switch_joycon2_encode_report` (unconditional sideways translation; header comment lines 57-60).
- `src/switch_joycon2/switch_joycon2.c:283-294` — `switch_joycon2_build_report(05)`; the encoder
  call sites; `s_side` (lines 22-25).
- `src/bt_hid/bt/bthid/devices/vendors/nintendo/switch2_ble.c:25-28` (PIDs, side),
  `:30-56` (SW2 button bits incl. `SW2_*_SR/SL`), `:312-370` (button/stick mapping — SL/SR & motion
  omitted).
- `src/bt_hid/ns2_seam.c:275-295` (`set_global_gamepad_input`/`set_global_device`),
  `:316-319` (`bthid_on_raw_report` → `set_global_raw_report`).
- `src/report.c:62-148` — `get_global_gamepad_input`, `get_global_raw_buttons`,
  `get_global_raw_report`, `get_global_device` (identity + raw report retained).
- `src/bt_hid/core/buttons.h:48-91` — normalized `JP_BUTTON_*` vocabulary (no SL/SR).
- `src/config.c:43-44,160-177` — `joycon2_*_accent` settings (the pattern a `joycon2_orientation`
  setting would follow).
- Protocol layout: `docs/switch2-joycon2/protocol.md`; motion: `docs/switch2/report-0x09-motion.md`.
