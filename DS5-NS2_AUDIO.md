# DualSense ↔ NS2 Audio Bridge — Live Codec Scheduling and Clock A/B

> Focused engineering analysis of *why live USB→DualSense audio is choppy* and what to do
> about it. This complements `AUDIO-INVESTIGATION.md` (the full history + protocol/reference
> study); this file is specifically about the live-codec scheduling bottleneck surfaced
> by the 2026-07-18 diagnostics and the contained fix being tested.
>
> Status: 🟢 **The standard Pico 2 W 300 MHz foreground-worker build is
> hardware-confirmed continuous by
> listening test with zero PCM drops.** LED/BOOTSEL, persistent configuration,
> cold boot, and console wake are also hardware-confirmed without regression. The
> 200 MHz foreground-worker build remained choppy. A Pico W fixed-point/XIP
> 300 MHz experiment passed build/memory gates but barely played audio on
> hardware; it was rejected and Pico W remains a non-audio target. Real-console
> headset insertion and output are confirmed. Subsequent real-console passes
> confirmed the audible activation and persistent ISO lifecycle: input, ordinary
> rumble, repeated removal/reinsert, and controller/dongle reconnect are stable.
> A transient build produced audio plus lighter native haptics, whereas the final
> recent-flow-gated build produced full legacy rumble but no audio. The current
> revision removes that startup deadlock by reserving native `0x39` audio/haptic
> ownership when the headset/audio path is requested, before its first packet.
> Audio/haptic coexistence and bonded-reconnect audio await hardware validation.

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

Real-console headset insertion/output plus simultaneous input, rumble, and wake are
confirmed. That pass exposed rumble-induced audio gaps and an unplug transition that
killed input until console restart. The revised in-band haptic PCM and corrected
AudioControl removal path still require a hardware pass.

## 9. Clock variants and the working 300 MHz configuration

The standard image and two clean lower-clock comparisons are produced from the same
foreground-worker source:

| Build flag | System clock | Core voltage | CYW43 divider | Effective CYW43 PIO clock |
|---|---:|---:|---:|---:|
| `-Audio` | 150 MHz | SDK default | SDK default (2) | 75 MHz |
| `-AudioOverclock` | 200 MHz | 1.20 V | 3 | 66.7 MHz |
| `pico2_w` (standard) | 300 MHz | 1.20 V | 4 | 75 MHz |

The project owner accepted the hardware-confirmed 300 MHz image as the Pico 2 W default.
The official RP2350 rating is up to 150 MHz and an ambient operating range through 85 °C.
Pimoroni demonstrated 312 MHz at 1.1 V and 25.6 °C on an early sample, followed by seven
samples reaching 316–336 MHz at 1.1 V. That makes 300 MHz plausible, but it does not qualify
every board, flash chip, temperature, or workload.

A heatsink can reduce junction temperature; it cannot correct marginal core/PLL timing,
flash timing, power integrity, or silicon variance. The standard build still needs long
audio, cold-start, reconnect/wake, configuration-write, and ordinary controller regression
testing. Its CYW43 bus is deliberately held to the normal 75 MHz rather than being
overclocked with the CPU.

Test order:

1. **300 MHz foreground worker is the working standard Pico 2 W configuration.**
2. Keep 200 MHz as a non-working comparison until/unless its hardware diagnostics are
   needed to explain the threshold.
3. Retain 150 MHz as the scheduler-only control.

The 200 MHz comparison and standard 300 MHz build retain the existing one-second startup
delay before the clock change.

### Rejected Pico W platform profile

The exact RP2350 layout is impossible on Pico W: Pico 2 W has 520 KiB SRAM while
Pico W has 264 KiB, and the floating-point Pico 2 image relocates roughly 281 KiB
of Opus code/tables into RAM. The experimental RP2040 port therefore:

- compiles Opus fixed-point with its float API disabled;
- executes Opus from XIP flash instead of relocating the archive;
- uses a 36 KiB explicit core1 codec/BT-interrupt stack;
- configures boot2 flash `/4` and CYW43 `/4` at 300 MHz, keeping both at 75 MHz.

The linked image uses 8,516 bytes of initialized RAM and 142,584 bytes of BSS.
The fixed-point stereo encoder requires 29,892 bytes, leaving approximately
79 KiB for remaining runtime heap activity. Flash usage is approximately 1.01 MiB
of the Pico W's 2 MiB.

This cleared the memory/build blocker but failed the physical one: audio barely
played during hardware testing. The fixed-point/XIP bridge, 300 MHz overclock,
codec stack, and flash/CYW43 divider changes are therefore not part of the
standard Pico W artifact. Pico W remains on its prior validated non-audio
configuration.

Pairing is not part of audio negotiation. `ds5_init()` runs on every Bluetooth
HID connection, resets the audio transaction state, connects the bridge, and
resends the extended `0x32` audio-enable report before streaming `0x39`. A saved
bond must therefore work after ordinary power-off/reconnect.

The reconnect defect was below `ds5_init()`: fresh pairing used raw direct L2CAP,
while a bonded reconnect used BTstack HID Host. Its output API has an eight-bit
report length and the local persistence buffer is 80 bytes, so it could not send
the 142-byte activation or 547-byte stream report. The Pico 2 W audio build now
captures each HID Host connection's negotiated interrupt CID and bypasses only
the oversized Sony audio reports through that CID. Ordinary HID output is
unchanged. This is compile-tested and pending the two physical reconnect cases:
controller power-cycle with the dongle attached, and dongle power-cycle with the
saved bond.

DualSense report `0x31` also supplies the physical jack state. Status bit 0 means
headphones are inserted and bit 1 distinguishes a microphone-equipped headset.
That normalized state crosses the input seam and drives Pro Controller 2 report
`0x09` (`0x00`, `0x05/0x0D`, or `0x07/0x0F`) plus the report-`0x05` headset bit.
No jack means no advertised headset, so the console should not route audio to the
bare DualSense speaker. A real Switch 2 now confirms jack insertion, headset
recognition, audio output, and simultaneous input/rumble/wake.

The first unplug test stopped input until the console restarted. The outgoing
DualSense control builder had treated AudioControl byte 11 as a simple
speaker/headphone route and resent `0x32` on the removal edge. The reference
`SetStateData` layout identifies microphone/channel-path controls there; the
actual per-block destination is report `0x39` byte 140.

A follow-up build proved two separate facts: eliminating the reverse removal
transaction lets ordinary rumble return after unplug, but setting AudioControl
to zero makes the recognized headset silent. Re-insertion also stalled input and
audio until dongle removal because the RP2350 ISO endpoint was reactivated even
though TinyUSB's persistent allocation API had never closed it at alt 0. The
current build restores the hardware-audible `0x02` insertion value, latches that
path so removal never sends `0x30`, and tracks endpoint activation plus pending
transfers across alt 1 → 0 → 1.

Rumble during console audio also exposed a transport conflict: the separate
legacy rumble report displaced an audio send and produced a stutter. Report
`0x39` already separates two 64-byte stereo signed-8 haptic PCM blocks at 3 kHz
(bytes 12..139) from its two Opus speaker frames (bytes 142..541). Its host test
verifies that nonzero haptics leave both Opus ranges unchanged.

The first ownership rule still had a startup race. Waiting for a recent accepted
`0x39` allowed recurring full-strength legacy `0x31` output to occupy the ordinary
send path before the first audio packet. The code therefore waited for an event
that its own scheduling could prevent. This matches the hardware A/B result:
audio plus lighter native haptics in a transient build, then full legacy rumble
plus no audio after recent-flow gating. Native ownership now begins when a physical
headset or active USB speaker path requests audio, before the first `0x39`; removal
returns to the established legacy path. Haptic strength/quality, audio continuity,
and audio after bonded reconnect require focused hardware validation.

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

The Pico 2 W bridge is not blocked by protocol or radio throughput. Earlier recordings proved that
every missing audible interval corresponded to a locally dropped PCM block. The 300 MHz
foreground-worker run eliminated those drops completely and sounded continuous, while
200 MHz remained below the real-time threshold. The principal 300 MHz platform
regressions are now hardware-cleared. Headset insertion and real-console output
are hardware-confirmed. Bonded reconnect, corrected unplug recovery, and in-band
haptic coexistence remain hardware-pending, followed by extended playback/thermal soak.
The Pico W experiment showed that fitting the encoder is not sufficient: its
fixed-point/XIP worker did not sustain useful playback on hardware. Audio is
therefore locked to Pico 2 W builds.

## 12. References

- `AUDIO-INVESTIGATION.md` — full history, protocol, reference-implementation and DSP-blob study.
- `nso-gc-refs/DS5Dongle/` @ `750bde8` — `src/{main,audio,bt}.cpp`, `cmake/relocate_to_ram.cmake`,
  `CMakeLists.txt` (RAM relocation; separate clock configurations by board).
- `src/ds5_audio_bridge.c`, `src/bt_hid/ns2_bt_host.c`,
  `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_bt.c` — the live bridge, timer, and transport.
- Raspberry Pi RP2350 datasheet: <https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf>.
- Raspberry Pi RP2040 datasheet: <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>.
- Raspberry Pi Pico SDK releases (RP2040 200 MHz certification):
  <https://github.com/raspberrypi/pico-sdk/releases>.
- Xiph Opus source and fixed-point build support: <https://github.com/xiph/opus>.
- Pimoroni Pico 2 overclock experiments:
  <https://learn.pimoroni.com/article/overclocking-the-pico-2>.
