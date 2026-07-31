# NFC investigation workflow and laboratory tooling

Status: 🟡 In Progress — the core offline laboratory exists; the remaining gaps are listed below.
Audience: engineers, coding agents, and future contributors doing amiibo/NFC reverse engineering.
Companion documents: [`evidence-standards.md`](evidence-standards.md),
[`../LLM/amiibo-v3-investigation-retrospective.md`](../LLM/amiibo-v3-investigation-retrospective.md),
[`../Amiibo-v3.md`](../Amiibo-v3.md).

## Why this exists

The figure-v3 investigation succeeded, but it spent hardware iterations on questions that were
already answerable from data on disk. The retrospective identified the cause precisely: there was
no integrated laboratory workflow, so a console symptom was repeatedly treated as a protocol
diagnosis and answered with a speculative firmware build.

Three failure patterns recurred and each now has a tool that prevents it:

| Pattern that cost iterations | What it produced | Tool that prevents it |
|---|---|---|
| One physical figure treated as representative | A tag-specific SRAM CRC looked like a controller constant | `amiibo_corpus.py` |
| Partial views of multi-chunk transfers | The 83-byte device result read as "32 bytes then zeros" | `ns2_trace.py nfc` |
| Console status treated as a diagnosis | `2115-0096` blamed on removal timing when a record page was rejected | `ns2_trace.py nfc` error pairing |
| Several variables moved in one test | A retracted provenance/signature conclusion | `nfc_lab.ps1` experiment manifest |

The governing rule: **spend hardware time only on information that cannot be obtained any other
way.** Parsing, framing, envelope classification, allocation validation, image comparison, and
corpus structure are deterministic and belong offline.

## The tools

### `tools/amiibo_corpus.py` — corpus analyzer

Recursively classifies 540/572/2048-byte dumps and emits a portable JSON manifest.

```powershell
python tools/amiibo_corpus.py <directory> --json build/amiibo-corpus.json
python tools/amiibo_corpus.py <directory> --retail-key <path>   # optional
python tools/amiibo_corpus.py <directory> --amiiboapi           # optional, never gating
```

Per image: size, SHA-256, CRC32, UID, amiibo identity and variant, format, SRAM window with its
CRC-16/MCRF4XX validity, discovered capability page and generation, sector-0 record start pages,
nonzero ranges, and a runtime-safety verdict with reasons.

Across the corpus: equivalence groups along three independent axes — catalog identity, encrypted
body, and machine SRAM — plus shared-UID warnings.

Exit status is `0` when every amiibo-shaped image is runtime-safe, `1` when any is not. Non-amiibo
binaries in the tree (SPI dumps, controller dumps) are skipped, not failed.

**Result this immediately establishes (2026-07-28):** the 16 Kirby Air Riders dumps are exactly
4 riders × 4 machines. Four encrypted-body groups, four SRAM groups, sixteen distinct (body, SRAM)
pairs, four UIDs each shared by four files. Rider identity is entirely in the encrypted body;
machine identity is entirely in the SRAM window; the two axes are orthogonal. Confidence:
**Confirmed** (byte-exact over the full local corpus). This is what makes "four captures of one
figure" visibly insufficient evidence for calling any field constant.

Cryptographic validity is delegated to the portal's own amiitool port so there is exactly one
implementation:

```powershell
node tools/verify_amiibo_crypto.mjs <file-or-dir> <key_retail.bin> [--json]
```

The key argument is required and is used in memory only. It is never written to a manifest, never
copied into the repository, and never committed.

### `tools/ns2_nfc_semantics.py` — NFC layout library

The single place where Switch 2 NFC wire layout is written down: subcommand names, status payloads,
read and sector descriptors, the 60-byte operation prefix, and the three staged-write envelopes.
`ns2_trace.py` imports it, so the timeline, the per-record decoder, and any future dissector cannot
drift apart on field names. Extend this module rather than decoding bytes inline elsewhere.

Layout authority is the firmware itself; the module header lists the exact headers and sources each
constant mirrors.

### `tools/ns2_trace.py nfc` / `nfc-diff` — transaction timeline and comparator

```powershell
python tools/ns2_trace.py nfc <capture.jsonl>
python tools/ns2_trace.py nfc-diff <genuine.jsonl> <virtual.jsonl> [--ignore-identity]
```

`nfc` reassembles multi-chunk `0x14` staging and `0x15` retrieval before classifying anything,
decodes each read descriptor (including whether it reaches past the 540-byte compatibility view),
names each write envelope with its self-described record list, and pairs every error state with the
operation that was in flight.

Older captures predate full NFC payload retention. The decoder distinguishes tracer truncation
(`captured < length`, reported as `capture_truncated`) from a genuine transport fault (a fully
captured record whose declared length exceeds its body — the signature of the split 88-byte `0x14`
that crashed the console with `2168-0002`). Buffer sizes are reconstructed from the declared chunk
lengths, so a 664-byte escalated read stays recognizable even in a truncated capture.

`nfc-diff` aligns on semantic step keys, so an extra status poll or a different chunk split does not
cascade into false differences, and reports the **first** semantic divergence. A ring buffer rarely
starts two captures at the same lifecycle point, so a leading-only offset is reported as alignment
rather than divergence. `--ignore-identity` normalizes UID, signature, and timeout so two different
figures can be compared for shape — the comparison that proved the Air Riders record pages were
allocation semantics rather than rider identity.

Worked example, entirely from a capture that already existed before the fix:

```text
  150  2732.180ms  stage envelope=extended_update records=['s0:0x04+4', 's0:0xB2+32', 's1:0x65+96']
  159  2881.673ms  status state_name=error

Error states and the operation in flight:
  seq 159: error state (detail 0x41) after seq 156 commit_extended
```

That is the whole King Dedede diagnosis: a different, self-described allocation rejected at the
extended commit — not the tag-removal bug the identical console status suggested.

### `tools/nfc_lab.ps1` — experiment runner

```powershell
./tools/nfc_lab.ps1 -Scenario v3-air-riders-write `
  -Hypothesis 'the capability page survives a power cycle' `
  -Variable 'firmware: persist the capability page in the extended commit' `
  -Expect 'reuse read returns generation 2, no 2115-0096' `
  -Action 'Scan King Dedede in Air Riders, save, then remove the figure' `
  -Baseline dumps/amiibo/genuine-air-riders-postwrite-read-only-2026-07-28.jsonl
```

One command arms the trace, captures before/after diagnostics, prompts for exactly one physical
action, retrieves the trace and tag image, decodes, compares, hashes every artifact, and writes
`experiment.json`. It orchestrates `read_uart_diag.ps1` rather than reimplementing the serial
transport, so UART framing has one owner.

Behavioral commitments worth knowing:

- The port is discovered, never assumed. `-Port` overrides. Motherboard legacy ports
  (`ACPI\PNP0501`) are excluded from auto-selection so a machine with one legacy port refuses
  rather than silently talking to a port that cannot answer.
- `amiibo dump` acknowledges and clears the firmware's dirty-state protection by default; the
  runner passes `-NoAcknowledge` unless `-Acknowledge` is given. An observational run must not
  mutate the state it observes.
- Omitting `-Variable` **prompts** for it rather than warning after the fact — a result that cannot
  be attributed is the failure mode that produced the retracted v3 provenance conclusion, and a
  warning printed once the run is already set up prevents nothing. Answering with an empty line
  records an honest `kind: observation` in `experiment.json` instead; capturing a genuine golden
  path is legitimately observational, and forcing a variable on those only trains people to type
  filler. A hypothesis test is then also asked for its pass/fail discriminator.
- Passing `-Variable` (or `-DwellSeconds` for an unattended run) skips all prompting, so scripted
  use is unaffected.
- `-DryRun` prints the plan without touching the UART.

Artifact bundle:

```text
dumps/experiments/<timestamp>-<slug>/
  experiment.json          hypothesis, variable, git revision + dirty state, verdict, hashes
  trace.raw.jsonl          validated capture
  trace.decoded.txt        transaction timeline
  diagnostics.before.json  amiibo status / v3diag / journal / trace status
  diagnostics.after.json
  image.before.bin         when a tag is loaded
  image.after.bin
  comparison.md            first semantic divergence from the baseline
```

### `src/nfc/ns2_amiibo_v3_runtime.c` — host-replayable v3 state machine

Status: ✅ extracted 2026-07-29. Behavior unchanged; verified static, build, and on hardware
(`dumps/experiments/20260729-101834-v3-post-extraction/`: scan → in-game save → remove → rescan,
firmware `errors:0`).

The figure-v3 command state machine used to be `ns2_v3_serve()` plus twenty
file-scope statics inside `src/switch_pro2/switch_pro2.c`, where it could not run
without USB, flash, BTstack, and physical time. The 540-byte path had been
replayable through `ns2_virtual_nfc_runtime_dispatch()` since it was written; v3
now has the same shape:

```c
bool ns2_amiibo_v3_runtime_step(runtime, host, now_ms, generation,
                                subcommand, request, request_size,
                                image, effects);
```

All state lives in one caller-owned struct, time is a parameter, and the three
side effects that reach durable state — apply a console write, set presented,
query persist-pending — go through a small `ns2_amiibo_v3_host_t` interface. The
personality keeps only the transport: fetch the image, step the runtime, send
the response. The command flow itself is byte-for-byte what it was, which is why
the interface was chosen over a pure effects-queue design that would have had to
restructure the commit path.

`tools/test_ns2_amiibo_v3_runtime.c` replays the sequences a real console sends,
reconstructed from the captures in `dumps/`:

| Scenario | What it pins |
|---|---|
| Recognition read | 540 descriptor → escalated 4-block descriptor → 600 B and 664 B buffers, prefix byte 18 = `0x06`, extended block served from the raw 2 KB image, 83 B device result carrying the tag's own SRAM |
| Air Riders write lifecycle | 355 B clear → `0x20`, a Stop that must **not** eject, 167 B update → `0x20` with the capability page retained, 454 B write → `0x08` |
| Error discrimination | Four distinct internal causes behind one console-facing `07 41` |
| Reuse sector read | `0x1E` bare-ACKs, transitions to state `0x15`, publishes a 196 B result |
| Persistence-gated eject | Removal waits for the flash flush; re-presentation suppressed for 3 s |
| Generation edge | A portal upload mid-transaction abandons the in-flight write instead of committing over the newer image |
| Unknown subcommand | Bare-ACKed with no state change, so the trace still shows it |

Writing the replay immediately caught a faithfulness bug in the *test's* fake
store, which is worth recording because it is a real property of the runtime:
after a commit it optimistically sets `observed_generation` to
`operation_generation + 1`, because the real store increments when it accepts a
write. A fake that did not advance made the next command look like a portal
upload. Any future fake must model that contract.

#### Internal error reasons

Every v3 failure reaches the console as status `0x07`, detail `0x41`, which it
renders as `2115-0096`. In this investigation that single value was produced
first by a genuine tag-removal timing bug and later by a fail-closed
record-layout rejection, and the second was misdiagnosed as the first.
`ns2_amiibo_v3_error_t` now records which rule actually fired, alongside the
specific `ns2_virtual_nfc_result_t` and the `0x14` stage offset where relevant:

```text
read_descriptor  sector_read  stage_framing  stage_not_active  stage_chunk
commit_state     commit_validation           commit_apply
```

Read it on hardware with `amiibo v3diag`:

```json
{"amiibo":"v3diag", ... ,"errors":1,"last_error":"commit_validation",
 "last_error_code":7,"last_error_sub":8,
 "last_error_result":"invalid write record","last_error_offset":0}
```

`commit_validation` with result `invalid write record` is a fail-closed record
rejection. `commit_apply` is a lost race with a portal upload. `stage_framing`
is a transport fault. These are three different bugs that were previously one
observable value.

#### `07 41` is also the removal signal

The very first hardware run of the extracted runtime demonstrated the counter's
value in an unplanned way. The decoded trace reported one error state; the
firmware reported `errors:0`. Both were right:

```text
226  commit_write
229  status write_committed (0x05)
230  stop
233  status error (07/41)      <- TagRemoved, not a failure
238  poll                      <- rescan succeeds
241  status tag_present
```

`finish_committed_eject()` deliberately sets status `0x07` / detail `0x41` to
report logical removal after a committed write, because the console needs that
edge to leave its amiibo UI. A healthy write/remove/rescan cycle therefore
always contains one. `error_context()` now separates removal edges — a `0x07`
whose preceding operation is a Stop that followed a commit — from genuine
failures, and `nfc_lab.ps1` cross-checks the decoded trace against `v3diag`'s
own counter, printing `DECODER DISAGREES WITH FIRMWARE` when they diverge.

The lesson is the retrospective's own, reproduced inside the analysis tooling:
**a wire value is not a diagnosis.** When the trace and the firmware disagree,
the firmware is the authority and the decoder is the suspect.

### `.claude/skills/picoswitch2-nfc-lab` — the enforced workflow

A Claude Code skill that loads on any NFC/amiibo request and drives the phases below using the real
scripts. It is deliberately not a reminder to be careful; every phase names a command to run.

## The workflow

**Phase 0 — classify the layer.** USB framing, command parsing, report-state sequencing, tag
memory bytes, image crypto/integrity, persistence, game lifecycle, or portal transport. If the
evidence cannot pick a layer, improve instrumentation before changing code.

**Phase 1 — inventory the static evidence.** Run the corpus analyzer on every dump involved.

**Phase 2 — decode what you already have.** Every existing capture in `dumps/` and `dumps/amiibo/`
is available to `nfc` and `nfc-diff` right now.

**Phase 3 — exhaust the host tests.** Deterministic parsing, bounds, and record-layout bugs belong
in `tools/test_ns2_amiibo_v3*.c`, `test_ns2_virtual_nfc*.c`, `test_ns2_nfc_semantics.py`, and
`test_amiibo_corpus.py` — not on hardware.

**Phase 4 — isolate one hypothesis.** State the hypothesis, the one intended variable, the expected
wire difference in the decoder's vocabulary, the pass/fail discriminator, and why it cannot be
answered offline. If you cannot state all five, return to Phase 0.

**Phase 5 — one instrumented hardware action.** The maintainer performs one described action. Two
actions means two experiments.

**Phase 6 — convert the result into permanent coverage.** A golden fixture, a fail-closed fixture,
or an entry in [`../experiments/refuted-hypotheses.md`](../experiments/refuted-hypotheses.md).

## Standing safety rules

- Never whitelist a figure, UID, or product to cover an unexplained protocol field.
- Never weaken a bounds check to accept a new dump.
- Never call a field constant from one physical identity. Minimum evidence is two independent
  identities or a parser-backed structural explanation.
- **A UID identifies a rider, not a run.** On a physical dump the UID is the tag's hardware serial,
  so two dumps sharing one really are the same figure. On a *virtual* serve it is only whatever
  image happens to be loaded, and the four machine variants of a rider all carry the same UID. Two
  hardware runs reporting the same UID may well have served different images. Identify the image by
  its machine-SRAM window (`0x3C0..0x3FF`) or its payload CRC, never by UID alone — this was
  misread on 2026-07-29 to claim one figure had changed its allocation, when in fact two different
  machine variants had been scanned.
- Never infer TagRemoved from console error `2115-0096` or status `07 41` alone.
- Never ship, copy, or commit `key_retail.bin`; the Virtual Amiibo library is import-only.
- Never overwrite a maintainer's source dump; write a new artifact.
- Never request a reflash without an offline discriminator and a named expected trace change.
- Never embed a local path or COM number in a tool.

## Remaining gaps

| Gap | Impact | Status |
|---|---|---|
| The report NFC-state field is not traced | State edges are inferred only from observed `0x05` replies | ⬜ Not started |
| No capture-to-fixture generator | Fixtures are still hand-written into `tools/test_*.c` | ⬜ Not started |
| No persistence fault injector | Power-loss recovery still needs physical testing | ⬜ Not started |
| No portal browser integration harness | Portal and protocol work still share a change surface | ⬜ Not started |
| No ISO14443A RF instrument | No second observation point at the tag boundary | Optional hardware |
