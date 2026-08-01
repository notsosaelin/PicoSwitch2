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
- The length-`0x28` PDU carries a packed multi-sample IMU payload plus a mode-3 orientation
  carrier. There is no magnetometer lane: the former `G6/G7/G8` "reference vector" bit ranges cross
  real packed gyro and acceleration samples, which is why a controlled magnet campaign found no
  polarity or distance response. Do not reintroduce a magnetometer premise, and do not repeat the
  refuted static-template generator — holding the other changing lanes static causes random motion.
  A software `0x28` generator remains a valid long-term target only once every changing lane can be
  synthesized coherently, including exact integer projection/rounding.
- Production DualSense/Edge motion uses the validated length-`0x1E` carrier.
- Pico 2 W uses the validated 300 MHz audio build. Pico W intentionally retains its non-audio
  profile.
- Controller-family remapping is intentionally absent. Keep the compiled base map stable and leave
  user remapping to the Switch's persistent emulated-controller settings.
- Every flashed UF2 must retain the page-aligned install-reset marker. First boot clears settings,
  Virtual Amiibo banks, wake identity, and Bluetooth bonds; ordinary reboots must not.
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

Python host tests:

```powershell
python tools\test_ns2_trace.py
python tools\test_ns2_nfc_semantics.py
python tools\test_amiibo_corpus.py
```

The figure-v3 NFC state machine is host-replayable. A deterministic v3 parsing,
state, timing, or record-layout bug belongs in this test, not on hardware:

```powershell
.\build\host-tests\test_ns2_amiibo_v3_runtime.exe
```

Motion-specific checks:

```powershell
.\build\host-tests\build-host-test-ns2-motion-pdu.exe
.\build\host-tests\build-host-test-ns2-motion-pdu40.exe
.\build\host-tests\build-host-test-ns2-ds5-motion.exe
.\build\host-tests\build-host-test-ns2-ds5-motion40.exe
python tools\test_ns2_magprobe.py
python tools\test_ns2_motion_packet.py
python tools\test_ns2_motion_carrier.py
```

`build-host-test-ns2-motion-pdu40` holds both firmware length-`0x28` packers to
byte-exactness against genuine hardware: **981 catch-up** and **853 high-rate**
packets, plus edge cases the corpus never reaches. Status is written verbatim in
both — 5 genuine packets carry status `0x00`, so a `status ? status : default`
idiom silently rewrites real data. Build it with:

```powershell
gcc -Iinclude -Itools\fixtures -Wall -Wextra `
    -o build\host-tests\build-host-test-ns2-motion-pdu40.exe `
    tools\test_ns2_motion_pdu40.c src\bt_hid\motion\ns2_motion_pdu.c
```

Its fixture is generated, not hand-written — regenerate after any capture
corpus change with `python tools\gen_motion40_fixture.py`.

`build-host-test-ns2-ds5-motion40` covers the layer above the packer — sample
scaling, slot placement across the emit window, cadence, saturation clamping,
the prefix epoch, and the modular prefix slice against 30 reference vectors:

```powershell
gcc -Iinclude -Itools\fixtures -Wall -Wextra `
    -o build\host-tests\build-host-test-ns2-ds5-motion40.exe `
    tools\test_ns2_ds5_motion40.c src\bt_hid\motion\ns2_ds5_motion40.c `
    src\bt_hid\motion\ns2_motion_pdu.c
```

### Length-`0x28` readiness gate — run before requesting a flash

```powershell
python tools/ns2_motion40_validate.py     # exits non-zero unless every
                                          # capture-answerable question passes
```

Byte-exact validation of a *generated* `0x28` is impossible from BLE captures:
the controller transmits derived products, never its inputs. The internal
800 Hz IMU stream is not sent (handle `0x000A` runs at the notification rate),
and the carrier at the prefix's epoch instant is not sent either — across 449
genuine packets the epoch coordinate landed on a transmitted `0x1E` **zero**
times. The bar is therefore physical accuracy against interpolated truth with a
stated per-field tolerance, not byte equality.

Supporting measurements, each re-runnable:

```powershell
python tools/ns2_motion40_prefix_epoch.py   # when the prefix describes
python tools/ns2_motion40_slot_timing.py    # where slots sit in the window
python tools/ns2_motion40_gyro_axes.py      # gyro axis order and sign
```

### Length-`0x28` motion gate (DEFAULT OFF)

```text
ds5motion pdu40 on       # emit interleaved high-rate 0x28 + 0x1E
ds5motion pdu40 off      # return to the proven 0x1E carrier
ds5motion pdu40 status   # emitted / starved / overlong / saturated counters
```

Emission is **interleaved high-rate**: the `0x1E` carrier fills the polls
between `0x28` packets, which is what genuine hardware does at this cadence.
That is not a preference — it supplies the chart state the modular prefix is
unwrapped against, and it delivers each `0x28` exactly once instead of ~20
times at a 1 kHz poll. High-rate is the layout with paired ground truth: 768 of
773 genuine `0x28` packets that have a `0x1E` alongside them are high-rate.

`packing_mode`, not elapsed, is the layout discriminator. Elapsed only selects
among the mode-3 cadence layouts; five corpus packets carry mode 0 and are a
different structure entirely. `starved > 0` means the emit interval outran the source sample
rate; saturation means motion exceeded the wire range (±2 g, ±499.5 dps — which
is the sensor's own full scale, not a codec artifact) or the scaling is wrong.
Both distinguish "well-formed but wrong" from "working".

A byte-exact packet can still describe the wrong timeline. Two invariants above
the packer are load-bearing and easy to break:

- **Slots span the emit window.** Slot 0 is the oldest sample in the window and
  the last slot the newest; filling from the first samples to arrive discards
  the freshest data. Evidence and reproduction:
  `python tools\ns2_motion40_slot_timing.py`.
- **`elapsed` describes the samples, not the poll.** `last_emit_us` advances by
  exactly the elapsed reported so truncation remainders carry forward rather
  than drifting; `last_sample_us` separately bounds the next selection window
  so a sample can never appear in two packets.

**Not hardware-validated.** Use it only for a deliberate A/B against `0x1E`.

Install-reset image checks:

```powershell
python tools\verify_install_reset_marker.py build\pico2_w\PicoSwitchWGA-pico2_w.bin --flash-size 0x400000
python tools\verify_install_reset_marker.py build\pico_w\PicoSwitchWGA-pico_w.bin --flash-size 0x200000
```

Build success is not hardware validation. State exactly which level was checked.

## Hardware and UART workflow

- The maintainer performs flashes and physical controller tests.
- A headered Pico 2 W can remain connected to the Switch while UART connects to a PC.
- Discover the local port with `tools\read_uart_diag.ps1 -List`; do not assume `COM11` on a new PC.
- Only run live UART mutations after the maintainer says the expected controller/personality is
  connected and ready.
- Prefer passive capture and exact A/B experiments over repeated blind firmware guesses.
- For non-NFC protocol work, follow
  [`docs/re-methodology/controller-protocol-lab.md`](docs/re-methodology/controller-protocol-lab.md).
  Use the domain runner (`motion_lab.ps1`, `audio_lab.ps1`, or `firmware_lab.ps1`) so every hardware
  action records Git/build provenance, diagnostics, complete captures, analysis, fixtures, and
  hashes. A dropped/overwritten capture cannot become a golden fixture.
- For NFC/amiibo work, follow
  [`docs/re-methodology/nfc-investigation-workflow.md`](docs/re-methodology/nfc-investigation-workflow.md):
  classify the failing layer, run `tools/amiibo_corpus.py` on any new dump, decode existing captures
  with `python tools/ns2_trace.py nfc`, and only then run one instrumented
  `tools/nfc_lab.ps1` experiment naming its single intended variable. Console status values are not
  diagnoses — `2115-0096` / `07 41` has had two unrelated causes.

## Git and release workflow

- Work on `ns2-testing` unless the maintainer directs otherwise.
- Preserve unrelated user changes.
- Update `STATUS.md`, `PLAN.md`, `CHANGELOG.md`, compatibility documentation, and protocol evidence
  when behavior changes materially.
- Commit, push, or publish a release only when the maintainer requests it.
- A release requires both board builds and the relevant hardware regression checklist.
