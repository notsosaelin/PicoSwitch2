# Amiibo identity, differentiation, and generation feasibility

Status: 🔵 Partial (research record). Format documented from primary references. **Key-free**
generation is **Refuted** (2026-07-26 hardware test): the Switch 2 validates amiibo cryptography, so
zero-HMAC images are rejected. **Key-based** generation using the user's own `key_retail.bin` was
prototyped in the portal (amiitool algorithm via Web Crypto, self-verifying against a genuine dump)
and then **removed** — the Virtual Amiibo library is now **import-only** by product decision. This
document is retained as the durable identity/crypto research; the portal ships no generator.
Last updated: 2026-07-26

## Purpose

Answer three questions the Virtual Amiibo Manager depends on:

1. What is actually stored on the board, and how many amiibo?
2. What bytes differentiate two amiibo of the same character?
3. Can PicoSwitch2 generate amiibo images itself — legally and safely — instead of requiring the
   user to supply dumps?

## 1. On-board storage: exactly one amiibo

The board stores **one** amiibo identity at a time. The flash journal's "Save 1 / Save 2" (clean /
used) pair is **not** two user amiibo: it is one identity's internal baseline plus its
latest-console-written recovery copy, kept in alternating banks for power-loss safety. See
[`../../src/nfc/virtual_amiibo_store.c`](../../src/nfc/virtual_amiibo_store.c) and
[`nfc-implementation.md`](nfc-implementation.md). The browser previously exposed two *quick-slot
pointers* into its local library; that was a browser-side convenience, retired in favor of a single
loaded-amiibo model to match the hardware and reduce confusion.

## 2. NTAG215 amiibo layout and per-tag differentiation

An amiibo is an NTAG215 dump: 540 bytes raw (the PicoSwitch2 canonical size), or 572 bytes with the
appended 32-byte NTAG originality signature. Key regions (confidence: **Strong**, from 3dbrew and
amiitool; offsets are into the raw image):

| Region | Raw offset | Purpose | Encrypted? |
|---|---|---|---|
| UID + BCC0/BCC1 + internal | `0x00`–`0x09` | NXP serial (`0x04…`), check bytes | No |
| Static lock bytes | `0x0A`–`0x0B` | `0F E0` | No |
| Capability container | `0x0C`–`0x0F` | `F1 10 FF EE` | No |
| Tag HMAC | `0x34`–`0x53` | SHA256-HMAC over tag/UID data | Signed |
| **Amiibo identity block** | `0x54`–`0x5B` | game+character, variant, type, model, series, format | No |
| Data HMAC | `0x80`–`0x9F` | SHA256-HMAC over settings/app data | Signed |
| Encrypted settings/app data | `0xA0`–`0x1B4` | owner Mii, nickname, app data, write counter | AES/HMAC |
| Dynamic lock + CFG0/CFG1 | `0x208`–`0x213` | `01 00 0F BD`, `AUTH0=04`, `ACCESS=5F` | No |
| PWD / PACK | `0x214`–`0x219` | UID-derived NTAG auth password/ack | No¹ |

¹ Genuine dumps normally carry **zero** PWD/PACK because a real reader never reads those pages back;
PicoSwitch2 treats zeros as canonical.

**What differentiates two amiibo of the same character:**

- **Identity block (`0x54`–`0x5B`)** — identical for the same product. This is what AmiiboAPI's
  `head`+`tail` (16 hex) map to, byte-for-byte.
- **UID (`0x00`–`0x08`)** — unique per physical tag. Games that rate-limit "the same amiibo" (e.g.
  the Zelda daily-spawn cooldown) key on **UID**, not identity block.
- **Encrypted settings block** — owner Mii, nickname, and the **write counter** (`0x00`–`0xFFFF`),
  plus per-tag keygen salt. Differs per tag and per write.

The two HMACs and the encrypted block are bound to the UID through Nintendo's key derivation
(`unfixed-info` + `locked-secret`). Changing the UID on a genuine dump invalidates both HMACs unless
they are recomputed with the retail keys — which we deliberately do not ship (see §4).

## 3. Presentation behavior (why "Random Mode" was removed)

The virtual reader presents the loaded image with its **stored identity and UID**; console writes
are applied to the used copy and persisted to flash. This is the hardware-validated behavior and
the only presentation mode.

A "Random Mode" was briefly implemented (2026-07-26) to defeat per-UID game cooldowns (e.g. the
Zelda daily-spawn limit) by overlaying a freshly drawn NTAG UID on each scan encounter. It was
**removed the same day** once the crypto-validation result below made clear the approach cannot
work: because the tag HMAC at page `0x34` is computed over the UID, swapping the UID at runtime
invalidates the HMAC, and recomputing it requires Nintendo's retail keys this project will not
ship. The console would reject the randomized tag exactly as it rejected the key-free generated
file. See §4 and
[`../experiments/generated-amiibo-console-rejection-2026-07-26.md`](../experiments/generated-amiibo-console-rejection-2026-07-26.md).

The only key-free ways to make "a different tag each tap" are therefore a **pool of distinct
genuine dumps** of the same character, or on-device HMAC recomputation (keys required, out of
scope). Neither is implemented.

## 4. Can we generate amiibo without user uploads?

**Community generators** (e.g. `hax0kartik/amiibo-generator`) confirm the *identity* portion needs
no keys: their output writes only the amiibo ID into an otherwise-zero amiitool **decrypted-layout**
template (identity at `0x1DC`, not the raw `0x54`), and the project states it uses "no keys or
encrypted bins." That is legally clean — an amiibo ID is a short non-copyrightable identifier
published by AmiiboAPI — but it is **not** directly serveable by PicoSwitch2, which speaks the raw
NTAG215 layout.

A now-removed helper (`tools/generate_test_amiibo.py`) built the raw-layout equivalent: correct
NTAG215 structure + the 8-byte identity block at `0x54`, all cryptographic regions zeroed. It
required no Nintendo keys, but §4 below records why its output is not console-usable.

**Resolved 2026-07-26 (Refuted):** a real Switch 2 rejected a generated zero-HMAC image with
"This isn't an amiibo" in both Save and Random mode, while the portal identified it correctly
(the portal reads only the plaintext identity block). See
[`../experiments/generated-amiibo-console-rejection-2026-07-26.md`](../experiments/generated-amiibo-console-rejection-2026-07-26.md).
The console validates amiibo cryptography (at least one of the tag HMAC, data HMAC, or originality
signature), so correct plaintext identity is necessary but not sufficient.

Consequences:

- **Key-free generation cannot produce console-usable amiibo.**
- **Random Mode was removed** for the same reason (a runtime UID swap breaks the UID-bound tag
  HMAC).

## 5. Key-based generation (prototyped, then removed — import-only by decision)

A key-based generator was implemented and validated, then removed at the project owner's direction
in favor of an import-only library: the crypto edge cases and freeze bugs it introduced were judged
not worth the convenience. The portal now ships **no generator** and stores no keys. This section
records what was learned so the work is reproducible if it is ever revived.

The implementation was a full amiitool port in JavaScript over Web Crypto — key derivation
(HMAC-SHA256 DRBG with a big-endian 16-bit counter), AES-128-CTR over internal `0x02C`..`0x1B4`, and
the tag/data HMAC-SHA256 signatures. It was gated on the user importing their own genuine 160-byte
`key_retail.bin` (data/unfixed-info + tag/locked-secret masters; **never shipped** — the same
posture as TagMo/amiitool/emuiibo, the user's own keys used for interoperability). Correctness was
provable without a console by decrypting a genuine dump and checking both HMACs verify. A Node
round-trip test confirmed pack↔unpack idempotence, HMAC verification, and wrong-key rejection.

If revived, the smallest useful step is that same self-verify-against-a-genuine-dump gate, followed
by one real-console scan of a generated tag; the offsets in §2 and the socram8888/amiitool reference
are the authority.

## References

- 3dbrew, *Amiibo*: https://www.3dbrew.org/wiki/Amiibo
- `socram8888/amiitool` (raw ↔ decrypted layout, HMAC/AES scheme)
- `hax0kartik/amiibo-generator` (identity-only decrypted template; "no keys")
- AmiiboAPI: https://amiiboapi.com/ (identity `head`+`tail`, artwork, series metadata)
- [`nfc-implementation.md`](nfc-implementation.md), [`nfc-protocol-inventory.md`](nfc-protocol-inventory.md)
