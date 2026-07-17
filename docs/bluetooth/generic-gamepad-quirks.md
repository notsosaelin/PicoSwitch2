# Generic Bluetooth Gamepad Quirk Architecture

Status: reconstructed on `refactor/quirk-split`; source/build/host-test validated, hardware
regression pass required before merge into `ns2-testing`.

## Purpose

`bthid_gamepad.c` is the lowest-priority Bluetooth HID fallback. It owns behavior common to
descriptor-driven gamepads: HID field discovery, axis scaling, hat conversion, report-ID
filtering, the descriptorless Classic-BT fallback, event lifecycle, and router submission.

Controller-specific deviations live under `devices/generic/quirks/`. A profile may provide:

- a fixed HID-usage button map or a map selector;
- a digital-trigger policy;
- an extra-input extractor for evidence-backed raw bytes;
- a validated output/rumble sender.

The ordered registry in `bthid_gamepad_quirks.c` resolves one profile from VID, PID, advertised
name, and descriptor button count. Exact-model matches precede wider vendor/mechanism fallbacks.
The generic profile is always returned when no quirk applies.

```text
Bluetooth report
  -> shared HID descriptor engine
  -> resolved profile's button table
  -> optional profile extra-input extractor
  -> shared input_event_t
  -> router
```

This does not make unknown special controls self-describing. A nonstandard controller still needs
a capture-backed profile; the architectural benefit is that adding it does not modify the shared
parser or unrelated controllers.

## Current profiles

| Profile | Match | Specialized behavior |
|---|---|---|
| `generic` | Fallback | Sequential HID usage map |
| `xbox` | Microsoft VID or Xbox name | BLE/Classic map selection, extra Share/Back byte, 20-byte paddle fallback, validated rumble |
| `xbox_elite2` | Microsoft Elite 2 PID | Xbox behavior plus four back paddles |
| `bitdo_paddle` | 8BitDo VID with more than 14 buttons | Usage 3/6 paddle layout |
| `bitdo_ultimate_mg` | `2DC8:200B` | 8BitDo map selection plus raw byte-8 L4/R4 paddles |
| `bitdo_m30` | M30 name or known PID | Sequential map with synthesized analog triggers suppressed |
| `bitdo_ngc_modkit` | `2DC8:286A` | Native GC button map, Z, and independent L/R detents |

## Preserved behavior constraints

- Profile resolution runs after descriptor parsing and again when asynchronous VID/PID data
  arrives. It also runs during initialization so descriptorless fallback devices have a profile.
- Xbox name matching remains valid for input when VID/PID is unavailable.
- Generic-driver Xbox rumble retains the v1.2 Microsoft-VID gate. The refactor does not send an
  Xbox output packet to an arbitrary name-only or generic HID device.
- Failed Xbox output sends keep the dirty state and rumble cache unchanged so STOP retries.
- All profile-owned native fields are cleared before every extraction, preventing stale detent/Z
  state across reports.
- The diagnostic `btid desc` JSON retains its existing identity booleans.

## Validation and rollback

`tools/test_bthid_gamepad_quirks.c` pins registry priority, every button-map variant, M30 trigger
policy, NGC native fields, MG/Elite paddles, Xbox Share/Back handling, and Xbox rumble dispatch.
The full host matrix contains 14 passing suites after adding it. Pico W, Pico 2 W, and the legacy
Switch 1 configuration all build successfully.

The pre-refactor release is permanently available at tag `v1.2.0`. The refactor branch has two
separate recovery commits:

1. `7cb7d55` — characterization tests and the preserved Elite fallback correction.
2. `b88279f` — shared-driver integration.

`_reverted_quirk_split/` remains an archival copy until hardware validation is complete. It is not
compiled and must not be copied over current sources; the active profiles include later fixes.

## Hardware gate before merge

- Generic controller: ordinary buttons, both sticks, triggers, and D-pad.
- 8BitDo Ultimate 2 MG: ordinary controls and both back buttons.
- 8BitDo NGC Modkit: ordinary controls, analog triggers, Z, and both full-pull detents.
- 8BitDo M30: digital L2/R2 remain remappable and do not leak through analog trigger axes.
- Xbox Elite Series 2: ordinary controls, all four paddles, rumble ON/STOP, and reconnect.
- Recheck BOOTSEL gestures and wake once with a generic-driver controller connected; this refactor
  does not touch scheduling, but those are release invariants.

