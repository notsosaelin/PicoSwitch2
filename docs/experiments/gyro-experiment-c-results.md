# Experiment C — Re-mining the existing USB capture (host→device side)

**Status:** ✅ complete for what this capture can answer (motion-enable mechanism **confirmed**;
Steam-side validation routes to Experiment A). **Date:** 2026-07-09.
**Parent plan:** [gyro-differential-re.md](gyro-differential-re.md) (Experiment C).

---

## 1. Question

In ndeadly's USB capture, what does the **host** send the controller (enumeration, feature
reports, any IMU-enable), and does motion turn on only after a specific command? We had only
ever parsed the *device→host input reports* (to decode the motion byte layout). The
*host→device* side was unmined and might reveal an enable step we never implement.

## 2. Why it matters

If a specific host command gates motion, then our always-on-motion model is wrong at the
protocol level, and the fix is to model motion as a negotiated capability rather than to keep
tweaking report bytes. This is the cheapest possible experiment (we own the file + a parser).

## 3. Method

The capture (`usb.pcapng`, 60 MB) turned out **not** to be a macOS URB capture as previously
assumed. `capinfos` reports **encapsulation `USB 2.0/1.1/1.0 (208 - usb-20)`, produced by
Packetry 0.5.0 (Great Scott Gadgets / Cynthion)** — a **raw USB-2.0 wire capture**: every PID
token, DATA packet, and handshake individually. That is *richer* than a URB capture (we see
SETUP packets and command-endpoint payloads directly), but tshark shows the metadata Custom
Blocks as "data," so transactions were reassembled in Python
(`scratchpad/usb_c_hostcmds.py`): classify each EPB payload by USB PID, pair each
SETUP/IN/OUT token with the DATA packet that follows it, decode.

Wire model confirmed by the PID histogram (1,339,738 packets):

| PID | name | count | meaning |
|---|---|---|---|
| 0xA5 | SOF | 753,586 | frame markers (125 µs) |
| 0x69 | IN | 195,099 | IN tokens |
| 0xD2 | ACK | 191,104 | handshakes |
| 0x4B/0xC3 | DATA1/DATA0 | 95,555 / 95,549 | data packets |
| 0x5A | NAK | 6,420 | not-ready |
| 0xE1 | OUT | 2,407 | OUT tokens |
| 0x2D | SETUP | 18 | control setups |

## 4. Results — CONFIRMED

### 4.1 Capture identity
A real **host ↔ genuine Switch 2 Pro Controller** wired USB-2.0 session. The controller
enumerates and is assigned **USB address 7**. A live rumble/haptic stream on EP 0x01 OUT
(`02 5X …`, incrementing sequence counter) begins ~packet 257 189, confirming an active
console-style session, not a passive descriptor dump.

### 4.2 Enumeration = vendor identity handshake (matches our model)
```
GET_DESCRIPTOR DEVICE (8 then 18)
SET_ADDRESS 7
GET_DESCRIPTOR CONFIG (9 then 80)
SET_CONFIGURATION 1
vendor IN  0xC0 bReq=0x03 wLen=64      <- identity block
vendor IN  0xC0 bReq=0x02 wLen=16
vendor OUT 0x40 bReq=0x04 wValue=0x0276 wLen=0
```
**No HID `SET_REPORT`/`GET_REPORT`/`SET_IDLE` anywhere.** Confirms the device is vendor-class
with a bulk command channel, exactly as `switch_pro2.c` emulates.

### 4.3 Command channel = structured protocol on bulk EP 0x02
Every command is `[cmd][0x91][0x00][sub][0x00][plen][0x00][0x00][data…]`, where **byte 1 =
`0x91` marker**, **byte 3 = subcommand**, **byte 5 = payload length** (verified: total length =
8-byte header + plen across many samples). Command families seen pre-motion, in order:

| pkt idx | cmd/sub | notes |
|---|---|---|
| 156 | 03/0d | Init USB |
| 223 | 07/01 | first-init |
| 283 | 16/01 | unknown (24 zero bytes) |
| 445–673 | 15/01,02,03 | BT pairing over USB |
| 735 | 09/07 | player LEDs |
| 741 | 0c/02 | **feature (configure) family** |
| 8749 | 03/0c | ×200 total across session (periodic poll/keepalive) |
| 8756 | 0a/02 | ×31 |
| 8817 | 02/04 | ×13 — flash memory reads (calibration) |
| 9138 | 11/03 | opaque query |
| 9257 | 0a/08 | |
| 9368 | 11/01 | |
| 9483 | 0c/06 | **feature configure** (plen 10) |
| 9608 | 0c/06 | **feature configure** (plen 11) |
| 9784 | 0c/04 | **feature configure** (plen 4) — last command before motion |
| **9815** | — | **first report-0x09 carrying 30-byte motion (report[0x0E]==30)** |
| 9903 | 03/0a | "select input report" — appears *after* motion already present |

### 4.4 Our firmware vs. the capture
`ns2_dispatch()` in [switch_pro2.c](../../src/switch_pro2/switch_pro2.c) handles all these
families, **but**:
- `0x0C` handler: `sub 0x01`→feature-info, `sub 0x06`→40-byte echo, **else (incl. 0x02, 0x04)
  → generic 4-byte ACK**.
- Motion is emitted **unconditionally** in every report 0x09 once `ns2_streaming` is set
  (which we gate on `0x03/0x0A`). **We do not model motion as a negotiated feature — this is
  the inverse of the real controller (§5).**
- Ordering resolved (§5): report 0x09 streams from packet **61** (right after
  `SET_CONFIGURATION`), long *before* `0x03/0x0A` (9903). So **`0x03/0x0A` is not the stream
  trigger** — the real controller streams reports from power-up; `0x03/0x0A` selects/re-confirms
  the report id. Our gating on `0x03/0x0A` is a simplification the console tolerates, but it is
  not what the hardware does.

## 5. Results — CONFIRMED: motion is a negotiated feature (not always-on)

The decisive scan tracked the report-0x09 **motion-length byte** (report offset `0x0E`) across
the whole session:

```
report-0x09 motion-len (report[0x0E]) transitions:
  idx     61 : motion-len = 0     <- 0x09 streams from power-up, but with NO motion payload
  idx   9815 : motion-len = 30    <- motion TURNS ON
  idx 370890 : motion-len = 0     <- turns OFF again (session teardown)
histogram over all 0x09 reports: { 0: 251 reports, 30: 50,650 reports }
```

The genuine controller sends report 0x09 continuously from packet 61 with **`motion-len = 0`
(no motion block)** for 251 reports, then flips to `30` at packet 9815 and streams motion for
the rest of the session. **Motion is a negotiated capability, not an always-on field.** Our
firmware does the opposite — it emits `len = 30` motion from the very first report.

### 5.1 The exact enable trigger
The transition to `motion-len = 30` is immediately preceded by the **`0x0C` "feature configure"
cluster**, ending in `0x0C/0x04`:

```
idx 9483  OUT 0x0C/0x06 plen10  0c910006000a0000 04000000 02 020100 8a00   feature id 0x02
idx 9490  IN  reply             0c01000600f80000 00000000 02 000000 0076…  (40 data bytes)
idx 9544  OUT 0x0C/0x06 plen10  …04000000 02 020100 0a70                    feature id 0x02
idx 9608  OUT 0x0C/0x06 plen11  …04000000 03 020100 9b0000                  feature id 0x03
idx 9669  OUT 0x0C/0x06 plen10  …04000000 02 020100 0a76                    feature id 0x02
idx 9784  OUT 0x0C/0x04 plen4   0c91000400040000 27000000                   <- LAST cmd
idx 9795  IN  reply             0c01000400f80000 00000000                   (4 zero data bytes)
idx 9814  IN  rpt09  counter=0xf9  motion-len=30                            <- MOTION ON
```

So the enabling handshake is: `0x0C/0x06` configures motion "features" (feature ids `0x02` and
`0x03`, params `02 01 00`), then `0x0C/0x04` (data `0x27`) commits/enables, and one transaction
later the controller begins populating the 30-byte motion block.

### 5.2 The counter-intuitive part: our REPLIES already match
The real controller's replies to this cluster are byte-compatible with what `ns2_dispatch()`
already emits:
- `0x0C/0x06` → 40 data bytes, zeros except `d[4] = requested feature id`. Ours: `memset(d,0,40);
  d[4]=c[12]; dl=40;` — **matches** (the real reply carries a couple of extra non-zero status
  bytes like `0x76` we currently zero; likely cosmetic).
- `0x0C/0x04` → header echo + 4 zero data bytes. Ours: `else → dl=4` (4 zero bytes) — **matches.**

**So the difference is behavior, not reply bytes.** The real controller withholds motion until
this handshake completes; we stream it immediately. The open question is whether a host *gates*
its motion parsing on this handshake/state, or merely reads whatever motion bytes are present.

## 6. Remaining questions

1. **Does the host gate motion parsing on the enable state?** Experiment C proves the controller
   *withholds* motion until enabled, but not that the *host* refuses to read early motion. If the
   host is a strict state machine (only trusts motion after it sent the `0x0C` enable and saw
   `len` go 0→30), our always-on `len=30` could be ignored or mis-latched. This is the crux and
   it needs a **host-side** capture.
2. **Does Steam use the same mechanism?** This capture's host is a **console** (live rumble
   stream on EP1 confirms it) talking to the controller over **wired USB** — *not* Steam/Windows,
   and not the report-0x05 profile Steam selects. So §5 is confirmed for the **console / report
   0x09** path; the Steam / report 0x05 path must be validated separately.
   → **Experiment A (USBPcap on the Steam PC).**
3. **Feature-id semantics of `0x0C/0x06`** — are ids `0x02`/`0x03` "accelerometer"/"gyro", and do
   the `02 01 00` params set rate/range? Worth decoding if we implement the gate.

## 7. Conclusion

Experiment C succeeded and is complete for what this capture can answer. It:
- corrected a wrong assumption about the capture (raw USB-2.0 **wire** capture from Packetry /
  Cynthion, not a macOS URB capture);
- confirmed our enumeration + command-channel model byte-for-byte;
- **proved that report-0x09 motion is a negotiated feature**: the genuine controller streams
  `motion-len = 0` from power-up and only emits the 30-byte motion block after the `0x0C/0x06`+
  `0x0C/0x04` enable handshake (packets 9483–9814);
- established that **our firmware replies to that handshake correctly but models motion as
  always-on**, the inverse of the hardware.

**Actionable outcome:** the highest-value change this unlocks is to make our firmware *match the
golden trace's behavior* — withhold the motion block (`len = 0`) until the `0x0C` enable
handshake completes, then emit `len = 30`. That is a small, well-scoped firmware change and it
directly tests hypothesis §6.1 on the **console**. For the **Steam/PC** failure the user actually
reported (report 0x05, frozen-after-one-update), the mechanism must still be confirmed on the PC
bus — **Experiment A** — because this capture does not contain a Steam host. Recommended order:
implement the console-side enable gate (matches ground truth regardless), then Experiment A to
resolve the Steam path.
