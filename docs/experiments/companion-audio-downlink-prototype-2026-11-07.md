# Companion audio prototype: downlink and microphone uplink

**Date:** 2026-11-07
**Branch:** `linux-companion`
**Status:** Partial.

- **Confirmed on hardware (first pass):** audible playback on Windows.
- **Found on hardware (second pass):** audible degradation and late inputs with
  a Bluetooth Classic DualSense Edge as the controller. The second- and
  third-pass fixes are implemented and software-tested.
- **Fourth pass, software only:**
  - the headset is scoped to Controller Link;
  - drift-controlled playout;
  - Android playback;
  - the companion **microphone uplink**, on Windows and Android.
- **Fifth pass, software only:** both audio planes on the **Linux** companion
  (SDL3 playback and capture, BlueZ GATT), and a firmware request that caps
  the management link at 15 ms while a session runs (BlueZ otherwise keeps
  30–50 ms). All three companions now have the headset and the microphone.

Everything after the first pass awaits hardware validation (procedure below).
The speaker stays at 96 kbit/s and the microphone runs at 64 kbit/s mono; both
are requirements (see Rules).
**Builds on:** [`companion-multiplane-ble-architecture-research-2026-09-04.md`](../architecture/companion-multiplane-ble-architecture-research-2026-09-04.md)
§9.4, §12.3, §22 (steps A–E) and §27 (F).

## Question

Can the console's audio play on a companion over the **existing trusted
management BLE link**? The management link already carries the JSON command plane
and the Controller Link pair. This would add a third plane on the same link, with
no second link and no Classic profile, and it must not cause any regression in
Controller Link or in controller audio.

## Background

- The console→adapter half has existed and been hardware-validated since
  2026-07: UAC1 speaker PCM is received in `switch_pro2.c`.
- The adapter's direct-CELT encoder has been hardware-validated at **240 B /
  20 ms / 96 kbit/s CBR** on the genuine Pro Controller 2 path. That validation
  sustained real console PCM on core 1 while BTstack streamed GATT writes
  ([`pro2-headset-audio.md`](../switch2/pro2-headset-audio.md)).
- Windows Path C already qualified a binary GATT plane beside management on one
  ACL ([`windows-controller-link-qualification-2026-09-03.md`](windows-controller-link-qualification-2026-09-03.md)).
- **Gate found during this pass (REPO):** the Switch opens its USB speaker stream
  to a Pro Controller 2 only while report `0x09` byte `0x0D` advertises an
  occupied jack (`switch_pro2.c`, `controller_headset_switch2_state()`). A
  companion plane therefore has to make the adapter advertise a jack.

## Hypothesis

Reusing the validated Pro Controller 2 encoder geometry unchanged and
retargeting its output from a controller GATT write to a companion GATT
notification will produce continuous audio on Windows. The research's §11
estimate puts the plane at about 35% of the link's airtime, so the hypothesis
includes no measurable regression to the Controller Link counters while it runs.

Deliberate deviation from the research's 64 kbit/s "opening bid" (§11.2): the
first experiment keeps 96 kbit/s, so the core-1 budget question (§24 #9) is
already answered by existing hardware evidence. Anything that fails is then the
transport or the playback, not the codec. Lowering the bitrate is a one-constant
follow-up.

## Design as implemented

```text
console UAC1 PCM ──> ds5_audio_bridge (shared producer, core 0)
                       │ companion_audio_active ⇒ Pro2 encoder geometry
                       v
                 direct-CELT encode, core 1 worker ──> pro2_encoded_frame_queue (depth 2)
                       │
  2 ms audio timer (core 1, BTstack ctx): btstack_host_service_companion_audio()
                       │ [ver][0x90][seq16] + 240-byte Opus packet = 244 B
                       v
        GATT notify on AUDIO-DOWN (management service, same bond)
                       │
  Windows: GattCompanionAudioDownlink ─> CompanionAudioReceiver (gap → PLC, cap 3)
                       ─> Concentus Opus decode ─> WaveOut sink (prebuffer 3, cap 6 × 20 ms)
```

| Piece | Where |
|---|---|
| Wire contract, admission order, headset rule (pure, host-tested) | `firmware/src/management/ns2_companion_audio.{h,c}`, `tests/firmware/test_ns2_companion_audio.c` |
| Companion consumer in the encoder | `ds5_audio_bridge_set_companion_active()` in `firmware/src/audio/ds5_audio_bridge.c` |
| Headset advertisement | `switch_pro2.c` at the output boundary, on every report 0x09 / 0x05 (third pass; was `ns2_seam.c`) |
| GATT characteristic `a31d6e52-9b04-4c8f-8e27-d5613af0b94c` (notify, encrypted CCC) | `setup_att_server()`, `btstack_host.c` |
| Session, send service, teardown on management loss, controller-headset preemption | `btstack_host_companion_audio_*()`, `btstack_host.c` |
| Control plane `caudio start\|stop\|status\|diag` (wireless-allowlisted) | `cmd_caudio()` in `firmware/src/settings/config.c`; `config_wireless_bridge.c` |
| Source-controller jack published to the plane | `ns2_seam.c` → `ds5_audio_bridge_note_source_headset()` |
| Adaptive jitter buffer (pure, tested) | `CompanionAudioJitterPolicy`, `PicoSwitch.Companion.Platform/CompanionAudio/` |
| Contract fixture | `contracts/management/protocol-v1.json` → `ble.companionAudioDownUuid` |
| Windows frame codec | `PicoSwitch.Bridge.Core/Protocol/CompanionAudioDataPlane.cs` |
| Receive, decode, lifecycle | `CompanionAudioReceiver`, `ConcentusCompanionAudioDecoder`, `CompanionAudioService` (Services, portable) |
| GATT downlink, playback | `PicoSwitch.Companion.Windows/CompanionAudio/` |
| UI | Play page **Headset on/off** toggle |

### Rules encoded

- **The packet is a public Opus packet.** TOC `0xFC` means CELT-only, full band,
  20 ms, stereo, one frame. The companion uses a standard decoder. The host test
  decodes the genuine console idle packet (`FC FF FE` + zeros) and firmware-shaped
  CBR packets.
- **96 kbit/s is a floor (maintainer requirement, 2026-11-07).** Companion audio
  must match the console's own Pro Controller 2 headset quality: 96 kbit/s CBR,
  240-byte packets. Lower rates are not offered at all; `admit: rate` refuses
  them, and the host test pins it.
  - A 64 kbit/s default was built once, to ease radio contention with a Classic
    DualSense. It was rejected before any hardware run: transport problems get
    fixed in transport, scheduling and buffering, never by spending quality.
  - `caudio status` still reports `kbps` and `frame_bytes`, and the companion
    sizes its receiver from them, so a higher rate later needs no companion
    change.
  - The DualSense path encodes 160 kbit/s in 10 ms frames (200-byte packets),
    the DualSense's own format. The companion plane does not match that number
    and is not claimed to.
- **The controller's own headset wins.** `start` is refused
  (`controller_headset`) while the controller in use reports an occupied jack.
  A jack occupied mid-session ends the session (`preempted_controller_headset`)
  and the console's audio moves to that controller's audio path. The jack is
  cleared when that controller disconnects. The app has no end-of-stream
  message to wait for, so after 2 s without a frame it asks `caudio status`
  once and shows the reason if the session has ended. A quiet console pauses
  its stream for seconds at a time, so silence alone never ends a session.
- **Geometry is reported, not assumed.** `caudio status` reports the codec, sample
  rate, channel count, frame duration and frame size. The Windows app refuses a
  mismatch instead of playing noise.
- **Exclusive with controller audio.** The plane is refused (`encoder_busy`)
  while a genuine Pro Controller 2 headset owns the encoder. If that headset
  becomes live mid-session, the plane yields with `last_stop: preempted_pro2`.
  While the plane is active, `ds5_audio_bridge_speaker_requested()` is false, so
  a DualSense stays on its ordinary rumble path rather than reserving report
  `0x39` for audio it will never receive.
- **No plane outlives its session.** The adapter stops on management loss
  (`mgmt_lost`), which clears the advertised jack. The app tears down its local
  half on management loss without sending anything.
- **Bounded everywhere.** The encoder queue has depth 2 and drops the oldest
  frame. The source PCM queue holds 4 blocks (42.7 ms) for the companion
  consumer and the validated 2 for every controller consumer. The adapter waits
  for an ACL slot instead of queuing. The receiver conceals at most 3 frames.
  The sink starts at a 60 ms prebuffer, grows it by 20 ms per underrun up to
  120 ms, gives 20 ms back after 30 s without one, and holds at most
  prebuffer + 60 ms.
- **Pico W:** the characteristic exists so the GATT database is identical on
  both boards, but `start` answers `admit: unsupported`.

## Validation performed

| Kind | Result |
|---|---|
| Firmware host suite (`tests/run_host_tests.ps1`) | 82/82, including the new `test_ns2_companion_audio` and the `caudio` allowlist assertions |
| Firmware builds | Pico 2 W and Pico W both build; install-reset markers verified |
| Windows suites (Bridge.Core, Management.Core, Companion.Services, Companion.Windows) | 287 / 170 / 702 / 127 passed |
| Linux companion suite (shares Services) | 109 passed |
| WinUI app build (x64 Debug) | succeeds |
| **Hardware, first pass (2026-11-07)** | **Audible on Windows: confirmed by the maintainer.** Counters below |
| **Fourth pass (software only)** | |
| Firmware host suite | 84/84, adding `test_ns2_companion_mic` (wire, ring, 1 h link simulation) and `test_companion_mic_decode` (libopus CELT on the host against the Windows encoder's vectors) |
| Firmware builds | Pico 2 W and Pico W build with no warnings; install-reset markers verified |
| Windows suites (Bridge.Core / Management.Core / Companion.Windows / Companion.Services) | 295 / 172 / 127 / 757 passed; WinUI app builds |
| Linux companion suite | 109 passed |
| Android (management-core / app unit tests, `assembleDebug`) | 162 / 541 passed; the APK builds |
| Command parity (`tools/check_management_command_parity.py`) | OK: 70 commands, all dispatched and reachable over BLE |
| Hardware | **None this pass** |
| **Fifth pass, Linux (software only)** | |
| Firmware host suite | 84/84; `test_ns2_bt_lifecycle` pins the companion-audio link parameters (≤ 15 ms, floor 7.5 ms, latency 0, 6 s timeout kept) |
| Firmware builds | Pico 2 W and Pico W build with no warnings; install-reset markers verified |
| Linux companion suite | 115 passed, adding `BluezCompanionAudioTests` (AUDIO-DOWN subscribe/deliver/unsubscribe, MIC-UP write-without-response order, failed write counted and not retried, missing planes, no plane without a live link) |
| Companion.Services suite | 764 passed, adding `HeadsetLineTests` (the Play words now shared by Windows and Linux) |
| SDL3 audio on a real Linux device | **Not run.** A harness drives the shipped sink and capture; the Ally X went offline before it ran |
| Hardware | **None this pass** |

### First hardware pass — 2026-11-07

**Environment.** Pico 2 W, firmware `e054bfd1+dirty` (this working tree), Switch 2
attached over USB, Windows companion Release x64 from the same tree, connected
and trusted over the management BLE link. UART on COM6 (CP210x) via
`tools/read_uart_diag.ps1`; `caudio status` reached through the `cfg` passthrough.
At sampling time no Bluetooth controller was the active input source
(`input status` VID/PID `0x0000`) and Controller Link was not streaming.

**Observed (maintainer):** console audio stopped on the TV and played from the
PC. Audio quality and latency have not yet been assessed in writing.

**Observed (UART):**

| Counter | Value | Reading |
|---|---|---|
| `att_mtu` / `min_att_mtu` | 527 / 247 | One frame per notification, no fragmentation |
| `frames.sent` over 10.77 s | 6732 → 7270 (538) | **49.96 frames/s**: the full 20 ms cadence |
| `frames.busy` / `failed` / `rejected` | 0 / 0 / 0 | The ACL never refused a notification at this load |
| `send_max_gap_us` | 15,500,968 | One 15.5 s pause, matching `pcm_max_gap_us` 15,478,893, so the console paused its PCM stream; not a transport stall. Use `usb_edges_on_off` [3, 2] to tell which |
| `pcm_dropped` / `pcm_short` | 0 / 0 | No producer overrun |
| `opus_errors` | 0 | over 7,303 encodes |
| `opus_encode_max_us` | 7,212 | About 36% of the 20 ms frame budget at worst |
| `opus_over_20ms` | 5,612 | Expected, not a fault: PCM crosses to core 1 in 512-frame (10.67 ms) blocks, so a 960-sample encode usually waits for two blocks (21.3 ms) |
| `core1_stack_free` | 10,940 B | Headroom remains on the 48 KiB codec stack |
| `codec_no_encoder` / `codec_usb_inactive` | 0 / 0 | |

An earlier session in the same boot ended `last_stop: mgmt_lost` (generation 2
is the current session), which exercised the management-loss teardown once. Its
console-side effect, audio returning to the TV, was not separately recorded.

**Conclusion of this pass:** the hypothesis's transport half holds at the
validated 96 kbit/s geometry with no adapter-side backpressure at all. The
regression half is **not yet tested**: these counters were taken with Controller
Link idle and no Bluetooth source controller, which is the easiest radio case.

### Second hardware pass — 2026-11-07

Same build and environment as the first pass. Every observation below is the
maintainer's, by ear and by hand:

| Scenario | Observed |
|---|---|
| Touch gamepad over Controller Link, Headset on | Sounded OK |
| Controller Link and touch gamepad off; DualSense Edge over Bluetooth Classic as the controller; Headset on | Audio streams, but chops, the level dips and recovers, and it skips. Inputs sometimes arrive late |
| Same, rumble | Worked normally (the DualSense stayed on its ordinary path) |
| Close the companion app with Headset on | Console audio returned to the TV immediately. This confirms the `mgmt_lost` teardown on the console side |
| Headset into the DualSense while companion audio plays | **Not tested** |

UART after the DualSense session. `caudio` counters cover that session
(generation 4); `audio status` counters are cumulative since boot:

| Counter | First pass | After the DualSense session | Reading |
|---|---|---|---|
| `caudio` busy / failed / rejected | 0 / 0 / 0 | 0 / 0 / 0 | The adapter never waited on the ACL |
| `caudio send_max_gap_us` | (pause only) | 67,590 | Over the 60 ms prebuffer: at least one audible sink underrun |
| `pcm_dropped` | 0 | **150** | 1.6 s of source PCM lost before the encoder saw it |
| `opus_encode_max_us` | 7,212 | **52,703** | Core 1 left an encode for about 45 ms |
| `core1_gap_max_us` | — | 6,237 | BTstack itself stayed responsive |

**Interpretation.**

- **Strong Evidence:** the chop and skips originate in the adapter, before
  transport. Zero `busy` rules out ACL backpressure. A dropped source block
  leaves the sequence numbers continuous but cuts the audio, which is heard as
  a skip with no concealment.
- **Hypothesis:** the 52.7 ms encodes caused the PCM drops. A 2-block source
  queue (21.3 ms) cannot survive a 45 ms absence. Two causes for the absence
  remain open:
  - BTstack IRQ work for the Classic link preempting the core-1 encode;
  - a flash write's multicore lockout, such as the bond save when the DualSense
    connected.

  The counters are cumulative since boot, so the drops cannot yet be attributed
  to the audio session rather than the connection event. The new `caudio diag`
  encoder block is reset per session to settle this.
- **Hypothesis:** the late inputs are radio airtime contention. The CYW43 is
  shared by the Classic DualSense link and the BLE notification stream.
  Nothing measured it: input-path latency has no counter here. The
  management link's connection interval and data length were also unknown,
  and `caudio diag` now reports both.
- **Hypothesis:** the level dips are Opus concealment fading in and out, or
  CELT's transient handling across a PCM discontinuity. The app's
  `session end` line (lost, concealed, underruns) distinguishes them; it was
  not captured.

**Changes made in response.** Each is software-tested; none is hardware-validated:

1. **Bitrate: unchanged at 96 kbit/s.** Lowering it was considered and rejected
   (Rules).
2. **Source PCM queue 2 → 4 blocks for the companion consumer only**, enough to
   ride a 42 ms core-1 absence. The DualSense and Pro Controller 2 headset
   paths keep their validated depth.
3. **`caudio diag`**, reset per session:
   - encoder max and over-20 ms count, plus PCM and queue drops;
   - send gaps over 30 and 60 ms;
   - link interval and LE data length;
   - the source jack.
4. **Adaptive jitter buffer** in the app (60 → 120 ms on underruns).
5. **Controller-headset priority** (Rules, above), plus the app's silence
   watchdog that notices an adapter-ended session.

**Superseded (fourth pass): the headset is now scoped to Controller Link by
maintainer decision.** As first written: *Not done, deliberately: locking
companion audio to Controller Link.* The maintainer suggested it as a possible
fix for the late inputs. It would remove
the configuration this feature is most useful for: a real controller in hand,
audio on the PC's headphones. It is the fallback if the measurements below show
the radio cannot carry a Classic controller and the 96 kbit/s plane together.

### Third hardware pass — 2026-11-07

Build with the second-pass changes and 96 kbit/s. Maintainer observations:

| Scenario | Observed |
|---|---|
| Controller Link + Headset on | No noticeable delay; audio steady |
| Controller Link off, no other source + Headset on | Audio steady |
| Controller Link off, no other source, Headset on → off | **Audio did not return to the console.** The headset stayed advertised; only the companion stopped accepting audio |
| DualSense Edge + Headset on | **Worse than the second pass:** noticeable gaps |

Plus one console observation. Audio played only once Controller Link had made the
adapter controller 1, and stopping Controller Link does not unassign it.

**UART** (session 4, no DualSense, `caudio diag`):
- 11,853 frames sent; `send.max_gap_us` 29,349; 0 gaps over 30 ms;
- link: `interval_us` 15,000, `tx_octets` 251 (one link-layer packet per frame);
- encoder: max 10,208 µs; 0 over 20 ms; 0 PCM drops; 3 queue drops.

Boot-cumulative counters, which include the DualSense session:
- `pcm_dropped` 429 even with the 4-block companion queue;
- `opus_encode_max_us` 59,197.

**Findings.**

- **Confirmed (repository + hardware): the jack was applied in the wrong layer.**
  The companion's virtual jack was set in `ns2_seam.c`, which runs only when a
  source controller sends an input event. With no source sending, Headset off
  never reached report `0x09`.
  - **Fix:** the companion jack is now resolved at the output boundary, in
    `switch_pro2.c` on every report 0x09 (byte 0x0D) and report 0x05. The seam
    carries only the physical jack.
  - Headset on/off now behaves like plugging a headset into the controller and
    pulling it out, whatever is sending input.
  - Hardware validation pending.
- **Confirmed (repository): the adapter's USB attach never depends on a source.**
  `usb.c` attaches at boot and re-enumerates only on a personality change.
  Losing Controller Link therefore leaves the adapter as the console's
  controller 1, as designed.
- **Hypothesis (one observation): the console opens its speaker stream only for
  a controller that holds a player slot.** A source has to produce input once,
  for example Controller Link or a button press, before companion audio plays.
- **Disproven: UART logging stalls core 1.** The DualSense driver logs on every
  output report, which made blocking UART output inside BTstack an attractive
  explanation for 45 ms encoder absences. It cannot be: `CMakeLists.txt`
  disables both stdio drivers (`pico_enable_stdio_uart 0`, `pico_enable_stdio_usb 0`),
  so `printf` formats and discards.
- **Strong Evidence: with a DualSense connected, core-1 encodes are preempted
  into 50–60 ms of wall time.** The deeper queue does not absorb that, and it
  does not happen without a DualSense (session 4: 10 ms worst case). The
  DualSense speaker path already handles exactly this condition in production:
  it holds the CYW43 async-context lock across each encode so the radio worker
  defers instead of preempting repeatedly.
  - **Change:** the same policy for the companion encode, applied only while a
    DualSense is connected (`bridge_connected`).
  - **Cost:** radio work waits for at most one encode (about 10 ms) per 20 ms
    frame.
  - **A/B:** `caudio lock on|off`, reported in `caudio diag` as `lock` and
    `lock_applies`.
  - Hardware validation pending.

## Fourth pass — scope, playout, Android, microphone (2026-11-07, software only)

No hardware run. The maintainer asked for this pass to be finished "by the
math, the numbers, the code" before the next flash. Everything below is
implemented and tested in software; **none of it is hardware-validated.**

### Scope: the headset belongs to Controller Link (maintainer decision)

The second pass deliberately did not tie companion audio to Controller Link.
The maintainer has now decided it: the goal is a phone or PC that IS a full
Switch 2 controller, and its headset jack goes with it. The second-pass
"not done" note is superseded.

- `start` is refused with `admit: no_controller_link` unless this
  management peer is streaming Controller Link, and with `other_controller`
  while another source drives the console.
- The session ends with `last_stop: controller_link_ended` or `source_changed`
  when that stops being true. Both apps grey the toggle out without Controller
  Link.
- **Strong Evidence (two maintainer observations: third pass, and the
  statement that "it doesn't switch over till the first button press"):** the
  console opens its speaker stream only once the controller has produced
  input. Both apps show "press any button so the console picks it up" until
  the first frame arrives (`AwaitingConsole`).

### Speaker playout: drift-controlled, simulated end to end

The first jitter buffer could only grow or shrink a whole 20 ms block, and the
console's USB clock and the device's audio clock drift apart. The playout now
holds a constant queue depth by stretching each block to 960 ± 1 frames:
`CompanionAudioDriftControl` (Windows) and its Kotlin twin. The parameters are
an EMA of 1/50 over the queue, 50 settle blocks, a 4 ms deadband and 1 ms
release. A block is grown with one concealment block before the queue runs
dry (low water 10 ms).

`CompanionAudioPipelineSimulationTests` models the whole path:

- console PCM → encoder → 2 ms service tick → connection events → host jitter
  → the real playout policy → a device clock;
- one hour of audio per seed, three seeds per profile.

Its ASSUMPTIONs are stated in the test:

- up to 2 frames per 15 ms connection event;
- 2 % of events empty on BLE, 20 % with a Classic HID link on the radio;
- host jitter of 8 ms;
- clocks 600 ppm apart.

Output of this pass:

| Profile | Underruns per hour (after 10 min warm-up) | Latency p50 / p99 |
|---|---|---|
| BLE Controller Link (Windows/Linux) | 0 (0) on every seed | 103 / 106 ms |
| Android: Classic HID plus the exclusive encode | 1–2 (0–1) | 144–162 / 165–184 ms |
| Stress: worst encode every frame, 33 % empty events | 1–6 (0–5) | 201 / 226 ms |
| Pass-2 DualSense with a preemptible encode (rejected) | 33, plus 162 PCM drops | — |

Negative knowledge kept in the suite:

- one PDU per event at 33 % loss cannot keep up;
- without drift correction, a gap appears every few minutes.

### Microphone uplink

"Headset on" now offers the headset's microphone: this device's microphone
becomes the controller headset's, so console voice chat hears it.

```text
companion mic (48 kHz mono) ─> Opus CELT-only 64 kbit/s CBR, 20 ms (160 B, TOC 0xF8)
  ─> [ver 1][0x91][seq16] + packet = 164 B ─> bounded writer (8 deep, one in flight)
  ─> GATT write-without-response on MIC-UP d4a7c3e1-5b2f-4e86-9c0d-71e3a5b8f2c6
adapter: BTstack (core 1) ─> parse ─> queue (4, drop oldest) ─> codec worker (core 1,
  after any speaker block) ─> libopus CELT decode (PLC ≤ 2 for gaps) ─> playout ring
  (8192 frames, drift law as above) ─> UAC1 mic EP 0x83, 48 stereo frames/ms, FU 0x05 mute/volume
```

| Piece | Where |
|---|---|
| Wire contract, sequence/concealment rules, playout ring (pure) | `firmware/src/management/ns2_companion_mic.{h,c}`; `tests/firmware/test_ns2_companion_mic.c` |
| CELT decode wrapper | `firmware/src/audio/companion_mic_decode.{h,c}` |
| Decode/queue/USB bridge, diagnostics, heap figure | `firmware/src/audio/companion_mic_bridge.{h,c}` |
| Characteristic, write handler, re-plug | `btstack_host.c` (`caudio_on_mic_frame`, `caudio_advertise_jack`) |
| Headset vs. headphones | `ns2_companion_audio_headset_state(source, active, mic_active)` |
| Commands | `caudio start mic`, `caudio mic`, `caudio mic decode`; `status` adds `mic` and `mic_supported` |
| Cross-codec fixture | `contracts/companion_mic_vectors.txt`, written by `CompanionMicTests` and decoded by `tests/firmware/test_companion_mic_decode.c` |
| Windows | `CompanionMicDataPlane`, `CompanionMicWriter`, `CompanionMicSession`, `WaveInCompanionMicCapture`, `GattCompanionMicUplink`; Controller page **Mic** toggle |
| Android | `management/audio/CompanionMic.kt`, `AndroidCompanionMic.kt` (AudioRecord, Concentus), `BleGattManagementTransport.writeCompanionMic`; Play **Mic** switch |

**Quality.**

- 64 kbit/s mono is above the DualSense's own microphone stream (71 B per
  10 ms = 56.8 kbit/s). A headset microphone is one capsule, and 64 kbit/s is
  per channel above the 96 kbit/s stereo downlink.
- Cross-codec, host-measured: Concentus encode → the firmware's libopus decode
  → the playout ring → USB packets reproduces a 1 kHz tone at **−0.00 dB with a
  0.9999 tone share** on both channels.

**Rules.**

- **Accessory.**
  - `caudio start mic` advertises a HEADSET (0x07/0x0F) instead of HEADPHONES.
  - Changing the microphone setting on a running session withholds the jack
    for `NS2_CAUDIO_REPLUG_MS` (200 ms), then advertises the other accessory.
  - **Hypothesis:** the console needs the jack to go NONE → occupied to notice
    a changed accessory.
- **The speaker is never the microphone's hostage.**
  - A build that cannot decode refuses with `admit: mic_unsupported`; both
    apps then start speaker-only.
  - A mic that will not open, or firmware without MIC-UP, re-plugs the adapter
    as headphones, so the console does not wait on a silent microphone. The
    reason is shown under the toggle.
  - The worker serves the speaker block first.
- **Bounded everywhere.**
  - Companion writer: 8 frames.
  - Adapter: input queue 4 frames, ring capped at target + 2 blocks, so bursts
    are shed. The target starts at 4 blocks (80 ms) on BLE and 6 (120 ms) with
    a Classic link present, with a maximum of 6.
  - At most 2 concealment blocks per gap; a gap over 50 is a discontinuity.
- **Echo.**
  - Windows: waveIn does no echo cancellation, so use headphones. This is
    stated in the toggle's tooltip.
  - Android: captures with VOICE_COMMUNICATION and the platform
    AcousticEchoCanceler. **Hypothesis:** whether that cancels this app's own
    AudioTrack output varies by device.

**Link simulation (`test_ns2_companion_mic.c`).**

The model:

- the companion captures on its own clock and the USB side reads on the
  adapter's, 600 ppm apart;
- 15 ms connection interval; ASSUMPTION: 2 PDUs per event;
- decode ≤ 13.2 ms, including a speaker encode ahead of it;
- one hour × three seeds.

Measured:

| Profile | Underruns per hour (after warm-up) | Shed | Queue p50 / max |
|---|---|---|---|
| BLE Controller Link | 0 (0) | 0 | 72 / 101 ms |
| BLE, clocks reversed | 0 (0) | 0 | 66–83 / 115 ms |
| Android: Classic on the radio, 20 % empty events (ASSUMPTION) | 2–3 (2) | 4–10 | 108 / 159 ms |
| Stress: 33 % empty events, 15 ms jitter | 38–53 | 242–320 | 115 / 159 ms |

The Android row is at the ring's maximum target (6 blocks). Going deeper needs
a larger ring, and SRAM does not allow one (below). The stress row is bounded
but audibly degraded; it is beyond any measured link.

**SRAM (measured on the ARM build).**

- Pro2 encoder heap allocation: ≈ 31.8 KB (encoder 10,068, PCM 7,680,
  TonalityAnalysisState 14,004).
- Mono CELT decoder state: 9,548 B, in a 10,240 B static buffer.
- `celt_decoder.c` code: 9,944 B, all relocated to SRAM.
- Free heap fell from 93,920 to 48,516 B, a ≈ 45 KB cost, leaving ≈ 15 KB of
  margin after the encoder.
- `caudio mic` reports `heap_free` live.

**Negative knowledge.**

- **Disproven: "runs of lost events cause an underrun at ~3599.76 s."** Every
  BLE seed showed one, which made link loss attractive. It was a simulation
  artifact: the simulated companion clock finished capturing before the USB
  side finished reading. Capturing 2 s more removed it. The 80 ms BLE target
  holds at zero.
- **Measured: a 4-frame companion writer is too shallow.** It lost ~70
  frames an hour in the Android profile; 8 lost none.
- **Measured: bursts held latency high for about a minute.** This is why the
  ring sheds above target + 2.

**Caveats.**

- Reply size: the status reply must stay ≤ 511 B (wireless response slot). The
  mic diagnostics are therefore split into `caudio mic` and `caudio mic decode`.
  The status buffer is 512 B, with a worst realistic case of ≈ 466 B.
- **Android GATT cache:** Android does not refresh a bonded device's GATT
  cache after a firmware update, so a phone may not see MIC-UP (or
  AUDIO-DOWN) until it re-bonds or the cache is cleared. The app then shows
  "does not accept a microphone" and plays speaker-only. Windows looks the
  characteristic up Uncached.
- Android capture continues off-screen only with the foreground service's
  microphone type. That type is added once RECORD_AUDIO is granted.
- **Linux (fifth pass):**
  - Playback and capture use SDL3, already the companion's gamepad library,
    so no native dependency is added. SDL picks PipeWire, PulseAudio or ALSA.
    The sink is the same `CompanionAudioPlayout` policy as Windows and
    Android, fed by `SDL_GetAudioStreamQueued`. The device period is asked
    to be 10 ms (`SDL_AUDIO_DEVICE_SAMPLE_FRAMES` = 480).
  - Capture applies no echo cancellation; PipeWire's echo-cancel module can
    provide it system-wide. Headphones avoid the problem, as with a real
    headset.
  - **BlueZ GATT cache:** BlueZ caches a bonded device's database. An adapter
    flashed with a build that adds AUDIO-DOWN or MIC-UP is seen only after
    BlueZ re-reads it (Service Changed, or a re-pair). Until then the planes
    report missing, which the app shows as it does on Android.
  - **Connection interval (Strong evidence, from kernel source; not
    measured).** As central, the Linux kernel opens LE links at 30–50 ms
    (`le_conn_min_interval` 0x18, `le_conn_max_interval` 0x28). The
    management request (7.5–50 ms) lets it stay there, so one late event
    would hold 2.5 frames. Both simulations assume 15 ms. `caudio start` now
    requests `ns2_bt_companion_audio_link_params()` (7.5–15 ms, latency 0,
    6 s), and the session's end re-requests the management range. The floor
    is unchanged, so a Windows or Android central that is already faster is
    not slowed. `caudio diag` reports the interval in force, which is the
    check for whether BlueZ honoured the request.

## Sixth pass — the Linux device plane, the adapter's hold, the sequence (2026-11-07, software)

### The Linux device plane, measured

`companions/linux/tools/PicoSwitch.Companion.Linux.AudioCheck` runs the
shipped `CompanionAudioService`, Concentus decoder and SDL3 sink and
microphone against a simulated adapter. The adapter is a real CELT
96 kbit/s stereo tone, framed as the adapter frames it, on 15 ms events
carrying at most 2 notifications. It holds and sheds frames as the firmware
does. Ran on WSL Ubuntu 24.04 (kernel 6.18, WSLg PulseAudio), SDL 3.5.0,
`pulseaudio` driver, 60 s per scenario:

| Scenario | Lost / concealed | Underruns | Sink drops | Pre-write depth error | Tone level |
|---|---|---|---|---|---|
| ideal (no empty events) | 0 / 0 | 0 | 0 | −2.9 ms, 0 growths | +0.01 dB |
| nominal (5 % empty events) | 0 / 0 | 0 | 0 | −2.5 ms, 1 growth | +0.01 dB |
| fast (+500 ppm) | 0 / 0 | 0 | 0 | +1.6 ms | +0.01 dB |
| slow (−500 ppm) | 0 / 0 | 0 | 0 | +4.2 ms, converging | +0.01 dB |
| stall (150 ms every 10 s) | 17 / 15 (= shed) | 0 | 7 | +28 ms | +0.00 dB |
| microphone (SDL capture → encoder) | 0 sequence gaps, 0 invalid | — | — | 50.00 frames/s, first after 240 ms | — |

- **The output is clean (Confirmed, software).** A splice detector checks
  every sample handed to the device against the sine recurrence, using a
  1013 Hz tone. At 1 kHz a whole 20 ms block is exactly 20 cycles, so a
  dropped block would be invisible. It found 0 splices on a clean link
  (largest residual 4 against a threshold of 99). Under stall it found 2
  per outage, each after a sink drop. Concealment itself produced none.
- **The growth in `nominal` comes from the link model, not the device.**
  `ideal` holds 60 ms with no growth. Two consecutive empty events (45 ms)
  trip the near-underrun guard, which costs one concealed block and is
  inaudible.
- WSLg's PulseAudio comes up a few seconds after WSL starts. Before that SDL
  falls back to ALSA, which fails with no card. The app reports this as
  "Could not open the audio output".
- **Not established:** what reaches the speaker after SDL. Under WSLg the
  remote-desktop audio path follows; on SteamOS it is PipeWire. The listening
  check is in procedure C below.

### The adapter's hold is bounded, and the sequence now counts frames encoded

While connection events are missed, the adapter holds at most **five frames**
(Strong evidence, from configuration):
- three in the controller (`MAX_NR_CONTROLLER_ACL_BUFFERS 3`, which a
  Classic controller on the radio shares);
- two in the encoder queue (`ENCODED_FRAME_QUEUE_DEPTH 2`, which drops its
  oldest frame).

The pipeline simulation had let the link hold every frame. With the hold
bounded:
- BLE Controller Link sheds nothing and is unchanged;
- Android Classic sheds 5–9 frames an hour, still within budget;
- Stress went from within budget to 41 underruns over three seeds.

The cause was the sequence. It advanced per frame **sent**, so a shed frame
left no gap: the receiver could not conceal it, the audio skipped ahead
uncounted, and the playout lost 20 ms of depth that its drift control
returns at about 1 ms/s. The header already said the sequence exists to
"count frames the adapter dropped". It now numbers frames **encoded**:
- `ns2_companion_audio_sequence()` computes it from an encode index carried
  with each frame (`ds5_audio_bridge.c`);
- a block that fails to encode is also a gap.

Result in simulation: Stress underruns fell from 41 to 13, and the
receiver's `lost` counter now shows what the adapter shed. Pinned by
`SequencingPerEncodedFrameTurnsShedFramesIntoConcealment` and
`test_sequence_counts_encoded_frames`. **Negative knowledge:** counting
sends looked harmless because the frames shed are "not on the wire", but it
hid them from the only component that could conceal them.

The wire format is unchanged. Receivers already conceal gaps of up to three
frames (`MaxConcealedFrames`). PCM blocks dropped before the encoder
(`pcm_dropped`) still leave no gap, because they do not map one-to-one onto
frames.

### Procedure C. Listening, Linux device plane

```
./PicoSwitch.Companion.Linux.AudioCheck --control 20 --gap 4 --scenario ideal,stall --seconds 20 --no-mic
```

The run has three segments:
1. A plain tone with no PicoSwitch code: the platform's own path.
2. The pipeline on a clean link. It should sound identical to segment 1.
3. Outages every 10 s, with an expected blip at each.

Stutter in segment 1 belongs to the platform. Stutter in segment 2 with
0 reported splices also lies downstream of the pipeline.

## Seventh pass — the adapter stops answering when the headset starts (2026-11-07, hardware)

### Observation

The user tested PC and Android companions on the Pico 2 W build with the
companion microphone. After the headset and microphone were switched on, the
adapter stopped responding. Core 0 and the UART stayed alive. UART readings
taken about 140 s later:

| Reading | Value | Meaning |
|---|---|---|
| `bthealth` core 1 sequence age | ≈141,900 ms | core 1 stopped |
| `bthealth` HCI last event age | ≈141,900 ms | BTstack (a core 1 IRQ) stopped with it |
| `audio` `codec_calls` / `codec_blocks` | 1 / 0 | core 1 died inside its first codec call |
| `audio` `pcm_packets` / `pcm_dropped` | 16,667 / 1,557 | core 0 kept producing; the queue overflowed |
| `audio` `core1_stack_free` | 30,268 B | not a stack overflow |

`codec_calls` counts on entry to the worker's per-block body, and
`codec_blocks` counts after the early-exit checks, which have their own
counters. Between the two the worker runs only `bridge_prepare_encoder()` and
the generation sync. The first companion block switches the encoder from the
DualSense format prepared at boot to the Pro Controller 2 format: free about
13.9 KB, then `malloc` one block of about 31.8 KB.

### Interpretation (Strong evidence, not confirmed)

The 31.8 KB allocation failed. The Pico SDK's malloc wrapper then calls
`panic("Out of memory")` (`PICO_MALLOC_PANIC` defaults to 1). This build has
no stdio, so the panic is silent, and it halts core 1, and with it the
Bluetooth interrupt that runs there.
- The heap was about 48.3 KB.
- The DualSense-format state freed just before the request leaves a 13.9 KB
  hole wherever later allocations sit above it.
- The "≈15 KB margin" quoted for the microphone build was a linker-map
  estimate. It never accounted for the heap's runtime tenants or for
  fragmentation.

**What would falsify it:** the same hang with the change below, with
`codec_stage` stuck at `prepare` or `sync`.

### Change

- **Static encoder storage.** The speaker encoder, in either format, now uses
  one static buffer: 31,924 B, `speaker_encoder_storage` in
  `ds5_audio_bridge.c`. The formats are never live together, so the buffer is
  shared. Its cost is now checked at link time, and nothing is left to
  fragment.
  - A libopus whose state outgrows the 10,240 B reserve makes the encoder
    refuse, counted in `encoder_reserve_short`, instead of panicking or
    overrunning.
  - Heap after the change: 16,424 B (`__HeapLimit − __end__`). The remaining
    heap users are newlib and the USB-host HID parser; libopus uses stack
    VLAs, not a pseudostack.
- **Stage breadcrumb.** The UART `audio` reply now carries `codec_stage` and
  `codec_stage_age_ms`, the last stage core 1 entered: `idle`, `mic`,
  `prepare`, `sync`, `encode`, `radio_lock` or `encode_locked`. A stage whose
  age keeps growing is where core 1 stopped.
- **`heap` command.** A new UART `heap` command reports the heap: `total`,
  `arena`, `in_use`, `free_in_arena`, `never_used` and `free`. It is a
  separate command because `mallinfo()` takes the malloc lock, which `audio`
  must never wait on.

**Negative knowledge:** "it fits in the map" is not "it can be allocated".
A runtime allocation of a third of the heap, made late, after a free, on the
core that hosts Bluetooth, turns any heap growth into a radio outage. Do not
reintroduce a runtime allocation for codec state.

Validation: host suite 84/84, both boards build with 0 warnings,
install-reset markers verified.

**Hardware retest (user, PC and Android companions, same day).** No crash,
Bluetooth stayed up, and headset plus microphone ran.
- **Runtime heap use:** `heap` reported 12,584 B in use out of 16,424 B,
  steady across readings. Those tenants were present in the old 48.3 KB heap
  too. That left about 35.8 KB, less any fragmentation, for the 31.8 KB
  block. This strengthens the out-of-memory explanation, which was not
  reproduced directly.
- **Stream health:** 1,220 frames, 0 Opus errors, 0 PCM drops, encode
  max 2.8 ms; core 1 idles in `mic`.
- **Open:** the first headset toggle did not start a session. The audio
  showed 3,201 `codec_disconnected` blocks, then the toggle succeeded. The
  refusal reason (`caudio status` `last_admit`) is not visible on the UART,
  so the cause is unknown.

## Eighth pass — Android delivered every frame and played none (2026-11-07, hardware)

Setup: adapter USB to Windows (Pro Controller 2 personality, Windows output
"Headphones (Switch 2 Pro Controller)"); Odin 2 Mini (Android 13, API 33)
as the Android companion controller; ADB logs.

- **Delivery was fine (Confirmed).** The adapter sent 6,998 frames and the
  app decoded 6,998, with 0 lost, 0 malformed and 0 stale.
- **The Android sink played nothing (Confirmed).** `AudioTrack` reported
  `PLAYSTATE_PLAYING`, but `playbackHeadPosition` stayed at 0. The sink
  accepted 9 blocks (8,640 frames), then dropped every later block as "queue
  full". Android's output power log was silent for the whole session.
- **Root cause.** A streaming `AudioTrack` does not consume until its buffer
  reaches the start threshold, and the default threshold is the whole
  buffer. On API 28–30 there is no threshold API, but the same full-buffer
  rule applies. The buffer was sized for the playout ceiling plus two
  blocks, and `CompanionAudioPlayout` deliberately caps the queue below that,
  so the start condition could never be met.
- **Fix** (`AndroidCompanionAudio.kt`):
  - API 31+: `setStartThresholdInFrames(one block)`, so the policy's own
    prebuffer is the only start condition.
  - API 28–30: just before `play()`, shrink `bufferSizeInFrames` to what is
    queued, then restore it once the head moves.
- **Result.** After the fix, the head advanced in real time (241,920 frames
  in the first 5 s), 255 of 256 blocks were accepted and 0 dropped. The
  user heard the audio on the Odin.
- **Negative knowledge:** "the track is playing" is not "the track is
  consuming". The Windows and Linux sinks have no such threshold, so the
  shared playout policy was correct and only the Android adapter of it was
  wrong.
- **Diagnostics added.** The sink logs state, head, written, accepted,
  dropped, write errors (negative `write()` codes, previously counted as a
  full buffer) and peak level every 5 s. The silence check logs the
  adapter's subscribed/sent/busy/failed counters next to the app's
  decoded/lost counters.
- **Also found, not fixed:** the adapter admits the Android companion only
  after it receives an input report carrying the bridge extension
  (`bthid_gamepad.c`). The bridge sends reports only on change, so after a
  reconnect with idle controls, `caudio start` is refused with
  `no_controller_link` until the user touches an input. The fix belongs in
  the firmware: note the companion when the bridge descriptor binds, not on
  the first report.

## Ninth pass — link recovery, admission, a headset that behaves like one (2026-11-07, hardware + software)

Environment: AYN Odin 2 Mini (Android 13) running the Android companion as the
controller; adapter on a Pico 2 W, USB to Windows 11 as a Pro Controller 2
(output "Headphones (Switch 2 Pro Controller)"); firmware from the seventh
pass plus the core 1 fault recorder and sampler (UART `fault`).

### Bluetooth restart left Controller Link down for good (fixed, Confirmed)

- **Observation.** After `cmd bluetooth_manager disable/enable` on the Odin,
  management sat in `Reconnecting` and the bridge cycled
  Idle/Unsupported with "bound with no establishment requested; staying
  passive" until the app was restarted.
- **Cause.** The app's one foreground reconnect had been spent while the radio
  was off; nothing re-armed it. Controller Link resumes only from a verified
  management session (`requestAutomaticControllerResume`), so it stayed down
  too. `HidEstablishmentPolicy` (profile rebinds stay passive) is correct and
  unchanged: recovery must come from an explicit event.
- **Fix.** `BluetoothAdapter.ACTION_STATE_CHANGED` → `bluetoothRadioChanged`
  re-arms the foreground reconnect on STATE_ON; a remote management loss gets
  bounded retries (1.5 / 4 / 10 / 20 s) through `requestReconnect`, which
  ignores a request while another attempt is active.
- **Result.** 3/3 Odin Bluetooth off/on cycles returned to `Playing`
  (radio on → management verified 1.4 s → Controller Link Playing 2.2 s);
  adapter core 1 healthy after every step (`bthealth`).

### "Press a button before the headset works" was the adapter, not the console (fixed in firmware; validation pending flash)

- **Question.** Does the console need controller input before it opens the
  speaker stream?
- **Measurement (Windows host).** Headset off → `usb_active` false; headset on
  with no input → `usb_active` true at once (`usb_edges_on_off` 11→13). No
  input needed on Windows.
- **What actually waited.** `caudio start` was refused
  `admit=no_controller_link` three times, then admitted right after one
  injected button. The adapter noted the Android companion only on an input
  report carrying the bridge extension
  (`bthid_gamepad.c`, per report). The bridge sends reports only on change, and
  its first one can arrive before the HID descriptor is bound, so with idle
  controls nothing satisfied the gate until a press.
- **Fix.** The companion is noted when the bridge descriptor is identified
  (`android_bridge_identify` succeeds on a live link); the per-report note
  stays as a second witness, the seam still clears it on disconnect.
- **Status.** Implemented, builds for both boards; hardware validation
  pending the flash. The earlier "the console only switches over after the
  first button press" observation (fourth pass, Strong Evidence) is now
  confounded by this gate and needs re-testing on a Switch 2.

### Mute instead of a microphone switch (Confirmed: no re-plug)

- A mid-session microphone change is a re-plug on the adapter: the jack is
  withdrawn for `NS2_CAUDIO_REPLUG_MS` (200 ms) and the console reroutes the
  speaker away and back. That is the stutter the owner heard when turning the
  microphone on.
- The headset is now one accessory chosen at plug time: a headset when the
  device can record (RECORD_AUDIO), headphones otherwise. Mute keeps the
  uplink's cadence and sequence and sends encoded silence
  (`CompanionMicStream.muted`).
- **Result.** Six mute/unmute toggles on a live headset: zero `caudio`
  commands, sink `underruns=0 dropped=0`, playback head advancing at 48 kHz.

### Re-plug after a link drop (software only)

- The session keeps the player's "plugged in" intent across Controller Link
  and management loss and re-plugs when both return (bounded: 5 tries,
  600 ms apart, retrying a refusal). Unplugging, or an adapter end for any
  reason other than `controller_link_ended`, clears it. Unit-tested; not yet
  run on hardware.

### Negative knowledge

- **Disproven for the Windows host:** "the console opens its speaker stream
  only after controller input". It opened immediately. The press users
  needed was the admission gate above.

## Hardware validation procedure (next)

Pico 2 W with this build. Pico W must refuse `caudio start` with
`unsupported`. Each step changes one variable. Record `caudio status`,
`caudio diag`, `caudio mic` and `caudio mic decode` where named, and the app's
`companion-audio session end` and `microphone end` diagnostic lines.

### A. Speaker, Controller Link scope

1. Controller Link off: the Headset toggle is disabled, and `caudio start`
   answers `no_controller_link`.
2. Controller Link on, Headset on. Until the first button press the label says
   to press a button. After one press, audio plays on the companion.
3. 10 minutes of play. Pass criteria:
   - no audible gap;
   - `caudio diag`: `encoder.over_20ms` 0, `encoder.pcm_dropped` 0,
     `send.over_60ms` 0;
   - `session end`: `underruns` 0.
4. Stop Controller Link. The session ends with "Controller Link stopped" and
   audio returns to the TV.
5. Headset off with Controller Link still on: audio returns to the TV at once.

### B. Microphone

1. Mic on, then Headset on. `caudio status` shows `mic:true`, the console sees
   a headset (a voice-chat app offers the microphone), and `caudio mic`
   shows `usb_mic:true` and `playing:true` once the console opens EP 0x83.
2. Speak for 60 s in a voice-chat app or a console recording. Pass criteria:
   - intelligible, no gaps;
   - `caudio mic`: `playout.underruns` 0 after the first second;
   - `caudio mic decode`: `errors` 0, `lost` near 0, and record `max_us`;
   - `microphone end`: `displaced` 0 and `failed` 0.
3. Mic off while playing: the accessory re-plugs as headphones within about
   200 ms, and speaker audio continues with no session restart. Mic on again:
   it re-plugs as a headset. If the console does not follow either change, the
   NONE → occupied hypothesis is false; record what it did.
4. Console mute and volume for the headset microphone: `gain_q15` follows,
   and muted is silent.
5. Speaker regression with the mic on: repeat A.3.
6. Android, phone speaker, no headphones: listen for echo on the console side.
   Repeat with headphones.

### Earlier procedures (third pass; still useful for isolation)

### Third-pass fixes

1. No source controller, Headset on → off: console audio returns to the TV.
   Headset on again: it moves back to the PC.
2. DualSense Edge + Headset on for 60 s. Sample `caudio diag` every 10 s;
   `lock_applies` must be true. Pass criteria:
   - `encoder.over_20ms` 0;
   - `encoder.pcm_dropped` 0;
   - `send.over_60ms` 0;
   - no audible gaps.
3. `caudio lock off`, toggle Headset off/on, and repeat step 2 for the A/B.
   Judge input feel in both runs: the lock defers radio work by up to one
   encode.

### Isolating the DualSense degradation

Run this first. Flash this build, connect the companion, and use the DualSense
Edge as the controller. Each step changes one variable.

1. `caudio diag` with Headset off: record `link.interval_us` and
   `link.tx_octets`. If `tx_octets` is 27, each frame costs several link-layer
   packets, and that is the next lever.
2. Headset on, the same game scene for 60 s, then `caudio diag`:
   - `encoder.pcm_dropped` and `encoder.over_20ms` > 0 mean the adapter
     encoder is still losing audio;
   - `send.over_60ms` > 0 with zero encoder drops points at send cadence.
3. Repeat step 2 with Headset off, and compare how inputs feel. Late inputs
   only with Headset on point at radio airtime.
4. Copy the app's `companion-audio session end` line from Diagnostics after
   each run: `lost`, `concealed`, `underruns` and `prebuffer_ms`.
5. Plug headphones into the DualSense mid-session. Expect:
   - the toggle switches off with "Audio moved to the headset plugged into
     your controller";
   - audio continues from the DualSense;
   - `caudio status` shows `last_stop: preempted_controller_headset`.

   Then press Headset on with the headphones still plugged in. Expect a refusal
   with `admit: controller_headset`.


### Original procedure

Pico 2 W running this build, Windows companion connected and trusted, Switch 2
attached, any controller playing.

1. `caudio status` over UART or the app's diagnostics. Expect `supported:true`,
   `att_mtu` of at least 247 (Windows negotiated 527 for Controller Link), and the
   boot log line `ATT server initialized (db size=N)` with N < 512
   (`MAX_ATT_DB_SIZE`). N is unmeasured; the estimate is about 410.
2. Turn **Headset on**. Expect the console to route audio away from the TV
   (`usb_speaker:true`) and audible, continuous playback on the PC.
3. Sample `caudio status` every 5 s for 60 s. Pass criteria: `frames.sent` rises
   about 50/s, `failed` stays 0, and `send_max_gap_us` stays well under 100 ms.
   Record `busy`.
4. **Regression:** with Controller Link streaming, compare `clink status`
   (`stale`, `max_gap_ms`, `applied` vs `received`) with Headset on and off
   (§21 / §22 E). Repeat with a DualSense connected: its rumble must stay on the
   ordinary path while Headset is on.
5. **Lifecycle:** close the app with Headset on (expect `last_stop: mgmt_lost`
   and audio back on the TV), toggle repeatedly, and plug a genuine Pro
   Controller 2 headset mid-session (expect `preempted_pro2`).
6. Read the app's `companion-audio session end` diagnostic line. It reports
   lost, concealed, sink drops and underruns, which feed the jitter-buffer tuning
   (`PrebufferBlocks`, `MaxQueuedBlocks`).

## Remaining unknowns

- Whether 96 kbit/s fits alongside the source controller's BR/EDR link on one
  CYW43 radio without audible loss or late inputs once the adapter-side drops
  are fixed. With a DualSense Edge it did not (second pass), and the counters
  point first at the encoder rather than the radio. The §11 screening estimate is
  45–53% airtime for the companion ACL alone at 96 kbit/s.
- What removed core 1 from the encoder for about 45 ms during the DualSense
  session.
- The management link's negotiated connection interval and LE data length.
  `caudio diag` reports them from this build. On Linux, also whether BlueZ
  honours the 15 ms request.
- Whether SDL3's PipeWire backend holds the 10 ms period and the playout
  depth on SteamOS/Bazzite, and opens the default microphone from a Game Mode
  session.
- **End-to-end latency.** The budget is about 20 ms encode framing, transport,
  60 ms prebuffer and the device buffer, but it is unmeasured.
- Whether the console treats an advertised jack with no controller actually
  holding audio the same as a real headset in every scenario: sleep, docked,
  handheld transitions.
- Whether the PCM capture probe (`ds5_audio_pcm_capture_arm`) is affected. It
  is stopped when the plane starts, just as on the Pro Controller 2 path.
- **Microphone:**
  - whether the console follows a mid-session headphones ↔ headset re-plug
    (the 200 ms NONE gap is a hypothesis). The apps no longer re-plug for the
    microphone (ninth pass), so this matters only to `caudio start` callers;
  - when the console opens the USB microphone alternate setting (always with
    a headset, or only for an app using voice chat);
  - the RP2350 decode time (`caudio mic decode` `max_us`) beside a speaker
    encode. The simulation assumes ≤ 13.2 ms for both;
  - the real fraction of empty connection events with a Classic link, which
    drives the Android rows of both simulations;
  - Android echo cancellation of the app's own output, per device.

## Not done, deliberately

- Protocol-level silence frames (§12.3). A lower bitrate is excluded by
  requirement.
- Capability negotiation beyond `caudio status` (§18).
