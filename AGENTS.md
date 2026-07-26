# PicoSwitch2 Agent Guide

This file is the durable repository entry point for Codex and other coding agents. Read it before
changing code. Then read:

1. [`STATUS.md`](STATUS.md) — current validated state and open blockers
2. [`PLAN.md`](PLAN.md) — prioritized roadmap
3. [`docs/README.md`](docs/README.md) — documentation authority and map
4. [`docs/LLM/current-context.md`](docs/LLM/current-context.md) — concise continuation handoff
5. The protocol or architecture document relevant to the task

`CLAUDE.md` contains the longer maintainer philosophy. It remains applicable, but current source,
captures, tests, and the documents above outrank old plans or conversation history.

## Project invariants

- The goal is behavior as close to genuine Nintendo hardware as practical, without regressing
  already validated input, rumble, audio, motion, reconnect, wake, LED, BOOTSEL, or config behavior.
- Treat direct Switch 2/UART captures and hardware tests as primary evidence.
- Do not treat third-party projects—including BlueRetro—as definitive Switch 2 protocol truth.
- Never promote a hypothesis to fact. Record negative results in
  `docs/experiments/refuted-hypotheses.md`.
- Preserve historical experiment reports and `.archived.md` files. Correct active summaries and
  links instead of rewriting what an old experiment observed.
- Treat a software-generated length-`0x28` DualSense reference/magnetometer solution as a valid
  long-term target. Do not repeat the refuted static-template experiment: hardware proved that
  replacing only timing and G6/G7/G8 while holding the other changing lanes static causes random
  motion. A future generator must model every console-relevant changing lane coherently.
- Production DualSense/Edge motion uses the validated length-`0x1E` carrier.
- Pico 2 W uses the validated 300 MHz audio build. Pico W intentionally retains its non-audio
  profile.
- Configuration mode is CDC-only. Serve `web/index.html` locally with
  `tools/run_config_portal.ps1`; do not reintroduce an MSC drive or embedded web disk.

## Repository layout

- `src/bt_hid/motion/` — genuine Pro2 motion ownership, DualSense translation, codecs, probes
- `src/switch_pro2/` — console-facing Pro Controller 2 personality
- `docs/switch2/` — current Switch 2 protocol references
- `docs/bluetooth/` — controller input/output and translation references
- `docs/experiments/` — dated methods/results, including negative evidence
- `docs/archive/` — historical files, always suffixed `.archived.md`
- `dumps/` — captures and derived media; read `dumps/README.md`
- `tools/` — UART, decoder, capture, and host-test utilities

## Build and verification

For a fresh workstation, clone the active branch with its Opus submodule:

```powershell
git clone --recurse-submodules -b ns2-testing https://github.com/notsosaelin/PicoSwitch2.git
```

If the repository was cloned without submodules:

```powershell
git submodule update --init --recursive
```

The Windows build helper expects the Raspberry Pi Pico VS Code extension toolchain described in
`README.md`. On a newly configured machine, use `.\build.ps1 pico2_w` or `.\build.ps1 pico_w` to
create the build directory before using direct `cmake --build` commands.

Use existing configured build directories when present:

```powershell
cmake --build build\pico2_w --config Release --parallel
cmake --build build\pico_w --config Release --parallel
```

Run all compiled host tests:

```powershell
$tests = Get-ChildItem build\host-tests -File -Filter 'test_*.exe'
foreach ($test in $tests) {
    & $test.FullName
    if ($LASTEXITCODE -ne 0) { throw "$($test.Name) failed" }
}
```

Motion-specific checks:

```powershell
.\build\host-tests\build-host-test-ns2-motion-pdu.exe
.\build\host-tests\build-host-test-ns2-ds5-motion.exe
python tools\test_ns2_magprobe.py
```

Build success is not hardware validation. State exactly which level was checked.

## Hardware and UART workflow

- The maintainer performs flashes and physical controller tests.
- A headered Pico 2 W can remain connected to the Switch while UART connects to a PC.
- Discover the local port with `tools\read_uart_diag.ps1 -List`; do not assume `COM11` on a new PC.
- Only run live UART mutations after the maintainer says the expected controller/personality is
  connected and ready.
- Prefer passive capture and exact A/B experiments over repeated blind firmware guesses.

## Git and release workflow

- Work on `ns2-testing` unless the maintainer directs otherwise.
- Preserve unrelated user changes.
- Update `STATUS.md`, `PLAN.md`, `CHANGELOG.md`, compatibility documentation, and protocol evidence
  when behavior changes materially.
- Commit, push, or publish a release only when the maintainer requests it.
- A release requires both board builds and the relevant hardware regression checklist.
