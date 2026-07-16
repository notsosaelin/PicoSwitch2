# Wake-from-sleep via a crafted BLE advertisement

**Status:** ✅ Implemented and hardware-confirmed 2026-07-16. The exact wake advertisement was
first confirmed from `ndeadly/switch2_controller_research`'s `bluetooth_interface.md` and
`commands.md`; PicoSwitch2 now learns the required console/controller identity during the ordinary
USB pairing exchange and transmits the advertisement with its own CYW43 radio. Both manual BOOTSEL
single-tap wake and automatic wake from real controller input work on a real Switch 2.

This supersedes both the original flat "out of scope" verdict
(`docs/bluetooth/btstack-implementation.md` "BLE wake-from-sleep",
`docs/experiments/ns-pc-control-audit-2026-07-12.md` §6) and this document's own first draft, which
assumed a "capture a real advertisement, replay it" model because no byte-exact reference was known
to this project at the time. That reference existed all along in the already-cloned
`E:\nso-gc-refs\switch2_controller_research` — just not yet read for this specific question.

---

## 1. Why the console wakes at all

The Switch 2 wakes from sleep only in response to a specific BLE advertisement from a controller
already bonded to it. PicoSwitch2 still presents to the console as a **USB** controller, but its
CYW43 radio temporarily changes from its ordinary central/scanning role to transmit that exact
non-connectable legacy advertisement directly to the console. It then restores the public-address
central configuration and resumes any discovery that was active before the wake request.

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

The implementation in `src/ns2_wake_protocol.c` constructs this byte-exact 31-byte advertising
payload. `tools/test_ns2_wake_protocol.c` locks the output to the independently hardware-tested
Joy-Con 2 example and also validates malformed pairing-data rejection.

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

## 4. Shipping implementation

The implementation is deliberately split by responsibility:

- `switch_pro2.c` stages the identity from USB command `0x15/01` and commits it only after the
  console finalises pairing with `0x15/03`.
- `config.c` stores the validated controller address, up to two host addresses, and PID. Flash work
  is deferred five seconds so it cannot stall the remaining USB pairing handshake. Config v5
  lightbar and remap settings migrate intact to v6.
- `ns2_wake_protocol.c` parses the Nintendo wire order and builds the advertisement without any
  BTstack dependency.
- `ns2_wake.c` owns the manual request and the automatic one-shot policy.
- `btstack_host.c` pauses BLE scan/Classic inquiry, advertises for 1.2 seconds per host identity,
  restores the ordinary public BLE address, and resumes discovery. Existing controller ACLs are
  never disconnected.

Automatic wake is intentionally conservative. Core 0 publishes TinyUSB mounted/suspended state.
Core 1 waits for 750 ms of stable USB inactivity plus a 2 s cold-boot grace, then accepts only a
non-neutral controller button report received after that boundary. This rejects the HOME/gameplay
input that put the console to sleep and the neutral reports controllers send after the dock's brief
power cycle. Wake is one-shot until USB becomes active again. If HID setup briefly owns the radio,
the request remains latched and retries every 500 ms.

The feature is restricted to the Pro Controller 2 USB personality. BOOTSEL single-tap remains a
manual fallback; double-tap pairing, triple-tap wipe, and five-second personality cycling retain
their existing meanings.

## 5. Hardware validation and remaining boundary

Confirmed on a real Switch 2 on 2026-07-16:

1. Put the console to sleep; the dock briefly removes power and the Pico/controller links recover.
2. A BOOTSEL single-tap wakes the console and normal controller operation resumes.
3. With no BOOTSEL action, the first real controller input automatically sends the wake request and
   wakes the console.
4. Neutral reconnect traffic alone does not immediately re-wake the console.

Controller sleep is a separate problem and is not part of this implementation. DualSense/Edge
(Classic Bluetooth in this firmware) naturally powers down during the dock outage. Xbox Series BLE
can advertise long enough to reconnect. A generic post-sleep ACL-disconnect experiment made the
same controller unable to reconnect without pairing and was fully reverted. Future work must not
delete bonds, install a pairing/admission gate, or suppress incoming connections merely to make a
controller sleep.
