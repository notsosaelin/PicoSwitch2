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

**⚠️ CORRECTED 2026-07-27 (see §14.7): escalation is not acceptance.** The paragraph below
concluded from escalation alone that bytes 18-50 are not tag-bound. That does not follow.
Escalation is decided early, from the prefix; whether the console *validates* those bytes happens
later, after the full read. The console has never accepted a served v3 tag, so the field may still
be tag-bound.

**The field does not affect the ESCALATION decision.** The loaded image is a *different physical amiibo* from
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

### 14.6 ✅ Extended read working — the descriptor gate was misparsing a timeout

`dumps/v3-serve-desc-fixed-2026-07-27.jsonl`. The console now completes the whole read:

```
0x06 (3-block, 540 B) -> status 0x04 -> reads 0..560 last=1
0x06 (4-block, 604 B) -> status 0x04 -> reads 0..630 last=1 (34 B) = 664 B total
0x14 WRITE BUFFER
```

**Root cause of the previous stall.** The `0x06` gate required
`request[0] == 0xD0 && request[1] == 0x07`, described in the code as "the D0 07 marker". Those bytes
are the **timeout, u16 LE**: `D0 07` = 2000 ms. The extended descriptor uses 3000 ms (`B8 0B`), so it
failed the gate, the operation never started, status never reached `0x04`, and the console issued
`0x04` STOP without a single read. The real layout is:

```
[0..1] timeout u16 LE   [2..8] UID   [9] tag type   [10] block count   [11..] (start,end) x N
```

The same misreading appeared in three places — the gate plus two parsers in `ns2_v3_build_buffer`
that derived a UID length from the timeout's high byte. All three now gate on structure and accept
any timeout. A 540-byte request still routes to the compatibility view; anything reaching past it
serves from the raw 2 KB image.

### 14.7 The console reads it all, then REJECTS it

**Correction to an earlier reading.** The single `0x14` after the extended read was taken as the
console "reaching the write stage". It is not. The nine `0x14` + `0x08` + `0x21` in the genuine
capture are the **owner/nickname registration the operator performed by hand** in that session, not
part of a plain read. In our test the console never advanced to the registration screen at all, so
it did not accept the tag.

So the read now completes **mechanically** — the console takes all 664 bytes — and then rejects the
**content**. Two candidates, in order:

1. **The replayed prefix bytes belong to a different tag.** The 33 bytes came from UID
   `049011CADB1F90`; the loaded image is `04B4438ADB1F90`. If bytes 19-50 are a per-tag originality
   signature, the console would read them, check them against the tag, and reject. Escalation would
   still have happened, because that decision is made earlier and evidently keys on byte 18.
2. **The `E2-E6` block content is wrong.** Its byte offsets are a linear-addressing *guess*
   (page 0xE2 -> byte 904). If NTAG I2C 2K sector addressing differs, the console is being fed the
   wrong 20 bytes.

**Cheapest discriminating test, and the right one to run next:** set `v3hdr 18 06` alone — byte 18
only, bytes 19-50 left zero. The NTAG215 control shows the genuine controller sends **all zeros**
there for a 540 tag and the console accepts it, so zeros are not inherently invalid.

- Still escalates, still rejects -> bytes 19-50 are not the problem; look at `E2-E6` (candidate 2).
- Still escalates and now *accepts* -> the mismatched signature was the problem, and a correct
  per-tag value is required.
- No longer escalates -> escalation needs more than byte 18, which is itself worth knowing.

That one test separates the two candidates without any new firmware.

### 14.7.1 Result: byte 18 alone drives escalation

`dumps/v3-serve-byte18only-2026-07-27.jsonl`, with `v3hdr 18 06` and bytes 19-50 **zero**:

- The console still issues the 4-block descriptor (blocks=4, timeout 3000) — **7 descriptors across
  the session, alternating 4-block and 3-block** — and still reads the full 664 bytes (highest
  requested offset 630).
- It still rejects. Screen stayed on "hold an amiibo to the controller"; no crash.

**Established:** bytes 19-50 have no bearing on the escalation decision. Byte 18 = `0x06` is
sufficient on its own. That is a genuine narrowing and it means the 33-byte replay is unnecessary
for this stage.

**Not established:** whether a *correct* per-tag value at 19-50 is required for acceptance. Zeros
fail and a foreign tag's bytes fail, which is equally consistent with "any wrong value is rejected".
The NTAG215 control sends zeros and is accepted, but that is a different tag type and the console
may only demand a signature for v3.

Remaining suspects, now in this order:

1. **`E2-E6` content.** Its byte offsets are still an unverified linear-addressing guess
   (page `0xE2` -> byte 904). This is the more tractable one: the mapping can be varied and retested
   without needing anything from a physical tag.
2. **A correct per-tag signature at 19-50.** Only distinguishable once (1) is ruled out, and if true
   it bounds virtual v3 amiibo to tags whose signature has been physically captured.

Side observation from the same capture: `0x14` appears 4x and `0x21` 4x across the retries, so some
write-side traffic does occur during a failed read. Worth decoding before assuming the write path is
untouched.

### 14.7.2 ❌ RETRACTED — the signature is NOT the root cause

**This section's conclusion is wrong and is kept only so the reasoning error is visible.**

Refuted by the dump set itself. `READ ME.txt` shipped with the Kirby Air Riders images states that
**Flashiibo Pro** and **Allmiibo/Pixl.js** *"need an update before they work"* — i.e. those emulators
do support v3 amiibo on a Switch 2. They are fed exactly these files: all 16 are **2048 bytes**,
with no companion signature and no 2080-byte variant, and those projects have no amiibo keys. If a
per-tag NXP signature were required, none of them could work either.

**The reasoning error:** the comparison below only covers the 60-byte **prefix**. The 604 bytes of
**tag data** in the same reply also differ, because the genuine capture is of a *different physical
amiibo* than the image we serve. Finding that `[19..50]` was the only differing prefix field does
not make it the cause — it was simply the most visible difference, and the tag-data difference was
never controlled for. The four-row table below is consistent with the signature mattering, but it is
equally consistent with any other per-tag content being wrong.

What survives from this section is only the **eliminations**, which are sound:

- `E2-E6` is served correctly (byte 904 of the image, byte-identical to genuine and constant across
  two physical tags).
- The descriptor echo at `[51..59]` is byte-exact.
- Byte 18 = `0x06` alone drives escalation.

The next step is therefore **not** to chase signatures but to study how pixl.js actually presents a
v3 tag — its `ntag_emu_v2.c` is already cited elsewhere in this document for the `SRAM_RF_READY`
behaviour, and it is the closest thing available to a known-good reference implementation.

<details><summary>Original (incorrect) conclusion, retained for the record</summary>

#### The console validates the NTAG21x originality signature for v3

Deep comparison of the extended-read prefixes (genuine vs ours) leaves exactly **one** structural
difference — bytes **[19..50], 32 bytes**:

```
[0..8]   identical
[9..14]  UID          differs (different physical tag, expected)
[15..18] identical, including our byte 18 = 0x06
[19..50] GENUINE: 32 bytes of data      OURS: zeros      <-- the only real difference
[51..59] identical: 04 00 3B 3C 77 78 91 E2 E6 (block count + the 4 ranges)
```

Our descriptor echo is byte-exact. `E2-E6` is byte-exact (verified: byte 904 of the Kirby image is
`0100FF00 00000004 07000000 ...`, identical to what the genuine controller returned, and identical
across two different physical tags — it is a constant config region). Both earlier suspects are
therefore eliminated.

32 bytes is exactly an **NTAG21x ECC originality signature**, and this repo's own code already
labels that slot: *"out[19..50]: originality signature — unknown for v3, left zero."*

The four observations settle it:

| Tag | Bytes 19-50 sent | Console |
|---|---|---|
| NTAG215 (genuine controller) | **zeros** | ✅ accepts |
| v3 (genuine controller) | 32 real bytes | ✅ accepts |
| v3 (ours) | zeros | ❌ rejects |
| v3 (ours) | another tag's signature | ❌ rejects |

**The console requires a valid originality signature for v3 tags, and does not for NTAG215.** It is
validated against the UID — a foreign tag's signature fails just as zeros do. The signature is
generated with NXP's private key over the tag UID, so it **cannot be computed, forged, or
transplanted**.

</details>

### 14.7.3 What this means for virtual v3 amiibo (also retracted)

- A v3 image alone is **not sufficient**. The 2048-byte dumps (pixl.js/flashiibo) contain the memory
  map only; the signature is returned by the `READ_SIG` command and lives outside it.
- Serving a v3 amiibo therefore requires **both** the image **and** that physical tag's 32-byte
  signature, captured together.
- Freely generated or UID-randomised v3 amiibo are **impossible** by this route — the same
  conclusion the project already reached for amiibo crypto generally, now for a second, independent
  reason.

**The one untested configuration** that would confirm this constructively: serve a tag's own image
*and* its own signature together. We have never had a matched pair — the captured signature belongs
to UID `049011CADB1F90`, and no local dump matches it. Obtaining a matched pair means capturing the
signature from a tag we also have a dump of.

If that works, the feature is real but scoped: v3 amiibo you have physically captured. If it still
fails, something beyond the signature is also checked.

**Superseded — see §14.7.2. Pixl.js and Flashiibo support these tags from the 2048-byte dump alone,
so none of the above scoping applies.**

### 14.7.4 The layer distinction that reconciles everything (pixl.js PR #381)

xSke's PR states the format plainly:

> *"The expected .bin format is a 2048-byte file containing all of sector 0 and 1 as it would be
> read by any NFC reader, and with the expected response already placed in the SRAM buffer."*

So everything the console needs is in the 2048 bytes. No keys, no separate signature file. That is
consistent with the shipped dumps and with the operator's objection.

**But pixl.js and PicoSwitch2 sit on opposite sides of the link:**

| | pixl.js / Flashiibo | PicoSwitch2 |
|---|---|---|
| Emulates | the **tag** (RF side) | the **controller** (USB side) |
| Who builds the 60-byte read prefix | a **genuine Pro Controller 2**, by reading the tag | **we** must synthesize it |
| Must answer `READ_SIG` etc. | yes, as a tag | n/a |
| Must produce prefix `[19..50]` | never | yes |

This is why "pixl.js needs no keys" does not settle what belongs at `[19..50]` — pixl.js never
produces that field. It is also why their reference implementation cannot be copied directly: it
solves the adjacent problem.

**The genuinely odd observation, which is now the sharpest lead:** in the NTAG215 control capture the
genuine controller emits **all zeros** at `[19..50]` — for a real retail NTAG215 amiibo, which
certainly *has* an originality signature. So the controller does not populate that field for
NTAG215, yet fills 32 bytes for v3. Whatever it is, the controller only sources it for v3 tags.

Two readings, both testable:

1. It is the originality signature, and the controller issues `READ_SIG` **only** for v3. Then a
   served v3 needs one — but pixl.js supplies it from somewhere in the 2048 bytes, so it would be
   *derivable from the dump*, not an unforgeable per-tag secret. Worth searching the image for the
   32 bytes the genuine controller sent.
2. It is not a signature at all but v3-specific tag metadata the controller reads out of the tag —
   in which case it is also in the dump and simply needs locating.

Either way the next concrete step is the same and needs no hardware: **take the 32 bytes the genuine
controller emitted and search the corresponding physical tag's own dump for them.** We cannot do
that yet — the captured bytes belong to UID `049011CADB1F90` and no local dump matches it. Getting a
dump of *that* tag, or a capture from a tag we already have a dump of, resolves it immediately.

### 14.7.5 prefix[19..50] is the SRAM window, and it is DYNAMIC — current wall

Two genuine captures of the **same physical tag** (UID `049011CADB1F90`) show the field taking two
distinct values inside one session. That alone refutes the signature reading — a signature would be
constant.

```
A  80925007B82D0E23F0FDE43D9DD2F12A4F6B750DACFCA3B5D6847547E895C086   (most reads)
B  0200732AB41C4AC291B9A5983C039400C9000A50423457313720010102000000   (one 4-block read)
```

**B is the SRAM window.** Against a local dump:

```
Kirby 0x3C0 : 0200 4C980F696FCF5128F89ED4B5AB00 9C0001 50423457313720010102000000
genuine   B : 0200 732AB41C4AC291B9A5983C039400 C9000A 50423457313720010102000000
```

Same head, same 13-byte tail, tag-specific middle — exactly xSke's *"expected response already
placed in the SRAM buffer"*. So it is derivable from the dump, no keys, no per-tag secret. That is
consistent with pixl.js/Flashiibo working from the 2048-byte file alone.

`ns2_v3_build_buffer()` now serves `image[0x3C0..0x3DF]` there, verified on the wire
(`dumps/v3-serve-sram-2026-07-27.jsonl`, prefix reads back `06 02004C98…0102000000`).

**Still rejected.** The console escalates, reads all 664 bytes, issues `0x14`/`0x21`, then loops —
the same wall. The remaining difference is that we serve **B always**, while a genuine controller
serves **A** on most reads and **B** on one.

**Next, in order:**

1. **Identify A.** 32 bytes, stable per tag across sessions. It is not in the tag data the console
   reads (searched, absent). Candidates: a different register window (session registers around
   `0x3B0`?), or the SRAM *before* the machine has written its response.
2. **Identify the trigger** that makes the controller switch A -> B. Both captures are on disk with
   full ordering, so this is a read of existing data, not new hardware work.
3. Only then decide what the serve path should emit per read.

### 14.8 Earlier next-step notes

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

---

## 15. The `0x21` device command (2026-07-27)

**Status: 🟡 identified from genuine captures, implemented, hardware test pending.**

### 15.1 Retraction — there is no dynamic SRAM window

§14.7.5 claimed the genuine read prefix field `prefix[19..50]` was an SRAM window that took two
distinct values ("A" and "B") for the same physical tag within one session, and that the serve path
should reproduce the "B" value. **That is wrong and is withdrawn.**

Re-reading both genuine captures record by record:

| Capture | Reads | `prefix[19..50]` |
|---|---|---|
| `v3-genuine-capture-2026-07-27.jsonl` | 5 | `80925007…E895C086` on every one |
| `v3-genuine-capture2-2026-07-27.jsonl` | 6 | `80925007…E895C086` on every one |

The value is **constant** across 11 reads, two sessions, and every descriptor variant
(3-block, 4-block, and the single-page `03-03` read). There is no A→B transition to explain.

The "B" value came from a **different operation** and was misattributed. It appears only in
records `seq 69/143/161` (capture 1) and `seq 137/211/229` (capture 2), where the buffer's first
byte is `0x18` rather than the `0x04` of a tag read. Those are replies to the `0x14`/`0x21`
sequence described below — never to a `0x06` read.

The cost of the error was one hardware test that served the right bytes in the wrong place.

### 15.2 `0x21` is v3-specific

Subcommand `0x21` appears in **no** NTAG215 flow. Counting console→device subcommands across every
capture in `dumps/`:

| Capture | Has `0x21` |
|---|---|
| `ntag215-genuine-capture-2026-07-27.jsonl` | no |
| `amiibo-540-working-read-2026-07-26.jsonl` (successful 540 read **and** write) | no |
| `virtual-amiibo-*` (validated write lifecycle) | no |
| `v3-genuine-capture*.jsonl` | **yes** (3 and 4 occurrences) |

So `0x21` is not part of the ordinary amiibo protocol at all. It is the step a v3 tag adds.

### 15.3 The sequence

Genuine, `v3-genuine-capture-2026-07-27.jsonl` seq 62-71:

```
62  C->D  0x14  offset=0 len=0x4A   D0 07 | 049011CADB1F90 | 01 01 | 00…    stage device command
63  D->C  0x14  ACK
64  C->D  0x21  (no payload)                                                execute
65  D->C  0x21  ACK
66  C->D  0x05  status
67  D->C  0x05  state = 0x18            <-- not 0x04
68  C->D  0x15  offset 0
69  D->C  0x15  70 bytes, flags 0x00
70  C->D  0x15  offset 0x46
71  D->C  0x15  13 bytes, flags 0x01    <-- 83-byte result buffer
```

Ours, `v3-serve-sram-2026-07-27.jsonl` seq 60-72 — identical up to the execute, then:

```
62  C->D  0x21
63  D->C  0x21  ACK
66  C->D  0x05  status
67  D->C  0x05  state = 0x04            <-- unchanged; nothing was produced
64  C->D  0x04  stop
66  C->D  0x03  restart discovery       <-- the "waits forever" loop
```

`0x21` fell through to `default:` and was answered with a bare ACK. The console has no result to
read, so it abandons the tag and restarts polling. This is exactly the reported symptom: *"just
waited for an amiibo to be scanned."*

### 15.4 The result buffer

83 bytes, reassembled from seq 68-71:

```
000  18 00 00 00 01 02 00 07  04 90 11 CA DB 1F 90 00
010  00 00 06 02 00 73 2A B4  1C 4A C2 91 B9 A5 98 3C
020  03 94 00 C9 00 0A 50 42  34 57 31 37 20 01 01 02
030  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00
040  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00
050  00 7A C4
```

| Offset | Meaning | Confidence |
|---|---|---|
| `[0]` | result type `0x18` (a tag read uses `0x04`) | ✅ Confirmed |
| `[4..7]` | `01 02 00 07` — as in a read prefix; `[7]` is the UID length | ✅ Confirmed |
| `[8..14]` | tag UID | ✅ Confirmed |
| `[18]` | `0x06` — same escalation byte as the read prefix | ✅ Confirmed |
| `[19..50]` | 32-byte device response | ✅ Confirmed present, 🔵 contents |
| `[51..79]` | zero | ✅ Confirmed |
| `[80..82]` | `00 7A C4` | 🔴 Unidentified |

The device response at `[19..50]` decomposes against the stored image at `0x3C0`:

```
genuine  02 00 | 732AB41C4AC291B9A5983C0394 00 | C9 00 0A | "PB4W17 " | 01 01 02 00 00 00
image    02 00 | 4C980F696FCF5128F89ED4B5AB 00 | 9C 00 01 | "PB4W17 " | 01 01 02 00 00 00
```

Same header, same ASCII product code `50 42 34 57 31 37 20` = `"PB4W17 "`, same 6-byte tail. The
14-byte middle and the 3 bytes before the product code are per-tag — and the two samples *are*
different physical tags (`049011CADB1F90` captured vs `04B4438ADB1F90` uploaded), which accounts
for the difference without requiring a live challenge/response.

This is what xSke's pixl.js PR #381 means by *"a 2048-byte file with the expected response already
placed in the SRAM buffer"*: the response is **stored in the image**, not computed. That is why
key-less tools can support these tags.

### 15.5 What was implemented

`ns2_v3_serve()` (`src/switch_pro2/switch_pro2.c`):

- `0x14` records that a device command was staged, gated on offset 0 and a UID match, so it cannot
  be confused with the NTAG215 write path.
- `0x21` builds the 83-byte result buffer, sets status `0x18`, and marks the operation active so the
  existing `0x15` chunker serves it.
- `[19..50]` comes from `image[0x3C0..0x3DF]`, the SRAM window.

### 15.6 The 23-byte body is invariant

The body at `[60..82]` is zeros ending in `00 7A C4`, and it is **byte-identical across all six
genuine result buffers** in both captures:

| Capture | `0x21` at seq | body |
|---|---|---|
| 1 | 64, 138, 156 | `00…00 7A C4` |
| 2 | 132, 206, 224 | `00…00 7A C4` |

**It does not carry written user data.** 23 bytes is close to an amiibo nickname (20 bytes, 10
UTF-16 characters), which makes that a reasonable hypothesis, but the data rules it out: capture 1
commits a write at seq 186, and capture 2 is a *later session* — the tag already carried its owner
and nickname — yet capture 2's buffers equal capture 1's pre-write ones exactly. Owner and nickname
live in the tag's encrypted region and are delivered through the `0x06` read path instead.

It also matched **no** CRC-16 variant (CRC-A, CRC-B, CCITT-FALSE, XMODEM, MODBUS, KERMIT) over any
contiguous prefix of the buffer, so it is not a simple checksum.

Because it is constant and 20 of 23 bytes are confirmed zero, the serve path **reproduces the
observation** rather than sourcing it from `image[0x3E0]`: a non-zero region there would emit bytes
the genuine reader never sends. Sourcing it from the image was the first implementation and was
wrong for that reason.

**Remaining unknown:** whether `7A C4` is tag-specific. Only one physical tag has been observed.
Reading a second tag with `tools/nfc_probe.ps1` (§16) settles it — a bench measurement now, not a
console capture.

---

## 16. Interrogating a genuine controller directly (2026-07-27)

**Status: ✅ implemented, host-tested, hardware test pending.**

The `nfcmirror` bridge was purely reactive: it forwarded whatever the console asked and returned the
genuine reply. Every question therefore cost a full console capture, and questions the console never
happens to ask could not be asked at all.

It now has an **initiator** mode. UART originates NFC commands and reads the genuine controller's
replies, with **no console in the loop** — the bridge is BLE-only, gated on `sw2_init_state` and a
connected `0x2069`, so it needs nothing but a powered dongle and a paired Pro Controller 2.

This makes a genuine controller an interrogatable oracle: arbitrary page ranges, arbitrary
commands, repeatable, at bench pace. It is also an amiibo dumper — including for v3 tags, which
nothing else in the repo can read.

### 16.1 Firmware

| Symbol | Role |
|---|---|
| `ns2_nfc_mirror_set_initiator(bool)` | arms initiator mode; implies `ns2_nfc_mirror_request(true)` |
| `ns2_nfc_mirror_initiator_submit()` | originate a command |
| `ns2_nfc_mirror_initiator_take()` | collect the reply |

The two directions are mutually exclusive by construction. While the initiator owns the slot,
`ns2_nfc_mirror_submit()` and `ns2_nfc_mirror_take_usb_response()` both refuse, and
`ns2_nfc_mirror_active()` reports false so an attached console sees ordinary local behavior rather
than a half-mirrored session. `nfcmirror off` clears both flags.

Submission is nonblocking: the BLE round trip is tens of milliseconds and core0 also drives 1 kHz
USB, so the reply is collected by a separate poll rather than by stalling core0.

### 16.2 UART commands

```
nfcmirror initiator on|off
nfcmirror send HEX          # >=8 bytes, <=40 (bounded by the 96-byte RX line)
nfcmirror reply             # {"ready":false} until the genuine reply lands
```

40 bytes covers every read-path command; the longest, the `0x06` read descriptor, is 27. Tag writes
are longer and stay on the console path.

### 16.3 Command vocabulary

Decoded from `v3-genuine-capture-2026-07-27.jsonl`:

| Command | Bytes |
|---|---|
| stop | `01 91 00 04 00 00 00 00` |
| start discovery | `01 91 00 03 00 05 00 00 00 E8 03 2C 01` |
| status | `01 91 00 05 00 00 00 00` |
| read descriptor | `01 91 00 06 00 13 00 00` + `timeout16` + `uid[7]` + `type` + `blocks` + `(start,end)x4` |
| read buffer | `01 91 00 15 00 02 00 00` + `offset16 LE` |

Replies: 8-byte header, then `[8]` flags (bit 0 = final chunk), `[9..10]` chunk length LE, then data.
Chunks are 70 bytes. The reassembled buffer is a 60-byte prefix followed by tag content.

### 16.4 Driver

`tools/nfc_probe.ps1` does the sequencing on the PC, keeping the firmware a dumb auditable
transport:

```powershell
.\tools\nfc_probe.ps1 -Port COM11 -Dump kirby.bin              # auto-detect v3 vs NTAG215
.\tools\nfc_probe.ps1 -Port COM11 -Ranges '00-3B,3C-77,78-91,E2-E6'
.\tools\nfc_probe.ps1 -Port COM11 -Raw '0191000500000000'      # one command, print the reply
```

`-Dump` writes the tag content and, alongside it, `<name>.prefix` with the 60-byte operation prefix.

---

## 17. Hardware iteration 2026-07-27 — structure solved, content outstanding

**Status: 🟡 the exchange is byte-exact; the console still declines to proceed.**

Five hardware runs in sequence. Each removed one difference from a genuine controller.

| # | Change | Result |
|---|---|---|
| 1 | `0x21` device command implemented | Inconclusive — `prefix[18]` regressed (see below) |
| 2 | `prefix[18]=0x06` moved into the serve path | Escalation restored, `0x21` fired, bare ACK only |
| 3 | Originality signature served in read `prefix[19..50]` | Read prefix matches genuine except the UID |
| 4 | `0x21` bumps the report NFC state | Console reads the result buffer; 138 → 1040 records |
| 5 | State `0x18` reports an empty status payload | `0x18` status byte-identical to genuine |

### 17.1 What is now byte-exact

- **Extended read (664 B).** Identical length, identical structure, `E2-E6` block identical. The
  60-byte prefix differs from genuine only at bytes 9-11, which are the UID of a different physical
  tag.
- **`0x21` result buffer (83 B).** Both `0x15` chunks, 70 + 13; the 13-byte tail is identical.
- **`0x05` status in state `0x18`.** Empty payload, matching genuine exactly.
- **All ACKs.** `0x14` and `0x21` replies are byte-identical.

`amiibo v3diag` confirms the path executes: `dev_cmd_staged:3, dev_results:3`.

### 17.2 Two lessons about instrumentation

**A UART override is not a fix.** `prefix[18]=0x06` lived only in the `v3hdr` probe overlay, which
does not survive a reflash. Run 1 therefore tested nothing: the console never escalated, read a
540-byte view of a 2 KB tag, and correctly answered "This is not an amiibo". Anything a test depends
on belongs in the serve path.

**A bare ACK hides everything.** `0x21` replies identically whether or not the staging gate passed,
so the wire could not distinguish "gate rejected the `0x14`" from "gate passed, console ignored the
result". The `amiibo v3diag` counters were added to settle that, and did.

### 17.3 The console loops on genuine hardware too

Genuine issues the device command **three times** (capture 1, seq 64/138/156) before breaking out.
Looping is therefore not itself the failure signal, and a session that loops is not evidence of a
broken device command.

The breakout is identifiable: at seq 166 the console issues a **targeted descriptor** — UID
populated, `blocks=1`, range `03-03` — and then the write phase (`0x14` × 6, `0x08` commit). Ours
never issues that descriptor; every one of ours carries a zero UID and `blocks` 3 or 4.

The ordering also differs. Genuine runs read/read/devcmd, read/read/devcmd, then **devcmd again with
no intervening read**, then breaks out. Ours returns to a full read cycle after every device
command.

### 17.4 What remains — content, not structure

Every structural difference is eliminated, so the remaining candidates are values we have never
validated against the physical figure:

1. **The originality signature.** We serve `80925007…C086`, which belongs to tag
   `049011CADB1F90`, while the loaded image is `04B4438ADB1F90`. The console does not reject on it
   up front — it escalated, read everything, and ran the device command — but it may well be
   required for the console to *proceed*.
2. **The device response** at result `[19..50]`, served from `image[0x3C0]` of the uploaded dump.
   Never confirmed against hardware.

Both are readable from a physical tag with `tools/nfc_probe.ps1` (§16): bytes 19-50 of the
`.prefix` file it writes are the signature.

### 17.5 Next

1. Dump the physical Kirby figure with `nfc_probe.ps1`; take its real signature and device response.
2. Load the signature with `amiibo v3sig <hex32>` (RAM-only — no reflash between attempts) and
   retest.
3. If it still declines, capture a genuine controller reading **the same physical tag** through
   `nfcmirror`, and diff that against our serve for the same UID. Every prior capture used a
   different tag, which is what forced the byte 9-11 caveat above.

---

## 18. v3 amiibo RECOGNIZED (2026-07-27)

**Status: ✅ read path complete and hardware-confirmed. 🔴 write path not implemented.**

A Switch 2 recognised a virtual v3 (NTAG I2C Plus 2K) amiibo for the first time.
Evidence: [`../dumps/v3-RECOGNIZED-2026-07-27.jsonl`](../dumps/v3-RECOGNIZED-2026-07-27.jsonl).

The console performed the full genuine sequence, including the breakout that had never occurred
before:

```
seq 164  0x06  blocks=1  uid=049011CADB1F90  range 03-03     targeted read
seq 172  0x14  len 88    D0 07 <uid> 01 06 01 04 FFFFFFFF…   encrypted write
seq 174-180  0x14 x4                                          remaining chunks
seq 184  0x08                                                 commit
```

`amiibo v3diag`: `dev_cmd_staged:5, dev_results:4`.

### 18.1 What actually unblocked it

Not a protocol discovery — a **provenance** fix. Every earlier test served the downloaded
`Kirby & Warp Star.bin` (UID `04B4438ADB1F90`) while the genuine reference capture was of a
*different physical copy* of the same character and machine (UID `049011CADB1F90`). The four Warp
Star dumps in that set share one SRAM block because they were produced from one physical machine
with four riders, so the block is per-unit, not per-model.

`tools/rebuild_v3_from_capture.py` reassembles the tested tag from its own capture: the 4-block read
covers 604 bytes (pages `00-3B, 3C-77, 78-91, E2-E6`), which is everything the console ever
requests, backfilling only never-read bytes from the downloaded dump, plus the `0x21` device
response carried into `0x3C0` separately because the page read does not reach it. Result:
`dumps/kirby-warpstar-rebuilt-from-genuine.bin`, crc32 `de7dafc0`.

**Implication for the general case:** a v3 image needs its own machine's SRAM block and its own
signature. A dump taken from a different physical machine will not be accepted, even for the same
character and machine model. This is the single most important practical consequence of this
session.

### 18.2 The signature is not in the dump

Searching all 16 confirmed-working dumps for the known 32-byte signature finds nothing, yet those
files work on pixl.js — so the console does not validate the originality signature against the tag.
It is still required *structurally* in read `prefix[19..50]`, which is why `amiibo v3sig` exists.

Flashiibo Pro firmware `Pro_Firmware_OTA_26.7.2` contains `pixljs.bin`: Flashiibo Pro **is** pixl.js,
making xSke's PR #381 the direct reference implementation rather than an analogue.

### 18.3 What froze

The console wrote and committed; nothing persisted. `ns2_v3_serve()` treats `0x14` only as device
command staging and does not handle `0x08` at all, so both fall through to a bare ACK. The console
commits, is acknowledged, and then waits for a write-complete state that never arrives.

The 540-byte NTAG215 path already implements all of this
(`ns2_virtual_nfc_write_begin/chunk/commit`, `NS2_VIRTUAL_NFC_EVENT_WRITE_COMPLETE`, the `0x05`
status transition and flash persistence). The v3 path needs the equivalent, with two differences:
`0x14` must now distinguish a device-command descriptor from a data chunk, and offsets address a
2048-byte image.

### 18.4 Next

1. Implement the v3 write path, reusing the validated 540 write machinery.
2. Have `nfc_probe.ps1` capture signature and SRAM block directly, so a user can enrol their own
   figure without needing a console capture first.
3. Persist the signature alongside the v3 image rather than requiring `amiibo v3sig` after a reflash.
