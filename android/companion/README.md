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
- body/lightbar and Joy-Con accent colors, followed by queued firmware persistence and an explicit
  same-personality USB identity refresh;
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
- the public Android `BluetoothHidDevice` controller bridge using the exact 81-byte descriptor and
  nine-byte payload pinned by `tools/fixtures/android_controller_hid.h`;
- one persisted adapter relationship with first-use **Pair Adapter**, returning direct GATT
  reconnect plus bounded scan fallback, and controller-mode reuse of the saved Classic bond,
  capacity-one full-state reports at an 8 ms ceiling, input-device hot-plug recovery, and
  neutralization on pause/stop/disconnect;
- collapsed Settings categories, including Developer diagnostics and a privacy-redacted share export; and
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
succeed. The app orchestrates that during **Pair Adapter** and does not expose a second controller-
host chooser. Internally the Companion Device association, Classic bond, management GATT session,
HID Device registration, and HID connection remain separate Android states.

## Responsive strategy and validation

The app selects navigation and content structure from available width, not orientation names:

- bottom navigation below 720 dp, navigation rail at 720 dp and above;
- two-pane hardware, controller, and color layouts at 760 dp; Amiibo uses two panes from 600 dp so
  short handheld landscapes retain a usable library and detail surface;
- adaptive Amiibo grid cells with a 168 dp minimum; compact Amiibo uses a bounded artwork/name hero
  and scrollable figure rows so 538 dp-wide landscape still exposes a usable primary action. A
  compact sort menu deterministically orders both surfaces by Name, Series, or Recently added;
- a 1240 dp content maximum on unusually wide displays;
- one shared spacing/radius/touch-target token set; scrolling rather than shrinking or clipping.

Theme and palette controls live in the same scrollable Settings surface, so they remain reachable in
short landscape windows and at larger font scales. Status and navigation bars follow the selected
scheme, including light-system-bar treatment in light mode.

The adapter connection row is deliberately Home-only. Home presents three focused tiles for the
adapter personality, active input, and loaded Amiibo; protocol warnings and raw identifiers stay in
Settings -> Developer. Settings starts as a compact category list and expands only the category the
user asks for. Amiibo key selection lives under Settings -> Amiibo metadata and has no delete or
library-page replacement control.

The debug APK was emulator-launched and visually inspected at 16:9 portrait/landscape, 16:10
landscape, 4:3 portrait/landscape, 1:1, 900x2100 narrow/tall, and 2400x1200 wide-handheld/tablet
profiles. The 16:9 landscape pass exposed underused horizontal space; lowering the content
two-pane threshold from 900 to 760 dp corrected it. A retained scroll-position issue across top-level
destinations was also fixed by keying each destination's composition. The second pass additionally
removed a duplicate/clipped empty-library card, bounded the compact Amiibo detail pane so its grid
remains reachable at 150% text, and exercised an 80-item library with long names. The current page
also treats an active adapter tag as a first-class display item when the private library index is
empty, so its catalog lookup and actions do not depend on importing or syncing first.

## Tests

The Android JVM run passed **85 tests**, **1 API-35 instrumented navigation/scroll smoke test**,
Android lint, and debug APK assembly. A connected AYN
Thor rerun of the UI test on 2026-08-13 did not expose a Compose hierarchy to the runner, so that
device rerun is not treated as new UI evidence. JVM coverage includes:

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
- Motion and rumble are not in the v1 Android HID contract and are labeled as unavailable.
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
- Color changes save first and remain pending until the user chooses **Apply identity changes**.
  Firmware then queues the existing same-personality USB re-enumeration path; the console-side
  controller pauses briefly. This path is host/build-tested but still needs the physical recovery
  checklist in `HARDWARE_VALIDATION.md`.
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
- Rotation/process restoration retains destination, Amiibo/source selection, color edits, and
  pending identity-refresh state without replaying protocol mutations.
- **Settings -> Developer** shows platform, HID, GATT, firmware, capability, report, and
  re-enumeration state. Its Android share export is bounded and redacted: no raw Amiibo bytes, JSON
  replies, keys, or Bluetooth addresses.

## Source/document discrepancies found

- `docs/bluetooth/app-interface-audit.md` retains the original gap table as rationale; its current
  status section records personality, bounded bonds, and wake as implemented. The `figureId` gap
  is likewise implemented by the current `src/config.c`/Amiibo status surface.
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
