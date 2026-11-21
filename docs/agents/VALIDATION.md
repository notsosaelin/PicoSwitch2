# Agent brief: validation by subsystem

What to run, and what must stay true, for each part of the repository. Read the
section for the code you changed. The command summary is in
[`AGENTS.md`](../../AGENTS.md); the full list of suites is in
[`tests/README.md`](../../tests/README.md).

Moved here from `AGENTS.md` on 2026-11-10 so the entry document stays short.
The rules are unchanged.

## Standard Firmware Validation

Run validation appropriate to the changed subsystem.

For significant shared firmware changes, both supported production board builds should remain clean unless the task explicitly affects only one board.

Run compiled host tests with:

```powershell
pwsh -File tests\run_host_tests.ps1
```

That is the authoritative command. It recreates `build/host-tests` empty, builds every declared host
test from current source, runs only what it just built, and reports a total derived from that run.
`tests/run_host_tests.ps1` owns the single build manifest; `-Group` and `-Filter` select a subset
(`tests/run_mgmt_tests.ps1` is the `management` group plus its Python suites).

Do NOT enumerate `build\host-tests\*.exe` and run whatever is present. That was the previous
instruction here, and it was wrong: only 23 of the repository's 79 host-test sources had a build
recipe, so the directory accumulated executables from ad-hoc `gcc` invocations of unknown vintage.
Totals derived that way described the directory, not the source tree. **A stale executable must
never count as a passing test.**

Every `tests/firmware/test_*.c` must appear either in the manifest or in the runner's `$notHostTests` table
with a reason. An unlisted source fails the run, so a test cannot silently stop being built.

`build/host-tests` is owned by whichever runner last executed, and each one recreates it empty. So
after a scoped run — `-Group`, `-Filter`, or `tests/run_mgmt_tests.ps1` — the directory holds only
that subset. **Its contents are never an authoritative count.** Only the total printed by the run
that built them is. Re-run the full suite before quoting a number.

Report the result as "N/N declared active host-test targets rebuilt from current source and passed",
with the count of sources classified outside the suite. Do not write "all tests pass": the excluded
sources include genuinely uncovered code — see [`docs/host-test-inventory.md`](../host-test-inventory.md).

Run the tool tests (Python, JavaScript and PowerShell, under `tests/tools/`) with:

```powershell
python tests\run_tool_tests.py            # all
python tests\run_tool_tests.py nfc        # only files whose name contains "nfc"
```

Run this one for any change to the build system, `btstack_config.h`, the HID Host
shim, or Bluetooth security/disconnect wiring. It guards the Pico SDK 2.3.0 /
BTstack 1.8.2 dependency contract, where every wrong answer still compiles:

```powershell
python tests\tools\test_btstack_dependency_contract.py
```

Do not rely on this short list as the complete subsystem test inventory.

Use:

- changed files
- existing test runners
- relevant documentation
- relevant agent briefs

to determine additional required validation.

## Install-Reset Marker Validation

For releaseable firmware artifacts (update and clean-install), preserve the install marker.

Current verification tooling:

```powershell
python tools\verify_install_reset_marker.py firmware\build\pico2_w\PicoSwitchWGA-pico2_w.bin --flash-size 0x400000
python tools\verify_install_reset_marker.py firmware\build\pico_w\PicoSwitchWGA-pico_w.bin --flash-size 0x200000
```

Failure here is a release blocker unless the current task deliberately changes the install/reset design.

## Motion Validation

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

## NFC / Amiibo Validation

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

## Android / Bridge Validation

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

## Windows Companion Validation

If the task changes the Windows companion under `companions/windows/`:

- run `companions/windows/build.ps1`, which builds and tests every project except the WinUI shell and then runs the descriptor parity check
- keep the two Core projects on `net9.0`; that target framework IS the guard that a Windows API cannot reach the platform-neutral layer
- the C# side is a Level 1 reimplementation of the documented contracts, never a translation of Android repository or ViewModel code
- UI rules live in `PicoSwitch.Companion.Services/Presentation/` as pure projections, never in a page: a rule in a
  code-behind cannot be tested, and every state the UI can reach must be reachable from a unit test
- a protocol change must move the shared fixture and BOTH languages in the same commit; the fixtures under `contracts/` are the only anti-drift mechanism this duplication has
- if `XamlCompiler.exe` fails with a bare `MSB3073 ... exited with code 1` and no message, rebuild with `-p:UseXamlCompilerExecutable=false` to get the real XAML error; a silent exit is a REPORTING failure and usually still means a genuine authoring mistake
- MSIX packaging (`build.ps1 -Msix`) needs .NET Framework MSBuild; `companions/windows/docs/README.md` §4 records why, so it is not rediagnosed

Current implementation state and the design it implements:

- `companions/windows/docs/README.md`

Source comments cite `WINDOWS_PASS.md` section numbers. That file was the
original task brief and was never committed; `companions/windows/docs/README.md`
is the record of what was built.

## Linux Companion Validation

If the task changes `companions/linux/` or a portable project under `companions/windows/src/`:

- the portable projects (Bridge.Core, Management.Core, Companion.Platform, Companion.Services) are SHARED with the Linux companion; a change there must keep `companions/windows/build.ps1` green AND the Linux suite: `dotnet test companions/linux/tests/PicoSwitch.Companion.Linux.Tests`
- BlueZ/D-Bus/SDL code lives only in `PicoSwitch.Companion.Linux`; `LayeringGuardTests` enforces it
- the Linux release is `companions/linux/packaging/build-linux.sh` (Linux only): one self-contained single-file executable per architecture. Not an AppImage: Steam Game Mode cannot mount one (see the script header); do not reintroduce it or a launch-option workaround
- the Windows release is `companions/windows/build.ps1 -Zip -Configuration Release -Platform x64|arm64`; Android is `./gradlew :app:assembleDebug` (release signing resolves only from outside the repository)
- Linux hardware validation is still pending; `companions/linux/README.md` §5–6 is the record and the open questions

## Web / Management Validation

If the task changes the web portal or management protocol:

- preserve the current CDC/BLE transport model
- preserve management authorization/security requirements
- run `python tests\run_tool_tests.py portal` (encoder goldens, page structure, paging rules,
  escaping, undefined calls)
- after a change to a page's markup or rendering, run `python tools\portal_preview.py --shots` and
  LOOK at `build/portal-preview/shots/`: the portal has no component tests, and a page that parses
  can still render wrongly. This is layout evidence only, never adapter behaviour
- new markup is built with `createElement`/`textContent`; do not add an `innerHTML` assignment
- the portal's Controller Link encoder is a fourth implementation of the bridge report: a
  contract change must update it in the same commit (`test_portal_controller_link.mjs` fails
  otherwise)
- run relevant management host tests
- check wire compatibility if command formats change
- run `python tools/check_management_command_parity.py` whenever a command verb is added,
  renamed, or removed on either side

A command reaches `handle_line()` in `firmware/src/settings/config.c` and, separately, must be accepted by
`config_wireless_command_allowed()` in `firmware/src/management/config_wireless_bridge.c`. Only the second gates BLE.
A verb added to the dispatcher alone compiles, passes every test, and works over USB CDC and UART
while answering `unknown command` over BLE — so the companion, which probes capabilities by
looking for exactly that reply, reports the feature missing and asks the user to update firmware
that already has it. `pairing start`/`status`/`cancel` shipped that way. The parity check compares
the companion's `ManagementCommands` against both C surfaces and fails loudly on the mismatch.
- verify browser-visible state is not merely cached stale state when testing synchronization behavior

Do not add a second configuration protocol when the existing management path can be extended cleanly.

## Firmware build variants

- `NS2_PRO=OFF` (the Switch 1 configuration) is compiled in CI as a build check
  only. It has not been run on hardware since the Switch 2 work began.
- `NS2_DISCARD_PRINTF` (default ON) compiles `printf()` out of project sources,
  because the firmware has no stdio back end. A `printf` argument must not have
  a side effect; `tests/tools/test_firmware_printf_policy.py` checks the
  textual cases.
- The product version comes from the root `VERSION` file;
  `tests/tools/test_version_consistency.py` holds the Android and Windows
  manifests to it.
