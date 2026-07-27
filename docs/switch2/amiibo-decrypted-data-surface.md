# What amiibo decryption unlocks

**Question:** now that the portal can import `key_retail.bin` and decrypt, what is actually readable,
and can we read *and display* the Mii assigned to an amiibo?

**Short answer:** everything in the settings block is readable, and we currently use about three
fields of it. The Mii is a verbatim 3DS structure, so **reading** it fully is easy. **Rendering** it
accurately requires a copyrighted Nintendo asset we cannot ship.

Status: 🔵 research, with §1 and §6 implemented (2026-07-27).

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

---

## 6. Implemented (2026-07-27)

**Detail fields.** The amiibo detail box now shows last-written date, registered date, write count,
and whether game data is present (with the writing title ID when there is one). All are pure
decodes of bytes the portal already had; §1's table is the reference.

**Import vs Sync.** One button, chosen by what the adapter is holding:

| Adapter state | Button |
|---|---|
| Holds an amiibo the library has never seen | **Import amiibo** — adds it to the library |
| Library has it, console has written since | **Sync amiibo** — pulls those writes back |
| Library has it, no console writes | hidden |

"Newer than the library copy" is the firmware's own `dirty` flag, not a decrypted date comparison.
`virtual_amiibo_apply_console_write()` sets it and `virtual_amiibo_acknowledge_download()` clears
it, so it already means exactly "has console writes not yet pulled back" — and it costs nothing,
whereas comparing write dates would mean downloading and decrypting the whole image on every status
refresh just to render a button. (The date lives inside the encrypted region, so the firmware
cannot read it; only the portal can, and only after a full download.)

**Initialization.** Wipes an amiibo back to factory state and re-signs it. Requires imported keys,
so the button is hidden without them.

Only `internal[0x02C..0x1B4)` is cleared — precisely the AES-encrypted settings and AppData region.
Everything the key derivation seeds from (`0x029-0x02B`, `0x1D4-0x1DB`, `0x1E8-0x208`), the UID, and
the amiibo identity block sit outside it and stay byte-identical, so the tag remains the same amiibo
with the same derived keys. Dates land on `0x0000`, which decodes as month 0 / day 0 and therefore
reads as "not set" — that is what makes zero a safe NULL rather than a bogus year-2000 date.

This needed a **pack** path (`amiiboPackInternal`), the inverse of the existing decrypt: recompute
the tag HMAC, then the data HMAC (which covers the tag HMAC, so the order is load-bearing), then
AES-CTR encrypt, then map back to tag layout. It writes into a copy of the original tag so bytes
outside the mapped regions survive.

Two safety properties, both tested:

- The result is verified before it can reach the library: it is decrypted again, both HMACs must
  pass, and every user field must read back blank. A re-signed image the console would reject is
  worse than no feature, so a failure aborts with the library unchanged.
- A dump that fails its own HMAC is refused rather than re-signed, so a corrupt or tampered image
  cannot be laundered into a valid one.

**Difference from a console reset:** the Switch's own amiibo reset clears registration and game data
but leaves the write counter incrementing. Initialization zeroes it, because the request was
factory-new state. Worth knowing if a game uses the counter to recognise a previously-seen amiibo.

Tests: `tools/test_amiibo_decrypt.mjs` builds a fully populated amiibo (owner, nickname, both dates,
write counter 300, title ID, random AppData), initializes it, and asserts both HMACs still pass,
every field reads blank, the UID / identity block / tail are byte-identical, initializing twice is
idempotent, and a tampered dump is refused. Plus date-decode edges (`0x0000`, `0xFFFF`, a known
value).

Not hardware validated — an initialized amiibo has not yet been presented to a console.

## 7. "Which games have data on this amiibo?" — there can only ever be one

An amiibo holds **one application's data at a time**. AppData is a fixed `0xD8` region, and a
different application **overwrites** it — which is the console prompt you see when a second game
wants to save to an amiibo that already carries data. So the answer to "which games" is always
exactly zero or one, and there is no list to render.

**Use the AppID, not the title ID.** 3dbrew is explicit that the system *writes* the application
title ID but **never compares** it, "doing the latter would break games' cross-platform
compatibility with 3DS<>Wii U". The same game therefore has different title IDs per platform while
its **AppID is stable**, which makes AppID the correct key for naming the owning game. Title ID
remains worth keeping as raw evidence of which build wrote it.

Implemented: `AMIIBO_APP_IDS` maps AppID (internal `0xB6`, 4 bytes BE) to a game name, and the
detail box shows that name instead of a hex blob.

| AppID | Game |
|---|---|
| `10110E00` | Super Smash Bros. |
| `0014F000` | Animal Crossing: Happy Home Designer |
| `00152600` | Chibi-Robo!: Zip Lash |
| `00132600` | Mario & Luigi: Paper Jam |
| `1019C800` | The Legend of Zelda: Twilight Princess HD |

🔵 The table is **partial** — these are the values 3dbrew documents, and they are all 3DS/Wii U era.
Switch titles (Smash Ultimate, Breath of the Wild, Splatoon 2) are not documented there. An
unrecognised AppID renders as `Unrecognised game (AppID XXXXXXXX)` rather than being guessed at.
Extend the table only from confirmed dumps: read a known amiibo written by the game in question and
record the AppID it carries.

### The other question this could mean

"Which games *can use* this amiibo" is a different, genuinely list-shaped question, and it is
already within reach: AmiiboAPI — which the portal fetches for its catalog — returns
`gamesSwitch` / `games3DS` / `gamesWiiU` arrays when queried with `?showgames`. That is compatibility
metadata about the character, not anything read from the tag, so it needs no keys and works for
every amiibo in the library rather than only written ones. Not implemented; noted as the natural
follow-up if a list is what is actually wanted.

## 8. AmiiboAPI `?showgames` — the AppID problem solved better (2026-07-27)

The five-entry 3dbrew AppID table in §7 was never going to be enough: it is 3DS/Wii U era and has no
Switch titles. **AmiiboAPI's `?showgames` replaces it**, and the portal already talks to that API.

The key observation is in the response shape:

```json
"games3DS":     [{"gameID":["0004000000188B00", ...], "gameName":"Mario Sports Superstars"}],
"gamesSwitch":  [{"gameID":["010015100B514000"],      "gameName":"Super Mario Bros. Wonder"}],
"gamesSwitch2": []
```

Those `gameID` values are **title IDs in exactly the format an amiibo stores at internal `0xAC`** —
8 bytes big-endian, 16 hex digits. So folding every `gameID` in the catalog into one map gives a
direct title-ID → game-name lookup for the game that wrote an amiibo's save data, covering Switch
and Switch 2 as well as the older platforms.

**Resolution order** (`amiiboAppDataLabel`): catalog title ID → 3dbrew AppID table → raw
identifiers. The catalog goes first because it is broader and maintained; the AppID table stays as
the fallback for a title ID the catalog does not list (a regional build, say). Nothing is guessed —
an unmatched amiibo shows its raw title ID or AppID so it can be looked up by hand.

**Cost, measured rather than assumed:**

| | Size |
|---|---|
| Catalog without `showgames` | 425 KB |
| Catalog with `showgames` | 1.25 MB |
| Derived title-ID map (222 entries) | **10 KB** |
| Derived per-amiibo game-name lists | 257 KB |

Only the derivations are persisted. `amiiboAbsorbGames()` folds each item's `gameID` arrays into the
shared map, reduces its games arrays to per-platform **name** lists, and `delete`s the bulky
originals before the catalog reaches IndexedDB. The map is stored alongside the catalog so a cached
start does not have to re-derive it. A cache written before this change has no map, which would
leave game names unresolved until it aged out a week later, so a missing map now counts as stale
and triggers a refresh.

**Compatibility list.** The same fetch also powers a "Works with" row listing the games each amiibo
can be used in, grouped by platform. That is catalog metadata about the character, not tag contents,
so it needs no keys and works for every library entry — written or not. The row is height-capped
with its own scroll, since some amiibo work with dozens of games.

**Game icons: not done.** The Switch shows the writing game's own icon next to amiibo save data.
AmiiboAPI supplies amiibo images but no game artwork, so this would need a separate title-ID → icon
source (a 3DS/Switch title database) and a second remote image fetch. Parked; the title ID needed to
key such a lookup is already decoded and displayed, so nothing blocks it later.

Tests: `tools/test_amiibo_games.mjs` extracts `amiiboAbsorbGames` from the page and runs the real
catalog through it when `tools/fixtures/amiibo-showgames.json` is present (git-ignored, ~1.2 MB;
fetch on demand), else a small inline fixture so it works offline. It asserts the raw arrays are
stripped, every derived key is a 16-hex-digit title ID, name lists are deduplicated and use known
platform labels, known lookups resolve, an unknown title ID does not, and the map stays small.

## 9. Settings flags are the authority, not the regions (2026-07-27)

Found on a real Charizard dump: the detail box reported
`Unrecognised game (title 11D10F5819F48509)`. That value matches **no** Nintendo title-ID prefix —
3DS are `00040000…`, Wii U `00050000…`, Switch `0100…` — which is what gave it away as leftover
bytes rather than a real title ID.

**Cause:** "has game data" was implemented as a non-zero scan of the AppData region. An amiibo that
has never had game data written still carries uninitialized bytes there, so the scan said yes and
the title ID / AppID fields beside it were rendered from junk.

**Fix — use the flags byte, which exists precisely for this.** From 3dbrew:

| Bit | Meaning |
|---|---|
| 4 (`0x10`) | amiibo was set up with amiibo Settings (owner registered) |
| 5 (`0x20`) | **AppData was initialized** |

> "Bit5=1 indicates that the AppData was initialized. `NFC:InitializeWriteAppData` will return an
> error if this is value 1, when successful that command will then set this bit to value 1."

So `hasAppData` is now `(flags & 0x20) !== 0`, and `titleId` / `appId` are only surfaced when it is
set. This is the same class of bug as the earlier mojibake owner/nickname, fixed the same way: bit 4
already gated those, and bit 5 was simply never wired up.

3dbrew also notes: *"Bits 4 and 5 control whether the setup date is loaded — the date is only used
when at least one of these bits is set to 1."* Both dates are now gated on `bit4 || bit5` for the
same reason.

Regression tests in `test_amiibo_decrypt.mjs` cover an amiibo that is registered but has **no** game
data, with deliberately randomised bytes across the title ID / AppID / AppData region: it must still
report its owner, must report `hasAppData === false`, must surface no title ID or AppID, must label
game data `None`, and must still read its setup date via bit 4. A second case with neither bit set
must report no dates at all.

**Also fixed:** the Eject amiibo button was rendered permanently greyed out when there was nothing
loaded and no adapter attached. It is now hidden in that state, and starts hidden so it cannot flash
before the first state update.

## 10. Initialization reported failure after succeeding (2026-07-27)

`Could not initialize this amiibo. setStatus is not defined` — `setStatus()` does not exist in the
portal; the idiom is `$("#amiiboMessage").textContent`.

Two separate problems, and the second is the worse one.

**The call was wrong.** Fixed to the real idiom.

**The error message lied.** The bad call sat on the *last* line of the handler, after the wiped image
had already been written to the library. So the amiibo *was* initialized, and the portal said it was
not. "Could not initialize" reads as "nothing happened", which invites the user to retry a
destructive operation that already completed.

The handler now tracks `committed`, set the moment the library write lands. Everything that can
genuinely fail happens before that point; a failure afterwards reports success plus "Refresh to
update the view" and logs the detail to the console. **Report the outcome of the operation, not the
outcome of the last statement.**

### The check that would have caught it

`tools/test_portal_symbols.mjs` flags calls to names that are defined nowhere in the page. The
portal is a single file with no build step and no type checking, so an undefined call surfaces only
at runtime, in whatever path reaches it — here, only once someone actually initialized an amiibo.

It is a name-existence check, not scope analysis: "is this name defined somewhere in the file, or a
known global?". Definitions are gathered from the raw source so the check can only miss a problem,
never invent one. Comments and string contents are blanked by a small character scanner rather than
a regex — a regex attempt both swallowed real code after an awkward template literal and read prose
like `amiibo (v3)` and CSS like `rgb(0,0,0)` as call sites. `${...}` inside template literals is
still scanned, since it holds real code.

Verified against the actual bug: reintroducing `setStatus()` makes it fail with the offending name
and line; removing it passes. 522 names currently in scope.
