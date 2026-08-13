# Android handheld motion — axis derivation and independent corroboration

Status: 🔵 corroborated by an independent working implementation; **not yet measured on our hardware**
Date: 2026-08-13
Related: [`android-controller-bridge.md`](android-controller-bridge.md),
[`../../src/bt_hid/motion/ns2_motion_seam.c`](../../src/bt_hid/motion/ns2_motion_seam.c)

## Question

`SWITCH_MOTION_SOURCE_ANDROID` needs an axis/sign row mapping the handheld's sensor frame into the
Pro Controller 2 carrier. A wrong-but-proper rotation passes every static check — the determinant
rule cannot catch it, and gravity alone cannot either (a single vector looks correct reflected or
rotated). This is exactly how the `SWITCH1` row was wrong for weeks. Can the row be established
without a physical pitch/yaw/roll capture?

## Evidence source

[Dycool/NS-PC-Control](https://github.com/Dycool/NS-PC-Control) implements phone gyro into this same
Switch 2 motion report from a Raspberry Pi USB gadget. It is an independent, shipped, working
implementation, and its motion carrier explicitly credits this project
("PicoSwitch2's hardware-validated Switch-1 seam", Apache-2.0). The owner has previously collaborated
with its author on decoding the motion format. Two files matter:

| Stage | File | Transform |
|---|---|---|
| 1. Screen normalization | `webapp/js/ns_core.js` (`NSCore.motion`) | `screenRemap()` by display angle |
| 2. Phone → Switch-1 Pro frame | same | `accel_out = [-a₂, -a₀, +a₁]`, `gyro_out = [-g₂, -g₀, +g₁]` |
| 3. Switch-1 Pro → Pro2 carrier | `server/src/s2_motion_carrier.cpp` | `out = [-in₁, +in₀, +in₂]` |

## Derivation

Let `p = (pₓ, p_y, p_z)` be the screen-normalized phone vector (X right, Y up the screen, Z out of
the screen toward the user — the standard Android/W3C device frame).

Stage 2: `s = (−p_z, −pₓ, +p_y)`

Stage 3: 
- `out₀ = −s₁ = −(−pₓ) = +pₓ`
- `out₁ = +s₀ = −p_z`
- `out₂ = +s₂ = +p_y`

**Composite: `out = (+pₓ, −p_z, +p_y)`**, i.e. in this project's seam notation
`src = {0, 2, 1}`, `sign = {+1, −1, +1}`.

That is **byte-for-byte the row already written** for `SWITCH_MOTION_SOURCE_ANDROID` (which was
reasoned independently from the frame conventions, then found to agree). Two independent derivations
landing on the same proper rotation is meaningfully stronger than either alone.

Determinant check: permutation `(0,2,1)` is one transposition (odd, parity −1); sign product is
`+1 · −1 · +1 = −1`; total `+1` — a proper rotation, as `tools/test_ns2_motion_seam.c` requires.

## Scale corroboration

| Quantity | NS-PC-Control | PicoSwitch2 Android bridge | Agreement |
|---|---|---|---|
| Gyro | `GYRO_COUNTS_PER_DPS = 16.384` | `16.384` counts/dps | ✅ exact |
| Accel | `4096 / 9.80665` → 4096 counts/g, carrier does **not** halve | 8192 counts/g at the event, `ns2_motion_seam_apply` **halves** to 4096 | ✅ equivalent at the carrier |

The accel difference is only *where* the halving happens. Both deliver the genuine Pro Controller 2
carrier's 4096 counts/g.

## What this corrected in our implementation

Stage 1 (`screenRemap`) had **no equivalent in our app** — the Android bridge read raw sensor axes.
Android reports sensors in the device's **natural** orientation, not the orientation being held, so:

- a phone held sideways to play (rotation 90/270) would send **pitch as roll** — aiming rotated 90°;
- a handheld whose natural orientation is already landscape would be correct, and one whose natural
  orientation is portrait would not — i.e. it would work on some devices and not others, which is
  the worst kind of bug to diagnose from user reports.

`MotionOrientation` now applies the same remap before the wire conversion
(`0°` identity, `90°` `[x,y] → [−y,x]`, `180°` `[−x,−y]`, `270°` `[y,−x]`; Z is the screen normal and
is never touched). Eight unit tests cover each rotation, the four-rotation identity, negative angles,
malformed angles falling back to identity rather than scrambling axes, and int16-extreme negation.

## Confidence and how this gets confirmed

- **Strong evidence, not proof.** This is a derivation from another project's working code plus our
  own frame reasoning. It is not a measurement on our hardware, and NS-PC-Control's transport (Pi USB
  gadget, its own client) is not identical to ours.
- **Confirmation is ordinary play, not a test ritual.** Do NOT ask the maintainer to perform
  deliberate pitch/yaw/roll rotations — that request has been made and declined repeatedly and is not
  to be raised again. An axis or sign error in this row is not subtle: aiming is rotated or inverted
  within seconds of using motion in any game that has it. Normal use is a strictly better detector
  than a staged pose sequence, and it costs nothing.
- **If aim is wrong, the fix is one row.** The symptom identifies the error directly:

  | Reported symptom | Likely cause | Fix |
  |---|---|---|
  | Aim moves left when tilting right (or up/down inverted) | one sign wrong | flip that slot's sign |
  | Tilting produces the *other* axis's motion (pitch acts as yaw) | two `src` slots swapped | swap those `src` entries |
  | Aim rotated 90° only on some devices | screen-orientation remap, not this row | `MotionOrientation` (app side) |

  Any correction must keep the row a proper rotation (determinant +1), which
  `tools/test_ns2_motion_seam.c` enforces.
- The row lives in firmware precisely so a correction is a flash, not a new APK.

## Future work

- If a handheld reports a non-standard sensor frame, prefer a per-device note over editing this row
  (the seam file's own rule: never edit one family's row to fix another).
