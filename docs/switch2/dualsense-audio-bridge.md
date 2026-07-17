# DualSense Audio Bridge — As-Built Reference

> Status (2026-07-17): 🟡 **Speaker activation and transport hardware-confirmed with a fixed Opus
> tone; live Windows PCM path implemented and pending a hardware pass.**
>
> This is the durable *as-built* reference for the bridge that routes the Pro Controller 2 USB
> audio (UAC1) stream out through a paired DualSense's own speaker over Bluetooth. It describes the
> code that exists, not the debugging history. For the chronological investigation that produced it,
> see [`docs/experiments/2026-07-17-dualsense-live-opus-failure.md`](../experiments/2026-07-17-dualsense-live-opus-failure.md);
> for the original feasibility research and the DS5Dongle reference comparison, see
> [`audio-passthrough-research.md`](audio-passthrough-research.md).

## 1. Scope and build gating

The bridge is **experimental, off by default, and RP2350-only** (Pico 2 W). It has three compile
configurations, selected in `CMakeLists.txt` and all requiring `NS2_PRO` + `NS2_AUDIO`:

| CMake option | Compile define(s) | `ds5_audio_bridge.c` branch | Encoder | Opus submodule |
|---|---|---|---|---|
| *(none — default)* | — | no-op stub | none | not built |
| `NS2_DS5_AUDIO_TEST_TONE` | `NS2_DS5_AUDIO`, `NS2_DS5_AUDIO_TEST_TONE` | fixed tone | none (replays `ds5_audio_test_tone_frames`) | not built |
| `NS2_DS5_AUDIO` | `NS2_DS5_AUDIO`, `NS2_DS5_AUDIO_LIVE_OPUS` | live Opus | `libopus` on core1 | `third_party/opus` |

A normal `./build.ps1 pico2_w` (and every Pico W build) uses the default column: the
`ds5_audio_bridge_*` symbols are cheap no-op stubs and USB/Bluetooth behavior is unchanged. Every
`src/*.c` file is always compiled (CMake globs `src/*.c`); the audio bodies are `#ifdef`-gated, not
excluded from the build.

## 2. Data-flow overview

```
 Windows / Switch 2                 Pico 2 W                              DualSense
 ┌──────────────┐   USB UAC1   ┌─────────────────────────────┐   BT HID   ┌──────────┐
 │ audio out    │─192 B/1 ms──▶│ core0: ns2_audio_xfer()      │           │ speaker  │
 │ (48 kHz st.) │              │  → submit_speaker_pcm()      │           │ (Opus)   │
 └──────────────┘              │        │ pico queue_t (12)   │           └────▲─────┘
                               │        ▼                     │                │
                               │ core1: ds5_audio_codec_task()│  report 0x39   │
                               │  accumulate 10 pkt → 480 fr  │──547 B/20 ms──▶│
                               │  opus_encode → 200 B frame   │  (direct L2CAP)│
                               │  buffer 2 frames = one pair  │                │
                               └─────────────────────────────┘
```

- **Producer (core0, TinyUSB):** `src/switch_pro2/switch_pro2.c`. The isochronous OUT completion
  callback `ns2_audio_xfer()` hands each 192-byte packet to `ds5_audio_bridge_submit_speaker_pcm()`.
  Alternate-setting changes call `ds5_audio_bridge_set_usb_streams()`; UAC `SET_CUR` mute/volume on
  the speaker Feature Unit (unit `0x02`) calls `ds5_audio_bridge_set_speaker_control()`.
- **Cross-core queue:** `pico/util/queue` SPSC-safe ring, depth 12, one 192-byte packet per slot.
  On overflow the producer drops the oldest packet (bounded latency, small glitch, self-recovering).
- **Consumer (core1, BTstack):** `src/ds5_audio_bridge.c` `ds5_audio_bridge_codec_task()` drains
  the queue, encodes, and buffers an encoded pair. `src/bt_hid/.../sony/ds5_bt.c`
  `ds5_audio_task()` peeks the pair, builds report `0x39`, and transmits it.

## 3. Rate contract

| Quantity | Value | Note |
|---|---|---:|
| USB packet | 192 B = 96 int16 = **48 stereo frames** | `NS2_AUDIO_PACKET_SIZE`, 1 ms @ 48 kHz |
| Opus input window | **480 frames** = 10 packets | `PCM_INPUT_FRAMES`, 10 ms |
| Opus frame (CBR) | **200 B** | `OPUS_SET_BITRATE(200*8*100)` = 160 kbps, VBR off |
| Report `0x39` | **2 frames = 20 ms** | 50 reports/s, 100 encoded frames/s |

Because 192 B is exactly 48 frames and ten packets are exactly one 480-frame window, the live path
performs **no resampling** — it accumulates 48 kHz PCM directly. (This is the pivot away from
DS5Dongle's 51.2 kHz→48 kHz model; see [§6](#6-vestigial-the-51248-khz-resampler).) The tail-
preservation branch in `codec_task()` only matters if a future endpoint packet size stops dividing
the window evenly; with the current 192-byte packet it never triggers.

## 4. Report `0x39` layout (as emitted by `ds5_audio_packet.c`)

547 bytes, beginning at the report ID; the outer `0xA2` HID transaction byte is prepended by the
caller and is not part of this buffer. Offsets are post-transaction-byte.

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 1 | `0x39` report ID |
| 1 | 1 | `(sequence & 0x0F) << 4` |
| 2–3 | 2 | `0x91`, `0x06` |
| 4 | 1 | mic flag: `0x7F` enabled, `0x7E` disabled |
| 5–8 | 4 | buffer length ×4 (currently `64`) |
| 9 | 1 | packet counter (advances by 2 per report = frames sent) |
| 10–11 | 2 | `0xD2`, `64` |
| 12–139 | 128 | two 64-byte haptics blocks — **zero** (PC2 USB exposes 2 PCM channels, not DS5Dongle's 2 speaker + 2 haptics) |
| 140 | 1 | route: `0x13\|0xC0` speaker, `0x16\|0xC0` headset (bits 6–7 mark the block valid) |
| 141 | 1 | Opus frame length (`200`) |
| 142–341 | 200 | Opus frame A |
| 342–541 | 200 | Opus frame B |
| 543–546 | 4 | reflected CRC32 over `0xA2 \|\| payload`, little-endian |

## 5. Activation, routing, and control

`ds5_audio_task()` (in `ds5_bt.c`) runs a small ordered state machine before any `0x39` stream,
because the DualSense ignores `0x39` until AudioControl is enabled and the direct-L2CAP transport
reports *queue acceptance*, not completed transmission:

1. **`0x32` control report** — `AllowAudioControl` + `AllowSpeaker/HeadphoneVolume` + `AllowMute`
   valid bits, explicit route (speaker `0x30` / headphones `0x02`), non-zero volume, explicit
   mute/unmute (`0x60` mute-speaker-and-hp / `0x00`). Sent once, then re-sent whenever volume,
   mute, or route change.
2. **30 ms settle** (mirrors the LED activation state machine's proven settling delay).
3. **`0x32` mic-status report** — kept ordered ahead of the first stream packet.
4. **`0x39` stream reports** — continuous while encoded pairs are available.

**Volume/mute mapping** (`ds5_audio_bridge_set_speaker_control()`): UAC1 volume is signed 1/256 dB
over `-60..0 dB` (`NS2_AUDIO_VOLUME_MIN..MAX`); it maps to DualSense speaker units `40..100`. UAC
mute maps to route-mute `0x60`. Windows control changes are forwarded live (an earlier defect
stored them without ever applying them).

**Headset routing** (`ds5_audio_headset_connected()`): the headset-present flag is read from the
normal `0x31` input report at byte 55 bit 0 (the reference's byte 56 minus the `0xA1` transaction
byte that BTHID strips). When a headset is present the stream and control reports select the
headphone route; otherwise the internal speaker.

**Microphone:** not implemented. `ds5_audio_bridge_mic_active()` always returns false, and
incoming mic-bearing `0x31` reports (header bit 1) are dropped in `ds5_process_report()` before the
gamepad parser — feeding them to the parser caused the first hardware pass's random-input spam.

## 6. Threading and scheduling

- **Codec is core1-only.** `libopus`'s encoder is created lazily inside `codec_task()` and every
  `opus_encode()` call runs on core1's explicit **48 KiB** stack (`main.c`
  `multicore_launch_core1_with_stack`). The RP2350's default core1 stack lives in the 4 KiB
  `SCRATCH_X` bank, far too small for Opus; `CMakeLists.txt` sets `PICO_CORE1_STACK_SIZE=0` and
  `main.c` supplies the SRAM stack instead.
- **Two service entry points**, both in `ds5_bt.c`:
  - `ds5_bt_audio_service()` — from the ~2 ms BTstack timer (`ns2_bt_host.c`). Runs the codec
    (`run_codec = true`) and transports.
  - `ds5_bt_audio_report_service()` — from inbound HID report boundaries (`ns2_bt_host.c`). In the
    **live** build it only transports already-encoded pairs (`run_codec = false`); live encoding is
    deliberately kept out of the deep receive callback. In the **test-tone** build the "codec" is
    just a timestamp comparison, so it may run at report boundaries too.
  This dual servicing exists because sustained DualSense Classic input was proven to delay BTstack
  timers enough to starve BOOTSEL, rumble, and — here — the 20 ms audio transport deadline.
- **Sniff mode is disabled** in experimental RP2350 audio builds only; its anchor interval imposed a
  periodic half-rate throughput ceiling on the large `0x39` reports (the "010101" beep/silence
  symptom). Ordinary builds keep the confirmed role-switch-plus-sniff policy.
- The **direct-L2CAP queue** is a 10-entry FIFO in audio builds (matching the DS5Dongle reference
  depth) to absorb short `CAN_SEND_NOW`/radio stalls; ordinary builds keep their 2-entry
  LED/rumble footprint. Audio reports receive backpressure rather than being silently coalesced —
  overwriting a queued `0x39` would drop real audio while advancing the packet counter.

## 7. Vestigial: the 512→480 kHz resampler

`src/ds5_audio_resample.c` (`ds5_audio_resample_512_to_480_stereo`) is **no longer called by any
firmware path.** It was written for the DS5Dongle-style model where USB PCM arrives at a 51.2 kHz
cadence and must be resampled to 48 kHz. The as-built live path accumulates exact 48 kHz packets, so
the only thing the bridge still consumes from that module is the `DS5_AUDIO_RESAMPLE_CHANNELS`
constant. The module and its host test (`test_ds5_audio_resample`) still compile and pass. This is
tracked technical debt: it should either be removed (replacing the one constant with a local
`#define`) or explicitly repurposed if a variable-rate source is ever added. Do not treat its
presence as evidence that resampling is on the live path.

## 8. Validation status

**Host tests (all passing):**
- `test_ds5_audio_packet` — `0x39`/`0x32` byte layout and CRC.
- `test_ds5_audio_control` — volume/mute/route control-value conversion.
- `test_ds5_audio_resample` — resampler numerical behavior (module now vestigial; see §7).
- `test_ds5_audio_tone` — libopus decode of the embedded fixed tone (non-silence, ~1 kHz, no
  low-energy frame boundaries). Requires linking `libopus`, so it is not built by a plain host gcc.

**Hardware:**
- ✅ **Fixed-tone build:** audible controller-speaker output; muting the Windows endpoint muted the
  controller. Confirms the full activation path, `0x39` transport + CRC, Opus decode, internal-
  speaker routing, non-zero volume, and Windows mute forwarding. Input, BOOTSEL, and the Windows
  endpoint stayed stable.
- 🟡 **Live Windows PCM build:** implemented, **not yet hardware-tested** at the current
  10-packet/480-frame accumulation cadence.

## 9. Remaining unknowns / next hardware pass

Watch items for the live-PCM hardware pass, in priority order:

1. **Continuity under load.** The fixed tone reached "almost perfect" only after sniff-off +
   report-boundary transport + 10-entry FIFO. Confirm the *encoded* path holds the 20 ms cadence
   while real gamepad input streams — encode CPU cost is the new variable the tone build never
   exercised.
2. **Encoder headroom.** `opus_encode()` at complexity 0 must complete well inside the 2 ms timer
   tick on RP2350. If it does not, the timer path stalls and audio underruns. No static bound
   exists; measure it.
3. **CBR size invariant.** The transport only counts a frame when `opus_encode()` returns exactly
   200 bytes; any other size is dropped (silent gap) rather than zero-padded. Confirm CBR holds 200
   B in practice.
4. **Idle/resume behavior.** When Windows stops sending PCM, `0x39` stops; confirm the DualSense
   speaker re-activates without a pop when playback resumes and that AudioControl need not be
   re-sent.
5. **Instrumentation gap.** Diagnosis so far has been by ear. A passive counter surface (encode
   successes, CBR-size drops, PCM-queue overflows, stream sends, `bt_send_interrupt` failures)
   would make the next iteration data-driven; see the recommendation in `PLAN.md`.

## 10. Cross-references

- [`audio-passthrough-research.md`](audio-passthrough-research.md) — feasibility, DS5Dongle
  reference, licensing.
- [`docs/experiments/2026-07-17-dualsense-live-opus-failure.md`](../experiments/2026-07-17-dualsense-live-opus-failure.md)
  — the full debugging narrative.
- `src/ds5_audio_bridge.c`, `src/switch_pro2/switch_pro2.c` (producer),
  `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_bt.c` and `ds5_audio_packet.c` (transport).
