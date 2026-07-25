# UART protocol trace decoder and differ

PicoSwitch2 can retain a bounded console-side protocol trace while its USB-C port remains attached
to a Switch 2. UART0 on GP0/GP1 carries the trace to the PC without sharing the Bluetooth radio or
the console-facing USB link. The firmware tracer, PowerShell reader, and Python decoder are designed
as one evidence path:

1. Firmware records fixed-size events without formatting or UART work in USB callbacks.
2. `read_uart_diag.ps1` freezes the ring and pulls one JSON record per request.
3. `ns2_trace.py` validates, decodes, and semantically compares the resulting JSONL.

The tracer is disabled by default and does not include high-rate controller input or USB audio
packets in this first increment.

## Bluetooth capture/control over the same UART

The same UART command channel also controls the separate Switch 2 BLE capture ring while the dongle
remains console-attached:

```powershell
./tools/read_uart_diag.ps1 -Port COM11 -Command 'blecap start'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'blecap gattdisc on'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'blecap variant 9'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'blecap status'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'blecap dump' -OutputPath 'dumps/BLE CAPTURE/run.jsonl'
./tools/read_uart_diag.ps1 -Port COM11 -Command motionauto
./tools/read_uart_diag.ps1 -Port COM11 -Command 'magraw status'
```

GATT discovery and a numbered variant are mutually exclusive per connection. The normal production
path requires neither: it automatically starts the named native-Pro2 profile after ordinary BLE
initialization. `motionauto` is read-only and reports why that automatic gate has or has not fired.
The 2026-07-21 workflow used these commands to establish descriptor `0x0010`, compare the 30 ms and
7.5 ms link rates, and validate no-UART automatic startup. See the linked native-motion experiment
in [`report-0x09-motion.md`](report-0x09-motion.md).

High-rate native `0x1E`/`0x28` motion uses the separate `motionpair capture` ring and the
host-side [`ns2_magprobe.py`](uart-magprobe.md) decoder. That path aligns genuine quaternions and
normal G6/G7/G8 PDUs, filters escalation forms, preserves unexplained bits, and supports
pose-gated A/B comparisons without adding formatting work to the Bluetooth callback. The analyzer
also accepts historical full `blecap dump` JSONL directly and provides a cross-capture `corpus`
command; this recovered moving evidence without requiring another physical controller trial.
The UART-only `magraw on|off|status` experiment replays the reference initialization for the
independent raw-magnetometer feature profile, validates each ACK, and runs the complete native
profile to restore motion afterward; `ns2_magprobe.py rawmag` summarizes the resulting
handle-`0x000A` signed-int16 lanes. See
[`uart-magprobe.md`](uart-magprobe.md) for the guarded workflow.

## Capture workflow

With the Pico connected to the Switch and the 3.3 V UART adapter connected to the PC:

```powershell
./tools/read_uart_diag.ps1 -Port COM11 -Command 'trace clear'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'trace start'
./tools/read_uart_diag.ps1 -Port COM11 -Command reenumerate
Start-Sleep -Seconds 2
./tools/read_uart_diag.ps1 -Port COM11 -Command 'trace stop'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'trace dump' -OutputPath dumps/pro2-a.jsonl
```

`-OutputPath` writes UTF-8 JSONL without a BOM and still returns the same lines to the PowerShell
pipeline. The parent directory must already exist. A completed file contains chronological
`trace:record` objects followed by one `trace:end` object with retained and overwritten counts.

Trace payloads can include synthetic/per-unit serial data, Bluetooth addresses, and cryptographic
pairing challenges. Keep raw captures as bench data unless they have been reviewed and redacted.

## Readable timeline

```powershell
python tools/ns2_trace.py decode dumps/pro2-a.jsonl
```

The default view includes:

- elapsed time and inter-event delta, including 32-bit Pico timestamp rollover;
- direction, personality, event kind, command and subcommand names;
- EP0 request fields and identity VID/PID;
- firmware controller/BT/DSP versions;
- flash-memory read address and length;
- feature-mask names, input-report selection, player LEDs, and vibration sample IDs;
- HID output report ID, interface instance, and length.

Per-unit serial/address fields and pairing material are redacted. `--show-payload` prints the raw
24-byte retained prefix when exact bench inspection is required.

The parser fails closed on malformed JSON, missing fields, invalid enum values, sequence gaps,
payload-length disagreement, invalid hex, and a mismatched final record count.

## Semantic comparison

```powershell
python tools/ns2_trace.py diff dumps/pro2-a.jsonl dumps/pro2-b.jsonl
python tools/ns2_trace.py diff dumps/pro2-a.jsonl dumps/pro2-b.jsonl --strict
```

The default comparison aligns records by event type, direction, command/subcommand, length, and
memory address where applicable. Timestamps, controller addresses, serials, and fresh pairing
challenges are excluded from semantic equality. Known fields are compared by meaning; unknown
payloads remain byte-compared so the tool does not erase unexplained evidence.

`--strict` additionally compares every retained payload byte. `--show-equal` includes matched events
in the report. Exit status is `0` for equivalent traces, `1` for a valid trace with differences, and
`2` for invalid input or an I/O error.

## First hardware A/B result

On 2026-07-21, two ordinary Pro Controller 2 same-personality re-enumerations were captured from a
real Switch 2 while a genuine Pro Controller 2 was paired to the dongle. Both rings had zero
overwrites. The semantic differ:

- ignored the expected fresh AES challenge/response bytes (strict mode reported them);
- identified a player LED assignment change from `0x00` to `0x01`;
- identified a conditional `0x03/0x0C` exchange;
- showed that Init USB and feature-mask traffic can move relative to the initial HID output writes.

Therefore an initialization capture is not assumed to have one rigid total ordering. Comparisons
must preserve genuinely inserted/removed commands while tolerating session-specific pairing bytes
and timing.

## Automated coverage

```powershell
python tools/test_ns2_trace.py -v
```

Coverage includes known-field decoding, default redaction, 32-bit timestamp wrap, sequence and hex
corruption rejection, pairing-aware semantic versus strict comparison, and memory-address-aware
alignment.
