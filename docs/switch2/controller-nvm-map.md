# Switch 2 controller non-volatile memory map

Status: 🔵 Partial — consolidated reference; per-row confidence and source below.
Scope: the 2 MB SPI flash inside genuine Switch 2 controllers (Pro Controller 2, Joy-Con 2 L/R,
NSO GameCube). PicoSwitch2 reads this material as reference evidence; it never writes it.

This consolidates two independent research efforts that had each mapped part of the device:

- **This project**, from full 2 MB dumps of genuine hardware in `dumps/SPI/` —
  [`../experiments/spi-dump-analysis-2026-07-10.md`](../experiments/spi-dump-analysis-2026-07-10.md),
  [`../experiments/nso-gc-spi-dump-analysis-2026-07-13.md`](../experiments/nso-gc-spi-dump-analysis-2026-07-13.md),
  [`../experiments/joycon2-spi-dump-analysis-2026-07-14.md`](../experiments/joycon2-spi-dump-analysis-2026-07-14.md).
- **ndeadly**, `memory_layout.md`, mirrored in this repository at
  `nso-gc-refs/switch2_controller_research/memory_layout.md` and re-checked against upstream
  2026-07-29 (identical).

Where they overlap they agree. Each supplies regions the other lacked; the table records which, so
no claim silently gains authority it has not earned.

## Map

| Range | Size | Contents | Confidence | Source |
|---|---|---|---|---|
| `0x000000`–`0x010FFF` | `0x11000` | Initial firmware, encrypted, header magic `0xAA640001` + ASCII `" SYS"` | Confirmed | both |
| `0x011000`–`0x011FFF` | `0x1000` | Address of the failsafe bank to use (u32 LE) | Strong | ndeadly |
| `0x012000`–`0x012FFF` | `0x1000` | Failsafe selector magic `0xBEEF` (u16 LE); absent ⇒ default to bank #1 | Strong | ndeadly |
| `0x013000`–`0x014FFF` | `0x2000` | Factory data: serial, VID/PID, colours, stick calibration | Confirmed | both |
| `0x015000`–`0x074FFF` | `0x60000` | Failsafe firmware update bank #1 | Confirmed | both |
| `0x075000`–`0x0D4FFF` | `0x60000` | Failsafe firmware update bank #2; erased until a firmware update | Confirmed | both |
| `0x0D5000`–`0x174FFF` | `0xA0000` | Unknown, probably another encrypted firmware blob | Unknown | ndeadly |
| `0x175000`–`0x1F9FFF` | `0x85000` | DSP firmware, magic `DSPH`, string `MT3616A0 DSP`; erased until a firmware update | Confirmed | both |
| `0x1FA000`–`0x1FAFFF` | `0x1000` | Bluetooth bond table: count, then `count × 0x28` entries of host address + LTK | Confirmed | both |
| `0x1FB000`–`0x1FBFFF` | `0x1000` | **Per-unit battery discharge curve** | Confirmed | **this project** (listed "unknown" upstream) |
| `0x1FC000`–`0x1FCFFF` | `0x1000` | User calibration: motion `+0x00`, sticks `+0x40` / `+0x60` | Confirmed | both |
| `0x1FD000` | `0x4` | Shipment flag; `00 00 00 00` from the factory, cleared to `FF` on first console connection | Strong | ndeadly |
| `0x1FD010` | `0x4` | Set to `00 00 00 00` after a controller firmware update | Confirmed | ndeadly (verified here) |
| `0x1FE000`–`0x1FEFFF` | `0x1000` | Unknown; present only on some controllers | Unknown | ndeadly |
| `0x1FF000`–`0x1FFFFF` | `0x1000` | Unknown; `0xCAFE`-tagged variable-length blocks, only on some controllers | Unknown | ndeadly |

Uninitialised memory is `0xFF` throughout.

### Factory data detail (`0x13000`)

| Offset | Size | Contents |
|---|---|---|
| `0x13002` | `0x10` | Serial number, 14 ASCII + 2 NUL |
| `0x13012` | `0x2` | Vendor ID (u16 LE) |
| `0x13014` | `0x2` | Product ID (u16 LE) |
| `0x13019` | `0x3` | Body colour RGB |
| `0x1301C` | `0x3` | Button colour RGB |
| `0x1301F` | `0x3` | Highlight colour RGB |
| `0x13022` | `0x3` | Grip colour RGB |
| `0x130A8` | `0x9` | Primary analog stick calibration |
| `0x130E8` | `0x9` | Secondary analog stick calibration |

Factory *motion* calibration is not in the `0x1FC000` user region — that slot is unprogrammed on
every unit dumped here. See §3.7 of the 2026-07-10 analysis, which corrects an earlier claim in the
same document.

## Verification performed here (2026-07-29)

Against `dumps/SPI/2069_spi_dump_2026-07-10_1422.bin` (Pro Controller 2) and
`dumps/SPI/NSO_GC_SPI_DUMP_1.bin` (NSO GameCube). Both are exactly 2 MB.

**Firmware header — Confirmed.** Identical structure at all three firmware offsets on the
Pro Controller 2:

```text
0x000000  01 00 64 AA 20 53 59 53 C6 38 8F E7 ...   |..d. SYS.8..|
0x015000  01 00 64 AA 20 53 59 53 D5 22 BD 15 ...   |..d. SYS."..|
0x075000  01 00 64 AA 20 53 59 53 08 A7 FB 72 ...   |..d. SYS...r|
```

The banks are `0x60000` apart, matching this project's own earlier observation of dual-bank regions
"`0x60000` apart" in the Joy-Con 2 analysis — two independent routes to the same number.

**Identity — Confirmed.** `0x13012`/`0x13014` read `VID 0x057E / PID 0x2069` on the Pro Controller
2 and `VID 0x057E / PID 0x2073` on the NSO GameCube. These are exactly the identities PicoSwitch2
emulates, now corroborated from the controller's own factory flash rather than only from
enumeration.

**DSP — Confirmed.** `DSPH` at `0x175000` and the string `MT3616A0 DSP` are both present on the
Pro Controller 2 and absent on the NSO GameCube, reproducing the upstream description exactly.

**Firmware-update state explains every difference between our two units — Confirmed.** An earlier
draft of this document attributed the missing DSP blob on the GameCube controller to that
controller having no audio jack. That inference was wrong, and `0x1FD010` disproves it:

| Indicator | Pro Controller 2 | NSO GameCube |
|---|---|---|
| `0x1FD010` post-update flag | `00 00 00 00` (set) | `FF FF FF FF` (unset) |
| Failsafe bank #2 at `0x075000` | populated | erased |
| DSP blob at `0x175000` | `DSPH` present | erased |

All three differences follow from one fact: **the Pro Controller 2 unit had received a controller
firmware update and the NSO GameCube unit had not.** Upstream states that bank #2 and the DSP
section are "uninitialised on original factory firmware", so this is the predicted behaviour, and
the flag at `0x1FD010` independently confirms which state each unit is in. The GameCube controller
does lack audio, but nothing here demonstrates that — the same evidence is fully explained by
update state alone.

**Shipment flag — consistent.** `0x1FD000` reads `FF FF FF FF` on both, i.e. cleared, as expected
for units that have been paired to a console.

**Failsafe selector — not observed.** `0x011000` and `0x012000` read `FF` on both units. Upstream
says an absent magic means the primary bank at `0x15000` is used, so an erased selector is the
normal resting state and this is agreement, not a discrepancy. It stays **Strong** rather than
Confirmed because no unit here has had the magic set.

**`0xCAFE` blocks — not present.** No `0xCAFE` occurrences in `0x1FB000`, `0x1FE000`, or
`0x1FF000` on either unit; the latter two are fully erased. Upstream flags both regions as "only
present on some controllers", so absence is expected rather than contradictory.

**Bond table — Confirmed.** Populated on both (48 and 45 non-blank bytes, consistent with the
documented two entries of `0x28`). Contents deliberately not reproduced here: this region holds
host addresses and link keys. Upstream adds a detail this project had not recorded — the second
entry's host address is the first with its final byte decremented, and both entries share one LTK.

## A coincidence worth not misreading

PicoSwitch2's own persistent-settings region on a Pico W begins at flash offset `0x1FA000`, the
same offset as the controller's Bluetooth bond table. **These are unrelated.** Both devices have
2 MB of flash and both reserve persistent data ~24 KB from the end, which is enough to produce the
collision. Nothing about the controller's layout influenced ours. Our layout is verified by
`tools/verify_install_reset_marker.py`.

## What PicoSwitch2 uses this for

Read-only, and mostly indirectly:

- **Serial and identity** — [`serial-generation.md`](serial-generation.md) derives format-valid
  per-Pico serials from the factory layout rather than copying a real unit's.
- **Colours** — the RGB offsets inform the configurable colour feature.
- **Calibration** — read from a *connected* controller over the normal command surface, not from a
  flash image.
- **Firmware identity** — [`firmware-versioning.md`](firmware-versioning.md).

The firmware never reads or writes a genuine controller's flash, and there is no reason for it to
start. See the scope note in [`controller-safe-mode.md`](controller-safe-mode.md).

## Remaining unknowns

| Question | Status |
|---|---|
| What is `0x0D5000`–`0x174FFF`? | ⬜ `0xA0000` of unknown, probably encrypted, firmware |
| Is `0xBEEF` at `0x012000` real, and when is it set? | ⬜ Needs a unit with a pending failsafe update |
| What are `0x1FE000` / `0x1FF000`, and which controllers have them? | ⬜ Erased on every unit dumped here |
| Is the `" SYS"` header tail fixed or a truncated field? | ⬜ Only one value observed |
| Is the second bond entry really a "private secondary interface"? | ⬜ Upstream hypothesis, untested here |

None are blocking. All are cheap to answer if a suitable genuine unit is ever dumped.
