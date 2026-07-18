# DualSense ↔ NS2 Audio Bridge — Core-1 Scheduling Problem & Path Forward

> Focused engineering analysis of *why live USB→DualSense audio is choppy* and what to do
> about it. This complements `AUDIO-INVESTIGATION.md` (the full history + protocol/reference
> study); this file is specifically about the **scheduling/architecture bottleneck** surfaced
> by the live-audio diagnostics on 2026-07-18, plus theories, recommendations, and candidate
> solves.
>
> Status: 🟡 root cause narrowed to **core-1 contention** (strong evidence); one software
> alternative (`usb_speaker_active` gating) not yet excluded. No fix committed.

## 1. Symptom

- **Fixed tone** build: plays, but with the pre-existing periodic ~57 ms dropout every ~560 ms
  (see `AUDIO-INVESTIGATION.md` §3).
- **Live USB audio** build: **super choppy** — far worse than the tone.

## 2. Diagnostics (live audio, on hardware)

| Measurement | Value |
|---|---|
| USB speaker packets received (core0) | ~88,000 (≈ 88 s @ 1 ms/packet) |
| Opus frames actually produced (core1) | **2,101** |
| PCM consumed vs delivered | **~25%** |
| Complete 512-frame PCM blocks discarded (queue full) | **~75%** |
| Core-1 max unresponsive gap | **10.3 ms** |
| Single Opus encode time | **≤ 4.2 ms** |

Downstream, the L2CAP/HCI gaps are a *consequence* of there being no encoded audio ready —
not the primary fault.

## 3. The pipeline (as built)

Three stages across two cores (`src/ds5_audio_bridge.c`, `NS2_DS5_AUDIO_LIVE_OPUS`):

```
core0 (USB)                         core1 (BTstack run loop)
─────────────────────────────       ───────────────────────────────────────────
submit_speaker_pcm()                ds5_audio_bridge_codec_task()   [2 ms timer]
  accumulate 512-frame block   ─▶     queue_try_remove(pcm_block)   (ONE per call)
  speaker_pcm_block_queue (depth 2)   resample 512→480
                                      opus_encode (≤4.2 ms)
                                      speaker_encoded_frame_queue (depth 2)
                                                    │
                                    ds5_audio_task() transport: peek 2 frames → 0x39 → L2CAP
```

Both FIFOs are **depth 2** (matching DS5Dongle). `codec_task` consumes **exactly one block per
call** (`ds5_audio_bridge.c:521`) and is called **only** from the 2 ms BTstack audio timer on
core1 (`ns2_bt_host.c audio_timer_handler` → `ds5_bt_audio_service` → `ds5_audio_task(run_codec=true)`).

## 4. Root cause — encode speed ≠ throughput

The 4.2 ms encode time is a **red herring**. It says a single frame is cheap; it says nothing
about how often the encoder *gets to run*. `codec_task` lives inside the **core-1 BTstack run
loop**, sharing it with:

- every incoming DualSense report (parse + `bthid_on_report_boundary`: bootsel/gesture/wake/transport),
- the control (30 ms), rumble (3 ms), and audio (2 ms) timers,
- the audio transport,
- and the encode itself, which **blocks the whole run loop for its full 4.2 ms**.

So the codec's effective duty cycle is set by *how much of core1 is left after BTstack* — not by
encode speed. In **audio mode the DualSense's report rate rises** (mic data multiplexed into
`0x31`), so per-report core-1 work grows exactly when the codec needs slots. The encoder ends up
getting ~1 of every 4 block-arrival windows (blocks arrive every ~10.7 ms; the depth-2 queue
tolerates only ~21 ms), so **core0 drops ~75% of blocks at a full `speaker_pcm_block_queue`**, and
the transport starves. That is the measured 25%.

**Load arithmetic:** 100 frames/s × 4.2 ms = **~42% of one core just to encode**. DS5Dongle spends
that 42% on a *dedicated* core1 (58% idle → fine). PicoSwitch2 spends it on a core1 that BTstack
already loads heavily during audio → **overload → starvation.**

### Why the tone is fine but live is not
The tone build has **no encoder and no PCM queue** — it just replays two pre-encoded frames, so
there is no block-drop cascade. It exposes only the raw transport jitter (the 57 ms/560 ms stall).
Live audio stacks the **codec-starvation discard cascade** on top → *super* choppy.

## 5. The architectural constraint (the crux)

RP2350 has **two cores, and both are already committed**:

- **core0** = the full **Switch 2 controller USB device** (enumeration, HID, UAC1 audio) — heavy,
  hard 1 ms USB timing.
- **core1** = **BTstack** + every vendor driver + wake + BOOTSEL + the timers + (now) the codec.

DS5Dongle's continuity trick is a **spare core for the codec**, which it has only because its USB is
a *trivial DualSense passthrough*. PicoSwitch2's USB is an order of magnitude heavier, so **there is
no free core to hand the codec.** This is the whole difference — protocol bytes are identical
(`AUDIO-INVESTIGATION.md` §12).

## 6. Why "move BTstack to core0" fails (tested — broke everything)

Attempted and observed: LED/BOOTSEL dead, device not recognized as a controller on Windows *or*
Switch 2. Expected and not fixable by iteration:

- core0's USB device is latency-critical (1 ms SOF, enumeration, isochronous audio). BTstack has
  long-blocking ops (flash, connection setup, report bursts). **Two heavy latency-critical loops on
  one core → USB misses deadlines → enumeration fails** ("not recognized").
- LED/BOOTSEL ride the BTstack control-timer loop; moving that loop onto the starved core killed them.

DS5Dongle survives BTstack+USB on core0 **only** because its USB is trivial. **Rule this direction
out for PicoSwitch2.**

## 7. What is already done (spent levers)

- **libopus relocated to RAM** (`CMakeLists.txt` ~94–115, `opus_ram_relocate` → `add_dependencies`).
- **Resampler in RAM** (`ds5_audio_resample_512_to_480_stereo` is `__not_in_flash_func`).
- **`codec_task`/encode in RAM** (`__not_in_flash_func`).
- **1-dongle-1-controller discovery gating** (removes always-on scan/inquiry contention;
  hardware-confirmed — commits `0798012`, `b6bc3c9`).

So the cheap CPU headroom (XIP-miss elimination) is **already claimed**; the 4.2 ms is the
RAM-resident cost at the default 150 MHz.

## 8. Theories (ranked, with how to confirm)

1. **Core-1 CPU contention (leading).** BTstack's audio-mode load + the ~42% encode exceed core1's
   budget; the codec is starved of run-loop slots. *Confirm:* codec-task counters (below).
2. **`usb_speaker_active` gating (not excluded — check first).** `codec_task` early-returns at
   `ds5_audio_bridge.c:517` when `!usb_speaker_active`. Windows shared-mode can **idle the speaker
   alternate setting during gaps** — and the choppiness creates gaps → a feedback loop that would
   *look* like 25% consumption but is a **software gate, not CPU.** If this is it, core work is
   irrelevant. *Confirm:* log `usb_speaker_active` transitions + count early-returns.
3. **Tone-build 57 ms/560 ms stall (separate, still open).** A periodic core-1 blocker unrelated to
   the encoder (≈ 18 × the 30 ms control tick). *Confirm:* the stall meter (`AUDIO-INVESTIGATION.md`
   §8), `core1MaxGapUs` vs `sendMaxGapUs`.

### The one measurement that must come first
Instrument `codec_task` with three counters — **calls**, **early-returns (and the reason)**, and
**actual encodes** — plus a histogram of the codec-task *inter-call interval*, and a log of
`usb_speaker_active` edges. This separates theory 1 (few real slots) from theory 2 (many calls,
early-return) **before** any core restructuring. Restructuring cores to fix a software gate would be
wasted effort.

## 9. Recommendations / candidate solves (ordered by leverage ÷ risk)

**A. Measure before restructuring.** §8's counters. Cheap, decisive, and can turn a "big rework" into
a one-line gate fix if it's theory 2.

**B. Overclock the RP2350 (if CPU-bound).** The default is 150 MHz; DS5Dongle's own build runs
200 MHz (with a vreg bump) on its larger boards. The Pico 2 / RP2350 is **widely reported to
overclock stably well beyond 150 MHz** (e.g. Pimoroni,
<https://learn.pimoroni.com/article/overclocking-the-pico-2>, and community reports of 200–300 MHz).
Even 200 MHz cuts the encode from ~42% toward ~31% of a core — **~33% more core-1 headroom**, and it
may simply make codec+BTstack fit. Lowest-risk high-leverage lever remaining. Validate: input,
enumeration, flash writes, and thermal/stability over a long session.

**C. Trim per-incoming-report core-1 work during audio.** In audio mode the DualSense floods reports;
each runs bootsel/gesture/wake/transport at the report boundary. Gate that heavy path down while
audio is actively streaming so the codec reclaims slots.

**D. Reshape *core1* (not core0) into a flat interleaved loop.** Keep BTstack on core1 (USB stays
safe on core0), but replace the async-timer model — where the codec is one competing timer callback —
with a **flat poll loop**: bounded `cyw43_arch_poll()` then exactly one codec encode, every
iteration, so the codec gets a guaranteed slot each pass. This is DS5Dongle's flat-loop idea applied
to core1 instead of core0 — the *contained* version of the failed experiment, with no USB risk.
Higher effort; do only if A–C are insufficient.

**E. (Ruled out) Move BTstack to core0.** §6. Structurally incompatible with a full controller USB
stack.

### Notes on encode cost
Already using `OPUS_APPLICATION_RESTRICTED_LOWDELAY` (pure CELT), complexity 0, VBR off, RAM-resident.
The 10 ms/200 B frame cadence is **fixed by the DualSense `0x39` protocol** (two 200 B frames per
report), so batching to longer Opus frames is not available. Fixed-point Opus is unlikely to beat the
M33 FPU's float path. So the encode cost is close to floor for this codec — meaning the fix lives in
**scheduling/headroom**, not in making one encode cheaper.

## 10. DS5Dongle vs PicoSwitch2 (why one is continuous)

| Aspect | DS5Dongle (works) | PicoSwitch2 (choppy) |
|---|---|---|
| core0 | trivial DualSense passthrough | **full Switch 2 controller USB emulation** |
| BTstack run loop | core0, flat untimed poll loop | core1, async timers |
| BT-audio transport | core0, same loop as radio poll | core1, 2 ms timer + report boundaries |
| **Opus codec** | **core1, dedicated (whole core)** | **core1, shares the run loop with BTstack** |
| libopus / resampler in RAM | yes | **yes (already)** |
| Clock | up to 200 MHz (big boards) | 150 MHz default (**overclock unspent**) |
| Encoded/PCM FIFO depth | 2 / 2 | 2 / 2 (same) |
| Result | continuous | ~25% PCM encoded → super choppy |

**Takeaway:** identical protocol and identical RAM optimization; the sole meaningful difference is
that DS5Dongle gives the codec a **whole core** and PicoSwitch2 cannot (no free core). Everything in
§9 is about buying back that headroom on a shared core1.

## 11. Feasibility verdict

**Not hardware-limited** — the same bridge runs on the same silicon in DS5Dongle. It is a
**core-allocation** problem on a 2-core chip whose cores are both fully committed, with the easy
(RAM) lever already spent. Likely path: measure (A) → overclock + trim (B, C) → reshape core1 (D)
only if needed. It is a real project, appropriately weighed against being a bonus feature on an
otherwise-solid controller emulator.

## 12. References

- `AUDIO-INVESTIGATION.md` — full history, protocol, reference-implementation and DSP-blob study.
- `nso-gc-refs/DS5Dongle/` @ `750bde8` — `src/{main,audio,bt}.cpp`, `cmake/relocate_to_ram.cmake`,
  `CMakeLists.txt` (RAM relocation + 200 MHz clock).
- `src/ds5_audio_bridge.c`, `src/bt_hid/ns2_bt_host.c`,
  `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_bt.c` — the live bridge, timer, and transport.
- Pico 2 / RP2350 overclocking: <https://learn.pimoroni.com/article/overclocking-the-pico-2>.
