# Contributing to PicoSwitch2

Thanks for helping. PicoSwitch2 is both a working adapter and a technical
reference for how the Switch 2 and its controllers behave, so a contribution is
judged on two things: does it work, and is the reasoning behind it recorded
well enough that the next person does not have to rediscover it.

## Getting set up

```powershell
git clone --recurse-submodules https://github.com/notsosaelin/PicoSwitch2.git
```

| To work on | You need | Start with |
| --- | --- | --- |
| Firmware | Raspberry Pi Pico VS Code extension (pinned toolchain), PowerShell 7 | [`firmware/README.md`](firmware/README.md) |
| Host tests | gcc (MinGW on Windows), PowerShell 7, Python 3, Node 18+ | [`tests/README.md`](tests/README.md) |
| Android companion | JDK 17 or 21, Android SDK | [`companions/android/README.md`](companions/android/README.md) |
| Windows companion | .NET 9 SDK | [`companions/windows/docs/README.md`](companions/windows/docs/README.md) |
| Web portal | a Chromium browser | [`web/`](web/) |

## Before you open a pull request

1. **Keep it to one coherent change.** A fix, a feature or a refactor, not all three.
2. **Test at the lowest practical layer.** Pure logic gets a host test in
   `tests/firmware/` (and a recipe in `tests/run_host_tests.ps1`); tool changes
   get a test in `tests/tools/`. A bug fix should come with a regression test.
3. **Run the suites your change touches.** At minimum, for firmware:
   ```powershell
   ./firmware/build.ps1                 # both boards must build
   pwsh tests/run_host_tests.ps1
   python tests/run_tool_tests.py
   python tools/check_doc_links.py
   ```
   CI runs these, plus both companion suites, on every pull request.
4. **Say what you validated and how.** "Builds and host tests pass" and
   "confirmed on a Switch 2 with a DualSense" are different claims. Never
   describe something as hardware-confirmed that was only tested in software.
5. **Update the documentation your change affects.** Behavior changes update the
   relevant protocol or architecture document; meaningful state changes update
   [`STATUS.md`](STATUS.md); user-visible changes get a line in
   [`CHANGELOG.md`](CHANGELOG.md).

## Protocol and reverse-engineering work

Claims about console or controller behavior need evidence. Read
[`docs/re-methodology/evidence-standards.md`](docs/re-methodology/evidence-standards.md)
and use its confidence tiers (Confirmed, Strong Evidence, Hypothesis, Unknown).
Name fields neutrally until their meaning is shown, not just correlated once.

An experiment that answers a protocol question gets a record in
[`docs/experiments/`](docs/experiments/README.md). Disproven theories are kept,
marked as refuted, in [`refuted-hypotheses.md`](docs/experiments/refuted-hypotheses.md);
check it before proposing one.

Do not change peer-visible contracts (USB descriptors, report layouts, the
bridge contract in [`contracts/`](contracts/), the management wire format)
without deciding whether the contract version must change. The parity and
contract tests exist to catch that; never weaken one to make a build pass.

## Captures and binary files

- Lab tools write to the git-ignored `dumps/` directory. Commit a capture only
  when a test replays it, under `tests/fixtures/captures/`, with its provenance
  (controller, firmware, console version, date, method) recorded in the test or
  document that uses it.
- Do not commit videos, photos, large captures, controller firmware images, or
  key material. Large evidence is indexed in
  [`docs/lab-evidence-index.md`](docs/lab-evidence-index.md) instead.

## Commits

Use the [Conventional Commits](https://www.conventionalcommits.org/) style the
history already follows, with a scope where it helps:

```text
fix(report05): stamp the IMU sample's own time, not the report's departure
test(bootsel): bound the concurrent park rounds by wall clock, not iterations
docs(architecture): ground the audio section in our own trimmed CELT encoder
```

The body explains why, and what evidence supports the change.

## Code style

Follow the surrounding code. C is formatted per [`.clang-format`](.clang-format).
Comments explain why: protocol reasoning, hardware quirks, invariants, and the
evidence behind a constant. Hypotheses belong in documentation, not in source
comments.

## Licensing

PicoSwitch2 is licensed under the [Apache License 2.0](LICENSE). By
contributing you agree that your contribution is licensed under the same terms.
Vendored code keeps its own license (for example
[`firmware/src/hid_host/LICENSE`](firmware/src/hid_host/LICENSE)).
