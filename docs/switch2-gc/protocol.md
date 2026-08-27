# NSO GameCube Controller — Protocol Reference

> Confidence key: **Confirmed** (hardware-observed by this project, or ≥2 independent
> hardware-derived sources agreeing) / **Strong** (one hardware-derived source, internally
> consistent, plus a corroborating analysis) / **Hypothesis** (documented but not independently
> reproduced) / **Unknown**.
>
> **Current status (2026-07-15): hardware-validated** for Switch 2 enumeration, input streaming,
> native controls, and rumble. This remains the byte-level evidence reference; current compatibility
> claims live in `STATUS.md`, and unresolved interpretations live in
> `docs/switch2-gc/open-questions.md`.

## Sources

| Source | Type | Retrieved | Commit/version |
|---|---|---|---|
| Genuine NSO GameCube Controller (live, USB) | Primary hardware | 2026-07-13 | N/A (physical unit, VID:PID `057E:2073`) |
| `dumps/NSO_GC_SPI_DUMP_1.bin` / `_2.bin` | Primary hardware (SPI flash dump) | Provided pre-session | See `docs/experiments/nso-gc-spi-dump-analysis-2026-07-13.md` |
| `ndeadly/switch2_controller_research` | Documentation + raw BLE/USB captures | 2026-07-13 | `d1c5a7f7ba298f83017fae84952a4e6d2ef8fc92` (branch `master`); optional local clone under ignored `nso-gc-refs/` |
| `SoulCalDan/Low-Latency-Gamecube-Controller-to-USB-Adapter-for-Switch-1-2-and-PC` | FPGA implementation source | 2026-07-13 | `dd8c9ecc690d5e6560ad9897584389e5fdbccc22` (branch `main`); optional local clone under ignored `nso-gc-refs/` |

**Critical framing, established this session**: SoulCalDan's repo does **not** implement the native
NSO GameCube Controller protocol. It implements the classic Wii U GameCube Adapter identity
(`057E:0337`, a completely different, older, simpler USB HID protocol — 37B/5B interrupt reports, no
BLE, no command channel, no SPI emulation) by reading a real physical GameCube controller over its
original 1998 SI-bus protocol and repackaging it. `grep`ing all its Verilog sources for `2073`/`2069`
returns zero matches. **ndeadly's repository is therefore the sole primary source for the actual
protocol this project needs to emulate.** SoulCalDan's repo is retained as secondary evidence only
for genuine GC-controller electrical/trigger behavior (see "Trigger and stick electrical behavior"
below), not for anything USB/BLE-protocol-shaped. The durable audit trail is
`docs/experiments/nso-gc-reference-repo-audit-2026-07-13.md`.

## USB identity — Confirmed (raw byte-exact capture, this session)

**Update, same session**: a raw, byte-exact USB descriptor capture was obtained via
`USBPcap --inject-descriptors` (elevated; replays descriptor-fetch transactions USBPcap's driver had
already cached from the device's original enumeration — no physical replug needed, fully
non-destructive, no driver changes to the controller itself). Preserved verbatim at
`docs/experiments/nso-gc-captures/genuine-controller-descriptors-2026-07-13.pcap`. This **promotes
essentially every field below from Strong to Confirmed** — real hardware bytes, not a documentation
transcription.

Raw device descriptor bytes (frame 2 of the capture, USB device address 123, 18 bytes, hex):
```
12 01 00 02 ef 02 01 40 7e 05 73 20 01 01 01 02 03 01
```

| Field | Value | Confidence |
|---|---|---|
| bcdUSB | `0x0200` | **Confirmed** (raw capture — not previously documented by either reference repo) |
| idVendor | `0x057E` | **Confirmed** (raw capture, and independently via live `pywinusb` query) |
| idProduct | `0x2073` | **Confirmed** (raw capture, and independently via live `pywinusb` query) |
| bcdDevice | `0x0101` | **Confirmed** (raw capture — matches ndeadly `descriptors.md:704-722`'s documented "2.01" exactly) |
| bDeviceClass/Sub/Proto | `0xEF`/`0x02`/`0x01` (composite, IAD) | **Confirmed** (raw capture, matches ndeadly) |
| bMaxPacketSize0 | 64 | **Confirmed** (raw capture, matches ndeadly) |
| iManufacturer / iProduct / iSerialNumber (string indices) | `1` / `2` / `3` | **Confirmed** (raw capture — index values only; the string *text* at these indices was not captured this session, see "Remaining evidence gaps") |
| bNumConfigurations | `1` | **Confirmed** (raw capture) |
| Manufacturer string | `"Nintendo"` | Confirmed (live `pywinusb` query, matches ndeadly `descriptors.md:810-814` exactly — string *text*, not just index) |
| Serial string | literal `"00"` | Confirmed (live `pywinusb` query, matches ndeadly `descriptors.md:816-817`). Good news for privacy: the standard USB serial string does not leak a real per-unit identifier (unlike the SPI flash's internal `HHW50001551810` string — see the SPI analysis doc). |
| Product string (device-level, index 2) | `"Nintendo GameCube Controller"` per ndeadly / Windows Device Manager | **Not yet captured byte-exact this session** — see open question below |
| Safe-mode PID | `0x2074` | Hypothesis (ndeadly `descriptors.md:1045-1063`, `safe_mode.md:12`) — not observed live |
| Safe-mode entry combo | `Z + START + SYNC` | Hypothesis (ndeadly `safe_mode.md:12`) |

**Open question, not yet reconciled**: live `pywinusb` query returned `"If_Hid"` for the *HID
interface's own* product-name field (a different string index than the device-level `iProduct=2`
captured above — most likely `iInterface=5` on interface 0, per the config descriptor below).
Windows Device Manager shows `"Nintendo GameCube Controller"` as the composite device's friendly
name (from `iProduct=2`, not yet captured byte-exact). These are plausibly two different, both-real
string descriptors (device-level product name vs. an HID-collection-specific interface label), not a
contradiction — but the interface string's actual text (`"If_Hid"`) is itself new, real evidence
worth recording: it suggests an internal/debug-style naming convention for this interface,
consistent with `iInterface=5`/`iInterface=6` both being populated (unusual — many devices leave
interface string indices at 0/unused).

## USB descriptor topology — Confirmed (raw byte-exact capture, this session)

Raw configuration descriptor bytes (frame 4 of the same capture, 80 bytes exactly matching
`wTotalLength`, hex):
```
09 02 50 00 02 01 04 c0 fa 08 0b 00 01 03 00 00 00 09 04 00 00 02 03 00 00 05
09 21 11 01 00 01 22 61 00 07 05 81 03 40 00 04 07 05 01 03 40 00 04 08 0b 01
01 ff 00 00 00 09 04 01 00 02 ff 00 00 06 07 05 02 02 40 00 00 07 05 82 02 40
00 00
```

Decoded, field-exact (all **Confirmed**, superseding the Strong-tier summary this section used to
carry):

- **Config descriptor**: `wTotalLength=0x0050` (80), `bNumInterfaces=2`, `bConfigurationValue=1`,
  `iConfiguration=4`, `bmAttributes=0xC0` (self-powered, **no** remote-wakeup — note: this is a real,
  previously-undocumented-by-either-repo fact; SoulCalDan's unrelated WiiU-adapter descriptor uses
  `0xE0`/remote-wakeup-enabled, so this is a genuine point of difference, not a copy error),
  `bMaxPower=0xFA` (500 mA).
- **IAD** (bytes 10-17): groups interface 0, class `0x03` (HID).
- **IF0 — HID class**: `bInterfaceNumber=0`, `bNumEndpoints=2`, `iInterface=5`. **HID descriptor**:
  `bcdHID=0x0111`, `bNumDescriptors=1`, report descriptor type `0x22`, `wDescriptorLength=0x0061`
  (**97 bytes, exactly matching ndeadly's documented figure — Confirmed**). EP `0x81` IN interrupt,
  `wMaxPacketSize=64`, `bInterval=4`. EP `0x01` OUT interrupt, same.
- **IAD** (bytes 62-69): groups interface 1, class `0xFF` (vendor).
- **IF1 — vendor class**: `bInterfaceNumber=1`, `bNumEndpoints=2`, `iInterface=6`. EP `0x02` OUT
  bulk, `wMaxPacketSize=64`, `bInterval=0`. EP `0x82` IN bulk, same.

Every byte of this 80-byte tree is now independently reproducible from the raw capture — this is the
single biggest evidence upgrade of this session, and Stage B (exact descriptor implementation) can
now proceed against **this table directly**, not against ndeadly's prose.

**Also independently confirmed via live `pywinusb` HID API query** (separate, non-destructive
method, same session): `usage_page=0x0001` (Generic Desktop), `usage=0x0005` (Game Pad),
`input_report_byte_length=64`, `output_report_byte_length=64`, `feature_report_byte_length=0`,
`number_link_collection_nodes=2` — all consistent with the raw capture above, a second independent
corroboration of IF0's shape via a completely different code path.

## HID report descriptor — Confirmed (raw byte-exact capture, 2026-07-13, this session)

Raw HID report descriptor bytes (97 bytes, `wDescriptorLength=0x61` from the config descriptor
above; captured via a live USBPcap replug on the genuine unit, device address 40, hub `USBPcap1`,
hex):
```
05 01 09 05 a1 01 85 05 05 ff 09 01 15 00 26 ff 00 95 3f 75
08 81 02 85 0a 09 01 95 02 81 02 05 09 19 01 29 15 25 01 95
15 75 01 81 02 95 01 75 03 81 03 05 01 09 01 a1 00 09 30 09
31 09 33 09 35 26 ff 0f 95 04 75 0c 81 02 c0 05 ff 09 02 26
ff 00 95 34 75 08 81 02 85 03 09 01 95 3f 91 02 c0
```
Archived capture: `docs/experiments/nso-gc-captures/genuine-controller-hid-report-descriptor-2026-07-13.pcap`.

Decoded field-exact (all **Confirmed**, superseding the Strong-tier ndeadly-only summary this
section used to carry). Two report IDs share one `Game Pad` application collection:

- **Report ID 5** (`0x05`): `Usage Page 0xFF00` (vendor), `Report Count 63, Report Size 8` — a
  63-byte flat vendor input array, common to all Switch 2 controller types.
- **Report ID 10** (`0x0A`): 2-byte vendor prefix (`Report Count 2, Report Size 8`), then a
  **standard HID Button usage page** block (`Usage Minimum 1`/`Usage Maximum 0x15` = 21 buttons, 1
  bit each) + 3 bits padding (`Input Const`), then a **Generic Desktop Pointer collection** with
  X/Y/Rx/Rz axes, each 12-bit (`Logical Maximum 0x0FFF` = 4095), then — still under report ID
  `0x0A`, no new Report ID tag intervenes — a trailing 52-byte vendor input block
  (`Usage Page 0xFF00`, `Report Count 0x34`, `Report Size 8`). Total report `0x0A` payload: 2 + 3 +
  6 + 52 = **63 bytes**, matching the live `pywinusb` query's `input_report_byte_length=64`
  (63 data bytes + 1 report-ID byte) exactly. Meaning: report `0x0A` is exposed as a genuine
  standard-gamepad HID collection (21 buttons + 4×12-bit axes) wrapped in vendor prefix/tail bytes,
  not only the proprietary byte layout below — any generic HID-aware host (not just the Switch OS)
  can read GC input through the standard gamepad usage page.
- **Output Report ID 3** (`0x03`): `Usage 0x01`, `Report Count 0x3F` (63), `Report Size 8` — **63
  bytes of output data**, not 41. This corrects a real gap in the existing byte table below (see
  "Output Report `0x03`" — ndeadly's table only accounts for 42 bytes total including the ID byte;
  the actual wire size is 64 bytes, matching `wMaxPacketSize=64` on EP `0x01` OUT).

## Input Report `0x05` — common framing, GC-specific tail — Strong layout, Confirmed as the format PC/Steam actually uses

**Update 2026-07-13, direct hardware evidence**: a live USBPcap capture of Steam initializing this
project's own Pico (GameCube personality, real hardware) proved Steam's `0x03/0x0A` "Select Input
Report" command requests report ID **`0x05`**, never `0x0A` — this is now the Confirmed, load-bearing
fact for anything targeting a PC/Steam host: report `0x0A` is real and implemented, but likely only
ever consumed by an actual Switch 2 console (Stage G, still untested). **Implemented**:
`switch_gc_encode_report05()` (`src/switch_gc/switch_gc_encode.c`) — same common bit layout as
`switch_pro2.c`'s own `ns2_build_report_05()` (A/B/X/Y, D-pad, Plus/Minus/Home/Capture/C, ZL and
**Z in the ZR slot**), plus the GC-specific analog-trigger tail at `0x3C`/`0x3D` below.

**GameCube `Z` IS the ZR control — corrected 2026-08-26.** Keep the physical legend and the
host-facing semantic apart for this button:

| | |
|---|---|
| Physical control / shell legend / Touch Gamepad legend | **`Z`** |
| Host-facing logical control, both reports | **ZR** |

Evidence: a real Switch 2's Test Input screen names this control "ZR" (the same screen whose
evidence fixed report `0x0A`'s nibble — see `mapping.md`, "the console displays as ZR"), and a
genuine NSO GameCube Controller's `Z` is recognized as ZR by Windows/Steam. Since report `0x05` is
the only report a PC host ever selects, the genuine unit can only be setting this format's ZR bit
(byte0 `0x80`).

An earlier version of this section claimed "Native GameCube Z and the independent L/R trigger
detents have no representable bit in this shared format and are simply omitted when streaming
`0x05`". **That was an assumption and it was wrong for Z**, and it was the entire reason this
personality's `Z` worked on a Switch 2 and did nothing at all on PC/Steam. It also left the format
lopsided — `ZL` was always emitted — which is the asymmetry `ns2_seam.c`'s generic GC-mode block
already had to work around. Do not reintroduce it.

It remains true for the **trigger detents**, which are a different control from `Z`: they have no
slot in this format and only report `0x0A` carries them. A PC sees trigger travel through the
continuous analog tail at `0x3C`/`0x3D` instead.

Both reports source `Z` from `gc_extra` alone, so the console and a PC can never disagree about
whether it is pressed; `tools/test_switch_gc_report.c` case 16 pins that parity.

| Offset | Size | Field |
|---|---|---|
| `0x0` | 4 | Counter (32-bit, increments per report) |
| `0x4` | 4 | Buttons bitfield (common layout across controller types) |
| `0x8` | 2 | Unknown |
| `0xA` | 3 | Left stick, packed 12-bit, uncalibrated |
| `0xD` | 3 | Right stick, packed 12-bit, uncalibrated |
| `0x10` | 8 | Mouse data (Joy-Con only; inapplicable to GC) |
| `0x18` | 1 | Always 0 |
| `0x19` | 6 | Magnetometer |
| `0x1F` | 2 | Battery voltage (mV) |
| `0x21` | 1 | Charging state/rate |
| `0x22` | 2 | Battery current(?) |
| `0x24` | 5 | Always `0x00` |
| `0x29` | 1 | Always `0x01` |
| `0x2A` | 0x12 | Motion data (timestamp, temp, accel xyz, gyro xyz) |
| **`0x3C`** | **1** | **Left analog trigger — GC-only, uncalibrated** |
| **`0x3D`** | **1** | **Right analog trigger — GC-only, uncalibrated** |
| `0x3E` | 1 | Reserved |

## Input Report `0x0A` — GC-specific — **Confirmed** (field-by-field live decode, this session)

**Update, same session**: decoding `ndeadly`'s own `captures/usb/rumble-procon-gccon.pcapng.gz` (a
real Cynthion USB analyzer capture of a genuine GC controller, a different physical unit from the one
this project has direct access to) found a neutral-state live report `0x0A` that **decodes correctly
against every field below with zero contradictions**, including two exact matches against specific
enumerated values (the feature-bit-5-dependent byte, and the motion-data-length byte matching one of
three documented possible values exactly). Full byte-level decode:
`docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`. This promotes the table below from Strong
to **Confirmed** for its structure/field boundaries. The button bitfield further below remains
Strong-only (no live button-press sample found yet — see that section).

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

Button bitfield (`hid_reports.md:213-219`, exact quote):

```
| Byte | 0x80        | 0x40  | 0x20 | 0x10 | 0x08 | 0x04 | 0x02    | 0x01 |
| 0    | Right Stick | Plus  | R    | Z    | X    | Y    | A       | B    |
| 1    | Left Stick  | Minus | L    | ZL   | Up   | Left | Right   | Down |
| 2    | -           | -     | -    | C    | -    | -    | Capture | Home |
```

**Interpretation — Strong, not Confirmed** (the live decode above confirms all *other* report `0x0A`
fields, but the one sample found was neutral-state — zero buttons pressed — so it confirms the buttons
*field's location* (offset `0x2`, 3 bytes, currently reading `00 00 00`) without pinning down individual
bit meanings): both `L` and `R` are digital press bits (byte 0 `0x20`=R, byte 1
`0x20`=L) that coexist with the *separate, continuous* analog trigger bytes at offsets `0xC`/`0xD` —
i.e., the genuine controller reports both a digital click AND continuous analog travel
simultaneously, not one or the other. A `Z` digital bit exists (byte 0, `0x10`). A `ZL` digital bit
exists (byte 1, `0x10`) — **Confirmed physically present, correction 2026-07-13**: the project owner
has directly confirmed ZL as a real physical control on genuine hardware. (An earlier version of this
document incorrectly stated ZL was physically absent and the bit was presumed always-0; that was
wrong and is superseded — do not reintroduce it.) **Live assertion of this specific bit is still not
directly observed** (would need either a live button-press capture or the USB init handshake sent to
this project's own unit, see "Remaining evidence gaps") — the bit's *existence and physical control*
are Confirmed by direct owner observation, its *live wire value when pressed* remains unconfirmed. A
`C` bit exists (byte 2, `0x10`) with unclear semantics (C-stick click vs. a dedicated GameChat-style
button — not established).
`Right Stick`/`Left Stick` bits (byte 0/1, `0x80`) are present in the shared bit layout but **per
NSO-GC.md's explicit physical correction, the genuine controller has no L3/R3 stick-click hardware**
— these bits are expected to always read 0 on real GC hardware; not yet directly observed to confirm.

## Output Report `0x03` — GC rumble — hardware-confirmed decoder

Report ID is literally `0x03` on the wire for USB (resolving ndeadly's "always `00` for Bluetooth
connections" caveat — that's BT-only framing behavior, USB sends the actual ID byte). The 4-byte
rumble-data field and **59-byte** reserved-zero padding (this project's own HID report descriptor
capture declares 63 data bytes total via `Report Count 0x3F`) are both **Confirmed** in
framing/length.

| Wire offset | Size | Meaning |
|---|---:|---|
| `0x00` | 1 | USB report ID `0x03` |
| `0x01` | 1 | Sequence/command byte; not amplitude |
| `0x02` | 1 | Candidate state position in one documented host form |
| `0x03` | 1 | Candidate state position in the genuine USB capture form |
| `0x04` | 1 | Opaque; zero in current samples |
| `0x05` | 59 | Reserved zero padding |

The genuine controller uses a binary ERM state (`0` OFF, `1` ON, `2` STOP), unlike Pro Controller
2's packed dual-LRA HD rumble. Two observed packet forms place that state in either candidate byte.
The firmware checks both, gives STOP precedence, accepts ON from either, and interprets two zeros
as OFF. A zero-length interrupt slot carries no new command.

Real-console testing confirms this decoder produces GameCube rumble without the previous
full-power hang. Each ON becomes a short bounded downstream pulse; repeated ON refreshes it, OFF
does not, and STOP ends it immediately. This preserves captured binary behavior without treating
either candidate byte as a continuous amplitude.

Why the state position differs remains unknown. Evidence, rejected amplitude models, and future
capture questions are in `docs/switch2-gc/open-questions.md` and
`docs/experiments/refuted-hypotheses.md`.

## GATT service map (BLE) — Strong (ndeadly `hid_reports.md:17-26`, `bluetooth_interface.md:110-154`)

| Report | GATT handle | UUID | Shared or GC-only |
|---|---|---|---|
| Input `0x05` | `0x000A` | `ab7de9be-89fe-49ad-828f-118f09df7fd2` | Shared (all Switch 2 controllers) |
| Input `0x0A` | `0x000E` | `8261cba1-9435-420c-84d6-f0c75a2c8e4d` | GC-only |
| Output `0x03` | `0x0012` | `3f8fb670-ab25-45bf-b540-38c72834d064` | GC-only |
| Command+Vibration (combined write) | `0x0016` | `af95885e-44b3-4a24-9cf0-483cc129469a` | GC-only |
| Command Response #2 (notify) | `0x001E` | `46f6ad29-cdaf-4569-a2fe-339020b94604` | GC-only |

Not directly relevant to a USB-only PicoSwitch2 personality, but the **command framing shown in the
BLE sequence below is directly informative for the equivalent USB vendor-bulk command channel**,
since ndeadly's `commands.md` documents the command/subcommand IDs as identical across both
transports (only the outer transport framing differs).

## GC-specific BLE init sequence — Strong (ndeadly `bluetooth_interface.md:481-511`, exact quote)

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

GC feature-flag value is `0x27`, vs `0x37` for Pro Controller 2's equivalent sequence documented
immediately adjacent in the same source file — consistent with GC lacking whatever feature bit
distinguishes them (Strong hypothesis: bit 4 = mouse data, per `hid_reports.md:43`; GC has no mouse
hardware, and `0x37 & ~0x10 = 0x27` arithmetically matches).

**Confirmed this session**: every SPI address this sequence reads from (`0x13000`, `0x13040`,
`0x13080`, `0x130C0`, `0x13100`, `0x13140`) lands on real structured data in the genuine controller's
own SPI dump, cross-validating both sources independently — see
`docs/experiments/nso-gc-spi-dump-analysis-2026-07-13.md`. Two addresses (`0x13060`, `0x1FC040`) read
as unwritten (`0xFF`) in the dump despite being in this documented read list — open discrepancy, not
resolved this pass.

## USB init command sequence — Confirmed for 0x0D/0x0A (2026-07-13, real USB bulk capture), Strong for 0x03

**Update 2026-07-13**: the byte sequences below were originally transcribed from ndeadly's
BLE-derived `commands.md`, with an explicit caveat that USB's "outer transport framing differs."
Re-analysis of this project's own already-obtained `rumble-procon-gccon.pcapng.gz` found real
traffic on the GC device's **bulk vendor interface** (endpoint 0x02 OUT/0x82 IN, IF1) that had not
previously been mined (only EP0 control requests had been). That traffic proves the USB framing is
**byte-identical** to the BLE-derived framing shown here, at least for the two commands that
actually matter for reaching streaming — see
`docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md` "Bulk vendor-interface (IF1) init
sequence" for the full frame-numbered evidence. **Implemented**: `switch_gc_vendor_dispatch()` in
`src/switch_gc/switch_gc.c`.

**Command `0x03` / Subcommand `0x03` — "Enable USB HID Reports"** (`commands.md:231, 259`): Request
`03 91 00 03 00 04 00 00 01 00 00 00` → Response `03 01 00 03 00 F8 00 00 01 00 00 00`. **Strong**
(still BLE-derived only) — this command was **never observed** in the real USB bulk capture
described above, across all 225 bulk OUT transactions in that session. Either it isn't required for
a PC host, or it's simply not part of the minimum path a real session takes. ACKed defensively in
firmware anyway (cheap, documented, matches switch_pro2.c's own handling of the same subcommand)
but not proven necessary.

**Command `0x03` / Subcommand `0x0D` — "Initialise USB"** (`commands.md:365-379`, exact quote):
> "Initialise the controller for USB. Required before the controller will send input reports over
> USB. Host address doesn't need to be valid."
- Request: `0x0` (1B) Unknown, `0x1` (1B) Unknown, `0x2` (6B) host Bluetooth address (byte-reversed).
- BLE-derived example: `03 91 00 0d 00 08 00 00` `01 00 31 7e c6 eb f1 48` → Response
  `03 01 00 0d 00 f8 00 00` `01 00 00 00`.
- **Confirmed** via real USB bulk capture, frame 370989 (request) / 371002 (response): `03 91 00 0d
  00 08 00 00 01 00 f3 b9 34 8c 81 78` → `03 01 00 0d 00 f8 00 00 01 00 00 00` — identical shape,
  differing only in the (expected-to-differ) host Bluetooth address bytes.

**Command `0x03` / Subcommand `0x0A` — "Select Input Report"** (`commands.md:334-347`, exact quote):
> "Selects one of the input report formats found in the HID report descriptor. Invalid report IDs
> are ignored. Defaults to 0x07|0x08|0x09|0x0A (depending on controller type) on power-up."
- Request: `0x0` (1B) Report ID, `0x1` (3B) unused.
- **Confirmed** via real USB bulk capture, frame 376898 (request) / 376909 (response): `03 91 00
  0a 00 04 00 00 0a 00 00 00` → `03 01 00 0a 00 f8 00 00` (8 bytes, no data tail — shorter than
  0x0D's response). This exactly matches what this document had previously only *speculatively
  constructed* by analogy — now directly observed, not inferred.
- **Update 2026-07-13, this project's own Pico, hardware round-trip**: a live USBPcap capture of
  Steam initializing the Pico (GameCube personality) shows Steam requesting report ID **`0x05`**,
  not `0x0A` — `03 91 00 0a 00 04 00 00 05 00 00 00` → `03 01 00 0a 00 f8 00 00` (same 8-byte
  response shape, only the requested-ID byte differs). This is genuine PC-host behavior, not a
  Pico bug — see "Input Report 0x05" above for what this means for implementation (both report
  IDs are now supported; `0x05` is what a PC/Steam host actually uses).

**Confirmed live, earlier session**: a 2-second passive HID listen on this project's own genuine
controller (no init commands sent) captured **zero** input reports — consistent with this
documented requirement (the controller doesn't stream until explicitly told to).

### EP0 vendor control requests — new architecture finding, this session

Decoding `rumble-procon-gccon.pcapng.gz` found that **the init handshake is not exclusively a bulk
(IF1) affair** — at least two vendor-class requests happen directly on the device's default control
endpoint (EP0) immediately after enumeration, before any bulk-interface traffic:

- `bRequest=3`, `wLength=64`: returns a 64-byte block that is **byte-for-byte the same structure**
  (and, cross-checked against this project's own SPI dump, the same *source address* — SPI `0x13000`)
  as ndeadly's documented BLE factory-read of that address. **Confirmed**: this is a factory/identity
  read, reachable over plain EP0 control transfers, no bulk interface or prior handshake required.
- `bRequest=2`, `wLength=16`: returns `01 01 02 00 00 00 0c 00 00 00 02 bb 5e ab a9 3c` — not yet
  matched to any documented meaning. **Unknown.**
- `bRequest=4`, `wValue=0x0276`, host-to-device (OUT), `wLength=0` — a third, previously
  undocumented EP0 vendor request, found 2026-07-13 while re-checking exact chronological order
  (it happens between `bRequest=2` and the first bulk command, at frame 370946). The genuine
  controller ACKs its status stage normally (not a STALL). Purpose still not explicitly identified,
  but **Confirmed required for real console recognition** (see update below) — not a prerequisite
  for a PC host, but part of the same three-request gate a real Switch 2 needs answered.

**Update 2026-07-13, second finding**: stalling `bRequest` 2/3/4 (as GC's EP0 handler did until
this point) is **exactly** why GC mode enumerated and streamed fine on Windows/Steam but was not
recognized by a real Switch 2 console — the identical gate `switch_pro2.c`'s
`ns2_vendor_control_xfer()` already documents solving for Pro Controller 2 (see that function's
own comment): the console issues these three EP0 vendor requests immediately after
`SET_CONFIGURATION` and refuses to proceed to the bulk command channel until they're answered,
while Windows/Steam never send them at all (confirmed by the same live Steam capture cited
above — Steam completed its entire bulk exchange with a Pico that was stalling all three).
**Implemented**: `switch_gc_vendor_control_xfer()` now answers all three, mirroring Pro2's
response shape exactly — 64-byte identity block for `bRequest=3` (`01 00`, 16-byte serial, VID,
PID, `01 04 01`, 0xFF fill — Confirmed layout from this project's own captured GC responses, serial
fictionalized per NSO-GC.md's exclusion rule), 16-byte info block for `bRequest=2` (Confirmed real
bytes, reused verbatim like Pro2's own equivalent field), and a bare ACK for `bRequest=4`.

Full detail and the exact cross-validation against the SPI dump:
`docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`. This is architecturally relevant for
Stage D: at least part of "init/factory behavior" may be implementable via `tud_vendor_control_xfer_cb`
(EP0), which this project's existing Pro2 personality **already uses** for its own WinUSB/identity
handshake (`switch_pro2.c:1063-1093`, per `docs/switch2-gc/usb-personality.md`'s architecture audit)
— a genuinely promising, evidence-backed reuse opportunity worth exploring at Stage D, not just a
new-from-scratch mechanism.

## Live firmware identity — Confirmed (2026-07-21)

The UART↔BLE bridge sent native command `0x10/0x01` to an updated genuine NSO GameCube controller
paired to PicoSwitch2. Its raw 12-byte reply was:

```
01 01 02 03 0C 00 00 00 FF FF FF FF
```

This confirms controller firmware `1.1.2`, type `0x03` (GameCube), Bluetooth `12.0.0`, and no DSP
firmware (`FF FF FF` plus pad). PicoSwitch2's EP0 surface already reported `1.1.2 / 12.x`, but its
command `0x10` reply incorrectly reported controller `1.1.5` and type `0x04`. Both surfaces now use
one shared identity builder and reproduce the genuine tuple. The Switch 2 Update Controllers check
already reported this personality up to date before the correction, so this closes an internal
protocol inconsistency rather than working around an active update prompt.

## Firmware-type identification field — Confirmed

> Subcommand `0x01` — Get Firmware Version Info, response offset `0x3` (1 byte): "Controller firmware
> type — `00` = JoyCon (L), `01` = JoyCon (R), `02` = Pro Controller, `03` = Gamecube"

The live reply above independently confirms that PicoSwitch2's GC personality must identify with
firmware-type byte `0x03`.

## SPI/factory-data memory map — Strong (ndeadly `memory_layout.md`), Confirmed region boundaries (this session's SPI dump cross-check)

Per ndeadly, the 2 MB SPI layout (firmware banks, factory data at `0x13000`-`0x14FFF` with
serial/VID/PID/color fields, calibration around `0x1FC000`, BT pairing info around `0x1FA000`)
applies identically regardless of controller type — no GC-specific offsets beyond the generic
"not all fields initialised for all controller types" caveat (`memory_layout.md:60`). No GC-specific
factory-data example dump is shown in ndeadly's docs (only a Pro-Controller-flavored example, serial
`HEJ7100112...`).

**This session's own SPI dump analysis independently confirmed the `0x13000`-region and
`0x1FA000`-region boundaries** (see the dedicated experiment doc) — real cross-validation, not a
restatement of ndeadly's claim. **Do not copy any bytes from this project's own SPI dump into
firmware** — the dump contains genuine per-unit identity (serial string) and pairing-shaped data;
see the SPI analysis doc's explicit exclusion table.

## Safe mode — Hypothesis (ndeadly `safe_mode.md`)

`Gamecube NSO Controller | 0x2074 | Z+START+SYNC` (`safe_mode.md:12`) is the only GC-specific line in
the safe-mode documentation; the safe-mode command table itself is generic across controller types
and explicitly hedged by ndeadly's own docs as "seemingly valid commands observed when fuzzing" —
i.e., Hypothesis-grade even in the source. **Per NSO-GC.md's explicit instruction, do not implement,
enter, or depend on safe mode** unless normal operation is later found to require it.

## Trigger and stick electrical behavior — secondary evidence only (SoulCalDan)

SoulCalDan's adapter reads a *real physical GC controller* over its original 1998 SI-bus protocol —
useful only for understanding genuine GC hardware's own trigger/stick electrical behavior, **not**
for the NSO controller's USB/BLE wire protocol (different device entirely, see "Critical framing"
above):

- Digital L/R "click" bits come from the real controller's own mechanical microswitches under each
  trigger (closes at near-full press), independent of the continuous analog potentiometer value —
  i.e., "digital detent" is genuine separate hardware, not software-synthesized from the analog
  value. (`GC_Read.v:36-46`, implementation evidence)
- Analog trigger values are auto-zeroed against a per-boot calibration baseline captured at connect
  time, not a hard-coded scale curve (`GC_Read.v:91-92`, implementation evidence).
- This offers no direct evidence about how the *NSO* controller's own analog trigger byte (report
  `0x0A` offset `0xC`/`0xD`, documented as "uncalibrated") behaves, since the NSO controller has an
  entirely different (unknown, not GC-SI-bus) internal sensing/calibration path.

## Remaining evidence gaps — explicit, not silently resolved

**Resolved this session** (previously listed here, now Confirmed — kept as a record of what changed):

- Raw device+configuration descriptor bytes — obtained via an elevated, non-destructive
  `USBPcap --inject-descriptors` capture (no replug, no driver changes), then independently
  re-confirmed byte-for-byte against a *second, different physical unit* by decoding
  `rumble-procon-gccon.pcapng.gz`. See "USB identity" and "USB descriptor topology" above.
- Report `0x0A`'s full field layout — Confirmed via a field-by-field live decode of a real neutral-state
  report in the same capture, zero contradictions, two exact-value matches. See "Input Report `0x0A`"
  above and `docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`.
- Report `0x03`'s framing (report ID, field length, padding) — Confirmed via 8 real rumble-test
  samples in the same capture. Byte-level *semantics* remain Hypothesis (see below).
- A new EP0 vendor factory-read mechanism (`bRequest=3`) — Confirmed and cross-validated against this
  project's own SPI dump (same source address, same record structure, different unit's serial).

Still open:

1. **The raw 97-byte HID Report Descriptor payload itself was not captured** as a standalone
   `GET_DESCRIPTOR(HID Report)` control transfer in either capture obtained so far. Lower priority than
   before, since the report's *effective* structure is now independently Confirmed via the live decode
   above — this gap now matters only for exact HID usage-page/collection encoding, not "does the input
   work this way." If needed: a live capture across an actual physical replug (needs the controller
   owner's hands), or continued searching of the existing `rumble-procon-gccon.pcapng.gz` capture.
2. **String descriptor text was not captured** for indices 2 (`iProduct`), 4 (`iConfiguration`), 5/6
   (the two `iInterface` strings, one independently confirmed as `"If_Hid"` via a different method).
3. **Report `0x0A`'s button bitfield has no live confirmed sample** — the neutral-state decode confirms
   the field's *location* (offset `0x2`, 3 bytes, reads `00 00 00` at rest) but not individual bit
   meanings. Two sampling passes of `rumble-procon-gccon.pcapng.gz` (different time windows) found no
   button presses in those windows; further searching that same file has diminishing returns — a live
   interactive capture on this project's own unit remains the more reliable path once available.
4. **Report `0x03`'s exact byte-level rumble encoding** — 8 real samples show a plausible but
   unconfirmed pattern (an apparent 2-byte mode selector plus a swept intensity byte); not enough data
   to commit to a specific bit-field meaning. See
   `docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md` for the full sample table and reasoning.
5. **EP0 vendor `bRequest=2`'s meaning is unidentified** — a new question raised by this session's
   decode, not a previously-known gap.
6. **The init command sequence has not been sent to this project's own genuine controller** — would
   require either explicit approval to bind a WinUSB-capable driver to the vendor bulk interface, or a
   live Switch 2 console actually driving it (physical hardware + user time, currently unavailable).
   Lower priority than before: EP0 vendor commands (item above) may not need this at all for at least
   the factory-read portion.
7. **SPI dump discrepancies** at `0x13060`/`0x1FC040` — see the dedicated SPI analysis doc.

## Follow-up

- Both cloned reference repos' full audit is permanently preserved at
  `docs/experiments/nso-gc-reference-repo-audit-2026-07-13.md` (done this session).
- `rumble-procon-gccon.pcapng.gz` has been decoded for its GameCube-controller traffic specifically;
  its Pro Controller 2 traffic (same file, frame 44 onward) is unanalyzed and could offer a rumble-encoding
  cross-check (see `docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md` "Follow-up").
- With the device/config descriptors and report `0x0A` layout both now Confirmed, Stage B and the
  descriptor-construction half of Stage C are evidence-ready to implement. Report `0x03`'s byte
  semantics and the button bitfield are the two remaining gaps most worth closing before Stage C/E are
  fully evidence-backed, but neither blocks Stage B from starting.
