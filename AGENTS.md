# PicoSwitch2 Agent Guide

This file is the concise operational entry point for coding agents working in this repository.

It defines repository navigation, important invariants, validation expectations, hardware rules, and task discipline.

Detailed engineering philosophy belongs in `CLAUDE.md`.

Current project state belongs in `STATUS.md`.

Future work belongs in `PLAN.md`.

Protocol evidence, architecture details, experiments, and technical reference material belong under `docs/`.

Do not turn this file into a project history, changelog, experiment notebook, or duplicate status document.

# Read Order

For a new task session, read:

1. `AGENTS.md` — this file
2. `STATUS.md` — current validated state and open gates
3. `PLAN.md` — accepted roadmap
4. `docs/README.md` — documentation authority and map
5. The protocol, architecture, methodology, or agent brief relevant to the task

Useful focused briefs live under:

- `docs/agents/COMMON.md`
- `docs/agents/MOTION.md`
- `docs/agents/RUMBLE.md`
- `docs/agents/ANDROID.md`

If `docs/LLM/current-context.md` contains an active continuation handoff relevant to the task, read it after the current-state documents.

Do not broadly read every document by default.

Inspect only what is necessary to understand and safely complete the active task.

# Source of Truth

Prefer evidence in approximately this order:

1. Reproducible hardware behavior and captures
2. Current implementation and automated tests
3. Specific protocol and architecture documentation
4. `STATUS.md`
5. `PLAN.md`
6. `AGENTS.md` and `CLAUDE.md`
7. Historical/archive documents
8. Conversation history and assumptions in task prompts

When sources conflict and the discrepancy matters to the task, resolve it rather than silently choosing one.

The repository must remain authoritative over prior conversations.

Do not treat an old comment, task description, or archived document as current merely because it is more detailed.

# Scope Discipline

Treat the current user task as the active engineering scope.

Repository context should inform that task, not automatically expand it.

Do not begin unrelated:

- features
- refactors
- cleanup
- protocol investigations
- compatibility campaigns
- documentation rewrites
- architecture projects
- roadmap work

unless they are necessary to safely complete the requested task.

If an unrelated issue is discovered:

1. Determine whether it blocks or materially compromises the current task.
2. If it does, make the smallest necessary correction.
3. If it does not, leave it untouched.
4. Mention it at completion only if materially important.

Do not treat general maintainability guidance as permission to rewrite stable systems.

Do not add speculative ideas to `PLAN.md` merely because they were mentioned in a task or conversation.

# Core Project Invariants

Preserve these unless the current task explicitly and deliberately changes them.

## Console-facing fidelity

The goal is behavior as close to genuine Nintendo hardware as practical.

Do not regress already validated:

- controller input
- controller identities
- rumble
- motion
- battery reporting
- audio
- LEDs
- reconnect
- wake
- BOOTSEL behavior
- management
- Virtual Amiibo
- USB personality behavior

when working on an unrelated subsystem.

## Evidence

Direct Switch 2 behavior, UART evidence, genuine-controller captures, and controlled hardware experiments are primary evidence.

Third-party projects may provide useful reference material but are not authoritative Switch 2 protocol truth.

Never promote a hypothesis to fact.

Preserve important disproven hypotheses when they are likely to be rediscovered.

Use the existing experiment/refutation documentation rather than allowing rejected theories to quietly return.

## Active input ownership

The console-facing stream has one active logical input owner.

Do not accidentally turn support for additional HID peers into arbitrary multi-controller merging.

The source registry, source arbiter, neutralization behavior, and fresh-report boundaries are deliberate safety mechanisms.

A logical source may contain more than one physical peer when a feature explicitly requires it, but that does not imply multiple independent controllers should simultaneously own the console stream.

## Controller mappings

Do not reintroduce the retired per-controller-family remapping system as a convenience.

The existing physical-controller base mapping is intentionally stable.

A new input source may have explicit source-specific mapping requirements when that is part of the task.

Do not silently repurpose unknown or additional physical controls.

Unknown or additional inputs should remain unassigned until deliberately supported.

## Motion

Production translated motion uses the currently validated common Switch 2 motion path.

Do not create alternate generic motion encoders merely because an existing implementation has a misleading historical name.

Do not resume synthesized translated length-`0x28` production work without:

- a concrete deficiency in the validated production path, or
- materially better evidence or observation capability

The rejected static/template-derived `0x28` approach must not return.

Read `docs/agents/MOTION.md` and the relevant motion evidence before modifying production motion behavior.

## Audio

Pico 2 W uses its validated audio-capable production configuration.

Pico W intentionally retains its validated non-audio profile.

Do not attempt to unify those configurations without a specific task and hardware evidence supporting it.

## Configuration mode

Configuration USB mode is CDC-only.

Do not reintroduce:

- MSC
- the old embedded web disk
- generated FAT/web-drive images

Use the repository's current external portal tooling.

## Install-reset marker

Every releaseable UF2 must preserve the install-reset marker behavior.

A newly flashed image performs the intended first-install reset.

An ordinary reboot must not behave like a new installation.

Do not modify persistence/reset semantics incidentally.

## Firmware/application compatibility

Peer-visible contracts must remain deliberate.

Do not change:

- descriptors
- report layouts
- field widths
- field units
- semantics
- capabilities
- management wire formats

without checking the repository's compatibility/version rules.

Preserve descriptor parity, contract-version tests, build identity reporting, and compatibility guards.

Do not bypass a compatibility test merely to make a build pass.

# Stable Architecture

Hardware-validated architecture should be presumed intentional.

Do not refactor a stable subsystem solely because another abstraction looks cleaner.

Refactor only when:

- the active task exposes a concrete limitation
- duplicated behavior is causing defects or divergence
- the current architecture prevents required functionality
- correctness or maintainability risk is demonstrated
- the maintainer explicitly requests architectural work

Prefer the smallest coherent integration over broad redesign.

When a subsystem has already survived hardware validation, require evidence before treating its architecture as the likely cause of a new failure.

# Repository Layout

Important locations include:

- `src/` — firmware implementation
- `src/bt_hid/` — Bluetooth HID controller handling and translation
- `src/bt_hid/motion/` — translated/native Switch 2 motion implementation
- `src/switch_pro2/` — Pro Controller 2 console-facing behavior
- `android/companion/` — Android companion and platform-neutral bridge modules
- `web/` — browser management interface
- `tools/` — host tests, UART tooling, capture tooling, fixtures, and analysis
- `docs/architecture/` — architecture documentation
- `docs/bluetooth/` — Bluetooth/controller documentation
- `docs/bridge/` — companion bridge protocol/backend documentation
- `docs/switch2/` — Pro Controller 2 protocol documentation
- `docs/switch2-gc/` — NSO GameCube documentation
- `docs/switch2-joycon2/` — Joy-Con 2 documentation
- `docs/experiments/` — dated reverse-engineering experiments
- `docs/re-methodology/` — experiment and evidence methodology
- `docs/status/` — compatibility/current validation matrices
- `docs/agents/` — short specialist briefs
- `docs/archive/` — historical documentation
- `dumps/` — captures and derived research artifacts

Read `docs/README.md` before inventing a new documentation location.

Prefer extending an existing authoritative document over creating an overlapping one.

# Starting a Task

Before editing:

1. Check the current working tree.
2. Check the current branch.
3. Identify the files and subsystems relevant to the task.
4. Read the current status relevant to those subsystems.
5. Identify existing tests and diagnostics.
6. Identify behavior that must remain unchanged.
7. Verify assumptions from the task against the repository.

Do not perform a repository-wide audit unless the task requires one.

Do not discard or overwrite unrelated user changes.

# Context Recovery

If the same task is resumed after compaction, interruption, or usage-limit reset:

- continue from current conversation and working state
- inspect the current diff/state as necessary
- do not reread the repository broadly
- consult only specific documents needed to recover missing facts
- do not repeat completed investigation merely because the session context changed

A new task session may perform targeted repository reconnaissance for the new task.

# Build Setup

Clone with submodules:

```powershell
git clone --recurse-submodules https://github.com/notsosaelin/PicoSwitch2.git
```

If the repository was cloned without submodules:

```powershell
git submodule update --init --recursive
```

Use the branch selected by the maintainer or the repository's current development branch.

Do not assume a historical branch name is still correct without checking.

The Windows build helper uses the Raspberry Pi Pico SDK/toolchain documented by the repository.

On a fresh workstation, initialize build directories with:

```powershell
.\build.ps1 pico2_w
.\build.ps1 pico_w
```

When configured build directories already exist, prefer incremental builds:

```powershell
cmake --build build\pico2_w --config Release --parallel
cmake --build build\pico_w --config Release --parallel
```

Do not delete or reconfigure known-good build directories without a reason.

# Standard Firmware Validation

Run validation appropriate to the changed subsystem.

For significant shared firmware changes, both supported production board builds should remain clean unless the task explicitly affects only one board.

Run compiled host tests with:

```powershell
pwsh -File tools\run_host_tests.ps1
```

That is the authoritative command. It recreates `build/host-tests` empty, builds every declared host
test from current source, runs only what it just built, and reports a total derived from that run.
`tools/run_host_tests.ps1` owns the single build manifest; `-Group` and `-Filter` select a subset
(`tools/run_mgmt_tests.ps1` is the `management` group plus its Python suites).

Do NOT enumerate `build\host-tests\*.exe` and run whatever is present. That was the previous
instruction here, and it was wrong: only 23 of the repository's 79 host-test sources had a build
recipe, so the directory accumulated executables from ad-hoc `gcc` invocations of unknown vintage.
Totals derived that way described the directory, not the source tree. **A stale executable must
never count as a passing test.**

Every `tools/test_*.c` must appear either in the manifest or in the runner's `$notHostTests` table
with a reason. An unlisted source fails the run, so a test cannot silently stop being built.

`build/host-tests` is owned by whichever runner last executed, and each one recreates it empty. So
after a scoped run — `-Group`, `-Filter`, or `tools/run_mgmt_tests.ps1` — the directory holds only
that subset. **Its contents are never an authoritative count.** Only the total printed by the run
that built them is. Re-run the full suite before quoting a number.

Report the result as "N/N declared active host-test targets rebuilt from current source and passed",
with the count of sources classified outside the suite. Do not write "all tests pass": the excluded
sources include genuinely uncovered code — see [`docs/host-test-inventory.md`](docs/host-test-inventory.md).

Run relevant Python test suites as required by the changed subsystem.

Useful general suites include:

```powershell
python tools\test_ns2_trace.py
python tools\test_ns2_nfc_semantics.py
python tools\test_amiibo_corpus.py
```

Do not rely on this short list as the complete subsystem test inventory.

Use:

- changed files
- existing test runners
- relevant documentation
- relevant agent briefs

to determine additional required validation.

# Install-Reset Marker Validation

For releaseable firmware artifacts, preserve the install-reset marker.

Current verification tooling:

```powershell
python tools\verify_install_reset_marker.py build\pico2_w\PicoSwitchWGA-pico2_w.bin --flash-size 0x400000
python tools\verify_install_reset_marker.py build\pico_w\PicoSwitchWGA-pico_w.bin --flash-size 0x200000
```

Failure here is a release blocker unless the current task deliberately changes the install/reset design.

# Motion Validation

If the task changes motion behavior, read:

- `docs/agents/MOTION.md`
- the current motion protocol documentation
- the relevant experiment records

Run the existing motion host, quality, and coherence suites appropriate to the code changed.

Do not copy stale hardcoded inventories of every historical motion executable into new task prompts.

The repository's current tests and motion brief are authoritative.

Synthesized translated length-`0x28` remains research-only/deferred unless explicitly reopened.

Do not request a hardware flash for a speculative motion change before available host/coherence gates pass.

Do not revive previously rejected chart, magnetometer, template, or alternate-encoder theories without new contradictory evidence.

# NFC / Amiibo Validation

If the task changes NFC or Virtual Amiibo behavior, follow:

`docs/re-methodology/nfc-investigation-workflow.md`

Use existing:

- corpus tooling
- semantic decoder
- host-replayable runtime
- persistence tests
- capture fixtures
- NFC lab runner

before requesting physical console testing.

A deterministic parsing, state, timing, persistence, or record-layout bug should be reproduced in the host laboratory when possible rather than debugged through repeated blind flashes.

Console-visible status values are observations, not diagnoses.

Do not replace hardware-confirmed Virtual Amiibo behavior with speculative protocol semantics.

# Android / Bridge Validation

If the task changes the Android companion or platform-neutral bridge:

- preserve the separation between platform-neutral bridge logic and Android-specific backends
- run the existing Gradle/JVM tests for affected modules
- build the affected APK variant
- run descriptor/contract parity checks when the bridge wire contract or descriptor is touched
- use ADB diagnostics when an Android device is available
- do not ask the maintainer to manually retrieve information that ADB can obtain
- preserve current firmware/application runtime compatibility handling

The bridge contract and backend architecture are documented under:

- `docs/bridge/PROTOCOL.md`
- `docs/bridge/PLATFORM_BACKEND.md`

Do not move Android-specific APIs into the platform-neutral module.

Do not interpret a firmware/application contract mismatch as evidence of a bridge architecture failure without checking runtime compatibility diagnostics first.

# Web / Management Validation

If the task changes the web portal or management protocol:

- preserve the current CDC/BLE transport model
- preserve management authorization/security requirements
- run existing portal/static tests
- run relevant management host tests
- check wire compatibility if command formats change
- verify browser-visible state is not merely cached stale state when testing synchronization behavior

Do not add a second configuration protocol when the existing management path can be extended cleanly.

# Hardware Validation Levels

Build success is not hardware validation.

State clearly which level was performed:

- static/source review
- unit/host tests
- board build
- application build
- automated device validation
- physical hardware validation
- real Switch 2 gameplay validation

Never describe a feature as hardware-confirmed when only software tests passed.

If hardware validation is pending, state exactly what remains unverified.

# Hardware Workflow

The maintainer performs physical flashes and interactions when physical access is required.

Agents should autonomously use software-visible diagnostics when available.

Useful sources may include:

- UART
- ADB
- firmware counters
- diagnostic exports
- capture tools
- repository scripts
- test harnesses

Do not assume a fixed COM port or Android device identifier on a different workstation or session.

Discover currently connected hardware first.

For UART, use repository tooling such as:

```powershell
tools\read_uart_diag.ps1 -List
```

or the current documented equivalent.

Do not perform destructive or mutating live UART operations until the required hardware/personality state is known.

Flashing a development build does not require the maintainer. `bootsel` over UART reboots the
adapter into the USB mass-storage bootloader (`reset_usb_boot`), because the physical BOOTSEL button
is bound to pairing/personality gestures and is not a bootloader entry. Copy the `.uf2` to the
mounted drive; the adapter reboots into the new build by itself. Only a build that predates the
command needs a physical unplug-and-hold.

Prefer passive observation and bounded A/B experiments over repeated firmware guesses.

# Protocol Research Workflow

For non-NFC protocol research, follow:

`docs/re-methodology/controller-protocol-lab.md`

Use the existing shared laboratory instead of inventing parallel capture infrastructure.

Where applicable, preserve:

- Git provenance
- firmware/build identity
- hardware identity
- controller firmware identity
- console firmware identity
- timestamps
- zero-loss status
- raw captures
- semantic analysis
- fixtures
- hashes

A dropped or overwritten capture cannot become authoritative evidence or a golden fixture.

Design experiments around one clear question and one meaningful variable whenever practical.

# Experiments

Create an experiment document when a task materially resolves or tests an unknown:

- protocol behavior
- hardware behavior
- timing behavior
- state-machine behavior
- competing hypothesis
- unexplained failure

Routine implementation verification does not require a new experiment document.

Experiment records belong under:

`docs/experiments/`

Preserve historical experiment observations even when later conclusions change.

Correct active summaries rather than rewriting historical evidence.

# Diagnostics

Prefer diagnostics that expose subsystem boundaries.

Useful patterns include:

- report counters
- accepted/rejected counts
- rejection reasons
- current source/profile/personality
- connection generations
- capability state
- contract/build identity
- descriptor match/mismatch state
- last-error state
- bounded state snapshots

Avoid high-frequency logging that materially changes timing.

When debugging a cross-layer failure, instrument boundaries before rewriting implementation logic.

Prefer evidence that can distinguish:

- input never arrived
- parsing rejected it
- state was not updated
- output was not generated
- output was not transmitted
- host rejected or ignored it

Do not infer a root cause solely from which subsystem looks suspicious.

# Documentation Updates

When behavior changes materially, update only the documentation affected by the change.

Common destinations include:

- `STATUS.md` for current state
- `PLAN.md` only when accepted future work changes
- protocol documentation for durable protocol knowledge
- architecture documentation for architectural changes
- compatibility matrix for hardware validation changes
- experiment documentation for reverse-engineering evidence
- `CHANGELOG.md` when appropriate for user-visible release history

`STATUS.md` is not append-only.

When updating it:

- replace superseded state
- remove resolved blockers
- remove obsolete diagnoses
- collapse completed investigation history into current conclusions
- link detailed evidence instead of duplicating it
- eliminate contradictions
- remove stale next-session instructions
- avoid duplicating `PLAN.md`

If a reader needs the full sequence of an investigation, that sequence belongs in `docs/experiments/` or another specific technical document, not `STATUS.md`.

Do not create speculative roadmap entries for ideas that are not accepted work.

# Documentation Quality

When documenting protocol behavior, distinguish:

- Confirmed
- Strong Evidence
- Hypothesis
- Unknown

Do not present an inference as a wire fact.

When a hypothesis is disproven and likely to be rediscovered, preserve the negative result in the appropriate evidence document.

Document why non-obvious behavior exists when a future maintainer might otherwise "clean it up" and reintroduce a bug.

Avoid duplicating the same current-state claim in multiple authoritative files.

# Subagents

Use subagents only when the active environment supports them and the task contains independent, substantial work that genuinely benefits from parallel execution.

Do not spawn subagents for trivial tasks.

Prompts should be short and self-contained.

Prefer pointing a specialist at:

- the exact question
- the files in scope
- the relevant `docs/agents/*.md` brief
- the task boundary
- the acceptance criteria
- the expected return format

rather than pasting large amounts of project history.

Do not assume a subagent can see the parent conversation.

Independent read-only investigations may run in parallel.

Concurrent writers must not edit the same worktree/files unless the environment provides safe isolation.

The primary agent remains responsible for validating and integrating subagent results.

Do not delegate simply to appear parallel.

# Git Workflow

Before changes:

```powershell
git status --short
git branch --show-current
```

Preserve unrelated user changes.

Do not discard, reset, stash, or overwrite work merely to obtain a clean tree unless explicitly authorized.

Use focused commits.

Do not combine unrelated cleanup with a feature or fix commit.

Do not commit:

- passwords
- signing secrets
- private keys
- keystores
- machine-specific sensitive configuration
- temporary captures not intended for the repository

# Commit and Push Policy

Do not assume every task requires a commit or push.

Follow the maintainer's current instruction.

When asked to commit:

- review the intended diff
- run appropriate validation
- use a focused commit message
- report the resulting commit identity

When asked to push:

- verify the intended branch
- push only after requested validation succeeds

Do not silently create tags or releases.

# Release Workflow

Do not tag or publish a release without explicit maintainer approval.

A release candidate should be attributable to a clean source revision.

Validation should match the changed subsystem and release risk.

As applicable, verify:

- Pico W production build
- Pico 2 W production build
- relevant host tests
- Python/tooling tests
- Android/JVM tests
- companion build
- descriptor parity
- bridge contract/version guards
- install-reset markers
- application signing
- signature verification
- firmware build identity
- artifact hashes
- hardware sanity checks

Do not require unrelated exhaustive hardware campaigns for a release that did not touch those subsystems.

Do not publish a release from an unverified dirty build unless the maintainer explicitly intends that result.

If firmware and companion share a peer-visible contract, release validation must ensure their versions are compatible.

# Completion

Before declaring a task complete:

1. Review the final diff.
2. Confirm the requested behavior is implemented.
3. Confirm important surrounding behavior remains intact.
4. Run appropriate automated validation.
5. Perform or clearly report required hardware validation.
6. Update affected durable documentation.
7. Remove temporary debugging instrumentation that should not remain.
8. Check for contradictions introduced into `STATUS.md` or other active docs.
9. Check whether compatibility/version metadata needs updating.
10. Report anything that remains unverified.

At completion, summarize:

- what changed
- important design decisions
- validation performed
- hardware validation performed or still required
- documentation updated
- remaining limitations
- repository status

Stop after the requested task.

Do not automatically begin the next feature, refactor, experiment, cleanup pass, or roadmap item.