# PicoSwitch2 Status

> Current-state snapshot. Historical implementation narratives are archived in
> [`docs/archive/status-through-2026-07-15.archived.md`](docs/archive/status-through-2026-07-15.archived.md).
> Planned work belongs in [`PLAN.md`](PLAN.md); evidence and protocol details belong under
> [`docs/`](docs/README.md).

Last verified: 2026-07-26 (Virtual Amiibo/configuration refactor automated validation; hardware pending)
Branch: `ns2-testing`

Documentation/resource audit: 2026-07-25

## Current release

[`v1.5.0`](https://github.com/notsosaelin/PicoSwitch2/releases/tag/v1.5.0) was published on
2026-07-22 with Pico W and Pico 2 W UF2 assets. All 35 host-test executables pass. This release adds
hardware-confirmed genuine Pro Controller 2 native-motion passthrough, UART protocol tracing,
current firmware identities, and bonded HOME reconnect through BTstack SM. Twenty consecutive
controller-off/HOME cycles restored input, P1 LED, and gyro without SYNC. Pico 2 W retains its
300 MHz live-audio/native-haptic build; Pico W retains its validated non-audio configuration.

The post-release `ns2-testing` branch also has hardware-confirmed genuine Pro Controller 2 headphone
output. Its 240-byte Opus/CELT framing now produces clean console audio while preserving input,
native gyro, rumble, headset insertion/removal, LED behavior, and BOOTSEL handling.

DualSense and DualSense Edge motion translation is also hardware-confirmed in Splatoon 3. The
production path emits the decoded length-`0x1E` Switch 2 quaternion carrier and preserves input,
audio, haptics, reconnect, LED, and BOOTSEL behavior. A deliberately gated synthetic length-`0x28`
experiment caused random motion and was removed; it established that the still-unknown
leading/middle fields cannot be held at a static genuine template.

The USB side of Config mode is now CDC-only in source and automated builds. The read-only MSC
drive, embedded FAT image/web page, callbacks, and generator were removed;
`tools/run_config_portal.ps1` serves the production portal locally. This removes 100,104 bytes
from the Pico 2 W binary and 100,160 bytes from the Pico W binary. Config enumeration, Virtual
Amiibo transfer, save/readback, and direct BOOTSEL exit are hardware-confirmed with the CDC-only
USB descriptor.

The same local portal now also has a Config-personality-only BLE management transport. It stops
controller discovery before advertising, classifies the incoming peripheral-role link before the
HID path, and executes an allowlisted production command set through the existing core-0 parser.
Normal controller personalities do not advertise, accept writes, notify, poll, or open a
management link. All build and host/static checks pass; Bluetooth hardware validation is pending.

Virtual Amiibo is now always available rather than controlled by a stored toggle. A blank adapter
presents no virtual tag and can still fall through to a real reader source. Each browser profile
keeps its own user-supplied, AmiiboAPI-ordered library as one mutable record per exact catalog ID;
neither firmware nor the site ships tag images. Two independent browser quick slots can reference
different library amiibo. A newly flashed UF2
performs a one-shot erase of all five PicoSwitch2 persistence sectors, clearing settings, both
virtual-tag banks, wake identity, and Bluetooth bonds. Ordinary power cycles retain state.
The production manager now follows the definitive staged-slot layout: its explicit Slot 1/Slot 2
switch chooses a quick slot, Assign updates that slot, Clear empties the slot pointer without
deleting the dump or touching the adapter, Load sends the slot to the adapter, Sync Amiibo
pulls console-written data back into the validated browser copy, and Eject stops presentation
without deleting the dump. Directory imports fill the visible library progressively. The centered
artwork is fixed in the middle at 100% size; four non-overlapping neighbors on each side use exact
80/60/40/20% scaling. Movement is animated, names are omitted from the carousel, and
game-series, amiibo-series, and product-type arrows cycle `All` followed by the imported library's
available values alphabetically without changing AmiiboAPI source order. The new presentation edge
is compiled and host/static-regression clean; real-console manual Eject/re-present validation is
pending.

Controller-family **button** remap persistence and its portal UI have been removed. Firmware uses
one locked, regression-tested physical-to-Nintendo base map; user button remapping belongs to the
emulated controller in the Switch UI and therefore survives a change of source controller.
Controller appearance is intentionally retained: the portal exposes the shared Pro2 body/Sony
lightbar color and independent Joy-Con 2 Left/Right accents. Config schema v10 stores appearance
and wake identity only. The production page presents these three colors in one compact panel and
no longer shows the obsolete current-input/current-output identity cards. BLE GAP and Config
advertisement names are both `PicoSwitch2`.

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
| Joy-Con 2 Bluetooth mouse bridge | ✅ Confirmed | Mouse-only feature gating, pointer activation/motion, buttons, disconnect cleanup, and wheel-to-stick menu navigation |
| Switch 2 controller firmware identity/update status | ✅ Confirmed | Genuine `0x10/01` replies plus Switch 2 Update Controllers; Pro2, NSO GC, and both Joy-Con 2 personalities report up to date |
| Out-of-band UART protocol tracer | ✅ Confirmed | Real Switch 2 + genuine Pro Controller 2 source; complete 63-record Pro2 re-enumeration capture, zero overwrites, pull-transport framing validated |
| UART trace decoder and semantic differ | ✅ Host + live-capture confirmed | Known EP0/bulk/HID fields, sensitive-field redaction, strict comparison, timestamp wrap, corruption rejection, and two-capture Pro2 A/B workflow |
| Genuine Pro Controller 2 native motion passthrough | ✅ Confirmed | Splatoon 3 axes/aim, stationary behavior, power-off hold, and 20 consecutive controller-off/HOME reconnect cycles without SYNC; input, P1 LED, and native `0x1E`/`0x28` motion restore at 133 Hz |
| DualSense/Edge → Switch 2 motion translation | ✅ Confirmed | Splatoon 3 direction, scale, rapid movement, stationary behavior, reconnect recovery, and coexistence with input/audio/haptics using the length-`0x1E` carrier |
| DualSense and DualSense Edge input | ✅ Confirmed | Real Switch 2 and Steam |
| Edge paddles, Fn buttons, and mute mapping | ✅ Confirmed | Real hardware |
| DualSense/Edge LEDs and rumble | ✅ Confirmed | Real hardware after report-boundary scheduler fix |
| Pro2 body/Joy-Con accents, Sony lightbar matching, and DualSense player-slot dots | ✅ Confirmed | Real Switch 2 and DualSense; config v8 hardware pass |
| BOOTSEL report-boundary scheduling and former double/triple/hold policy | ✅ Confirmed | Real hardware after report-boundary gesture service |
| Revised single/double/triple/two-second BOOTSEL action matrix | 🟡 Host/build confirmed; hardware pending | Pure gesture/action policy coverage; both board builds |
| Config-only BLE management transport | 🟡 Host/build confirmed; hardware pending | Shared USB/BLE command parser, bounded cross-core bridge, production-command allowlist, and local Web Bluetooth portal |
| Virtual Amiibo persistence, mutable library, and two quick slots | 🟡 Prior lifecycle hardware-confirmed; refactor hardware pending | Write-before-eject journal is unchanged; exact AmiiboAPI validation, one-dump writeback, slot switching, reset-on-UF2, and Config BLE require regression validation |
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
| Genuine Pro Controller 2 headphone audio — Pico 2 W | ✅ Confirmed | Clean Switch 2 console audio through the physical jack; input, gyro, rumble, headset lifecycle, LED, and BOOTSEL regression checks pass |
| DualSense Bluetooth internal-speaker audio — Pico 2 W | ✅ Confirmed | Standard 300 MHz build; 13,225/13,225 PCM blocks encoded, zero drops/errors |
| DualSense Bluetooth internal-speaker audio — Pico W | ❌ Not supported | Fixed-point/XIP 300 MHz experiment barely played audio; standard build restored to validated non-audio profile |
| Standard 300 MHz Pico 2 W platform regression | ✅ Confirmed | LED/BOOTSEL, config persistence/readback, cold boot, and ten wake attempts per known controller |
| Standard 300 MHz Pico 2 W extended stability soak | ✅ Confirmed | Eight-hour Smash session with no observed thermal or stability issue; temperature was not instrumented |
| DualSense audio after bonded reconnect | ✅ Confirmed | Controller and dongle power cycles restore audio and native rumble through the saved bond; no fresh pair required |
| Switch 2 headset insertion and output | ✅ Confirmed | Physical DualSense jack is recognized; console audio plays through connected headphones with input/rumble/wake intact |
| Switch 2 headset removal/reinsert | ✅ Confirmed | Repeated cycles restore input, audio, and native haptics; unplugged full legacy rumble remains stable |
| DualSense rumble during console audio | ✅ Confirmed | Native-mode restoration, capture-derived peak preservation, and the waveform-preserving 3.25× curve are stable and judged close to HD Rumble |
| DualSense rumble without headset/audio | ✅ Confirmed | Pico 2 W reuses the native renderer with valid Opus silence only during active rumble plus a bounded two-packet STOP tail; Pico W retains compatibility rumble |
| Pico W and Pico 2 W builds | ✅ Compile-confirmed | Pico W uses the validated non-audio profile; Pico 2 W includes live audio at 300 MHz |

## Current USB personalities

Every boot starts in Pro Controller 2 mode. With a controller HID-ready, a single BOOTSEL tap
advances the volatile controller-only cycle:

1. Switch 2 Pro Controller 2 (`057E:2069`)
2. NSO GameCube Controller (`057E:2073`)
3. Joy-Con 2 Left (`057E:2067`, experimental)
4. Joy-Con 2 Right (`057E:2066`, experimental)
5. Back to Pro Controller 2

A two-second hold enters CDC/configuration mode (`CAFE:4012`) directly from any controller
personality; a two-second hold in Config returns directly to Pro2. Config is never part of the
single-tap cycle. The selection is not persisted across power cycles.

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
  radio contention noted in
  [`docs/switch2/audio-passthrough-research.md`](docs/switch2/audio-passthrough-research.md).
- Config management is a separate BLE Peripheral role and is armed only by the explicit Config USB
  personality. Entering Config stops controller discovery before advertising; leaving Config
  disconnects the browser before discovery resumes. The normal controller path performs only a
  mode-state comparison and generates no management radio traffic.
- Switch 2 controllers use a custom ATT pairing handshake, so the wipe policy cannot depend only on
  BTstack's LE bond database.
- Successful custom pairing persists the normalized LTK in both the reconnect record and BTstack's
  LE database with RAND/EDIV zero. HOME reconnect must run through `sm_request_pairing()` so
  BTstack restores its bonded security state; issuing raw HCI encryption alone encrypts the ACL but
  leaves the controller in its running-LED/pre-active state. After SM success the dongle restores
  ACK/input CCCs, reasserts P1, and reruns the validated native-motion feature sequence.
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
| P3 | Additional controller IMUs → console-native report `0x09` translation | 🔵 Native Pro2 passthrough and DualSense/Edge synthesis are confirmed; each remaining family needs verified calibration, axis, scale, and timestamp handling |
| P3 | NFC/amiibo transactions | 🟡 Genuine Pro2 physical-tag reads and the complete Virtual Amiibo read/write/persist/eject/re-present/library workflow are hardware-confirmed. Native physical writes, production native-reader gating, and Switch 1 translation remain open |

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
- Native Pro2 motion snapshot validation for length-30/length-40 packets, source-slot ownership,
  freshness and timer wrap, malformed input, disconnect hold, and clear semantics
- DualSense calibrated motion translation, smallest-three quaternion encoding, carrier boundaries,
  timing/bias handling, and exact length-`0x28` G6/G7/G8 field packing
- UART trace JSONL validation, known-field decoding, default sensitive-data redaction, timestamp
  rollover, address-aware semantic alignment, and strict raw-prefix comparison
- Switch 2 pairing cryptography
- Switch 2 wake identity parsing and byte-exact advertisement construction
- Automatic wake policy across reconnect startup state, per-controller session cleanup, repeated
  held reports, BOOTSEL triple-tap maintenance suppression, and Switch 1 Pro initialization
  quarantine
- USB personality cycling
- BOOTSEL paired/unpaired/Config action policy, including the controller-only single-tap cycle,
  bond-preserving paired double-tap handoff, triple-tap wipe, and two-second Config toggle
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
- Virtual amiibo 540/572-byte validation and transactional upload, exact export, dirty-state
  protection, a 61-byte status codec, the primary-capture-confirmed 600-byte reader buffer and
  70-byte offset chunks, a 64-byte write-preparation buffer, exact-UID write selection, atomic
  454-byte staged-write validation, generation-safe RAM commit, and modulo-eight NFC events
- Virtual Amiibo internal baseline/latest-write recovery, automatic console-write persistence
  request, deferred removal until persistence, version-1 migration, and
  alternating version-2 flash-bank CRC verification
- UART-gated genuine Pro Controller 2 NFC relay: extended `0x0016` command framing, asynchronous
  response translation, report-state passthrough, bounded timeout handling, and one
  hardware-confirmed physical amiibo read recognized by the Switch 2
- Loaded-tag-gated Virtual Amiibo runtime using the same 600-byte/chunk model, hardware-confirmed with
  an uploaded tag and a non-NFC source controller; the guarded transactional write completes on a
  real console without crashing, including complete 88-byte chunks, commit, and `05 00`. Logical
  post-write removal, next-scan re-presentation, same-session updated readback, and generation-safe
  UART export of a genuinely mutated 540-byte image are hardware-confirmed.
- Console vendor-OUT stream reassembly for the 88-byte `0x14` write command, including exact
  64+24-byte split reproduction, arbitrary fragmentation, coalesced commands, oversized-command
  discard/recovery, and USB-mount reset
- Live UART Virtual Amiibo export with generation-stable 64-byte pulls, PC-side exact-length and
  UID/BCC validation, and dirty acknowledgement only after the binary is safely written
- Config-portal recursive directory scanning and browser-local IndexedDB caching for all 1,035
  validated maintainer collection files; selected-tag identity/catalog display and cache-first
  replacement of console-written save data
- Offline production-library access, exact catalog-ID deduplication, two independent quick slots,
  and versioned full-library export/import backup preserving each mutable dump
- Standalone no-serial Virtual Amiibo diagnostic page with a separate simulated adapter slot,
  transactional chunk/CRC checks, controlled write injection, cache-first save-back, persistence,
  known-ID AmiiboAPI verification, and browser self-test
- Production and diagnostic amiibo libraries now use artwork carousels. The production carousel
  displays only imported owned files, fills during directory scanning, centers enlarged selected
  artwork with four progressively smaller neighbors on each side, animates navigation, preserves
  AmiiboAPI order, and filters by the imported library's game/amiibo series and product types
- Config mode links as CDC-only with a compile-time-checked descriptor and no MSC/web-disk symbols;
  both local portals pass JavaScript, DOM-reference, and localhost delivery checks
- Config-only BLE command transport with fragmented-write assembly, one-command backpressure,
  response chunking, session invalidation, stale-response rejection, and production-command
  allowlisting; the browser uses the same settings/Amiibo UI over Web Serial or Web Bluetooth

The firmware builds under the Pico SDK 2.2.0 toolchain. The standard `pico_w`
artifact retains its validated non-audio clock, memory layout, and Bluetooth
scheduling. The standard `pico2_w` artifact uses the hardware-confirmed
floating-point/SRAM audio path at 300 MHz/1.20 V. Both legacy `NS2_PRO=OFF`
Pico W build directories also pass their compile gates. The current workspace has 49
passing host-test executables, including battery decoder/source/encoder, DualSense
audio packet/control/tone/resampler, native-haptic lifecycle, peak preservation, and
bonded-reconnect transport suites, plus the virtual-tag store/codec, vendor transfer pump,
Config-only BLE cross-core bridge, and locked base mapping.

Config v10 stores only the Pro Controller 2 body color, independent Joy-Con 2 Left/Right accents,
and learned wake identity. Every newly flashed UF2 starts from defaults, a blank virtual-tag
store, and no Bluetooth bonds; this is intentionally different from an ordinary reboot. Joy-Con
accents default to genuine retail values (`9B E1 E6` Left, `FF 8C 5F` Right). Each personality
advertises its configured appearance during enumeration, and the active Pro2/Joy-Con color drives
supported DualShock 4/DualSense lightbars independently of player-indicator LEDs. The locked base
button map replaces the retired per-family remap table.

## Documentation map

- [`docs/README.md`](docs/README.md) — documentation index and authority rules
- [`docs/status/compatibility-matrix.md`](docs/status/compatibility-matrix.md) — controller/personality validation
- [`docs/architecture/overview.md`](docs/architecture/overview.md) — runtime architecture and data flow
- [`docs/architecture/config-transports.md`](docs/architecture/config-transports.md) — USB Serial and Config-only BLE management
- [`docs/re-methodology/evidence-standards.md`](docs/re-methodology/evidence-standards.md) — evidence tiers and experiment rules
- [`docs/switch2/`](docs/switch2/) — Pro Controller 2 protocol
- [`docs/switch2-gc/`](docs/switch2-gc/) — NSO GameCube protocol and mapping
- [`docs/switch2-joycon2/`](docs/switch2-joycon2/) — Joy-Con 2 protocol and mapping
- [`docs/bluetooth/`](docs/bluetooth/) — Bluetooth host, identity, pairing, and controller profiles
- [`docs/experiments/`](docs/experiments/) — immutable experiment records and refuted hypotheses

## Next recommended work

1. Add DualSense microphone Opus decode and USB return.
2. Investigate why the current BLE-native motion bridge requires the 1 ms Pro2 USB interval before
   attempting any future 4 ms fidelity restoration; the isolated 4 ms hardware test killed gyro.
3. Add a reproducible release checklist with board, firmware revision, controller firmware,
   console firmware, and result data.
4. Capture a genuine Pro2 physical-tag write/readback before enabling native writes.
5. Revisit controller sleep only after capturing a verified per-family sleep command or a stable
   distinction between automatic-reconnect and user-wake advertisements.
