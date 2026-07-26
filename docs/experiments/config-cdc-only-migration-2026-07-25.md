# Config Mode CDC-Only Migration

Date: 2026-07-25
Status: 🟡 automated validation complete; hardware validation pending

## Question

Can the configuration personality stop presenting a read-only FAT/MSC drive and use only its
existing CDC serial protocol, without changing the BOOTSEL personality lifecycle or configuration
commands?

## Design

- Keep config USB identity `CAFE:4012`, serial string, and CDC endpoints unchanged.
- Reduce the configuration descriptor from CDC+MSC to the two-interface CDC function.
- Disable TinyUSB MSC with `CFG_TUD_MSC=0`.
- Remove `src/msc.c`, generated `src/web_disk.h`, and `tools/make_web_disk.py`.
- Serve the production `web/index.html` locally with `tools/run_config_portal.ps1`.
- Keep browser-side library data in IndexedDB at the stable localhost origin.

No controller personality, Bluetooth path, config command, flash layout, or BOOTSEL state-machine
logic was changed.

## Automated method

1. Build the standard Pico 2 W and Pico W release configurations.
2. Build the legacy `NS2_PRO=OFF` Pico W configuration because it shares the config descriptor.
3. Run all compiled host-test executables.
4. Parse both portal JavaScript blocks and verify their DOM references.
5. Serve both pages over localhost and require successful HTTP responses.
6. Inspect the linked firmware symbol tables for any remaining `tud_msc`, `msc_`, or `web_disk`
   symbol.
7. Compare the resulting binary and ELF section sizes with the immediately preceding build.

## Results

| Measurement | Before | After | Reduction |
|---|---:|---:|---:|
| Pico 2 W `.bin` | 991,080 bytes | 890,976 bytes | 100,104 bytes |
| Pico W `.bin` | 860,372 bytes | 760,212 bytes | 100,160 bytes |
| Pico 2 W `.bss` | 176,116 bytes | 175,540 bytes | 576 bytes |
| Pico W `.bss` | 106,912 bytes | 106,336 bytes | 576 bytes |

The Pico W binary reduction differs by 56 bytes from Pico 2 W because their linked layouts are not
identical; that small delta was not attributed further. The current source contains no MSC
descriptor, callbacks, embedded disk, or web-disk generator. The configuration descriptor has a
compile-time length assertion.

## Conclusion

Source and automated evidence support the migration. Presentation is now decoupled from firmware:
portal changes consume only host storage, while the Pico exposes the same CDC command transport.

Hardware must still confirm:

1. the fifth BOOTSEL personality enumerates as **PicoSwitch Config** without a storage drive;
2. the local portal connects, reads state, changes a setting, saves, and reads it back;
3. a five-second BOOTSEL hold exits config mode and returns to Pro Controller 2;
4. LED/BOOTSEL behavior and a normal controller personality remain unaffected.
