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
./tools/read_uart_diag.ps1 -Port COM11 -Command 'imuref status'
```

GATT discovery and a numbered variant are mutually exclusive per connection. The normal production
path requires neither: it automatically starts the named native-Pro2 profile after ordinary BLE
initialization. `motionauto` is read-only and reports why that automatic gate has or has not fired.
The 2026-07-21 workflow used these commands to establish descriptor `0x0010`, compare the 30 ms and
7.5 ms link rates, and validate no-UART automatic startup. See the linked native-motion experiment
in [`report-0x09-motion.md`](report-0x09-motion.md).

High-rate native `0x1E`/`0x28` motion uses the separate `motionpair capture` ring and the
host-side [`ns2_motion_reference.py`](../../tools/ns2_motion_reference.py) and historical
[`ns2_magprobe.py`](uart-magprobe.md) decoders. The passive `motionpair trigger` path continuously
retains 63 pre-trigger records, detects a length-`0x1E` chart change, retains 64 post-trigger
records, and stops automatically. The production motion stream is not modified. One complete
transition can be armed, awaited, validated, drained, and saved through the same UART session:

```powershell
./tools/read_uart_diag.ps1 -Port COM11 `
  -Command 'motionpair trigger capture' -TimeoutMs 30000 `
  -OutputPath 'dumps/BLE CAPTURE/pro2-chart-transition.jsonl'
```

The diagnostic-only unresolved trigger was used for opportunistic state-2
coverage during ordinary play. It ignores states 0, 1, and 3 and freezes only
when a transition involves state 2. It updates its baseline across every
ignored transition, so it cannot manufacture a false boundary:

```powershell
./tools/read_uart_diag.ps1 -Port COM11 -Command 'motionpair trigger unresolved'
# Play normally; no deliberate controller motion is required.
./tools/read_uart_diag.ps1 -Port COM11 -Command 'motionpair status'
# Only after chart.complete becomes true:
./tools/read_uart_diag.ps1 -Port COM11 -Command 'motionpair dump' `
  -OutputPath 'dumps/BLE CAPTURE/pro2-chart-unresolved.jsonl'
```

The status reports `target_mask: 4` for this mode. The capture ring is volatile;
adapter power loss discards an armed or completed window. Do not call
`motionpair dump` early because dumping intentionally stops the capture.
The first successful run captured the zero-drop `3 → 2 → 3` fixture documented
in
[`../experiments/pro2-carrier-chart-transition-2026-07-29.md`](../experiments/pro2-carrier-chart-transition-2026-07-29.md);
the command remains useful for repeatability controls rather than missing-state
discovery.

That path supports time-aligned A/B comparisons without adding formatting work to the Bluetooth
callback. The analyzers also accept historical full `blecap dump` JSONL directly and provide
cross-capture corpus operations; this recovered moving evidence without requiring another
physical controller trial.
The UART-only command name `magraw on|off|status` is historical. The guarded experiment replays the
candidate initialization, validates each ACK, and runs the complete native profile to restore
motion afterward; `ns2_magprobe.py rawmag` summarizes handle-`0x000A` signed-int16 lanes. Corpus
analysis disproved the original interpretation: the public GATT path did not expose an independent
raw-magnetometer stream. Keep the command only as a reproducible negative experiment. See
[`uart-magprobe.md`](uart-magprobe.md) for the guarded workflow.

`imuref on|off|status` is the current raw-IMU selector recovered from the genuine
`btle_procon2_motion_0x000A.pcapng` control sequence. It uses feature `0x2F` and the common-report
CCC, counts both common/native notifications, and restores the production native profile on
`imuref off`. `imuref dual on|off` is a nested, UART-only discriminator that toggles the native
CCC without changing the raw selector. Hardware confirms native priority rather than simultaneous
delivery; disabling that CCC resumes raw reports. `imuref interval 6-24` requests a controlled BLE
connection interval in 1.25 ms units while the raw experiment is active; production restore
returns to the validated six-unit link and resets the diagnostic target accordingly. The completed
cadence matrix establishes exact length-`0x28` format branches at tick 11 and tick 15; use this
command for prefix/tail or escalation experiments, not to rediscover the sample maps.
Analyze its `blecap dump` JSONL with `ns2_motion_reference.py --blecap`.

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

## Live Virtual Amiibo export

The portal's USB CDC transport cannot be used while the Pico USB port remains attached to the
Switch. UART therefore exposes a bounded read surface for the active mutable tag:

```powershell
./tools/read_uart_diag.ps1 -Port COM11 -Command 'amiibo status'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'amiibo dump' `
  -OutputPath 'dumps/amiibo-after-console-write.bin'
```

The helper snapshots the image size/generation, pulls at most 64 bytes per request, rejects a
generation change, validates exact 540/572-byte length and NTAG215 UID/BCC, writes the binary, and
only then acknowledges the download to clear dirty protection. No polling, flash write, or USB
personality change occurs. The firmware-side primitives are `amiibo status`,
`amiibo read OFFSET`, and `amiibo acknowledge`.

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

## NFC transaction view

Command `0x01` traffic needs reassembly before it means anything: staging and retrieval are
multi-chunk, and a per-record view cannot classify an envelope it has only seen a third of. Two
extra operations provide that view, built on the shared layout module `tools/ns2_nfc_semantics.py`:

```powershell
python tools/ns2_trace.py nfc <capture.jsonl>
python tools/ns2_trace.py nfc-diff <genuine.jsonl> <virtual.jsonl> [--ignore-identity]
```

`decode` also gained per-record NFC fields (status state names, descriptors, chunk offsets) from
the same module, so the two views never disagree about what a byte is called. Full workflow and
rationale: [`../re-methodology/nfc-investigation-workflow.md`](../re-methodology/nfc-investigation-workflow.md).

## Automated coverage

```powershell
python tools/test_ns2_trace.py -v
python tools/test_ns2_nfc_semantics.py -v
```

`test_ns2_trace.py` covers known-field decoding, default redaction, 32-bit timestamp wrap, sequence
and hex corruption rejection, pairing-aware semantic versus strict comparison, and
memory-address-aware alignment.

`test_ns2_nfc_semantics.py` pins each capture-derived NFC layout and adds regression assertions
against real committed captures: the escalated 4-block descriptor, the 83-byte device result, the
allocation-relative King Dedede records, the tracer-truncation versus transport-fault distinction,
and the pairing of each error state with the operation in flight.
