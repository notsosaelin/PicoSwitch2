# Genuine Pro Controller 2 Headset Audio

> Status (2026-07-22): hardware validated. Switch 2 USB speaker PCM reaches a
> genuine Pro Controller 2 headphone jack without regressions to input, gyro,
> rumble, LED behavior, or BOOTSEL handling. Activation remains UART-gated
> while the implementation is evaluated for promotion to a standard feature.

## Confirmed BLE transport

Live GATT discovery against PID `0x2069` and decrypted HCI captures agree on
these firmware-2.x attribute handles:

| Handle | Direction | Purpose |
| --- | --- | --- |
| `0x002C` | host to controller | Speaker Opus packet chunks |
| `0x002E` | controller to host | Extended input, headset/mic audio, and motion notifications |
| `0x002F` | host to controller | CCC for `0x002E` notifications |
| `0x0032` | host to controller | Audio/control setup commands |

Each 20 ms transport interval carries one 240-byte, 48 kHz stereo Opus/CELT
packet at 96 kbit/s. GATT divides that codec packet into two ordered writes:

1. `00 04 78` plus bytes `0..119`; byte zero is the Opus TOC (`FC`).
2. `00 02 78` plus bytes `120..239`; this is continuation data and has no TOC.

The fixed idle packet is `FC FF FE` followed by 237 zero bytes, split at the
same 120-byte boundary. The setup command's trailing `F0 00` is therefore the
combined `0x00F0`/240-byte codec-frame size, not an independent stream size.

This framing was proven against all 1,846 packet pairs in the genuine capture:

- `0x04 + 0x02` decoded as 960 samples per channel with zero failures and zero
  duration mismatches.
- Reversing the order caused 98 decode failures and 1,710 duration mismatches.
- Decoding only the first 120 bytes produced a lower-fidelity version of the
  same events, consistent with Opus packet truncation.
- The production-shaped direct-CELT encoder produces a valid 240-byte public
  Opus packet and converges to the captured fixed idle packet after one silent
  frame.

The earlier interpretation of `0x04` and `0x02` as separate speaker and haptic
codecs was incorrect. Encoding a complete 120-byte Opus packet into `0x04` and
then appending 120 zero bytes in `0x02` made the controller decode the pair as a
malformed 240-byte range-coded packet. That caused the repeatable, recognizable
but severely distorted playback heard during development.

## Live encoder and scheduling

The Pico 2 W path keeps the memory-safe direct-CELT encoder that previously
preserved LED and BOOTSEL behavior. It emits one `FC` TOC byte plus a fixed
239-byte CELT payload, then the BT transport splits the completed packet into
two 120-byte writes. The encoder runs at 96 kbit/s, CBR, 48 kHz stereo, with a
20 ms/960-sample frame.

Transport primes the controller with eight exact idle packets. If PCM is late,
the transport sends another fixed idle packet and advances the stateful encoder
through matching silence before live audio resumes. The first half is sent
approximately 5 ms before the second while complete frames remain paced at
20 ms. A frame is removed from the queue only after both GATT writes succeed.

## Extended input report (`0x002E`)

The enabled report is 112 bytes:

- `0x00`: counter
- `0x01`: power
- `0x02..0x04`: buttons
- `0x05..0x0A`: sticks
- `0x0B`: flags
- `0x0C`: NFC
- `0x0D`: physical jack state
- `0x0E`: microphone/audio length (`0` or `0x32`)
- `0x0F..0x40`: 50-byte microphone/audio payload
- `0x41`: motion length
- `0x42..0x69`: motion payload (up to 40 bytes)
- `0x6A..0x6F`: reserved

Existing research labels `0x05/0x0D` as headphones and `0x07/0x0F` as a
headset with a microphone. A headphone-only TRS plug nevertheless produced the
`0x07/0x0F` pair and a full 50-byte high-entropy field. Those values are
therefore preserved byte-for-byte but are not treated as proof that the
accessory contains a microphone.

`switch2_pro2_audio_compact_input()` removes the 50-byte audio field and moves
motion back to the offsets used by the ordinary `0x000E` pipeline. Since
`0x000E` normally continues alongside `0x002E`, compacted input is used only
after the ordinary report has been absent for 50 ms. This avoids frozen or
duplicated controls and motion.

## UART controls

```powershell
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'pro2audio on'
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'pro2audio live on'
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'pro2audio status'
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'pro2audio live off'
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'pro2audio off'
```

`pro2audio replay` remains a historical transport diagnostic. It combines the
captured second halves with a fixed idle first half and is intentionally not a
valid reconstruction of the original 240-byte packets. Use `live on` for real
console PCM playback.

## Validation

Hardware validation passed with a genuine Pro Controller 2 and wired
headphones:

- clean, continuous console audio;
- normal input and native gyro;
- normal rumble with and without headphones;
- headset insertion and removal;
- LED and BOOTSEL behavior; and
- no observed regression in the wider controller path.

Both Pico W and Pico 2 W firmware targets build. The complete host regression
set of 36 executables passes, and the dedicated host probe verifies the 240-byte encode,
120+120-byte split/reassembly, 960-sample decode, and fixed-idle convergence.
