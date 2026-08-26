# PicoSwitch Companion for Android

Second-pass status and the ordered physical handoff live in
[`FEATURE_PARITY.md`](FEATURE_PARITY.md) and [`HARDWARE_VALIDATION.md`](HARDWARE_VALIDATION.md).

Native Material companion app for PicoSwitch2. It is a second client of the firmware's shared
management interface; it is not a WebView and does not make the portal part of the runtime.

## Current capability

The debug build provides real implementations for:

- BLE discovery by the PicoSwitch2 management service UUID, connection state, disconnect, retry,
  timeouts, notification reassembly, and serialized command writes;
- adapter identity, firmware, connected-controller, battery, current personality, management-gate,
  color, LE bond, and Amiibo state reads;
- Pro Controller 2, GameCube, Joy-Con 2 L, and Joy-Con 2 R personality switching with an explicit
  USB re-enumeration warning;
- body/lightbar and Joy-Con accent colors, followed by authoritative readback, persistence
  completion when the firmware reports it, and automatic same-personality USB identity refresh;
- queued console wake requests;
- a private, versioned, recoverable on-device Amiibo library using app-internal files and atomic replacement;
- import and validation of exact 540-byte, 572-byte, and 2048-byte user backups;
- a foreground one-shot phone NFC backup action for ordinary NTAG215 tags: exact GET_VERSION,
  page READ/FAST_READ assembly, optional 32-byte READ_SIG, strict manufacturer/BCC checks, and
  atomic library persistence only after the complete image validates;
- richer local Amiibo identity details (character code/variant, tag type, model/series, format,
  and extended variant) without a network or catalog dependency;
- optional cache-first AmiiboAPI enrichment matched by the portal's uppercase head+tail figure ID:
  friendly name/character/game series/Amiibo series/type/release, compatible games/title-ID labels,
  and best-effort artwork. The bounded network enhancement never gates local imports or adapter
  operations and degrades to local identity when offline;
- optional portal-compatible Amiibo metadata decryption and local re-signing from the user’s own validated
  160-byte `key_retail.bin` (owner, nickname, registration/write dates, write count, and game-data
  identifiers), with keys stored only in app-private storage;
- explicit local Amiibo initialization: confirmation-gated wipe/re-sign of an imported ordinary or
  figure-v3 image with the user-owned key, HMAC self-verification, and atomic private replacement;
- portal-compatible v3 full-library ZIP export/import (flat `library.json` + `.bin` files), with
  bounded traversal-safe parsing and all-or-nothing local replacement;
- transactional `begin -> chunk -> commit -> persist` Amiibo uploads, 32 bytes per chunk;
- adapter-to-library Sync, structural validation, strict figure-v3 whole-image CRC verification,
  ordinary-image unavailable-CRC compatibility, `downloaded`, persistence polling, and
  immediate state/cache replacement after console-modified data is retrieved;
- present, eject, clean/used copy selection, and guarded adapter clear operations;
- Android built-in controller discovery and live input diagnostics;
- the public Android `BluetoothHidDevice` controller bridge using the exact 161-byte descriptor and
  26-byte input payload pinned by `tools/fixtures/android_controller_hid.h`;
- one persisted adapter relationship with a generation-owned association/bond/connect coordinator,
  first-use **Pair Adapter**, returning direct GATT reconnect, one clean retry for recoverable
  Android stack failures, an address-pinned service-scan fallback, explicit repair for missing
  Android bonds, and controller-mode reuse of the saved Classic bond,
  capacity-one full-state reports at an 8 ms ceiling, input-device hot-plug recovery, and
  neutralization on pause/stop/disconnect;
- a full-screen **Touch Gamepad** that turns the touchscreen itself into the controller carried by
  the same bridge: two analog sticks, an eight-way sliding D-pad, a positional face diamond, L/R,
  ZL/ZR, L3/R3, `-`, `+`, Home, Capture and C, all usable simultaneously through one deliberate
  multi-touch router keyed on stable contact identifiers, with responsive safe-area geometry,
  double-tap-to-hold on digital controls, optional local touch haptics, and an optional private
  background image;
- the complete Keyboard & Mouse management surface: live device and role status with names resolved
  from the adapter's own source registry, input mode, both mapping profiles with a focused
  per-input editor, mouse-button mapping, sensitivity with linked or independent axes, deadzone
  compensation, inversion, the advanced movement-window control, adapter-default restore, and an
  explicit live-apply/save model;
- a grouped Diagnostics screen with copy affordances on the values worth sharing and a one-tap
  summary, plus a privacy-redacted share export; and
- five-second controller and Amiibo state refresh while connected and idle, including an adapter-only download,
  present/eject, and guarded-clear workflow when no local item matches.

## Appearance

Settings -> Appearance stores the app's theme choice locally and applies it immediately. The four
choices are **System**, **Light**, **Dark**, and **OLED black** (true `#000000` background and
surface). System follows Android's current setting; the other choices are explicit and do not
depend on device orientation. A separate accent selector offers the default PicoSwitch palette,
an **inspired** blue/red Joy-Con 1 palette, and an **inspired** Joy-Con 2 palette. The latter uses
the repository's verified `#9BE1E6` left and `#FF8C5F` right accent references; inspired palettes
change only app UI accents and never write controller identity colors. Theme controls are full-row
radio choices with text labels, not color-only state, and keep Material error/status roles readable.

## Product structure

Five top-level destinations, chosen so each names something the user is trying to do rather than a
group of commands:

| Destination | Contents |
|---|---|
| **Adapter** | Which adapter, its state, what is attached, controller mode (personality), and appearance colours. |
| **Keyboard** | The complete Keyboard & Mouse management surface: devices, input mode, both mapping profiles, and mouse tuning. |
| **Amiibo** | The artwork-first library. Search, sort, and per-figure actions. |
| **Gamepad** | Which controller drives the console, using this handheld as one, and the Touch Gamepad. |
| **Settings** | Appearance, Amiibo settings, adapter pairing, About. |

Two screens are pushed over a destination rather than being one: **Diagnostics** (from Settings ->
About, or the copy action in its own header) and **Amiibo settings** (from the Amiibo overflow or
Settings -> Amiibo). Both are troubleshooting or maintenance surfaces that are visited rarely and
should not hold permanent navigation space.

**Touch Gamepad** is neither a destination nor an overlay but a full-screen application *mode*,
entered deliberately from Gamepad. Android Back — including a committed gesture from either screen
edge — opens its own menu during play and closes that menu while it is visible; leaving the mode is
the explicit Exit action inside the menu. It is not a sixth navigation item because it is not
somewhere you browse to; it owns edge-to-edge presentation, hides the navigation chrome, and prefers
landscape, none of which the scaffold's content column can give it. Nothing covers the app with
virtual controls merely because no physical gamepad is present.

Personality and colours moved onto Adapter because they are properties of the one physical device;
having them on a separate page made changing a colour a navigation task. Diagnostics absorbed the
former Settings -> Developer block, which had grown to dominate ordinary settings.

### Connection state

One authoritative connection state, rendered by the app shell as a strip above every destination.
No screen derives its own notion of connected, and controls are enabled from that single state, so
a session that ends while the user is on another page is visible where it happens. The strip also
carries the unsaved-changes marker, because that marker outlives the page that created it.

Removing a stored pairing may revoke this phone's own authorization, and Android does not expose
our Bluetooth address, so the app cannot tell whether an entry is itself. `AdapterRepository`
therefore probes the link after the removal and reports whether the session survived; the
ViewModel disconnects and reconciles when it did not, rather than leaving the UI connected to a
relationship that no longer exists.

### Adapter relationship and GATT lifecycle

The app keeps four truths separate: its one **Saved adapter**, app-owned Android companion
associations, Android's Bluetooth bond, and the adapter's own Bluetooth LE bond database. **Forget**
clears the first two (including stale app-owned associations) and deliberately retains both
Bluetooth bonds. **Disconnect** closes only the
management GATT session and retains the complete relationship; it never calls the independent
Controller Bridge or a physical-controller path.

`AdapterRelationshipCoordinator` is the single product-level authority. Every pairing/connect
attempt has a generation, duplicate API-33 CDM completion through both `onAssociationCreated` and
the Activity result is idempotent, a bond broadcast advances only the attempt waiting for that
address, and foreground/manual requests cannot overlap it. A relationship is persisted only after
the subscribed GATT session answers the management identity probe as PicoSwitch2.

Public API evidence: Android documents the dual association delivery in
[`CompanionDeviceManager.Callback`](https://developer.android.com/reference/android/companion/CompanionDeviceManager.Callback),
asynchronous bond completion in
[`BluetoothDevice.createBond`](https://developer.android.com/reference/android/bluetooth/BluetoothDevice#createBond()),
and app-owned association removal separately from Bluetooth bonding in
[`CompanionDeviceManager.disassociate`](https://developer.android.com/reference/android/companion/CompanionDeviceManager#disassociate(int)).

`BleGattManagementTransport` separately owns one Android `BluetoothGatt` generation. Retirement
requests `disconnect()`, waits up to 1.25 seconds for that same client's disconnected callback, and
then closes exactly once; late callbacks from older generations are diagnostic-only. Status 133,
the public connection-timeout status, and connection congestion receive at most one clean direct
retry. A failed direct sequence gets one service-filtered scan restricted to the saved address, so
another nearby PicoSwitch2 cannot silently replace the relationship. Repeated failures remain
actionable but do not erase trust or create a reconnect loop.

### Shared UI primitives

`ui/Components.kt` holds the recurring surfaces exactly once: `SectionCard`, `SettingsRow`,
`LabelValueRow`, `DeviceStatusRow`, `StatusChip`, `InlineNotice`, `EmptyStateBlock`,
`SegmentedSelector`, `ExpandableSection`, and the `PicoDialog`/`ConfirmDialog` pair that every
popup goes through. `LayoutTokens` is the single place a spacing, radius, breakpoint or row height
is decided; a literal dp inside a screen is treated as suspicious, because the previous per-screen
copies meant one spacing correction had to be repeated per screen and was missed on the screens
nobody re-opened.

Two of those primitives measure rather than assume:

- `SegmentedSelector` measures its longest label and falls back to a wrapping chip row when the
  segments cannot hold it. A fixed threshold was wrong in both directions -- it clipped
  "Keyboard + Mouse" into "Keyboard…" in a two-column landscape, and would have pushed four short
  labels into chips where they fit. Measuring is also correct at every font scale for free.
- The navigation bar compares the per-item width against the width one label needs at the current
  font scale, and drops to Material's icon-only bar below it. An ellipsised destination name is a
  navigation the user has to guess at.

### Keyboard & Mouse

The complete `kbm` command surface is wired into the client, so ordinary keyboard and mouse
configuration no longer requires UART. `KbmModels.kt` names the wire values and translates them
into product language; `ManagementProtocol` parses them strictly; `AdapterRepository` owns every
command, including the paginated `kbm map` assembly. No Composable builds a command string.

The page is four areas: devices, input mode, mapping, mouse tuning.

- **Devices.** `kbm status` reports only a transport connection index per admitted role. The peer
  names live in `input sources`, keyed by the same index, so `resolveKbmDeviceName` correlates the
  two and shows a plain "Connected" when they disagree -- rather than borrowing a name from an
  unrelated source. Keyboard-only and mouse-only are ordinary operation and get a quiet chip, not a
  warning banner.
- **Input mode.** The chosen override and the mode actually in force are shown as two facts, because
  under **Automatic** the adapter infers the live mode from what is admitted and the two legitimately
  differ.
- **Mapping.** Both profiles are editable and are presented as the independent layouts they are;
  which one the adapter is running is marked separately from which one is on screen. A row opens a
  focused editor with a searchable, grouped destination list that scrolls to the current binding.
  Unassign (`none`) and Restore default (`default`) are distinct actions because they are distinct
  firmware commands.
- **Mouse tuning.** Sensitivity uses a logarithmic slider: the adapter accepts a 512:1 range, so a
  linear control cannot resolve the useful low end at all. Axes are linked by default with an
  explicit unlink. The radial anti-deadzone is presented as **Deadzone compensation** in percent,
  with zero clearly meaning off. `recenterMs` is surfaced under Advanced as **Movement window**,
  named for what it does now: the firmware's own header records that it is the velocity model's
  reference interval and no longer a recentring delay.

Every accepted range comes from the adapter's own `kbm mouse` reply. The client carries no copy of
a limit, because a client that did would refuse values a newer firmware had widened.

### Live apply versus save

KB/M changes apply to adapter RAM immediately; `save` is what writes flash. The UI models exactly
that: a change takes effect at once, the page becomes locally unsaved, and **Save** persists it.
Slider drags are debounced during the gesture and sent unconditionally on release, so tuning stays
live without flooding a link that carries one command at a time. The adapter's reply is adopted as
the new truth rather than the value that was asked for, because it validates and rejects rather
than clamping.

The protocol cannot say whether an arbitrary runtime value matches flash, so a fresh connection
starts clean by definition rather than by inference, and the unsaved marker is cleared with the
rest of the session state on disconnect. A rejected save leaves the marker set.

### Touch Gamepad

A phone or tablet with no gamepad attached is still a complete controller, because its screen is
one. Gamepad -> **Touch Gamepad** opens a full-screen controller whose input travels the existing
bridge; the adapter knows it is the declared Android bridge source but cannot distinguish touch
input from the handheld's built-in controls.

The engineering is split so that the interesting half is portable. `:bridge-core`'s
`dev.picoswitch.bridge.touch` owns personality profiles, immutable templates, sparse overrides,
composition, validation, editor operations, contact ownership, stick/D-pad geometry and release
rules. The app supplies rendering, pointer events, editor widgets, an interaction-safe rectangle,
app-private persistence and haptics. Its current directory is historical because Android is the
first consumer; `:bridge-core` is still a plain JVM shared module and must be relocated rather than
duplicated when a second host appears. See
[`docs/bridge/PLATFORM_BACKEND.md` §3.7](../../docs/bridge/PLATFORM_BACKEND.md).

The confirmed management personality selects exactly one profile: Pro Controller 2, NSO GameCube,
sideways Joy-Con 2 Left, or sideways Joy-Con 2 Right. `Config` and `Unknown` remain neutral. Each
profile declares every visible output and its fixed bridge binding; GameCube cannot acquire L3/R3,
and Joy-Con face/direction labels are personality-fixed rather than rewritten by the Pro2 face
presentation setting. A live personality change releases once, quarantines already-down contacts,
atomically installs the new profile, and keeps the Classic controller link alive.

Related controls use one normalized group anchor plus logical-unit offsets, rather than unrelated
normalized points. A Pro2 or Joy-Con Right face diamond therefore stays square when the window aspect
ratio changes, and group scaling changes both button size and cluster spacing while individual scaling
leaves the selected button centred. The GameCube face group preserves its native large-A,
south-west-B, east-X and north-west-Y relationship; its X/Y silhouettes have separate touch-friendly
bounds. Pro2 and GameCube place the main stick at upper-left and the real D-pad at lower-left; each
D-pad's outer well, cross and touch region scale together to the main stick's complete footprint.
GameCube reuses the Pro2 major-control composition directly: main stick at the Pro2 left-stick
anchor, D-pad at the Pro2 D-pad anchor, C-stick at the Pro2 right-stick anchor, and the complete
A/B/X/Y compound bounds centred over the Pro2 face-diamond region. Its approved asymmetric internal
face geometry remains intact. The GameCube touch profile exposes no Select/Minus control; its single
Start/Plus control is centred on the viewport midline. The Capture/Home/C utility trio is symmetric
around that same
midline, and the equal-size D-pad/C-stick pair is mirrored around it at a slightly tighter spacing.
Joy-Con Left and
Right place the primary stick on the left and their independent direction or
face cluster on the right. Left orders `SL L | ZL SR`; Right orders `SL R | ZR SR`; stick clicks are
labelled L3/R3. Joy-Con directions are recessed triangle indicators, not a text-arrow D-pad.

**A sideways Joy-Con is a whole controller turned a quarter turn, and its action cluster turns with
it.** This was wrong until 2026-08-26: the four controls were placed by reading their logical names
as screen positions, so `direction-up` was drawn at the top of the display. It is the shell's *up*
button, which points at the player's **left** once the half is held sideways — so the player was
pressing the button in the X position to get Y, and the layout was misleading in a way nothing
crashed over. `TouchClusterRotation` now states the turn once (Left anticlockwise, Right clockwise,
matching the firmware's own `joycon2_pack_sideways_stick`, whose halves rotate opposite ways), and
`squareDiamond` places a cluster keyed by each control's slot **on the shell**. Direction markings
turn with the shell because an arrow's meaning *is* its orientation; the Joy-Con Right letters stay
upright because a letter's meaning is the letter, and four sideways letters would just be
unreadable. The correction is purely visual — every binding still matches the firmware's sideways
encoder exactly, and `TouchSidewaysJoyConTest` asserts physical identity, logical action and screen
position separately, because conflating any two of them is how the defect got in. No control id
changed, so stored profiles still load and keep every position their owner chose; the two templates
declare a newer revision to record that the shipped defaults moved. Pro2
retains one real cross D-pad: its drawing stays separate from touch routing, and diagonal
pressed arms are composited once so their translucent overlap cannot darken the hub.

#### Layout editing

Layout customization never mutates the shipped template. The pipeline is one direction only:

```text
controller personality -> immutable template -> selected profile's sparse overrides -> resolved layout
```

**Editing is a mode, not a settings page.** Layout editing is a spatial task, and a panel large
enough to hold the controls is a panel that hides the layout being judged. Edit mode keeps the whole
controller drawn and turns it into editable objects: input forwarding pauses, every control gets a
visible hit-bound outline, and the only chrome is a small floating toolbar plus a contextual bar for
whatever is selected. The toolbar docks to the bottom, top, left or right — a preference, because
the right edge depends on which hand is holding the device and where the system's own gesture areas
are — and it folds itself into two lines rather than pushing Save off a short window. While a
control is actually being dragged or pinched, the chrome fades to almost nothing so the control
under it stays visible. The contextual bar itself takes no touches, so a control beneath it can
still be picked.

**Direct manipulation.** Tap selects, drag moves, pinch resizes, long-press adds a second control to
the selection. One gesture loop handles all four rather than a stack of competing detectors, and it
deliberately drops the centroid jump when a second finger lifts instead of forwarding it as
movement. The **Group** toggle expands a selection to its authored cluster — face diamond, direction
cluster, utility trio, secondary controls — and what is highlighted is exactly what an edit moves.

**Alignment assists, never restricts.** An optional grid is drawn inside the interaction rectangle
and anchored to its centre, so a symmetric layout stays symmetric. With snapping on, a moved
selection is pulled onto nearby guides — the region centre lines, another control's centre, the
innermost safe edge, a grid line — but any movement larger than the tolerance always wins, so no
position becomes unreachable. The correction is computed from one reference control and applied to
the whole movement, so a multi-control selection keeps its internal spacing exactly. Guides are
drawn only while something is genuinely aligned. All of it is pure logic in
`TouchEditorAlignment`, operating on resolved pixel geometry, and is therefore tested at every
window shape without a device.

**Adding a control** means restoring one that was hidden. Templates are immutable and every output
has a fixed personality binding, so there is nothing else it could mean: a control outside the
shipped template would have no binding, and the audit refuses that.

**Profiles are per personality.** GameCube's profiles are not Pro Controller 2's. Each personality
has an immutable **Default** plus up to twelve user profiles; the default is synthesized from the
shipped template on every read and is never written to storage, which makes "cannot be overwritten,
cannot be deleted, always available" a property of the type rather than a rule somebody has to
remember. Saving an edit while Default is selected does not fail and does not overwrite it — it
names and creates a profile of your own, because the alternatives are discarding the user's work or
destroying the one layout that is always supposed to be recoverable. Profiles can be created,
duplicated (including from Default), renamed, reset and deleted; deleting the active one falls back
to Default. Leaving edit mode or switching profiles with unsaved changes asks first.

`TouchProfileMetadata` carries a reserved `gameKey`, unused by this build, so the per-game profiles
the architecture anticipates do not require migrating every stored document later.
`TouchProfileLibraryJsonCodec` also encodes a single profile as a standalone document — the
export/import foundation; the transport is a separate decision.

Android persists one versioned JSON library per personality through `AndroidTouchProfileStore`.
The single anonymous override the first release stored is adopted once, on upgrade, as an ordinary
named profile rather than discarded. Unreadable or future documents are kept raw while the runtime
falls back to the immutable default. The shared Kotlin reference serializers are deliberately named
`TouchLayoutOverrideJsonCodec` and `TouchProfileLibraryJsonCodec` — the JSON schemas are portable,
the kotlinx.serialization implementation is not universal.

The editor refuses to open on a window below the layout engine's minimum interaction rectangle,
because no edit can make a window bigger; that is a different failure from an audit finding, and
`ResolvedTouchLayout.regionTooSmall` is what tells the two apart.

**Contacts are owned by identifier, never by position.** Android guarantees a stable `PointerId`
for a contact's lifetime and explicitly does *not* guarantee its index; a router keyed on the index
works perfectly with two fingers and swaps which control a thumb is holding the moment a third
arrives or the first lifts. One deliberate root-level router (`awaitPointerEventScope`) does the
hit-testing rather than a forest of gesture detectors, which would otherwise compete for gesture
ownership, wait for touch slop before moving a stick, and have no notion of a five-finger chord.

**A claim is made once, on Down.** After that a contact belongs to its control until it ends. A
stick keeps its contact when the thumb leaves the visual circle and clamps at full deflection,
because the alternative is a wide turn ending as a face-button press. A second contact landing on
an owned control is ignored rather than stealing it — two contradictory positions for one stick
have no correct answer.

**Double-tap, hold, and slide to keep a button held.** Some games want one button held while another
is tapped repeatedly, and a touchscreen has no tactile control a finger can rest on — planting a
thumb on `Y` costs the grip and the reach that the rest of the layout needs. Double-tapping a digital
control, holding the second press and then **sliding away** locks it held instead, so the thumb can
leave. The published state is

```text
effectivePressed = (touchPressed || latchedPressed) && !retriggering
```

which is why the transport, the firmware and the console are entirely unaware of the feature: a
latched button is a button that stays pressed in successive reports.

**Timing alone cannot create a hold, because timing alone collides with real play.** Two gestures
were tried and shipped before this one, and both were wrong in the same way. A plain double tap
collides with mashing, which *is* a stream of double taps — gameplay immediately latched whichever
button a game wanted hammered. A double tap whose second press is merely *held* collides with the
very ordinary "double tap, then keep holding" that a game may ask for directly, and no dwell,
however long, separates those two, because they are the same input.

So the dwell no longer commits anything: it only **arms** the gesture. While armed, nothing has
changed — the control is an ordinary physically held button, and letting go simply ends the press.
A deliberate **slide** away from where that press began is what locks it. Nothing a game asks a
player to do involves pressing a button and dragging off it, which is exactly why that motion is
safe to spend on this. Telling users to disable the feature per button before they know which
buttons a game hammers was never a solution; making the gesture impossible to perform by accident
is.

**Creating a hold is deliberately harder than removing one.** A hold the user did not mean is a
stuck button they have to work out how to clear; a hold they lose by accident is one gesture away
from coming back. So the two are not symmetric:

```text
UNLATCHED   tap                                  ordinary press
            tap, press, hold 2x the base         armed; STILL an ordinary press
            tap, press, hold, slide away         latch

LATCHED     quick tap                            retrigger: release edge, then held again
            press held 1x the base               unlatch
```

Releasing needs no leading tap, no slide, and half the dwell. Both dwells come from one
`holdThresholdNanos` (180 ms) with `latchEngageThresholdNanos` and `latchReleaseThresholdNanos`
derived from it by a named rule, so the asymmetry is a stated policy rather than two constants that
can drift apart, and retuning the base moves both together. The base is well short of the platform's
500 ms long-press timeout on purpose — this has to stay usable mid-fight, not feel like a context
menu. The same argument that protects an unlatched button from mashing protects a latched one:
no mashed press stays down long enough to reach even the shorter dwell.

The commit distance is `latchCommitDistanceUnits` (64 logical units, roughly a centimetre on a
handset) — a threshold of its own rather than a multiple of the drift tolerance, because the two
answer different questions: slop is "did the user stay still", this is "did the user perform a
deliberate motion". Direction is irrelevant; any slide will do. The moment the contact crosses it
the latch commits, so the instant the button locks is the instant the user feels it, and sliding
back afterwards cannot undo it — the gesture is over. Releasing while armed but before crossing it
is not a latch and never was.

Arming is announced rather than silent: the lightest haptic tick the surface has, and the badge
appears as an **open** padlock that closes when the slide commits. One drawing with one difference,
so the two states read as the same idea rather than as two marks to learn. Both fire during the
perfectly ordinary act of holding a button after double-tapping it, which is the point — they are
what makes an otherwise invisible gesture discoverable at the only moment it matters.

The recognizer **observes** presses that the engine has already applied rather than intercepting
them. That distinction is the feature's whole latency story: a detector that waited to see what the
user meant would make every first tap late, which on a gameplay surface is unusable. Instead every
press and release is published immediately and the gesture is recognized alongside them. The latch
toggles while the second press is still down, so the control is already pressed at the instant the
state changes and no transition can produce a blip — in particular, unlatching under a finger keeps
the button down until that finger lifts.

The double-tap window, its minimum gap, and the long-press timeout that bounds the *first* tap all
come from the platform (`ViewConfiguration`), not from invented constants, so the gesture matches
every other double tap on the device including whatever the user's accessibility settings have done
to it. The dwells have no platform equivalent and are the project's own, kept in `TouchLatchConfig`
with the rest so they stay tunable from one place; they are deliberately not user-facing settings.
Recognition is scoped per control — tapping `A` then `B` quickly is two first taps — and one attempt
consumes one sequence, so a candidate released too early does not leave a half-armed recognizer. A
platform that reports no contact timestamps gets no gesture rather than one that fires at random.

Both distances are displacement from the contact's own origin, in logical units, and neither has
anything to do with the control's bounds. Before the dwell, travelling past `gestureSlopUnits`
abandons the candidate, because a drag is not the hold the gesture asked for — that is what stops a
thumb that presses a button and sweeps away from arming anything. After arming, travelling past
`latchCommitDistanceUnits` commits. Natural drift reaches neither. A bounds test for either would
make the gesture easy on a large button, hard on a small one, and unreliable anywhere near an edge;
logical units rather than pixels keep both a distance on the glass, so laying the controller out
smaller does not change how the gesture feels.

**Tapping a held button presses it again.** A latched control is already published as pressed, so an
ordinary tap on it would produce no edge at all and the game would see nothing — "latch `Y` to keep
running, then tap `Y` to swing" was impossible without unlatching first. A tap on a latched control
therefore *retriggers* it: the hold is masked for `retriggerReleaseNanos`, producing a real release
edge, and then reasserts itself.

```text
HELD  ->  tap  ->  release edge  ->  press edge  ->  HELD
```

**The pulse is decided when the press ENDS, not when it starts**, because a press on a latched
control is ambiguous until then: quick means "press it again", held means "stop holding it". Pulsing
on the way down would make every unlatch emit a pointless release/press first. Deferring costs
nothing — the game sees the button as held throughout the decision window, which is exactly what the
hold is for — and it never touches the unlatched path, so an ordinary gameplay press is still
published the instant it happens.

Unlatching is equally careful at the other end: when the release dwell elapses the latch clears, but
the finger that performed the gesture is still down and stays authoritative. The button releases when
that finger lifts and not a moment earlier, so clearing the hold produces no edge of its own.

The mask is applied at publish time rather than by mutating the accumulators, so ownership, latch
state and the press/release bookkeeping are all untouched, nothing has to be undone when it expires,
and no boundary has to know it existed. The duration matters and is not arbitrary: the session
coalesces state onto a 125 Hz report cadence through a *conflated* mailbox, so a release and a press
emitted in the same instant collapse into no change at all. 48 ms survives both that and the
consumer — five report intervals, and longer than one frame at 30 Hz — while being far too short for
a held run button to visibly stutter. The badge does not blink and the latch haptics do not fire: a
retrigger is an ordinary press of a button the user is deliberately still holding, and gets exactly
the press and release haptics any other button contact gets — a held control that felt dead to the
touch would be the clearest possible way to say "this control is broken now". Repeated taps produce
repeated fresh presses and cannot unlatch, because unlatching needs the deliberate dwell.

**Two things need a clock, and the engine does not own one.** The dwell and the retrigger mask are
timed rather than event-driven, and a still finger produces no pointer events. The engine states
when it next has work (`nextDeadlineNanos`) and the surface sleeps until then and calls `onTick`. A
pull model rather than a scheduler, deliberately: there is no queued closure carrying a captured
control, so a tick that lands after a release, a layout change or a teardown finds nothing to do.
That *is* the session-safety for all three — there is no queue to invalidate. Taking the gesture
away from a control does the same thing directly: disabling its latch clears the hold, the pending
dwell and any pulse together, so a button cannot be left down with nothing left that could release
it. `TouchLatchOutputTest` pins every one of these as published controller-state edges rather than
as internal flags, because "the hold is briefly masked" is worth nothing if a released snapshot
never reaches the state machine.

Only digital controls latch. Sticks and the D-pad are excluded structurally, in
`TouchControlKind.supportsLatch`, rather than by configuration: their value *is* the contact's
position, and a held direction the user cannot see themselves holding is the most disruptive thing
on the layout. The global **Lock a button held** setting is the default for controls that state no
preference; the editor's per-control **Default / Enabled / Disabled** — the padlock beside the
visibility toggle — overrides it either way, which is what a button somebody mashes needs. That
choice rides the existing sparse-override schema as one optional `latch` flag, so layouts written
before the feature load unchanged and the schema version deliberately did not move: a bump would
make older builds refuse the whole document and discard geometry, to protect a preference that
degrades to its default anyway.

A latched control is unmistakable. It keeps the pressed fill and gains a padlock badge on its
diagonal, drawn in the theme's guaranteed-contrast label colour — shape first, because colour alone
survives neither a user-chosen background nor colour-blind vision — and the badge is placed
identically for round, rectangular and contoured silhouettes. The badge is read from the latch and
never from what is currently published, so a retrigger pulse cannot blink it off. Engaging and
releasing feel different through the existing haptic policy, once per transition and never per
frame. Latch transitions are the only thing logged: `controller/touch latch` names the control, the
resulting state and the reason. Retrigger pulses are counted rather than logged — they are ordinary
presses and would be log volume — and the touch diagnostics line carries `latched=`, `latch=`
engaged/released/cleared and `retrigger=` beside the contact counters, because "held by a finger",
"held by a latch" and "held but pulsing" look identical in `held=` and are different faults.

The recognizer is deliberately not Boolean-shaped: it knows about contacts and time only, and the
dwelling contact's movement is already routed to it. The planned analog-trigger hold, where that
movement will select a held trigger value, can reuse this gesture rather than growing a parallel
one.

**Face controls are positions, not letters.** `FaceButtonPosition` names South/East/West/North and
resolves through the same `ControllerLayoutResolver` the physical path uses. The companion sends the
result as logical A/B/X/Y usages; descriptor-proven bridge provenance makes the firmware seam map
those four directly instead of reapplying the B/A/Y/X map for directly paired physical controllers.
The presentation is persisted separately (there is no physical descriptor to key the per-device
store) and deliberately never resolves to `Auto`, which exists to guess a *printed* legend that a
drawn control does not have. The default is Nintendo, because the diamond being drawn is a Switch
controller's. Both presentations and every fixed GameCube/Joy-Con face cluster share one exhaustive
golden fixture. A catalog test requires exact equality with its `(personality, template,
presentation, control id)` keys, labels and Android HID usages. The C host golden then sends all 20
cases through the production descriptor parser, source-aware seam and selected final personality
encoder while also proving the direct-controller base map and raw Joy-Con bitmap remain unchanged.

**Exactly one host control set is the controller.** `InputAuthority` is explicit: entering the mode
neutralizes through the still-live link, takes authority, and rebinds the session to the host itself
so console rumble reaches the phone's own actuator; leaving reverses all of it and restores the
previous physical selection. Physical events are declined while touch is authoritative, and touch
events are declined while it is not. Home, Capture and C remain available from either, because they
are host actions rather than a second controller. There is no merge, at any point.

**Every boundary releases.** Contact end, gesture cancellation, a contact the platform stops
reporting, menu open, mode exit, `ON_PAUSE`/`ON_STOP`, geometry invalidation, authority change, link
down, link stop and disposal all call one idempotent release. Ordering is load bearing: the engine
is released *before* the session neutralizes, so the neutral report still crosses a live transport.
Configuration is persisted; nothing that describes what is currently held ever is.

Latched holds ride that same single release, which is the only reason the feature is safe: a
latch is the one thing on this surface that outlives the contact that made it, so it can never be
given a lifetime of its own. It is additionally dropped when the latch configuration itself changes,
because a setting that reads "off" above a button that is still down is exactly the confusion the
setting exists to end. A latch therefore lives only for the lifetime of the active touch session —
never across a restart, a reconnect, an authority change or a personality change, and never onto a
control map where the same id means something else.

**Geometry is declarative and audited.** The layout is normalized against the interaction-safe
rectangle (system gesture insets, cutouts, caption bar) while the background draws edge to edge, so
no control lives under a back-gesture strip. Controls stop growing past an ergonomic ceiling and the
extra room becomes gutter — a twelve-inch tablet does not come with larger thumbs. `TouchLayoutAudit`
mechanically rejects overlapping hit regions, targets under 44 units, controls outside the
rectangle and duplicate ids, and it runs on real resolved geometry at every representative window
shape rather than on the authored numbers. A window that genuinely cannot hold the controller says
so after neutralizing, rather than drawing overlapping targets that would send the console input the
user did not choose.

The custom background is copied into app-private storage rather than referenced. A picker grant can
lapse and a picture can be deleted; a downsampled private copy is a few hundred kilobytes and simply
keeps working, with no storage permission to request and nothing to revoke. Nothing leaves the
device.

### Controller remapping

There is still no controller remapping editor, and this is a firmware capability gap rather than a
product decision. `NS2_BASE_BUTTON_MAP` in `include/ns2_remap.h` is a compile-time `const uint8_t`
table with no runtime override storage and no management command; `src/config.c` exposes nothing
for it. Adding the page needs a `remap` command family analogous to `kbm bind` -- list, bind,
reset -- plus persisted overrides and a wireless allowlist entry. Nothing client-side can substitute
for that, and inventing an unsupported command would be worse than the gap.

## Architecture

Two independent paths. Both management and Controller Bridge have a platform-neutral core plus an
Android backend; their contracts, transports, and state machines remain separate.

```text
Compose adaptive screens
  -> CompanionViewModel / StateFlow
    -> AdapterRepository
      -> ManagementClient                    (management-core, neutral)
        -> ManagementChannel                 (management-core, neutral)
          -> BleGattManagementTransport      (app, Android)
            -> PicoSwitch2 GATT newline-JSON service

Activity KeyEvent / MotionEvent
  -> AndroidInputBackend                        (app,  Android)
    -> ControllerInputState                     (core, neutral)
      -> BridgeSession                          (core, neutral)
        -> ControllerReportEncoder              (core, neutral)
          -> AndroidHidTransport                (app,  Android)
            -> BluetoothHidDevice -> PicoSwitch2 Classic HID host

Compose pointer events                          (Touch Gamepad, same destination)
  -> Modifier.touchGamepadContacts               (app,  Android)
    -> TouchContactTracker -> TouchControlEngine (core, neutral)
      -> TouchContribution
        -> ControllerInputState ...              (the identical path from here down)

PicoSwitch2 -> AndroidHidTransport -> BridgeOutputCodec -> BridgeSession
            -> RumbleRequest -> AndroidOutputBackend -> vibrator
```

### Modules

| Module | Contents | Android? |
|---|---|---|
| `:management-core` | `dev.picoswitch.management` — logical commands/replies, domain models, portable workflows, and connected-session serialization | **No.** Plain Kotlin/JVM; it is the tested reference implementation, while `docs/management/PROTOCOL.md` and the shared fixture specify non-JVM interoperability. |
| `:bridge-core` | `dev.picoswitch.bridge.{core,protocol,session,touch}` — normalized controller model, canonical motion convention, capabilities, report codec, session and transport interfaces, and the portable on-screen controller (contact ownership, stick/D-pad geometry, declarative layout) | **No.** Plain Kotlin/JVM; the Android SDK is not on its classpath, so a leak is a build failure. |
| `:app` | Android BLE discovery/pairing/GATT, Controller Bridge Android backends, UI, local Amiibo library, and NFC | Yes |

`AndroidBridge` is the assembly point: it plugs the four Android backends into the shared
`BridgeSession` and is the only class that knows both sides. The contract a second platform would
implement is documented in [`docs/bridge/PLATFORM_BACKEND.md`](../../docs/bridge/PLATFORM_BACKEND.md)
and [`docs/bridge/PROTOCOL.md`](../../docs/bridge/PROTOCOL.md).

`:management-core` owns command construction, framing limits, JSON parsing, typed errors, paging,
external-state refresh, mutation/readback, and adapter Amiibo workflows. `AdapterRepository` adapts
those typed results to app state. The Android BLE backend owns discovery, GATT subscription,
fragmentation/reassembly, and session cleanup; it serializes transactions because the firmware
bridge has one command slot and one reply slot. See
[`docs/management/README.md`](../../docs/management/README.md).

The controller bridge is an independent Classic Bluetooth connection. It never sends controller
input over the management GATT service and does not impersonate Nintendo, Xbox, or Sony hardware.

## Build

Prerequisites:

- JDK 21
- Android SDK Platform 36 and Build Tools 35+
- network access for the first Gradle dependency resolution

From this directory:

```powershell
$env:JAVA_HOME = 'path\to\jdk-21'
.\gradlew.bat testDebugUnitTest assembleDebug
```

The APK is written to `app\build\outputs\apk\debug\app-debug.apk`.

Install to a connected device:

```powershell
adb install -r app\build\outputs\apk\debug\app-debug.apk
```

The app requires Android 9/API 28 or newer. Android 12+ presents the standard Nearby Devices
permission. Pairing uses Android-owned chooser and bond-confirmation UI; no root, Shizuku,
accessibility service, or hidden API is used. Ordinary pairing/reconnect stays in the app. Android
Settings is requested only when Repair pairing finds a still-present platform bond that public APIs
cannot remove.

## Firmware setup and safety

Normal source builds boot with normal-personality wireless management available. New management
bonds are admitted only while the adapter's physical double-tap pairing window is open; subsequent
connections reuse the stored LE bond. RX and notification-subscription writes require an active
16-byte encrypted link and a durable bond. Android no-display Just Works cannot provide MITM
authentication, so the app describes this accurately as bonded/encrypted. `mgmt off` disables the
service for the current boot; an ordinary reboot restores it.

Before pairing Android as a controller, open the adapter's physical controller-pairing window. The
Android system must obtain user consent and create the bond before `BluetoothHidDevice.connect()` can
succeed. The app orchestrates that during **Pair Adapter** and does not expose a second controller-
host chooser. Internally the Companion Device association, Classic bond, management GATT session,
HID Device registration, and HID connection remain separate Android states.

## Responsive strategy and validation

The app selects navigation and content structure from available space, never from orientation or
device names:

- bottom navigation below 720 dp, navigation rail at 720 dp and above;
- the bottom bar drops to Material's icon-only form when the per-item width is below what one label
  needs at the current font scale;
- two columns of section cards from 760 dp of **content** width -- measured after the rail and the
  page gutters, not from the window's size bucket. Those two disagree exactly on a landscape
  handheld, and reading the bucket left a 960 dp display running one column of very wide rows;
- cards that branch internally measure themselves, because in a two-column layout a card only gets
  half the page. The appearance tiles fall back to rows below 320 dp of card width;
- adaptive Amiibo grid cells with a 132 dp minimum, so the column count follows the display instead
  of stretching a fixed number of columns;
- a tighter vertical rhythm and a smaller page title under 560 dp of height, which is where
  landscape handhelds and raised font scales both land;
- a 1240 dp content maximum on unusually wide displays;
- one shared spacing/radius/touch-target token set; scrolling rather than shrinking or clipping.

Dialogs are bounded at 520 dp and their scrolling lists at 280 dp, so a popup stays a focused
decision on a wide display and cannot push its buttons off a short one.

### The layout lab

Most of these screens only exist in their interesting form while an adapter with a keyboard, a
mouse and an Amiibo library is attached, so inspecting only the disconnected empty states would
have inspected the half of each screen with no layout in it. `app/src/debug` therefore carries a
**layout lab** activity that renders the real application shell against synthetic adapter state.
It is debug-variant only and reachable through:

```powershell
adb shell am start -n dev.picoswitch.companion.debug/dev.picoswitch.companion.lab.LayoutLabActivity `
    --es section Keyboard [--es overlay Diagnostics] [--ez empty true] [--ez touch true] `
    [--es personality pro2|gc|jcl|jcr]
```

`--ez touch true` opens the real Touch Gamepad mode, and `--es personality` selects any shipped
profile, which is how its geometry is inspected at an arbitrary window size with no adapter paired.
It enters the mode the product way rather than
rendering a mock, because a mock is what would get inspected otherwise.

Window shapes come from `wm size`/`wm density` overrides on one AVD. Two properties of that
approach are worth recording because both produced wrong evidence before they were handled: the
resize is asynchronous, so a screenshot taken immediately captures the previous shape, and the
emulator restarts often enough under repeated resizes that a launch can be backgrounded. The
capture script polls the shape readback, checks `am start -W` for the activity that actually came
up, and finally verifies the pulled PNG's own dimensions before accepting it as evidence.

## Tests

A clean 2026-08-23 JVM run passed **226 `:app` tests** in each of the debug and release variants,
**217 `:bridge-core` tests**, and **45 `:management-core` tests**. Android lint reported no errors
in either variant, and both debug and release APKs assembled. The last instrumented suite passed
**8 tests** (every top-level destination rendering offline, the Diagnostics overlay opening and
closing, and six Touch Gamepad pointer-adapter cases); it was not rerun for this source-only pass.
A connected AYN Thor rerun of the UI test on 2026-08-13 did not expose a
Compose hierarchy to the runner, so that device rerun is not treated as new UI evidence. JVM
coverage includes:

- the Keyboard/Mouse wire contract against replies shaped exactly like the firmware's own
  `snprintf` templates: complete status, a refused unknown mode, a status missing a role field,
  mapping pages, a refused unknown destination, mouse configuration with its reported limits, a
  refused reply that omits them, and the longest `kbm bind` command against the 127-byte frame;
- the KB/M client model: source wire round trips in both widths, refusal of the usage ids the
  firmware will not bind, every destination/mode/profile name the firmware emits being
  representable, every default binding resolving to a named key, and the neutral fallback for an
  unnamed usage;
- the logarithmic sensitivity mapping: round trip across the range, the default landing near the
  middle of the travel rather than at 6% of it, clamping to adapter-reported bounds, and a
  degenerate min == max range that would otherwise divide by zero;
- KB/M repository behaviour through a scripted transport: paginated mapping assembly, refusal of a
  page for the wrong profile, of a total that changes mid-pagination, and of a non-terminating
  cursor; `none` versus `default` staying distinct commands; a mouse change adopting the adapter's
  reply rather than the requested value; a mode change re-reading the effective mode; the unsaved
  marker surviving a failed save; and a disconnect dropping one session's mapping and marker
  entirely;

- the Touch Gamepad's portable half: the single conversion into bridge units asserted against the
  physical path's own conversion at every endpoint, circular clamping proving a diagonal has
  magnitude one rather than root two, the radial deadzone rescaling to full magnitude, all eight
  D-pad sectors with both the radial and the angular hysteresis boundaries, a full-circle slide that
  never produces opposites, and non-finite input resolving to rest rather than to a garbage axis
  byte;
- contact ownership: a contact claiming exactly one control, an unowned contact that cannot seize
  one by wandering onto it, a second contact that cannot steal an owned stick, a stick keeping its
  contact across the whole area, a seven-contact chord, batches delivered in a different order each
  time with non-contiguous identifiers, a contact the platform stops reporting being cancelled, and
  release-all being idempotent and unable to be resumed by a contact held across it;
- input authority: touch state reaching the wire, physical events discarded while touch is
  authoritative and the reverse, both transitions neutralizing, software Home/Capture/C surviving
  either, every Nintendo/Xbox face position producing its drawn logical HID usage, and a touch
  trigger encoding as both the digital bit and the analog byte; paired production-parser,
  provenance, and seam-resolver coverage proves bridge face usages bypass only the second
  physical-layout swap;
- the default layout resolved at seven representative window shapes and four display densities,
  audited for overlapping hit regions, undersized targets, controls outside the safe rectangle and
  duplicate ids, plus the refusal of a window too small to hold a controller;
- Touch Gamepad settings decoding: out-of-range and non-finite stored values becoming usable
  defaults, and a structural assertion that the persisted model carries no gameplay state;
- command framing/limits, config, personality, complete Amiibo status, malformed/error replies;
- accepted key_retail.bin length/master labels, reversed-master normalization, richer identity,
  packed-date edges, HMAC-invalid metadata suppression, private key-store replacement behavior, and the
  portal-generated valid decrypt golden vector;
- cache-first AmiiboAPI parsing, portal-style figure-ID matching, release/name/game/title-ID
  enrichment, cache restart, and offline unmatched-ID behavior;
- exact neutral report, all 14 button bits, every hat direction/opposites, Thor-style GAS/BRAKE
  trigger normalization, stick dead zones/endpoints/inversion, and descriptor size;
- Amiibo accepted sizes, identity extraction, and standard CRC32;
- portal-compatible ZIP manifest/round-trip, traversal and size limits, transactional library
  preservation, and initialize/re-sign wipe/self-verification against the shared golden vector;
- upload ordering, 32-byte chunk count, and dirty-store replacement protection through a scripted
  fake management transport;
- versioned local-library restart/recovery/corruption/collision/rollback behavior;
- generation-safe download acknowledgement, unsupported/malformed/false-success failures, and
  exact 511/512-byte reply-limit handling; and
- strict Android-independent NTAG215 protocol tests with fake transceivers covering command order,
  540/572-byte assembly, signature fallback, malformed-response aborts, NTAG213/216 and figure-v3
  rejection, raw manufacturer/BCC rejection, and absence of write/auth/NDEF/sector commands; and
- capacity-one HID report replacement plus descriptor/report golden vectors.

The emulator run proves install, launch, navigation, rotation/configuration handling, and responsive
rendering. It does not emulate Bluetooth HID Device or a real PicoSwitch2 radio.

## Honest limitations

- A real BLE management session on AYN Thor now validates discovery, adapter/controller display,
  personality switching, Amiibo visibility, and the ordinary-image Sync transfer path. Wake and
  the complete mutation matrix still require their focused hardware checks.
- The AYN Thor's built-in controller is live-validated from the app input panel through public HID
  Device, PicoSwitch2, and a real game without root or Shizuku. Its OEM stack can return `false`
  immediately from registration or connection and then succeed asynchronously, so callbacks—not
  those immediate booleans—are authoritative. The revised one-relationship pairing/reconnect UI,
  Nintendo face-label correction, and restart/power-cycle matrix still need focused hardware
  confirmation.
- The v2 Android HID contract adds motion, battery, console rumble, and player LED feedback while
  preserving every v1 input offset. The manifest declares Android's normal `VIBRATE` permission
  (no runtime prompt); physical Thor validation of those v2 extensions remains open.
- Phone-NFC physical-tag backup is host-tested but still awaits the physical gate on an NFC-capable
  Android device. It is deliberately ordinary NTAG215 only: figure-v3/NTAG I2C 2K is rejected,
  the reader is armed only from Amiibo -> Scan, disabled on pause/one-shot completion, and no
  controller-as-reader or adapter commands are involved.
- Raw backup share/export and Mii rendering remain future client-side work. Initialization and ZIP
  exchange are local-only and still await focused physical Amiibo/adapter validation. User-supplied
  retail keys are accepted only in the exact 160-byte portal format, stored in an app-private file,
  and never sent to firmware, diagnostics, or a library export.
- Android controller source and Auto/Nintendo/Xbox face layout are persisted by input descriptor.
  Auto recognizes the audited AYN `0x2020/0x0111` and Retroid built-in identities as legend-reporting
  and otherwise falls back to Android's positional/Xbox convention; explicit selection covers unknown
  devices. AYN's button-layout toggle changes the device IDENTITY, so the same handheld in Xbox mode
  (`0x0112`, "Xbox Wireless Controller") is correctly treated as positional — the two modes send
  different key codes for the same physical button and must not resolve alike.
  The setting drives two SEPARATE mappers — the built-in pad's keys are corrected against the
  legend its plastic prints, while an on-screen slot simply sends the letter it draws — and under a
  given layout those are opposites. Merging them inverts one origin; see `docs/bridge/PROTOCOL.md`
  §3.2.
  Capture remains an OEM-specific C/Z choice until a labeled Thor/Retroid input pass is recorded.
- The Input page reads the firmware's bounded source registry and lets a bonded/encrypted management
  client choose one Active controller. A handoff neutralizes the console output and waits for a fresh
  complete report from the selected source; it never merges two controllers.
- A color commit mutates, reads back, saves, waits for identified persistence completion when
  supported, and automatically queues same-personality USB re-enumeration. Only a partial failure
  leaves **Retry** visible with the truthful message that color was saved but identity refresh is
  pending. This path is source/JVM-tested and still needs the physical recovery checklist in
  `HARDWARE_VALIDATION.md`.
- App appearance preferences are local to this Android install. Joy-Con-inspired palettes are UI
  references only; they do not imply or alter the adapter's body/lightbar/Joy-Con identity values.
- LE bond lists are bounded by the wireless reply bridge's 511-byte payload ceiling. Firmware now
  returns a backward-compatible v2 envelope for a complete `bonds list`, a compact
  `response_too_large` error instead of a partial array, and cursor pages through
  `bonds list v2 [cursor]`. Android verifies totals and cursor progress, and hides legacy
  unversioned results whose completeness cannot be proven. Classic controller bonds are not
  individually removable through management.

## Second-pass hardening

- Sync stores adapter bytes durably before `amiibo downloaded`, rechecks generation/CRC, and polls
  queued persistence. Failed local storage leaves firmware dirty protection intact.
- Optional feature probes distinguish unavailable firmware commands from transient communication
  failure without discarding valid core state.
- Rotation/process restoration retains destination, Amiibo/source selection, color edits, and a
  partial color-apply retry without replaying protocol mutations. An association result can recover
  its active ViewModel generation after Activity recreation.
- **Diagnostics** (Settings -> About -> Diagnostics) groups identity, management link, controller
  bridge, keyboard/mouse arbitration, adapter state, Android platform, and live input by the layer
  each describes, so a cross-layer failure's first question -- which side disagrees -- is answered by
  reading down one column. Identifiers render monospaced with copy affordances, and a header action
  copies a short summary. Its Android share export remains bounded and redacted: no raw Amiibo
  bytes, JSON replies, keys, or Bluetooth addresses.
- The wireless-management gate is a development control and lives in Diagnostics behind a
  confirmation that states the cost, because disabling it ends the session issuing the command. It
  is not an ordinary product setting: management is normal product behaviour and boots enabled.

## Source/document reconciliation

- `docs/bluetooth/app-interface-audit.md` retains the original gap table as rationale; its current
  status section records personality, bounded bonds, and wake as implemented. The `figureId` gap
  is likewise implemented by the current `src/config.c`/Amiibo status surface.
- Active transport documentation now reflects the bonded/encrypted normal-personality service and
  keeps Config as the CDC route; dated experiment reports retain their original observations.
- The broad wireless `amiibo ` allowlist technically admits low-level reader commands, but the Web
  Portal documentation correctly says the physical-reader scan flow is not production-safe. This
  app does not expose it.
- Portal Sync acknowledges queued persistence without waiting for `persistPending=false`; this app
  polls to verified completion.
- Portal requests have no IDs, so a response arriving after timeout may be attributed to the next
  command if the link stays open. The Android transport closes the GATT session on timeout before
  allowing another command, preventing that stale-response attribution.

## Amiibo page parity handoff

The Android Amiibo page now follows the portal’s high-value identity/details surface while keeping
adapter operations independent of optional crypto and network services. The raw image parser is
shared by import, local-library recovery, and the details panel. A cache-first AmiiboAPI store
matches IDs the same way as the portal (uppercase `head + tail`), persists compact metadata for
seven days, and refreshes through bounded mirrors; artwork is best-effort and has an offline icon
fallback. `AmiiboCrypto` is a direct port of
the tested amiitool-compatible block in `web/index.html`: it validates the two 80-byte masters and
160-byte total length, derives keys with HMAC-SHA256 + AES-CTR, verifies both HMACs, and only then
reads the settings fields documented in `docs/switch2/amiibo-decrypted-data-surface.md`.

The key file lives at app-private `filesDir/amiibo-private/.amiibo-retail-key.bin`; the Amiibo
overflow menu is the only key import/replace entry point, so the default library surface stays
focused on artwork and identity. Android backup
is disabled, and neither raw key bytes nor decrypted values enter diagnostics, management commands,
the local library export, or firmware. The page exposes explicit confirmation-gated initialization
for an imported local copy; uninstall removes the app-private key file, while local dumps and the
adapter remain unaffected during normal use. An
HMAC failure is visibly reported and never renders owner/nickname junk.

This slice implements local portal-compatible initialization/re-signing and ZIP exchange plus a
strict ordinary-NTAG215 phone NFC backup path; figure-v3 phone scanning remains deliberately
rejected. Raw backup sharing and Mii rendering remain future work. No network request is required
for import, selection, initialization, ZIP exchange, phone NFC backup, upload, Sync,
present/eject, or clean/used copy operations. See the dated evidence note
[`../../docs/experiments/android-amiibo-page-parity-2026-08-13.md`](../../docs/experiments/android-amiibo-page-parity-2026-08-13.md).
