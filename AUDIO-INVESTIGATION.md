# DualSense Bluetooth Audio — Investigation Log

> **Status (2026-07-18): 🟢 live internal-speaker audio is continuous in the
> standard 300 MHz Pico 2 W build.** A hardware run consumed and encoded all 13,225
> delivered PCM blocks with zero drops/errors. LED/BOOTSEL, config persistence,
> cold boot, and ten wake attempts with every known controller passed. A real
> Switch 2 now recognizes a physical DualSense headset and plays console audio
> through it with input, rumble, and wake intact. That first pass exposed two
> follow-ups: legacy rumble interrupted audio, and jack removal killed input
> until the console restarted. Follow-up hardware passes confirmed that the
> audible activation must remain enabled and that the persistent RP2350 ISO
> endpoint must survive repeated alt-setting cycles. Input, ordinary rumble,
> repeated jack removal/reinsert, and controller/dongle reconnect are now stable.
> A transient build produced audio plus lighter native haptics, while the final
> flow-gated build produced full legacy rumble but no audio. That A/B result
> identified a startup scheduling deadlock: recurring legacy `0x31` output could
> prevent the first combined `0x39` audio/haptic report whose arrival was required
> to transfer ownership. The current build reserves `0x39` ownership when the
> headset/audio path is requested, before the first stream packet. Audio after a
> bonded reconnect and audio/haptic coexistence remain hardware-pending. Remaining work is
> microphone return and an extended thermal soak. A 300 MHz Pico W fixed-point/XIP port cleared compile
> and memory gates but barely played audio on hardware. It was rejected, and
> Pico W is locked to its prior validated non-audio configuration.
>
> **Historical note:** early implementation/tooling was reverted in `e6c43b0`
> after a stale incremental build was mistaken for a pairing regression. Later
> increments were selectively re-applied and validated. Sections describing those
> attempts are chronological evidence, not the current-state summary; current
> architecture and results are consolidated in `DS5-NS2_AUDIO.md`.

---

## 1. Goal

Bridge the Pro Controller 2 USB audio (UAC1) stream out through a **paired
DualSense's own speaker** over Bluetooth. The structurally identical reference is
[`awalol/DS5Dongle`](https://github.com/awalol/DS5Dongle) (MIT), a Pico 2 W firmware
that does the same bridge with a PC as USB host instead of a Switch 2. It streams
cleanly on the same hardware, so a working target exists.

---

## 2. What works (✅ confirmed on hardware)

The audio path is almost entirely functional. Confirmed on a Pico 2 W + real
DualSense + Windows PC:

- Speaker **activation** (extended `0x32` `AllowAudioControl` transaction).
- `0x39` **stream report** delivery and CRC.
- **Opus decode** on the DualSense (audible tone, not noise).
- **Internal-speaker routing** (`0x13|0xC0`) and non-zero volume.
- **Windows mute** forwarding (muting the Windows endpoint silences the controller).
- Controller **input, BOOTSEL, and the Windows endpoint stay stable** during audio.

The only defect is **continuity**: the audio is chopped by a periodic gap.

---

## 3. The symptom, precisely measured

Early descriptions were subjective ("garbled and dropping", "010101", "Morse-like",
"beep beep beep"). A recording of the fixed diagnostic tone
(`dumps/audio/dualsense/tone-22_5ms.m4a`, ~21 s) analyzed with
`tools/analyze_audio_capture.py` turned that
into numbers:

| Quantity | Value |
|---|---|
| BEEP (tone on) | mean 478 ms, median 503 ms (clusters ~462 / ~503) |
| GAP (silence) | **mean 55.7 ms, median 57 ms, sd 7 ms** (very consistent) |
| PERIOD | mean 538 ms, median 560 ms |
| Duty cycle | **89 %** |
| Gap rate | ~1.9 / s |
| Tone frequency | ~925 Hz (Goertzel), ~937 Hz (zero-cross) — the real ~1 kHz tone |

**Corrected interpretation:** this is a controller-buffer resynchronization caused by
the wrong stream clock, not a periodic host stall. The measured ~937 Hz frequency is
the decisive clue: a nominal 1 kHz/48 kHz Opus tone played on the controller's
effective 45 kHz clock becomes `1000 × 45/48 = 937.5 Hz`. The same mismatch makes a
20 ms two-frame producer overfill a stream the controller consumes every 21.3333 ms.

The test conditions that produced this measurement: **PC host** (not a Switch 2),
fixed-tone build, recording taken ~1 minute after pairing (well past the 30 s pairing
window).

---

## 4. Ruled out (with evidence)

| Hypothesis | Verdict | Evidence |
|---|---|---|
| **Live Opus encoder / its scheduling** | ❌ Ruled out | The **fixed-tone** build (no encoder, just replays two pre-encoded frames) has the *same* gaps. The encoder is not in the path that fails. |
| **DualSense stream-clock mismatch / buffer resync** | ✅ **Root cause** | This is not crystal drift. The controller protocol deliberately consumes nominal 48 kHz Opus at an effective 45 kHz clock. DS5Dongle implements this as 512 real 48 kHz samples → 480 encoded samples; daidr implements it as 45 kHz source data labeled 48 kHz. The recorded ~939 Hz tone independently matches the predicted 937.5 Hz. |
| **Classic sniff mode** (periodic radio anchors) | ❌ Not the cause | Sniff was globally enabled for idle power; it was disabled in audio builds (`gap_set_default_link_policy_settings` role-switch only). The gaps persisted. |
| **Config flash writes** (a `multicore_lockout` freeze fits ~57 ms) | ❌ Ruled out on PC | The only two `save_requested` triggers are the Switch 2 wake-identity `0x15` handshake (needs the console; has no callers on the PC path) and a config-mode CDC command. Neither fires during a PC audio test. |
| **ACL TX pipeline depth** | ❌ Doesn't apply | Deepened `MAX_NR_CONTROLLER_ACL_BUFFERS`/host buffers 3→12 for audio builds; no change. Root reason: the producer only ever holds **one report** (see §6), so nothing ever queues deep enough for buffer depth to matter. |
| **Background scan/inquiry as simple radio contention** | ❌ Not cleanly the cause / entangled | A gentle "stop scan when connected" (`bc647ae`) stopped scanning by the 1‑minute mark, yet the beep was unchanged. A "hard-suppress scan at connect" instead **killed all audio** — stopping scan during connection setup breaks the DualSense link entirely. So scanning is *entangled with connection establishment* but is not the periodic-gap source, and must **not** be aggressively stopped. |

> **Update 2026-07-18:** the 1-dongle-1-controller scope landed (`0798012`, `b6bc3c9`):
> discovery now idles once a controller is HID-ready and resumes at zero connections, keyed on
> `hid_ready` so it never stops scan mid-handshake (the flaw that made the "hard-suppress" attempt
> kill audio). Hardware-confirmed with no regressions (Classic + BLE reconnect, wake, wipe/re-pair).
> This removes the general scanning contention, but per the row above it is **not expected to
> resolve the 57 ms / 560 ms periodic stall** — that was shown independent of scan state. The stall
> meter (§8) remains the decisive next step.

---

## 5. Superseded timer-period hypothesis

The period ~540 ms appeared close to **18 × the 30 ms control-timer tick**
(`CONTROL_TICK_MS`), and the jitter (~520–560 ms) is ±1 tick. That points at a
**periodic blocking operation reached from the control-timer path** (or something
else on the core-1 BTstack run loop) that takes ~57 ms roughly every 18 ticks. The
57 ms duration is characteristic of a flash erase+program under
`multicore_lockout_start_blocking()` (freezes both cores) — but config flash is ruled
out on PC (§4), so the candidate would be **btstack's own TLV/bond flash bank**
(`btstack_tlv_flash_bank`, backing link-key + LE device DB) or another
`multicore_lockout` user. The later stall meter disproved this: core-1 and L2CAP
submission remain steady through the audible gaps. The reference-clock match and
recorded pitch now explain the apparent timer multiple without that hypothesis.

---

## 6. Key architectural facts learned

- **Both implementations use shallow producer queues.** PicoSwitch2 holds one
  ready `0x39`; DS5Dongle's raw, encoded-speaker, and haptics queues are each depth
  two. DS5Dongle drains a report whenever two Opus and two haptics frames exist.
  Queue depth is therefore not the key architectural difference; source cadence is.
- **The DualSense uses the direct-L2CAP path**, not HID-host: it is tracked in
  `wiimote_conn` with `hid_cid == 0xFFFF`, drained via `L2CAP_EVENT_CAN_SEND_NOW`
  into a bounded `direct_output_queue`. A 548-byte `0x39` report *requires* this path
  — the HID-host path caps reports at an 80-byte buffer.
- **Report `0x39` header bytes match DS5Dongle** exactly (`0x91`, `0xD2`,
  `buffer_length`×4 = 64, packet counter += 2 per report). Protocol framing is not
  the problem.
- **This is a BT-only firmware** (`pico_cyw43_arch_none`, no WiFi). The conservative
  `MAX_NR_CONTROLLER_ACL_BUFFERS 3` + controller-to-host flow control in
  `btstack_config.h` are pico-examples defaults sized for WiFi+BT SPI-bus
  coexistence, which does not exist here. (Relaxing them did not fix the gap, but the
  observation stands for future tuning.)
- **Stopping scan/inquiry at `ds5_init` (connection time) breaks the link.** Any
  scan-management change must run from a safe point (control timer) and never during
  connection setup.

---

## 7. Current conclusion and next validation

The host-side hypotheses are resolved. Hardware diagnostics repeatedly show core-1
max gaps near 6 ms, successful L2CAP submission below 27 ms, and submitted/completed
ACL totals tracking without loss. HCI completion notifications batch up to two
packets and can arrive about 70 ms apart, but changing the producer cadence changes
the acoustic cycle exactly as a controller-buffer rate error predicts.

The next test is no longer a fitted cadence experiment. Flash the reference-derived
tone build using buffer length 64 and an exact long-term 21.3333 ms report period. If
continuous, move directly to the live build, which now accumulates 512 real 48 kHz
frames, resamples them to 480, and encodes one controller-clocked Opus frame.

> **Hardware result:** the reference-derived tone remained completely solid with
> no gaps through checkpoints at 30, 60, 90, and 120 seconds. This confirms the
> stream-clock mismatch as the periodic-dropout root cause and rules out CYW43/HCI
> completion batching as an audible continuity fault under this load. Proceed to
> live Windows-audio validation with the same 512→480 conversion.
>
> **First live result after the clock fix:** fixed transport remains proven, but
> Windows audio is still severely chopped. Acoustic analysis of
> `dumps/audio/dualsense/live-initial-garbled.m4a` found about 49-64 ms audible
> clips separated by a median
> 507 ms silence (9.2% duty). Roughly 64 ms is three 21.333 ms reports, strongly
> indicating that the live producer supplies the verified transport in short
> three-report bursts. The next diagnostic increment measures USB PCM packet
> cadence/content, queue drops, Opus frame cadence/encode time, pipeline resets,
> and the already-existing L2CAP/HCI stages in one `audiostat` snapshot.

---

## 8. Tooling built for this investigation (in git history)

| Tool | What it does | Commit |
|---|---|---|
| `tools/analyze_audio_capture.py` | Decodes any recording (ffmpeg) and reports beep/gap/period/duty + tone frequency, pure stdlib. Turns "it beeps" into numbers. **Validated** against a synthetic 20/20 ms 1 kHz pattern. | `3ef8e4c` |
| **Stall meter** | `core1MaxGapUs` (core-1 tick interval) vs `sendMaxGapUs` (interval between `0x39` sends), fed from core 1. Read in config mode via CDC `audiostat` / `audiostat reset` and a "DualSense Audio Diagnostics" web-UI panel with a plain-English verdict (CORE-1 FREEZE / RADIO-SEND STALL / controller-side). | `d0ea159` |
| `docs/switch2/dualsense-audio-bridge.md` | Durable as-built reference: build modes, data flow, `0x39` layout, rate contract, threading. | `4a669c7` |
| `build.ps1 -Tone` / `-Audio` | Reproducible builds of the fixed-tone and live-Opus variants (RP2350 only, dedicated build dirs). | `e8f740d` |

**Recommended next step:** re-apply the stall meter (`d0ea159`) in isolation — it
never touched pairing — flash the `-Tone` build, let it beep ~15 s, then read the
web-UI verdict. One number settles core-1-freeze vs radio-block and ends the
guessing.

> **Implementation update (2026-07-18, current working tree):** the stall meter is
> re-applied on top of `4ebb073` without `bc647ae` or the ruled-out deep-ACL tuning.
> It is more precise than the reverted version: core-1 liveness is sampled at both
> the 2 ms audio timer and inbound HID report boundaries, so ordinary timer
> starvation cannot masquerade as a frozen core; the send interval is recorded
> only after a successful report-`0x39` `l2cap_send()`, not at earlier queue
> admission. Config mode exposes `audiostat` / `audiostat reset` and the web verdict
> panel. Safe dedicated `build.ps1 -Tone` / `-Audio` build directories are restored
> without restoring the ACL-buffer experiment. The clean tone build, live-Opus
> build, both ordinary board builds, and all 25 available host-test executables pass.
> Hardware measurement is the only remaining step for this diagnostic increment.

> **First stall-meter result:** over 3,476 audio reports (about 69.5 seconds),
> `core1MaxGapUs=75600` with only 3 gaps over 10 ms, while
> `sendMaxGapUs=24100` with no gaps over 40 ms. The original web verdict called
> this a core-1 stall, but that conclusion is contradicted by the simultaneous
> send cadence: a 75.6 ms core freeze during active streaming must also delay the
> next core-1-owned `l2cap_send()`, which did not happen. The three core gaps were
> therefore setup/pre-stream events, not the recurring roughly 560 ms audio
> defect. The diagnostic now scores core liveness only after the first audio
> report and separately measures the active ACL handle's HCI
> `Number Of Completed Packets` cadence. This distinguishes stable BTstack
> submission from CYW43/link-side completion batching. Audio behavior is
> unchanged by this refinement.

> **Hardware localization and buffer experiment (2026-07-18):** refined
> measurements show a steady core-1 run loop (max 5.6-6.9 ms) and steady BTstack
> submission (max 22.7-24.7 ms), while HCI completion events arrive as much as
> 65.7-69.8 ms apart in batches of at most two. Submitted and completed packet
> totals remain effectively equal; the completion total also includes other ACL
> traffic, so a small positive difference is expected. This rules out host-side
> report loss and local scheduling starvation. Completion-event timing alone is
> not an over-air arrival trace, but the fault is now localized after successful
> L2CAP submission.
>
> Raising the tone build's controller `AudioBufferLength` from 64 to 128 roughly
> doubled the continuous-tone interval. Adding a six-pair (120 ms) startup prefill
> made the next dropout arrive sooner. Both observations identify controller
> buffer overflow/resynchronization rather than underrun. Slowing only the
> deterministic tone from 20 ms to 22.5 ms increased the continuous interval to
> about two seconds. Quantitative analysis of
> `dumps/audio/dualsense/tone-22_5ms.m4a` found a
> 2.070 s median tone, 110 ms median gap, and 2.182 s median period (92.4% duty).
> Results were identical at 1 ms and 2 ms envelope windows. Combining that period
> with the earlier buffer-64, 20 ms / 560 ms-period result estimates the
> controller's equilibrium cadence at 25.9-26.0 ms per report pair. The next
> isolated tone build therefore tests 26.0 ms; live PCM pacing remains unchanged
> pending hardware confirmation.
>
> **26 ms correction:** hardware immediately disproved the linear-capacity
> assumption behind that estimate. `dumps/audio/dualsense/tone-26ms.m4a` has a stable main
> pattern of approximately 642 ms tone / 86 ms silence (about a 728 ms period),
> so 26 ms crossed to the underrun side instead of approaching zero drift.
> The 22.5 ms and 26 ms captures therefore bracket the actual cadence. Weighting
> their opposing drift times gives about 23.25 ms per report pair. The next
> tone-only build tests that value with `AudioBufferLength=128`; live audio remains
> unchanged.
>
> **23.25 ms refinement:** `dumps/audio/dualsense/tone-23_25ms.m4a` measured a stable main
> pattern of approximately 1.33 s tone / 74 ms silence (about a 1.418 s median
> period at a 2 ms analysis window). This is longer than the 26 ms result but
> shorter than the 22.5 ms result, placing 23.25 ms on the underrun side and
> narrowing the opposing-rate bracket to 22.5-23.25 ms. Weighting the two nearest
> measured drift times estimates zero drift at 22.79 ms. The next isolated build
> tests 22.8 ms.
>
> **Reference verification supersedes cadence fitting:** direct source review of
> current `daidr/dualsense-tester` and DS5Dongle gives the protocol clock exactly.
> DS5Dongle accumulates 512 samples from a real 48 kHz USB stream, resamples
> 512→480, encodes a nominal 10 ms Opus frame, and sends two frames per `0x39`.
> daidr independently resamples source audio to 45 kHz, labels it 48 kHz to the
> Opus encoder, and schedules one-frame `0x36` reports every `480/45000` seconds.
> Both therefore use 10.6667 ms per Opus frame and 21.3333 ms per two-frame
> `0x39`, with audio-buffer fields set to `0x40` (64). The acoustic captures
> corroborate this: the nominal 1 kHz diagnostic was recorded near 939 Hz,
> matching the expected `1000 × 45/48 = 937.5 Hz` when true-48-kHz PCM is encoded
> without the required conversion.
>
> The working tree now follows those reference semantics instead of fitting a
> cadence from beep periods: tone reports use an exact long-term `64/3` ms
> schedule, the live bridge restores its existing tested 512→480 resampler, and
> both tone and live reports use buffer length 64. The report format remains
> DS5Dongle's proven two-frame `0x39`; daidr's newer one-frame `0x36` format is
> valuable independent confirmation of the 45 kHz controller clock but is not
> needed for this transport.
>
> **Hardware result:** the reference-clock tone remained continuous with no gaps
> after 30, 60, 90, and 120 seconds. That validates the `0x39` format, controller
> setup, Bluetooth FIFO, and 21.3333 ms report cadence. Windows audio on the first
> live build was still severely chopped:
> `dumps/audio/dualsense/live-initial-garbled.m4a` measured roughly
> 49 ms audible bursts separated by roughly 471 ms gaps, with only 9.2% audible
> duty. The approximately 64 ms common burst is three valid `0x39` reports, so
> the fault moved upstream of the now-proven transport.
>
> A second line-by-line reference comparison found the remaining architectural
> difference. DS5Dongle accumulates a complete 512-frame USB source window on its
> USB core, queues two complete windows, and only then lets its codec core
> resample and encode them. `dualsense-tester` similarly decouples playback by
> pre-encoding the full source before scheduling reports. Our first live bridge
> instead crossed cores as individual 1 ms / 192-byte USB packets with only
> about 12 ms of queue slack, then assembled windows incrementally under
> Bluetooth load. The live bridge now mirrors DS5Dongle's producer contract:
> core0 assembles complete 512-frame stereo blocks, a depth-two whole-block FIFO
> crosses cores, and core1 consumes complete blocks into the existing two-frame
> Opus pair. The Bluetooth report FIFO was already depth ten, matching
> DS5Dongle, and remains unchanged.
>
> DS5Dongle also inherits TinyUSB's isochronous OUT behavior: its audio transfer
> callback always rearms the endpoint even when the preceding packet was lost.
> The custom UAC1 driver previously returned immediately on a non-success
> transfer, leaving speaker OUT unarmed until Windows recovered the interface.
> That is a direct mechanism for the recording's short PCM bursts and long
> silence. The callback now submits only successful payloads but rearms speaker
> and microphone endpoints after either result, matching TinyUSB's audio driver.
>
> Resetting counters in Config mode before every live test is not a useful
> validation workflow: audio exists only in the Pro Controller 2 personality.
> Reading the accumulated counters in Config mode *after* playback is useful and
> was subsequently confirmed to preserve the evidence needed below. Dedicated
> live, tone, ordinary Pico W/Pico 2 W builds and all 25 available host-test
> executables pass; the whole-block live path is awaiting one direct Pro
> Controller 2 hardware playback test.
>
> **Whole-block hardware result and corrected diagnostic conclusion:** the
> counters did remain readable after the test without an explicit reset, so the
> earlier assumption that Config mode necessarily loses them was incorrect.
> Over 21,194 continuous USB packets (maximum gap 1.4 ms), the bridge encoded
> only 276 frames and submitted 138 reports, while 1,710 complete PCM blocks
> were dropped. Opus took as much as 9.3 ms per frame; Bluetooth report gaps
> reached 235 ms and HCI completion gaps reached 6.5 seconds. This is not a USB
> delivery or queue-sizing problem. At that input duration approximately 1,987
> encoded frames were required, so the encoder/BT core produced only about 14%
> of real time and the depth-two queue correctly exposed the starvation.
>
> The final relevant DS5Dongle optimization had not yet been ported: its build
> relocates libopus code and read-only tables (roughly 220 KiB in that build)
> into SRAM because XIP cache stalls previously prevented real-time encoding at
> the stock 150 MHz clock. It also supplies SRAM-resident `memcpy`, `memset`, and
> `memmove`, and runs the codec wrapper/resampler from SRAM. Our live image was
> still running all of those from flash on the same core as Bluetooth. The live
> build now applies the same strategy: linked Opus code/tables, codec wrapper,
> 512→480 resampler, and common memory primitives all resolve to `0x200...`
> SRAM addresses. Binary inspection confirms the relocation. This leaves the
> USB and Bluetooth protocol behavior unchanged while removing the measured
> encoder-side XIP contention.
>
> **SRAM-relocated hardware result:** performance improved materially but was
> still below real time. Across 96,833 continuous USB packets, 3,104 Opus frames
> and 1,552 reports were produced; approximately 9,078 frames and 4,539 reports
> were required. Complete-block drops reached 5,974. Maximum encode time fell
> from 9.3 ms to 4.2 ms, proving the relocation worked, but core-1 gaps still
> reached 14.1 ms, L2CAP report gaps 111.2 ms, and HCI completion gaps 6.376 s.
> Every encoded pair was submitted (`3104 / 2 = 1552`), so the codec was being
> backpressured by the transport rather than failing or accumulating unsent
> frames.
>
> The live codec loop was still encoding both available frames consecutively in
> one BTstack timer callback. At the measured maximum that monopolized the
> Bluetooth core for roughly 8.4 ms before the run loop could service CYW43/HCI.
> It now encodes at most one frame per callback, guaranteeing a run-loop return
> and radio-service opportunity between frames. The 2 ms timer plus one 4.2 ms
> encode still has ample margin against the required 10.667 ms source-frame
> cadence. In addition, the encoder now uses
> `OPUS_APPLICATION_RESTRICTED_LOWDELAY`, matching daidr's explicitly required
> pure-CELT mode and reducing work on the shared core while preserving the same
> 200-byte, nominal-10-ms Opus payload accepted by the controller.
>
> **Interleaved-frame hardware result:** audio improved audibly and BT run-loop
> liveness became effectively healthy (10.3 ms maximum, one event over 10 ms),
> but real-time throughput still failed. Over 88,853 USB packets the codec
> produced 2,101 frames and submitted 1,050 complete reports, while 6,226 PCM
> blocks were dropped. The source required about 8,330 frames. The output pair
> remained the backpressure boundary, and matching 21.33-second maxima appeared
> across USB, Opus, L2CAP, and HCI during a stream reset/pause. This proves that
> merely interleaving Opus and Bluetooth on the same core is insufficient even
> after SRAM relocation.
>
> The live build now adopts DS5Dongle's actual core ownership rather than only
> its buffering and memory optimizations. Core0 initializes the CYW43
> threadsafe-background BTstack context and then runs the existing TinyUSB
> foreground loop; core1 owns only the SRAM-resident Opus worker. The former
> shared `encoded_pair` state is replaced by a thread-safe depth-two encoded
> frame FIFO. Each frame carries a pipeline generation so a connect,
> disconnect, or Windows alternate-setting reset cannot race an in-progress
> encode and combine stale/new frames. BOOTSEL's cooperative core1 park and the
> flash lockout victim move with codec/core1 in this live-only layout. Ordinary
> firmware and the tone build retain their established core ownership.

---

## 9. What broke pairing + the clean-build lesson

Commit `bc647ae` ("stop background scan/inquiry while a controller is connected") — a
shared BT scan/pairing state-machine change — broke DualSense pairing on hardware.
The whole `7d9d47d..d0ea159` range was reverted (`e6c43b0`).

Two process lessons:

- **A change to the shared BT scan/pairing state machine must be validated in
  isolation on hardware before stacking anything on top.** Four more commits were
  built on `bc647ae`, which made the regression hard to unwind.
- **After a `git revert`, always do a `-Clean` build.** An incremental rebuild
  reused stale ninja objects and produced a uf2 that still contained the reverted
  code, which looked exactly like "the revert didn't take." `build.ps1 <board>
  -Clean` produced a pure binary and pairing worked immediately. Verify the binary
  when in doubt: `strings build/<board>/*.elf | grep <reverted-symbol>` must be empty.

---

## 10. Commit map

All on branch `ns2-testing`:

| Commit | Contents | State |
|---|---|---|
| `7d9d47d` | Battery passthrough + experimental DualSense audio bridge (project owner's work) | ✅ **working baseline** |
| `4a669c7` | Audio-bridge as-built docs | reverted, recoverable |
| `e8f740d` | BT ACL pipeline tuning + `build.ps1 -Tone/-Audio` | reverted, recoverable |
| `bc647ae` | Scan-while-connected gating — **broke pairing** | reverted |
| `3ef8e4c` | Acoustic beep/gap analyzer | reverted, recoverable |
| `d0ea159` | Audio stall meter | reverted, recoverable |
| `e6c43b0` | Revert of the above back to `7d9d47d` | current HEAD |

To recover a reverted change without its regressions, cherry-pick the specific
commit onto a fresh branch and hardware-validate it alone.

---

## 11. Cross-references

- `docs/switch2/audio-passthrough-research.md` — original feasibility research and
  the DS5Dongle protocol/licensing survey.
- `docs/experiments/2026-07-17-dualsense-live-opus-failure.md` — the chronological
  live-Opus debugging narrative that preceded this summary.
- `nso-gc-refs/DS5Dongle/src/{audio,bt,btstack_config}.*` — the working reference
  implementation (MIT).
- `src/ds5_audio_bridge.c`, `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_bt.c`,
  `.../sony/ds5_audio_packet.c` — the bridge, transport, and report builder (at
  `7d9d47d` / reverted commits).

---

# Deep-dive research (2026-07-18)

Reference-implementation and hardware/protocol study to locate the root cause of the
periodic stall and map every audio path involved. **No firmware was modified.** Local
sources studied (full clones, pinned):

- `nso-gc-refs/DS5Dongle/` — [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle)
  @ `750bde8` (MPL/MIT). The working reference: same bridge, PC host instead of Switch 2.
- `nso-gc-refs/switch2_controller_research/` —
  [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research)
  @ `d1c5a7f`. Genuine Switch 2 controller protocol reverse-engineering.
- `dumps/SPI/2069_spi_dump_*.bin` — genuine Pro Controller 2 (`057E:2069`) SPI flash.
- `dumps/SWITCH2_JOYCON_{L,R}_1.bin` — genuine Joy-Con 2 SPI flash.

## 12. DS5Dongle architecture — how the working reference stays continuous

The single most important finding. DS5Dongle and PicoSwitch2 emit **byte-identical**
`0x39`/`0x32` reports (verified earlier), yet DS5Dongle streams cleanly. The difference
is entirely in **where code runs**.

### 12.1 Threading (`src/main.cpp`, `src/audio.cpp`)

```
DS5Dongle:
  core0  main() { while(1): cyw43_arch_poll()  // BTstack run loop
                            tud_task()          // USB
                            audio_loop()        // USB read + audio_bt_task() -> bt_write()
                            interrupt_loop() ... }
  core1  core1_entry() { while(1): speaker_proc()  // opus_encode
                                   mic_proc()    } // opus_decode
```

- **BTstack + the BT-audio transport + USB all live on core0's single flat, untimed
  loop.** `bt_write()` is called right after `cyw43_arch_poll()` every iteration, so a
  `0x39` goes out the instant the FIFO fills and the radio is ready — no 2 ms timer, no
  cross-core hop.
- **Only the Opus codec is on core1**, in its own tight `while(1)`. The encoder never
  competes with the BTstack run loop.

### 12.2 Pipeline (`src/audio.cpp`)

- USB OUT delivers **4 channels** (2 speaker + 2 haptics) @ 48 kHz. core0 `audio_loop`
  splits them: speaker → `audio_fifo` (512-frame float blocks); haptics → 48 k→**3 kHz**
  resample → int8 → `haptics_fifo`.
- core1 `speaker_proc`: `audio_fifo` → **adaptive WDL resample 512→480** (`SetRates(51200,
  48000)`, feed mode) → `opus_encode_float` (480 samples/10 ms, 200 B, VBR off,
  complexity 0, 160 kbps) → `audio_spk_fifo`.
- core0 `audio_bt_task`: when `audio_spk_fifo ≥ 2` **and** `haptics_fifo ≥ 2`, build the
  547 B `0x39` (2×64 B haptics + 2×200 B Opus) and `bt_write`. FIFO depth is **2**
  (shallow — same as PicoSwitch2). The send FIFO is depth **10**, `CAN_SEND_NOW`-drained
  — **identical** to PicoSwitch2's `direct_output_queue`.

### 12.3 What DS5Dongle does that PicoSwitch2 does not

| Aspect | DS5Dongle | PicoSwitch2 (live/tone bridge) |
|---|---|---|
| BTstack run loop | **core0**, flat loop | **core1** |
| BT-audio transport | **core0**, same loop as BTstack | core1, 2 ms timer + report boundaries |
| Opus codec | **core1, isolated** | **core1, shares the run loop with BTstack** |
| core0 workload | trivial (DualSense passthrough) | **full Switch 2 controller USB emulation** |
| USB→Opus rate | adaptive WDL resample 512→480 | direct 48 kHz accumulation, no resample |
| Haptics in `0x39` | **real 3 kHz data** | zeros |
| Send FIFO | 10-deep, CAN_SEND_NOW | 10-deep, CAN_SEND_NOW (same) |
| BTstack config | Classic-only, no BLE, no host flow control | BLE+Classic+ERTM, CYW43 flow control |

**Conclusion.** DS5Dongle structurally cannot suffer core-1 contention: the codec is
isolated and the transport rides the same flat loop as the radio poll. PicoSwitch2 runs
BTstack *and* the encoder on core1 while core0 is busy emulating a full Switch 2
controller — so anything that briefly monopolises core1 stalls the transport. This
directly supports the leading **core-1-freeze** hypothesis (§5–§7), and it explains why
even the *encoder-free tone build* still stalls: the blocker is a periodic core-1 event
(≈ 57 ms every ≈ 540 ms = 18 × the 30 ms control tick), not the encoder itself.

## 13. DualSense audio architecture (the BT-Classic side)

- Transport is **BT Classic HID output/input reports — not A2DP.** The host owns timing
  by pushing HID reports; there is no negotiated A2DP jitter buffer, so continuity is
  purely a function of steady `0x39` delivery.
- **`0x39`** (547 B, output): seq, `0x91`, len 6, mic flag `0x7E/0x7F`, buffer-length ×4,
  packet counter (`+= 2`), `0xD2`/64, two 64 B haptics blocks, route byte
  (`0x13` speaker / `0x16` headset, `| 0xC0`), `200`, two 200 B Opus frames, CRC32.
- **`0x32`** (142 B, output): the `SetStateData` control block — `AllowAudioControl`,
  `AllowSpeaker/Headphone/MicVolume`, `AllowMuteLight`, `SpeakerCompPreGain`, `MicSelect`,
  route, mute. Sent before streaming and on control change.
- **`0x31`** (input): gamepad; the controller **mic** is multiplexed in via header bit
  (`data[2] & 0x02`), with the 71 B Opus mic frame at `data+4`. Headset-present is
  `data[56] & 1` (raw, incl. `0xA1`) → byte 55 after BTstack strips the transaction byte;
  the adjacent bit distinguishes headphones from a microphone-equipped headset.
- Codec is **Opus** on every path (BT-Classic HID bandwidth cannot carry raw PCM):
  speaker 200 B/10 ms stereo; mic 71 B/10 ms mono.

## 14. Switch 2 native audio (the console side — what PicoSwitch2 emulates)

Genuine Switch 2 controller audio is **completely different** from the DualSense's, on
both transports (from `switch2_controller_research`):

- **Wireless (BLE GATT), Pro Controller only, firmware ≥ 2.0.0:** output handle
  **`0x002c`** (WRITE-NO-RESPONSE, console→controller headset audio) and input handle
  **`0x002e`** (NOTIFY, controller→console mic). Updated-firmware controllers add these
  attributes, shifting later handles by +8.
- **Input report `0x3F`** carries headset audio inline: state @ `0xD`
  (`0x07/0x0F` headset, `0x05/0x0D` headphones, `0x00` none), length @ `0xE` (always
  `0x32` = 50), then **50 bytes of "unknown format" audio** @ `0xF`. Not labelled Opus;
  likely a proprietary low-bitrate codec the on-controller DSP handles.
- **Wired (USB UAC1):** Audio Control + Audio Streaming interfaces, **2-channel speaker +
  1-channel mic** (`descriptors.md`). **This is the path PicoSwitch2 emulates** and
  bridges to the DualSense.
- So PicoSwitch2's job is: Switch 2 USB UAC1 PCM → DualSense BT-Classic Opus `0x39`. The
  console's native *wireless* audio format (BLE `0x3F`, 50 B chunks, DSP-processed) is a
  separate, unexplored avenue and is **not needed** for the USB-bridge approach.

### Bonded reconnect and conditional headset advertisement

Fresh Sony pairing uses PicoSwitch2's raw direct-L2CAP path. A bonded DualSense reconnects
through BTstack HID Host instead. BTstack's `hid_host_send_report()` length is only eight
bits, and PicoSwitch2's normal HID Host persistence buffer is 80 bytes, so that path could
not represent either the 142-byte `0x32` activation or 547-byte `0x39` stream report. Input
therefore reconnected while audio silently stopped.

The Pico 2 W audio build now records the control/interrupt CIDs of each already-negotiated
HID Host connection. Only Sony audio reports bypass the length-limited wrapper and use that
captured interrupt CID; ordinary controller output remains on the established HID Host path.
The audio state machine keeps ownership of retry timing, so a temporarily busy L2CAP channel
does not turn a failed immediate send into a false success.

DualSense report `0x31` status byte 55 is normalized as none, headphones, or headset and
crosses the existing input-event seam. Pro Controller 2 report `0x09` then emits `0x00`,
alternating `0x05/0x0D`, or alternating `0x07/0x0F`; report `0x05` mirrors the common
headset-present bit. The state is nonzero only while the physical DualSense jack is occupied,
preventing a bare controller speaker from capturing ordinary console audio.

The first real-console pass confirmed insertion, recognition, and console audio through
the physical jack, with input, rumble, and wake still functional. It also found that removal
stopped input for the remainder of the console session. The removal edge had been sending
another `0x32` transaction because byte 11 was incorrectly treated as a simple
speaker/headphone route. `SetStateData` instead identifies that byte as `AudioControl`
(`MicSelect` plus input/output channel paths); report `0x39` byte 140 is the
per-block speaker/headphone selector.

The next hardware pass supplied decisive A/B evidence. Leaving AudioControl at zero
made the console recognize the headset but produced no DualSense audio. Suppressing
the reverse transaction did allow ordinary rumble to return on removal, so that part
of the diagnosis was sound. Re-inserting the headset then stopped input and audio
until the dongle was unplugged. The RP2350 ISO allocation API keeps endpoints allocated
and does not actually close them at alt 0; the custom driver was reactivating and
rearming that still-live endpoint on alt 1. The revised build restores the known-audible
`0x02` headset AudioControl value, latches it for the Bluetooth connection so removal
never sends the harmful `0x30` reverse transaction, and preserves endpoint activation/
pending-transfer state across alt 1 → 0 → 1.

The same hardware pass confirmed that a separate legacy `0x31` rumble report causes
an audible gap while `0x39` audio is live. DS5Dongle establishes why: bytes 12..139
of every `0x39` packet are two 64-byte stereo signed-8 haptic PCM blocks at 3 kHz,
while bytes 142..541 are the two independent Opus speaker frames. Host coverage
asserts that nonzero haptics do not modify either Opus range.

The latest A/B hardware pass exposed a scheduling rather than a packet-layout fault.
A transient revision produced audio plus lighter native haptics. The subsequent
recent-flow gate produced full legacy rumble but no audio: recurring legacy `0x31`
generations could fill the ordinary output path before the first `0x39`, while the
code waited for that first accepted `0x39` before suppressing legacy output. The
current revision breaks that loop by reserving native audio/haptic ownership as soon
as the physical headset or USB speaker path requests it. Removal returns ownership
to the validated full-strength legacy path. Input, rumble, repeated unplug/replug,
and ordinary controller/dongle reconnect are hardware-confirmed stable; audio/haptic
coexistence and audio after bonded reconnect await focused validation.

### Source-boundary correction after the rejected full-waveform experiment

The genuine Pro Controller 2 USB descriptor rules out a hidden haptic-audio
channel: its speaker endpoint is ordinary 48 kHz, 16-bit, two-channel PCM.
Nintendo actuator commands remain on independent HID report `0x02`. DS5Dongle's
four-channel USB stream (speaker L/R plus haptic L/R) is a DualSense-facing PC
convention and must not be projected onto the Switch 2 endpoint.

A first attempt to preserve all Switch 2 frequency fields and synthesize a new
waveform changed enough hot-path code/state to regress both audio and haptics.
It was rejected in hardware and completely reverted; artifact SHA-256
`A890974E5EAB673257F98F10ABCBA02C8757AA25FE318D7B30E7B7F8CC259272`
is the byte-identical restored baseline.

The next candidate keeps that baseline's proven fixed-wave report construction.
It addresses two narrower facts:

1. Compatible report-`0x31` rumble sets DualSense's `UseRumbleNotHaptics`
   selector during the headset-free interval. Replugging previously replayed
   AudioControl without explicitly returning that selector to native PCM. The
   report-`0x32` control state now sets the rumble-emulation valid bit while
   leaving `UseRumbleNotHaptics` clear, and a genuine reinsert replays that
   ordered transaction.
2. The first native path was intentionally conservative. Its existing signed-8
   PCM is now scaled by the independently proven maximum 2× gain with saturation;
   zero-rumble packets remain byte-zero and Opus offsets are unchanged.

Hardware confirmed the selector correction: first-insertion audio, unplugged
legacy rumble, audio plus native haptics after replug, and repeated unplug/replug
all work without regressions. The 2× native PCM was clearly stronger than the
original but still lighter than DualSense's internal compatible-rumble path.
The next isolated candidate raises only that PCM curve to a conservative 3×;
all validated transport and state logic remains unchanged.

The 3× hardware pass again preserved every lifecycle and audio behavior. It felt
closer to legacy rumble and briefly reached the same perceived peak, but usually
remained lighter. Replaying ndeadly's genuine 4 ms report-`0x02` sequence against
the 64-frame/3 kHz (`21.333 ms`) DualSense packet cadence identified a temporal
reduction that gain cannot fix:

- latest-value sampling misses the interval peak in 74–78% of active packets,
  depending on relative packet phase;
- roughly one third retain less than half of the peak observed during that same
  packet interval; and
- mean retained magnitude is only 61–66% of the interval peak.

The next isolated candidate adds a scalar peak accumulator beside the existing
current rumble state. Console updates continue to drive legacy controllers exactly
as before; each native audio packet consumes the independent left/right maxima
observed since its predecessor. A new audio session resets the accumulator to the
live current value so stale headset-free peaks cannot leak into the first packet.
STOP can be held for at most one native packet (21.333 ms) when it arrives just
after a consume boundary. No frequency synthesis, Opus data, audio scheduling, or
DualSense packet layout changes.

Hardware found that envelope candidate fuller but still lighter than headset-free
compatible rumble. Importantly, direct comparison also found the headset/native-PCM
feel more accurate, so the follow-up deliberately preserves its 187.5 Hz waveform
instead of trying to imitate Sony's opaque compatibility synthesis. The genuine
capture's largest collapsed scalar is 68: the validated 3× curve renders a peak of
approximately 102/127. A small 13/4× (3.25×) adjustment raises that to 110/127
without clipping the observed sequence. This is only an 8.3% gain increase; packet
layout, peak accumulation, Opus data, selector state, and lifecycle logic remain
unchanged.

## 15. The `DSPH` DSP blob (`dumps/SPI/2069_*`)

- `DSPH` magic at flash **`0x175000`**; region `0x175000–0x1F9FFF` (`0x85000`), of which
  ~207 KB is real content (rest is erased `0xFF`). Header size field `0x00032ab0`
  (207024) matches. String **`MT3616A0 DSP`** at `0x175318`. Identical across both
  genuine Pro Controller 2 dumps.
- It is the **MT3616A0 audio-DSP firmware** for the controller's own 3.5 mm headset jack
  (MediaTek DSP/codec). `commands.md` exposes a "DSP firmware version" field, present only
  on updated-firmware Pro Controllers; `memory_layout.md` marks the region "DSP firmware …
  for the audio jack output."
- **Not usable by the Pico or the bridge**: it is target code for the MT3616 chip, not a
  USB audio engine and not an Opus implementation. The **Joy-Con 2 dumps contain no DSP
  blob** (`grep` for `DSPH`/`MT3616` → none) — consistent with their having no headset
  jack. This confirms the earlier note in `audio-passthrough-research.md` §3.1.

## 16. Bluetooth audio transfer — the two paradigms in play

- **A2DP** (standard BT stereo profile, SBC/AAC, negotiated buffering) is **not** used by
  any device here.
- **Vendor HID-report audio** is: DualSense `0x39` (Opus over BT-Classic HID) and Switch 2
  `0x3F` (proprietary over BLE GATT). In both, the *link owner* paces delivery via
  reports/notifications, so audio continuity is entirely at the mercy of report-scheduling
  jitter — exactly the failure mode PicoSwitch2 hits and DS5Dongle's flat-loop transport
  avoids.

## 17. Synthesis and recommended direction (no code changed)

1. **The stall is an architecture/scheduling problem, not a protocol problem.** Every
   protocol byte matches the working reference; the difference is core-1 contention on a
   platform whose core0 is committed to Switch 2 USB emulation and core1 to BTstack + the
   encoder.
2. **Decisive next measurement remains the stall meter** (§8): `core1MaxGapUs` vs
   `sendMaxGapUs` will confirm core-1-freeze vs radio-block. §12 predicts core-1-freeze.
3. **If core-1-freeze is confirmed**, the fixes worth evaluating (in rough order):
   - Find and remove/relocate the periodic core-1 blocker (the ~540 ms / 18-tick event —
     inspect everything the control timer and `btstack_host_process()` do per tick,
     including any `multicore_lockout`/flash path and BTstack housekeeping).
   - Move the *transport* (not the whole stack) so a queued `0x39` can reach L2CAP without
     waiting on the encoder or the timer — i.e., approximate DS5Dongle's "assemble-and-send
     right after the radio poll" flat-loop behaviour.
   - Only then consider matching DS5Dongle's secondary choices (adaptive 512→480 resample;
     real non-zero haptics) — lower-probability, but cheap to try once continuity holds.
4. **Out of scope for the bridge:** the MT3616 DSP blob and the Switch 2 wireless `0x3F`
   audio format — documented here for completeness, not required for USB→DualSense audio.

## 18. Live-audio core split: rejected after hardware regressions

The first attempt to reserve core1 for Opus moved Bluetooth initialization to core0 but
left the target on `pico_cyw43_arch_none` and never executed the BTstack run loop there.
Hardware made the failure unambiguous: the status LED, BOOTSEL gestures, and pairing were
all nonfunctional because their BTstack timers never advanced. Do not reuse that UF2.

A second attempt used `pico_cyw43_arch_poll` with `CYW43_LWIP=0` and explicitly called
the poll pump beside `tud_task()`. Although it compiled and passed the host suites, the
result did not enumerate as a controller on hardware. This proves that source/build tests
were insufficient validation for changing ownership of the SDK's USB and Bluetooth
contexts.

The entire core-split experiment was therefore reverted. Core0 again owns TinyUSB and
core1 again owns CYW43/BTstack, its timers, and cooperative Opus work on the established
48 KiB stack. The earlier audio improvements remain: complete 512-frame PCM blocks,
512→480 resampling, low-delay Opus, SRAM relocation, and the reference-derived report
clock. Future work must optimize or budget the established architecture without moving
Bluetooth into the USB execution context.

## 19. Clock A/B proves the live-audio drop point; foreground worker implemented

Hardware captures of the same continuous 1 kHz source at 150 and 200 MHz correlate exactly
with the PCM diagnostics:

| Clock | Expected PCM blocks (`USB packets × 48 / 512`) | Encoded | Queue-dropped | Encoded share | Recorded audible duty |
|---|---:|---:|---:|---:|---:|
| 150 MHz | 16,231 | 6,160 | 10,070 | 38.0% | 38.2% |
| 200 MHz | 16,781 | 14,608 | 2,170 | 87.1% | 87.1% |

Thus every silent interval is accounted for by a complete PCM block discarded before
encoding. The USB speaker interface stayed active across each playback (one on edge, one
off edge), and submitted `0x39` reports closely matched completed ACL packets. The live
fault is therefore not the source recording, Windows alternate-setting gating, the
DualSense decoder, or downstream packet loss.

Inspection of the Pico SDK `threadsafe_background` architecture changed the safe solution.
BTstack/CYW43 is serviced by a low-priority IRQ on core1; the foreground call to
`btstack_run_loop_execute()` otherwise waits. Live Opus had been run from a 2 ms BTstack
timer callback, making encoding wait for and then occupy the Bluetooth context.

The current live build instead runs a non-returning
`ds5_audio_bridge_codec_worker()` in core1 foreground. It blocks directly on the PCM
queue, wakes on core0's producer notification, and immediately resamples/encodes one real
block. The background Bluetooth IRQ may preempt it and the 2 ms timer remains responsible
only for short maintenance and transport of completed frame pairs. USB remains on core0;
BTstack initialization, ownership, and service remain on core1. This is not the rejected
core split.

Clean hardware-test variants remain available at 150 MHz and 200 MHz/1.20 V; the
standard Pico 2 W build uses the hardware-confirmed 300 MHz/1.20 V configuration.
CYW43 PIO is held at or below its ordinary 75 MHz rate (divider 3 at 200 MHz and
divider 4 at 300 MHz). The lower clocks are retained as non-working comparisons
for diagnosing the real-time threshold.

The Pico W port used the same 300 MHz/1.20 V system clock but a different codec
placement: fixed-point Opus from XIP flash, a 36 KiB core1 stack, and `/4` boot2
flash/CYW43 dividers. Although it cleared compile and memory gates, hardware
playback barely worked. The experiment is retained here as negative evidence;
all Pico W audio, overclock, codec, stack, and divider changes were removed from
the standard artifact.

> **Hardware result:** the 200 MHz foreground-worker image remained audibly choppy.
> The 300 MHz image sounded continuous/perfect by ear. Its accounting is complete:
> 141,070 USB packets correspond to 13,225.3 complete 512-frame blocks; the worker
> made 13,225 calls, dequeued and encoded 13,225 frames, reported zero errors and zero
> dropped PCM blocks, and submitted 6,612 two-frame audio reports. Queue depth peaked
> at one. The single remaining encoded frame is the expected odd frame at the end.
>
> The displayed `USB PCM DELIVERY STALL` verdict is a stale-threshold false positive.
> It is triggered by one 992 ms historical maximum even though no delivered PCM was
> lost. The `>10 ms` codec-call count is also expected under the blocking worker because
> source blocks arrive every 10.667 ms; that counter was meaningful for the former 2 ms
> polling callback, not the new producer-paced loop.
>
> **300 MHz regression result:** LED and BOOTSEL behavior passed; mapping/color
> configuration persisted and read back after reconnect; cold boot passed; and console
> wake passed ten attempts with every known controller. Real-console headset insertion
> and audio output are confirmed, including input/rumble/wake while connected. The
> corrected unplug transition and in-band haptic PCM build remain hardware-pending.

### Sources
- `nso-gc-refs/DS5Dongle/src/{main,audio,bt,btstack_config}.*` @ `750bde8`
- `nso-gc-refs/switch2_controller_research/{bluetooth_interface,hid_reports,commands,descriptors,memory_layout}.md` @ `d1c5a7f`
- `dumps/SPI/2069_spi_dump_*.bin`, `dumps/SWITCH2_JOYCON_{L,R}_1.bin`
