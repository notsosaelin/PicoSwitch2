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
- body/lightbar and Joy-Con accent colors, followed by queued firmware persistence;
- queued console wake requests;
- a private, versioned, recoverable on-device Amiibo library using app-internal files and atomic replacement;
- import and validation of exact 540-byte, 572-byte, and 2048-byte user backups;
- transactional `begin -> chunk -> commit -> persist` Amiibo uploads, 32 bytes per chunk;
- adapter-to-library Sync, structural validation, strict figure-v3 whole-image CRC verification,
  ordinary-image unavailable-CRC compatibility, `downloaded`, persistence polling, and
  immediate state/cache replacement after console-modified data is retrieved;
- present, eject, clean/used copy selection, and guarded adapter clear operations;
- Android built-in controller discovery and live input diagnostics;
- the public Android `BluetoothHidDevice` controller bridge using the exact 81-byte descriptor and
  nine-byte payload pinned by `tools/fixtures/android_controller_hid.h`;
- in-app Android companion-device chooser, bonding handoff, saved bonded-host reconnect,
  capacity-one full-state reports at an 8 ms ceiling, input-device hot-plug recovery, and
  neutralization on pause/stop/disconnect;
- a separate Developer/diagnostics screen and privacy-redacted share export; and
- five-second controller and Amiibo state refresh while connected and idle, including an adapter-only download,
  present/eject, and guarded-clear workflow when no local item matches.

There is deliberately no user remapping editor. PicoSwitch2's compiled controller map is stable and
user remapping belongs in the Switch's persistent controller settings.

## Architecture

```text
Compose adaptive screens
  -> CompanionViewModel / StateFlow
    -> AdapterRepository
      -> ManagementTransport
        -> BleGattManagementTransport
          -> PicoSwitch2 GATT newline-JSON service

Activity KeyEvent / MotionEvent
  -> AndroidInputRouter -> immutable ControllerState
    -> ControllerReportEncoder
      -> BluetoothHidDevice -> PicoSwitch2 Classic HID host
```

`ManagementProtocol` owns UUIDs, framing limits, JSON parsing, and typed adapter errors. Raw command
strings never appear in Compose screens. `AdapterRepository` owns workflows and external-state
refresh. The BLE transport accepts one transaction at a time because the firmware bridge has one
command slot and one reply slot.

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
permission. The Android controller bridge also uses Android-owned chooser and bond-confirmation UI;
no root, Shizuku, accessibility service, hidden API, or visit to Bluetooth Settings is required.

## Firmware setup and safety

Normal source builds keep normal-personality wireless management off and RAM-only. Initially enter
the physical Config personality and issue `mgmt on`; a diagnostic image built with `-MgmtOn` skips
that step for the current boot. Repository source does **not** yet ship production always-on
management. Current firmware also does not enforce authenticated management writes, so use either
form only in a trusted environment.

Before pairing Android as a controller, open the adapter's physical controller-pairing window. The
Android system must obtain user consent and create the bond before `BluetoothHidDevice.connect()` can
succeed.

## Responsive strategy and validation

The app selects navigation and content structure from available width, not orientation names:

- bottom navigation below 720 dp, navigation rail at 720 dp and above;
- two-pane hardware, controller, and color layouts at 760 dp; Amiibo uses two panes from 600 dp so
  short handheld landscapes retain a usable library and detail surface;
- adaptive Amiibo grid cells with a 168 dp minimum;
- a 1240 dp content maximum on unusually wide displays;
- one shared spacing/radius/touch-target token set; scrolling rather than shrinking or clipping.

The debug APK was emulator-launched and visually inspected at 16:9 portrait/landscape, 16:10
landscape, 4:3 portrait/landscape, 1:1, 900x2100 narrow/tall, and 2400x1200 wide-handheld/tablet
profiles. The 16:9 landscape pass exposed underused horizontal space; lowering the content
two-pane threshold from 900 to 760 dp corrected it. A retained scroll-position issue across top-level
destinations was also fixed by keying each destination's composition. The second pass additionally
removed a duplicate/clipped empty-library card, bounded the compact Amiibo detail pane so its grid
remains reachable at 150% text, and exercised an 80-item library with long names.

## Tests

The current clean run passes **42 JVM tests**, **1 API-35 instrumented navigation/scroll smoke
test**, Android lint (**0 errors; 15 advisory warnings**), and debug APK assembly. JVM coverage includes:

- command framing/limits, config, personality, complete Amiibo status, malformed/error replies;
- exact neutral report, all 14 button bits, every hat direction/opposites, Thor-style GAS/BRAKE
  trigger normalization, stick dead zones/endpoints/inversion, and descriptor size;
- Amiibo accepted sizes, identity extraction, and standard CRC32;
- upload ordering, 32-byte chunk count, and dirty-store replacement protection through a scripted
  fake management transport;
- versioned local-library restart/recovery/corruption/collision/rollback behavior;
- generation-safe download acknowledgement, unsupported/malformed/false-success failures, and
  exact 511/512-byte reply-limit handling; and
- capacity-one HID report replacement plus descriptor/report golden vectors.

The emulator run proves install, launch, navigation, rotation/configuration handling, and responsive
rendering. It does not emulate Bluetooth HID Device or a real PicoSwitch2 radio.

## Honest limitations

- A real BLE management session on AYN Thor now validates discovery, adapter/controller display,
  personality switching, Amiibo visibility, and the ordinary-image Sync transfer path. Wake and
  the complete mutation matrix still require their focused hardware checks.
- The AYN Thor's `Odin Controller` is live-validated, including its input panel, and the ordinary
  app reaches registered HID Ready without root or Shizuku. Its OEM stack can report an immediate
  registration rejection and then asynchronously succeed, so the callback—not that immediate
  boolean—is authoritative. App-led bond, Pico receipt, and end-to-end console input remain
  unvalidated.
- Motion and rumble are not in the v1 Android HID contract and are labeled as unavailable.
- Phone-NFC physical-tag backup is not implemented yet. Controller-as-reader commands are low-level
  and intentionally not exposed as a production user workflow.
- Owner, nickname, registration/write dates, write count, encrypted-data initialization, catalog
  artwork, ZIP library exchange, and raw backup share/export remain future client-side work. Those
  need user-supplied retail keys and/or catalog logic; firmware correctly does not receive the keys.
- Android controller source selection is persisted by descriptor. Capture remains an OEM-specific
  C/Z choice until a labeled Thor/Retroid input pass is recorded.
- Color changes save correctly but current firmware has no color-triggered USB re-enumeration
  request. The app warns that a reconnect/re-enumeration is needed before the console refreshes the
  host-visible identity color.
- LE bond lists approach the wireless reply bridge's 511-byte practical response ceiling. Firmware
  can return valid but silently incomplete JSON because it provides no total/truncation marker; the
  app warns that reported results are not provably complete. Firmware still needs a versioned
  bounded/paginated list command. Classic controller bonds are not individually removable through
  management.

## Second-pass hardening

- Sync stores adapter bytes durably before `amiibo downloaded`, rechecks generation/CRC, and polls
  queued persistence. Failed local storage leaves firmware dirty protection intact.
- Optional feature probes distinguish unavailable firmware commands from transient communication
  failure without discarding valid core state.
- Rotation/process restoration retains destination, Amiibo/source selection, color edits, and
  pending identity-refresh state without replaying protocol mutations.
- **More -> Developer / diagnostics** shows platform, HID, GATT, firmware, capability, report, and
  re-enumeration state. Its Android share export is bounded and redacted: no raw Amiibo bytes, JSON
  replies, keys, or Bluetooth addresses.

## Source/document discrepancies found

- `docs/bluetooth/app-interface-audit.md` still calls personality switch, bonds, wake, and
  `figureId` gaps; current `src/config.c` implements all four.
- `web/README.md` and parts of `docs/architecture/config-transports.md` retain Config-only BLE text;
  current firmware also arms the service in normal personality when `mgmt on` is active.
- The broad wireless `amiibo ` allowlist technically admits low-level reader commands, but the Web
  Portal documentation correctly says the physical-reader scan flow is not production-safe. This
  app does not expose it.
- Portal Sync acknowledges queued persistence without waiting for `persistPending=false`; this app
  polls to verified completion.
- Portal requests have no IDs, so a response arriving after timeout may be attributed to the next
  command if the link stays open. The Android transport closes the GATT session on timeout before
  allowing another command, preventing that stale-response attribution.
