# Controller-side command atlas

Status: offline corpus audit complete on 2026-08-13; new hardware coverage remains bounded.

## Evidence boundary

`tools/ns2_command_atlas.py` accepts both console-side `trace` JSONL and controller-side
`blecap` JSONL. It admits a capture only when the terminal record declares the exact record count
and explicitly reports zero `overwritten`/`dropped` records. Older NDJSON without terminal loss
metadata is excluded rather than assumed complete.

The atlas reports observed packet shapes. `inferred_name` is a label from the active command audit,
not a conclusion derived from payload shape. Each request and response retains:

- capture boundary (`console_side` or `controller_side`);
- link transport (`usb_vendor_bulk` or `bluetooth_gatt`);
- the command header's independently observed transport byte;
- BLE value handles, when present;
- declared and captured lengths, completeness, payload hashes, and source-file hashes.

The schema deliberately does not label every `blecap` response "genuine." The capture format proves
where the bytes were observed, not which physical device generated them. The experiment record for
the two controller-command captures establishes that their source was a genuine Pro Controller 2.

## Reproducible 2026-08-13 corpus result

The repository contains 46 admissible `trace` captures and 30 admissible `blecap` captures. Across
them, the atlas finds 30 command/subcommand pairs. Only these two lossless BLE captures contain
framed controller commands:

- `dumps/BLE CAPTURE/sw2_uart_variant8_console_core_tight_2026-07-21.jsonl`
- `dumps/BLE CAPTURE/sw2_uart_variant8_verified_rate_2026-07-21.jsonl`

Their controller-side coverage is:

| Command/subcommand | Observed transaction | Request handle | Response handle | Completeness |
|---|---|---:|---:|---|
| `0x02/0x04` | initialization memory reads | `0x0014` | `0x001A` | request complete; some 80-byte responses captured only to 64 bytes |
| `0x0C/0x02` | feature configure | `0x0014` | `0x001A` | complete |
| `0x0C/0x04` | feature enable | `0x0014` | `0x001A` | complete |

Both captures repeat the same variant-8 initialization path. They do not provide independent
controller-side evidence for LED, rumble, reconnect/power, headset control, NFC, or firmware-update
commands. Console-side records for those features cannot close that provenance gap.

Run the focused controller-side atlas with:

```powershell
python tools/ns2_command_atlas.py `
  'dumps/BLE CAPTURE/sw2_uart_variant8_console_core_tight_2026-07-21.jsonl' `
  'dumps/BLE CAPTURE/sw2_uart_variant8_verified_rate_2026-07-21.jsonl'
```

Use `--json` when exact handles, header transport values, payload hashes, or source hashes are
needed. Do not add legacy captures lacking terminal loss metadata merely to increase coverage.

## Ranked gaps

The next capture should settle one state discriminator, in this order:

1. **Reconnect/power transition** — highest user value and passive to observe. Capture a bonded
   genuine Pro Controller 2 disconnect, power-on, and restored input; distinguish a controller
   command/state transition from ordinary GATT reconnect traffic.
2. **Player LED and rumble** — high gameplay value and reversible. Change exactly one player state
   or trigger one controlled rumble event, then compare against an idle control window.
3. **Headset/audio control** — high value but audio-regression sensitive. Start from the established
   speaker baseline and vary only headset insertion or one audio control action.
4. **Native NFC** — valuable but requires a physical tag and tighter state control. Capture one
   read lifecycle before attempting controller-side write enablement.
5. **Firmware update** — potentially high protocol value but opportunity-driven and destructive if
   mishandled. Keep the capture-only sink ready and wait for a real console update.

Initialization is already represented and should not receive another broad capture. The existing
BLE capture path and `PicoSwitch2Lab.psm1` can package every ranked experiment, so this audit found
no justification for another generic runner or artifact format.

Promotion still requires a zero-loss capture, a semantic A/B discriminator, an update to the
relevant active protocol document, and a replay fixture when the transaction is deterministic.
