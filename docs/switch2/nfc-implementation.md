# NFC / amiibo implementation design

> Current design and feasibility decision. Protocol evidence is inventoried in
> [`nfc-protocol-inventory.md`](nfc-protocol-inventory.md); the reproducible resource and source
> audit is in
> [`nfc-feasibility-audit-2026-07-25.md`](../experiments/nfc-feasibility-audit-2026-07-25.md).
>
> Status: **both a genuine Pro Controller 2 physical-tag read through the UART-gated relay and a
> non-NFC-controller Virtual Amiibo read are recognized by a real Switch 2. A Virtual Amiibo write
> reaches complete staging, commit, and accepted completion status on hardware without crashing.
> Logical removal, next-scan re-presentation, same-session updated readback, and validated UART
> export, automatic dual-bank persistence, power-cycle recovery, internal write recovery,
> offline library use, and full-library backup restore are hardware/browser-confirmed. Native
> physical writes remain open. The 2048-byte figure-v3 read/write/persist path is also
> hardware-confirmed with an untouched downloaded dump; only its production-portal Sync check
> remains.**

## 1. Decision

NFC is no longer blocked by an inability to observe the console. The headered Pico 2 W can remain
attached to the Switch while UART reaches the PC, and the Bluetooth host can exchange native
commands with a paired Nintendo controller. The old design that assumed NFC could not work through
the dongle is archived at
[`nfc-implementation-through-2026-07-12.archived.md`](../archive/nfc-implementation-through-2026-07-12.archived.md).

Treat the requested feature as three related implementations:

| Source controller | Intended behavior | Feasibility now | Remaining gate |
|---|---|---|---|
| DualSense, Xbox, generic, or any source without NFC | Serve one user-supplied 540- or 572-byte virtual amiibo; accept console writes and persist/download the modified image | **Complete read/write/persist/eject/re-present/library lifecycle hardware-confirmed** | Explicit manual present/remove controls |
| Any source, 2048-byte figure-v3 image loaded | Serve one NTAG I2C Plus 2K virtual amiibo and preserve console writes | **Read/write/persist hardware-confirmed with an untouched downloaded dump** | Production-portal Sync of the retained dirty generation |
| Pro Controller 2 | Relay the controller's real NFC reader and physical tag | **Read hardware-confirmed through the diagnostic bridge** | Validate production gating/reconnect and capture a physical write |
| Joy-Con 2 Right | Relay the controller's real NFC reader and physical tag | **Likely, not assumed byte-identical** | Repeat the read/write capture with this controller |
| Switch 1 Pro Controller / Joy-Con Right | Use the real Switch 1 NFC reader through a Switch 2-facing personality | **Possible through translation, not byte passthrough** | Implement and capture the Switch 1 MCU reader/writer state machine, then map it to the Switch 2 command family |
| Joy-Con Left / Joy-Con 2 Left | No reader exists on that half | Use virtual-tag mode only | None beyond the virtual implementation |
| Unknown third-party controller claiming NFC | Do not assume capability from name alone | Per-profile only | Verified identity, report path, and a successful read/write capture |

The virtual path should be implemented first. It is the smallest, best-supported increment and also
provides the tag store and Switch 2 command codec that both native-reader paths will reuse.

## 2. Protocol surface to implement

Switch 2 uses top-level command `0x01`. The current firmware only implements the genuine init-time
`0x0C` response (`61 12 50 10`) and capture-confirmed bare-ack shape for other subcommands.

The current structured model is:

| Subcommand | Working interpretation | Evidence level |
|---|---|---|
| `0x01` | Init/reset-like operation; bare ack on the genuine Pro2 USB capture | This repository's capture |
| `0x03` | Enter scan mode | Primary Pro2 relay capture |
| `0x04` | Leave scan mode/eject | Primary Pro2 relay capture |
| `0x05` | Return 61-byte status including present/operation state and 7-byte UID | Primary Pro2 relay capture |
| `0x06` | Begin read/write; captured zero UID selects read, exact selected UID selects write | Read is primary; write selection is strong external evidence |
| `0x08` | Atomically commit a complete staged write | External capture-derived implementation; host-tested here |
| `0x0C` | Return four opaque controller/NFC bytes | Genuine Pro2 capture; already implemented |
| `0x14` | Receive offset-addressed chunks of a 454-byte write image | External capture-derived implementation plus ndeadly example; host-tested here |
| `0x15` | Return read-buffer data | Primary capture: two-byte offset and `[last][length:u16le][data]` on USB and BLE |

Do not equate the report NFC-state byte with the status payload. The report field is a small
event counter that advances modulo eight; command `0x05` returns a separate status/detail pair
such as present `09 00`, active `04 00`, committed `05 00`, or absent/error `07 41`.

## 3. Virtual-tag format and state

Accept three input formats:

- **540 bytes:** raw NTAG215 image.
- **572 bytes:** the 540-byte raw image followed by the 32-byte NTAG originality signature.
- **2048 bytes:** one complete NTAG I2C Plus 2K figure-v3 image with a contiguous seven-byte UID.

For a 540-byte upload, do not silently claim the missing signature is genuine. The first
implementation may either:

1. require the 572-byte extended form for console use; or
2. allow a documented compatibility signature only behind an explicit UI warning.

The safer default is option 1 until hardware proves the console accepts the fallback used by the
external reference.

Validate an NTAG215 image before accepting it:

- exact length;
- UID extraction from raw bytes `0,1,2,4,5,6,7`;
- BCC0 at byte 3: `0x88 ^ uid[0] ^ uid[1] ^ uid[2]`;
- BCC1 at byte 8: `raw[4] ^ raw[5] ^ raw[6] ^ raw[7]`.

For a 2048-byte v3 image, the UID is contiguous at bytes `0..6`; bytes `7..8` are `00 44`.
The complete machine response is stored at `0x3C0..0x3FF`, and its final two bytes are the
big-endian CRC-16/MCRF4XX over the preceding 62 bytes.

The raw image does include machine-readable identity but not friendly product names. Bytes
`0x54`–`0x5B` form the eight-byte amiibo ID; bytes `0x5C`–`0x5F` optionally distinguish variants.
The portal can display those values, the seven-byte UID, raw product type, model, series, and format
codes offline. Human-readable character, game series, amiibo series, product name, artwork, and
release date require a catalog. The portal downloads AmiiboAPI's public catalog once, caches it,
and performs exact ID matching locally; it never sends the dump, model ID, UID, or save data.

No Nintendo retail keys are needed. The dongle serves and stores the raw encrypted bytes; the
console owns interpretation, authentication, and re-encryption. Generating, editing, decrypting, or
forging tag data is outside this feature.

### Save-data and library model

Keep each browser/phone as its own user-supplied amiibo library and the adapter as one active tag
identity. The browser stores one mutable validated dump per tag identity and one loaded-amiibo
pointer. Known NTAG215 tags normally use their catalog identity; unknown/new tags remain usable
without an AmiiboAPI entry. Distinct 2048-byte v3 rider/machine combinations use content-derived
keys because several combinations share one catalog identity.

The adapter's validated journal still retains an internal imported baseline and latest
console-written image so an interrupted browser sync cannot destroy data. That implementation
detail is not exposed as a reset feature. **Sync amiibo** reads the latest active image, validates
its raw identity, commits it to the loaded or UID-matching IndexedDB record, and only then
acknowledges the adapter's dirty protection. AmiiboAPI metadata is optional enrichment, never a
validation gate. The browser's one stored dump is overwritten. Users remain responsible for
formatting or erasing amiibo data through the console.

Nintendo documents that a physical amiibo can hold read/write data for one compatible game at a
time.
`emuiibo` can exceed that physical limit because it stores application areas as separate per-game
files, but doing so requires parsing/decrypting the tag model and is unnecessary here. PicoSwitch2
instead preserves the entire encrypted 540-byte image losslessly, so any console write—including
the active game's save area, write counter, dates, and ownership data—returns in the downloaded
file without retail keys.

The relevant `emuiibo` user-interface lesson is separate selected-tag and present/removed state.
PicoSwitch2's offline portal remembers one loaded library tag without requiring an adapter.
Virtual Amiibo itself is always available; an empty store simply presents no virtual tag. The
Config-only BLE transport makes the portal reachable while the dongle remains physically attached
to the console, although entering Config temporarily replaces the controller USB personality.

The board stores exactly one amiibo; the browser exposes one loaded-amiibo pointer. Connected
**Load amiibo** (between the carousel arrows) uploads/presents the highlighted image immediately;
offline **Select amiibo** only remembers it. A conditional **Import amiibo** or **Sync amiibo**
action retrieves an unknown or console-written adapter image and commits it to the validated
browser record. Removal uses one button always labeled **Eject amiibo**. Its tooltip and
confirmation reflect the scope selected by `amiiboEjectActionState()`: remove only the local
pointer, wipe the adapter, or do both. Adapter wipes clear presentation and both flash journal
banks; cancelling a confirmation aborts everything, and library dumps are never deleted. The
console-driven Stop/write-back lifecycle is unchanged; re-presenting a retained identity still
uses the lightweight `amiibo present` path without reprogramming flash.

The amiibo keeps its stored identity and UID; console writes are saved to the used copy. A
short-lived "Random Mode" that swapped in a random UID per scan was removed after a 2026-07-26
hardware test showed the Switch 2 validates the UID-bound cryptography — a runtime UID change
invalidates the tag HMAC, which cannot be recomputed without retail keys. That test also refuted
key-free image generation. See
[`amiibo-identity-and-generation.md`](amiibo-identity-and-generation.md) and
[`../experiments/generated-amiibo-console-rejection-2026-07-26.md`](../experiments/generated-amiibo-console-rejection-2026-07-26.md).

References:

- [Nintendo: one read/write game's data per physical amiibo](https://www.nintendo.com/au/support/articles/how-much-game-data-can-i-store-on-an-amiibo-at-any-one-time/)
- [`emuiibo` selected-tag, application-area, and connected/removed model](https://github.com/xortroll/emuiibo/blob/28b357d5ce4aa373891c5294127f79137e0917ff/README.md)
- [TagMo raw amiibo ID extraction at offset `0x54`](https://github.com/HiddenRamblings/TagMo/blob/9a09bfde9fa4689868ba31f90c7dba098b20d952/app/src/main/java/com/hiddenramblings/tagmo/amiibo/tagdata/AmiiboData.kt)
- [AmiiboAPI ID, variant, and catalog fields](https://amiiboapi.org/docs/)
- [MDN `showDirectoryPicker()`](https://developer.mozilla.org/en-US/docs/Web/API/Window/showDirectoryPicker)

The runtime state machine must be event-driven:

```text
inactive
  └─ 0x03 scan ─> scanning
       ├─ no active tag ─> absent status
       └─ active tag ─> present status + UID
            ├─ 0x06 read ─> build 600-byte buffer ─> serve 70-byte offset chunks
            └─ 0x06 write ─> receive 0x14 chunks ─> 0x08 commit ─> dirty
  └─ 0x04 leave ─> inactive
```

It must do no tag polling, hashing, flash work, or formatting while NFC is idle.

## 4. USB transport requirement

The generic partial-write-safe vendor-IN pump remains implemented and host-tested, but the
successful read did not require a 630-byte transfer. A full `0x15` chunk response is 81 bytes
(eight-byte envelope, three-byte chunk header, 70 data bytes), which fits the existing 128-byte
TinyUSB FIFO. The final response is 51 bytes.

The confirmed reader buffer is exactly 600 bytes:

- 60 bytes of fixed/UID/signature/operation metadata;
- 540 raw NTAG215 bytes.

The console requests offsets `0x0000, 0x0046, ... 0x0230`. Responses contain
`[last:u8][length:u16le][data]`; the first eight carry 70 bytes and the final response carries 40.

Console writes add an independent OUT-direction framing requirement. A normal `0x14` request is
88 bytes (eight-byte envelope plus an 80-byte payload) and crosses the 64-byte USB packet
boundary. The first hardware write attempt proved that dispatching each `tud_vendor_read` result
as one command is unsafe: the Switch crashed with `2168-0002`. `ns2_vendor_rx` now reads the
big-endian payload length at header bytes 4–5 and dispatches only the complete reassembled
command. A repeated write hardware-confirmed that transport fix and reached accepted `05 00`.

The completion trace then showed Stop followed by one-second scan/status/Stop cycles because a
browser-loaded image and a physically presented tag were represented by one flag. They are now
separate. After a committed Stop, the runtime waits for the automatic flash snapshot to verify,
then emits a removal edge and stops presenting the tag. The latest written image remains selected
internally and dirty for browser save-back. The next `0x03` scan presents that same updated image as a fresh tag
encounter. The portal additionally exposes manual Eject; its store-level presentation edge resets
the core-0 NFC transaction runtime before the next Present, preventing stale scan/write state from
leaking across the action. Manual Eject is intentionally a presentation action, not deletion. See
[`virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md`](../experiments/virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md).

Because console testing occupies the Pico USB port, `read_uart_diag.ps1 -Command 'amiibo dump'
-OutputPath FILE.bin` can export the live dirty image over the independent UART link. It reads
bounded 64-byte chunks against one generation, validates size and UID/BCC on the PC, writes the
file, and only then acknowledges dirty state. Console writes are now persisted automatically, so
this export is a portable copy rather than the only protection against power loss.

## 5. Native Switch 2 reader relay

The UART-gated bridge now proves the read architecture:

- command writes use extended characteristic `0x0016` with the genuine 33-byte zero prefix;
- `0x001E`/CCC `0x001F` is subscribed without disturbing existing traffic;
- matching ordinary NFC replies were observed on primary response handle `0x001A`;
- USB/BLE envelopes are translated asynchronously without blocking the USB task;
- the genuine report-state byte is reflected to the console.

The relay therefore needs an asynchronous adapter:

The bridge has a bounded cross-core response slot and timeout. It is still diagnostic/UART-gated:
production auto-selection, disconnect/removal behavior, physical writes, and Joy-Con 2 Right must
be validated separately.

## 6. Native Switch 1 reader translation

Switch 1 NFC uses a different controller-MCU protocol:

- select input report `0x31`;
- enable/configure the NFC/IR MCU with subcommands `0x22` and `0x21`;
- send MCU requests in output report `0x11`;
- receive extended input report `0x31`, including CRC8, sequence, status, UID, and chunked tag data.

The current Switch 1 Bluetooth driver selects report `0x30`, parses only `0x30`/`0x3F`, and has no
MCU ownership layer. Directly forwarding Switch 2 command `0x01` bytes cannot work.

Existing public sources are enough to bootstrap physical NTAG detection and reading, but not enough
to call full read/write translation validated:

- `jc_toolkit` has an MIT-licensed physical reader sequence and NTAG213/215/216 chunk assembly.
- historical JoyControl/Poohl work documents virtual Switch 1 read/write behavior, but its
  implementation is GPL and some earlier NFC code was removed over source-provenance concerns.
- this repository has no primary capture of a physical Switch 1 reader write.

Implement this only after the Switch 2 virtual path is confirmed. The adapter should expose a
transport-neutral tag source (`present`, UID, 540 raw bytes, 32-byte signature, `write_pages`) so
the Switch 2 command codec never depends on Switch 1 report framing.

## 7. Config-mode web portal

Configuration mode provides one local page over USB Web Serial or the Config-only Web Bluetooth
transport, with:

- a `.bin` file picker using `File.arrayBuffer()`;
- a read-only `showDirectoryPicker()` path that scans subdirectories recursively;
- mutable browser-local IndexedDB dumps keyed by validated tag identity (and content for distinct
  v3 combinations), with one loaded pointer, search, and explicit clear; the visible library starts
  empty and fills only from validated imported files, progressively during a directory scan;
- an artwork carousel that centers and enlarges the selected or active image, shows four
  progressively smaller neighbors on each side, animates selection changes, and supports click,
  button, and arrow-key navigation;
- AmiiboAPI source order by default, with game-series, amiibo-series, and product-type arrows that
  cycle `All` followed by the imported library's available field values alphabetically and never
  upload private tag identity;
- a compact friendly detail card containing Name, Character, Game series, Amiibo series, and
  Product type, enriched from the optional browser-cached AmiiboAPI catalog;
- size/BCC validation in both the browser and firmware;
- offset-addressed 32-byte hex chunks with declared total and whole-image CRC32;
- an always-available virtual source whose blank state presents no tag;
- a single-loaded-pointer layout with connected Load/offline Select, Eject, and
  adapter-to-browser Import/Sync;
- **Sync Amiibo from Adapter**, which retrieves the latest active image and overwrites the matching
  validated cached entry so application-area writes survive library switching;
- versioned flat-ZIP export/import of the complete mutable library and loaded pointer (legacy JSON
  backups remain importable);
- explicit save/persist operation.

The library manager remains enabled without a serial connection. The directory handle is used only
for the user-initiated scan. The browser caches copied bytes, not write access to the original
folder, so no source file can be changed silently. Chromium browsers provide the native directory
picker used here; single-file import remains the fallback.

The CDC parser currently accepts 127 payload characters per line. Do not expand it to hold a whole
base64 dump. Use small binary-as-hex chunks that fit the existing line buffer, or add a bounded
binary framing mode with exact length and CRC. A failed/interrupted upload must leave the previous
tag intact.

## 8. RAM, flash, and CPU budget

Measured after the two-save persistence integration in the standard release build directories on
2026-07-25:

| Measurement | Pico 2 W | Pico W |
|---|---:|---:|
| firmware `.bin` | 903,544 bytes | 773,588 bytes |
| `.data` | 128,292 bytes | 7,332 bytes |
| `.bss` | 179,848 bytes | 112,612 bytes |
| fixed-section gap before scratch X | 216,144 bytes | 144,244 bytes |

Removing the embedded 96,768-byte FAT image, MSC callbacks, and TinyUSB MSC allocation reduced the
Pico 2 W binary by **100,104 bytes**, the Pico W binary by **100,160 bytes**, and `.bss` on each
board by **576 bytes**. The portal is now entirely local; its future growth does not consume device
flash or RAM.

A compact single-tag implementation needs approximately:

| Item | Bytes |
|---|---:|
| internal imported baseline + signature | 572 |
| optional latest console-written image | 540 |
| command snapshot raw tag + signature | 572 |
| write staging | 454 |
| write coverage bitmap | 57 |
| operation metadata/state | less than 64 |
| virtual read buffer | 600 |
| one maximum chunk response (stack) | 73 |
| static flash-program buffer | 1,280 |
| **Conservative resident total** | **about 4.1 KiB** |

This is well within both measured boards. NFC must not accidentally pull the Pico 2 W audio/Opus
profile into the Pico W build.

Persistent tag storage must not be added to `pico_config_t`; that structure is deliberately limited
to one 256-byte flash page. Flash allocation is currently:

- sector `-5`: virtual amiibo bank 1;
- sector `-4`: settings;
- sector `-2` and `-1`: BTstack TLV/bonds;
- sector `-3`: virtual amiibo bank 0 and read-only version-1 migration source.

Each version-2 bank uses one 1,280-byte snapshot with magic, version, length, generation,
header/payload CRC, flags, baseline/latest-written images, and optional signature. Snapshots
alternate banks.
The previous valid snapshot remains untouched until the destination sector is erased, programmed,
and verified. A newly flashed UF2 erases both banks before the first normal startup.

The current flash writer parks core 0, which would interrupt USB, audio, and input. Therefore:

- apply console writes immediately to RAM;
- update/select the internal latest-written image and mark it dirty;
- expose/download the changed image immediately;
- queue flash erase/program on the existing core1 service point;
- defer the post-write TagRemoved edge until the new snapshot verifies;
- show the user whether the latest write is persisted or still pending.

CPU cost can be effectively zero when idle. An active transaction requires bounded copies,
validation, CRC/BCC checks, and USB/BLE state transitions only; it needs no cryptography and no
clock increase beyond the validated 300 MHz. No continuous NFC timer or polling loop is allowed.

## 9. Implementation order and gates

1. **Transport/test foundation** — complete
   - generic partial-write-safe vendor-IN pump;
   - transport-neutral tag codec and strict 540/572 validation;
   - host tests for read payload, write staging, malformed offsets, duplicate/conflicting chunks,
     interruption, and transfer pumping.
2. **Virtual read** — hardware-confirmed
   - config upload/activate/eject/download;
   - loaded-tag command `0x01` state machine; blank state falls through to native/no-tag behavior;
   - exact 600-byte reader buffer and 70-byte offset chunks, recognized by a real Switch 2;
   - no flash mutation during reads.
3. **Virtual write** — complete lifecycle hardware-confirmed
   - exact-UID write selection and 64-byte preparation buffer;
   - bounded 454-byte staging with duplicate/conflict coverage;
   - atomic page-record validation and generation-safe RAM update;
   - automatic snapshot request after the RAM commit;
   - dirty/download workflow;
   - real Switch 2 staging/commit/completion validated without a crash;
   - post-write logical removal, next-scan updated readback, and UART export validated;
   - automatic post-write persistence and power-cycle recovery hardware-confirmed.
4. **Safe persistence** — hardware-confirmed
   - alternating dedicated sectors `-3`/`-5`;
   - verified write-before-eject snapshot;
   - internal baseline/latest-written images plus recovery selection;
   - version-1 migration;
   - power-loss, live-console timing, and selection recovery validated.
5. **Figure-v3 virtual read/write** — hardware-confirmed; production-portal Sync pending
   - separate captured `01 01` device-command and `01 06` mutable-data transactions;
   - 83-byte device result = 19-byte controller header + all 64 stored SRAM bytes, including the
     response-specific CRC-16/MCRF4XX;
   - six offset-addressed chunks fill one 454-byte coverage-checked envelope;
   - three records plus the captured four-byte header field update the mutable image while
     preserving UID, extended data, and SRAM;
   - atomic generation-checked update, dirty/readback status, alternating-bank snapshot,
     700 ms completion edge, and persistence-gated logical removal;
   - untouched downloaded Kirby/Warp completed read, six-chunk write, `0x08`, `05 00`, Stop, and
     HMAC-valid persisted export with zero write errors;
   - host coverage for the full-SRAM response, successful captured write layout, retry/conflict,
     incomplete, UID mismatch, protected/out-of-range records, and trailing data.
6. **Native Pro2/Joy-Con 2 Right**
   - Pro2 physical read captured and recognized;
   - asynchronous USB↔BLE command adapter implemented behind UART;
   - production gating, Joy-Con 2 Right, reconnect/removal, and physical write validation pending.
7. **Switch 1 Pro/Joy-Con Right**
   - physical-reader MCU layer;
   - transport-neutral translation;
   - read and write validation.

Do not ship “full NFC” until virtual read, virtual write-back, power-cycle persistence, malformed
upload recovery, controller reconnect, audio, motion, input, rumble, LED, BOOTSEL, and wake
regressions all pass. Native reader support should be advertised per verified controller profile,
not as a generic Nintendo capability.
