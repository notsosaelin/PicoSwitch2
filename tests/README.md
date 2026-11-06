# Tests

Automated validation that runs without hardware. Behavior that only the console
or a real controller can show is validated on hardware and recorded under
[`docs/`](../docs/README.md). A green suite here is not hardware validation.

| Command | What it runs | Needs |
| --- | --- | --- |
| `pwsh tests/run_host_tests.ps1` | Firmware logic compiled for the host (`tests/firmware/`) | PowerShell 7, gcc |
| `pwsh tests/run_host_tests.ps1 -Group motion` | One group (`management`, `bluetooth`, `input`, `drivers`, `usb`, `diag`, `motion`, `audio`, `nfc`) | as above |
| `pwsh tests/run_mgmt_tests.ps1` | The in-band management slice plus its structural Python checks | as above |
| `python tests/run_tool_tests.py` | Python, JavaScript and PowerShell tests for `tools/` and `web/` (`tests/tools/`) | Python 3, Node 18+, PowerShell 7, gcc |
| `pwsh tests/run_android_tests.ps1` | Android companion JVM unit tests | JDK 17, Android SDK |
| `pwsh companions/windows/build.ps1` | Windows companion build, tests and descriptor parity | .NET 9 SDK |
| `python tools/check_doc_links.py` | Every relative Markdown link resolves | Python |

## Layout

```text
tests/
├── firmware/          host tests for firmware modules (test_*.c), plus host_stubs/
├── tools/             tests for the analysis tools in tools/ and the web portal
├── fixtures/
│   ├── amiibo/        tag images read by the amiibo crypto vectors (contracts/amiibo/)
│   └── captures/      hardware captures that tests replay (BLE, motion, NFC)
└── run_*.ps1 / run_tool_tests.py
```

Wire contracts shared by firmware, companions and tests live in
[`contracts/`](../contracts/), not here: they are inputs to production code,
not test data.

## Rules

- `run_host_tests.ps1` is the single build manifest for host tests. Every
  `tests/firmware/test_*.c` must have a recipe there or an explicit reason in
  `$notHostTests`; the runner fails otherwise.
- Captures under `fixtures/captures/` are evidence. Do not edit one to make a
  test pass; record a new capture instead.
- Local lab output goes to the git-ignored `dumps/` directory at the repository
  root. A capture becomes a fixture only when a test needs it.
