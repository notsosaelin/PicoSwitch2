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
 physical controls / host device APIs      touch contacts / pointer APIs
              |   PLATFORM INPUT BACKEND              |   PLATFORM TOUCH ADAPTER
              |                                       v
              |                          TouchContactTracker            (Bridge Core)
              |                                       v
              |                           TouchControlEngine            (Bridge Core)
              |                                       |   TouchContribution
              v                                       v
      ControllerInputState                (Bridge Core)   <- ONE of the two, by authority
              |
              v
        BridgeSession                     (Bridge Core)
              |   ControllerReportEncoder  (Bridge Core)
              v
       BridgeTransport                    (PLATFORM)
              v
         PicoSwitch2
```

Both input paths terminate in the same normalized state, and the adapter cannot tell them apart —
which is the point. Touch origin is a host-side concern; the firmware learns nothing new.

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
> That path is historical ownership, not an Android architecture boundary. Do not create a second
> touch-layout core beside it. When a second host appears, relocate this existing module upward
> without changing its package/API; nothing in it depends on the current path.

A JVM platform (a desktop Kotlin/Java frontend) can depend on `:bridge-core` directly — Level 2
sharing. A non-JVM platform reimplements the same documented model — Level 1. Both are supported
outcomes; this document is written so Level 1 is sufficient.

### 2.1 The second host arrived, and did NOT trigger the relocation

**Settled 2026-08-29.** A Windows companion now exists under `windows/companion/` (design:
`WINDOWS_PASS.md`). It is **C#**, so it cannot link Kotlin/JVM bytecode: it is a **Level 1**
consumer, and relocating `:bridge-core` upward would have delivered churn and zero reuse. The
relocation trigger in the quotation above is specifically a **JVM** second consumer. `:bridge-core`
and `:management-core` therefore stay exactly where they are.

Recorded here because the sentence above reads as an instruction to relocate on the arrival of
*any* second host, and a future contributor should not spend a pass carrying out a move this
project already declined for a stated reason.

What makes Level 1 safe is that both implementations read the **same fixtures**, not copies of
them:

| Fixture | Guards |
|---|---|
| `tools/fixtures/android_controller_hid.h` | descriptor bytes, offsets, contract version |
| `tools/fixtures/bridge_report_goldens.csv` | **normalized state → wire bytes**, in both encoders |
| `tools/fixtures/controller_link_face_mapping.csv` | physical key + layout → logical button/usage |
| `tools/fixtures/touch_face_mapping.csv` | on-screen slot + layout → usage |
| `tools/fixtures/management/protocol-v1.json` | management encode/decode conformance |

`tools/check_android_descriptor_parity.py` covers C, Kotlin and C#; a one-sided edit to any of the
three fails it. The report goldens are new with the Windows pass and close a gap the descriptor
guard could not: the descriptor proves both ends **describe** the same report, the goldens prove
they **fill** it identically.

A Windows *backend* — the transport and the input/motion/battery/output implementations this
document specifies — does not exist yet; it is Roadmap Phase 2 and Phase 6, and Phase 6 is gated on
the peripheral-role experiment in `WINDOWS_PASS.md` §14.5. This section will gain the Windows
backend's capability notes when that work lands.

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
| Face/shoulder/stick-click/Start/Select/Home key | `pressButton(reported, pressed)` |
| D-pad key | `pressDpad(up=, right=, down=, left=)` — pass only what changed |
| One analog event (sticks + triggers + hat) | `applyAnalog(AnalogFrame(...))` |
| On-screen / software button | `setVirtualButton(logical, pressed)` |
| Source changed | `setSource(identity)` |
| Focus lost, link lost, teardown | `neutralize()` |

Four rules that are easy to get wrong:

1. **`pressButton` takes the button AS THE SOURCE REPORTED IT** — forward the platform's own key
   code meaning and nothing else. `ControllerLayoutResolver.mapPhysicalFaceKey` applies the
   Nintendo/Xbox correction at publish time, and a backend that pre-swaps A/B breaks the layout
   setting. Note that "as reported" is not the same as positional: a positional/Xbox-style pad
   names its bottom button `A`, but a Nintendo-labelled handheld names its keys after the printed
   legend. Deciding between those two is the layout setting's whole job, and it belongs in the
   resolver, not in a backend.
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

### 3.7 Drive the on-screen controller, if the host has a touchscreen (optional)

A touchscreen host is a complete controller source even with no gamepad attached. Bridge Core owns
the whole of what that means; the platform supplies contacts, a rectangle, and a way to buzz.

The profile/layout pipeline is shared core logic:

```text
confirmed controller personality
  -> TouchControllerProfile: fixed output bindings + a control CATALOG
    -> TouchLayoutDocument.authoredDefault  (the shipped starting point)
      + TouchProfileLibrary: factory profile + user profiles, one selected
        + that profile's versioned TouchLayoutDocument (a list of INSTANCES)
          -> TouchLayoutComposer -> TouchLayoutAudit -> resolved layout
            -> PLATFORM renderer and pointer adapter
```

#### Two statements that keep the model honest

> **Control instance identity is not logical button identity.**
> A `TouchControlInstance` is an on-screen object. Its `instanceId` is unique within a layout; its
> `catalogId` says what kind of thing it is, and is not. Any number of instances may name the same
> catalog entry and therefore the same binding, and they remain separate objects with separate hit
> regions, transforms, latches and contacts. Anything downstream that keys state by BINDING rather
> than by INSTANCE reintroduces the bug this model exists to prevent: a second A button whose
> release also releases the first.

> **Default layout membership is not personality capability.**
> `TouchControllerProfile.catalog` says what may be instantiated. `inDefaultLayout` says only
> whether the SHIPPED arrangement places one. A control absent from a layout is simply not on
> screen — it is still fully addable, and one that is present may be genuinely deleted. There is no
> hidden template ghost that "showing" reveals; that was the version 1 model and it is why an
> optional control could not be removed.

`TouchControllerProfile`, the catalog, authored defaults, composition, editor operations,
undo/redo, toolbar placement, alignment/snapping, audit rules, schema metadata, the profile library
and both store interfaces (`TouchLayoutOverrideStore`, `TouchProfileLibraryStore`) are in
`:bridge-core`. A platform owns the store implementation and raw storage mechanism, its renderer,
pointer/event conversion, and editor widgets. Android's implementation uses app-private
`SharedPreferences`, Compose Canvas, and Compose pointer events; none is visible to the shared
model. `TouchProfileLibraryJsonCodec` is the Kotlin JSON reference codec for the platform-neutral
document schema. A non-JVM host implements that JSON schema rather than treating a
kotlinx.serialization implementation as universal.

A profile is an ENVELOPE around a layout document, not a second layout representation: its
personality, template id and template revision are read back off the document rather than stored
again beside it. The factory profile is synthesized from the shipped template on every read and is
never persisted, which is what makes its protection structural instead of a flag a stored document
could clear. Saving onto it creates a new user profile; the editor never has an outcome that
discards an edit or overwrites the recoverable default.

#### Instance geometry, and why it has two position terms

```text
centre = region.left + anchorX * region.width + offsetXUnits * unit
```

`anchorX`/`anchorY` are NORMALIZED, so they follow the rectangle and keep a control at the edge a
thumb can reach. `offsetXUnits`/`offsetYUnits` are in LOGICAL UNITS, so they are immune to
aspect-ratio distortion — which is what keeps a square button diamond square on a display the
layout was not authored for. An ungrouped control normally carries only an anchor; a member of a
cluster shares its anchor with its siblings and differs by its offset.

Group transforms exploit exactly that. Grouping and ungrouping are pure `groupId` changes and never
touch geometry, so they are lossless on any window shape; a group scale or rotation writes the
displacement into the offsets, where it stays rigid. `groupId` lives on the instance rather than in
a separate group table, which makes "an instance in two groups" and "a group with a dangling
member" states this model cannot represent.

#### Rotation

`TouchControlInstance.rotationDegrees` is the USER's rotation, RELATIVE to the catalog entry's
authored orientation, normalized to `[-180, 180)`. The composer adds the two into
`TouchControlSpec.visualRotationDegrees` — one total, so a renderer and a hit tester cannot read
different angles — and carries the authored value separately so "reset orientation" means the
authored angle rather than a blind zero.

Rotation is visual and hit geometry ONLY. It never changes a binding, never re-labels a D-pad
direction, and never turns an analog trigger's position-derived travel axis. `ResolvedTouchControl`
hit-tests a rotated control by inverse-transforming the POINT rather than by building a rotated
polygon, so the same local shape test serves both cases and nothing is allocated on the contact
path. `hitExtentX`/`hitExtentY` are the answerable region's screen-space half-extents after
rotation, computed per shape, and every screen-space question — inside the safe rectangle? could
these two overlap? how far may this be dragged? — is asked with those rather than with the
control's own unrotated frame.

> Measuring a rotated control with its unrotated box is not hypothetical. The shipped GameCube
> layout brings `z` and the `Y` bean close enough to touch at every aspect ratio at or squatter
> than the 2:1 authoring reference — that is, on every ordinary phone in landscape — and the pre-2.0
> audit could not see it at all, because the bean is authored at −11° and its unrotated box is 3.7
> units shorter than the shape reaches.
>
> What the corrected measurement then showed is that the contact is entirely between the two
> controls' hit MARGINS; the drawn shapes clear each other by about a unit. So the shipped geometry
> is fine and stayed at y=42 (GameCube template revision 2). An earlier pass moved the row to y=34
> on the strength of the finding alone and was reverted. The audit now separates the two cases —
> only an ARTWORK collision blocks, a margin contact is reported and playable — because that is the
> distinction that decides whether the router has to let z-order pick what the user pressed. See
> `TouchGameCubeDefaultTest` for the probe that establishes it.

#### Duplicate-safe runtime aggregation

`TouchControlEngine` keys every contribution by INSTANCE and aggregates at publish:

```text
digital   any live instance holding a binding keeps that binding pressed
trigger   the deepest live instance of that side wins (max)
stick     the owning instance speaks; the others say nothing
D-pad     the owning instance speaks; the others say nothing
```

Sticks and the D-pad cannot be ORed — two full deflections in different directions have no
meaningful sum, and averaging invents a third direction nobody asked for — so ownership arbitrates:
the first instance to move one wins it until its contact ends, then the next instance still being
touched takes over, chosen in layout order so the answer is deterministic. The retrigger mask is a
set of instance ids applied at publish, so tapping one held A to re-fire it cannot silence another.

The router picks by priority, then z-order, then centrality. Z-order sits above centrality because
once instances may be freely stacked, the control the user can SEE on top is the one they believe
they are pressing. Two instances of the SAME output may overlap freely and the audit permits it;
overlap between DIFFERENT outputs stays blocking.

Alignment assistance (`TouchEditorAlignment`) is pure and operates on already-resolved pixel
geometry, so grid pitch, snap tolerance and guide matching behave identically on every host and are
testable at every window shape without a device. Snapping computes one correction from a single
reference control and applies it to the whole movement, so a multi-control selection cannot be
pulled apart; a movement larger than the tolerance always wins, so guides never restrict placement.

`TouchGroupGeometry` supplies the shipped square-diamond and irregular-cluster relationships that
the authored templates are written against; the anchor-plus-offset representation those produce is
the same one every user group uses, described above. A group scale changes offsets and member sizes
together; an individual scale changes only that control's visual/hit size. The platform still
receives a flat list of independent controls for rendering and contact ownership.

The editor's toolbar placement is shared logic too (`TouchToolbarLayout`), because every rule in it
is a rule about REACHABILITY — which safe edge is close enough to dock to, whether a remembered
floating position still lies inside the window — and a reachability rule that lives in one
platform's UI layer is a rule the next platform gets wrong. A toolbar off the edge of the window is
a toolbar with no Done button.

Undo/redo (`TouchEditorHistory`) is a bounded stack of whole DOCUMENTS rather than of invertible
commands. Every editor operation is already a pure total function from one document to the next, so
a revision stack cannot desynchronize from what it is undoing, needs no inverse written and kept
correct for each new operation, and makes gesture coalescing a question of when a revision is
pushed rather than of merging command objects. Undoing a delete restores every field the control
had — transform, rotation, group membership, z-order, hold setting — because a revision is the
whole scene.

The GameCube face-button shape and placement were checked against the maintainer's local controller
reference and Dolphin's upstream
[`InputOverlay.kt`](https://github.com/dolphin-emu/dolphin/blob/051133787e77a154c83fcf54c7acc83b76fe7d81/Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/overlay/InputOverlay.kt),
[`InputOverlayDrawableButton.kt`](https://github.com/dolphin-emu/dolphin/blob/051133787e77a154c83fcf54c7acc83b76fe7d81/Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/overlay/InputOverlayDrawableButton.kt),
and Dolphin's `gcpad_a`, `gcpad_b`, `gcpad_x`, and `gcpad_y` drawable alpha silhouettes.
The X/Y contours retain those separate silhouettes, then apply template-owned visual rotations so
each inner concavity points toward A; their interlocking relationship is therefore portable rather
than an Android-only renderer guess. The shared contour also defines the answerable touch shape,
allowing the beans to wrap around A without treating their empty concavity as an overlapping target.
Only geometric relationships were adapted; PicoSwitch2 continues to use its own Canvas renderer,
touch semantics, and code-native art.

#### The persisted schema

Schema version **2** stores a list of instances. Absent optional fields mean the stated default, so
a control the user only moved writes an anchor and nothing else and a round trip stays lossless:

```json
{
  "schemaVersion": 2,
  "personality": "gc",
  "selectedProfileId": "p1",
  "profiles": [
    {
      "id": "p1",
      "name": "Smash",
      "templateId": "picoswitch.touch.gc.v1",
      "templateRevision": 3,
      "controls": [
        { "instanceId": "a", "catalogId": "a", "anchorX": 0.85, "anchorY": 0.42,
          "offsetXUnits": -2.7, "offsetYUnits": 2.3, "zIndex": 12, "groupId": "face-cluster" },
        { "instanceId": "a#2", "catalogId": "a", "anchorX": 0.5, "anchorY": 0.5,
          "scale": 1.15, "rotationDegrees": 30.0, "zIndex": 18, "latch": false }
      ]
    }
  ]
}
```

Anchors are normalized `0..1`; scale, offset and rotation are bounded by `TouchLayoutLimits`, and
the codec applies exactly the checks an editor operation would have enforced, so a hand-edited file
cannot construct geometry the editor refuses to make.

Two kinds of damage are handled differently, on purpose:

| | Handled by | Outcome |
|---|---|---|
| A value that is not a number, or out of range | `TouchProfileLibraryJsonCodec` | Document refused; raw text kept |
| Duplicate instance id, dangling `catalogId`, impossible transform | `TouchLayoutDocumentValidator` | That instance dropped, layout `degraded`, everything else intact |

An unreadable number means the document is corrupt; an instance naming a control this build no
longer has is simply a control that is gone, and must not cost the user a whole layout.

**Schema version 1** — the retired sparse-override model — is still READ, and is migrated on decode
by `TouchLayoutMigration`. The conversion is deterministic and one-way:

```text
legacy control hidden      -> no instance; Add Control can bring it back
legacy control visible     -> one instance, id = the template control id
anchor / scale override    -> the instance's anchor / scale
groupOffsetScale override  -> baked into the instance's offset
latch override             -> the instance's latch
template editGroupId       -> the instance's group
```

Android's store writes the migrated document back on the first read and copies the original to a
`.v1` key in the same atomic commit, so a migration that produced something unusable costs nothing
that cannot be read back out of storage by hand. The very first release's single anonymous override
file is never touched at all.

Future schema versions are rejected without deleting the raw document. Profile/template mismatch
and a future template revision select the shipped matching-profile default, never another
personality.

**Implement three things and no more:**

```text
Here is contact #42, Down at x/y.        -> TouchContact(id, TouchPhase.Down, x, y)
Here is contact #42, Move at x/y.        -> TouchContact(id, TouchPhase.Move, x, y)
Here is contact #42, Up / cancelled.     -> TouchContact(id, TouchPhase.Up | Cancel, ...)
The safe interaction rectangle changed.  -> TouchLayoutResolver.resolve(layout, region)
Perform a light local press haptic.      -> TouchFeedbackBackend
```

Wire them through `TouchGamepad`, which holds the engine, the contact tracker and the authority
transitions.

Five rules, all of which have a specific failure behind them:

1. **`TouchContact.id` must be the platform's STABLE contact identifier, never its position in the
   platform's array.** Every touch platform reorders that array between events. An index-keyed
   implementation is correct with two contacts and silently swaps which control a thumb is holding
   the moment a third arrives or the first lifts.
2. **Each batch must describe every contact the platform currently knows about**, not only the one
   that changed. `TouchContactTracker` uses the batch to notice a contact the platform stopped
   mentioning without ever ending — otherwise that control is held down forever with nothing left
   that could release it. A platform that reports one changed contact per event must accumulate
   first.
3. **Coordinates are in the same space the layout was resolved into.** Density, window origin,
   rotation and system-gesture insets are the platform's to resolve BEFORE building the region. By
   the time a contact reaches the engine it is a point in the same plane as the control geometry.
4. **Resolve the layout against the interaction-SAFE rectangle**, not the window. A background may
   draw edge to edge; a trigger under a back-gesture strip is a control the user cannot press.
   Re-resolve on every size, rotation or inset change — `TouchControlEngine.setLayout` releases
   first, because every retained contact position was measured against the previous rectangle.
5. **Local touch feedback is not console rumble.** `TouchFeedbackBackend` is a UI affordance and
   must use whatever API respects the user's own haptic setting. Routing it through `OutputBackend`
   would let the interface mutate bridge output state and fight a game's own effects.

**Two platform conventions to overwrite, not invent.** `TouchLatchConfig`'s double-tap window,
minimum gap and first-tap bound, and `TouchTriggerConfig.dragSlopUnits`, all default to the stock
Android values so a host that never sets them still behaves conventionally — but a host with a
toolkit should pass its own (`ViewConfiguration.doubleTapTimeoutMillis`, `doubleTapMinTimeMillis`,
`longPressTimeoutMillis`, `touchSlop`), so the gestures match every other gesture on the device
including whatever the user's accessibility settings have done to them. Slop is reported in pixels
and stored in logical units; convert.

**Analog triggers are a profile property, not a control kind.** `TouchControlAction.Trigger(analog
= true)` means the CONSOLE-FACING controller has real trigger travel, which today is only the NSO
GameCube `L`/`R`. Such a control publishes nothing on the way down — its value comes from finger
displacement projected onto a position-derived axis — while every other trigger presses fully the
instant it is touched. A backend needs no new call for any of this; it is entirely inside the
engine. What a backend MUST NOT do is assume a trigger asserts on `Down`.

Both halves of the gesture are derived from the REGION — the direction from the control's
normalized position within it, and the distance from two budgets scaled by its shorter side — so
rule 3 above is load-bearing here in a way it is not for a button: a region that disagrees with the
space contacts are reported in does not merely misplace a hit box, it silently rotates the pull and
rescales trigger sensitivity, differently in each direction. Both were verified end to end against
a real device before the geometry was last changed; if you are porting this, capture the same
values rather than assuming.

**Input authority.** `ControllerInputState.setAuthority` decides which host control set is the
gameplay controller. Exactly one contributes; the inactive origin's mutations are discarded rather
than retained, and every transition neutralizes. Software/meta buttons (`setVirtualButton` — Home,
Capture, C) are outside the rule and always contribute, because they are host actions rather than a
second controller. Do not merge two complete controllers: a physical stick left and a touch stick
right have no defensible combined meaning, and whichever event arrived last would win by accident.

**Capabilities and actuators when the screen is the controller.** Report the on-screen controller's
own `DeviceCapabilities` (buttons, two sticks, analog triggers, D-pad) — a physical-device probe
correctly reports nothing for a touchscreen, and "nothing" is the wrong answer. Then call
`bindSource(null, touchCapabilities)`: binding to no source is what resolves the HOST's own actuator,
which is the legitimate one when the host itself is the controller. **Do not invent a synthetic input
device.** Something downstream always tries to resolve a descriptor back to a real device, and a
fabricated one resolves to nothing at exactly the moment it matters.

**Release on every boundary.** `TouchGamepad.release(reason)` is idempotent and must be called on:
contact end, gesture cancellation, mode exit, host inactivity (pause/stop/focus loss/screen lock),
geometry invalidation, authority change, link down, link stop, teardown, and any fault caught at a
boundary. Order matters while the link is still up: release the engine, THEN neutralize the session,
so the neutral report actually crosses the transport. Once the link is gone there is no longer a way
to clear a held input from the console.

**Never persist gameplay state.** Configuration survives a process restart; held contacts, pressed
buttons, stick positions and D-pad direction must not. A process that came back from the dead and
immediately told the console a button was down is the worst possible restoration.

### 3.8 Expose diagnostics

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
| `ControllerLayoutResolver` + `ControllerFaceLayout` | The audited handheld identity table, plus the two OPPOSITE A/B–X/Y mappers — one per input origin. See `docs/bridge/PROTOCOL.md` §3.2. |
| `ControllerCandidates` | The usability/exclusion rule. |
| `TouchControlEngine`, `TouchContactTracker`, `TouchGamepad` | Contact ownership, claim/exclusivity rules, release-all, authority transitions. A second host reimplementing "which control does this thumb own" would reproduce the index-versus-identifier bug from scratch. |
| `TouchStick`, `TouchDpad`, `TouchAxis` | Circular clamping, radial deadzone rescaling, eight-way sectors with radial and angular hysteresis, and the single conversion into bridge units. |
| `TouchControllerProfile`, `TouchProfileCatalog`, `TouchLayoutTemplate` | Personality contracts, complete output inventories, immutable defaults and fixed bridge bindings. |
| `TouchLayoutOverride`, `TouchLayoutComposer`, `TouchLayoutEditor`, `TouchLayoutAudit` | Sparse user state, template composition, portable edit operations over a SELECTION of controls, schema/template revision policy, and mechanical validation of overlap, target size and bounds. |
| `TouchLayoutProfile`, `TouchProfileLibrary`, `TouchProfileLibraryEditor` | Named per-personality profiles, the synthesized immutable factory entry, and the create/duplicate/rename/delete/reset/save/import rules including default-profile protection. |
| `TouchEditorAlignment` | Grid generation, guide matching and snapping over resolved geometry; assists placement without restricting it. |
| `TouchLayoutOverrideJsonCodec`, `TouchProfileLibraryJsonCodec`, `TouchLayoutOverrideStore`, `TouchProfileLibraryStore` | Kotlin reference implementations of the neutral JSON documents plus the storage boundaries; the platform implements only the backend. |
| `InputAuthority` | Which host control set is the controller, and that it is never two. |
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
- If it has a touchscreen: can it report stable contact identifiers, a complete contact set per
  event, and an interaction-safe rectangle — without the shared layer learning anything about its
  pointer API?

## 6. Sketch: the three platforms

Documentation only. No Windows or Linux backend exists, and none is planned in this pass.

```text
Android      (implemented, hardware-validated)
  input      InputDevice / KeyEvent / MotionEvent
  touch      Compose awaitPointerEventScope, keyed on PointerId; WindowInsets.safeContent
  motion     SensorManager (TYPE_GYROSCOPE, TYPE_ACCELEROMETER) + DisplayManager rotation
  output     InputDevice-scoped VibratorManager, falling back to the system vibrator
  feedback   View.performHapticFeedback (VIRTUAL_KEY / CLOCK_TICK)
  transport  BluetoothHidDevice (Classic BR/EDR HID Device profile, API 28+)
  battery    ACTION_BATTERY_CHANGED sticky broadcast

Windows      (not implemented)
  input      RawInput / XInput / Windows.Gaming.Input
  touch      WM_POINTER (pointerId is the stable identifier) or Windows.UI.Input
  motion     Windows.Devices.Sensors, or a handheld's vendor IMU
  output     XInput / Windows.Gaming.Input vibration, or the vendor SDK
  transport  Windows.Devices.Bluetooth — NOTE: acting as a Classic HID *device* is the
             open feasibility question here, not a formality
  battery    Windows.System.Power / WMI

Linux        (not implemented)
  input      evdev (/dev/input/event*) or hidraw
  touch      libinput touch events, or evdev ABS_MT_TRACKING_ID as the stable identifier
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
