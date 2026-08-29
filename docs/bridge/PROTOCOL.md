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
`tools/check_android_descriptor_parity.py` fails loudly if the C, Kotlin and C# descriptors ever
diverge. (The C# copy lives in the Windows companion and is optional: on a checkout without
`windows/`, the script reports two-way parity and still passes.)

---

## 1. Contract version and runtime skew

The bridge carries a single integer contract version, defined once in
[`tools/fixtures/android_controller_hid.h`](../../tools/fixtures/android_controller_hid.h) as
`ANDROID_BRIDGE_CONTRACT_VERSION`, mirrored by `BridgeContract.VERSION` in Kotlin and
`BridgeContract.Version` in C#. `tools/check_android_descriptor_parity.py` fails if any of them
disagree.

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
| `BridgeContractTests` (C#) | any change to the C# descriptor without a bump |
| `check_android_descriptor_parity.py` | any change to the **C** descriptor without a bump, plus C↔Kotlin↔C# divergence and a disagreement between the Kotlin and C# digest registries |

Byte-for-byte parity between the languages is deliberately **not** sufficient on its own: a
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

Usages 1–4 are logical A/B/X/Y in this bridge contract. The generic firmware parser represents
them internally as `JP_BUTTON_B1..B4`, which are also the source slots used by directly paired
physical controllers. The seam therefore uses descriptor-proven `from_android_bridge` provenance
to map only those four bridge usages directly to canonical A/B/X/Y. Direct controllers retain the
locked `NS2_BASE_BUTTON_MAP` B/A/Y/X policy, and every non-face bridge usage retains the ordinary
base map. This is a semantic correction, not a report-layout or contract-version change.

That provenance flag is **per device, not per origin**: the adapter sees one bridge stream and
cannot tell an on-screen press from a built-in-pad press. Getting both origins into the logical
contract is therefore entirely the companion's job — see §3.2.

The exact bridge descriptor also selects the plain sequential/no-extra generic parse profile before
the seam. A host exposes its phone or PC name and VID/PID, and those incidental values may resemble
a supported physical controller; they must never activate that controller's button table, trigger
rules, or extra-field extractor for a descriptor that already declares the bridge contract. Late
identity resolution cannot displace the descriptor-selected profile.

### 3.2 Physical layout vs logical semantics

The wire contract above is **logical**: usages 1–4 mean Nintendo A/B/X/Y. Face input reaches it
from two origins that speak different dialects, so each has its own mapper and the two are
**opposites under the same layout**:

```text
physical face key (source's own dialect) -> mapPhysicalFaceKey   -\
                                                                   >-- LOGICAL ControllerButton
on-screen face position (drawn letter)   -> mapTouchFacePosition -/         -> HID usage 1..4
                                                                           -> Android-bridge seam
                                                                           -> canonical state
                                                                           -> personality encoder
```

`ControllerFaceLayout` is `Auto` / `Nintendo` / `Xbox`. `Auto` resolves via a bounded,
hardware-audited handheld identity table; a manual override is always authoritative.

| Layout | Physical key (Controller Link) | On-screen slot (Touch Gamepad) |
| --- | --- | --- |
| `Nintendo` | pass-through — the handheld already reports its **printed legend**, so its `A` is the console's A | swap — the south slot is drawn `B` and sends B |
| `Xbox` | swap A↔B, X↔Y — a positional pad names its **bottom** button `A`, and the console's bottom button is B | pass-through — the south slot is drawn `A` and sends A |

The physical column is a **source-device quirk correction**, and both of its rows produce the same
user-visible rule: the face button you press lands on the face button in the same place on the
Switch. The on-screen column is a **presentation** choice: the letter drawn is the letter sent, and
`faceLabel()` is derived from the same mapper so a legend cannot drift from its bit.

That a Nintendo-labelled handheld reports by legend rather than by position is evidence, not
assumption: the first AYN Thor in-game pass forwarded key codes untranslated and came out inverted,
which is only possible for a legend-reporting device.

> **Do not merge the two mappers.** They were one function until 2026-08-24, and it could only ever
> be right for one origin at a time: correcting the on-screen pad silently inverted every physical
> face key on console, with every test still green.

Held physical buttons are stored **as the source reported them** and mapped at publish time, so a
layout change cannot leave a key stuck under its old meaning. (Held input is also cleared on a
layout change — a stuck button on a console is among the worst failures this bridge has.)

Home, Capture, C and the other non-face actions are already logical and are never face-swapped, on
either origin. Touch and physical input remain separate origins: releasing one must not cancel the
other. Tests for face correctness must continue through the source-aware seam and final personality
encoder — matching a drawn label to an intermediate Kotlin enum is not sufficient. Both origins now
have that cross-layer coverage, keyed off a shared fixture:

| Origin | Fixture | Head (Kotlin) | Tail (C) |
| --- | --- | --- | --- |
| On-screen | `tools/fixtures/touch_face_mapping.csv` | `TouchProfileCatalogTest` | `tools/test_touch_layout_face_goldens.c` |
| Physical | `tools/fixtures/controller_link_face_mapping.csv` | `ControllerLinkFaceMappingTest` | `tools/test_controller_link_face_goldens.c` |

The physical fixture gained a third consumer with the Windows companion:
`ControllerLinkFaceMappingTests` (C#). It also carries the property behind the table rather than
only its rows — that `mapPhysicalFaceKey` and `mapTouchFacePosition` can **never** resolve alike for
a face button. Collapsing the two mappers is what broke both origins in turn, and asserting the
property is what stops a future simplification from doing it again.

### 3.3 Unmapped physical buttons — durable rule

> Unknown or additional physical controller buttons are preserved as candidates for future custom
> mapping rather than silently assigned to unrelated controller actions.

Capture has no physical key by default on any audited device; it is reached through a virtual
button, as are Home (which also accepts a platform "mode" key) and C/GameChat.

### 3.4 Encoder goldens — `tools/fixtures/bridge_report_goldens.csv`

**Added 2026-08-29 with the Windows companion.** The descriptor guard proves that every
implementation **describes** the same report. It cannot show that they **fill** it identically, and
two implementations can agree on all 161 descriptor bytes while disagreeing about which bit `GR`
sets, where the hat byte moved to in contract 4, or whether a battery level is clamped before or
after the valid flag is decided. None of that is visible until a console misbehaves.

The file is 47 vectors of `normalized state -> wire bytes`, generated from
`ControllerReportEncoder` (Kotlin) and read by both encoders:

| Consumer | Test |
| --- | --- |
| Kotlin | `BridgeReportGoldenTest` |
| C# | `BridgeReportGoldenTests` |

Coverage is asserted by the tests themselves, not left to inspection: every logical button pressed
alone, every hat code 0..8 including opposites cancelling, axis and battery clamping at both ends,
both flag halves independently, and the 16-bit motion timestamp wrap.

Regenerating the file is a protocol change, not maintenance. `BridgeReportGoldenTest.regenerate()`
exists but is deliberately **not** a test: a suite that silently rewrote its own goldens would erase
the evidence of exactly the divergence the file exists to catch. Rewrite it by hand, together with
the contract bump the change requires.

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
