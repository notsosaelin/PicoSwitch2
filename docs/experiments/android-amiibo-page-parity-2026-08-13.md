# Android Amiibo page parity — bounded evidence note — 2026-08-13

## Question

What is the highest-value safe Amiibo-page parity slice for the native Android companion, given
that the Web Portal already has richer identity/details, local `key_retail.bin` handling, catalog
enrichment, initialization, and a mature single-slot adapter workflow?

## Portal evidence used

`web/index.html` is the current portal authority for this slice:

- `parseAmiiboIdentity()` reads the UID, figure ID, character game code/variant, tag type, model,
  series, format version, and extended variant from the plaintext identity block.
- The `amiibo-decrypt` block validates exactly 160 bytes split into two 80-byte masters, derives
  keys with HMAC-SHA256/DRBG, decrypts the settings block with AES-CTR, and verifies both HMACs.
- `amiiboReadRegisterInfo()` gates owner/nickname on the registered flag, gates title/AppID on the
  AppData flag, decodes packed dates, and reads the big-endian write counter. Its offsets are
  documented in `docs/switch2/amiibo-decrypted-data-surface.md`.
- Portal import, upload, Sync, dirty protection, present/eject, and clear never require keys. The
  portal catalog is enhancement-only and its network failure never blocks local import.

The portal’s crypto path and the documented offsets are implementation evidence, not a claim that
Android has been hardware-validated. No retail key or raw decrypted dump is included in this
repository.

## Implemented Android slice

`android/companion` now:

1. Persists the richer plaintext identity fields in each private library index record and renders
   them in both the adaptive grid and detail pane.
2. Accepts only a portal-compatible 160-byte user key file. The two master labels are checked,
   reversed master order is normalized, and invalid files are rejected before replacement.
3. Stores the validated key only at app-private `filesDir/amiibo-private/.amiibo-retail-key.bin`.
   Android backup is disabled; key bytes are absent from diagnostics, management JSON, and library
   files/exports. Import/replace lives under Settings rather than the library surface; uninstall
   removes the app-private key file.
4. Reads owner, nickname, registration date, last-write date, write count, AppID/title ID, and a
   conservative game-data label only after both HMACs verify. Invalid HMACs show no decrypted
   strings, preventing random encrypted bytes from appearing as names.
5. Adds a compact cache-first AmiiboAPI enrichment layer. It derives the same uppercase `head + tail`
   figure ID as the portal, stores friendly name/character/series/type/release, compatible game
   names, and title-ID labels for seven days, limits responses to 4 MiB, tries two mirrors with
   short timeouts, and never sends local tag bytes, UIDs, keys, or decrypted fields. Detail artwork
   is a bounded best-effort image fetch with an offline icon fallback.
6. Leaves all import, local selection, upload, Sync, present/eject, clear, and clean/used copy
   controls key-free and network-free.
7. Redesigns the page around portal-like artwork/library/detail hierarchy. On compact widths the
   selected hero is bounded and the saved figures remain a scrollable list. An active adapter tag
   is a first-class display item even when `library.json` is `{"version":1,"items":[]}`: its
   figure ID starts catalog lookup immediately, and the UI reports loading, offline, or unmatched
   instead of silently rendering an empty card. Adapter actions (download, present/eject, guarded
   clear) remain visible without importing or syncing first.

## Deliberate deferrals

Initialization/re-signing is not exposed in this slice. A future implementation must port the
portal pack/self-verify path and add a local crash-safe replacement transaction before offering a
destructive button. ZIP exchange, phone NFC reads, and Mii rendering remain deferred. Catalog and
artwork are enhancement-only: a cold/offline cache miss leaves the local identity/details and every
adapter operation usable.

## Validation level

- JVM tests cover key length/master-label validation, reversed-master normalization, richer identity
  parsing for ordinary and figure-v3 images, packed-date edges, HMAC-invalid field suppression,
  private key-store import/replacement behavior, and a deterministic portal-generated golden vector that
  extracts owner/nickname/dates/write count/title ID/AppID in Kotlin.
- `tools/test_amiibo_decrypt.mjs` both generates/checks the same dummy-key golden vector (with
  `--write-golden`) and runs the independent portal crypto round-trip. The Android port was not
  flashed or exercised against a real adapter in this bounded change.
- With a temporary official Temurin JDK 21, the Android module passed 52 JVM tests, lint, and debug
  APK assembly. The default Android Studio JBR 25 remains unsuitable for this Gradle wrapper; no
  permanent toolchain change was made.

## Follow-up redesign evidence

The Thor screenshot repro (1240x1080 physical, 538x444dp app surface) showed a thin bullet-like
adapter row because the old screen spent the viewport on a permanent title, three large action
buttons, privacy copy, and a fixed-height compact detail pane. The live app-private index was also
empty, so the old `catalogEntriesFor()` never queried the adapter figure ID. The redesign removes
the destructive-looking key action, keeps import/replace compact, uses a bounded hero plus scroll,
and introduces `AmiiboCatalogState` (`Loading`, `Available`, `Offline`, `Unmatched`) for adapter-only
identity. A compact sort menu now deterministically orders both local rows and the wide grid by
friendly Name, Series, or Recently added. This is source/build evidence; a rebuilt APK still needs
a physical Thor visual pass.
