# Joy-Con 2 — Protocol Reference

> Confidence key: **Confirmed** (hardware-observed by this project, or ≥2 independent
> hardware-derived sources agreeing) / **Strong** (one hardware-derived source, internally
> consistent, plus a corroborating analysis) / **Hypothesis** (documented but not independently
> reproduced) / **Unknown**.
>
> **Status: 🟢 Confirmed on real hardware 2026-07-14 — both Joy-Con 2 (L) and (R) enumerate, pair
> ("Paired" notification shown, matching Pro2/GameCube), and stream on a genuine Switch 2 console.**
> Two real bugs found and fixed before this passed: (1) the vendor bulk dispatcher was missing
> `case 0x11`/`0x18` (streaming-phase gap, same class GC hit); (2) the EP0 identity block's
> fictitious serial was 2 bytes short (9 digits instead of the Confirmed 11), shifting VID/PID off
> their real offsets and blocking "Paired" entirely — see `STATUS.md` for the full account of both.
> `src/switch_joycon2/`, `include/switch_joycon2.h`/`switch_joycon2_encode.h`. This document
> remains the evidence base implementation work cites against; do not write further personality
> code from memory of it without re-checking the citation. Button mapping is reported "off" on
> real hardware — active investigation, see `docs/switch2-joycon2/mapping.md`. Rumble byte
> semantics remain Hypothesis-tier (not yet independently confirmed for Joy-Con2).
>
> Originally written as pure Stage A research before the project owner obtained genuine Joy-Con 2
> L and R hardware (a USB adapter for direct connection, plus a full SPI flash dump of each),
> moving this personality from "complete blank slate, 60-70% templatable at best" (the prior status
> in `docs/switch2-gc/usb-personality.md`) to real, hardware-confirmed evidence, then to a first
> implementation the same day.

## Sources

| Source | Type | Retrieved | Notes |
|---|---|---|---|
| Genuine Joy-Con 2 Left + Right (SPI flash dump via USB adapter) | Primary hardware | 2026-07-14 | `dumps/SWITCH2_JOYCON_L_1.bin`, `SWITCH2_JOYCON_R_1.bin` — see `docs/experiments/joycon2-spi-dump-analysis-2026-07-14.md` |
| Real Linux kernel "HID: nintendo" driver | Primary hardware-adjacent (PC-facing driver source, not a console) | 2026-07-14 | Vicki Pfau, linux-input mailing list v11 patch series, `https://marc.info/?l=linux-input&w=2&r=1&s=hid+switch2&q=b` |
| This project's own Pro Controller 2 / GameCube Controller work | Primary hardware (cross-family comparison) | 2026-07-06 – 2026-07-13 | `docs/switch2-gc/protocol.md`, `docs/experiments/spi-dump-analysis-2026-07-10.md` |
| `ndeadly/switch2_controller_research` | Documentation + raw BLE/USB captures (real decrypted BLE traffic, nRF52840 sniffer + Ubertooth One) | 2026-07-14 | Optional ignored clone under `nso-gc-refs/`; its `descriptors.md`, `hid_reports.md`, `bluetooth_interface.md`, and `commands.md` contain Joy-Con 2 sections |

**Note on this pass**: the USB descriptor captures earlier in this document were taken independently
by this project (via live USBPcap, before checking whether ndeadly's repo already had them). They
turned out to match `descriptors.md`'s own Joy-Con 2 (L)/(R) sections byte-for-byte — good
independent cross-validation, but also a reminder to check already-available reference material
before re-capturing it. `hid_reports.md`, `bluetooth_interface.md`, and `commands.md` had no USB
equivalent already captured — genuinely new information below, not cross-validation.

## USB identity — Confirmed (two independent sources)

| Field | Value | Confidence |
|---|---|---|
| idVendor | `0x057E` (Nintendo) | **Confirmed** — matches every other Switch 2 controller this project has |
| idProduct (Left) | `0x2067` | **Confirmed** — kernel source (`USB_DEVICE_ID_NINTENDO_NS2_JOYCONL`) **and** the genuine unit's own SPI flash identity block (`0x13012`) agree exactly |
| idProduct (Right) | `0x2066` | **Confirmed** — same two-source agreement (`_JOYCONR`) |

This is a stronger evidence tier than Pro2/GC ever needed for their own PIDs at this stage, since
both of *those* were already independently known before this project started. Joy-Con 2's PID
assignment briefly had a real mix-up in this project's own docs (an older, ndeadly/BLE-sourced entry
had L/R reversed) — now resolved and doubly confirmed.

## USB descriptors — Confirmed, byte-exact (USBPcap `--inject-descriptors` capture, 2026-07-14)

**Update, same day**: both Joy-Cons were live-connected to the same machine this project's tooling
runs on, via the owner's USB adapter, and already enumerated in Windows (Device Manager: "Joy-Con 2
(L)"/"(R)", composite devices at `VID_057E&PID_2067` / `VID_057E&PID_2066`, each with a HID
sub-interface `MI_00` and a vendor sub-interface `MI_01`). USBPcap + `--inject-descriptors` replayed
the already-cached descriptor-fetch transactions from their original enumeration — same
non-destructive method used for GameCube's own Stage B capture, no replug needed. This **promotes
essentially every USB-descriptor field to Confirmed** for both sides. Raw capture preserved at
`docs/experiments/joycon2-captures/genuine-controller-descriptors-2026-07-14.pcap`.

**A third device shares Nintendo's VID on the same hub**: address 17, PID `0x2068`, `bDeviceClass
0x09` (standard USB **hub** class). **Confirmed (project owner): this is the official Joy-Con 2
Charging Grip**, which has its own USB-C port — not a third-party adapter. Electrically it's exactly
what its USB descriptor says: a plain USB hub, not a controller identity of its own. Functionally
notable per the project owner: **docking a Joy-Con 2 in the Charging Grip adds GL/GR as available
buttons** when used with a real Switch 2. Given the grip enumerates as a bare hub with no HID
interface of its own, GL/GR most likely arrive as extra contact-pin inputs read by the *Joy-Con's
own* firmware (through the grip's physical rail contacts), the same way Switch 1's Joy-Con Grip
SL/SR buttons work — not as a separate USB HID report from the grip itself. Not independently
confirmed from a capture (no HID traffic was captured with a Joy-Con actually docked in the grip),
but consistent with the USB topology observed and with known Switch 1 precedent.

### Device descriptor (18 bytes, identical shape, PID-only difference)

```
Joy-Con 2 L: 12 01 00 02 EF 02 01 40 7E 05 67 20 00 01 01 02 03 01
Joy-Con 2 R: 12 01 00 02 EF 02 01 40 7E 05 66 20 00 01 01 02 03 01
```

| Field | Value | Confidence |
|---|---|---|
| bcdUSB | `0x0200` | **Confirmed** |
| bDeviceClass/Sub/Proto | `0xEF`/`0x02`/`0x01` (Misc, IAD composite) | **Confirmed** — identical shape to Pro2/GC |
| bMaxPacketSize0 | 64 | **Confirmed** |
| idVendor / idProduct | `0x057E` / `0x2067` (L), `0x2066` (R) | **Confirmed** — now a *third* independent source agreeing (kernel driver + genuine unit's own SPI flash + live Windows enumeration) |
| bcdDevice | `0x0100` (1.00) real captured value; **implementation deliberately uses `0x0110` (1.10) instead** | Captured value **Confirmed**; deviation **Confirmed necessary** — see below |

**Update, 2026-07-14, first hardware test of the implementation**: using the real captured
`bcdDevice` (`0x0100`) verbatim caused exactly the Windows WinUSB driver-binding cache collision
Pro2 and GameCube each already hit and fixed (`docs/switch2-gc/usb-personality.md` "`bcdDevice`
WinUSB-cache collision") — enumerated under "Other devices" with Code 28 ("no compatible
drivers"), on the same PC that has genuine Joy-Con 2 L/R hardware's own real binding already
cached from this session's earlier captures. Fixed the same way both prior personalities were:
`switch_joycon2.c`'s device descriptors now use `0x0110` (1.10), plausible-looking but
deliberately distinct from the real `0x0100` so Windows' VID+PID+bcdDevice cache key can't
conflate the two. Not yet re-tested after this fix.
| iManufacturer / iProduct / iSerialNumber | `1` / `2` / `3` | **Confirmed** (index values; string *text* not captured this pass) |
| bNumConfigurations | `1` | **Confirmed** |

### Configuration descriptor (80 bytes — same total length as GameCube's, identical structural shape)

```
09 02 50 00 02 01 04 C0 FA
08 0B 00 01 03 00 00 00                          IAD: IF0, HID
09 04 00 00 02 03 00 00 05                       IF0: HID, 2 endpoints, iInterface=5
09 21 11 01 00 01 22 64 00                       HID desc: bcdHID 1.11, Report descriptor = 100 bytes
07 05 81 03 40 00 04                             EP 0x81 interrupt IN,  64B, bInterval 4
07 05 01 03 40 00 04                             EP 0x01 interrupt OUT, 64B, bInterval 4
08 0B 01 01 FF 00 00 00                          IAD: IF1, vendor
09 04 01 00 02 FF 00 00 06                       IF1: vendor, 2 endpoints, iInterface=6
07 05 02 02 40 00 00                             EP 0x02 bulk OUT, 64B
07 05 82 02 40 00 00                             EP 0x82 bulk IN,  64B
```

Byte-for-byte identical between L and R (only the device descriptor's PID differs between sides).
**Structurally identical to the NSO GameCube Controller's own confirmed config descriptor**
(`docs/switch2-gc/protocol.md`) — same IAD-grouped two-interface shape (HID + vendor bulk), same
endpoint addresses, same endpoint types/sizes, same `bInterval 4` on the HID endpoints, same
`bmAttributes 0xC0` (self-powered, no remote wakeup), same `bMaxPower 0xFA` (500 mA). The only
differences from GameCube's descriptor: `iConfiguration` reads the same index (4) but the HID Report
descriptor length is **100 bytes, not 97** — expected, since Joy-Con 2 has a different button/axis
count than GameCube (single stick, SL/SR buttons, no analog trigger pair). This is strong, direct
evidence that **GameCube is the right template to start from for Stage B**, not Pro2 (which uses a
larger, audio-bearing composite descriptor) — confirms the recommendation already in "Suggested next
steps" below, now with real bytes behind it rather than just structural reasoning.

### HID Report descriptor (100 bytes) — Confirmed, byte-exact (live capture across a physical replug, 2026-07-14)

**Update, same day.** A software disable/enable cycle didn't trigger a real USB bus reset (see
"Attempted and deliberately not pursued further" below — that was the original plan to avoid asking
for a physical replug), so the project owner did a real unplug/replug of Joy-Con 2 Left while a live
(non-injected) USBPcap capture ran. This caught the *complete* enumeration sequence, including the
HID Report descriptor fetch and two string descriptor fetches. Raw capture preserved at
`docs/experiments/joycon2-captures/genuine-controller-full-enumeration-replug-2026-07-14.pcap`.

```
05 01 09 05 A1 01 85 05 05 FF 09 01 15 00 26 FF 00 95 3F 75 08 81 02
85 07 09 01 95 02 81 02 05 09 19 01 29 10 25 01 95 10 75 01 81 02
05 FF 09 01 26 FF 00 95 01 75 08 81 02
05 01 09 01 A1 00 09 30 09 31 26 FF 0F 95 02 75 0C 81 02 C0
05 FF 09 02 26 FF 00 95 37 75 08 81 02
85 01 09 01 95 3F 91 02 C0
```

100 bytes total (matches the config descriptor's own `wDescriptorLength` exactly). Decoded structure
— a HID application collection with **three report IDs**, all under a single `Game Pad` usage:

| Report ID | Direction | Size (bytes) | Shape |
|---|---|---|---|
| `5` | Input | 63 | One flat vendor-page (`0xFF`) blob, no sub-structure |
| `7` | Input | 63 | Structured: 2B vendor + 2B (16) buttons + 1B vendor + 3B stick (2×12-bit X/Y) + 55B vendor |
| `1` | Output | 63 | One flat vendor-page blob (rumble/LED — byte semantics not yet known) |

This mirrors Pro2/GameCube's own two-input-report pattern (a simple flat "PC/generic" report plus a
structured "extended" report) almost exactly — **except the report IDs themselves are different**:
Joy-Con 2 uses `5`/`7` for its two input reports and `1` for output, where Pro2 uses `5`/`0x09` (BLE)
or `5`/`0x0A` (USB) and GameCube uses `5`/`0x0A` with output on `3`. **Report ID 7 as the
console-facing extended report is new, previously-unknown information** — a future implementation
must not assume `0x0A` carries over from the other two personalities.

The structured report's shape independently **confirms the SPI factory-data finding**: exactly one
12-bit-packed stick (X/Y only, no Rx/Rz) and 16 buttons — consistent with "one physical stick, fewer
buttons than a full Pro Controller" from the calibration-slot analysis, now confirmed from the wire
format too, not just inferred from unprogrammed calibration bytes.

### String descriptors 5 and 6 — Confirmed

Same live capture also caught the interface strings (Windows didn't request `iManufacturer`/
`iProduct`/`iSerialNumber`, indices 1-3, at this point in enumeration — only the two `iInterface`
indices from the config descriptor):

- Index 5 (`IF0`, HID): `"If_Hid"` — **identical string to GameCube's own** `iInterface` text.
- Index 6 (`IF1`, vendor): `"Joy-Con 2 (L)"` — matches Windows Device Manager's own friendly name for
  this device exactly, and confirms the string is per-side (a Right-side capture would presumably
  read `"Joy-Con 2 (R)"`, not independently confirmed here).

### Attempted and deliberately not pursued further (superseded — see above)

Before the physical replug, a software disable/enable cycle (`Disable-PnpDevice`/`Enable-PnpDevice`)
was tried as a way to force re-enumeration without asking for a physical replug. It doesn't trigger a
real USB bus-level reset — the capture showed only the driver tearing down, no fresh
`GET_DESCRIPTOR` sequence. No lower-level reset tool (`devcon`, `Restart-PnpDevice`) was available on
the machine used. Noted for future reference: **a real physical replug was necessary** here; software
alone was not sufficient, unlike the injected-descriptor method used for the device/config
descriptors (which works precisely because it *doesn't* need a fresh enumeration).

## Live firmware identity — Confirmed (2026-07-21)

The out-of-band UART↔BLE bridge queried updated genuine Joy-Con 2 Left and Right controllers with
native command `0x10/0x01` while each was paired to PicoSwitch2. Raw 12-byte payloads:

```
Left:  02 01 04 00 0C 00 00 00 00 00 00 00
Right: 02 01 04 01 0C 00 00 00 00 00 00 00
```

Both decode as controller firmware `2.1.4`, Bluetooth `12.0.0`, and no DSP firmware (`0.0.0`), with
type `0x00` for Left and `0x01` for Right. The previous emulation had internally inconsistent stale
identities—EP0 `1.1.7`, command `0x10` controller `1.1.5`, type `0x07`, and `FF` DSP padding—and the
console offered an update. Both version surfaces now come from one side-aware builder. On
2026-07-21, the Switch 2's Settings → Update Controllers check reported both emulated Left and Right
personalities up to date, independently hardware-validating the corrected identities.

## Wire input/output report contents — Confirmed (`ndeadly/switch2_controller_research`)

This project's own captures established the HID Report *descriptor* shape (§ above): three report
IDs, sizes, and the raw field grouping. `ndeadly`'s `hid_reports.md` and `bluetooth_interface.md`
document the actual **field-level meaning** of every byte, cross-checked against real decrypted BLE
traffic (nRF52840 sniffer captures in the same repo, `captures/nrf52840/btle_joycon2_*.pcapng`).
Treated as Confirmed here, consistent with this project's existing practice of treating this
reference repo as reliable primary material (already used the same way for GameCube's protocol
work).

**Report ID 7 — Joy-Con 2 (L) structured input report** (63 bytes; BLE GATT notification uses the
same report ID number, unlike Pro2 which differs between its BLE (`0x09`) and USB (`0x0A`) reports):

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0x0` | 1 | Counter | Increments every report |
| `0x1` | 1 | Power info | Bit 0 external power, bit 1 charging, bits 2-5 battery level (0-9) |
| `0x2` | 2 | Buttons | See table below |
| `0x4` | 1 | Unknown | Observed always `0x07` |
| `0x5` | 3 | Analog stick | Packed 12-bit X/Y, uncalibrated |
| `0x8` | 1 | Unknown | — |
| `0x9` | 5 | Mouse data (relative) | Delta X/Y (16-bit each) + 1 unknown byte ("lift-off distance?"); active via a feature-enable bit |
| `0xE` | 1 | NFC state | **Always 0 — Left Joy-Con 2 has no NFC** |
| `0xF` | 1 | Motion data length | Observed `{0, 30, 40}` |
| `0x10` | 0x28 | Motion data | Active via a feature-enable bit; packed format not decoded by ndeadly either |
| `0x38` | 7 | Reserved | — |

Left button bitmap: byte 0 = `Stick`/`Minus`/`ZL`/`L`/`Up`/`Left`/`Right`/`Down` (MSB→LSB); byte 1 =
`SL`/`SR`/-/-/-/-/-/`Capture`.

**Report ID 8 — Joy-Con 2 (R) structured input report** — byte-identical layout, with two
differences: offset `0xE` (NFC state) is a real, live value on R (`0x00`-`0x07`, confirming **only
the Right Joy-Con 2 has NFC** — matches the `PN7160`/`PN7161` NFC controller datasheet present in
`ndeadly`'s repo, see "Datasheets" below), and the button bitmap: byte 0 = `Stick`/`Plus`/`ZR`/`R`/
`X`/`Y`/`A`/`B`; byte 1 = `SL`/`SR`/-/`C`/-/-/-/`Home`.

**Notable**: neither L nor R's individual button bitmap includes GL/GR bits. Pro Controller 2's own
combined report *does* have GL/GR (byte 2 of its 3-button-byte layout). Since a HID report
descriptor's shape is fixed and can't conditionally grow when docked, if GL/GR reach the host at all
from a Joy-Con 2 docked in the Charging Grip, they must reuse one of the existing `-` (reserved/
unused) bit positions above, rather than adding new ones — relevant to the open "GL/GR when docked"
question elsewhere in this document. Not confirmed either way; no capture exists of a docked Joy-Con
2's actual traffic.

**Report ID 5 — flat/simple input report** (63 bytes, both L and R): a single undifferentiated
vendor-page blob per this level of documentation — likely the same "PC/generic" role Pro2/GameCube's
own report `0x05` plays (this project's existing pattern of a simpler report for non-console hosts),
not decoded further by ndeadly either.

**Report ID 1 — output report** (rumble/LED, both L and R, both over USB and BLE — the *same* ID
number on both transports, again unlike Pro2/GameCube): BLE framing is Report ID (1 byte, always
`0x00` for BLE) + 16 bytes packed HD-rumble data for the LRA + 25 reserved bytes (42 bytes total on
BLE). The USB HID output report is 63 bytes total per this project's own captured descriptor — the
extra length beyond the 16-byte rumble payload is presumably command-channel piggyback space, the
same pattern Pro2/GameCube already use for their own output reports, not decoded further here.

### Datasheets (not yet analyzed in depth, noted for completeness)

`ndeadly`'s repo also includes real component datasheets: `ds-000451_icm-42670-p-datasheet.pdf`
(TDK InvenSense **ICM-42670-P**, a real 6-axis IMU part — the actual physical gyro/accelerometer
chip), `PN7160_PN7161.pdf` (NXP NFC controller — matches the Right-only NFC finding above),
`max98388-max98389.pdf` (audio amplifier — consistent with this project's own "Pro2-only DSPH blob
= headphone jack" conclusion), `bq25619.pdf` (battery charger IC), `C5129967.pdf` and
`joycon_connector_male.png` (connector/pinout reference). None of these have been read in depth this
pass — flagged as available reference material for whenever a specific question needs the real part
number's exact behavior (e.g. IMU full-scale range/sensitivity for cross-checking the factory
calibration floats already decoded from the SPI dump), not analyzed proactively without a concrete
question to answer first.

## Factory data block (`0x13000`) — Confirmed, direct SPI read

Full field-by-field results: `docs/experiments/joycon2-spi-dump-analysis-2026-07-14.md`. Summary
relevant to a future implementation:

- **Per-model hardware type code** at `0x13002`: `HB` (Left), `HC` (Right) — new, previously
  undocumented, distinct from the USB VID/PID. Confirmed present; no evidence yet that any host
  (console or PC) actually reads or cares about it — flagged as informational, not necessarily
  load-bearing for emulation.
- **Serial number** (`0x13004`–`0x1300F`, 12-char ASCII) — same shape as Pro2/GC's, per-unit,
  not reproduced in docs (redaction policy, see the analysis doc).
- **Appearance colors** (`0x13019`–`0x13024`) — body/button/grip remain genuine fixed values.
  Config v8 makes the highlight/accent at `0x1301F` independently configurable per side, defaulting
  to the genuine dumped colors `9B E1 E6` (Left) and `FF 8C 5F` (Right). The active side's accent
  also drives supported Sony RGB lightbars. The console reads these bytes during enumeration, so a
  saved UI change requires re-entering that Joy-Con personality; the physical lightbar updates live.
- **Stick calibration**: the classic Switch-family 9-byte packed format, **confirmed present in only
  one of the two calibration slots** the shared layout provides (`0x13080`/`0x130A8` populated,
  `0x130C0`/`0x130E8` entirely unprogrammed on both L and R). Directly reflects the real hardware:
  **a single Joy-Con has exactly one analog stick** — a future implementation should read/expose
  only the first slot, never assume the second exists.
- **Factory motion calibration** (`0x13040` temp+gyro bias, `0x13100` mag+accel bias, all float32):
  present, populated, physically plausible on both L and R. **The accelerometer bias lands on a
  different axis position than on Pro Controller 2** (Strong evidence — see the analysis doc §3.7)
  — a real implementation must NOT reuse Pro2's axis mapping verbatim for Joy-Con; the exact
  remapping (which axis, which sign flips) is not yet derived, only the fact that one is needed.
- **No DSP audio blob** (`0x175000`, Pro2-exclusive) — confirmed absent on both L and R, consistent
  with Joy-Con having no headphone jack.
- **Battery discharge curve** (`0x1FB000`): populated, byte-identical between L and R (same cell),
  close in shape to Pro2's own curve.
- **Bond table** (`0x1FA000`): populated on both, same host address recorded on L and R (both
  bonded to the same console) — real per-console data, not reproduced.

## What this does NOT tell us

The SPI dump is factory data + (mostly) compiled firmware this project cannot read the instructions
of (same "high entropy, no recoverable plaintext logic" limit as every prior SPI analysis here). It
says nothing about:

- The **wire input report format** — Pro2/GC stream report `0x09`/`0x0A` over USB and `0x09` over
  BLE; there is no equivalent confirmation for Joy-Con 2 yet, over either transport, from this SPI
  dump. `docs/switch2-gc/usb-personality.md`'s existing note that the kernel driver "uses bulk
  endpoints exclusively with no EP0 vendor control identity handshake" is the only lead so far, and
  it's about a generic PC driver, not a real console.
- Whether the **EP0 vendor identity handshake** (`bRequest` 0x02/0x03/0x04, required by a real
  Switch 2 *console* for both Pro2 and GameCube, invisible to generic PC hosts) is also required for
  Joy-Con 2. Strong reason by analogy to expect yes, not independently confirmed.
- ~~Whether connecting over USB even exposes a full HID-gamepad interface at all~~ — **Resolved,
  2026-07-14**: yes. Windows enumerates a genuine HID sub-interface (`MI_00`, `HIDClass`,
  "HID-compliant game controller") on both L and R, confirmed live via the capture above, alongside
  the vendor sub-interface (`MI_01`).

## Why not simultaneous L+R (settled architecture, 2026-07-14)

Joy-Con 2 Left and Right are implemented as two **separate, individually-selectable experimental
personalities** (`USB_PERSONALITY_JOYCON2_L` / `_R`) — never a merged identity, never a "pair"
option, and never a runtime side-toggle inside a single generic Joy-Con personality. This went
through three revisions before settling; the final reasoning, evidence-based rather than assumed:

1. **A genuine wired Joy-Con pair is not one combined identity.** Our own live USB capture of the
   real Charging Grip (see "USB descriptors" above) shows three independently-addressed devices: the
   grip itself (a plain, standards-compliant USB hub, `bDeviceClass 0x09`, zero vendor interfaces —
   confirmed from its raw descriptor, no controller-specific coordination logic of any kind) plus
   Joy-Con L and Joy-Con R as two genuinely separate child devices behind it. There is no
   alternate "combined" wire shape to emulate — the real hardware's answer to "how do two Joy-Cons
   share one cable" is a real hub with two real children, not a special descriptor.
2. **This project's single Pico can only hold one USB address at a time.** Confirmed at the
   register level, not a TinyUSB limitation: the RP2040/RP2350 SDK's own register definitions
   (`hardware/regs/usb.h`, identical on both chips) show exactly one `ADDR_ENDP.ADDRESS` field for
   the device-mode address, and TinyUSB's driver (`dcd_rp2040.c`) writes `SET_ADDRESS` requests
   directly into that single register — there is no second address to hold concurrently, and the
   SIE hardware matches incoming tokens against that one value before firmware could ever intervene.
   Emulating the grip's three-device topology from one Pico would need three concurrent addresses
   (hub + L + R), which this hardware architecture cannot represent.
3. **A from-scratch PIO-based USB stack is not a near-term option.** PIO bit-bangs the signaling in
   software rather than using the fixed hardware comparator, so it isn't flatly blocked the way the
   built-in peripheral is — but no existing driver (this project's toolchain's PIO-USB support is
   host-mode only) implements multi-address device-mode or hub-class behavior. This is genuine,
   unproven research with uncertain odds, not a documented small next step.
4. **No known working reference does this either.** `Dycool/NS-PC-Control` — a real, actively
   maintained project emulating Switch 2 controllers from a Raspberry Pi over Linux's USB Gadget
   subsystem (configfs/libcomposite), a substantially more capable and flexible USB stack than this
   project's bare-metal TinyUSB setup — explicitly states: *"S2 mode exposes one native controller
   on P1. Pro Controller 2 and individual Joy-Con 2 L/R are supported; L+R pair mode is rejected."*
   Even with a full Linux kernel and a much richer gadget framework, nobody has shipped simultaneous
   L+R. This corroborates points 2-3 rather than contradicting them.

**Conclusion**: Pro Controller 2 remains the recommended, default, production-quality personality
for using one paired controller as a complete Switch 2 controller. Joy-Con 2 Left and Right exist
as separate, clearly-labeled experimental/test personalities for hardware validation — selectable
one at a time via the ordinary BOOTSEL mode-cycle, with no config-UI side toggle and no pair option
anywhere in this project.

## Open questions

1. ~~Can a USB descriptor capture be taken from the same physical connection used for the SPI
   dump?~~ **Resolved, 2026-07-14** — yes: device, configuration, and **HID Report descriptors**,
   plus two string descriptors, all captured byte-exact for Joy-Con 2 Left (see "USB descriptors"
   and "HID Report descriptor" above). Still open: live traffic (actual input report contents,
   rumble/LED output byte semantics) and the R-side string descriptors — both need a live capture of
   real use, not just enumeration.
2. ~~Left vs Right as one personality or two?~~ **Resolved, 2026-07-14** — two separate,
   always-available personalities, no merged/pair mode. See "Why not simultaneous L+R" above for
   the full reasoning (this took three revisions to settle correctly).
3. **Single-stick input mapping** (when presenting as a single L-only or R-only identity) — this
   project's shared input pipeline (`switch_pro_input_t`, `ns2_seam.c`) currently assumes a
   dual-stick controller feeding a dual-stick output. A Joy-Con 2 personality needs a policy for what
   a source controller's *second* stick/trigger maps to (or whether it's simply dropped) — likely a
   smaller version of the same mapping question GameCube's `mapping.md` already had to answer for its
   analog triggers.
4. **GL/GR when docked in the Charging Grip** (project owner) — the grip adds GL/GR as available
   buttons, most likely via the Joy-Con's own rail-contact inputs rather than a separate USB HID
   report (see "USB descriptors" above). The captured HID Report descriptor's 16-button usage range
   is fixed regardless of dock state (a HID report descriptor can't conditionally change shape at
   runtime) — so if GL/GR exist in the wire format at all, they're almost certainly two of those same
   16 button bits, always present but only ever set when docked. Which bit positions, and whether
   they read zero when un-docked, needs a live input sample (docked vs. un-docked) to resolve.

## Suggested next steps (in priority order)

1. **Priority reset (project owner, 2026-07-14): Pro Controller 2 production-quality validation
   comes first.** Joy-Con 2 L/R are experimental/test personalities for hardware validation, not
   the recommended full-controller path — further Joy-Con 2 work (items below) is secondary to
   making Pro2 itself solid: enumeration, complete button/stick mapping, rumble, motion, reconnect
   behavior, wake behavior where feasible, and long-term stability.
2. Implementation is done (two separate personalities, no selector, no pair mode — see "Why not
   simultaneous L+R" above) and build-verified; the `bcdDevice` WinUSB-cache-collision fix
   (`0x0110`, matching Pro2/GameCube's own established pattern) is applied to both sides. **Not yet
   hardware-tested** — that's the next real Joy-Con2-specific step, whenever it's prioritized.
3. Remaining real unknowns, lowest priority, bundle into whatever hardware-testing pass eventually
   validates this rather than a dedicated capture session: GL/GR's actual bit position (if any)
   when docked in the Charging Grip, and the output report's rumble byte semantics beyond "16 bytes
   of packed LRA data" (same level of decode Pro2/GameCube's own rumble had before this project's
   kernel-source-driven correction — worth checking whether the same "not a linear
   amplitude byte" lesson applies here before assuming otherwise).
