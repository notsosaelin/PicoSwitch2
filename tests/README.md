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
| `dotnet test companions/linux/tests/PicoSwitch.Companion.Linux.Tests` | Linux companion backend (BlueZ transport via a fake bus, SDL mapping, single instance; real-bus and socket tests run only on Linux) | .NET 9 SDK |
| `bash companions/linux/packaging/build-linux.sh` | Linux tests, then the x64 and arm64 single-file executables | .NET 9 SDK, Linux |
| `python tools/check_management_command_parity.py` | Every companion command is dispatched by the firmware and reachable over BLE | Python |
| `python tools/check_doc_links.py` | Every relative Markdown link resolves | Python |

## Layout

```text
tests/
├── firmware/          host tests for firmware modules (test_*.c), plus host_stubs/
├── tools/             tests for the analysis tools in tools/ and the web portal
├── fixtures/
│   ├── amiibo/        LOCAL ONLY, git-ignored: genuine tag images (see below)
│   ├── captures/      hardware captures that tests replay (BLE, motion, NFC)
│   └── motion40/      length-0x28 motion research data; nothing includes it
└── run_*.ps1 / run_tool_tests.py
```

### Amiibo dumps are not in the repository

`tests/fixtures/amiibo/` and `contracts/amiibo/library-archive-v3.zip` were
removed on 2026-11-11 and are git-ignored. A tag image is a copy of a physical
figure, and a set-up one carries its owner's names. (They remain in history
before that date.)

What that costs, measured 2026-11-11:

| Suite | Without the dumps |
| --- | --- |
| `AmiiboLibraryTests` (C#) | 30 run against synthetic images of the same structure; 4 skip (the bulk import of a real collection, and the three checks against the archive the Android companion wrote) |
| `AmiiboCryptoTests` (C#) | 10 skip: every check that reads a vector's source dump |
| `AmiiboCryptoFixtureTest` (Kotlin) | 6 skip for the same reason (plus the key-dependent ones that always skipped without `-Damiibo.retailKey`) |
| `tests/tools/test_ns2_v3_compat_view.mjs` | Prints SKIPPED and exits 0, so the tool-test runner counts it as a pass |

`contracts/amiibo/crypto-vectors.json` stays: it holds digests and expected
values, no tag bytes. To run the skipped tests, put your own dumps back under
the file names the vectors list (and regenerate the archive with
`AmiiboCryptoFixtureTool`, `-Damiibo.regenerate=true`). A present file always
wins over a synthetic image. **A skipped test is not a passed one:** say which
were skipped when reporting, and note that the cross-implementation amiibo
crypto check does not run on a clean checkout.

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
