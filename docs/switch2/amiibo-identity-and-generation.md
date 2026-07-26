# Amiibo identity, differentiation, and generation feasibility

Status: 🔵 Partial — format documented from primary references; console acceptance of
signature-less generated images is **Unknown** and gated on a hardware experiment.
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

## 3. Random Amiibo Mode vs Save Amiibo Mode

Two volatile presentation modes, selectable in the portal (`amiibo mode save|random`; never
persisted, defaults to Save on every boot):

- **Save Mode** (default, **Confirmed** lifecycle): the loaded image keeps its stored identity and
  UID; console writes are applied to the used copy and persisted to flash. This is the existing
  hardware-validated behavior.
- **Random Mode** (**Hypothesis**, hardware pending): on each fresh console scan encounter the
  virtual reader overlays a newly drawn NTAG UID (and, only when the source dump carries a nonzero
  PWD, a UID-derived PWD/PACK) onto the presented image. The overlay is stable within one encounter
  so status/select/read stay consistent, and console writes made under a random UID are **discarded**
  with the encounter — flash is never touched. Purpose: defeat per-UID game cooldowns by making each
  tap look like a different physical copy.

Implementation: `ns2_virtual_nfc_runtime_set_randomize_uid()` +
`apply_session_uid()` in
[`../../src/nfc/ns2_virtual_nfc_runtime.c`](../../src/nfc/ns2_virtual_nfc_runtime.c); mode flag in
[`../../src/nfc/virtual_amiibo_store.c`](../../src/nfc/virtual_amiibo_store.c); host coverage in
[`../../tools/test_ns2_virtual_nfc_runtime.c`](../../tools/test_ns2_virtual_nfc_runtime.c)
(`test_random_uid_mode`).

**Open question (Hypothesis → needs hardware):** whether a real Switch 2 in Save Mode revalidates
HMACs against the overlaid UID. Because Save Mode does **not** overlay a UID, only Random Mode is
exposed to this risk, and Random Mode already discards writes and never persists. The BCC bytes are
recomputed correctly; what is unverified is whether the console rejects a scan whose UID no longer
matches the (unchanged, UID-bound) HMACs. If it does, Random Mode requires either genuine per-UID
dumps or on-device HMAC recomputation (keys required) — document the result before promoting.

## 4. Can we generate amiibo without user uploads?

**Community generators** (e.g. `hax0kartik/amiibo-generator`) confirm the *identity* portion needs
no keys: their output writes only the amiibo ID into an otherwise-zero amiitool **decrypted-layout**
template (identity at `0x1DC`, not the raw `0x54`), and the project states it uses "no keys or
encrypted bins." That is legally clean — an amiibo ID is a short non-copyrightable identifier
published by AmiiboAPI — but it is **not** directly serveable by PicoSwitch2, which speaks the raw
NTAG215 layout.

[`../../tools/generate_test_amiibo.py`](../../tools/generate_test_amiibo.py) builds the raw-layout
equivalent: correct NTAG215 structure + the 8-byte identity block at `0x54`, with all cryptographic
regions (both HMACs, encrypted settings, originality signature) zeroed. It requires no Nintendo keys
and ships no copyrighted data.

**The blocking unknown:** whether a real Switch 2 accepts a signature-less, HMAC-zero raw image
through the virtual reader path. Evidence pointing both ways:

- For: STATUS records that normal 540-byte dumps do not contain `READ_SIG`, the successful native
  read buffer had a zeroed signature field, and a real console accepted the virtual reader path.
- Against: those tests used genuine dumps whose **HMACs were valid**. A zero-HMAC image has never
  been presented to a console from this project.

Until that experiment runs (see below), generated images remain an **experiment artifact**, and a
genuine user dump is the only **Confirmed**-tier input. If the console validates HMACs, key-free
generation is limited to Random-Mode-style throwaway taps at best, and full generation would require
the retail keys we will not ship.

### Smallest useful next experiment

1. `python tools/generate_test_amiibo.py <id>` for a low-risk game (not a save-bearing title).
2. Upload via the portal, Activate, scan on a real Switch 2, capture the UART trace.
3. Record whether the console reports a valid tag, an "unsupported/damaged amiibo," or silence.
4. Repeat once with a genuine dump of the same title as the control.
5. File the result in `docs/experiments/` and update this file's confidence tiers.

Define acceptance in advance: **accepted** = the game reads the amiibo and responds as it would to a
genuine tag; **rejected** = any error dialog or no-read. Preserve a negative result — it settles the
generation question either way.

## References

- 3dbrew, *Amiibo*: https://www.3dbrew.org/wiki/Amiibo
- `socram8888/amiitool` (raw ↔ decrypted layout, HMAC/AES scheme)
- `hax0kartik/amiibo-generator` (identity-only decrypted template; "no keys")
- AmiiboAPI: https://amiiboapi.com/ (identity `head`+`tail`, artwork, series metadata)
- [`nfc-implementation.md`](nfc-implementation.md), [`nfc-protocol-inventory.md`](nfc-protocol-inventory.md)
