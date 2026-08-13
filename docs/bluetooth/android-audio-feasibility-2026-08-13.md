# Android companion audio / microphone — feasibility assessment

Status: 🔵 INVESTIGATION ONLY — no implementation, no firmware change
Date: 2026-08-13
Related: [`android-controller-bridge.md`](android-controller-bridge.md),
[`../switch2/audio-passthrough-research.md`](../switch2/audio-passthrough-research.md),
[`../switch2/pro2-headset-audio.md`](../switch2/pro2-headset-audio.md)

## Question

Can an Android handheld connected through the companion app carry **game audio out** and
**microphone in**, the way a DualSense does — and can it be offered *only* when the handheld is the
sole connected source, so it never competes with the existing controller audio paths (owner's
framing)?

## Reference: how NS-PC-Control does it

[Dycool/NS-PC-Control](https://github.com/Dycool/NS-PC-Control) states "Audio and microphone are
supported in S2 mode". Its structure (`server/src/s2_uac1_audio.cpp`, `server/src/udp_audio.cpp`,
`client/src/audio_client.cpp`, `webapp/js/feat_audio.js`):

- **Console side: USB UAC1** — 16-bit PCM, speaker popped from the raw USB gadget
  (`s2_rawgadget_pop_console_audio`), mic submitted back
  (`s2_uac1_submit_microphone_audio` → `s2_rawgadget_queue_microphone_audio`), with gain/mute applied
  from the host's UAC1 controls.
- **Client side: UDP over WiFi/LAN** — a separate `udp_audio` transport carries PCM between the Pi and
  the PC/phone client.

**The decisive difference:** his phone link is a **WiFi/UDP socket**; ours is **Bluetooth HID**. His
audio design is therefore not portable to our current transport — it is portable only if we add the
same *kind* of transport.

## What PicoSwitch2 already has (verified in source)

Substantially more than expected — the console-facing half is already built:

| Piece | State | Evidence |
|---|---|---|
| UAC1 **speaker OUT** (console → adapter), 48 kHz stereo 16-bit | ✅ operational | `switch_pro2.c` IF3 alt 1, consumed into a sink |
| UAC1 **microphone IN** (adapter → console), 48 kHz stereo 16-bit | ✅ **endpoint operational, currently transmitting silence** | `switch_pro2.c` IF4 alt 1, EP `0x83`; "continuously supplying silent microphone PCM makes the USB audio function operational" |
| Writable master mute/volume (Feature Units 0x02/0x05) | ✅ | same |
| Console game audio available as PCM inside the adapter | ✅ | the DualSense speaker bridge already consumes it |
| Live audio budget on Pico 2 W | ⚠️ tight | 300 MHz/1.20 V, SRAM-resident Opus, a **dedicated core1 foreground worker**; Pico W was evaluated and **rejected** — it "barely played audio" |
| WiFi | ❌ **not compiled in** | `CMakeLists.txt` links `pico_cyw43_arch_none` (BT only; no lwIP) |

So the missing piece is **only the transport between adapter and phone** — plus the CPU/RAM to run it.

## Transport options

### A. Over the existing Bluetooth HID link — impossible
`BluetoothHidDevice` carries HID reports only; the profile has no audio channel. No descriptor work
changes this. (HID reports could technically smuggle PCM, but at 125 Hz × tens of bytes it is orders
of magnitude short of 1.5 Mbit/s stereo, and would destroy input latency.)

### B. Bluetooth A2DP / HFP — possible but the worst fit
The adapter would need to be an A2DP **sink** (phone mic → adapter) *and* an A2DP **source** (game
audio → phone), i.e. both roles, plus codec work, while already running Classic HID, BLE management,
and USB. A2DP's ~100–200 ms latency is unacceptable for the only feature that motivates the mic
(voice chat), and HFP's 8/16 kHz SCO is a quality floor. It also directly contends with the DualSense
audio path this proposal is meant to avoid competing with.

### C. WiFi + UDP, mirroring NS-PC-Control — the only credible path
The CYW43 does WiFi as well as Bluetooth, and raw 48 kHz stereo 16-bit PCM (~1.5 Mbit/s each way) is
comfortable for WiFi, so **no codec is needed** — which is cheaper than the existing Opus path, not
more expensive. This also matches the owner's scoping instinct exactly: if the handheld is the only
source, the adapter is not simultaneously running the DualSense Opus encoder, so the two audio paths
never coexist.

**What it would require (none of which exists today):**
1. Link `pico_cyw43_arch_lwip_*` instead of `pico_cyw43_arch_none`, adding lwIP and the WiFi driver.
2. **WiFi/BT coexistence on one CYW43 radio.** They time-share; this is the single biggest unknown and
   must be measured, not assumed — the adapter would be running WiFi audio *and* Classic HID input
   from the same phone concurrently.
3. Network onboarding: SSID/credentials or SoftAP, discovery, and reconnect — a whole UX surface the
   project does not have, plus its own security story (an open UDP audio socket on the user's LAN).
4. Jitter buffering and clock sync between the WiFi packet rate and the USB isochronous 1 ms cadence,
   in both directions.
5. RAM: lwIP plus buffers on top of an audio path that already keeps Opus SRAM-resident. **Pico W
   (264 KB, already rejected for audio) is out; Pico 2 W (520 KB) needs a real budget check.**
6. Feeding the mic endpoint from a live source instead of the silence generator, and gating the
   console's UAC1 mute/volume onto it.

**Simplification worth noting:** if WiFi carries audio, it could carry *input* too (as NS-PC-Control
does), removing Bluetooth from the phone path entirely and dissolving the coexistence problem. That
is a strictly larger redesign, but it is the architecturally cleaner end state and should be
considered before building a hybrid BT-input + WiFi-audio adapter.

## Recommendation

**Do not implement now. Keep the parity claim honest: audio is the one controller feature the Android
bridge does not provide, and the HID link cannot ever provide it.**

If it is pursued, the order should be:
1. **Measure CYW43 WiFi+BT coexistence first** on a Pico 2 W — a bounded experiment with no product
   commitment. If concurrent WiFi throughput and Classic HID input cannot both hold, everything above
   is moot and the answer is final.
2. Land the **microphone return path** end to end with a *local* test source first (the endpoint
   already exists and sends silence, so this is the smallest possible increment and is independently
   useful — PLAN.md already tracks "DualSense microphone report decoding and Opus-to-USB return").
3. Only then add the WiFi transport, app-exclusive (refuse to arm while any other controller is
   connected, per the owner's framing).

## Confidence

- **Verified from source:** the UAC1 speaker/mic endpoints, mic-currently-silent, no WiFi compiled in,
  the Pico 2 W audio budget and the Pico W rejection.
- **Verified from the reference project:** that its audio is UAC1 on the console side and UDP on the
  client side.
- **Estimated, not measured:** CYW43 WiFi/BT coexistence headroom, RAM budget with lwIP, and achievable
  end-to-end latency. These are the three things that decide feasibility and none of them is answered
  by reading code.
