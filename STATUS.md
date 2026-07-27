# PicoSwitch2 Status

> Current-state snapshot. Historical implementation narratives are archived in
> [`docs/archive/status-through-2026-07-15.archived.md`](docs/archive/status-through-2026-07-15.archived.md).
> Planned work belongs in [`PLAN.md`](PLAN.md); evidence and protocol details belong under
> [`docs/`](docs/README.md).

Last verified: 2026-07-26 (Virtual Amiibo/configuration refactor automated validation; hardware pending)
Branch: `ns2-testing`

Documentation/resource audit: 2026-07-25

## Switch 1 Joy-Con / Pro Controller motion — 🟢 hardware-confirmed working 2026-07-27

Switch 1 controllers paired in Pro Controller 2 mode now stream 6-axis motion.
Implemented in `switch_pro_bt.c`: the `ENABLE_IMU` init step (subcommand `0x40`),
the three-frame IMU decode from report `0x30` bytes 13–48 with a per-axis mean
across the three 5 ms-apart frames, per-device axis signs, and
`SWITCH_MOTION_SOURCE_SWITCH1` provenance so `switch_pro2.c` routes it to the
validated quaternion translator rather than the known-bad generic phase encoder
(the generic encoder was the cause of the "spamming all over the place" first
seen on hardware — the raw data was clean, confirmed by live `input status`).

Per-unit SPI-flash calibration is now read too: subcommand `0x10` fetches the
factory block at `0x6020` and the user block at `0x8026`, the report-`0x21`
reply is parsed by `switch_parse_spi_reply()`, and the user block overrides the
factory one when its `B2 A1` magic is present. The §7.4 conversion is folded
into the interchange scale so the hot path stays integer-only. Calibration is
strictly an improvement, never a dependency: an absent, erased, or zero-span
block is rejected and the nominal §6 constants are used, so motion works from
the first report onward regardless.

The §6 nominal-vs-datasheet gyro-scale disagreement was settled by hardware in
favour of the LSM6DS3 `0.070` dps/count value; the nominal assumption
under-reported rate noticeably against a DualSense.

Still open: 🟡 Joy-Con L (`0x2006`) / R (`0x2007`) gyro signs are inherited from
the Pro and unverified — §8 says the halves mount the IMU mirrored, so at least
one axis is probably wrong on at least one half. See
[docs/bluetooth/switch1-motion.md](docs/bluetooth/switch1-motion.md) §10–§11.

## Wii Remote motion — 🟢 hardware-confirmed working 2026-07-27

Accelerometer + Wii MotionPlus gyro now stream through the existing motion
carrier and are confirmed working on hardware by the project owner.

Implemented: split 10-bit accelerometer assembly, MotionPlus detection
(`0xA600FA`), 32-byte calibration read at `0xA60020` **before** activation with
CRC32 verification, the MotionPlus init pair, activation via `0xA600FE` with the
passthrough mode chosen from the downstream extension, verification at
`0xA400FA` with retries, and per-frame decode honouring `is_mp_data`, the
cross-byte slow bits and per-axis slow/fast calibration blocks. The documented
rumble-latch bug is also fixed (every output report rewrites the motor latch).

No new motion representation was invented: the Wii publishes the same SInput
convention the Sony parsers use (`±32767 = ±2000 dps` / `±4 g`) in the DualSense
slot frame, which `ns2_seam.c` already remounts into the Pro2 frame. It carries
its own `SWITCH_MOTION_SOURCE_WII` provenance so future IMU-bearing controllers
have a place for per-family policy.

Still open: EEPROM accelerometer calibration (fallback constants in use),
passthrough bit-reversal for an extension behind an active MotionPlus, and
re-expressing orientation detection on the calibrated vector. See
[docs/bluetooth/wii-motion.md](docs/bluetooth/wii-motion.md) §12.5.

## Virtual amiibo — v3 (NTAG I2C Plus 2K / Kirby Air Riders) — 🔴 PARKED 2026-07-27

Full record: [`docs/Amiibo-v3.md`](docs/Amiibo-v3.md).

Working and kept in place: the 2048-byte v3 store with flash persistence, the
complete console read state machine with descriptor-driven page ranges, byte-exact
chunk framing (0 mismatches vs the source dump), and all 16 Kirby dumps verified
HMAC-valid against the owner's retail keys.

Blocked on one unknown: the console always requests the NTAG215 page set (540
bytes) and a v3 tag's encrypted region ends at 0x248 (584 bytes), so it can never
validate one. Every field a controller can influence has been eliminated with
evidence (status payload, read-buffer prefix, subcommand replies, capability
container, firmware version). Resume by capturing a genuine controller reading a
genuine v3 amiibo through `nfcmirror`.

Two real bugs were found and fixed along the way, both of which affected normal
use and not just v3:

- **Flash region collision (serious).** The amiibo journal bank 0 sat on BTstack's
  TLV region on RP2350 (pico-sdk 2.2.0 moves it one sector lower there), so writing
  a tag destroyed the Bluetooth bonds and BTstack destroyed the stored tag. Banks
  relocated; asserts now check `PICO_FLASH_BANK_STORAGE_OFFSET`.
- **v3 uploads were never durable.** `amiibo persist` gated on the 540 store's
  `loaded` flag, making it a silent no-op for v3, and the portal never called it.


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
The board stores exactly one amiibo; the flash "Save 1/Save 2" pair is that one identity's
baseline/latest-written recovery copies, not two amiibo. The production manager is a single-slot
layout: Load Amiibo (centered between the carousel arrows) stages the highlighted amiibo, Activate
Amiibo sends it to the adapter (showing a disabled "Amiibo activated" while already presented), and
Sync Amiibo pulls console-written data back into the validated browser copy. One merged eject/clear
button always labels its exact scope: "Eject Amiibo" removes the loaded adapter-held image and
unloads it after a save-confirmation prompt, "Clear Loaded Amiibo" only unloads, and "Eject Virtual
Amiibo" removes an adapter image that is not loaded here. Adapter-destructive modes discard the
stored image and both flash journal banks through `amiibo clear`; cancelling aborts everything, and
library dumps are never deleted. The console-driven Stop/write-back lifecycle is unchanged.

The Virtual Amiibo library is **import-only**: users supply their own genuine dumps (single file or
recursive directory). A 2026-07-26 hardware test showed the Switch 2 validates amiibo cryptography,
so key-free generated images are rejected ("This isn't an amiibo") even though the portal identifies
them, and a random-UID "Random Mode" was removed because a runtime UID swap invalidates the
UID-bound tag HMAC. A key-based generator (amiitool over Web Crypto, user-supplied `key_retail.bin`)
was prototyped and then **removed** in favor of import-only simplicity; the identity/crypto research
is retained in
[`docs/switch2/amiibo-identity-and-generation.md`](docs/switch2/amiibo-identity-and-generation.md)
and [`docs/experiments/generated-amiibo-console-rejection-2026-07-26.md`](docs/experiments/generated-amiibo-console-rejection-2026-07-26.md).
The library exports/imports as a flat **.zip** (`library.json` manifest + one `.bin` per amiibo;
legacy `.json` backups still import). Directory imports fill the visible library progressively. The
carousel navigates with clean, non-wrapping arrows (disabled at the ends) and shows the centered
amiibo's release date above it. The centered
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
Pico W build directories also pass their compile gates. The current workspace has 50
passing host-test executables, including battery decoder/source/encoder, DualSense
audio packet/control/tone/resampler, native-haptic lifecycle, peak preservation, and
bonded-reconnect transport suites, plus the virtual-tag store/codec, vendor transfer pump,
Config-only BLE cross-core bridge, locked base mapping, and the isolated NTAG I2C 2K
(figure v3) amiibo data model.

NTAG I2C 2K (Kirby Air Riders "figure v3") support is staged. The portal identifies and
imports these 2048-byte tags in full (see the amiibo section above). On the firmware side,
Phase 1 adds an isolated, host-tested v3 data model (`src/nfc/ns2_amiibo_v3.c`) — validation,
identity/UID, GET_VERSION, bounded reads — with no changes to the validated 540/572 NTAG215
store, flash journal, or NFC serve path. Serving a 2 KB tag to the console (Phases 2–3) is
hardware-gated on a UART trace of the console's read framing; the staged plan and flash
constraints are in
[`docs/switch2/kirby-air-riders-extended-amiibo.md`](docs/switch2/kirby-air-riders-extended-amiibo.md).

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
