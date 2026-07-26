# Virtual amiibo offline foundation — 2026-07-25

## Scope

Build the parts of Virtual Amiibo that can be validated without a dongle, controller, or live
Switch 2 NFC transaction. Do not change production command `0x01` behavior.

## Inputs

- Current PicoSwitch2 command and flash architecture.
- The 2026-07-25 NFC feasibility/protocol audits.
- `emuiibo` commit `28b357d5ce4aa373891c5294127f79137e0917ff` for user-facing selected-tag,
  connected/removed, and save-data semantics—not Switch 2 controller framing.
- A maintainer-provided collection containing 1,035 540-byte `.bin` files.

## Collection validation

All 1,035 `.bin` files had exact 540-byte length and valid NTAG UID cascade BCC0/BCC1 fields.
There were 1,035 distinct seven-byte UID values (and 1,021 distinct shorter UID prefixes in the
initial inventory). No collection file contained the optional
32-byte NTAG `READ_SIG` suffix.

This supports accepting ordinary 540-byte dumps in the portal. It does not prove that the Switch 2
accepts a fabricated originality signature, so runtime console reads remain gated.

## Implemented

- One mutable 540/572-byte tag with transactional, out-of-order, idempotent chunk upload.
- Browser and firmware length/BCC validation plus whole-upload CRC32.
- Dirty-state replacement protection and lossless same-format download.
- Dedicated sector `-3` journal with five append slots, generation ordering, and header/payload
  CRCs. Flash work runs only from the existing core1 config-save service.
- Cleaned Web Serial portal with NFC controls in the main view and research diagnostics collapsed.
- Recursive read-only directory scanning plus single-file import. All valid images are copied into
  browser-local IndexedDB, searchable by filename/path and parsed ID/UID/type/model fields.
- The production and diagnostic selectors use a nine-position artwork carousel with the current
  selection centered, four neighbors on each side, active-tag marking, responsive narrowing,
  buttons/arrow keys, and the selected tag's full details.
- Offline raw identity display plus a cached full AmiiboAPI catalog for friendly details. Catalog
  entries are matched locally; tag bytes, ID, UID, and save data remain local. Downloading an active
  console-mutated image also updates its selected cached library entry.
- A separate `web/diagnostic.html` test harness requires no serial device. It reconstructs uploads
  through 32-byte chunks and CRC32, keeps its simulated adapter state in a separate IndexedDB,
  injects controlled RAM writes, exercises download/cache refresh, and includes a browser self-test
  plus a full-catalog/local-match AmiiboAPI request. `tools/run_amiibo_portal_test.ps1` serves it
  from localhost so storage has a stable origin.
- Partial-write-safe 630-byte vendor-IN queue/pump.
- Transport-neutral 61-byte status, 622-byte read, and atomic 454-byte staged-write codecs.

## Resource result

After implementation, before the later same-day CDC-only migration:

> Current firmware no longer embeds this FAT image. See
> [`config-cdc-only-migration-2026-07-25.md`](config-cdc-only-migration-2026-07-25.md) for the
> post-migration measurements.

| Measurement | Pico 2 W | Pico W |
|---|---:|---:|
| firmware `.bin` | 991,080 bytes | 860,372 bytes |
| `.data` | 128,284 bytes | 7,332 bytes |
| `.bss` | 176,116 bytes | 106,912 bytes |
| estimated fixed-section gap before scratch X | 217,564 bytes | 145,656 bytes |

The added BSS is approximately 1.8 KiB on each board, matching the feasibility estimate. No clock,
audio allocation, periodic NFC work, or controller path was added.

## Automated validation

- Embedded portal JavaScript parses successfully.
- Generated FAT12 config disk extracts byte-identically (`90,796`-byte HTML in a `96,768`-byte
  image).
- All 43 compiled `test_*.exe` host tests pass.
- Motion-specific host executables and all eight magnetometer-probe tests pass.
- Pico W and Pico 2 W release builds compile successfully.
- The diagnostic page has valid JavaScript, no missing/duplicate DOM references, no Web Serial
  calls, and serves successfully from localhost. The 946-entry AmiiboAPI catalog locally matches
  944 collection files. The 91 unmatched files all share ID `026A000100000502` and are the
  Happy Home Designer item set, rather than false misses among normal catalogued amiibo.

## Deliberate boundary

Production `0x01` dispatch is unchanged. The optional gate defaults off, and the console-facing
state machine will be connected only for a controlled real-Switch read/write test. Native Nintendo
reader relay remains blocked on a complete `0x001E` read/write capture.
