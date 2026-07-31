# Current Continuation Context

Last reconciled: 2026-07-29

**Start here if you are resuming v3 amiibo work:** `docs/Amiibo-v3.md` §18 is authoritative; the
NFC boundary section below carries the short form and the open experiment.

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

## Protocol laboratory boundary

- Shared workflow: `docs/re-methodology/controller-protocol-lab.md`.
- `motion_lab.ps1` captures one short stationary condition and packages `ns2_magprobe` analysis,
  baseline comparison, fixtures and hashes. Use separate no-magnet, sham, polarity, distance and
  recovery runs. The 2026-07-29 genuine-Pro2 campaign is complete. For a stimulus bracketed by
  baseline and recovery, use `ns2_magprobe.py aba ... --fraction F` to remove time-weighted drift.
- `audio_lab.ps1` is observational by default and protects the validated speaker path. `audio
  headset` reports normalized jack state without changing routing.
- `ns2_firmware_update.py` is a host-only capture model. It rejects the generic tracer's truncated
  long `0x0D/04` records; the dedicated on-device flash sink is still pending.
- Repository-local Codex skills live under `.agents/skills/`. Existing NFC/Claude workflow remains
  under `.claude/skills/`.

## Motion boundary

- Production genuine Pro2 input transports its controller-generated `0x1E`/`0x28` PDUs opaquely.
- Production DualSense/Edge output synthesizes only the decoded length-`0x1E` quaternion carrier.
- Exact ICM-42670-P FIFO tables plus decrypted genuine Pro2 PCAPs establish two ground-truth paths:
  handle `0x000A` has an 18-byte raw timestamp/temperature/accel/gyro sample; length-`0x28` on
  `0x000E` is a 4-byte timer/elapsed/status prefix plus a 288-bit packed multi-sample IMU payload.
  The reference Pro2 session is almost entirely catch-up layout because it spans 17–19 internal
  ticks per notification.
- The former G6/G7/G8 signed aliases cross packed latest-gyro and accel bit ranges. They are not
  independent magnetic/reference lanes or a second quaternion. The completed magnet matrix remains
  valid negative evidence and helped expose the false semantic model.
- The template experiment—genuine static `0x28` body plus dynamic timing/G6/G7/G8—caused immediate
  random motion because it corrupted fragments of real packed gyro/accel samples. The runtime
  generator was removed.
- `tools/ns2_motion_reference.py` analyzes PCAPNG through TShark plus live UART `blecap` and
  `motionpair` JSONL.
  UART `imuref on|off|status` is default-off and replays the known-good `0x000A` control profile;
  hardware now confirms clean raw sample delivery, exclusive CCC ownership, ATT success, and
  reversible restoration of fresh native motion. The PDU now classifies itself:
  `elapsed_ticks = (pdu[2] << 4) | (pdu[1] >> 4)`. This matched the preceding carrier delta in
  `1274/1274` zero-drop comparisons and remains usable when capture records are missing. Byte 3
  independently maps `0x0D` to high-rate, `0x0E` to normal, and `0x0F` to catch-up in
  `1292/1292` zero-drop records.
- A zero-drop 7.5–30 ms cadence matrix fully maps the accel/gyro fields. High-rate (`0..10`
  ticks) is signed22 accel/gyro/accel with eight fractional bits; normal (`11..14`) is mixed
  13/14-bit accel/gyro; catch-up (`15+`) is mixed 13/14/16-bit accel/gyro. Scaled axes agree with
  raw handle `0x000A`, and all stationary accel lanes measure approximately `1.052 g`.
- The prefix boundary is corrected. It is packing mode `3` plus `s24+s23+s25` in high-rate and
  `s22+s21+s23` in normal/catch-up. Carrier 2 is split on the wire: its low two bits precede its
  signed high bits. The former “separate state” was that low fragment. After dividing high-rate by
  four and grouping only by the length-`0x1E` carrier state, fixed slopes
  `sqrt(2)/2^24`, `sqrt(2)/2^23`, and `sqrt(2)/2^23` align it to the retained length-`0x1E`
  carrier. The epoch is packet-derived: `current tick - encoded elapsed + 4`, equivalently four
  ticks after the preceding carrier. It gives `0.999996` correlation in both dynamic captures and
  improves the mixed-cadence pitch fixed NRMSE from `0.008728` to `0.002718`. A causal modular
  history decoder has `0.000968°` and `0.004682°` median error against interpolated length-`0x1E`
  truth, with zero observed chart mismatch.
- Reciprocal zero-drop lazy-susan captures now cross `3 → 0` and `0 → 3`. State 0 wire
  `(G0,G1,G2)` and state 3's local state-0-boundary projection `(G1,G2,G0)` are continuous at both
  boundaries. Sixteen rapid genuine samples exceed retained energy 1.0, so the literal
  strict-smallest-three interpretation is refuted as an exact genuine model. The prefix seam
  selected chart 0 in both directions. A clean `0 → 1` seam selected local projection
  `(G2,G0,G1)`, but a held-out zero-drop `3 → 1 → 0` capture refutes composition into one global
  unsigned map: its `1 → 0` edge has minimum residual `1.185389`. The cyclic omitted-component
  topology's paired-sign branch fits that edge at `0.024716`; all five captured boundaries give
  RMS/max `0.025302/0.047878`, so state 1 now has both branch types covered. A later zero-drop
  state-2 trigger captured reciprocal `3 → 2 → 3` seams. Both select topology `(G2,G0,G1)` and
  opposite-branch signs `(+,−,−)`, at residuals `0.036162` and `0.011824`. Across all nine
  accepted boundaries, all four chart states are represented and the cyclic model has RMS/max
  `0.023541/0.047878`. The direct `3 → 2` length-`0x28` seam selects chart 3
  (`0.003833` versus `0.196168`), and the formerly suppressed `3 → 1` seam selects chart 1
  (`0.008416` versus `0.242898`). Exact integer projection/rounding remains open; do not promote
  this to a production `0x28` generator.
- The high-rate/normal tail carries two Q3 IMU-temperature samples sharing a signed ten-bit
  integer part. Two zero-drop raw/native/raw captures independently bracket native means
  `4.28125` and `3.951923` with handle-`0x000A` raw-temperature means near `4`. The low fractions
  matched in `993/1023` records because the adjacent samples usually share a sub-count value.
- The catch-up format remains unchanged through the maximum accepted 30 ms interval. Bit 287 is
  the single byte-alignment remainder after 287 established data bits and stayed zero in all 1,066
  catch-up packets across 14 repository captures. Treat it as observed reserved-zero padding.
- Dual CCCs do not emit both paths simultaneously: native takes priority, and disabling only its
  CCC resumes raw delivery. The raw/native clocks remain monotonic across A/B/A switching but have
  a path-dependent 23--29 tick offset. `imuref interval 6-24` is now a validated cadence probe;
  `imuref off` restores the production six-unit interval and clears the experiment target.
- A coherent software-generated `0x28` solution remains valid only after exact carrier
  projection/rounding is modeled with the already-decoded multi-sample IMU/temperature data.
  All chart states, both cyclic sign branches, and five prefix seam choices now have direct
  evidence.

## NFC boundary

- Config mode accepts strictly validated 540/572-byte NTAG215 and 2048-byte
  NTAG I2C Plus 2K images through transactional 32-byte chunks.
  Virtual Amiibo is always available: a blank store presents no virtual tag, while a loaded image
  automatically owns the virtual source. The adapter's internal baseline/latest-written recovery
  pair remains implementation-only; the portal does not expose reset-to-original behavior.
- The production portal library remains available without an adapter connection. It accepts one
  file or recursively scans a selected directory and caches one record per AmiiboAPI catalog ID
  with one mutable dump in browser-local IndexedDB. v3 rider/machine variants use content-derived
  keys so distinct combinations sharing one catalog identity remain available without duplicate
  copies of the same dump. The shared catalog is downloaded and stored once, then matched
  locally, so original tag bytes/IDs/UIDs/save data never leave the browser.
- The production carousel starts empty and displays only user-imported records. Directory scans
  add those records to the visible library progressively. AmiiboAPI order remains authoritative;
  each compact filter chip cycles `All`, then only the imported library's available values
  alphabetically.
  Centered artwork stays fixed in the middle at 100%; four non-overlapping neighbors on each side
  are exactly 80/60/40/20%. Selection changes animate without rebuilding the track, and carousel
  names are omitted. Mouse wheel/trackpad while hovered, horizontal touch/pen swipe, and keyboard
  arrows navigate; visible arrow buttons and the position counter are intentionally absent. The
  three filter chips sit together below the compact primary action.
- **Sync amiibo** retrieves the latest active image, validates its raw tag identity, overwrites the
  loaded or UID-matching cached entry, and acknowledges firmware dirty state only after IndexedDB
  confirms the write. The AmiiboAPI catalog enriches the record but never gates it. Browser
  security does not permit silently overwriting the original OS file.
- `web/diagnostic.html` and `tools/run_amiibo_portal_test.ps1` provide the same library/metadata
  workflow plus a browser-only simulated adapter, transactional upload, controlled write,
  persistence, cache-first save-back, and self-test without Web Serial or hardware.
- Both portals use an artwork carousel. The production manager shows centered save facts below the
  artwork; clicking the centered amiibo toggles a non-modal context drawer with friendly catalog
  metadata, compatible games, and secondary/destructive actions.
  `tools/run_config_portal.ps1` serves the real portal from the same localhost origin.
- The board stores exactly one amiibo (the alternating flash banks are persistence generations,
  not two active amiibo). The manager is single-slot: connected **Load amiibo** uploads the
  highlighted entry; offline **Select amiibo** remembers it. A conditional **Import amiibo** or
  **Sync amiibo** action pulls an unknown or console-written adapter image into IndexedDB. The
  merged button is always labeled **Eject amiibo**; `amiiboEjectActionState()` and its tooltip
  choose between loaded-pointer-only removal, confirmed adapter wipe, or both. Cancel aborts
  everything; library dumps are never deleted; the console-driven Stop/write-back lifecycle and
  `amiibo present` re-activation are unchanged.
- Virtual Amiibo library is **import-only** (single file or recursive directory of the user's own
  genuine dumps). Amiibo crypto is enforced (2026-07-26 hardware test,
  [[amiibo-identity-and-generation]] / `docs/experiments/generated-amiibo-console-rejection-2026-07-26.md`):
  the Switch 2 rejects key-free generated images ("This isn't an amiibo"). Random Mode was removed
  (UID-bound HMAC). A key-based identity generator was removed for import-only simplicity. The
  narrower browser-local Initialize operation remains: it requests the user's own
  `key_retail.bin`, clears/re-signs an imported ordinary or v3 dump, self-verifies, and works
  without an adapter. Alternate-UID copies are feasible only through a complete keyed
  re-sign/re-encrypt path and are not implemented.
- Library export/import is a flat **.zip** (`library.json` manifest + one `.bin` per amiibo) via a
  self-contained store-only ZIP writer/reader (`amiiboZipStore`/`amiiboZipEntries`, deflate fallback
  via DecompressionStream); import reconstructs from the `.bin` files and legacy `.json` still
  imports. Import is structural-only: `coerceAmiiboImport()` accepts 540/572 and larger
  emulator-container dumps. Exact 2048-byte figure-v3 images remain whole with their contiguous
  UID layout; 540/572-byte NTAG215 inputs retain their native layout and BCC checks. The AmiiboAPI
  catalog is enhancement-only and never gates import, so brand-new amiibo not yet in AmiiboAPI
  import fine.
- Carousel navigation wraps at both ends (`moveAmiiboCarousel`) while the visible neighbor window
  uses real non-wrapping indices for clean slides; the centered amiibo's release date shows above
  it. Compact search remains because the owned library exceeds 900 entries. Game series, Amiibo
  series, and product type are tap-to-cycle chips. One center slot conditionally shows
  Load/Select/Import/Sync; connection status follows the adapter's active cached entry once without
  hijacking later browsing. Download/Initialize/Eject/Delete and compatible-game details live in
  the centered item's inline drawer. The **Scan physical amiibo** control stays hidden until
  firmware exposes a capability-reported Config-transport API; UART `nfc_probe` alone is not a
  production contract.
- Kirby Air Riders "Figure Player" amiibo (**v3** = NTAG I2C Plus 2K, 2048 bytes) are ✅
  **READ/WRITE CONFIRMED on hardware as of 2026-07-28**. Authoritative detail is
  `docs/Amiibo-v3.md` §19 and
  `docs/experiments/v3-full-sram-response-validation-2026-07-28.md`. An untouched downloaded
  Kirby/Warp dump completed the full sequence with no signature override: three device results,
  targeted read, six encrypted `0x14` write chunks, `0x08` commit, `05 00`, and zero write errors.
- Layout, re-measured 2026-07-27 by diffing all 16 dumps: **rider identity is in the encrypted body;
  machine identity is entirely in the SRAM block `0x3C0..0x3FF`, outside amiibo crypto.** A rider's
  four machine variants are byte-identical except for 21–22 bytes there, and all four share one UID
  (they are one physical figure reconfigured four times). Machine fields = an ASCII code
  (`"PB4W717"` Warp/Winged, `"PB5T432"` Shadow, `"PC6V628"` Tank) plus an `01 01 0X` byte; the
  12-byte blob at `0x3C2` is per-unit.
- The v3 write path is hardware-confirmed. Capture-derived classification keeps the 74-byte
  `01 01` device command on the `0x21` path while `01 06` starts the six-chunk, 454-byte data
  transaction completed by `0x08`. The full stored SRAM response is preserved across writes.
  Owner/format write, Stop/eject, next-scan readback, HMAC-valid export, flash persistence, and
  power-cycle recovery have passed; only production-portal Sync of the retained dirty generation
  remains.
- A genuine Pro Controller 2 plus physical Kirby & Warp Star positive control captured the missing
  Air Riders protocol. `0x20` accepts two sector-aware record envelopes: 355 bytes clears
  sector-0 pages `0x92..0xE1`; 167 bytes updates page 4, sector-0 pages `0x92..0x99`, and
  sector-1 pages `0x01..0x18`. Record count is at byte 22 and records are
  `(sector,page,length,data)`. Genuine completion is bare ACK followed by empty status `0x16`,
  selected-UID page-3 read, then the ordinary 454-byte/`0x08` write. The post-write physical
  snapshot exactly matches the 32-byte sector-0 record. Firmware now implements both shapes,
  generation-safe journaling without intermediate ejection, and `0x16`; 53 host tests, both board
  builds, magnetometer tests, and install markers pass. The prepared build's ordinary System
  Settings read/write control passed with zero write errors and a valid exported image. Later
  bullets record the completed Air Riders lifecycle and its follow-up corrections.
- The apparent initial-read regression on that prepared build was an invalid test image, not code:
  historical `v3-write-output-2026-07-28.bin` calculates SRAM CRC `7AC4` but stores `E511`.
  Replacing it over UART with corrected capture-rebuilt CRC32 `8D337603` restored read/write
  immediately without a reflash.
- The first prepared-build Air Riders run reached the 355-byte clear, `0x16`, page-3 read, six
  ordinary chunks, `0x08`, and `05 00` with zero errors, then failed `2115-0096`. Exact trace
  comparison showed why: genuine hardware reports the same tag present about 130 ms after the
  first Stop so the console can send the 167-byte stage; PicoSwitch2 returned cooldown `07 41`.
  The next build suppresses auto-eject only for this clear-to-update checkpoint, expires abandoned
  sequences after five seconds, and restores normal eject after the update checkpoint. Host tests
  cover timing and wrap.
- That lifecycle build completed the full Air Riders operation on hardware. Diagnostics ended at
  18 ordinary chunks, three `0x08` commits, eight extended chunks, two `0x20` completions, and zero
  write errors. The exported 2048-byte image is HMAC-valid, keeps SRAM CRC `7AC4/7AC4`, and
  contains the captured sector-0 and sector-1 game data. Primary files:
  `dumps/amiibo/v3-extended-two-stage-success-reuse-freeze-2026-07-28.jsonl` and
  `dumps/amiibo/v3-extended-two-stage-success-output-2026-07-28.bin`.
- Reusing that written image then froze because virtual NFC did not stage subcommand `0x1E`.
  Genuine capture
  `dumps/amiibo/genuine-kirby-warp-reuse-sub1e-usb-2026-07-28.jsonl` proves `0x1E` itself returns
  a bare ACK, then changes to empty state `0x15` and exposes a 196-byte result through `0x15`
  chunks. The result is a 64-byte prefix plus sector-0 pages `0x92..0x99` and sector-1 pages
  `0x00..0x18`. Sector-1 page 0 read as chip metadata `A5 00 01 00` after the first Air Riders
  update even though portable dumps leave that slot zero. The prepared build reproduced all 196
  bytes in a host fixture and passed
  53 host tests, both board builds, eight magnetometer tests, and both reset-marker checks.
  Hardware then validated the entire `0x1E` path: state `0x15`, three chunks, and Stop.
- After the validated reuse read, Air Riders immediately sent another 167-byte update. Trace
  `dumps/amiibo/v3-reuse-sub1e-write-freeze-2026-07-28.jsonl` first exposed
  `A5 00 02 00`; a temporary page-4 comparison let the update complete, but its persisted result
  was called corrupted on the next `0x1E` read.
- Decisive positive controls:
  `dumps/amiibo/genuine-air-riders-existing-data-read-write-2026-07-28.jsonl` is a complete
  genuine read/write cycle, and
  `dumps/amiibo/genuine-air-riders-postwrite-read-only-2026-07-28.jsonl` reads that same physical
  tag afterward without writing. A second pair,
  `genuine-air-riders-second-existing-data-write-2026-07-28.jsonl` and
  `genuine-air-riders-second-postwrite-read-only-2026-07-28.jsonl`, repeats the result.
  Together they prove the header is the next chip-managed sector-1 page-0 value, not a page-4
  echo: genuine `0x1E` advanced `A5 00 01 00 → A5 00 02 00 → A5 00 03 00`, while page 4
  independently advanced `03 → 04 → 05` and the explicit sector-1 record still began at page 1.
  The source
  validates/retains that implicit state at image offset `0x400` and serves it dynamically;
  zero-filled ecosystem images retain the generation-1 fallback. Both board builds, all 53 host
  tests, all eight magnetometer tests, and both install-reset markers pass. Hardware then completed
  a virtual update to sector-1 page 0 `A5 00 02 00`; the exported 2048-byte image was HMAC-valid
  and retained the customized figure state. Its immediate second reuse was accepted by Air Riders,
  which loaded the saved custom color. Captures:
  `dumps/amiibo/v3-dynamic-sector1-page0-write-2026-07-28.jsonl` and
  `dumps/amiibo/v3-dynamic-sector1-page0-second-read-success-2026-07-28.jsonl`. A physical
  adapter power cycle restored the exact generation-4 image with CRC `91A6178B`; the clean
  read-only trace
  `dumps/amiibo/v3-dynamic-sector1-page0-powercycle-read-success-2026-07-28.jsonl` again served
  `A5 00 02 00`, and Air Riders accepted it. The dynamic-state persistence lifecycle is fully
  hardware-confirmed.
- A non-cosmetic save after completing an Air Riders level is captured in
  `dumps/amiibo/v3-air-riders-learned-state-save-2026-07-28.jsonl`, with exact before/after images.
  It uses the same 167-byte extended update plus ordinary six-chunk commit, advances generation
  7 → 9, page 4 `A5 00 05 00 → A5 00 06 00`, and sector-1 page 0
  `A5 00 03 00 → A5 00 04 00`. The 552 changed bytes are confined to the modeled ranges:
  423 in `0x000..0x247`, 32 in `0x248..0x267`, one generation byte at `0x402`, and all
  96 bytes at `0x404..0x463`; nothing after `0x463` changed. The output is HMAC-valid, persisted,
  dirty, and recorded zero write errors. No new command or storage shape is needed.
- King Dedede & Tank Star proves Air Riders storage is allocation-relative. It uses sector-0 page
  `0xB2` and sector-1 capability/data pages `0x64/0x65`; Kirby uses `0x92` and `0x00/0x01`.
  The header-only fix accepted all three chunks, but its two `0x20` completions still failed
  because commit validation retained Kirby's pages. The prepared codec derives pages from the
  record envelope, bounds sector 0 to the proven clear window and sector 1 to the tag, tracks
  capability generation at the selected page, and makes `0x1E` fallback descriptor-relative.
  No UID/product table exists. All 16 available Air Riders v3 dumps completed both reads and
  writes on a real Switch 2. Both board builds, all 53 host tests, portal suites,
  motion/magprobe checks, and both install markers pass. Evidence:
  `docs/experiments/v3-air-riders-dynamic-allocation-2026-07-28.md`.
- ⚠️ The "provenance" conclusion (that a v3 image needs its own machine's SRAM block and signature)
  is **RETRACTED** — see `docs/Amiibo-v3.md` §18.1a. That experiment moved three variables at once;
  the downloaded dump was never retried after `prefix[18]=0x06` moved into `ns2_v3_build_buffer()`,
  which alone explains every earlier rejection. **Retail keys are ruled out as the discriminator:**
  all 16 downloaded dumps *and* the accepted rebuilt image verify HMAC-VALID
  (`node tools/verify_amiibo_crypto.mjs <path>`), and the firmware serves each image's own UID via
  `ns2_amiibo_v3_uid()`, so the UID/key binding is never broken.
- The signature/carrier hypothesis is refuted. The successful downloaded image used
  `signature_set=false`; `key_retail.bin` was needed only for offline evidence verification. The
  actual blocker was truncating the 64-byte SRAM response to 32 bytes and forcing the captured
  figure's CRC `7A C4`. The firmware now carries all of `image[0x3C0..0x3FF]`; downloaded
  Kirby/Warp ends `E5 11`, Meta/Shadow ends `30 61`. `amiibo v3sig` remains diagnostic only.
- Corrected capture-rebuilt baseline: `dumps/kirby-warpstar-rebuilt-from-genuine.bin`
  (CRC32 `8D337603`, UID `049011CADB1F90`, SRAM CRC `7A C4`).
- `amiibo status` now reports the active v3 image's 2048-byte size, contiguous UID, generation,
  dirty/persisted state, and payload CRC. Config and UART reads route to the v3 store, and the
  portal uses UID plus CRC to identify the exact rider/machine variant. Sync replaces the old
  content-keyed IndexedDB record only after the complete updated image validates.
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
  work. All 53 host tests, three firmware build axes, and portal static checks pass; Config BLE
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
  of the complete versioned library backup. The current one-dump/single-loaded-pointer manager is
  host/static-regression clean; its manual Eject/Present path still awaits real-console validation.
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

1. Run production-portal **Sync amiibo** against the currently retained dirty v3 generation and
   confirm acknowledgement occurs only after IndexedDB persistence.
2. Hardware-validate the implemented manual Eject/Present path, including replacement and reconnect.
3. Capture a genuine Pro2 physical-tag write/readback before enabling native writes.
4. Resolve exact carrier projection/rounding before attempting the accepted
   software-generated `0x28` path; all four chart states and both cyclic sign branches now have
   adjacent hardware evidence.
5. Add DualSense microphone return only after preserving the confirmed speaker/haptic path.
6. Extend motion translation to another controller family only after verifying its calibration,
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
