# Switch 2 Controller NFC / Amiibo Protocol Inventory

> Archived 2026-07-25. Preserved for its exact 2026-07-12 packet-mining record. Use
> [`../switch2/nfc-protocol-inventory.md`](../switch2/nfc-protocol-inventory.md) for the current
> evidence map.

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
| 5 | Protocol behavior demonstrated on Pro Controller 2 | ✅ **Confirmed, this repo** | `usbpcaptures/genuine_procon_2.pcapng` (this repo's own capture) contains two real command-`0x01` request/response exchanges — see §2. **Device number corrected 2026-07-12** (see §2.5): the Pro Controller 2 in *this specific file* is USBPcap `device` 38, not 7 — "device 7" is a fact about a *different* file (`ndeadly`'s `captures/usb/rumble-procon-gccon.pcapng`, cited in `switch_pro2.c`'s header comment) that had been mistakenly cross-applied to this one. |
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

**✅ Fixed 2026-07-12.** `ns2_dispatch()`'s `case 0x01` (NFC) now sends `dir=0x04` for its bare/
no-data-payload fallback path, matching the genuine capture exactly, while `sub == 0x0C` (which
carries a 4-byte payload) keeps `dir=0x01`. Scoped strictly to the NFC command per the discipline
below (the "two data points across two top-level commands" hypothesis is *not* generalized to
`0x08` or any other command — those already work on hardware and were left untouched). Still
**untested on hardware** — NFC has never been exercised in play in this project, so this is a
byte-exact-per-capture fix with no observable on-console symptom either way.

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

Both exchanges occur within an 18-packet window (#30520-#30538), interleaved with the host's
mandatory init sequence — nearby packets carry `cmd=0x03/sub=0x0A` (report-select — **corrected
2026-07-12, see §2.5: this packet's actual payload selects report `0x05`, not `0x09`** as
previously stated here) and `cmd=0x08` (Charging Grip, per this repo's own command table)
requests. This matches `docs/switch2/usb-spec.md`'s existing note that command `01/0c` NFC appears
"interleaved (order not strict)" with the init handshake. **New precision this pass:** across all
164,242 packets in the capture, these are the *only* two NFC-command exchanges present — NFC
status is queried exactly once per connection during setup, not polled during normal operation.
(This does not rule out polling during an actual amiibo interaction — this capture almost
certainly contains no amiibo tap; see §5.)

### 2.5 Correction (2026-07-12): `genuine_procon_2.pcapng` is a PC/Windows session, not a console
session — two documentation errors fixed, and this file cannot answer the report-0x09 NFC-state
question

Built a proper USBPcap-header-aware tool (`tools/extract_report09_timeseries.py`, parses the real
27-byte `USBPCAP_BUFFER_PACKET_HEADER` struct — `headerLen`/`irpId`/`status`/`function`/`info`/
`bus`/`device`/`endpoint`/`transfer`/`dataLength` — rather than scanning payload bytes) to execute
the task queued since 2026-07-10 (§7 below, `STATUS.md`/`PLAN.md` "Next Recommended Tasks"): build
a real report-`0x09` time series and check whether the NFC-state byte ever leaves `0x00`. Two real
errors surfaced in the process, both now fixed here:

1. **Device-number conflation, fixed.** Claim 5's table cell (§1) and this repo's own
   `switch_pro2.c` header comment both say "Pro Controller 2 = device 7" — that fact is true of
   **`ndeadly`'s own capture file** (`captures/usb/rumble-procon-gccon.pcapng`, a different file
   from a different project), not of this repo's `usbpcaptures/genuine_procon_2.pcapng`. A 2026-07-10
   session log entry (`SESSION.md`) incorrectly cross-applied it ("Pro Controller 2 = device 7 —
   i.e. `usbpcaptures/genuine_procon_2.pcapng`"), and that claim propagated into this doc's §1.
   **Verified directly** by parsing the exact packets already confirmed to carry the two NFC
   exchanges (#30520/30526/30528/30532) and reading their USBPcap `device` field: **all four are
   device 38**, not 7 — and their payload bytes match this doc's §2.1/§2.2 hex exactly, confirming
   both the correction and that the new parser is reading the header correctly. No code or prior
   *analysis* used "device 7" as an actual filter against this file (grepped `tools/*.py` to check)
   — the error was confined to prose, so nothing upstream needs re-verification.
2. **Report-select target, fixed.** §2.4 (before this correction) said the nearby `cmd=0x03/sub=0x0A`
   packet "selects report 09." Its actual payload (packet #30539) is
   `03 91 00 0a 00 04 00 00 05 00 00 00` — the report-ID byte (`c[8]` in this project's own
   `ns2_dispatch()` convention) is **`0x05`, not `0x09`**.
3. **Structural finding this corrects both errors point to: this whole capture is a PC/Windows
   session with the real Pro Controller 2, never a console session.** Filtering strictly by USBPcap
   header fields (`endpoint == 0x81`, `transfer == 1` Interrupt, `dataLength > 0` — i.e. real HID
   IN completions on the confirmed input endpoint, not payload-content guessing) across all 164,242
   packets finds **19,554 report-`0x05` records for device 38 and exactly zero report-`0x09`
   records for any device.** The only four places byte `0x09` appears as a leading payload byte
   anywhere for device 38 are non-HID coincidences: two USB Configuration Descriptor reads on EP0
   (`bLength=0x09` — the *exact* collision §5 already documented, just recurring at different
   packet numbers) and two command-channel (`cmd=0x09` = Player LEDs, per `usb-spec.md`) request/ack
   pairs. **Consequence: the original task ("does the NFC-state byte in report 0x09 ever leave
   idle across this session") cannot be answered from this file, structurally — report `0x09` is
   simply never present.** This is a conclusive negative result, not a tooling failure: this
   project's real console-only USB capture gap (already the long-standing blocker for report-0x09
   gyro work, `docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`) applies equally to NFC.
   Answering "does NFC state ever change on the console" needs an actual console-side USB capture of
   a genuine controller — a capture type this project has never obtained for *any* purpose. The
   19,554 real report-`0x05` samples remain a genuine, newly-quantified asset for anything that
   *does* use report `0x05` (e.g. `PLAN.md`'s report-0x05 roll-sign verification task) — not applied
   to that here, out of scope for this pass.

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
| `0x01` | "Unknown" | 🔵 Partial | Capture-confirmed bare-ack response (`dir=0x04`, no payload) on genuine Pro Controller 2, USB. Semantic meaning (start scan? reset? get-state trigger?) still unknown. |
| `0x02` | — (not listed) | ⬜ Unknown | No evidence found. |
| `0x03` | "Unknown" | 🔵 Hypothesis (2026-07-12) | `Dycool/NS-PC-Control` implements this as "enter NFC scan mode" (schedules an HID-state advance ~40ms later per their cited capture). Unverified — their captures aren't bundled/re-checkable. See `docs/experiments/ns-pc-control-audit-2026-07-12.md` §2. |
| `0x04` | "Unknown" | 🔵 Hypothesis (2026-07-12) | NS-PC-Control: "leave NFC scan mode," conditionally ejects the virtual tag. Same caveat as `0x03`. |
| `0x05` | "Get status" | 🔵 Hypothesis (2026-07-12) | NS-PC-Control: 61-byte status payload (status byte, detail byte, 7-byte UID when a tag is present). Same caveat. |
| `0x06` | "Read device" | 🔵 Hypothesis (2026-07-12) | NS-PC-Control: "begin read/write operation" — a zero UID in the `D0 07 ...` request selects read mode, a matching UID selects write mode. Same caveat. |
| `0x08` | "Write device" | 🔵 Hypothesis (2026-07-12) | NS-PC-Control: commits a staged `0x14` write image; status becomes `0x05` afterward. Same caveat. |
| `0x0C` | "Unknown" | ✅ Confirmed (this repo) | Real request/response traced to exact packets (#30520/#30526); response `61 12 50 10` already implemented in `switch_pro2.c` and now shown to be genuinely capture-sourced, not guessed. **Semantic meaning of the 4 bytes is still unknown** — plausible-but-unconfirmed hypothesis: an NFC controller IC identifier/version tag (common 4-byte chip-ID+rev pattern); no NFC IC datasheet cross-check performed. |
| `0x14` | "Write buffer" | 🔵 Hypothesis (2026-07-12) | NS-PC-Control: 454-byte staging image (`D0 07` header + UID + lock bytes + page-record count + `(page,length,data)` records, pages 5-129 only), sent as 6 chunks. Same caveat. |
| `0x15` | "Read buffer" | 🔵 Hypothesis (2026-07-12) | NS-PC-Control: 622-byte payload (63-byte metadata incl. UID + NTAG originality signature + 9 bytes echoed from the preceding `0x06` + 540-byte raw NTAG215 dump + 19-byte trailer) — a concrete answer to ndeadly's previously-undocumented response field. Same caveat: unverified, not ported to this repo's code. |

**Not evidenced by this repo's own primary sources, any pass:** tag detect/mount/unmount
transitions, a real amiibo read (NTAG215 540-byte EEPROM per general amiibo knowledge — not
Switch-2-specific evidence), a real amiibo write, checksums/authentication/framing for multi-chunk
transfers, timing/latency requirements for tag operations. **2026-07-12 update:** `Dycool/
NS-PC-Control` has detailed, internally-consistent answers for all of the above (§4 table) — but
sourced from private captures this project cannot re-verify. Treat as a structured hypothesis to
test against, not as filling this gap. The gap remains genuinely open until this project captures
its own real amiibo transaction.

---

## 5. A methodology dead end, documented so it is not repeated

Attempted: scan every report-`0x09` HID packet in the capture for a nonzero NFC-state byte
(offset `0x0C`), reasoning that a real amiibo tap during the capture session would show up as a
state transition. Filter used: `payload[0] == 0x09` (report ID) on decoded USBPcap payloads.
**Result was a false lead**: only 17 packets matched, two with a nonzero "state" byte — but
inspection showed all 17 are USB **Configuration Descriptors** (`bLength=0x09, bDescriptorType=0x02`),
not HID input reports at all; `0x09` is simultaneously a valid report ID and a valid descriptor
`bLength`, and the filter didn't distinguish them. **Correct approach, implemented 2026-07-12**
(§2.5): `tools/extract_report09_timeseries.py` filters by real USBPcap header fields (endpoint
address + transfer type = Interrupt IN on the confirmed HID input endpoint), not by
first-payload-byte. Result: report `0x09` never appears anywhere in this capture (it's a
PC/Windows session — see §2.5 for the full finding), so the original question is unanswerable from
this file, not merely hard to answer.

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
- `switch_pro2.c`'s response `dir` byte for NFC bare acks — **fixed 2026-07-12** (§2.3): now sends
  `dir=0x04`, matching the one genuine bare-ack response observed. Scoped to NFC only; the same
  `dir=0x04` shape on the unrelated `cmd=0x08` response in the same capture window was *not*
  generalized (two data points, one command each — not a confirmed universal rule). Still needs a
  hardware pass to confirm it has any observable effect (NFC has never been exercised in play).
- Subcommand `0x01`'s behavior (bare ack, no payload) is now a documented fact rather than an
  open "Unknown" — narrows, but does not close, ndeadly's own subcommand table.
- **2026-07-12:** the report-`0x09` NFC-state time series task (§2.5) is now resolved as a
  conclusive **negative-but-informative** result — not "not yet done." `genuine_procon_2.pcapng`
  contains zero report-`0x09` records (it's a PC/Windows session with the real controller, which
  only ever streams report `0x05`); the console-only NFC-state question needs an actual
  console-side USB capture, a capture type this project has never obtained for any purpose. Two
  incidental documentation errors (a device-number mix-up between this file and a different,
  external capture; a mis-read report-select target) were found and fixed in the process — see
  §2.5 for both.

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

**(c) One exact next capture/analysis task — done 2026-07-12, result is a hard requirement, not a
technique to try:** properly filtering `genuine_procon_2.pcapng`'s USBPcap records by endpoint
address + transfer type (not first-payload-byte, which §5 showed produces false positives against
USB descriptor traffic) proves report `0x09` never appears in this capture at all — it's a
PC/Windows session, and the real controller only streams report `0x05` to a PC host. **The actual
next task is now capture acquisition, not analysis technique**: answering whether NFC state ever
changes on the console requires an actual console-side USB capture of a genuine controller, which
this project has never obtained (the same evidence gap already blocking report-0x09 gyro work —
`docs/experiments/usb-relay-feasibility-audit-2026-07-10.md` remains the most-developed path
toward getting one). `tools/extract_report09_timeseries.py` (new) is ready to run against such a
capture the moment one exists.

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
