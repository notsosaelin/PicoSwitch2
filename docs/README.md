# PicoSwitch2 Documentation

The durable technical record for PicoSwitch2. Code defines current behavior;
protocol documents explain that behavior and its evidence; experiment records
preserve how each claim was tested. Conversation transcripts and temporary
handoff files are not authorities.

## Start here

| Need | Document |
| --- | --- |
| What the project is, how to install and build | [`../README.md`](../README.md) |
| Where things stand right now | [`../STATUS.md`](../STATUS.md) |
| Where it is going | [`../PLAN.md`](../PLAN.md) |
| What has physically been validated | [`status/compatibility-matrix.md`](status/compatibility-matrix.md) |
| How the firmware is organised | [`../firmware/README.md`](../firmware/README.md), [`architecture/overview.md`](architecture/overview.md) |
| How to run the tests | [`../tests/README.md`](../tests/README.md) |
| Contributing | [`../CONTRIBUTING.md`](../CONTRIBUTING.md) |
| A fresh coding-agent session | [`../AGENTS.md`](../AGENTS.md), then [`LLM/current-context.md`](LLM/current-context.md) and [`agents/`](agents/) |

## Architecture and design

Why the system is shaped the way it is.

- [Runtime architecture overview](architecture/overview.md): cores, data flow, ownership
- [Management](management/README.md): the logical contract, transports, clients, conformance, [decisions](management/decisions/)
- [USB Serial and BLE management carriers](architecture/config-transports.md)
- [Bridge contract](bridge/PROTOCOL.md) and [implementing a platform backend](bridge/PLATFORM_BACKEND.md)
- [Native Android companion](android-companion.md), and the [Windows companion](../companions/windows/docs/README.md)
- [Composable amiibo library](architecture/amiibo-composable-library-hld.md), [KB/M profile system](architecture/kbm-profile-system-hld.md), [companion library interaction](architecture/companion-library-interaction.md)
- [Single-ACL multiplane companion BLE research](architecture/companion-multiplane-ble-architecture-research-2026-09-04.md)
- [What we discovered versus inherited](provenance.md)

## Protocol and subsystem reference

What the console and controllers actually do, with evidence and confidence.

**Switch 2 Pro Controller 2** ([`switch2/`](switch2/))
- [USB protocol](switch2/usb-spec.md), [console command surface](switch2/command-surface.md), [controller-side command atlas](switch2/controller-command-atlas.md)
- [BLE protocol inventory](switch2/ble-controller-protocol-inventory.md), [firmware identity](switch2/firmware-versioning.md), [serial numbers](switch2/serial-generation.md)
- [Safe Mode](switch2/controller-safe-mode.md), [2 MB NVM/SPI map](switch2/controller-nvm-map.md)
- Motion: [report `0x09`](switch2/report-0x09-motion.md), [UART `0x28` analyzer](switch2/uart-magprobe.md)
- Audio: [DualSense audio research](switch2/audio-passthrough-research.md), [genuine headset audio](switch2/pro2-headset-audio.md)
- NFC: [implementation](switch2/nfc-implementation.md), [protocol evidence](switch2/nfc-protocol-inventory.md), [amiibo identity](switch2/amiibo-identity-and-generation.md), [figure-v3 (Kirby Air Riders)](Amiibo-v3.md)
- [UART trace capture and decoding](switch2/uart-trace-tooling.md)

**Other personalities**
- [NSO GameCube](switch2-gc/protocol.md)
- [Joy-Con 2](switch2-joycon2/protocol.md) and its [Bluetooth mouse bridge](switch2-joycon2/mouse-mode.md)

**Bluetooth input** ([`bluetooth/`](bluetooth/README.md))
- [BTstack implementation](bluetooth/btstack-implementation.md), [generic gamepad quirks](bluetooth/generic-gamepad-quirks.md), [battery passthrough](bluetooth/battery-passthrough.md)
- [Keyboard and mouse input](bluetooth/keyboard-mouse-input.md), [Android controller bridge](bluetooth/android-controller-bridge.md)
- Motion sources: [DualSense](bluetooth/dualsense-motion.md), [Switch 1](bluetooth/switch1-motion.md), [Wii](bluetooth/wii-motion.md)
- [Controller sleep](bluetooth/controller-sleep-research.md), [Retro Fighters BattlerGC Pro](bluetooth/retrofighters-battlergc-pro.md), [validation matrix](bluetooth/VALIDATION.md)

## Methods

How to investigate without guessing.

- [Evidence standards](re-methodology/evidence-standards.md): confidence tiers and what promotes a claim
- [Controller protocol lab](re-methodology/controller-protocol-lab.md): shared capture and A/B tooling
- [NFC investigation workflow](re-methodology/nfc-investigation-workflow.md)
- [Specialist agent briefs](agents/): common, motion, rumble, Android

## Records

- [Experiments](experiments/README.md): dated records by subsystem, starting with the [refuted hypotheses](experiments/refuted-hypotheses.md)
- [Archive](archive/): superseded plans, status logs and implementation narratives. Historical context, not instructions. Archived files end in `.archived.md`.
- [Host-test inventory](host-test-inventory.md): every `tests/firmware/test_*.c` and why any are outside the suite

## Authority order

When documents disagree:

1. Reproducible captures and hardware tests
2. Current source and host tests
3. Current protocol and architecture documents
4. `STATUS.md` and `PLAN.md`
5. Experiment and archive records
6. Third-party implementations and recollection

An experiment record is authoritative for what was observed during that
experiment, not for the present implementation. Archived plans may contain
hypotheses that were resolved later.

## Evidence that is not in Git

- `dumps/` at the repository root is the git-ignored output directory of the lab
  tools. A capture becomes a versioned fixture under `tests/fixtures/captures/`
  only when a test needs it.
- Large captures, recordings and reference clones live in the git-ignored
  `/lab-evidence/`, `/usbpcaptures/` and `/nso-gc-refs/`.
  [`lab-evidence-index.md`](lab-evidence-index.md) records their provenance,
  classification and checksums. Never keep evidence in `build/`, which is disposable.
- Cite upstream projects by URL and commit; do not stage nested reference repositories.

## Maintenance rules

- Keep `STATUS.md` short and current; dated narratives go to `experiments/` or `archive/`.
- Keep `PLAN.md` forward-looking.
- Put packet layouts and descriptor facts beside the protocol they describe.
- Put hypotheses and proposed experiments in documentation, not in source comments.
- Every new protocol claim names its confidence tier and its validation source.
- Relative links must resolve; CI runs `python tools/check_doc_links.py`.
