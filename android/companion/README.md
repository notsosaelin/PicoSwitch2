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
| **Gamepad** | Which controller drives the console, and using this handheld as one. |
| **Settings** | Appearance, Amiibo settings, adapter pairing, About. |

Two screens are pushed over a destination rather than being one: **Diagnostics** (from Settings ->
About, or the copy action in its own header) and **Amiibo settings** (from the Amiibo overflow or
Settings -> Amiibo). Both are troubleshooting or maintenance surfaces that are visited rarely and
should not hold permanent navigation space.

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

PicoSwitch2 -> AndroidHidTransport -> BridgeOutputCodec -> BridgeSession
            -> RumbleRequest -> AndroidOutputBackend -> vibrator
```

### Modules

| Module | Contents | Android? |
|---|---|---|
| `:management-core` | `dev.picoswitch.management` — logical commands/replies, domain models, portable workflows, and connected-session serialization | **No.** Plain Kotlin/JVM; it is the tested reference implementation, while `docs/management/PROTOCOL.md` and the shared fixture specify non-JVM interoperability. |
| `:bridge-core` | `dev.picoswitch.bridge.{core,protocol,session}` — normalized controller model, canonical motion convention, capabilities, report codec, session and transport interfaces | **No.** Plain Kotlin/JVM; the Android SDK is not on its classpath, so a leak is a build failure. |
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
    --es section Keyboard [--es overlay Diagnostics] [--ez empty true]
```

Window shapes come from `wm size`/`wm density` overrides on one AVD. Two properties of that
approach are worth recording because both produced wrong evidence before they were handled: the
resize is asynchronous, so a screenshot taken immediately captures the previous shape, and the
emulator restarts often enough under repeated resizes that a launch can be backgrounded. The
capture script polls the shape readback, checks `am start -W` for the activity that actually came
up, and finally verifies the pulled PNG's own dimensions before accepting it as evidence.

## Tests

A clean JVM run passed **145 `:app` tests**, **114 `:bridge-core` tests**, and **45
`:management-core` tests**, plus **2 instrumented
emulator tests** (every top-level destination rendering offline, and the Diagnostics overlay opening
and closing), Android lint with zero errors, and debug APK assembly. A connected AYN
Thor rerun of the UI test on 2026-08-13 did not expose a Compose hierarchy to the runner, so that
device rerun is not treated as new UI evidence. JVM coverage includes:

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
  Auto recognizes the audited AYN and Retroid built-in identities as Nintendo-style and otherwise
  falls back to Android's positional/Xbox convention; explicit selection covers unknown devices.
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
