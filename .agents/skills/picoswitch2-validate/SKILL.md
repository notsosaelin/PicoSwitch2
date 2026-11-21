---
name: picoswitch2-validate
description: Choose and run the correct PicoSwitch2 validation for a change, then report it at the right level. Use after editing firmware, companions, the web portal, contracts, tools or CI, before saying a task is done, and whenever asked to "run the tests", "validate", "check nothing broke" or "is this safe to commit".
---

# PicoSwitch2 validate

## Find what changed

Run `git status --short` and `git diff --stat`. Map each changed path to a row
below. Run every row that matches; a change to `contracts/` or to a portable
project under `companions/windows/src/` matches several.

| Changed path | Run |
| --- | --- |
| `firmware/src/**`, `firmware/CMakeLists.txt`, `firmware/config/**` | Both board builds, host tests, tool tests |
| `firmware/src/management/**`, `firmware/src/settings/config.c` | The above, plus the management parity check |
| `firmware/src/hid_host/bt/btstack/**`, `firmware/config/btstack_config.h` | The above, plus `python tests\tools\test_btstack_dependency_contract.py` |
| `firmware/src/motion/**` | The above; read `docs/agents/MOTION.md` first |
| `firmware/src/nfc/**` | The above; follow `docs/re-methodology/nfc-investigation-workflow.md` |
| `contracts/**` | Host tests, tool tests, Android tests, Windows build, Linux tests |
| `companions/android/**` | Android tests; `:app:assembleDebug` if app code changed |
| `companions/windows/src/PicoSwitch.{Bridge.Core,Management.Core,Companion.Platform,Companion.Services}/**` | Windows build and Linux tests (these projects are shared) |
| `companions/windows/**` (other) | Windows build |
| `companions/linux/**` | Linux tests |
| `web/**` | Tool tests (they include the portal symbol and crypto tests) |
| `tools/**`, `tests/**` | Tool tests; host tests if a C test or the runner changed |
| `*.md` | Doc links |
| `.github/workflows/**` | Parse the YAML; say that the workflow itself has not run |

## Commands

Run from the repository root in PowerShell 7 (`pwsh`). Windows PowerShell 5.1
cannot parse the host-test runner.

```powershell
cmake --build firmware\build\pico2_w --parallel
cmake --build firmware\build\pico_w --parallel
python tools\verify_install_reset_marker.py firmware\build\pico2_w\PicoSwitchWGA-pico2_w.bin --flash-size 0x400000
python tools\verify_install_reset_marker.py firmware\build\pico_w\PicoSwitchWGA-pico_w.bin --flash-size 0x200000

pwsh -File tests\run_host_tests.ps1
python tests\run_tool_tests.py
python tools\check_management_command_parity.py
python tools\check_doc_links.py

pwsh -File companions\windows\build.ps1
dotnet test companions\linux\tests\PicoSwitch.Companion.Linux.Tests -c Release --nologo
cd companions\android; .\gradlew --console=plain :bridge-core:test :management-core:test :app:testDebugUnitTest
```

If a build directory does not exist, create it with `firmware\build.ps1 <board>`.
Do not delete or reconfigure a working build directory without a reason. gcc and
the Pico toolchain may not be on `PATH`; check the session's memory or
`firmware\build.ps1` for where they live before concluding they are missing.

Use `-Group` or `-Filter` on the host runner while iterating, and the full run
before reporting.

## Rules

- A test that fails is a finding. Do not weaken it, skip it or edit a fixture to
  make it pass. If the test encodes an outdated assumption, establish the new
  behavior first, then change the test and say why.
- Never count executables already in `build\host-tests`. Only the total printed
  by the run that built them is evidence.
- A parity, digest or contract guard that fails means two sides disagree. Fix
  the disagreement.
- A firmware size or RAM regression matters on Pico W. Compare
  `arm-none-eabi-size` before and after for a change that adds buffers.

## Report

State each suite with its count, and the level reached:

- "host suite N/N declared targets rebuilt and passed, M excluded by declaration"
- "both boards build; install markers verify"
- what was not run, and why
- "hardware validation pending: <the exact behavior that needs a console or controller>"

Software results are never described as hardware-confirmed. If the change
alters validated state, update the matching row in `STATUS.md`.
