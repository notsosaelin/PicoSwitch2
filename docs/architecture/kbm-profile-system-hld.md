# Keyboard / Mouse Layout, Template and Profile System

**Status:** SHIPPED across firmware, Windows and Android, 2026-08-30. Source- and
test-validated; end-to-end hardware validation outstanding (see §14).
**Date:** 2026-08-29, rewritten 2026-08-30 to describe what actually shipped
**Scope:** KB/M mapping selection in firmware, the management surface, Windows and Android
**Evidence baseline:** hardware failures recorded in
[`ble-keyboard-classified-as-mouse-2026-08-29.md`](../experiments/ble-keyboard-classified-as-mouse-2026-08-29.md)

## 1. What this fixes

PicoSwitch2 had two KB/M mapping profiles. They were not chosen — they were
**derived** from which peer roles happened to be filled, one-to-one from the
input mode. There was no `kbm profile` command and there never had been.

That produced a hardware-observed failure: a user bound `key:05 → B`, the adapter
reported success, the binding persisted and read back correctly, and the key did
nothing at the console — because the adapter was resolving the *other* mapping.
Every management operation was truthful and the product was still wrong.

A second, quieter problem: the editor wrote `kbm bind` on every keystroke. That
erased flash once per changed key, and made Save and Discard impossible to offer,
because the change had already happened.

## 2. The five concepts, kept apart

Collapsing any two of these is what produced the failures above.

| Concept | Answers | Chosen by |
|---|---|---|
| **Runtime mode** | Which peers may own the console? | User, or inferred (`auto`) |
| **Layout** | Keyboard, or keyboard and mouse? | Derived from admitted roles. **Never** a user assertion about hardware. |
| **Template** | What does a mapping start from? | Immutable ROM. One `Default` per layout. |
| **Saved profile** | A named mapping in the library | User. Six custom, across both layouts. |
| **Active realized mapping** | What the console is **actually** running | Only ever changed by Apply. |

The critical one is the last. Active is a **snapshot**, not a pointer into a
mutable profile slot. A pointer model cannot express "saved but not applied" at
all — saving would silently become applying.

## 3. Storage (schema 14)

```c
#define NS2_KBM_MAX_PROFILES 6u        // CUSTOM profiles; Defaults are built in
#define NS2_KBM_PROFILE_NAME_MAX 20u   // 19 usable characters

typedef struct {                       // everything a profile OWNS
    ns2_kbm_profile_overrides_t overrides;   // sparse, unchanged from v13
    ns2_kbm_mouse_config_t mouse;            // now profile-owned
} ns2_kbm_content_t;                   // 206 bytes

typedef struct {
    uint8_t used, layout, profile_id, reserved;
    uint16_t revision;
    char name[NS2_KBM_PROFILE_NAME_MAX];
    ns2_kbm_content_t content;
} ns2_kbm_profile_slot_t;              // 232 bytes

typedef struct {
    uint8_t source_id, reserved;       // which profile produced this snapshot
    uint16_t source_revision;
    ns2_kbm_content_t content;         // the REALIZED mapping
} ns2_kbm_active_t;                    // 210 bytes

typedef struct {
    uint8_t mode, next_profile_id, reserved[2];
    ns2_kbm_profile_slot_t profiles[NS2_KBM_MAX_PROFILES];
    ns2_kbm_active_t active[NS2_KBM_LAYOUT_COUNT];
} ns2_kbm_config_t;                    // 1816 bytes
```

**`config_record_t` is 1888 of `CONFIG_RECORD_BYTES` (2048)**, asserted at compile
time. Widening from 1024 was verified against the persistence map rather than
inferred from the sector size:

```
SIZE-1S  reserved by the SDK on RP2350
SIZE-2S  BTstack TLV bank B
SIZE-3S  BTstack TLV bank A
SIZE-4S  PicoSwitch2 config   <- ours alone, erased whole on every save
SIZE-5S  amiibo journal bank 1
SIZE-6S  amiibo journal bank 0
```

There is no A/B config record whose atomicity widening could compromise. The
erase already covers all 4096 bytes whatever `CONFIG_RECORD_BYTES` is, so the
window in which a power loss costs the settings is unchanged.

### Identity, revision, fingerprint

- **`profile_id`** is stable across storage-slot reuse. A companion caches drafts
  against an id; if ids were slot indexes, deleting a profile and creating
  another would silently rebind a stale draft to an unrelated mapping. Ids are
  monotonic from 2, wrapping and skipping ids still in use. `1` is the built-in
  Default; `0` is none.
- **`revision`** is bumped by every successful save. A draft carries the revision
  it was built from, and a mismatch is a **conflict**, never a silent overwrite.
  Rename deliberately does *not* bump it: no draft's mapping content became
  stale, so invalidating drafts would be a conflict the user cannot explain.
- **Fingerprint** is FNV-1a over canonicalized content, computed identically in
  C, C# and Kotlin. Canonical form sorts overrides by (kind, code) and drops
  entries that merely restate the layout's default, so two mappings that *behave*
  identically fingerprint identically however they were built. It answers "is
  what I saved what is running?" without trusting a local flag, and lets Apply
  skip a flash write when content is already realized.

**It is not a security primitive and not the record's integrity check.**

### What is profile-owned

Audited rather than assumed:

| Field | Owner | Why |
|---|---|---|
| Key and mouse-button bindings | **Profile** | The mapping itself |
| Mouse sensitivity X/Y | **Profile** | A user expects a different feel between a mapping built for one game and one built for another |
| Mouse inversion X/Y | **Profile** | Same |
| Anti-deadzone | **Profile** | Compensation for a specific destination's dead low end |
| Velocity window / idle deadline | Compile-time | Properties of the Bluetooth transport, not preferences |
| Input mode override | Global | Admission policy, unrelated to mapping |
| Colours, wake identity, bonds | Global | Nothing to do with KB/M |

## 4. Migration v13 → v14

Non-destructive, and proven by resolving bindings through the migrated record
rather than by comparing structs.

Per layout:

- **the stored mapping equals the canonical default** → realize the built-in
  Default template. No custom slot is consumed, because there is nothing of the
  user's to keep.
- **it differs** → keep it as a named custom profile, `Current Keyboard` or
  `Current KB + Mouse`, and realize it. The user's mapping survives, gains a name
  they can see, and is immediately the one in use, so nothing about their console
  changes on upgrade.

Mouse settings were global in v13 and are profile-owned in v14, so they are
copied into **both** layouts — whichever layout the adapter resolves, it finds
the settings the user had.

**The v13 management-companion table survives byte for byte.** A dedicated
regression asserts a v13 companion is still recognised after migration, because
regressing it would bring back fake Paired Controllers and let a front end be
forgotten through the wrong companion.

v11 and v12 lift to the v13 shape first and then through the same one migration,
so an adapter on any supported schema reaches byte-identical v14 state.

## 5. Save is not Apply

The contract, pinned by tests at the model layer rather than left to UI
convention:

```
Work revision 3, applied.        console runs revision 3
  user edits locally             console runs revision 3, ZERO adapter writes
  user saves                     Work becomes revision 4
                                 console STILL runs revision 3
                                 activeMatchesSaved -> false
  user presses Set Active        console runs revision 4
                                 activeMatchesSaved -> true
```

`ns2_kbm_resolve()` reads **only** the realized snapshot. Resolving through the
library would make Save an implicit Apply, which is precisely the bug.

`ns2_kbm_active_matches_source()` checks both the revision and the content: the
revision catches "saved but not applied", and the content comparison catches a
legacy per-binding write that mutated the realized mapping without touching any
saved profile.

## 6. Staged save transaction

A profile does not fit one management frame, and a loop of `kbm bind` is not a
transaction — a disconnect halfway leaves the adapter running half of one mapping
and half of another, and every step erases flash.

```
kbm draft begin <kb|kbm> <id|new> <baseRevision> <name>
kbm draft bind <src> <dst>          (repeated, bounded by the override table)
kbm draft mouse <field> <value>     (repeated)
kbm draft commit                    -> {"ok":true,"id":N,"revision":M}
kbm draft abort
```

Nothing before `commit` touches stored or realized state. `begin` replaces the
staging buffer rather than failing, so a client that reconnected after a dropped
session can start over without a stuck transaction it cannot see. One buffer is
enough because management admits a single trusted session; it is static for the
same reason the peers workspace is — Pico W's core-1 stack is 2048 bytes.

**This protects against a partial or abandoned TRANSFER.** It is *not* protection
against power loss during the final config-sector write, which remains an
existing limitation of the single-bank, non-CRC record.

## 7. Management surface

| Command | Purpose |
|---|---|
| `kbm profiles` | The library. Paginated defensively with a suffix reserve. |
| `kbm active` | Both realized mappings, with `matchesSaved` |
| `kbm apply <kb\|kbm> <id\|default>` | **The only command that changes console behaviour** |
| `kbm pmap <id> [page]` | One STORED profile's mapping |
| `kbm profile rename\|dup\|delete` | Library metadata |
| `kbm draft …` | The staged save above |
| `kbm status` | Gains `activeProfile`, `activeRevision`, `activeFingerprint`, `activeMatchesSaved` |

Error strings are stable and specific: `profile storage full or name in use`,
`profile not found`, `profile not found for that layout`, `stale revision`,
`no draft`, `incomplete transaction`, `invalid settings`, `mapping storage full`.

All new verbs are `kbm …`, already covered by the BLE allowlist's `kbm ` prefix
and gated behind the existing trusted management session. Parity: **60 companion
commands, all dispatched and all reachable over BLE.**

### Legacy compatibility

`kbm map`, `kbm bind`, `kbm reset` and `kbm mouse` keep their existing meaning:
they name a **layout** and act on its **realized** mapping, immediately, as their
clients have always relied on. A legacy write therefore makes the snapshot stop
matching the profile that produced it, and `matchesSaved` reports that truthfully
rather than letting a client keep claiming the profile is applied.

`kbm map kb` still reports what the console is really using, which is what an old
client expects.

## 8. Client draft model

Both companions implement the same state machine, and the rules live beside the
wire types (`KeyboardMouseDraft` / `KbmDraft`) so the two cannot drift:

| State | Meaning |
|---|---|
| Clean | Draft equals the saved profile, which is not applied |
| Active | The realized mapping matches this profile's saved content |
| Dirty | Edited locally. **Zero adapter writes have happened** |
| SavedNotApplied | Stored in the library; the console runs something else |
| Conflict | The adapter's profile moved on since this draft was based on it |
| Disconnected | No live session — nothing may be presented as live truth |

Dirty is computed from **content**, not tracked with a flag, so putting an edit
back correctly disables Save. "Active" requires the id **and** the content to
match; an id match alone is what would let the UI claim a profile is applied when
it was saved and never applied.

**Flash writes:** editing 30 controls → 0. Save → 1. Apply → 1 (0 when the
content is already realized). Rename / duplicate / delete → 1 each.

## 9. UX

Both platforms: a profile selector that **opens** a profile (never applies it),
`Save` / `Discard` / `Set Active` as separate controls with separate enablement,
`New` / `Rename` / `Delete`, and one line of status plus a detail sentence for the
states a single word cannot carry. Built-in Default is labelled as such and
cannot be renamed or deleted; saving while viewing it creates a profile of the
user's own.

Windows keeps its section-scoped busy ring rather than disabling the page, and
business rules stay in Services/Presentation. Android keeps rules in the
repository and ViewModel; the composable reads state and raises events.

## 10. Older firmware

`kbm profiles` answering `unknown command` is not a degraded state to apologise
for: that adapter has exactly one mapping per layout and behaves as the app
always did. The profile controls are not offered, and per-binding writes remain —
because that is genuinely the only mapping surface such an adapter has. Detected
by capability probe, never by version-string comparison.

## 11. Config durability — stated plainly

- The settings record is **single-bank, erase-then-program, and carries no CRC**.
- `ns2_kbm_config_sanitize()` rejects malformed or impossible state: a profile
  whose layout is unreadable is dropped rather than resolved against the wrong
  canonical map, duplicate ids are dropped, a live profile carrying the reserved
  revision-0 sentinel is repaired, and an active snapshot naming a source that no
  longer exists **keeps its content** — the user's console behaviour does not
  change because a profile vanished — and is re-labelled as matching nothing.
- **Sanitize is not torn-write detection.** A power loss during the final flash
  programming remains an existing durability limitation.
- The staged transaction protects against partial or abandoned BLE transfers, not
  against power loss during the final config-sector write.
- A CRC or A/B config store is a separate future durability decision and was
  deliberately **not** smuggled into this feature.

## 12. Deliberately not built

- Per-game or title-aware switching — the adapter cannot know what is running.
- Profile-switch key chords — every chord is a chord some game wants.
- User-selectable layout — asserting Keyboard + Mouse with no mouse silently
  drops the right stick.
- Game-shaped templates (FPS, platformer, …) — inventing them without evidence is
  the speculation `PLAN.md` warns against. The mechanism takes a new template as
  a one-row data change.
- A generic mapping framework.

## 13. Settled decisions

1. **Six CUSTOM profiles**, shared across layouts. Built-in Defaults consume no
   slot.
2. **`CONFIG_RECORD_BYTES` = 2048.** Verified against the flash map; nothing
   transactional is compromised.
3. **Default template only.** Content is a data-only addition later.
4. **Mouse settings are profile-owned.**
5. **No CRC / A-B config store in this feature.**

## 14. What is not proven

Everything here is source- and test-validated. **No part of the profile system
has run on hardware.** Specifically unproven until the smoke test in the final
report is run:

- the v13 → v14 migration against a real adapter's stored bytes;
- the staged transaction over a real BLE management session;
- that Apply changes console behaviour and Save does not, at the console;
- persistence of the realized mapping across a real power cycle;
- cross-platform hand-off between the two companions.

The hardware-confirmed 8BitDo multi-report-ID fix and the durable
management-companion fix are both covered by regressions in the host suite and
are not affected by this work.
