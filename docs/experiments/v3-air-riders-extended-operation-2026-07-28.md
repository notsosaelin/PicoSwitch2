# Figure-v3 Air Riders extended operation — 2026-07-28

## Question

Why does a validated Meta Knight & Shadow Star virtual amiibo read successfully, but Kirby Air
Riders locks when it writes game-specific data?

## Primary capture

- `dumps/v3-kirby-air-riders-game-write-lock-2026-07-28.jsonl`
- `dumps/v3-meta-shadow-after-game-write-lock-2026-07-28.bin`

The game was Kirby Air Riders. The selected virtual figure was Meta Knight & Shadow Star.

## Observation — confirmed

After the ordinary targeted read, the console sent five command-`0x14` bodies:

| Offset | Declared bytes |
|---:|---:|
| 0 | 76 |
| 76 | 76 |
| 152 | 76 |
| 228 | 76 |
| 304 | 51 |

They form one 355-byte buffer. It begins:

```text
88 13 04 98 8B 22 AB 1F 90 01 06 00 00 00 00 00
00 00 00 00 00 00 02 00 92 F0 ...
```

The only later nonzero bytes are `CE 50` at offsets 267–268. The console terminates this operation
with empty subcommand `0x20`, not the normal mutable-image `0x08`.

Before the attempt:

```text
dev_cmd_staged=13  dev_results=13
write_chunks=12    write_commits=2    write_errors=10
```

After the attempt:

```text
dev_cmd_staged=14  dev_results=14
write_chunks=12    write_commits=2    write_errors=15
```

Every game chunk was rejected by the old normal-write gate. The next console status read returned
`07 41`, then Stop. No stored image byte changed.

## Interpretation

**High confidence:** bytes 0–1 are a little-endian timeout (`0x1388` = 5000 ms), not a fixed
`D0 07` marker. The earlier read-descriptor investigation independently established the same
field meaning.

**High confidence:** this is not the 454-byte owner/format record transaction. It has zero record
count, a 355-byte extent, descriptor `02 00 92 F0`, and terminator `0x20`.

**Moderate confidence:** it is the controller transport for the
[Switch 2 NFC service's extended application area](https://www.switchbrew.org/wiki/NFC_services).
The public API and Air Riders-only timing fit, but no public source currently documents this exact
controller-wire payload.

## Gated experiment

The firmware now has a separate classifier and staging mode. It accepts only the complete captured
shape, verifies selected UID and gap-free 355-byte coverage, and acknowledges `0x20` with `05 00`.
It deliberately does not mutate or persist the tag. UART `amiibo v3diag` exposes
`extended_chunks` and `extended_completions`.

Expected discriminator:

- `extended_chunks += 5`, `extended_completions += 1`, and no new `write_errors`;
- if the game proceeds, capture its next request before implementing persistence;
- if the game still rejects, capture the returned status and immediate next command;
- generation, dirty state, and the exported 2048-byte image must remain unchanged in either case.

## First hardware result

Capture: `dumps/v3-air-riders-extended-noop-2115-0088-2026-07-28.jsonl`.

The classifier and completion were accepted by the firmware with no errors. The console repeated
the transaction three times:

```text
extended_chunks=15
extended_completions=3
write_errors=0
```

Every `0x20` was followed by status `05 00`, then Stop. The first post-Stop scan received `09 00`
with the same UID because the diagnostic completion had not entered the committed-write removal
lifecycle. After three identical attempts the game reported `2115-0088`.

The image remained generation 2 with payload CRC `F0D17070`, confirming the test was non-mutating.

## Second hardware result — removal fixed, persistence still missing

Capture: `dumps/v3-air-riders-tagremoved-2115-0096-2026-07-28.jsonl`.

The corrected lifecycle worked. After each successful `0x20`, the adapter returned `05 00`; after
Stop, the next scan during the cooldown received absent status `07 41`. There was no Switch crash
and no write-classifier error:

```text
extended_chunks=15
extended_completions=3
write_errors=0
```

The console nevertheless waited for the retained tag to become available, read it again, and
repeated the same 355-byte operation three times. Because this diagnostic path still does not
mutate the 2048-byte image, its generation and payload CRC did not change across the operation.
The game now ended with `2115-0096` rather than `2115-0088`.

This confirms the three-second TagRemoved window and rules out another release-timing change as the
next useful experiment. The new result is consistent with a post-write persistence/readback check:
the console accepts the transport completion and removal edge, then observes that the extended
application data was not saved.

## Genuine successful transaction — decisive result

The positive control used a genuine Pro Controller 2 and a physical Kirby & Warp Star amiibo:

- complete console-facing trace:
  `dumps/amiibo/genuine-kirby-warp-air-riders-write-usb-2026-07-28.jsonl`;
- genuine-controller BLE trace:
  `dumps/amiibo/genuine-kirby-warp-air-riders-write-ble-2026-07-28.jsonl`;
- packed pre-write snapshot: `dumps/amiibo/Kirby.bin`;
- packed post-write snapshot:
  `dumps/amiibo/genuine-kirby-warp-after-air-riders-2026-07-28.bin`.

The game completed normally. The USB trace retained all 224 NFC records with zero overwrite, so it
is the primary protocol evidence; the smaller BLE ring dropped 121 records and is corroborating
evidence only.

### `0x20` completion semantics

The bare response was already correct:

```text
01 04 <id> 20 00 F8 00 00
```

The next status is **not** `05 00`. Genuine hardware reports state `0x16` with all remaining 60
payload bytes zero. It then accepts a selected-UID `0x06` descriptor for page 3, reports normal
active state `0x04`, returns the 60-byte operation prefix plus page 3, and receives the ordinary
454-byte/six-chunk write completed by `0x08`. Only that ordinary commit reaches `05 00` and Stop.

This complete sequence occurs twice in the successful operation.

### The bodies are sector-aware record envelopes

Both envelopes place the record count at byte 22. Each record is:

```text
sector:u8, page:u8, length:u8, data[length]
```

The first envelope is 355 bytes and carries two records:

| Sector | Page | Length | Captured data |
|---:|---:|---:|---|
| `0` | `0x92` | `0xF0` | all zero |
| `0` | `0xCE` | `0x50` | all zero |

Together they clear sector-0 offsets `0x248..0x387`, ending immediately before v3 configuration
page `0xE2`.

The second envelope is 167 bytes and carries three records:

| Sector | Page | Length | Meaning confirmed by capture |
|---:|---:|---:|---|
| `0` | `0x04` | `0x04` | capability-container tail; physical tag preserves read-only `A5 00` |
| `0` | `0x92` | `0x20` | sector-0 Air Riders data at `0x248..0x267` |
| `1` | `0x01` | `0x60` | sector-1 data at `0x404..0x463` |

The post-write physical snapshot changed page 4 from `A5 00 02 00` to `A5 00 03 00`. Pages
`0x92..0x99` changed from zero to:

```text
C2 A5 82 33 16 18 65 AF 18 EC 8C 1F 7C 0F 5F 8F
44 4E 4F E2 1B C0 96 8B 3E FD 0A 47 BF DC B1 AF
```

Those 32 bytes are byte-identical to the second record's data. The current 8-bit probe descriptor
cannot observe sector 1, but the complete console command directly supplies its sector/page/length
mapping.

### Implementation prepared

The firmware now:

- classifies the 355-byte clear and 167-byte update independently;
- requires exact size, gap-free staging, UID match, captured header, record layout, and zero
  trailing padding;
- applies the records atomically, preserving page 4's read-only `A5 00`;
- generation-checks and journals each `0x20` stage without ejecting;
- reports genuine empty state `0x16`;
- leaves the following page-3 read, ordinary `0x08` commit, `05 00`, and Stop/eject lifecycle
  unchanged.

All 53 host tests, both board builds, the magnetometer checks, and both install-reset-marker checks
pass. Real-console validation of this prepared build is still pending.

### Prepared-build ordinary read/write control

Before exercising Air Riders, the prepared build was checked against the ordinary System Settings
amiibo read/write flow:

- trace: `dumps/amiibo/v3-extended-build-valid-read-write-control-2026-07-28.jsonl`;
- input: corrected capture-rebuilt image, CRC32 `8D337603`, UID `049011CADB1F90`;
- result: System Settings completed read and write;
- trace retention: 212 records, zero overwritten;
- diagnostics: six normal-write chunks, one `0x08` commit, zero write errors, and zero extended
  chunks/completions;
- export: `dumps/amiibo/v3-extended-build-valid-read-write-output-2026-07-28.bin`, CRC32
  `56CD52C0`, HMAC valid, SRAM CRC calculated/stored `7AC4/7AC4`.

An immediately preceding rejection was not a firmware regression. The adapter had been loaded with
the historical `dumps/v3-write-output-2026-07-28.bin` artifact. Its amiibo HMACs are valid, but its
SRAM body calculates CRC `7AC4` while the stored trailer is `E511`; the console correctly rejected
that inconsistent device result before sending any extended-write command. Loading the corrected
`8D337603` image restored read/write without another firmware flash.

### First prepared-build Air Riders result — `2115-0096`

Capture: `dumps/amiibo/v3-extended-persist-2115-0096-2026-07-28.jsonl`.

The trace retained all 206 records with zero overwrite. The implementation correctly:

- accepted and committed all five chunks of the 355-byte clear;
- reported empty state `0x16`;
- accepted the selected-UID page-3 read;
- accepted all six chunks of the following ordinary write;
- completed `0x08`, reported `05 00`, and persisted generation 6;
- recorded zero write errors.

The exported image,
`dumps/amiibo/v3-extended-persist-2115-0096-output-2026-07-28.bin`, is HMAC-valid,
retains SRAM CRC `7AC4/7AC4`, and has the expected cleared extended window. The console did not send
the 167-byte update.

The failure is the inter-stage removal edge. On genuine hardware, after the first ordinary
`0x08`/`05 00`/Stop, the console starts the second transaction about 130 ms later and the same tag
reports present state `09 00`. PicoSwitch2 instead applied its ordinary three-second auto-eject and
answered that scan with `07 41`. The console therefore never issued the 167-byte stage and returned
`2115-0096`.

The next build keeps the tag presented only across the clear-stage ordinary checkpoint. A bounded
five-second sequence state prevents an abandoned clear from affecting a later unrelated write.
Once the 167-byte update and its ordinary checkpoint complete, the established auto-eject behavior
applies normally. Host tests cover clear/update selection, exact timeout, and `uint32_t` time wrap;
all 53 host tests, both board builds, magnetometer checks, and install-reset markers pass.

### Complete two-stage write succeeds; reuse isolates `0x1E`

The bounded inter-stage lifecycle worked on real hardware. Primary capture:
`dumps/amiibo/v3-extended-two-stage-success-reuse-freeze-2026-07-28.jsonl`.
The ring retained the final 256 records after 606 older records were overwritten, but UART
diagnostics and the persisted image provide unambiguous completion evidence:

```text
write_chunks:          18
write_commits:          3
write_errors:           0
extended_chunks:        8
extended_completions:   2
```

Eight extended chunks are exactly the five-chunk 355-byte clear plus the three-chunk 167-byte
update. The corresponding
`dumps/amiibo/v3-extended-two-stage-success-output-2026-07-28.bin` is 2048 bytes, HMAC-valid,
retains SRAM CRC `7AC4/7AC4`, stores the expected sector-0 `0x92..0x99` data, and persists the
96-byte sector-1 page-1 record. The game accepted the write.

Attempting to use the written virtual tag again then stalled. The retained trace shows the console
repeatedly sent:

```text
01 91 00 1E 00 17 00 00
D0 07 04 90 11 CA DB 1F 90 01 02
00 92 99
01 00 18
00 00 00 00 00 00
```

The virtual path returned the same bare ACK as other unknown commands but did not change state.
The console waited exactly three seconds, sent Stop, restarted discovery, and repeated.

### Genuine reuse capture decodes the missing state machine

With the same already-written physical Kirby & Warp Star tag and a genuine Pro Controller 2,
`dumps/amiibo/genuine-kirby-warp-reuse-sub1e-usb-2026-07-28.jsonl` captured all 88 NFC records
with zero overwrite.

The immediate `0x1E` response is still the bare ACK:

```text
01 04 00 1E 00 F8 00 00
```

The important behavior is internal. Genuine hardware:

1. changes NFC status from `0x18` to `0x15` and signals a report-state edge;
2. reports `0x15` with the remaining 60 status bytes zero;
3. serves a 196-byte operation buffer through offsets `0x0000`, `0x0046`, and `0x008C`;
4. marks the final 56-byte chunk complete, then accepts Stop.

The operation buffer is:

- 64-byte prefix: result type `0x15`, UID, chip type `0x06`, 32-byte originality signature, and
  the 13-byte sector descriptor copied from request byte 10;
- 32 bytes from sector 0 pages `0x92..0x99`;
- 100 bytes from sector 1 pages `0x00..0x18`.

Sector 1 page 0 is `A5 00 01 00` on genuine hardware. Portable 2048-byte ecosystem dumps leave
that slot zero and begin the game-written record at page 1, so this is modeled as read-only chip
metadata in the response rather than written back into the user's `.bin`.

The prepared implementation validates the UID, tag type, two sector ranges, bounds, and six zero
reserved bytes; stages the exact 64-byte prefix and requested pages; reports empty state `0x15`;
and reuses the existing bounded `0x15` chunk transport. A host fixture reproduces every one of the
196 genuine bytes. Both board builds, all 53 host tests, all eight magnetometer tests, and both
install-reset markers pass.

### Virtual reuse validates `0x1E` and exposes a dynamic update header

Hardware capture:
`dumps/amiibo/v3-reuse-sub1e-write-freeze-2026-07-28.jsonl`.

The prepared sector read behaved correctly:

- `0x1E` returned a bare ACK;
- status changed to empty `0x15`;
- the console fetched offsets `0x0000`, `0x0046`, and `0x008C`;
- chunk lengths were 70, 70, and 56 bytes with the final bit set;
- the returned data contained the expected `A5 00 01 00` synthetic sector-1 page;
- the console sent Stop rather than waiting or retrying.

This hardware-validates the entire `0x1E` implementation.

Air Riders then started a fresh selected-UID page-3 read and sent the 167-byte update directly,
without another 355-byte clear. Its header differed from the first-use capture in one meaningful
field:

```text
first use: A5 00 01 00
reuse:     A5 00 02 00
```

The first correction treated those bytes as a page-4 echo because they happened to equal the
loaded virtual image's page 4. That accepted the update and all later transport/persistence
steps, but its next read was rejected as “This amiibo is corrupted.” Trace:
`dumps/amiibo/v3-second-reuse-corrupted-2026-07-28.jsonl`.

### Genuine full-cycle and post-write read isolate implicit page 0

The decisive A/B uses:

- `dumps/amiibo/genuine-air-riders-existing-data-read-write-2026-07-28.jsonl`;
- `dumps/amiibo/genuine-air-riders-postwrite-read-only-2026-07-28.jsonl`;
- virtual successful update
  `dumps/amiibo/v3-reuse-dynamic-header-result-2026-07-28.jsonl`;
- virtual rejected next read
  `dumps/amiibo/v3-second-reuse-corrupted-2026-07-28.jsonl`.

The genuine and virtual update records contain identical new sector-0 and sector-1 game bytes.
Genuine hardware, however, performs one additional implicit transition:

| State | Sector-0 page 4 | Sector-1 page 0 |
|---|---|---|
| Genuine before update | `A5 00 03 00` | `A5 00 01 00` |
| 167-byte envelope targets | record tail `00 00 04 00` | header `A5 00 02 00` |
| Genuine after update | `A5 00 04 00` | `A5 00 02 00` |
| Virtual failed image/read | `A5 00 03 00` | synthesized `A5 00 01 00` |

The explicit record list begins at sector-1 page 1, so the header is the next chip-managed
sector-1 page-0 value. It is not a fixed marker and not current page 4.

A second genuine save confirms the rule:

| Cycle | Pre-write page 4 | Pre-write sector-1 page 0 | Envelope header | Page-4 record | Post-write sector-1 page 0 |
|---|---|---|---|---|---|
| First observed reuse | `A5 00 03 00` | `A5 00 01 00` | `A5 00 02 00` | `00 00 04 00` | `A5 00 02 00` |
| Second observed reuse | `A5 00 04 00` | `A5 00 02 00` | `A5 00 03 00` | `00 00 05 00` | `A5 00 03 00` |

The second write is
`dumps/amiibo/genuine-air-riders-second-existing-data-write-2026-07-28.jsonl`; its post-write
read-only control is
`dumps/amiibo/genuine-air-riders-second-postwrite-read-only-2026-07-28.jsonl`. Both retained every
record with zero overwrite.

The prepared implementation retains that header at image offset `0x400`, validates one-step
advancement, and serves it dynamically in `0x1E`. This makes the implicit hardware state durable
through the existing flash journal and portal Sync/export without another sidecar allocation.
Zero-filled ecosystem source images retain the observed generation-1 fallback until their first
extended update supplies an explicit value. Both firmware builds, all 53 compiled host tests,
all eight magnetometer tests, and both install-reset-marker checks pass.

### Virtual dynamic-state correction succeeds

Hardware captures:

- `dumps/amiibo/v3-dynamic-sector1-page0-write-2026-07-28.jsonl`;
- `dumps/amiibo/v3-dynamic-sector1-page0-write-output-2026-07-28.bin`;
- `dumps/amiibo/v3-dynamic-sector1-page0-second-read-success-2026-07-28.jsonl`.

The adapter began with a known-good first-use image whose ecosystem sector-1 page-0 slot was zero.
Air Riders completed the next save with three device results, six ordinary write chunks, one
ordinary commit, six extended chunks, one extended completion, and zero write errors. The
persisted/exported image reports page 4 `A5 00 03 00` and sector-1 page 0 `A5 00 02 00`; both
amiibo HMACs remain valid, and the saved nickname/owner remain intact.

Without re-uploading the image, the next scan was accepted. Air Riders loaded the custom color
stored during the earlier save. The read-only trace contains the retained `A5 00 02 00` in the
sector-1 page-0 position of the `0x1E` result. This is the positive A/B for the correction and
closes the “This amiibo is corrupted” second-reuse failure.

### Power-cycle persistence control succeeds

The adapter was physically power-cycled without reflashing. Before scanning, UART reported the
same 2048-byte generation-4 image, payload CRC `91A6178B`, `persisted:true`, and no pending write.
Air Riders then accepted and loaded the saved figure state.

Capture:
`dumps/amiibo/v3-dynamic-sector1-page0-powercycle-read-success-2026-07-28.jsonl`.
It contains 86 NFC records with zero overwrite. The operation staged one v3 device result, no
write chunks, no ordinary or extended commits, and returned the retained sector-1 page-0 value
`A5 00 02 00` in the `0x1E` data at sequence 79. Post-read status still reported generation 4,
CRC `91A6178B`, clean and persisted. This isolates successful flash recovery from the earlier
live-RAM second-reuse result.

### Learned gameplay-state write control

After completing an Air Riders level, the figure was saved with its newly learned gameplay state.
This separates meaningful game training data from the earlier nickname/color/hat changes.

Artifacts:

- `dumps/amiibo/v3-air-riders-trained-before-save-2026-07-28.bin`;
- `dumps/amiibo/v3-air-riders-learned-state-save-2026-07-28.jsonl`;
- `dumps/amiibo/v3-air-riders-learned-state-after-save-2026-07-28.bin`.

The trace contains 152 NFC records with zero overwrite. Diagnostics advanced by one device
transaction, three extended chunks, one extended completion, six ordinary chunks, one ordinary
commit, and zero errors. Store generation advanced 7 → 9 and remained dirty/persisted so the same
image can be used for the production-portal Sync test.

The extended header advances sector-1 page 0 from `A5 00 03 00` to `A5 00 04 00`; its page-4
record advances `A5 00 05 00` to `A5 00 06 00`. The before/after images differ in 552 bytes:
423 within `0x000..0x247`, 32 within sector-0 `0x248..0x267`, one generation byte at `0x402`,
and all 96 sector-1 game bytes at `0x404..0x463`. No byte after `0x463` changes. The resulting
2048-byte image has payload CRC `3DEE59FF`, preserves nickname `K Kirbo` and owner `Miles`, and
passes both amiibo HMAC checks.

Conclusion: non-cosmetic learned state uses the already-implemented reuse transaction and storage
map. It does not expose a new command, record shape, or memory region.
