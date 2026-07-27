# amiibo v3 (NTAG I2C Plus 2K) — complete working file

Status: 🔴 **Blocked on one unknown** — the console-facing tag-type signal.
Everything else (storage, persistence, framing, crypto, page serving) is 🔵 built and
verified. Last updated: 2026-07-27.

This is the consolidated working document for Kirby Air Riders "Figure Player" amiibo
support in PicoSwitch2. It supersedes scattered notes; see also
[`switch2/kirby-air-riders-extended-amiibo.md`](switch2/kirby-air-riders-extended-amiibo.md)
for the historical narrative.

---

## 1. What the tag is (Confirmed, from published research)

Primary source: **xSke**,
[N3evin/AmiiboAPI#243 comment 3591686037](https://github.com/N3evin/AmiiboAPI/issues/243#issuecomment-3591686037).
Corroborated by [bettse/amiitool b066e85](https://github.com/bettse/amiitool/commit/b066e85d344355224cd0390064f9b5a995b47b2d)
and [solosky/pixl.js#381](https://github.com/solosky/pixl.js/pull/381) (merged; xSke
verified emulation on a real Switch 2 with the game).

| Property | Value |
|---|---|
| Chip | NTAG I2C Plus 2K — 2048 bytes = sector 0 (1024) + sector 1 (1024) |
| `GET_VERSION` | `00 04 04 05 02 02 15 03` (vs NTAG215 `00 04 04 02 01 00 11 03`) |
| UID | 7 contiguous bytes, **no BCC interleave**; `data[7]=0x00`, `data[8]=0x44` |
| v3 delta | **64 bytes inserted at `0x80`**; encrypted HMAC `0x80`→`0xC0`, data `0xA0`→`0xE0`, data ends page `0x92` (`0x248`) |
| Keys | **Standard retail amiibo keys, unchanged** |
| Identity | `0x54`; **format byte `0x5B` = `0x03`** (standard amiibo = `0x02`) |
| Config pages | `0xE2`–`0xE6` (moved from the NTAG215 positions) |
| SRAM buffer | pages `0xF0`–`0xFF` = bytes `0x3C0`–`0x3FF` |
| Kirby game data | pages `0x9A`–`0xA1` and `0x11A`–`0x131`, encrypted with **unobtainable keys** |

**The machine ("Warp Star" etc.) exists only in the SRAM block.** The amiibo ID is
rider-only — Kirby & Warp Star and Kirby & Winged Star are both `1F00000004C41E03`.
The SRAM block is authenticated by an unknown checksum over its first 26 bytes
(flipping any bit there causes a read error; padding beyond it is free).

**RF-level SRAM handshake** (what a real reader does — *not* available to us, see §3):
`FAST_WRITE` a request block to `0xF0`–`0xFF` → poll page `0xED` until `NS_REG`
byte 2 bit 3 (`SRAM_RF_READY`) is set → `FAST_READ` `0xF0`–`0xFF`. Genuine dumps store
that bit **clear** (`0x21`); emulators raise it dynamically (`0x29`).

---

## 2. What we have built and proven ✅

- **Storage**: 2048-byte v3 slot, flash-persistent in the existing journal banks,
  mutually exclusive with the 540/572 store. Survives power cycles.
- **Portal**: import, content-keyed per rider+machine, carousel collapse, Swap Combo,
  upload/eject, owner/nickname decryption via user-supplied keys.
- **Framing**: our serve is byte-identical to a genuine 540 read — 9 chunks at offsets
  `0,70,…,560`, final `last=1 len=40`, 600-byte buffer, **0 byte mismatches** verified
  against the source dump.
- **Descriptor-driven serving**: we now parse the console's `0x06` page ranges and
  return exactly those pages.
- **Crypto equivalence** (`tools/test_ns2_v3_compat_view.mjs`, all 16 dumps): removing
  the inserted 64-byte block yields a 540-byte image for which
  `tag_to_internal(compat, v3=false)` is **byte-identical** to
  `tag_to_internal(full_v3, v3=true)`. So a console validating our compat view as a
  plain NTAG215 computes exactly the HMACs the tag was signed with.
- **RE tooling**: `v3mode` (source view) and `v3probe <index> <hex>` (live overlay of
  the `0x05` status payload), both switchable over UART with the dongle attached to
  the console — no reflash per experiment.

---

## 3. Why this is hard: we are the controller, not the tag

pixl.js emulates the **tag over RF**. The reader (a genuine controller) issues
`GET_VERSION`, learns the chip is a 2K part, and then does sector selects and the SRAM
handshake itself.

PicoSwitch2 emulates the **controller**. The console tells *us* which pages to fetch.
It decides that page set from a tag type it believes it already knows — and it always
believes NTAG215. Nothing in pixl.js applies to our layer; the RF commands
(`SECTOR_SELECT`, `FAST_READ`, `FAST_WRITE`) never appear on our wire.

---

## 4. Console-facing protocol map (Confirmed)

Command `0x01` = MCU. Subcommands (Narr the Reg; confirmed on our own traces):

| Sub | Meaning |
|---|---|
| `0x03` | start polling — data `00 <timeout u16le> <interval u16le>` |
| `0x04` | stop polling |
| `0x05` | MCU/NFC state — 61-byte payload |
| `0x06` | read device (issue tag read) |
| `0x08` | write device |
| `0x14` | write buffer |
| `0x15` | read buffer — `[last:u8][len:u16le][data ≤70]` |

**`0x06` descriptor** (verified against german77/JoyconDriver's dissector):

```
D0 | uid_len(1) | uid(7) | McuTagType @[9] | block_count | (start_page,end_page) × N
d0   07           <uid>    01                03            00-3b, 3c-77, 78-86
```

`parse_mcu_tag_type()` in that dissector knows **only** `0x01 = NTAG 215`. Pages
`0x00`–`0x86` = 135 pages = **exactly 540 bytes**.

**Read buffer** = 60-byte prefix + tag data (genuine capture,
`experiments/pro2-native-nfc-read-2026-07-25.md`):

| Off | Size | Meaning |
|---:|---:|---|
| 0 | 4 | `04 00 00 00` |
| 4 | 4 | identity/type `01 02 00 07` |
| 8 | 7 | UID |
| 19 | 32 | originality signature |
| 51 | 9 | echo of `0x06` bytes 10–18 |
| 60 | 540 | tag image |

**Read loop**: the console pulls 70-byte chunks until it receives `last=1`, with a hard
cap of **15 chunks / offset < 1024** (~1050 bytes). Proven twice: given a 2108-byte
buffer it stopped at chunk 15 without `last=1`.

---

## 5. The `0x05` status payload — our field map 🔵

Genuine bytes we send: `09 00 00 00 01 01 02 00 07 <uid×7>` then 45 zeros.

| Byte | Genuine | Finding | Evidence |
|---|---|---|---|
| `[0]` | `09` | NFC status (`09` ready → `04` during read) | Narr; our traces |
| `[1]` | `00` | detail | |
| `[2..3]` | `00 00` | parsed — part of the crashing region | FF-fill crash |
| `[4]` | `01` | parsed; `02` → **hard crash**. Plausibly *tag count* (2 makes it parse a second record from zeros) | probe |
| `[5]` | `01` | changing to `02` had **no effect** on the descriptor | v3mode sweep |
| `[6]` | `02` | untested individually | |
| `[7]` | `00` | **gates read behaviour** — see matrix below | probe |
| `[8]` | `07` | UID length | |
| `[9..15]` | UID | | |
| `[16..60]` | zeros | **IGNORED — proven** | all 45 set to `FF`: descriptor and read byte-identical |

### `status[7]` behaviour matrix (new, this session)

| Value | Console behaviour |
|---|---|
| `00` (genuine) | Issues `0x06` (`McuTagType 01`, 540-byte page set), reads 9 chunks |
| `01` | **Hard crash 2011-0301**, immediately after the status reply, before any `0x06` |
| `02` | **Never issues a read** — loops poll → status → stop, no crash |
| `03` | **Hard crash 2011-0301**, same as `01` |

Odd values crash, `02` is "understood but unreadable". This byte is real and load-bearing;
its 2 KB value (if it exists) is not yet found. The tag-count reading of `[4]` explains
why `[16..60]` is dead: with one record, the tag descriptor occupies exactly `[5..15]`
(`protocol, type, ?, uid_len, uid×7` = 11 bytes).

---

## 6. Full experiment log

| # | Configuration | Console result |
|---|---|---|
| 1 | Stub: `0x06` bare-ACK | Infinite stall, retries every 3 s, never reads |
| 2 | Drive state machine; prefix + 2048 (2108 B) | 15 chunks, never `last=1` → **2115-0176 "not an amiibo"** |
| 3 | Raw sector 0 (1024), `last=1` @1024, no prefix | 15 chunks, complete read → **crash 2011-0301** |
| 4 | Raw full 2048, no prefix | Still 15 chunks, never asks chunk 16 → crash. **Proves the 1050-byte cap** |
| 5 | System Settings, same config | Identical read pattern, crash — not app-specific |
| 6 | `SRAM_RF_READY` bit injected at `0x3B6` | No change (correct per xSke, but not the blocker) |
| 7 | Descriptor-driven, raw v3 pages `00-86` | 9 chunks, `last=1`, **genuine framing** → "not an amiibo" |
| 8 | Descriptor-driven, **compat540 view** | 9 chunks, `last=1`, byte-perfect → **"not an amiibo"** |
| 9 | `status[16..60]` = all `FF` | **Zero change** — region ignored |
| 10 | `status[2..7]` = all `FF` | Crash right after status reply |
| 11 | `status[4]` = `02` | Hard crash |
| 12 | `status[7]` = `01` / `02` / `03` | crash / no-read / crash |

Traces preserved in `dumps/` (`v3probe-*`, `kirby-v3-serve-trace-*`,
`amiibo-540-working-read-2026-07-26.jsonl`).

---

## 7. Hypotheses (ranked)

**H1 — The console refuses a v3-format amiibo delivered as a 540-byte NTAG215.** 🔵 Strong.
Experiment 8 served a cryptographically perfect standard amiibo (proven byte-identical
internal buffer) in genuine framing, and was still rejected. The most likely
discriminator is the identity **format byte `0x5B = 0x03`**: the console reads it,
concludes the tag must be a 2 KB part with extended data, sees it was read as an
NTAG215, and rejects. This is *signed* data, so it cannot be rewritten.
→ **Implication: there is no path to success without the tag-type signal.**

**H2 — The tag type is not carried in the `0x05` status at all.** 🔵 Moderate–strong.
`[16..60]` are provably ignored, and `[7]`/`[4]` deviations crash rather than
re-target the read. A field the console *derives* a page set from would more likely
produce a changed descriptor than a fatal. Candidate alternative channels: the
**`0x03` polling response** and the **`0x0C` command** (Narr: "unknown") — we
bare-ACK both with an empty payload; a genuine controller may return tag-identity data
there. **Untested — highest-value next experiment.**

**H3 — `status[7]` is a capability/type field whose 2 KB value we haven't hit.** 🔵 Weak–moderate.
`02` = recognized-but-unreadable is suggestive. Sweeping is possible but each wrong
value costs a console hard-crash + dongle reboot + amiibo reload, and the space is
unbounded.

**H4 — The Pro Controller 2 NFC path may not support 2 KB tags at all.** ⬜ Unknown.
xSke's testing was explicitly "S2 with JC1" (Joy-Con 1). We have never confirmed a
genuine PC2 reads a Kirby amiibo. If the console only drives the extended read for
certain controller NFC firmware, PC2 emulation may be a dead end and a **Joy-Con 2
personality** (we already have `JOYCON2_L/R`) could behave differently.

**H5 — Machine/Figure Player is unreachable regardless.** ✅ Near-certain.
It lives only in the SRAM block, which is delivered by an RF handshake that has no
representation on the controller↔console wire we can see. Even a fully working rider
read would likely present the amiibo without its machine.

---

## 8. Next experiments (ordered by value/cost)

1. **Probe the `0x03` and `0x0C` response payloads** (H2). Requires a firmware change:
   generalize `v3probe` to attach arbitrary payloads to *any* subcommand response, then
   sweep. Cheap once built, and covers the only untested channels. **Do this first.**
2. **Increase trace capture depth** beyond 24 bytes so full 61-byte status replies and
   longer descriptors are visible in one pass (currently truncated).
3. **Joy-Con 2 personality NFC test** (H4). Switch personality and observe whether the
   console's NFC negotiation differs at all. Cheap, purely observational.
4. **`status[6]` and `[2..3]` single-byte sweeps** — the last unmapped bytes in the
   parsed region.
5. **`status[7]` extended sweep** (`04`…`0F`) — only if 1–4 are exhausted; expensive.

## 9. Offline / portal ideas (independent of the console blocker)

- **Decrypt with the user's retail keys** to confirm the format byte, dump register
  info, and inspect the Kirby extended regions. Our portal already implements the full
  amiitool port with the v3 `+0x40` shift, and `tools/test_amiibo_decrypt.mjs` proves
  the round-trip. Worth dumping a decrypted v3 image to see exactly which fields the
  console would read.
- **Mii rendering** ([Stewared/MiiJS](https://github.com/Stewared/MiiJS)) — we already
  extract the 96-byte owner Mii at internal `0x4C`; MiiJS could render it in the portal
  instead of showing only the name.
- **SRAM/machine catalogue**: collect SRAM blocks per machine (the community has a few)
  and document the 26-byte authenticated prefix. Even without console support this
  makes the dataset complete for future work.

## 10. Hard constraints (do not re-litigate)

- Kirby's own game data (pages `0x9A`–`0xA1`, `0x11A`–`0x131`) is encrypted with keys
  that require a Switch 2 softmod. **Not recoverable.** ✅ Confirmed by xSke.
- Do **not** re-add a 60-byte prefix in front of a 1024-byte payload: the total exceeds
  the console's ~1050-byte read window, `last=1` never arrives, and it regresses to
  "not an amiibo".
- The amiibo identity/format bytes are inside the signed region — they cannot be
  rewritten to masquerade as v2.
