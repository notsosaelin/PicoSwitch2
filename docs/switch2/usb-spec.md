# Switch 2 Pro Controller — Exact USB Emulation Spec

Byte-exact reference for making PicoSwitch2 enumerate to a Switch 2 as a **wired-USB
Switch 2 Pro Controller** (VID `0x057E` / PID `0x2069`). Companion to the higher-level
[protocol-research.md](protocol-research.md).

**Source & confidence.** Descriptors, report layouts, and command definitions are transcribed
from [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research)
@ `master`. The **endpoint transport and the init handshake below were independently verified**
by parsing that repo's USB capture (`captures/usb/rumble-procon-gccon.pcapng`, a Great Scott
Gadgets link-layer capture; the Pro Controller 2 is device address 7) with tshark. High
confidence throughout; remaining unknowns are called out in [§11](#11-open-questions).

---

## 1. Device identity

Device descriptor (18 bytes), exact:
```
0x12, 0x01, 0x00,0x02, 0xEF, 0x02, 0x01, 0x40,
0x7E,0x05, 0x69,0x20, 0x00,0x02, 0x01, 0x02, 0x03, 0x01
```
- `bDeviceClass/SubClass/Protocol = EF/02/01` → **IAD multi-interface composite**.
- `idVendor 0x057E`, `idProduct 0x2069`.
- `bcdDevice = 0x0200`. ⚠️ ndeadly's doc *comment* says "4.00" but the actual bytes are
  `00 02` = 2.00. **Replicate the bytes (`00 02`)**, not the comment.
- Strings: iManufacturer=`Nintendo`, iProduct=`Switch 2 Pro Controller`, iSerial=`00`.

## 2. Configuration descriptor (composite)

Real device: `wTotalLength = 268 (0x010C)`, `bNumInterfaces = 5`, self-powered, 500 mA.

| IAD | Interface | Class | Endpoints | Purpose |
|---|---|---|---|---|
| #1 (IF0, count 1, class 03) | **IF0 HID** | 03/00/00 | `0x81` int IN, `0x01` int OUT (both 64B, bInterval 4) | Input reports + rumble |
| #2 (IF1, count 1, class FF) | **IF1 Vendor** | FF/00/00 | `0x02` bulk OUT, `0x82` bulk IN (both 64B) | **Command channel** (confirmed, §4) |
| #3 (IF2–4, count 3, class 01) | **IF2** Audio Control + **IF3** Audio Streaming OUT + **IF4** Audio Streaming IN | 01/xx | iso `0x03` OUT / `0x83` IN (192B, 48 kHz stereo 16-bit) | Headset audio |

**IF0 (HID) + HID descriptor + endpoints — exact bytes** (identical in both build options):
```
0x08,0x0B,0x00,0x01,0x03,0x00,0x00,0x00,            // IAD: IF0, class 0x03 (HID)
0x09,0x04,0x00,0x00,0x02,0x03,0x00,0x00,0x05,       // Interface 0, 2 EP, HID
0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x61,0x00,       // HID desc, bcdHID 1.11, report len 0x0061=97
0x07,0x05,0x81,0x03,0x40,0x00,0x04,                 // EP 0x81 Interrupt IN  64B, bInterval 4
0x07,0x05,0x01,0x03,0x40,0x00,0x04,                 // EP 0x01 Interrupt OUT 64B, bInterval 4
```
**IF1 (vendor bulk) — exact bytes:**
```
0x08,0x0B,0x01,0x01,0xFF,0x00,0x00,0x00,            // IAD: IF1, class 0xFF (vendor)
0x09,0x04,0x01,0x00,0x02,0xFF,0x00,0x00,0x06,       // Interface 1, 2 EP, vendor
0x07,0x05,0x02,0x02,0x40,0x00,0x00,                 // EP 0x02 Bulk OUT 64B
0x07,0x05,0x82,0x02,0x40,0x00,0x00,                 // EP 0x82 Bulk IN  64B
```
The full audio block (IF2–4) is in ndeadly's `descriptors.md` (Pro Controller 2 section).

### Two build options
- **Option A — faithful (5 interfaces, with audio):** emit the exact 268-byte config. Requires
  TinyUSB **isochronous audio** (`CFG_TUD_AUDIO`); heavier.
- **Option B — minimal (2 interfaces, no audio):** emit only IF0+IF1 with `wTotalLength =
  0x0050 (80)`, `bNumInterfaces = 2`. Mirrors the **Joy-Con 2** / **NSO GameCube Controller**
  descriptors (genuine Switch-2 controllers with no audio), so the console clearly accepts
  audio-less S2 controllers. **Start here.** Risk: the console *might* reject `PID 0x2069`
  specifically without audio; if so, fall back to Option A.

## 3. HID report descriptor (97 bytes, exact)
```
0x05,0x01, 0x09,0x05, 0xA1,0x01,
0x85,0x05, 0x05,0xFF, 0x09,0x01, 0x15,0x00, 0x26,0xFF,0x00, 0x95,0x3F, 0x75,0x08, 0x81,0x02,   // Report 0x05: 63B input
0x85,0x09, 0x09,0x01, 0x95,0x02, 0x81,0x02,                                                     // Report 0x09: 2B vendor
0x05,0x09, 0x19,0x01, 0x29,0x15, 0x25,0x01, 0x95,0x15, 0x75,0x01, 0x81,0x02,                    //   21 buttons (1 bit)
0x95,0x01, 0x75,0x03, 0x81,0x03,                                                                //   3-bit padding
0x05,0x01, 0x09,0x01, 0xA1,0x00, 0x09,0x30, 0x09,0x31, 0x09,0x33, 0x09,0x35,
0x26,0xFF,0x0F, 0x95,0x04, 0x75,0x0C, 0x81,0x02, 0xC0,                                           //   4x 12-bit axes (both sticks)
0x05,0xFF, 0x09,0x02, 0x26,0xFF,0x00, 0x95,0x34, 0x75,0x08, 0x81,0x02,                           //   52B vendor tail
0x85,0x02, 0x09,0x01, 0x95,0x3F, 0x91,0x02,                                                      // Report 0x02: 63B output
0xC0
```
Console uses **input report `0x09`** for the PC2 (report `0x05` is a common alternate). Output
report `0x02` carries rumble.

## 4. Endpoints & command transport — CONFIRMED

Verified from the USB capture (PC2 = device 7):

| Interface | Endpoints | Carries |
|---|---|---|
| **IF0 HID** | `0x81` int IN, `0x01` int OUT | Input reports (on `0x81`) + rumble output report `0x02` (on `0x01`) |
| **IF1 vendor bulk** | `0x02` bulk OUT, `0x82` bulk IN | **All commands + responses** |

Evidence: `03 91 00 0d …` (Init USB) and `03 91 00 0a …09 00 00 00` (Select report `0x09`) were
seen as `host → 7.2` (**endpoint 2**); rumble `02 5b84…` and input reports on endpoint 1. So:
**drive commands over the vendor-bulk endpoints; input/rumble over the HID endpoints.**

## 5. USB initialisation handshake — OBSERVED (device 7 = PC2)

Header (8 bytes): `[cmdID][dir][transport][subcmd][unk][len/ack][00][00]`, `dir=0x91` request /
`0x01` response, **`transport=0x00` for USB (all commands)**. **USB response rule (from the
capture):** echo `cmdID`, `dir=0x01`, `transport=0x00`, echo `subcmd`, then `00 f8 00 00` +
data. (The `10 78` ACK in ndeadly's `commands.md` is the *Bluetooth* form, transport `0x01`.)

Actual ordered sequence on EP2 after enumeration (request → response, data truncated):

| # | Cmd | Request | Response |
|---|---|---|---|
| 1 | Init USB | `03 91 00 0d 00 08 00 00 01 00 <6B hostaddr>` | `03 01 00 0d 00 f8 00 00 01 00 00 00` |
| 2 | First-init | `07 91 00 01 00 00 00 00` | `07 01 00 01 00 f8 00 00 00` |
| 3 | 0x16/01 | `16 91 00 01 00 00 00 00` | `16 01 00 01 00 f8 00 00` + 24×`00` |
| 4 | Pair: exch addr | `15 91 00 01 00 0e 00 00 00 02 <6B host1> <6B host2>` | `15 01 00 01 00 f8 00 00 01 04 01 <6B ctrl addr>` |
| 5 | Pair: confirm LTK | `15 91 00 02 00 11 00 00 00 <16B A2 challenge>` | `15 01 00 02 00 f8 00 00 01 <16B B2=AES128(LTK,A2)>` |
| 6 | Pair: finalise | `15 91 00 03 00 01 00 00 00` | `15 01 00 03 00 f8 00 00 01` |
| 7 | LED pattern | `09 91 00 07 00 08 00 00 <8B>` | `09 01 00 07 00 f8 00 00` |
| 8 | Set feature mask | `0c 91 00 02 00 04 00 00 27 00 00 00` | `0c 01 00 02 00 f8 00 00 00 00 00 00` |
| 9 | Memory reads | `02 91 00 04 00 08 00 00 <len> 7e 00 00 <addr LE>` | `02 01 00 04 00 f8 00 00 <len> 00 00 00 <addr> <data>` |
| 10 | Enable features | `0c 91 00 04 00 04 00 00 27 00 00 00` | `0c 01 00 04 00 f8 00 00` |
| 11 | Select report 09 | `03 91 00 0a 00 04 00 00 09 00 00 00` | `03 01 00 0a 00 f8 00 00` |
| 12 | Firmware info | `10 91 00 01 00 00 00 00` | `10 01 00 01 00 f8 00 00 01 01 05 02 0c 00 00 00 ff ff ff ff` (type `02`=Pro) |

Interleaved (order not strict): `03/0c`, `0a/02` vibration, `0c/06` configure, `11/01`, `11/03`,
`01/0c` NFC, `18/01`. Feature mask **`0x27`** = buttons(01)+sticks(02)+IMU(04)+rumble(20). After
step 11 the controller streams input report `0x09` on EP1 IN. **Implement a dispatcher keyed on
`cmd+subcmd`, reply to whatever arrives (order-independent), and only begin streaming input once
report `0x09` is selected + features enabled.**

### The 0x15 pairing exchange happens over USB
The console runs the full Bluetooth pairing handshake (`0x15/01→02→03`) **over the wired USB
link** to provision the controller for later wireless reconnect. Step 5 requires answering an
AES-128-ECB challenge:

```
B2 = AES128_ECB(LTK, A2),  LTK = A1 XOR B1,  B1 = 5CF6EE792CDF05E1BA2B6325C41A5F10  (public)
```
`A1` = host key from `0x15/04` (over USB the console already knows/derives it), `A2` = challenge
in step 5. **All 16-byte values are byte-reversed on the wire.** mbedtls (in the Pico SDK) does
AES-128-ECB, so this is cheap to implement. **Open question:** whether a wired-only controller
*must* answer correctly to stream input, or whether the console tolerates a bad/again reply —
resolve on hardware. **Implement it correctly to remove the risk.**

Feature-info encode (`0x0C/01` response tail):
```python
def encode_output(flags):
    out = bytearray(8)
    out[0] = 0x07 if flags & 0x01 else 0
    out[1] = 0x07 if flags & 0x02 else 0
    out[2] = 0x01 if flags & 0x04 else 0
    out[3] = 0x01 if flags & 0x80 else 0
    out[4] = 0x01 if flags & 0x10 else 0
    out[5] = 0x03 if flags & 0x20 else 0
    return out
```

## 6. Feature flags (`0x0C`)
`0x01` buttons · `0x02` sticks · `0x04` IMU · `0x10` mouse (JoyCon only) · `0x20` rumble ·
`0x80` magnetometer. Console mask observed = `0x27` (buttons+sticks+IMU+rumble).

## 7. Input report `0x09` (63 bytes) — streamed on `0x81`

| Off | Sz | Field | Emit |
|---|---|---|---|
| 0x0 | 1 | Counter | increment each report |
| 0x1 | 1 | Power | `[0]`ext power `[1]`charging `[2:5]`battery 0–9. USB full ≈ `0x25` |
| 0x2 | 3 | Buttons | bitmap below |
| 0x5 | 3 | Left stick | 12-bit X,Y packed |
| 0x8 | 3 | Right stick | 12-bit X,Y packed |
| 0xB | 1 | — | `0x30` (`0x38` once rumble/feature bit5 on) |
| 0xC | 1 | NFC state | `0x00` |
| 0xD | 1 | Headset state | `0x00` |
| 0xE | 1 | Motion length | `0x00` (start with IMU off) |
| 0xF | 40 | Motion data | zeros initially — report-`0x09` motion is an *unknown packed format*; defer IMU |
| 0x37 | 8 | Reserved | zeros |

**Buttons** (byte.bit):
- Byte0: `80` RStick `40` Plus `20` ZR `10` R `08` X `04` Y `02` A `01` B
- Byte1: `80` LStick `40` Minus `20` ZL `10` L `08` Up `04` Left `02` Right `01` Down
- Byte2: `10` C `08` GL `04` GR `02` Capture `01` Home

**12-bit stick packing** (two values `x`,`y`, 0–4095, center 2048; same nibble packing as
Switch 1):
```
b0 = x & 0xFF
b1 = (x >> 8) | ((y & 0x0F) << 4)
b2 = y >> 4
```

## 8. Output report `0x02` (rumble, received on HID `0x01`) — 42 bytes used
`00` report id · `0x1..0x10` HD rumble left LRA (16B) · `0x11..0x20` HD rumble right LRA (16B)
· 9B reserved. Real capture example (idle-ish sample): `02 5b8401101e0000…` repeated L/R. Packed
HD-rumble format — decode later; first pass: non-zero → forward a generic rumble to bluepad32.

## 9. Factory / calibration blob for `0x02/04` memory reads

Serve on memory-read requests (real captured example values — safe to hardcode; console reads
`0x13040`, `0x13060`, `0x13080`, `0x130A8`, `0x130E8`, … during init):

| Addr | Sz | Bytes |
|---|---|---|
| 0x13002 | 16 | serial, e.g. `48 45 4A 37 31 30 30 31 31 32 31 32 34 37 00 00` |
| 0x13012 | 2 | `7E 05` (VID) |
| 0x13014 | 2 | `69 20` (PID) |
| 0x13019 | 3 | body colour `23 23 23` |
| 0x1301C | 3 | button colour `A0 A0 A0` |
| 0x1301F | 3 | highlight `E6 E6 E6` |
| 0x13022 | 3 | grip `32 32 32` |
| 0x13080 | 40 | `01 AD D9 9A 55 56 65 A0 …` (read during init) |
| 0x130A8 | 9 | primary stick cal `B3 67 83 2E 66 5E 3A 06 5F` |
| 0x130E8 | 9 | secondary stick cal `2C 08 84 D1 65 63 2A 26 62` |
| 0x1FA000 | 1 | pairing count — return `00` (no bonds) for USB |

Memory outside these can return `0xFF` fill. Only `≥ 0x1F5000` is writable (`0x02/05`); reject
writes below with status `0x81`. Back this with a small static table, not real flash.

## 10. Mapping bluepad32 → report `0x09`
- Buttons: A/B/X/Y, L/R/ZL/ZR, Plus/Minus/Home/Capture, L3/R3, dpad → §7 bitmap.
- **C, GL, GR are not exposed by bluepad32** (existing project limitation) → leave 0.
- Sticks: bluepad32 axes → rescale to 0–4095, center 2048.
- IMU/rumble: later phases (motion format unknown; rumble packing TBD).

## 11. Open questions

- ✅ **RESOLVED — USB command transport:** vendor-bulk EP2 for commands, HID EP1 for input+rumble.
- ✅ **RESOLVED — response ACK bytes:** USB uses `00 f8` with `transport=0x00`.
- **Is the `0x15` AES pairing mandatory for wired input?** Implement it (public key, mbedtls) to
  be safe; a later test can tell whether it's skippable.
- **Audio interfaces required?** Try Option B (no audio) first; fall back to Option A.
- **Report `0x09` motion packing** (40B) — unknown; ship with IMU off first.
- Which memory reads are strictly mandatory to reach "connected" (serve them all to be safe).
- bcdDevice comment/byte discrepancy (use bytes `00 02`).

## 12. Recommended first milestone
Option-B descriptor (`PID 0x2069`, HID + vendor bulk, no audio) + a command dispatcher on EP2
that replays the §5 responses (**including a correct `0x15/02` AES reply**) + a static report
`0x09` on EP1 (neutral sticks `0x0800`, no buttons). **Success = the Switch 2 shows a connected
Pro Controller 2.** Then wire bluepad32 input → §7 packing, then IMU, then rumble.
