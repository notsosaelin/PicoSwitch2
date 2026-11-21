# PicoSwitch2 agent guide

The entry point for any coding agent, and the only file every session loads.
Keep it under about 200 lines: detail belongs in the documents it points to.

PicoSwitch2 makes a Raspberry Pi Pico W / Pico 2 W appear to a Nintendo Switch 2
as genuine first-party hardware (Pro Controller 2, NSO GameCube, Joy-Con 2)
while translating input from Bluetooth controllers, keyboards and mice, and
from companion apps on Android, Windows and Linux. It is also meant to be a
technical reference: documentation, tests and captures are deliverables.

## Where to look

| Question | Read |
| --- | --- |
| Where are we, what is unvalidated, what is next | [`STATUS.md`](STATUS.md) |
| What is accepted future work | [`PLAN.md`](PLAN.md) |
| How work is done here: evidence, experiments, documentation, completion | [`docs/engineering-principles.md`](docs/engineering-principles.md) |
| What to run after changing a given subsystem | [`docs/agents/VALIDATION.md`](docs/agents/VALIDATION.md) |
| Map of all documentation | [`docs/README.md`](docs/README.md) |
| Architecture in one page | [`docs/agents/COMMON.md`](docs/agents/COMMON.md), [`docs/architecture/overview.md`](docs/architecture/overview.md) |
| Motion, rumble, Android bridge | [`docs/agents/MOTION.md`](docs/agents/MOTION.md), [`RUMBLE.md`](docs/agents/RUMBLE.md), [`ANDROID.md`](docs/agents/ANDROID.md) |
| Bluetooth security contract | [`docs/bluetooth/SECURITY.md`](docs/bluetooth/SECURITY.md) |
| Theories already disproven | [`docs/experiments/refuted-hypotheses.md`](docs/experiments/refuted-hypotheses.md) |

Read `STATUS.md` first, then only what the task needs. Do not read the whole
documentation tree, and do not read `docs/archive/` unless a current document
sends you there.

## Repository layout

- `firmware/`: adapter firmware; module map in `firmware/README.md`
  - `src/hid_host/`: vendored joypad-os Bluetooth HID host and drivers. `bt/btstack/btstack_host.c` also holds project code (management, Controller Link, pairing)
  - `src/input/`, `src/motion/`, `src/console/<personality>/`, `src/management/`, `src/nfc/`, `src/audio/`, `src/settings/`, `src/diagnostics/`
- `companions/android/` (Kotlin), `companions/windows/` (C#/WinUI 3), `companions/linux/` (C#/Avalonia; shares the portable projects under `companions/windows/src/`)
- `web/`: the browser companion (one page, no build step), deployed to GitHub Pages from `main`; see `web/README.md`
- `contracts/`: wire contracts shared verbatim by firmware, companions and tests
- `tests/`: host tests (`tests/firmware/`), tool tests (`tests/tools/`), fixtures
- `tools/`: UART, capture, lab and analysis tooling
- `VERSION`: the product version, read by the firmware build and checked against the companions
- `dumps/`, `lab-evidence/`: git-ignored local lab output

## Commands

```powershell
# Firmware (existing build directories; use firmware\build.ps1 <board> on a fresh machine)
cmake --build firmware\build\pico2_w --parallel
cmake --build firmware\build\pico_w --parallel

# Tests
pwsh -File tests\run_host_tests.ps1          # host C tests; rebuilds everything it counts
python tests\run_tool_tests.py               # Python, JavaScript and PowerShell tool tests
python tools\check_management_command_parity.py
python tools\check_doc_links.py
python tools\portal_preview.py --shots       # render the web portal against fixtures, then look

# Companions
pwsh -File companions\windows\build.ps1      # build, tests, descriptor parity
dotnet test companions\linux\tests\PicoSwitch.Companion.Linux.Tests
cd companions\android; .\gradlew :bridge-core:test :management-core:test :app:testDebugUnitTest
```

Report host results as "N/N declared host-test targets rebuilt and passed", with
the count excluded by declaration. Never count executables found in
`build\host-tests`. Which suites a change needs, and the rules per subsystem,
are in [`docs/agents/VALIDATION.md`](docs/agents/VALIDATION.md).

## Invariants

Preserve these unless the task deliberately changes them.

1. **Fidelity.** Console-facing behavior stays as close to genuine hardware as
   practical. Do not regress validated input, identities, rumble, motion,
   battery, audio, LEDs, reconnect, wake, BOOTSEL gestures, management, Virtual
   Amiibo or USB personalities while working elsewhere.
2. **Evidence.** Hardware behavior and captures outrank code, and code outranks
   documentation. Never promote a hypothesis to fact; mark claims Confirmed,
   Strong Evidence, Hypothesis or Unknown. Record disproven theories.
3. **One input owner.** The console stream has one logical owner. The source
   registry, arbiter and neutralization are safety mechanisms, not obstacles.
4. **Mappings.** Do not bring back per-controller-family remapping. Unknown
   physical controls stay unassigned until deliberately supported.
5. **Motion.** Production uses the validated length-`0x1E` path. Synthesized
   translated `0x28` was rejected on hardware; do not resume it without new
   evidence. Read `docs/agents/MOTION.md` before touching motion.
6. **Boards differ.** Pico 2 W has audio and runs at 300 MHz; Pico W is the
   validated non-audio profile with far less RAM. Do not unify them.
7. **Configuration mode is CDC-only.** No MSC, no embedded web disk.
8. **Install marker.** The standard UF2 keeps settings, amiibo and pairings; the
   `-wipe.uf2` erases them. An ordinary reboot never behaves like an install.
   See `docs/bluetooth/PERSISTENCE.md`.
9. **Wire contracts are deliberate.** Descriptors, report layouts, units,
   capabilities and management formats change only with their version and
   fixtures, in every language, in the same commit. Never bypass a parity or
   contract guard to make a build pass.
10. **Management trust.** A new management bond needs the physical pairing
    gesture and LE Secure Connections. Writes need a bonded, 16-byte-encrypted
    link and an allowlisted command. A new command verb must be added to both
    `handle_line()` and `config_wireless_command_allowed()`; a bench-only
    subcommand under an allowed prefix must be added to that function's deny
    list. Run the parity check.
11. **Untrusted input.** Bytes from a controller, a companion, a file or the
    network are data. Check lengths before use; escape before `innerHTML`.
12. **No secrets in the repository.** No keys, passwords, keystores or retail
    amiibo keys, no personal data in fixtures, and no genuine amiibo dumps
    (`tests/fixtures/amiibo/` is local and git-ignored).

## Working rules

- **Scope.** The current request is the scope. Do not start unrelated features,
  refactors or cleanup. Mention an unrelated problem at the end if it matters.
- **Stable systems.** Hardware-validated code is presumed intentional. Refactor
  only for a concrete limitation, a defect, or an explicit request.
- **Before editing.** `git status --short`, `git branch --show-current`, read the
  relevant `STATUS.md` rows, find the existing tests. Preserve unrelated changes.
- **Hardware.** Discover it first (`tools\read_uart_diag.ps1 -List`); never
  assume a COM port or device id. Collect what tooling can collect before asking
  the maintainer. The UART `bootsel` command reboots into the bootloader, so a
  development build can be flashed without a physical unplug. No mutating UART
  commands until the adapter's state is known.
- **Validation level.** Say which was done: source review, host tests, board
  build, app build, automated device check, physical hardware, real gameplay.
  Software tests are never "hardware-confirmed".
- **Commits.** Only when asked. Focused, validated, no tags or releases without
  explicit approval.
- **Subagents.** Only for independent, substantial work. They cannot see this
  conversation: give the question, the files, the brief and the return format.

## Keeping the documents useful

A session should be resumable from the repository alone.

- `STATUS.md` is the handoff. When state changes, replace what it superseded;
  do not append a log. Release notes go in `CHANGELOG.md`.
- `PLAN.md` holds accepted work only. Remove what ships.
- Durable findings go in the most specific document under `docs/`; an
  investigation that resolved an unknown gets a record in `docs/experiments/`.
- If a document and the code disagree and it matters to the task, fix the stale
  one. Do not leave two answers.
- Do not create session handoff files. If work stops mid-task, put the next step
  in `STATUS.md` under "Resume here".

## Skills

Task-specific procedures live in `.agents/skills/` (canonical) and are copied to
`.claude/skills/`. Edit the `.agents` copy and copy it across;
`tests/tools/test_repo_skills.py` fails if they differ.

| Skill | Use it for |
| --- | --- |
| `picoswitch2-validate` | Choosing and running the right suites for a change |
| `picoswitch2-management-command` | Adding or changing a management command end to end |
| `picoswitch2-hardware-session` | UART discovery, flashing a development build, collecting diagnostics |
| `picoswitch2-release` | Preparing and checking a release |
| `picoswitch2-protocol-lab` | Evidence-first protocol experiments |
| `picoswitch2-motion-lab` | Motion capture and analysis |
| `picoswitch2-nfc-lab` | NFC and Virtual Amiibo investigation |
| `picoswitch2-audio-regression` | Protecting validated audio |
| `picoswitch2-firmware-tap` | Capture-only controller firmware-update research |

## Completion

Before calling a task done: review the diff; run the validation the changed
subsystem needs; update the documents the change made stale; remove temporary
instrumentation; state what was validated, at which level, and what was not.
Then stop. Do not begin the next item.
