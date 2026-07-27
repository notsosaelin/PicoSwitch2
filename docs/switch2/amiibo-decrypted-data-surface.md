# What amiibo decryption unlocks

**Question:** now that the portal can import `key_retail.bin` and decrypt, what is actually readable,
and can we read *and display* the Mii assigned to an amiibo?

**Short answer:** everything in the settings block is readable, and we currently use about three
fields of it. The Mii is a verbatim 3DS structure, so **reading** it fully is easy. **Rendering** it
accurately requires a copyrighted Nintendo asset we cannot ship.

Status: ⬜ research only — no code changed.

---

## 1. The decrypted settings block

Decryption yields the amiitool *internal* layout. The settings section begins at internal offset
`0x2C`; 3dbrew documents it relative to that start. Both are given here because the portal indexes
the internal buffer directly.

| Internal | Rel. | Size | Field | Portal today |
|---|---|---|---|---|
| `0x2C` | `0x00` | 1 | Settings flags (bit `0x10` = registered) | ✅ used |
| `0x2D` | `0x01` | 1 | Country code | ⬜ |
| `0x2E` | `0x02` | 2 | CRC32-mismatch counter (BE u16) | ⬜ |
| `0x30` | `0x04` | 2 | Setup date (BE u16, relative to 2000) | ⬜ |
| `0x32` | `0x06` | 2 | Last-write date (BE u16) | ⬜ |
| `0x34` | `0x08` | 4 | CRC32 with console-unique hash | ⬜ |
| `0x38` | `0x0C` | 20 | amiibo nickname, **UTF-16BE** | ✅ used |
| `0x4C` | `0x20` | **96** | **Owner Mii** | 🔵 name only |
| `0xAC` | `0x80` | 8 | Application title ID (BE) | ⬜ |
| `0xB4` | `0x88` | 2 | Write counter (BE u16) | ⬜ |
| `0xB6` | `0x8A` | 4 | amiibo AppID (BE u32) | ⬜ |
| `0xBA` | `0x8E` | 2 | Unknown | ⬜ |
| `0xBC` | `0x90` | 32 | Probable SHA256-HMAC | ⬜ |
| `0xDC` | `0xB0` | **216** | **AppData** — the per-game save blob | ⬜ |

Source: [3dbrew Amiibo](https://www.3dbrew.org/wiki/Amiibo). The relative offsets confirm the
portal's existing constants exactly (`0x38 − 0x2C = 0x0C`, `0x4C − 0x2C = 0x20`).

### Immediately useful, zero new dependencies

- **Write counter** (`0xB4`) — how many times the amiibo has been written. Already identified in
  [`amiibo-identity-and-generation.md`](amiibo-identity-and-generation.md) §2 as one of the few
  fields that differentiate two copies of the same character. Good library-list column.
- **Setup / last-write dates** (`0x30`, `0x32`) — "registered 2016-04-03, last written 2023-11-12".
  BE u16, days relative to 2000.
- **Title ID** (`0xAC`) and **AppID** (`0xB6`) — which game last wrote the amiibo. Turns
  "amiibo has data" into "amiibo has *Smash Bros.* data".
- **AppData** (`0xDC`, 216 B) — the actual game save. Not interpretable without per-game knowledge,
  but its presence/absence and a hash of it are worth surfacing, and it is what a "back up my
  Smash FP" feature would need.

## 2. The owner Mii is a verbatim 3DS structure

The 96-byte block at `0x4C` is 3DS **`Ver3StoreData`** (`0x60`), stored unmodified.

Two pieces of evidence, both already in hand:

1. The portal reads the owner name at Mii `+0x1A` as 20 bytes of UTF-16 and it works on real dumps.
   `Ver3StoreData` puts the name at exactly `0x1A`, 10 UTF-16 characters.
2. **The endianness split proves it.** The portal decodes the amiibo nickname as UTF-16**BE** and
   the Mii name as UTF-16**LE** — and both are correct. A field written by Nintendo's amiibo layer
   is big-endian; the Mii block is little-endian because it is a *pasted-in 3DS structure* that was
   never byte-swapped. That is strong evidence the whole 96 bytes are a standard, unmodified
   `Ver3StoreData`, which means any 3DS-capable Mii library can consume it directly.

So beyond the name, the block also carries: Mii ID / creator ID, gender, birthday, favorite colour,
favourite flag, height and build, and the full facial feature set (face shape, skin tone, wrinkles,
makeup, hair style/colour/flip, eyebrows, eyes, nose, mouth, beard/moustache, glasses, mole) plus
the creator's name.

## 3. MiiJS assessment

[MiiJS](https://github.com/Stewared/MiiJS) parses DS, Wii, 3DS, Wii U, Switch, Miitomo, Mii Studio
and QR formats, and explicitly lists **amiibo (`.ntag`)** with `extractMiiFromAmiibo()` /
`insertIntoAmiibo()`. It runs in the browser (a prebuilt `miijs.browser.js` is shipped) as well as
Node.

**Reading: ✅ yes, and we are already most of the way there.** Two integration notes:

- MiiJS expects **already-decrypted** amiibo data — it does no key handling. That is exactly what
  our amiitool port produces, so the pieces fit.
- Cleaner still: skip `extractMiiFromAmiibo()` and hand MiiJS the 96 bytes at internal `0x4C` as a
  3DS `Ver3StoreData`. That sidesteps any ambiguity about which layout ("as on the tag" vs internal)
  its amiibo path expects, and we already know exactly where the block is.

**Rendering: 🔴 blocked on a copyrighted asset.** MiiJS renders locally via FFL.js — no network
call, which would otherwise have been the objection — but it requires **`FFLResHigh.dat`**,
Nintendo's Mii model and texture archive. The README is explicit that it is *"not provided by the
library"* and must be sourced from a Wii U (`sys/title/0005001b/10056000/content/FFLResHigh.dat`) or
a Miitomo cache. We cannot bundle it.

## 4. Options for "display", ranked

**1. Structured attribute readout — recommended, ship-ready.**
No new assets, no licensing question, works offline, and needs no 3D renderer. Show the owner's
name, gender, birthday, a favourite-colour swatch, height/build, and optionally the full decoded
feature list. This is the honest 90 % of the value: the user wants to know *whose* Mii it is and
what it looks like in words. Pairs naturally with the write counter and dates from §1, which are
free.

**2. User-supplied `FFLResHigh.dat` — the elegant version, if a real render is wanted.**
Exactly the precedent already set by `key_retail.bin`: the portal does not ship the copyrighted
asset, the user imports their own, and the feature lights up when present. Falls back to option 1
when absent. Same UX pattern, same legal posture, and it reuses the import flow that already exists.
Costs: pulling in MiiJS + FFL.js + a WebGL render path, and `FFLResHigh.dat` is several MB (exact
size unverified) — a meaningful weight increase for a page that is currently one self-contained
file.

**3. Draw an approximate Mii ourselves in SVG.**
No copyrighted assets and fully offline, but it is a genuine art project — dozens of facial features
each with many variants — and the result will not match Nintendo's rendering. Not worth it unless
the render becomes a headline feature.

**4. Nintendo's Mii Studio render API.** ❌ Not recommended. Requires network from a page designed
to work offline, needs a `Ver3StoreData` → studio-format conversion, and sends the user's Mii data
to a third party. The offline-render path in option 2 is strictly better.

## 5. Suggested order of work

1. Surface write counter, setup/last-write dates, and title ID in the amiibo detail box — pure
   decode of bytes we already have, no dependencies.
2. Expand the Mii from "name" to the full attribute readout (option 1).
3. Only then decide whether a real render (option 2) earns its weight.

Steps 1 and 2 are self-contained and carry no licensing or bundle-size risk.
