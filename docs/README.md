# PicoSwitch2 Documentation

This directory is the durable technical record for PicoSwitch2. Code defines current behavior;
protocol documents explain that behavior and its evidence; experiment reports preserve how claims
were tested. Conversation transcripts and temporary handoff files are not authorities.

## Start here

| Need | Document |
|---|---|
| Fresh coding-agent session | [`../AGENTS.md`](../AGENTS.md) |
| Concise current handoff | [`LLM/current-context.md`](LLM/current-context.md) |
| Current project state | [`../STATUS.md`](../STATUS.md) |
| Forward roadmap | [`../PLAN.md`](../PLAN.md) |
| Runtime architecture | [`architecture/overview.md`](architecture/overview.md) |
| USB Serial and Config-only BLE management | [`architecture/config-transports.md`](architecture/config-transports.md) |
| Compatibility results | [`status/compatibility-matrix.md`](status/compatibility-matrix.md) |
| Evidence and RE rules | [`re-methodology/evidence-standards.md`](re-methodology/evidence-standards.md) |
| What we discovered vs. inherited | [`provenance.md`](provenance.md) |
| Bluetooth implementation | [`bluetooth/btstack-implementation.md`](bluetooth/btstack-implementation.md) |
| Battery passthrough and validation matrix | [`bluetooth/battery-passthrough.md`](bluetooth/battery-passthrough.md) |
| DualSense audio implementation and research | [`switch2/audio-passthrough-research.md`](switch2/audio-passthrough-research.md) |
| DualSense motion translation | [`bluetooth/dualsense-motion.md`](bluetooth/dualsense-motion.md) |
| Switch 1 motion reference | [`bluetooth/switch1-motion.md`](bluetooth/switch1-motion.md) |
| Wii motion reference | [`bluetooth/wii-motion.md`](bluetooth/wii-motion.md) |
| Controller sleep research | [`bluetooth/controller-sleep-research.md`](bluetooth/controller-sleep-research.md) |
| Generic Bluetooth gamepad quirks | [`bluetooth/generic-gamepad-quirks.md`](bluetooth/generic-gamepad-quirks.md) |
| Retro Fighters BattlerGC Pro | [`bluetooth/retrofighters-battlergc-pro.md`](bluetooth/retrofighters-battlergc-pro.md) |
| 8BitDo Ultimate Bluetooth paddles | [`../8Bitdo/docs/8bitdo-ultimate-bluetooth-paddles.md`](../8Bitdo/docs/8bitdo-ultimate-bluetooth-paddles.md) |
| Pro Controller 2 USB protocol | [`switch2/usb-spec.md`](switch2/usb-spec.md) |
| Console command surface | [`switch2/command-surface.md`](switch2/command-surface.md) |
| Controller firmware identity | [`switch2/firmware-versioning.md`](switch2/firmware-versioning.md) |
| NFC implementation design | [`switch2/nfc-implementation.md`](switch2/nfc-implementation.md) |
| Figure-v3 read/write working record | [`Amiibo-v3.md`](Amiibo-v3.md) |
| Amiibo identity, differentiation, and generation | [`switch2/amiibo-identity-and-generation.md`](switch2/amiibo-identity-and-generation.md) |
| Kirby Air Riders extended amiibo format | [`switch2/kirby-air-riders-extended-amiibo.md`](switch2/kirby-air-riders-extended-amiibo.md) |
| NFC protocol evidence | [`switch2/nfc-protocol-inventory.md`](switch2/nfc-protocol-inventory.md) |
| NFC feasibility/resource audit | [`experiments/nfc-feasibility-audit-2026-07-25.md`](experiments/nfc-feasibility-audit-2026-07-25.md) |
| Virtual amiibo offline foundation | [`experiments/virtual-amiibo-foundation-2026-07-25.md`](experiments/virtual-amiibo-foundation-2026-07-25.md) |
| Genuine Pro2 native NFC read | [`experiments/pro2-native-nfc-read-2026-07-25.md`](experiments/pro2-native-nfc-read-2026-07-25.md) |
| Virtual Amiibo real-console read | [`experiments/virtual-amiibo-read-validation-2026-07-25.md`](experiments/virtual-amiibo-read-validation-2026-07-25.md) |
| Smash native NFC write attempt | [`experiments/smash-native-nfc-write-attempt-2026-07-25.md`](experiments/smash-native-nfc-write-attempt-2026-07-25.md) |
| Virtual Amiibo write reconstruction | [`experiments/virtual-amiibo-write-evidence-2026-07-25.md`](experiments/virtual-amiibo-write-evidence-2026-07-25.md) |
| Virtual write crash / USB RX correction | [`experiments/virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md`](experiments/virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md) |
| Virtual write/eject/re-present/export validation | [`experiments/virtual-amiibo-lifecycle-validation-2026-07-25.md`](experiments/virtual-amiibo-lifecycle-validation-2026-07-25.md) |
| Virtual Amiibo power-loss persistence and clean/used model | [`experiments/virtual-amiibo-persistence-and-library-model-2026-07-25.md`](experiments/virtual-amiibo-persistence-and-library-model-2026-07-25.md) |
| Figure-v3 full SRAM response and downloaded-dump validation | [`experiments/v3-full-sram-response-validation-2026-07-28.md`](experiments/v3-full-sram-response-validation-2026-07-28.md) |
| Figure-v3 dynamic Air Riders allocation | [`experiments/v3-air-riders-dynamic-allocation-2026-07-28.md`](experiments/v3-air-riders-dynamic-allocation-2026-07-28.md) |
| Config-mode CDC-only migration | [`experiments/config-cdc-only-migration-2026-07-25.md`](experiments/config-cdc-only-migration-2026-07-25.md) |
| Serial-number structure | [`switch2/serial-generation.md`](switch2/serial-generation.md) |
| Genuine Pro Controller 2 headset audio | [`switch2/pro2-headset-audio.md`](switch2/pro2-headset-audio.md) |
| Native Pro Controller 2 motion result | [`experiments/native-pro2-motion-passthrough-2026-07-21.md`](experiments/native-pro2-motion-passthrough-2026-07-21.md) |
| UART trace capture, decoding, and comparison | [`switch2/uart-trace-tooling.md`](switch2/uart-trace-tooling.md) |
| UART native-motion `0x28`/magnetic analysis | [`switch2/uart-magprobe.md`](switch2/uart-magprobe.md) |
| NSO GameCube protocol | [`switch2-gc/protocol.md`](switch2-gc/protocol.md) |
| Joy-Con 2 protocol | [`switch2-joycon2/protocol.md`](switch2-joycon2/protocol.md) |
| Joy-Con 2 Bluetooth mouse bridge | [`switch2-joycon2/mouse-mode.md`](switch2-joycon2/mouse-mode.md) |

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

### `LLM/`

Concise, current continuation notes for fresh agent sessions. These are navigation aids, not a
replacement for source, captures, `STATUS.md`, or protocol evidence.

### `archive/`

Superseded roadmaps, implementation narratives, and migration plans. These are historical context,
not current instructions. Archived Markdown filenames end in `.archived.md`, keeping them readable
and searchable while making their lifecycle status explicit in links and file listings.

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
