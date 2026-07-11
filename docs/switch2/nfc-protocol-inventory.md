# Switch 2 Controller NFC / Amiibo Protocol Inventory

> Status Legend: ✅ Complete · 🟡 In Progress · 🔵 Partial · 🔴 Blocked · ⬜ Not Started
> **Overall status: 🔵 Partial.** Command framing and one full request/response pair are
> capture-confirmed on a genuine Pro Controller 2. Tag-level operations (detect/read/write/
> mount/unmount) are entirely undemonstrated. No NFC IC has been identified in either
> controller. **This document supersedes nothing** — it is the first dedicated NFC doc; prior
> mentions (`docs/switch2/unmapped-features.md` §3) are cross-linked, not duplicated.

---

## 0. Why this doc exists, and the discipline it follows

2026-07-10: gyro/report-0x09 encoder investigation is **paused** (see `STATUS.md` "Current
Objective" and `PLAN.md` v1.1 — not abandoned, deferred until new primary evidence or broader
controller RE changes the picture). Active RE work shifted to systematic reverse engineering of
remaining genuine Switch 2 Pro Controller features, starting with **NFC/amiibo as the first
bounded subsystem** — chosen because Nintendo's own documentation confirms the feature exists,
this repo already stubs a real command for it, and a third-party research repo
(`ndeadly/switch2_controller_research`) has independently documented the command family.

**Six claims are kept strictly separate throughout this document** (per explicit instruction —
Nintendo reusing components/command concepts across controller types is not evidence that a
Joy-Con 2 or Switch 1 implementation transfers to the Pro Controller 2):

1. Official confirmation the **Pro Controller 2** has NFC.
2. Physical NFC hardware identified in **Joy-Con 2**.
3. Physical NFC hardware identified in the **Pro Controller 2**.
4. Protocol behavior demonstrated on **Joy-Con 2**.
5. Protocol behavior demonstrated on the **Pro Controller 2**.
6. **Switch 1** NFC/IR behavior that might, but has not been shown to, carry forward.

Every finding below is tagged with which of these six claims it supports.

---

## 1. Claim-by-claim status

| # | Claim | Status | Evidence |
|---|---|---|---|
| 1 | Pro Controller 2 has NFC (official) | ✅ **Confirmed** | Nintendo's own support page: *"On the Nintendo Switch 2 Pro Controller, the NFC touchpoint is located over the Nintendo Switch logo at the top-center of the controller."* |
| 2 | Joy-Con 2 NFC hardware identified | ⬜ **Unknown** | No IC/chip identified in any source reviewed this pass. Nintendo confirms a touchpoint (right stick) exists; no teardown or datasheet evidence found. |
| 3 | Pro Controller 2 NFC hardware identified | ⬜ **Unknown** | Same — no IC/chip identified. No teardown evidence in this repo or any source reviewed. |
| 4 | Protocol behavior demonstrated on Joy-Con 2 | 🔵 **Unclear attribution** | `ndeadly/switch2_controller_research/commands.md` documents the `0x01` subcommand family with real example bytes, but does not state which controller type(s) each example was captured from. Treat as **not confirmed Joy-Con-2-specific** until re-checked against that repo's raw captures. |
| 5 | Protocol behavior demonstrated on Pro Controller 2 | ✅ **Confirmed, this repo** | `usbpcaptures/genuine_procon_2.pcapng` (this repo's own capture, explicitly identified as "device 7 = Pro Controller 2" in `switch_pro2.c`'s header comment) contains two real command-`0x01` request/response exchanges — see §2. |
| 6 | Switch 1 NFC behavior carries forward | ❌ **Refuted at the command-ID level** | Switch 1's NFC/IR control lives at MCU subcommands `0x21`/`0x22` inside an entirely different single-byte-subcommand protocol (`src/switch_pro/switch_pro.c`). Switch 2 uses a dedicated top-level command `0x01` with its own subcommand family and a different envelope shape (`[cmd][0x91][transport][sub][len_lo][len_hi]...`). The command *numbering scheme* does not transfer; deeper payload-level comparison not attempted (no evidence either format shares primitives). |

---

## 2. Confirmed: real NFC command exchange, genuine Pro Controller 2, USB

**Source:** `usbpcaptures/genuine_procon_2.pcapng` (164,242 packets, USBPcap format, 27-byte
per-packet header + raw USB payload). Re-mined this pass with a new tool,
`tools/extract_nfc_traffic.py`, which scans for the command envelope signature
`[0x01][0x91][transport]` (cmd=NFC, req-marker, transport 0x00=USB/0x01=BLE — this repo's own
established convention, see `switch_pro2.c`'s `ns2_dispatch()`). The scan found **7 raw byte
matches; only 2 are real command frames** — the other 5 have subcommand bytes (`0x6B`, `0x00`,
`0xB3`, `0xA0`) that don't appear in any documented NFC subcommand table and are almost certainly
coincidental 3-byte collisions inside high-entropy payload data (a separate check —
"report-0x09 NFC-state-byte scan" — attempted to correlate this against live input reports and
instead hit a **methodology dead end** documented in §5, kept here so it isn't silently repeated).

### 2.1 Exchange 1 — subcommand `0x0C`

| | Packet | Payload (post-27-byte USBPcap header) |
|---|---|---|
| Request | #30520 | `01 91 00 0c 00 00 00 00` |
| Response | #30526 | `01 01 00 0c 00 f8 00 00 61 12 50 10` |

Decoded: `cmd=0x01(NFC) req=0x91 transport=0x00(USB) sub=0x0C len=0x0000`, response
`cmd=0x01 dir=0x01 transport=0x00 sub=0x0C [00 f8 00 00 ack] payload=61 12 50 10`.

**This exactly matches `switch_pro2.c`'s already-implemented hardcoded response** for NFC
subcommand `0x0C` (`ns2_dispatch()`, `case 0x01: if (sub == 0x0C) { memcpy(d, {0x61,0x12,0x50,0x10}, 4); ... }`)
— confirms this capture is the actual, direct provenance of that hardcoded byte sequence (the
file's own header comment already said as much; this is now traced to the exact packet pair).

### 2.2 Exchange 2 — subcommand `0x01` (NEW — not in any source consulted this pass)

| | Packet | Payload |
|---|---|---|
| Request | #30528 | `01 91 00 01 00 00 00 00` |
| Response | #30532 | `01 04 00 01 00 f8 00 00` |

Decoded: `cmd=0x01 sub=0x01 len=0x0000` (no request payload) →
`cmd=0x01 dir=0x04 transport=0x00 sub=0x01 [00 f8 00 00 ack], no payload`.

`ndeadly/switch2_controller_research/commands.md` lists subcommand `0x01` as **"Unknown"** with
no example bytes at all. This capture is, as far as this session's research reached, the **first
concrete behavioral evidence for NFC subcommand `0x01`**: it's a bare acknowledgment with zero
payload, distinct in shape from `0x0C`'s 4-byte data response.

### 2.3 A real, previously undocumented finding: the response "dir" byte is not always `0x01`

`switch_pro2.c`'s own inline comment states the response header convention as *"echo cmd, dir=0x01,
echo transport, echo subcmd, ACK 00 f8"* — and its code hardcodes `r[1] = 0x01` for **every**
response, unconditionally (`ns2_dispatch()` line ~633).

The genuine capture contradicts this as a universal rule: exchange 2 above (`0x01`/`0x01`) returns
**`dir=0x04`**, not `0x01`. The same window (packet #30538, response to a `cmd=0x08` request at
#30534) shows the identical shape — `08 04 00 02 00 f8 00 00` — also `dir=0x04`, also a bare
zero-payload ack. Meanwhile exchange 1 (`0x01`/`0x0C`, which *does* carry a 4-byte payload) uses
`dir=0x01`. The pattern visible in this narrow window: **`dir=0x01` accompanies a data-bearing
response; `dir=0x04` accompanies a bare/no-data acknowledgment.** This is a hypothesis from two
data points across two different top-level commands (`0x01` and `0x08`), not a confirmed rule —
but it is a concrete, capture-sourced discrepancy against this repo's own code, worth stating
precisely: **`switch_pro2.c` currently sends `dir=0x01` for its `case 0x01: else dl=0;` fallback
path (subcommand `0x01` and any other un-handled NFC subcommand), where the one genuine capture
of that exact exchange used `dir=0x04`.** No hardware symptom has ever been attributed to this
(NFC has never been the subject of hardware testing in this project), so it cannot be called a
confirmed bug — it's a **validation target**, exactly the kind of thing DATA.md's branch-1
next-step asks this pass to surface. **Not fixed this turn** (analysis/documentation only, per
explicit constraint).

### 2.4 Timing: NFC status is queried once, at connection time, not polled

Both exchanges occur within an 18-packet window (#30520-#30538), interleaved with the console's
mandatory init sequence — nearby packets carry `cmd=0x03/sub=0x0A` ("select report 09") and
`cmd=0x08` (Charging Grip, per this repo's own command table) requests. This matches
`docs/switch2/usb-spec.md`'s existing note that command `01/0c` NFC appears "interleaved (order
not strict)" with the init handshake. **New precision this pass:** across all 164,242 packets in
the capture, these are the *only* two NFC-command exchanges present — NFC status is queried
exactly once per connection during setup, not polled during normal operation. (This does not
rule out polling during an actual amiibo interaction — this capture almost certainly contains no
amiibo tap; see §5.)

---

## 3. Report-offset evidence (both transports)

| Transport | Location | Field | Confirmed value | Source |
|---|---|---|---|---|
| USB | report `0x09`, offset `0x0C` | NFC state | `0x00` (idle) — never written by `switch_pro2.c`'s report builder, defaults via `memset` | `docs/switch2/usb-spec.md`, `docs/switch2/protocol-research.md` (both capture-derived), confirmed by code inspection this pass |
| BLE | GATT handle `0x002E` ("Input Report — Headset Audio", Pro Controller only, firmware ≥2.0.0), report offset `0xC` | NFC state | `0x00`-`0x07`, `0x00`=Idle | `ndeadly/switch2_controller_research/bluetooth_interface.md` — **strong evidence, third-party, not independently re-validated against this repo's own GATT capture for this specific field** |

Both transports place "NFC state" at the analogous offset within their respective input report —
consistent, not yet independently cross-checked byte-for-byte by this repo against a live
non-idle state (no capture with a tag present exists anywhere in this project).

---

## 4. Confidence-qualified subcommand table

Base table per `ndeadly/switch2_controller_research/commands.md` (strong evidence, third-party,
controller-type attribution unclear per claim 4 above), annotated with this repo's own findings:

| Sub | Name (ndeadly) | Confidence | This pass's contribution |
|---|---|---|---|
| `0x01` | "Unknown" | 🔵 Partial | **New**: capture-confirmed bare-ack response (`dir=0x04`, no payload) on genuine Pro Controller 2, USB. Semantic meaning (start scan? reset? get-state trigger?) still unknown. |
| `0x02` | — (not listed) | ⬜ Unknown | No evidence found this pass. |
| `0x03` | "Unknown" | ⬜ Unknown | ndeadly has example bytes; not independently re-verified this pass. |
| `0x04` | "Unknown" | ⬜ Unknown | Same. |
| `0x05` | "Get status" | ⬜ Unknown (name only) | ndeadly has example bytes; not independently re-verified this pass. |
| `0x06` | "Read device" | ⬜ Unknown (name only) | Same. |
| `0x08` | "Write device" | ⬜ Unknown (name only) | Same. |
| `0x0C` | "Unknown" | ✅ Confirmed (this repo) | Real request/response traced to exact packets (#30520/#30526); response `61 12 50 10` already implemented in `switch_pro2.c` and now shown to be genuinely capture-sourced, not guessed. **Semantic meaning of the 4 bytes is still unknown** — plausible-but-unconfirmed hypothesis: an NFC controller IC identifier/version tag (common 4-byte chip-ID+rev pattern); no NFC IC datasheet cross-check performed. |
| `0x14` | "Write buffer" | ⬜ Unknown (name only) | ndeadly has example bytes; not independently re-verified. |
| `0x15` | "Read buffer" | ⬜ Unknown (name only) | ndeadly documents request field "Read offset" (u16) and response field "Unknown" (4 bytes); chunking/sequence numbers/checksums/max-transfer-size explicitly undocumented by ndeadly. Not independently re-verified this pass. |

**Not evidenced at all, any source, this pass:** tag detect/mount/unmount transitions, a real
amiibo read (NTAG215 540-byte EEPROM per general amiibo knowledge — not Switch-2-specific
evidence), a real amiibo write, checksums/authentication/framing for multi-chunk transfers,
timing/latency requirements for tag operations.

---

## 5. A methodology dead end, documented so it is not repeated

Attempted: scan every report-`0x09` HID packet in the capture for a nonzero NFC-state byte
(offset `0x0C`), reasoning that a real amiibo tap during the capture session would show up as a
state transition. Filter used: `payload[0] == 0x09` (report ID) on decoded USBPcap payloads.
**Result was a false lead**: only 17 packets matched, two with a nonzero "state" byte — but
inspection showed all 17 are USB **Configuration Descriptors** (`bLength=0x09, bDescriptorType=0x02`),
not HID input reports at all; `0x09` is simultaneously a valid report ID and a valid descriptor
`bLength`, and the filter didn't distinguish them. **Correct approach, not implemented this pass**
(scope: analysis/documentation only): filter by USBPcap header fields (endpoint address + transfer
type = Interrupt IN on the known HID endpoint), not by first-payload-byte alone. Flagged as the
concrete blocker for §6's proposed next task.

---

## 6. Conflicts between sources

None found at the command-ID or offset level — `ndeadly`'s independent documentation and this
repo's own capture-derived docs agree that command `0x01` = NFC and that report offset `0xC`
carries NFC state on both transports. The one discrepancy found (§2.3, response `dir` byte) is
between this repo's **own code** and this repo's **own capture** — not a cross-source conflict,
but a self-consistency gap worth the same rigor.

---

## 7. Chosen next step (branch 1 of 3)

**An existing Pro Controller 2 capture with real NFC traffic exists in this repo
(`usbpcaptures/genuine_procon_2.pcapng`) and has now been re-mined for it** — this selects branch
1 ("reproduce its exact protocol map offline and identify what this repository can validate")
over branch 2 (Joy-Con-2-only evidence — not the case; we have Pro Controller 2 evidence directly)
or branch 3 (no actionable evidence — not the case).

**What this repository can validate from this pass, without new hardware:**
- `switch_pro2.c`'s subcommand-`0x0C` response is now traced to its exact source packets, not
  merely "known to be capture-derived" — fully closed, no further action needed.
- `switch_pro2.c`'s response `dir` byte (`0x01` for every response, unconditionally) conflicts
  with the one genuine bare-ack response observed (`dir=0x04` for subcommand `0x01`, and for an
  unrelated `cmd=0x08` response in the same window) — a precise, named validation target for
  either a future capture-driven fix or a targeted hardware test to see if it matters in practice.
- Subcommand `0x01`'s behavior (bare ack, no payload) is now a documented fact rather than an
  open "Unknown" — narrows, but does not close, ndeadly's own subcommand table.

---

## 8. Closing summary (per DATA.md's required format)

**(a) Strongest existing Pro Controller 2 NFC evidence:** the two genuine, packet-traced
request/response exchanges in `usbpcaptures/genuine_procon_2.pcapng` (§2) — subcommand `0x0C`
(response `61 12 50 10`, matching this repo's own already-implemented hardcoded value exactly)
and subcommand `0x01` (bare `dir=0x04` ack, newly discovered, undocumented anywhere else
consulted this pass). Both are real USB traffic from a device this repo's own code explicitly
identifies as a genuine Pro Controller 2, not inferred or borrowed from another controller type.

**(b) Most important unsupported assumption:** that NFC command framing, subcommand numbering,
and offset conventions are identical across Joy-Con 2 and Pro Controller 2. No source consulted
this pass attributes ndeadly's example bytes to a specific controller type, and this repo has
*zero* Joy-Con-2-specific NFC evidence of its own. Every fact in this document that traces back to
`ndeadly/switch2_controller_research` should be treated as "demonstrated on *some* Switch 2
controller" until re-checked, not "demonstrated on the Pro Controller 2" — only §2's two exchanges
(this repo's own capture) carry that specific attribution.

**(c) One exact next capture/analysis task:** properly filter `genuine_procon_2.pcapng`'s USBPcap
records by endpoint address + transfer type (Interrupt IN on the confirmed HID input endpoint) —
not by first-payload-byte, which §5 showed produces false positives against USB descriptor
traffic — to build a real report-`0x09` time series across the full 164,242-packet capture, and
check whether the NFC-state byte (offset `0x0C`) ever leaves `0x00` anywhere in the session. This
extends `tools/extract_nfc_traffic.py`'s sibling need rather than replacing it.

**(d) Why this has higher information value than implementing NFC from Switch 1 assumptions:**
Switch 1's NFC/IR scheme is already refuted at the command-ID level (claim 6, §1) — there is no
known shared primitive to port. Implementing anything now would mean inventing tag-transaction
semantics (chunking, checksums, mount/unmount) with zero real evidence, on a subsystem this repo
has never exercised even passively. The proposed task instead extracts more *real* Pro-Controller-2-
specific evidence from a capture this repo already has, at zero new-hardware cost, and either (i)
finds a real non-idle NFC-state transition — which would be the first ground truth for what a
live tag interaction looks like on the wire — or (ii) confirms the capture never exercised NFC
beyond the two init-time queries already found, which correctly rules out this file as a source
for tag-transaction semantics and redirects future effort toward capturing a *new* session with a
real amiibo present (the only path that can actually answer the open questions in §4's "not
evidenced at all" list).
