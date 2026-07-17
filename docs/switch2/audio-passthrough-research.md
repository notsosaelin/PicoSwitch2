# DualSense Audio Passthrough — Research and Implementation Notes

> Status (2026-07-17): 🟡 **USB milestone hardware-validated; Pico 2 W speaker transport under
> diagnosis after the first live bridge failed hardware validation.**
> The old descriptor-only class stub has been replaced by a PC2-specific UAC1 driver. It opens
> both 192-byte/1-ms isochronous endpoints, consumes speaker PCM, continuously supplies silent
> microphone PCM, and implements writable mute/volume controls. The RP2350 build now queues USB
> PCM across cores, converts the proven 51.2 kHz cadence to 48 kHz, encodes fixed 10 ms stereo Opus
> frames, and emits DualSense reports `0x39`/`0x32`. Windows starts both USB audio endpoints without
> Device Manager Code 10, and the UAC1-only hardware pass found no controller regressions. The
> first live-Opus pass failed with no audio and severe DualSense scheduling/input regressions, so
> live encoding is disabled by default. A codec-free tone pass fixed those regressions but remained
> silent through an AudioControl retest. A subsequent byte audit found that the existing
> compatibility initialization explicitly applied zero headphone/speaker/microphone volume and an
> implicit route. The consolidated diagnostic now applies nonzero volume, explicitly selects and
> unmutes the controller speaker, forwards Windows UAC mute/volume changes, validates the
> negotiated 548-byte L2CAP path, and uses a louder host-decoded 1 kHz Opus stream.

## 1. Goal

The Switch 2 Pro Controller has a genuine headset jack; our USB descriptor already advertises the
matching 3 audio interfaces (`src/switch_pro2/switch_pro2.c`, `NS2_AUDIO` Option A). As of
2026-07-17, a real UAC1 endpoint/control driver services those interfaces; its initial transport
milestone deliberately sinks speaker PCM and emits silent microphone PCM. The complete feature:
when the connected Bluetooth controller is a
**DualSense** (which has its own onboard speaker, 3.5 mm jack, and mic), bridge the Switch 2's USB
audio stream to and from the DualSense's own audio hardware — so a headset plugged into the Pro
Controller-shaped dongle, or the DualSense's own speaker/mic, actually works.

## 2. A working reference implementation exists: `awalol/DS5Dongle` (MIT license)

[`awalol/DS5Dongle`](https://github.com/awalol/DS5Dongle) is a **Pico 2 W firmware that does the
structurally identical bridge**, just with a PC as the USB host instead of a Switch 2:

```
DS5Dongle:   PC  <--USB Audio Class-->  Pico 2 W  <--BT-->  DualSense (own speaker/jack/mic)
This repo:   Switch 2  <--USB Audio Class-->  Pico  <--BT-->  DualSense (own speaker/jack/mic)
```

Same board family (RP2350/RP2040 + CYW43), same SDK (Pico SDK + TinyUSB, pinned 0.21.0), same
general shape of problem (USB Audio Class on one side, DualSense's own proprietary BT audio
protocol on the other). **License: MIT** (confirmed via GitHub API) — permissive, safe to read,
adapt, and vendor pieces of with attribution.

### 2.1 The DualSense's own BT audio protocol (what DS5Dongle reverse-engineered/implements)

This is the part our own `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_bt.c` driver does **not**
currently touch at all — it only parses/sends report `0x31` (standard BT input/output: buttons,
sticks, rumble, LED, touchpad; confirmed by reading that file, `DS5_REPORT_BT_INPUT`/`_OUTPUT`
`#define`s). DS5Dongle's `src/bt.cpp`/`src/audio.cpp` use two additional report IDs entirely
unhandled here:

- **Report `0x39`** — a 547-byte bidirectional report carrying BOTH speaker audio (device→host,
  i.e. controller mic actually — see below) AND haptic/trigger data in one packet:
  - bytes 0-3: report ID + sequence counter + flags
  - bytes 4-11: audio config/packet sequencing
  - bytes 12-139: two 64-byte haptic-feedback blocks
  - bytes 140-541: speaker block header plus two 200-byte Opus frames (when audio is enabled)
- **Report `0x32`** — microphone *status* updates (separate from the actual mic audio data, which
  arrives via `0x39`-shaped packets too, decoded through `mic_add_queue()`).

### 2.2 Codec: Opus, not raw PCM

The DualSense does not send/receive raw PCM over Bluetooth (BT Classic HID bandwidth/latency at
the report rate can't carry it) — it uses **Opus**, tuned aggressively low-latency/low-bitrate:
- Speaker path (USB→controller): 48 kHz stereo, 200 bytes/frame
- Mic path (controller→USB): 48 kHz mono, 71 bytes/frame
- `OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS)`, `OPUS_SET_BITRATE(200*8*100)` = 160 kbps,
  `OPUS_SET_VBR(false)`, `OPUS_SET_COMPLEXITY(0)` (lowest — CPU-constrained, not quality-constrained)

### 2.3 USB-side plumbing (DS5Dongle, PC-facing — would need Switch2-facing adaptation)

- Real TinyUSB audio endpoint lifecycle. Pico SDK 2.2.0's generic `tud_audio_*` driver cannot be
  used unchanged here: its `open()` explicitly accepts UAC2 interface protocol only, while the
  byte-verified retail PC2 descriptor is UAC1. PicoSwitch2 therefore implements the equivalent
  UAC1 endpoint/control lifecycle in its narrow PC2 class driver while retaining the retail bytes.
- Speaker: USB OUT delivers 384-byte PCM blocks (192 frames, 4ch, 16-bit — DS5Dongle multiplexes 2
   speaker channels + 2 haptic channels into one USB audio stream); channels 0-1 buffered to
   512-frame blocks, interpreted at the proven 51.2 kHz cadence, resampled to 480-frame/48 kHz
   Opus windows, and encoded on core1.
- Mic: DualSense's Opus packets decoded on core1 to 480-frame PCM, mono duplicated to stereo (for
  Windows compatibility — may not be needed for a console host), written via `tud_audio_write()`.
- **Runs entirely on core1**, `__not_in_flash_func()`-marked (RAM execution — avoids XIP flash
  cache stalls on the hot path), with a dedicated ~28 KB core1 stack. This is a real, deliberate
  performance-engineering choice, not incidental — worth replicating, not simplifying away, if this
  is ever ported. **Direct conflict to resolve:** this repo's core1 is already fully committed to
  the joypad-os BTstack input stack (`src/bt_hid`). DS5Dongle's core1 does BT *and* audio codec
  work together in one loop; this repo would need to either fold audio into the existing BT core1
  loop (bandwidth/timing budget unverified) or find a third execution context — RP2350 has no third
  core, so this is a real open design question, not a checklist item.

### 2.4 Third-party dependencies, license status

- **`lib/opus`** (Xiph.Org's Opus reference implementation) — **BSD-3-Clause**, confirmed via the
  upstream `COPYING` file. Safe to vendor with attribution, matching this project's existing
  pattern for `src/bt_hid` (joypad-os, Apache-2.0, attributed in `src/bt_hid/LICENSE`).
- **`lib/WDL`** (Cockos WDL, used for resampling) — **license unconfirmed this pass.** GitHub's API
  reports no detected `license` field for `justinfrankel/WDL`, and the expected `license.txt` path
  404'd during this research session. WDL has historically been distributed under permissive terms
  stated inline in source comments rather than a root LICENSE file, but that was **not verified
  directly** here — read the actual license text in the specific files needed before vendoring
  anything from it. Do not assume permissive terms apply without checking.

## 3. What this means for PicoSwitch2, concretely

This is a **real, substantial feature**, not a small polish item — comparable in scope to the past
BT-stack migration (`docs/bluetooth/bt-stack-migration.md`), not to the NFC/audio-stub/poll-rate
items from the 2026-07-12 pass. Rough shape of the work, for whenever it's picked up:

1. **Core1 budget analysis first.** Determine whether Opus encode/decode (10 ms frames, complexity
   0) can coexist with the existing joypad-os BTstack loop on one RP2350 core, or whether the
   architecture needs to change (e.g. move some BT work to core0, currently TinyUSB-only). This is
   the load-bearing open question — everything else is detail work until this is answered.
2. 🟡 **Extend `ds5_bt.c` for report `0x39`/`0x32`.** Speaker report construction, CRC, bounded
   queues, Opus encoding, and direct-L2CAP transmission are implemented for Pico 2 W and covered
   by host tests, but the first live-encoder hardware architecture failed. Audio-bearing incoming
   `0x31` reports are now filtered from gamepad parsing. The first deterministic-tone pass was
   stable but silent and exposed a missing prerequisite: DS5Dongle sends an extended `0x32`
   `AllowAudioControl` state transaction before `0x39`. Adding it preserved stability but did not
   restore sound. The next audit found separate conflicts: the pre-existing compatibility report
   marked all volume and audio-route fields valid but wrote zeros, and the UAC driver stored host
   mute/volume changes without forwarding them. The consolidated diagnostic now retains the
   confirmed rumble/LED flags while applying headphone/speaker volume 100, microphone volume 64,
   explicit speaker routing and unmute in both initialization paths, and subsequent host-control
   updates. That pass produced audible controller-speaker output and Windows mute correctly
   silenced it, confirming activation, routing, Opus decode, and host-control forwarding. The tone
   was discontinuous; its guaranteed 21.333 ms pacing for 20 ms of encoded audio and a headset
   status off-by-one were corrected, followed by USB-idle isolation and lossless queue
   backpressure. Each correction improved but did not eliminate the intermittent tone. The next
   diagnostic addresses the remaining project-local starvation path by servicing lightweight
   transport at inbound report boundaries and using a ten-entry RP2350 audio FIFO; live encoding
   remains outside the deep receive callback. Reserved/haptics zeros and the disabled microphone
   path are intentional.
3. ✅ **Replace the descriptor-only stub with operational UAC1 USB plumbing.** Implemented
   2026-07-17 without changing the byte-verified descriptor. The first Windows hardware pass is
   confirmed that Device Manager Code 10 is gone with no known controller regressions.
4. ✅ **Vendor Opus without importing WDL.** Opus is pinned as a submodule to the reference's
   known-working commit. The immutable 512-to-480 conversion is a small project-owned linear
   resampler with host coverage, avoiding another dependency and its licensing ambiguity.
5. Scope to **DualSense only** first (per the user's framing) — Xbox/other controllers with
   headset jacks are a separate, later generalization (`unmapped-features.md` "Audio Over
   Bluetooth" already tracks this as unmapped for all controller families).

### 3.1 The captured DSPH blob is not the USB audio engine

The 2 MiB Pro Controller 2 SPI dumps contain a `DSPH` region at offset `0x175000`. It is useful
evidence for Nintendo firmware/update compatibility and may contain headset-DSP firmware or
coefficients for the retail controller's own hardware. The Pico cannot execute that target-specific
blob, and serving it does not create USB endpoints, buffering, or Bluetooth audio transport. It is
therefore retained for later updater/DSP research but is not a substitute for the UAC1 and Opus
implementations described above.

## 4. Cross-references

- `docs/switch2/unmapped-features.md` §"USB Audio" / §"Audio Over Bluetooth" — the existing
  unmapped-feature entries this doc supersedes with real technical detail.
- `PLAN.md` "Advanced haptics" backlog item, "Out of Scope (confirmed)" — audio over BT controllers
  is explicitly out of scope for the *current* milestone; this doc is the research trail for
  whenever that changes, not a signal that it's changing now.
- `docs/bluetooth/bt-stack-migration.md` — the precedent for how this project plans a large
  core1-affecting architectural change before starting it.
