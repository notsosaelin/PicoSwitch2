---
name: picoswitch2-firmware-tap
description: Safely analyze or implement capture-only Switch 2 controller firmware-update research for command 0x0D, firmware SPI images, failsafe banks, CRC validation, and CDC export. Use for firmware blob extraction, console update capture, update state machines, or version/bank comparison. Never use it to flash Nintendo firmware to a Pico or genuine controller.
---

# PicoSwitch2 firmware tap

## Hold the safety boundary

Read `AGENTS.md`, `docs/switch2/firmware-versioning.md`,
`docs/switch2/command-surface.md`, and the genuine memory-layout evidence.

The repository already has a genuine 2 MB Pro Controller 2 SPI dump. Main MCU
images target Nintendo hardware and cannot run on RP2040/RP2350. This workflow
captures bytes only.

Never:

- send a write/update command to a genuine controller;
- use Nintendo's failsafe address as a Pico flash address;
- let command `0x0D/02` select a local write destination;
- commit a proprietary firmware blob;
- report success from a truncated capture.

## Validate offline first

Exercise the host state machine:

```powershell
python tools/test_ns2_firmware_update.py -v
```

Analyze a complete retained trace:

```powershell
./tools/firmware_lab.ps1 -Trace <full-0x0d-trace.jsonl>
```

`ns2_firmware_update.py` enforces `01→02→03→04*→05→06→07`, validates
`0x15000`/`0x75000`, declared size, USB chunk length `1..0x4C`, observed byte
count, and CRC32. It rejects the generic tracer's 24-byte truncation and tells
the operator to use the dedicated sink.

## Implement the on-device sink in stages

1. Prove the capture partition cannot overlap code, config, Bluetooth TLV,
   Virtual Amiibo banks, or the install-reset marker.
2. Keep the handler behind a research-only compile definition.
3. Journal metadata and progressively flush 4 KiB sectors.
4. Defer ACK while flash is unavailable; never buffer beyond declared size.
5. Persist incomplete state safely across power loss.
6. Export over CDC only after the console session ends.
7. Compare exported size, CRC32, SHA-256, header, region, and declared address.

Do not guess ACK form. Capture the console's exact request/retry behavior and
record unknown response semantics as unknown.

## Preserve artifacts safely

Keep the raw image local/ignored. Commit only sanitized metadata, hashes,
structure and host fixtures. Treat the unencrypted DSP blob separately from
the signed/encrypted main images.
