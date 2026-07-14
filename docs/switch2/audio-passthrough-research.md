# DualSense Audio Passthrough — Research Notes (Future Feature)

> Status: ⬜ **Not started — research only.** No code in this repo implements any part of this.
> Deferred per explicit user direction (2026-07-12): audio is a real long-term goal, but not
> current work. This doc exists so a future session can act directly instead of re-deriving the
> protocol from scratch.

## 1. Goal

The Switch 2 Pro Controller has a genuine headset jack; our USB descriptor already advertises the
matching 3 audio interfaces (`src/switch_pro2/switch_pro2.c`, `NS2_AUDIO` Option A) so the console
accepts the device, but they're served by a stub class driver (`audio_stub_*`) that claims the
interfaces and answers a couple of Feature-Unit control requests (2026-07-12 polish pass) with no
real audio I/O behind them. The aspirational feature: when the connected Bluetooth controller is a
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
  - bytes 140-279: two 200-byte speaker Opus frames (when audio is enabled)
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

- Real TinyUSB `tud_audio_*` API (`CFG_TUD_AUDIO`), **not** the class-driver-stub approach this
  repo currently uses for its audio interfaces. Adapting this means swapping our
  `audio_stub_driver` for a real `CFG_TUD_AUDIO`-based one, while keeping our already
  byte-verified Switch2-faithful descriptor bytes (`ns2_config_desc` under `NS2_AUDIO`).
- Speaker: USB OUT delivers 384-byte PCM blocks (192 frames, 4ch, 16-bit — DS5Dongle multiplexes 2
  speaker channels + 2 haptic channels into one USB audio stream); channels 0-1 buffered to
  512-frame blocks, resampled 48→51.2 kHz, Opus-encoded on core1.
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
2. **Extend `ds5_bt.c`** (or add a sibling file) to handle report `0x39`/`0x32` — subscribe,
   parse/build the packet shapes in §2.1, wire to a new audio queue (mirroring how `ns2_seam.c`
   bridges input, not reusing it directly — this is a different data path).
3. **Replace `audio_stub_driver`** with a real `CFG_TUD_AUDIO` implementation, keeping the existing
   byte-verified descriptor. Console-side behavior is unverified either way (the console has never
   been observed sending/expecting real audio traffic in any capture this project holds) — treat
   the first real hardware pass here as an open question, not an assumption.
4. **Vendor Opus** (BSD-3-Clause, low risk) **and resolve WDL's actual license** before vendoring
   any resampling code from it — or use a different resampler if WDL's terms don't fit.
5. Scope to **DualSense only** first (per the user's framing) — Xbox/other controllers with
   headset jacks are a separate, later generalization (`unmapped-features.md` "Audio Over
   Bluetooth" already tracks this as unmapped for all controller families).

## 4. Cross-references

- `docs/switch2/unmapped-features.md` §"USB Audio" / §"Audio Over Bluetooth" — the existing
  unmapped-feature entries this doc supersedes with real technical detail.
- `PLAN.md` "Advanced haptics" backlog item, "Out of Scope (confirmed)" — audio over BT controllers
  is explicitly out of scope for the *current* milestone; this doc is the research trail for
  whenever that changes, not a signal that it's changing now.
- `docs/bluetooth/bt-stack-migration.md` — the precedent for how this project plans a large
  core1-affecting architectural change before starting it.
