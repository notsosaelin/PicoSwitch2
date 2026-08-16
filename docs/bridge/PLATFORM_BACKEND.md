# Implementing a PicoSwitch Bridge platform backend

Status: 🟡 architecture in place and Android-validated as the first backend; no second backend
exists yet.

This document is the contract. It describes what a Windows, Linux, or any other host
implementation has to provide, and — just as importantly — what it must **not** reimplement
because Bridge Core already owns it.

Read [`PROTOCOL.md`](PROTOCOL.md) alongside this for the wire contract, and
[`../agents/ANDROID.md`](../agents/ANDROID.md) for the reference backend's hard-won platform
specifics.

---

## 1. The shape

```text
 physical controls / host device APIs
              |   PLATFORM INPUT BACKEND
              v
      ControllerInputState                (Bridge Core)
              |
              v
        BridgeSession                     (Bridge Core)
              |   ControllerReportEncoder  (Bridge Core)
              v
       BridgeTransport                    (PLATFORM)
              v
         PicoSwitch2
```

and in reverse:

```text
         PicoSwitch2
              |
       BridgeTransport                    (PLATFORM)
              v
    BridgeOutputCodec -> BridgeOutput     (Bridge Core)
              |
        BridgeSession                     (Bridge Core)
              |   RumbleRequest
              v
     PLATFORM OUTPUT BACKEND  ->  rumble / haptics
```

## 2. Where the code lives

| Layer | Location | Platform-specific? |
|---|---|---|
| Bridge Core | `android/companion/bridge-core/` (Gradle module `:bridge-core`) | **No.** Plain Kotlin/JVM, no Android dependency. |
| Android backend | `android/companion/app/src/main/java/dev/picoswitch/companion/bridge/` | Yes |
| Android app/UI | `.../companion/ui/`, `MainActivity` | Yes |

The module boundary is the guard: `:bridge-core` is a `java-library` + `kotlin("jvm")` module with
no Android SDK on its compile classpath, so an Android type in the shared model is a **build
failure**, not a review finding. `ArchitectureGuardTest` additionally scans the sources for
platform vocabulary in identifiers and string literals, which a classpath cannot catch.

> The module currently lives under `android/companion/` because that is where the only consumer is.
> When a second host appears, move it up a level; nothing in it depends on that path.

A JVM platform (a desktop Kotlin/Java frontend) can depend on `:bridge-core` directly — Level 2
sharing. A non-JVM platform reimplements the same documented model — Level 1. Both are supported
outcomes; this document is written so Level 1 is sufficient.

---

## 3. What you must implement

### 3.1 Enumerate and select an input device

Produce a `ControllerCandidate` per host input device that plausibly relates to a controller:

```kotlin
ControllerCandidate(
    id, descriptor, name, vendorId, productId,
    hasMotionAxes,       // at least one stick/trigger analog axis
    hasGamepadButtons,   // at least one standard gamepad button
    isVirtual,           // the PLATFORM's own synthetic classification, passed through
    hasGamepadSource,    // classified as gamepad/joystick, not merely D-pad/keyboard
)
```

`ControllerCandidates` then decides usability, exclusion reasons, and auto-selection. **Do not
write your own filter.** The rule is subtle (see the KDoc: it must hide the platform's virtual
keyboard without hiding a real handheld that reports itself as external and virtual-node-backed)
and it has already been wrong once.

`descriptor` is an opaque, stable per-device string. The bridge only compares and persists it.

### 3.2 Emit normalized controller state

Drive `ControllerInputState`. Never construct a `ControllerState` yourself, and never build report
bytes.

| Host event | Call |
|---|---|
| Face/shoulder/stick-click/Start/Select/Home key | `pressButton(positional, pressed)` |
| D-pad key | `pressDpad(up=, right=, down=, left=)` — pass only what changed |
| One analog event (sticks + triggers + hat) | `applyAnalog(AnalogFrame(...))` |
| On-screen / software button | `setVirtualButton(logical, pressed)` |
| Source changed | `setSource(identity)` |
| Focus lost, link lost, teardown | `neutralize()` |

Four rules that are easy to get wrong:

1. **`pressButton` takes the POSITIONAL button.** The bottom face button is `A` no matter what the
   plastic says. `ControllerLayoutResolver` applies the Nintendo/Xbox face mapping at publish
   time. A backend that pre-swaps A/B breaks the layout setting.
2. **One host event = one `applyAnalog` call.** Publishing sticks, triggers and hat separately
   emits three snapshots per event and lets an observer see a half-applied frame.
3. **`AnalogFrame.dpad == null` means "this source has no hat axes"**, not "the hat is centered".
   Passing `DpadState.None` for a source with no hat would repeatedly cancel a held D-pad key.
4. **Normalize axes with `AxisRange`**, using the host's reported minimum/maximum/flat. Do not
   assume `-1..1`, and do not invent a dead zone. Sticks land on `0..255` centred at `128`;
   triggers on `0..255` from `0`.

Also report the SOURCE half of `DeviceCapabilities` (buttons, sticks, triggers, D-pad). The
session fills in the host half (IMU, actuators, battery) from its own backends.

### 3.3 Convert motion into canonical coordinates

Implement `MotionBackend`. The convention is defined once, in `MotionConvention`, and is
**not negotiable per platform**:

- right-handed, in the frame the device is **held**: `+X` right along the top screen edge, `+Y` up
  toward the top of the screen, `+Z` out of the screen toward the user;
- gyroscope: angular rate about those axes, right-hand-rule positive, **16.384 counts/dps**;
- accelerometer: proper acceleration, **gravity-inclusive** (face-up reads about `+1 g` on Z),
  **8192 counts/g**;
- timestamp: free-running **100 µs** ticks from the SENSOR SAMPLE, never from send time,
  truncated to 16 bits (wrap is expected and handled downstream).

`MotionScale` converts from SI (rad/s, m/s²) and truncates a nanosecond sensor clock.
`ScreenOrientation.apply(sample, rotationDegrees)` converts the device's natural frame to the held
frame — convert your platform's rotation constant to degrees first; the shared layer never learns
your platform's enum.

Return `ControllerMotion.None` until every required sensor has reported at least once. Tolerate
repeated `start()`/`stop()`: motion is gated on the console's real demand for it.

`diagnostics()` must return all three layers — the host's own reading, the canonical conversion,
and the frame rotation applied plus whether it could be **measured**. An unmeasurable rotation is
itself the defect, not a value worth trusting.

### 3.4 Advertise capabilities

Fill in `DeviceCapabilities` honestly. `rumbleMotors` is the number of **independently drivable**
actuators: `0` means the bridge skips output entirely, `1` means amplitudes get collapsed, `2+`
means left/right survive to hardware.

Do not add capabilities the bridge does not read. An unread flag is a claim nobody checks.

### 3.5 Implement the transport

Implement `BridgeTransport` and `BridgeHost`. Your responsibility is **transport mechanics only**:

- register the descriptor (`BridgeHidDescriptor.bytes`, plus the SDP name/description/provider)
  with the host's Bluetooth stack as a HID **device**;
- enumerate paired adapters (`knownHosts()`);
- open/close the link and push report bytes (`send(reportId, payload)`);
- report progress through `BridgeTransport.Listener`.

Three portable lessons from the Android backend:

- **Trust the callbacks, not the return values.** At least one shipping stack returns `false` from
  registration and then reports success asynchronously. Use bounded timeouts only for the case
  where the callback never arrives at all.
- **Release host-wide resources on disconnect.** Android has exactly one HID Device slot per
  system; holding it across a dropped link makes the next attempt collide with your own orphaned
  registration. Assume other hosts have similar single-owner resources.
- **Output reports arrive in more than one framing.** Forward whatever you received to
  `Listener.onOutputReport(data, reportId)`; `BridgeOutputCodec` already tolerates a payload with
  or without a leading report ID, and rejects anything that cannot be a bridge output report.

`Listener.currentReport()` must be answerable synchronously — it composes a fresh, complete report
for a control-channel poll.

### 3.6 Receive normalized output requests

Implement `OutputBackend`. You are handed a `RumbleRequest(left, right)` — two amplitudes,
`0..255`, and nothing else. There is no duration, waveform, effect handle or platform constant in
the protocol, because the console holds an amplitude until it changes it.

Yours to decide: effect construction, amplitude control, retrigger rate, usage classification, and
which actuator to bind. `bindToSource(identity)` **must** be honoured on every source change; at
least on Android the correct actuator belongs to the selected device rather than the application,
and getting that wrong is silent, total rumble loss.

`RumbleShaping` (gate, hysteresis, quantization) is in Bridge Core and available to you. Use it if
your API cannot change an effect's amplitude in flight; skip it if it can.

`keepAlive()` is ticked while the link is live, so a repeating effect cannot outlive a bridge that
went quiet. `stop()` is called on every teardown path.

### 3.7 Expose diagnostics

Implement `BridgeDiagnostics` over whatever your platform already has (logcat, event log,
journald, a file). Bridge Core, the transport and the backends all write to it, so the operator
gets one stream.

Keep the layers distinct — `MotionDiagnostics` and `OutputDiagnostics` exist so a fault localizes
itself:

```text
platform raw     what the host reported, in the host's units
canonical        what the backend made of it, in bridge units
wire             what the encoder produced
```

A platform reading that disagrees with the canonical one is a backend bug; a canonical reading that
disagrees with the wire bytes is a protocol-layer bug. Structured and edge-triggered, never
always-on at report cadence.

---

## 4. What you must NOT reimplement

| Belongs to Bridge Core | Why |
|---|---|
| `ControllerState`, `ControllerButton`, `ControllerMotion`, `ControllerBattery` | One normalized model, or the protocol has as many dialects as it has backends. |
| `ControllerReportEncoder` / `BridgeHidDescriptor` | The firmware matches the descriptor **byte for byte**. A second encoder is a second chance to diverge. |
| `BridgeOutputCodec` | Framing tolerance and strict rejection are protocol properties, not platform workarounds. |
| `ControllerInputState` | Held state, D-pad merging, layout application, neutralization. |
| `ControllerLayoutResolver` + `ControllerFaceLayout` | The audited handheld identity table and the A/B–X/Y swap. |
| `ControllerCandidates` | The usability/exclusion rule. |
| `BridgeSession` | Cadence, motion gating, battery polling, report accounting, teardown ordering. |
| `AxisRange`, `DpadState.fromAxes`, `MotionScale`, `ScreenOrientation`, `RumbleShaping` | Shared normalization maths. |
| `SessionResumePolicy` | When it is safe to take the console back after a focus resume. |

---

## 5. Sanity checks for a new backend

Before writing code, answer these. If any answer is "I would have to pretend to be Android", the
abstraction is wrong and the boundary should move — not your backend.

- Can this platform produce a `ControllerState` from its own input API without borrowing Android's
  key codes or axis constants?
- Can it satisfy `MotionConvention` — the held frame, gravity-inclusive acceleration, 100 µs sample
  timestamps — from its own sensor API?
- Can it consume `RumbleRequest(left, right)` without knowing anything about Android vibrators?
- Can it register a HID report descriptor and connect to a paired host as a HID **device**? (This
  is the real feasibility gate, exactly as it was for Android.)

## 6. Sketch: the three platforms

Documentation only. No Windows or Linux backend exists, and none is planned in this pass.

```text
Android      (implemented, hardware-validated)
  input      InputDevice / KeyEvent / MotionEvent
  motion     SensorManager (TYPE_GYROSCOPE, TYPE_ACCELEROMETER) + DisplayManager rotation
  output     InputDevice-scoped VibratorManager, falling back to the system vibrator
  transport  BluetoothHidDevice (Classic BR/EDR HID Device profile, API 28+)
  battery    ACTION_BATTERY_CHANGED sticky broadcast

Windows      (not implemented)
  input      RawInput / XInput / Windows.Gaming.Input
  motion     Windows.Devices.Sensors, or a handheld's vendor IMU
  output     XInput / Windows.Gaming.Input vibration, or the vendor SDK
  transport  Windows.Devices.Bluetooth — NOTE: acting as a Classic HID *device* is the
             open feasibility question here, not a formality
  battery    Windows.System.Power / WMI

Linux        (not implemented)
  input      evdev (/dev/input/event*) or hidraw
  motion     IIO (industrialio) for a handheld IMU
  output     evdev force feedback (FF_RUMBLE)
  transport  BlueZ, exporting a HID device profile record
  battery    /sys/class/power_supply
```

The Windows transport line is the one to investigate first. Input, motion and output are ordinary
work on both platforms; being a Bluetooth HID **device** rather than a host is the part that
decided Android's feasibility and will decide theirs.

---

## 7. Related documents

- [`PROTOCOL.md`](PROTOCOL.md) — the wire contract this backend speaks
- [`../bluetooth/android-controller-bridge.md`](../bluetooth/android-controller-bridge.md) — the
  full Android reference, feasibility evidence, and hardware validation record
- [`../agents/ANDROID.md`](../agents/ANDROID.md) — durable Android traps worth reading even if you
  are implementing a different platform
- `tools/fixtures/android_controller_hid.h` — the C-side source of truth for the descriptor and
  wire layout
