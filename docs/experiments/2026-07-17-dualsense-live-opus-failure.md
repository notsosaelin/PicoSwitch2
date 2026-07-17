# DualSense live-Opus first hardware pass — failed

Date: 2026-07-17  
Board: Pico 2 W  
Scope: Pro Controller 2 USB audio to DualSense Bluetooth speaker

## Observed

- No audible controller-speaker output.
- The Windows audio endpoint disappeared and reappeared.
- DualSense input became random/high-rate button and direction spam.
- BOOTSEL gestures could not be used while the DualSense was paired.
- The regressions were isolated to DualSense/audio mode; the preceding UAC1-only hardware pass
  had no known regressions.

## Findings

1. The live encoder ran inside the same core-1 BTstack run loop that owns DualSense input and
   report output. This architecture has no safe latency bound and is rejected for default builds.
2. Audio mode multiplexes microphone Opus data into input report `0x31` (`header bit 1`). The first
   implementation incorrectly passed those packets to the ordinary gamepad parser, explaining the
   random buttons and axes. The driver now identifies and drops them until microphone decode is
   implemented.
3. The report/CRC and live codec were enabled at the same time, so the failed pass did not isolate
   large-report transport from encoder scheduling.

## Follow-up

- Live Opus is now opt-in and disabled by default.
- The normal Pico 2 W build returns to the hardware-confirmed UAC1 sink/silent-microphone behavior.
- A separate deterministic 1 kHz Opus-tone build exercises the `0x39` transport at the real packet
  cadence without invoking the encoder.

## Fixed-tone result

- Controller input, BOOTSEL, and the Windows endpoint were stable. This confirms the input-report
  filter and removal of live encoding from the BTstack core fixed the severe regressions.
- No 1 kHz tone was audible. Silence therefore remains in audio activation/report transport rather
  than being caused solely by the live encoder.
- Comparison with DS5Dongle commit `750bde88b482de965a9909a8829d941052eec04c` found a concrete
  omission: before streaming `0x39`, it sends a 142-byte extended `0x32` state report with
  `AllowAudioControl` set. PicoSwitch2 had copied the stream and microphone-status reports but not
  this activation transaction.
- The next diagnostic adds that one-time transaction, preserves it ahead of stream traffic with a
  settling delay, and tests its byte layout and CRC on the host.

## AudioControl retest

- The controller remained stable and silent after adding the extended `0x32` activation report.
- DualSense, Xbox, and generic-controller baseline checks found no other regressions.
- The activation-only hypothesis is therefore refuted.
- A subsequent byte audit found that PicoSwitch2's already hardware-confirmed DualSense
  compatibility initialization sets validity bits for headphone, speaker, and microphone volume
  while leaving all three corresponding bytes zero. That explicitly programs the speaker volume
  to zero before the AudioControl and `0x39` reports are sent.
- The next build retains the compatibility flags but initializes headphone/speaker volume to 100
  and microphone volume to 64. The exact bytes and revised CRC are host-tested.

## Preflight audit before the volume retest

The user paused the next flash and requested a complete audit of remaining zero-filled fields,
stubs, and omitted commands. The audit compared PicoSwitch2 with DS5Dongle
`750bde88b482de965a9909a8829d941052eec04c`, Gamepad-Core
`efaf368ccd98246e6832220d031b1f7841e80ef7`, and Linux `hid-playstation`.

Additional corrections included in the consolidated diagnostic:

- Both the ordinary `0x31` initialization and extended `0x32` activation now explicitly select
  the internal-speaker route (`AudioControl = 0x30`) instead of marking AudioControl valid while
  writing its zero/automatic value.
- The extended `0x32` activation repeats nonzero headphone/speaker volume and explicit unmute
  state. It no longer depends on an earlier output report or controller state retained from a
  previous host.
- Windows UAC1 speaker mute/volume changes are now converted to DualSense control values and
  resent over Bluetooth. The previous driver accepted and stored those host controls but never
  applied them to the controller.
- The direct L2CAP path now rejects a negotiated interrupt MTU smaller than the 548-byte HID
  transaction instead of accepting the report and retrying a permanent
  `L2CAP_DATA_LEN_EXCEEDS_REMOTE_MTU` failure.
- The embedded tone was regenerated as a steady-state 160-kbit/s CBR Opus stream at a materially
  higher level. A host libopus decoder verifies both 10 ms stereo frames, non-silence, and the
  expected approximately 1 kHz waveform.

Intentional zeros/omissions retained:

- The two 64-byte haptics payloads are zero because the PC2 USB stream exposes two speaker
  channels, not DS5Dongle's four speaker+haptics channels. Silent haptics are valid and do not
  mute the speaker block.
- Microphone USB return remains silent and microphone Opus decode is not implemented. The
  controller mic is now explicitly disabled, avoiding useless traffic and the earlier input-spam
  failure.
- Reserved report bytes remain zero. Speaker pre-gain remains at its neutral value; references
  describe it as optional boost, not an audio-enable gate.
- Feature GET reports `0x05`, `0x09`, `0x20`, `0x22`, and `0x70` are read-only
  calibration/pairing/firmware/model discovery. No reference identifies them as audio activation,
  so speculative feature traffic was not added.

## Consolidated diagnostic result

- The controller speaker produced an audible repeating/pulsed tone without application audio
  playing. Muting the Windows endpoint muted the controller. This hardware-confirms the complete
  activation path, `0x39` delivery and CRC, Opus decode, internal-speaker routing, nonzero volume,
  and Windows mute forwarding.
- The pulsed "Morse code" quality exposed two continuity defects rather than another activation
  omission:
  - A report contains exactly two 10 ms Opus frames, but the diagnostic scheduled one every
    21.333 ms. That guaranteed at least a 1.333 ms underrun after every pair. It now uses an
    absolute 20 ms deadline so task jitter does not accumulate.
  - The headset-present flag was copied from a reference offset that included the outer `0xA1`
    transaction byte. BTHID removes that byte before driver dispatch, so reading byte 56 instead
    of byte 55 could route the stream using unrelated status data. The offset is corrected and
    host-tested.
- The microphone-status transaction now returns after queuing, preserving its order ahead of the
  first stream report in the bounded direct-L2CAP queue.

## Continuity retest

- Corrected pacing and route parsing made the tone better, but it remained intermittent and
  Morse-like while no Windows application was playing audio.
- The diagnostic still incorrectly gated its embedded tone on the USB speaker alternate setting.
  Windows shared-mode power management may idle that setting when no playback client is active,
  so it was not a pure Bluetooth continuity test. The fixed-tone build now runs continuously while
  a DualSense owns the bridge; live audio remains correctly gated on real USB PCM.
- The direct-L2CAP queue also reported success when it overwrote an already occupied successor
  slot. That is acceptable coalescing for LED/rumble state but lossy for Opus: the driver advanced
  the packet counter and discarded the source pair even though the queued report never reached
  L2CAP. Audio reports now receive backpressure whenever accepting them would overwrite either
  incoming or already queued `0x39` data.
- A 400-frame host decode confirms that indefinitely replaying the embedded two-frame tone through
  one stateful Opus decoder does not decay or contain low-energy frame boundaries.

## Report-starvation retest

- Removing USB-idle gating and replacing queue overwrites improved the tone again, but it remained
  intermittently Morse-like.
- This exposed a project-local scheduling mismatch: sustained DualSense Classic input was already
  proven to delay BTstack timers enough that BOOTSEL gestures and rumble required report-boundary
  safe points. Audio still depended solely on a nominal 2 ms timer, so its 20 ms transport
  deadline remained vulnerable to the same traffic.
- The lightweight audio transport now runs at inbound HID report boundaries as well as from the
  timer. The deterministic tone may advance its clock there because it performs no encode. Live
  Opus encoding remains excluded from the deep receive callback; only an already encoded pair can
  be transported from that path.
- The direct-L2CAP queue is now a ten-entry FIFO in RP2350 audio builds, matching the current
  DS5Dongle reference depth and absorbing short CAN_SEND_NOW/radio scheduling stalls. Ordinary
  builds retain a two-entry footprint and their existing LED/rumble coalescing behavior.

## Regular 50% duty-cycle result

- Report-boundary service and the larger FIFO improved the result, but the user described the
  remaining audio as an almost perfectly alternating `010101...` beep/silence pattern, with only
  small occasional dropped/doubled intervals. The intended diagnostic is one uninterrupted tone.
- This is evidence of a sustained approximately half-rate transport rather than random jitter:
  each report contains 20 ms of valid sound, while the link appears to deliver one about every
  40 ms.
- PicoSwitch2 globally enabled Classic Bluetooth sniff mode for compatibility and idle power.
  Sniff anchor intervals can impose exactly this sort of periodic throughput ceiling on large
  HID reports. The DS5Dongle audio reference does not enable sniff policy.
- Sniff mode is now disabled only in explicitly experimental RP2350 audio builds. Ordinary Pico W
  and Pico 2 W builds retain the previously hardware-confirmed role-switch-plus-sniff policy.

## Transition to live Windows audio

- The user explicitly chose to stop iterating on the synthetic fixed tone and proceed to live
  Windows PCM after speaker activation and transport were proven.
- The live path no longer copies DS5Dongle's 512-to-480 conversion. PicoSwitch2's UAC endpoint
  supplies exact 48 kHz stereo in 192-byte packets: 48 frames per millisecond. Ten packets now
  accumulate directly into one 480-frame/10 ms Opus input with no resampling or guaranteed
  0.667 ms source deficit.
- Because CBR is configured for exactly 200 bytes per 10 ms frame, a different encoder return size
  is rejected rather than blindly zero-padding an invalid Opus packet.
