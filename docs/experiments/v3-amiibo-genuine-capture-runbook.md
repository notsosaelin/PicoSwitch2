# Runbook — capturing a genuine controller reading a tag

**Status: ✅ Run 2026-07-27, and reused for every genuine capture since.** The procedure below is
current and reusable; §5 records what the original v3 capture answered.

**Purpose.** Observe a genuine Pro Controller 2 read a genuine tag through the `nfcmirror` bridge,
so a question about the console↔controller wire can be answered from a real controller's replies
instead of from our own serve path. Originally written to capture an NTAG I2C Plus 2K ("figure v3")
amiibo; it applies unchanged to any tag.

---

## 1. What the original capture answered

The question was: *when a genuine controller reads a genuine v3 amiibo, what does it report
differently — and where — such that the console asks for the v3 page set instead of the NTAG215
one?*

**Answer: read-buffer prefix byte 18.** A genuine controller emits `0x06` there for a v3 tag and
`0x00` for an NTAG215, and that byte alone makes the console escalate from the 3-block 540-byte
descriptor to a 4-block 604-byte one. The `0x05` status is byte-identical between the two tag types
except for the UID, so it can never carry the signal. Full protocol write-up:
[`../Amiibo-v3.md`](../Amiibo-v3.md) §3.

Captures: `dumps/v3-genuine-capture-2026-07-27.jsonl` (206 records) and
`dumps/ntag215-genuine-capture-2026-07-27.jsonl` (36 records, the control). Both `overwritten: 0`;
bridge health `rejected: 0`, `timeouts: 0`.

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

Always capture an **ordinary NTAG215 tag as a control** and diff the two. Either capture alone is
much weaker evidence — that diff is what isolated prefix byte 18.

Compare, in this order:

1. **The `0x06` descriptor `block_count` and `(start,end)` page ranges.** A different set for the
   tag under test is a direct answer the serve path can reproduce.
2. **The 60-byte read-buffer prefix**, byte by byte. This is the field *we* synthesize, so any
   difference here is something we can emit.
3. **The `0x05` status bytes 4–8.** These are an NCI `RF_INTF_ACTIVATED_NTF` passthrough from the
   controller's PN7160 and carry no tag-type information, but capturing them rules the channel out
   rather than assuming it.
4. **Anything after the read** — device commands (`0x14`/`0x21`), extended operations (`0x20`),
   or sector-aware reads (`0x1E`) appear here and nowhere else.

## 6. Notes and hazards

- **One scan per capture.** The ring buffer is 256 records; a repeated scan risks wrap and the
  beginning of a session is the informative part.
- **The mirror is a single-slot bridge.** It forwards one command at a time and waits for the
  reply. That is fine at human scan pace but will reject under a burst — hence checking `rejected`.
- **Turn the mirror off afterwards** (`nfcmirror off`). It is inert when disarmed, but leaving it
  armed means the genuine controller keeps owning NFC.
- **Do not re-run the refuted approaches** while the amiibo is in hand. The refuted-claims table in
  [`../Amiibo-v3.md`](../Amiibo-v3.md) §14 lists them; serving a v3 tag as a 540-byte amiibo (compat
  view or re-signed) is the most tempting one and is wrong in principle.

## 7. Preparation done before the first run (2026-07-27)

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
