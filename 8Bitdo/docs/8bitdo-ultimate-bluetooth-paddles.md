# 8BitDo Ultimate Bluetooth (first model) back paddles

Status: independent P1/P2 transport, Pico translation to GL/GR, ordinary input,
reconnection, and console wake are hardware-confirmed with the known-good
paddle firmware. The bounded reconnect-timeout experiment was hardware-rejected:
it did not improve reconnect speed and broke console wake.

This note covers the first-generation 8BitDo Ultimate Bluetooth Controller with two rear
buttons labelled P1/P2. It does not claim the same layout for Ultimate 2, Ultimate 2C, the
2.4 GHz-only model, or later revisions.

## Identity boundary

In Bluetooth mode the controller impersonates a Nintendo Switch Pro Controller:

- SDP VID/PID: `057E:2009`
- product name: `Pro Controller`
- tested Bluetooth OUI: `E4:17:D8` (per-device suffix redacted)
- IEEE MA-L prefix `E4:17:D8`: `8BITDO TECHNOLOGY HK LIMITED`

VID/PID and product name therefore cannot distinguish it from a genuine Nintendo
controller. The stock-profile fallback path requires all of:

1. Bluetooth address prefix `E4:17:D8`
2. exact product name `Pro Controller`
3. `057E:2009`, or a still-unresolved zero VID/PID that does not contradict that identity

BTstack stores addresses in reverse byte order, so the runtime comparison is
`bd_addr[5..3] == E4:17:D8`. Genuine Nintendo controllers and contradictory
resolved identities do not enter the chord-fallback path. The custom
reserved-bit transport does not depend on identity metadata.

## Transport investigation

The physical rear switches are independent, but the stock runtime transports hide them:

| Transport/mode | Result |
|---|---|
| Direct USB XInput runtime (`2DC8:3106`) | P1/P2 produce no input-report changes |
| Bluetooth simple report `0x3F` | P1/P2 produce no input-report changes |
| Stock Bluetooth full Switch report `0x30` | P1/P2 produce no input-report changes |
| Custom Bluetooth full Switch report `0x30` | P1=`data[4] 0x80`, P2=`data[4] 0x40`, both independent |
| USB vendor/config mode (`2DC8:6007`) | Independent bits: P1=`0x02`, P2=`0x01`, both=`0x03` |

The stock controller firmware scans both switches, but its Switch report builder omits them.
A dongle-only quirk cannot recover state that never reaches Bluetooth.

The official configuration format has 20 source mappings per profile. The last two are the rear
buttons, but the UI does not offer P1/P2 as output targets for this model. Writing the reserved
P1/P2 output codes (`0x02000000`/`0x04000000`) was accepted and read back, but still emitted
nothing over Bluetooth.

## Controller firmware path

Official type-41 controller firmware has a 28-byte header followed by an address-dependent
encoded payload. Firmware 1.11 targets product `0x6007`, writes 104,960 payload bytes at
`0x01018000`, and executes through the firmware's `0x00018000` mapping. The official updater's
native generic path parses that header and streams the already-encoded payload to the bootloader;
no application signature or host-side cryptographic verification was found.

The transform was recovered and verified by decoding and re-encoding official 1.09, 1.10, and
1.11 images byte-for-byte. In decoded 1.11:

- `FUN_00025ad4` scans the physical matrix.
- physical P1 is internal mask bit `0x04000000`.
- physical P2 is internal mask bit `0x02000000`.
- the profile button is the adjacent `0x10000000` input.
- the generic/configuration report preserves P1/P2 independently.
- `FUN_00020d40`, the Switch `0x30`/`0x3F` report builder, never tests P1/P2.
- both P1/P2 bits survive the call path into that builder.

The firmware's bonded Bluetooth reconnect path was also traced independently:

- four 28-byte host records exist in persistent storage, but the active
  Bluetooth/Switch mode selects one record rather than cycling all four
- `FUN_00025D04` sends that record's six-byte address, 16-byte link key, and a
  4,800-slot reconnect timeout to the Bluetooth coprocessor
- 4,800 Bluetooth baseband slots are 3,000 ms
- the controller schedules a matching 6,000 ms watchdog and retries the same
  record on reconnect failure, up to 21 attempts
- official controller firmwares 1.09, 1.10, and 1.11 use the same timeout

A separately named reconnect test image reduced the module timeout to 3,200
slots (2,000 ms) and the watchdog to 4,000 ms, preserving the exact 2:1
real-time relationship. It inherited the known-good paddle patch and changed
only three additional decoded bytes. Hardware testing found no reconnect-speed
improvement and broke console wake. The result confirms the observed delay is
not governed by this timeout pair; the image is rejected and must not be
flashed again.

The custom 1.11 patch is deliberately confined to the normal full-report exit:

| Address | Stock | Patch |
|---|---|---|
| `0x00020FD2` | displaced report write + branch to stock epilogue | four-byte ARMv6-M `bl 0x00031838` |
| `0x00031838` | referenced-by-nothing zero padding | 36-byte Thumb helper |

The helper restores the displaced write, copies P1/P2 to reserved system-byte bits 6/7, and
executes the original stack epilogue. It does not alter simple startup reports, standard button
mapping, sticks, motion, rumble, pairing, profile logic, or sleep behavior.

The patch builder is pinned to the exact official 1.11 image and rejects any other source hash,
header, decoded payload, hook bytes, or nonzero cave. Its decoded changed-byte check permits only
the hook and helper. The encoded custom image is deterministic:

- official 1.11 SHA-256:
  `1030145fec364aceb55ceaed221396131dcf02eaaeeb8bd9ad4044ba5596074d`
- custom paddle recovery image SHA-256:
  `8a561682ad6174322c95e70a53edd2c0ab080a41d826dcb1694a55cfde53167c`
- custom decoded payload SHA-256:
  `e74e6280b4ebf7ed6d79d44839b1d2e06a3a0effed8732b86f18a9adc12bcd98`

The superseded hardware-test image
`bc99674803782e59a25cc97655fbaeeed388fb7156bf33cc9e4663861475d989`
used `b.w` at the hook. Pairing and startup still worked, but entering the full
Bluetooth report path produced no input. Cortex-M0 assembly validation confirms
that `b.w` is unavailable on ARMv6-M while the replacement `bl` is supported.

Reproducible tooling and the guarded recovery procedure are in
`8Bitdo/firmware/`.

## Working profile encoding

Physical profile indicator 2 (internal profile index 1) was configured with:

| Internal mapping entry | Stored output bitmask |
|---|---|
| 18 | `0x00003030` (`A+B+X+Y`) |
| 19 | `0x00024C00` (`L+R+ZL+ZR`) |

The physical-button capture, not the assumed entry labels, defines the resulting semantics:

| Physical input | Full-report button bytes 3..5 | Held behavior |
|---|---|---|
| P1 | `40 10 C0` | Stable for the full hold |
| P2 | `0F 00 00` | Stable for the full hold |
| P1+P2 | `4F 10 C0` | Exact union |
| A control | `04 00 00` | Independent |

The first signature decodes as R + Home + L + ZL for this firmware/gamepad-mode combination.
It is not the literal four-shoulder chord requested in the stored bitmask. The second is all four
face buttons. Opposing D-pad bitmasks were rejected as a final encoding because firmware normalized
each pair to one ordinary D-pad direction.

## Dongle translation

`switch_pro_8bitdo.c` runs after the normal Switch parser and before router submission:

- custom full-report reserved bit 7 -> physical P1 -> `JP_BUTTON_L4` -> default NS2 mapping GL
- custom full-report reserved bit 6 -> physical P2 -> `JP_BUTTON_R4` -> default NS2 mapping GR
- both bits -> L4+R4 -> GL+GR
- the stock-profile chord signatures remain supported as a recovery-compatible fallback
- fallback-injected face/shoulder/Home bits are removed
- unrelated simultaneously held buttons are preserved

The custom transport is decoded directly from full-report wire byte `data[4]`
instead of through implementation-defined C bitfield packing. Reserved bits are
not identity-gated because stock Switch controllers leave them clear and late
identity metadata can otherwise discard the extension. Only fallback chord
recognition remains restricted to the captured 8BitDo identity. The
high-entropy fallback chords can theoretically be pressed manually; that known
limitation does not apply to the custom firmware's independent-bit path.

The changing second byte in captures such as
`30 f0 60 00 80 00 ...` is the normal Switch report timer/sequence byte, not
motion data.

## Reversibility and validation

The original managed configuration was backed up before any write:

- size: 1,652 bytes
- SHA-256: `09e49d03739a807f19665b8d0c7a838455a81dd189cb32c79d709c5b877ea015`

Each profile experiment performed a fresh read, saved another pre-write image, allowed mutations only in
the CRC and two mapping fields, wrote through the official native configuration library, and
required a successful readback.

The first controller flash exposed the ARMv6-M hook regression described above.
The controller remained recoverable over USB and through the separate
bootloader. The flash harness:

- accepts only the exact official recovery hash or exact custom hash above
- has a device-free `--validate` mode
- requires an explicit `--flash-approved-image` gate
- refuses to write unless the separate manual bootloader `2DC8:3208` is already present

Keeping manual boot entry independent of the application means the untouched official 1.11 image
remains the recovery path if the custom application fails to start.

Automated coverage in `8Bitdo/tests/test_switch_pro_8bitdo.c` pins:

- exact and provisional identity matching
- genuine-Nintendo and contradictory-identity rejection
- P1, P2, and simultaneous translation
- consumption of injected controls
- preservation of unrelated controls
- partial-chord and neutral behavior
- custom P1/P2 reserved bits, both together, and coexistence with the fallback

The expanded host suite passes, as do Pico W, Pico 2 W, and legacy Switch 1 builds. Offline
firmware validation confirms:

- official input hash/header/payload
- exact hook and zero cave preconditions
- only 39 decoded bytes actually change (zero bytes in the helper remain zero)
- custom encode/decode and decode/encode round trips are exact
- the hook disassembles to the intended ARMv6-M `bl` cave branch
- Cortex-M0 assembly rejects the superseded `b.w` and accepts the replacement
  `bl`
- the helper disassembles to the intended P1/P2 tests, report ORs, and original epilogue
- both custom and untouched stock images pass the guarded harness's offline validation

The independent-paddle and wake hardware gates pass on the known-good paddle
image. Reconnect-timeout shortening failed its hardware gate and was reverted;
future reconnect research must target a different part of the controller's
connection path.
