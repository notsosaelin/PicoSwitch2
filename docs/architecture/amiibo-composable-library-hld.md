# Composable Virtual Amiibo library HLD

Status: **Proposed — research and design only; no implementation or new hardware validation in
this revision.**

Date: 2026-08-31

Scope: split known figure-v3 rider state from its selectable vehicle response, extract reusable
vehicle assets from user-owned dumps, make Amiibo acquisition a normal companion workflow, and
define a safe ordinary-amiibo UID-alias workflow that can preserve user save data.

Authorities: the current PicoSwitch2 implementation and retained hardware evidence remain
authoritative. [`DanTheMan827/AmiiboZero`](https://github.com/DanTheMan827/AmiiboZero/tree/c9fcb08a032320825e46f0c8958fa5ea53df50db)
is a behavioral/design reference, not Switch 2 protocol evidence and not a source-code dependency.

## 1. Decision

Implement both candidate features **in the local library/client layer**. Continue sending one
complete, validated tag image through the existing transactional adapter upload.

1. For known 2048-byte figure-v3 tags, represent a user-owned **rider snapshot** separately from
   reusable **vehicle response** assets. At load time, materialize a complete 2048-byte image by
   replacing only `0x3C0..0x3FF` with the selected 64-byte response.
2. For ordinary 540/572-byte NTAG215 tags, expose an explicit **Create UID alias** operation. It
   must create a new local backup by decrypting the latest source state with the user's own keys,
   replacing the UID and UID-derived fields, re-signing/re-encrypting, and self-verifying. It is not
   a runtime UID overlay and it is not initially available for figure-v3 tags.
3. Do not add firmware vehicle slots, firmware crypto, a new management verb, or another active-tag
   model for the first release. The firmware still owns exactly one materialized active image and
   its existing dual-bank persistence/dirty lifecycle.
4. Treat acquisition as a small capability-gated companion feature with user-facing entry points
   such as **Import file**, **Scan with this device**, and **Scan with connected controller**. File
   import is available now. Physical actions appear only for an attached provider that can return
   the requested format completely; ordinary-tag support does not imply figure-v3 support, and a
   diagnostic/partial capture never enters the library as a usable dump.
5. The stock Pico W/Pico 2 W adapter is a **bridge**, not an NFC reader. It has no NFC transceiver
   or antenna. Controller acquisition uses the genuine controller's NFC hardware; Android-local
   acquisition uses the phone/tablet NFC hardware. A future add-on or USB reader is a separate
   provider and is never inferred from the stock adapter.

This delivers the requested 4 riders × 4 vehicles selection without retaining 16 complete bins in
the product model, while keeping the hardware-validated controller-facing runtime unchanged.

## 2. What the evidence actually supports

### 2.1 Rider and vehicle separation — Confirmed for the retained Air Riders corpus

The current corpus tooling already established these byte-exact groups:

- 4 encrypted/body groups, one per rider;
- 4 SRAM groups, one per vehicle;
- 16 rider/vehicle pairs;
- one UID per rider, shared by that rider's four vehicle images.

The rider identity and mutable rider state are outside the 64-byte machine response. The vehicle
response is the complete v3 SRAM window at `0x3C0..0x3FF`. The runtime publishes all 64 bytes in
the `0x21` device result, and captured console writes preserve that window.

This also supports extraction from a user's own compatible dump. For example, importing a valid
2048-byte Kirby + Warp Star v3 image can retain that complete image as the rider snapshot and
losslessly copy its exact `0x3C0..0x3FF` bytes into a deduplicated Warp Star vehicle asset. That
asset may then be selected for another rider snapshot in the same evidence-backed Air Riders
composition profile. Extraction does not modify, shorten, normalize, or replace the source image.

The maintainer-provided 2026-08-31 Flipper bundle independently agrees with the retained images:

| Vehicle file | Size | Stored and calculated CRC-16/MCRF4XX | Local evidence match |
|---|---:|---:|---|
| `Warp Star.ext` | 64 | `E5 11` | exact match to the retained Warp Star response |
| `Winged Star.ext` | 64 | `BB 21` | exact match to the retained Winged Star response |
| `Tank Star.ext` | 64 | `25 63` | exact match to the retained Tank Star response |
| `Shadow Star.ext` | 64 | `30 61` | exact match to the retained Shadow Star response |

No friendly-name schema inside those 64 bytes is established here. The bytes remain opaque; a
vehicle's display name comes from user/import metadata, not speculative byte decoding.

Vehicle model identity is not exact asset identity. The genuine captured Warp Star response and
the supplied Warp Star response have different bytes and CRCs while representing the same named
vehicle. The library therefore deduplicates only byte-identical 64-byte responses (content hash),
allows multiple assets with the same friendly/model name, and retains every provenance edge.

AmiiboZero uses the same conceptual split: a saved v3 rider/tag plus a separate 64-byte lock-on
response, with **Change lock-on** replacing only the response. Its current README explicitly says
that this does not regenerate rider data or discard game-side rider writes. This is supporting
design evidence, not a substitute for the PicoSwitch2 corpus and console captures; AmiiboZero's
own documentation also says its v3 path still needs relevant real-hardware verification.

### 2.2 Random UID while retaining data — Partly correct, with important limits

The user's premise is correct for the operation AmiiboZero actually performs on **ordinary
NTAG215** images:

1. stop RF/emulation;
2. synchronize the latest reader-written state;
3. authenticate and decrypt;
4. replace UID/BCC and UID-derived tag fields;
5. derive new keys, recompute both HMACs, and re-encrypt;
6. save the rewritten state, then resume emulation.

That preserves the decrypted owner, nickname, application data, dates, and write state unless the
operation deliberately changes them. It is materially different from PicoSwitch2's rejected old
Random Mode, which changed only the served UID and therefore invalidated UID-bound cryptography.

The following claims are **not** established:

- A new UID does not universally bypass a game's timer. It helps only when that game keys the
  relevant limit solely on UID.
- Preserving the amiibo plaintext does not prove that a particular game's private application blob
  contains no UID-derived or anti-clone state of its own.
- A 572-byte dump's NTAG originality signature cannot honestly be reused for a different UID.
- A cryptographically coherent alternate-UID image has not yet been accepted by a real Switch 2 in
  this repository's controlled evidence.
- AmiiboZero deliberately disables v3 UID changes because changing v3 identity may make the rider
  unrecognizable to games. PicoSwitch2's in-memory v3 HMAC feasibility probe does not overrule that
  game-level risk.

Therefore the production-facing name is **UID alias**, not Random Mode or cooldown bypass.

### 2.3 Physical acquisition correction — Pro2 v3 completeness is proven

The prior design incorrectly promoted a limitation of one descriptor into a controller-wide
limitation. The corrected evidence boundary is:

- **OUR HARDWARE EVIDENCE:** complete normalized 2048-byte figure-v3 acquisitions already exist
  from a physical v3 amiibo read by a genuine Switch 2 Pro Controller through PicoSwitch2 and the
  UART/testing path, with host-side assembly;
- **OUR HARDWARE EVIDENCE:** the same controller path has produced a complete 540-byte ordinary
  NTAG215 acquisition;
- **REPOSITORY SOURCE EVIDENCE:** subcommand `0x06` uses sector-0-limited 8-bit page ranges;
- **REPOSITORY SOURCE EVIDENCE:** subcommand `0x1E` is a distinct sector-aware v3 operation whose
  captured request explicitly contains `(sector, start, end)` ranges for both sector 0 and sector
  1;
- **REPOSITORY SOURCE EVIDENCE:** the `0x1E` operation stages a larger result, and repeated `0x15`
  reads retrieve that result by offset in bounded chunks.

A complete 2048-byte acquisition is consequently allowed to be assembled from multiple
sector/page operations and staged-buffer reads. It does not mean one controller response carried
2048 contiguous bytes. Complete acquisition feasibility is **PROVEN IN OUR HARDWARE**; the exact
historical request grouping, maximum reliable range size, `0x15` offset progression, retry/timing
policy, and dynamic/session normalization recipe were not fully preserved and remain a research
gate before production orchestration.

The two facts must not be weakened into one another: a missing historical transcript does not make
complete Pro2 v3 acquisition unproven, and proven feasibility does not justify inventing the
production command recipe.

### 2.4 Companion-local NFC evidence

- **EXTERNAL SOURCE EVIDENCE:** Android `NfcA` implementations perform complete ordinary NTAG215
  reads and a complete v3 flow using v3 recognition, UID-derived `PWD_AUTH`, the SRAM
  request/write/read handshake, session-register polling, sector selection, sector-1 reads, and
  2048-byte normalization.
- **REPOSITORY SOURCE EVIDENCE:** PicoSwitch Android already has a strict ordinary NTAG215 path for
  complete 540-byte data plus an optional 32-byte originality signature; its documented physical
  phone gate remains separate from source/host correctness.
- **UNKNOWN for PicoSwitch product use:** Android v3 has not been implemented or physically
  validated in this companion. External feasibility does not permit routing v3 through the
  ordinary reader or advertising it before its own provider passes the acquisition ladder.

## 3. Current baseline that must remain intact

The design preserves these current contracts:

- the adapter stores and serves exactly one active image;
- accepted image sizes remain 540, 572, and 2048 bytes;
- upload remains `status -> begin -> chunks -> commit -> persist -> status`;
- an interrupted upload leaves the previous tag intact;
- unsynchronized dirty data blocks replacement/clear;
- adapter acknowledgement happens only after client-local sync succeeds;
- figure-v3 console writes remain generation-checked, fail-closed, and SRAM-preserving;
- key material stays in the browser/app key store and never enters adapter firmware;
- the web, Android, and Windows libraries remain archive-compatible;
- existing complete `.bin` imports remain valid and exportable.

This HLD evaluates physical-reader acquisition architecture, but does not implement or claim
product readiness for it. It does not reopen phone-RF figure-v3 behavior, application-area
virtualization, console-facing NFC wire semantics, or firmware flash allocation.

## 4. Domain model

### 4.1 Standard tag snapshot

No structural change is required for ordinary images:

```text
StandardTagSnapshot
  id                  local stable UUID
  displayName
  image               complete 540- or 572-byte image
  uid
  figureId
  crc32
  imported/updated
  dirtyFromAdapter
  derivedFrom          optional source snapshot UUID for a UID alias
```

Each UID alias is a separate snapshot. Two aliases can receive divergent console writes and must
never be silently merged.

### 4.2 Figure-v3 rider snapshot

```text
V3RiderSnapshot
  id                  local stable UUID; never derived from mutable bytes
  displayName
  baseImage           one complete, serveable 2048-byte image
  uid
  figureId
  crc32
  bodyFingerprint     SHA-256 over bytes excluding 0x3C0..0x3FF
  baseVehicleId       vehicle embedded in baseImage; recovery fallback
  selectedVehicleId   user's current local selection
  imported/updated
  dirtyFromAdapter
  sourceArtifactId    immutable original import/dump record
```

The base remains a complete 2048-byte image rather than a novel 1984-byte hole format. That gives
three safety properties:

- the rider is still a standalone backup if its metadata is damaged;
- an older client can still import a materialized `.bin`;
- local index loss cannot turn the only copy of user save data into an unusable fragment.

The embedded response is ignored during ordinary composition except as a recovery fallback.

### 4.3 Vehicle response

```text
V3VehicleResponse
  id                  SHA-256 of the exact 64 bytes
  displayName
  modelAliases[]      friendly/catalog names; never used as content identity
  response            64 opaque bytes
  crc16               stored big-endian bytes 62..63
  imported
  optional aliases    alternate friendly names for the same response
  compatibility       evidence-backed composition profile IDs
  provenance[]        one or more immutable source/extraction records
```

Vehicle assets are deduplicated globally by exact content. Two responses with different bytes are
different assets even when both are called Warp Star; alternate friendly/model names never collapse
them. They are not tied to a UID, rider, or catalog entry. Compatibility is scoped separately: the
initial UI offers these responses only for the known Air Riders v3 family, and future v3 families
require their own evidence rather than inheriting this behavior automatically.

Each provenance record contains at least acquisition kind, source artifact/snapshot ID, original
filename or reader identity, source SHA-256, extraction offset `0x3C0`, extraction length `64`, and
timestamp. If the same response is extracted from ten owned dumps, the library keeps one vehicle
asset with ten provenance edges rather than ten payload copies or one arbitrarily chosen source.

### 4.4 Immutable source artifact

```text
AmiiboSourceArtifact
  id                  local stable UUID for this acquisition event
  acquiredBytes       exact imported file or canonical reader output; immutable
  rawEvidenceRefs[]   optional immutable controller/NFC response fragments or capture reference
  sourceName          original filename or reader-generated name
  acquisitionKind     file | companion-nfc | controller-via-adapter | future-reader
  readerIdentity      optional stable backend/controller descriptor
  acquiredAt
  sha256
  canonicalSnapshotId optional validated library projection
```

The source artifact and the serveable library snapshot are deliberately separate. Current
ordinary-tag importers may repair derived NTAG215 BCC bytes on a clone; the original acquisition
must still remain byte-exact if provenance preservation is enabled. A v3 extraction always reads
from the accepted immutable 2048-byte acquisition and creates a separate 64-byte copy. No import,
normalization, extraction, composition, Sync, initialization, alias, or deduplication operation is
allowed to rewrite or delete the source artifact.

For a controller transaction assembled from several operations, `acquiredBytes` is the completed
canonical reader output while `rawEvidenceRefs` and the normalization record describe the fragments
that produced it. The product need not retain an unbounded packet trace, but it must retain enough
bounded evidence to distinguish directly acquired ranges from canonicalized positions.

### 4.5 Acquisition normalization record

```text
AmiiboNormalizationRecord
  sourceArtifactId
  providerId
  resultKind            complete-ordinary | complete-v3 | partial | unsupported
  physicalRanges[]      sector/page ranges actually acquired
  destinationRanges[]   mapping into the canonical image
  sramSource            direct range | device-result | unavailable
  canonicalizedRanges[] reserved/invalid fields and the rule applied
  dynamicRanges[]       session/dynamic fields and retained/canonicalized policy
  algorithmId           stable normalizer name and version
  completedAt
  completeNormalized    whether a validated canonical image was produced
  losslessPhysical      whether every meaningful source byte belongs to this exact tag
```

`completeNormalized` and `losslessPhysical` are independent. The historical donor-backed
Kirby/Warp Star reconstruction is structurally useful as a complete normalized image, but it is
not a wholly physical backup of that particular tag because uncovered bytes came from a matching
donor. It must not be conflated with the separate complete 2048-byte Pro2/UART acquisitions whose
feasibility is proven by hardware evidence.

### 4.6 Active selection

The local client records a tuple, not just a file key:

```text
ActiveAmiiboSelection
  kind                standard | v3-composite
  standardSnapshotId  when standard
  riderSnapshotId     when v3
  vehicleId           when v3
  materializedCrc32   exact bytes last uploaded
  adapterGeneration   generation observed after upload/sync
```

UID and figure ID alone are insufficient to recognize the active v3 combination. The adapter's
payload CRC plus the local tuple distinguishes the exact materialized image.

## 5. Figure-v3 composition

### 5.1 Vehicle discovery and extraction

Every accepted 2048-byte import or complete v3 scan runs non-mutating vehicle discovery after the
full source artifact and rider snapshot have been stored successfully. Discovery has two separate
answers:

1. **Structurally extractable:** the image is a recognized 2048-byte v3 image and the exact 64 bytes
   at `0x3C0..0x3FF` pass the observed response CRC-16/MCRF4XX check (stored big-endian at bytes
   `62..63`, calculated over bytes `0..61`).
2. **Composable in this product:** the figure maps to an evidence-backed compatibility profile
   whose captures establish that this window is a separable response and that other riders in that
   same profile accept it. Initially the only such profile is the retained Kirby Air Riders v3
   family.

The first answer permits lossless identification; the second controls whether the UI offers the
asset for reuse. A structurally valid but unknown v3 figure is still imported and preserved, but
its response is not promoted to a composable library asset merely because it occupies the same
offset. Compatibility comes from a versioned data/profile rule backed by corpus/capture evidence,
not a generic “all v3” assumption and not runtime behavior keyed on one rider UID.

When discovery succeeds, the companion offers **Extract vehicle** (or an equivalent import-time
checkbox). Accepting it performs this transaction:

```text
store and hash immutable 2048-byte source artifact
  -> store/locate complete rider snapshot
  -> copy exactly source[0x3C0..0x400] into a new 64-byte buffer
  -> verify length, stored CRC, and byte-for-byte copy
  -> vehicleId = SHA-256(exact 64 bytes)
  -> create vehicle asset, or reuse the identical existing asset
  -> append provenance edge to source artifact and rider snapshot
  -> commit asset/index metadata atomically
```

Declining extraction leaves the complete dump imported and unchanged; it can be extracted later.
Failure to validate the response also leaves the full dump imported, with extraction unavailable
and an explicit reason. The implementation must never “repair” a bad response CRC, zero-fill a
short source, or roll back/delete the complete image because optional extraction failed.

The first release therefore accepts vehicle assets only from:

- an exact 64-byte `.ext` response whose last two bytes equal CRC-16/MCRF4XX over bytes `0..61`;
  or
- the exact `0x3C0..0x3FF` copy from an accepted, supported-profile 2048-byte image.

Do not initially copy AmiiboZero's convenience acceptance of arbitrary 1–62 byte payloads. The
maintainer's files and PicoSwitch2 evidence are complete 64-byte responses; zero-padding partial
input would manufacture unobserved bytes. A later relaxation needs a documented format authority
and fixtures.

Import and extraction never mutate an existing rider, vehicle, source artifact, or user file. A
duplicate response reuses the content-addressed asset and appends provenance/optional name aliases;
it never silently overwrites prior metadata.

### 5.2 Materialization algorithm

```text
compose(rider, vehicle):
  validate rider.baseImage as a 2048-byte v3 image
  validate rider UID/figure ID against cached metadata
  validate vehicle length and stored CRC
  out = clone(rider.baseImage)
  out[0x3C0..0x3FF] = vehicle.response
  assert out outside 0x3C0..0x3FF equals rider.baseImage byte-for-byte
  assert UID and figure ID are unchanged
  assert v3 SRAM validation passes
  if retail keys are available:
      verify source and output decrypt to the same authenticated Amiibo payload
  return out
```

No re-signing is required because the vehicle response sits outside the established 520-byte
Amiibo crypto mapping. The optional key-backed comparison is a defense-in-depth assertion, not a
dependency for composing user-owned known-good dumps.

### 5.3 Load/switch transaction

Changing vehicle means replacing the adapter's complete active image, not mutating 64 flash bytes
in place:

1. read adapter status;
2. if dirty, require successful Sync or an explicit existing discard workflow;
3. materialize and validate the selected rider/vehicle pair locally;
4. use the existing full-image transactional upload;
5. request persistence and wait for authoritative completion;
6. re-read status and require matching UID, figure ID, size, and payload CRC;
7. update the active tuple only after success.

If any step fails, the selected rider/vehicle preference may remain local, but the UI must continue
to identify the previous adapter image as active.

### 5.4 Console write and Sync

The adapter returns one complete 2048-byte materialized image. Sync must split responsibility
without losing evidence:

1. download with the current generation/CRC guard;
2. validate v3 structure, UID, figure ID, and SRAM CRC;
3. require the downloaded SRAM fingerprint to match the active vehicle;
4. compare the downloaded image outside SRAM with the rider's previous base image and accept only
   changes allowed by the existing v3 runtime/write validator;
5. atomically replace the rider's base image with the downloaded complete image;
6. set `baseVehicleId` to the active vehicle and update rider metadata;
7. leave the shared vehicle asset byte-identical;
8. only then send `amiibo downloaded` and complete persistence acknowledgement.

An unexpected SRAM change is not silently adopted as a vehicle rename. Preserve the downloaded
bytes in a recovery/quarantine result, leave dirty acknowledgement unset, and report that the
adapter image no longer matches the selected composition. Current captured writes preserve SRAM,
so a difference means new behavior, another client changed the adapter, or corruption.

Rider snapshots with the same UID may have different saved states. Sync updates only the exact
locally active snapshot; it never fans writes out to every snapshot sharing that UID.

## 6. Existing 4 × 4 library migration

Migration must be lossless and non-destructive.

For each imported v3 image:

1. retain and hash the complete source artifact before deriving anything;
2. validate the complete image and discover its exact 64-byte response;
3. calculate `{figureId, uid, bodyFingerprint, vehicleFingerprint}`;
4. group images only when figure ID, UID, and every non-SRAM byte agree;
5. create one rider snapshot per exact body group;
6. create/reuse one vehicle asset per exact response only when its compatibility profile permits
   composition;
7. append source filename/hash/extraction provenance even when the vehicle payload is a duplicate;
8. retain any same-UID image with different non-SRAM bytes as a separate rider snapshot.

The migration transaction writes and verifies the new index/assets before changing the old index.
Original complete bins/source artifacts are retained permanently by the acquisition model unless
the user later invokes a separately designed, explicit source-artifact deletion workflow. Automatic
deletion is not part of migration. An optional later **Consolidate redundant combos** action may
remove only redundant *derived library projections* after a backup and explicit confirmation; it
must not remove the immutable provenance source.

For immediate compatibility, clients may first build this as a derived projection over today's
complete bins. That changes the UI from 16 combinations to 4 riders + 4 vehicles without forcing a
storage migration in the same release.

## 7. UID alias creation

### 7.1 Scope and UX contract

Initial support is restricted to standard NTAG215 images. The action is explicit and local:

> Create UID alias… — Make a new backup with a different tag UID while retaining the current
> decrypted owner and game data. Requires your own keys. The source backup is unchanged. Some
> games may still recognize or reject the alias; timer bypass is not guaranteed.

Do not rotate a UID automatically per presentation. Do not rewrite a library item in place. Do not
offer the action when keys are missing, source HMAC verification fails, the source is dirty on the
adapter, or the source is figure-v3.

### 7.2 Algorithm

```text
createUidAlias(source, retailKeys, random):
  require source is 540 or 572 bytes
  authenticate/decrypt source; abort on either HMAC failure
  snapshot the complete decrypted user/settings/application state
  draw a 7-byte RF UID with manufacturer byte 0x04
  reject collisions against every local UID; retry with a bounded count
  rebuild the raw UID/BCC representation
  replace every UID/key-derivation input defined by the format
  update UID-derived PWD/PACK fields according to canonical-image policy
  derive keys, recompute tag HMAC then data HMAC, and re-encrypt
  emit a 540-byte alias until originality-signature handling is validated
  decrypt the output again and require:
      both HMACs valid
      requested UID present
      figure ID unchanged
      decrypted mutable/user state byte-identical to the source
  import as a new snapshot with derivedFrom=source.id
```

The tag HMAC/data HMAC order is load-bearing and must reuse the existing initialized/re-sign path,
not introduce a fourth crypto implementation.

### 7.3 Originality signature policy

The trailing 32-byte `READ_SIG` value in a 572-byte source belongs to the original physical UID and
cannot be regenerated with `key_retail.bin`. Reusing it after UID replacement would be a false
claim and may fail verification.

Therefore the first experimental alias is a 540-byte canonical image with no claimed originality
signature. Shipping still requires a real-console A/B showing that this form is accepted on the
target read path. If later evidence shows a valid signature is required, UID aliases remain
unshippable; no compatibility signature should be invented.

### 7.4 Save-data meaning

“Retains save data” means the decrypted Amiibo settings and application area are byte-identical at
alias creation. After creation, source and alias are independent timelines:

- a console write to the alias updates only the alias;
- a console write to the source updates only the source;
- there is no automatic bidirectional save merge;
- creating a newer alias from the latest synchronized source is the safe way to carry newer state
  forward.

Game-private application data may still contain its own identity assumptions. The UI and release
notes must state this limitation until per-game evidence exists.

### 7.5 Figure-v3 UID policy

Figure-v3 aliases are deferred, not merely hidden:

- AmiiboZero rejects them deliberately;
- the rider/vehicle simplification needs no UID change;
- current PicoSwitch2 evidence proves only HMAC coherence in memory, not Air Riders recognition,
  load, save, or reuse with a changed UID;
- v3 has newer game-level identity behavior and dynamic extended allocations.

Reopening this requires a separate experiment with one changed variable and a preserved genuine
control. It is not an implementation follow-on implied by ordinary alias success.

## 8. Client and archive boundaries

### 8.1 Shared behavior first

The implementation starts with language-neutral fixtures for:

- vehicle extraction and CRC validation;
- composition and the “only 64 bytes changed” invariant;
- body and vehicle fingerprints;
- migration grouping and same-UID/different-state non-collapse;
- standard UID-alias input, requested UID, expected encrypted result, and decrypted-state equality;
- invalid key, invalid HMAC, UID collision, partial response, bad response CRC, and v3-alias refusal.

The current crypto fixture is the correct extension point. It contains no retail keys or personal
data; new vectors must preserve that property. Do not copy the maintainer's downloaded key or
vehicle files into fixtures without explicit provenance/licensing approval.

Web, Kotlin, and C# implementations must pass the same vectors. A format/archive change moves all
three clients together.

### 8.2 Archive v4

The current cross-platform archive v3 contains complete images only. A future v4 may add immutable
sources, vehicle assets, provenance edges, and selection metadata while retaining one complete
serveable `.bin` per rider:

```text
library.json
sources/<source-id>.bin      byte-exact acquired artifact
riders/<safe-name>.bin       complete current base image
vehicles/<safe-name>.ext     exact 64-byte response
```

Manifest additions identify item kind, stable local relationship, base vehicle, selected vehicle,
source-artifact/extraction provenance, compatibility profiles, and content hashes. Identical source
and rider bytes may share one manifest-addressed payload in a content-addressed implementation;
logical provenance records must still remain distinct. Existing bounds, path traversal defenses,
duplicate handling, and advisory manifest behavior remain.

Backward behavior is deliberate:

- v4-aware clients reconstruct the full model;
- older clients ignore an unsupported/advisory manifest and can still import the complete rider
  `.bin` files;
- older clients ignore `.ext` files and therefore retain one valid vehicle per rider rather than
  losing the rider entirely;
- older clients may see immutable source `.bin` files as additional valid backups; no source is
  hidden in a proprietary-only payload;
- v3 archives import as today and can be projected/migrated locally.

Do not bump the archive version until all three writers/readers and shared fixtures land together.

### 8.3 Firmware and management protocol

No peer-visible change is required for the composition, extraction, file-import, or UID-alias
releases. `amiibo status`, transactional upload, readback, dirty acknowledgement, present/eject,
persist, and clear remain unchanged.

Physical acquisition is a later exception: the high-level reader service in Phase D requires new
capability/session/result management surfaces. It must move the shared fixtures and all portable
clients together and remain capability-probed for older firmware.

A future 64-byte patch command would save transfer time but would also add a new wire contract,
new flash transaction semantics, and new dirty/generation races. Do not add it unless measured
full-image upload latency is a demonstrated product problem.

## 9. UI model

### 9.1 Figure-v3

The gallery shows one card per rider snapshot. Its inspector contains:

- rider identity and saved-data details;
- a **Vehicle** selector populated from compatible vehicle assets;
- a clear composed summary such as `Kirby + Warp Star`;
- **Load Amiibo** for the selected pair;
- separate indicators for local selection and the exact pair active on the adapter.

Changing the local vehicle does not immediately overwrite an adapter with dirty data. When clean
and connected, Load performs the existing full transaction. Offline selection only updates local
preference.

The old “Swap Combo” treatment can remain as a temporary projection, but the destination model is
an explicit selector rather than cycling opaque full bins.

### 9.2 Ordinary tags

The inspector adds **Create UID alias…** only when the selected item is standard. If keys are not
present, invoking it opens the existing user-key import explanation. Completion focuses the new
alias and labels its relationship to the source; it does not load the alias automatically.

No batch UID randomization is proposed. Bulk creation would make provenance, collisions, divergent
save histories, and user intent unnecessarily hard to understand.

### 9.3 Acquisition actions

The library presents acquisition as ordinary product work, not a diagnostics page:

```text
Add Amiibo
  Import file
  Scan with this device
  Scan with connected controller
```

The wording adapts to the platform. Android may say **Scan with this phone/tablet**; Windows may show
**Scan with connected Pro Controller 2** when a capable controller is attached through PicoSwitch.
With no NFC-capable source, scanning is unavailable and file import remains usable.

An unavailable hardware action is hidden or disabled with the concrete missing capability. The
application must not infer support from a controller display name, and ordinary NTAG215 capability
must not imply figure-v3 capability. The stock Pico W/Pico 2 W has no NFC hardware, so there is no
stock-board **Scan with adapter** action. A future adapter-attached reader, companion-local reader,
or USB reader appears only through its own capability-bearing provider.

UART and raw Nintendo NFC opcodes remain diagnostics/research surfaces. No enabled production
workflow requires a user to select a COM port, run a script, or send controller subcommands.

## 10. Amiibo acquisition and dumping

### 10.1 Sources and bridge boundary

Acquisition uses a small capability-driven `AmiiboAcquisitionSource` concept, with likely providers:

```text
FileImport
AndroidLocalNfc
ProController2ViaAdapter
Switch1ControllerViaAdapter
FutureUsbNfcReader
```

This is a product boundary, not a framework for every imaginable reader. Providers own their
hardware/transport lifecycle and feed one shared validation and normalization pipeline:

```text
file or physical reader
  -> bounded provider session
  -> immutable source evidence
  -> completeness + format validation
  -> normalization record + canonical library snapshot
  -> optional supported-v3 vehicle discovery/extraction
  -> local library commit
```

The stock Pico W/Pico 2 W is never the NFC reader. For controller-backed acquisition the physical
flow is:

```text
physical amiibo
  -> genuine controller NFC hardware
  -> controller transport
  -> PicoSwitch bridge
  -> high-level management acquisition service
  -> companion validation/normalization
  -> local library
```

No acquisition path loads the result onto the adapter automatically. Scan, save to library, select,
and load remain distinct user actions. A timeout, removal, disconnect, truncated read, missing
sector, or failed integrity check can produce a typed error or partial diagnostic artifact only; it
cannot be mislabelled as a complete 540/572/2048-byte library dump.

### 10.2 Acquisition feasibility matrix

| Acquisition source | Complete dump possible? | Evidence level | Current PicoSwitch support | Remaining work | Major unknowns |
|---|---|---|---|---|---|
| Existing file import | Yes: ordinary 540/572 and v3 2048 | **STRONGLY SUPPORTED** by current software/corpus | Native companions already accept exact canonical sizes | Common provenance/normalization and optional extraction pipeline | Explicit decoders for any additional wrapper/container formats |
| Android local NFC → ordinary | Yes: 540, optionally 572 with `READ_SIG` | **PROVEN BY EXTERNAL IMPLEMENTATION**; repository path is host-tested | Strict ordinary Android reader exists; physical-phone gate tracked separately | Physical device matrix, lifecycle/error validation, immutable-source integration | Device-specific timeout/transceive quirks |
| Android local NFC → v3 | Yes: normalized 2048 | **PROVEN BY EXTERNAL IMPLEMENTATION** | Not implemented; current ordinary provider must continue to reject it | Dedicated v3 provider, SRAM/session/sector flow, normalization, physical validation | Device-specific frame limits/timing and dynamic-field policy |
| Pro Controller 2 via PicoSwitch → ordinary | Yes: 540 | **PROVEN IN OUR HARDWARE** | Low-level BLE/UART/management research bridge exists; no production service | High-level session/result/cancel service and companion library workflow | Production disconnect/removal/retry policy |
| Pro Controller 2 via PicoSwitch → v3 | Yes: normalized 2048 | **PROVEN IN OUR HARDWARE** | `0x1E` sector-aware and `0x15` staged retrieval foundations exist; current probe remains partial | Preserve exact full-dump orchestration, implement bounded high-level service, normalization/provenance | Reliable grouping/range limits, offsets, timing/retries, dynamic/session handling |
| Switch 1 Pro via PicoSwitch → ordinary | Yes: 540 | **PROVEN BY EXTERNAL IMPLEMENTATION** | No MCU/report-`0x31` reader service | Implement/capture MCU lifecycle and management handoff; same-tag byte comparison | PicoSwitch transport timing and cancellation behavior |
| Switch 1 Pro via PicoSwitch → v3 | Unknown | **UNKNOWN** | None | Capability-only experiment after ordinary reader foundation | Whether controller MCU exposes sector selection and SRAM operations |
| Joy-Con R via PicoSwitch → ordinary | Yes: 540 | **PROVEN BY EXTERNAL IMPLEMENTATION** | No MCU/report-`0x31` reader service | Same bounded ordinary-reader work, validated separately | Joy-Con-specific transport/timing behavior |
| Joy-Con R via PicoSwitch → v3 | Unknown | **UNKNOWN** | None | Separate V3 capability experiment | Console-side presentation does not prove host-driven physical acquisition |
| Future generic USB NFC reader | Ordinary: yes; v3: reader-dependent | Ordinary **PROVEN BY EXTERNAL IMPLEMENTATION**; v3 **PLAUSIBLE** for transparent readers | None | Select reader/API, capability probe, provider and physical validation | Whether a reader exposes native sector-select, 64-byte SRAM exchange and required timeouts |

“Cards” are not a separate wire container here. A recognized card identity in a structurally valid
540/572-byte NTAG215 image follows the ordinary path. “V1/V2/V3” in UI/catalog metadata must not be
confused with file parsing: the canonical raw containers are exactly 540-byte NTAG215, 572-byte
NTAG215 plus the 32-byte reader signature, and 2048-byte figure-v3. Import may accept figures,
cards, yarn, and other identity types carried by those formats when validation succeeds; it does
not implicitly accept arbitrary vendor `.nfc` text, encrypted wrappers, or oversized files unless
an explicit, tested decoder is added.

### 10.3 Shared provider and result contract

The companions consume a transport-neutral provider contract conceptually equivalent to:

```text
AcquisitionSourceDescriptor
  id, kind, displayName, connected
  supportedFormats       ordinary-540 | ordinary-572 | figure-v3-2048
  evidenceLevel          production | experimental
  canStart, unavailableReason

AcquisitionSession
  sourceId, state, progress, timeout
  tagPresent, detectedUid (redacted in ordinary logs)
  expectedCoverage, receivedCoverage
  cancel()

AcquisitionResult
  CompleteOrdinaryDump   raw540 + optional signature32 + provenance
  CompleteV3Dump         normalized2048 + coverage/normalization provenance
  PartialCapture         bytes/fragments + known layout/ranges + incomplete reason
  UnsupportedTag         observed identity/technology + reason
  TransportFailure       bounded error/retry evidence
  Cancelled              explicit non-result
```

Exact implementation names may differ. The invariant is that an arbitrary byte array cannot claim
success without a typed completeness result and its evidence. Capability is format-specific. The
UI re-evaluates it when platform NFC availability, source-controller connection/profile, reader
identity, or the firmware/client contract changes.

For controller-backed acquisition the eventual production boundary is high-level:

```text
companion:  begin scan -> observe progress/state -> cancel or receive completed evidence
firmware:   own controller-specific NFC initialization, polling, reads, retries and teardown
```

Windows/Android must not send Nintendo `0x03`, `0x05`, `0x1E`, or `0x15` operations manually.
Exact management command names are deferred until the full Pro2 orchestration transcript is
preserved; raw opcode forwarding remains diagnostic-only.

### 10.4 Complete versus partial captures

Ordinary completeness is exact:

| Form | Completeness |
|---|---|
| 540 bytes | Complete physical pages `0x00..0x86` |
| 572 bytes | The same complete 540-byte image plus the 32-byte originality signature |

Both are complete ordinary library captures. Signature presence is provenance/capability metadata,
not a different Amiibo generation. A missing signature may be recorded as unavailable; it is never
synthesized.

Figure-v3 completeness is a validated normalized 2048-byte image plus a normalization record. It
does not imply that one physical or controller command returned 2048 bytes. A complete acquisition
may be assembled from several sector/page operations and repeated staged-buffer reads.

The record preserves physical ranges, sector/page destination mapping, SRAM source,
canonicalized/reserved regions, dynamic/session policy, provider/timestamp, and normalizer version.
This keeps two properties separate:

```text
complete normalized image
lossless physical backup of this exact tag
```

A donor-backed reconstruction may satisfy the first and not the second. A capture does not become
partial merely because it was assembled from multiple reads; it is partial when required coverage
or normalization provenance is missing.

**Safety invariant:** `PartialCapture` can be retained for diagnostics only. It can never be
imported, renamed, padded, donor-filled, or promoted as a complete library dump.

### 10.5 The two 668-byte representations

“668-byte capture” is currently ambiguous and must always name its layout:

1. **Historical evidence:** 604 directly read page bytes plus a separate 64-byte SRAM/vehicle
   device response. Those bytes map to several normalized-image ranges; they are not one contiguous
   prefix.
2. **Current packed probe:** 648 bytes from sector-0 pages `0x00..0xA1` plus 20 bytes from pages
   `0xE2..0xE6`. It contains no sector 1 and no 64-byte vehicle SRAM response.

Neither representation is complete. Neither is a contiguous 668-byte prefix of the normalized
2048-byte image. The current probe remains useful for research comparison, but its output is always
`PartialCapture`.

### 10.6 Import file

File import is the first production acquisition backend and remains offline. Windows, Android, and
web must converge on one exact normalization/validation fixture set. The current shared intent is:

- accept a raw 540-byte NTAG215 image;
- accept a 572-byte image containing the same 540 raw bytes plus the observed 32-byte reader
  signature;
- accept a complete 2048-byte v3 image with the established contiguous UID and `00/44` marker;
- retain the parser-recognized format-version metadata at `0x5B`, including ordinary V1/V2 values,
  while treating complete 2048-byte figure-v3 as the distinct v3 storage format;
- recognize the identity type at `0x57`, including cards, without treating a card as a different
  raw storage container;
- preserve the byte-exact input as source provenance before any cloned BCC normalization;
- record a direct full-coverage normalization result rather than implying that file size alone
  proves semantic validity;
- automatically discover a supported v3 vehicle and offer optional extraction after import.

The current browser importer truncates some inputs larger than 572 bytes to a standard image,
whereas the native clients require exact supported sizes. That divergence must be resolved before
claiming a common acquisition contract; archive/container decoding should be explicit rather than
implicit truncation.

### 10.7 Scan with companion-local NFC

Android-local ordinary acquisition is a separate provider from controller acquisition. The current
strict path identifies NTAG215 with its exact `GET_VERSION` reply, reads pages `0x00..0x86`, validates
manufacturer/BCC fields, and optionally retains an exact 32-byte `READ_SIG`. Its source/host logic is
established; any remaining physical-device gate is tracked independently and does not change the
format definition.

**EXTERNAL SOURCE EVIDENCE** establishes that Android `NfcA` can also complete a v3 acquisition, so
v3 is technically feasible rather than fundamentally blocked. It remains a distinct provider
protocol requiring:

- v3/chip recognition;
- UID-derived `PWD_AUTH` where protected;
- a 64-byte SRAM request/write/read exchange;
- session-register polling and bounded timeouts;
- `SECTOR_SELECT` and sector-1 reads;
- transceive-size-aware chunking;
- complete 2048-byte normalization and coverage provenance.

Do not route v3 through the ordinary NTAG215 reader or advertise it from generic `NfcA` availability.
The provider enables ordinary and v3 capabilities independently after their respective validation
ladders. Raw RF acquisition does not require Amiibo retail keys; NFC password authentication and
Nintendo application cryptography are separate layers.

### 10.8 Scan with a genuine Switch 2 Pro Controller

This source uses the genuine controller's NFC hardware. PicoSwitch is the BLE/management bridge.

**Ordinary NTAG215 — PROVEN IN OUR HARDWARE.** A successful physical read produced a 600-byte
controller buffer containing a 60-byte prefix and the complete 540-byte image. PicoSwitch writes
the extended command to BLE handle `0x0016`, subscribes to `0x001E`/CCC `0x001F`, and has also
observed matching ordinary replies through `0x001A`. Repeated `0x15` offset reads returned the
buffer in bounded chunks.

**Figure-v3 — PROVEN IN OUR HARDWARE.** Complete normalized 2048-byte V3 acquisition has already
been achieved through physical tag → genuine Pro Controller 2 → PicoSwitch2 → UART/testing → host
assembly. Repository evidence independently preserves the mechanism that resolves the old sector-1
blocker:

- `0x06` is the older sector-0-limited 8-bit range descriptor;
- `0x1E` is a separate sector-aware operation with captured sector-0 and sector-1 range triples;
- the controller bare-ACKs the operation, transitions to staged-result state, and exposes the larger
  result through repeated `0x15` offset reads;
- host-side normalization maps each returned `(sector, page range)` into the 2048-byte image.

The exact historical full-dump transcript is not preserved. Before production implementation, one
complete run must capture request grouping, maximum reliable range size, exact `0x15` offsets,
timing/retries, teardown, and dynamic/session canonicalization. That is a recipe-preservation gate,
not a feasibility gate.

Current `nfcmirror`, `nfc_probe.ps1`, and low-level management reader commands remain research
footholds. The current probe's packed 668-byte output is intentionally partial and does not reduce
the proven status of the separate complete path. Production must put controller-specific
initialization, polling, reads, retries, removal/disconnect handling, stale generations, result
assembly, and teardown behind the high-level acquisition service. It must also preserve one reader
session and mutual exclusion with console NFC mirroring without changing active input ownership.

### 10.9 Scan with a genuine Switch 1 NFC-capable controller

Switch 1 Pro Controller and Joy-Con Right require a distinct backend, not Pro2 byte passthrough.
The known host transport uses output report `0x11`, extended input report `0x31`, MCU configuration
subcommands `0x21`/`0x22`, CRC8/sequence state, and chunked tag data. At the pinned `jc_toolkit`
source revision, its physical reader sequence selects `0x31`, enables NFC MCU mode, detects a UID,
and assembles NTAG213/215/216 page reads. Complete ordinary acquisition is therefore **PROVEN BY
EXTERNAL IMPLEMENTATION**, not yet proven through PicoSwitch2.

The current PicoSwitch Switch 1 Bluetooth driver selects report `0x30`, parses only `0x30`/`0x3F`,
and has no MCU ownership layer. Required work therefore includes report-mode arbitration, MCU
initialization/state, output `0x11` transport, input `0x31` parsing, complete NTAG215 assembly,
disconnect/removal handling, and a transport-neutral handoff to the acquisition service. It must be
validated first by primary captures and a byte comparison against the same ordinary tag dumped by
a known-good reader.

No current evidence establishes physical figure-v3 acquisition through a Switch 1 controller, and
this HLD does not assume the older controller/MCU exposes sector selection or the V3 SRAM exchange.
Console-side V3 presentation/emulation through an older controller is adjacent evidence, not proof
that host software can command that controller to dump a physical tag. V3 remains **UNKNOWN**. The
backend stays disabled until implemented and physically validated; if evidence supports only
ordinary NTAG215, its capabilities must say exactly that.

Joy-Con 2 Right is also separate. The repository does not yet establish that it uses byte-identical
NFC transactions to Pro Controller 2, so the Pro2 backend must not enable itself for Joy-Con 2 by
product-family guess.

### 10.10 Future USB or adapter-attached NFC reader

A future desktop USB reader or explicit adapter add-on is a separate capability-bearing provider.
Ordinary NTAG215 is broadly supported by external PC/SC/libnfc-style implementations. V3 is only
plausible for a reader/API that exposes native Type 2 operations including the two-phase sector
select, required no-response timing, 64-byte SRAM exchange, session-register polling, and suitable
frame lengths.

The stock Pico boards remain unsupported for direct scanning. A future reader is enabled only after
one concrete model/provider proves exact complete output and interruption behavior. Marketing-level
“NFC support” or ordinary reads alone do not authorize V3 capability.

### 10.11 Physical-acquisition validation ladder

Each backend/format combination advances independently:

1. protocol/source evidence and bounded host fixtures;
2. complete acquisition through the actual firmware/backend with no UART dependency;
3. byte-for-byte comparison against the same physical tag's trusted dump;
4. removal, timeout, disconnect, cancellation, stale-reply, and repeat-read tests;
5. management transfer over both supported companion transports;
6. Windows/Android library save, reopen, archive export/import, and provenance verification;
7. for supported v3 only, extracted `.ext` equality, deduplication, and compatible composition.

The product action appears only at the level actually passed. A successful console-mediated relay,
UART probe, partial v3 snapshot, or public source implementation cannot by itself enable a normal
companion dump button.

## 11. Security, privacy, provenance, and licensing

Keep the lifecycle as six explicit layers:

1. acquisition;
2. validation/normalization;
3. extraction;
4. composition;
5. cryptographic mutation/re-signing;
6. console presentation.

Reading physical tag memory does not require Nintendo Amiibo retail keys. Tag-level UID-derived
`PWD_AUTH`, when a tag protects a region, is an NFC access mechanism and is not Nintendo application
cryptography. Retail keys are relevant only when a client semantically decrypts, verifies, changes,
re-signs, or re-encrypts Amiibo application data.

- `key_retail.bin` remains user-supplied and local. Never upload it to the adapter, include it in a
  library archive, log it, hash it into diagnostics, copy it into fixtures, or commit it.
- Physical acquisition providers are read-only. Their product contract contains no tag-write,
  initialize, compose, or re-sign operation.
- Generated aliases are created only from a source image that authenticates first. A corrupt dump
  cannot be laundered into a valid one.
- Every mutation writes a new/temporary artifact, verifies it, then atomically updates library
  metadata. Source backups and shared vehicle assets are not overwritten by initialization, alias
  creation, composition, vehicle extraction, or console Sync.
- Console-written state updates a clearly identified mutable rider/snapshot generation only after
  the existing dirty/readback and local-durability guards succeed. It never silently replaces the
  immutable acquisition artifact, and existing adapter dual-bank/archive-integrity rules remain.
- The current raw `amiibo reader send` surface is available only inside an authorized management
  session, but it still accepts controller opcodes. Production acquisition should expose bounded
  high-level operations and keep raw opcode access diagnostic-only rather than teach companions to
  drive it.
- Support bundles expose format, size, CRC/fingerprint prefixes, and error category only; no raw
  save data, Mii data, keys, or full UIDs by default.
- The reviewed AmiiboZero commit has no top-level repository license file. Reuse the independently
  established behavior and current PicoSwitch2 primitives; do not copy source until its licensing
  is explicit and compatible.
- The downloaded FAP, metadata databases, vehicle payloads, and retail key are research inputs, not
  automatically redistributable project assets. This HLD stores no raw payload or key material.

## 12. Implementation phases and gates

This HLD authorizes no implementation. Before the corresponding product work begins, preserve the
smallest missing primary evidence:

1. **Pro2 complete v3 acquisition capture.** Repeat one known full 2048-byte acquisition while
   recording the complete controller command/reply sequence: `0x1E` grouping, `0x15` offsets,
   timing, retries, teardown, normalization, and dynamic fields.
2. **Repeat the same v3 tag several times.** Identify stable physical data versus dynamic/session
   values and establish the canonical normalization policy.
3. **Android v3 direct NFC acquisition.** Reproduce the externally proven `NfcA` flow in the
   intended companion/platform environment and compare the normalized image with a trusted read of
   the same tag.
4. **Switch 1 Pro/Joy-Con Right bounded capability experiment.** First establish complete ordinary
   reading through PicoSwitch; then determine, without assuming, whether the MCU exposes v3
   sector/SRAM operations.
5. **Vehicle extraction repeatability/composition experiment.** Repeat extraction from one physical
   source, prove exact-content identity and provenance, then test a known-compatible cross-rider
   composition and writeback without changing the immutable source or shared vehicle asset.

These experiments are documentation gates, not instructions to run them during this revision.
Every implementation phase below remains proposed until its evidence gate is satisfied and
explicitly authorized.

### Phase A — offline contract and projection

1. Add shared source-artifact, extraction, deduplication, composition/migration vectors and negative
   cases.
2. Teach the corpus/library analyzers to report rider-body and vehicle-response groups separately.
3. Project existing complete v3 bins as rider + vehicle choices without deleting or rewriting
   source entries.
4. Prove that every available 4 × 4 factory pair is reconstructed byte-for-byte when the complete
   corpus is available.
5. Prove that importing a supported user v3 dump keeps its 2048 bytes byte-identical, extracts the
   exact 64-byte response, appends provenance, and deduplicates identical responses without losing
   either source edge.

Gate: deterministic tests only; no firmware or hardware change.

### Phase B — v3 rider/vehicle product path

1. Implement vehicle import/extraction and explicit selection in web, Android, and Windows.
2. Materialize locally and use the unchanged management upload.
3. Implement split-responsibility Sync and unexpected-SRAM quarantine.
4. Add archive v4 only after the three clients agree.

Software gates:

- existing v3 structure/runtime/write tests;
- corpus and NFC semantic suites;
- each client's library/archive/composition tests;
- cross-language fixtures and archive round-trips;
- management transactional upload regression;
- no firmware build is required if firmware/source is untouched.

Hardware gate, one instrumented Air Riders sequence:

1. load a known rider + vehicle control;
2. let the game write/learn state, then Sync;
3. select a different vehicle and load the same rider snapshot;
4. confirm the new vehicle and retained rider state;
5. switch back and confirm the earlier state still loads;
6. save again, Sync, power-cycle the adapter, and re-read;
7. capture `v3diag`, zero-loss NFC trace, before/after images, hashes, and the selected tuple.

Only after that central same-rider/different-vehicle test passes should the UI claim that vehicle
switching retains Air Riders state.

### Phase C — companion acquisition foundation and file import

1. Define the cross-language reader descriptor/session/result fixtures and immutable source-artifact
   model.
2. Converge web, Android, and Windows exact-size/normalization behavior; remove implicit oversized
   input truncation from the claimed common contract.
3. Make **Import file** flow through the common validation, provenance, and optional v3 extraction
   pipeline.
4. Add capability-gated **Scan with this device** and **Scan with connected controller** UI states
   with honest format-specific unavailable reasons; no raw/research command is exposed in
   production UI and the stock Pico is never presented as an NFC reader.

Gate: archive round-trips preserve original-source hashes and all provenance edges; unsupported
hardware actions remain disabled. This phase does not require firmware changes.

### Phase D — Pro Controller 2 physical acquisition

1. Preserve the exact complete-v3 transcript and repeat-read normalization policy from the research
   gates above. Complete feasibility is already proven; this step preserves the production recipe.
2. Turn the proven ordinary and v3 controller sequences into one bounded firmware-owned service
   with independent output capabilities.
3. Add capability/status/result/cancel management surfaces, BLE allowlist/parity coverage, portable
   client wrappers, and shared fixtures together.
4. Integrate Windows and Android workflows without UART, raw opcodes, or developer tooling.
5. Validate complete 540 output and normalized 2048 output against trusted reads of the same
   physical tags, including removal, disconnect, cancellation, stale replies, repeat reads,
   persistence, and provenance.

Gate: ordinary and v3 capabilities advance independently through §10.11. Proven UART feasibility
does not by itself enable the product action, but v3 must not be described as unsupported while the
high-level workflow is pending.

### Phase E — additional acquisition providers

Run independent evidence tracks rather than sharing capability by controller or API family:

1. **Android local NFC:** close the physical ordinary gate if still open, then validate the separate
   externally proven v3 SRAM/sector protocol and normalization record.
2. **Switch 1 Pro Controller/Joy-Con Right:** build a capture-only MCU/report `0x11`/`0x31`
   laboratory, prove complete ordinary output through PicoSwitch, and probe v3 only as a separate
   capability question.
3. **Joy-Con 2 Right:** capture its physical read and compare commands/replies with Pro2 before
   sharing any backend profile.
4. **Future USB/add-on reader:** evaluate one explicitly selected reader/API; stock Pico W/Pico 2 W
   remains unsupported for direct NFC.

Each successful track adds only the format capabilities it proves. A partial v3 capture never
passes the gate, and console-side tag presentation does not count as physical acquisition evidence.

### Phase F — ordinary UID alias, experimental

1. Extend the existing crypto vectors before adding UI.
2. Implement new-copy alias creation in each client.
3. Keep the feature explicitly experimental until console testing closes the signature and
   game-private-state questions.

Hardware A/B:

1. present the genuine-source 540-byte image as a positive control;
2. create one alias with a recorded requested UID and prove local HMAC/plaintext invariants;
3. present it in System Settings and the target game;
4. verify owner/nickname/application state is retained;
5. write to the alias, Sync, and rescan it;
6. for one named cooldown game, demonstrate source limited vs. alias accepted or preserve the
   negative result;
7. repeat with a 572-byte source emitted as a 540-byte alias.

Gate: no general “bypasses time limits” claim. Document only the exact game/path observed.

### Phase G — optional consolidation

After archive v4 and reopen/restore testing, offer an explicit consolidation tool for redundant
derived combinations. It must preview which projections are byte-reconstructible, export a backup
first, never remove same-UID images whose non-SRAM bytes differ, and never remove the immutable
source artifacts that establish acquisition provenance.

## 13. Acceptance criteria

The upgrade is complete only when:

- 4 rider bodies + 4 vehicle responses can represent and reconstruct all 16 known factory pairs;
- imported existing 2048-byte bins remain accepted without conversion;
- every imported/scanned complete source remains byte-exact, immutable, exportable, and linked from
  all derived rider/vehicle records;
- a supported user-owned v3 dump is automatically recognized as vehicle-extractable, optional
  extraction copies exactly 64 bytes, and identical payloads deduplicate without losing provenance;
- unknown/unproven v3 families remain valid full-dump library items but do not expose their response
  for cross-rider composition;
- vehicle selection changes exactly `0x3C0..0x3FF` in the materialized image;
- a v3 console write updates one rider snapshot and no shared vehicle asset;
- dirty adapter data cannot be lost during rider or vehicle replacement;
- archive export/import is cross-platform and old-client fallback retains valid rider bins;
- file import accepts the common parser's canonical 540/572/2048 formats and recognized identity
  types (including cards) without silently treating arbitrary containers as raw dumps;
- acquisition UI exposes Import/this-device/controller methods only according to live,
  format-specific capabilities, never describes the stock Pico as an NFC reader, and never needs
  UART for an enabled production path;
- ordinary 540 and 572 captures are both complete, with the 32-byte originality signature treated
  as optional acquisition metadata rather than a different tag class;
- a complete v3 result is a validated normalized 2048-byte image with range/SRAM/canonicalization
  provenance and need not come from one command;
- no partial capture—including either 668-byte representation—can enter the library as a complete
  usable dump;
- complete normalized and lossless physical-backup status remain distinct and visible in
  provenance;
- every enabled physical backend passes complete-read, byte-comparison, interruption, management
  transfer, reopen, and archive/provenance tests independently;
- ordinary UID aliases are new verified items, never runtime overlays or in-place rewrites;
- alias plaintext save state is byte-identical at creation and divergent histories are not merged;
- figure-v3 UID aliases remain refused;
- no key, private save data, or unapproved response payload enters firmware, logs, fixtures, Git, or
  distributable application assets;
- claims remain separated into offline/source-tested, application-built, automated device,
  physical console, and real-game validation.

## 14. Open questions

| Question | Current confidence | Closure |
|---|---|---|
| Is the 64-byte response sufficient for all four current Air Riders vehicles? | **Confirmed** across retained corpus/current runtime | byte-exact reconstruction plus one same-rider vehicle-switch hardware pass |
| Do game writes preserve the response? | **Confirmed** for captured PicoSwitch2 v3 writes | keep invariant and quarantine any contradiction |
| Does switching vehicle preserve every useful rider state? | **Strongly supported, not directly validated as a product flow** | central Phase B Air Riders A/B |
| Can partial 1–62-byte vehicle payloads be safely normalized? | **Unknown / unnecessary** | defer; exact 64-byte import only |
| Can a user's compatible 2048-byte v3 dump yield a reusable `.ext`? | **Yes for the evidence-backed Air Riders profile** | exact-copy/CRC/dedup/provenance fixtures, then product import test |
| Should every structurally valid v3 response be reusable? | **No evidence for that generalization** | profile-gated; add a family only after controlled composition evidence |
| Can stock PicoSwitch hardware scan a tag directly? | **No** — the stock Pico W/Pico 2 W BOM has no NFC reader | requires an explicitly supported external/local reader backend |
| Can Pro Controller 2 produce a complete ordinary dump? | **PROVEN IN OUR HARDWARE** — complete 540-byte acquisition | preserve/productize the high-level no-UART session |
| Can Pro Controller 2 produce a complete v3 library dump? | **PROVEN IN OUR HARDWARE** — complete normalized 2048-byte acquisition; `0x1E` sector access and `0x15` retrieval independently retained | preserve exact command grouping/timing/normalization, then productize |
| Can Android NFC acquire complete v3? | **PROVEN BY EXTERNAL IMPLEMENTATION; not implemented here** | reproduce on intended Android devices and compare the same tag |
| Can a Switch 1 Pro Controller/Joy-Con Right dump ordinary tags? | **PROVEN BY EXTERNAL IMPLEMENTATION; unimplemented/unvalidated here** | primary PicoSwitch capture plus MCU backend and same-tag comparison |
| Can a Switch 1 controller dump v3? | **Unknown** | do not advertise; separate evidence after ordinary closure |
| Does Joy-Con 2 Right share the Pro2 NFC transaction? | **Unknown** | separate physical capture before backend reuse |
| Will a re-signed 540-byte UID alias be accepted? | **Cryptographically coherent; console acceptance unverified** | Phase F control/alias A/B |
| Does a UID alias bypass a timer? | **Game-specific** | one named game at a time; no universal claim |
| Can v3 UIDs be safely aliased? | **Unknown and contradicted by AmiiboZero's product policy** | deferred separate research, not part of this upgrade |

## References

- [`../switch2/nfc-implementation.md`](../switch2/nfc-implementation.md)
- [`../switch2/nfc-protocol-inventory.md`](../switch2/nfc-protocol-inventory.md)
- [`../Amiibo-v3.md`](../Amiibo-v3.md)
- [`../management/PROTOCOL.md`](../management/PROTOCOL.md)
- [`../switch2/amiibo-identity-and-generation.md`](../switch2/amiibo-identity-and-generation.md)
- [`../re-methodology/nfc-investigation-workflow.md`](../re-methodology/nfc-investigation-workflow.md)
- [`../experiments/pro2-native-nfc-read-2026-07-25.md`](../experiments/pro2-native-nfc-read-2026-07-25.md)
- [`../experiments/v3-amiibo-genuine-capture-runbook.md`](../experiments/v3-amiibo-genuine-capture-runbook.md)
- [`../experiments/v3-full-sram-response-validation-2026-07-28.md`](../experiments/v3-full-sram-response-validation-2026-07-28.md)
- [`../experiments/v3-air-riders-dynamic-allocation-2026-07-28.md`](../experiments/v3-air-riders-dynamic-allocation-2026-07-28.md)
- [`../../dumps/README.md`](../../dumps/README.md)
- [`../../dumps/amiibo/genuine-kirby-warp-reuse-sub1e-usb-2026-07-28.jsonl`](../../dumps/amiibo/genuine-kirby-warp-reuse-sub1e-usb-2026-07-28.jsonl)
- [`../../src/nfc/ns2_amiibo_v3.c`](../../src/nfc/ns2_amiibo_v3.c)
- [`CTCaer/jc_toolkit` pinned Switch 1 reader source](https://github.com/CTCaer/jc_toolkit/tree/9d0cc455aebd07930b557840b47cb26df9eb4a1f)
- [Raspberry Pi Pico W/Pico 2 W official hardware documentation](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html)
- [NXP NTAG I²C Plus 2K datasheet](https://www.nxp.com/docs/en/data-sheet/NT3H2111_2211.pdf)
- [Android `NfcA` API](https://developer.android.com/reference/android/nfc/tech/NfcA)
- [TagMo v3 `NfcA` acquisition source at the reviewed commit](https://github.com/HiddenRamblings/TagMo/blob/25ca8d90bc8d405bb0d7e4a98fb0da671ba581c0/app/src/main/java/com/hiddenramblings/tagmo/nfctech/AirRiders.kt)
- [Switch 2 controller BLE interface research at the reviewed repository](https://github.com/ndeadly/switch2_controller_research/blob/master/bluetooth_interface.md)
- [AmiiboZero README at reviewed commit](https://github.com/DanTheMan827/AmiiboZero/blob/c9fcb08a032320825e46f0c8958fa5ea53df50db/README.md#standard-and-v3lock-on-amiibo)
- [AmiiboZero persistence and UID-randomization description](https://github.com/DanTheMan827/AmiiboZero/blob/c9fcb08a032320825e46f0c8958fa5ea53df50db/README.md#nfc-persistence-and-uid-randomization)
- [AmiiboZero UID implementation guard](https://github.com/DanTheMan827/AmiiboZero/blob/c9fcb08a032320825e46f0c8958fa5ea53df50db/src/amiibo_nfc.c#L829-L854)
