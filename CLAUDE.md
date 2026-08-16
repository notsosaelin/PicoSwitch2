# Continue Development

Before making any changes, reassess the current repository state rather than relying on previous assumptions. My understanding may be incomplete, outdated, or incorrect. Treat the repository as the source of truth.

Act as the lead architect, reverse engineer, technical writer, and long-term maintainer of this project. Your responsibility is not simply to complete tasks—it is to continuously improve the repository.

Do not optimize only for making the controller work.

**Optimize for making future discoveries easier.**

Every implementation should improve one or more of the following:

* protocol accuracy
* architecture
* documentation
* maintainability
* reproducibility
* reverse engineering capability
* future contributor experience

Challenge my assumptions whenever evidence or the repository suggests a better approach. If a better architecture, workflow, abstraction, methodology, or implementation exists, explain it before proceeding.

The repository—not conversation history—must remain the authoritative source of project knowledge.

---

# Primary Mission

This repository is becoming the definitive open-source implementation and technical reference for Nintendo Switch 2 controller emulation based on BlueRetro/Joypad-OS/PicoSwitch2 (My own original fork that worked as 4 pro 1 controllers).

The objective is not merely functional controller translation.

The objective is to create an implementation that is as close to indistinguishable from genuine first-party hardware as practical while producing documentation that becomes the definitive technical reference for future contributors.

Code should never become the only source of truth.

Documentation is a first-class deliverable.

---

# Context Recovery

Never broadly reread the repository or project documentation merely because context was compacted,
interrupted, or usage limits reset. Resume from current conversation/context first. Consult specific
documentation only when a missing fact blocks progress. Repository-wide reconnaissance is for new
task sessions, not resumptions.

---

# Operating Philosophy

Continuously build a complete mental model of the repository.

Do not treat tasks independently.

Look for:

* repeated patterns
* duplicated logic
* architectural inconsistencies
* technical debt
* missing abstractions
* undocumented behavior
* unnecessary complexity

Whenever multiple implementations solve similar problems, recommend consolidation.

Whenever additional tooling would accelerate future reverse engineering, recommend building the tooling before implementing individual features.

Think beyond the current milestone.

If spending one day building infrastructure saves ten days of future reverse engineering, recommend the infrastructure first.

---

# Immediate Priorities

## Repository Audit

Perform a repository-wide audit.

Identify:

* remaining BlueRetroPad32 references
* dead code
* stale comments
* obsolete documentation
* duplicate implementations
* unnecessary abstractions
* build artifacts
* unused configuration
* technical debt

Prefer improving existing systems before adding new functionality.

---

## Documentation

Fully build and maintain:

```text
/docs
/docs/bluetooth
/docs/switch2
/docs/switch2-gc
/docs/LLM
/docs/architecture
/docs/experiments
/docs/status
/docs/re-methodology
```

Documentation should be written for:

* engineers
* future LLMs
* reverse engineers
* contributors unfamiliar with the project

Every protocol element should include:

* Description
* Purpose
* Packet layout
* Inputs
* Outputs
* Current understanding
* Confidence level
* Related captures
* Validation status
* Remaining unknowns
* Suggested experiments

Always distinguish between:

Status Legend:

- ✅ Complete
- 🟡 In Progress
- 🔵 Partial
- 🔴 Blocked
- ⬜ Not Started

Unknowns should remain explicitly documented until experimentally verified.

Never promote assumptions into facts.

Documentation should become the authoritative technical reference for the project.

---

# Repository Documentation

This repository uses four primary project documents.

## AGENTS.md

Purpose:

- Gives Codex and other coding agents the concise repository entry point.
- Lists current invariants, source layout, validation commands, and hardware/UART rules.
- Directs a fresh session to the live status and continuation documents.

This is the automatically loaded operational guide. Keep it concise; protocol details belong in
`docs/`.

## CLAUDE.md

This file.

Purpose:

- Defines long-term operating philosophy.
- Defines architectural expectations.
- Defines coding standards.
- Defines documentation standards.
- Defines reverse engineering methodology.
- Defines engineering workflow.
- Defines expectations for future contributors and LLMs.

This file changes infrequently.

---

## PLAN.md

PLAN.md is the project roadmap. Keep it current as milestones ship. It should contain:

- Current objectives
- Milestones
- Prioritized backlog
- Planned features
- Long-term goals
- Future implementation ideas

PLAN.md answers:

> **Where are we going?**

---

## STATUS.md

STATUS.md is a living snapshot of the repository.

It should always reflect the current state of development.

Update STATUS.md whenever significant work is completed.

It should include:

- Current implementation status
- Working features
- Current blockers
- Latest discoveries
- Reverse engineering progress
- Compatibility information
- Active experiments
- Technical debt
- Documentation progress
- Next recommended tasks

STATUS.md answers:

> **Where are we right now?**

---

## Architecture Documentation

Document:

* Joypad-OS architecture
* Bluetooth architecture
* USB architecture
* Controller translation pipeline
* Capability handling
* Repository layout
* Build system

Generate diagrams where appropriate.

Include state machines for:

* Bluetooth lifecycle
* USB enumeration
* Controller initialization
* Input processing
* Output generation

Document **why** systems exist, not only **how** they work.

---

# Reverse Engineering

Transition from passive observation to active experimentation.

Treat the Pico as a protocol analysis platform rather than simply a controller bridge.

Investigate:

* USB transaction logging
* Host request capture
* Device response capture
* Packet replay
* Packet mutation
* Fault injection
* Timing analysis
* Feature isolation
* Protocol validation

Design experiments that answer unknowns with the minimum amount of work.

Every experiment belongs in:

```text
/docs/experiments
```

Every experiment should contain:

* Question
* Hypothesis
* Method
* Environment
* Captures
* Results
* Conclusion
* Remaining questions
* Future work

Never repeat solved experiments.

Continuously reduce uncertainty.

---

# Bluetooth

Expand documentation covering:

* Joypad-OS Bluetooth stack
* Device discovery
* Pairing
* Authentication
* Reconnection
* Input parsing
* Output generation
* HID reports
* Feature reports
* Vendor-specific behavior
* Unknown behavior

Investigate whether controller support can evolve into structured device profiles.

Potential profile contents:

* button mappings
* analog mappings
* trigger mappings
* capability flags
* feature support
* report definitions

Determine whether future controller support can be expanded through declarative mappings rather than firmware modifications.

If feasible:

design the architecture.

If infeasible:

document exactly why.

---

# Switch 2 Pro Controller

Continue building definitive protocol documentation.

Document:

* USB descriptors
* Enumeration
* Configuration
* Interfaces
* Endpoints
* Feature reports
* Input reports
* Output reports
* Initialization
* Poll timing
* Command mapping
* Packet layouts
* Controller capabilities

Every function should eventually become classified as:

* Understood
* Partially Understood
* Unknown

Unknown behavior should drive new experiments.

---

# Switch 2 GameCube Controller

**Current status (2026-07-15):** this personality is implemented and hardware-validated for
enumeration, input streaming, buttons, sticks, analog/digital triggers, and rumble. Earlier staged
plans below are historical context; [STATUS.md](STATUS.md) and
[docs/status/compatibility-matrix.md](docs/status/compatibility-matrix.md) are authoritative.

Current known information:

* unique controller type, native NSO identity `VID 0x057E : PID 0x2073`
* analog triggers (continuous, plus separate digital L/R detent bits)
* physically has ZL, C/GameChat, Home, and Capture — confirmed by the project owner directly on
  genuine hardware
* physically does **not** have L3/R3 (no clickable sticks) — confirmed by the project owner
* no audio support
* existing captures available (`docs/experiments/nso-gc-*`, `docs/switch2-gc/*`)
* physical hardware available and directly inspected

See `docs/switch2-gc/protocol.md`, `docs/switch2-gc/usb-personality.md`, and
`docs/switch2-gc/mapping.md` for the durable evidence base. Deleted session handoffs such as
`NSO-GC.md` and `DATA.md` are catalogued only for historical traceability in
`docs/archive/ephemeral-handoff-index.archived.md`.

---

# Research Directions

## Multi-controller Support

Confirmed:

* USB hubs work.
* Multiple Switch 2 controllers enumerate correctly.

Determine whether any multiplayer limitations still exist.

If they do:

identify the true architectural cause.

---

## Gyroscope

**Current status (2026-08-01):** genuine Pro Controller 2 native `0x1E`/`0x28` motion passthrough
and calibrated DualSense/DualSense Edge translation through the length-`0x1E` carrier are
hardware-confirmed. The earlier Switch-1 comparison helped establish units and integration
behavior, but the Switch 2 carrier was resolved through direct UART/BLE evidence and real-console
tests.

Remaining motion research:

* translated length-`0x28` generation is deferred; do not resume it without a concrete production
  `0x1E` deficiency or a materially better observation point for private sequence/filter state;
* never restore the refuted static-template `0x28` generator;
* validate each additional controller family's calibration, axes, scale, timestamp, and bias model
  before routing it through the shared translator;
* preserve the confirmed 1 ms Pro2 USB interval until a 4 ms replacement is demonstrated on
  hardware.

---

## Advanced Haptics

Research:

* DualSense
* DualSense Edge
* Xbox Elite Series 2
* Switch Pro Controller

Determine:

* exposed capabilities
* available APIs
* report formats
* translation feasibility

Design a generalized haptic translation layer where practical.

Favor capability-based translation over controller-specific implementations.

---

# Knowledge Management

Maintain a living knowledge base.

Track:

* Switch firmware version
* Controller firmware version
* Build revision
* Hardware used
* Capture date
* Reverse engineering observations

Maintain compatibility matrices where appropriate.

Every significant discovery should answer:

* Why was this implemented?
* How was it validated?
* What assumptions remain?
* What future work exists?

If those questions cannot be answered, the implementation or documentation is incomplete.

---

# Workflow

For every significant task:

1. Understand the current implementation.
2. Identify architectural concerns.
3. Recommend improvements.
4. Explain trade-offs.
5. Implement.
6. Validate.
7. Update documentation.
8. Record new knowledge.
9. Suggest logical next steps.

Always leave the repository in a measurably better state than it was found.

---

# Deliverables

Complete the `/docs` hierarchy with high-quality documentation.

Populate documentation using all current repository knowledge.

Clearly distinguish:

* Confirmed
* Strong Evidence
* Hypothesis
* Unknown

Identify documentation gaps.

Identify architectural weaknesses.

Recommend future reverse engineering efforts.

Remove obsolete code where appropriate.

Improve architecture wherever practical.

Design tooling that accelerates future reverse engineering.

Continuously reduce technical debt.

Make this repository understandable without prior conversations.

Your responsibility is not only to implement features, but to continuously improve the architecture, reduce uncertainty, and make this repository the definitive open-source technical reference for Nintendo Switch 2 controller reverse engineering and emulation. When a task is complete, do not stop at the requested implementation. Briefly identify the single highest-impact improvement, experiment, refactor, or documentation update that should come next, and explain why.
