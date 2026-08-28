# Android Companion

The native Android client lives in [`android/companion/`](../android/companion/README.md). Its README
is the build, architecture, protocol, responsive-layout, test, limitation, and hardware-validation
handoff.

The application consumes two intentionally separate PicoSwitch2 interfaces:

1. BLE GATT newline-JSON management, shared with the Web Portal.
2. Android Classic HID Device input, consumed by PicoSwitch2's generic Bluetooth gamepad parser.

The Android client does not redefine either protocol. Firmware sources and host fixtures remain the
authority for the wire contracts.

Management's Android-free reference implementation, normative language-neutral contract, BLE
session rules, and conformance material are indexed at
[`management/README.md`](management/README.md). Android keeps discovery, pairing, GATT lifecycle,
local files, NFC, and presentation; it consumes portable command/reply and workflow behavior from
`:management-core`.

The UI models these transports as a registry of known adapters with one selected, while keeping four
underlying truths explicit: app registry record, app-owned CompanionDeviceManager association,
Android Bluetooth bond, and adapter-side LE bond database. One generation-owned coordinator
arbitrates association, bond, foreground/manual reconnect, and verified management progression.
Android 13+ documents that association success is delivered through both `onAssociationCreated` and
the Activity result; that duplicate is idempotent, and `BOND_BONDING` never starts GATT. First use
says **Pair Adapter**, returning use reconnects without a chooser, a missing platform bond becomes
**Repair pairing**, and controller mode reuses the saved Classic bond without a second chooser.

### The adapter registry

Several adapters may be known; at most one holds a management session. That split is the whole
model, and it exists because the previous one-adapter design forced anyone who owned two adapters
into a *Forget / Pair / Forget / Pair* cycle.

| Concept | Type | Owns |
|---|---|---|
| Identity | `AdapterId` | The adapter's public BD_ADDR, normalized. Never the alias, never a list position. |
| Registry row | `AdapterRecord` | Alias, last known name, association ID, cached firmware/personality, last-connected, per-adapter repair flag. |
| Document | `AdapterRegistry` | The rows plus which one is active. |
| Storage | `AdapterRegistryStore` | Where and when, only. Schema, migration and tolerance are in `AdapterRegistryCodec`. |
| System reconciliation | `AdapterRegistryReconciler` | Registry against `CompanionDeviceManager.myAssociations`. |
| Connection lifecycle | `AdapterRelationshipCoordinator` | One attempt at a time, for the active adapter. Unchanged by the registry. |

Rules that must not be re-litigated:

- **Adapter identity is not adapter display name.** The alias is app-local, sanitised on entry, and
  is never written to the adapter or into its advertised name. Duplicate aliases are allowed and
  disambiguated with a four-character identity suffix.
- **Connecting to one adapter never unregisters another.** A verified connect used to delete the
  previously saved adapter's CompanionDeviceManager association whenever the association ID
  differed. That one call was the cause of the Forget/Pair churn. Adapters are unregistered only
  when the user asks.
- **Repair, remove and reconcile are all per-adapter.** Reflashing one adapter marks only that
  record `repairRequired`; repair keeps the row so the alias and history survive and the repaired
  unit rebinds to it; removing an adapter from the app drops one association, never all of them.
- **Losing an association never deletes a record.** An adapter that is powered off, or whose
  association the user removed in system settings, keeps its alias and cached state.
- **`Ambiguous` no longer means plurality.** It means two association records claim *one* adapter.
  It is reported and offers Repair; it does not block connecting, because the registry always has a
  definite address to dial.
- **Adoption never elects.** Reconciliation creates records from associations this app owns, but
  will not choose the active adapter — except for the one case that cannot be ambiguous, a registry
  holding exactly one adapter and no selection.

Live adapter state (`AdapterSnapshot`) belongs to the one active session and is cleared on
disconnect. The registry caches only what an adapter list can honestly say about a unit that is not
currently connected, and phrases it as history.

### Switching the active adapter

A switch from adapter A to adapter B is **one generation-owned transition**, owned by
`ActiveAdapterCoordinator` and executed by `AdapterSwitch`:

```text
begin(B)                      generation++, B is selected from this instant
  -> selectionCommitted(B, A) UI can name the choice before anything is dismantled
  -> stopControllerLink(A)    a controller session belongs to the adapter it was established on
  -> retireManagement(A)      AWAITED, including joining any in-flight connect job
  -> retirementComplete(gen)  false if a newer switch replaced this one; stop without activating
  -> clearAdapterScopedState()
  -> beginActivation(B)
```

Four rules, and where each is enforced:

- **A is retired completely before B becomes authoritative.** `retireManagement` returns only after
  `AdapterRepository.disconnect()` completes, which returns only after the transport has retired its
  GATT generation and emitted its final state. This is the primary guarantee; everything else is
  defence in depth.
- **No stale event from A can reach B.** `ActiveAdapterCoordinator.accepts(address)` gates the
  connection collector — nothing during a retirement, unattributed events only outside one,
  otherwise the active adapter's own address.
- **A result for A cannot settle B.** `activationSucceeded` / `activationFailed` are guarded by
  adapter identity rather than by generation, because the connect path is shared with ordinary
  reconnects that never involved a switch.
- **A failed switch never falls back.** `activeId` becomes the target when the switch begins and
  stays there on failure. The registry's selection follows the coordinator, so the two cannot
  disagree. The user sees "Selected, not connected · tap to retry" against the adapter they chose.

`AdapterRelationshipCoordinator` is untouched by this and still owns one attempt at one
relationship. The counters are separate on purpose: merged, a connection retry would be
indistinguishable from a change of adapter.

The ordering is testable without a radio — `AdapterSwitch` drives an `AdapterSwitchPort`, and
`AdapterSwitchTest` asserts the sequence against a recording fake.

The design this implements, and the audit of what it replaced, are in
[`bluetooth/bt-management-2.0-phase0-audit.md`](bluetooth/bt-management-2.0-phase0-audit.md).

### Controller names and per-adapter history

The adapter's peer inventory is authoritative about what exists, and amnesiac about what it is. Role
classification is live evidence only, so a rebooted adapter reports every saved controller that has
not reconnected as `unknown` with no name, and a controller the user unpaired last week is simply
absent from the list. Two things close that gap.

**Derived identity on the wire.** `peers list` now carries `class` (the bthid driver identity the
adapter reached, e.g. `Sony DualSense`) alongside `name` (whatever the device calls itself), plus
`vid`/`pid`. `class` outranks `name` because the remote name is a claim by the device and its owner
can change it. The full hierarchy is in `PeerNaming.label`: user alias, `class`, `name`, USB
identity, then a four-character suffix of the address — never the bare address, which reads as a
name when rendered where a name belongs.

**App-side history**, which is the design's §24.1 and is required rather than optional:

| Concept | Type | Owns |
|---|---|---|
| One remembered peer | `PeerHistoryRecord` | Best name and classification ever seen, strongest role ever *proven*, accumulated transports, first/last seen, last connected, whether the adapter still held a key at the last complete read. |
| One adapter's memory | `AdapterPeerHistory` | The fold of a complete inventory read into the above, and the `forgotten` set behind the Recent section. |
| Every adapter's memory | `PeerHistoryBook` | Keyed by `AdapterId`; dropped with the adapter. |
| Storage | `PeerHistoryStore` | Where and when, only. Schema and tolerance are in `PeerHistoryCodec`. |
| Presentation | `ControllerInventory` | Merges the live inventory and history into Connected / Saved pairings / Recent / This phone. |

Rules that must not be re-litigated:

- **History never rewrites the adapter's answer.** `PeerListing.role` is always the adapter's live
  classification. What memory supplies is a *label* and a `rememberedRole` beside it, and the row
  says so ("Controller, remembered"). The protocol's requirement that `unknown` be rendered as
  unidentified and never promoted to `controller` is preserved literally.
- **Memory is allowed to exclude, because the cost is asymmetric.** A peer this app has seen proven
  to be the user's own phone stays out of the controller list even when the adapter can no longer
  identify it. Being wrong there costs a row under "This phone"; being wrong the other way offers to
  forget the management relationship.
- **Only a COMPLETE read is recorded.** `AdapterPeerHistory.observing` refuses a partial inventory.
  A missing row is indistinguishable from a peer the adapter has forgotten, so recording one would
  move live saved pairings into Recent and tell the user controllers were unpaired when nothing
  happened.
- **Absence from a complete read is what creates a Recent row**, not a timer and not the app's own
  guess. The record is kept and marked unbonded.
- **Sections are decided by bond and connection state, not by role.** Role decides only whether a
  row is a controller at all or the phone.
- **"Remove from history" is not "Forget".** It deletes an app-local memory of a device the adapter
  already holds no key for. Selective forget of a live pairing is a later phase and is deliberately
  absent from the UI until the firmware can perform it atomically.
- **Timestamps are the phone's.** The adapter has no RTC and no time sync; it never supplies a
  wall-clock time and none is invented for it.

One inventory read happens automatically per verified management session, so history advances
without the user pressing refresh — otherwise Recent would stay permanently empty. It is deliberately
not routed through the modal progress path: it is not a user action.

**Why there is no firmware half.** The design offers persisting the same metadata on the adapter
(§24.2), which would make it survive being managed from a different phone. It was attempted after
Phase 4's first pass, destabilised the Bluetooth core, and was withdrawn. §24.2 is explicitly
conditional and names app-side history as sufficient for release. Do not reintroduce adapter flash
writes for peer metadata without new evidence about that failure.

### The management bond is always started on the LE transport

The adapter is genuinely dual-mode on one public BD_ADDR, so once a phone has observed its Classic
identity Android caches `DEVICE_TYPE_DUAL` and keeps that across *Forget*. `createBond()` is
`createBond(TRANSPORT_AUTO)`, which then prefers **BR/EDR** and runs SSP against the adapter's
*controller* admission gate; that gate correctly refuses an unbonded Classic ACL and Android reports
the refusal as "Couldn't pair because of incorrect PIN or passkey" (hardware-confirmed 2026-08-21).
A Classic link key could not satisfy `mgmt_session_authorized()` in any case, so BR/EDR was never a
slower-but-valid route here.

`AdapterBondStarter` therefore only ever picks LE mechanisms, and **TRANSPORT_AUTO is never used,
not even as a fallback**:

| Mechanism | When | API |
|---|---|---|
| `le-create-bond` | normal | `createBond(TRANSPORT_LE)` — public from API 37, otherwise reached through the single reflective seam `CompanionViewModel.androidBondPlatform` |
| `le-gatt-initiated` | the seam is blocked or absent | open the `TRANSPORT_LE` management GATT link and let the encryption-required characteristics provoke SMP — public API only |
| `none` | a working entry point refused | reported, not papered over |

`getMethod` is the runtime feature detection: a platform that hides or blocks the overload throws,
and the policy routes to the public GATT path. That one method is the *only* non-SDK Bluetooth entry
point in the app. The chosen mechanism and Android's cached device type are always logged as
`relationship/bond.mechanism`, so a future pairing failure is attributable from one line.

The GATT-initiated path carries Android's own human-paced pairing dialog inside its connect
deadline, so `ManagementConnectionContext.expectsBonding` raises that one connect's timeout; every
other connect keeps the ordinary bonded-link deadline.

The Android backend also owns one `BluetoothGatt` generation at a time. Teardown waits for its
matching disconnected callback with a bounded timeout before closing exactly once; stale callbacks
cannot mutate a newer session. A recoverable 133, connection timeout, or congestion result receives
one clean retry, followed by one service scan pinned to the saved address. Management reports
Connected only after the PicoSwitch2 identity probe succeeds. Explicit Disconnect closes management
only and does not call the independent Controller Bridge or physical-controller paths.

Color commit is now one happy-path action: mutate, authoritative readback, save, identified
persistence completion when supported, and automatic same-personality USB re-enumeration. Only a
partial re-enumeration failure leaves a Retry affordance. These lifecycle/color changes are
source/JVM-tested as of 2026-08-20 and await the focused AYN Thor matrix in
[`HARDWARE_VALIDATION.md`](../android/companion/HARDWARE_VALIDATION.md). The original app-led HID
path through PicoSwitch2 into a real game remains hardware-confirmed from 2026-08-13.

The application is organized around five destinations — **Adapter**, **Keyboard**, **Amiibo**,
**Gamepad**, **Settings** — with **Diagnostics** and **Amiibo settings** pushed over them rather
than holding permanent navigation space. Layout branches on measured content width, never on
orientation or device names. The full information architecture, shared Compose primitives,
responsive rules, and the debug-only layout lab used to inspect them are documented in
[`android/companion/README.md`](../android/companion/README.md).

**Keyboard & Mouse** is a first-class management area covering the firmware's complete `kbm`
surface: device and role status, input mode, both mapping profiles with a per-input editor, mouse
button mapping, and mouse translation tuning. Those settings apply to adapter RAM immediately and
are written to flash only by an explicit Save, and the UI represents exactly that rather than
implying persistence. Every accepted range is taken from the adapter's own `kbm mouse` reply, so
the client never carries a duplicate copy of a firmware limit.

Two requested management features are **blocked by missing firmware capability**, not by client
work. Controller remapping would need a `remap` command family with persisted overrides —
`NS2_BASE_BUTTON_MAP` is a compile-time table today. Adapter renaming would need a persisted name
plus dynamic advertising, EIR and ATT Device Name construction — the name is currently a
compile-time constant whose length is locked by a `_Static_assert`.

The companion's appearance is intentionally client-local. It supports System, Light, Dark, and
true OLED-black themes, with a small set of labeled Joy-Con-inspired accent palettes. The verified
Joy-Con 2 references are the left `#9BE1E6` and right `#FF8C5F` accents documented in
[`switch2-joycon2/protocol.md`](switch2-joycon2/protocol.md); selecting an inspired palette does
not issue `body`, `jcl`, or `jcr` firmware commands and is not a hardware identity claim.

### System-bar appearance is drawn by the app, not set on the window

The companion is edge-to-edge on every supported API level, and its status- and navigation-bar
regions are painted by `CompanionTheme` as `ColorScheme.background` behind fully transparent bars.
Only the icon polarity is still asked of the window, via `WindowInsetsControllerCompat`, and it is
derived from the same resolved light/dark decision as the colour scheme — so forcing Dark on a
light device still gets readable light icons.

This is not stylistic. The app targets SDK 35, where the platform forces edge-to-edge and turns
`Window.setStatusBarColor` / `setNavigationBarColor` into no-ops. Those setters were what used to
colour the bars, so on API 35+ they coloured nothing and the bar regions fell through to the
platform theme's `windowBackground` of `#FAFAFA` — measured on an Android 16 tablet as white
strips above and below a dark application. Do not reintroduce either setter as a fix: on a modern
device it does nothing, and the app would silently diverge between API levels again.

Navigation-bar contrast enforcement is disabled, which is the platform opt-out for an app that
guarantees contrast itself. Left enabled it was measured compositing a low-alpha overlay over the
app's own surface on a 3-button device — `(16,19,26)` rendered as `(28,21,27)` in dark, `(247,249,255)`
as `(254,254,255)` in light — a faint band buying no legibility. Gesture navigation draws nothing
either way.

`res/values-night/` carries only what the starting window needs before Compose exists:
`@color/window_background` and `@bool/system_bar_icons_dark`. Those duplicate `ColorScheme.background`
by necessity and must be changed with it, or a cold launch shows a seam for one frame.

Current implementation status is tracked in
[`android/companion/FEATURE_PARITY.md`](../android/companion/FEATURE_PARITY.md). The intentionally
short eventual hardware session is
[`android/companion/HARDWARE_VALIDATION.md`](../android/companion/HARDWARE_VALIDATION.md).
The first AYN Thor live results and remaining end-to-end gates are preserved in
[`experiments/android-companion-ayn-thor-live-2026-08-13.md`](experiments/android-companion-ayn-thor-live-2026-08-13.md).

The Android Amiibo page parity slice is recorded in
[`experiments/android-amiibo-page-parity-2026-08-13.md`](experiments/android-amiibo-page-parity-2026-08-13.md).
It now presents the raw-image identity fields that the portal uses and can read encrypted owner,
nickname, registration/write dates, write count, and game-data identifiers with the user’s own
portal-compatible 160-byte `key_retail.bin`. The key remains app-private, is excluded by the
no-backup manifest, and never enters firmware commands, diagnostics, or library export. Import,
selection, upload, Sync, present/eject, and clean/used copy operations remain key-free and offline-
safe. A compact seven-day AmiiboAPI cache now adds portal-matched friendly names, series/type/release,
compatible games/title-ID labels, and best-effort artwork; stale or offline catalog/image requests
never gate the local library or adapter flows. Android now also provides confirmation-gated local
initialization/re-signing with the user's own key, a bounded traversal-safe portal-compatible v3 ZIP
library exchange, and a host-tested foreground one-shot ordinary-NTAG215 NFC backup path. None
changes the adapter. The physical NFC gate is pending, figure-v3 phone reads remain deliberately
rejected, and Mii rendering remains deferred.
