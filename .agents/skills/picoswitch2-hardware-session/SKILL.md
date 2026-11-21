---
name: picoswitch2-hardware-session
description: Work with a physically attached PicoSwitch2 adapter. Discover the UART bridge, read state without disturbing it, flash a development build through the UART bootsel command, and collect diagnostics for a report. Use when a task needs live adapter state, a flash, a UART query, btstate or fault output, or when about to ask the maintainer to read something off a terminal.
---

# PicoSwitch2 hardware session

The adapter exposes a line-based diagnostic console on GP0/GP1 through a USB
UART bridge (115200 8N1). Use it before asking the maintainer for anything a
command can read.

## 1. Discover, never assume

```powershell
pwsh -File tools\read_uart_diag.ps1 -List
```

Do not reuse a COM port from an earlier session or a document. If no bridge is
listed, there is no UART attached: say so and stop. Do not guess a port. Some
test rigs have no adapter UART at all (check the session's memory).

## 2. Read state first

Reading is safe. Establish the adapter's state before anything that changes it.

```powershell
pwsh -File tools\uart_query.ps1 -Command 'ping'
pwsh -File tools\uart_query.ps1 -Command 'btstate'
pwsh -File tools\uart_query.ps1 -Command 'fault'
```

- `btstate`: personality, radio state, connections, the pairing window, the
  management service (`cble`), and lifecycle counters.
- `fault`: the last HardFault record and the core 1 program-counter samples.
  Map addresses with `arm-none-eabi-addr2line` on the ELF of the SAME build.
- The firmware build identity in the replies says what is actually flashed.
  Compare it with `git rev-parse --short=8 HEAD` before drawing a conclusion
  from source.

The full command list is in `firmware/src/diagnostics/ns2_uart_diag.c` and
`docs/bluetooth/DIAGNOSTICS.md`. Management commands can be run through the
UART passthrough, which is how to test a command without a companion.

## 3. Before a mutating command

Know the personality, whether a console is attached and awake, and whether a
controller or a companion session is live. Then ask whether the maintainer is
using the adapter. These change state: pairing and wipe commands, `save`,
personality changes, `reenumerate`, amiibo writes, `bootsel`.

Never run a wipe, a bond removal or a clean-install (`-wipe.uf2`) flash without
an explicit request: they erase pairings the maintainer has to redo by hand.

## 4. Flash a development build

The physical BOOTSEL button is bound to pairing gestures, so the bootloader is
entered over UART:

1. Build: `cmake --build firmware\build\<board> --parallel`. Use the UPDATE
   UF2 (`PicoSwitchWGA-<board>.uf2`), which keeps settings and pairings.
2. Confirm the board: Pico W and Pico 2 W images are not interchangeable.
3. `pwsh -File tools\uart_query.ps1 -Command 'bootsel'`
4. Wait for the `RPI-RP2` / `RP2350` drive, copy the `.uf2` to it. The adapter
   reboots into the new build by itself.
5. Read the build identity again and confirm it is the one just built.

Flashing interrupts whatever the adapter was doing. Get the maintainer's
go-ahead for the first flash of a session unless they already asked for it. A
build older than the `bootsel` command needs a physical unplug-and-hold.

## 5. Capture evidence

On Pico W, build with `firmwareuild.ps1 pico_w -DiagProfile lab` before a soak, a
full protocol trace or a BLE capture: ordinary Pico W builds carry short
diagnostic rings (`docs/bluetooth/DIAGNOSTICS.md`, "Diagnostic buffer profile").
Pico 2 W always has the full sizes.

Use the lab runners instead of ad-hoc captures; they record provenance:
`tools\motion_lab.ps1`, `tools\nfc_lab.ps1`, `tools\firmware_lab.ps1`,
`tools\mgmt_watch.ps1`, `tools\mgmt_soak.ps1`. Output goes to the git-ignored
`dumps\`. A capture that dropped or overwrote records cannot become a fixture.

## 6. Report

Say what was observed, on which board, at which firmware build identity, with
which controller and console state. Separate what tooling read from what the
maintainer saw. An observation is not a diagnosis: a console error code or a
counter value is recorded as seen. If the result changes validated state, update
`STATUS.md` and `docs/status/compatibility-matrix.md`.
