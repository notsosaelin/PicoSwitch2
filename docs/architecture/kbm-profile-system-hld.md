# Keyboard / Mouse Mode, Template and Profile System — High-Level Design

**Status:** Proposed architecture; implementation is not authorized by this document
**Date:** 2026-08-29
**Scope:** KB/M mapping selection in firmware, the management surface, Windows and Android
**Evidence baseline:** hardware failures recorded in
[`ble-keyboard-classified-as-mouse-2026-08-29.md`](../experiments/ble-keyboard-classified-as-mouse-2026-08-29.md);
current implementation in `include/ns2_kbm.h`, `src/ns2_kbm.c`, `src/config.c`

## 1. Executive summary

PicoSwitch2 has two KB/M mapping profiles. They are not chosen — they are
**derived** from which peer roles happen to be filled, one-to-one from the input
mode. There is no `kbm profile` command, and there never was.

That produced a real, hardware-observed failure: a user bound `key:05 → B`, the
adapter reported success, the binding persisted and read back correctly, and the
key did nothing at the console — because the adapter was resolving the *other*
profile. Every management operation is truthful in that situation and the
product is still wrong. The Windows companion now warns about it, which treats
the symptom.

This design separates three things the current model collapses into two:

| Concept | Question it answers | Chosen by |
|---|---|---|
| **Mode** | Which peers may own the console? | User (or inferred, `auto`) |
| **Layout** | Which *shape* of mapping applies — keyboard alone, or keyboard and mouse? | Derived from admitted roles. Never chosen. |
| **Profile** | *Which* mapping of that shape is in use? | **User. This is new.** |

A **profile** is a named set of overrides belonging to exactly one layout.
Several may exist per layout; exactly one per layout is **active**. A
**template** is immutable ROM data — a starting point that can be applied into a
profile, never a thing that is selected or edited.

The sparse-override model is preserved unchanged: a profile stores only
deviations from its layout's canonical defaults, so "restore defaults" remains
"drop the overrides" rather than a procedural rebuild.

### Architectural decisions

| Decision | Selected approach | Reason |
|---|---|---|
| Vocabulary | Rename the derived `kb`/`kbm` axis to **layout**; `profile` becomes the user-selected named mapping | The word "profile" currently names a thing the user cannot select, which is exactly the confusion that produced the silent-binding failure |
| Profile scope | A profile belongs to one layout | The bindable source domain differs — mouse buttons exist only in `kbm` — and the two canonical defaults are deliberately not supersets of each other |
| Active selection | Exactly one active profile per layout, persisted | Gives the editor an unambiguous target, which removes the "bound into the wrong profile" failure class structurally rather than by warning |
| Templates | Immutable ROM tables, applied *into* a profile | Costs no flash, makes "apply" auditable and reversible, and keeps template content a data-only change |
| Override storage | Unchanged sparse override list over canonical defaults | Already validated; restoring defaults stays a deletion |
| Wire compatibility | `kb` / `kbm` keep working, resolving to that layout's active profile | Old clients and existing UART habits keep working unchanged |
| Schema | Append-only v14, record widened within the existing sector | `config.c` already documents widening the programmed region as costing "nothing but a slightly longer program" |
| Layout selection | Still derived, still not user-selectable | Which roles are filled is a fact, not a preference; letting a user assert `kbm` with no mouse would silently drop the right stick |

## 2. What exists today

Established by reading the implementation, not assumed:

- `ns2_kbm_mode_t` — `controller` / `keyboard` / `kbmouse` / `auto`. Persisted as
  an **override**; the live mode comes from `ns2_kbm_effective_mode(override,
  keyboard_present, mouse_present)`.
- `ns2_kbm_profile_t` — exactly `NS2_KBM_PROFILE_KEYBOARD` and
  `NS2_KBM_PROFILE_KEYBOARD_MOUSE`, obtained from `ns2_kbm_mode_profile(mode)`.
  **A pure function of the mode.** There is no selection anywhere.
- Canonical defaults are two immutable ROM tables (`KBM_DEFAULT_KEYBOARD`,
  `KBM_DEFAULT_KEYBOARD_MOUSE`). They are genuinely different, and the source
  says why: in `kbm` the mouse owns the right stick, so IJKL are unassigned and
  R3 moves to the middle mouse button.
- User configuration is `ns2_kbm_profile_overrides_t profiles[2]` — a count plus
  up to `NS2_KBM_MAX_OVERRIDES` (48) `{source, destination}` entries. 196 bytes
  each.
- Command family: `kbm status`, `kbm mode`, `kbm map <kb|kbm> [page]`,
  `kbm bind <kb|kbm> <src> <dst|none|default>`, `kbm reset <kb|kbm|all>`,
  `kbm mouse …`.

### Measured budget

| | |
|---|---|
| `config_record_t` today | **476 bytes** |
| `CONFIG_RECORD_BYTES` | 1024 (4 × 256-byte pages) |
| Hard ceiling | `FLASH_SECTOR_SIZE` = 4096 |
| One override table | 196 bytes |

`config.c` states the widening rule explicitly: *"The settings sector is 4 KiB
and is erased whole on every save, so widening the programmed region costs
nothing but a slightly longer program; keep it a whole number of pages."*

## 3. The problems this solves

1. **The active mapping is unselectable.** Hardware-proven silent failure.
2. **Two profiles are two identities, not two slots.** A user cannot keep a
   Splatoon layout and a Zelda layout.
3. **No starting points.** The only path from the canonical default to a
   different feel is rebinding ~28 keys by hand.
4. **Mode and mapping are welded together.** Choosing a mapping means choosing a
   mode, which also changes *admission policy* — an unrelated concern.

## 4. Model

```
effective_mode = ns2_kbm_effective_mode(override, keyboard_role, mouse_role)   [unchanged]
layout         = ns2_kbm_mode_layout(effective_mode)                           [renamed]
profile        = config.active_profile[layout]                                 [NEW]
binding(src)   = override(profile, src)  ??  canonical_default(layout, src)     [unchanged shape]
```

Only the third line is new. Resolution, admission, publication and the whole
runtime path below it are untouched.

### Invariants

- **P1.** A profile belongs to exactly one layout and may never be applied to
  another. Its source domain and its defaults are layout-specific.
- **P2.** Every layout always has exactly one active profile. There is no
  "nothing selected" state to represent or recover from.
- **P3.** Slot 0 of each layout is the built-in **Default** profile. It may be
  edited and reset, but never deleted or renamed, so P2 can always be satisfied
  by falling back to it.
- **P4.** Deleting the active profile activates slot 0 of that layout.
- **P5.** A template is never referenced after it is applied. Applying copies
  overrides; the profile does not remember where they came from. A profile that
  remembered its template would have to answer what happens when the user edits
  one binding, and there is no good answer.
- **P6.** The wire names `kb` and `kbm` continue to mean "the active profile of
  that layout" and remain valid everywhere they are accepted today.

## 5. Storage (schema v14)

```c
#define NS2_KBM_MAX_PROFILES      6u   // total, shared across layouts
#define NS2_KBM_PROFILE_NAME_MAX 16u   // bytes including NUL

typedef struct {
    uint8_t used;
    uint8_t layout;                        // ns2_kbm_layout_t
    char    name[NS2_KBM_PROFILE_NAME_MAX];
    uint8_t reserved[2];                   // keeps overrides 4-byte aligned
    ns2_kbm_profile_overrides_t overrides; // 196, unchanged
} ns2_kbm_profile_slot_t;                  // 216 bytes

typedef struct {
    uint8_t mode;
    uint8_t active[NS2_KBM_LAYOUT_COUNT];  // slot index per layout
    uint8_t reserved;
    ns2_kbm_profile_slot_t profiles[NS2_KBM_MAX_PROFILES];
    ns2_kbm_mouse_config_t mouse;
} ns2_kbm_config_t;
```

**Cost:** 6 × 216 = 1296 bytes of profiles. The record grows from 476 to
approximately 1390, so `CONFIG_RECORD_BYTES` must rise from 1024 to **2048** —
still a whole number of pages and still inside the 4 KiB sector that is already
erased whole on every save. This also leaves headroom for the appearance HLD's
nine appended triplets, so the widening happens once.

**Six profiles, not four:** four fits in the current 1024 but leaves nothing.
Six is two spare per layout beyond Default, which is what makes the feature
worth having, and the page cost is already sunk by the erase.

### Migration v13 → v14

Append-only, following the established protocol in `include/config_persist.h`:

- Freeze `config_record_v13_t`.
- `profiles[0]` ← v13's `profiles[NS2_KBM_PROFILE_KEYBOARD]`, layout Keyboard,
  name `"Default"`.
- `profiles[1]` ← v13's `profiles[NS2_KBM_PROFILE_KEYBOARD_MOUSE]`, layout
  Keyboard + Mouse, name `"Default"`.
- Remaining slots unused; `active[]` = {0, 1}.

**An upgraded adapter is byte-for-byte equivalent in behaviour.** Every existing
binding keeps working and lands in the profile that is already active. This is
the migration test's central assertion.

## 6. Templates

Immutable ROM data, in the same shape the canonical defaults already use:

```c
typedef struct {
    const char *name;
    ns2_kbm_layout_t layout;
    const kbm_default_binding_t *bindings;
    uint8_t count;
} ns2_kbm_template_t;
```

Applying template `T` to profile `P` computes the override set that turns `P`'s
layout defaults into `T`, and writes exactly that. So a template identical to
the defaults produces an **empty** override list, and "apply Default" and "reset"
converge on the same state by construction rather than by agreement between two
code paths.

### Template content is a separate product decision

This design ships the mechanism and exactly one template per layout — the
existing canonical defaults, named **Default** — because that content is already
hardware-validated and documented.

Inventing game-shaped templates ("FPS", "Platformer") without evidence about
what users actually want would be exactly the speculation
[`PLAN.md`](../../PLAN.md) warns against. Adding one later is a data-only change:
one table entry, no logic. Candidates worth *asking about* rather than assuming:

- a left-handed mirror (arrows drive movement, WASD the right stick) — the one
  with a genuine accessibility argument;
- an arrow-keys-movement variant for compact boards with an awkward WASD reach.

Neither is proposed for implementation here.

## 7. Management surface

New verbs, following the existing pagination and naming conventions:

| Command | Reply |
|---|---|
| `kbm profiles [page]` | `{"profiles":[{"id":0,"layout":"kb","name":"Default","active":true,"overrides":3}],"page":0,"more":false}` |
| `kbm profile use <layout> <id>` | `{"ok":true,"layout":"kb","id":2}` |
| `kbm profile new <layout> <name>` | `{"ok":true,"id":4}` — or `{"error":"profile storage full"}` |
| `kbm profile rename <id> <name>` | `{"ok":true}` |
| `kbm profile delete <id>` | `{"ok":true,"active":0}` — reports the resulting active id (P4) |
| `kbm templates` | `{"templates":[{"name":"Default","layout":"kb"}]}` |
| `kbm template apply <id> <name>` | `{"ok":true,"overrides":0}` |

Extended, compatibly:

- `kbm map <kb|kbm|id> [page]` — a bare layout name still means its active
  profile.
- `kbm bind <kb|kbm|id> …`, `kbm reset <kb|kbm|id|all>` — the same widening.
- `kbm status` gains `activeProfile` (id) and `activeProfileName`, so a client
  can show what is live without a second round trip.

Replies stay inside the ~512-byte management frame: six profiles at 16-character
names paginate comfortably, and `kbm profiles` is bounded by
`NS2_KBM_MAX_PROFILES` rather than by peer state, so it cannot grow unbounded
the way `peers list` did.

## 8. Client behaviour

**Windows.** The Keyboard & Mouse page gains a profile selector beside the
existing layout indicator. The selector changes *which profile is active on the
adapter* — not merely which one is being edited — so the editor's target and the
console's behaviour are the same thing by construction. The
`EditingInactiveProfile` warning added earlier stays as a safety net for the case
where the adapter's active profile changes underneath the page (another client,
a reconnect), but it should stop firing in normal use.

**Android.** Same surface, same wire. No new capability negotiation: absent
verbs report Unsupported and the page falls back to today's single-profile
behaviour, matching how `peerForget` and `remotePairing` already degrade.

**Both.** Layout stays presented as a *fact* — "Keyboard and mouse connected" —
never a control. The user picks a profile within it.

## 9. What this design deliberately does not do

- **No per-game or per-title switching.** The adapter cannot know what is
  running on the console; a title-aware profile would be a guess wearing a
  feature's clothes.
- **No profile switching from a key combination.** Tempting and cheap, but every
  chord is a chord some game wants. Needs its own product decision.
- **No user-selectable layout.** Asserting `kbm` with no mouse silently loses the
  right stick.
- **No profile import/export.** Useful, but it is a management-transport and file
  question, not a mapping-model one.
- **No generic mapping framework.** `PLAN.md` is explicit, and this design stays
  within the KB/M feature it belongs to.

## 10. Risks

| Risk | Mitigation |
|---|---|
| Schema migration reinterprets stored bytes | Append-only, frozen v13, field-by-field migration, and the existing `CONFIG_PERSIST_VERSION` tripwire test which forces whoever bumps the version to confirm every older layout still migrates |
| Record widening breaks the save path | `CONFIG_RECORD_BYTES` stays a whole number of pages inside one erase sector; the existing static asserts already enforce both |
| Flash wear from profile switching | `use` writes one byte of `active[]`; it is still a full sector erase, so the UI must not switch profiles speculatively — only on explicit user action |
| Wire regressions for old clients | `kb`/`kbm` keep their meaning (P6); the shared protocol fixture gains vectors for the new verbs and the old ones are asserted unchanged |
| Two clients disagreeing about the active profile | `kbm status` carries `activeProfile`; clients re-read after every mutation, as they already do for peers |

## 11. Test matrix

Host, firmware:

1. v13 → v14 migration preserves both existing override sets, names them
   Default, and activates them.
2. A migrated adapter resolves byte-identical bindings to before the upgrade.
3. Applying the Default template yields an **empty** override set.
4. Applying a template then resetting the profile converge on the same state.
5. A profile cannot be applied to the wrong layout.
6. Deleting the active profile activates slot 0 (P4).
7. Slot 0 cannot be deleted or renamed (P3).
8. `NS2_KBM_MAX_PROFILES` exhaustion reports `profile storage full`, never
   silently overwrites.
9. Override exhaustion inside one profile still reports `mapping storage full`.
10. `kbm map kb` and `kbm map <active id>` return identical pages (P6).
11. Name sanitisation: length, NUL termination, non-printable rejection.
12. `kbm profiles` pagination terminates for every reachable slot count.

Client:

13. Selecting a profile issues `use` and re-reads status.
14. An adapter without the new verbs degrades to single-profile behaviour.
15. The inactive-profile warning does not fire in ordinary use.

## 12. Implementation order

Each step is independently shippable and testable:

1. **Vocabulary only.** Rename the derived axis to *layout* in firmware, keeping
   `kb`/`kbm` on the wire. No behaviour change, no schema change.
2. **Schema v14 + migration**, with the profile table present but exactly two
   slots used. Still no new commands. Proves the migration in isolation.
3. **Selection**: `active[]`, `kbm profile use`, `activeProfile` in status.
4. **Lifecycle**: `new` / `rename` / `delete`.
5. **Templates**: table, `kbm templates`, `kbm template apply`.
6. **Windows** profile selector.
7. **Android** parity.

Steps 1 and 2 carry the schema risk and no user-visible change, which is
deliberate: if either is wrong, it is wrong on its own commit.

## 13. Open product decisions

These need the maintainer, not more code:

1. **Six profiles, or four?** Six requires widening the record to 2048 bytes.
   Four fits today with no headroom. This design recommends six.
2. **Which templates ship?** The mechanism is content-free by design; see §6.
3. **Is a profile-switch chord wanted?** Explicitly out of scope here.
4. **Should profiles be per-peer?** A user with two keyboards may want different
   mappings per device. This design says no — the mapping belongs to the layout,
   not the hardware — but that is a judgement, not a fact.
