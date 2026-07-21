# PicoSwitch2 Status

> Current-state snapshot. Historical implementation narratives are archived in
> [`docs/archive/status-through-2026-07-15.md`](docs/archive/status-through-2026-07-15.md).
> Planned work belongs in [`PLAN.md`](PLAN.md); evidence and protocol details belong under
> [`docs/`](docs/README.md).

Last verified: 2026-07-21
Branch: `ns2-testing`

## Current release

[`v1.4.0`](https://github.com/notsosaelin/PicoSwitch2/releases/tag/v1.4.0) was published on
2026-07-18 with Pico W and Pico 2 W UF2 assets. All 28 host-test executables pass. Pico 2 W adds
hardware-confirmed live DualSense audio, conditional headset routing, native PCM rumble with and
without a headset, and saved-bond reconnect recovery. Pico W retains its validated non-audio
configuration. The prior controller, personality, pairing, wake, BOOTSEL, battery, and mapping
baseline remains intact.

## Hardware-confirmed behavior

| Area | Status | Evidence |
|---|---|---|
| Switch 2 Pro Controller 2 USB identity and input | ✅ Confirmed | Real Switch 2 and PC/Steam |
| NSO GameCube USB identity and input | ✅ Confirmed | Real Switch 2 |
| NSO GameCube rumble | ✅ Confirmed | Real Switch 2; genuine-capture decoder |
| Real Pro Controller 2 input in NSO GameCube mode | ✅ Confirmed | L/R full-pull detents; ZL/ZR become GC ZL/Z |
| Joy-Con 2 Left and Right enumeration/input streaming | ✅ Confirmed | Real Switch 2 |
| Joy-Con 2 Left PC/Steam classification | ✅ Confirmed | Fresh Windows node and Steam UI; SDL Switch 2 driver enabled |
| Joy-Con 2 Left and Right sideways mappings | ✅ Confirmed | Real Switch 2; face/shoulder/trigger/stick profile |
| Joy-Con 2 rumble and STOP/reconnect behavior | ✅ Confirmed | Real Switch 2 |
| Switch 2 controller firmware identity/update status | ✅ Confirmed | Genuine `0x10/01` replies plus Switch 2 Update Controllers; Pro2, NSO GC, and both Joy-Con 2 personalities report up to date |
| Out-of-band UART protocol tracer | ✅ Confirmed | Real Switch 2 + genuine Pro Controller 2 source; complete 63-record Pro2 re-enumeration capture, zero overwrites, pull-transport framing validated |
| DualSense and DualSense Edge input | ✅ Confirmed | Real Switch 2 and Steam |
| Edge paddles, Fn buttons, and mute mapping | ✅ Confirmed | Real hardware |
| DualSense/Edge LEDs and rumble | ✅ Confirmed | Real hardware after report-boundary scheduler fix |
| Pro2 body/Joy-Con accents, Sony lightbar matching, and DualSense player-slot dots | ✅ Confirmed | Real Switch 2 and DualSense; config v8 hardware pass |
| BOOTSEL double-tap, triple-tap, and five-second hold with DualSense paired | ✅ Confirmed | Real hardware after report-boundary gesture service |
| Late BLE DIS VID/PID handoff and input continuity | ✅ Confirmed | Xbox Series BLE hardware regression after notification-first identity fix |
| Triple-tap post-wipe admission lock | ✅ Confirmed for the reported workflow | Wipe disconnects and requires an explicit new pairing window |
| Explicit re-pair after triple-tap wipe | ✅ Confirmed | Real hardware |
| Switch 2 wake from sleep | ✅ Confirmed | First real post-sleep controller input on real Switch 2 hardware |
| Reconnect/triple-tap false-wake protection | ✅ Confirmed | Most-controller pass plus genuine Switch 1 Pro initialization/reconnect regression |
| 8BitDo NGC Modkit rumble | ✅ Confirmed | Real hardware with BlueRetro-derived `0xA5 / DB LL RR` output |
| Retro Fighters BattlerGC Pro mapping | ✅ Confirmed | Pairing, labels, shoulders, analog/click triggers, L3/R3 suppression, and separate Home report |
| 8BitDo Ultimate Bluetooth P1/P2 | ✅ Confirmed | Custom firmware transport maps independent paddles to GL/GR and preserves wake |
| Bluetooth battery passthrough | ✅ Confirmed | Native HID/BLE sources and console-native USB power fields |
| Pro Controller 2 UAC1 USB audio function | ✅ Confirmed | Windows audio endpoints start without Code 10; no controller regressions |
| DualSense Bluetooth internal-speaker audio — Pico 2 W | ✅ Confirmed | Standard 300 MHz build; 13,225/13,225 PCM blocks encoded, zero drops/errors |
| DualSense Bluetooth internal-speaker audio — Pico W | ❌ Not supported | Fixed-point/XIP 300 MHz experiment barely played audio; standard build restored to validated non-audio profile |
| Standard 300 MHz Pico 2 W platform regression | ✅ Confirmed | LED/BOOTSEL, config persistence/readback, cold boot, and ten wake attempts per known controller |
| DualSense audio after bonded reconnect | ✅ Confirmed | Controller and dongle power cycles restore audio and native rumble through the saved bond; no fresh pair required |
| Switch 2 headset insertion and output | ✅ Confirmed | Physical DualSense jack is recognized; console audio plays through connected headphones with input/rumble/wake intact |
| Switch 2 headset removal/reinsert | ✅ Confirmed | Repeated cycles restore input, audio, and native haptics; unplugged full legacy rumble remains stable |
| DualSense rumble during console audio | ✅ Confirmed | Native-mode restoration, capture-derived peak preservation, and the waveform-preserving 3.25× curve are stable and judged close to HD Rumble |
| DualSense rumble without headset/audio | ✅ Confirmed | Pico 2 W reuses the native renderer with valid Opus silence only during active rumble plus a bounded two-packet STOP tail; Pico W retains compatibility rumble |
| Pico W and Pico 2 W builds | ✅ Compile-confirmed | Pico W uses the validated non-audio profile; Pico 2 W includes live audio at 300 MHz |

## Current USB personalities

Every boot starts in Pro Controller 2 mode. A five-second BOOTSEL hold advances the volatile cycle:

1. Switch 2 Pro Controller 2 (`057E:2069`)
2. NSO GameCube Controller (`057E:2073`)
3. Joy-Con 2 Left (`057E:2067`, experimental)
4. Joy-Con 2 Right (`057E:2066`, experimental)
5. CDC/configuration mode (`CAFE:4012`)
6. Back to Pro Controller 2

The selection is not persisted across power cycles.

## Bluetooth and BOOTSEL architecture

- Core 1 runs BTstack plus the vendored joypad-os HID layer.
- A persistent global pairing lock is installed before triple-tap disconnect/erase begins. Only an
  explicit double-tap pairing window reopens admission.
- One dongle serves one controller. Background BLE scan and Classic inquiry run only while no
  controller is connected; once a controller is HID-ready the pairing window closes (LED goes
  solid) and discovery idles, freeing Bluetooth bandwidth. The host stays connectable/discoverable,
  so a bonded Classic controller reconnects by paging in and a bonded BLE controller reconnects once
  discovery resumes at zero connections. Hardware-confirmed (Classic + BLE reconnect, wake, wipe/
  re-pair). Retiring the always-on multi-controller discovery is the general fix for the scanning
  radio contention noted in [`AUDIO-INVESTIGATION.md`](AUDIO-INVESTIGATION.md).
- Switch 2 controllers use a custom ATT pairing handshake, so the wipe policy cannot depend only on
  BTstack's LE bond database.
- Core 0 samples BOOTSEL using a cooperative cross-core SRAM handshake at a 30 ms cadence.
- Incoming HID report boundaries service raw BOOTSEL sampling, gesture recognition, and
  `bthid_task()`. This prevents sustained DualSense Classic traffic from starving controller output
  or button gestures. The timers remain the quiet/disconnected fallback.
- BLE HID binds immediately from the best identity available and enables report notifications
  before querying Device Information Service. A later DIS VID/PID is always handed to BTHID for an
  idempotent re-evaluation; contradictory Xbox BLE, Stadia, and MouthPad name matches can no longer
  pin the wrong parser while input is already streaming. The updated path is hardware-confirmed
  with Xbox Series BLE.

See [`docs/architecture/overview.md`](docs/architecture/overview.md) and
[`docs/bluetooth/btstack-implementation.md`](docs/bluetooth/btstack-implementation.md).

## Known open issues

| Priority | Issue | State |
|---|---|---|
| P2 | DualSense microphone return | 🟡 Headset presence is implemented; microphone Opus decode and USB return remain |
| P2 | Let reconnecting BLE controllers sleep with the console without touching bonds or admission | 🔵 Research concluded: no safe generic host-only path; controller-specific evidence required |
| P3 | Console-native report `0x09` motion semantics | 🔴 Blocked on better primary evidence |
| P3 | NFC/amiibo transactions | 🔴 Blocked on a genuine console-side capture |

## Validation

Current automated coverage includes:

- DualSense Bluetooth output report layout and CRC
- Switch 2 player-LED command-mask decoding
- Xbox rumble payload construction and STOP semantics
- Genuine-capture NSO GameCube rumble decoding
- GameCube and Joy-Con 2 input report encoders
- Joy-Con 2 per-side identity and configurable accent placement
- HID output normalization
- Retained UART protocol-trace disabled mode, payload truncation, chronological wraparound, and
  overwrite accounting
- Switch 2 pairing cryptography
- Switch 2 wake identity parsing and byte-exact advertisement construction
- Automatic wake policy across reconnect startup state, per-controller session cleanup, repeated
  held reports, BOOTSEL triple-tap maintenance suppression, and Switch 1 Pro initialization
  quarantine
- USB personality cycling
- BOOTSEL gesture timing under timer-only, report-only starvation, and mixed scheduling
- Late BLE identity correction, including provisional and generic binding, transport filtering,
  idempotent confirmation, and input notifications immediately before and after a driver rebind
- Battery decoding for DualShock 3/4, DualSense, Switch Pro, Wii U Pro, and Wiimote; recurring BLE
  BAS updates with native-HID priority; and power-field encoding for Switch 1 Pro, Pro Controller 2,
  NSO GameCube, and both Joy-Con 2 personalities
- First-generation 8BitDo Ultimate Bluetooth identity gating and P1/P2 signature conversion to
  L4/R4 (GL/GR by default), including simultaneous and ordinary-input preservation
- 8BitDo NGC Modkit rumble report framing, per-profile VID authorization, and send-result
  propagation
- `gcusb` safety and protocol helpers
- Pro Controller 2 UAC1 descriptor topology, advertised Feature Units, both 192-byte isochronous
  streams, RP2040/RP2350 allocation path, and full mute/volume request surface
- DualSense audio report `0x39`/`0x32` byte layout and CRC, physical-jack parsing,
  Nintendo headset-state encoding, exact reconnect transport selection, native-haptic
  start/STOP lifecycle, interval peak preservation, plus the fixed 512-to-480 stereo
  resampler's constant-signal, channel-isolation, and ramp behavior

The firmware builds under the Pico SDK 2.2.0 toolchain. The standard `pico_w`
artifact retains its validated non-audio clock, memory layout, and Bluetooth
scheduling. The standard `pico2_w` artifact uses the hardware-confirmed
floating-point/SRAM audio path at 300 MHz/1.20 V. Both legacy `NS2_PRO=OFF`
Pico W build directories also pass their compile gates. The current release has 28
passing host-test executables, including battery decoder/source/encoder, DualSense
audio packet/control/tone/resampler, native-haptic lifecycle, peak preservation, and
bonded-reconnect transport suites.

Config v8 stores a Pro Controller 2 body color plus independent Joy-Con 2 Left/Right accent colors.
Existing v5/v6 users retain their effective slot-0 color and remap/wake data; v7 body, remap, and
wake fields migrate intact. Joy-Con accents default to genuine retail values (`9B E1 E6` Left,
`FF 8C 5F` Right). Each personality advertises its configured appearance during enumeration, and
the active Pro2/Joy-Con color drives supported DualShock 4/DualSense lightbars independently of
player-indicator LEDs. Pro2 body rendering, Joy-Con accents, DualSense lightbar matching, live
player-dot reordering, and the prior wake/input/rumble baseline are hardware-confirmed.

## Documentation map

- [`docs/README.md`](docs/README.md) — documentation index and authority rules
- [`docs/status/compatibility-matrix.md`](docs/status/compatibility-matrix.md) — controller/personality validation
- [`docs/architecture/overview.md`](docs/architecture/overview.md) — runtime architecture and data flow
- [`docs/re-methodology/evidence-standards.md`](docs/re-methodology/evidence-standards.md) — evidence tiers and experiment rules
- [`docs/switch2/`](docs/switch2/) — Pro Controller 2 protocol
- [`docs/switch2-gc/`](docs/switch2-gc/) — NSO GameCube protocol and mapping
- [`docs/switch2-joycon2/`](docs/switch2-joycon2/) — Joy-Con 2 protocol and mapping
- [`docs/bluetooth/`](docs/bluetooth/) — Bluetooth host, identity, pairing, and controller profiles
- [`docs/experiments/`](docs/experiments/) — immutable experiment records and refuted hypotheses

## Next recommended work

1. Add DualSense microphone Opus decode and USB return.
2. Run an extended playback/thermal soak on the Pico 2 W 300 MHz build.
3. Add a reproducible release checklist with board, firmware revision, controller firmware,
   console firmware, and result data.
4. Build a reproducible console-side capture path before resuming NFC or report `0x09` motion work.
5. Revisit controller sleep only after capturing a verified per-family sleep command or a stable
   distinction between automatic-reconnect and user-wake advertisements.
