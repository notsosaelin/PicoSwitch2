# Current Continuation Context

Last reconciled: 2026-07-25

Working branch: `ns2-testing`

Latest published release: `v1.5.0` (2026-07-22)

Always run `git status`, `git log -1`, and read `STATUS.md` at the start of a new session. This file
deliberately does not pin the latest commit because it should remain useful after later checkpoints.
On a new workstation, clone `ns2-testing` with `--recurse-submodules`; `third_party/opus` is a
required Git submodule for Pico 2 W audio builds.

## Current validated baseline

- Pico 2 W: standard 300 MHz build with DualSense audio/native haptics.
- Pico W: validated non-audio profile.
- Pro Controller 2, NSO GameCube, Joy-Con 2 Left, and Joy-Con 2 Right personalities enumerate and
  report current firmware identities without an update prompt.
- Input, mappings, rumble, battery passthrough, wake, reconnect, LEDs, BOOTSEL gestures, and config
  persistence have broad hardware coverage; see the compatibility matrix for per-controller scope.
- Genuine Pro Controller 2 native `0x1E`/`0x28` motion passthrough, bonded HOME reconnect, P1 LED,
  gyro, and headset audio are hardware-confirmed.
- DualSense and DualSense Edge translation to the Switch 2 length-`0x1E` motion carrier is
  hardware-confirmed in Splatoon 3, including rapid movement and reconnect.
- DualSense console/Windows audio, physical headset state, reconnect audio, and waveform-preserving
  native haptics are hardware-confirmed on Pico 2 W.

## Motion boundary

- Production genuine Pro2 input transports its controller-generated `0x1E`/`0x28` PDUs opaquely.
- Production DualSense/Edge output synthesizes only the decoded length-`0x1E` quaternion carrier.
- G6/G7/G8 in normal `0x28` PDUs have an exact signed 22/22/20-bit codec, but their semantics and
  the changing leading/middle lanes remain incomplete.
- The template experiment—genuine static `0x28` body plus dynamic timing/G6/G7/G8—caused immediate
  random motion. The runtime generator was removed. Do not repeat it without decoding every
  console-relevant changing lane.
- A coherent software-generated `0x28` reference/magnetometer solution is the accepted direction
  for controllers that lack the hardware. The prohibition is against the refuted static-template
  method, not against a fully modelled future software solution.

## NFC boundary

- Config mode accepts strictly validated 540/572-byte images through transactional 32-byte chunks.
  Virtual Amiibo is always available: a blank store presents no virtual tag, while a loaded image
  automatically owns the virtual source. The adapter's internal baseline/latest-written recovery
  pair remains implementation-only; the portal does not expose reset-to-original behavior.
- The production portal library remains available without an adapter connection. It accepts one
  file or recursively scans a selected directory and caches one record per AmiiboAPI catalog ID
  with one mutable dump in browser-local IndexedDB. Imports require an exact local AmiiboAPI match;
  duplicate owned files do not create extra records. Two independent quick-slot pointers can
  reference different entries. The shared catalog is downloaded and stored once, then matched
  locally, so original tag bytes/IDs/UIDs/save data never leave the browser.
- The production carousel starts empty and displays only user-imported records. Directory scans
  add those records to the visible library progressively. AmiiboAPI order remains authoritative;
  each filter row cycles `All`, then only the imported library's available values alphabetically.
  Centered artwork stays fixed in the middle at 100%; four non-overlapping neighbors on each side
  are exactly 80/60/40/20%. Selection changes animate without rebuilding the track, and carousel
  names are omitted.
- **Sync Amiibo from Adapter** retrieves the latest active image, validates its raw format and
  exact AmiiboAPI identity, overwrites the selected or UID-matching cached entry, and acknowledges
  firmware dirty state only after IndexedDB confirms the write. Browser security does not permit
  silently overwriting the original OS file.
- `web/diagnostic.html` and `tools/run_amiibo_portal_test.ps1` provide the same library/metadata
  workflow plus a browser-only simulated adapter, transactional upload, controlled write,
  persistence, cache-first save-back, and self-test without Web Serial or hardware.
- Both portals use an artwork carousel. The production detail card intentionally exposes only
  friendly Name, Character, Game series, Amiibo series, and Product type fields.
  `tools/run_config_portal.ps1` serves the real portal from the same localhost origin.
- The board stores exactly one amiibo (flash Save 1/Save 2 = one identity's baseline/latest-written
  recovery pair, not two amiibo). The manager is single-slot: **Load Amiibo** stages the highlighted
  carousel entry → **Activate Amiibo** (disabled "Amiibo activated" while already presented).
  **Sync Amiibo** pulls the latest console-written image into IndexedDB. One merged eject/clear
  button labels its exact scope from `amiiboEjectActionState()`: **Eject Amiibo** (loaded amiibo is
  on the adapter → confirm, `amiibo eject` + `amiibo clear`, unload), **Clear Loaded Amiibo**
  (unload only, no confirm), or **Eject Virtual Amiibo** (adapter holds an image not loaded here →
  confirm, adapter wipe only). Cancel aborts everything; library dumps are never deleted; the
  console-driven Stop/write-back lifecycle and `amiibo present` re-activation are unchanged.
- Virtual Amiibo library is **import-only** (single file or recursive directory of the user's own
  genuine dumps). Amiibo crypto is enforced (2026-07-26 hardware test,
  [[amiibo-identity-and-generation]] / `docs/experiments/generated-amiibo-console-rejection-2026-07-26.md`):
  the Switch 2 rejects key-free generated images ("This isn't an amiibo"). Random Mode was removed
  (UID-bound HMAC). A key-based generator (amiitool over Web Crypto, user `key_retail.bin`) was
  prototyped then **removed** for import-only simplicity — do not re-add unless the user asks. The
  identity/crypto research doc is retained.
- Library export/import is a flat **.zip** (`library.json` manifest + one `.bin` per amiibo) via a
  self-contained store-only ZIP writer/reader (`amiiboZipStore`/`amiiboZipEntries`, deflate fallback
  via DecompressionStream); import reconstructs from the `.bin` files and legacy `.json` still
  imports. Import is structural-only: `coerceAmiiboImport()` accepts 540/572 and larger
  emulator-container dumps (e.g. 2048-byte Pixl.js/allmiibo/flashiibo files) by taking the leading
  540-byte NTAG215 image and recomputing BCC0/BCC1 (pure UID checksums); the AmiiboAPI catalog is
  enhancement-only and never gates import, so brand-new amiibo not yet in AmiiboAPI import fine.
- Carousel loops at both ends (`moveAmiiboCarousel` wraps) while the neighbor window uses real
  non-wrapping indices for clean slides; the centered amiibo's release date shows above it. Sort is
  two filter-panel cycle rows: **Sort by** (Default/Alphabetically/Numerically/Release date) and
  **Order** (Ascending/Descending). Action stack has Activate/Sync/Eject (always labeled "Eject
  Amiibo"; "Amiibo Active" when presented) plus Download .bin / Delete from Library / Refresh.
- Star-variant caveat (e.g. Kirby Air Riders): the 4 files per rider are byte-identical in the whole
  540-byte NTAG215 image; the difference lives only in the emulator container (offset 0x3C2+),
  outside the NFC data. They dedup to one console amiibo; separating them into functionally distinct
  tags would need amiibo keys to merge the save into the encrypted app-data (out of scope).
- Sync clears dirty-write protection only after IndexedDB persistence; it does not unload the tag.
  Console formatting/reset remains the authority.
- The USB side of Config mode is now CDC-only. The MSC descriptor/callbacks, generated
  `src/web_disk.h`, embedded FAT image, `CONFIG.HTM`, and generator were removed. The config
  VID/PID and CDC command protocol remain unchanged; `tools/run_config_portal.ps1` is the
  production entry point. Config enumeration, Virtual Amiibo transfer, save/readback, and direct
  BOOTSEL exit are hardware-confirmed with the CDC-only USB descriptor.
- That same portal now supports a Config-personality-only BLE GATT transport. Controller discovery
  stops before low-duty management advertising; the incoming Peripheral-role link is classified
  before HID/GATT-client setup; and a bounded cross-core bridge executes only production
  settings/Amiibo commands through the existing core-0 parser. Leaving Config disconnects the
  browser before discovery resumes. Normal controller personalities perform no management radio
  work. All 49 host tests, three firmware build axes, and portal static checks pass; Config BLE
  hardware validation is pending.
- Config schema v10 removes the Virtual Amiibo flag and controller-family mapping tables. A locked
  base button map feeds the emulated Nintendo identity, leaving persistent button remapping to the
  Switch. The shared Pro2 body/Sony lightbar color and independent Joy-Con accent controls remain
  together in one compact production-portal panel and in config storage. The obsolete visible
  current-input/current-output cards are removed. Each
  flashed UF2 contains a one-shot marker that erases the five persistence sectors (settings, both
  amiibo banks, wake identity, and BTstack bonds) before normal startup; an ordinary reboot sees
  the consumed marker and retains state. Both BLE-visible names are `PicoSwitch2`.
- BOOTSEL now has a host-tested action matrix: single-tap cycles only controller personalities
  when a controller is HID-ready; double-tap opens pairing; triple-tap wipes/disconnects; and a
  two-second hold enters Config directly. Config ignores single/double, permits triple-tap wipe,
  and uses the same hold to return directly to Pro2. Paired double-tap first disconnects without
  deleting the bond. Both board builds pass; this revised physical gesture matrix awaits hardware
  validation.
- Sectors `-3` and `-5` hold alternating version-2 snapshots with generation, header/payload CRC,
  internal baseline/latest-written images, selection, dirty state, and optional signature. The previous bank stays
  valid until the new bank is programmed and verified. Flash work runs only through the existing
  core1 config-save service after the install-reset first boot.
- A genuine Pro2 physical-tag read through the UART-gated bridge was recognized by the Switch.
  Primary capture proves USB uses a 600-byte reader buffer fetched as eight 70-byte chunks plus one
  40-byte final chunk; it does not request one 622-byte payload.
- The always-available virtual runtime handles the confirmed read flow after a tag is loaded. A real Switch 2
  recognized an uploaded Virtual Amiibo using a non-NFC source controller.
- With a stable native write capture unavailable, a conservative transactional virtual write path
  was reconstructed from the local command examples, existing primary read/state captures, and the
  pinned capture-derived Dycool codec: exact-UID `0x06`, 64-byte write preparation, bounded
  454-byte `0x14` staging, atomic `0x08` commit, generation-safe store update, dirty state, and a
  modulo-eight report event counter. A real-console write completes through `0x08` and accepted
  `05 00` without a crash. Each commit updates/selects the internal latest-written image and queues
  persistence.
- The first write test crashed the Switch with `2168-0002`. Root cause was exact and local:
  `0x14` totals 88 bytes, while `ns2_task` dispatched each 64-byte `tud_vendor_read` result
  immediately. `ns2_vendor_rx` now reassembles the envelope-declared command across the observed
  64+24 split, handles arbitrary/coalesced/oversized framing, and resets on USB mount. The rebuilt
  UF2 is hardware-confirmed not to crash.
- The successful retest trace shows the console received `05 00`, sent Stop, then rescanned once
  per second because the retained RAM image still appeared physically present. The runtime now
  separates retained image state from presentation. A committed Stop waits for the pending flash
  snapshot, emits logical removal only after verification, and keeps the latest-written image selected for
  saving. The next `0x03` scan re-presents that same updated image as a fresh tag. The removal and
  re-presentation lifecycle, persistence gate, and power-cycle recovery are hardware-confirmed.
- The production portal retains its cached library without an adapter connection. Prior
  hardware/browser validation confirms cache-first writeback and export/clear/import restoration
  of the complete versioned library backup. The new one-dump/two-quick-slot presentation awaits
  browser and hardware regression validation.
- USB CDC remains unavailable while the Pico is attached to the console. A two-second BOOTSEL
  hold can now enter Config there and expose the local portal over BLE for settings and Virtual
  Amiibo management. UART remains the independent live-console research/export path during a
  normal controller personality; `amiibo status/read/acknowledge` and `amiibo dump -OutputPath`
  retain generation, exact-size, and UID/BCC validation before dirty acknowledgement.
- Hardware validation captured committed write → `05 00` → Stop → absent `07 41`, followed by a
  later scan, the same UID, and a complete updated read. UART status changed from clean generation
  4 to dirty generation 7; export produced a valid 540-byte image with 426 changed bytes confined
  to writable ranges and cleared dirty only after saving.
- Normal 540-byte files do not contain the NTAG `READ_SIG` result. The successful native buffer's
  signature field appeared zeroed, and the real console accepted the virtual reader path; the
  original hardware test did not record whether its selected upload was 540 or 572 bytes, so
  per-format signature compatibility is not yet distinguished.
- Native Pro2 relay writes `0x0016`, subscribes to `0x001E`, and receives matching ordinary NFC
  replies on `0x001A`. It remains UART-gated and read-only pending production/reconnect/removal and
  write validation.
- A 2026-07-25 Smash native-write attempt produced no `0x14`, `0x08`, or UID-bearing `0x06`
  because the multi-amiibo device changed UID between presentations. It cannot validate native
  writes. Its `4,5,6,7,0` progression independently supports the implemented modulo-eight event
  counter.
- Switch 1 Pro/Joy-Con Right NFC uses report `0x31` plus its MCU protocol and requires translation,
  not raw forwarding.

## Highest-value open work

1. Hardware-validate the implemented manual Eject/Present path, including replacement and reconnect.
2. Capture a genuine Pro2 physical-tag write/readback before enabling native writes.
3. Decode/model the unresolved genuine `0x28` lanes for the accepted software-reference path.
4. Add DualSense microphone return only after preserving the confirmed speaker/haptic path.
5. Extend motion translation to another controller family only after verifying its calibration,
   axes, units, timestamps, and stationary-bias behavior.

## Known traps

- A fresh Codex session has no conversation memory. Repository evidence is the handoff.
- Do not use symptom-driven “spin and tell me” tuning when captures, UART, or screen analysis can
  answer the question.
- Do not assume a controller's VID/PID can be recovered late from every saved bond; fresh pairing
  and already-bonded paths differ.
- Do not merge Pico 2 W audio scheduling into Pico W.
- Do not alter genuine Pro2 audio framing: one 240-byte/20 ms CELT frame is split into ordered
  120-byte `0x04` and `0x02` GATT writes.
- Do not add large captured media without updating `dumps/README.md` and binary attributes.
