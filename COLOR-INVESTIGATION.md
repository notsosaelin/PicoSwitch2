# COLOR-INVESTIGATION.md

Investigation into what **controller colors** each emulated Switch 2 personality advertises to the
console, and whether the bytes we emit match genuine hardware. Prompted by the project owner's
question "does the controller advertise any particular color to the console?" (2026-07-16).

**Method:** byte-diff the color region of each personality's emulated identity/factory block
against genuine-unit SPI flash dumps in `dumps/`. Read-only; no code was changed by this
investigation.

> **Implementation outcome (v1.2.0):** this file preserves the pre-implementation investigation
> and therefore describes the old placeholders and proposed design in the present tense below.
> The resulting feature is now implemented: config v8 provides one Pro Controller 2 body color
> and independent Joy-Con 2 Left/Right accent colors; the genuine `#232323`, `#9BE1E6`, and
> `#FF8C5F` values are their defaults. Supported DualShock/DualSense lightbars follow the active
> personality color, while the console's `0x09` player assignment independently drives DualSense
> player dots. The owner hardware-confirmed the advertised colors, lightbar behavior, and live
> player reassignment on 2026-07-16. Xbox LED work remains deferred pending transport research.

---

## Background: how color is advertised

The Switch 2 console reads controller colors from the controller's SPI flash **factory block** at
`0x13000`, via a `0x02` SPI-memory-read (the same request that returns serial / VID / PID during
init). Four 3-byte RGB fields live at fixed offsets:

| Field | SPI offset | Meaning |
|---|---|---|
| Body | `0x13019` | main shell color |
| Button | `0x1301C` | face-button color |
| Highlight / accent | `0x1301F` | accent color (this is the *colored* one on Joy-Con) |
| Grip | `0x13022` | grip color |

This project does not have real 2 MB SPI flash; it serves a synthesized `factory[]` / identity
block from a table (`src/switch_pro2/switch_pro2.c`, `src/switch_joycon2/switch_joycon2.c`,
`src/switch_gc/switch_gc.c`). The console reads colors identically over USB (SPI read emulated at
EP0) and, for BLE, over the equivalent GATT path.

Because color lives in the factory block that the console reads only via the vendor/SPI path,
**colors never affect PC/Steam enumeration** — a PC never reads this block. This is purely about
how the console renders the controller in its pairing / registration / player UI.

---

## Evidence sources

| Personality | PID | Genuine dump(s) used |
|---|---|---|
| Pro Controller 2 | `0x2069` | `dumps/SPI/2069_spi_dump_2026-07-06_2107.bin`, `dumps/SPI/2069_spi_dump_2026-07-10_1422.bin` |
| Joy-Con 2 (L) | `0x2067` | `dumps/SWITCH2_JOYCON_L_1.bin` |
| Joy-Con 2 (R) | `0x2066` | `dumps/SWITCH2_JOYCON_R_1.bin` |
| NSO GameCube | `0x2073` | `dumps/SPI/NSO_GC_SPI_DUMP_1.bin`, `dumps/SPI/NSO_GC_SPI_DUMP_2.bin` |

All dumps are from genuine hardware. The two Pro2 dumps agree with each other byte-for-byte in this
region, as do the two GC dumps.

---

## Findings per personality

### Pro Controller 2 (`0x2069`) — ✅ FAITHFUL (Confirmed exact)

Genuine `0x13019`-`0x13024` (identical in both dumps):
```
0x13019: 23 23 23   body
0x1301C: A0 A0 A0   button
0x1301F: E6 E6 E6   highlight
0x13022: 32 32 32   grip
```
Our emission (`switch_pro2.c:253-256`): **byte-for-byte identical.**

| Field | Bytes | RGB | Appearance |
|---|---|---|---|
| Body | `23 23 23` | `#232323` | near-black charcoal |
| Button | `A0 A0 A0` | `#A0A0A0` | medium grey |
| Highlight | `E6 E6 E6` | `#E6E6E6` | off-white |
| Grip | `32 32 32` | `#323232` | dark grey |

This is the standard black Switch 2 Pro Controller colorway. **These are the genuine retail
colors, not placeholders** — a code comment near `switch_pro2.c:219` calls the block "built to
match a capture's structure," which undersells it; the values are confirmed-exact against two
independent genuine dumps.

Only the serial differs (genuine `HEW70001504982`, ours `HEJ71001121247`), which is **correct by
design** — a per-unit serial must be fictitious to avoid collision with real hardware.

### Joy-Con 2 (L) (`0x2067`) — 🔴 HIGHLIGHT COLOR WRONG (placeholder)

Genuine (`dumps/SWITCH2_JOYCON_L_1.bin`, `0x13000`-`0x1302F`):
```
00013010: 0000 7e05 6720 0108 0232 3232 aaaa aa9b
00013020: e1e6 3232 32ff ...
```
Decoded:
```
0x13019: 32 32 32   body
0x1301C: AA AA AA   button
0x1301F: 9B E1 E6   highlight  <-- light blue
0x13022: 32 32 32   grip
```
Our emission (`switch_joycon2.c:296`, EP0 identity block byte 25 onward, where EP0 byte N maps to
SPI `0x13000 + N`):
```
body      32 32 32   ✓ match
button    AA AA AA   ✓ match
highlight 00 00 00   ✗ genuine is 9B E1 E6 (light blue)   <-- comment literally says "neutral placeholder"
grip      32 32 32   ✓ match
```

**We advertise the L Joy-Con's accent as black (`00 00 00`) instead of its genuine light-blue
`#9BE1E6`.** Body/button/grip are correct.

### Joy-Con 2 (R) (`0x2066`) — 🔴 HIGHLIGHT COLOR WRONG (placeholder AND structurally can't differ)

Genuine (`dumps/SWITCH2_JOYCON_R_1.bin`):
```
00013010: 0000 7e05 6620 0108 0232 3232 aaaa aaff
00013020: 8c5f 3232 32ff ...
```
Decoded:
```
0x13019: 32 32 32   body
0x1301C: AA AA AA   button
0x1301F: FF 8C 5F   highlight  <-- coral / orange
0x13022: 32 32 32   grip
```
Our emission: the R identity block is a `memcpy` of L (`switch_joycon2.c:306-314`) that patches
**only** the type code (bytes 2-3) and PID (bytes 20-21) — it does **not** touch the color region.
So R inherits L's placeholder `00 00 00` highlight.

**Two problems here:**
1. Highlight is the same `00 00 00` placeholder as L; genuine R is coral/orange `#FF8C5F`.
2. Even if L's highlight were corrected, R would inherit L's (blue) color via the `memcpy`, because
   R's derivation never patches the color bytes. A correct fix must set L and R highlights
   independently.

This is the classic Joy-Con L=blue / R=red accent scheme, and we currently render both as black.

### NSO GameCube (`0x2073`) — ✅ FAITHFUL (Confirmed: no color)

Genuine (both GC dumps, identical):
```
00013010: 0000 7e05 7320 0104 01ff ffff ffff ffff
00013020: ffff ffff ffff ffff ...
```
The color region (`0x13019` onward) is **all `0xFF` — unprogrammed / no color fields** on genuine
hardware. The GC controller's identity constant is `01 04 01` (vs Pro2's `01 06 01`), and unlike
Pro2 it carries **no color bytes at all.**

Our emission (`switch_gc.c:313-319`): `01 04 01` followed by `0xFF` fill to 64 bytes. **Byte-for-byte
faithful** — we correctly advertise "no color," matching the genuine unit.

---

## Summary

| Personality | Body | Button | Highlight | Grip | Verdict |
|---|---|---|---|---|---|
| Pro Con 2 `2069` | ✅ `23 23 23` | ✅ `A0 A0 A0` | ✅ `E6 E6 E6` | ✅ `32 32 32` | **Faithful (exact)** |
| Joy-Con 2 L `2067` | ✅ `32 32 32` | ✅ `AA AA AA` | 🔴 `00 00 00` vs genuine `9B E1 E6` | ✅ `32 32 32` | **Highlight wrong** |
| Joy-Con 2 R `2066` | ✅ `32 32 32` | ✅ `AA AA AA` | 🔴 `00 00 00` vs genuine `FF 8C 5F` | ✅ `32 32 32` | **Highlight wrong + can't differ** |
| NSO GameCube `2073` | ✅ none (`0xFF`) | ✅ none | ✅ none | ✅ none | **Faithful (no color)** |

**Direct answer to the original question:** yes, the controller advertises four RGB colors (body,
button, highlight, grip) via the SPI factory block at `0x13019`-`0x13024`. Our Pro Controller 2 and
GameCube emulations are byte-exact against genuine hardware. Our Joy-Con 2 emulation gets body,
button, and grip right but emits a placeholder black `00 00 00` for the **highlight/accent** color,
where genuine Joy-Con 2 uses light blue (L, `#9BE1E6`) and coral/orange (R, `#FF8C5F`).

---

## Customization capability (color is fully under our control)

**Confirmed (project owner, 2026-07-16):** because we synthesize the entire factory block, we
control every color the console reads, and **the console validates none of them.** It renders
whatever RGB we put at `0x13019`-`0x13024`.

Empirical proof: the Joy-Con 2 highlight placeholder `00 00 00` produces a **black accent on the
console's on-screen controller** — a colorway that **does not exist on any retail Joy-Con 2**
(every real one has a colored accent). The console accepted and displayed it without complaint.
This establishes two things for future work:

1. **Arbitrary color customization is possible.** Any 3-byte RGB per field (body / button /
   highlight / grip) will be rendered. A user-facing color-picker feeding these four fields is
   feasible with no protocol risk — it is the same bytes the console already reads, just different
   values.
2. **Colors need not correspond to real hardware.** Impossible/unofficial colorways (e.g. a black
   accent, or a body color no SKU ships in) render fine. So customization is not limited to the
   set of genuine retail colors; it is the full 24-bit RGB space per field.

**Design notes for a future customization feature (not implemented; documentation only):**
- The four fields are per-personality, at the same offsets in each personality's own block
  (`switch_pro2.c`, `switch_joycon2.c`; GameCube has no color fields — see its finding above, so a
  GC color picker would have nothing to write unless we deviate from genuine and *add* fields,
  which is untested).
- Values would need to reach `factory[]` / the identity constants before the console reads them
  (i.e. set at personality init / mode-entry, or persisted in config flash and applied on init).
- The `0x13016` type/format constant (`01 06 01` Pro2 / `01 08 02` JC2) precedes the color fields
  and may gate how many color fields the console expects — its semantics are **Hypothesis** (see
  Remaining unknowns). A customization feature should not change this constant without testing.

## Reading a connected Bluetooth controller's color (passthrough feasibility)

The other direction the project owner raised: can we **read** the color a connected Bluetooth
source controller advertises, and mirror it into what we present to the console? Investigated
read-only; **no such reading exists in the codebase today.** The `r,g,b` field in
`feedback.h` is **output-only** (host→controller: setting a DualSense lightbar or player LEDs), not
a readback of any controller's hardware color. No driver reads a source controller's advertised
color region.

Whether it is even *possible* depends entirely on the source controller — most do not advertise a
hardware color at all:

| Source controller | Advertises a readable hardware color? | How it would be read |
|---|---|---|
| Switch 1 Pro Controller / Joy-Con (BT Classic) | **Yes** | SPI flash read (subcmd `0x10`) at `0x6050`: body / button / left-grip / right-grip, 4×RGB. Well-documented Nintendo protocol. Not currently read by `switch_pro_bt.c`. |
| Switch 2 Pro / Joy-Con 2 (BLE) | **Yes** | The same `0x13019`-`0x13024` factory region this doc maps, over the BLE SPI/GATT path. Not currently read by `switch2_ble.c`. |
| DualSense / DualShock (Sony) | **No** | Lightbar is host-set output only; no hardware body color is exposed over BT. |
| Xbox / Xbox Elite | **No** | No color advertised in any descriptor or report. |
| Generic HID / 8BitDo / Stadia | **No** | Standard HID has no color concept. |

**Implications for a passthrough feature (not implemented; documentation only):**
- A "mirror the source controller's real color" feature is only meaningful for **Nintendo source
  controllers** (Switch 1 and Switch 2 families). For every other source there is nothing to read,
  so those would fall back to a config default or per-player assignment.
- Reading the source color requires adding a **read path** the drivers don't have: a Switch-1 SPI
  read at `0x6050` (`switch_pro_bt.c`), and/or a Switch-2 factory-region read (`switch2_ble.c`).
  Both are additive and independent of the emit-side findings above.
- End-to-end, passthrough = (read source color) → (store) → (write into the emitted factory block
  before the console reads it). The **write** half is exactly the customization plumbing above; the
  **read** half is the new work. So customization is the natural first step, with passthrough as a
  source-populated variant of it.

**Confidence:** the "which controllers advertise a color" table is **Strong** — the Nintendo SPI
color regions (`0x6050` for Switch 1, `0x13019` for Switch 2) are documented/confirmed, and the
"no color" entries follow from those controllers having no such field in any known descriptor or
report. Whether reading `0x6050` over BT Classic works reliably through this project's BTstack
path is **Unknown** — never attempted here.

## Design idea: unify lightbar color with the emitted controller color

Raised by the project owner (2026-07-16): "link the lightbar color to the controller color, so a
red lightbar pairs as a red Pro Con." Documented here as a design note; **not implemented.**

**Why this is easier than the passthrough above, not harder:** the DualSense lightbar is an
**output this project already sets** (the Pico is the BT host; `ds5_send_output()` writes
`led_r/g/b`). The emitted Pro Con factory color is likewise something we write. Neither is read
from anywhere. So this is not "read the lightbar and mirror it" (the lightbar has no independent
value to read — it is whatever we last wrote); it is **one shared color value fanned out to two
outputs we already own:** the lightbar output *and* the emitted factory block. The relevant
infrastructure already exists: `PLAYER_COLORS[]` (`ds5_bt.c:22`) assigns a per-player RGB and
`ds5_send_output()` pushes it to the lightbar; the same value would additionally be written into
the emitted highlight/body fields at `0x13019`/`0x1301F`.

**The one caveat that shapes the UX — the two endpoints have different update timing:**
- The **lightbar is live**: settable at any moment during a session.
- The **on-screen controller color is sticky**: the console reads the factory block **once** during
  enumeration and does not re-read it. Changing `factory[]` afterward has no effect until the device
  re-enumerates (re-plug, or a personality mode-cycle).

Consequences for how a real feature would behave:
- **User-chosen color (config, set before pairing):** works fully both ways. The value is in the
  factory block when the console reads it, and is pushed to the lightbar too → red lightbar + red
  Pro Con on screen. This is the clean, recommended shape.
- **Color derived from the assigned player slot:** works for the **lightbar** (live) but **not** for
  the on-screen color — the console assigns the player slot *after* it has already read the factory
  color (chicken-and-egg). Matching the on-screen color to the player slot would require
  re-enumerating after the slot is known, which is disruptive. So player-slot→lightbar is fine;
  player-slot→on-screen-color is not, without a re-enumeration step.
- **Controllers with no lightbar** (Xbox, Switch Pro, generic): the emitted controller color still
  works, there is simply no physical light to match it to. So the "unified" experience is specific
  to lightbar controllers (DualSense / DualShock 4); everyone else just gets a settable on-screen
  color.

**Minor note:** `PLAYER_COLORS[]` values are deliberately dim (max component `0x40`, tuned for
lightbar aesthetics). Reused verbatim as the on-screen controller color they would render dark; a
real feature would likely want a brighter/full-range value for the factory block, i.e. decouple
"lightbar tint" from "controller color" even when driven by the same logical choice.

### Refinement: separate slot indication (LED row) from color (lightbar)

Raised by the project owner (2026-07-16), referencing
<https://github.com/daidr/dualsense-tester>: **unlink the lightbar from the player slot** — let the
lightbar be pure color customization (and feed the unified controller color), and show the actual
**player slot on the DualSense's dedicated player-indicator LED row** instead.

This maps directly to how DualSense hardware actually works, and **the project already drives both
LED systems independently** — so the slot half is not new work, it already exists:

| DualSense LED system | Output-report field | Current source | What it is |
|---|---|---|---|
| RGB lightbar (strips beside touchpad) | `led_r/g/b` (`ds5_send_output()`) | `PLAYER_COLORS[]`, per player | full 24-bit color |
| Player-indicator row (5 white LEDs below touchpad) | `player_leds` | `PLAYER_LED_PATTERNS[]`, per player | slot shown by *which dots are lit* (`0x04`/`0x0A`/`0x15`/`0x1B`/`0x1F` = P1–P5), color-independent |

**Confirmed by inspection (`ds5_bt.c`):** `player_leds` is already computed from the player index
and sent on every output update. So the dedicated slot indicator the reference points at is not
something to "tap into" — it is already live. The player slot is currently signaled **redundantly**
on *both* the lightbar color and the LED-dot row.

So this refinement is mostly an **un-wiring**, not new plumbing:
- **Keep** `player_leds` ← player index (the correct, color-independent slot indicator; already
  works, no change needed).
- **Repoint** the lightbar `led_r/g/b` from `PLAYER_COLORS[player]` to the **user's chosen color**,
  which is the same value written into the emitted factory block (the unified-color idea above).

Result: slot is shown by the white dot pattern (unchanged), and the lightbar color becomes free for
customization while staying visually consistent with the on-screen controller color. No conflict
between the two — slot is encoded by *pattern*, color by *hue*, on physically separate LEDs.

**Note on scope:** this is DualSense/DualShock-4-specific (they have both LED systems). Slot
indication on other controllers uses whatever that controller exposes (e.g. Switch Pro's 4 player
dots, Xbox has none), unchanged and unaffected by this. **Confidence: Confirmed** that both DS5 LED
systems are independently driver-controlled today; the refinement itself is **not implemented**
(documentation only).

## Console → dongle: does the console report our player position (1st–4th)?

Yes. The console assigns a player slot and transmits it to the controller via a **"Set Player
LEDs" command** — the same mechanism that lights the 1–4 player dots on genuine hardware. This is
the *input* half that the slot/color design above needs, so it is documented here alongside it.

### Switch 2 (`0x09` — Confirmed received, currently discarded)

The console sends command **`0x09` "Set Player LEDs"** during init. Genuine captured payload
(`docs/switch2-gc/protocol.md`, cross-referenced in `docs/switch2/usb-spec.md` and
`docs/switch2/nfc-protocol-inventory.md`):
```
09 91 01 07 00 08 00 00   01 00 00 00 00 00 00 00
└┬┘└──── header ────┘      └┬┘
 cmd 0x09                   player bitmask (byte index 8) = 0x01 -> Player 1
```
The byte at index 8 is the player bitmask, identical in convention to Switch 1's (see below).

**We receive it and throw it away.** All three USB personalities bare-ACK `0x09` without parsing
the payload:
- `src/switch_pro2/switch_pro2.c:685` — `case 0x09:  // player LEDs -> ACK only` (`dl = 0`)
- `src/switch_gc/switch_gc.c:546` — `case 0x09:  // player LEDs -> bare ack`

So the connected Bluetooth controller's player indicator currently reflects the **dongle's own
internal player index** (`PLAYER_COLORS`/`PLAYER_LED_PATTERNS`, assigned dongle-side), **not the
slot the Switch 2 actually assigned.** The real assignment is arriving in that `0x09` payload,
unparsed.

### Switch 1 reference (`0x30` — fully parsed in THIS repo's own code)

Switch 2's OS is the same base as Switch 1's, hardened. The Switch 1 mechanism is well-documented
(community reference: dekuNukem's *Nintendo_Switch_Reverse_Engineering*, subcommand `0x30` "Set
player lights") **and this project already implements the parse** in its Switch 1 build —
`src/switch_pro/switch_pro.c:250-263`:

```c
case SUBCMD_SET_PLAYER: {              // 0x30
    uint8_t bf = c->request[11];       // the player bitfield argument
    if      (bf == 0x01 || bf == 0x10) c->player_number = 1;
    else if (bf == 0x03 || bf == 0x30) c->player_number = 2;
    else if (bf == 0x07 || bf == 0x70) c->player_number = 3;
    else if (bf == 0x0F || bf == 0xF0) c->player_number = 4;
    ...
}
```

The bitfield convention (Confirmed on Switch 1, both from the community RE and this repo's own
working parse):

| Player | Steady (low nibble) | Flashing (high nibble) | Which LED dots |
|---|---|---|---|
| 1 | `0x01` | `0x10` | dot 1 |
| 2 | `0x03` | `0x30` | dots 1–2 |
| 3 | `0x07` | `0x70` | dots 1–3 |
| 4 | `0x0F` | `0xF0` | dots 1–4 |

Low nibble = LEDs lit steady, high nibble = LEDs flashing (flashing is used during
searching/registration). The Switch 2 `0x09` payload's `0x01` for Player 1 matches this convention
exactly, so the mapping is almost certainly identical — **Strong**, pending a multi-slot Switch 2
capture to confirm the 2/3/4 values byte-for-byte.

Note the Switch 1 parse *stores* `player_number` but does not forward it to a physical LED or the
BT controller (it is tracked, not acted on). It is nonetheless a complete, working decode we can
reference directly; the Switch 2 side does not even decode this far.

### "Change Grip/Order": is the slot a one-shot, or a live update?

This is the crux of the project owner's question. On **Switch 1**, "Controllers → Change
Grip/Order" is the registration screen (press L+R / SL+SR to register). While on it, connected
controllers show a cycling/flashing LED animation, and — critically — **when controllers are
reordered or (dis)connected, the console reassigns slots and re-sends `0x30` with the new
bitfield.** The player-light command is therefore a **live, re-sent indicator on Switch 1, not a
one-shot at init.** This project's own `SUBCMD_SET_PLAYER` handler would naturally track that: it
overwrites `player_number` on every `0x30` it receives, so a fresh command with a new bitfield
updates the stored slot immediately.

For **Switch 2**, whether the equivalent `0x09` is re-sent on reorder is **Unknown** — this
project's captures only show `0x09` during init, and no reorder/Change-Grip-Order sequence has been
captured. Given the shared OS base and the exact `0x30`→`0x09` correspondence, the **Strong prior**
is that Switch 2 behaves identically (re-sends `0x09` with an updated bitmask on reorder), but that
is a prediction, not a Confirmed observation.

### Experiment to determine it (not yet run; no code changes made)

**Key constraint (do not design around this incorrectly): CDC config mode is itself a USB
personality** (`usb_mode_cycle.c` — the cycle is Pro2 → GameCube → Joy-Con2 L → Joy-Con2 R →
CDC_CONFIG → back to Pro2), and only one personality is active at a time. **You cannot be
enumerated to the Switch 2 as a controller and be in CDC config mode at the same time** — config
mode re-enumerates the single USB port as a serial device to a *PC*, not as a controller to the
console. So there is no "config window open while paired to the console" state. Any observation
method must respect this.

**Ruled out — inline hardware USB analyzer.** A hardware sniffer (Great Scott Gadgets / Beagle)
inline between dongle and console would capture the console→dongle `0x09` directly and cleanly, with
no firmware change. **The project owner has ruled this out (2026-07-16): no hardware analyzer is
available to this project.** (Also note it could not be USBPcap — the tool behind this repo's
`usbpcaptures/*.pcapng` captures a *Windows host's* stack, but here the Switch 2 is the host, so
USBPcap would never see the `0x09`. A sniffer would have to be inline hardware.) Recorded here only
so it is not re-proposed; every method below is firmware-only or observation-only.

Firmware-only / observation-only methods, roughly cheapest-first:

**Method E — the genuine-controller LED oracle (project owner's idea, 2026-07-16; best zero-cost
method — do this first).**
Use a **real Switch 2 controller** (Pro Controller 2 or Joy-Con 2) as the readout. Its physical
player LEDs (the 1–4 dot indicators) can *only* be changed by the console sending it the "Set Player
LEDs" command — there is no other mechanism. So the physical LEDs are a **direct, definitive readout
of the command itself**, not a proxy.

**Owner-confirmed procedure and result (2026-07-16):** open Change Grip/Order → the controller goes
off → press **L and R** → it re-appears assigned as **player 1/2/3/4 with the LED changed to that
slot.** The LED changing on (re)assignment is the Set-Player-LEDs command taking effect —
**Confirmed: the console sends the slot command.** (One nuance still worth a glance: here the change
happens on the L+R *re-registration* within Change Grip/Order; confirming whether a pure drag-reorder
*without* re-registration also re-sends is a minor follow-up, but registration-with-new-slot already
proves the send.)

Why this beats Method C: Method C watches the console's *on-screen* controller list, which the
console could in principle update from its own internal state without notifying any controller. The
genuine controller's *physical LEDs* cannot change unless a command was actually sent on the wire —
so this proves transmission, where Method C only suggests it.

**What it does and does not settle:**
- **Settles (definitively):** whether the Switch 2 *sends* a player-slot command and whether it
  *re-sends on reorder* (the central Unknown). The owner reports "it changes with a real one," which
  — if that includes live change during Change Grip/Order — already answers the live-update question
  **Yes** for the console side. Worth confirming explicitly that the change happens *during* the
  reorder, not only after a re-pair.
- **Does not settle:** the exact bytes (you infer the slot by counting LEDs, not by reading the
  payload) — but those are already Confirmed/Strong from the Switch 1 parse and the `0x09` capture
  (`0x01`=P1, `0x03/0x07/0x0F`=P2/3/4).

**The "does it send to OUR emulated controller?" gap is now essentially closed — owner-confirmed
(2026-07-16).** The project owner reports having **used the reorder menu to change our dongle to a
different player position.** That matters because, in the Switch protocol, "assign this controller to
player N" *is* the Set-Player-LEDs command — there is no separate "set position" path distinct from
sending the LED command. So the console reassigning our dongle's position means **it sent our dongle
the slot command**, over USB (our transport), exactly as it does to a genuine BT controller. The
owner's note "even if ours is USB" is the right read: the console sends the slot command over
whatever link the controller uses, and our USB personality receives it — we just **discard it today**
(`switch_pro2.c:685`, bare-ACK). The only theoretical residue (that the console might reassign
internally *without* sending controllers it thinks lack LEDs) is very unlikely — the console has no
"has-LEDs" capability flag to key off, and sends the command uniformly to the device class. Logging
the actually-received `0x09` payloads (Method A) would upgrade this from near-certain to byte-proven,
but is no longer the *primary* question.

**→ Direct answer to "isn't that helpful for the DualSense info?": yes, decisively — and it's now
more than helpful, it closes the loop.** The DualSense player-slot-indicator feature (drive the
DualSense white LED dot-row from the *real* console slot) needed exactly one protocol fact: does the
console provide the slot to us and update it on reorder? Between Method E (genuine controller's LEDs
change on reassignment) and the owner reassigning our own dongle via the reorder menu, **the console
demonstrably sends us the slot.** The feature therefore reduces entirely to firmware we control:
parse the `0x09` we currently discard → map bitmask to player number → drive the DualSense
`player_leds` row from it. No further protocol RE is required to start it.

**Method C — indirect: does Change Grip/Order reassign the emulated controller on-screen? (zero
firmware, weaker than Method E).**
Connect the emulated Pro2 to the Switch 2, open Change Grip/Order, try to move it to a different
slot, and watch the console's on-screen controller list.
- If the console **reassigns it on-screen**, that is **circumstantial** evidence of a fresh `0x09` —
  weaker than Method E because the console could update its own UI without notifying the controller.
- If the console **refuses to reassign** the emulated controller, that is itself a finding about how
  the emulation is seen (and a reason to compare against Method E's genuine-controller behavior).
Use this only as a cross-check on the emulated side; Method E is the definitive protocol test.

**Method A — flash-backed record, read after unplug (firmware; the definitive firmware method).**
Sequential, not simultaneous, and it **must persist to flash, not RAM**, for a reason worth stating
plainly:

> **The dongle is bus-powered** (`usb_descriptors.c:184`, `bmAttributes = 0xA0`; no battery, no RTC
> backup — the "external power + battery full" byte at `switch_pro2.c:792` is a *fiction reported to
> the console*, not the dongle's real supply). Recording happens plugged into the **Switch 2**;
> reading the log happens plugged into a **PC** (config mode is a serial device for a PC; the Switch
> 2 is not a serial terminal). Getting from one to the other means **physically unplugging, which
> cuts power and wipes all SRAM.** So the log must live in **flash** — a RAM ring, however well it
> survives an in-place mode-cycle, is gone the instant the dongle leaves the console's port.

1. Firmware records each received `0x09` payload (bitmask + a monotonically increasing change
   counter, so repeats vs. genuine updates are distinguishable) to a **dedicated scratch flash
   sector**, separate from `CONFIG_FLASH_OFFSET` (`config.c:33`, last 4 sectors) and the BTstack TLV
   bond region so it clobbers neither. Flash wear is a non-issue — slot assignment is low-frequency
   (init + reorders), a handful of writes.
2. **Cross-core constraint (same lesson as the 2026-07-15 BOOTSEL work):** the `0x09` handler runs
   on **core0**, which cannot park itself to write flash. Core0 only stores the bitmask + a dirty
   flag in RAM; the actual write is done by **core1's existing deferred-save path** (what
   `config_service_save()` already does — core1 parks core0, then erases/programs). Do **not** call
   `flash_range_program` inline from the core0 handler.
3. **Robustness — a manual commit trigger.** To remove any doubt that the last update reached flash
   before you unplug, add a deliberate "commit now" gesture (e.g. a specific button combo on the
   *connected Bluetooth* controller) that forces the deferred flush. Reorder on the console, press
   the commit combo, *then* unplug. This sidesteps any race between the last `0x09` and the flush
   cadence.
4. Unplug from the Switch 2, plug into a PC, enter CDC config mode, and read the scratch sector back
   (a `sw2cap drain`-style command). Flash contents survive the unplug; RAM would not.

The existing `sw2_capture.c` ring (`sw2_capture_record()` / `sw2_capture_drain_one()`, drained by
`sw2cap drain`) is the right *pattern* but is RAM-backed and BLE-oriented today; Method A is the
flash-backed, USB-device-side variant.

**Method D — input-echo: make the emulated controller *show us* the slot through its own input
(firmware; no unplug, no LED counting, but low-bandwidth/hacky).**
The one output channel a controller has *to the console screen* is its own input report. So firmware
can encode the decoded slot as a distinctive, deliberate input that is visible on any console screen
that displays controller input (e.g. System Settings → Controllers → **Test Input Devices**, or
simply watching menu navigation).
- On a **latched trigger** (a button combo on the connected BT controller — *not* automatically, to
  avoid interfering with Change Grip/Order's own L+R registration inputs), the emulated pad emits a
  slot-encoded pattern: e.g. press **A** *N* times where *N* = decoded player number, or hold a
  distinct D-pad direction per slot. You read *N* off the screen.
- Procedure: reorder in Change Grip/Order → back out to a normal menu / the input-test screen →
  fire the echo trigger → count the emulated presses → that is the slot the console last told us.
- **Confirms receipt AND decode without ever unplugging**, at the cost of being manual and
  low-bandwidth (a few distinguishable states per "frame"). Genuinely hacky; use only if Method A's
  flash plumbing is undesirable.
- **Caveat:** must not auto-fire in Change Grip/Order itself — injected inputs there could
  register/deregister the controller. Gate it behind an explicit trigger and read it on a *different*
  screen.

**Not viable:** the onboard LED blink-code path (`NS2_DIAG`) — the project owner has ruled out
reading LED counts, and it is low-bandwidth/error-prone. Listed so it is not re-proposed.

**What any of these answers:**
- **A new `0x09` arrives with a changed bitmask** (`0x03`/`0x07`/`0x0F`) → slot assignment is a live
  update; the DualSense player-LED row (`player_leds`) can be driven from the *real* console slot and
  will follow Change Grip/Order in real time.
- **No new `0x09` arrives** → assignment is init-only; the dongle only learns its true slot at
  registration, and reorder would not be reflected without a re-registration.

This also informs the "live color" open question from the unified-color section: if the console
re-issues per-slot commands mid-session, it proves the console *does* push slot state after
enumeration. (Color is still read only from the factory block at enumeration — the `0x09` command
carries the slot bitmask, not color — so this does not by itself make the on-screen *color*
live-updatable; it only settles the *slot* question.)

### Theories & hypotheses (explicit, with confidence)

- **Confirmed:** Switch 1 `0x30` "Set player lights" exists, its bitfield mapping
  (`0x01/0x03/0x07/0x0F` = P1–P4, high nibble = flashing), and that it is **re-sent live on
  reorder** — from both community RE and this repo's own `switch_pro.c:250-263` parse.
- **Confirmed:** Switch 2 sends `0x09` "Set Player LEDs" at init, with a player bitmask at payload
  byte index 8 = `0x01` for Player 1 (this repo's captures).
- **Confirmed:** all three Switch 2 personalities currently **bare-ACK `0x09` and discard the
  payload** (`switch_pro2.c:685`, `switch_gc.c:546`) — so no slot-following feature is possible
  without first adding the parse.
- **Strong (prediction, not observed):** Switch 2's `0x09` bitmask values for P2/P3/P4 match Switch
  1's (`0x03`/`0x07`/`0x0F`). Basis: exact Player-1 match + shared OS base.
- **Confirmed (console behavior):** the Switch 2 **sends the player-slot command on (re)assignment**.
  Basis: owner-observed that a genuine controller re-registered via Change Grip/Order (off → L+R)
  re-appears as player N **with its LED changed** — the LED can only change if the command was sent.
- **Confirmed (sent to us too):** the console sends the slot command **to our emulated dongle**, over
  USB. Basis: the owner has **used the reorder menu to change our dongle to a different player
  position**, and in the Switch protocol "assign to player N" *is* the Set-Player-LEDs command (no
  separate position path). We receive it and currently **discard** it (`switch_pro2.c:685`). The only
  un-nailed detail is the exact received bitmask bytes, which Method A would log — but the mapping is
  already Confirmed/Strong.
- **Minor follow-up:** whether a pure *drag*-reorder (without the L+R re-registration) also re-sends,
  vs. only registration-with-a-new-slot. The latter is already Confirmed; the former is a small gap.
- **Unknown:** whether the console re-sends `0x09` on any event *other* than (re)assignment (e.g.
  another controller joining/leaving, sleep/wake). Not investigated; would fall out of Method A's log
  if it captured a full session.
- **Unknown:** whether, once parsed, the console-assigned slot should *override* or *coexist with*
  the dongle's own internal player index (`manager.h`'s `player_number`). A design decision for
  whenever a feature is built, not a protocol fact.

**Confidence:** **Strong** that the mechanism works (both endpoints are writes we already perform;
the emit side is Confirmed by this doc's dumps, the lightbar side by the existing driver). The
enumeration-time stickiness of the factory read is **Confirmed** behavior of the SPI/identity path.
Whether the console re-reads color on any lighter-weight event than full re-enumeration is
**Unknown** and would be the one thing worth testing before committing to a "live" color feature.

## Xbox Elite Series 2 LED: can we read it, and can we control it?

Raised by the project owner (2026-07-16): the Xbox Elite Series 2 (BT PID `0x0B22`, wired `0x0B05`)
has a controllable LED, and Steam can change it over Bluetooth "so it's not static" — can we read
it? Investigated read-only. **The framing needs one correction, which is the same lesson as the
DualSense lightbar:**

### The Xbox LED is an OUTPUT (host-set), not an advertised value — there is nothing to "read"

The Xbox button (guide/Nexus) LED is set by the *host*, exactly like the DualSense lightbar. "Steam
can change it over Bluetooth" proves a host→controller **set command exists**; it does **not** mean
the controller reports a color/state that can be read back. Confirmed by inspection of this repo's
Xbox drivers:
- **Xbox input reports carry no LED field** — buttons, sticks, triggers, and (Elite 2) the 4 back
  paddles in byte 19, and nothing else. There is no LED-state byte to read.
  (`xbox_ble.c`: "Xbox BLE HID reports are 16 bytes"; `xbox_elite2` quirk: paddles in byte 19.)
- **No driver anywhere reads an LED state** from any source controller — every "LED" reference in
  the codebase is a *send* (Nintendo Wiimote/Wii U Pro player-LED output). LED is universally a
  set-direction concept here.

So "read the Xbox LED color" does not map to how Xbox LEDs work, the same way it didn't for the
DualSense lightbar. Whoever last *set* it knows the value because they set it; the controller does
not advertise it.

**Also — Steam is a different host than us.** Steam changes the LED when the Xbox pad is paired to a
**PC running Steam**. When that same pad is connected to **our dongle**, Steam is not in the loop —
*we* are the BT host. So the capability Steam demonstrates ("the LED is settable over BT") would, on
our dongle, be **ours to drive**, not something to read from Steam or the controller. The useful
question is therefore **"can we SET it,"** not "can we read it."

### What is the Xbox LED, physically? (Confirmed by owner: RGB color + brightness)

**Corrected 2026-07-16 by the project owner's direct observation:** in Steam, the Elite Series 2
exposes **both brightness and color** control, and both genuinely change over Bluetooth. So the
earlier "white-only guide LED" assumption is **wrong for the Elite 2** — its LED is **RGB
color-capable**, not a plain white guide light. (This may be an Elite-2-specific capability; the
plain Xbox One/Series guide LED is still white-only as far as is known, so this finding is scoped to
the Elite 2 unless a wider capture says otherwise.)

**Corroborated by independent web sources (2026-07-16):** the Elite 2 guide button is a genuine RGB
LED — it *appears* white by default only because all three channels are driven to max; Steam exposes
a full RGB picker (hex code) plus brightness, ~16M colours. Reported as an Elite-2-specific,
originally Steam-only feature. (ResetEra, Newsweek, GameRant, ScreenRant, Gamepur — see Sources.)
So the owner's correction is firmly Confirmed and my original "white-only" was wrong.

### Can we set it over BT? — a real conflict in the evidence, unresolved

**The reference `xbledctl` (github.com/Leclowndu93150/xbledctl) both helps and complicates this.**

What it gives us (the **USB / GIP** command, Confirmed by that tool):
- Transport: **GIP (Game Input Protocol) over USB**, via Windows `xboxgip.sys` (`\\.\XboxGIP`,
  IOCTL `0x40001CD0`).
- 20-byte GIP header + 3-byte payload. Header: `deviceId` (8B), `commandId` **`0x0A`** (LED) at
  offset 8, `clientFlags 0x20`, `sequence`, `length=3`.
- **3-byte LED payload: `[0x00 = guide-LED sub-command, mode (0x00 off / 0x01 on / 0x02 fast
  blink …), intensity 0–47]`.**

Two problems this creates, both of which must be stated honestly rather than glossed:

1. **This command is brightness/mode only — there are NO colour bytes in it.** The payload is
   sub-command + mode + intensity (0–47); no R/G/B. Note byte 0 is explicitly a *sub-command
   selector* (`0x00` = guide LED), which strongly implies **other sub-command values exist** — the
   RGB-colour path is very likely a *different* sub-command that xbledctl simply did not implement.
   So xbledctl is a **partial** reference: it confirms the `0x0A` LED command family and its
   framing, but **not** the colour payload. The full RGB command format is still **Unknown**.

2. **xbledctl explicitly states the LED is NOT controllable over Bluetooth** — verbatim: *"Bluetooth
   is not supported (the LED is not controllable over Bluetooth at the firmware level)."* This
   **directly contradicts** the owner's observation that Steam changes it over BT, and it is the
   single most important open question for this project, because **our dongle connects to the
   controller over Bluetooth.** If xbledctl is right, we **cannot** set the Elite 2 LED from our
   dongle *at all*, regardless of command format — and the Xbox unified-colour feature is
   **impossible over BT**, full stop.

Possible reconciliations (none yet confirmed):
- xbledctl's "not over BT" may be scoped to **GIP specifically** — GIP is inherently a USB /
  Xbox-Wireless protocol, so "no GIP over BT" is trivially true, yet a separate **BT-HID vendor
  report** could still exist that Steam uses. The web reporting that this is a *Steam-only* feature
  hints Steam has some special/undocumented method.
- Alternatively, Steam may set the colour only when the Elite 2 is on **USB or the Xbox Wireless
  dongle**, and the "over Bluetooth" impression is mistaken. (Worth double-checking directly, not
  assumed either way — the owner has been a reliable primary source, but this is exactly the claim
  xbledctl disputes at the firmware level.)

**This walks back the earlier over-confident conclusion.** Two messages ago this doc said the Elite
2 "IS a valid unified-colour candidate" and a "BT LED-set command definitely exists." Given
xbledctl, the honest status is: **RGB capability Confirmed; BT-settability Contested; therefore the
unified-colour feature for Xbox is CONDITIONAL on the LED being settable over BT, which is now
specifically in doubt.**

### The experiment that resolves it (software-only, PC is host — NOT the ruled-out sniffer)

Because Steam/Xbox Accessories run on a **Windows PC that is the USB/BT host**, the PC can log its
*own* outbound traffic — this is a different situation from the ruled-out Switch 2 sniffing (there
the console was the host):
1. **Connect the Elite 2 to the PC over Bluetooth specifically** (not USB, not the Xbox dongle).
2. Start a **Windows Bluetooth HCI / btsnoop capture** (built-in Windows BT logging, or Wireshark on
   the local BT interface).
3. Change the colour in Steam.
4. **Does any HID output report appear on the BT link?**
   - **Yes** → the LED *is* BT-settable; the captured bytes are the command we need (directly usable
     via `bthid_send_output_report()`), and xbledctl's BT claim is wrong/GIP-scoped. Feature is
     possible.
   - **No** (nothing on the BT link when colour changes) → confirms xbledctl: the physical set does
     not happen over BT, so **our dongle cannot drive the Elite 2 LED** — an important negative
     result that closes the Xbox unified-colour idea over BT.
- USBPcap over a USB connection captures the GIP `0x0A` colour sub-command as a cross-reference, but
  **does not answer the BT question** — only the BT-HCI capture does.

`xpadneo` (already referenced here for the Confirmed rumble `0x03` layout) is worth checking for any
Elite 2 LED output report *over BT*; if xpadneo has none, that is weak corroboration of xbledctl's
BT claim.

### Summary and confidence

| Claim | Confidence |
|---|---|
| Xbox LED is host-set output, not an advertised/readable value | **Confirmed** (input reports have no LED field; no read path exists) |
| Nothing to "read" — reframe to "can we set it" | **Confirmed** (follows from the above) |
| On our dongle we are the host, not Steam | **Confirmed** (architecture) |
| Elite 2 guide LED is a genuine **RGB** LED (appears white = all channels max) | **Confirmed** — owner (Steam + Xbox Accessories) *and* multiple web sources |
| The USB/GIP LED command family (`commandId 0x0A`) and its brightness/mode payload | **Confirmed** (xbledctl) — but that payload is brightness/mode only, **no colour bytes** |
| The RGB **colour** command format (BT or USB) | **Unknown** — likely a different GIP sub-command; xbledctl did not implement it |
| The LED is settable **over Bluetooth** (the transport our dongle uses) | **CONTESTED** — owner says yes (Steam over BT); xbledctl says *"not controllable over Bluetooth at the firmware level."* Unresolved, and make-or-break |
| Elite 2 as a unified-colour candidate for our dongle | **CONDITIONAL** on the above — possible only if the LED is genuinely BT-settable; walked back from an earlier over-confident "Yes" |
| The BT question is answerable software-only (PC is host) | **Confirmed feasible** — a Windows BT-HCI/btsnoop capture over a BT connection settles it |
| Plain (non-Elite) Xbox guide LED is white-only | **Strong** (general knowledge + web), not re-verified — out of scope |

**Suggested next step (no code):** resolve the one make-or-break unknown — **is the LED actually set
over the Bluetooth link, or only over USB / Xbox-Wireless?** Connect the Elite 2 to a PC **over
Bluetooth**, run a Windows **BT-HCI / btsnoop** capture, and change the colour in Steam. If a HID
output report appears on the BT link, capture it (directly usable via `bthid_send_output_report()`)
and the Xbox unified-colour feature is possible; if nothing crosses the BT link, xbledctl is right,
our dongle cannot drive the LED, and the feature is closed over BT. USBPcap/GIP and `xpadneo` are
cross-references for the command *bytes* but do **not** answer the BT question — only the BT-HCI
capture does. As with the DualSense, if it is settable this is a **set** feature (mirror the chosen
controller colour), never a read.

---

## Confidence

- **Confirmed:** all genuine bytes above are read directly from genuine-hardware SPI dumps; Pro2
  and GC each corroborated by two independent dumps.
- **Confirmed:** the four-field layout and offsets (`0x13019`/`0x1301C`/`0x1301F`/`0x13022`) — see
  `docs/switch2/usb-spec.md`, `docs/switch2/protocol-research.md`, `docs/switch2-gc/protocol.md`.
- **Strong (not yet visually verified):** that the console actually *renders* these specific colors
  in its UI. The bytes are confirmed; the on-screen result of correcting Joy-Con 2's highlight has
  not been observed on a real console by this project.

## Remaining unknowns / suggested follow-up

1. **Does correcting Joy-Con 2's highlight change the on-screen color on a real Switch 2?** The one
   experiment that would move "Strong" to "Confirmed": set L highlight `9B E1 E6` / R highlight
   `FF 8C 5F`, and observe the console's pairing UI. (Fix requires making the R identity block patch
   its own color bytes rather than inheriting L's — see Joy-Con 2 R finding above.)
2. **Are there other retail colorways?** These dumps are single specific units. Nintendo ships
   multiple Joy-Con 2 / Pro 2 colorways; the *layout* is universal but the *values* here are one
   unit each. Not a correctness issue for matching "a genuine controller," but worth noting before
   treating any single value as canonical.
3. **The `01 06 01` (Pro2) / `01 08 02` (JC2) / `01 04 01` (GC) constant at `0x13016`** appears to
   be a type/format/color-count descriptor preceding the color fields. Its exact field semantics
   are not decoded here; the values are Confirmed but their meaning is Hypothesis.

## Related files

- `src/switch_pro2/switch_pro2.c` (`0x13019`-`0x13022`, Pro2 factory block)
- `src/switch_joycon2/switch_joycon2.c` (`switch_joycon2_ctrl_identity_l`, `_right()` derivation)
- `src/switch_gc/switch_gc.c` (`switch_gc_ctrl_identity`)
- `dumps/SPI/2069_spi_dump_*.bin`, `dumps/SWITCH2_JOYCON_L_1.bin`, `dumps/SWITCH2_JOYCON_R_1.bin`,
  `dumps/SPI/NSO_GC_SPI_DUMP_*.bin`
- `docs/switch2/usb-spec.md`, `docs/switch2/protocol-research.md`, `docs/switch2-gc/protocol.md`
