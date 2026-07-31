---
name: picoswitch2-audio-regression
description: Measure and protect PicoSwitch2 DualSense or genuine Pro Controller 2 audio while changing codecs, scheduling, haptics, headset routing, or microphone return. Use for stutter, Opus/CELT, report 0x39, USB PCM, core-1 timing, headset classification, microphone gating, and any audio-adjacent change that must not regress validated speaker playback.
---

# PicoSwitch2 audio regression lab

## Freeze the working speaker path

Read `AGENTS.md`, `PLAN.md`, `docs/switch2/audio-passthrough-research.md`,
`docs/switch2/pro2-headset-audio.md`, and current source. Pico 2 W uses the
validated 300 MHz audio build; Pico W remains non-audio.

Run an observational baseline:

```powershell
./tools/audio_lab.ps1 -Scenario <slug> `
  -Action '<one playback/control action>' `
  -DurationSeconds 30 -SampleSeconds 5
```

The default runner does not clear counters or send codec, route, stream, or
Pro2-audio commands. Use `-ResetCounters` only for an explicitly controlled
baseline. Compare later runs with `-Baseline <audio.samples.json>`.

Treat PCM drops and Opus errors as hard failures. Timing maxima identify where
to investigate but do not alone prove audible failure. Measure source delivery,
successful sends, queue depth, encoder errors, core-1 liveness, and stack
headroom together.

## Gate microphone work

Do not alter playback while discovering microphone framing.

1. Physically validate DualSense report `0x31` byte 55 with known TRS
   headphones and known TRRS mic headset.
2. Capture incoming microphone packet metadata without requesting or decoding
   it.
3. Reassemble/decode to WAV offline.
4. Measure decoder flash, SRAM, stack and runtime before USB mic output.
5. Enable embedded return only for DualSense/Edge, normalized state
   `HEADSET`, and an active USB microphone alternate setting.

`HEADPHONES` must remain speaker-only. Keep speaker encoder state, queues,
haptics and connection state independent from microphone state. On pressure,
drop microphone frames before delaying speaker/audio-haptic delivery.

Keep microphone support behind a compile-time research gate until TRS, TRRS,
mute, unplug/replug, reconnect, gyro, rumble, LED, BOOTSEL and speaker
continuity all pass.

## Verify proportionally

Run audio-specific host tests, all compiled host tests, the Pico 2 W build, and
the install-reset marker check. Do not claim audio validation from a build:
speaker continuity and accessory classification require hardware evidence.
