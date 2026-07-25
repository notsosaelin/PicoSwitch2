# Bluetooth Controller Battery Passthrough

Status: ✅ hardware-confirmed for the tested controller/personality matrix
Last updated: 2026-07-24

## Behavior

PicoSwitch2 carries a normalized `0..100` battery percentage, charging flag, and explicit validity
state from the connected Bluetooth controller to the active Nintendo USB personality. A valid 0%
reading is distinct from a controller that exposes no battery telemetry.

Controller-native HID telemetry is authoritative. The standard BLE Battery Service (BAS) is a live
fallback: it accepts 0%, follows later notifications, and cannot overwrite a native HID reading.
BAS does not expose charging state, so BAS-only controllers report their percentage as not charging.

If no trustworthy source exists, output retains the previous compatibility default—full battery
and external/wired power—instead of claiming that the physical controller is empty.

Config mode's Current Input Type panel shows the normalized percentage/charging state or
`Battery unavailable`. This separates input-source validation from the console UI: if config mode
has the correct value but the Switch does not, the remaining problem is output interpretation.

## Input coverage

| Controller family | Source | Level | Charging | Software validation |
|---|---|---:|---:|---|
| DualShock 3 | Native HID status | Coarse | Yes | Golden raw-status decoder tests |
| DualShock 4 | Native HID status | 10% steps | Yes | Golden wireless/charging/full/error tests |
| DualSense / Edge | Native HID status | 10% steps | Yes | Golden discharge/charge/full/error tests |
| Switch 1 Pro / Joy-Con / Switch-format 8BitDo | Native report `0x30` | Coarse | Yes | Golden level/charging tests |
| Wii U Pro | Native extension status | 25% steps | Yes | Golden active-low charging tests |
| Wiimote / Wiimote extensions | Native status report | Byte scale | No | Golden scale tests; conversion matches [upstream Linux](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-wiimote-modules.c) |
| Switch 2 BLE family | Standard BAS when exposed | Percentage | No | BAS zero/update/priority tests |
| Xbox BLE | Standard BAS when exposed | Percentage | No | BAS zero/update/priority tests |
| Stadia Bluetooth | Standard BAS when exposed | Percentage | No | BAS zero/update/priority tests |
| MouthPad BLE | Standard BAS when exposed | Percentage | No | BAS zero/update/priority tests |
| Generic BLE controller | Standard BAS when exposed | Percentage | No | BAS zero/update/priority tests |
| Classic Xbox, BattlerGC Pro, generic Classic HID | None currently | — | — | Explicit unknown/full fallback |

“When exposed” matters: the host can consume BAS, but a controller or firmware mode that does not
advertise the service cannot be queried generically. Some 8BitDo models change their available
services with their mode switch and therefore belong to either the BAS or fallback row at runtime.

## USB output coverage

| Personality | Encoded field | Quantization |
|---|---|---|
| Switch 1 Pro | Standard input byte 2 high nibble | Native `0/2/4/6/8`; low nibble remains wired |
| Pro Controller 2 | Report `0x09` power byte | Native `0..9`, plus external-power and charging bits |
| NSO GameCube | Report `0x0A` power byte | Native `0..9`, plus external-power and charging bits |
| Joy-Con 2 Left / Right | Report `0x07/0x08` power byte | Native `0..9`, plus external-power and charging bits |

The shared PC/Steam report `0x05` has voltage/current/charge-state fields rather than the main
Nintendo power byte. PicoSwitch2 does not invent battery voltage from a percentage, so those
existing compatibility placeholders remain unchanged. Console-facing native reports carry the
actual percentage.

## Hardware validation matrix

The available native-HID/BLE sources and all console-native USB power fields passed the physical
matrix without input, rumble, reconnect, or wake regressions. Unsupported Classic HID sources
retained the prior full/wired compatibility fallback. Use the procedure below for new controller
families and release regressions; software decoder coverage alone still does not justify marking a
new device confirmed.

For each available controller:

1. Note its own displayed/known battery state before pairing.
2. Pair it in the transport mode listed above.
3. Open the Switch controller/status UI in each applicable USB personality.
4. Confirm the displayed level is plausible and changes category at a sufficiently different
   source charge.
5. For controllers with native charge status, connect external power and confirm charging state
   without input, rumble, reconnect, or wake regressions.
6. For an unsupported Classic controller, confirm the old full/wired fallback remains stable.

No new hardware row should be promoted to confirmed from decoder tests alone.
