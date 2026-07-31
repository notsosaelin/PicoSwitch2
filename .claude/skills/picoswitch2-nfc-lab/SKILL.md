---
name: picoswitch2-nfc-lab
description: Investigate PicoSwitch2 NFC, amiibo, and Virtual Amiibo behavior using the offline lab tools before asking for hardware. Use for any work touching amiibo (540/572/2048 v3), NTAG I2C Plus 2K, Kirby Air Riders figures, NFC command 0x01, virtual tag serving, native controller tag passthrough, UART NFC captures, console NFC errors (2115-0096, 2168-0002, "This isn't an amiibo"), or the Virtual Amiibo portal's tag pipeline.
---

# PicoSwitch2 NFC laboratory

The v3 amiibo investigation succeeded but cost far more hardware iterations than
the evidence required. The retrospective is
`docs/LLM/amiibo-v3-investigation-retrospective.md`. Its central finding: console
symptoms were treated as protocol diagnoses, and questions answerable offline
were sent to hardware anyway.

**The rule this skill enforces: spend hardware time only on information that
cannot be obtained any other way.** Parsing, framing, envelope classification,
allocation validation, image comparison, and corpus structure are all settled
before the maintainer touches the console.

## Read before changing anything

1. `AGENTS.md` — invariants, build and verification commands
2. `STATUS.md` — what is currently validated, and at what level
3. `docs/Amiibo-v3.md` — the authoritative v3 protocol reference
4. `docs/switch2/nfc-implementation.md` and `docs/switch2/nfc-protocol-inventory.md`
5. The experiment document linked from the active blocker

Repository over memory. If a recalled fact and the repository disagree, the
repository wins and the memory is wrong.

## Phase 0 — classify the layer before proposing a change

A console symptom names no layer. Error `2115-0096` with status `07 41` was
produced by a genuine removal-timing bug *and later* by a fail-closed record
rejection. Identify which of these is failing:

| Layer | Evidence that identifies it |
|---|---|
| USB stream framing | a command's declared length disagrees with delivered bytes; `nfc` timeline emits a `WARNING` |
| NFC command parsing | `stage envelope=unknown`, or a `malformed` field in the decoded step |
| Report-state / status sequencing | the console stops or retries with no error; a status edge is missing |
| Tag view / memory bytes | wrong page range served, `needs_v3_source=True` served from the 540 view |
| Image crypto or integrity | `verify_amiibo_crypto.mjs` says INVALID, or `sram_crc_valid=false` |
| Persistence | `amiibo journal` generation does not advance, or does not survive a power cycle |
| Game lifecycle | the extended `0x20` operations, Stop timing, reuse, re-presentation |
| Portal transport / storage | the library, IndexedDB, Sync, or import path |

If the available evidence cannot pick a layer, **improve instrumentation
first**. Do not flash a guess.

## Phase 1 — inventory the static evidence (offline, always)

```powershell
python tools/amiibo_corpus.py <dump-directory> --json build/amiibo-corpus.json
python tools/amiibo_corpus.py <dump-directory> --retail-key <path-to-key> # optional
```

Answers, before hardware: which images are structurally serviceable; how many
*distinct* images there really are along the identity, encrypted-body, and
machine-SRAM axes; which are unwritten versus console-written; where a written
image put its allocation-relative capability page.

Run this on any new dump. It is what makes "one figure is representative"
visibly unsafe: the 16 Air Riders dumps are 4 riders × 4 machines sharing 4
UIDs, so four "independent" captures of one figure prove nothing about a field
being constant.

Crypto validity is delegated to the portal's own amiitool port:

```powershell
node tools/verify_amiibo_crypto.mjs <file-or-dir> <key_retail.bin>
```

Never add a second crypto implementation. Never copy or commit `key_retail.bin`.

## Phase 2 — decode every capture you already have

```powershell
python tools/ns2_trace.py nfc <capture.jsonl>          # transaction timeline
python tools/ns2_trace.py decode <capture.jsonl>       # per-record view
```

The `nfc` timeline reassembles multi-chunk `0x14` staging and `0x15` retrieval,
classifies each write envelope (`device_command`, `write`, `extended_clear`,
`extended_update`) with its self-described record list, and pairs every error
state with the operation that was in flight. Existing captures live in `dumps/`
and `dumps/amiibo/`.

Compare against a genuine or known-good capture before writing code:

```powershell
python tools/ns2_trace.py nfc-diff <genuine.jsonl> <virtual.jsonl>
python tools/ns2_trace.py nfc-diff <a.jsonl> <b.jsonl> --ignore-identity  # different figures
```

It reports the *first semantic divergence*, normalizing timing, chunk splits,
and ring-buffer alignment. Layout lives in `tools/ns2_nfc_semantics.py`; extend
that module rather than decoding bytes inline anywhere else.

## Phase 3 — exhaust the host tests

```powershell
cmake --build build\pico2_w --config Release --parallel
$tests = Get-ChildItem build\host-tests -File -Filter 'test_*.exe'
foreach ($test in $tests) { & $test.FullName; if ($LASTEXITCODE -ne 0) { throw "$($test.Name) failed" } }
python tools/test_ns2_trace.py
python tools/test_ns2_nfc_semantics.py
python tools/test_amiibo_corpus.py
```

Both NFC state machines are host-replayable, so a deterministic parsing, bounds,
timing, state, or record-layout bug must be reproduced offline before hardware:

- v3: `tools/test_ns2_amiibo_v3_runtime.c` drives the real command sequences
  through `ns2_amiibo_v3_runtime_step()` with a fake clock and a fake store that
  can refuse an apply or report a pending flush.
- 540: `tools/test_ns2_virtual_nfc_runtime.c` over
  `ns2_virtual_nfc_runtime_dispatch()`.
- Layout coverage: `test_ns2_amiibo_v3.c`, `test_ns2_amiibo_v3_write.c`,
  `test_ns2_virtual_nfc.c`.

When a v3 run fails on hardware, read the internal cause instead of guessing
from the console error — `amiibo v3diag` reports `last_error`,
`last_error_result`, `last_error_sub`, and `last_error_offset`. `2115-0096`
alone still names no layer.

**If the decoded trace and `v3diag` disagree, the firmware wins.** Status `0x07`
/ detail `0x41` is both a failure state and the deliberate TagRemoved signal
after a committed write, so a healthy write/remove/rescan cycle contains one by
design. `errors:0` with an apparent error state in the trace means the decoder
is wrong, not the run.

## Phase 4 — one hypothesis, one variable

Before asking for a flash, be able to state all five:

1. the hypothesis;
2. the **one** intended variable;
3. the expected wire difference, in the decoder's own vocabulary;
4. the pass/fail discriminator;
5. why this cannot be answered offline.

Cannot state all five? Go back to Phase 0. The retracted v3 provenance
conclusion came from a test that moved a prefix byte, a device-command
implementation, a status payload shape, and the source dump together, then
attributed the result to the dump.

## Phase 5 — one instrumented hardware action

```powershell
./tools/nfc_lab.ps1 -Scenario <slug> `
  -Hypothesis '<what you believe>' `
  -Variable '<the one thing that changed>' `
  -Expect '<the discriminator>' `
  -Action '<the single physical action>' `
  -Baseline dumps/amiibo/<a-genuine-capture>.jsonl
```

The runner owns port discovery, git provenance, before/after diagnostics, trace
arming, the image download, decoding, comparison, and hashing. The maintainer
performs exactly one described action and presses Enter. If you need two
actions, you need two experiments.

Only `-Scenario` and `-Action` are required; the runner prompts for the intended
variable and the discriminator when they are missing. Supply them on the command
line anyway when you already know them — you are the one who knows what changed,
and the maintainer should not have to reconstruct it at the prompt. An empty
variable records `kind: observation`, which is correct for a golden-path capture
and wrong for anything you are trying to attribute.

Rules for live hardware:

- Do not run UART mutations until the maintainer confirms the expected
  personality is connected and ready.
- Never assume a COM port; the runner discovers it (`-Port` overrides).
- `amiibo dump` acknowledges and clears dirty protection by default; the runner
  passes `-NoAcknowledge` unless `-Acknowledge` is given. Do not change that.
- `amiibo status` cannot identify which v3 image is loaded. Use `amiibo journal`
  and CRC-match the payload CRC (header bytes 16..19, little-endian) offline.

## Phase 6 — convert the result into permanent coverage

A hardware run is not finished until it becomes one of:

- a golden positive fixture in the relevant `tools/test_*.c`;
- a negative / fail-closed fixture;
- an entry in `docs/experiments/refuted-hypotheses.md`.

Then update `STATUS.md`, `docs/Amiibo-v3.md`, and `CHANGELOG.md` if behavior
changed. State separately which of these three you actually verified: static
tests, build success, hardware validation. Build success is not hardware
validation.

## Safety rules

- **Never** whitelist a figure, UID, or product to cover an unexplained field.
  If a field varies, find what describes it — King Dedede proved the record
  pages were allocation semantics, not identity.
- **Never** weaken a bounds check just to accept a new dump.
- **Never** call a field constant from one physical identity. Minimum evidence
  is two independent identities or a parser-backed structural explanation.
- **Never** infer TagRemoved from console error `2115-0096` or status `07 41`.
- **Never** ship, copy, or commit `key_retail.bin`; never re-add amiibo
  generation to the portal (import-only, by the owner's decision).
- **Never** overwrite a maintainer's source dump; write a new artifact.
- **Never** ask for a reflash without an offline discriminator and an expected,
  named trace change.
- **Never** embed a local path or COM number in a tool.
- Preserve unrelated working-tree changes. Commit, push, or release only when
  the maintainer asks.

## Artifact layout

```text
dumps/experiments/<timestamp>-<slug>/
  experiment.json          hypothesis, variable, git revision, verdict, hashes
  trace.raw.jsonl          validated capture
  trace.decoded.txt        transaction timeline
  diagnostics.before.json  amiibo status / v3diag / journal / trace status
  diagnostics.after.json
  image.before.bin         when a tag is loaded
  image.after.bin
  comparison.md            first semantic divergence from the baseline
```

## Known gaps — worth fixing before the next investigation

- The report NFC-state field is not traced, so state edges are inferred only
  from observed `0x05` replies.
- No capture-to-fixture generator: replay scenarios are still written by hand
  into `tools/test_ns2_amiibo_v3_runtime.c` rather than generated from a trace.
- No persistence fault injector, so power-loss recovery still needs physical
  testing.
