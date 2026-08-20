# Management

This directory is the durable entry point for PicoSwitch2 management. It describes the logical
command/reply contract, the connected-session boundary, the production BLE carrier, and the
Android-free reference client.

The intended readers are firmware maintainers, Android maintainers, and developers implementing a
new management client. Management configures or inspects PicoSwitch2; it is separate from the
Controller Bridge, which supplies controller input.

## Where to start

| Need | Document |
|---|---|
| Understand boundaries and dependency direction | [Architecture](ARCHITECTURE.md) |
| Implement or audit exact commands and replies | [Protocol reference](PROTOCOL.md) |
| Implement BLE connection and request ownership | [Transports](TRANSPORTS.md) |
| Build a JVM or non-JVM client | [Client implementation](CLIENT_IMPLEMENTATION.md) |
| Verify a client against shared vectors | [Conformance](CONFORMANCE.md) |
| Understand significant choices | [Decisions](decisions/README.md) |

## Source-of-truth map

The evidence order is firmware behavior, automated tests, this normative contract, and then client
implementations. A disagreement between those surfaces is a defect.

| Concern | Current authority |
|---|---|
| Command dispatch and JSON formatting | `src/config.c`, especially `handle_line`, `cmd_kbm`, `cmd_amiibo`, `cmd_bonds` |
| Wireless command allowlist and bounded slots | `src/config_wireless_bridge.c` and `include/config_wireless_bridge.h` |
| BLE access decisions | `src/mgmt_access.c`, `src/mgmt_access.h` |
| BLE ATT carrier | `src/bt_hid/bt/btstack/btstack_host.c` |
| Bond cursor encoding | `src/mgmt_bonds.c`, `include/mgmt_bonds.h` |
| Android-free reference implementation | `android/companion/management-core` |
| Language-neutral vectors | `tools/fixtures/management/protocol-v1.json` |
| Host firmware regression suite | `tools/run_mgmt_tests.ps1` and its referenced tests |

The Kotlin module is reusable code for JVM clients, not the specification for other languages.
Non-JVM clients implement [PROTOCOL.md](PROTOCOL.md) and validate against the fixture.

## Evidence status

This portability pass changes no firmware wire behavior. Its code and contract are **Source-Tested**:
the firmware host suites, both board builds, shared JVM tests, Android tests, lint, and APK assembly
pass. No physical BLE, Switch 2, emulator, or instrumented-device validation was performed during
this pass. Existing hardware claims remain in `STATUS.md` and the focused Bluetooth evidence docs.
