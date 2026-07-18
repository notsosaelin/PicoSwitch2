# DualSense ↔ NS2 Audio Bridge — Live Codec Scheduling and Clock A/B

> Focused engineering analysis of *why live USB→DualSense audio is choppy* and what to do
> about it. This complements `AUDIO-INVESTIGATION.md` (the full history + protocol/reference
> study); this file is specifically about the live-codec scheduling bottleneck surfaced
> by the 2026-07-18 diagnostics and the contained fix being tested.
>
> Status: 🟢 **300 MHz foreground-worker build is hardware-confirmed continuous by
> listening test with zero PCM drops.** LED/BOOTSEL, persistent configuration,
> cold boot, and console wake are also hardware-confirmed without regression. The
> 200 MHz foreground-worker build remained choppy.

## 1. Symptom

- **Fixed tone** build: reference-clock correction is hardware-confirmed continuous for at
  least 120 seconds. The former ~57 ms/~560 ms dropout was a controller-clock mismatch,
  not the current live-audio fault.
- **Live USB audio** build: **super choppy** — far worse than the tone.

## 2. Diagnostics and recording correlation

The same continuous 1 kHz source was captured at
`dumps/audio/dualsense/live-150mhz.m4a` and
`dumps/audio/dualsense/live-200mhz.m4a`. Five-millisecond RMS-window analysis produced:

| Build | USB packets | Expected 512-frame blocks | Encoded | Queue-dropped | Encoded share | Recorded audible duty |
|---|---:|---:|---:|---:|---:|---:|
| 150 MHz | 173,130 | 16,231 | 6,160 | 10,070 | **38.0%** | **38.2%** |
| 200 MHz | 179,000 | 16,781 | 14,608 | 2,170 | **87.1%** | **87.1%** |

Expected blocks are `USB packets × 48 stereo frames / 512`. In both runs,
`encoded + dropped ≈ expected`, and the encoded ratio independently matches the audible
duty cycle. This proves that the chopped silence is created by complete PCM blocks being
dropped **before Opus encoding**. It is not an audio-file problem, a controller decoder
problem, or loss after L2CAP submission. Submitted audio reports and completed ACL
packets also remained closely matched.

The 200 MHz improvement further proves that encoder service rate is the limiting resource.
Single-encode time improved from at most 4.2 ms to 3.3 ms. The required rate is 93.75 Opus
frames/s: 48,000 real input frames/s divided into 512-frame blocks, each resampled to the
DualSense's 480-sample effective-45-kHz frame.

The USB alternate-setting theory is rejected for these captures. Speaker activity had one
on edge and one off edge across the playback, rather than repeatedly gating the codec.

## 3. The pipeline (old and new)

Before the current change:

```
core0 USB producer                  core1 CYW43 background IRQ
─────────────────────────────       ─────────────────────────────────────────
accumulate 512-frame PCM       ─▶   2 ms BTstack timer callback
depth-2 PCM queue                     dequeue one block
                                       resample 512→480 + Opus encode
                                       depth-2 encoded queue
                                       transport pairs as 0x39 reports
```

The Pico SDK `threadsafe_background` architecture does not run BTstack in the foreground
call to `btstack_run_loop_execute()`. BTstack/CYW43 work is dispatched by a low-priority IRQ
on core1; the core1 foreground was effectively waiting/sleeping. Encoding inside its timer
therefore both waited for a BTstack callback and lengthened that callback.

The current live build keeps all ownership unchanged but uses the otherwise-idle foreground:

```
core0 USB producer                  core1
─────────────────────────────       ─────────────────────────────────────────
accumulate 512-frame PCM       ─▶   foreground: block on PCM queue
depth-2 PCM queue                     wake immediately, resample + encode
                                       │
                                    background IRQ: BTstack/CYW43 may preempt
                                      short 2 ms timer maintains/activates audio
                                      transport completed pairs as 0x39 reports
```

`ds5_audio_bridge_codec_worker()` is non-returning, blocks directly on the cross-core
PCM queue, and encodes as soon as a complete block arrives. The SDK queue notification wakes
the core from core0. Bluetooth can preempt the encoder, but Opus no longer waits for or occupies
a BTstack timer callback. The timer remains a short Bluetooth-safe maintenance and transport
point.

This is deliberately not the failed core split. USB is still on core0; BTstack and CYW43
are still initialized and serviced on core1. Only previously-unused core1 foreground time
has been assigned to Opus.

## 4. Why the tone is fine but live was not

The tone build has no live encoder and no PCM queue: it replays pre-encoded frames. Once its
reference clock was corrected, hardware confirmed a solid tone past 120 seconds. Live audio
added the PCM overrun/drop cascade, explaining why an identical Bluetooth transport could
sound much worse.

## 5. Constraints preserved

- core0 remains the full Switch 2 USB controller/UAC1 device.
- core1 remains the owner of CYW43, BTstack, vendor drivers, wake, BOOTSEL, and timers.
- Opus uses core1 foreground; Bluetooth uses the SDK background IRQ on that same core.
- The protocol, FIFO depths, USB descriptors, and report format are unchanged.

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

## 8. Foreground-worker validation

Hardware result:

| Clock | Listening result | PCM blocks | Dropped | Encoded frames | Audio reports |
|---|---|---:|---:|---:|---:|
| 200 MHz | still choppy, possibly worse | not captured | not captured | not captured | not captured |
| 300 MHz | **continuous/perfect by ear** | 13,225 | **0** | 13,225 | 6,612 |

At 300 MHz, 141,070 USB packets contain
`141070 × 48 / 512 = 13225.3` complete source blocks. The worker dequeued and encoded
13,225 blocks with zero errors and the transport submitted 6,612 two-frame reports,
leaving only the expected single unpaired frame. The PCM queue reached depth one but never
overflowed. This accounts for the entire pipeline and confirms real-time operation.

Expected diagnostic signature:

- `Codec calls` should be approximately equal to `blocks`.
- `no PCM` calls should fall to zero because the worker blocks for a real queue item.
- complete PCM blocks dropped should ideally be zero.
- the codec inter-call histogram now describes real PCM-block arrival, not a 2 ms polling
  callback.

The successful result matches this signature exactly: calls = blocks = encoded frames,
with zero `no PCM`, inactive, disconnected, no-encoder, encode-error, or dropped counts.

The headline `USB PCM DELIVERY STALL` is a diagnostic-classifier false positive for this
run. It reacts to a single historical maximum gap of 992 ms even though the complete
packet-to-block accounting and zero-drop result prove that the bridge consumed everything
Windows delivered. Likewise, `codec gaps over 10 ms` is no longer an error criterion:
real 512-frame blocks arrive every 10.667 ms, so most blocking-worker wake intervals should
exceed 10 ms. These thresholds were designed for the old 2 ms polling callback.

### 300 MHz regression validation

Hardware-confirmed after the continuous-audio result:

- LED and BOOTSEL behavior works.
- Config mode saves mapping and color changes and reads them back after reconnect.
- Wake from console sleep passed ten attempts with each known controller.
- Cold boot behavior works.

Real-console input and rumble during audio remain untested for a routing reason rather
than a known controller regression: audio currently targets the DualSense internal
speaker, while the Switch does not identify a headset as connected to the emulated
controller. Headset-detection/routing behavior remains a separate open item pending
additional hardware observations.

## 9. Clock variants and the working 300 MHz configuration

Three clean live-audio images are produced from the same foreground-worker source:

| Build flag | System clock | Core voltage | CYW43 divider | Effective CYW43 PIO clock |
|---|---:|---:|---:|---:|
| `-Audio` | 150 MHz | SDK default | SDK default (2) | 75 MHz |
| `-AudioOverclock` | 200 MHz | 1.20 V | 3 | 66.7 MHz |
| `-AudioOverclock300` | 300 MHz | 1.20 V | 4 | 75 MHz |

The 300 MHz image is a useful experimental ceiling, not the default recommendation. The
official RP2350 rating is up to 150 MHz and an ambient operating range through 85 °C.
Pimoroni demonstrated 312 MHz at 1.1 V and 25.6 °C on an early sample, followed by seven
samples reaching 316–336 MHz at 1.1 V. That makes 300 MHz plausible, but it does not qualify
every board, flash chip, temperature, or workload.

A heatsink can reduce junction temperature; it cannot correct marginal core/PLL timing,
flash timing, power integrity, or silicon variance. The 300 MHz build therefore needs long
audio, cold-start, reconnect/wake, configuration-write, and ordinary controller regression
testing. Its CYW43 bus is deliberately held to the normal 75 MHz rather than being
overclocked with the CPU.

Test order:

1. **300 MHz foreground worker is the working live-audio configuration.**
2. Keep 200 MHz as a non-working comparison until/unless its hardware diagnostics are
   needed to explain the threshold.
3. Retain 150 MHz as the scheduler-only control.

All overclock builds retain the existing one-second startup delay before the clock change.

## 10. DS5Dongle vs current PicoSwitch2

| Aspect | DS5Dongle | PicoSwitch2 foreground-worker build |
|---|---|---|
| core0 | trivial DualSense passthrough + BT poll | full Switch 2 controller USB/UAC1 |
| Bluetooth | core0 flat poll loop | core1 SDK background IRQ |
| Opus | core1 tight foreground loop | core1 blocking foreground worker |
| Bluetooth/codec relationship | separate cores | same core, Bluetooth preempts codec |
| libopus / resampler in SRAM | yes | yes |
| PCM / encoded FIFO depth | 2 / 2 | 2 / 2 |
| report format | DualSense `0x39` | byte-compatible DualSense `0x39` |

The new design reproduces the important property of DS5Dongle—Opus has an immediate
foreground work loop—without moving PicoSwitch2's latency-sensitive USB or changing
Bluetooth ownership.

## 11. Feasibility verdict

The bridge is not blocked by protocol or radio throughput. Earlier recordings proved that
every missing audible interval corresponded to a locally dropped PCM block. The 300 MHz
foreground-worker run eliminated those drops completely and sounded continuous, while
200 MHz remained below the real-time threshold. The principal 300 MHz platform
regressions are now hardware-cleared. Remaining audio work includes extended
playback/thermal soak testing and headset-presence/routing behavior; real-console input
and rumble during an audio session can be validated once the console exposes that route.

## 12. References

- `AUDIO-INVESTIGATION.md` — full history, protocol, reference-implementation and DSP-blob study.
- `nso-gc-refs/DS5Dongle/` @ `750bde8` — `src/{main,audio,bt}.cpp`, `cmake/relocate_to_ram.cmake`,
  `CMakeLists.txt` (RAM relocation; separate clock configurations by board).
- `src/ds5_audio_bridge.c`, `src/bt_hid/ns2_bt_host.c`,
  `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_bt.c` — the live bridge, timer, and transport.
- Raspberry Pi RP2350 datasheet: <https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf>.
- Pimoroni Pico 2 overclock experiments:
  <https://learn.pimoroni.com/article/overclocking-the-pico-2>.
