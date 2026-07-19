# Changes

Reverse-chronological log of notable changes on the `ns2-testing` branch. The last
tagged release was [`v1.3.0`](https://github.com/notsosaelin/PicoSwitch2/releases/tag/v1.3.0)
(2026-07-17); entries below are post-release development.

## 2026-07-18

### Changed
- **DualSense native haptic mode now survives headset replug, with capture-derived
  envelope preservation under test.** Hardware confirms first-insertion audio,
  3× native PCM, unplugged legacy rumble, and repeated audio/haptic restoration.
  The explicit report-`0x32` selector clears the persistent
  `UseRumbleNotHaptics` mode after legacy report-`0x31` use. Genuine Switch 2
  capture replay then showed latest-value sampling loses the interval peak in
  74–78% of active DualSense packets, so the focused follow-up accumulates
  independent left/right scalar peaks between audio reports without changing
  Opus bytes, packet layout, or the validated legacy path.
  Hardware preferred the native PCM character but found it slightly light, so the
  current comparison candidate preserves that waveform and raises only its fixed
  gain from 3× to 13/4× (3.25×). The capture's maximum scalar remains unsaturated
  at a 110/127 PCM peak.
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
  the 142-byte audio activation or 547-byte stream report. The audio build now
  captures the negotiated HID interrupt CID and bypasses only those Sony audio
  reports. DualSense physical-jack status is normalized through the input seam and
  emitted as the documented Pro Controller 2 report-`0x09`/`0x05` headset states;
  no jack continues to advertise none. Both clean firmware builds, the full host
  suite, and the audio verifier pass. Switch 2 insertion/output are now confirmed;
  reconnect and the revised unplug path remain pending.
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
