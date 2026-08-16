# Agent brief — common

Read this before a focused investigation. It is deliberately short. Protocol detail lives in
`docs/`; this file exists so a subagent prompt can be three lines instead of three pages.

Specialist briefs: [MOTION.md](MOTION.md) · [RUMBLE.md](RUMBLE.md) · [ANDROID.md](ANDROID.md)

## What this project is

PicoSwitch2 makes a Raspberry Pi Pico W / Pico 2 W present itself to a Nintendo Switch 2 as genuine
first-party hardware (Pro Controller 2, NSO GameCube, Joy-Con 2) while translating input from
Bluetooth controllers and an Android companion app. The goal is behavior indistinguishable from
real hardware, plus documentation good enough to be the reference implementation.

## Architectural boundaries

```
physical controller / Android handheld
        ↓  vendored joypad-os bthid driver (src/bt_hid/bt/bthid/devices/…)
   input_event_t                     unified, shared interchange units
        ↓  src/bt_hid/ns2_seam.c     the ONE bridge into this project's own state
   switch_pro_input_t                cross-core seam (src/report.c seqlock)
        ↓  src/switch_pro2/          console-facing personality, USB core0
   Switch 2 report
```

- Controller-specific knowledge belongs in the driver and in `ns2_motion_seam.c`.
- Console-protocol knowledge belongs in `src/switch_pro2/` and `src/bt_hid/motion/`.
- Anything that needs a `switch (controller_family)` inside the console-facing encoder is almost
  certainly in the wrong layer.

## Hardware behavior is the source of truth

Priority when sources conflict: reproducible hardware observation → captures/traces → current
implementation behavior → code → documentation → comments → names → architectural assumptions.

A function named `generic_*` is not evidence that it is generic or correct. This project has
already lost weeks to exactly that (see [MOTION.md](MOTION.md)).

**Do not soften a recorded hardware observation.** "Never worked" does not become "may have
issues"; "zero rumble" does not become "unreliable rumble". Those rewrites change the engineering
conclusion. If a hardware fact is documented, cite the document rather than restating it.

## Documentation conventions

- Never promote a hypothesis to fact. Mark confidence: **CONFIRMED / LIKELY / UNKNOWN**
  (or the repo's ✅ 🟡 🔵 🔴 ⬜ status legend in status documents).
- Record negative results in `docs/experiments/refuted-hypotheses.md`.
- Preserve historical experiment reports. Correct *active summaries* instead of rewriting what an
  old experiment observed.
- **When a model is overturned, fix the summary table at the top of the document, not only the
  section that proved it wrong.** A stale summary outranks working code in practice — that is
  precisely how the refuted motion encoder survived.

## Build and validation

```powershell
cmake --build build\pico2_w --config Release --parallel
cmake --build build\pico_w  --config Release --parallel
pwsh -File tools\run_mgmt_tests.ps1        # management + Android HID contract host tests
```

Android companion (needs JDK 21 — Android Studio's bundled JBR is 25 and AGP rejects it):

```powershell
$env:JAVA_HOME="C:\Program Files\Eclipse Adoptium\jdk-21.0.12.8-hotspot"
$env:ANDROID_HOME="$env:LOCALAPPDATA\Android\Sdk"
cd android\companion; .\gradlew.bat --offline testDebugUnitTest
```

Known pre-existing break: the `build\pico_w_switch1` configuration (`NS2_PRO=OFF`) does not
compile — `src/config.c` references `g_usb_reenumerate_request_pending` outside an `NS2_PRO`
guard. It fails identically at `HEAD` and is unrelated to motion or rumble.

Always separate **LOCALLY VERIFIED** from **REQUIRES HARDWARE TEST** in any report. Never claim
hardware validation that was not performed.

## Working with subagents

- Investigate before changing architecture. Return evidence and a recommendation; the primary
  agent owns integration.
- Several agents must not independently refactor the same subsystem.
- Keep prompts short and point at these briefs. Do not paste project history into a prompt.

## Cost rules

- Maintainer time on physical tests is expensive. Prefer diagnostics that make one test decisive
  over several tests that each narrow the field slightly.
- Never ask for staged motion poses (deliberate pitch/yaw/roll sequences). Validate motion by
  derivation, host tests, and ordinary play.
- Development-build backward compatibility between the firmware and the Android app is **not**
  required. They may evolve together and break old builds. Do not add negotiation or shims for
  hypothetical old installations.
