# PicoSwitch2 Status

> Current-state snapshot. Historical implementation narratives are archived in
> [`docs/archive/status-through-2026-07-15.archived.md`](docs/archive/status-through-2026-07-15.archived.md).
> Planned work belongs in [`PLAN.md`](PLAN.md); evidence and protocol details belong under
> [`docs/`](docs/README.md).

Last verified: 2026-07-28 (all 16 available figure-v3 dumps read and wrote successfully)
Branch: `ns2-testing`

Documentation/resource audit: 2026-07-25

## Switch 1 Joy-Con / Pro Controller motion — 🟢 at parity with genuine hardware 2026-07-27

**A/B against a natively-connected Switch 1 Pro Controller: 98–100 % identical.** The small
residual lag is present on the native connection too, so it belongs to the controller (120 Hz
reports, no sensor timestamp), not to this firmware. Full write-up:
[docs/bluetooth/switch1-motion.md](docs/bluetooth/switch1-motion.md) §10.2–§10.3.

The axis map is `src {1,0,2}`, `sign {-1,1,1}` in `ns2_motion_seam.c`. It was resolved by
measurement, not iteration: reading a *resting* controller's accelerometer over UART put gravity on
slot 2 at +4245 against a genuine Pro Controller 2's 4279/4309 — a 1 % match — which pinned that
lane with nobody touching the controller.

The bug four sign guesses had missed was that the row had **determinant −1**: a reflection, which
cannot describe a physical sensor remount. Gravity cannot detect a reflected frame — a single
vector looks correct reflected — so the accelerometer matched genuine hardware while the gyro
produced no horizontal aim at all. `tools/test_ns2_motion_seam.c` now enforces determinant +1 on
every row and is verified against the shipped bug.

Gyro also switched from the three-frame mean to the newest frame, removing ~7.5 ms of group delay
(the frames span 15 ms while the Pro reports every 8.3 ms). Accel keeps the mean — it is the
console's gravity reference, where steadiness beats latency.

Implemented in `switch_pro_bt.c`: the `ENABLE_IMU` init step (subcommand `0x40`),
the three-frame IMU decode from report `0x30` bytes 13–48, and
`SWITCH_MOTION_SOURCE_SWITCH1` provenance so `switch_pro2.c` routes it to the
validated quaternion translator rather than the known-bad generic phase encoder
(that encoder was the cause of the "spamming all over the place" first seen on
hardware — the raw data was clean, confirmed by live `input status`).

Per-unit SPI-flash calibration is read too: subcommand `0x10` fetches the factory
block at `0x6020` and the user block at `0x8026`, the report-`0x21` reply is
parsed by `switch_parse_spi_reply()`, and the user block overrides the factory
one when its `B2 A1` magic is present. Calibration is strictly an improvement,
never a dependency: an absent, erased, or zero-span block is rejected and the
nominal §6 constants are used, so motion works from the first report regardless.

The §6 nominal-vs-datasheet gyro-scale disagreement was settled by hardware in
favour of the LSM6DS3 `0.070` dps/count value; the nominal assumption
under-reported rate noticeably against a DualSense.

Still open: 🟡 Joy-Con L (`0x2006`) / R (`0x2007`) share the Pro's seam row and
are unverified — §8 says the halves mount the IMU mirrored, so at least one axis
is likely wrong on at least one half. Note the halves differ from the Pro by a
*proper* rotation (Linux negates Y and Z on both sensors for the right half), so
any correction must keep determinant +1.

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

## Virtual amiibo — v3 (NTAG I2C Plus 2K / Kirby Air Riders) — 🟡 IN PROGRESS 2026-07-28

Full record: [`docs/Amiibo-v3.md`](docs/Amiibo-v3.md) §19.

The complete 2048-byte read/write path is hardware-confirmed with both the capture-rebuilt baseline
and an untouched downloaded `Kirby & Warp Star.bin`. It includes descriptor-driven page ranges, the
v3-only `0x14`/`0x21` device command, and an 83-byte `0x18` result formed from a 19-byte controller
header plus the dump's complete 64-byte SRAM response. The last two SRAM bytes are the
CRC-16/MCRF4XX over the preceding 62 bytes; they are per response (`7A C4` for the captured figure,
`E5 11` for the downloaded Kirby/Warp, `30 61` for Meta/Shadow), not a fixed controller trailer.

The untouched dump worked with no signature override and no key-based transformation, refuting the
signature/carrier theories. Its trace contains three correct full-SRAM results, six write chunks,
one `0x08` commit, `05 00`, and zero write errors. The persisted export remains HMAC-valid with the
console-written nickname/owner and unchanged SRAM. Capture:
[`docs/experiments/v3-full-sram-response-validation-2026-07-28.md`](docs/experiments/v3-full-sram-response-validation-2026-07-28.md).

Owner/format write, Stop/eject, next-scan updated readback, and power-cycle recovery are
hardware-confirmed. The one remaining lifecycle check is production-portal Sync of the intentionally
retained dirty generation, followed by firmware acknowledgement only after IndexedDB succeeds.

The genuine Pro Controller 2/physical Kirby & Warp Star positive control is now captured. It proves
that Air Riders uses two sector-aware `0x20` envelopes, not one fixed 355-byte no-op: a 355-byte
two-record clear and a 167-byte three-record update. Genuine completion reports empty state
`0x16`, then the console performs a selected-UID page-3 read and an ordinary 454-byte/`0x08`
write; only that later commit reports `05 00` and ejects on Stop. The post-write physical snapshot
confirms the second envelope's 32 bytes land at sector-0 pages `0x92..0x99`; its third record
directly addresses sector-1 pages `0x01..0x18`.

The firmware validates/applies both record layouts, generation-checks and journals each `0x20`
stage without ejecting, reports genuine `0x16`, and preserves the proven ordinary-write lifecycle.
The complete two-stage Air Riders write is hardware-confirmed: 18 ordinary chunks, three `0x08`
commits, eight extended chunks, two `0x20` completions, and zero write errors. The persisted
2048-byte export remains HMAC-valid, SRAM-valid, and contains both the sector-0 and sector-1 game
records.

Trying to reuse that written tag exposed one final read command. The console sends sector-aware
subcommand `0x1E`; the old virtual path bare-ACKed it but left state at `0x18`, causing repeated
three-second Stop/restart loops. A genuine Pro Controller 2 capture proves the ACK itself was
correct: genuine hardware stages a 196-byte result, changes status to empty `0x15`, signals one
report-state edge, and serves the result through three ordinary `0x15` chunks. The result is a
64-byte identity/signature/descriptor prefix plus sector-0 pages `0x92..0x99` and sector-1 pages
`0x00..0x18`; the chip-managed sector-1 capability page was `A5 00 01 00` after the first
Air Riders update even though ecosystem dumps leave that hardware slot zero.

That `0x1E` implementation is now hardware-confirmed: the console observed empty `0x15`, fetched
all 196 bytes in three chunks, and sent Stop. It then began another legitimate 167-byte update.
The first classifier correction let that subsequent 167-byte update and ordinary checkpoint
complete, persist, and export with zero write errors. Reusing the result then produced
“This amiibo is corrupted” during `0x1E`, before another write.

A genuine Pro Controller 2 positive control completed the same full read/write cycle, then read
the physical tag again without writing. A second full write and read-only control repeated the
transition. The captures prove that the envelope field is not page 4: it is the **next sector-1
page-0 value**. Genuine hardware advanced that implicit chip state
`A5 00 01 00 → A5 00 02 00 → A5 00 03 00`, while sector-0 page 4 independently advanced
`03 → 04 → 05` and every explicit sector-1 record began at page 1. The virtual path discarded
the field and continued serving `A5 00 01 00`, which is the only mismatched extended-read state.

The correction validates the next generation, stores the four-byte capability state in
the otherwise zero ecosystem-dump slot at `0x400`, persists/exports it with the 2048-byte image,
and serves the retained value through `0x1E`; zero-filled first-use/legacy images retain the
hardware-confirmed generation-1 fallback. Hardware then completed the virtual update from
`A5 00 01 00` to `A5 00 02 00`; the saved image exported with page 4 `A5 00 03 00`, sector-1
page 0 `A5 00 02 00`, valid amiibo HMACs, and the retained nickname/owner. Its immediate second
reuse was accepted by Air Riders and loaded the previously saved custom color. The successful
read trace returned the stored `A5 00 02 00` through `0x1E`. Both board builds, all 53 host tests,
all eight magnetometer tests, and both install-reset marker checks pass. A physical adapter
power cycle then restored the exact generation-4 image/CRC from flash, served the retained
`A5 00 02 00` through `0x1E`, and Air Riders accepted and loaded it without another write.
The dynamic sector-1 page-0 lifecycle is hardware-confirmed. A later save after completing an
Air Riders level validates non-cosmetic learned gameplay state: it used the same 167-byte extended
update plus ordinary six-chunk commit, advanced page 4 `05 → 06` and sector-1 page 0 `03 → 04`,
changed only the modeled writable ranges through `0x463`, and exported HMAC-valid with zero
write errors. See
[`docs/experiments/v3-air-riders-extended-operation-2026-07-28.md`](docs/experiments/v3-air-riders-extended-operation-2026-07-28.md).

King Dedede & Tank Star exposed allocation-relative Air Riders storage. Its update uses sector-0
page `0xB2` plus sector-1 capability/data pages `0x64/0x65`; Kirby uses `0x92` plus `0x00/0x01`.
The first correction accepted all three Dedede chunks, but the fixed Kirby record table rejected
both completions and returned error state `07 41`/`2115-0096`. The prepared codec now derives both
allocations from the envelope, validates them against the proven cleared/user-memory bounds, tracks
generation at the selected capability page, and makes `0x1E` fallback descriptor-relative. There
is no figure/UID whitelist. The portal Initialize path clears the complete second user-memory
sector for the same reason. The maintainer then flashed every one of the 16 available Air Riders
v3 dumps; all 16 completed both real-console reads and writes. This validates the generalized
allocation path across the complete available dump set rather than only the original Kirby
capture. All 53 host tests, both board builds, portal suites, motion checks, magnetometer checks,
and reset markers also pass. See
[`docs/experiments/v3-air-riders-dynamic-allocation-2026-07-28.md`](docs/experiments/v3-air-riders-dynamic-allocation-2026-07-28.md).

Retracted: the earlier claim that the read prefix carried a *dynamic* SRAM window
alternating between two values. It is constant across all 11 genuine reads; the
second value belonged to the `0x21` result buffer and was misattributed.

New instrument: `nfcmirror` gained an **initiator** mode, so UART can originate NFC
commands at a genuine controller with no console attached
(`tools/nfc_probe.ps1`). This dumps real tags — including v3, which nothing else
here can read — and turns per-question console captures into bench measurements.

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
keeps its own user-supplied library as locally validated mutable records; AmiiboAPI supplies
optional ordering/details, and v3 rider/machine variants use content-derived keys. Neither
firmware nor the site ships tag images.
One browser-local loaded-slot pointer tracks the selected adapter image. A newly flashed UF2
performs a one-shot erase of all five PicoSwitch2 persistence sectors, clearing settings, both
virtual-tag banks, wake identity, and Bluetooth bonds. Ordinary power cycles retain state.
The board stores exactly one amiibo; the alternating flash banks are persistence generations, not
two active amiibo. The production manager is a single-slot layout: connected Load amiibo sends the
highlighted entry to the adapter, offline Select amiibo remembers it, and Sync amiibo pulls
console-written data back into the validated browser copy. Load/Select and Import/Sync occupy one
context-sensitive center action. On connection the portal selects the adapter's active cached entry
once, including a dirty same-UID v3 image, without forcing the carousel back on later status polls.
One merged button is uniformly labeled
"Eject amiibo"; its tooltip, confirmation, and `amiiboEjectActionState()` scope determine whether
it removes only the loaded pointer, wipes the adapter, or does both. Adapter-destructive modes
discard the stored image and both flash journal banks through `amiibo clear`; cancelling aborts
everything, and library dumps are never deleted. The console-driven Stop/write-back lifecycle is
unchanged.

The Virtual Amiibo library is **import-only**: users supply their own genuine dumps (single file or
recursive directory). A 2026-07-26 hardware test showed the Switch 2 validates amiibo cryptography,
so key-free generated images are rejected ("This isn't an amiibo") even though the portal identifies
them, and a random-UID "Random Mode" was removed because a runtime UID swap invalidates the
UID-bound tag HMAC. A key-based identity generator was removed, but the smaller browser-local
rewrite path remains for explicit Initialize on an imported dump: it requests the user's own
`key_retail.bin`, clears and re-signs ordinary or v3 save state, and self-verifies before replacing
the IndexedDB copy. It works without an adapter and does not create catalog identities. Research is
retained in
[`docs/switch2/amiibo-identity-and-generation.md`](docs/switch2/amiibo-identity-and-generation.md)
and [`docs/experiments/generated-amiibo-console-rejection-2026-07-26.md`](docs/experiments/generated-amiibo-console-rejection-2026-07-26.md).
The library exports/imports as a flat **.zip** (`library.json` manifest + one `.bin` per amiibo;
legacy `.json` backups still import). Directory imports fill the visible library progressively. The
carousel wraps deliberately at the ends while keeping the visible neighbor window non-wrapping,
and shows the centered amiibo's release date above it. The centered
artwork is fixed in the middle at 100% size; four non-overlapping neighbors on each side use exact
80/60/40/20% scaling. Movement is animated, names are omitted from the carousel, mouse-wheel,
touch-swipe, and keyboard-arrow navigation are supported, and the redundant visible arrows/count
are removed. Game-series, amiibo-series, and product-type chips cycle `All` followed by the
imported library's available values alphabetically without changing AmiiboAPI source order and sit
together below the compact primary action. Search stays in the compact header toolbar. Clicking
the centered artwork toggles a non-modal context drawer for
compatibility metadata and secondary/destructive actions; the center button alone owns
Load/Select/Import/Sync. Physical-tag scanning remains hidden behind a future firmware capability
rather than presenting a dead production control. The new presentation edge is
host/static-regression clean; real-console manual Eject/re-present validation is pending.

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
| Virtual Amiibo persistence and mutable single-slot library | 🟡 540 and v3 read/write/persistence hardware-confirmed; portal refactor pending | All 16 available v3 dumps completed real-console reads/writes; v3 Config Sync, reset-on-UF2, and Config BLE still require regression validation |
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
- Offline production-library access, exact catalog/content deduplication, one loaded-slot pointer,
  and versioned full-library export/import backup preserving each mutable dump
- Standalone no-serial Virtual Amiibo diagnostic page with a separate simulated adapter slot,
  transactional chunk/CRC checks, controlled write injection, cache-first save-back, persistence,
  known-ID AmiiboAPI verification, and browser self-test
- Production and diagnostic amiibo libraries now use artwork carousels. The production carousel
  displays only imported owned files, fills during directory scanning, centers enlarged selected
  artwork with four progressively smaller neighbors on each side, animates navigation, preserves
  AmiiboAPI order, and filters by tap-to-cycle game/amiibo-series and product-type chips. The
  production manager now uses one context-aware center action, compact search, active-tag
  auto-selection on connection, save metadata below the artwork, and a non-modal details drawer
- Config mode links as CDC-only with a compile-time-checked descriptor and no MSC/web-disk symbols;
  both local portals pass JavaScript, DOM-reference, and localhost delivery checks
- Config-only BLE command transport with fragmented-write assembly, one-command backpressure,
  response chunking, session invalidation, stale-response rejection, and production-command
  allowlisting; the browser uses the same settings/Amiibo UI over Web Serial or Web Bluetooth

The firmware builds under the Pico SDK 2.2.0 toolchain. The standard `pico_w`
artifact retains its validated non-audio clock, memory layout, and Bluetooth
scheduling. The standard `pico2_w` artifact uses the hardware-confirmed
floating-point/SRAM audio path at 300 MHz/1.20 V. Both legacy `NS2_PRO=OFF`
Pico W build directories also pass their compile gates. The current workspace has 53
passing host-test executables, including battery decoder/source/encoder, DualSense
audio packet/control/tone/resampler, native-haptic lifecycle, peak preservation, and
bonded-reconnect transport suites, plus the virtual-tag store/codec, vendor transfer pump,
Config-only BLE cross-core bridge, locked base mapping, the NTAG I2C 2K data model, and its
capture-derived staged-write codec.

NTAG I2C 2K (Kirby Air Riders "figure v3") support is active. The portal imports, loads, and can
read/sync the complete 2048-byte image. The console read path is hardware-recognized; the isolated
write path, dirty/readback status, and power-safe journal integration pass host and board builds
but await the real-console write lifecycle described in
[`docs/Amiibo-v3.md`](docs/Amiibo-v3.md) §18.3.

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
