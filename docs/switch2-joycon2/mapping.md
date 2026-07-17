# Joy-Con 2 Left/Right Output Personalities — Sideways Mapping Policy

> Current policy (2026-07-16): each individual Joy-Con personality is intended for the
> Switch's horizontal single-controller mode. The translation is host-tested in both report
> formats (`0x07`/`0x08` and `0x05`) and hardware-confirmed on Switch 2.

## Input model

Bluetooth drivers expose both a normalized physical button bitmap and the configurable Pro
Controller 2 semantic mapping. The explicit face, shoulder, trigger, Select/Start, Home, L3, and
D-pad sources below read the normalized physical bitmap directly. This is intentional: the table
describes controls on the paired controller and must not change because a persistent Pro2 remap
assigns those controls different semantic names.

Capture and C/GameChat are the two deliberate exceptions. They read the configured Pro2 Capture
and C destinations so controller-specific controls such as DualSense Touchpad/Mute, Switch 2
Capture/C, Edge Fn buttons, and user remaps continue to behave exactly like the Pro2 personality.

## Joy-Con 2 (L), held sideways to the left

| Joy-Con 2 (L) output | Paired controller source |
|---|---|
| SL | L1 / LB |
| SR | R1 / RB |
| Minus | Select / Create / Back |
| Stick | Left stick and D-pad |
| Stick click | L3 |
| D-pad Up | Square / X (left face button) |
| D-pad Down | Circle / B (right face button) |
| D-pad Right | Triangle / Y (top face button) |
| D-pad Left | Cross / A (bottom face button) |
| L | L2 / LT |
| ZL | R2 / RT |
| Capture | The configured Pro2 Capture source (DualSense touchpad by default) |

## Joy-Con 2 (R), held sideways to the right

| Joy-Con 2 (R) output | Paired controller source |
|---|---|
| SL | L1 / LB |
| SR | R1 / RB |
| Plus | Start / Options / Menu |
| Stick | Left stick and D-pad |
| Stick click | L3 |
| B | Square / X (left face button) |
| X | Circle / B (right face button) |
| Y | Triangle / Y (top face button) |
| A | Cross / A (bottom face button) |
| R | L2 / LT |
| ZR | R2 / RT |
| Home | Home / Guide / PS |
| C / GameChat | The configured Pro2 C source (DualSense Mute and Switch 2 C by default) |

## Stick and D-pad synthesis

The paired controller's left analog stick remains analog. Its D-pad is a second source for that
same Joy-Con stick and overrides only the pressed axis at full deflection. This permits digital
diagonals and also permits, for example, digital horizontal plus analog vertical input. Opposing
directions cancel on their axis and leave the analog value in control. The paired D-pad never
leaks into the Joy-Con's physical D-pad or face-button cluster; those are sourced only from the
paired face buttons in the tables above.

Axes are rotated into the sideways Joy-Con shell's local coordinates:

- Left applies a clockwise input transform because the emulated shell is held 90 degrees
  counter-clockwise. Paired Up therefore becomes the Left Joy-Con's local Right.
- Right applies a counter-clockwise input transform because the shell is held 90 degrees
  clockwise. Paired Up therefore becomes the Right Joy-Con's local Left.

The transform preserves the exact 12-bit center and endpoints despite the asymmetric
`0x000..0xFFF` range around center `0x800`.

## Report coverage and deliberate exclusions

`switch_joycon2_encode_report()` and `switch_joycon2_encode_report05()` implement the same policy;
the latter merely writes the translated controls into the shared report `0x05` positions.
Pro2 GL/GR paddle destinations no longer become Joy-Con SL/SR. SL/SR now have the unambiguous
L1/R1 sources requested for a standard paired controller, and spare paddles remain unused in
individual Joy-Con mode.

Mouse mode, NFC, and motion data remain outside this mapping pass because the project does not
currently supply those data streams. The physical capability and byte layouts remain documented
in `protocol.md`.
