# Joy-Con 2 Bluetooth Mouse Bridge

Status: ✅ hardware-confirmed

Last updated: 2026-07-24

## Current behavior

PicoSwitch2 can pair a Bluetooth HID mouse and expose it through either Joy-Con 2 personality's
native relative-mouse field. The Switch 2 displays its pointer and consumes relative movement.
Mouse buttons and wheel-to-stick menu navigation are hardware-confirmed.

Mouse output is gated twice:

1. the connected source must be classified as `INPUT_TYPE_MOUSE` from a relative X/Y HID report;
2. the console must enable Joy-Con feature bit `0x10` through command `0x0C`.

A Bluetooth gamepad therefore cannot activate mouse mode merely because the active USB personality
is Joy-Con 2.

## Input classification and parsing

`src/bt_hid/bt/bthid/devices/generic/bthid_mouse.c` supports Classic and BLE HID mice. Descriptor
parsing requires relative Generic Desktop X and Y fields in the same report, which prevents
absolute gamepad sticks from being misclassified. It also discovers the wheel and up to 16
Button-page inputs. Simple Classic devices can use a three-byte boot-report fallback when no
descriptor is available.

Relative X/Y/wheel values are accumulated across Bluetooth reports and consumed once by the USB
encoder. Battery notifications and other cached-state republishes cannot replay old motion.
Disconnect clears both accumulated movement and any held wheel pulse.

## Console report

The genuine capture established that mouse mode is feature bit `0x10`, not command `0x13`. The
console normally requests mask `0x37` (buttons, sticks, IMU, mouse, and rumble).

For native Joy-Con reports `0x07` (Left) and `0x08` (Right):

- offsets `0x09..0x0A`: signed relative X;
- offsets `0x0B..0x0C`: signed relative Y;
- offset `0x0D`: a constant on-surface value in the genuine range;
- the motion area carries the stationary sideways posture/timing needed for pointer activation.

The report-`0x05` absolute mouse form is not implemented. Relative events are still consumed while
that format is active so changing formats cannot release stale movement as a burst.

## Buttons and wheel

The generic source map is:

| Mouse input | Normalized source | Right Joy-Con result | Left Joy-Con result |
|---|---|---|---|
| Left click | L2 | R | L |
| Right click | R2 | ZR | ZL |
| Middle click | Home | Home | Unused |
| Back | left face / Square | B | D-pad Up |
| Forward | bottom face / Cross | A | D-pad Left |

Additional discovered buttons occupy spare normalized controls and remain available for later
remapping work.

Each wheel notch produces a bounded 40 ms full local-stick Up/Down pulse; repeated same-direction
notches extend the pulse up to 400 ms and reversing direction switches immediately. The console
rotates the local stick for a sideways half, so menu navigation follows the active horizontal or
vertical menu without hard-coding screen orientation.

## Evidence and tests

- Genuine Joy-Con 2 decrypted capture: feature mask and native relative field.
- Hardware: pointer activation, motion, buttons, mouse-only gating, disconnect cleanup, and wheel
  menu navigation.
- Host: descriptor classification, relative-axis extraction, button count, and gamepad rejection
  in `tools/test_bthid_mouse_report.c`.
- Command evidence:
  [`../experiments/2026-07-19-usb-command-ab-diff.md`](../experiments/2026-07-19-usb-command-ab-diff.md).
