# PicoSwitch2 Documentation

This directory is the durable technical record for PicoSwitch2. Code defines current behavior;
protocol documents explain that behavior and its evidence; experiment reports preserve how claims
were tested. Conversation transcripts and temporary handoff files are not authorities.

## Start here

| Need | Document |
|---|---|
| Current project state | [`../STATUS.md`](../STATUS.md) |
| Forward roadmap | [`../PLAN.md`](../PLAN.md) |
| Runtime architecture | [`architecture/overview.md`](architecture/overview.md) |
| Compatibility results | [`status/compatibility-matrix.md`](status/compatibility-matrix.md) |
| Evidence and RE rules | [`re-methodology/evidence-standards.md`](re-methodology/evidence-standards.md) |
| What we discovered vs. inherited | [`provenance.md`](provenance.md) |
| Bluetooth implementation | [`bluetooth/btstack-implementation.md`](bluetooth/btstack-implementation.md) |
| Battery passthrough and validation matrix | [`bluetooth/battery-passthrough.md`](bluetooth/battery-passthrough.md) |
| DualSense live-audio implementation/result | [`../DS5-NS2_AUDIO.md`](../DS5-NS2_AUDIO.md) |
| DualSense audio investigation history | [`../AUDIO-INVESTIGATION.md`](../AUDIO-INVESTIGATION.md) |
| Controller sleep research | [`bluetooth/controller-sleep-research.md`](bluetooth/controller-sleep-research.md) |
| Generic Bluetooth gamepad quirks | [`bluetooth/generic-gamepad-quirks.md`](bluetooth/generic-gamepad-quirks.md) |
| Retro Fighters BattlerGC Pro | [`bluetooth/retrofighters-battlergc-pro.md`](bluetooth/retrofighters-battlergc-pro.md) |
| 8BitDo Ultimate Bluetooth paddles | [`../8Bitdo/docs/8bitdo-ultimate-bluetooth-paddles.md`](../8Bitdo/docs/8bitdo-ultimate-bluetooth-paddles.md) |
| Pro Controller 2 USB protocol | [`switch2/usb-spec.md`](switch2/usb-spec.md) |
| Genuine Pro Controller 2 headset audio | [`switch2/pro2-headset-audio.md`](switch2/pro2-headset-audio.md) |
| Native Pro Controller 2 motion result | [`experiments/native-pro2-motion-passthrough-2026-07-21.md`](experiments/native-pro2-motion-passthrough-2026-07-21.md) |
| UART trace capture, decoding, and comparison | [`switch2/uart-trace-tooling.md`](switch2/uart-trace-tooling.md) |
| NSO GameCube protocol | [`switch2-gc/protocol.md`](switch2-gc/protocol.md) |
| Joy-Con 2 protocol | [`switch2-joycon2/protocol.md`](switch2-joycon2/protocol.md) |

## Authority order

When documents disagree, use this order:

1. Reproducible captures, dumps, and hardware tests
2. Current source and host tests
3. Current protocol/architecture documents
4. Current `STATUS.md` and `PLAN.md`
5. Historical experiment and archive documents
6. Third-party implementations and undocumented recollections

An experiment report is authoritative for what was observed during that experiment, not for the
repository's present implementation status. Archived plans and status logs may contain hypotheses
or pending items that were resolved later.

## Directory map

### `architecture/`

Current component ownership, data flow, concurrency, and lifecycle documentation.

### `bluetooth/`

BTstack/joypad-os integration, controller identity, pairing/reconnection, profiles, and wake design.

### `switch2/`

Switch 2 Pro Controller 2 USB/BLE protocol, reports, descriptors, motion, NFC, audio, and open
features.

### `switch2-gc/`

Official NSO GameCube output personality: descriptors, command protocol, mapping, and rumble.

### `switch2-joycon2/`

Joy-Con 2 Left/Right output personalities: descriptors, reports, mapping, and open questions.

### `experiments/`

Date-stamped methods and results. Preserve negative results and refuted hypotheses; update only to
fix provenance, dead links, or an explicit factual error. Do not rewrite an old observation merely
because the current implementation changed.

### `status/`

Current compatibility matrices and release verification records.

### `re-methodology/`

Evidence classification, capture discipline, source-comment policy, and reproducibility rules.

### `archive/`

Superseded roadmaps, implementation narratives, and migration plans. These are historical context,
not current instructions.

## External reference repositories

`nso-gc-refs/` is a local workspace for nested upstream Git repositories. It is intentionally not
part of PicoSwitch2's source history. Cite upstream URL, branch, and commit in an experiment report;
do not edit or stage the nested repositories as if their Markdown were first-party documentation.

## Documentation maintenance rules

- Keep `STATUS.md` concise and current; move chronological narratives to `archive/` or
  `experiments/`.
- Keep `PLAN.md` forward-looking; remove completed implementation logs.
- Put packet layouts and descriptor facts beside the protocol they describe.
- Put hypotheses, competing explanations, and proposed experiments in documentation—not in runtime
  source comments.
- Keep source comments for invariants, ownership, non-obvious safety constraints, exact offsets,
  and short evidence links.
- Every new protocol claim must name its confidence tier and validation source.
- Relative links must resolve from the file containing them.
