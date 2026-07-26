# NFC feasibility and resource audit — 2026-07-25

## Question

Does the current repository contain enough protocol, transport, memory, flash, and CPU capacity to
support:

1. virtual read/write amiibo images for controllers without NFC; and
2. native use of NFC readers in connected Nintendo controllers?

## Method

- Read the current PicoSwitch2 USB vendor path, Switch 2 BLE command path, Switch 1 Bluetooth
  driver, config-mode CDC/web flow, flash layout, and current Pico 2 W linker output.
- Re-audited these external source revisions:
  - [`Dycool/NS-PC-Control`](https://github.com/Dycool/NS-PC-Control/tree/5f0a815615b8b7f11ff8c1d1f994ba51dc7da0c7)
    `5f0a815615b8b7f11ff8c1d1f994ba51dc7da0c7`;
  - [`ndeadly/switch2_controller_research`](https://github.com/ndeadly/switch2_controller_research/tree/d1c5a7f7ba298f83017fae84952a4e6d2ef8fc92)
    `d1c5a7f7ba298f83017fae84952a4e6d2ef8fc92`;
  - [`CTCaer/jc_toolkit`](https://github.com/CTCaer/jc_toolkit/tree/9d0cc455aebd07930b557840b47cb26df9eb4a1f)
    `9d0cc455aebd07930b557840b47cb26df9eb4a1f`;
  - [`mart1nro/joycontrol`](https://github.com/mart1nro/joycontrol/tree/18a09da1a04306534ff9e1df8a1a69c0192a3244)
    history through `18a09da1a04306534ff9e1df8a1a69c0192a3244`;
  - [`Poohl/joycontrol`](https://github.com/Poohl/joycontrol/tree/3e80cb315dab8c2e2daedc80d447836b7d4d85f7)
    `3e80cb315dab8c2e2daedc80d447836b7d4d85f7`.
- Measured `build/pico2_w/PicoSwitchWGA-pico2_w.elf` with
  `arm-none-eabi-size -A` and inspected the configured flash offsets.
- No firmware was changed and no hardware NFC transaction was performed.

## Results

### Virtual tag

**Feasible.** The available structured protocol is sufficient for a gated first implementation of
540/572-byte upload, scan, read, staged write, RAM write-back, and download. The remaining
uncertainty is hardware validation, not resource capacity.

### Native Switch 2 reader

**Feasible architecture, insufficient primary transaction evidence.** The existing USB/BLE bridge
removes the old architectural blocker. The missing work is an asynchronous USB-full/BLE-chunk
adapter, subscription to the secondary response handle, NFC input-state preservation, and a
complete genuine read/write capture.

### Native Switch 1 reader

**Read is source-supported; full read/write translation is not yet supported by primary evidence.**
The physical MCU read sequence is public. The present PicoSwitch2 driver does not select or parse
report `0x31`, and the audit did not locate an equally strong physical tag-write implementation.

## Resource measurements

Current board builds:

| Measurement | Pico 2 W | Pico W |
|---|---:|---:|
| firmware `.bin` | 944,936 | 814,364 |
| flash capacity | 4,194,304 | 2,097,152 |
| `.data` | 128,284 | 7,332 |
| `.bss` | 174,236 | 105,032 |
| heap reservation | 2,048 | 2,048 |
| fixed-section gap before scratch X | 219,444 | 147,536 |

The compact NFC RAM design is approximately 1.8 KiB including the raw image, signature, write
staging, bit coverage, metadata, and one full USB response buffer. It is less than two percent of
either measured main-SRAM gap (214.3 KiB Pico 2 W; 144.1 KiB Pico W).

Flash sectors at the top of flash:

| Sector | Current owner |
|---|---|
| `-4` | `pico_config_t` settings |
| `-3` | unused in current source |
| `-2`, `-1` | BTstack TLV/bond banks |

One 4 KiB journal sector at `-3` is enough for one active 540/572-byte tag plus metadata and several
append records. It must not overlap the firmware image and must have compile-time overlap
assertions.

### CPU and scheduling

No tag cryptography is required. Idle overhead can be one report-state byte assignment and a
branch—no timer, polling, or flash access. Active work consists of bounded memory copies, small
validations, and USB/BLE transaction state. Nothing in the protocol justifies raising the system
clock beyond the validated 300 MHz.

The risk is not average CPU use; it is blocking:

- a one-shot 630-byte `tud_vendor_write()` exceeds the current 128-byte TX FIFO;
- flash erase/program parks the other core and can stall USB/audio/input;
- waiting synchronously for BLE NFC chunks would stall the USB task.

The implementation must therefore queue USB response fragments, await BLE chunks asynchronously,
and defer persistence to config mode or an explicit proven-safe save.

## Source-quality notes

- The current NS-PC-Control virtual codec is useful and internally consistent, but cites captures
  not shipped in its public repository.
- JoyControl's old NFC implementation was removed after a source-provenance/license dispute. Its
  code is not an appropriate source to copy into this project.
- `jc_toolkit` is MIT-licensed and directly drives a physical Nintendo NFC reader, but implements
  tag reading rather than the full physical write requirement.
- Neither Switch 1 source proves that its MCU framing transfers to Switch 2. A translation layer is
  required.

## Conclusion

Proceed with the virtual-tag codec and non-blocking USB transport first. Do not begin native-reader
runtime support until the UART/BLE tracer can capture full `0x001E` payloads and a genuine
read/write transaction is available. This ordering adds no idle CPU load, fits comfortably in RAM
and flash, and contains the highest-risk transport changes behind host tests before touching
hardware behavior.
