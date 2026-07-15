# Reverse-Engineering Evidence Standards

## Confidence tiers

| Tier | Meaning | Acceptable basis |
|---|---|---|
| Confirmed | Directly observed and reproducible | This project's capture, dump, hardware test, or byte-exact test against such evidence |
| Strong | Specific, checkable evidence but not independently reproduced here | Named upstream capture/source at a pinned revision; multiple consistent sources |
| Hypothesis | Plausible interpretation awaiting a discriminating test | Structural similarity, incomplete capture, third-party behavior |
| Unknown | Insufficient evidence | Unmapped bytes, unexplained timing, missing transaction |
| Refuted | A controlled observation contradicts the claim | Preserve in `experiments/refuted-hypotheses.md` |

Implementation status and evidence status are separate. Code may implement a provisional behavior;
that does not make the wire semantics Confirmed.

## Protocol-document requirements

For each protocol surface, document:

- Description and purpose
- Packet/report layout and length
- Direction and transport
- Inputs and outputs
- Current implementation
- Confidence tier per claim
- Capture/dump/source provenance
- Validation status
- Remaining unknowns
- Smallest useful next experiment

## Experiment format

Every new experiment report should contain:

1. Question
2. Competing hypotheses
3. Method and environment
4. Exact artifacts and hashes/revisions
5. Results
6. Conclusion
7. Explicit non-claims
8. Remaining questions

Define in advance what observation would support or reject each hypothesis. Preserve negative
results so future work does not repeat dead ends.

## Source-comment policy

Runtime source comments should explain:

- Ownership and concurrency invariants
- Exact packet offsets, lengths, flags, and descriptor deviations
- Safety constraints and failure consequences
- Why an apparently simpler change is unsafe
- A short link to the durable evidence document

Runtime source comments should not contain:

- Multi-paragraph investigation history
- Ranked theories or competing hypotheses
- Proposed experiments
- Chat/task instructions
- Claims that are meaningful only with a date-stamped hardware session

Move those items to the relevant protocol or experiment document and leave a concise source pointer.
Field names such as `unknown` and comments marking genuinely unmapped bytes are implementation facts
and should remain.

## External sources

- Prefer primary captures, dumps, datasheets, and upstream source.
- Pin repository URL, branch, and commit.
- Distinguish another implementation's behavior from Nintendo protocol evidence.
- Never edit nested reference repositories to make them agree with PicoSwitch2.
- Respect per-device serials, Bluetooth addresses, keys, and other sensitive identity material;
  use fictionalized values in firmware and public examples.

## Promoting a claim

Before changing a confidence tier:

1. Locate the raw artifact.
2. Re-run or independently reproduce the decoding.
3. Verify the current code consumes/produces the claimed bytes.
4. Record hardware context.
5. Update the protocol document and compatibility matrix together.

Conversation statements are useful leads, but durable promotion requires a repository record.
