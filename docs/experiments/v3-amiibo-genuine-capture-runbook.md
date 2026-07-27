# Runbook — capturing a genuine controller reading a v3 amiibo

**Purpose.** Observe a genuine Pro Controller 2 read a genuine NTAG I2C Plus 2K ("figure v3")
amiibo, once. That single capture is the one thing blocking v3 support — see
[`../Amiibo-v3.md`](../Amiibo-v3.md) §13.

**Status:** ⬜ prepared, not yet run. Everything below was dry-run against live hardware on
2026-07-27 except the amiibo itself.

---

## 1. What this is trying to answer

The console decides which tag pages to request **before** any read, and always asks for the NTAG215
set (`00-3b, 3c-77, 78-86` = 540 bytes). A v3 tag's encrypted region ends at `0x248` (584 bytes), so
that read can never validate one.

Every field the *controller* can influence has already been eliminated with evidence (Amiibo-v3.md
§13). So the question is narrow:

> When a **genuine** controller reads a **genuine** v3 amiibo, what does it report differently —
> and where — such that the console asks for the v3 page set instead of the NTAG215 one?

The two records that matter most are the `0x05` **status** (the NCI `RF_INTF_ACTIVATED_NTF`
passthrough from the controller's PN7160) and the `0x06` **read-device descriptor**
(`D0 | uid_len | uid | McuTagType | block_count | (start,end)×N`). The descriptor carries the page
ranges directly. Everything else is corroboration.

## 2. Preconditions

| Requirement | Why |
|---|---|
| Dongle flashed with a build at/after `1a4f2fd` | Needs the 256×128 trace buffer and the mirror-first dispatch order |
| USB-TTL adapter on the dongle's UART0 (here `COM11`) | The whole session is driven over UART |
| Dongle attached to the Switch 2 | The console must be the one issuing NFC commands |
| A **genuine Pro Controller 2** paired to the dongle over BT | The mirror forwards to it and returns its real replies |
| A genuine v3 amiibo (e.g. a Kirby Air Riders rider **on its machine**) | The rider alone does not scan — the machine is the antenna and holds the I2C device |

**No longer required:** ejecting the virtual slot. The mirror is now tried *before* the local serve
path, so when it is armed the genuine controller owns NFC regardless of what is loaded. (Previously
the local runtime was consulted even with an empty slot and could answer first, which would have
produced an empty capture.)

## 3. Sequence

Run from the repo root. `-Port` is whatever `.\tools\read_uart_diag.ps1 -List` reports.

```powershell
# 1. Confirm the genuine controller is connected and the bridge can see it
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'nfcmirror on'
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'nfcmirror status'
#    require: "active":true, "state":2 (ACTIVE), source_pid 0x2069

# 2. Arm the tracer. The NFC filter keeps the 1 kHz HID stream from
#    overwriting a human-paced session.
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'trace clear'
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'trace start nfc'

# 3. On the console: open the amiibo reader and scan the v3 amiibo.
#    Do this ONCE. Then immediately:

.\tools\read_uart_diag.ps1 -Port COM11 -Command 'trace stop'
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'trace dump' `
    -OutputPath dumps\v3-genuine-capture-<date>.jsonl -TimeoutMs 60000

# 4. Capture the bridge's own counters alongside it
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'nfcmirror status'
.\tools\read_uart_diag.ps1 -Port COM11 -Command 'nfcmirror off'
```

**For comparison, repeat the whole sequence with an ordinary NTAG215 amiibo.** The diff between the
two captures is the actual result; either one alone is much weaker evidence.

## 4. How to tell the capture is good

From `trace status` / the dump footer:

- `overwritten: 0` — nothing was lost to ring-buffer wrap. If nonzero, the session was too long or
  too chatty; clear and retry with a shorter scan.
- Records present with `sub: 5` (status) and `sub: 6` (read device). Without a `0x06` the console
  never got as far as describing the tag, and the capture cannot answer the question.
- `captured == length` on the records of interest. Payload is 128 bytes, matching
  `NS2_NFC_MIRROR_RESPONSE_MAX`, so nothing mirrored should truncate — if `captured < length`
  anywhere, note it, because the limit needs raising again.

From `nfcmirror status`:

- `sent` ≈ `submitted`, `rejected: 0`, `timeouts: 0`. A large `rejected` count means commands were
  arriving faster than the single-slot bridge could forward them, and the capture has holes.

## 5. Reading the result

Compare v3 against NTAG215, in this order:

1. **`0x06` descriptor `block_count` and the `(start,end)` page ranges.** If the genuine controller
   reports a different set for v3, that is the answer outright and the serve path can reproduce it.
2. **`0x06` `McuTagType`.** The local path currently emits the NTAG215 value; a different one here
   is the tag-type signal that was hypothesised but never observed.
3. **`0x05` status bytes 4–8.** Already probed exhaustively from our side (`status[6]` must be
   `0x02`, others crash or abort), but this shows what a genuine PN7160 actually sends for a 2K tag
   rather than what survives our guessing.
4. **Anything after the read** — if the console issues an SRAM sequence for the machine block, it
   will appear here and nowhere else.

## 6. Notes and hazards

- **One scan per capture.** The ring buffer is 256 records; a repeated scan risks wrap and the
  beginning of a session is the informative part.
- **The mirror is a single-slot bridge.** It forwards one command at a time and waits for the
  reply. That is fine at human scan pace but will reject under a burst — hence checking `rejected`.
- **Turn the mirror off afterwards** (`nfcmirror off`). It is inert when disarmed, but leaving it
  armed means the genuine controller keeps owning NFC.
- **Do not re-run the refuted approaches** while the amiibo is in hand. Serving a v3 tag as a
  540-byte amiibo (compat view or re-signed) is documented as rejected in Amiibo-v3.md §13 — both
  produce cryptographically valid tags and both are wrong in principle.

## 7. Preparation done for this runbook (2026-07-27)

- **Trace buffer raised** from 128×72 to **256×128**. The former 72-byte payload would have
  truncated read-buffer replies, which can reach 128 bytes; the former 128-record capacity risked
  wrapping mid-session. Cost ~26 KB bss; measured headroom afterwards is ~296 KB free on RP2350 and
  ~108 KB on RP2040.
- **Dispatch order corrected** so `ns2_nfc_mirror_submit()` runs before
  `ns2_virtual_nfc_dispatch_usb()`. The local runtime was consulted even with an empty slot and
  could answer the console first, which would have yielded an empty capture. The mirror returns
  false immediately unless armed over UART, so ordinary operation is unchanged.
- **Dry-run on live hardware:** `nfcmirror status`, `amiibo status`, `trace status`,
  `trace start nfc`, `trace dump -OutputPath`, `trace stop`, `trace clear` all verified working
  against the attached dongle.
