# Nintendo Switch Online SNES Controller Protocol Research

**Date:** 2026-08-27

**Device under test:** official Nintendo Switch Online Super Nintendo Entertainment System Controller, model HAC-042

**Scope:** read-only research; no PicoSwitch2 personality was implemented, no production source or tests were changed, no controller or adapter was flashed, and no undocumented persistent command was sent.

## Executive summary

The attached controller is a legacy Switch-family Nintendo HID controller, not a Switch 2 custom-BLE controller. Its directly observed USB identity is `057E:2017`, product `SNES Controller`, release `0x0210`, with one HID interface, two 64-byte interrupt endpoints, and the same 203-byte HID report descriptor used by the original Switch Pro Controller. Nintendo-specific device information—not the generic HID fields alone—distinguishes it: the USB proprietary status response reports controller type `0x0B`, named **Lucia** by Horizon/libnx and **SNES** by Linux, SDL, Steam, MissionControl, and BlueRetro.

On the captured battery-retained USB reconnect, the controller emitted 64-byte input report `0x30` before any host Nintendo command. Steam then used the legacy USB handshake (`0x80`/`0x81`), read device status, performed two SPI reads, enabled vibration, and set player LED 1. The controller acknowledged the vibration command, but Linux and SDL deliberately expose no rumble capability for type `0x0B`; the command is compatibility behavior, not evidence of an actuator. The capture contained no HID `SET_REPORT`, feature report, or vendor-specific interface traffic.

The native `0x30` button format is consistently decoded by Linux, SDL, MissionControl, and BlueRetro. For this SNES model the usable physical controls are D-pad, A/B/X/Y, L/R, ZL/ZR, Select/Minus, and Start/Plus. There are no physical Home, Capture, SL/SR, or stick-click controls. Nintendo's official manual states that, inside the SNES Nintendo Switch Online application, **ZL acts as Capture, ZR acts as HOME, and ZL+ZR opens the suspend menu**. Those are application/system semantics; the wire report still carries the ZL and ZR bits and does not thereby set the native Home or Capture bits.

The evidence is sufficient to begin a separately authorized **USB personality implementation pass**, provided it is treated as implementation plus console validation rather than as already-proven console fidelity. A true battery-removed cold-start trace, a Switch 1/2 USB capture, and an authoritative Bluetooth SDP/HID capture remain the principal gaps. A Bluetooth personality is not safely specified by this pass.

## 1. Evidence vocabulary and limits

The following labels are used deliberately:

- **DIRECTLY OBSERVED FROM THE PHYSICAL CONTROLLER** — bytes or behavior captured from the attached HAC-042 in this session.
- **CONFIRMED FROM SOURCE CODE / CAPTURE** — exact behavior present in a public implementation/capture, or multiple independent hardware-derived implementations agreeing.
- **EXTERNAL DOCUMENTATION** — Nintendo, platform, or community documentation; not a local wire observation.
- **REASONED INFERENCE** — the most economical interpretation of the evidence, not a wire fact.
- **UNKNOWN / NEEDS CAPTURE** — evidence is insufficient or the relevant state was not observed.

Important boundaries:

1. The successful USB capture began with a software-controlled hub-port cycle. USB power was removed at the port, but the controller has an internal battery and may have retained volatile protocol state. The observed pre-command `0x30` stream therefore proves battery-retained reconnect behavior, not a battery-removed factory-cold state.
2. A later attempted button exercise did not overlap the packet capture and produced only neutral reports. It is not used as per-button evidence. No further manual button test is required or requested by this document; the provisional physical mapping is based on the exact native format plus independent device-specific source mappings.
3. Windows HID APIs reconstruct a report descriptor from preparsed HID data. The 206-byte HIDAPI result was not byte-identical to the 203-byte wire descriptor and is retained only as evidence about Windows behavior. The USBPcap descriptor transaction is authoritative for raw bytes.
4. PC/Steam recognition demonstrates PC behavior. It does not, by itself, prove Switch 1 or Switch 2 console acceptance.
5. No public raw SNES NSO USB or Bluetooth PCAP suitable for independent byte-level replay was located. The local USB capture is the primary raw USB evidence for this document.

## 2. Raw-evidence manifest

### 2.1 Authoritative local capture

| Item | Value |
|---|---|
| Capture | `C:\Users\notso\AppData\Local\Temp\nso-snes-hub-port-cycle-20260827.pcapng` |
| Size | 455,172 bytes |
| SHA-256 | `A92586D77C84F9A504F656476DA3C3DAF3CCA39FAC9A514BC9EBF08BFA578F69` |
| Capture mechanism | USBPcap, with a read-only host hub-port cycle on the already connected controller |
| Target USB address after re-enumeration | 80 |
| Enumeration begins | frame 128, capture time 13.202283 s |
| Capture safety | standard enumeration, normal HID traffic, and normal Steam interaction only |

The capture remains outside the repository because it is a temporary 455 KiB binary artifact. Its essential raw bytes, frame numbers, hashes, and decoded transactions are preserved below. It can be copied into a future dedicated capture archive if the maintainer elects to retain the binary.

### 2.2 Non-authoritative attempted interaction capture

`C:\Users\notso\AppData\Local\Temp\nso-snes-physical-session-2-20260827.pcapng` contains neutral report `0x30` traffic but did not overlap the user's button interactions. It must not be cited as a physical button matrix.

### 2.3 Read-only commands and mechanisms used

The investigation used these categories of read-only inspection:

```powershell
git status --short
git branch --show-current
Get-PnpDevice
Get-PnpDeviceProperty
Get-CimInstance Win32_PnPEntity
Get-ItemProperty <Windows HID and joystick registry keys>
```

HIDAPI was used to enumerate the attached HID path and query strings, release number, interface, usage page, usage, and Windows' reconstructed report descriptor. USBPcap/Wireshark/tshark were used to capture and decode USB enumeration and interrupt traffic. A narrowly targeted hub-port cycle was used to reproduce enumeration without unplugging the controller. Public projects were cloned into temporary directories and audited with `rg`; no external project files were copied into PicoSwitch2.

Representative reproducible post-capture checks were:

```powershell
$capture = 'C:\Users\notso\AppData\Local\Temp\nso-snes-hub-port-cycle-20260827.pcapng'
Get-FileHash -Algorithm SHA256 -LiteralPath $capture
& 'C:\Program Files\Wireshark\tshark.exe' -r $capture -Y 'usb.device_address == 80'
& 'C:\Program Files\Wireshark\tshark.exe' -r $capture -Y 'usb.device_address == 80 && usb.transfer_type == 0x01'
```

The actual USB-address filter must be rediscovered after every reconnect. The hub-cycle helper was a temporary PowerShell/.NET wrapper around `IOCTL_USB_HUB_CYCLE_PORT` for the already resolved root-hub port 5; it did not send a Nintendo vendor command or modify controller storage.

## 3. USB device identity

### 3.1 Device descriptor

**DIRECTLY OBSERVED FROM THE PHYSICAL CONTROLLER**, frame 129, 18 bytes:

```text
12 01 00 02 00 00 00 40 7e 05 17 20 10 02 01 02 03 01
```

SHA-256: `c6846ead8682d0f838a78939c5f508a9d663eac97efe8dc28822aca7dbd64b3a`

| Field | Value | Evidence |
|---|---:|---|
| `bLength` | 18 | Direct capture |
| `bDescriptorType` | Device (`0x01`) | Direct capture |
| `bcdUSB` | `0x0200` | Direct capture |
| Device class/subclass/protocol | `00/00/00` | Direct capture; class is defined per interface |
| EP0 maximum packet | 64 bytes | Direct capture |
| VID | `0x057E` (Nintendo) | Direct capture |
| PID | `0x2017` | Direct capture |
| `bcdDevice` | `0x0210` | Direct capture and Windows `REV_0210` |
| String indexes | manufacturer 1, product 2, serial 3 | Direct capture |
| Configurations | 1 | Direct capture |

### 3.2 Strings and serial behavior

| String | Directly observed value |
|---|---|
| Manufacturer | `Nintendo Co., Ltd.` |
| Product | `SNES Controller` |
| USB serial | `000000000001` |

The capture obtained serial at frames 171/179, manufacturer at 173/181, and product at 175/185; the repetition came from more than one host enumeration client.

HIDAPI independently returned those strings and release `0x0210`. The USB serial is **STRONGLY SUPPORTED** as a fixed Nintendo USB-transport marker rather than a unique unit serial: BetterJoy explicitly tests `serialNum == "000000000001"` to distinguish USB, and public original Pro Controller USB enumeration examples use the same value. Steam did not use it as the controller identity; after the proprietary status request, Steam logged the controller MAC `ac-fa-e4-4f-02-ee` as its serial-like identity.

The physical device's status response contains address bytes `ee 02 4f e4 fa ac`, which normalize to `ac-fa-e4-4f-02-ee`. This address is unit-specific evidence and should not be hard-coded by a future personality.

### 3.3 Configuration, interface, and endpoints

**DIRECTLY OBSERVED FROM THE PHYSICAL CONTROLLER**, frame 133, 41 bytes:

```text
09 02 29 00 01 01 00 a0 fa
09 04 00 00 02 03 00 00 00
09 21 11 01 00 01 22 cb 00
07 05 81 03 40 00 08
07 05 01 03 40 00 08
```

SHA-256: `1497f4cc8491afdbf88f45e0864897d9e07a4c740f7b997de72cdcbfd9ab9de1`

| Descriptor | Decoded value |
|---|---|
| Configuration | total 41 bytes, one interface, value 1, no configuration string |
| Power | `bmAttributes=0xA0`: bus-powered and remote-wakeup capable |
| Declared maximum draw | `bMaxPower=0xFA`: 500 mA |
| Interface 0 | HID `03/00/00`, alternate 0, two endpoints, no interface string |
| HID descriptor | HID 1.11, country 0, one report descriptor, report length 203 (`0x00CB`) |
| Endpoint `0x81` | interrupt IN, maximum packet 64 bytes, interval 8 ms |
| Endpoint `0x01` | interrupt OUT, maximum packet 64 bytes, interval 8 ms |

There is exactly one HID interface and no vendor-specific interface, IAD, bulk endpoint, audio interface, or vendor-specific configuration descriptor. This sharply distinguishes the controller from Switch 2-generation Pro Controller 2 and NSO GameCube devices.

### 3.4 Windows enumeration

**DIRECTLY OBSERVED FROM THIS WINDOWS HOST:**

- USB parent instance: `USB\VID_057E&PID_2017\000000000001`.
- HID child: `HID\VID_057E&PID_2017\7&3B41BD4A&1&0000` after the captured reconnect.
- Hardware revision: `USB\VID_057E&PID_2017&REV_0210`.
- Bus-reported description: `SNES Controller`.
- Windows generic nodes: `USB Input Device` and `HID-compliant game controller`.
- Driver: Microsoft `HidUsb`, version `10.0.26100.8875` on this system.
- HID top-level usage: Generic Desktop page `0x0001`, Joystick usage `0x0004`.
- One Windows HID interface, not multiple.
- Windows' OEM game-controller registry name was `Wireless Gamepad`; this generic registry label is weaker identity evidence than the USB string and Steam HIDAPI identity.

## 4. HID report descriptor

### 4.1 Authoritative raw bytes

**DIRECTLY OBSERVED FROM THE PHYSICAL CONTROLLER**, frame 139, 203 bytes:

```text
05 01 15 00 09 04 a1 01 85 30 05 01 05 09 19 01
29 0a 15 00 25 01 75 01 95 0a 55 00 65 00 81 02
05 09 19 0b 29 0e 15 00 25 01 75 01 95 04 81 02
75 01 95 02 81 03 0b 01 00 01 00 a1 00 0b 30 00
01 00 0b 31 00 01 00 0b 32 00 01 00 0b 35 00 01
00 15 00 27 ff ff 00 00 75 10 95 04 81 02 c0 0b
39 00 01 00 15 00 25 07 35 00 46 3b 01 65 14 75
04 95 01 81 02 05 09 19 0f 29 12 15 00 25 01 75
01 95 04 81 02 75 08 95 34 81 03 06 00 ff 85 21
09 01 75 08 95 3f 81 03 85 81 09 02 75 08 95 3f
81 03 85 01 09 03 75 08 95 3f 91 83 85 10 09 04
75 08 95 3f 91 83 85 80 09 05 75 08 95 3f 91 83
85 82 09 06 75 08 95 3f 91 83 c0
```

SHA-256: `25f0b3b7e59bdfec05e8cced16e43a8878509865a0cb223f05025c556f3bedba`

It is byte-for-byte the public original Switch Pro Controller descriptor. That fact does **not** make the SNES controller a Pro Controller at the native-protocol level; PID and proprietary controller type distinguish it.

### 4.2 Collection and report summary

The descriptor opens a Generic Desktop / Joystick Application collection. Report `0x30` begins as a generic joystick-shaped input report. A vendor-defined usage page `0xFF00` then declares two more input IDs and four output IDs. No feature report is declared.

| Report ID | Direction | Payload | Total wire size | Descriptor meaning |
|---:|---|---:|---:|---|
| `0x30` | Input | 63 bytes | 64 bytes | 18 buttons, four 16-bit axes, one hat, then constant padding |
| `0x21` | Input | 63 bytes | 64 bytes | vendor usage 1, constant/variable bytes; native subcommand reply |
| `0x81` | Input | 63 bytes | 64 bytes | vendor usage 2, constant/variable bytes; USB command reply |
| `0x01` | Output | 63 bytes | 64 bytes | vendor usage 3, constant/variable/volatile bytes; observed rumble+subcommand transport |
| `0x10` | Output | 63 bytes | 64 bytes declared | vendor usage 4; Linux names it rumble-only (ID, counter, 8 rumble bytes), but SNES capability gates prevent use |
| `0x80` | Output | 63 bytes | 64 bytes | vendor usage 5; observed USB command transport |
| `0x82` | Output | 63 bytes | 64 bytes | vendor usage 6, semantic role not established for SNES and not observed in this capture |

The descriptor flags are important: report `0x30` data fields are `Data, Variable, Absolute`; its pad is `Constant, Variable, Absolute`. Vendor input reports use `Constant, Variable, Absolute` (`0x03`), while vendor outputs use `Constant, Variable, Absolute, Volatile` (`0x83`). These declarations intentionally prevent a generic HID parser from assigning standard semantics to the Nintendo command payloads.

### 4.3 Fully decoded declarative layout of report `0x30`

Offsets below include the report ID at byte 0.

| Wire bytes/bits | HID usage | Size/count | Ranges and units | Main item |
|---|---|---|---|---|
| byte 0 | Report ID `0x30` | 8 bits | — | — |
| byte 1 bits 0–7 | Button 1–8 | 1 bit × 8 | logical 0..1, no physical/unit | Input Data/Var/Abs |
| byte 2 bits 0–1 | Button 9–10 | 1 bit × 2 | logical 0..1 | Input Data/Var/Abs |
| byte 2 bits 2–5 | Button 11–14 | 1 bit × 4 | logical 0..1 | Input Data/Var/Abs |
| byte 2 bits 6–7 | padding | 1 bit × 2 | — | Input Constant |
| bytes 3–4 | X | 16 bits | logical 0..65535 | Input Data/Var/Abs |
| bytes 5–6 | Y | 16 bits | logical 0..65535 | Input Data/Var/Abs |
| bytes 7–8 | Z | 16 bits | logical 0..65535 | Input Data/Var/Abs |
| bytes 9–10 | Rz | 16 bits | logical 0..65535 | Input Data/Var/Abs |
| byte 11 bits 0–3 | Hat switch | 4 bits | logical 0..7; physical 0..315; unit `0x14` (degrees) | Input Data/Var/Abs |
| byte 11 bits 4–7 | Button 15–18 | 1 bit × 4 | logical 0..1 | Input Data/Var/Abs |
| bytes 12–63 | padding | 8 bits × 52 | — | Input Constant |

Collection structure:

```text
Application Collection: Generic Desktop / Joystick
├─ Report 0x30 generic button block
├─ Physical Collection: Generic Desktop / Pointer
│  └─ X, Y, Z, Rz (four 16-bit absolute fields)
├─ Hat and remaining generic buttons
├─ Report 0x30 constant tail
└─ Vendor page 0xFF00 reports 0x21, 0x81, 0x01, 0x10, 0x80, 0x82
```

### 4.4 Descriptor view versus native Nintendo view

**CONFIRMED FROM SOURCE CODE / CAPTURE:** Nintendo's enhanced report `0x30` does not honor the generic declarative meanings above. The same bytes are parsed natively as timer, battery/connection, three button bytes, packed sticks, vibrator status, and IMU samples. On this controller, interpreting bytes 1–2 as generic Buttons 1–14 would turn timer and battery changes into phantom buttons; interpreting bytes 3–10 as four 16-bit axes would combine button and zero-valued controller-specific fields into false axes.

This is why Chromium/Mozilla, SDL, Linux hid-nintendo, MissionControl, and BlueRetro use controller-specific raw parsing. The descriptor still matters for enumeration, report sizing, and generic-host behavior, but it is not the native wire-field specification.

### 4.5 Windows reconstructed descriptor discrepancy

HIDAPI on Windows returned a 206-byte reconstruction with SHA-256 `3dd130...`, not the captured 203-byte descriptor. HIDAPI's Windows implementation and libusb's Windows HID documentation explain that Windows exposes preparsed HID capabilities and client libraries rebuild descriptor-like bytes. **The 203 USB bytes above are authoritative.** A future personality should reproduce those bytes, not the Windows reconstruction.

## 5. Native input reports

### 5.1 Reports observed

| Report ID | Count in target traffic | Length | Role |
|---:|---:|---:|---|
| `0x30` | 2,343 | 64 bytes | periodic full input/status report |
| `0x21` | 4 | 64 bytes | replies to report `0x01` subcommands |
| `0x81` | 6 | 64 bytes | replies to report `0x80` USB commands |

The first `0x30` arrived at capture time 13.260573 s, approximately 19 ms after the report-descriptor response and before the first Nintendo-specific host OUT command.

Across 26.375613 s of captured `0x30` traffic, the median inter-report delta was 8.002 ms. The mean including host scheduling gaps was 11.262 ms and the maximum gap was 375.992 ms. The descriptor endpoint interval is 8 ms; the trace shows mostly 8/16 ms scheduling. It does not justify a stronger constant-rate claim.

### 5.2 Native `0x30` byte map

**CONFIRMED FROM SOURCE CODE / CAPTURE:** byte positions match Linux, SDL, MissionControl, BlueRetro, and dekuNukem's Switch protocol notes. Direct neutral samples confirm the SNES controller zeros the stick and IMU regions.

| Byte(s) | Meaning | SNES observation |
|---:|---|---|
| 0 | Report ID `0x30` | `30` |
| 1 | rolling timer/counter | changes while streaming; sample `75` |
| 2 | battery/connection status | sample `91`: full, charging, host/USB-powered |
| 3 | right/face button byte | neutral `00` |
| 4 | shared/system button byte | neutral `80`; bit 7 is the Charging Grip slot, not a physical button |
| 5 | left/D-pad button byte | neutral `00` |
| 6–8 | left stick, packed 12-bit X/Y on controllers that have one | all zero in captured SNES traffic |
| 9–11 | right stick, packed 12-bit X/Y | all zero |
| 12 | vibrator input/status byte | zero |
| 13–48 | three IMU samples on IMU-capable controllers | all zero |
| 49–63 | unused/padding | zero in samples |

Representative idle report prefix:

```text
30 75 91 00 80 00 00 00 00 00 00 00 00 ... 00
```

The full 64-byte sample represented by that prefix was:

```text
30 75 91 00 80 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

### 5.3 Exact native button bits

All source implementations treat these bits as **active-high**; neutral bytes are `00 80 00` because shared byte bit 7 reports the charging-grip/USB slot.

| Wire location | Native meaning | Physical SNES control | Confidence |
|---|---|---|---|
| byte 3 bit 0 | Y | Y | **STRONGLY SUPPORTED** |
| byte 3 bit 1 | X | X | **STRONGLY SUPPORTED** |
| byte 3 bit 2 | B | B | **STRONGLY SUPPORTED** |
| byte 3 bit 3 | A | A | **STRONGLY SUPPORTED** |
| byte 3 bit 4 | right SR | none | **CONFIRMED format; absent control** |
| byte 3 bit 5 | right SL | none | **CONFIRMED format; absent control** |
| byte 3 bit 6 | R | R | **STRONGLY SUPPORTED** |
| byte 3 bit 7 | ZR | ZR | **STRONGLY SUPPORTED** |
| byte 4 bit 0 | Minus | Select | **STRONGLY SUPPORTED** |
| byte 4 bit 1 | Plus | Start | **STRONGLY SUPPORTED** |
| byte 4 bit 2 | right-stick click | none | **CONFIRMED format; absent control** |
| byte 4 bit 3 | left-stick click | none | **CONFIRMED format; absent control** |
| byte 4 bit 4 | Home | none | **CONFIRMED format; absent control** |
| byte 4 bit 5 | Capture | none | **CONFIRMED format; absent control** |
| byte 4 bit 6 | reserved | none | **CONFIRMED format** |
| byte 4 bit 7 | Charging Grip / USB slot | status, not a physical control | **DIRECTLY OBSERVED set at idle** |
| byte 5 bit 0 | D-pad Down | Down | **STRONGLY SUPPORTED** |
| byte 5 bit 1 | D-pad Up | Up | **STRONGLY SUPPORTED** |
| byte 5 bit 2 | D-pad Right | Right | **STRONGLY SUPPORTED** |
| byte 5 bit 3 | D-pad Left | Left | **STRONGLY SUPPORTED** |
| byte 5 bit 4 | left SR | none | **CONFIRMED format; absent control** |
| byte 5 bit 5 | left SL | none | **CONFIRMED format; absent control** |
| byte 5 bit 6 | L | L | **STRONGLY SUPPORTED** |
| byte 5 bit 7 | ZL | ZL | **STRONGLY SUPPORTED** |

Why these physical mappings are not merely guessed:

- Linux's exact `snescon_button_mappings` exposes A/B/X/Y, L/R, ZL/ZR, Minus/Plus, and D-pad at these native bits, and deliberately omits Home/Capture/SL/SR/stick clicks.
- SDL's dedicated Nintendo Classic handler identifies PID `0x2017`, type 11, and parses the same native fields.
- MissionControl's packed `SwitchButtonData` layout agrees bit-for-bit.
- BlueRetro's native report `0x30` map and SNES masks agree.
- Nintendo's physical-control diagram and manual agree on the shell controls.

The lack of an overlapping direct button trace is preserved as a confidence boundary, but there is no remaining mapping conflict that warrants a large manual test matrix.

### 5.4 Report `0x21`: subcommand response

For the observed legacy Nintendo protocol:

| Byte(s) | Meaning |
|---:|---|
| 0 | `0x21` |
| 1–12 | same timer/status/buttons/stick/vibrator input core as `0x30` |
| 13 | ACK/status (`0x80` for observed setters, `0x90` for observed SPI reads) |
| 14 | echoed subcommand |
| 15–63 | subcommand response data/padding |

Example SPI-read response prefix, frame 446:

```text
21 c7 91 00 80 00 00 00 00 00 00 00 00 90 10 10 80 00 00 16
```

It acknowledges subcommand `0x10`, echoes little-endian address `0x00008010`, and length `0x16`. The following 22 bytes were `FF` in this capture.

### 5.5 Simple report `0x3F` and mode dependence

Report `0x3F` was not observed over USB and is not declared in the captured USB report descriptor. SDL and BlueRetro handle it as the legacy/simple Bluetooth input mode. Their SNES-specific parsing uses the common face/shoulder/start/select button block plus a hat switch; BlueRetro contains a specific SNES ZR case at simple-report bit 15.

Therefore:

- enhanced/native report `0x30` over captured USB is **CONFIRMED**;
- Bluetooth/simple report `0x3F` and its provisional mapping are **STRONGLY SUPPORTED** by independent implementations;
- the exact SNES Bluetooth HID descriptor and a raw wireless `0x3F` sample remain **UNKNOWN / NEEDS CAPTURE**.

## 6. USB output, control, and feature traffic

### 6.1 Enumeration sequence

**DIRECTLY OBSERVED FROM THE PHYSICAL CONTROLLER:**

1. `GET_DESCRIPTOR(Device, 18)`.
2. `GET_DESCRIPTOR(Configuration, 9)`.
3. `GET_DESCRIPTOR(Configuration, 41)`.
4. `SET_CONFIGURATION(1)`.
5. HID class `SET_IDLE`.
6. `GET_DESCRIPTOR(HID Report, 203)`.
7. First interrupt-IN report `0x30`.
8. String descriptor reads for serial, manufacturer, and product.
9. Steam's interrupt-OUT Nintendo initialization.

The exact setup tuples for the standard/class transition requests and HID descriptor fetch were:

| Frame | `bmRequestType` | `bRequest` | `wValue` | `wIndex` | `wLength` |
|---:|---:|---:|---:|---:|---:|
| 134/135 | `00` | `09` SET_CONFIGURATION | `0001` | `0000` | `0000` |
| 136/137 | `21` | `0A` SET_IDLE | `0000` | `0000` | `0000` |
| 138/139 | `81` | `06` GET_DESCRIPTOR | `2200` | `0000` | `00CB` |

No HID `SET_REPORT` transfer occurred. No HID feature report was read or written. There is no feature report in the descriptor. All observed Nintendo commands used interrupt OUT endpoint `0x01` and responses used interrupt IN endpoint `0x81`.

Linux names feature IDs `0x70`–`0x75` for OTA/update-memory operations in its broader Switch-controller driver. They are not declared by this SNES USB HID descriptor and were not requested in the trace. They must not be treated as part of the ordinary SNES personality without device-specific evidence; deliberately exercising update-memory paths would also violate this pass's read-only boundary.

### 6.2 Steam initialization observed after reconnect

Every OUT packet was padded to 64 bytes.

| Order | Frame(s) | Host request | Device reply | Interpretation |
|---:|---|---|---|---|
| 1 | 186 → 190 | `80 01` | `81 01 00 0b ee 02 4f e4 fa ac ...` | USB status: controller type `0x0B`, controller address |
| 2 | 406 → 412 | `80 02` | `81 02` | USB handshake command 2 |
| 3 | 414/422 → 424 | `80 03` | `81 03` | USB baud-rate command; host retried before reply |
| 4 | 426/428/430 → 431/435/438 | `80 02` | `81 02` | second handshake; delayed duplicate replies visible |
| 5 | 434 | `80 04` | none required | USB timeout-disable/transition command |
| 6 | 439 → 446 | output `01`, subcommand `10`, address `0x8010`, length `0x16` | input `21`, ACK `90`, 22 × `FF` | SPI read |
| 7 | 448 → 454 | output `01`, subcommand `10`, address `0x603D`, length `0x12` | input `21`, ACK `90`, 18 × `FF` | SPI read |
| 8 | 456 → 462 | output `01`, subcommand `48`, value `01` | input `21`, ACK `80 48` | enable vibration command accepted |
| 9 | 464 | `80 04` | none required | repeated USB transition command |
| 10 | 468 → 476 | output `01`, subcommand `30`, value `01` | input `21`, ACK `80 30` | set player LED 1 |

Exact observed subcommand requests:

```text
01 00 00 01 40 40 00 01 40 40 10 10 80 00 00 16 ...
01 01 00 01 40 40 00 01 40 40 10 3d 60 00 00 12 ...
01 02 00 01 40 40 00 01 40 40 48 01 ...
01 03 00 01 40 40 00 01 40 40 30 01 ...
```

Report `0x01` layout for these packets is:

| Offset | Meaning |
|---:|---|
| 0 | report ID `0x01` |
| 1 | 4-bit packet counter/sequence in low nibble |
| 2–9 | two 4-byte legacy HD-rumble frames; neutral value observed as `00 01 40 40` per side |
| 10 | subcommand |
| 11–63 | subcommand data and padding |

No `0x03` set-input-mode subcommand appeared in this reconnect. The controller was already sending `0x30`, so Steam did not need to request that transition. Linux's normal cold initialization does issue subcommand `0x03` to select report `0x30`; a future implementation must support it even though it is absent from this battery-retained trace.

### 6.3 Control-transfer conclusion

The only HID class control request observed was `SET_IDLE`. There were no vendor-class control requests, feature operations, or control-path output reports. This controller's proprietary host interaction in the captured state is entirely interrupt-endpoint based.

## 7. Initialization and state transition

### 7.1 What the physical trace proves

**DIRECTLY OBSERVED:** after USB enumeration and `SET_IDLE`, the device began report `0x30` without waiting for `0x80 01`, SPI access, vibration enable, LED assignment, or a set-report-mode command. Steam then learned type/address and performed normal Switch-family setup.

### 7.2 What public host code does

**CONFIRMED FROM SOURCE CODE:** Linux and SDL use the original Switch USB handshake:

```text
host 80 02  -> device 81 02   handshake
host 80 03  -> device 81 03   set USB baud rate
host 80 02  -> device 81 02   handshake again
host 80 04                    disable USB timeout / finish transition
```

They query controller/device information and select enhanced input report `0x30` with subcommand `0x03` where needed. Player LEDs use subcommand `0x30`. The sequence is tolerant of retries and of controllers already in enhanced mode.

### 7.3 Cold-start uncertainty

**UNKNOWN / NEEDS CAPTURE:** whether a battery-removed SNES controller initially streams simple `0x3F`, enhanced `0x30`, or no periodic input over USB before the first Nintendo command. The direct capture cannot settle it because internal battery power may have preserved mode. Safe future implementation should accept the full legacy sequence and may emit a conservative initial mode consistent with an additional genuine cold-start capture.

### 7.4 Difference from other Nintendo controller generations

- Original Switch Pro Controller: same legacy USB descriptor shape and legacy `0x80`/`0x81` plus report `0x01`/`0x21` command family; richer hardware capabilities.
- Pro Controller 2: composite USB device with HID, vendor bulk, and audio interfaces; command channel is vendor bulk and primary console input is report `0x09`. It is a different generation and protocol.
- NSO GameCube Controller for Switch 2: composite HID + vendor-bulk device, primary console input `0x0A`, output `0x03`, and Switch 2 command framing. It is also a different generation.
- SNES NSO: original Switch-era legacy HID, type `0x0B`/Lucia, report `0x30`, no vendor bulk interface.

## 8. PC and Steam behavior

### 8.1 Steam identity

**DIRECTLY OBSERVED FROM LOCAL STEAM LOGS:**

- Name: `Nintendo SNES Controller`.
- VID/PID: `057e:2017`.
- Version: 528 decimal (`0x0210`).
- Serial-like identity after protocol query: `ac-fa-e4-4f-02-ee`.
- Driver: `SDL_JOYSTICK_HIDAPI_NINTENDO_CLASSIC (ENABLED)`.
- Nintendo controller subtype: 11.
- Usage page/usage: `1/4` (Generic Desktop / Joystick).
- A separate fallback identity appeared as `57e-2017-57706c0` when a unit serial was unavailable/invalid.

Steam therefore identifies the device specifically, not merely as an anonymous generic joystick. The decisive evidence is the exact VID/PID plus Nintendo status/type response. The generic report descriptor is shared with Pro Controller and cannot independently convey “SNES.”

### 8.2 Live Steam normalized mapping

The current local Steam/SDL layer logged:

```text
030048f67e050000172000001002680b,*,a:b0,b:b1,back:b4,dpdown:h0.4,
dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b9,lefttrigger:a4,
rightshoulder:b10,righttrigger:a5,start:b6,x:b2,y:b3,
hint:!SDL_GAMECONTROLLER_USE_BUTTON_LABELS:=1,crc:f648,platform:Windows,
```

This is a normalized Steam game-controller mapping, not raw report-`0x30` byte positions. It demonstrates that Steam exposes the four face buttons, D-pad, shoulders, ZL/ZR as trigger-class inputs, and Select/Start as back/start.

Current upstream SDL's database entry uses different normalized indices:

```text
030000007e0500001720000011810000,Nintendo SNES Controller,crc:f648,
a:b0,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,
leftshoulder:b4,lefttrigger:b6,rightshoulder:b5,righttrigger:b7,start:b9,
x:b3,y:b2,hint:!SDL_GAMECONTROLLER_USE_BUTTON_LABELS:=1,
```

This difference is not a raw-protocol contradiction: normalized index assignment changes with the active HIDAPI/backend path. The local live mapping is authoritative for this Steam installation; SDL's native parser remains authoritative for its own source path.

### 8.3 Does behavior change when Steam opens it?

Yes. The controller was already streaming `0x30`, but Steam sent Nintendo USB status/handshake commands, read two SPI regions, enabled the vibration protocol, and assigned player LED 1. The input report ID did not change in the captured session. Steam-specific initialization therefore changes command/LED state but was not required to begin the already-active enhanced stream.

### 8.4 Source disagreement

PCGamingWiki community text has described Steam as recognizing the unit as a Switch Pro Controller. The directly observed current Steam logs instead identify `Nintendo SNES Controller`, subtype 11, through SDL's Nintendo Classic driver. The direct local evidence and current SDL source are stronger and supersede that generic community description for this environment.

## 9. Nintendo-specific identity

### 9.1 Confirmed identity signals

| Signal | SNES value | Evidence strength |
|---|---|---|
| USB VID/PID | `057E:2017` | **DIRECTLY OBSERVED** |
| USB product | `SNES Controller` | **DIRECTLY OBSERVED** |
| USB release | `0x0210` | **DIRECTLY OBSERVED** |
| Proprietary controller type | `0x0B` | **DIRECTLY OBSERVED** in `81 01` |
| Horizon device type | Lucia = 11 | **CONFIRMED FROM SOURCE CODE / platform RE** |
| Horizon style tag | `NpadLucia` | **CONFIRMED FROM SOURCE CODE / platform RE** |
| Horizon footer UI type | Lucia = 19 | **CONFIRMED FROM SOURCE CODE / platform RE** |
| Native report family | enhanced `0x30`, reply `0x21`, USB reply `0x81` | **DIRECTLY OBSERVED** |
| Capability pattern | digital controls, no sticks/IMU/rumble/home light | **STRONGLY SUPPORTED** |

libnx exposes Lucia as a distinct controller style, device type, footer UI type, and region variant. This supports a distinct system UI identity rather than a generic Pro Controller skin. Switchbrew independently lists USB `057E:2017` and device type 11/Lucia.

### 9.2 PID is necessary but not sufficient wirelessly

BlueRetro notes that some Nintendo classic controllers can present misleading shared identity; in particular Genesis/Mega Drive variants may appear under the SNES-family PID in Bluetooth paths. SDL therefore uses controller device information/type as well as VID/PID. **REASONED INFERENCE:** a faithful personality should provide both the exact PID and type `0x0B`, not rely on the PID alone.

### 9.3 Comparison with named families

- Standard Switch Pro Controller: PID `0x2009`, proprietary type `0x03`, same legacy HID descriptor but sticks, motion, rumble, Home/Capture, and home light are real capabilities.
- Pro Controller 2: PID `0x2069`, Switch 2 composite USB and custom wireless protocol, not legacy Lucia.
- Joy-Con: legacy Switch report family but left/right controller types and split capability/layout identities.
- NSO N64: legacy Nintendo Classic family with different PID/type and an analog stick plus rumble capability in Linux's table.
- NSO GameCube: PID `0x2073`, Switch 2-generation composite USB, device-specific report `0x0A`, analog controls and simple motor rumble.
- Other NSO classic controllers: device name/PID/type/capability combination matters; a shared generic HID report descriptor is not enough.

No evidence found indicates that SNES identity depends on writable persistent configuration. The captured identity is visible through immutable descriptors and read-only status responses.

## 10. Special system and application behavior

### 10.1 Officially documented behavior

Nintendo's [SNES Controller information sheet](https://www.nintendo.com/eu/media/downloads/support_1/other_19/SuperNintendoEntertainmentSystem_Information_Controller.pdf) documents the physical controls and the following behavior in the SNES Nintendo Switch Online application:

- ZL performs Capture.
- ZR performs HOME.
- ZL+ZR opens the suspend menu.

These are application/system translations. They do not imply that ZL sets the native Capture bit or that ZR sets the native Home bit.

Nintendo's [product page](https://www.nintendo.com/en-ca/store/products/super-nintendo-entertainment-system-controller/) states compatibility with Nintendo Switch and Nintendo Switch 2, an approximate 20-hour battery life, and a Nintendo Switch system-version requirement of 9.0.0 or later. Nintendo's [Switch 2 accessory compatibility page](https://www.nintendo.com/en-gb/Hardware/Nintendo-Switch-2/Compatibility-with-Nintendo-Switch-accessories-2786091.html) states that original Switch controllers work wirelessly and can be charged via the Switch 2 dock, but original Switch controllers cannot wake Switch 2 from sleep using HOME.

Nintendo's [SNES Controller troubleshooting page](https://www.nintendo.com/en-gb/Support/Nintendo-Switch/Super-NES-Controller-Does-Not-Respond-Correctly-1670147.html) says use in other games and system menus is possible but full functionality is not guaranteed. It also documents that when two SNES controllers are connected, only player 1 navigates the SNES application's game-selection menu, and that approximate battery state is visible under Controllers.

### 10.2 System icon and Lucia identity

**STRONGLY SUPPORTED:** libnx's distinct `HidAppletFooterUiType_Lucia` and `HidNpadStyleTag_NpadLucia` provide an explicit OS-level path for an SNES-specific controller icon/footer. This pass did not capture the Switch UI visually, so the exact rendered icon is not directly observed here.

### 10.3 Game semantics

The controller exposes standard Switch logical ZL/ZR and Plus/Minus semantics at the native HID layer. The SNES application can remap those logical controls to Capture/HOME/suspend functions. Other games can receive them as ordinary logical controls but may not be designed around the reduced physical layout. No HID-level evidence shows synthesized Home/Capture.

MissionControl's `ApplyButtonCombos` synthesizes Home from Minus+D-pad Down and Capture from Minus+D-pad Up for its own forwarding environment. That is **MissionControl-added policy**, not evidence that the official SNES controller emits those bits.

### 10.4 Community observation

Community reports describe SNES application menu sounds changing to Super Mario World-style sounds when this controller is connected. This is a low-level-of-authority application easter egg and has no demonstrated HID protocol consequence. It is recorded as **community observation**, not a requirement.

## 11. LED and player indicators

The controller has four player-number indicators and a separate recharge LED, per Nintendo's physical documentation and platform support.

**DIRECTLY OBSERVED PROTOCOL:** Steam sent subcommand `0x30` with value `0x01`; the device replied with report `0x21`, ACK/subcommand bytes `80 30`. This is player 1.

**CONFIRMED FROM SOURCE CODE:** legacy Switch player LED control uses subcommand `0x30`; low-nibble bits select steady LEDs and high-nibble bits select flashing LEDs. Linux/SDL expose player LED support for SNES. Common masks are individual `01`, `02`, `04`, `08` or cumulative host conventions, with the corresponding high-nibble flash bits.

Only mask `0x01` was captured on this physical controller. Exact SNES visual behavior for every steady/flashing combination and the unassigned/pairing startup animation is **UNKNOWN / NEEDS VISUAL OR CONSOLE CAPTURE**. There is no supported Home-button light capability for type `0x0B`.

## 12. Rumble, haptics, and motion

### 12.1 Rumble

**DIRECTLY OBSERVED:** the controller accepted and acknowledged subcommand `0x48 01` (“enable vibration”) while neutral rumble frames accompanied subcommands.

**STRONGLY SUPPORTED:** the hardware has no rumble actuator. Linux's `joycon_has_rumble()` returns false for SNES while true for Joy-Con, Pro, and N64 types. SDL's Nintendo Classic capability selection likewise excludes SNES rumble. BlueRetro's SNES capability mask has no rumble output. Nintendo's official specifications do not advertise vibration.

Consequently, acknowledging `0x48` is a shared legacy-protocol compatibility behavior and should be treated as a no-op for physical output. It is not evidence of HD rumble or ERM vibration. The controller was not opened and an actuator was not destructively inspected, so “no physical rumble” remains **STRONGLY SUPPORTED**, not a direct internal-hardware observation.

### 12.2 Motion and analog controls

Linux and SDL capability tables exclude both IMU and joysticks for SNES. Direct reports had bytes 6–11 and 13–48 all zero. The hardware has a digital D-pad and no analog sticks. There is no evidence of accelerometer, gyro, or motion reporting.

## 13. Power, battery, connection, and status

### 13.1 Captured status byte

The captured input-core status byte was `0x91`.

| Bits | Linux native interpretation | Captured state |
|---|---|---|
| 7–5 | battery capacity value | `100b` = level 4 / full |
| 4 | charging | 1 |
| 3–1 | connection-specific/reserved in this parser | 0 |
| 0 | host-powered / USB | 1 |

Thus the attached device reported full, charging, and USB/host-powered. Byte 4 bit 7 was also set in the shared input core, matching the legacy charging-grip/USB slot.

### 13.2 Other identity/status data

- Connection-info low nibble was 1 in the captured replies.
- The proprietary USB status response exposed controller type `0x0B` and the physical controller address.
- Player assignment is host-controlled via subcommand `0x30`.
- The USB configuration declares up to 500 mA bus draw. Nintendo's external charging specification and USB-C behavior are separate from that descriptor limit.
- Public regulatory records for model HAC-042 list a 3.7 V, 525 mAh (1.94 Wh) battery; this was not measured locally.
- `bcdDevice=0x0210` is a USB device-release field. It should not be mislabeled as the controller firmware version.
- No explicit firmware-version response was captured. The two SPI reads returned `FF` data at `0x8010` and `0x603D`; without a device-specific memory map that result must not be interpreted as absent firmware or calibration.

## 14. Bluetooth research

### 14.1 Transport and identity

The best available evidence supports **Bluetooth Classic BR/EDR HID/HIDP**, not BLE:

- BlueRetro handles `SNES Controller` in its Bluetooth HIDP path.
- Linux hid-nintendo matches `057E:2017` on `BUS_BLUETOOTH`.
- MissionControl intercepts it through the original Switch Bluetooth HID controller path.
- The controller belongs to the original Switch generation; Switch 2's custom BLE controller protocol is a separate family.

The Bluetooth name `SNES Controller` is **STRONGLY SUPPORTED** by BlueRetro, MissionControl, SDL/classic-controller handling, and community enumeration. Official instructions pair it with the small SYNC button through Change Grip/Order.

### 14.2 Wireless reports

- Enhanced native input `0x30`: **STRONGLY SUPPORTED** wirelessly by Linux/SDL/MissionControl/BlueRetro.
- Simple input `0x3F`: **STRONGLY SUPPORTED** as legacy/simple Bluetooth mode; not observed locally.
- Output `0x01`, subcommand reply `0x21`, and player LED command `0x30`: **STRONGLY SUPPORTED** over the legacy Bluetooth HID channel.
- Exact wireless report cadence and transition sequence: **UNKNOWN / NEEDS CAPTURE**.

### 14.3 Remaining Bluetooth unknowns

No authoritative SNES-specific raw SDP dump or wireless PCAP was located. The following remain unknown:

- exact Bluetooth Class of Device;
- exact SDP service and attribute records;
- exact Bluetooth HID report descriptor and whether it differs by firmware;
- link authentication/encryption requirements and pairing-key lifecycle;
- controller firmware/device-info response over Bluetooth;
- exact simple-to-enhanced mode sequence on a clean pairing;
- behavior on Switch 1 versus Switch 2 reconnect/wake.

MissionControl accepts several broad peripheral/gamepad/joystick/keyboard Class-of-Device categories. That allowlist does not identify this device's exact CoD and must not be promoted to one.

No wireless pairing, bond reset, fuzzing, or invasive test was performed in this pass.

## 15. Existing open-source support audit

### 15.1 Source snapshot manifest

| Project | Audited revision | Most relevant files/findings |
|---|---|---|
| [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research) | `d1c5a7f7ba298f83017fae84952a4e6d2ef8fc92` | Methodology and Switch 2 Pro/GC comparison captures; no SNES-specific capture/table found |
| [Linux hid-nintendo](https://github.com/torvalds/linux/blob/73e3f0710014fe6d4ed98cfc02292f6121db7558/drivers/hid/hid-nintendo.c) | `73e3f0710014fe6d4ed98cfc02292f6121db7558` | PID/type, exact native bits, SNES mapping, init, battery, LEDs, capability gates |
| [Linux HID IDs](https://github.com/torvalds/linux/blob/73e3f0710014fe6d4ed98cfc02292f6121db7558/drivers/hid/hid-ids.h) | same | `USB_DEVICE_ID_NINTENDO_SNESCON 0x2017` |
| [SDL Nintendo HIDAPI](https://github.com/libsdl-org/SDL/blob/774a3a0ab1b8d0717b412bb2aadc0168d930e87d/src/joystick/hidapi/SDL_hidapi_switch.c) | `774a3a0ab1b8d0717b412bb2aadc0168d930e87d` | dedicated Nintendo Classic recognition, native/simple reports, initialization, capability exposure |
| [SDL USB IDs](https://github.com/libsdl-org/SDL/blob/774a3a0ab1b8d0717b412bb2aadc0168d930e87d/src/joystick/usb_ids.h) and [gamepad DB](https://github.com/libsdl-org/SDL/blob/774a3a0ab1b8d0717b412bb2aadc0168d930e87d/src/joystick/SDL_gamepad_db.h) | same | PID and USB/BT normalized mappings |
| [MissionControl switch_controller.hpp](https://github.com/ndeadly/MissionControl/blob/d3941d433f15827de8aea116d61ea17bb61d0bcc/mc_mitm/source/controllers/switch_controller.hpp) | `d3941d433f15827de8aea116d61ea17bb61d0bcc` | exact packed native button/report definitions; PID table |
| [MissionControl switch_controller.cpp](https://github.com/ndeadly/MissionControl/blob/d3941d433f15827de8aea116d61ea17bb61d0bcc/mc_mitm/source/controllers/switch_controller.cpp) | same | controller processing and MissionControl-added Home/Capture combos |
| [BlueRetro `sw.c`](https://github.com/darthcloud/BlueRetro/blob/e1a9831a875f5313a923160a1379a7ebbfaa2b11/main/adapter/wireless/sw.c) | `e1a9831a875f5313a923160a1379a7ebbfaa2b11` | SNES name/subtype, native `0x30`, simple `0x3F`, special ZR bit, capability masks |
| [BetterJoy `Program.cs`](https://github.com/Davidobot/BetterJoy/blob/b6715638a3ed1084f8968e8cafebbc6fe2ed0096/BetterJoyForCemu/Program.cs) / [`Joycon.cs`](https://github.com/Davidobot/BetterJoy/blob/b6715638a3ed1084f8968e8cafebbc6fe2ed0096/BetterJoyForCemu/Joycon.cs) | `b6715638a3ed1084f8968e8cafebbc6fe2ed0096` | PID recognition and fixed USB serial heuristic |
| [libnx HID types](https://github.com/switchbrew/libnx/blob/dbcc1beafc6b47b5ffbeb8ba82463a7d45da40bb/nx/include/switch/services/hid.h) | `dbcc1beafc6b47b5ffbeb8ba82463a7d45da40bb` | Lucia style, device type 11, footer UI, region/state structure |
| [joycond](https://github.com/DanielOgorchock/joycond) | `0df025ac5dc284b1f31172b6b252321ba788c4de` | exact PID/model and Linux evdev pairing/group policy |
| [Dolphin](https://github.com/dolphin-emu/dolphin) | `4f8af23db516d8b6e9cd00e7b261a65b026514a8` | no dedicated SNES NSO raw handler found in audited InputCommon; consumes generic/SDL input |

### 15.2 Project-specific conclusions

**ndeadly/switch2_controller_research.** Valuable methodological reference and authoritative comparison material for Pro Controller 2 and NSO GameCube. It contains no SNES-specific raw capture or device implementation at the audited revision. Its greatest value here is preventing a generational category error: SNES uses the older Switch HID protocol, whereas its documented Switch 2 devices use composite USB/vendor bulk and custom BLE.

**Linux hid-nintendo.** Strongest public SNES-specific native decoder. The type table names `0x0B` SNES, the exact SNES input map excludes nonexistent controls, and capability predicates exclude sticks, IMU, rumble, and home LED while retaining player LEDs and battery.

**SDL.** Strongest current PC/game-library corroboration. It recognizes exact PID/type through a Nintendo Classic HIDAPI driver, labels the device `Nintendo SNES Controller`, skips IMU calibration and rumble exposure, but still sends shared initialization including enable-vibration. This exactly explains the local Steam capture.

**MissionControl.** Provides a clean packed native report definition and command constants matching Linux/SDL. Its button-combo synthesis is explicitly software policy. MissionControl is therefore useful for layout and compatibility behavior but cannot be used to claim the genuine controller emits synthesized system buttons.

**BlueRetro.** Provides independent native and simple-report decoders, SNES name/subtype identification, and the unusual simple-report ZR special case. It is particularly useful Bluetooth evidence, though a raw genuine capture remains preferable.

**BetterJoy and joycond.** Corroborate PID, fixed USB serial behavior, SNES model recognition, and Linux input exposure. Their higher-level pairing/group rules are application policy, not wire protocol.

**Dolphin.** No dedicated `057E:2017` raw-protocol handler was found in the audited InputCommon source. SNES use is expected through Dolphin's generic/SDL controller interfaces. This is a scoped source finding, not a claim that Dolphin cannot use the controller.

**Ryujinx and other Switch emulators.** The original Ryujinx repository is no longer the stable primary upstream it once was. Accessible mirrors expose generic SDL2 input infrastructure; no exact, dedicated SNES raw-protocol handler was identified. This is weak negative evidence and should not be treated as absence of usable controller support.

**Steam Input.** Local logs are the authoritative current-system evidence and show a dedicated Nintendo Classic/SNES identity. Steam's implementation is not fully public, so SDL source and the live logs are used together.

## 16. Comparison table

| Property | SNES NSO | Standard Switch Pro Controller | Pro Controller 2 | NSO GameCube Controller |
|---|---|---|---|---|
| Generation | Switch 1 legacy Classic | Switch 1 legacy | Switch 2 | Switch 2 |
| VID/PID | `057E:2017` **direct** | `057E:2009` | `057E:2069` | `057E:2073` |
| Device class | `00/00/00`, per-interface HID | `00/00/00`, per-interface HID | `EF/02/01` composite | `EF/02/01` composite |
| USB configuration | 41 B, 1 HID IF | substantially same 1-HID legacy topology | 268 B, 5 IF: HID + vendor bulk + audio control/streaming | 80 B, 2 IF: HID + vendor bulk |
| HID report descriptor | 203 B; legacy Pro descriptor | same 203 B public descriptor | 97 B | 97 B |
| HID endpoints | IN `81`, OUT `01`, interrupt 64 B, interval 8 ms | same legacy shape | HID IN/OUT 64 B interval 4 ms genuine; vendor bulk IN/OUT; audio endpoints | HID IN/OUT 64 B interval 4 ms; vendor bulk IN/OUT |
| Primary console input | `0x30` | `0x30` | `0x09` (`0x05` alternate/PC) | `0x0A` (`0x05` PC path) |
| Primary HID output | `0x01`; USB setup `0x80` | `0x01`; USB setup `0x80` | `0x02` rumble; commands on vendor bulk | `0x03` rumble; commands on vendor bulk |
| Nintendo type identity | `0x0B`, Lucia | `0x03`, Pro | Switch 2 firmware/device type fields | distinct Switch 2 GC identity; some response semantics still open |
| Buttons | D-pad, A/B/X/Y, L/R, ZL/ZR, Select/Start | full Pro set | full Pro 2 set including new controls | GameCube layout, Z→ZR, analog-trigger/detent semantics |
| Analog controls | none | two sticks | two sticks | two sticks + analog L/R triggers |
| Motion | none | 6-axis IMU | IMU, native packed Switch 2 reports | report supports motion field; console enable/use not fully closed in repository evidence |
| Rumble | no actuator strongly supported; accepts legacy enable command | HD rumble | Switch 2 HD-rumble-class dual LRA | simple native motor rumble, hardware-confirmed by PicoSwitch2 work |
| LEDs | four player LEDs + recharge; no home light | four player + home light | player/home/body functions per Switch 2 protocol | player indicators; no Pro-style home-light claim here |
| Initialization | legacy interrupt HID `80/81`, `01/21`; can already stream `30` on reconnect | same legacy family, richer subcommands | vendor-bulk Switch 2 command handshake and feature/report select | vendor-bulk Switch 2 command handshake and report select |
| PC behavior | Steam: `Nintendo SNES Controller`, subtype 11, Nintendo Classic HIDAPI | Steam/SDL Switch Pro | dedicated Switch 2 paths; report `05` used on PC in project evidence | Steam selects common report `05` in project evidence |
| Switch behavior | distinct Lucia style/footer; official Switch 1/2 compatibility; reduced controls/app remaps | native original Pro | native Switch 2 controller, audio/headset features | native Switch 2 GC identity, controls, and rumble |
| Wireless family | Bluetooth Classic HID/HIDP strongly supported | Bluetooth Classic HID | custom Switch 2 BLE | custom Switch 2 BLE |

The Pro Controller 2 row uses genuine descriptor values from ndeadly/PicoSwitch2 research. PicoSwitch2's production Pro 2 personality intentionally differs in some fields; that implementation choice is outside this research pass. The GameCube row follows this repository's hardware evidence and preserves its remaining motion/identity caveats.

## 17. Requirements for a PicoSwitch2 SNES NSO Personality

This section is a future implementation specification only. It does not authorize or contain implementation changes.

### 17.1 USB identity and descriptors

| Requirement | Classification | Basis / caveat |
|---|---|---|
| Emulate VID/PID `057E:2017`. | **CONFIRMED** | Direct device descriptor |
| Emit device descriptor bytes matching the 18-byte dump, including USB 2.0, class `00/00/00`, EP0 64, `bcdDevice=0x0210`, and string indexes 1/2/3. | **CONFIRMED** | Direct capture |
| Emit manufacturer `Nintendo Co., Ltd.` and product `SNES Controller`. | **CONFIRMED** | Direct string reads |
| Emit USB serial `000000000001` for transport fidelity. | **STRONGLY SUPPORTED** | Direct on this unit; BetterJoy and public Pro evidence support model-wide fixed behavior |
| Emit the exact 41-byte configuration: one HID IF, interrupt IN `81` and OUT `01`, 64-byte packets, 8 ms intervals, remote wake and 500 mA declaration. | **CONFIRMED** | Direct capture |
| Emit the exact 9-byte HID descriptor and 203-byte report descriptor preserved above. | **CONFIRMED** | Direct capture and hash |
| Do not add a vendor interface, bulk endpoints, audio interfaces, or feature reports. | **CONFIRMED** | Direct descriptor tree |

### 17.2 Input behavior

| Requirement | Classification | Basis / caveat |
|---|---|---|
| Support 64-byte enhanced input report `0x30`. | **CONFIRMED** | Direct traffic |
| Implement native bytes 1–12 as timer, status, three native button bytes, zero sticks, and zero vibrator status. | **STRONGLY SUPPORTED** | Layout/zeros are direct; timer progression semantics come from Linux/SDL |
| Emit physical A/B/X/Y, L/R, ZL/ZR, Select→Minus, Start→Plus, and D-pad at the exact bits in §5.3, active-high. | **STRONGLY SUPPORTED** | Four independent device-specific decoders; no direct transition trace |
| Keep Home, Capture, SL/SR, and stick-click bits clear for ordinary physical input. | **STRONGLY SUPPORTED** | No such physical controls; Linux SNES map omits them |
| Keep stick fields and IMU block zero. | **STRONGLY SUPPORTED** | Direct neutral stream and source capability gates |
| Implement report `0x21` replies with the shared input core, ACK byte, echoed subcommand, and response data. | **CONFIRMED** | Directly captured for the observed commands |
| Provide report `0x81` replies for supported USB commands. | **CONFIRMED** | Direct capture |
| Decide initial report behavior for a battery-removed cold boot. | **UNKNOWN / NEEDS CAPTURE** | Reconnect may retain state |
| Support simple report `0x3F` only if future Bluetooth/simple-mode scope requires it. | **STRONGLY SUPPORTED** | SDL/BlueRetro agree; exact SNES BT descriptor still needs capture |

### 17.3 Host commands and handshake

| Requirement | Classification | Basis / caveat |
|---|---|---|
| Accept USB commands `80 01`, `80 02`, `80 03`, and `80 04`; return `81 01/02/03` as observed, with `80 04` not requiring a reply. | **CONFIRMED** | Direct Steam capture |
| Return controller type `0x0B` in status and a valid controller address in Nintendo byte order. | **CONFIRMED** | Direct `81 01` response |
| Accept report `0x01` framing with sequence, two rumble slots, subcommand, and payload. | **CONFIRMED** | Direct capture |
| Accept subcommand `0x03` and select enhanced report `0x30`. | **STRONGLY SUPPORTED** | Linux/SDL cold init; absent only because captured unit was already in `0x30` |
| Accept SPI read subcommand `0x10` and correctly frame replies, including at least addresses `0x8010`/length `0x16` and `0x603D`/length `0x12`. | **CONFIRMED** | Direct command/framing capture; semantic content remains open |
| Accept subcommand `0x48` and acknowledge it without requiring a physical rumble effect. | **STRONGLY SUPPORTED** | ACK is direct; no-op actuator behavior comes from capability sources |
| Accept player LED subcommand `0x30`. | **CONFIRMED** | Direct command and ACK |
| Tolerate retries and duplicate handshake packets. | **STRONGLY SUPPORTED** | Retries/delayed duplicate replies in live trace and host-source behavior |
| Do not require HID feature reports or `SET_REPORT` for the captured Windows path. | **CONFIRMED** | Descriptor and trace |
| Determine exact device-info subcommand `0x02` contents and firmware/version policy. | **UNKNOWN / NEEDS CAPTURE** | Not present in direct USB trace |
| Determine whether Switch 1/2 sends additional SPI, pairing, shipment, IMU, or trigger commands to Lucia. | **UNKNOWN / NEEDS CAPTURE** | Requires a console capture; PC host is not console evidence |

### 17.4 LEDs, rumble, power, and identity

| Requirement | Classification | Basis / caveat |
|---|---|---|
| Implement four player LEDs controlled by subcommand `0x30`; at minimum reproduce mask `0x01` and ACK `80 30`. | **CONFIRMED** | Direct capture |
| Interpret low-nibble steady and high-nibble flashing masks. | **STRONGLY SUPPORTED** | Linux/SDL/common Switch protocol |
| Reproduce exact startup/unassigned visual pattern. | **UNKNOWN / NEEDS CAPTURE** | Requires visual correlation; not observed |
| Do not advertise or implement a Home-button light. | **STRONGLY SUPPORTED** | Linux/SDL capability gates and physical hardware |
| Do not generate haptic output; consume/ignore rumble frames safely and ACK enable-vibration. | **STRONGLY SUPPORTED** | No-rumble capability evidence plus a directly observed ACK |
| Report battery/charging/USB state in native byte 2 and preserve host-powered bit semantics. | **CONFIRMED** | Direct `0x91` plus Linux's exact decoder |
| Use type `0x0B`/Lucia as the Nintendo controller-type identity. | **CONFIRMED** | Direct response plus libnx/Linux/SDL |
| Avoid treating `bcdDevice=0x0210` as the runtime firmware version. | **CONFIRMED** | USB descriptor semantics; no runtime firmware response was captured |

### 17.5 PC, Switch 1, and Switch 2 compatibility

| Requirement | Classification | Basis / caveat |
|---|---|---|
| Enumerate on Windows as one HID joystick and be discoverable by Nintendo Classic HIDAPI using exact VID/PID/name/type. | **CONFIRMED** | Direct Windows/Steam evidence |
| Preserve button-label semantics and expose ZL/ZR as trigger-class controls through SDL/Steam normalization. | **STRONGLY SUPPORTED** | Live mapping and SDL source |
| Preserve Select/Start as Minus/Plus and do not synthesize Home/Capture at the USB wire layer. | **STRONGLY SUPPORTED** | Native maps and official app semantics |
| Be recognized by Horizon as Lucia/SNES, including the distinct controller UI style. | **STRONGLY SUPPORTED** | libnx/Switchbrew type/style data; no direct console screenshot |
| Support Switch 1 and Switch 2 USB initialization exactly enough for enumeration and gameplay. | **UNKNOWN / NEEDS CAPTURE** | Official compatibility is documented; console USB trace is absent |
| Respect Switch 2 restriction that original Switch controllers cannot wake the console via HOME. | **STRONGLY SUPPORTED** | Official Nintendo compatibility page; controller lacks physical HOME |
| Treat ZL→Capture, ZR→HOME, and ZL+ZR→suspend as SNES-app semantics, not alternate wire bits. | **STRONGLY SUPPORTED** | Official manual plus the native report format |

### 17.6 Bluetooth requirements

| Requirement | Classification | Basis / caveat |
|---|---|---|
| Treat this as legacy Bluetooth Classic HID/HIDP, not Switch 2 BLE. | **STRONGLY SUPPORTED** | Linux, BlueRetro, MissionControl |
| Use Bluetooth name `SNES Controller` and controller type `0x0B`. | **STRONGLY SUPPORTED** | Independent stacks |
| Implement exact SDP, CoD, HID descriptor, pairing/authentication, and wireless timing. | **UNKNOWN / NEEDS CAPTURE** | No authoritative raw wireless evidence located |
| Do not claim Bluetooth fidelity from the USB personality alone. | **CONFIRMED** | Protocol transports have unclosed differences |

## 18. Remaining unknowns and recommended future evidence

These gaps do not require a large manual button checklist:

1. **Battery-removed cold USB enumeration and first-mode behavior.** Use a passive USB capture after a controlled true power-off state if one becomes practical; do not write persistent vendor state.
2. **Switch 1 and Switch 2 USB command traces.** Capture genuine controller enumeration and a short gameplay session on each console with the shared protocol lab. This is the largest gate for claiming console fidelity.
3. **Bluetooth SDP/HID and clean-pair capture.** Obtain exact CoD, SDP records, HID descriptor, security sequence, and report-mode transitions using non-destructive passive capture. Do not reset pairings merely for convenience.
4. **Firmware/device-info response.** Capture subcommand `0x02` or the relevant controller-info exchange from a normal host.
5. **SPI content semantics.** Determine why the SNES unit returns `FF` for the Steam-requested ranges and whether Switch hosts expect different ranges/content.
6. **LED visuals.** Record the four steady/flashing masks and unassigned animation with automated command/camera correlation if fidelity needs it.
7. **Direct transition matrix.** The source-based physical map is internally consistent and implementation-ready. If later hardware verification is desired, use an automated fixture or a narrowly instrumented implementation validation rather than asking for a long manual proof of known buttons.
8. **Lucia region variant.** libnx exposes J/E/U Lucia variants, but this pass did not capture the source field selecting one.
9. **Exact Switch UI rendering and application restrictions.** libnx and Nintendo documentation support distinct UI/application behavior; screenshots and console traces would convert it from strong/documented evidence to direct observation.

## 19. Readiness conclusion

For a future, separately authorized **USB-only SNES NSO personality**, the evidence is sufficient to proceed: identity, raw descriptors, endpoints, native report sizes, controller type, complete provisional physical mapping, PC initialization, command framing, player-LED command, battery bits, and absence of analog/motion/rumble capabilities are all confirmed or strongly supported.

It is **not** sufficient to declare the eventual personality console-complete without a genuine Switch 1/2 capture and hardware validation. It is also **not sufficient for a Bluetooth personality** because exact SDP, security, and wireless-mode evidence is missing. The appropriate next implementation pass can build the exact USB surface behind tests, then use the remaining captures as validation gates rather than redesign inputs from manual button trials.

## 20. External references

- [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research)
- [ndeadly/MissionControl](https://github.com/ndeadly/MissionControl)
- [Linux hid-nintendo](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c)
- [SDL Nintendo HIDAPI driver](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_switch.c)
- [SDL gamepad database](https://github.com/libsdl-org/SDL/blob/main/src/joystick/SDL_gamepad_db.h)
- [BlueRetro Switch-family parser](https://github.com/darthcloud/BlueRetro/blob/master/main/adapter/wireless/sw.c)
- [BetterJoy](https://github.com/Davidobot/BetterJoy)
- [joycond](https://github.com/DanielOgorchock/joycond)
- [libnx HID service types](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hid.h)
- [Switchbrew HID services](https://switchbrew.org/wiki/HID_services)
- [dekuNukem Nintendo Switch reverse engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering)
- [HIDAPI Windows report-descriptor implementation](https://github.com/libusb/hidapi/blob/master/windows/hid.c)
- [libusb Windows HID notes](https://github.com/libusb/libusb/wiki/Windows#Known_Restrictions)
- [Nintendo SNES Controller information sheet](https://www.nintendo.com/eu/media/downloads/support_1/other_19/SuperNintendoEntertainmentSystem_Information_Controller.pdf)
- [Nintendo SNES Controller product page](https://www.nintendo.com/en-ca/store/products/super-nintendo-entertainment-system-controller/)
- [Nintendo SNES Controller troubleshooting](https://www.nintendo.com/en-gb/Support/Nintendo-Switch/Super-NES-Controller-Does-Not-Respond-Correctly-1670147.html)
- [Nintendo Switch 2 accessory compatibility](https://www.nintendo.com/en-gb/Hardware/Nintendo-Switch-2/Compatibility-with-Nintendo-Switch-accessories-2786091.html)
