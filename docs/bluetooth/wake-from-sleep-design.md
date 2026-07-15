# Wake-from-sleep via a crafted BLE advertisement

**Status:** 🔵 Partial — design substantially strengthened, no firmware implemented yet. Written
2026-07-14, prompted by the project owner. **Major update, same day**: the exact wake advertisement
byte format is now **Confirmed** from `ndeadly/switch2_controller_research`'s
`bluetooth_interface.md` and `commands.md` — this is no longer a "capture an unknown payload"
problem, it's an implementation problem against an already-fully-documented protocol.

This supersedes both the original flat "out of scope" verdict
(`docs/bluetooth/btstack-implementation.md` "BLE wake-from-sleep",
`docs/experiments/ns-pc-control-audit-2026-07-12.md` §6) and this document's own first draft, which
assumed a "capture a real advertisement, replay it" model because no byte-exact reference was known
to this project at the time. That reference existed all along in the already-cloned
`E:\nso-gc-refs\switch2_controller_research` — just not yet read for this specific question.

---

## 1. Why the console wakes at all

The Switch 2 wakes from sleep only in response to a specific BLE advertisement from a controller
already bonded to it. This dongle presents to the console purely as a **USB** device
(`src/usb.c`/`src/usb_descriptors.c`), so nothing it currently does can trigger this — USB and the
console's BLE wake mechanism are architecturally unrelated. Implementing this would require the
dongle's own Bluetooth radio (currently used only as a central/host, connecting to a physical
controller) to also transmit a BLE advertisement directly to the console.

## 2. The wake advertisement — Confirmed, byte-exact

Per `bluetooth_interface.md` "Bluetooth LE Advertisements": advertisements from a Switch 2 controller
to the console consist of just two AD structures, Flags and Manufacturer Data, and **the wake
advertisement is byte-identical to the ordinary reconnection advertisement except for a single flag
bit**:

| Advertisement Type | Manufacturer Data (hex) |
|---|---|
| Reconnection | `53 05 01 00 03 7e 05 69 20 00 01 00 5f 11 85 eb f1 48 0f 00 00 00 00 00 00 00` |
| **Wake Console** | `53 05 01 00 03 7e 05 69 20 00 01 81 5f 11 85 eb f1 48 0f 00 00 00 00 00 00 00` |

(Both examples shown are for a Pro Controller 2 — `7e 05 69 20` at offset 5 is VID `0x057E`/PID
`0x2069` little-endian. `5f 11 85 eb f1 48` is a real host BD_ADDR from the example capture, not
reproduced from a real device by this project.) Field-by-field (26 bytes total):

| Offset | Size | Field | Value |
|---|---|---|---|
| `0x0` | 2 | Manufacturer ID | `0x0553` (Nintendo), always |
| `0x2` | 1 | Unknown | Always `0x01` |
| `0x3` | 1 | Unknown | Always `0x00` |
| `0x4` | 1 | Unknown | Always `0x03` |
| `0x5` | 2 | Vendor ID | `0x057E`, always |
| `0x7` | 2 | Product ID | This dongle's active personality's PID |
| `0x9` | 1 | Unknown | Always `0x00` |
| `0xA` | 1 | Unknown | Always `0x01` |
| `0xB` | 1 | **Wake flag** | **`0x81` for wake, `0x00` for plain reconnect** — the entire distinction |
| `0xC` | 6 | Host address | The bonded console's own BD_ADDR, reverse byte order |
| `0x12` | 1 | Unknown | Always `0x0F` |
| `0x13` | 7 | Reserved | All `0x00` |

This means constructing a wake advertisement requires exactly two pieces of information this project
either already has or can derive without any capture step: **the target personality's VID/PID**
(already known for every personality this project implements) and **the bonded console's own
BD_ADDR** (this project's own SPI dump analysis already found and structurally decoded this exact
field — see `docs/experiments/spi-dump-analysis-2026-07-10.md` §3.4 and
`docs/experiments/joycon2-spi-dump-analysis-2026-07-14.md` §3.11 — real per-console addresses
redacted in both docs, but the *field* is fully understood). PDU type is plain `ADV_IND`, Flags AD
structure is `0x06` (`GeneralDiscovery | BrEdrNotSupported`) — both standard, already
straightforward for BTstack to construct.

**This is no longer a "what bytes do we send" question — that's answered.** The open question is
purely whether this dongle's BTstack/CYW43 setup can transmit a raw advertisement at all (see §4).

## 3. How a genuine controller triggers this itself

`commands.md` documents Command `0x03`, Subcommand `0x01` ("Bluetooth Wake") — sent over the same
USB/vendor command channel this project already implements handling for (`ns2_dispatch()` in
`switch_pro2.c`, `switch_gc_vendor_dispatch()` in `switch_gc.c`): "Starts broadcasting Bluetooth LE
advertisements to wake the console when argument is nonzero." This is the real, documented mechanism
— not a guess. It confirms the wake advertisement's byte format is genuine (used by real hardware,
real command channel, not reverse-engineered from an unrelated third party's reimplementation), and
that a genuine controller's own firmware is what actually starts transmitting it — this dongle would
need to do the equivalent itself, not receive this command from anywhere (nothing on this dongle's
USB side would ever send it `0x03`/`0x01`, since the dongle *is* the USB device, not a host).

## 4. What's still genuinely unknown

- **Whether this dongle's BTstack/CYW43 configuration can transmit a raw BLE advertisement at all,
  concurrently with or separately from its existing central/host role.** `src/btstack_config.h`
  already defines `ENABLE_LE_PERIPHERAL` (inherited from the bluepad32 template this file was based
  on) but it's currently inert — no advertising-parameter or advertising-enable API is called
  anywhere in `src/`. This is the one real remaining technical unknown, and it's an implementation
  question, not an evidence-gathering one — the next step is simply to try calling BTstack's
  `gap_advertisements_set_data()`/`gap_advertisements_enable()` (or equivalent) and see if it works,
  not to capture more data first.
- **Whether address+payload match is sufficient for the console to actually wake**, or whether it
  additionally validates something a simple advertisement can't satisfy (e.g. checking the address
  is in its own bond list before waking, which is likely and satisfiable, since this project already
  knows its own bonded host's address from the SPI-decoded bond table — versus something exotic like
  a rolling counter or challenge-response, which the advertisement's own byte layout above gives no
  evidence of: every field is either constant or the two pieces of info already listed in §2).
- **Whether the Pico stays powered while the console sleeps** — needed for it to transmit anything
  during that window at all. Depends on whether the Switch 2's USB port keeps VBUS live in sleep;
  not confirmed here.
- **When to actually transmit** — presumably only while no controller is connected and the console is
  presumed asleep, mirroring `0x03`/`0x01`'s own semantics (advertise "when argument is nonzero",
  i.e. on command, not continuously) — not designed yet, but no longer blocked on missing byte-level
  evidence.

## 5. Suggested next steps

1. **Attempt a minimal BLE advertising test**: construct the exact byte sequence from §2 (using this
   dongle's own real bonded-console address, decodable via the same SPI-bond-table understanding
   already in this project, or simply observed live once connected) and try to get BTstack to
   actually transmit it as a raw advertisement. This is now a firmware/BTstack-API question, testable
   independent of whether it actually wakes anything (a nearby BLE scanner — e.g. this project's own
   passive advertisement logging, or a phone's BLE scanner app — can confirm the bytes go out
   correctly before ever testing against a real sleeping console).
2. Only once (1) works: test against a real sleeping, bonded Switch 2.
3. If it doesn't wake the console, that's the point to investigate deeper validation requirements
   (§4) — not before, since there's no evidence yet that anything beyond the documented byte format
   is required.
