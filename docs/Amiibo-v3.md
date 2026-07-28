# amiibo v3 (NTAG I2C Plus 2K) — complete working file

Status: 🔴 **PARKED 2026-07-27** — blocked on one unknown: the console-facing
tag-type signal. Everything else (storage, persistence, framing, crypto, page
serving) is ✅ built, verified and left in place. Resume from §13.

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

---

## 11. BREAKTHROUGH (2026-07-27): `status[7]` is the NTAG **model** byte

Found by cross-referencing **CTCaer/jc_toolkit** (`jctool.cpp`), which implements the
Switch 1 MCU NFC read. Its tag-detected parser:

```
buf[56]: MCU/NFC state   (0x09 = Tag detected)
buf[62]: nfc tag IC      (0x02 = NTAG, else MIFARE)
buf[63]: nfc tag Type
buf[64]: UID size
buf[65..]: UID
```

Our Switch 2 `0x05` status payload aligns on **four independent anchors**:

| ours | value | Switch 1 |
|---|---|---|
| `[0]` | `09` | `buf[56]` state = Tag detected |
| `[6]` | `02` | `buf[62]` tag IC = **NTAG** |
| `[7]` | `00` | `buf[63]` tag Type / **model** |
| `[8]` | `07` | `buf[64]` UID size |
| `[9..]` | UID | `buf[65..]` |

And jc_toolkit derives the page count — and therefore the **block ranges** — from the
model byte:

```c
if (tag_type == 2) {                    // NTAG
    switch (model) {
        case 0: ntag_pages = 135; break;   // NTAG215
        case 3: ntag_pages = 45;  break;   // NTAG213
        case 4: ntag_pages = 231; break;   // NTAG216
    }
}
```

| pages | block ranges built |
|---|---|
| 45 | `00-2c` |
| **135** | **`00-3b, 3c-77, 78-86`** ← byte-identical to what our console requests |
| 231 | `00-3b, 3c-77, 78-b3, b4-e6` |

**This closes the loop.** We report model `0x00`, so the console builds the NTAG215
135-page (540-byte) read — the exact ranges we have seen in every single trace. It
explains the whole `status[7]` matrix:

| `[7]` | model | observed |
|---|---|---|
| `00` | NTAG215, 135 pages | reads 540 B ✓ |
| `01` | invalid | hard crash |
| `02` | invalid | refuses to read |
| `03` | NTAG213, 45 pages | crash — 180 B is too little to parse as an amiibo |
| **`04`** | **NTAG216, 231 pages = 924 B** | **NEVER TESTED** |

### Why `[7] = 0x04` is the experiment to run

- 231 pages = **924 bytes**, and the v3 amiibo crypto region ends at `0x248` (584 B) —
  fully covered, unlike the 540-byte NTAG215 read which falls 40 bytes short.
- 60-byte prefix + 924 = **984 bytes**, comfortably under the console's measured
  ~1050-byte read window, so `last=1` will land properly.
- Serve **raw v3 pages** (`v3mode 0`) and the console's own documented
  backwards-compatibility path — xSke: *"the NFC sysmodule does have handling for
  skipping the extra data chunks and reading standard Amiibo data from the right
  offsets"* — can do the `0x40` unshift itself.
- Config pages `0xE2`–`0xE6` (which v3 moved) sit at bytes `0x388`–`0x39B`, **inside**
  the 924-byte window.

**Commands:** `v3mode 0` then `v3probe 7 04`, scan, dump. Expect the `0x06` descriptor
to change to **4 blocks** ending `b4-e6` — that alone confirms the mechanism even if
validation still fails.

**Caveat:** the NTAG216 range stops at page `0xE6`; the SRAM buffer (`0xF0`-`0xFF`)
is still not requested, so the machine/Figure Player remains out of reach. Rider
recognition is the realistic win. If a dedicated NTAG I2C 2K model value exists it
would be some value > 4, and the same probe sweeps it.

### Crypto is fully ruled out (verified with the owner's `key_retail.bin`)

| dump | v3 offsets | compat540 view |
|---|---|---|
| all four riders | **HMAC VALID** | **HMAC VALID** |
| control 540 amiibo | VALID | — |

The dumps are genuinely signed, and the compat view we served was a cryptographically
perfect standard amiibo. The rejection was never about crypto — it was always about
the console not being told to read enough of the tag.


---

## 12. SOLVED MECHANISM (2026-07-27): the model byte is **read-buffer prefix offset 18**

### The evidence

`CTCaer/jc_toolkit` (`jctool.cpp`, Switch 1 MCU NFC read) parses the NFC read response:

```c
else if (buf2[49] == 0x3a && buf2[51] == 0x07) {      // NFCReadData
    if (ntag_init_done) {
        if (buf2[52] == 0x01)                          // first package
            memcpy(dst, buf2 + 116, payload_size - 60); //   skips a 60-byte header
        else
            memcpy(dst, buf2 + 56,  payload_size);
    }
    else if (buf2[52] == 0x01) {                       // FIRST (discovery) read
        if (tag_type == 2) {                           // tag IC == NTAG
            switch (buf2[74]) {                        // <-- MODEL BYTE
                case 0: ntag_pages = 135; break;       // NTAG215
                case 3: ntag_pages = 45;  break;       // NTAG213
                case 4: ntag_pages = 231; break;       // NTAG216
                default: goto step9;                   // abort
            }
        }
    }
}
```

MCU data begins at `buf2 + 56`; the first package skips a **60-byte header** to reach
tag data at `buf2 + 116`. Therefore `buf2[74]` = **header offset 18**.

Our own primary capture (`experiments/pro2-native-nfc-read-2026-07-25.md`) documents
the 60-byte read-buffer prefix as:

| Off | Size | Meaning |
|---:|---:|---|
| 0 | 4 | `04 00 00 00` |
| 4 | 4 | `01 02 00 07` |
| 8 | 7 | UID |
| **15** | **4** | **"zero/reserved"** ← **offset 18 lives here** |
| 19 | 32 | originality signature |
| 51 | 9 | echo of the `0x06` descriptor |
| 60 | … | tag image |

**Conclusion: prefix byte 18 is the NTAG model, we emit `0x00` (= NTAG215 = 135
pages), and that is exactly why the console has requested `00-3b, 3c-77, 78-86`
(540 bytes) in every trace we have ever taken.** The page ranges are downstream of a
byte in a buffer *we generate*.

### Correction: the `0x05` status cannot carry a model

`status[4..8]` = `01 01 02 00 07` is a straight passthrough of the NCI
`RF_INTF_ACTIVATED_NTF` from the controller's **PN7160** NFC front-end
(`ndeadly/switch2_controller_research/datasheets/PN7160_PN7161.pdf`):

| ours | NCI field | value |
|---|---|---|
| `[4]` | RF Discovery ID | `01` |
| `[5]` | RF Interface | `01` = Frame |
| `[6]` | RF Protocol | `02` = **T2T** |
| `[7]` | RF Technology & Mode | `00` = NFC_A passive poll |
| `[8]` | NFCID1 length | `07` |
| `[9..]` | NFCID1 | UID |

This explains the entire `status[7]` matrix — `01`/`02`/`03` are NFC_B / NFC_F /
NFC_A-active, i.e. bogus RF technologies, hence the faults and the refusal to read.
An NTAG I2C 2K is *also* T2T / NFC_A passive, so **the status is identical for both
chips and can never distinguish them.** Every probe of that structure was doomed;
this closes that avenue permanently.

Confirmed independently: ndeadly's `commands.md` shows a genuine Pro Controller 2
`0x05` reply for a real amiibo as
`09 00 00 00 01 01 02 00 07 <uid> 00…` — byte-identical to ours.

### What to do with it

1. Sweep **prefix[18]** (not the status). Known: `0`=NTAG215, `3`=NTAG213,
   `4`=NTAG216. Nintendo added NTAG I2C Plus 2K support for Kirby Air Riders, so its
   value is a **new** enum member — sweep `5, 6, 7, 8, …` (and `1`, `2`).
2. **Success signal**: the console issues a *second* `0x06` whose `block_count` and
   ranges differ from `03 | 00-3b 3c-77 78-86`. Ranges reaching pages `0xE1`+ mean it
   is reading sector 0 as a 2 KB part; ranges touching `0xF0`–`0xFF` mean it has
   entered the **SRAM** path.
3. Once it asks for `0xF0`–`0xFF`, parity is reachable: we already serve arbitrary
   requested page ranges from the full 2048-byte image and already inject
   `SRAM_RF_READY` at `0x3B6`. The remaining work is accepting the SRAM request
   write (`0x08` write device + `0x14` write buffer) and ACKing it, exactly as
   pixl.js does at RF level ("for now we ignore the writes and just ack").

This is a real path to **parity**, not a compatibility shim: the console would be
reading the tag as the 2 KB part it is, including the machine block.


---

## 14. 🟢 UNBLOCKED 2026-07-27 — genuine capture obtained

The capture described in §13 was taken. **The console recognized the v3 amiibo through the
genuine controller, read it, and wrote to it.** Captures:
`dumps/v3-genuine-capture-2026-07-27.jsonl` (206 records) and
`dumps/ntag215-genuine-capture-2026-07-27.jsonl` (36 records, the control). Both
`overwritten: 0`; bridge health `rejected: 0`, `timeouts: 0`.

### 14.1 The §13 premise was wrong — the console DOES request the v3 page set

It asks twice. Descriptors are **console → device** (`sub 0x06`), i.e. the console tells the
controller which pages to read:

| # | Timeout | Blocks | Ranges | Size |
|---|---|---|---|---|
| 1 (seq 12) | 2000 ms | 3 | `00-3B, 3C-77, 78-86` | 540 B — the NTAG215 set |
| **2 (seq 36)** | 3000 ms | **4** | `00-3B, 3C-77, **78-91**, **E2-E6**` | **604 B** |
| 3 (seq 166) | 2000 ms | 1 | `03-03` | 4 B — the write-back |

The NTAG215 control issues **only** descriptor #1 and stops. So the escalation is v3-specific and
happens *after* the first read: the console probes with the 540 set, learns what the tag is from
what comes back, then re-reads with an extended 4-block descriptor reaching page `0x91` plus a
separate `E2-E6` block.

§13's "the console always asks for the NTAG215 set" was true only because our serve path never gave
it a reason to escalate.

### 14.2 The discriminator: read-buffer prefix bytes 18–50

The `0x15` read reply is `[8-byte header][flags,len16][60-byte prefix][tag image]`. Confirmed by the
control: 8×70 + 1×40 = 600 = 60 + 540, last chunk flagged `01`.

First chunk, prefix bytes, v3 vs NTAG215:

```
        [ 0] 04 00 00 00 01 02 00 07   <- identical; [7] = uid_len
        [ 8] .. UID (7 bytes) ..       <- differs per tag, as expected
        [15] 00 00 00                  <- identical
v3      [18] 06 80 92 50 07 B8 2D 0E 23 F0 FD E4 3D 9D D2 F1
        [34] 2A 4F 6B 75 0D AC FC A3 B5 D6 84 75 47 E8 95 C0 86
ntag215 [18] 00 00 00 00 ... all zero through [50] ...
        [51] 03 00 3B 3C 77 78 86 00 00   <- identical: block count + echoed ranges
        [60] tag image begins
```

**Bytes 18–50 (33 bytes) are populated for v3 and all-zero for NTAG215.** Everything else in the
prefix is identical or trivially tag-specific. This is the signal, and it is the only candidate in
the entire exchange.

Note `[20] = 0x92` — the v3 tag's ending page. `[18] = 0x06`, `[19] = 0x80`. The remaining 30 bytes
are high-entropy; a 32-byte field starting at `[19]` is the right shape for an NTAG21x ECC
originality signature (`READ_SIG`), but that is 🔵 **hypothesis, not established** — do not build on
it without checking.

**Why §13's elimination table missed this.** It records `prefix[18]` as *"jc_toolkit's NTAG model
byte — no effect on Switch 2"*. That test set byte 18 **alone**. The genuine controller populates
18 **through 50** as a block, and a lone byte 18 is evidently not sufficient.

### 14.3 The tag image confirms the v3 UID layout

The v3 image begins `04 90 11 CA DB 1F 90 | 00 44` — 7 contiguous UID bytes then `00`/`44`, exactly
the figure-v3 layout the portal already implements. The control begins
`04 1A 96 | 00 | 72 55 49 80 | EE | 48` — the NTAG215 BCC0/BCC1 interleave (BCC0 = `88^04^1A^96` =
`00` ✓, BCC1 = `72^55^49^80` = `EE` ✓). Both decode correctly, which independently validates the
existing v3 tag-format handling.

### 14.4 Confirmed by the capture: status can never discriminate

The `0x05` status is **byte-identical** between the two tags except for the UID. §13 reached this
conclusion by elimination; the genuine capture proves it directly. No further status probing is
warranted.

### 14.5 ✅ CONFIRMED — the prefix triggers escalation, and it is NOT tag-bound

Test 2026-07-27, `dumps/v3-serve-prefixtest-2026-07-27.jsonl`. Our own serve path, mirror **off**,
a v3 tag loaded, and `v3hdr 18 <the 33 captured bytes>` overlaid onto the read prefix.

**The console escalated.** seq 34 is the 4-block descriptor
`00-3B, 3C-77, 78-91, E2-E6` = 604 B — the same one the genuine controller produced. Our served
prefix reads back exactly as intended (`[18] 06 80 92 50 07 B8 … 95 C0 86`, `[51] 03`).

**And the field is not bound to the tag.** The loaded image is a *different physical amiibo* from
the one captured (captured UID `049011CADB1F90`; no local dump matches it). The console still
escalated. So bytes 18–50 are **not** validated against the tag's UID at this stage — which answers
§14.5 questions 1 and 2 in the most useful direction available: whatever they are, they can be
replayed. A per-tag ECC signature that the console checks *here* is ruled out.

**What now blocks it: the console aborts the extended read.** The cycle repeats ~7 times:

```
0x05 status -> 0x06 (4-block, 604 B) -> 0x04 STOP -> 0x03 poll -> 0x05 ->
0x04 -> 0x03 -> 0x05 -> 0x06 (3-block, 540 B) -> 0x05 -> read off=0 -> abort -> retry
```

It asks for 604 bytes, our serve path does not satisfy it, the console stops, falls back to the
540-byte descriptor, reads that, fails, and retries the whole sequence. Consistent with the reported
symptom: no error, no recognition.

So the remaining work is entirely on our side: **make `ns2_v3_serve()` answer the 4-block
descriptor** — including the `E2-E6` block, which lies outside the ordinary page space and is
presumably the machine/SRAM window. The descriptor-driven page-range machinery already exists; it
has simply never been exercised with 4 blocks.

### 14.6 Next step

Populate prefix bytes 18–50 in `ns2_v3_serve()`'s read replies and re-test. If the console escalates
to the 4-block descriptor, the serve path already has the machinery to answer it (descriptor-driven
page ranges are implemented and byte-exact). Open questions to settle in that order:

1. Does *any* non-zero content at 18–50 trigger escalation, or is the field validated? Replaying the
   captured 33 bytes verbatim answers this in one test — and since they came from this exact
   physical amiibo, a match is expected to work.
2. What are bytes 21–50 actually? If they are an originality signature they are per-tag and cannot
   be synthesised for an arbitrary dump, which would bound what virtual v3 amiibo can ever do.
3. What does the console expect at `E2-E6`? That block is outside the 2048-byte image and is
   presumably the machine/SRAM window.

---

## 13. PARKED — final state, and where to resume

> ⚠️ **Superseded 2026-07-27 by §14.** The blocker described below was resolved by the genuine
> capture. The elimination work here remains valid and is what made the capture cheap to interpret —
> with one correction: `prefix[18]` was tested alone, and the real signal is `prefix[18..50]`.

### What is finished and working ✅

- **2048-byte v3 store** with flash persistence, mutually exclusive with the
  540/572 store, surviving power cycles and mode changes.
- **Flash layout fixed** (this was a real, serious bug): amiibo journal bank 0 sat
  on top of BTstack's TLV region on RP2350, so writing a tag destroyed the
  Bluetooth bonds and BTstack destroyed the stored tag. Banks moved to `SIZE-6S` /
  `SIZE-5S`, asserts now check `PICO_FLASH_BANK_STORAGE_OFFSET`.
- **Durable v3 uploads**: `amiibo persist` used to be a silent no-op for v3
  (it gated on the 540 store's `loaded` flag) and the portal never called it.
- **Serve path**: full read state machine, descriptor-driven page ranges,
  `SRAM_RF_READY` injection, byte-exact chunk framing (verified 0 mismatches
  against the source dump).
- **Crypto**: all 16 Kirby dumps verified **HMAC-valid** with the owner's
  `key_retail.bin`, using the v3 (+0x40) offsets.
- **RE tooling**, all runtime-switchable over UART with the dongle attached:
  `v3mode`, `v3probe`, `v3hdr`, `v3reply`, `amiibo journal`, and 72-byte trace
  capture.

### The single blocker

The console decides which tag pages to request **before** any read, and always
asks for the NTAG215 set (`00-3b, 3c-77, 78-86` = 540 bytes). A v3 tag's encrypted
region ends at `0x248` (584 bytes), so that read can never validate one, and the
console never issues the SRAM sequence that carries the machine block.

### Exhaustively eliminated (every field a controller can influence)

| Field | Result |
|---|---|
| `status[16..60]` | **Ignored** — all 45 bytes set to `0xFF`, descriptor and read byte-identical |
| `status[4]` | NCI RF Discovery ID — `02` hard-crashes the console |
| `status[5]` | NCI RF Interface — no effect on the descriptor |
| `status[6]` | Tag IC family (jc_toolkit `buf[62]`) — **must be `0x02`**; `03` makes the console abort before `0x06` |
| `status[7]` | NCI RF Tech/Mode — `01`/`03` crash, `02` aborts the read |
| `prefix[18]` | jc_toolkit's NTAG model byte — **no effect on Switch 2** |
| `0x0C` reply | Matched a genuine PC2 byte-for-byte (`61 12 50 0d`) — no effect |
| Capability container | `F1 10 FF EE` on **both** 540 and v3 tags — cannot discriminate |
| Controller firmware version | Ruled out: amiibo work on un-updated controllers |

The `0x05` status is a condensed **NCI `RF_INTF_ACTIVATED_NTF`** passthrough from
the controller's **PN7160** front-end. An NTAG I2C 2K is also T2T / NFC_A-passive,
so that structure is **identical for both chips and can never distinguish them**.

### Approaches rejected (do not retry)

- **Serving a v3 tag as a 540-byte amiibo** (compat view, or re-signed with the
  owner's keys via `tools/amiibo_v3_to_540_resign.mjs`). Both produce
  cryptographically valid tags, and both are wrong in principle: the rider
  figurine does not scan at all without its machine — the machine is the antenna
  *and* holds the I2C device that answers the SRAM pass-through. A rider-only
  540 tag is not a degraded Kirby amiibo, it is not one.
- **Restoring the 60-byte prefix in front of a 1024-byte payload** — exceeds the
  console's ~1050-byte read window, `last=1` never arrives.
- **Sweeping enum values by crash-oracle** — unbounded, and each wrong value hard
  crashes the console (`2011-0301`).

### Useful context

`2115-0176` is the console's **generic "this tag failed validation"**, not a
tag-type rejection: r/Switch `1pvfm1v` documents a **genuine retail** Kirby amiibo
producing the identical symptoms (`2115-0176` in-game, "Not an amiibo" in System
Settings) across multiple Switch 2s — it was simply a defective tag.

### To resume

> **Prepared 2026-07-27:** the capture procedure is now written up step by step in
> [`experiments/v3-amiibo-genuine-capture-runbook.md`](experiments/v3-amiibo-genuine-capture-runbook.md),
> dry-run against live hardware. Two blockers were found and fixed while preparing it: the trace
> buffer would have truncated read-buffer replies (72-byte payload vs 128-byte mirrored responses)
> and could wrap mid-session, and the local serve path was consulted *before* the mirror, so it
> could answer the console even with an empty slot and return an empty capture. Ejecting the
> virtual slot is consequently no longer a precondition.

The one thing that would unblock this is **observing a genuine controller read a
genuine v3 amiibo once** — the repo already has the instrument for it
(`nfcmirror`, `src/bt_hid/bt/btstack/btstack_host.c`), which forwards the console's
NFC commands to a paired genuine Pro Controller 2 and returns its real replies to
the tracer. Requirements: a genuine Switch 2 controller over BT, `nfcmirror on`,
and the virtual slot **ejected** so the local serve path does not intercept.
That capture would show the real tag-type signal directly, and everything else is
already built to act on it.
