---
name: picoswitch2-management-command
description: Add, change or remove a PicoSwitch2 management command across firmware, the BLE allowlist, the companions, the web portal, the shared fixtures and the protocol documentation. Use when a task touches handle_line, config_wireless_command_allowed, ManagementCommands, ManagementProtocol, contracts/management, or when a companion reports "unknown command" or "command unavailable over Bluetooth".
---

# PicoSwitch2 management command

One command has up to seven homes. Missing one compiles, passes most tests, and
fails for a user. `pairing start` shipped working over USB and UART while BLE
answered `unknown command`, so the companion told users to update firmware that
already had the feature.

## Before changing anything

Read `docs/management/PROTOCOL.md` for the command family and
`docs/management/CONFORMANCE.md` for what the fixtures pin. Decide:

1. **Transports.** USB CDC and UART only (bench or lab), or BLE as well?
2. **Authority.** What can a holder of a bonded session do with it? A command
   that reaches a physical tag, changes pairing state, or is an A/B switch for
   testing does not belong on BLE.
3. **Reply size.** A BLE reply must fit 511 bytes with every counter at its
   maximum. A larger answer needs a cursor or a second command.
4. **Core.** State owned by the BTstack thread (core 1) is reached by a deferred
   request and collected on a later tick. Core 0 must not busy-wait over BLE.

## The homes

| # | Where | What |
| --- | --- | --- |
| 1 | `firmware/src/settings/config.c`, `handle_line()` | Dispatch and the `cmd_*` handler. Bound every parse; reject trailing input |
| 2 | `firmware/src/management/config_wireless_bridge.c`, `config_wireless_command_allowed()` | Add the verb for BLE. Families are matched by prefix: a bench-only subcommand under an allowed prefix goes in the deny list at the top of that function |
| 3 | `tests/firmware/test_config_wireless_bridge.c` | Assert the new form is allowed, and that a near-miss is not |
| 4 | `contracts/management/protocol-v1.json` | Request and reply fixture. This is the only thing that keeps the two languages in step |
| 5 | Android: `companions/android/management-core/.../ManagementProtocol.kt` (commands and parsing) | Plus its conformance test |
| 6 | C#: `companions/windows/src/PicoSwitch.Management.Core/ManagementCommands.cs` and `ManagementProtocol.cs` | Shared by Windows and Linux. A Level 1 reimplementation of the contract, not a translation of the Kotlin |
| 7 | `docs/management/PROTOCOL.md` | The command row: transports, fields, errors, source function |

The web portal (`web/index.html`) sends raw command strings; update it if the
portal uses the command. Text from a reply is data: never put it into
`innerHTML` without `escapeHtml`.

A UART-only diagnostic lives in `firmware/src/diagnostics/ns2_uart_diag.c`
instead and needs none of the above.

## Rules

- The fixture, the firmware and both companion languages move in the same
  commit.
- An unknown reply field must be ignored by clients; a missing optional field
  must not be an error. Add fields, do not repurpose them.
- Changing a field's meaning, width or unit is a contract change. Check whether
  a version must move (`docs/management/PROTOCOL.md`,
  `contracts/android_controller_hid.h` for the bridge contract).
- No key material in any reply: no link keys, LTKs, IRKs or PIN bytes.
- A mutating BLE command states in a comment what the worst holder of an
  authorised session can do with it.

## Validate

```powershell
python tools\check_management_command_parity.py
pwsh -File tests\run_host_tests.ps1 -Group management
pwsh -File companions\windows\build.ps1
cd companions\android; .\gradlew --console=plain :management-core:test
```

The parity check compares verbs, not subcommands, so the host test in row 3 is
what proves a subcommand's reachability. Exercise the real command on hardware
through the UART passthrough when an adapter is attached (see the
`picoswitch2-hardware-session` skill); otherwise report hardware validation as
pending.
