# Amiibo Crypto & Data-Model Research (TagMo / amiitool)

**Status:** research record, 2026-08-12. No code changes. Purpose: capture mature amiibo knowledge
from TagMo and its upstream sources (amiitool) so we don't rediscover it during troubleshooting, and
compare it to our current model. **Confirmed vs. inferred is marked throughout.**

Sources reviewed: [TagMo](https://github.com/HiddenRamblings/TagMo) (Android; credits libamiibo,
amiitool, AmiiboAPI), [amiitool `amiibo.c`](https://github.com/socram8888/amiitool/blob/master/amiibo.c)
(the reverse-engineered crypto reference — traced directly), the
[Kevin Brewster RE writeup](https://kevinbrewster.github.io/Amiibo-Reverse-Engineering/), and
[PyAmiibo](https://pyamiibo.readthedocs.io/). Motion cross-reference:
[Dycool/NS-PC-Control](https://github.com/Dycool/NS-PC-Control) (see §7).

---

## 1. Our current model (what we do today)

**We are a passthrough amiibo emulator, not a crypto tool.** Confirmed from our source:
- `virtual_amiibo_validate_raw()` (`src/nfc/virtual_amiibo.c`) checks **only structure**: the two
  NTAG215 UID check-bytes — `raw[3] == BCC0` and `raw[8] == BCC1` — nothing cryptographic.
- Storage integrity is a **CRC32** over the stored record (`virtual_amiibo_crc32`), unrelated to
  amiibo crypto.
- We serve the **raw ~540-byte tag dump** to the console over the controller NFC command channel; the
  **console** validates the amiibo crypto and originality signature. Game-owned writes come back from
  the console and we persist them (`virtual_amiibo_store_apply_console_write` + banks).
- We do **no** AES, no HMAC, no key derivation, no UID re-binding. (Consistent with the memory note
  "Switch 2 validates amiibo crypto; no key-free generation or random-UID.")

**Implication:** for backup / import / save-modification-by-console, the crypto is the console's job,
not ours. This is a deliberate, correct simplification — see §6.

---

## 2. Amiibo crypto — ground truth from amiitool (CONFIRMED, traced in source)

Amiibo user data is **AES-128-CTR encrypted** and protected by **two HMAC-SHA256 signatures**, all
keyed by data derived per-tag. The keys are **not** hardcoded directly; they are derived.

### Key derivation (DRBG via HMAC-SHA256)
Two master key sets ship in `key_retail.bin`: **"unfixed-info" (data key)** and **"locked-secret"
(tag key)** — the public reverse-engineered Nintendo keys everyone uses. Per-tag keys are derived by
seeding a DRBG (HMAC-SHA256) from tag-unique data. The DRBG seed (from amiitool
`nfc3d_amiibo_calc_seed`, internal format offsets) is:
- `intl[0x000:0x002]` write-counter region,
- `intl[0x010:0x018]` and `intl[0x018:0x020]` the **UID (8 bytes, twice)**,
- `intl[0x020:0x040]` the **keygen salt** (factory-hardcoded, plaintext).
The Kevin Brewster writeup corroborates: seed = type-string + magic (data key prepends the **2-byte
write counter**) + UID×2 + keygen salt. **The write counter feeds the data key** — an edge case: if
data + counter change, the data key changes.

### Encrypted vs plaintext (CONFIRMED)
- **Encrypted (AES-128-CTR):** internal `0x02C:0x1B4` (0x188 = 388 bytes) — the tag settings +
  application/app-save data.
- **Plaintext:** UID, lock/CC, **amiibo model/figure ID**, keygen salt, and the two HMACs.

### The two HMACs and their ORDER (CONFIRMED — easy to get wrong)
- **Tag HMAC** (internal `0x1B4`, 32 B): signs UID + model info + keygen salt (plaintext
  `0x1D4:0x208`). Keyed by the tag key.
- **Data HMAC** (internal `0x008`, 32 B): signs settings + **decrypted** data + **the Tag HMAC** +
  UID + salt. Keyed by the data key.
- **Order matters:** Tag HMAC is computed first; the Data HMAC input *includes* the Tag HMAC bytes.

### Pack / unpack sequence (CONFIRMED)
- **Unpack:** tag→internal → derive keys → AES-CTR decrypt `0x02C:0x1B4` → recompute+compare Tag HMAC
  then Data HMAC → valid only if **both** match.
- **Pack:** derive keys → Tag HMAC over `0x1D4:0x208` → Data HMAC over `0x029:0x208` ++ Tag HMAC →
  AES-CTR encrypt → internal→tag.

---

## 3. The raw-tag ↔ internal-format mapping (CONFIRMED — the piece we'd need)

amiitool crypto works on an **internal** layout; the **raw NTAG215 tag** (what we serve) is
rearranged. Exact `nfc3d_amiibo_tag_to_internal` mapping (traced):

| internal | ← raw tag | len | meaning |
|---|---|---|---|
| `0x000` | `0x008` | 0x008 | lock/CC + data-section start |
| `0x008` | `0x080` | 0x020 | **Data HMAC** |
| `0x028` | `0x010` | 0x024 | tag settings (write counter, init date, flags…) |
| `0x04C` | `0x0A0` | 0x168 | main app/save data |
| `0x1B4` | `0x034` | 0x020 | **Tag HMAC** |
| `0x1D4` | `0x000` | 0x008 | **UID** (pages 0–1) |
| `0x1DC` | `0x054` | 0x02C | **amiibo model/figure ID** + keygen-salt start |

So in the **raw tag we serve**: UID @`0x000`, **Tag HMAC @`0x034`**, **model/figure ID @`0x054`**,
keygen salt @`~0x064`, **Data HMAC @`0x080`**, encrypted app-data @`0x0A0:0x208`.

### Raw NTAG215 amiibo page map (CONFIRMED, Kevin Brewster)
- Pages 0–2 (`0x00`): UID(7) + BCC(2). Page 3: Capability Container.
- Pages 4–11 (`0x10`): Tag Settings (write counter, init date, nickname/flags).
- Pages 12–19 (`0x30`): **Tag HMAC**.
- Pages 20–23 (`0x54`): **Amiibo Model Info** = figure ID (series, game series, character, variation)
  — **PLAINTEXT**.
- Pages 24–31 (`0x64`): **Keygen Salt** (factory, plaintext).
- Pages 32–39 (`0x80`): **Data HMAC**.
- Pages 40–127 (`0xA0`): **Encrypted** app/save data (360 B).
- Pages 128+: dynamic lock, CFG0/CFG1, PWD, PACK.
- **PWD** derived from UID (CONFIRMED): `pw[0]=0xAA^(uid[1]^uid[3])`, `pw[1]=0x55^(uid[2]^uid[4])`,
  `pw[2]=0xAA^(uid[3]^uid[5])`, `pw[3]=0x55^(uid[4]^uid[6])`.

---

## 4. What TagMo/amiitool taught us that we did not previously have written down

1. **The exact tag↔internal offset mapping and HMAC/UID/salt positions in the RAW format we serve**
   (§3). Previously we only did structural BCC checks; we now know precisely where every crypto field
   lives in the dumps we handle.
2. **The two-HMAC dependency + ordering** (Tag HMAC first, Data HMAC includes it). Any future re-sign
   must honor this or the console rejects the tag.
3. **The figure/model ID and keygen salt are PLAINTEXT.** We can read the amiibo's identity from a
   dump **without any keys** — directly closes interface-audit gap G4's "status has no figureId":
   `figureId = raw[0x54:0x5C]` (8 bytes). (CONFIRMED plaintext location.)
4. **The write counter feeds the data key.** Relevant only if we ever re-encrypt; a non-obvious trap.
5. **Cloning/UID-rebinding** (re-derive keys + re-encrypt + re-sign to a *new* UID) is a distinct
   TagMo operation **we do not need** — we emulate, serving the original dump's own UID.
6. **Dump-size edge cases:** valid dumps are 540 bytes; 520-byte "incomplete" dumps exist (missing the
   config/PWD tail). Our store already distinguishes 540/572/2048; worth confirming we reject/normalize
   520-byte inputs on import. (INFERRED relevance — verify against our import validation.)

---

## 5. Comparison: our model vs TagMo's

| Capability | TagMo | Us (today) | Need crypto? |
|---|---|---|---|
| Read a physical amiibo | ✅ (phone NFC) | Controller NFC relay (UART-only, gap G4/Path B) | No — raw capture |
| Import a dump | ✅ | ✅ (`amiibo begin/chunk/commit`) | No |
| Serve to console | N/A | ✅ passthrough | No (console validates) |
| Save modification (game data) | via crypto edit | **console writes it, we persist** | No |
| Clone to a blank physical tag | ✅ (re-key to new UID) | ✗ (not needed — we emulate) | Yes (TagMo's job, not ours) |
| Edit character / restore app-data offline | ✅ (full amiitool crypto + keys) | ✗ | **Yes** (only if we add an editor) |
| Show amiibo identity (figure ID) | ✅ | ✗ (status lacks it) | **No** — plaintext `raw[0x54:0x5C]` |

---

## 6. The load-bearing conclusion (why this de-risks us)

**Our passthrough architecture avoids amiibo crypto for every use case we actually need:**
- **Backup** = capture an already-valid raw dump (via phone NFC → import, or controller-as-reader).
- **Import** = upload an already-valid dump.
- **Save modification** = the **console** re-encrypts/re-signs when a game writes to the amiibo; we
  persist the resulting valid bytes. (Our "game-owned write + completion" is already hardware-confirmed.)
- **Identity display** = plaintext figure ID, no keys.

The **only** thing that would require us to implement AES/HMAC/key-derivation is an **independent
in-app amiibo editor** (change character, hand-restore app-data). That is optional. If we ever add it,
amiitool is the exact, complete reference and the keys are public — but it is **not** on the critical
path for backup/import/console-driven modification.

---

## 7. NS-PC-Control (motion cross-reference)

[Dycool/NS-PC-Control](https://github.com/Dycool/NS-PC-Control) streams a Switch 2's display+controls
(incl. gyro) to a phone via a Pi Zero 2W (C++ USB-gadget server, UDP transport). Its README **cites
PicoSwitch2 as its motion-controls reference** — i.e., it *consumed* our decode (consistent with the
owner having collaborated with Dycool on the format). Therefore it is **corroborating evidence**, not
an independent source: a second project emitting Switch 2 motion using our decoded format is a
positive signal that the format is right. Its USB-gadget motion-emit code is a useful future A/B
cross-check for our report-0x05/0x09 packing. (No new format facts extracted; the doc has no offsets,
the format lives in its code.)

---

## 8. Confirmed vs inferred / remaining uncertainties

**Confirmed (traced in source or multiple corroborating sources):** the two-HMAC + AES-CTR scheme;
key derivation inputs (UID×2, keygen salt, write counter for data key); the tag↔internal offset
mapping; encrypted range; HMAC ordering; figure ID + salt are plaintext; PWD derivation.

**Inferred / to verify against our code (not blocking):**
- Whether our import path rejects/normalizes 520-byte dumps and enforces the 540-byte layout.
- Exact sub-page split of plaintext vs encrypted at the settings boundary (amiitool encrypts from
  internal `0x02C`, so a few settings bytes are plaintext) — matters only for an editor.
- Whether the console additionally checks the NXP **ECC originality signature** (separate from the two
  amiibo HMACs) and whether our captured dumps carry it. (TagMo/amiitool do not forge it; genuine
  dumps include it. Passthrough preserves it. Relevant only if a source lacks it.)
- Per-controller NFC read protocol for Path B backup (Pro2 implemented; Joy-Con 2 R undocumented).

**No remaining crypto uncertainty blocks our planned features** (backup/import/console-modification),
because none of them require us to compute amiibo crypto.
