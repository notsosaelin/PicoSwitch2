# Changelog

Release notes describe user-visible behavior. Detailed implementation history remains in
`docs/archive/` and the experiment records.

## Unreleased

### Added

- Hardware-confirmed DualSense and DualSense Edge IMU translation to the Switch 2 Pro Controller 2
  motion carrier. Splatoon 3 validates direction, scale, rapid movement, stationary behavior,
  reconnect recovery, and coexistence with controller input, audio, and native haptics.
- Passive UART analysis for genuine length-`0x28` motion PDUs, including exact G6/G7/G8
  signed-field decoding, capture summaries, and regression tests.
- Virtual Amiibo configuration infrastructure: strict 540/572-byte validation,
  transactional upload, full-image retrieval, dirty-write protection, and alternating power-safe
  flash snapshots.
- Browser-local amiibo library with recursive directory import, single-file import, IndexedDB
  caching, search, parsed tag identity, optional cached AmiiboAPI catalog details, and preservation
  of downloaded game-written state in the cached copy.
- Standalone browser-only Virtual Amiibo diagnostic portal with no serial dependency, including
  transactional upload/CRC checks, a separately persisted simulated adapter slot, controlled write
  injection, download/cache verification, AmiiboAPI testing, and an automated self-test.
- Artwork-first amiibo carousel in both portals. The production view starts empty, adds only
  exact-AmiiboAPI-matched owned files, fills progressively during directory scans, keeps the
  selection centered at 100%, and renders four non-overlapping neighbors on each side at exact
  80/60/40/20% sizes. Carousel names are omitted; navigation remains animated.
- Single-slot Virtual Amiibo Manager matching the board's one-amiibo storage: a carousel Load
  Amiibo action, Activate Amiibo with a disabled already-activated state, validated
  adapter-to-browser writeback, and one merged eject/clear button that labels its exact scope
  (loaded-plus-adapter eject, unload-only, or adapter-only eject of an image not loaded here).
  Adapter-destructive modes confirm first and remove the stored image (`amiibo clear`) while
  leaving the console Stop/write-back lifecycle unchanged.
- Amiibo identity/generation research (`docs/switch2/amiibo-identity-and-generation.md`,
  `docs/experiments/generated-amiibo-console-rejection-2026-07-26.md`): a 2026-07-26 hardware test
  established that the Switch 2 validates amiibo cryptography, so key-free generated images are
  rejected. A briefly implemented random-UID presentation mode was removed for the same reason.
- The Virtual Amiibo library is import-only (single file or recursive directory of the user's own
  genuine dumps). A key-based generator using a user-supplied `key_retail.bin` was prototyped and
  removed in favor of import-only simplicity; the identity/crypto research is retained under
  `docs/switch2/`.
- Library export/import is now a flat `.zip` (`library.json` manifest + one `.bin` per amiibo) via
  a self-contained store-only ZIP writer/reader; legacy `.json` backups still import.
- The AmiiboAPI catalog is enhancement-only: entries always display (on-tag identity when the
  catalog is unavailable), the catalog loads cache-first from two mirrors, and it never gates
  display or import.
- Import accepts larger emulator-container dumps (e.g. 2048-byte Pixl.js/allmiibo/flashiibo files
  for newer amiibo such as Kirby Air Riders) by taking the leading 540-byte NTAG215 image and
  recomputing the UID check bytes; newer amiibo not yet in AmiiboAPI import fine.
- Carousel loops at both ends with clean non-wrapping slides, shows the centered amiibo's release
  date above it, and has a Sort control (Default/Alphabetically/Numerically/Release date, plus
  Ascending/Descending) in the filter column. Added Download .bin, Delete from Library, and Refresh
  actions; the eject button is uniformly labeled "Eject Amiibo" and the active state reads "Amiibo
  Active".
- Stable localhost launcher for the production USB Serial/Bluetooth portal.
- Config-personality-only BLE management service and Web Bluetooth client. It pauses controller
  discovery before low-duty advertising, classifies its incoming Peripheral-role link before HID,
  sends production settings/Amiibo commands through a bounded cross-core bridge to the existing
  parser, and disconnects before normal discovery resumes.
- Host-tested Switch 2 NFC foundations: a partial-write-safe 630-byte vendor response pump,
  61-byte tag status, primary-capture-corrected 600-byte reader buffer/70-byte offset chunks, and
  atomic 454-byte staged-write validation.
- UART-gated genuine Pro Controller 2 NFC relay. A physical amiibo read completed through the
  dongle and was recognized by a real Switch 2; the trace confirms the native command sequence,
  `0 → 1 → 2 → 3` report-state progression, and nine offset reads.
- Virtual NFC read dispatch for the confirmed
  `0x03/0x04/0x05/0x06/0x15` flow, with no idle polling. A real Switch 2 recognizes an uploaded
  Virtual Amiibo through this path with a non-NFC source controller.
- Guarded transactional Virtual Amiibo write dispatch reconstructed from existing Switch 2 evidence:
  exact-UID `0x06` selection, a 64-byte write-preparation buffer, bounded 454-byte `0x14` staging,
  atomic `0x08` page updates, stale-generation rejection, dirty/download handoff, and
  modulo-eight NFC event handling. A real-console write reaches commit and `05 00` without a
  crash; logical removal, next-scan updated readback, and UART export are hardware-confirmed.
- Bounded console vendor-OUT command reassembly. An NFC `0x14` request is 88 bytes and spans the
  64-byte USB packet boundary; firmware now waits for the envelope's complete declared length
  before dispatch and safely handles arbitrary splits, coalesced commands, and oversize recovery.
- Post-write logical tag removal keeps the mutated RAM image selected for download and future use
  while reporting the physical-style removal edge needed to end the console's remove-the-tag wait;
  the next NFC scan presents that same updated image as a fresh tag encounter.
- Live UART Virtual Amiibo export reads one generation in bounded chunks, validates the exact
  540/572-byte image and NTAG UID/BCC on the PC, and acknowledges dirty state only after the binary
  has been written, so console-side tests no longer require moving the Pico USB connection.
- Incremental UART trace-dump persistence when `-OutputPath` is used, preserving every validated
  record already pulled if a crash/reboot resets the Pico before the dump finishes.
- A pure BOOTSEL action-policy module and host regression suite covering paired, unpaired, and
  Config-mode behavior independently of the gesture recognizer.

### Changed

- Virtual Amiibo is always available. Blank firmware presents no virtual tag, and the virtual
  runtime owns NFC only after the user loads one of their own images.
- Browser libraries use one AmiiboAPI-ordered mutable dump per exact catalog identity, expose two
  independent quick slots, ignore duplicate owned files, and cache the shared public catalog only
  once instead of duplicating matched metadata. The production carousel preserves
  that native order while its arrows cycle `All` and the imported library's available game-series,
  amiibo-series, and product-type values alphabetically.
- Every newly flashed UF2 performs a one-time reset of settings, both Virtual Amiibo journal banks,
  wake identity, and BTstack bonds. Ordinary reboots continue to preserve state.
- Per-controller-family button remapping and its portal controls were removed. The shared Pro2
  body/Sony lightbar color and independent Joy-Con accent controls remain. A locked,
  host-tested base map now feeds the stable emulated Nintendo identity so users can remap on the
  console and keep that mapping across source-controller changes.
- The production portal no longer displays the obsolete current-input/current-output identity
  cards. Its retained body/lightbar and Joy-Con accent controls share one compact full-width panel.
- The large setup card was removed. Live Amiibo status now appears in the header, the quick-slot
  switch is colocated with its actions, the three lower panels use equal widths and centered
  typography, the detail panel has separated formatting, and the log lives under Developer
  diagnostics.
- Bluetooth GAP and Config advertisement identity now use the single name `PicoSwitch2`.
- Active technical references now live under `docs/`; superseded plans and development narratives
  use the explicit `.archived.md` suffix.
- AmiiboAPI resolution now downloads one cacheable catalog and matches IDs locally instead of
  issuing one request per selected tag. This eliminates intermittent/throttled false “no entry”
  results and enriches library labels/search without disclosing selected IDs.
- The USB side of Config mode is now CDC-only. The firmware no longer exposes the read-only
  `PICOSWITCH` mass-storage drive or embeds a FAT image/web page; the production portal is served
  locally from `web/index.html`. This removes 100,104 bytes from Pico 2 W and 100,160 bytes from
  Pico W while preserving the config VID/PID and serial protocol.
- The failed synthetic length-`0x28` generator was removed from runtime firmware. Its first
  hardware test caused random motion because the unresolved leading/middle lanes are semantically
  active; the exact field codec, captures, and negative result remain documented.
- The active NFC model no longer treats USB as one 622-byte/630-byte read response. Direct
  Switch/UART/BLE evidence shows the console requests the same 600-byte reader buffer in bounded
  offset chunks.
- The first Virtual Amiibo write attempt crashed the console with error `2168-0002`. The cause was
  not the tag codec: the old 64-byte vendor read loop dispatched the first fragment of an 88-byte
  `0x14` request and misframed its remaining 24 bytes. The stream reassembler fixes that transport
  boundary. The repeated hardware write no longer crashes and confirms complete 88-byte commands,
  `0x08` commit, and accepted `05 00` status.
- The complete Virtual Amiibo lifecycle is now hardware-confirmed: write, `05 00`, logical removal
  as `07 41`, later fresh scan, same-session updated read, and generation-safe UART export. The
  exported 540-byte image is UID/BCC-valid and differs from its unique collection original across
  426 bytes confined to three permitted writable ranges.
- BOOTSEL now uses a faster, explicit action matrix. A single tap cycles only the four controller
  personalities when a controller is active; double-tap opens pairing; triple-tap wipes/disconnects;
  and a two-second hold enters Config directly. In Config, single/double taps do nothing,
  triple-tap remains an emergency wipe, and a two-second hold returns directly to Pro2.
- The portal action formerly labelled **Download current file**, **Save current Amiibo**, and
  **Sync to app** is now the unambiguous **Sync Amiibo from Adapter** action. It validates and
  overwrites the matching browser-local IndexedDB entry (or creates one) and acknowledges adapter
  dirty state only after that cache write succeeds.
- Console-written Virtual Amiibo data now queues a flash snapshot automatically. The runtime
  defers logical TagRemoved until the snapshot verifies, so a successful write is no longer only a
  RAM update.
- Virtual Amiibo's adapter journal still retains an internal baseline/latest-write recovery pair,
  but the browser exposes one mutable dump per identity. Users format/erase through the console;
  the portal does not expose reset-to-original behavior.
- The production amiibo library remains visible and usable without Web Serial. A versioned
  **Export saved library** JSON backup preserves every mutable dump and both quick-slot assignments
  and can be imported after browser storage is cleared.

### Validation

- DualSense gyro immediately returned to normal when the experimental length-`0x28` gate was
  disabled, confirming the validated production path remains the length-`0x1E` carrier.
- All 49 host-test executables pass, including the Config BLE bridge and locked base-map test, and the motion/PDU tests
  compile cleanly with warnings treated as errors against the reorganized source tree. Pico W,
  Pico 2 W, and legacy Switch 1 Pico W builds succeed.
- The local USB Serial/Bluetooth portal passes JavaScript syntax, DOM-reference, and localhost delivery
  checks. Both firmware binaries link without MSC callbacks or embedded-web symbols. Virtual NFC
  console read/write dispatch is feature-gated and hardware-validated through same-session
  lifecycle and UART export. Automatic write-before-eject persistence, dongle power-cycle recovery,
  adapter write recovery, offline library access, and full-library backup restore are
  hardware/browser-confirmed.
- The standalone diagnostic portal passes JavaScript/DOM reference checks and local HTTP delivery.
  AmiiboAPI's 946-entry catalog locally matches 944 of the 1,035 maintainer files; the remaining 91
  are Happy Home Designer item files that all share the same out-of-catalog ID.
- All 49 host-test executables pass after the Virtual Amiibo write, BOOTSEL-policy, and Config BLE
  bridge integration,
  including complete
  six-chunk commit, retry/conflict, incomplete/UID mismatch, format-promotion, 700 ms completion,
  atomic failure coverage, and retained-image/logical-removal separation. Pico W and Pico 2 W
  release builds both succeed; the complete write/eject/re-present/export lifecycle is also
  hardware-confirmed.

## 1.5.0 — 2026-07-22

### Added

- Native motion passthrough from a genuine Switch 2 Pro Controller to the Pro Controller 2 output
  personality, including automatic reconnect recovery and stationary source-off hold.
- Out-of-band UART protocol tracing and diagnostics for console USB traffic, BLE capture,
  firmware reads, bonded reconnect state, and native-motion ownership.
- Genuine current firmware identities for Pro Controller 2, NSO GameCube, and both Joy-Con 2
  personalities; the Switch 2 now reports each emulated personality as up to date.

### Changed

- The controller-side native-report setup now uses console-captured commands, verified GATT
  handles, and a 7.5 ms BLE interval in a named production profile. UART variants remain isolated
  diagnostics.

### Fixed

- Genuine Pro Controller 2 bonded HOME reconnect now restores the controller through BTstack's
  Security Manager instead of raw HCI encryption, preserving input and native gyro without SYNC.
- Player 1 LED state is reasserted after controller power cycles instead of remaining in the
  running/search pattern after a successful reconnect.
- Switch 2 custom pairing now serializes GATT transactions, preserves the controller's
  authoritative LTK and raw-HCI address order, and retains the durable bond across recoverable
  reconnect failures.

### Validation

- Splatoon 3 confirms correct native motion aim and stable reconnect behavior.
- Twenty consecutive controller-off/HOME cycles restored input, P1 LED, and gyro without SYNC.
- All 35 host-test executables pass; Pico W and Pico 2 W release builds compile successfully.
- An eight-hour 300 MHz Pico 2 W gameplay soak completed without an observed stability issue.

### Known limitations

- Translating motion from non-Nintendo controllers into genuine Pro Controller 2 native PDUs
  remains unresolved; the current native path is specific to a genuine Pro Controller 2 source.
- Joy-Con 2 bonded HOME reconnect has not received the same 20-cycle regression pass.
- NFC/amiibo passthrough remains research-only.

## 1.4.0 — 2026-07-18

### Added

- Full Pro Controller 2 UAC1 speaker/microphone USB function, replacing the descriptor-only
  audio stub.
- Live Switch 2/Windows audio output through a paired DualSense on Pico 2 W, using the
  hardware-confirmed 300 MHz floating-point/SRAM Opus path.
- Conditional Switch 2 headset presence from the DualSense physical jack, including stable
  removal and reinsertion without freezing controller input.
- DualSense native PCM haptics during console audio and headset-free rumble, with
  peak-preserving 3.25× rendering and a bounded two-packet STOP tail.
- Bluetooth battery passthrough across native HID telemetry, BLE Battery Service, and every
  console-facing USB personality.

### Changed

- Pico 2 W now uses the validated 300 MHz live-audio configuration by default. Pico W retains
  its previously validated non-audio clock and Bluetooth scheduling.
- DualSense and DualSense Edge rumble now use the more accurate native PCM renderer on Pico 2 W,
  whether or not a headset is connected.
- DualSense Edge Fn L/Fn R default to GL/GR.
- Bluetooth discovery idles after one controller connects, making one dongle to one controller
  the explicit supported scope and preserving radio bandwidth for audio.

### Fixed

- Audio and native rumble after a saved-bond DualSense reconnect or dongle power cycle; no fresh
  pair is required.
- DualSense headset unplug/replug input freezes and failure to restore audio/haptics.
- Chopped DualSense playback caused by incorrect stream timing, Opus scheduling, XIP stalls, and
  underspeed RP2350 execution.
- Native haptic intensity loss caused by sampling only the latest Nintendo rumble value instead
  of preserving each audio interval's left/right peaks.

### Known limitations

- Live DualSense audio is Pico 2 W-only; the Pico W experiment could not sustain playback.
- DualSense microphone return is not implemented.
- Extended 300 MHz thermal soak testing and the Pro Controller 2 update-prompt investigation
  remain open.
- Native console motion report `0x09` and NFC/amiibo remain blocked on stronger capture evidence.

## 1.3.0 — 2026-07-17

### Added

- Dedicated Retro Fighters BattlerGC Pro Bluetooth XInput profile with native NSO GameCube
  shoulders, click-gated analog triggers, separate Home-event handling, and Xbox-compatible rumble.
- First-generation 8BitDo Ultimate Bluetooth integration for independent P1/P2 paddles as GL/GR,
  plus guarded firmware patch, validation, recovery, and flash tooling.
- 8BitDo NGC Modkit rumble using its BlueRetro-derived `0xA5 / DB LL RR` output format.
- Host regressions for BOOTSEL gesture scheduling, late Bluetooth identity resolution, reconnect
  wake policy, Classic Xbox/Battler reports, and 8BitDo controller extensions.

### Fixed

- Joy-Con 2 Left Windows/Steam classification by restoring the missing Microsoft OS interface
  property.
- Late BLE Device Information VID/PID handoff without delaying initial input notifications.
- False console wake from controller reconnect/startup reports, including the genuine Switch 1
  Pro Controller initialization sequence.
- Classic-Xbox unsigned stick decoding and BattlerGC Pro pairing, shoulder, trigger, Home, and
  GameCube-mode L3/R3 behavior.
- BOOTSEL gesture progression under sustained report-driven scheduling.

### Known limitations

- The first-generation 8BitDo Ultimate custom paddle transport requires its guarded controller
  firmware patch; stock Bluetooth firmware does not expose P1/P2 independently.
- BattlerGC Pro screenshot and P1/P2 controls are not distinguishable in Bluetooth XInput reports.
- Native console motion report `0x09` and NFC/amiibo remain blocked on stronger capture evidence.

## 1.2.0 — 2026-07-16

### Added

- Hardware-confirmed individual Joy-Con 2 Left and Right personalities and sideways mappings.
- Configurable Pro Controller 2 body color and independent Joy-Con 2 Left/Right accent colors.
- Sony lightbar color matching and DualSense player-indicator forwarding.

### Fixed

- Preserved the confirmed Pro Controller 2, NSO GameCube, wake, rumble, pairing, and BOOTSEL
  behavior while adding the new personalities and appearance controls.

## 1.1.0 — 2026-07-15

### Added

- Native NSO GameCube Controller output personality with real-console input and rumble.
- Experimental Joy-Con 2 Left and Right output personalities.
- DualSense Edge paddles, Fn buttons, mute button, lightbar, and rumble support.
- Xbox rumble framing shared across the Xbox input paths.
- Explicit Bluetooth admission control for pairing windows and post-wipe forgotten devices.
- Host tests for GameCube rumble decoding, DualSense output, Xbox rumble, and existing reports.

### Fixed

- GameCube rumble ON/OFF/STOP decoding and unbounded full-strength output.
- DualSense and DualSense Edge input, LEDs, button mapping, and rumble regressions.
- BOOTSEL double-tap, triple-tap, and five-second hold starvation while a high-rate
  DualSense report stream is active.
- Switch 2 Pro Controller shoulder-button mapping.
- Pairing wipe now disconnects active controllers and prevents immediate automatic readmission.

### Known limitations

- Joy-Con 2 mappings need a complete real-console validation pass.
- Joy-Con 2 Left may require manual setup in Steam on Windows while Right is recognized.
- Native console motion output and wake-from-sleep remain unfinished.

## 1.0.0

- Initial Switch 2 Pro Controller release.
