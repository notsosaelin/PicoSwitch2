# PicoSwitch Bridge — platform-neutral contract

Status: ✅ wire contract hardware-validated (Android backend, AYN Thor, 2026-08-13 for v1 input;
v2 extension host-tested). 🟡 the platform-neutral *framing* of that contract is new as of
2026-08-15 and has not yet been re-validated on hardware.

This document defines the PicoSwitch Bridge independently of any host operating system. The
Android companion is **an implementation of this contract**, not the contract itself. A second
implementation should be writable from this document plus
[`PLATFORM_BACKEND.md`](PLATFORM_BACKEND.md), without reading Android code.

The C-side source of truth for the byte layout remains
[`tools/fixtures/android_controller_hid.h`](../../tools/fixtures/android_controller_hid.h);
`tools/check_android_descriptor_parity.py` fails loudly if the Kotlin and C descriptors ever
diverge.

---

## 1. Contract version and runtime skew

The bridge carries a single integer contract version, defined once in
[`tools/fixtures/android_controller_hid.h`](../../tools/fixtures/android_controller_hid.h) as
`ANDROID_BRIDGE_CONTRACT_VERSION` and mirrored by `BridgeContract.VERSION` in Kotlin.
`tools/check_android_descriptor_parity.py` fails if the two disagree.

| Version | Meaning |
|---|---|
| 1 | v1 only: buttons 1..14, sticks, triggers, hat. No vendor extension. |
| 2 | Vendor extension added — motion, battery, flags, **millisecond** motion timestamp — plus output report 2. |
| 3 | **Current.** Buttons 1..15 (15 = C / GameChat, same two bytes) and the motion timestamp redefined to **100 µs** ticks. |

### 🔴 When to bump — read before editing the descriptor

Bump for **anything a peer can observe**:

- descriptor bytes of any kind (report count, usage minimum/maximum, field width, logical range,
  collection structure, report IDs);
- wire layout (offsets, field sizes, endianness);
- units or semantics of an existing field;
- capabilities implied by the profile (adding or removing motion, battery, output);
- the output report's contents or meaning.

Do **not** bump for implementation-only changes — comments, formatting, internal refactors, test
edits — anything that leaves the bytes and their meaning identical.

### How the rule is enforced

A **SHA-256 of all 161 descriptor bytes** is registered per contract version in
`BridgeContract.DESCRIPTOR_DIGESTS`. Change any byte and the digest moves, so the only way forward
is to bump the version and register the new digest — a deliberate act, which is exactly what was
missing when 14 buttons became 15.

Two independent checks, covering both languages:

| Check | Catches |
|---|---|
| `BridgeContractTest` (JVM) | any change to the Kotlin descriptor without a bump |
| `check_android_descriptor_parity.py` | any change to the **C** descriptor without a bump, plus C↔Kotlin divergence |

Byte-for-byte parity between the two languages is deliberately **not** sufficient on its own: a
coordinated edit to both sides keeps them equal while silently changing what goes on the wire. The
digest is what makes that case fail. Verified by mutating one byte in the vendor motion block — far
from the three that define contract 3 — in both languages at once: both checks failed and printed
the digest to register.

Both failures print the exact steps, including *reflash the adapter before testing the new APK*.

### Why this exists — incident, 2026-08-15

> C/GameChat changed the descriptor from 14 buttons + 2 pad bits to 15 + 1. The companion APK was
> updated while the adapter kept running older firmware. `android_bridge_identify()` does an
> **exact** 161-byte match, so it failed and the firmware fell back to the v1 generic profile.
> Buttons, sticks, triggers and the hat kept working — they are v1 fields — while **battery,
> motion and rumble/player-LED disappeared together**, because all three are gated on that one
> match. Every source-level parity check passed: they compare the source tree to the source tree
> and cannot see what is flashed.

**That symptom set — ordinary input fine, battery + motion + rumble all gone — is the signature of
a contract skew.** Check it first.

### Diagnosing a skew

Adapter, over UART:

```text
bridge          -> {"bridge_identify":{"contract":3,"build":"3f1627fb","calls":2,"matched":2,
                    "rejected":{"null":0,"length":0,"content":0},"last_len":161,"expected_len":161,
                    "first_mismatch":-1,"profile":"v2-bridge","suspected_skew":false}}
bridge clear    -> reset the trace
```

`profile` is the answer: `v2-bridge` means identification succeeded and all features are enabled;
`v1-generic` means it failed. On a content rejection, `first_mismatch` gives the byte offset with
the expected and received values. `suspected_skew` is true when a peer presented a descriptor of
exactly the expected **length** but different **content** — almost certainly this bridge built
against a different contract rather than an unrelated gamepad. It is a hint for humans and is never
used to authorize anything.

Companion: the adapter's `info` reply carries `bridge_contract` and `build`; the app compares them
with its own `BridgeContract.VERSION`. A mismatch, **or firmware that reports no contract at all**,
appears on the controller-link card and in the diagnostics export. Firmware that reports nothing
predates contract reporting and is therefore older than the app, so it is reported as
`UNVERIFIED` — never as compatible.

## 2. Identity

The adapter identifies a bridge by an **exact byte match on the HID report descriptor**
(`BridgeHidDescriptor.bytes`, 161 bytes), never by VID/PID. A host reports its own phone or PC
identity, which varies per vendor and cannot authorize output. Motion, battery, rumble and the
player indicator are enabled only for a device that declares this exact contract.

SDP record: name `PicoSwitch Bridge Controller`, description `Host controls passthrough`, provider
`PicoSwitch2`, subclass = combo | gamepad. The firmware does not read these; they exist so a
capture is legible.

## 3. Normalized controller state

The single value that crosses the platform boundary in the input direction.

| Field | Range | Neutral |
|---|---|---|
| `leftX`, `leftY`, `rightX`, `rightY` | `0..255` | `128` |
| `leftTrigger`, `rightTrigger` | `0..255` | `0` |
| `buttons` | set of `ControllerButton` | empty |
| `dpadUp/Right/Down/Left` | four retained booleans | all false |
| `motion` | `ControllerMotion`, see §5 | invalid |
| `battery` | `ControllerBattery` | invalid |

Units are the wire's units on purpose, so the encoder is a copy rather than a second place scaling
can be wrong. Backends normalize into them using the host's reported axis range
(`AxisRange(minimum, maximum, flat)`), not an assumed `-1..1`.

The D-pad is **four retained directions**, not a hat code: opposite directions cancel only at
encode time, and releasing one side restores the still-held side without inventing an edge.

### 3.1 Buttons — ordinal is the wire bit

```text
0  A            5  R1        10 LeftStick     (L3)
1  B            6  L2        11 RightStick    (R3)
2  X            7  R2        12 Home
3  Y            8  Select    13 Capture
4  L1           9  Start     14 C  (GameChat)
```

The encoder writes `1 shl ordinal`; the firmware's generic sequential profile reads usage
`ordinal + 1`. **Append only, never reorder.** The high button byte is masked `0x7F`, not `0x3F` —
a stale mask silently drops C while everything else keeps working.

### 3.2 Physical layout vs logical semantics

Three distinct things, kept distinct:

```text
platform key / bit  ->  POSITIONAL ControllerButton  ->  layout mapper  ->  LOGICAL ControllerButton
     (backend)                 (backend)                   (core)              (wire)
```

Hosts report face buttons positionally: the bottom face button is `A` on Android and on XInput
regardless of the printed legend. `ControllerFaceLayout` is `Auto` / `Nintendo` / `Xbox`;
`Nintendo` swaps A↔B and X↔Y, and `Auto` resolves via a bounded, hardware-audited handheld
identity table with a manual override that is always authoritative.

Held buttons are stored **positionally** and mapped at publish time, so a layout change cannot
leave a key stuck under its old meaning. (Held input is also cleared on a layout change — a stuck
button on a console is among the worst failures this bridge has.)

Virtual (on-screen) buttons are already logical and are **not** face-swapped. They are a separate
origin from physical keys: releasing one must not cancel the other.

### 3.3 Unmapped physical buttons — durable rule

> Unknown or additional physical controller buttons are preserved as candidates for future custom
> mapping rather than silently assigned to unrelated controller actions.

Capture has no physical key by default on any audited device; it is reached through a virtual
button, as are Home (which also accepts a platform "mode" key) and C/GameChat.

## 4. Capabilities

`DeviceCapabilities` has two halves with different owners, merged by the session:

- **Source half** (input backend): `gamepadButtons`, `analogSticks`, `analogTriggers`, `dpad`.
- **Host half** (session, from its own backends): `gyroscope`, `accelerometer`, `rumbleMotors`,
  `battery`.

`rumbleMotors` is the count of independently drivable actuators: `0` = no output path, `1` =
amplitudes collapse, `2+` = left/right survive to hardware.

Every field is read by something. Capabilities no code consumes are deliberately absent.

## 5. Motion convention

**One convention, defined once, for every platform.**

Frame — right-handed, in the orientation the device is **held**, not manufactured:

```text
+X  right, along the top edge of the screen
+Y  up, toward the top of the screen
+Z  out of the screen, toward the user
```

- **Gyroscope**: angular rate about those axes, right-hand-rule positive, **16.384 counts/dps**.
- **Accelerometer**: proper acceleration, **gravity-inclusive** — face-up on a table reads about
  `+1 g` on Z, not zero. The firmware's stillness and bias tracking depend on this.
- **Timestamp**: free-running **100 µs** ticks, from the sensor sample, truncated to 16 bits
  (wraps every 6.5536 s; the firmware takes the delta in the field's own modulus).

Fixed-point counts rather than SI because the wire is fixed point and a float intermediate would
put rounding in two places. These match what the adapter already receives from a DualSense, so a
host reuses the hardware-validated translation instead of adding a second scaling convention.

Two rules that have each cost real debugging time:

1. **The timestamp must come from the sensor sample, never from send time.** The report cadence
   (125 Hz) is faster than a typical IMU delivers, so the same physical sample is sent more than
   once and the firmware de-duplicates on this field. Stamping at send time makes every repeat look
   like a fresh IMU frame, which the console integrates as real movement.
2. **The natural→held frame correction is mandatory and must be measurable.** Platforms report
   sensors in the device's manufactured orientation. On a natural-portrait handheld held in
   landscape, skipping the correction is a 90° error about the screen normal: yaw survives, pitch
   and roll are exchanged and inverted. A backend that cannot *measure* display rotation must say
   so (`MotionDiagnostics.frameRotationMeasured = false`) rather than silently report 0°.

`MotionConvention` / `MotionScale` / `ScreenOrientation` implement all of this in Bridge Core; a
backend converts its own units and rotation constant and calls them.

## 6. Output semantics

The adapter sends what it **wants**, never how to produce it.

```text
BridgeOutput(
    rumble          = RumbleRequest(left 0..255, right 0..255),
    playerIndicator = 0..8,   // 0 = none assigned
    motionRequested = Boolean,
)
```

- **`rumble`** carries two amplitudes and nothing else — no duration, waveform, effect handle or
  platform constant. The console holds an amplitude until it changes it. A single-actuator host
  collapses via `RumbleRequest.strongest`; that collapse belongs to the host, not the model.
- **`playerIndicator`** is the console's player number, for whatever the host can show.
- **`motionRequested`** is a resource gate, not an output. The adapter derives it from the
  console's real negotiated IMU state (`ns2_motion_negotiated()`); the host registers sensors only
  while it is set. It is edge-triggered — a repeated request must not re-register sensors.

An output report the codec cannot parse is **rejected**, never half-applied.

## 7. Wire format

### Input report 1 — host → adapter, 26 bytes on the wire

| Bytes | Field |
|---|---|
| `0` | report ID (`1`) |
| `1..6` | X, Y, Z, Rz, Rx, Ry — left stick, right stick, then triggers, `0..255` |
| `7..8` | buttons 1..15 + 1 pad bit (15 = C / GameChat) |
| `9` | hat, low nibble, `8` = neutral |
| `10..21` | gyro X,Y,Z then accel X,Y,Z, `int16` LE |
| `22` | battery `0..100` |
| `23` | flags: `0x01` charging, `0x02` motion valid, `0x04` battery valid |
| `24..25` | motion timestamp, 100 µs ticks, `uint16` LE |

The payload a transport sends excludes the report ID, so `ControllerReportEncoder`'s offsets are
these minus one. Motion and battery are written only when their validity bit is set, so idled
sensors clear rather than latch a stale sample.

Keep all six axes **unsigned**. The adapter's generic parser scales a raw field against its logical
maximum and does not apply a signed logical minimum, so a conventional signed `-32768..32767`
descriptor would parse incorrectly even though it is valid HID.

### Output report 2 — adapter → host, 5 bytes

`[id=2][rumble L][rumble R][player 0..8][flags]`, flags bit 0 = motion wanted.

### Versioning

There is none, and none is needed. The v2 vendor block is **appended after** the v1 fields, so
every hardware-validated byte offset is unchanged; the firmware derives the report length from the
descriptor. A v1 host against v2 firmware still works, and a v2 host against v1 firmware still
delivers buttons. Development-build compatibility is explicitly not a goal — see
[`../agents/ANDROID.md`](../agents/ANDROID.md).

## 8. Session semantics

Owned by `BridgeSession`, identical on every platform.

**Link phases** — `Idle → Preparing → Registering → Ready → Connecting → Playing`, plus
`Unsupported` (this host cannot act as a HID device at all) and `Failed`. `Preparing` resets
per-link counters; every other transition keeps them.

**Send cadence** — 125 Hz ceiling (8 ms). Two modes:

- **motion off**: change-driven. An unchanged state generates no traffic. This is the
  hardware-validated v1 behavior.
- **motion on**: time-driven. A fresh IMU sample goes out every interval even when nothing moved.

A capacity-one conflating mailbox sits between input and the sender, so a pending snapshot is
always replaced by newer truth rather than queueing stale history.

**Battery** is polled on a 30 s timer, and only while the link is up.

**Neutralization** is required at every boundary: source change, layout change, focus loss, link
loss, and teardown. A held input that outlives its boundary reaches the console as a stuck button.
`stop()` sends a neutral report *before* tearing the link down, while the link can still carry it.

**Teardown ordering** — losing the link must stop the actuators, unregister the sensors, clear held
input, and forget the adapter's last request. All four, on every path.

**Report accounting** — a send the transport rejects is surfaced as a message, not counted.

## 9. Transport boundary

`BridgeTransport` is deliberately narrow: attach a listener, enumerate known hosts, start/connect,
push report bytes, stop, close. Everything above — composition, cadence, gating, rumble, battery,
neutralization — is session semantics and stays in Bridge Core.

Progress is reported through callbacks rather than return values, because on every host stack
examined so far the synchronous return of a registration or connection call is a request
acknowledgement, not an outcome.

`Listener.currentReport()` answers a control-channel poll with a freshly composed report.

## 10. Diagnostics contract

Three layers, kept separate so a fault localizes itself:

```text
platform raw   what the host reported, in the host's own units and frame
canonical      what the backend made of it, in bridge units
wire           what the encoder produced
```

`MotionDiagnostics` carries the first two plus the frame rotation applied and whether it was
measured. `OutputDiagnostics` carries a free-text route (which actuator was bound and what every
other candidate reported — platform-specific by nature), the motor count, and a separate
`warning` for the different question of whether something outside the app will discard the effect
anyway.

Structured and edge-triggered. Never always-on at report cadence.
