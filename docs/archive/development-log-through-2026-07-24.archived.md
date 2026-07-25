# Development Log Through 2026-07-24 (Archived)

Reverse-chronological log of notable changes on the `ns2-testing` branch. The
2026-07-21 and 2026-07-22 entries below are included in
[`v1.5.0`](https://github.com/notsosaelin/PicoSwitch2/releases/tag/v1.5.0).

## Unreleased

### Fixed

- **Genuine Pro Controller 2 headphone audio is hardware-confirmed.** Each 20 ms interval is one
  240-byte, 48 kHz stereo Opus/CELT packet at 96 kbit/s, divided only for GATT transport into an
  ordered 120-byte `0x04` first half and 120-byte `0x02` continuation. The earlier independent-frame
  interpretation produced recognizable but severely distorted audio. The corrected direct-CELT
  encoder, fixed-idle synchronization, and commit-after-both-writes scheduling produce clean console
  audio through the controller's headphone jack while preserving input, native gyro, rumble,
  headset insertion/removal, LED behavior, and BOOTSEL handling.

### Validation

- Pico W and Pico 2 W firmware targets compile, all 36 host-test executables pass, the production-
  shaped codec probe round-trips a 240-byte frame as 960 samples, and all 1,846 genuine capture
  pairs decode in `0x04 + 0x02` order with zero failures or duration mismatches.

## 2026-07-22

### Fixed

- **Genuine Pro Controller 2 bonded HOME reconnect is hardware-confirmed.** The custom ATT bond's
  LTK is restored through BTstack's Security Manager instead of raw HCI encryption, which had
  encrypted the ACL without restoring the controller's active session. Input and native gyro now
  return without SYNC, and the P1 LED is reasserted after each controller power cycle. Twenty
  consecutive controller-off/HOME cycles passed.

### Added

- **UART protocol tracer and reconnect diagnostics.** The headered Pico 2 W can expose console USB
  transactions, firmware reads, raw BLE captures, SM state, and native-motion ownership while the
  primary USB port remains connected to the Switch 2.
- **Current Switch 2 firmware identities.** Pro Controller 2, NSO GameCube, and both Joy-Con 2
  personalities report up to date on real hardware.

### Validation

- All 35 host-test executables pass. Pico W and Pico 2 W release targets compile successfully.

## 2026-07-21

### Added

- **Genuine Pro Controller 2 native motion passthrough is hardware-confirmed.** The UART console
  tracer and live GATT discovery resolved the exact controller setup, report-rate descriptor, and
  notification handles. The normal Pro2 path now automatically requests a 7.5 ms BLE interval,
  preserves native `0x1E`/`0x28` motion PDUs byte-for-byte, and normalizes buttons/sticks from the
  same `0x000E` report. Splatoon 3 confirms correct aim, no stationary drift, power-cycle and
  bonded reconnect recovery. Powering off while rotating retains a stationary genuine frame with
  advancing timing rather than leaving the console extrapolating motion.
- **Native motion is source-owned.** Cross-core snapshots record their Bluetooth source slot and
  are accepted only while console-facing slot 0 identifies as genuine `057E:2069`, preventing a
  retained Pro2 frame from leaking into a later controller session. The UART experiment matrix is
  retained separately from the named production profile.

### Validation

- The standard 300 MHz Pico 2 W completed an eight-hour Smash session without an observed thermal
  or stability problem. Temperature was not instrumented, so this is a long-duration functional
  soak rather than a measured thermal characterization.

## 2026-07-18

### Changed
- **Headset-free DualSense native PCM rumble is hardware-confirmed on Pico 2 W.**
  The hardware-confirmed 3.25× peak-preserving renderer now has an
  event-driven no-headset path using valid Opus silence in report `0x39`.
  Streaming exists only for active rumble plus a two-packet STOP tail; repeated
  zeros cannot keep it alive. Live console audio has priority and its queue is
  not consumed by silence. Pico W and the fixed-tone diagnostic keep their
  existing compatibility path. Bonded controller and dongle-power-cycle
  reconnections restore native rumble without requiring a fresh pair.
- **DualSense native haptic mode survives headset replug with capture-derived
  envelope preservation.** Hardware confirms first-insertion audio,
  3× native PCM, unplugged legacy rumble, and repeated audio/haptic restoration.
  The explicit report-`0x32` selector clears the persistent
  `UseRumbleNotHaptics` mode after legacy report-`0x31` use. Genuine Switch 2
  capture replay then showed latest-value sampling loses the interval peak in
  74–78% of active DualSense packets, so the focused follow-up accumulates
  independent left/right scalar peaks between audio reports without changing
  Opus bytes, packet layout, or the validated legacy path.
  Hardware preferred the native PCM character but found it slightly light, so the
  final renderer preserves that waveform and raises only its fixed gain from 3×
  to 13/4× (3.25×). The capture's maximum scalar remains unsaturated at a 110/127
  PCM peak, and hardware judged the result close to HD Rumble.
- **Real Switch 2 DualSense-headset lifecycle is stable; audio/haptic startup
  ownership is revised for focused retest.** The audible `0x02` activation remains
  latched and the persistent RP2350 ISO endpoint survives alt-setting cycles.
  Hardware now confirms stable input, ordinary rumble, repeated jack removal/reinsert,
  and controller/dongle reconnect. A transient build produced audio plus lighter
  native haptics, while the final recent-flow-gated build produced full legacy
  rumble but no audio. The cause was a scheduling loop: recurring legacy `0x31`
  output could prevent the first `0x39` whose acceptance was required to transfer
  ownership. Native `0x39` ownership now begins at headset/audio request time,
  before the first stream packet, and removal restores the legacy path. Haptic
  bytes 12..139 and Opus bytes 142..541 remain independent and are host-tested.
- **Bonded DualSense reconnect audio and conditional Switch 2 headset presence are
  implemented for Pico 2 W.** Bonded reconnects arrive through BTstack HID Host,
  whose eight-bit report length and 80-byte persistence buffer could not represent
  the 142-byte audio activation or 547-byte stream report. The original interrupt-CID
  bypass was invalid because those channel events are private to HID Host. The revised
  build gives HID Host a narrowly scoped 16-bit-length entry point for the exact
  DualSense `0x32`/`0x39` shapes. DualSense physical-jack status is normalized
  through the input seam and
  emitted as the documented Pro Controller 2 report-`0x09`/`0x05` headset states;
  no jack continues to advertise none. Both clean firmware builds, the full host
  suite, and the audio verifier pass. Switch 2 insertion/output are now confirmed;
  reconnect, removal/reinsert, audio, and native rumble are hardware-confirmed.
- **Live Windows PCM → DualSense speaker audio is continuous in the standard Pico 2 W
  300 MHz build.** The bridge now follows the independently corroborated DualSense
  45 kHz effective stream clock (512 real 48 kHz frames → 480 Opus samples), rearms
  isochronous USB endpoints after failed transfers, relocates Opus and hot memory
  primitives to SRAM, and runs a producer-paced Opus worker in core1 foreground while
  CYW43/BTstack remains in its established background IRQ context. A hardware run
  encoded all 13,225 delivered PCM blocks with zero drops/errors; LED/BOOTSEL,
  persistent configuration, cold boot, and ten wake attempts with every known
  controller passed. The validated 300 MHz/1.20 V clock and live Opus bridge are now
  the normal `pico2_w` artifact. A memory-constrained Pico W fixed-point/XIP port
  passed build and memory gates but barely produced audio on hardware, so it was
  rejected and the standard Pico W artifact was restored to its validated
  non-audio configuration. Headset routing, microphone return, and long Pico 2 W
  thermal soak remain open.
- **DualSense Edge Fn L / Fn R now default to GL / GR** (previously Capture / C), per
  community feedback — they join the back paddles, which already default to GL/GR. Both
  Fn buttons keep distinct source bits and remain independently reassignable in config
  mode; only the built-in default changed. Applies to devices with no saved config (or
  after a per-family "reset to default"). (`5f34bd8`)
- **Scope reduced to one dongle → one controller**, retiring the legacy up-to-4
  local-multiplayer discovery. Once a controller is HID-ready, background BLE scan and
  Classic inquiry idle — freeing Bluetooth bandwidth — and the pairing LED goes solid
  immediately instead of blinking for the rest of the 30 s window. The host stays
  connectable/discoverable, so a bonded controller still reconnects (Classic pages back
  in with discovery off; BLE reconnects once discovery resumes at zero connections).
  Delivered as two hardware-validated increments; confirmed across Classic + BLE
  reconnect, wake-from-sleep, and wipe/re-pair with no regressions. (`0798012`, `b6bc3c9`)

### Reverted
- The experimental DualSense-audio-bridge iterations (BT ACL-pipeline tuning, the first
  scan-while-connected attempt, the acoustic analyzer, and the audio stall meter) were
  reverted back to the `7d9d47d` baseline after a **stale incremental build** was
  mistaken for a pairing regression (a clean rebuild pairs fine). All reverted work
  remains in git history and is catalogued in `AUDIO-INVESTIGATION.md` for selective,
  isolated re-application. The battery passthrough + audio-bridge groundwork committed in
  `7d9d47d` itself is unchanged. (`e6c43b0`)

### Documentation & research
- **Added `AUDIO-INVESTIGATION.md`** — the full record of the DualSense Bluetooth-audio
  dropout investigation: the measured **~57 ms dropout every ~560 ms**, every ruled-out
  hypothesis with evidence, the tooling built, and an in-depth study of DS5Dongle's
  architecture, the DualSense audio protocol, genuine **Switch 2 native audio** (wireless
  BLE headset audio + the wired USB UAC1 path), and the **MT3616A0 `DSPH` DSP blob** in
  the Pro Controller 2 SPI dumps. Conclusion: the stall is a **core-1 scheduling problem**
  on PicoSwitch2's specific core split — not a protocol or hardware limit (the same bridge
  works on identical silicon in DS5Dongle). (`d37708b`, `aab0f1e`)
- Recorded the one-controller scope in `STATUS.md` / `PLAN.md` (multi-controller output
  retired from the roadmap). (`02eaf70`)

### Notes
- Process lesson: after a `git revert`, always do a `-Clean` build — an incremental
  rebuild reused stale object files and looked exactly like "the revert didn't take."
