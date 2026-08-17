# Changelog

Release notes describe user-visible behavior. Detailed implementation history remains in
`docs/archive/` and the experiment records.

## Unreleased

### Added

- **Bluetooth keyboard and keyboard + mouse input.** A new **Input source** setting selects
  Controller (the default and unchanged behavior), Keyboard, or Keyboard + Mouse. A Bluetooth
  keyboard can drive the console by itself, and a keyboard plus a mouse act as one controller — one
  logical input source, as before, just backed by two peers. A single device exposing both
  keyboard and pointer fills both roles over one connection.
- **Remappable keyboard and mouse profiles.** Each mode has its own independently configurable
  profile over documented defaults (WASD/IJKL for the keyboard, WASD plus mouse look for keyboard +
  mouse). Bindings can be changed, unassigned, or restored per profile; changes persist across
  reboot and never affect unrelated settings. Mouse movement uses the Joy-Con 2 pointer where the
  selected personality has one and translates to the right stick otherwise, with adjustable
  sensitivity, inversion, and recenter time.
- Portal card for selecting the input mode and seeing keyboard/mouse connection state, plus `kbm`
  commands on management and UART for the full mapping surface.

Hardware-validated 2026-08-16 with a Bluetooth keyboard and mouse: both connect together as one
controller, either can be switched off and back on and rejoins on its own without re-pairing, and
the one still connected keeps working throughout. See
[`docs/bluetooth/keyboard-mouse-input.md`](docs/bluetooth/keyboard-mouse-input.md).

### Changed

- Persistent settings schema 10 → 11. Existing controller colours and the learned wake identity are
  migrated in place; nothing a user had configured is reset by the upgrade.
- A pairing window now stays open until the **selected** input source is complete, so pairing a
  keyboard in Keyboard + Mouse mode no longer closes the window the mouse still needs. Controller
  and Keyboard modes behave exactly as before.

### Fixed

- **A bonded Bluetooth peer failed to reconnect after being powered off and on again when a second
  peer stayed connected.** The adapter tracked only one reconnect identity — whichever peer
  connected most recently — so when a keyboard and mouse were both paired, powering one off made
  the adapter chase the one that was still connected. Each of those attempts also stopped the scan,
  so the device that had actually gone away was never looked for, and only re-pairing brought it
  back. The adapter now selects from all of its saved pairings and never targets a device that is
  already connected. Single-controller reconnect behaviour is unchanged.
- **With a keyboard and mouse both paired and switched on, only the first one to connect worked.**
  The adapter stopped looking for devices as soon as anything connected — a rule from when it only
  ever served one controller — so the second device was never found, and the only way to get it on
  was to power the first one off and then power-cycle the one that was waiting. The adapter now
  keeps looking while the selected input source is still missing a device, and stops once the source
  is complete. A single controller still ends discovery exactly as before.
- A stale-pairing cleanup could delete the saved pairing of the wrong device when two were
  connected, removing one that was still working.

## 2.0.0 — 2026-08-15

A major-generation release. v1.5.0 was a firmware-and-controller milestone whose only
configuration surface was a `CONFIG.HTM` page served from an embedded USB mass-storage drive and
opened in desktop Chrome or Edge; there was no Android application and no wireless management of
any kind.

2.0.0 replaces that whole model. It adds a native Android companion, a host-controller bridge that
lets an Android handheld drive the console, bonded/encrypted in-band Bluetooth management, a
complete Virtual Amiibo workflow, and translated motion from controller families beyond a genuine
Pro Controller 2. The embedded web disk is gone; the local portal is served over CDC instead.

The bridge contract is version 3 and is unchanged by this version bump — it is a wire-compatibility
identifier, not a product version.

### Added

- **Native Android companion application** (`android/companion/`). Adapter management over
  bonded/encrypted BLE, adapter mode and identity-colour configuration, an Amiibo library with
  import/export/initialization and phone-NFC capture, active-controller selection, live input
  diagnostics, and a shareable diagnostic export. No root, Shizuku, accessibility service, or
  manual Bluetooth Settings visit is required at any point.
- **Android controller bridge.** The handheld's own controls drive the console through the
  adapter: buttons, both sticks, analog triggers, D-pad, motion, rumble and battery reporting.
  Hardware-confirmed on an AYN Thor and an Odin 2.
- **C / GameChat button.** The Switch 2 C button is now reachable, offered as an on-screen control
  alongside Home and Capture — neither audited handheld has a physical key for any of the three.
  The wire contract carries fifteen buttons within the existing report bytes, so no field moved.
- **Handheld battery reporting.** The console shows the handheld's real battery level and charging
  state instead of nothing.
- **Bridge contract versioning with runtime compatibility reporting.** The adapter and the
  companion each declare the bridge contract they implement, and the companion compares them on
  connect. When they disagree the app says so plainly and names the fix, rather than silently
  losing battery, motion and rumble while ordinary input keeps working. Firmware that predates
  contract reporting is reported as unverified, never as compatible.
- **Firmware build identity.** The adapter reports the git short hash of the build actually flashed
  on it (with a `+dirty` marker), over both management `info` and UART. "What firmware is on this
  adapter?" no longer has to be inferred from file timestamps.
- **Descriptor fingerprint guard.** A SHA-256 over the complete 161-byte bridge HID descriptor is
  registered per contract version. Any byte change in either the C or the Kotlin definition fails
  the build until the contract version is deliberately bumped and the new digest registered —
  including coordinated edits to both languages, which byte-for-byte parity alone cannot catch.
- **Platform-neutral bridge core.** The companion's controller path is split into a shared core
  (normalized controller model, canonical motion convention, capabilities, protocol codecs, session
  and cadence) and an Android backend (input, sensors, battery, haptics, HID transport). The core
  compiles without the Android SDK on its classpath, so platform types cannot leak into the shared
  model. This ships the Android implementation; it does not ship any other platform.
- **Bridge diagnostics.** Ordered boundary counters across the whole path, a firmware-side
  identification trace with the failure reason and first mismatching descriptor byte, `bridge` /
  `bridge clear` UART commands, and a startup wiring audit. All surfaced through the app's
  diagnostic export.

- Android reconnect now treats adapter identity validation as part of the saved-address attempt,
  falls back to service-filtered discovery when that address resolves to the wrong device, retries
  once per foreground session, and clears all adapter-derived UI state on disconnect. Terminal GATT
  command failures remain visible instead of racing an asynchronous disconnect back to Idle, and
  every Amiibo mutation is disabled while disconnected or another operation is active.
- Android now presents the firmware source registry as a clean **Active controller** selector, so a
  user can hand console input between a physical controller and the paired handheld without
  re-pairing. The bonded/encrypted wireless management path now allowlists `input active
  <id|none>`; every handoff reuses the existing neutral-then-fresh-report arbiter boundary and never
  merges sources.
- The offline Switch 2 command atlas now accepts both console-side `trace` and controller-side
  `blecap` captures, fails closed on absent/non-zero loss metadata, and retains capture-boundary,
  transport, GATT-handle, completeness, payload-hash, and source-hash provenance. A durable corpus
  report records the three controller-side initialization pairs and ranks the next bounded capture
  gaps; the audit found no need for another generic protocol runner.
- Declared Android's normal `VIBRATE` permission for the companion's implemented console-rumble
  feedback path, closing the four `MissingPermission` lint errors without adding a runtime prompt.
- In-band BLE management is now bonded/encrypted and production-default-on. RX and notification-
  subscription writes require ATT encryption with a 16-byte key, callbacks also require a durable
  LE bond, and new management Just-Works bonds are admitted only during the existing double-tap
  pairing window. Existing controller-central pairing is unchanged. Android no-display Just Works
  does not provide MITM authentication, so the implementation does not claim
  `ATT_SECURITY_AUTHENTICATED`. `mgmt off` remains a current-boot escape hatch; reboot restores on.
- Wireless `bonds list` and `bonds remove` no longer busy-wait core 0 for the BTstack-thread result.
  Their response is completed on later task ticks, remains bound to the originating BLE session,
  and keeps the one-command/one-reply bridge busy until completion so overlapping writes cannot
  reorder or inherit a bond result.
- Saved controller-appearance changes can now be applied deliberately without changing output
  personality. The new bonded management command `reenumerate` queues the existing same-personality
  USB detach/reset/reconnect path on core 0; both the Android companion and web portal expose an
  explicit **Apply identity changes** action and warn about the brief console-side reconnect.
- Android's private Amiibo library now supports bounded portal-v3 ZIP import/export and explicit
  key-gated local initialization/re-signing. Imports validate every image before transactionally
  replacing the library; archives exclude retail keys and adapter state.
- Android can make a read-only physical backup of an ordinary NTAG215 Amiibo using foreground
  `NfcA`: exact page reads produce 540 bytes and append the real 32-byte originality signature only
  when `READ_SIG` succeeds. Malformed tags, other NTAG sizes, and figure-v3 fail closed; no NFC
  writes, password authentication, NDEF, or adapter command is used.
- LE bond enumeration is now versioned and complete-or-paginated within the wireless bridge's
  511-byte response ceiling. Android verifies page cursors, totals, and duplicate-free assembly and
  never presents a legacy or malformed partial list as complete.
- Android companion navigation now uses a compact handheld dashboard: the adapter connection row
  appears only on Home, Home keeps controller/Amiibo status in focused tiles, and Settings exposes
  Appearance, Amiibo metadata, connections, About, and Developer details as collapsed categories.
  The Amiibo page is an artwork-led library with working Name/Series/Recently added sorting;
  adapter-only figures use the same catalog identity and artwork on Home and in the library.
  Amiibo keys have no library-page controls or delete action and are chosen only from Settings.
- Bluetooth adapter identity now consistently uses the canonical `PicoSwitch2` name for Classic
  GAP/EIR, BLE GAP/Config advertising, and the Android UI. Android retains an explicitly labeled
  legacy `Joypad Adapter` discovery matcher for pre-name-change firmware; names do not migrate
  bonds, whose saved address/link keys remain authoritative.
- In-band BLE management transport (production-default-on; `mgmt off` is temporary). The configuration BLE service (RX/TX
  GATT + wireless command bridge) can now be armed in a normal controller personality, so a phone or
  the web portal manages the adapter — Amiibo, colors, personality, bonds — over Bluetooth **while a
  controller drives the console**, without the CDC Config re-enumeration that drops the console.
  Gated by a runtime flag `g_mgmt_enabled` (RAM-only, standard builds restore on at reboot); when disabled
  the path is byte-identical to before (the proven zero-cost early return). Wireless flash ops
  (`save`, `amiibo clear`, `amiibo persist`) are deferred rather than busy-waited so they never stall
  the controller report loop. The web portal gains a Management panel (`mgmt on/off/status`) and its
  Connect Bluetooth now works in normal Pro Controller 2 mode once enabled.
  The pairing-window first-bond gate and bonded 16-byte ATT-encryption enforcement are now landed;
  physical rejection/reconnect testing remains. See
  `docs/bluetooth/in-band-management-plan.md`.
- UART coexistence diagnostics for the in-band management path (GP0/GP1 diag link, all personalities):
  `btstate` (live BLE/management snapshot + scan-suppression cause counters) and `btlife read <N>`
  (48-entry lifecycle event ring with HCI disconnect reasons and ordering), to isolate a hardware
  failure where management + controller both drop and only a power cycle recovers. The former
  diagnostic `NS2_MGMT_DEFAULT_ON` option is now the production default; `build.ps1 -MgmtOn`
  remains a compatibility alias. See
  `docs/experiments/in-band-mgmt-coexistence-failure-2026-08-12.md`.
- Phone-app management command surface (USB CDC + the wireless/BLE allowlist), wiring the firmware
  interface so a management app is a thin client — see `docs/bluetooth/app-interface-audit.md`:
  - `personality` reports the current output personality and selectable list; `personality
    <pro2|gc|jcl|jcr>` switches it via the (owner-hardware-confirmed) re-enumeration path.
  - `wake` wakes a sleeping Switch 2 using the learned wake identity (paired once while on).
  - `bonds list` / `bonds remove <index>` manage saved LE pairings (Classic bonds stay on triple-tap
    wipe).
  - `amiibo status` now includes the plaintext `figureId` (NTAG215 and 2 KB v3) for identity lookup.
  - `amiibo reader on|off|send <hex>|reply` drives a connected genuine Pro Controller 2 as an amiibo
    reader for backing up physical amiibo (NTAG215 full; v3 sector 0).
- Pico-side contract coverage for a no-root Android handheld controller bridge. The canonical
  Classic HID descriptor and neutral report are versioned with the firmware, and a host test drives
  their exact report ID, axes, 14-button map, hat, full-state retention, and disconnect cleanup
  through the production generic gamepad parser. Android-initiated HID binding is also pinned for
  OEMs that retain a phone Class of Device; Pico-initiated inquiry remains strict.
- Android companion pairing now presents one persisted PicoSwitch2 relationship. First use says
  **Pair Adapter**; returning sessions try the known management GATT address before bounded service
  discovery; controller mode reuses the saved Classic bond and no longer exposes a second host
  chooser. The real Android association, bond, GATT, HID registration, and HID connection states
  remain separate in diagnostics.
- Android controller input now has persisted Auto/Nintendo/Xbox face-layout normalization. Auto
  recognizes the audited AYN Thor and Retroid built-in-controller identities as Nintendo-style and
  otherwise preserves Android's standard positional/Xbox interpretation.
- Android Amiibo details now add cache-first portal-style AmiiboAPI identity, artwork, release and
  compatible-game metadata plus optional read-only owner/save metadata using a validated, app-
  private user `key_retail.bin`; keys and decrypted values never enter firmware or diagnostics.
- Android appearance now includes persisted System, Light, Dark, and true OLED-black themes plus
  labeled PicoSwitch and Joy-Con-inspired accent palettes that never change firmware identity.
- Firmware-side explicit input ownership is now host-testable: connected HID sources receive stable
  boot-scoped IDs plus connection generations, only one selected source can publish to console slot 0,
  source changes emit a complete neutral boundary and require a fresh report, and active disconnects
  remain neutral without silent fallback. `input sources` is a bounded status surface; wired UART
  also accepts `input active <id|none>`. The default first-source path remains unchanged when no
  explicit selection is requested. Same-link driver rebinds preserve the logical source, while
  transport callbacks carry the generation token through report/disconnect boundaries; the
  transport-integrated lifecycle fixture covers recycled-index stale events. This is source/build
  evidence only; multiple-controller radio, latency, audio/motion, and Android end-to-end switching
  still require hardware validation.

- Default-off genuine/generated motion fitment harness. A verified Nintendo Pro Controller 2
  remains the immutable timing/status/packing/tail source while an independently aligned
  DualSense donor can replace only acceleration, gyro, or carrier-prefix fields. Accel/gyro
  operate on genuine high-rate `0x28`; orientation ownership spans both interleaved `0x1E` and
  every mode-3 `0x28` cadence layout so it cannot alternate between two controller histories.
  Physical-group failures emit byte-identical genuine data; an acquired orientation is held across
  short donor scheduling gaps. UART exposes
  explicit positive-control and one-group modes; retained capture exports the exact base plus
  output XOR, and `motion_lab.ps1` audits loss, mask ownership, fallback integrity, and fixture
  provenance before automatically restoring `off`. Hardware validates the byte-identical,
  acceleration-only, and gyro-only bisections. The first prefix run exposed and localized the
  mixed-source carrier defect; its coherent cross-length correction is host/build validated but
  intentionally unflashed. The translated-`0x28` campaign is now deferred: `0x28` is native
  cadence/history packaging, not a higher-fidelity replacement for the validated production
  `0x1E` path.

- Experimental DualSense length-`0x28` high-rate generation remains behind its default-off UART
  gate. Existing evidence resolves gyro at seven fractional wire bits (`/128`), but the first
  corrected-scale hardware A/B still failed because `0x1E` and `0x28` were emitted on independent
  clocks and incompatible USB ownership cadence. The replacement scheduler uses the proven shared
  PDU tick/elapsed relationship, holds each native-rate PDU across intervening USB polls, and has
  host coverage for `0x1E → 0x28` continuity. Hardware then rejected that scheduler despite healthy
  counters. A tick-weighted gyro replacement was then hardware-refuted as the primary cause: the
  stream still rotated with every gyro axis forced to zero. Live UART found a real cross-layer
  mismatch: the motion seam already outputs Pro2-scaled acceleration at 4096 counts/g, while the
  `0x28` builder divided it by two again. A new independent closed-loop physical-trajectory gate
  then found that bare physical 1 g was still 5.23% below the validated `0x1E` output calibration.
  The default high-rate path now uses the same acceleration gain as `0x1E`; its host test closes 17
  complete representation loops, and six fault injections prove the gate rejects stale prefix,
  scale, axes, time, and following-carrier defects. UART-selectable `live`, `half`, and `zero`
  acceleration modes made the hardware run causal inside one image. The fully coherent LIVE recipe
  was still hardware-rejected with continuous chaotic camera motion and no useful response to
  controller rotation; disabling it immediately restored `0x1E`. The readiness gate now records
  that hard failure and blocks another flash of the same semantic recipe. The validated production
  `0x1E` path is unchanged.

- Shared controller protocol laboratory. A common PowerShell module records Git provenance,
  discovers UART safely, writes BOM-free artifacts, hashes outputs, and emits a stable experiment
  manifest. New motion, audio, command-atlas, capture-to-fixture, and host-only firmware-update
  tools turn one controlled action into replayable evidence. Four repository-local Codex skills
  enforce the protocol, motion, audio, and capture-only firmware workflows. The new
  `audio headset` UART diagnostic is read-only and reports normalized jack state without changing
  playback. A fail-closed flash-space auditor identifies candidate room for a future research-only
  update sink without treating unreserved space as safe. No new controller behavior is enabled by
  these tools. The motion runner also supports offline packaging recovery, active-native-motion
  preflight checks, and time-fraction-aware A/B/A drift subtraction. Its first genuine-Pro2
  magnetic-stimulus campaign found no resolved polarity- or distance-dependent response. A
  passive chart-transition trigger plus reciprocal lazy-susan captures now establish the genuine
  state-0/state-3 boundary projection, prefix seam selection, and the limits of a literal
  strict-smallest-three interpretation. A held-out zero-drop `3 → 1 → 0` gameplay capture
  subsequently refuted the broader one-unsigned-permutation-per-state model; the offline solver
  now reports local edge fits and rejects noncomposable global candidates. The same capture
  validates the cyclic omitted-component paired-sign branch across state 1; the structured model
  fits all five observed boundaries with `0.047878` maximum residual. A later zero-drop
  state-2-only capture crossed `3 → 2 → 3`; both reciprocal seams select the predicted cyclic
  topology and opposite-sign branch. The resulting nine-boundary corpus covers all four chart
  states at RMS/max `0.023541/0.047878`. The stateful local-frame analyzer also resolves the
  interleaved `3 → 2` and formerly suppressed `3 → 1` prefix seam choices.

- The figure-v3 NFC state machine is host-replayable. It moved out of the USB personality into
  `src/nfc/ns2_amiibo_v3_runtime.c` behind a `step(state, host, now_ms, generation, command)`
  interface matching the 540-byte path, with durable side effects behind a small host interface a
  test can fake. `tools/test_ns2_amiibo_v3_runtime.c` replays the recognition read, the Air Riders
  clear/update/write lifecycle, the `0x1E` reuse read, the persistence-gated eject, and the
  mid-transaction generation edge with a fake clock. Console behavior is unchanged.
- Internal v3 error reasons. Every v3 failure reaches the console as the same status `0x07` /
  detail `0x41` (`2115-0096`), which during the investigation was produced by both a tag-removal
  timing bug and a fail-closed record rejection. `amiibo v3diag` now reports which of eight
  internal rules fired, plus the specific validation result and `0x14` stage offset. Its first
  hardware run immediately paid for itself by contradicting the trace decoder (see Fixed).

- NFC investigation laboratory. `tools/amiibo_corpus.py` classifies a directory of 540/572/2048-byte
  dumps offline — structure, SRAM CRC, discovered allocation, and equivalence groups along the
  identity, encrypted-body, and machine-SRAM axes — and emits a portable JSON manifest.
  `tools/ns2_nfc_semantics.py` holds every NFC wire layout in one importable module, giving
  `ns2_trace.py` two new operations: `nfc` reassembles multi-chunk `0x14`/`0x15` transactions into a
  transaction timeline that names each write envelope and pairs error states with the operation in
  flight, and `nfc-diff` reports the first semantic divergence between two captures.
  `tools/nfc_lab.ps1` turns one hardware run into a hashed artifact bundle with its hypothesis,
  intended variable, git revision, before/after diagnostics, decode, and comparison. The
  `picoswitch2-nfc-lab` skill drives the workflow. Rationale and gaps:
  [`docs/re-methodology/nfc-investigation-workflow.md`](docs/re-methodology/nfc-investigation-workflow.md).

- Hardware-confirmed DualSense and DualSense Edge IMU translation to the Switch 2 Pro Controller 2
  motion carrier. Splatoon 3 validates direction, scale, rapid movement, stationary behavior,
  reconnect recovery, and coexistence with controller input, audio, and native haptics.
- Passive UART analysis for genuine length-`0x28` motion PDUs, including exact G6/G7/G8
  signed-field decoding, capture summaries, and regression tests.
- Virtual Amiibo configuration infrastructure: strict 540/572-byte validation,
  transactional upload, full-image retrieval, dirty-write protection, and alternating power-safe
  flash snapshots.
- Capture-derived 2048-byte figure-v3 Virtual Amiibo write infrastructure. It keeps the v3-only
  74-byte `0x14`/`0x21` device command separate from the six-chunk data transaction, validates the
  complete 454-byte envelope before updating the captured mutable ranges, rejects stale generations, persists
  dirty state through the alternating journal, and exposes v3 UID/size/generation/CRC plus full
  readback to Config, UART, and Sync amiibo. Real-console read, write, Stop/eject, next-read, and
  power-cycle recovery are hardware-confirmed.
- General 2048-byte figure-v3 dump support: the `0x21` result now carries all 64 stored SRAM bytes,
  including each image's CRC-16/MCRF4XX, instead of substituting the first captured tag's `7A C4`.
  An untouched downloaded Kirby/Warp dump completed a full read and write with no signature
  override or retail-key transformation; its exported console-written image remains HMAC-valid.
- Capture-derived Air Riders extended-data support handles both sector-aware `0x20` envelopes:
  a 355-byte clear and a 167-byte update spanning page 4, sector-0 pages `0x92..0xE1`, and
  sector-1 pages `0x01..0x18`. It reports genuine empty state `0x16`, journals each stage without
  intermediate ejection, and preserves the following proven 454-byte/`0x08` commit lifecycle.
- Genuine post-write capture proves the 167-byte header also advances chip-managed sector-1 page
  0 independently of its explicit records. Virtual v3 images now retain that dynamic four-byte
  state at `0x400`, persist/export it, and serve it through subsequent sector-aware reads. Air
  Riders hardware validation accepted the resulting second reuse and loaded the previously saved
  customized figure state; a cold adapter power cycle then restored and served the same state
  successfully. A learned gameplay-state save after completing a level also uses the modeled
  extended/ordinary transaction and storage ranges, with an HMAC-valid result.
- Air Riders extended updates now use their self-described allocation instead of Kirby-specific
  pages. King Dedede & Tank Star selects sector-0 page `0xB2` and sector-1 capability/data pages
  `0x64/0x65`; UID, generation, record shape, cleared-window bounds, total length, and padding
  remain strict. Reuse reads and portal initialization follow the selected/full user-memory
  allocation without a figure whitelist. All 16 available Air Riders v3 dumps subsequently
  completed both reads and writes on a real Switch 2.
- Committed Virtual Amiibo removal now has a three-second re-presentation cooldown for both
  540-byte and figure-v3 tags. This gives the console a stable TagRemoved window instead of
  immediately detecting the retained image again and trapping the user in an amiibo prompt.
- Browser-local amiibo library with recursive directory import, single-file import, IndexedDB
  caching, search, parsed tag identity, optional cached AmiiboAPI catalog details, and preservation
  of downloaded game-written state in the cached copy.
- Standalone browser-only Virtual Amiibo diagnostic portal with no serial dependency, including
  transactional upload/CRC checks, a separately persisted simulated adapter slot, controlled write
  injection, download/cache verification, AmiiboAPI testing, and an automated self-test.
- Artwork-first amiibo carousel in both portals. The production view starts empty, adds only
  validated user-imported files, fills progressively during directory scans, keeps the
  selection centered at 100%, and renders four non-overlapping neighbors on each side at exact
  80/60/40/20% sizes. Carousel names are omitted; navigation remains animated. The production
  carousel now uses hovered mouse-wheel/trackpad movement, touch/pen swipes, and keyboard arrows
  instead of visible arrow buttons and a redundant position counter. Its three cycle filters sit
  together below the compact context-aware action and collapse cleanly on mobile.
- Single-slot Virtual Amiibo Manager matching the board's one-amiibo storage: connected Load
  amiibo/offline Select amiibo, validated conditional Import/Sync writeback, and one uniformly
  labeled Eject amiibo button whose tooltip and confirmation reflect its exact scope
  (loaded-plus-adapter eject, unload-only, or adapter-only eject of an image not loaded here).
  Adapter-destructive modes confirm first and remove the stored image (`amiibo clear`) while
  leaving the console Stop/write-back lifecycle unchanged.
- Amiibo identity/generation research (`docs/switch2/amiibo-identity-and-generation.md`,
  `docs/experiments/generated-amiibo-console-rejection-2026-07-26.md`): a 2026-07-26 hardware test
  established that the Switch 2 validates amiibo cryptography, so key-free generated images are
  rejected. A briefly implemented random-UID presentation mode was removed for the same reason.
- The Virtual Amiibo library is import-only (single file or recursive directory of the user's own
  genuine dumps). A key-based generator using a user-supplied `key_retail.bin` was prototyped and
  removed in favor of import-only simplicity. A narrower explicit Initialize action is retained:
  it works offline on an imported dump, requests user-owned keys when needed, clears ordinary and
  Air Riders v3 save state, re-signs locally, and self-verifies before changing IndexedDB.
  Identity/crypto research is retained under `docs/switch2/`.
- Library export/import is now a flat `.zip` (`library.json` manifest + one `.bin` per amiibo) via
  a self-contained store-only ZIP writer/reader; legacy `.json` backups still import.
- The AmiiboAPI catalog is enhancement-only: entries always display (on-tag identity when the
  catalog is unavailable), the catalog loads cache-first from two mirrors, and it never gates
  display or import.
- Import preserves exact 2048-byte NTAG I2C Plus 2K figure-v3 images with their contiguous UID
  layout. NTAG215 inputs remain 540/572 bytes with their native BCC validation; newer amiibo not
  yet in AmiiboAPI still import.
- Carousel loops at both ends with clean non-wrapping slides and shows the centered amiibo's
  release date above it. The production manager now uses compact search, three tap-to-cycle filter
  chips, one context-aware Load/Select/Import/Sync action, active-tag auto-selection on connection,
  save metadata and write-count badge around the centered artwork, and an inline details drawer
  for compatibility plus Download/Initialize/Eject/Delete. Physical scanning remains
  capability-gated until a Config-transport firmware API exists.
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

### Fixed

- Console rumble now reaches the handheld's actuator. The companion binds the vibrator belonging to
  the selected input device rather than the phone's system vibrator, classifies the effect so the
  platform does not discard it as touch feedback, and holds a connected-device foreground service
  for the life of the link so effects are not dropped while the user is looking at the television.
- The companion's motion frame is corrected for display rotation, so aim is not rotated on a
  handheld whose natural orientation differs from the way it is held.
- Removed the unused transport-level `bt_disconnect()` operation whose only CYW43 implementation
  was a silent no-op. Real link teardown remains owned by BTstack's handle/HID-CID paths. The
  Switch 2 BLE report driver's stale rumble TODO now points to the existing connection-level LRA
  renderer instead of implying native rumble is unimplemented.
- Android packaging now declares explicit no-backup rules for both legacy backup and Android 12+
  cloud/device transfer, keeps the adaptive icon in the minimum-SDK resource bucket with an
  Android 13 monochrome override, and uses window bounds for compact layouts. This removes the
  actionable manifest/resource/layout lint findings without changing the tested target SDK or
  opportunistically upgrading runtime dependencies.
- Disconnecting a Bluetooth input controller now clears its published name, VID, and PID together
  with the neutral input report. Management clients no longer keep showing the last controller as
  attached after it powers off.
- Android companion setup no longer crashes when Android 13 enforces the Companion Device Manager
  feature declaration. HID-profile acquisition/registration failures are reported as recoverable
  bridge states, and ordinary 540/572-byte Amiibo Sync no longer mistakes firmware's `00000000`
  unavailable-CRC sentinel for a whole-image checksum. Figure-v3 CRC enforcement remains strict.
- Android HID registration now treats `onAppStatusChanged()` as authoritative. The AYN Thor can
  return `false` synchronously from `registerApp()` and then successfully register milliseconds
  later; the old client closed that accepted proxy and made its next attempt collide with its own
  live registration as "another HID Device app." Retry state now waits for the callback and times
  out cleanly without unregistering a genuinely different provider.
- Android HID connection now likewise waits for `onConnectionStateChanged()` with a bounded timeout,
  and re-entering controller mode reuses an existing app registration instead of colliding with it.
- Pro Controller 2 and NSO GameCube personalities are now recognized on a **fresh** Windows PC
  without a manual WinUSB reset. Both now serve the Microsoft OS 1.0 Extended Properties descriptor
  (`wIndex=0x0005`) on their vendor interface, registering the Nintendo device-family
  `DeviceInterfaceGUID {6F13725E-EF0E-4FD3-AE5F-B2DE989EC825}` that libusb/SDL/Steam need to open the
  interface. Previously only Joy-Con 2 served it, so Pro2/GameCube worked only on a machine that had
  already hosted a genuine Nintendo controller (which had registered the GUID); a truly fresh PC
  showed Steam's "Begin Setup" / generic-device state. Windows-only enumeration metadata; Switch 2
  descriptors and protocol behavior are unchanged.
- Descriptor-backed generic Bluetooth gamepads now drop truncated input reports atomically instead
  of interpreting absent trailing fields as zero or released. Valid reports, longer vendor reports,
  and descriptorless Classic fallback behavior are unchanged.
- `nfc_lab.ps1` wrote its artifact bundle with `Set-Content -Encoding utf8`, which emits a UTF-8
  BOM under Windows PowerShell 5.1 and none under pwsh 7. A bundle captured from a 5.1 session was
  unreadable by any JSON parser. All artifact writes now go through an explicit BOM-free writer.
- `nfc_lab.ps1` warned about a missing `-Variable` after the run was already set up instead of
  asking for one. It now prompts, and accepts an empty answer to record an honest
  `kind: observation` run; passing `-Variable` or `-DwellSeconds` skips prompting for scripted use.
- The NFC trace decoder reported every status `0x07` / detail `0x41` as a failure. That pair is
  also the deliberate TagRemoved signal after a committed write, so a healthy write/remove/rescan
  cycle was flagged as broken. `ns2_trace.py nfc` now separates expected removal edges from
  failures, and `nfc_lab.ps1` cross-checks its verdict against the firmware's own error counter and
  reports a disagreement instead of trusting the trace.

### Changed

- Active transport and handoff documents now consistently describe the production-default-on,
  bonded/encrypted in-band management path, retained CDC Config fallback, current console-sleep
  policy, completed command atlas, and restored main-repository Android ownership. Dated experiment
  reports remain unchanged.

- `tools/verify_amiibo_crypto.mjs` now requires the retail-key path instead of defaulting to a
  maintainer-specific absolute path, and accepts `--json`. `tools/validate_amiibo_collection.py` is
  removed; `tools/amiibo_corpus.py` supersedes it and additionally handles 2048-byte images.
- UART port auto-discovery no longer selects a motherboard legacy serial port, so a machine with
  one such port refuses rather than appearing to find a dead board.
- Virtual Amiibo is always available. Blank firmware presents no virtual tag, and the virtual
  runtime owns NFC only after the user loads one of their own images.
- Browser libraries use AmiiboAPI-ordered mutable dumps with content-derived keys for distinct v3
  rider/machine combinations, expose one loaded-slot pointer, ignore duplicate owned files, and
  cache the shared public catalog only once instead of duplicating matched metadata. The production carousel preserves
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
- The large setup card and three-column Amiibo action/filter/detail grid were replaced by one
  carousel-centered manager. Amiibo operation messages now remain inside that manager instead of
  consuming the global connection header, and the log remains under Developer diagnostics.
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
  **Export saved library** JSON backup preserves every mutable dump and loaded-slot state
  and can be imported after browser storage is cleared.

### Validation

- Clean Pico 2 W and Pico W release builds; both images carry a verified install-reset marker.
- 311 JVM tests across the bridge core and the Android companion; Android lint clean; instrumented
  test sources compile.
- 11/11 in-band management and bridge C host suites, including the descriptor identification trace
  and its failure modes.
- Descriptor parity and the contract digest guard both pass. Both guard paths were verified by
  deliberate mutation: a single vendor-block byte changed in both languages, and a version bump
  with no registered digest — each was rejected with the corrective step printed.
- Hardware sanity pass on an AYN Thor with no regressions: buttons, sticks, triggers, D-pad,
  C/GameChat, battery, motion and rumble all confirmed, with the adapter reporting `v2-bridge`
  identification.
- The management session integration suite now links the production access-control module instead
  of a weaker local imitation. It proves allowlisted commands are rejected for bonded-but-plaintext
  and encrypted-but-disconnected states as well as unbonded, disabled, and diagnostic cases;
  `tools/run_mgmt_tests.ps1` remains 11/11 green.
- DualSense gyro immediately returned to normal when the experimental length-`0x28` gate was
  disabled, confirming the validated production path remains the length-`0x1E` carrier.
- All 53 host-test executables pass, including the v3 write codec, Config BLE bridge, locked
  base-map test, and motion/PDU tests. They
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
- All 53 host-test executables pass after the Virtual Amiibo write, full-SRAM figure-v3 response,
  BOOTSEL-policy, Config BLE,
  and v3 write-codec integration. Coverage includes the captured v3 six-chunk commit,
  retry/conflict, incomplete/UID mismatch, protected/out-of-range records, trailing data, the
  validated 540 format-promotion and 700 ms completion, atomic failure handling, and retained-image
  logical removal. Pico W and Pico 2 W release builds both succeed. The 540-byte and 2048-byte
  write/eject/re-present/persistence lifecycles are hardware-confirmed; production-portal v3 Sync
  remains pending.

### Known limitations

- The adapter firmware and the Android companion must be updated together. 2.0.0 uses a newer
  bridge contract (version 3) than earlier builds; a mismatch disables battery, motion and rumble while
  ordinary input continues to work. The companion now detects and reports this.
- Keep the companion open on the handheld while playing. Android delivers controller input only to
  the focused app, so the companion must be the foreground window to read the handheld's controls —
  which is the normal case, since you are looking at the television rather than the handheld's
  screen. A connected-device foreground service keeps the link and the rumble path alive
  throughout, and backgrounding the app releases any held input rather than leaving it stuck.
  Android also permits only one HID Device application system-wide, so another HID-emulation app
  can prevent the bridge from registering.
- Motion translation is validated per controller family, not universally: genuine Pro Controller 2
  passthrough, DualSense/DualSense Edge, and the Android companion are hardware-confirmed. Any
  additional family still needs its own calibration, axis, scale and timestamp validation before
  being routed through the translator.
- No audio over the Android bridge. HID cannot carry audio, and the only transport with sufficient
  bandwidth is WiFi, which this firmware will not enable. This is closed, not deferred.
- DualSense audio requires Pico 2 W at 300 MHz. Pico W cannot sustain it and ships a non-audio
  profile. DualSense microphone return remains unimplemented.
- Joy-Con 2 reconnect is confirmed but has not had the twenty-cycle soak the Pro Controller 2
  received.
- Amiibo: the Virtual Amiibo read/write/persist/eject/re-present/library workflow and genuine
  Pro Controller 2 physical tag reads are hardware-confirmed. Native physical writes, production
  native-reader gating, and Switch 1 translation remain open.
- The legacy `pico_w_switch1` (`NS2_PRO=OFF`) build target does not compile. Pre-existing and
  unrelated to this release; the shipped Pico W and Pico 2 W targets are unaffected.

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
