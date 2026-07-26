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
  The adapter keeps one imported immutable **Unused** copy and one optional console-written
  **Used** copy; either can be selected without deleting the other.
- The production portal library remains available without Web Serial. It accepts one file or
  recursively scans a selected directory, caches separate Unused/Used bytes and optional
  AmiiboAPI details in browser-local IndexedDB, and can export/import the complete library in a
  versioned JSON backup. One public catalog is downloaded and matched locally, so original tag
  bytes/IDs/UIDs/save data never leave the browser.
- **Save current Amiibo** retrieves both adapter copies, writes them directly into the selected or
  UID-matching cached entry, and acknowledges firmware dirty state only after IndexedDB confirms
  the write. Browser security does not permit silently overwriting the original OS file.
- `web/diagnostic.html` and `tools/run_amiibo_portal_test.ps1` provide the same library/metadata
  workflow plus a browser-only simulated adapter, transactional upload, controlled write,
  persistence, cache-first save-back, and self-test without Web Serial or hardware.
- Both portals use an artwork carousel: the selected/active tag is centered with four neighbors on
  either side. `tools/run_config_portal.ps1` serves the real portal from the same localhost origin.
- Portal follow-up after the persistence pass: add distinct **Eject** (retain the selected image
  but present no tag) and **Remove active amiibo** (clear the adapter slot after protecting dirty
  data) controls. Saving clears dirty-write protection after IndexedDB persistence; it does not
  unload the tag. Selecting Unused acts as a reversible browser-side reset without deleting Used.
- Config mode is now CDC-only. The MSC descriptor/callbacks, generated `src/web_disk.h`, embedded
  FAT image, `CONFIG.HTM`, and generator were removed. The config VID/PID and CDC command protocol
  remain unchanged; `tools/run_config_portal.ps1` is the production entry point. Config
  enumeration, Virtual Amiibo transfer, save/readback, and direct BOOTSEL exit are
  hardware-confirmed with the CDC-only descriptor.
- BOOTSEL now has a host-tested action matrix: single-tap cycles only controller personalities
  when a controller is HID-ready; double-tap opens pairing; triple-tap wipes/disconnects; and a
  two-second hold enters Config directly. Config ignores single/double, permits triple-tap wipe,
  and uses the same hold to return directly to Pro2. Paired double-tap first disconnects without
  deleting the bond. Both board builds pass; this revised physical gesture matrix awaits hardware
  validation.
- Sectors `-3` and `-5` hold alternating version-2 snapshots with generation, header/payload CRC,
  Unused/Used images, selection, dirty state, and optional signature. The previous bank stays valid
  until the new bank is programmed and verified; version-1 sector-`-3` records migrate as an
  Unused baseline. Flash work runs only through the existing core1 config-save service.
- A genuine Pro2 physical-tag read through the UART-gated bridge was recognized by the Switch.
  Primary capture proves USB uses a 600-byte reader buffer fetched as eight 70-byte chunks plus one
  40-byte final chunk; it does not request one 622-byte payload.
- The disabled-by-default virtual runtime handles the confirmed read flow. A real Switch 2
  recognized an uploaded Virtual Amiibo using a non-NFC source controller.
- With a stable native write capture unavailable, a conservative transactional virtual write path
  was reconstructed from the local command examples, existing primary read/state captures, and the
  pinned capture-derived Dycool codec: exact-UID `0x06`, 64-byte write preparation, bounded
  454-byte `0x14` staging, atomic `0x08` commit, generation-safe store update, dirty state, and a
  modulo-eight report event counter. A real-console write completes through `0x08` and accepted
  `05 00` without a crash. Each commit now creates/selects the Used image and queues persistence.
- The first write test crashed the Switch with `2168-0002`. Root cause was exact and local:
  `0x14` totals 88 bytes, while `ns2_task` dispatched each 64-byte `tud_vendor_read` result
  immediately. `ns2_vendor_rx` now reassembles the envelope-declared command across the observed
  64+24 split, handles arbitrary/coalesced/oversized framing, and resets on USB mount. The rebuilt
  UF2 is hardware-confirmed not to crash.
- The successful retest trace shows the console received `05 00`, sent Stop, then rescanned once
  per second because the retained RAM image still appeared physically present. The runtime now
  separates retained image state from presentation. A committed Stop waits for the pending flash
  snapshot, emits logical removal only after verification, and keeps the Used image selected for
  saving. The next `0x03` scan re-presents that same updated image as a fresh tag. The removal and
  re-presentation lifecycle, persistence gate, and power-cycle recovery are hardware-confirmed.
- The production portal retains its cached library without Web Serial. Hardware/browser validation
  confirms reversible Unused/Used selection, Save current Amiibo cache-first writeback, and
  export/clear/import restoration of the complete versioned library backup.
- The portal cannot reach USB CDC while the Pico remains attached to the console. The rebuilt
  firmware and PC helper add `amiibo status/read/acknowledge` plus `amiibo dump -OutputPath` over
  UART, with generation, exact-size, and UID/BCC validation before dirty acknowledgement.
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

1. Add explicit portal Eject/Present controls and validate manual removal/replacement and reconnect.
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
