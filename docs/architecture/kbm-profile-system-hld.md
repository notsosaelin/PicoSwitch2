# Keyboard / Mouse Layout, Template and Profile System

**Status:** SHIPPED across firmware, Windows and Android, 2026-08-30. Source- and
test-validated; end-to-end hardware validation outstanding (see §14).
**Date:** 2026-08-29; rewritten 2026-08-30 to describe what shipped; revised
2026-08-30 for schema 16 — bank positions, switch keys, boot vs runtime, and the
local-library / resident-bank split (§2a, §3, §4a, §5, §7, §8, §9).
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

## 2a. Two stores, and the position that addresses one of them

The original model had one store and it was the adapter's. That was the second
architectural mistake, and it is the one this revision fixes.

| Store | Holds | Bounded by | Needs a connection |
|---|---|---|---|
| **Local library** | The user's collection, on the companion | Nothing | **No** |
| **Resident bank** | The adapter's working set | 3 positions × 2 layouts, plus a built-in Default | Yes |

Six was never a statement about how many mappings a person may have. It is how
many the **adapter** can hold so that it keeps working with no companion
attached. Treating it as the user's capacity made `New` a flash erase, made
`Save` change what the console might run, and made both impossible while
disconnected — for a task, composing a key mapping, that never needed a device.

A **position** is what the user actually selects: `Default`, `Profile 1`,
`Profile 2`, `Profile 3`, scoped to a layout. It is deliberately *not* a storage
slot and *not* a `profile_id`. "Profile 1" means the same thing to a person in
both banks, and the derived layout decides which one is read — which is exactly
what lets a single switch key serve both. Exposing ids here would force the user
to know which record lives where.

Copying between the two stores is always explicit:

```
local library  --Assign / Update-->  resident bank
local library  <--Copy to library--  resident bank
```

Neither direction ever happens as a side effect of an edit.

## 3. Storage (schema 16)

```c
#define NS2_KBM_MAX_PROFILES 6u          // = POSITIONS_PER_LAYOUT * LAYOUT_COUNT
#define NS2_KBM_POSITIONS_PER_LAYOUT 3u  // custom positions in ONE bank
#define NS2_KBM_POSITION_DEFAULT 0u      // the built-in; consumes no record
#define NS2_KBM_PROFILE_NAME_MAX 20u     // 19 usable characters
#define NS2_KBM_SWITCH_BINDINGS_MAX 4u   // Default + Profile 1..3

typedef struct {                       // everything a profile OWNS
    ns2_kbm_profile_overrides_t overrides;   // sparse, unchanged from v13
    ns2_kbm_mouse_config_t mouse;            // profile-owned since v14
} ns2_kbm_content_t;                   // 206 bytes

typedef struct {
    uint8_t used, layout, profile_id, position;   // v16 spends v15's reserved byte
    uint16_t revision;
    char name[NS2_KBM_PROFILE_NAME_MAX];
    ns2_kbm_content_t content;
} ns2_kbm_profile_slot_t;              // 232 bytes, unchanged in size

typedef struct {
    uint8_t source_id, reserved;       // which profile produced this snapshot
    uint16_t source_revision;
    ns2_kbm_content_t content;         // the REALIZED mapping
} ns2_kbm_active_t;                    // 210 bytes

typedef struct {                       // v15: change profile with no app
    uint8_t used, kind, position;
    uint8_t code;                      // HID usage, or mouse button
} ns2_kbm_switch_binding_t;

typedef struct {
    uint8_t mode, next_profile_id, reserved[2];
    ns2_kbm_profile_slot_t profiles[NS2_KBM_MAX_PROFILES];
    ns2_kbm_active_t active[NS2_KBM_LAYOUT_COUNT];
    ns2_kbm_switch_binding_t switches[NS2_KBM_SWITCH_BINDINGS_MAX];  // ONE table
    uint8_t boot_position[NS2_KBM_LAYOUT_COUNT];   // what to realize at power-up
    uint8_t reserved3[…];
} ns2_kbm_config_t;
```

`NS2_KBM_MAX_PROFILES == NS2_KBM_POSITIONS_PER_LAYOUT * NS2_KBM_LAYOUT_COUNT` is
asserted at compile time, so the record cannot grow a seventh profile that no
position addresses.

**The switch table is layout-free and shared.** A binding names a *position*, and
the adapter resolves it through whichever layout is derived when the key is
pressed. One key therefore works in both banks and selects that bank's profile,
which is the behaviour a person expects and the reason position is the unit
rather than id.

### Boot position is not runtime position

```
boot_position[layout]     persisted. Costs a flash write. Set by "On startup".
active[layout]            realized NOW. Costs nothing. Moved by Apply and by a
                          switch key.
```

They are separate because a switch key moves the second and not the first, so
after one press they legitimately differ for the rest of the session. A client
that assumed the persisted choice was live would report the wrong profile as
active — and a hotkey that wrote flash on every press would be unusable.

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

## 4a. Migration v14 → v15 → v16

`CONFIG_PERSIST_VERSION` is **16**.

- **v15** adds the switch-key table and `boot_position[]`. Both start empty /
  `Default`, which is the behaviour a v14 adapter already had.
- **v16** gives every stored profile a `position`. Assignment is deterministic —
  slot order, within the profile's own layout — so the same stored record always
  lands on the same position, and two adapters with identical bytes migrate
  identically.

`boot_position[]` is derived from **what was already realized**, not reset. An
adapter that came up running the user's mapping keeps coming up running it.

**The rule is applied on every MIGRATED path, not just the newest one.** An
earlier version applied boot realization only in the v14 branch, so a record
arriving from v11/v12/v13 was migrated correctly and then had its mapping
discarded on the way through. A record that has travelled further must not end
up worse off than one that started closer.

## 5. Save is not Apply

The contract, pinned by tests at the model layer rather than left to UI
convention. With the local library in place there are now **three** steps, not
two, and each one is a separate act by the user:

```
Halo is in the library and assigned to Profile 1, which is running.

  user edits locally     library unchanged, ZERO adapter writes
  user SAVES             the LIBRARY changes. The adapter does not.
                         the row reads "Profile 1 · adapter copy out of date"
  user UPDATES           Profile 1 now holds the new content
                         the console STILL runs the old snapshot
                         matchesSaved -> false, "activate to use changes"
  user ACTIVATES         the console runs the new content
                         matchesSaved -> true
```

Save reaching the adapter is the defect this replaces; Assign reaching the
console would be the same defect one layer along. Assigning into the position
that is currently running deliberately preserves the realized snapshot, so
refreshing a stored copy can never change gameplay mid-session.

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
| `kbm profiles [cursor]` | The resident bank, walked by cursor. Each row carries its `position` |
| `kbm active` | Both realized mappings, with `matchesSaved`, `bootPosition`, `runtimePosition` |
| `kbm apply <kb\|kbm> <id\|default>` | **The only command that changes console behaviour.** Runtime only; no flash write |
| `kbm boot <kb\|kbm> <default\|1-3>` | The power-up choice. The one profile selection worth a flash write |
| `kbm remove <kb\|kbm> <1-3>` | Empty one bank position. The local library is untouched |
| `kbm switches` | The shared switch-key table |
| `kbm switch <src> <none\|default\|1-3>` | Bind or clear one switch key |
| `kbm map <kb\|kbm> [cursor]` | One layout's REALIZED mapping, walked by cursor |
| `kbm pmap <id> [cursor]` | One STORED profile's mapping, same walk |
| `kbm profile rename\|dup\|delete` | Resident metadata |
| `kbm draft …` | The staged save/assign transaction above |
| `kbm status` | Product state only. Gains `activeProfile`, `activeProfileName`, `activeRevision`, `activeFingerprint`, `activeMatchesSaved` |
| `kbm counters` | The 15 ingress counters, split out of `kbm status` |

`kbm draft begin` takes either a profile id (save into that record) or
**`pos:N`** (assign into that position). The second form is what the local
library uses: the client names the position the user picked and the adapter
decides which record backs it.

**`default` is a word on the wire, not `0`.** `kbm_position_arg()` accepts the
literal `default` and rejects any number below 1, so a client that sent a
position's numeric value would have its command refused for the one action a
user is most likely to bind a key to.

**Removing a position can never leave a dangling reference.** If the removed
position was the runtime choice or the boot choice, that layout falls back to
`Default` inside the same operation. A client re-reads the realized mapping
afterwards rather than assuming, because what the console is running may have
changed underneath it.

### The wire budget

`CONFIG_WIRELESS_RESPONSE_MAX_JSON` is **511 bytes** — the 512-byte slot minus
the newline the bridge appends. That constant is the single authority; deriving
from it rather than restating 512 matters, because a formatter budgeted against
the raw capacity is off by one exactly at the boundary.

A longer reply is **not truncated**. The bridge substitutes
`{"error":"response_too_large","code":413}` and the caller loses the *whole*
read, so one oversized reply takes down every value the page needed.

This was not theoretical. Adding the active-mapping identity to `kbm status`
pushed it to **729 bytes worst case** (559 measured live), which made the entire
Keyboard & Mouse page fail to load on hardware. Hence: **product state and
counters are two commands** (318 B and 414 B worst case), merged by the client.

### Cursor pagination — and why not page indices

**A fixed page size is unsafe here, and its failure mode is silent data loss.**

Rows are variable width (a 6–7 character source, a 4–12 character destination),
so the number that fits one 511-byte reply is not a constant. The first
implementation asked for `page N`, answered from `first = N * PAGE_SIZE`, and
then stopped early when the byte budget ran out. Those two rules contradict:
emitting 7 rows for a nominal page size of 8 and answering the next request from
index 8 skips index 7 entirely.

Measured against the Default template that ships on an adapter: the Keyboard
layout has **26** bindings and a client received **25** — index 7,
`key:0F → rstick_right`, never sent. `rstick_right` is the longest destination
name, so it was the row the budget cut. Every reply was individually valid and
under the limit; `more` was true; progress looked fine. The only symptom was
`Adapter returned an incomplete KB/M binding list`, and the page never loaded.

No constant rescues this: any page size is either unsafe for the worst-case row
or wasteful for the common one, and guessing wrong loses rows without a trace.

So the request carries a **cursor** — the index of the next logical item — and
the firmware, the only party that knows how many rows it actually serialized,
answers with where to resume:

```
-> kbm map kb 0
<- {"profile":"kb","profileId":1,"cursor":0,"total":26,"bindings":[…],"next":8}
-> kbm map kb 8
<- {…,"cursor":8,"total":26,"bindings":[…],"next":null}
```

`next` is null exactly when the walk is complete — the same shape `bonds list v2`
already uses here. Guaranteed properties, each pinned by a host test:

1. every reply ≤ `NS2_KBM_REPLY_MAX_BYTES`, for worst-case content;
2. every logical item appears exactly once — no boundary gaps, no duplicates;
3. a reply with non-null `next` always advanced, so a client cannot loop;
4. `next` is null if and only if the walk is complete;
5. at least one item per reply while any remain, so (3) does not hold by luck.

Clients reject a reply whose `next ≠ cursor + rows` at the **decoder**: that is
an internally inconsistent reply, and catching it there names the real problem
instead of a downstream short mapping.

### Where these formatters live, and why

The read formatters are in **`src/ns2_kbm_commands.c`**, not `src/config.c`.
`config.c` is bound to TinyUSB, BTstack and the flash driver, so it does not
compile on the host — which meant its pagination was covered only by
hand-written client fixtures, written from the same misunderstanding that
produced the bug. Both C# and Kotlin fixtures agreed with the broken firmware.

`tools/test_ns2_kbm_commands.c` drives the real formatter across every layout,
profile and cursor under worst-case content, and **generates
`tools/fixtures/management/kbm-wire-corpus.json`** — the exact firmware bytes,
which the Windows and Android integration tests replay through their real
clients. Three implementations now check against one authority.

`tools/test_ns2_kbm_status.c` saturates every field and asserts the **wire**
limit. Its earlier version asserted against 2048 — a firmware-local buffer —
which is why it passed while the adapter was refusing the reply.

### Diagnosing this on the bench

`cfg <command>` on the UART console runs any management command and reports the
size the wireless bridge would see:

```
cfg kbm map kb 0   ->  {"bytes":488,"limit":511,"fits":true,"reply":{…}}
```

It exists because both defects above had to be diagnosed from source: the UART
dispatcher knows only a few `kbm` verbs, and everything else was reachable only
over BLE or the CDC Config personality. UART-only, off the BLE allowlist.

Error strings are stable and specific: `profile storage full or name in use`,
`profile not found`, `profile not found for that layout`, `stale revision`,
`no draft`, `incomplete transaction`, `invalid settings`, `mapping storage full`.

All new verbs are `kbm …`, already covered by the BLE allowlist's `kbm ` prefix
and gated behind the existing trusted management session. Parity: **62 companion
commands, all dispatched and all reachable over BLE.**

### Legacy compatibility

`kbm map`, `kbm bind`, `kbm reset` and `kbm mouse` keep their existing meaning:
they name a **layout** and act on its **realized** mapping, immediately, as their
clients have always relied on. A legacy write therefore makes the snapshot stop
matching the profile that produced it, and `matchesSaved` reports that truthfully
rather than letting a client keep claiming the profile is applied.

`kbm map kb` still reports what the console is really using, which is what an old
client expects.

## 8. Client model

The editing path runs on the **local draft** (`KbmLocalDraft`, both languages),
keyed on a library UUID that no adapter has ever seen. Its only states are:

| State | Meaning |
|---|---|
| Clean | Equals what is stored in the library |
| Dirty | Edited locally. **Zero adapter writes have happened** |

Dirty is computed from **content**, not tracked with a flag, so putting an edit
back correctly disables Save.

There is deliberately no third state here. Where a profile stands relative to a
device is a **relationship**, not a property of the draft, and it is computed by
the bank projection (`KbmBankView`, both languages) from three independent
comparisons — local vs resident, resident vs runtime, and the conclusion drawn
from them:

| Relationship | Meaning |
|---|---|
| LocalOnly | In the library only. Never sent to this adapter |
| OnAdapter | Assigned to a position, and the copies agree |
| AdapterCopyOutOfDate | Assigned, edited locally since. The adapter holds the older content |
| Active | Assigned, in agreement, and running now |
| ResidentUpdatedNotActivated | The resident copy was updated; the console has not picked it up |

A single `matchesSaved` boolean cannot express the third row, which is the
**ordinary** state right after a local Save and the exact fact the user needs.

### Matching a local profile to its resident copy

The two companions mint their own ids and never see each other's, so a resident
copy is matched by **content** — the fingerprint — with the name as evidence.
A resident belongs to at most one library row, and claiming consumes:

```
1. name AND content   strongest
2. name
3. content            weakest
```

**Name outranks content alone, and the order is load-bearing.** Editing is the
common case and changes content, so right after a Save a profile no longer
matches its own resident — but it may coincidentally match a *different* one.
Content-first made an edited "Halo" claim an unrelated Profile 2 and report
itself safely on the adapter, hiding the one fact that mattered. Content still
decides when no name matches, which is what keeps a locally renamed profile
attached to the resident the other companion wrote.

Unique claiming matters for a second reason: two untouched copies of Default have
identical content, and without it both would read as "on adapter".

### Offline

The library, the editor and the whole mapping grid work with **no adapter**. A
profile stores sparse overrides, and the canonical table those are applied
against is firmware data shipped in both companions
(`tools/fixtures/management/kbm-default-mappings.json`, generated from
`src/ns2_kbm.c`). Parity tests on both platforms assert the shipped copy still
matches what the firmware emits, so carrying it cannot silently drift.

Adapter-only controls are **disabled, not hidden**: a user who cannot see them
cannot tell that connecting would bring them back.

**Flash writes:** editing 30 controls → 0. Local save / duplicate / rename /
delete → 0. Assign → 1. On startup → 1. Activate → 0. Remove → 1.

## 9. UX

Both platforms present the two stores as two cards.

**Your library** — the layout selector, every local profile with its relationship
line, and `New` / `Save` / `Discard` / `Duplicate` / `Rename` / `Delete`. All
local, all available offline. Selecting a row **opens** it; it never applies it.
Built-in Default is always offered, is labelled as such, and cannot be renamed or
deleted — saving while viewing it creates a profile of the user's own. The single
control here that reaches the adapter is `Assign to adapter…`, and it says so.

**On adapter** — one row per position including the empty ones, because
"Profile 3 · Empty" is what tells a user they have somewhere to assign to.
Each row carries `Activate`, `On startup`, `Copy to library` and `Remove`, with
`active now` / `on startup` / `key F1` shown as separate marks.

**Profile switch keys** — one row per semantic action, bound or not, rendered
from the actions rather than from the bindings so the ones the user came to set
up are visible.

Windows keeps its section-scoped busy ring rather than disabling the page, and
business rules stay in Services/Presentation. Android keeps rules in the
repository and ViewModel; the composable reads state and raises events.

## 10. Older firmware — ONE contract, no fallback

**The companions do not support pre-profile firmware.** This is a deliberate
product decision, taken because the alternative shipped a worse failure than the
one it was meant to soften.

The earlier design probed each family and degraded: `kbm profiles` unsupported
meant "show the pre-profile editor", `kbm counters` unsupported meant "show
zeroed counters". So when a genuine protocol defect made the read fail, the user
was left looking at an old half-working mapping page with no profile controls and
no statement that anything had gone wrong — indistinguishable from a feature that
had never been built, and it cost a hardware round trip to tell the two apart.
Zeroed counters are worse still: they read as a healthy adapter receiving no
input, which is precisely the condition that display exists to diagnose.

`kbm status`, `kbm counters`, `kbm profiles`, `kbm active` and `kbm map` are all
**required**. A missing one is reported as **Firmware update required**, naming
the command. There is no legacy editor behind that screen.

Both companions expose the page's state explicitly
(`KeyboardMouseReadiness` / `KbmReadiness`):

| State | Meaning |
|---|---|
| NotRead | Never read this session |
| Loading | Read in progress |
| **Ready** | The contract loaded. **The only state with a usable editor** |
| FirmwareUpdateRequired | A required command is missing — update the adapter |
| Error | Current firmware, unusable answer. A defect to chase, not a version gap |

Separating the last two matters: they send the user to different remedies, and
collapsing them was part of what made the pagination defect unreadable.

What is *kept* is **firmware config migration**. Real adapters hold real
persisted state; v11/v12/v13 → v14 migration stays supported, including the
durable management-companion table. Old *storage* is supported; old *protocol* is
not.

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
- Profile-switch key **chords** — every chord is a chord some game wants. Single
  switch keys *are* built (§3); chords are not.
- Syncing the two companions' libraries to each other. They share content through
  the adapter's resident bank and nothing else; a local-only profile stays local.
  Anything more would need an account, a transport and a conflict policy that
  this feature does not have.
- User-selectable layout — asserting Keyboard + Mouse with no mouse silently
  drops the right stick.
- Game-shaped templates (FPS, platformer, …) — inventing them without evidence is
  the speculation `PLAN.md` warns against. The mechanism takes a new template as
  a one-row data change.
- A generic mapping framework.

## 13. Settled decisions

1. **Six resident profiles = 3 positions × 2 layouts.** Built-in Defaults consume
   no record. This is the ADAPTER's working set, not the user's capacity: the
   local library is unbounded.
2. **`CONFIG_RECORD_BYTES` = 2048.** Verified against the flash map; nothing
   transactional is compromised.
3. **Default template only.** Content is a data-only addition later.
4. **Mouse settings are profile-owned.**
5. **No CRC / A-B config store in this feature.**
6. **Position, not id, is the user-facing unit**, and the switch-key table is one
   shared, layout-free table.
7. **Boot and runtime selection are separate**, and only boot writes flash.
8. **Save is local.** Assign, Update, Remove, Activate and On startup are the
   only operations that reach the adapter, and all five are explicit.
9. **The fingerprint is the cross-platform bridge.** Local ids are never shared
   between companions.

## 14. What is not proven

Everything here is source- and test-validated. **No part of the profile system
has run on hardware.** Specifically unproven:

- the v13 → v14 → v15 → v16 migration chain against a real adapter's stored bytes;
- the staged transaction, in both its `<id>` and `pos:N` forms, over a real BLE
  management session;
- that Apply changes console behaviour and Save does not, at the console;
- that a switch key changes the running profile at the console, and that it does
  **not** change the power-up choice;
- persistence of `boot_position[]` across a real power cycle;
- cross-platform hand-off between the two companions.

On the last point, what *is* proven in software is the contract the hand-off
rides on rather than the hand-off itself. Both companions' fingerprints are
replayed against vectors emitted by the firmware's own
`ns2_kbm_content_fingerprint` (`KbmFingerprintParityTest`,
`KbmFingerprintTests`), and the Android suite drives the whole
Windows → adapter → Android → adapter → Windows scenario through the real
library and bank projection using those firmware-derived vectors as the content
(`KbmCrossPlatformScenarioTest`). Two processes have never actually met.

The hardware-confirmed 8BitDo multi-report-ID fix and the durable
management-companion fix are both covered by regressions in the host suite and
are not affected by this work.
