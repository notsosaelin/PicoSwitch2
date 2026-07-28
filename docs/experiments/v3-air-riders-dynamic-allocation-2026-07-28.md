# Figure-v3 dynamic Air Riders allocation — 2026-07-28

## Question

Why does an untouched King Dedede & Tank Star dump fail its first Kirby Air Riders write with
`2115-0096` when the same virtual NFC implementation accepts Kirby and Meta Knight?

## Test and evidence

- source figure: King Dedede & Tank Star
- UID: `0465B0228F2190`
- console/game: Switch 2 / Kirby Air Riders
- failed trace:
  `dumps/amiibo/v3-air-riders-king-dedede-tank-2115-0096-2026-07-28.jsonl`
- post-failure image:
  `dumps/amiibo/v3-air-riders-king-dedede-tank-after-2115-0096-2026-07-28.bin`
- first-fix retest:
  `dumps/amiibo/v3-dedede-fix1-2115-0096-2026-07-28.jsonl`
- first-fix post-failure image:
  `dumps/amiibo/v3-dedede-fix1-after-2115-0096-2026-07-28.bin`

The 256-record trace ring retained sequence 106 onward and overwrote 106 earlier records. It still
contains the complete failing 167-byte extended update at sequences 346–353.

The staged envelope begins:

```text
88 13 04 65 B0 22 8F 21 90 01 06 01 01 64 FF FF
FF FF A5 00 01 00 03 00 04 04 ...
```

The corresponding successful Kirby envelopes begin:

```text
88 13 <uid:7> 01 06 01 01 00 FF FF FF FF A5 00 <generation> 00 03 ...
```

The first trace established that byte 13 varies: Dedede sends `0x64`, while captured Kirby
operations send `0x00`. The initial analysis incorrectly treated it as opaque because the old
classifier rejected the envelope before its dynamic record addresses were compared.

## First failure mechanism

The firmware incorrectly classified the seven bytes at offsets 11–17 as one constant:

```text
01 01 00 FF FF FF FF
```

The valid Dedede operation therefore never entered extended-write staging. Its three `0x14`
fragments and `0x20` completion produced four write errors, status became `07 41`, and the console
reported `2115-0096`.

The post-failure image is still structurally sound: it is 2048 bytes, has UID
`0465B0228F2190`, and both amiibo HMACs validate with the owner's retail keys. This rules out
source-dump corruption as the cause of this failure.

## First-fix result: classification passed, commit still failed

The first fix removed only byte 13 from the fixed header comparison. On hardware, the update then
entered staging, but its `0x20` completion still failed:

```text
before: extended_chunks=0  extended_completions=0  write_errors=0
after:  extended_chunks=11 extended_completions=1  write_errors=2
```

The five clear chunks and their completion succeeded. The console then staged the three update
chunks twice; both update completions failed. The returned `07 41` was the fail-closed write-error
state, not the committed-write TagRemoved lifecycle.

The complete body exposes the real allocation difference:

| Field | Kirby | King Dedede |
|---|---:|---:|
| envelope byte 13 | `0x00` | `0x64` |
| sector-0 32-byte record page | `0x92` | `0xB2` |
| sector-1 capability page | `0x00` | `0x64` |
| sector-1 96-byte record page | `0x01` | `0x65` |

Byte 13 selects the capability page, and the sector-1 data record begins at the following page.
The former commit codec still required Kirby's fixed pages `0x92` and `0x01`, explaining the
second failure exactly.

## Generalized fix

The runtime now derives allocation from the validated self-describing envelope, with no UID,
character, product, or known-dump table:

- selected-image UID and command/type must match;
- update prefix `01 01` and suffix `FF FF FF FF` remain exact;
- sector-0 application data remains bounded to the proven cleared window `0x92..0xE1`;
- sector-1 capability plus the following 96-byte record must fit inside the 2 KB image;
- the data page must equal capability page + 1;
- capability generation must advance exactly one at the selected page;
- record count, lengths, gap-free staging, and trailing-zero validation remain strict.

The `0x1E` reuse builder already copies console-selected ranges. Its generation-1 fallback is now
injected at the first page of the descriptor-selected 25-page sector-1 range instead of always at
page zero. The production portal's Initialize operation clears the complete second user-memory
sector, so it resets Kirby, Dedede, and unseen allocations without a rider list.

## Verification

- both Pico 2 W and Pico W firmware builds: pass
- all 53 compiled host-test executables: pass
- dedicated Pro2 and DualSense motion tests: pass
- all eight magnetometer probe tests: pass
- both install-reset markers: pass
- portal crypto/initialization, symbol, AmiiboAPI game-map, and all-16-dump compatibility tests:
  pass
- real Switch 2 read/write across all 16 available Air Riders v3 dumps: pass

## Confidence

- **High confidence:** both former failures are explained by fixed Kirby fields. The first rejected
  byte 13; the second accepted staging but rejected the different record pages at completion.
- **High confidence:** the safe generalized codec supports any figure following this captured
  three-record schema, independent of identity.
- **Hardware coverage:** all 16 available v3 dumps completed both reads and writes with the
  generalized build.
- **Not claimed:** compatibility with a future figure that introduces a new record count, record
  length, memory region, or controller protocol. Such a protocol change must fail closed and be
  added from a capture rather than guessed.
