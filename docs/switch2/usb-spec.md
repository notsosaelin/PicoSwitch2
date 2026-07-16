# Switch 2 Pro Controller — Exact USB Emulation Spec

> **⚠️ Historical bootstrap spec — the shipped code is now the source of truth.** This was the
> pre-implementation spec; the firmware in `src/switch_pro2/` has since shipped and *legitimately
> deviates* from parts of it. Where this doc and the code disagree, **the code + `STATUS.md` win.**
> Known drifts corrected inline below; do not "fix" working code back toward this doc. Current
> deviations of record: bcdDevice ships **0x0210** (not `00 02` — 0x0200 collides with a retail
> unit's WinUSB cache; see the branch memory); input maps via the **joypad-os seam**
> (`src/bt_hid/ns2_seam.c`), **not** bluepad32 (retired); **GL/GR/C are exposed** and confirmed
> on-console; report-0x09 **motion is decoded** (int32 phase + Q16.16 — see
> [report-0x09-motion.md](report-0x09-motion.md)); **IF0 HID `bInterval` ships `0x01` (1000 Hz)**,
> not the genuine `0x04` (250 Hz) — a deliberate latency-over-fidelity deviation that works on
> console hardware; its latency benefit has not been measured (see §13). The load-bearing,
> still-correct parts — §5 handshake, §7 packing,
> §9 factory/calibration blob — are what to rely on.

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
⚠️ **This repo deliberately ships `bInterval = 0x01` (1000 Hz), not the genuine `0x04` (250 Hz)
shown above** — see §13 "Poll-rate deviation" for rationale and hardware-test status. The bytes
above remain the genuine reference value; do not "fix" the shipped code back to `0x04`.
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
| 12 | Firmware info | `10 91 00 01 00 00 00 00` | Captured older retail unit: `10 01 00 01 00 f8 00 00 01 01 05 02 0c 00 00 00 ff ff ff ff` (FW 1.1.5, type `02`=Pro, BT 12.0.0, no DSP) |

Interleaved (order not strict): `03/0c`, `0a/02` vibration, `0c/06` configure, `11/01`, `11/03`,
`01/0c` NFC, `18/01`. Feature mask **`0x27`** = buttons(01)+sticks(02)+IMU(04)+rumble(20). After
step 11 the controller streams input report `0x09` on EP1 IN. **Implement a dispatcher keyed on
`cmd+subcmd`, reply to whatever arrives (order-independent), and only begin streaming input once
report `0x09` is selected + features enabled.**

### Firmware-version compatibility

The bundled USB capture above is a genuine but older Pro Controller 2 running controller firmware
`1.1.5`, Bluetooth patch `12.0.0`, with no DSP firmware reported. Current consoles try to update a
PicoSwitch2 that repeats those bytes. A later retail Pro Controller 2 capture cited as
`PC2_Gyro_*.pcapng` by NS-PC-Control reports:

```
02 00 11 02 0C 00 00 00 00 02 02 00
```

That decodes as controller firmware `2.0.17`, type `0x02` (Pro), Bluetooth patch `12.0.0`, and DSP
firmware `0.2.2`. PicoSwitch2 reports this later genuine version from both the EP0 firmware triplet
and command `0x10/0x01`. The reference capture is privately held, so this is **Strong Evidence**,
not independently byte-verified in this repository; real-console prompt suppression is the required
hardware validation.

The console's `0x0D` firmware-update protocol transfers an approximately 240 KiB controller image,
checks CRC32, switches failsafe banks, and reboots the controller. That image targets Nintendo's
controller MCU and cannot be installed as RP2040/RP2350 firmware. PicoSwitch2 therefore reports the
compatible retail version instead of pretending to accept or persist Nintendo firmware writes.

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
· 9B reserved. Real capture example (idle-ish sample): `02 5b8401101e0000…` repeated L/R. The
rumble is forwarded to the pad via the joypad-os per-vendor output-report path (shipped).

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

## 10. Mapping controller input → report `0x09`
Input arrives from the joypad-os bthid stack as `input_event_t` and is mapped in
`src/bt_hid/ns2_seam.c` (per-family remap) → `switch_pro_input_t` → §7 packing.
- Buttons: A/B/X/Y, L/R/ZL/ZR, Plus/Minus/Home/Capture, L3/R3, dpad → §7 bitmap.
- **C, GL, GR are exposed** by the per-vendor drivers (DualSense Edge paddles/Fn, Xbox Elite
  paddles → GL/GR/Capture/C via the config remap) and confirmed on-console.
- Sticks: driver axes (0–255) → 12-bit (0–4095), center 2048, Y inverted.
- IMU: report 0x05 shipped; report-0x09 int32 rewrite pending ([report-0x09-motion.md](report-0x09-motion.md)). Rumble: shipped (§8).

## 11. Open questions

- ✅ **RESOLVED — USB command transport:** vendor-bulk EP2 for commands, HID EP1 for input+rumble.
- ✅ **RESOLVED — response ACK bytes:** USB uses `00 f8` with `transport=0x00`.
- ✅ **RESOLVED — EP0 identity handshake:** after SET_CONFIGURATION the console reads 3 vendor
  control requests on EP0 (0x03 identity / 0x02 info / 0x04 ack) *before* the bulk channel — this was
  the real console-detection gate (see §5).
- ✅ **RESOLVED — `0x15` AES pairing is not required for wired input.** Implemented anyway; a
  well-formed but cryptographically-wrong `B2` is accepted (the wired session stores, not verifies).
- ✅ **RESOLVED — audio interfaces:** the full Option-A descriptor (HID + vendor + 3 audio) ships and
  the console accepts it (audio stubbed).
- ✅ **RESOLVED — report `0x09` motion packing:** decoded — int32 phase + Q16.16 accel, len 30, gated
  behind the `0x0C` enable. See [report-0x09-motion.md](report-0x09-motion.md).
- ✅ **RESOLVED — bcdDevice:** ship **0x0210**. `00 02`/`0x0200` and `0x0201` collide with a retail
  unit's Windows WinUSB cache; 0x0210 is console-neutral and keeps PC enumeration clean.
- 🔵 **Audio stub control requests (2026-07-12):** confirmed by reading TinyUSB's vendored
  `usbd.c` source (`process_control_request()`, `TUSB_REQ_RCPT_INTERFACE` case) that standard
  `SET_INTERFACE`/`GET_INTERFACE` on IF2-4 already get a mandatory core-level ACK fallback even
  though our audio stub's `control_xfer_cb` stalls everything — so alt-setting switches were never
  actually broken. The stub now also answers Feature-Unit Mute/Volume `GET_CUR` (UAC1 opcode
  `0x81`) with static unmuted/0dB values, purely to avoid control-transfer retry noise if a host's
  USB Audio class driver ever binds here — **not a functional-audio claim**; `SET_CUR`, `RANGE`,
  and actual iso streaming still stall/are unserviced.
- ✅ **RESOLVED — command framing (§14):** the whole EP2 handshake re-verified byte-exact against
  the raw capture (not the summary table, which turned out to omit some trailing bytes); three
  previously-undocumented subcommands confirmed real; one real, previously-unwired memory address
  (`0x13100`) found and fixed; one real unexplained byte and one real cross-source value conflict
  found and documented, neither fixed (insufficient evidence to act on either yet).
- **Open:** which memory reads are strictly mandatory to reach "connected" (we serve them all).

## 12. First milestone — ✅ achieved
The bootstrap target (the Switch 2 shows a connected Pro Controller 2) shipped, then live input,
rumble, and PC/Steam gyro on top. Historical path: an Option-B (no-audio) descriptor + the §5 EP2
dispatcher + a static report `0x09` first reached "connected"; the shipped build uses the full
Option-A descriptor and maps live input via the joypad-os seam (§10).

## 13. Poll-rate deviation (2026-07-12) — console-compatible, latency benefit unmeasured

**Change:** IF0's HID interrupt IN/OUT endpoints ship `bInterval = 0x01` (1 ms = 1000 Hz, the
fastest a Full-Speed interrupt endpoint can be polled), not the genuine `0x04` (250 Hz, §2). This
is a deliberate **latency-over-fidelity** deviation — the first one in this doc's "deviations of
record" that isn't forced by a Windows/console compatibility constraint (unlike bcdDevice). `bInterval`
has never been observed to be part of any console compatibility gate in this project's history
(the actual gate was the EP0 identity handshake and several post-pairing response mismatches, both
long since fixed — see §11/§5) — but nothing about a USB descriptor field has been safe to assume
without an actual hardware pass in this project, so this is recorded as **untested**, not fact,
until confirmed on real Switch 2 hardware.

**Side effect, handled:** `ns2_task()`'s streaming loop has no rate limiter of its own — it streams
a report every time the endpoint goes ready, so raising `bInterval` directly raises how often
`ns2_build_report()` runs. The report-0x09 motion tracker's bias/low-pass/jitter EMAs
(`switch_pro2.c`, `ns2_motion_tick()`) use fixed per-call weights, not per-elapsed-time scaling, so
their real-time time-constants are inversely proportional to call rate — running them at 1000 Hz
instead of 250 Hz would have silently compressed the (already first-cut, unvalidated — see
`STATUS.md` "Technical Debt") bias adaptation time from ~1s to ~250ms with nobody deciding that.
Fixed by gating both callers of `ns2_motion_tick()` (the streaming path and the config-mode debug
hook) through a shared `ns2_motion_tick_gated()` that caps the tracker's own call rate to ~250 Hz
regardless of USB poll rate — button/stick freshness gets the full benefit, the paused gyro
tracker's tuning is unaffected. Phase integration itself is already `dt_us`-scaled (real-time-based)
and needed no change.

**Not yet done:** the parallel Switch-1 `src/usb_descriptors.c` path (`main` branch, not
`ns2-testing`) has the identical `bInterval = 4` pattern and could get the same treatment later —
out of scope for this pass, which targets the active branch's actual target (the NS2/Switch 2
path) only.

## 14. Command-framing audit against the raw capture (2026-07-12) — ✅ done

**Why:** `PLAN.md`'s "Controller surface inventory" ranked "Initialization / command framing (USB
`0x03` family, unverified)" as Tier 1 — checkable directly against `switch_pro2.c` with zero new
hardware, since this repo already holds `ndeadly`'s raw capture reference (`captures/usb/
rumble-procon-gccon.pcapng.gz`, cited throughout this doc but not previously re-parsed at the raw
link-layer level from a live session — earlier passes worked from tshark-summarized markdown
tables). This pass re-cloned that capture, decompressed it (60 MB, Great Scott Gadgets USB-LL
format — `usbll`/`usb` in Wireshark, not USBPcap's transfer-level format), and used `tshark`'s
frame-reassembly (`-x` hex dump, correlated by USB address/endpoint 7.2) to extract every
EP2 vendor-bulk request/response pair **byte-exact from the wire**, superseding the earlier
markdown summary (which turned out to have silently truncated some trailing response bytes — see
finding 4 below). Method note for reuse: `usbll.device_addr`/`usbll.endp` filter fields only
populate on TOKEN packets (IN/OUT), not on the DATA packets that carry payload — filter by
`frame.number` range instead and correlate DATA0/DATA1 (`usbll.pid` `0xc3`/`0x4b`) payloads by
transaction order; `usb.data_len`/`usb.capdata` are unpopulated for this capture format (those are
USBPcap-specific), use `-x`'s "USB transfer (N bytes):" reassembly annotation instead.

**Finding 1 — three previously-undocumented subcommands confirmed real** (all bare-acked already,
matching this repo's existing default-ACK fallback — no functional bug, just newly-confirmed
evidence, closing three "Unknown" gaps):
- `cmd=0x03 sub=0x0C`: request carries 4 bytes `01 00 00 00`; response is a bare ack. Occurs
  *before* the previously-earliest-known `0x03/0x0D` ("Init USB") in the real sequence.
- `cmd=0x0A sub=0x02`: request carries 4 bytes `03 00 00 00`; response is a bare ack.
- `cmd=0x0A sub=0x08`: request carries 20 bytes (`01 ff ff ff ff ff ff ff 35 00 46 00 00 00 00 00
  00 00 00`); response is a bare ack. (`0x0A` is the documented "vibration" command family —
  this is host→device only, so no response content to verify beyond the ack shape.)

**Finding 2 — `0x0C/0x04` (enable features) response DOES carry 4 data bytes, resolving an open
question.** §5's summary table showed this response as exactly 8 bytes (`0c 01 00 04 00 f8 00 00`,
implying `dl=0`), which looked like a mismatch against `switch_pro2.c`'s `dl=4`. The raw capture
shows the true response is `0c 01 00 04 00 f8 00 00 00 00 00 00` — **12 bytes, 4 data bytes, all
zero** — matching this repo's existing `dl=4` implementation exactly. The table's apparent
8-byte response was an artifact of its own "(data truncated)" disclaimer, not a real 8-byte
response. **No code change; a suspected bug is ruled out**, and the table's ambiguity is now
resolved with primary evidence.

**Finding 3 — `0x0C/0x06` (configure features) has an unexplained extra byte, not reproduced,
not fixed.** Four real instances captured. All four match this repo's `memset(d,0,40); d[4]=
c[12];` implementation (zero-filled 40-byte response echoing the requested feature ID at offset 4)
— **except** the first instance, which has a nonzero byte `0x76` at offset 9 that the other three
(same feature ID, later in the same session) do not have. Too little data (4 points, one outlier)
to characterize what offset 9 means or whether it's meaningful protocol content vs. capture noise
on this specific 3rd-party hardware — **flagged, not fixed**. If revisited: check a second capture
session before changing the response shape.

**Finding 4 — `0x13100` (magnetometer + accelerometer bias, float32×6): real, confirmed by TWO
independent sources, but was never wired into `ns2_factory_init()` — fixed.** This address/layout
was already decoded 2026-07-10 from this repo's own SPI dump
(`docs/switch2/report-0x09-motion.md` "Factory motion calibration") but the actual 24-byte value
was never added to the served factory table — any real `0x02/04` read at `0x13100` was silently
answered with zero-fill instead. This pass's fresh capture decode (a **different** physical unit —
`ndeadly`'s) independently confirms the console really does read exactly this address (`len=0x18`)
during init and that it holds genuine non-zero, physically-plausible data there
(accelerometer Z ≈ 9.84 m/s² on that unit, vs. ≈ 10.38 m/s² on this repo's own dumped unit — both
near-gravity, confirming physical-unit floats independently on two separate controllers). **Fixed**
in `switch_pro2.c` using this repo's own SPI-dumped bytes (for internal consistency with the
surrounding factory-block entries, which are all sourced from the same dump where available).

**Finding 5 — `0x13060`: real value conflicted with what was hardcoded — ✅ fixed 2026-07-12
(same day, later pass) after a second independent source agreed.** This repo's factory table
hardcoded `fac(0x13060, {0x4C, 0x09, 0x00, 0x00}, 4)` with no source annotation. The raw capture
above showed a full 32-byte read spanning this address on `ndeadly`'s real unit returns **all
`0xFF`** (unprogrammed flash) — no trace of `4C 09 00 00` anywhere in that span. Left unchanged
at the time (one source, real regression risk, per-unit variance couldn't be ruled out). **A
second, independent source removed that ambiguity the same day**: auditing `Dycool/NS-PC-Control`
(a different project entirely) found its own factory table carries an explicit comment — "reads
back erased (0xFF) on the real unit... Captured read: addr=0x13060 len=0x20" — describing the
identical 32-byte all-`0xFF` span from *their* reference unit's capture. Two independent captures
of two different physical units, in two unrelated projects, now agree. Fixed: `switch_pro2.c`
explicitly fills this span with `0xFF`. Full audit:
`docs/experiments/ns-pc-control-audit-2026-07-12.md`.

**Finding 6 — memory-region reuse cross-validated.** The single `blk[40]` constant this repo reuses
for both `0x13080` and `0x130C0` reads is confirmed correct against the raw capture: both real
responses' first 40 bytes are byte-identical to each other and to this repo's `blk[40]`, on
`ndeadly`'s unit too. Only the trailing per-unit stick-calibration bytes (at `0x130A8`/`0x130E8`,
outside `blk`'s 40 bytes but within the same 64-byte reads) differ between units, as expected.

**Not re-examined this pass** (out of scope, already extensively hardware-validated in prior
sessions — see `docs/experiments/gyro-hardware-validation-2026-07-10.md` §9-14 and the branch
history): the `0x15` pairing sequence, the `0x11` opaque-value replays, report-0x09's own byte
layout. This audit targeted specifically the items `PLAN.md` flagged as "unverified."
