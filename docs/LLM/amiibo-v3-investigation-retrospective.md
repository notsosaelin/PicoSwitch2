# Amiibo v3 investigation retrospective and tooling proposal

Date: 2026-07-28  
Audience: future coding agents and maintainers building PicoSwitch2 investigation tools  
Scope: the 2048-byte NTAG I2C Plus 2K / Kirby Air Riders Virtual Amiibo investigation

This is a process retrospective, not protocol authority. Current behavior belongs in
[`../../STATUS.md`](../../STATUS.md), protocol details in [`../Amiibo-v3.md`](../Amiibo-v3.md),
and individual observations in [`../experiments/`](../experiments/).

## Executive summary

The investigation succeeded. PicoSwitch2 now reads and writes all 16 available Air Riders v3
dumps on a real Switch 2, persists console-written state, reopens written figures, handles
allocation-relative storage, and remains independent of a figure/UID whitelist.

It took substantially more hardware iterations than it should have.

The main problem was not a lack of intelligence or source material. It was the absence of an
integrated laboratory workflow. We repeatedly observed a console symptom, changed firmware,
flashed it, and asked the maintainer to repeat a physical test before fully separating four
different layers:

1. USB command framing between the Switch and PicoSwitch2;
2. Pro Controller 2 NFC command and report-state behavior;
3. NTAG I2C memory, SRAM-mailbox, and sector semantics;
4. the game's higher-level read, write, Stop, remove, and reuse lifecycle.

Several failures at one layer looked identical at the console UI. Error `2115-0096`, for example,
appeared during a genuine removal-timing bug and later during a fail-closed record-layout
rejection. Treating the visible error as a diagnosis caused avoidable work.

The highest-return future investment is a trace-driven NFC laboratory that can:

- capture one complete experiment into a self-describing artifact bundle;
- reassemble and decode every command automatically;
- replay the transaction against the firmware state machine on the host;
- compare genuine and virtual timelines while normalizing irrelevant timing;
- validate every available dump and infer its allocation before hardware is involved;
- turn a successful or failing capture directly into a regression fixture.

The second-highest-return investment is a repository skill that enforces that workflow. The skill
should call real scripts and tests; it should not merely remind the agent to be careful.

## What ultimately worked

The decisive advances were all improvements in observation quality.

### The UART bridge changed the problem

Keeping the dongle connected to the Switch while talking to it over UART made console-facing
behavior observable without guessing from UI errors. It exposed report states, command counters,
trace records, store generations, and exported images.

### Genuine Pro Controller 2 initiation provided a positive control

The `nfcmirror` initiator path let PicoSwitch2 send NFC commands to a genuine controller and dump
a physical v3 amiibo. That supplied behavior the console-side virtual trace alone could not
explain:

- the `0x14`/`0x21` device-command path;
- the complete 83-byte result;
- the two Air Riders `0x20` operations;
- empty states `0x15` and `0x16`;
- the selected-UID `0x1E` reuse read;
- the chip-managed capability generation.

### Full captures beat field guessing

The correct `0x21` result was not “32 meaningful bytes, then zeros, then constant `7A C4`.” It was
a 19-byte controller header followed by the dump's complete 64-byte SRAM response. The final two
bytes are a per-response CRC-16/MCRF4XX. Reassembling the entire result immediately explained why
downloaded images failed while the one captured physical figure appeared to work.

### A second allocation exposed overfitting

Kirby used sector-0 page `0x92` and sector-1 capability/data pages `0x00/0x01`. King Dedede used
`0xB2` and `0x64/0x65`. The second figure proved that the envelope describes its allocation and
that a fixed Kirby record table was incorrect. The final implementation consumes the
self-described pages, validates safe bounds, and contains no rider/UID/product table.

### Fail-closed validation was retained

The eventual generalization did not become “accept any pages.” It preserved command identity,
record count, lengths, generation step, complete coverage, padding, cleared-window boundaries,
and total image bounds. An unknown future protocol shape will fail safely instead of corrupting a
user's image.

## Condensed investigation sequence

### 1. NTAG215 assumptions leaked into a 2 KB problem

The initial implementation treated v3 as a larger ordinary amiibo and experimented with raw
sector-0 serving, prefixes, page ranges, and advertised model values. Some experiments advanced
the console farther, but the mental model still mixed the console's Pro2 abstraction with the RF
protocol used by projects that emulate the physical tag.

The important distinction arrived late:

- projects such as pixl.js emulate an NFC tag and answer RF commands such as `GET_VERSION` and
  sector selection;
- PicoSwitch2 emulates the controller, so it receives already-abstracted controller commands and
  must reproduce genuine controller behavior.

### 2. Recognition was solved before full validation was solved

The console could recognize the tag while later rejecting or crashing. “Recognized” proved that
identity and the initial view were close enough; it did not prove device-command framing, SRAM
response integrity, write transport, or lifecycle.

This distinction should have been encoded into the test vocabulary immediately:

- discovered;
- identity accepted;
- device command accepted;
- data read completed;
- ordinary write committed;
- extended write committed;
- Stop/removal completed;
- reuse completed;
- power-cycle recovery completed.

Instead, several updates used broad phrases such as “read solved,” which overstated the evidence.

### 3. USB stream framing caused a console crash

A normal `0x14` request is 88 bytes, but the firmware read vendor OUT traffic into a 64-byte
buffer and dispatched the first packet immediately. The trailing 24 bytes became a second command,
causing error `2168-0002`.

This was a conventional transport bug and should have been caught before hardware. A stream
reassembler and split/coalesced host tests fixed it. The lesson is that protocol work should never
assume one USB read equals one protocol message unless the transport contract explicitly says so.

### 4. One physical figure caused false constants

Six genuine results were byte-identical because they all came from the same physical Warp Star.
That made a tag-specific SRAM CRC look like a controller constant. Downloaded Warp, Winged, Tank,
and Shadow images had different valid response CRCs.

The first response implementation copied only half of the SRAM buffer and synthesized the rest.
Serving all 64 stored bytes fixed untouched downloaded dumps without a signature override,
carrier conversion, or key-based transformation.

### 5. Multiple variables were changed together

At one point the investigation moved prefix fields, signature data, SRAM content, and tag sources
across related tests. A downloaded dump was not retried immediately after the prefix correction.
That produced an incorrect provenance/signature conclusion and sent work toward retail-key and
carrier theories.

The repository now records that retraction. The better rule is stronger: a hypothesis test must
name the one intended variable and automatically record every other byte-level difference.

### 6. Basic write worked before game-specific write worked

System Settings owner/nickname writes used the ordinary six-chunk `0x14` transaction and `0x08`
commit. Air Riders added two sector-aware `0x20` operations around ordinary writes:

- a 355-byte clear;
- a 167-byte three-record update.

Treating the first operation as a harmless no-op let simple writes pass but game writes fail.
Capturing the physical positive control revealed that both operations mutate durable state.

### 7. Tag removal was both a real bug and a misleading later diagnosis

The first Air Riders lifecycle failure genuinely involved removal timing. A real tag remained
available between the clear checkpoint and the update roughly 130 ms later, while PicoSwitch2
reported removal. A sequence-aware hold fixed that.

Later, King Dedede again produced `2115-0096` and status `07 41`. It looked like the same lifecycle
bug, but diagnostics showed all chunks staged and the commit validator rejecting different record
pages. The same status value represented two different causes.

The diagnostic protocol should expose a structured internal error reason in addition to the
console-facing status.

### 8. Reuse added a command and implicit state

A written v3 tag did not simply repeat the initial read. The console sent `0x1E`, expected empty
state `0x15`, and fetched a sector-aware result. After implementing that, a second reuse still
failed because the capability generation at a sector-1 page was changing independently of the
explicit data records.

The genuine controller positive control showed the generation advancing. Persisting and serving
that state completed reuse and power-cycle recovery.

### 9. The second rider forced the correct abstraction

King Dedede proved byte 13 and the record pages were allocation semantics rather than opaque
identity-specific values. The implementation was rewritten around a validated dynamic layout.
All 16 available dumps then passed real-console read and write.

## What I missed or handled poorly

### I overfit to the first successful sample

Kirby's pages, SRAM trailer, and lifecycle became implicit constants too early. A field should not
be called constant after repeated observations from one physical identity. The minimum evidence
should be two independent identities or a structural explanation backed by a parser.

### I treated console symptoms as protocol diagnoses

“Not recognized,” a system crash, `2115-0096`, a frozen menu, and “corrupted” were useful
outcomes, but none identified the failing layer. I should have required trace, internal state,
store generation, and exported-image evidence before proposing a code change.

### I asked for too many incremental flashes

Several firmware builds tested one byte or one narrow interpretation that could have been rejected
offline by capture reassembly, corpus comparison, or a host replay harness. This placed excessive
physical burden on the maintainer and made the investigation feel circular.

The correct hardware unit is an experiment packet, not a speculative build:

- exact hypothesis;
- expected wire difference;
- prepared trace;
- prepared diagnostic snapshot;
- one action requested from the maintainer;
- explicit pass/fail discriminator;
- automatic artifact export.

### I did not establish a complete genuine positive control early enough

Once the real Pro Controller 2 could be queried through UART, the highest-return action was to
capture the complete read/write/reuse sequence before more virtual changes. Instead, some virtual
experiments continued while key genuine states remained unknown.

### I conflated controller protocol with RF/tag behavior

Third-party projects were valuable for memory format and tag-side semantics, but their transport
was not the transport PicoSwitch2 needed to emulate. I should have written the layer boundary down
before using their constants.

### I changed multiple variables in some experiments

The incorrect signature/provenance conclusion was the clearest result. Prefix, source dump,
signature, and response body were not always isolated. A test matrix generated from a baseline
would have prevented that.

### I lacked an allocation/corpus report at the start

All 16 dumps were available locally. A recursive analyzer could have reported:

- sizes and hashes;
- UID and catalog identity;
- encrypted-body equivalence groups;
- machine SRAM equivalence groups;
- SRAM CRCs;
- nonzero regions;
- cross-dump byte clusters.

That would not have revealed the console's dynamic write pages by itself, but it would have made
the “one figure is representative” assumption visibly unsafe.

### The UART tooling was too fragile

Observed friction included:

- hard-coded COM port assumptions;
- trace JSON lines interleaving or being truncated;
- a small ring overwriting the first record;
- incorrect guessed commands such as `trace save`;
- counters without a reset command, requiring manual baselines;
- dump scripts writing relative paths under `C:\Windows`;
- PowerShell `AddRange` type conversion failures;
- repeated manual sequences to clear, start, stop, dump, diagnose, and export.

Each issue was individually small. Together they consumed attention and increased the chance of
misreading an experiment.

### The runtime state machine was not replayable

Protocol handling lived inside the USB personality runtime. Host tests covered helpers, but we
could not feed a genuine command timeline into the whole state machine and inspect responses,
status edges, persistence calls, and final image bytes. That forced hardware to validate logic
that should have been deterministic offline.

### The portal and protocol work were interleaved

The offline library and diagnostic portal were useful, especially for validating AmiiboAPI and
crypto behavior. However, UI restructuring during active protocol RE increased the change surface
and documentation load. A stable mocked adapter contract would have allowed portal work and NFC
runtime work to proceed independently.

### I allowed local paths into research tooling

Some scripts initially embedded the maintainer's Downloads path. This was caught before the final
commit and replaced with arguments/environment variables. Investigation tools should be portable
from their first commit.

## A better investigation method

The following workflow would have reduced the v3 investigation substantially.

### Phase 0: classify the layer

Before changing code, classify the failure as one or more of:

- USB framing;
- controller command parsing;
- controller report-state sequencing;
- tag view/memory bytes;
- image crypto/integrity;
- persistence;
- game lifecycle;
- portal transport/storage.

If the evidence cannot identify a layer, improve instrumentation first.

### Phase 1: inventory all static evidence

Run one corpus command over every available dump. Produce a checked-in manifest containing hashes,
identities, CRCs, structural groups, and inferred safe allocations. Never require or copy the
retail key into the repository.

### Phase 2: capture the genuine golden path

Capture read, write, Stop, reuse, and power-cycle behavior from a genuine controller where
possible. Reassemble it immediately and save:

- raw trace;
- decoded timeline;
- initial and final images;
- firmware/controller identity;
- experiment metadata;
- exact physical actions.

### Phase 3: replay before flashing

Feed the golden console commands into a host build of the PicoSwitch2 NFC state machine. Compare
every response, event edge, status, persistence request, and final image against the genuine
timeline or an explicitly documented emulation policy.

### Phase 4: isolate one hypothesis

Generate an experiment manifest specifying:

- baseline artifact;
- one intended variable;
- expected result;
- forbidden incidental differences;
- pass/fail predicate.

Reject a build if the offline diff contains unexplained changes.

### Phase 5: perform one instrumented hardware action

The maintainer should normally need to say only `ready`, perform one described action, and say
`done`. The runner should own every UART command and output filename.

### Phase 6: convert evidence into regression coverage

A result is not complete until the trace becomes either:

- a golden positive fixture;
- a negative/fail-closed fixture;
- a refuted-hypothesis entry.

## Recommended tools, in priority order

| Priority | Tool | Problem eliminated | Minimum useful contract |
|---|---|---|---|
| P0 | NFC experiment runner | Repeated manual UART sequences and missing metadata | One command arms, captures, stops, exports diagnostics/image, and writes a manifest |
| P0 | Full NFC state-machine replay harness | Hardware flashes for deterministic parsing/state bugs | Feed command records; return responses, state edges, persistence operations, and final image |
| P0 | Genuine-vs-virtual trace comparator | Manual byte inspection and false constants | Reassemble commands/results, normalize timing/counters, report first semantic divergence |
| P0 | v3 corpus analyzer | Single-sample overfitting and unknown dump structure | Recursively validate/deduplicate/group dumps and emit a portable JSON manifest |
| P1 | Capture-to-fixture generator | Hand-transcribed C arrays and incomplete tests | Convert a selected transaction into C/JSON fixtures plus expected state transitions |
| P1 | Structured UART error telemetry | Confusing identical console status values | Expose internal error enum, command, state, offset, expected/actual, and store generation |
| P1 | Portal integration harness | Manual IndexedDB/Sync/transport testing | Browser automation with mock serial/BLE adapter and persistent IndexedDB assertions |
| P1 | Persistence fault injector | Power-loss bugs requiring risky physical testing | Interrupt erase/program at every journal step and verify recovery |
| P2 | Protocol transaction visualizer | Difficult review of multi-stage operations | Render a timeline of commands, states, records, image ranges, and generation changes |
| P2 | Bounded protocol fuzzer | Unseen malformed/future record shapes | Mutate lengths/pages/order/padding and assert fail-closed behavior with no image mutation |
| Optional hardware | ISO14443A RF capture/emulation instrument | Ambiguity between controller abstraction and tag behavior | Observe genuine tag-side `GET_VERSION`, sector selection, SRAM mailbox, and removal timing |

## Tool 1: `nfc_lab` experiment runner

This should be the first tool built.

Example interface:

```powershell
.\tools\nfc_lab.ps1 `
  -Scenario v3-air-riders-write `
  -Port Auto `
  -Controller GenuinePro2 `
  -TagLabel dedede-tank `
  -Output dumps\experiments
```

Suggested behavior:

1. Verify repository/branch/commit and record dirty state.
2. Auto-discover the UART port by `ping`, not by remembered COM number.
3. Query firmware build ID, personality, controller identity, store status, and diagnostic
   baselines.
4. Clear and start a sufficiently large or streaming trace.
5. Print the single physical action the maintainer should perform.
6. After `done`, stop capture and retrieve:
   - raw trace;
   - decoded trace;
   - `amiibo status`;
   - `amiibo v3diag`;
   - current image when permitted.
7. Hash every artifact and write `experiment.json`.
8. Run the decoder/comparator and print a concise verdict.

The UART transport should use framed records with length and checksum, or stream trace records to
the PC as they occur. Newline-delimited JSON is convenient but should not be able to interleave
two records into invalid JSON.

The firmware should add:

- `diag reset nfc`;
- a monotonic experiment/session ID;
- an internal error enum separate from console-facing `07 41`;
- trace streaming or a lossless snapshot command;
- a command that atomically returns status, counters, active state, and store generation.

## Tool 2: replayable NFC core

Extract the controller-facing NFC state machine behind a small interface:

```c
result = ns2_nfc_step(&state, now_ms, command, command_size, &effects);
```

`effects` should describe:

- response bytes;
- report-state edge;
- console-facing NFC status;
- internal error;
- persistence request;
- presented/removed state;
- modified image ranges.

The host harness should replay JSONL or a compact fixture without TinyUSB, BTstack, flash, or
physical time. A fake clock should make the 130 ms continuation window, five-second sequence
timeout, three-second re-presentation cooldown, and wraparound deterministic.

This would have caught or shortened:

- the 88-byte USB command split;
- premature removal between Air Riders stages;
- missing `0x1E` transition;
- stale capability generation;
- fixed Kirby record pages;
- persistence-before-eject ordering.

## Tool 3: semantic trace comparator

Raw hex diffs are insufficient because sequence numbers, timestamps, chunk boundaries, and
counters legitimately vary.

The comparator should decode:

- command/subcommand;
- declared and reassembled length;
- UID;
- record list `(sector, page, length)`;
- status and detail;
- result type;
- report-state edges;
- read ranges;
- image writes;
- generation transitions;
- Stop/remove/re-present events.

Example output:

```text
MATCH through transaction 4 / command 0x20
DIVERGENCE:
  genuine: completion -> status 0x16, tag remains presented
  virtual: completion -> status 0x07/0x41, internal RECORD_PAGE_MISMATCH
  record 1:
    genuine sector=0 page=0xB2 len=0x20
    virtual expected page=0x92
```

That one report would have diagnosed the final Dedede failure without another speculative
TagRemoved fix.

## Tool 4: v3 corpus analyzer

Example:

```powershell
python tools\amiibo_corpus.py `
  --directory D:\OwnedAmiiboDumps `
  --retail-key D:\Private\key_retail.bin `
  --output build\amiibo-corpus.json
```

The key argument must be optional and used only in memory. The output must never contain key
material or decrypted private fields unless explicitly requested.

For every image, report:

- filename relative to the selected root;
- size, SHA-256, CRC32;
- UID and raw amiibo identity;
- standard/v3 format;
- HMAC validity when a key is supplied;
- SRAM CRC validity;
- catalog match when locally available;
- duplicate/content group;
- nonzero ranges;
- candidate capability pages and generation values;
- known extended-allocation schema;
- whether the image is safe for the current runtime.

Across the corpus, report byte-difference clusters and equivalence groups. The analyzer should be
able to say, before a flash:

```text
16 valid v3 images
2 distinct extended allocation layouts
4 rider encrypted-body groups
4 machine SRAM groups
0 unsupported record schemas
```

## Tool 5: portal integration harness

Use browser automation against `tools/run_config_portal.ps1` with a mock adapter implementing the
same command contract as USB CDC and Config BLE.

Required tests:

- import single file and recursive directory;
- deduplicate by intended library identity;
- preserve distinct rider/machine variants;
- initialize with an in-memory user-supplied key;
- load selected image;
- simulate console write;
- show dirty/Sync state;
- Sync only acknowledges after IndexedDB commit;
- browser restart retains the library;
- export/import library round-trip;
- offline operation remains functional;
- no adapter operation can silently delete the library copy.

This would isolate UI changes from firmware protocol changes.

## Proposed skill: `picoswitch2-nfc-lab`

A skill is worthwhile after the first four scripts exist. Without scripts, a skill would merely
repeat guidance that agents may still bypass.

### Trigger

Use when a request involves NFC, amiibo, Virtual Amiibo, v3, native controller tag passthrough,
NFC UART captures, or console NFC errors.

### Required reading

1. `AGENTS.md`
2. `STATUS.md`
3. `PLAN.md`
4. `docs/LLM/current-context.md`
5. `docs/Amiibo-v3.md`
6. the experiment document linked for the active blocker

### Enforced workflow

1. Audit `git status`; preserve unrelated files.
2. Run the corpus analyzer for any new dump before firmware changes.
3. Classify the failing layer.
4. Create an experiment ID and manifest.
5. Prefer replay/comparison over a flash.
6. Do not mutate live UART state until the maintainer says `ready`.
7. Ask for one physical action at a time.
8. Retrieve all artifacts automatically after `done`.
9. Convert the result into a fixture or refuted hypothesis.
10. Run relevant host tests, both board builds when firmware changes, and reset-marker checks.
11. State separately: static test, build success, and hardware validation.
12. Never commit/push/release unless requested.

### Safety rules

- Never use a figure/UID whitelist to cover an unexplained protocol field.
- Never weaken bounds merely to accept a new dump.
- Never ship or copy `key_retail.bin`.
- Never overwrite a user's source dump; write a new artifact.
- Never infer TagRemoved solely from console error `2115-0096` or status `07 41`.
- Never call a field constant from one physical identity.
- Never ask for a reflash without an explicit offline discriminator and expected trace change.

### Skill output

Each run should leave:

```text
dumps/experiments/<timestamp>-<slug>/
  experiment.json
  trace.raw.jsonl
  trace.decoded.json
  diagnostics.before.json
  diagnostics.after.json
  image.before.bin        # when available/authorized
  image.after.bin         # when available/authorized
  comparison.md
```

## Additional hardware that would have helped

The UART bridge and genuine Pro2 were sufficient to solve this task. If broader NFC work continues,
an ISO14443A-capable RF capture/emulation instrument would provide a second observation point at
the tag boundary. It could confirm:

- tag `GET_VERSION`;
- sector-select behavior;
- NTAG I2C SRAM mailbox traffic;
- RF field removal timing;
- whether future figures change the underlying tag family.

This should complement, not replace, console/controller captures. PicoSwitch2 still has to emulate
the controller protocol, so RF evidence must be translated across that boundary.

USBPcap plus a purpose-built Wireshark dissector would also help when a genuine controller can be
connected over USB. The dissector should share the same decoding library as the UART comparator so
field names do not diverge across tools.

## Concrete success metrics for the tooling effort

The tools are successful when:

- a normal NFC investigation requires no manual UART command sequence;
- every hardware run produces a complete, named, hashable artifact bundle;
- a deterministic state bug is reproduced without flashing;
- a new dump is structurally classified before it reaches the console;
- the first semantic divergence between genuine and virtual traces is reported automatically;
- diagnostic counters never require hand-calculated baselines;
- no script contains a maintainer-specific absolute path or COM number;
- no protocol conclusion relies on repeated samples from one identity alone;
- each hardware failure becomes a permanent fixture or a documented refuted hypothesis;
- most bugs require at most one diagnostic flash and one confirming flash.

## Recommended implementation order

1. Extract the NFC state machine into a host-replayable core.
2. Build the semantic trace decoder/comparator around the existing JSONL captures.
3. Wrap UART capture/export into `nfc_lab.ps1` and add structured internal error telemetry.
4. Build the recursive v3 corpus analyzer and checked-in manifest format.
5. Add capture-to-fixture generation and persistence fault injection.
6. Add the portal browser integration harness.
7. Package the stable workflow and scripts as `picoswitch2-nfc-lab`.

The guiding principle is simple: spend hardware time collecting information that cannot be
obtained elsewhere. Parsing, framing, state transitions, allocation validation, image comparison,
and persistence recovery should be settled by deterministic tools before asking the maintainer to
flash or move hardware.
