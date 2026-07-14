# NSO GameCube Controller Reference Audit

Retrieval date: 2026-07-13
Clone root: `E:\nso-gc-refs\` (outside the PicoSwitch2 worktree)

## Repo Metadata

| Repo | URL | Default branch | Commit SHA | Local path |
| --- | --- | --- | --- | --- |
| ndeadly/switch2_controller_research | https://github.com/ndeadly/switch2_controller_research | `master` | `d1c5a7f7ba298f83017fae84952a4e6d2ef8fc92` (latest commit dated 2026-04-27) | `E:\nso-gc-refs\switch2_controller_research` |
| SoulCalDan/Low-Latency-Gamecube-Controller-to-USB-Adapter-for-Switch-1-2-and-PC | https://github.com/SoulCalDan/Low-Latency-Gamecube-Controller-to-USB-Adapter-for-Switch-1-2-and-PC | `main` | `dd8c9ecc690d5e6560ad9897584389e5fdbccc22` (latest commit dated 2026-06-14) | `E:\nso-gc-refs\Low-Latency-Gamecube-Controller-to-USB-Adapter` |

---

## ndeadly/switch2_controller_research findings

This is a pure documentation + raw-capture repo (BLE sniffs via nRF52840 sniffer and Ubertooth One, one USB pcap). All content below is prose/tables in markdown, i.e. **not independently re-derived by this audit** — it is ndeadly's own claimed transcription of captured traffic. Where a raw capture backs a specific claim, I've noted the capture file; most tables in this repo are stated to be derived from console/controller USB or BLE traffic sniffs the author performed (not directly re-verified by me in this pass — no capture parsing was done).

### USB identity (descriptors.md:704-722)

- VID `0x057E`, PID `0x2073`, bcdDevice `0x0101` (2.01), bDeviceClass `0xEF`/Sub `0x02`/Proto `0x01` (composite/IAD), bMaxPacketSize0 64.
- Manufacturer string: `Nintendo`; Product string: `Nintendo GameCube Controller` (descriptors.md:810-814); Serial string is literally `00` (descriptors.md:816-817).
- Safe-mode device: PID `0x2074`, bcdDevice `0x0113`, bDeviceClass `0xFF` (vendor), single-interface, bulk IN/OUT only (descriptors.md:1045-1063). Safe-mode entry combo: `Z+START+SYNC` (safe_mode.md:12).

### USB descriptor topology (descriptors.md:724-819)

Config descriptor: `wTotalLength=80`, 2 interfaces (IAD-grouped):
- **IF0** (HID, class 0x03): bcdHID 1.11, one HID report descriptor of length 97 bytes(for interface's own report, separate from the 214+ byte descriptor discussed below — actually the printed HID Report Descriptor block spans report IDs 5 and 0x0A totaling well past 97 bytes; note: descriptors.md's per-descriptor wDescriptorLength field for IF0 says `0x61,0x00` = 97, which appears to only cover report ID 5 fields, not the combined listing shown starting descriptors.md:819). EP 0x81 IN interrupt (64B, interval 4), EP 0x01 OUT interrupt (64B, interval 4).
- **IF1** (vendor class 0xFF): EP 0x02 OUT bulk (64B) and EP 0x82 IN bulk (64B) — this is the command/vendor channel (used for command `0x0N` protocol, flash reads, etc.), separate from the HID report interface.
- Safe mode config: `wTotalLength=32`, 1 interface, class 0xFF, EP 0x01 OUT bulk + EP 0x81 IN bulk only (descriptors.md:1065-1098).

### HID report descriptor (descriptors.md:819-858+)

Two report IDs defined for the GC controller HID collection:
- Report ID 5 (0x05): Usage Page 0xFF (vendor), 63-byte input array (`0x95,0x3F` = Report Count 63, `0x75,0x08` Report Size 8) — this is the raw vendor byte-array framing for input report 0x05 payload.
- Report ID 10 (0x0A) (descriptors.md:832-857): 2-byte vendor input prefix, then a **standard HID Button usage page** block — `Usage Minimum 0x01`/`Usage Maximum 0x15` (21 buttons, `0x95,0x15`=Report Count 21, 1 bit each) + 3-bit padding, followed by a **Generic Desktop Pointer collection** with X/Y/Rx/Rz axes, each `0x75,0x0C`=12-bit, Logical Max 4095 (`0x26,0xFF,0x0F`). This means report 0x0A is exposed to the OS as a *real* standard USB gamepad HID collection (21 buttons + 4x 12-bit axes) in addition to the raw vendor byte stream — i.e., the console (or any generic HID host) can read GC controller input via the standard gamepad HID usage page, not only via the proprietary byte layout in hid_reports.md.

### Input Report 0x05 (hid_reports.md:32-54) — common to all Switch 2 controllers

| Offset | Size | Field |
| --- | --- | --- |
| 0x0 | 4 | Counter (32-bit, increments per report) |
| 0x4 | 4 | Buttons bitfield |
| 0x8 | 2 | Unknown |
| 0xA | 3 | Left analog stick, packed 12-bit uncalibrated |
| 0xD | 3 | Right analog stick, packed 12-bit uncalibrated |
| 0x10 | 8 | Mouse data (Joycon only) |
| 0x18 | 1 | Always 0 |
| 0x19 | 6 | Magnetometer data |
| 0x1F | 2 | Battery voltage (mV) |
| 0x21 | 1 | Charging state/rate |
| 0x22 | 2 | Battery current(?) |
| 0x24 | 5 | Always 0x00 |
| 0x29 | 1 | Always 0x01 |
| 0x2A | 0x12 | Motion data (timestamp, temp, accel xyz, gyro xyz) |
| **0x3C** | **1** | **Left analog trigger — GC-only, uncalibrated** |
| **0x3D** | **1** | **Right analog trigger — GC-only, uncalibrated** |
| 0x3E | 1 | Reserved |

Button format for report 0x05 (hid_reports.md:58-63) — same bitfield for all controllers, byte 3 includes `Headset`, `GL`, `GR` bits (byte 0-2 layout common; GC-specific button *names* only appear in report 0x0A's own bitfield table below).

### Input Report 0x0A (hid_reports.md:195-219) — GC-specific, exact quote

```
| Offset | Size | Value                       | Comment                                                                               |
| 0x0    | 0x1  | Counter                     | 8-bit report counter. Increments by 1 each report                                     |
| 0x1    | 0x1  | Power Info                  | Bitfield. [0]=external power, [1]=charging, [2:5]=battery level (0-9), [6:7]=reserved |
| 0x2    | 0x3  | Buttons                     | Bitfield                                                                              |
| 0x5    | 0x3  | Left Analog Stick           | Uncalibrated. Packed 12-bit values                                                    |
| 0x8    | 0x3  | Right Analog Stick          | Uncalibrated. Packed 12-bit values                                                    |
| 0xB    | 0x1  | Unknown                     | 0x38 if feature bit 5 has been set, otherwise 0x30                                    |
| 0xC    | 0x1  | Left Analog Trigger         | Gamecube analog trigger (uncalibrated)                                                |
| 0xD    | 0x1  | Right Analog Trigger        | Gamecube analog trigger (uncalibrated)                                                |
| 0xE    | 0x1  | Motion Data Length          | Length of following motion data. Observed values {0, 30, 40}                          |
| 0xF    | 0x28 | Motion Data                 | Activated via feature bit 2. Unknown packed format                                    |
| 0x37   | 0x8  | Reserved                    | Unused                                                                                |
```

Button format table (hid_reports.md:213-219), exact quote:

```
| Byte | 0x80        | 0x40  | 0x20 | 0x10 | 0x08 | 0x04 | 0x02    | 0x01 |
| 0    | Right Stick | Plus  | R    | Z    | X    | Y    | A       | B    |
| 1    | Left Stick  | Minus | L    | ZL   | Up   | Left | Right   | Down |
| 2    | -           | -     | -    | C    | -    | -    | Capture | Home |
```

Notable: this confirms the GC NSO controller HAS a digital `ZL` bit (byte 1, 0x10) even though it has no physical ZL trigger — this is presumably always 0, or possibly mapped to some alternate input; also has `R`, `L`, `Z` digital bits (byte 0 bit 0x20=R, byte 1 bit 0x20=L, byte 0 bit 0x10=Z), a `C` bit (byte 2, 0x10 — this is "C-stick click" or a dedicated C/GameChat-style button, unclear), `Capture` and `Home`. Both L and R are digital-only in this bitfield; **actual analog force comes from the separate Left/Right Analog Trigger bytes at offsets 0xC/0xD** — i.e., GC controller exposes both digital press bits (L/R/Z) AND continuous analog trigger bytes simultaneously, not a detent-only mechanism. No raw capture confirms live values in this pass (see "Unverified" below) — this table is stated from the author's fuzzing/observation, not with an inline capture citation.

### Output Report 0x03 (hid_reports.md:248-256) — GC-specific rumble, exact quote

```
| Offset | Size | Value                | Comment                               |
| 0x0    | 0x1  | Report ID            | Always `00` for Bluetooth connections |
| 0x1    | 0x4  | Gamecube Rumble Data | Packed rumble data for Gamecube motor |
| 0x5    | 0x25 | Reserved             | Unused                                |
```

This is dramatically simpler than the JoyCon2/Pro2 HD-rumble formats (16 or 32 bytes of packed LRA data) — the GC controller has only a single ERM-style rumble motor, encoded in 4 bytes.

### GATT mapping for GC controller (hid_reports.md:17-26, bluetooth_interface.md:110-154)

- Input 0x05 → GATT handle `0x000A`, UUID `ab7de9be-89fe-49ad-828f-118f09df7fd2` (shared/common across all controllers)
- Input 0x0A → GATT handle `0x000E`, UUID `8261cba1-9435-420c-84d6-f0c75a2c8e4d` (GC-only)
- Output 0x03 → GATT handle `0x0012`, UUID `3f8fb670-ab25-45bf-b540-38c72834d064` (GC-only)
- Command+Vibration combined write → handle `0x0016`, UUID `af95885e-44b3-4a24-9cf0-483cc129469a` (GC-only)
- Command Response #2 notify → handle `0x001E`, UUID `46f6ad29-cdaf-4569-a2fe-339020b94604` (GC-only)

### GC-specific BLE init sequence (bluetooth_interface.md:481-511), exact quote of the handle/data table

```
| Handle   | Example Data                                                                                | Comment                                                       |
| 0x0005   | 01 00                                                                                       |                                                               |
| 0x001B   | 01 00                                                                                       | Enable notifications for command responses on handle 0x001A  |
| 0x001F   | 01 00                                                                                       | Enable notifications for command responses on handle 0x001E  |
| 0x0023   | 01 00                                                                                       | Enable notifications for command responses on handle 0x0022  |
| 0x0016   | 07 91 01 01 00 00 00 00                                                                     |                                                               |
| 0x0016   | 02 91 01 04 00 08 00 00 40 7e 00 00 00 30 01 00                                             | Read 0x40 bytes from 0x13000                                  |
| 0x0016   | 10 91 01 01 00 00 00 00                                                                     |                                                               |
| 0x0016   | 16 91 01 01 00 00 00 00                                                                     |                                                               |
| 0x0016   | 15 91 01 01 00 0e 00 00 00 02 81 eb 3a eb f1 48 80 eb 3a eb f1 48                           | Exchange Bluetooth addresses                                  |
| 0x0016   | 15 91 01 04 00 11 00 00 00 35 03 e9 29 82 87 71 24 be a8 0c 66 46 15 83 4b                  | Exchange Bluetooth LTK components                             |
| 0x0016   | 15 01 01 02 10 78 00 00 01 13 4c 97 f5 11 b9 b6 dd 4d 86 fd 40 f5 36 e9 ed                  | Exchange Bluetooth LTK confirm challenge/response             |
| 0x0016   | 15 91 01 03 00 01 00 00 00                                                                  | Finalise Bluetooth pairing                                    |
| 0x0016   | 0a 91 01 02 00 04 00 00 03 00 00 00                                                         | Play connection vibration sample                               |
| 0x0016   | 09 91 01 07 00 08 00 00 01 00 00 00 00 00 00 00                                             | Set player LEDs                                                |
| 0x0016   | 0c 91 01 02 00 04 00 00 27 00 00 00                                                         | Initialise feature flags (0x27)                                |
| 0x0016   | 02 91 01 04 00 08 00 00 40 7e 00 00 80 30 01 00                                             | Read 0x40 bytes from 0x13080                                   |
| 0x0016   | 02 91 01 04 00 08 00 00 40 7e 00 00 c0 30 01 00                                             | Read 0x40 bytes from 0x130C0                                   |
| 0x0016   | 02 91 01 04 00 08 00 00 40 7e 00 00 40 c0 1f 00                                             | Read 0x40 bytes from 0x1fC040                                  |
| 0x0016   | 02 91 01 04 00 08 00 00 10 7e 00 00 40 30 01 00                                             | Read 0x10 bytes from 0x13040                                   |
| 0x0016   | 02 91 01 04 00 08 00 00 18 7e 00 00 00 31 01 00                                             | Read 0x18 bytes from 0x13100                                   |
| 0x0016   | 11 91 01 03 00 00 00 00                                                                     |                                                                |
| 0x0016   | 02 91 01 04 00 08 00 00 02 7e 00 00 40 31 01 00                                             | Read 0x02 bytes from 0x13140                                   |
| 0x0016   | 02 91 01 04 00 08 00 00 20 7e 00 00 60 30 01 00                                             | Read 0x20 bytes from 0x13060                                   |
| 0x0016   | 0a 91 01 08 00 14 00 00 01 ff ff ff ff ff ff ff ff 35 00 46 00 00 00 00 00 00 00 00         | Send vibration data                                             |
| 0x0016   | 0c 91 01 04 00 04 00 00 27 00 00 00                                                         | Confirm feature flags (0x27)                                   |
| 0x0010   | 85 00                                                                                       |                                                                |
| 0x000F   | 01 00                                                                                       | Enable notifications for HID input reports on handle 0x000E    |
```

Note: this is the **BLE** init sequence (bluetooth_interface.md), not USB. GC feature flags value confirmed here is `0x27`, vs `0x37` for the Pro Controller 2 sequence documented immediately above it in the same file (bluetooth_interface.md:468) — i.e. GC lacks whatever feature bit(s) distinguish `0x37` from `0x27` (bit 4 = mouse data, per hid_reports.md:43 "Mouse Data ... Activated via feature bit 4" — GC has no mouse hardware, consistent with `0x37 & ~0x10 = 0x27`).

### USB init command sequence (commands.md) — the two subcommands the task specifically asked about

**Command 0x03 / Subcommand 0x03 - "Enable USB HID Reports"** (commands.md:231, 259-... not fully quoted above but present in table): Request `03 91 00 03 00 04 00 00 01 00 00 00` → Response `03 01 00 03 00 F8 00 00 01 00 00 00`. This is distinct from and comes before/alongside subcommand 0x0D.

**Command 0x03 / Subcommand 0x0D - "Initialise USB"** (commands.md:365-379), exact quote:
> "Initialise the controller for USB. Required before the controller will send input reports over USB. Host address doesn't need to be valid."
- Request: offset 0x0 (1B) Unknown, 0x1 (1B) Unknown, 0x2 (6B) Host Bluetooth address (byte-reversed).
- Example: `03 91 00 0d 00 08 00 00` `01 00 31 7e c6 eb f1 48` → Response `03 01 00 0d 00 f8 00 00` `01 00 00 00`

**Command 0x03 / Subcommand 0x0A - "Select Input Report"** (commands.md:334-347), exact quote:
> "Selects one of the input report formats found in the HID report descriptor. Invalid report IDs are ignored. Defaults to 0x07|0x08|0x09|0x0A (depending on controller type) on power-up."
- Request: offset 0x0 (1B) Report ID (0x05 or the controller-specific ID), 0x1 (3B) Unknown/unused.
- Example: `03 91 00 0a 00 04 00 00` `09 00 00 00` → Response `03 01 00 0a 00 f8 00 00` (example shown uses `09` = Pro Controller 2's ID, not GC's `0x0A`, but the mechanism is identical per controller type)

So for GC specifically, PicoSwitch2 would send `03 91 00 0a 00 04 00 00 0A 00 00 00` to select input report 0x0A, matching the general mechanism.

### Firmware Info / controller-type field (commands.md:1072-1097), exact quote

> Subcommand 0x01 - Get Firmware Version Info, response offset 0x3 (1 byte): "Controller firmware type — `00` = JoyCon (L), `01` = JoyCon (R), `02` = Pro Controller, `03` = Gamecube"

This is a strong signal for how PicoSwitch2 should self-identify: firmware-type byte `0x03` in the `0x10`/`0x01` response.

### SPI/memory map (memory_layout.md) — generic across all Switch 2 controllers, not GC-specific

The 2MB SPI layout (firmware banks, factory data at 0x13000-0x14FFF with serial/VID/PID/colors, calibration at 0x1FC000, BT pairing info at 0x1FA000) applies identically regardless of controller type; no GC-specific offsets are documented beyond the generic "not all fields initialised for all controller types" caveat (memory_layout.md:60). No GC-specific factory-data dump/example is shown in this file — only a Pro-Controller-flavored example (serial `HEJ7100112...`).

### Safe mode (safe_mode.md) — GC-specific line only

> "Gamecube NSO Controller | 0x2074 | `Z`+`START`+`SYNC`" (safe_mode.md:12)

Safe-mode command table (safe_mode.md:14-28) is generic/shared across controller types; no GC-specific safe-mode command behavior is documented (this is flagged in the source itself as "seemingly valid commands observed when fuzzing," i.e. hypothesis-grade, not capture-confirmed).

### Raw capture files found (ndeadly repo)

None of the checked-in raw captures are GC-controller-specific — they're all JoyCon2/ProCon2 BLE sniffs, **except**:

| File | Approx size | Notes |
| --- | --- | --- |
| `captures/usb/rumble-procon-gccon.pcapng.gz` | 16,578,103 bytes (~15.8 MiB) compressed | **Only capture in the repo that plausibly includes GC controller USB traffic** (filename implies Pro Controller + GC controller rumble USB traffic). Not decompressed/parsed in this pass — flagged for follow-up byte-level analysis. |
| `captures/nrf52840/btle_procon2_*.pcapng` (10 files, 24 KB – 1.3 MB each) | — | Pro Controller 2 only, not GC |
| `captures/nrf52840/btle_joycon2_*.pcapng` (10 files, 112 KB – 1.3 MB each) | — | JoyCon 2 only, not GC |
| `captures/ubertooth-one/btle_*procon2*.pcapng` (4 files, 4 KB – 48 KB) | — | Pro Controller 2 only, not GC |
| `captures/nrf52840/joycon2.ltk`, `procon2.ltk` | small | LTK key files for decrypting the above BLE captures, not GC |

**No dedicated GC-controller BLE or USB capture file exists in this repo.** All GC-specific claims in hid_reports.md / commands.md / bluetooth_interface.md / safe_mode.md / descriptors.md are prose transcriptions by the author, not something this audit could cross-check against a raw capture in-repo. The `rumble-procon-gccon.pcapng.gz` file is the closest thing to primary GC USB evidence and should be prioritized for decompression + Wireshark/tshark analysis in a follow-up pass (there's also a companion wireshark dissector referenced at tools/README.md:8, https://github.com/german77/JoyconDriver/tree/master/switch2, not cloned in this pass).

Also present: `tools/switch2-spi-dump.nro` (homebrew SPI dumper for the original Switch), useful for producing fresh SPI dumps from a GC controller if hardware is available.

---

## SoulCalDan/Low-Latency-Gamecube-Controller-to-USB-Adapter findings

**Critical framing note up front:** this repo does **NOT** implement the native NSO GameCube Controller BLE/USB protocol (VID:PID `057E:2073`, report IDs 0x05/0x0A/0x03) that ndeadly documents. Instead, `GC_Adapter_Files/SwitchWiiU_Adapter.v` hard-codes USB VID:PID **`057E:0337`** — the **official Nintendo Wii U GameCube Controller Adapter** identity (the classic 4-port passthrough adapter used by Smash Bros. / Dolphin, a totally different, much older USB HID class-compliant protocol). This is implementation evidence, not raw-capture-backed, but it is unambiguous from the source: the device descriptor bytes literally spell out `7e 05 37 03` (VID 0x057E, PID 0x0337) in `SwitchWiiU_Adapter.v:129`. There is no reference anywhere in the `.v` sources to PID `0x2073`, `0x2069`, `0x2072`, or any of the BLE GATT UUIDs/report IDs ndeadly documents (verified via grep across all `.v` files — zero matches). The Switch/Switch 2 OS apparently recognizes the WiiU adapter's real GameCube controller input and remaps it into the "GameCube Controller — Nintendo Switch Online" software layer on the console side; this adapter never speaks the native NSO-controller wire protocol itself.

Given that framing, most of what this repo can usefully evidence is: (a) the real physical GameCube controller's own wire protocol (not Switch2-specific at all — this is the 1998 GC serial/SI protocol), and (b) how the WiiU-adapter-compatible USB HID report is built, not how the native `0x2073` NSO controller reports work. I've extracted what's there per the requested topics, all tagged "implementation evidence" since none of it is cross-checked against a raw capture in this repo (no `.pcap`/`.pcapng` files exist in this repo at all).

### USB identity and strings (SwitchWiiU_Adapter.v:128-141) — implementation evidence

```verilog
.DESCRIPTOR_DEVICE ( {
     144'h12_01_10_01_00_00_00_08_7e_05_37_03_00_01_01_02_03_01  // WiiU Adapter default
} ),
```
Decoded: bLength 0x12, bDescriptorType 0x01, bcdUSB 0x0110, bDeviceClass/Sub/Proto = 0/0/0, bMaxPacketSize0 = 8 (not 64 — notably tiny for EP0), idVendor `0x057E`, idProduct **`0x0337`**, bcdDevice 0x0301, iManufacturer 1, iProduct 2, iSerialNumber 3, bNumConfigurations 1.

String descriptors (SwitchWiiU_Adapter.v:132-141):
- iManufacturer (STR1): `"NintendoSwitchMode"` (a made-up label chosen by the author, not Nintendo's real string)
- iProduct (STR2): `"SoulCal Adapter"`
- iSerialNumber (STR3): `"Isaac Make Us Whole"` (author's own joke/signature string, not a real serial)

This confirms: **the strings are custom author-chosen text, not a reproduction of genuine Nintendo string descriptors** — of no evidentiary value for what real Nintendo hardware reports.

### USB descriptor topology (SwitchWiiU_Adapter.v:142-146) — implementation evidence

```verilog
.DESCRIPTOR_CONFIG ( {
        72'h09_02_29_00_01_01_00_E0_FA ,  72'h09_04_00_00_02_03_00_00_00 ,
        72'h09_21_10_01_00_01_22_D6_00 ,  56'h07_05_81_03_25_00_01 ,
        56'h07_05_02_03_05_00_01 , 3768'h0
} ),
```
Decoded: Config descriptor (wTotalLength 0x0029=41, 1 interface, bmAttributes 0xE0 self-powered+remote-wakeup, bMaxPower 0xFA=500mA) → Interface 0 (2 endpoints, class 3 = HID) → HID descriptor (bcdHID 0x0110, 1 report descriptor, length 0x00D6 = 214 bytes) → EP 0x81 IN interrupt, wMaxPacketSize **37** bytes, bInterval 1 → EP 0x02 OUT interrupt, wMaxPacketSize **5** bytes, bInterval 1. This 37-byte-IN/5-byte-OUT/214-byte-HID-report-descriptor triple is the well-known, publicly documented WiiU GC Adapter signature (used by Dolphin's `GCAdapter.cpp` community docs) — consistent with the PID `0x0337` identity, further confirming this is not the NSO-controller descriptor shape (which per ndeadly is 64B/64B interrupt EPs plus a separate 64B/64B bulk vendor interface).

The `DESCRIPTOR_HID` constant (SwitchWiiU_Adapter.v:109-114) is a 214-byte HID report descriptor built from raw hex, structured as 5 sub-collections keyed by report IDs 0x11, 0x21, 0x12, 0x22, 0x13, 0x23, 0x14, 0x24, 0x15, 0x25 — this is a different, non-standard multi-report-ID vendor scheme unrelated to anything ndeadly documents for report IDs 0x05/0x0A. (Also worth noting: `ep00_setup_cmd[15:0] == 16'h0681` at line 118 is the EP0 GET_DESCRIPTOR(HID Report) match condition — standard control-transfer descriptor fetch, no vendor-specific control requests are implemented in this file.)

### Rumble/output report handling (SwitchWiiU_Adapter.v:32-67) — implementation evidence

```verilog
if ( out_host_en && (out_host_data[7:0] == 8'h11) ) begin
    RUMBLE1 <= out_host_data[8];
    RUMBLE2 <= out_host_data[16];
    RUMBLE3 <= out_host_data[24];
    RUMBLE4 <= out_host_data[32];
end
```
This reads 5 bytes of host OUT data (matching the 5-byte OUT endpoint above), checks that byte 0 equals `0x11`, and extracts one rumble-enable bit per port from bytes 1-4 (bit 0 of each byte). This is the same simplistic on/off rumble scheme long documented for the real WiiU GC adapter (report ID `0x11`, one enable byte per port) — **not** a Gamecube-motor-intensity-packed 4-byte scheme like ndeadly's Output Report 0x03. This is further confirmation the two repos describe genuinely different USB protocols, not two views of the same one.

### Input report field layout (TOP_Adapter.v:130-134, comments at TOP_Adapter.v:144-156) — implementation evidence

```verilog
key_value <= {8'h21 ,
connect1 , GCBD1[03:00], GCBD1[11:08], 4'h0, GCBD1[06:04], GCBD1[12], GCLA1[15:00], GCRA1[15:00], GCTA1[15:00], // Controller 1
connect2 , GCBD2[03:00], GCBD2[11:08], 4'h0, GCBD2[06:04], GCBD2[12], GCLA2[15:00], GCRA2[15:00], GCTA2[15:00], // Controller 2
...
```
Per-port packed layout: 1 byte "connect" status (`0x14`=connected+rumble-capable, `0x04`=disconnected, per TOP_Adapter.v:110-127), then reconstructed 16-bit button word (with a nibble gap), then a "type" bit, then 2 bytes left stick (X,Y), 2 bytes right/C-stick (X,Y), 2 bytes trigger analog (L,R). This 9-byte-per-port structure × 4 ports = 36 bytes + 1 leading report-ID byte `0x21` = 37 bytes total, matching the wMaxPacketSize=37 EP0x81 declared above.

Button bit mapping, exact quote (TOP_Adapter.v:144-156):
```
//A  01 00
//B  02 00
//X  04 00
//Y  08 00
//Z  00 02
//St 00 01
//DU 80 00
//DD 40 00
//DR 20 00
//DL 10 00
//L  00 08
//R  00 04
```
This is the classic real-GameCube-controller button bit layout (as transmitted natively over the GC controller's own serial/SI wire protocol) — 16 bits total, byte0={A,B,X,Y,Start + 3 unused high bits}, byte1={D-pad, L, R, Z, +1 unused}. No "expanded" Switch 2 buttons (ZL, C/GameChat, Home, Capture) exist anywhere in this bit layout or anywhere else in the repo — **this adapter cannot and does not implement any of the Switch-2-exclusive GC-controller buttons that ndeadly's report 0x0A documents** (Capture, Home, C, ZL). It only reproduces the original 1998 GameCube controller's own button set, because it is quite literally reading a real GameCube controller's SI-bus output (`GC_Read.v`) and repackaging it as a WiiU-adapter-format USB report.

### Analog trigger scaling / digital detent (GC_Read.v:36-46, GC_Read.v:1-4) — implementation evidence

```verilog
if ( ( {1'b0,GCdata[16:09]} - $signed(offsetLT) ) > 8'b11111111 ) begin
    LAT[7:0] <= 8'b0;
end else begin
    LAT[7:0] <= GCdata[16:09] - $signed(offsetLT);
end
```
Left/right analog trigger values (`LAT`/`RAT`, 8 bits each) come straight from the real GC controller's own SI-protocol trigger bytes, offset-corrected by a per-boot calibration baseline captured at connect time (`offsetLT`/`offsetRT`, GC_Read.v:91-92) — i.e., a runtime auto-zero/calibration, not a hard-coded scale curve. Digital L/R "click" bits are separately taken straight from `GCdata[06:04]` (part of the raw 16-bit button word, comment-mapped to L/R/Z above) — these come from the **real controller's own mechanical/electrical detent switches** (every original Nintendo GC controller has a physical microswitch under each trigger that closes at nearly-full press, independent of the analog potentiometer value), not something synthesized by the adapter. So: "digital trigger detent behavior" in this repo is pure passthrough of stock 1998 GC controller hardware behavior, giving no new information about how the Switch2 NSO controller's own analog trigger byte (hid_reports.md offset 0xC/0xD, "uncalibrated") behaves or scales.

Large per-axis lookup-table remapping (GC_Read.v:17, the giant `case` statements for x/y/cx/cy) rescales each calibrated stick axis from a ~0-255 raw range into a clamped ~33-222 output range — described in the README as fixing "the joystick calibration issue present in Nintendo's Gamecube emulator" (README.md:1) — this is the author's own corrective curve for known analog-stick centering problems in Nintendo's software GC emulation, not a documented Nintendo-original scaling table.

### NSO calibration-fix mode toggle (GC_Read.v:5, 21, 86) — implementation evidence

`button2` input, when held during controller connect/reset, sets a register `NSOCal`; when `NSOCal==0` ("held to enter NSO MODE," GC_Read.v:21) the adapter substitutes a table-remapped analog value (`x`,`y`,`cx`,`cy`) instead of the raw offset-corrected value. This is a physical-button-triggered firmware toggle on the adapter itself, unrelated to any Switch2 protocol behavior — it only affects what values get put into the existing WiiU-adapter-format report.

### No safe-mode, no BLE, no vendor command channel — implementation evidence (absence)

- No Bluetooth/BLE code exists anywhere in this repo (it's a wired FPGA/USB-only device) — cannot corroborate or contradict any of ndeadly's BLE findings.
- No vendor vendor-command endpoint parsing (no analog to ndeadly's command 0x01-0x18 framework) — the only "commands" this device recognizes are the single hard-coded rumble byte pattern above.
- No factory-data / SPI-flash emulation of any kind (no analog to ndeadly's memory_layout.md).
- No distinction in source between "Switch 1" vs "Switch 2" host behavior at the protocol level — the README claims Switch 2 gets 1000Hz vs Switch 1's 125Hz-limited polling (README.md:5), but this is a **host-side polling-rate behavior claim**, not something implemented differently in the checked-in `.v` files (all four `GC_PollGen` instances and the single `SwitchWiiU_Adapter` instance in `TOP_Adapter.v` are unconditional — there is no runtime host-version detection code found). This claim should be treated as an unverified README assertion, not confirmed by source inspection.
- The README also claims the adapter can "Act as a wired Gamecube NSO controller, or a Switch 2 Pro Controller" (README.md:6) — but only one descriptor set (`SwitchWiiU_Adapter`, PID `0x0337`) is instantiated in `TOP_Adapter.v`; no second mode/descriptor table for a "Switch 2 Pro Controller" (PID `0x2069` or similar) or a native GC-NSO descriptor (PID `0x2073`) exists anywhere in the `.v` sources. Either this capability lives only in the precompiled `GC_Adapter_Loader/firmware/GC_Adapter.fs` bitstream (not verifiable from source — `.fs` is a compiled Gowin bitstream, not something this audit decoded), or the README is describing how the *console* reinterprets WiiU-adapter input inside its GameCube-NSO software layer rather than a literal second USB identity. This is a documentation/implementation gap worth flagging, not resolving speculatively.

---

## Disagreements between the two sources

1. **Fundamental protocol identity mismatch.** ndeadly documents the native NSO GC Controller as VID:PID `057E:2073` with BLE GATT services, report IDs 0x05/0x0A/0x03, and a proprietary command-channel protocol (commands.md). SoulCalDan's adapter implements VID:PID `057E:0337` (WiiU GC Adapter) with a completely different, older, simpler USB HID scheme (37B IN / 5B OUT, report ID 0x11 for rumble, no command channel, no BLE). **These are not two observations of the same protocol** — they are two different Nintendo protocols that both happen to carry GameCube-controller-shaped input to a Switch console. Any implementation work in PicoSwitch2 aiming to emulate the *native* NSO GC controller (which is presumably the goal, given the project's `docs/switch2/*` structure) should treat ndeadly's repo as the primary/only source, and SoulCalDan's repo as at most a secondary reference for legitimate physical-GC-controller trigger/stick electrical behavior, not for the NSO wire protocol.

2. **Rumble encoding.** ndeadly: GC output report 0x03 carries a 4-byte "Gamecube Rumble Data" field (packed, format not decoded) at offset 0x1 of a 0x03-report. SoulCalDan: rumble is a single on/off bit per port derived from byte 0 of a 5-byte WiiU-adapter OUT report (report ID 0x11). These cannot be reconciled — they are different reports on different protocols (per point 1). Neither source fully decodes what the 4 "Gamecube Rumble Data" bytes in ndeadly's 0x03 report actually mean bit-for-bit; this remains an open unknown for PicoSwitch2 to determine experimentally (motor on/off vs. amplitude/frequency encoding is not established by either repo).

3. **Button set.** ndeadly's report-0x0A button table includes Switch-2-exclusive buttons not present on a real 1998 GC controller: `ZL` (byte1, 0x10), `C` (byte2, 0x10), `Capture` (byte2, 0x02), `Home` (byte2, 0x01), plus `Plus`/`Minus` in place of the original `Start`. SoulCalDan's device, because it forwards a genuine physical GC controller's own button word, can only ever produce the original GC button set (`A B X Y Z Start D-pad L R`) — it structurally cannot produce ZL/C/Capture/Home/Plus/Minus since no such physical buttons or wire-protocol bits exist on a real GameCube controller. This isn't a contradiction so much as confirmation that the NSO GC controller's expanded button set is a genuinely new piece of the controller's own PCB/firmware (there is no legacy GC-controller wire-protocol bit for any of them), which matters for PicoSwitch2: those buttons must be synthesized from a Pico-side abstraction (e.g. spare GPIO or held-combo), not read from any pre-existing GC-controller physical signal.

4. **500mA vs 500mA bMaxPower — no disagreement**, both descriptors independently specify `0xFA` (500mA) for bMaxPower, for what it's worth as a minor cross-check that both are describing genuine Nintendo-class self-powered/high-current USB devices.

---

## Raw capture files worth byte-level analysis later

| Repo | File | Size | Priority notes |
| --- | --- | --- | --- |
| ndeadly/switch2_controller_research | `captures/usb/rumble-procon-gccon.pcapng.gz` | ~15.8 MiB compressed | **Highest priority** — the only capture in either repo that plausibly contains real GC-controller (0x2073) USB traffic, specifically framed around rumble. Needs gunzip + tshark/Wireshark pass to extract: exact 0x03 output-report rumble byte encoding, exact 0x0A input-report byte stream in practice (to cross-check hid_reports.md's table), and USB enumeration/init command sequence over USB rather than BLE (commands.md's command 0x03/0x0D-0x0A USB sequence has no capture citation in the docs — this pcap is the way to get one). |
| ndeadly/switch2_controller_research | `captures/nrf52840/btle_procon2_motion_0x000A.pcapng` / `btle_procon2_motion_0x000E.pcapng` | 928 KB / 1.2 MB | Not GC, but useful analog for decoding the shared Motion Data (0x2A/0x12 in report 0x05) format, which GC also carries. |
| ndeadly/switch2_controller_research | remaining 18 `.pcapng` BLE captures (JoyCon2/ProCon2 pairing, reconnect, advertise, wake, OTA) | 4 KB – 1.3 MB each | Not GC-specific; useful for cross-controller-type protocol commonality (pairing/command-header framing is shared per bluetooth_interface.md) but lower priority for GC-specific work. |
| SoulCalDan repo | none | — | Zero `.pcap`/`.pcapng` files exist in this repo. All claims are implementation-only. |

---

## Evidence-grading summary (per project standard)

- **Independently confirmed protocol truth**: nothing in this pass — no capture bytes were parsed in this session; all ndeadly claims are "documented by the author from captures I have not personally re-verified," and all SoulCalDan claims are "implementation evidence" (source code that works, but reverse-engineered/adapted for a *different* Nintendo protocol than the NSO GC controller).
- **Strong evidence**: ndeadly's report 0x0A/0x03 layouts and the command 0x03/0x0A/0x0D init sequence — internally consistent across multiple files (hid_reports.md, commands.md, bluetooth_interface.md all cross-reference each other correctly), and the author explicitly distinguishes capture-derived vs. fuzzed-and-unconfirmed content within his own docs (e.g. safe_mode.md explicitly hedges with "seemingly valid").
- **Hypothesis / unverified**: SoulCalDan's README claims about a second "Switch 2 Pro Controller" mode and Switch-1-vs-2 polling-rate differentiation — not found in the `.v` sources, possibly only in the compiled `.fs` bitstream.
- **Unknown / open**: GC rumble byte-for-byte encoding (both repos agree it exists in some form but neither decodes it); exact contents of GC report 0x0A byte 0xB ("Unknown, 0x38 if feature bit 5 set else 0x30" — meaning of feature bit 5 for GC specifically undetermined); whether GC controller's SPI factory-data section at 0x13000 differs from the Pro-Controller example shown in memory_layout.md.
