## What and why

<!-- What this changes, and the problem it solves. Link an issue if there is one. -->

## Validation

<!-- Say exactly what was run and at what level. Software tests are not hardware validation. -->

- [ ] Both firmware boards build (`./firmware/build.ps1`)
- [ ] Host tests pass (`pwsh tests/run_host_tests.ps1`)
- [ ] Tool tests pass (`python tests/run_tool_tests.py`)
- [ ] Companion tests pass, if a companion changed
- [ ] Hardware validation: <!-- what was tested, on which board, controller and console, or "none" -->

## Contracts and documentation

- [ ] No peer-visible contract changed, or the contract version and parity fixtures were updated
- [ ] Affected docs, `STATUS.md` and `CHANGELOG.md` are updated
