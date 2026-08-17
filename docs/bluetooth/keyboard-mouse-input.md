# Bluetooth Keyboard / Keyboard + Mouse input

Authoritative reference for PicoSwitch2's direct Bluetooth HID keyboard and keyboard + mouse
input source: its logical-source model, HID classification, mapping model, defaults, persistence,
management API, diagnostics, and known limitations.

- **Status:** **Complete — hardware validated 2026-08-16** (ASUS ROG FALCHION RX keyboard + ROG
  KERIS II ACE mouse, Pico W). See [Hardware validation](#hardware-validation).
- **Scope:** the input source and its remapping-capable configuration foundation. The graphical
  remapping editor is UX_PASS work and builds on the management surface documented here.

---

## Input modes

The adapter works out what to be from what is actually admitted. Pairing an ordinary Bluetooth HID
device does **not** require selecting a mode first.

The persisted setting is an **override**, defaulting to `auto`:

| Override | Meaning |
|---|---|
| `auto` (default) | infer the mode from the admitted role composition |
| `controller` | KB/M off entirely; neither role is ever taken |
| `keyboard` | keyboard role only; a mouse never joins |
| `kbmouse` | both roles; keeps the KB/M profile even while the mouse is absent |

The **effective** mode is derived (`ns2_kbm_effective_mode()`):

| Roles held | Effective mode |
|---|---|
| none | Controller |
| keyboard only | Keyboard (Keyboard + Mouse if the override pinned it) |
| mouse only | Keyboard + Mouse, partial source |
| keyboard + mouse | Keyboard + Mouse |

A gamepad is never a KB/M role, so a recognized controller always yields Controller behavior. The
same two physical devices reach the same semantics regardless of which connected first: inference
reads the composition, never the arrival order.

### Native Joy-Con mouse exception

Under `auto`, a mouse **alone** does not bootstrap a KB/M source while the output personality is
Joy-Con 2 Left or Right — the role policy simply does not offer the mouse role, so the established
native pointer path keeps it. Once a keyboard joins, the composite forms and the mouse joins it, and
it *still* reaches the native pointer because `ns2_kbm_resolve()` passes relative deltas straight
through on a personality that has one. Nothing about that path is regressed either way.

### Precedence

```
recognized controller (quirk table)   -> Controller behavior, KB/M never touches it
unresolved peer classified keyboard   -> keyboard role
unresolved peer classified mouse      -> mouse role (subject to the Joy-Con exception)
explicit override                     -> narrows the above; never widens it
```

Classification runs first and is unchanged; inference operates on its result.

---

## Capability is not role ownership

Two separate questions, and conflating them is a hardware-proven failure:

| Question | Answered by |
|---|---|
| What reports can this connection emit? | **capabilities** (`kbcap` / `mousecap`) |
| Which logical role does this device stand for? | **primary** (`ns2_kbm_primary_t`) |

**Only primary decides role ownership.**

An ASUS ROG KERIS II gaming mouse reports `kbcap:true` *and* `mousecap:true` — its macro
buttons put a keyboard collection in its descriptor, and bthid consequently binds it to the
`Generic BT Keyboard` driver. It is still a mouse. If "has both capabilities" meant "is a combo
device", that one peer would occupy both roles, the composite would look complete, and the user's
actual keyboard could never join.

**Driver ownership is not the logical role either.** The keyboard driver is bound to anything whose
descriptor carries keyboard usages; that is a parsing decision, not a classification.

### Classification precedence for an unresolved peer

```
Class-of-Device "combo keyboard/pointing" (0x03)  -> COMBO      (positive declaration only)
else pointer capability present                   -> MOUSE
else keyboard capability present                  -> KEYBOARD
else                                              -> not a KB/M peer
```

Pointer wins deliberately. Gaming mice commonly carry auxiliary keyboard usages; keyboards carrying
a genuine relative pointer are rarer, and a keyboard misclassified as a mouse still leaves the
keyboard role free for the next peer, whereas the reverse consumes the role the user needs.

**COMBO is never inferred from capabilities.** It requires the device to declare itself one, which
only Class of Device can do — so a BLE peer is never classified combo.

Primary is recorded once per connection generation and does not accumulate: a later capability
discovery, or a macro keystroke arriving as a keyboard report, cannot promote a mouse into
something that claims the keyboard role.

### Role assignment

| Primary | Both free | Keyboard taken | Mouse taken |
|---|---|---|---|
| MOUSE | mouse only | mouse | duplicate — never becomes a keyboard |
| KEYBOARD | keyboard only | duplicate — never becomes a mouse | keyboard |
| COMBO | both | mouse | keyboard |

The COMBO row is what fixes the original bug (a genuine combo with the keyboard role taken is still
offered the free mouse role); the first two rows are what stop capability inflation from becoming
role inflation.

---

## The five-button mouse contract

The mouse role carries the standard five-button mouse, and all five are ordinary mouse inputs. None
of them requires the keyboard role, and `kbcap:true` on a gaming mouse is auxiliary macro capability
that is **not** needed for any of them.

| Button | Parser | Driver → JP | Controller / native Joy-Con | KB/M default |
|---|---|---|---|---|
| 1 Left | ✓ | `JP_BUTTON_L2` | ZL | ZR |
| 2 Right | ✓ | `JP_BUTTON_R2` | ZR | ZL |
| 3 Middle | ✓ | `JP_BUTTON_A1` | HOME | R3 |
| 4 Back | ✓ | `JP_BUTTON_B3` | Y | Y |
| 5 Forward | ✓ | `JP_BUTTON_B1` | B | B |

The existing chain already preserved all five and was not redesigned: the descriptor parser handles
16 buttons, the generic mouse driver normalizes 1–8, and `input_event_t::hid_buttons` carries the
raw HID Usage Page 0x09 mask so the KB/M model can bind by button number. Back and Forward keep the
destinations the Controller-mode base map already gives them, so the same physical button does the
same thing in both modes. Buttons 1/2 are swapped relative to Controller mode on purpose — left
click is the fire trigger (ZR) in a keyboard+mouse context.

Anything beyond button 5 remains vendor/macro functionality and is not addressable.

## Logical-source model

The project rule is unchanged: **one logical input source owns the console stream at a time.** A
Keyboard + Mouse source is one logical source that happens to be backed by two Bluetooth peers.

```
Active input source
├── Controller
│     └── one controller peer
├── Keyboard
│     └── one keyboard peer
└── Keyboard + Mouse
      ├── keyboard role   ─┐
      └── mouse role      ─┴─ one composite source (may be one combo peer)
```

Two layers implement this, and neither loosens the connection limit:

1. **`ns2_kbm_roles_t`** (`src/ns2_kbm.c`) binds peers to roles. It is the only component that
   knows two peers are halves of one controller. A second keyboard or a second mouse is rejected
   here and counted, before it can reach the source registry.
2. **The source arbiter** (`src/bt_hid/ns2_input_arbiter.c`) gained a `group_id` on each source.
   Sources sharing a nonzero group are one console owner: any member may publish while the group
   owns the console, and losing one member hands the owning token to a surviving member instead of
   surrendering the console. A source with `group_id == 0` still owns the console alone, which is
   every pre-existing source unchanged.

The composite handle is opaque and never reused. It is allocated when the first role binds to an
empty composite and retired only when the last role is released, so a peer that reconnects rejoins
the same logical source rather than appearing as an unrelated new one.

Stale-generation protection is preserved end to end. Role identity is
`stable Bluetooth address + connection generation`, so a disconnect callback carrying an older
generation matches no role and cannot release a replacement peer that reused the transport index.

### Interaction with the source arbiter's existing policy

Automatic-versus-explicit ownership is unchanged. Additions:

- A newly arrived peer of the composite that **already** owns the console does not trigger a
  handover; without this the second half of a KB/M source would move the owning token between two
  peers of one owner and emit a neutral boundary in the middle of live input.
- Losing a composite member is not whole-source loss while another member survives, so the caller
  is not told to neutralize the slot. It clears only the departed role's own state.
- `ns2_input_arbiter_disconnect()` now treats a pending selection as in-flight only while the
  request is genuinely outstanding (`pending_request != applied_request`). Previously a
  long-settled explicit choice still matched `pending_id`. For a standalone source the outcome was
  identical either way; for a composite it would have suppressed the handover above.

---

## HID classification

Classification is structural. Device names are never used.

| Transport | Keyboard | Mouse |
|---|---|---|
| Classic | Class of Device peripheral type `01` (keyboard) or `11` (combo) | peripheral type `10` (pointing) |
| BLE | report descriptor declares Usage Page 0x07 input fields | report descriptor declares relative Generic Desktop X **and** Y |

BLE has no Class of Device, so `bthid_set_hid_descriptor()` reclassifies from the descriptor once
it arrives. **Keyboard is tested before mouse**, because a combo peer declares relative X/Y too and
only the keyboard driver can serve both roles from one connection; testing mouse first would strand
the keyboard half. The Classic driver registry is ordered the same way for the same reason.

A gamepad is never claimed as a keyboard or a mouse: the keyboard driver's Classic match is
Class-of-Device only, its BLE match is `false` (descriptor reclassification decides), and the
descriptor test requires real keyboard input fields rather than "contains unusual usages".

### Classification applies to unresolved peers only — do not remove

**Keyboard/mouse descriptor classification is for UNRESOLVED generic HID peers. A peer the quirk
table has already recognized as a known controller family is never a candidate, whatever its
descriptor also contains.**

This is the layering, and KB/M integrates with it rather than competing:

```
supported/known gamepad
    -> generic gamepad driver (the BLE catch-all)
    -> gamepad_quirks_identify()   name / VID / PID, most-specific-first
    -> QUIRK_XBOX / QUIRK_XBOX_ELITE2 / QUIRK_BITDO_* / ...
    -> quirk->extract_extra() at report time

unresolved generic HID peer  (quirk == generic)
    -> keyboard descriptor?  -> keyboard driver
    -> mouse descriptor?     -> mouse driver
```

`gamepad_quirks_identify()` is the project's established answer to "is this a supported
controller", and it stays that way. It is deliberately **name-driven** as well as VID/PID-driven
because the BLE PnP query often fails to resolve VID/PID — an Xbox pad with `vid=0 pid=0` still
resolves to `QUIRK_XBOX` by name at `gamepad_init()`, before any descriptor arrives, and that is
what makes the Elite's "Xbox + 20-byte report" paddle fallback reachable in `xbox_extract_extra()`.

`bthid_gamepad_identity_unresolved()` exposes that state to `bthid.c`, which gates both the
keyboard and mouse reclassification branches on it.

**The regression this prevents.** The first version of this pass claimed any generic-driver peer
whose descriptor contained keyboard usages. Xbox-class controllers declare a keyboard collection
alongside their Game Pad collection for the share/profile button, so an Xbox Elite was moved off
the generic driver before `bthid_gamepad_set_descriptor()` ever ran — its report map was never
built and its quirk was never consulted. The pad connected, registered as an active input source,
and published nothing. It also poisoned the rest of that peer's driver lifecycle, because every
later re-evaluation re-fed the cached descriptor and landed back on the keyboard driver.

`VID/PID 0/0` was a symptom, not the cause: registering a source before DIS resolves is normal and
pre-existing, and the quirk table is built to work without it.

**Secondary guard.** For a peer the quirk table genuinely cannot help with — an unknown, no-name
controller — a descriptor declaring a Generic Desktop Joystick (`0x04`) or Game Pad (`0x05`)
application collection also blocks keyboard classification.
`bthid_keyboard_parse_descriptor()` reports it as `has_gamepad_collection` and returns false, so a
caller can distinguish "not a keyboard" from "a controller that also has keys". This is a hazard
guard for unresolved peers, **not** an identification mechanism — nothing supported is identified
by it.

`bthid.c` additionally demotes a peer already bound to the keyboard driver back to the generic
driver if its descriptor turns out not to be a keyboard's, which is reachable when a Classic Class
of Device claims "combo keyboard/pointing" for something that is really a pad.

The mouse test was never vulnerable to the descriptor trap: it requires *relative* Generic Desktop
X and Y in one report, which a gamepad's absolute sticks cannot satisfy. It is gated on the quirk
state anyway, for one rule rather than two.

Known limitation: the quirk table matches any Microsoft VID (`0x045E`) to `QUIRK_XBOX`, so a
Microsoft-branded Bluetooth **keyboard** would be treated as a recognized controller and refused
keyboard classification. That returns it to its pre-KB/M behavior (unsupported), and narrowing the
quirk table is a quirk-architecture decision, not a KB/M one.

Regression coverage:

| Test | Proves |
|---|---|
| `test_xbox_elite_quirk_pipeline` | the real quirk chain: name-only identity with `vid=pid=0` still resolves to `QUIRK_XBOX`; the 20-byte report extracts all four Elite paddles through `quirk->extract_extra`; a 16-byte report keeps normal Xbox Share/Back behavior and never synthesizes paddles; exact Elite PIDs select `QUIRK_XBOX_ELITE2`; ordinary keyboards/mice stay generic |
| `test_ns2_active_input_lifecycle` | the bthid boundary: a quirk-recognized controller is refused reclassification (keyboard *and* mouse), keeps its source identity and connection generation, and keeps delivering reports — while unresolved peers still classify normally |
| `test_bthid_keyboard_report` | the structural discriminator against `tools/fixtures/composite_gamepad_keyboard_hid.h` |

`test_bthid_gamepad_quirks` (the pre-existing quirk contract suite, including Elite paddle
extraction) previously could not be linked at all — it needs
`devices/vendors/microsoft/xbox_rumble.c` for `xbox_rumble_build_payload()`, and the linker
reported nothing beyond `ld returned 1`. It is now wired into `tools/run_mgmt_tests.ps1` and runs.

### Why the keyboard descriptor has its own parser

`src/bt_hid/bt/bthid/devices/generic/bthid_keyboard_report.c` walks HID report-descriptor items
directly instead of using the shared LUFA-derived parser. Two concrete reasons, both found while
implementing this:

1. That parser's report-item filter (`CALLBACK_HIDParser_FilterHIDReportItem`) rejects Usage Page
   0x07 outright, so a keyboard descriptor produces zero items and parses as a failure.
2. It expands every element of a Report Count into its own pooled item, and the pool holds 50. A
   full NKRO keyboard declares one bit per usage — 232 items — which would exhaust the pool and
   trip its `assert()`.

Widening a parser every other device depends on would be a much larger blast radius than a ~200
line scanner for a descriptor shape this simple.

Supported report shapes:

- **Boot layout** — 8 modifier bits, a constant byte, and a 6-slot key array. Also available as a
  fallback for a Classic keyboard whose descriptor never arrived.
- **NKRO bitmap** — one bit per usage over a declared usage range.
- Both, in one descriptor, with or without report IDs.

A report whose key array contains `ErrorRollOver` (usage 0x01) is reported as
`BTHID_KEYBOARD_DECODE_ROLLOVER` and **retains the previous held set**: the keyboard is saying it
cannot tell us which keys are down, and replacing the set with an empty one would release every
control mid-input. Rollover reports are counted in diagnostics.

---

## Keyboard state representation

The Bluetooth layer decides only *which keys are held*, as a 32-byte bitmap of HID Usage Page 0x07
usage ids covering `0x00..0xFF`. Modifier keys are ordinary usages (`0xE0..0xE7`) in that same
space, so there is exactly one keyboard identity domain and no separate modifier concept.

Usages `0x00..0x03` (no-event and the three error codes) are never physical keys and are never
settable or bindable.

The mapping layer decides what those held usages mean. No controller mapping exists anywhere in
the Bluetooth report parsers.

---

## Mouse state representation

Bluetooth mouse input reuses the existing mouse implementation rather than introducing a second
model:

- The existing generic mouse driver and its descriptor parser are unchanged.
- The existing relative-mouse fields in `switch_pro_input_t` and the existing accumulate/consume
  seam in `report.c` are what carry movement to the console, exactly as they do for Joy-Con 2 mouse
  mode today.
- The only addition is `input_event_t::hid_buttons`, the raw HID Usage Page 0x09 button bitmap.
  The pre-existing `buttons` field is a lossy gamepad-shaped projection whose inverse is not a
  stable identity; the KB/M model binds mouse controls by HID button number, so it needs the
  original. Buttons 1..5 are supported.

Mouse-owned state (buttons, pending relative deltas, translated stick deflection) is stored
separately from keyboard-owned state and merged only at resolve time.

---

## Mapping model

```
Bluetooth HID report -> bthid_keyboard / bthid_mouse   (what is held)
                     -> ns2_kbm_state_t                (source-owned state)
                     -> ns2_kbm_config_t               (what it means)
                     -> ns2_kbm_output_t               (normalized controller)
```

`src/ns2_kbm.c` is free of Pico SDK, BTstack, bthid, and `report.c` dependencies, so the whole
contract is host testable. `src/bt_hid/ns2_kbm_runtime.c` is the firmware adapter.

**The output is recomputed from the complete held-source set on every publish.** Nothing is
incrementally latched. That single decision is what makes duplicate bindings safe, makes report
order irrelevant, and makes it impossible for a mapping change to leave a destination stuck.

### Stable source-input identifiers

| Domain | Identity | Wire form |
|---|---|---|
| Keyboard | HID Usage Page 0x07 usage id, `0x04..0xE7` | `key:1A` (hex) |
| Mouse | HID Usage Page 0x09 button number, `1..5` | `mouse:1` |

Usage ids are stable across layouts, modifier state, report formats, persistence, management
readback, tests, and the later UX editor. Translated characters and report byte offsets are not,
and are deliberately not exposed.

### Controller destination identifiers

Destinations are the project's existing normalized vocabulary (`NS2_DST_*` in
`include/ns2_remap.h`), never Switch report bit positions:

`a b x y l r zl zr l3 r3 minus plus home capture dup ddown dleft dright gl gr c`

plus eight digital stick directions added by this pass:

`lstick_up lstick_down lstick_left lstick_right rstick_up rstick_down rstick_left rstick_right`

A physical controller never needs the stick directions (its sticks are already analog), so the
locked physical base map never uses them and `ns2_seam.c` ignores them. They exist so a digital
source can express stick deflection in the same vocabulary as every other control.

`none` is a valid destination meaning "explicitly unassigned".

The destination → Pro Controller wire-bit table lives in **one** place,
`ns2_kbm_apply_destination()`. The locked physical-controller map in `ns2_seam.c` calls it too, so
the two mapping systems cannot drift apart about what "ZR" is.

### One source, one destination

One source input maps to at most one destination. There is no macro engine and the data model
cannot express one.

### Duplicate destinations

Several source inputs *may* map to the same destination, and the canonical Keyboard + Mouse profile
uses this (key `3` and mouse button 1 both reach ZR). Because the output is recomputed from the
held set as a **set of destinations**, releasing one contributing input while another is still held
cannot release the destination. There is no reference count to get wrong.

### Opposing directions

Opposing digital directions **neutralize**. If both `lstick_left` and `lstick_right` are held the
axis reads centre; the same rule applies to the digital right stick and to opposing D-pad edges.

This is the simplest rule that is independent of report order, cannot latch, and produces a state
a physical controller could actually be in. It applies identically to canonical defaults and to
user-remapped bindings. Digital directions never accumulate: the axis is derived from the currently
held set every publish, so releasing one side restores the other immediately.

### Mapping changes while an input is held

Changing a binding runs through an explicit neutralization boundary. Every configuration mutation
bumps a generation counter; when the input core observes a new generation it snapshots the
configuration, **drops the held keyboard and mouse state**, and publishes a neutral slot before
adopting the new mapping.

Recomputation alone would already release the old destination on the next report — but a held,
quiet source sends no next report. The unconditional boundary is what lets a future remapping UI
change a binding without implementing its own input cleanup. Held state is deliberately not
migrated to the new mapping.

---

## Default Keyboard profile

The keyboard is the entire controller, so it carries both sticks.

| Key | HID usage | Destination |
|---|---|---|
| W / S / A / D | 1A / 16 / 04 / 07 | Left stick up / down / left / right |
| I / K / J / L | 0C / 0E / 0D / 0F | Right stick up / down / left / right |
| ↑ / ↓ / ← / → | 52 / 51 / 50 / 4F | D-pad up / down / left / right |
| Space | 2C | B |
| F | 09 | A |
| E | 08 | X |
| Left Shift | E1 | Y |
| Q | 14 | L |
| R | 15 | R |
| 1 | 1E | ZL |
| 3 | 20 | ZR |
| Left Ctrl | E0 | L3 |
| Left Alt | E2 | R3 |
| Enter | 28 | + |
| Backspace | 2A | − |
| Escape | 29 | HOME |
| F12 | 45 | Capture |

`C`/GameChat, GL, and GR are addressable destinations but unassigned by default.

## Default Keyboard + Mouse profile

Deliberately **not** the Keyboard profile plus mouse buttons: the mouse owns the right stick, so
IJKL are unassigned here and R3 moves to the middle mouse button. This is what makes the two
profiles genuinely independent, and it is why a disconnected mouse must not silently fall back to
the Keyboard profile.

| Input | Destination |
|---|---|
| W / S / A / D | Left stick up / down / left / right |
| ↑ / ↓ / ← / → | D-pad up / down / left / right |
| Space / F / E / Left Shift | B / A / X / Y |
| Q / R / 1 / 3 | L / R / ZL / ZR |
| Left Ctrl | L3 |
| C (usage 06) | C / GameChat |
| Enter / Backspace / Escape / F12 | + / − / HOME / Capture |
| Mouse button 1 (primary) | ZR |
| Mouse button 2 (secondary) | ZL |
| Mouse button 3 (middle) | R3 |
| Mouse buttons 4, 5 | unassigned |
| Mouse movement | see below |

Buttons 4 and 5 are addressable identities but intentionally unbound: there is no evidence-backed
convention for them and inventing one would be a guess a user cannot see.

---

## Mouse movement

Mouse movement is not a button binding and is not modelled as one. Its behavior depends on the
selected Switch-facing output personality.

### Native mouse output (Joy-Con 2 personalities)

When the active personality is Joy-Con 2 Left or Right, relative deltas are handed to the existing,
hardware-validated Joy-Con 2 mouse path unchanged and the right stick is left to whatever digital
bindings are held. Nothing is converted to a stick first, and none of that wire behavior is made
configurable — the native path is proven and is deliberately not parameterized for UI symmetry.

Publishing goes through the accumulating seam (`accumulate_global_mouse_input`) even for a
keyboard-triggered republish, so relative movement cannot be lost between USB frames.

### Mouse-to-stick translation (every other personality)

Movement is translated to the **right stick**:

- Each report adds `delta × sensitivity` (Q8.8) to a per-axis deflection, clamped to full scale.
- The deflection recenters at a **constant rate**: `recenter_ms` is the time a full deflection takes
  to return to neutral, so a smaller deflection returns proportionally sooner. Decaying by a
  fraction of the current magnitude would be an exponential that never actually reaches zero, which
  is the wrong shape for a setting whose purpose is guaranteeing the stick comes back to centre.
- The recenter is driven by a 3 ms core-1 tick, not only by mouse reports, because a mouse that
  stops moving stops reporting. This is what makes "movement never leaves the stick stuck
  off-centre" true rather than merely likely.
- Deltas are never permanently accumulated: the deflection is clamped, and a gap longer than
  `recenter_ms` snaps to exact neutral.
- A digital right-stick binding that is actually held wins over the translation, so the two never
  fight over the same axis.

No smoothing and no response-curve selection exist, because the production translator does not use
them. Only parameters with a technical reason are exposed.

### Configurable mouse settings

| Setting | Range | Default | Meaning |
|---|---|---|---|
| `sensitivity` | 16..8192 | 512 | Q8.8 stick units per mouse count, both axes |
| `sensitivityx` / `sensitivityy` | 16..8192 | 512 | per-axis override |
| `recenter` | 10..2000 ms | 120 | full-deflection recenter time |
| `invertx` / `inverty` | 0/1 | 0 | axis inversion |

Every value is validated on entry and again on load. An out-of-range management value is **rejected**
rather than silently clamped: a client that sent a bad value must be told, not quietly given a
different one. Out-of-range *persisted* values fall back to their canonical defaults.

These apply only to the mouse-to-stick translator.

---

## State ownership and disconnect behavior

Keyboard-owned and mouse-owned state are stored separately and merged only at resolve time, so
neither role's report can erase the other's contribution and either role can be cleared
independently. There is no last-report-wins path.

| Event | Result |
|---|---|
| Keyboard disconnects | keyboard-derived buttons released, keyboard-derived axes neutral, held-key set cleared; mouse-owned state preserved and still publishing |
| Mouse disconnects | mouse buttons released, pending deltas dropped, translated stick centred; keyboard-owned state preserved and still publishing |
| Last role disconnects | the arbiter reports whole-source loss and the existing authoritative release path neutralizes slot 0 and clears retained native motion |
| Stale disconnect (older generation) | matches no role; the replacement peer is untouched |
| Reconnect | starts from a fully cleared role state; no button, axis, or delta from the previous generation can survive |
| Mode change | all roles released, all state cleared, registry rebuilt, slot neutralized |

Partial connection is safe and does **not** change the selected mode or profile. With Keyboard +
Mouse selected and no mouse connected, the keyboard keeps working on the **KB/M** profile — IJKL
stay unassigned, because that profile is what the user selected. Mouse-only operation is likewise
safe: it never synthesizes keyboard state.

---

## Pairing and reconnection

The existing bonding and reconnection system is extended, not duplicated. There is no second bond
database and no separate KB/M pairing framework, and pairing admission/security is unchanged.

- **Discovery completion is per logical source, not per peer.** A KB/M composite counts as complete
  only when BOTH roles are held, so pairing a keyboard leaves the window open for the mouse. With no
  KB/M role held this is exactly the pre-existing "a controller is HID-ready" test.
- **Opening a pairing window never disconnects a KB/M role.** Historical replacement semantics
  ("pairing with a controller connected means replace it") apply to a standalone controller source
  only. A composite is assembled one role at a time, so opening pairing while one role is connected
  means "add the other" — disconnecting what is already there is exactly wrong, and was what made
  keyboard + mouse impossible to establish on hardware.

  **Deliberate limitation:** because the BOOTSEL gesture cannot say *which* role the user means, a
  KB/M role is replaced by powering its device off (which frees the role) or by the pairing wipe
  gesture — not by opening a pairing window.
- **Classic discovery** auto-connects gamepad-class peripherals only. Keyboard and pointing
  Class-of-Device values are admitted **only while the selected mode is still looking for that
  role**, and admission stops the moment the role is filled. Controller mode's admission policy is
  byte-for-byte what it was.
- **BLE discovery** already admits any peer advertising the HID service UUID, which covers BLE
  keyboards and mice; no change was needed.
- **Roles on reconnect** are recovered from capability, not connection order: a reconnecting
  keyboard is classified as a keyboard and takes the keyboard role regardless of which peer
  reconnects first.
- **Reconnect targeting is per bonded identity, not a single slot.** The host used to reconnect only
  `hid_state.last_connected_*`, which names the peer that connected *most recently* — with a
  composite source, whichever role connected second. When the other role powered off, the host chased
  the surviving peer, and since every connect attempt stops the scan, the retry cascade also erased
  the windows in which the absent peer's advertisements would have appeared. Neither role could
  auto-reconnect. Selection now runs over BTstack's LE device DB (`ns2_ble_reconnect_select()`) and
  never returns an identity that already has a live link. A peer whose metadata is stored is
  direct-connected; any other absent peer is reached through discovery, which supplies its name and
  profile. No KB/M-specific addresses exist in this path — it is a Bluetooth-layer policy that the
  composite source simply benefits from. See
  [`../experiments/multi-peer-bonded-reconnect-2026-08-16.md`](../experiments/multi-peer-bonded-reconnect-2026-08-16.md).
- **Replacing a role** is done by disconnecting the peer holding it and pairing another; the role
  frees on disconnect. A second peer offering an already-held role is rejected and counted.
- **Switching back to Controller mode** releases both roles. Bonds are not touched, so the
  keyboard and mouse remain bonded and reconnect when a KB/M mode is selected again.
- **Clear/reset pairing** (the triple-tap wipe) affects KB/M bonds exactly as it affects controller
  bonds — it is the same bond database.

---

## Persistence

KB/M configuration lives in the existing settings record and is written by the existing deferred
core-1 flash path. Persisted state: selected mode, both profiles' override tables, and the mouse
translation settings.

### Schema

Config schema **10 → 11**. `src/config_persist.c` owns the layout, the factory defaults, and the
migration, so all three are host testable — the failure this guards against is a schema change
silently reinterpreting an existing adapter's bytes as different settings, which a firmware build
cannot catch because both layouts compile fine. `_Static_assert`s pin every v10 field offset and
require new fields to start past the last v10 field.

Migration from v10 preserves body colour, both Joy-Con 2 accents, and the learned wake identity;
only the new block takes canonical defaults. An unknown schema (older or newer) falls back to
defaults rather than reinterpreting its bytes. The upgraded record is written back once.

The stored record no longer fits one 256-byte flash page, so the settings sector — which was
already erased whole on every save — now programs 1 KiB. The install-reset marker and the five-sector
persistence region are untouched, and a newly flashed UF2 still performs the intended first-install
reset.

### Sparse overrides

Canonical defaults are immutable data. User configuration is a bounded sparse list of overrides on
top of them (48 per profile), so restoring defaults is "drop the overrides" rather than a
procedural rebuild, and unmodified bindings cost no storage.

- An override with destination `none` is an **explicit unassign** and persists as one.
- Removing an override restores that source's canonical default.
- Setting a binding to exactly its canonical default stores no override, so reverting by hand
  reaches the same state as a reset.
- A full override table refuses new entries rather than dropping an existing one.

### Reset scope

| Command | Affects |
|---|---|
| `kbm reset kb` | Keyboard profile overrides only |
| `kbm reset kbm` | Keyboard + Mouse profile overrides only |
| `kbm reset all` | both profiles and the mouse translation settings |

`kbm reset all` does **not** change the selected input mode and does not touch any unrelated
adapter setting. Only the repository's normal global reset/install behavior does that.

### Validation and fail-closed behavior

Persisted and management-supplied mapping data is validated before it can reach the input path:

- an impossible override count discards that table rather than reinterpreting surviving bytes;
- an entry with an unknown source or destination is **dropped**, never remapped to an arbitrary
  destination, so an unknown future identifier can never become a current one;
- duplicate sources collapse to the first;
- out-of-range mouse values fall back to canonical defaults;
- the selected mode falls back to Controller.

Arbitrary bytes always produce a usable configuration. The repair is reported so the loader can
persist the cleaned record and log it.

---

## Management and configuration API

One command family on the existing management parser and transports (USB CDC and bonded/encrypted
BLE GATT). No second protocol. `kbm` is on the wireless allowlist including its mutating forms;
wireless RX has already passed `mgmt_allow_write()` (enabled, bonded, encrypted).

Responses stay inside the 512-byte wireless slot; the profile listing is paged and reports its own
bounds.

| Command | Result |
|---|---|
| `kbm` / `kbm status` | mode, profile, role connection state, native-mouse output, composite handle, source id, report counters, rejection counters, rollover count, mapping generation, neutralization count |
| `kbm mode` | current mode and the available modes |
| `kbm mode <controller\|keyboard\|kbmouse>` | select the input mode |
| `kbm map <kb\|kbm> [page]` | one page of the **effective** profile: `{src, dst, custom}` per binding, plus `total` and `more` |
| `kbm bind <kb\|kbm> <key:NN\|mouse:N> <dest>` | set a binding |
| `kbm bind <kb\|kbm> <src> none` | explicitly unassign |
| `kbm bind <kb\|kbm> <src> default` | drop the override, restoring the canonical default |
| `kbm reset <kb\|kbm\|all>` | restore defaults |
| `kbm mouse` | mouse translation settings **and their valid ranges** |
| `kbm mouse <field> <value>` | update one setting |

Mapping changes are RAM-immediate and take effect at the next neutralization boundary; `save`
persists them, like every other setting.

A future editor therefore never needs to edit firmware constants, understand a Bluetooth HID
parser, reconstruct defaults from source, manipulate configuration memory, infer valid destination
identifiers, or restart the adapter to discover current mappings. `kbm map` returns the effective
profile including bindings that exist only as user overrides, and `kbm mouse` returns each
setting's bounds alongside its value.

### UART

`kbm status` and `kbm mode <mode>` are also available on the UART diagnostic channel.

---

## Diagnostics

`kbm status` (UART or management) is one bounded snapshot answering the cross-layer questions for a
KB/M session:

| Field | Answers |
|---|---|
| `mode`, `profile` | which mode is selected and which profile is live |
| `keyboard`, `mouse`, `keyboard_conn`, `mouse_conn` | which roles are filled, by which connection |
| `native_mouse` | whether mouse movement is going to the Joy-Con 2 pointer or the right stick |
| `group`, `source` | the composite handle and the arbiter source id |
| `kb_reports`, `mouse_reports` | whether reports are arriving at all |
| `rejected_mode` | a peer of a kind this mode does not admit |
| `rejected_duplicate` | a second keyboard or second mouse |
| `rejected_not_owner` | admitted to a role but the composite does not own the console |
| `rollover` | keyboard reported ErrorRollOver; previous state retained |
| `role_losses` | role releases, including stale-generation rejections implicitly (they do not count) |
| `map_generation`, `neutralizations` | configuration changes and the boundaries they produced |
| `publishes`, `recenters` | output activity and mouse-to-stick recenter ticks |

These are bounded counters and a state snapshot; there is no per-report logging.

`btdev` (UART) answers the other half — which driver is actually bound to each live peer, which is
where every identity and classification question ultimately resolves:

```
{"btdev":[{"conn":4,"gen":1,"ble":true,"name":"...","vid":"0x0000","pid":"0x0000",
           "driver":"Generic BT Gamepad","type":3,"desc_len":123,"desc_mine":true,
           "kbcap":false,"mousecap":false,"generic":true}]}
```

`kbcap`/`mousecap` re-run the structural descriptor tests against the cached descriptor. bthid
caches one descriptor at a time, so they read `null` for any peer that descriptor does not belong
to — that means "not this peer's descriptor", not "capability absent". `generic` is the
generic-gamepad fallback state, which is what a controller should be sitting on.

Note that `set_global_raw_buttons()` publishes 0 for a KB/M source. That field carries the source's
`JP_BUTTON_*` bitmap, which a keyboard genuinely does not have; publishing the resolved
Switch-facing bytes there would label them as something they are not. Use `input status` for the
mapped result.

---

## Resource findings (Pico W / Pico 2 W)

Two simultaneous Bluetooth HID peers fit inside the existing capacities on **both** boards. No
connection limit was raised.

| Resource | Value | Headroom with a KB+M source |
|---|---|---|
| `MAX_NR_HCI_CONNECTIONS` | 4 | 2 HID peers + 1 management client = 3 |
| `MAX_BLE_CONNECTIONS` (central-role HID) | 2 | exactly 2; a management client is peripheral-role and is classified out before it can take a HID slot |
| `MAX_NR_HID_HOST_CONNECTIONS` (Classic) | 4 | 2 |
| `MAX_NR_L2CAP_CHANNELS` | 6 | 2 Classic HID peers use 4 (control + interrupt each), leaving room for a transient SDP channel |
| `BTHID_MAX_DEVICES` | 4 | 2 |
| `NS2_INPUT_ARBITER_MAX_SOURCES` | 8 | 2 |

`MAX_BLE_CONNECTIONS = 2` is the practical ceiling for BLE HID peers and happens to be exactly what
Keyboard + Mouse needs; a third BLE HID peer was never supported and still is not.

Firmware footprint, measured with `arm-none-eabi-size` against a clean build of the pre-change tree
(commit `505a0c8`) in a separate worktree:

| Board | Flash (text) | RAM (bss) |
|---|---|---|
| Pico W | 856 192 → 876 664 (**+20 472**, +2.4 %) | 164 232 → 168 244 (**+4 012**, +2.4 %) |
| Pico 2 W | 981 716 → 1 000 316 (**+18 600**, +1.9 %) | 231 456 → 235 476 (**+4 020**, +1.7 %) |

Pico W is the constrained board: 168 KiB of its 264 KiB SRAM is now static, and 876 KiB of its
2 MiB flash. Both remain comfortable. The RAM is dominated by the two configuration copies
(core-0 authoritative plus the core-1 snapshot, ~404 bytes each), the keyboard driver's per-device
state (4 slots, 1 104 bytes total), the widened settings program buffer (1 024 bytes), and the
bounded effective-binding response buffer (384 bytes).

There is no board on which this feature is disabled or behaves differently: the code is shared and
board-agnostic, Pico W's non-audio profile and Pico 2 W's audio profile are unaffected, and the
mouse recenter tick shares the existing 3 ms core-1 timer rather than adding one — which keeps it
off Pico 2 W's audio-sensitive scheduling.

---

## Testing

Host tests (`pwsh -File tools/run_mgmt_tests.ps1` builds and runs them into `build/host-tests`):

| Test | Covers |
|---|---|
| `test_ns2_kbm` | identifiers, canonical defaults, key down/up, simultaneous keys, report-order independence, unmapped keys, opposing directions, overrides, profile independence, override-table bounds, duplicate destinations, one-source-one-destination, remap while held, mouse translation (sensitivity, inversion, clamping, neutral return, one-shot deltas, native vs stick), state ownership and per-role clearing, configuration validation against arbitrary bytes, role admission and composite identity |
| `test_bthid_keyboard_report` | boot and NKRO descriptor classification, gamepad rejection, truncated descriptors, key down/up, modifiers, simultaneous keys, slot-order independence, rollover, truncated reports, report-ID filtering, >6 keys, error-code usages, boot fallback |
| `test_ns2_kbm_config_persistence` | defaults, erased sector, wrong magic, unknown schema, truncated record, v10→v11 migration determinism and preservation, mapping round trip, profile independence across reboot, explicit unassign persistence, reset scope, corrupt mapping repair |
| `test_ns2_input_arbiter` | (extended) composite group ownership, non-member gating, member-loss handover, stale disconnect after index reuse, whole-source loss falling back to policy, group-0 standalone behavior unchanged, explicit selection of a composite member |
| `test_ns2_active_input_lifecycle` | (unchanged) driver rebinding and lifecycle, with keyboard stubs added |

---

## Hardware validation

**Performed 2026-08-16** — ASUS ROG FALCHION RX keyboard + ROG KERIS II ACE mouse on Pico W.

Confirmed on hardware, with no re-pairing at any point in the sequence:

- Keyboard alone and mouse alone each pair, produce input, and reconnect after a power cycle.
- Both connect together as one logical source with distinct connections
  (`keyboard=true mouse=true`, `keyboardConn`/`mouseConn` distinct, one group, `ble_conns=2`), in
  either connection order.
- The KERIS II keeps `PRIMARY_MOUSE` semantics despite reporting both keyboard and pointer
  capability, and never absorbs the keyboard role when it is freed.
- Powering either role off leaves the survivor connected and usable; the vacated role clears and
  `roleLosses` increments.
- Powering that role back on rejoins it automatically, without re-pairing, without touching the
  surviving peer, and without a manual mode change.
- Discovery follows source completeness: active while the source is partial or empty
  (`scan_active=true`), retired once both roles are present (`scan_active=false`).

Remaining checklist items, exercised in host tests but not individually re-confirmed on hardware in
this session:

Keyboard mode: pair a keyboard, confirm classification and role binding, mapped movement and
buttons, simultaneous keys, opposing directions, a custom remap through `kbm bind`, clearing a
binding, restoring defaults, disconnecting while holding a mapped key (no stuck input), reconnect
neutrality, and persistence across reboot.

Keyboard + Mouse mode: pair both, confirm they form one logical source (`kbm status` group and
`input sources`), keyboard controls, mouse buttons, remapped keyboard and mouse bindings, native
mouse output under a Joy-Con 2 personality, mouse-to-stick under Pro Controller 2, mouse
translation settings, independent disconnect/reconnect of each role, partial-source safety, second
keyboard and second mouse rejection, unrelated gamepad rejection, and intact controller behavior
after returning to Controller mode.

Regression: ordinary Pro Controller 2 output, existing Joy-Con 2 mouse output, and confirmation
that no mapping configuration changes Switch-facing descriptors.

---

## Known limitations

- While a KB/M source is deliberately partial (intentional keyboard-only or mouse-only use),
  discovery stays active indefinitely. That is exactly what lets a missing role rejoin without
  re-pairing; bounding it with a completion window is deferred future work, not a defect.
- Only one keyboard report ID is followed. A keyboard declaring a second keyboard report (some
  vendor NKRO alternates) uses the first declared one, chosen deterministically so decoding never
  depends on which report happens to arrive first.
- Consumer-control keys (Usage Page 0x0C media keys) are not bindable.
- Mouse buttons beyond 5 are not addressable.
- In Controller mode, Bluetooth mouse movement continues to reach only a native mouse output
  (Joy-Con 2). Mouse-to-stick translation is a Keyboard + Mouse feature; Controller-mode behavior
  is deliberately unchanged.
- A second keyboard or mouse that connects while its role is filled stays connected and occupies a
  Bluetooth slot; it simply never reaches the console. It is counted in `rejected_duplicate`.
- After a mode change, a connected peer is re-evaluated immediately, but a peer that connects
  during the few milliseconds before core 1 applies the change is re-evaluated on its next report.
