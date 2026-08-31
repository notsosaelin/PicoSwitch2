# Companion library interaction model

Status: **Implemented on Windows and Android; behaviour verified on both.**

Date: 2026-08-31

Scope: how a user browses, inspects and bulk-edits the Amiibo library in the companion apps. This
is a *behavioural contract*, not a layout: it is shared source on both clients and is intended to
govern any future touch-capable PicoSwitch companion.

Not in scope: the Amiibo backend, crypto, sync model, or the firmware's active-tag lifecycle. None
of them are touched by this model.

## 1. The rule

```
single tap / click   →  browse   (move the highlight)
double tap / click   →  inspect  (open the details surface)
long press           →  select   (begin a multi-selection)
```

Everything below is a consequence of those three lines.

## 2. Three concepts, not one

The clients each carried a single "selected Amiibo" that silently meant three different things.
They are now separate, in one shared record:

| Concept | Field | Means | Changed by |
| --- | --- | --- | --- |
| Focused | `focusedId` | which card is highlighted | tap, click, arrow keys |
| Inspected | `inspectedId` | whose details surface is **open** | double tap/click, "Open details" |
| Selected | `selection` | the set a bulk command acts on | long press, Ctrl+click, "Select" |

Conflating them is why the details pane used to follow every highlight, and why there was nowhere
to put a multi-selection.

### The invariant

**When the inspector is open, `inspectedId == focusedId`.** Opening details moves focus to the item
being described, and moving focus elsewhere closes the inspector.

This is what keeps single-item commands correct. Send to adapter, Rename, Initialize and Delete all
live in the details surface and are keyed on *focus*; if the two could disagree, a button in a pane
headed "Link" could act on Zelda. Every transition preserves it and a test walks all of them.

### Selection mode is derived

`selecting` is `selection.isNotEmpty()`, never a separate flag — a flag can disagree with its own
set. Removing the last item therefore leaves selection mode, which is also what a user expects from
un-ticking the last box.

## 3. Transitions

`AmiiboInteraction` is the complete set. Gesture and pointer handlers translate input into these
and do nothing else, which is what makes the model testable without rendering anything.

| Transition | Effect |
| --- | --- |
| `activate(id)` | browse in normal mode; toggle membership while selecting |
| `focus(id)` | move the highlight; **closes an open inspector** unless it is already this item |
| `openInspector(id)` | open details; **refused while selecting** |
| `closeInspector()` | dismiss details; focus and query state untouched |
| `enterSelection(id)` | begin a selection; **closes the inspector**; adds rather than restarts if already selecting |
| `toggleSelection(id)` | add/remove; also starts a selection from nothing |
| `clearSelection()` | leave selection mode; focus and query state untouched |
| `escape()` | cancel a selection, else close the inspector, else **no change** |
| `prune(libraryIds)` | reconcile with a library that lost entries |
| `afterRemoval(ordered, removed)` | settle after a bulk delete: re-focus the neighbour, stop selecting |

`escape()` returning an unchanged state is how a caller knows Android's Back should navigate away
rather than be swallowed.

### Why focus does not carry the inspector with it

The user explicitly asked to see A. Clicking B is a return to browsing, not a request to see B.
Making the pane follow the highlight is the behaviour this model exists to remove. B is inspected by
asking for it.

### Why details and multi-select are mutually exclusive

A multi-selection and a single-item inspector on screen together give every command in the inspector
an ambiguous scope — does Initialize mean this one, or those twelve? The two are exclusive by
construction rather than by warning.

## 4. Selection identity and the filter policy

**Chosen and pinned: selection holds stable library ids and SURVIVES a query change.** Sorting,
searching and filtering cannot disturb it, because none of them change what an item's id is. Only an
entry actually leaving the library removes it, via `prune`.

The consequence is that the set can legitimately contain entries the current query hides.
`hiddenSelectedCount(visibleIds)` exists so both the selection bar and the destructive confirmation
can say how many of the doomed entries are off screen. A count that silently disagrees with what is
displayed is how somebody deletes more than they meant to.

The alternative policy — clear the selection whenever the query changes — was rejected as more
surprising, but it is a legitimate choice; if it is ever adopted it must be done explicitly and
tested, not allowed to happen by accident.

## 5. Where the details surface appears

**Width decides WHERE, never WHETHER.** Browsing owns the whole surface until the user explicitly
inspects something. Both clients previously reserved space for a pane describing whatever happened
to be highlighted.

| Client / size | Treatment |
| --- | --- |
| Android compact (portrait, phone landscape) | modal bottom sheet |
| Android large (tablet) | two-pane, created on open and dismantled on close |
| Windows ≥ 860 pt content width | pane beside the browser |
| Windows below that | overlay spanning the browser |

Always dismissible. The sheet by swipe or Back; the pane by an explicit close button, because a pane
has neither native gesture and would otherwise be pinned for the session. Closing preserves scroll
position, search, filters, sort, direction, view mode and focus.

## 6. Bulk operations

Exactly two: **Initialize** and **Delete**. Multi-selection existing is not a reason to invent a
dozen batch commands, and a crowded bar makes the destructive pair harder to aim at.

- **One confirmation per batch**, never one per entry. Twelve dialogs is a prompt users learn to
  dismiss without reading.
- **Both are local.** Initialize rewrites stored bytes; Delete removes local backups. Neither
  introduces adapter traffic, and neither affects the resident tag. This matches the single-item
  commands exactly.
- **Entries are attempted independently.** A dump the imported key cannot verify will refuse to
  re-sign, so partial failure is expected rather than exceptional. `AmiiboBulkOutcome` carries the
  successes and the failures, and `summary(verb)` never rounds a partial result up to a success —
  "12 Amiibo initialized" after three failed is a lie the user discovers much later, from a tag that
  did not change.
- After a delete the highlight lands on the neighbour that took the removed item's place, so
  deleting from the middle of a thousand-item library does not send the user back to the top.

## 7. Per-platform input

| Intent | Android | Windows |
| --- | --- | --- |
| Browse | tap | click |
| Inspect | double tap | double click, or **Details** button |
| Begin selection | long press | Ctrl+click, touch long press, or **Select** button |
| Toggle membership | tap while selecting | click while selecting, or Ctrl+click |
| Cancel | Back | Escape, or the bar's ✕ |

### Traps worth remembering

Both are real defects found by driving the built UI, not by reading the source.

- **WinUI raises `SelectionChanged` only when the host's own selection MOVES.** Driving selection
  from it meant clicking the already-highlighted tile raised nothing, so that one tile could be
  ticked and never un-ticked. `Tapped` fires for every tap.
- **A `GridViewItem` marks `Tapped` and `PointerPressed` as HANDLED** when the tap changes its
  selection, and a handler attached in XAML never sees a handled event. `AddHandler` with
  `handledEventsToo: true` is the only way, and it cannot be expressed in markup.

## 8. Accessibility

Long press and double tap are invisible affordances, and neither exists for a keyboard, a screen
reader or a switch device. Every transition therefore has a non-gesture route:

- Android: named custom accessibility actions (`Select` / `Deselect`, `Open details`) on each item;
  items announce their **membership**, not their highlight, because membership is what a destructive
  command acts on.
- Windows: **Details** and **Select** buttons, `Escape`, and per-tile automation names — the tree
  previously announced the type name for all thousand items.

## 9. Where it lives

| | Path |
| --- | --- |
| Shared model (C#) | `windows/companion/src/PicoSwitch.Companion.Services/Presentation/AmiiboInteractionState.cs` |
| Shared model (Kotlin) | `android/companion/app/src/main/java/dev/picoswitch/companion/data/AmiiboInteractionState.kt` |
| Contract tests | `AmiiboInteractionTests.cs` / `AmiiboInteractionTest.kt` — the same 36 properties on both |
| Windows surface | `windows/companion/src/PicoSwitch.Companion.App/Pages/AmiiboPage.xaml{,.cs}` |
| Android surface | `android/companion/app/src/main/java/dev/picoswitch/companion/ui/AmiiboScreen.kt` |

The two test suites are deliberately parallel. They are the guard against the clients drifting into
behaving differently while each looks correct on its own.

## 10. Validation

Automated: 36 contract properties on each platform, plus the surrounding library suites (Windows
832 tests; Android 518).

Manual, on the built applications:

- Windows, 962-entry library: full-width browsing; single click highlights only; double click opens;
  focusing another item closes rather than replaces; Ctrl+click and the Select button both start a
  selection; clicking the highlighted tile toggles it; Escape exits; bulk bar and both confirmations.
- Android, via the debug layout lab: portrait, phone landscape and tablet all give the browser the
  whole canvas; long press enters selection; taps toggle, including on the focused item; double tap
  opens the sheet (compact) or the pane (tablet); single tap closes an open pane; Back exits
  selection; bulk delete confirmation states the count. No crashes in logcat.

Not covered by automation: Windows touch long press on real touch hardware — `Holding` is raised for
touch and pen only, so a mouse cannot exercise it.
