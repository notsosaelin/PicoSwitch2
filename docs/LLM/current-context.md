# Current Continuation Context

Last reconciled: 2026-07-24

Working branch: `ns2-testing`

Latest published release: `v1.5.0` (2026-07-22)

Always run `git status`, `git log -1`, and read `STATUS.md` at the start of a new session. This file
deliberately does not pin the latest commit because it should remain useful after later checkpoints.
On a new workstation, clone `ns2-testing` with `--recurse-submodules`; `third_party/opus` is a
required Git submodule for Pico 2 W audio builds.

## Current validated baseline

- Pico 2 W: standard 300 MHz build with DualSense audio/native haptics.
- Pico W: validated non-audio profile.
- Pro Controller 2, NSO GameCube, Joy-Con 2 Left, and Joy-Con 2 Right personalities enumerate and
  report current firmware identities without an update prompt.
- Input, mappings, rumble, battery passthrough, wake, reconnect, LEDs, BOOTSEL gestures, and config
  persistence have broad hardware coverage; see the compatibility matrix for per-controller scope.
- Genuine Pro Controller 2 native `0x1E`/`0x28` motion passthrough, bonded HOME reconnect, P1 LED,
  gyro, and headset audio are hardware-confirmed.
- DualSense and DualSense Edge translation to the Switch 2 length-`0x1E` motion carrier is
  hardware-confirmed in Splatoon 3, including rapid movement and reconnect.
- DualSense console/Windows audio, physical headset state, reconnect audio, and waveform-preserving
  native haptics are hardware-confirmed on Pico 2 W.

## Motion boundary

- Production genuine Pro2 input transports its controller-generated `0x1E`/`0x28` PDUs opaquely.
- Production DualSense/Edge output synthesizes only the decoded length-`0x1E` quaternion carrier.
- G6/G7/G8 in normal `0x28` PDUs have an exact signed 22/22/20-bit codec, but their semantics and
  the changing leading/middle lanes remain incomplete.
- The template experiment—genuine static `0x28` body plus dynamic timing/G6/G7/G8—caused immediate
  random motion. The runtime generator was removed. Do not repeat it without decoding every
  console-relevant changing lane.

## Highest-value open work

1. Decode the unresolved genuine `0x28` lanes through passive UART captures and offline analysis.
2. Add DualSense microphone return only after preserving the confirmed speaker/haptic path.
3. Capture genuine NFC/amiibo transactions before implementing the command surface.
4. Extend motion translation to another controller family only after verifying its calibration,
   axes, units, timestamps, and stationary-bias behavior.

## Known traps

- A fresh Codex session has no conversation memory. Repository evidence is the handoff.
- Do not use symptom-driven “spin and tell me” tuning when captures, UART, or screen analysis can
  answer the question.
- Do not assume a controller's VID/PID can be recovered late from every saved bond; fresh pairing
  and already-bonded paths differ.
- Do not merge Pico 2 W audio scheduling into Pico W.
- Do not alter genuine Pro2 audio framing: one 240-byte/20 ms CELT frame is split into ordered
  120-byte `0x04` and `0x02` GATT writes.
- Do not add large captured media without updating `dumps/README.md` and binary attributes.
