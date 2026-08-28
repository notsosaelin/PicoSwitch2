# Bluetooth Management 2.0 — Phase 0 audit, and the phase record

Date: 2026-08-27, last extended 2026-08-28
Scope: `BT_MANAGEMENT_UPGRADE_PASS.md` §96 Phase 0 ("Repository and Protocol Audit"), and each
implementation phase that has followed it.

Phase 0's deliverable is §§1–7: the HLD's concepts mapped onto the files and functions that exist,
and the §111 stop conditions answered. §8 onwards records what each phase then did — including the
places where an implementation deliberately departs from the obvious reading of the design, and the
one place where the design's own recommendation was tried and withdrawn (§8c).

| Phase | State | Firmware changed | Reflash |
|---|---|---|---|
| 0 — audit | Complete 2026-08-27 | no | no |
| 1 — registry | Complete 2026-08-27 | no | no |
| 2 — switching | Complete 2026-08-27 | no | no |
| 3 — peer inventory | Complete 2026-08-27 | yes | yes |
| 4 — names, classification, history | Complete 2026-08-28 | yes | yes |
| 5 — selective forget | Complete 2026-08-28 | yes | yes |
| 6 — remote pairing | Complete 2026-08-28 | yes | yes |

Every phase's software validation is in §10; each phase's hardware gate is a checklist the
maintainer runs (§§11–14) and is **not** claimed as performed here.

Evidence classes follow [`../re-methodology/evidence-standards.md`](../re-methodology/evidence-standards.md).

---

## 1. Verdict

Phase 0 is complete and **Phase 1 may proceed**, with two findings the HLD did not anticipate and
one it under-stated. None of them is a hard stop; all three change what Phase 1/2 must do.

| # | Finding | Effect |
|---|---|---|
| F1 | The companion already uses `CompanionDeviceManager` as a load-bearing part of the relationship model, and **treats more than one association as `Ambiguous` → `RepairRequired`**. | HLD §3.2 ("evaluate CDM, do not migrate to it") is out of date. Phase 1 must *widen* an existing CDM-backed model, not add a registry beside a CDM-free one. |
| F2 | On every verified connect, the app **actively `disassociate()`s the previously saved adapter** when the association ID differs. | This is the single line that makes multi-adapter impossible. It is a deliberate one-relationship enforcement, not an oversight, and Phase 1 must retire it explicitly. |
| F3 | The phone is simultaneously the LE management peer **and** the Classic Controller Link peer, and `ENABLE_CROSS_TRANSPORT_KEY_DERIVATION` is on. | HLD §16 is not a hypothetical here — it is the project's *most common* multi-entry peer. Selective forget (Phase 5) must be designed around this case first, not as an edge case. |

Positive findings that de-risk later phases:

- A stable adapter identity **already exists and is already used**: the management peripheral
  advertises with `BD_ADDR_TYPE_LE_PUBLIC`, and the saved relationship is keyed on that address.
  No new identifier needs to be invented or newly broadcast (§8.2, §8.3 satisfied without cost).
- An atomic `forget_peer` **already exists** as `btstack_host_forget_device_typed()`, and it already
  does the §63 sequence: cancel in-flight connect, disconnect live links, delete the LE bond via
  `gap_delete_bonding()`, drop the Classic link key, clear reconnect state.
- Opening the controller pairing window **does not tear down BLE management** (§111 stop condition 5
  does not fire). Confirmed by reading the call graph, see §7.3.
- The management framing already has a working cursor-pagination precedent (`bonds list v2`) that
  Phase 3 should reuse verbatim rather than redesign (§45 satisfied).

---

## 2. HLD concept → current implementation map

### 2.1 Android companion

| HLD concept | Current reality | File |
|---|---|---|
| `ManagementOwner` (§2.4) | Exists, application-scoped singleton, holds exactly one `AdapterRepository`. Built after a hardware-confirmed five-`MainActivity` incident on 2026-08-23. | `app/.../data/ManagementOwner.kt` |
| `AdapterRegistryRepository` (§84) | **Does not exist.** Nearest thing is `AdapterRelationshipStore`, a `SharedPreferences` file holding exactly **one** record (address, associationId, displayName). | `app/.../data/AdapterRelationship.kt` |
| `ActiveAdapterCoordinator` (§84) | **Does not exist as such.** `AdapterRelationshipCoordinator` owns generation-safe association/bond/connect progression for the one relationship; it is the correct thing to generalise, not to replace. | `app/.../data/AdapterRelationshipLifecycle.kt` |
| Stale-generation safety (§2.5) | Exists at two layers: `AdapterRelationshipCoordinator.generation` (logical attempt) and `BleGattManagementTransport` `OwnedGatt.generation` + `GattCallbackAuthority`. | `AdapterRelationshipLifecycle.kt`, `transport/BleGattManagementTransport.kt`, `transport/GattRecoveryPolicy.kt` |
| `ManagementSession` serialization (§3.4) | Exists. `SerializedManagementSession` plus a single-flight lock in the transport. | `management-core/.../SerializedManagementSession.kt` |
| Repair-required path (§12) | Exists and is adapter-specific *by accident of there being one adapter*. Triggered by `AdapterResetSignature.isBondMismatch` (HCI 0x05/0x06 while Android still bonded) or by a `BOND_NONE` broadcast. | `AdapterRelationshipLifecycle.kt`, `GattRecoveryPolicy.kt` |
| Scan filter (§67) | Service-UUID filtered on the project's own 128-bit management service; optionally address-restricted for a saved adapter. Never adopts a different Pico silently. | `BleGattManagementTransport.scanDeviceLocked` |
| Management protocol parser (§43) | `management-core` — `ManagementProtocol` (decode), `ManagementCommands` (encode), `ManagementClient` (command semantics + capability probing). | `management-core/.../ManagementProtocol.kt`, `ManagementClient.kt` |
| Capability negotiation (§42) | Exists as **probe-based**, not declared: `AdapterCapabilities` with `Available/Unsupported/Unknown` per family, derived from `unknown command` replies. | `management-core/.../Domain.kt`, `ManagementClient.optional` |
| Peer inventory UI (§25) | **Does not exist.** The only bond surface is Settings → "Adapter Bluetooth LE bonds", a flat list of LE device-DB slots with a per-row remove icon. | `app/.../ui/SettingsScreen.kt` |
| UI state keyed by adapter (§85) | **Not keyed.** `AdapterSnapshot` is one global object on the single repository. | `management-core/.../Domain.kt`, `data/AdapterRepository.kt` |
| Android permissions (§69) | `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`, legacy `BLUETOOTH`/`BLUETOOTH_ADMIN`/`ACCESS_FINE_LOCATION` capped at SDK 30. `minSdk 28`, `targetSdk 35`, `compileSdk 36`. Nothing new is required for a registry. | `app/src/main/AndroidManifest.xml`, `app/build.gradle.kts` |

### 2.2 Firmware

| HLD concept | Current reality | File |
|---|---|---|
| Management command dispatcher (§75) | One `handle_line()` if/else chain over newline-framed JSON commands, shared by CDC Config and in-band BLE. | `src/config.c:1451` |
| Management authorization (§51) | `mgmt_access.c` — pure predicates, exhaustively host-tested. Writes require enabled + connected + bonded + encrypted + allowlisted. Trust additionally requires a 16-byte key. | `src/mgmt_access.c`, `src/mgmt_access.h`, `tools/test_mgmt_access.c` |
| Wireless command allowlist | `config_wireless_command_allowed()`. Any new management verb must be added here or it is unreachable over BLE. | `src/config_wireless_bridge.c:204` |
| Adapter stable identity (§8.2) | **Not exposed by any command.** But the LE peripheral advertises with the public BD_ADDR (`hci_le_set_own_address_type(BD_ADDR_TYPE_LE_PUBLIC)`), so the app already has it. | `btstack_host.c:2469` |
| Classic link-key backend (§17) | BTstack TLV `BTL0…BTL15`. Read/drop APIs are already used (`gap_get_link_key_for_bd_addr`, `gap_drop_link_key_for_bd_addr`, `gap_delete_all_link_keys`). **No iterator is used anywhere** — there is currently no enumeration. | `btstack_host.c`, [`PERSISTENCE.md`](PERSISTENCE.md) |
| LE device DB backend (§18) | BTstack TLV `BTD0…BTD15`. Enumerated by capacity, not by count, via `btstack_host_le_bond_entry_at`. Deletion goes through `gap_delete_bonding()` so resolving-list side effects survive. | `btstack_host.c:6767`, `PERSISTENCE.md` |
| Bond capacities (§41) | `NVM_NUM_LINK_KEYS 16`, `NVM_NUM_DEVICE_DB_ENTRIES 16`, `MAX_NR_LE_DEVICE_DB_ENTRIES 16`. (Equal to BlueRetro's published 16/16 by coincidence, not by copying.) | `src/btstack_config.h` |
| Cross-transport derivation (§16) | `ENABLE_CROSS_TRANSPORT_KEY_DERIVATION` is **on**. | `src/btstack_config.h:32` |
| Physical pairing state machine (§32) | `open_pairing_window(now_ms)`, a 30 s window (`PAIRING_WINDOW_MS`) driven by the BOOTSEL double-tap gesture. Single authoritative entry point. | `src/bt_hid/ns2_bt_host.c:128` |
| Bond list/remove marshalling (§81) | Established pattern: core0 command → `btstack_run_loop_execute_on_main_thread` → core1 op → `*_done` flag → core0 formats reply. Wireless callers complete asynchronously; CDC pumps USB. | `btstack_host.c:6746`, `src/config.c:1195` |
| Atomic forget (§63) | `btstack_host_forget_device_typed()` already performs the full sequence. Currently only reachable via LE-slot removal and the MouthPad relay. | `btstack_host.c:12446` |
| Role/classification tracking (§14, §61) | Partial and **live-only**: `ns2_input_arbiter` records per-source transport, BD address, VID/PID, 32-char name, and a source class (`DIRECT` vs `BRIDGE` vs `UNKNOWN`). Nothing is persisted per peer. | `include/ns2_input_arbiter.h`, `src/config.c:829` |
| Remote name acquisition (§21) | Already implemented in the Classic inquiry path — EIR complete-local-name, else `gap_remote_name_request()`. | `btstack_host.c:4321`, `:4613`, `:4834` |
| Peer metadata storage (§55) | **Does not exist.** Project-owned TLV tags are an established pattern (`JPLC` last-connected hint, `JPLK` pairing lockout), so the mechanism is available. | `btstack_host.c:1529` |
| Host-testable bond logic | `ns2_bt_lifecycle.c` already hosts the pure parts (`ns2_bt_find_bond_slot`, `ns2_bt_forget_matches_address_type`) with `tools/test_ns2_bt_lifecycle.c`. This is where Phase 3/5 pure logic belongs. | `src/ns2_bt_lifecycle.c` |

### 2.3 Management wire

| Property | Value | Source |
|---|---|---|
| Command capacity | 128 bytes | `CONFIG_WIRELESS_COMMAND_CAPACITY` |
| Reply capacity | 512-byte slot, **511-byte JSON payload** + newline | `CONFIG_WIRELESS_RESPONSE_CAPACITY`, `BleManagementContract.MAX_REPLY_PAYLOAD_BYTES` |
| Framing | newline-delimited JSON lines, one command in flight | `config_wireless_bridge.h` |
| Delivery model | command/response only. The TX characteristic is the reply channel; **there is no event/notification stream** | `btstack_host.c` `config_ble`, `BleReplyAssembler` |
| Pagination precedent | `bonds list v2 [cursor]` → `{"v":2,"total":N,"bonds":[…],"next":int\|null}` | `src/mgmt_bonds.c`, [`../management/PROTOCOL.md`](../management/PROTOCOL.md) |
| Version negotiation | **None.** `info` returns `bridge_contract` (Controller Bridge, not management) and `build`. Management compatibility is per-command probing plus the `v:2` bond envelope | `PROTOCOL.md` §"Personality, wake, management, bonds, and save" |

---

## 3. F1 — CompanionDeviceManager is already load-bearing, and rejects plurality

HLD §3.2 says to "evaluate CDM during Phase 0, but do not migrate the existing trusted management
architecture to CDM merely because it exists." The premise is wrong: the migration already happened.

`AdapterRelationship` carries an `associationId`. `AdapterRelationshipReconciler.reconcile()` is the
startup authority that decides which adapter the app is talking to, and it reconciles the one saved
record against `CompanionDeviceManager.myAssociations`. Its outcomes are:

- exact association-ID match → `Present`;
- single address match → `Present`;
- saved record with no association → `SavedWithoutAssociation` / `Missing`;
- **no saved record and exactly one association → adopt it**;
- **no saved record and more than one association → `Ambiguous`.**

`Ambiguous` is not a soft state. `AdapterRelationshipCoordinator.restore()` maps it to
`AdapterRelationshipPhase.RepairRequired` with the copy *"Android has multiple PicoSwitch2 companion
associations; choose Repair pairing before reconnecting."* Repair then calls
`disassociateAllCompanionAssociations()`, deleting every app-owned CDM record.

**Confidence: Confirmed** (read directly from current source).

Consequence for Phase 1: the registry cannot be bolted on beside this. Multiple associations are
exactly the normal state Phase 1 creates, so `reconcile()` must become a *set* reconciliation
returning one `AdapterRecord` per association, and `Ambiguous` must be redefined to mean "two
associations claim the same identity" rather than "more than one association exists". The
`OnlyAssociation` adoption rule must not silently adopt one of several.

CDM also turns out to be an asset the HLD did not credit: association records give the registry a
system-managed, reboot-surviving handle per adapter, and `AssociationInfo.deviceMacAddress` supplies
the address key. Phase 1 should keep CDM as the registry's backing identity source rather than
introducing a parallel one.

## 4. F2 — the app deliberately deletes the previous adapter's association

In `CompanionViewModel`, immediately after a connection is verified and the relationship is saved:

```kotlin
val previous = relationshipStore.load()
relationshipStore.save(verified)
if (previous?.associationId != null && previous.associationId != verified.associationId) {
    disassociate(previous)
}
```

This is the mechanism behind the user-visible "Forget / Pair / Forget / Pair" churn the HLD opens
with. It is a *correct* implementation of a one-relationship product, not a defect, and it is
paired with a diagnostic that records "Android bond retained" — the Android bond is deliberately
left alone; only the CDM association is destroyed.

**Confidence: Confirmed.**

Consequence for Phase 1: this call site is the definition of single-adapter and must be removed as
part of introducing the registry, together with the `disassociate()` call in the ordinary Forget
path being re-scoped to "remove *this* adapter from the app" (§48). It must not be left in place
"until Phase 2", because with the registry present it would silently delete registry entries.

## 5. F3 — the management phone is the project's canonical CTKD multi-entry peer

The HLD treats cross-transport peers (§16) as a caution for physical controllers. In PicoSwitch2 the
clearest instance is the phone itself:

- **BLE management** — the companion bonds to the LE peripheral. Its identity is recorded live in
  `config_ble.client_addr` and persisted as a `BTD*` LE device-DB entry.
- **Controller Link** — the same phone connects as a Classic HID *Device* to the adapter's HID Host
  role, producing a `BTL*` Classic link key and an entry in `classic_state.connections`.
- `ENABLE_CROSS_TRANSPORT_KEY_DERIVATION` is enabled, so one of those keys may be *derived* from the
  other rather than independently established.

The existing code already knows this matters. `btstack_host_forget_device_typed()` takes a
`match_address_type` flag precisely so that a typed LE removal "MUST NOT disturb a same-address
Classic relationship", and only the address-only helper drops the Classic key as well. That comment
is the invariant Phase 5 has to preserve.

The project also carries a standing risk note that BTstack 1.8.2's CTKD persistence behaviour is
imperfect and was accepted as a Stage C migration risk (see [`STATUS.md`](../../STATUS.md) and
[`bt-stack-migration.md`](bt-stack-migration.md)). Phase 5 must therefore be validated against the
pinned revision (§80) rather than against the API contract alone.

**Confidence: Confirmed** for the topology and the config flag. **Hypothesis** for whether any
specific stored pair on a real adapter is genuinely CTKD-linked rather than independently bonded —
that requires a capture and is the first Phase 3 experiment, not a Phase 0 conclusion.

Consequence: Phase 3's role model must classify `MANAGEMENT_COMPANION` and `ANDROID_CONTROLLER_LINK`
before it classifies anything else, and the peer model must allow one logical peer to hold both a
`BTD*` and a `BTL*` entry from the first version. Doing that later is a schema break.

---

## 6. Storage audit (HLD §24.2, §55, §56, §89)

Physical flash map, highest address first (from [`PERSISTENCE.md`](PERSISTENCE.md) and
`src/nfc/virtual_amiibo_store.c`):

| Offset | Owner |
|---|---|
| `SIZE-1S` | reserved by the SDK on RP2350 |
| `SIZE-2S` | BTstack TLV bank B |
| `SIZE-3S` | BTstack TLV bank A |
| `SIZE-4S` | PicoSwitch2 settings record |
| `SIZE-5S` | amiibo journal bank 1 |
| `SIZE-6S` | amiibo journal bank 0 |

All six sectors are allocated; there is no free sector. Two viable homes for peer metadata exist:

1. **Project TLV tags** in the existing BTstack banks, following the `JPLC`/`JPLK` precedent
   (`btstack_host.c:1529`). Preferred: it puts metadata beside the security records it annotates, it
   is already wear-managed by BTstack's TLV log, and it survives without touching the settings
   erase cycle.
2. **The settings sector's spare space.** `CONFIG_RECORD_BYTES` is `4 * FLASH_PAGE_SIZE` = 1024 B
   inside a 4096 B erase sector, so roughly 3 KiB is unused. Sixteen peers at ~56 B each (identity +
   type + role + 32-char sanitized name + classification + coarse counters) is ≈ 900 B and fits.
   Drawback: the sector is erased whole on **every** settings save, so peer metadata would inherit
   the settings write cadence rather than its own (§56).

**Conclusion: storage capacity is sufficient; §111 stop condition 6 does not fire.** Option 1 is the
recommendation. Either way the record must be schema-versioned and advisory — §57's rule that
security data is authoritative and metadata is never allowed to delete a valid key.

> **Superseded in practice by Phase 4 (2026-08-28).** Capacity was never the binding constraint.
> Adapter-side peer metadata was implemented and it **destabilised the Bluetooth core**; it was
> withdrawn and Phase 4 shipped the app-side half alone. This section's capacity finding still
> stands and is still what a future attempt would build on, but "storage permits it" is not on its
> own a reason to build it. See §8c.

Note for §93 (timestamps): the adapter has no RTC and no time sync. Firmware metadata must store
monotonic/coarse sequence values only; wall-clock `lastSeenAt` belongs to the app.

Note for §2.6: `PERSISTENT_FLASH_START` covers all six sectors and the install marker erases the
whole region on first boot after a new UF2. Firmware peer metadata will therefore be destroyed by a
reflash exactly as bonds are — which is consistent with current policy, and is another reason
app-side history (§24.1) is the required half and firmware metadata the optional half.

---

## 7. §111 stop conditions

| # | Condition | Result |
|---|---|---|
| 1 | No stable adapter identity, or adding one has uncovered privacy consequences | **Does not fire.** The management peripheral already advertises with the public BD_ADDR and the app already keys on it. No new broadcast identifier is needed, so §8.3's privacy trade-off does not arise. See §7.1 below. |
| 2 | BTstack bond stores configured differently than expected | **Does not fire.** 16 Classic link keys, 16 LE device-DB entries, one TLV instance, sparse slots traversed by capacity. Exactly the shape §17/§18 assume. |
| 3 | Controller Link and management peer identities cannot be distinguished | **Does not fire, with a caveat.** They are cleanly distinguishable live (`config_ble.client_addr` vs `classic_state.connections` + arbiter `source_class`) and by transport at rest (`BTD*` vs `BTL*`). The caveat is F3: they are the *same phone*, so distinguishing the *roles* is easy while distinguishing the *peer* is not — which is the correct answer for §29 anyway. |
| 4 | CTKD makes selective deletion ambiguous | **Does not fire as a blocker, but is elevated.** The typed/untyped split in `btstack_host_forget_device_typed()` is an existing, tested boundary that already answers "delete one transport" vs "delete the peer". See §5. |
| 5 | Pairing mode requires management teardown for a hardware reason | **Does not fire.** See §7.3 — this is a decisive positive result for Phase 6. |
| 6 | Metadata storage capacity insufficient | **Does not fire.** See §6. |
| 7 | Existing management framing cannot safely carry peer inventory | **Does not fire.** 511-byte replies with an existing cursor-pagination precedent. Phase 3 must reuse `bonds list v2`'s envelope and its "a page must make progress" fail-closed rule rather than inventing framing. |
| 8 | Old/new firmware compatibility needs a breaking migration | **Does not fire.** New verbs return `unknown command` on old firmware and the client's `optional{}` probe already maps that to `CapabilityState.Unsupported`. §42's declared-capability set can be added later as an optimisation, not a prerequisite. |

### 7.1 Adapter identity — detail

`config_ble_start_advertising()` calls `hci_le_set_own_address_type(BD_ADDR_TYPE_LE_PUBLIC)` before
`gap_advertisements_enable(1)`, so the management peripheral is reachable at the CYW43439's public
BD_ADDR. `AdapterRelationshipStore` already persists that address as the adapter's key.

Is it stable across a firmware flash? **Strong evidence: yes.** The entire repair path is built on
the observation that after a reflash the adapter reconnects *at the same address* while rejecting
the stored key — `AdapterResetSignature.isBondMismatch` matches HCI `0x05`/`0x06` *while Android is
still bonded*, and the 2026-08-23 hardware observation recorded in `AdapterRelationshipLifecycle.kt`
saw six such attempts across fourteen minutes at one address. A changed address would have produced
scan failures, not authentication failures.

This is not promoted to Confirmed because it has never been the *subject* of a test — it is a
by-product of a different investigation. Phase 1 should record the address alongside a firmware-read
identity if one is added, and must implement §59's identity-mismatch path rather than assuming
address equality forever.

**Recommendation:** do not add a new identity command in Phase 1. Use the public BD_ADDR, and revisit
only if §59 mismatch handling proves it insufficient.

### 7.2 Peer inventory framing — detail

`mgmt_bonds_format_page()` already implements the discipline §45 asks for: a reserved suffix budget,
refusal to emit a partial array, a null-vs-integer `next` cursor, and a fail-closed error when even
one entry cannot fit so a client cannot spin on a cursor. `ManagementClient.collectBondPages()` is
the matching client with total-stability, unique-index, and cursor-progress validation.

Phase 3 should extend this rather than write a second pager, and must budget for the wider peer
record (role, transport, name, classification) against the same 511-byte reply — a 32-char name
alone is a large fraction of one entry, so the per-page entry count will be small.

### 7.3 Pairing does not tear down management — detail

HLD §83 asks Phase 0 to check whether the physical pairing state machine unnecessarily destroys the
management session. It does not.

```
BOOTSEL double-tap
  -> bootsel_action_resolve(...) == BOOTSEL_ACTION_OPEN_PAIRING     (src/bt_hid/ns2_bt_host.c:182)
  -> open_pairing_window(now)                                       (:128)
       -> if a standalone controller is connected:
              btstack_host_disconnect_all_devices()                 (:151)
       -> bt_set_pairing_mode(true)
            -> btstack_host_set_pairing_window_open(true) + start_scan
       -> pairing_until_ms = now + PAIRING_WINDOW_MS                (30 s)
```

`btstack_host_disconnect_all_devices()` walks only `classic_state.connections` and
`hid_state.connections` — the controller tables. It never touches `config_ble.handle`. The
contrast is explicit in the wipe path, which has to terminate the management link *separately* with
a comment noting that wipe-all is "intentionally global", precisely because ordinary disconnects do
not reach it.

**Confidence: Confirmed by source; Hypothesis until observed on hardware** that no radio-level
side effect (inquiry/page-scan contention) degrades the management link in practice. Note the
2026-08-13 result already recorded in `mgmt_access.h`: suppressing the advertiser during discovery
*caused* reconnect starvation, so advertising and controller discovery are known to coexist.

Two consequences for Phase 6:

- **Controller Link will be disconnected** by `open_pairing_window()` when it is the active
  standalone source, because the phone's Classic HID link lives in `classic_state.connections`. This
  makes §72's confirmation prompt mandatory, not optional. BLE management survives.
- **The pairing window is shared.** `mgmt_accept_bonding()` reads the *same* `pairing_window_open`
  flag as controller admission, so a remotely-started controller pairing window would also open a
  30-second window in which a *new phone* could form a management bond. That is a real security
  widening the HLD did not name. Phase 6 must either gate management bonding on the physical gesture
  specifically, or accept and document it. **This should be decided before Phase 6 begins.**

---

## 8. Phase 1 — done 2026-08-27

Implemented in the companion only; no firmware change, by design, so the first slice's regression
surface is entirely JVM-testable.

| Planned | Outcome |
|---|---|
| Versioned multi-record registry replacing the single-adapter store, with migration (§58) | `AdapterRegistry`, `AdapterRegistryCodec` (schema 1), `AdapterRegistryStore`. The legacy `adapter_relationship` preferences file is read once and then left on disk, so a bad migration is recoverable by hand. |
| Set reconciliation over `myAssociations`; `Ambiguous` redefined (F1) | `AdapterRegistryReconciler`. Plurality is normal; `Ambiguous` now means two association records claim one adapter, and no longer forces a phase — see below. |
| Delete the previous-adapter `disassociate()` on verified connect (F2) | Removed, with a comment naming what it caused. The "clear every app-owned association" helper it shared a rationale with is also gone. |
| App-local alias with §22/§49 sanitisation and §8.5 duplicates | `AdapterAlias`, rename dialog, four-character identity suffix on colliding display names. |
| Per-adapter cached state (§85) | Cached firmware/personality/last-connected/repair on `AdapterRecord`. Live `AdapterSnapshot` stays single, because there is one active session and it is already cleared on disconnect. |
| Leave `ManagementOwner` alone (§2.4) | Untouched. |

Two decisions worth recording because they are not the obvious reading of the HLD.

**`Ambiguous` does not block connecting.** The old escalation to `RepairRequired` was correct for
the old model: with one saved relationship and several association records, the app genuinely could
not tell which adapter was its adapter, so it had no address to dial. The registry always holds a
definite address per adapter, so a duplicate association is now stale bookkeeping rather than a
broken pairing. It is reported, Repair tidies it, and connecting is unaffected. Restoring the old
escalation would be a regression.

**Selecting an adapter does not chain into a connect.** `selectAdapter` changes the selection and
retires a session belonging to a different adapter, then stops. A teardown and a connect issued back
to back is exactly the race Phase 2's generation-safe switch coordinator exists to own, and a
half-version of it here would be the hidden lifecycle coupling this subsystem has already been
burned by. The Phase 1 gate does not require switching; Phase 2's does.

Not in scope and deliberately left: a dedicated Adapters page and adapter picker (§46/§47) — the
list lives in Settings for now, and the navigation-reset rules in §86 belong with Phase 2; and
filtering already-registered adapters out of pairing discovery (§11 step 3), which today means
"Pair Adapter" near a registered adapter re-verifies that adapter rather than duplicating it. Safe,
but not the intended UX.

## 8a. Phase 2 — done 2026-08-27

The rule the phase implements, stated once because everything below follows from it:

> A switch from adapter A to adapter B is **one generation-owned transition**. A is retired
> completely before B becomes authoritative. No stale callback from A can update B's UI or
> lifecycle. If B fails, the app ends in a truthful selected-but-disconnected state for B; it must
> not silently fall back to A.

| Planned | Outcome |
|---|---|
| `ActiveAdapterCoordinator` (§84) | New. Owns the active adapter, the switch generation, and the `Settled / Retiring / Activating` phase. |
| Generation-safe switch (§9.2, §2.5) | `AdapterSwitch` executes the ordered transition against an `AdapterSwitchPort`; `AdapterSwitchExecutor` in the ViewModel is the Android half. |
| Disconnect old, connect new | The retirement is **awaited**, including joining any in-flight connect job, before activation starts. |
| Controller Link teardown (§9.3) | `stopControllerLink` exits the on-screen controller and stops the bridge session before management is retired. A live controller session is never carried across to another adapter. |
| Repair state per adapter | Done in Phase 1. |
| Navigation state reset (§86) | A switch closes overlays and drops to the Adapter section, so no screen stays bound to the previous adapter's contents. |

### Two coordinators, not one

`AdapterRelationshipCoordinator` is unchanged and still owns *one attempt at one relationship* —
association, bonding, connect progression, and the generation that makes a stale attempt inert.
`ActiveAdapterCoordinator` owns the layer above: which adapter that coordinator is working on, and
the handover between two of them.

They are deliberately not merged. One generation counter covering both would make a connection
**retry** indistinguishable from a change of **adapter**, which is exactly the confusion the phase
is meant to eliminate.

### How each half of the rule is enforced

**Ordering.** `AdapterSwitch.switchTo` runs `selectionCommitted → stopControllerLink →
retireManagement → retirementComplete → clearAdapterScopedState → beginActivation`, re-checking the
generation after every suspension point. `retireManagement` returns only after
`AdapterRepository.disconnect()` has completed, which itself returns only after the transport has
retired its GATT generation and emitted its final connection state. So by the time B is being
activated, A has nothing left in flight. This is the primary guarantee.

**Attribution.** `ActiveAdapterCoordinator.accepts(address)` gates the ViewModel's connection
collector: nothing is accepted during a retirement; an event with no address (scan and idle-reset
states carry no device) is accepted outside one; otherwise the address must be the active adapter's.
This is defence in depth — it is what survives someone later reordering the switch.

**Outcome guarding.** `activationSucceeded` / `activationFailed` are guarded by **identity**, not
generation, because the connect path is shared with ordinary reconnects that never involved a
switch. A result for A cannot settle B — that is unrepresentable rather than merely unlikely.

**No fallback.** `begin` sets `activeId` to the target immediately and `activationFailed` keeps it
there. The registry's selection follows the coordinator rather than being decided independently, so
the two cannot disagree about which adapter is active. The UI reports "Selected, not connected · tap
to retry" against the adapter the user chose.

### One design bug the tests caught

`begin` originally returned "already active" whenever the target was active and settled, which meant
a **failed** switch was a dead end: the truthful selected-but-disconnected state could not be
retried, because tapping the adapter again was treated as tapping a healthy one. The guard now also
requires `connected`. A retry of the already-selected adapter retires nothing and goes straight to
activation.

### Not in scope

The dedicated Adapters page and picker (§46/§47) remain outstanding; the list is still in Settings.
Simultaneous management sessions remain explicitly out of scope (§5) — one active GATT, as before.

## 8b. Phase 3 — done 2026-08-27

The first phase to change firmware.

| Planned | Outcome |
|---|---|
| Classic iterator (§17) | `gap_link_key_iterator_*`, used for the first time in this project. The iterator hands back the link key itself; it is written to a local that nothing reads, wiped, and has nowhere to go — `mgmt_peer_bond_t` has no field for it. |
| LE DB iterator (§18) | Reuses the existing `btstack_host_le_bond_entry_at`, traversed by capacity because slots are sparse. |
| Role model (§14, F3) | `mgmt_peers_classify`, ordered management → Controller Link → controller → unknown. |
| Live connection merge (§62) | Management client, Classic HID links, BLE HID links, and the `JPLC` reconnect record. |
| Non-secret management API (§44, §45) | `peers list [cursor]`, envelope `v:1`, reusing the `bonds list v2` pagination discipline including its fail-closed "a page must make progress" rule. |
| Saved Pairings UI (§25) | A card on the Controllers page with role filtering and a separate section for this phone and unidentified peers. |

### Peers are a different model from bonds, and that is the point

`bonds` enumerates LE device-DB slots. `peers` enumerates devices: both databases merged by
identity address, so one physical device is one row however many key records it holds. With CTKD
configured and the phone holding both an LE management bond and a Classic Controller Link key, a
bond-shaped list would show the user's phone twice and call it a controller. That is F3 arriving
exactly where the audit predicted it would.

### `unknown` is a feature

Role classification is **live evidence only**. There is no persistent per-peer role store, so after
a reboot a stored bond whose owner has not reconnected is reported `unknown` — and the UI says
"Saved pairing, not yet identified" rather than guessing it into the controller list. The security
databases hold an address, a key and a key type; they hold nothing about what the device is.

This is the honest reading of §60, and it is also what keeps the gate true in the hardest case: a
freshly booted adapter cannot misclassify the management bond as a controller, because it declines
to classify it at all.

Persisting non-secret peer metadata would fix the post-reboot gap and is Phase 4's optional half.

### One bug the tests caught

`mgmt_peers_merge` resolved competing observations of one peer by comparing role enum values
numerically, and the enum's declaration order is the opposite of the precedence order. A phone that
was both the management companion and a connected input source was therefore classified as a
**controller** — precisely the misclassification the phase gate forbids. Precedence is now an
explicit `role_precedence()` function, written down separately so that reordering the enum cannot
silently change the answer.

### Deliberately not done

No forget, no mutation of any kind: `peers` is read-only and has no mutating form in protocol
version 1. Selective forget is Phase 5, and offering the action before the firmware can perform it
atomically would be worse than not offering it. Names are sanitised to printable ASCII rather than
validated as UTF-8, which costs accents in a non-English controller name and buys a guarantee that
no remote name can terminate a JSON string or inject a log line; a real device that needs the
accents is the trigger for a proper validator.

## 8c. Phase 4 — done 2026-08-28

Names, classification and history. Two thirds firmware-cheap and one third deliberately not done.

| Planned | Outcome |
|---|---|
| Classified name capture (§20, §21) | The adapter now reports `class` — the bthid driver identity it derived for a live connection — beside the remote-supplied `name`, plus `vid`/`pid`. `peers_identity_for()` reads the bthid device table on the BTstack thread, which already owns it. |
| Name hierarchy (§20) | `PeerNaming.label`: alias, `class`, `name`, USB identity, four-character address suffix. Never the bare address. |
| Sanitised metadata (§22) | The classification goes through the same sanitiser as a remote name, and app-side history re-sanitises on read. |
| App-side history (§24.1) | `PeerHistoryRecord` / `AdapterPeerHistory` / `PeerHistoryBook` / `PeerHistoryCodec` / `PeerHistoryStore`, keyed `adapterId + peerId`. |
| Connected / Saved / Recent (§25) | `ControllerInventory` builds them; the card is `PairedControllersCard` on the Settings page. Recent's only action is "Remove from history" (§54), which is not and does not read as a forget. |
| Firmware peer metadata (§24.2) | **Deliberately not done.** See below. |
| Forget | Still absent, as the phase gate requires. |

### No active name acquisition, and no inquiry

§21 offers issuing a remote-name request for a stored peer. It is not done, and the reason is that
it does not pay for itself here: the adapter already learns the name of every peer that connects,
history keeps it, and a remote-name request for a device that is switched off returns nothing while
still costing radio time during a management session. The name gap this phase actually had to close
was "the adapter rebooted", and history closes it without touching the radio at all.

### Why the firmware half was not built, and must not be rebuilt casually

§6 concluded storage capacity was sufficient and recommended project TLV tags. That conclusion is
still correct about *capacity*. It is not the whole answer: adapter-side peer metadata was
implemented after this phase's first pass, **destabilised the Bluetooth core, and was withdrawn**.
The end-of-Phase-3 commit records the same fact.

This is worth preserving as negative knowledge because the idea is attractive and will be
rediscovered: it is the clean way to make history adapter-centric so a second management phone
inherits it, and §6 appears to bless it. §24.2 is explicitly conditional — "recommended if storage
audit permits", "app-side history is sufficient for initial release" — and app-side history is the
required half. Anyone returning to this should first establish *what* destabilised the core (TLV
write contention with the security databases on the same banks is the obvious first hypothesis, and
is untested), rather than reimplementing the same shape and hoping.

Consequence to keep in mind: history lives on one phone. A different management phone sees the
adapter's live answers only, which is exactly the Phase 3 behaviour.

### How history avoids lying about the adapter

The protocol says a client MUST render `unknown` as unidentified and MUST NOT promote it to
`controller`. History does not: `PeerListing.role` is always the adapter's live answer, and the
remembered role travels beside it as `rememberedRole` with the UI saying "remembered". The one
decision memory does make on its own is **exclusion** — a peer this app has seen proven to be the
user's own phone stays out of the controller list even when the adapter cannot currently identify
it. That asymmetry is deliberate: being wrong costs a row under "This phone", and being wrong the
other way offers to forget the management relationship.

The other load-bearing rule is that only a **complete** inventory read is recorded. A partial read
is indistinguishable from an adapter that has forgotten a controller, so recording one would move
live saved pairings into Recent and tell the user their controllers had been unpaired.

### UI move

The saved-pairings card left the Controllers page for Settings, where it is now **Paired
controllers**, beside **Paired adapters**. Two cards, not one: removing an adapter is an app-local
operation and forgetting a controller is not, and a single card carrying both invited the user to
read a destructive action against the wrong list. The Controllers page keeps its subject — who is
driving right now.

## 8d. Phase 5 — done 2026-08-28

Selective forget. The prediction held: `btstack_host_forget_device_typed()` was already the atomic
sequence, so the phase was a command surface, a guard, and a verification rather than new Bluetooth
machinery.

| Planned | Outcome |
|---|---|
| Atomic `forget_peer` (§63) | `peers forget <id>` → `peers_forget_run()`. Resolve, guard, disconnect, delete, verify, all inside one run-loop callback. |
| Classic delete (§26) | `gap_drop_link_key_for_bd_addr()` via the address-only form, after the live ACL is dropped. |
| LE delete (§27) | `gap_delete_bonding()` on the stored slot, which also refreshes the controller resolving list. |
| Active connection handling | Existing sequence: cancel in-flight connect, drop BLE links, drop Classic links, then delete. |
| Multi-entry peers (§91) | The **address-only** form deliberately, so one physical device loses every credential it holds. |
| App confirmation (§53) | Consequence, not factory-reset language; the disconnect sentence appears only for a connected controller. |
| Refresh after result (§92) | `AdapterRepository.forgetPeer()` re-reads the inventory even when the forget failed. |
| History retained (§30) | Untouched. A forgotten controller becomes a `Recent` row. |
| Management protected (§27, §28) | Refused with `management_peer`. |

### The address-only form is the whole point

`btstack_host_forget_device_typed()` has a typed form that removes one transport of a
cross-transport peer. Forget uses the **untyped** form on purpose: "forget this controller" means the
device, not one of its credentials. A peer left holding half its keys reconnects on the surviving
transport and looks like the forget silently failed — §91's partial-delete hazard, avoided by never
creating the partial state rather than by reporting it well.

### Verification is a re-enumeration, not an assumption

§63 step 8 asks the operation to confirm the peer is no longer bonded. It does: after the delete it
rebuilds the inventory and looks the id up again, and the reply carries `bonded` and `tr` from that
second read. `incomplete` exists so a partial failure is visible instead of a client cache claiming a
pairing is gone while the adapter still holds one.

### Idempotency is a success, deliberately

§64 requires that forgetting an already-forgotten peer succeed. An unresolvable id returns
`already_absent` with `ok:true`, because a management reply can be lost after the command has already
run and a retry must not report failure for completed work. A **malformed** id is still a usage
error: reported as `already_absent` it would tell the user a controller was removed when nothing was
addressed.

### Two guards, not one

The companion is refused on its durable identity (structural, immune to a classification mistake)
**and** on role (which also catches the same phone in its Controller Link relationship). Getting this
wrong would cut off the app issuing the command, so it is worth the redundancy.

## 8e. Phase 6 — done 2026-08-28

Remote physical-controller pairing, and the product decision Phase 0 said was owed first.

| Planned | Outcome |
|---|---|
| One state machine (§32) | `pairing start` sets a request the existing control tick consumes and calls the SAME `open_pairing_window()` the BOOTSEL gesture calls. No second flow, no duplicated radio behaviour. |
| Bounded window (§34) | Firmware-owned, the established 30 s controller window. |
| State machine (§35) | Mapped onto the existing one: `idle / discovering / connecting / paired / timed_out / cancelled / blocked`. |
| Management API (§36) | `pairing start\|status\|cancel`, deferred-reply shape shared with `peers`/`bonds`. |
| Polling (§37) | Command/response only, so the app polls at 1 s through the existing serialized session. No new GATT surface. |
| Success (§39) | The ordinary pairing path persists the bond; the app re-reads the inventory. Nothing extra is written. |
| Failure codes (§40) | `no_controller`, `management_disabled`, `busy`, `locked_out` — named, not collapsed. |
| Idempotent cancel (§64) | Cancelling when idle succeeds and reports idle. |
| Operation generations (§65) | `op` increments per start; a status for an older one cannot describe the current attempt, and an adapter switch mid-operation drops the client's view. |

### The §7.3 decision: split the two pairing authorities

`mgmt_accept_bonding()` read the same flag as controller pairing, so opening a controller window also
admitted a new management bond. Locally that is defensible — someone was holding the adapter and
pressed the button, and physical presence is the authority. Remotely it is not: a "pair a controller"
request travelling over the air would open a window in which a **different phone** could claim the
management relationship, which is a far larger grant than the user asked for.

So they are split. `btstack_host_set_pairing_window_open()` (the local gesture) grants both;
`btstack_host_set_controller_pairing_window_open()` (remote) grants controller discovery only.
Closing clears both. This is the last of Phase 0's open product decisions.

### Deliberately not done

The window stays at 30 s rather than §34's suggested 60 s default. §32 requires one state machine,
and that machine's duration is what the physical gesture already uses; changing it would alter
behaviour the user relies on in order to satisfy a recommendation about a number. The client is told
the real remaining time rather than a nominal one.

## 8f. What Phase 7 should do

Remote physical-controller pairing. The Phase 0 audit's decisive positive result still stands: opening
the pairing window does not tear down BLE management. The product decision owed before starting is
§7.3's — whether `mgmt_accept_bonding()` sharing `pairing_window_open` with controller pairing is
acceptable, or must be split.

## 9. Remaining unknowns

- Whether any stored bond pair on a real adapter is actually CTKD-linked (§5). First Phase 3
  experiment.
- Whether the public BD_ADDR is stable across reflash as a *tested* property rather than an
  inference (§7.1).
- Whether the shared pairing window's management-bonding side effect is acceptable product
  behaviour, or must be split (§7.3). Product decision, owed before Phase 6.
- Live radio behaviour of management during a remotely-triggered inquiry, on hardware.
- **What actually destabilised the Bluetooth core when adapter-side peer metadata was added**
  (§8c). Untested first hypothesis: TLV write contention with the security databases sharing the
  same flash banks. Nobody should reattempt §24.2 without answering this.
- Whether any real controller produces a name that ASCII-only sanitisation mangles badly enough to
  justify a UTF-8 validator. Unchanged from Phase 3; Phase 4 added a second string (`class`) through
  the same sanitiser, but that one is firmware-controlled today.

## 10. Validation performed

**Phase 0 (2026-08-27):** static audit only, no source changed.

**Phase 1 (2026-08-27):** companion source only; no firmware source changed and no adapter reflash
is required. 1163 Android JVM test executions with 0 failures — app debug 275, app release 275,
`:bridge-core` 568, `:management-core` 45 — of which 32 are new registry/alias/reconciler cases and
4 were replaced (see `AdapterRegistryReconcilerTest`'s header for which invariants carried forward
and why one did not). `lintDebug` and `lintRelease` both 0 errors / 13 warnings, none in the changed
files. Both APKs assembled. `check_android_descriptor_parity.py` green at bridge contract 4 with an
unchanged 161-byte descriptor digest, confirming this pass changes no wire contract.

**Phase 2 (2026-08-27):** companion source only, again. 1203 JVM test executions with 0 failures —
app debug 295, app release 295, `:bridge-core` 568, `:management-core` 45 — the 20 new cases being
`AdapterSwitchTest` and `ActiveAdapterCoordinatorTest`, which pin the ordering property directly:
retirement precedes activation, nothing is accepted from the outgoing adapter while it is retiring,
a result for the abandoned adapter cannot settle the chosen one, A→B→A repeated never activates
twice without a retirement between, and a failed activation leaves the chosen adapter selected with
no fallback. Lint unchanged at 0 errors / 13 warnings with no findings in the changed files; both
APKs assembled; descriptor parity still green at bridge contract 4 with the same 161-byte digest.

The switch is deliberately testable without a radio: `AdapterSwitch` drives an `AdapterSwitchPort`,
so the order of operations is asserted against a recording fake. A test that needed Bluetooth to
prove an ordering property would not be run often enough to protect it.

**Phase 3 (2026-08-27):** the first firmware change of this pass. Both boards build clean with no
new warnings. 73/73 declared active host-test targets rebuilt from source and passed, up from 72;
the new target is `test_mgmt_peers` (20 cases covering role precedence, cross-transport merging,
address-stable ordering, untrusted-name sanitisation, fail-closed pagination, and the structural
absence of key material). 1222 Android JVM test executions with 0 failures — app debug 295, app
release 295, `:bridge-core` 568, `:management-core` 65 — the 20 new Kotlin cases being
`PeerInventoryTest` plus peer vectors added to the shared conformance fixture
`tools/fixtures/management/protocol-v1.json`, so a non-Kotlin client inherits the same guards.
Lint unchanged at 0 errors / 13 warnings with no findings in the changed files; both APKs assembled;
descriptor parity green at bridge contract 4 with the same 161-byte digest, confirming the Controller
Bridge wire is untouched.

**Phase 4 (2026-08-28):** firmware and companion. Both boards rebuild clean from scratch with no new
warnings — the only warnings emitted are the four pre-existing `third_party/opus` ones. 73/73
declared active host-test targets rebuilt from source and passed; `test_mgmt_peers` grew by 5 cases
covering classification carry-through, the rule that a later observation cannot erase a known
classification, absent-rather-than-empty optional fields, sanitisation of the classification string,
and a widest-possible row still fitting a page. 1283 Android JVM test executions with 0 failures — up from 1222; app debug 321,
app release 321, `:bridge-core` 568, `:management-core` 73 — the new cases being `PeerHistoryTest`, `ControllerInventoryTest`, and naming and
classification cases in `PeerInventoryTest`; the shared conformance fixture's peer vectors gained
`class`/`vid`/`pid` so a non-Kotlin client inherits them. Lint unchanged at 0 errors / 13 warnings
with no findings in the changed files; both APKs assembled; descriptor parity green at bridge
contract 4 with the same 161-byte digest.

The history model is testable without a radio and without Android: `AdapterPeerHistory`,
`PeerHistoryCodec` and `ControllerInventory` are plain Kotlin over the management-core domain
types, so the rules that matter — a reboot must not erase a proven role, a partial read must not be
recorded, the phone must never reach the controller list — are pinned by ordinary JVM tests.

**Phase 5 (2026-08-28):** firmware and companion. Both boards rebuild clean from scratch with no new
warnings. 73/73 declared active host-test targets passed; `test_mgmt_peers` grew by five cases
covering id-shape validation, id→peer resolution, the verified-state reply for all four outcomes,
buffer and malformed-id refusals, and the phase gate in miniature — forgetting one of three peers
leaves the other two addressable by the ids the client already holds. 1301 Android JVM test
executions with 0 failures, up from 1293, the new cases pinning that `already_absent` is a success,
that a refusal is nameable, that a partial delete surfaces, that an unknown outcome still yields
`bonded`, and that a reply without the verified state is rejected. The shared conformance fixture
gained a `peersForget` vector, asserted to carry no key material. Lint unchanged at 0 errors, both
APKs assembled, descriptor parity green at bridge contract 4 with the same 161-byte digest.

**A reflash is required for Phase 5**, as for Phases 3 and 4: `peers forget` does not exist in older
firmware, and the client's optional-family probe maps `unknown command` to unsupported.

**A reflash is required for Phase 4**, as for Phase 3: `class`, `vid` and `pid` do not exist in
older firmware. The envelope version deliberately did not move, so a Phase 4 app against a Phase 3
adapter degrades to remote-supplied names rather than failing to read the page.

**A reflash is required for Phase 3**, unlike Phases 1 and 2. The `peers` command does not exist in
older firmware, and the app reports that honestly rather than showing an empty list: an adapter that
cannot answer says "Update the adapter firmware to see its saved pairings here."

No hardware action was taken in any phase (HLD §109). The Phase 1, 2, 3, 4 and 5 gates are hardware
checks the maintainer runs; see §11–§15.

## 11. Phase 1 hardware checklist

Software cannot establish any of this. Each step is one observation.

1. Pair Adapter A. Confirm it appears in Settings → Adapters and is marked Selected/Connected.
2. Without forgetting A in Android Bluetooth settings, tap **Pair another adapter** and pair
   Adapter B.
3. Confirm Android's own Bluetooth settings still list **both** adapters as paired. This is the
   result the whole phase exists for; before this change, connecting to B unregistered A.
4. Confirm Settings → Adapters lists both rows.
5. Rename one to "Living Room" and the other to "Bedroom".
6. Force-stop the app and reopen it. Confirm both rows and both names are still there.
7. Give both adapters the same name. Confirm each row gains a distinct four-character suffix.
8. Remove one adapter from the app. Confirm the other row is untouched, and that Android's Bluetooth
   settings still list the removed adapter as paired.

An upgrade check, if an adapter was already paired with a previous build: install over it and
confirm that adapter is still present and still selected, without re-pairing.

## 12. Phase 2 hardware checklist

1. With A connected, tap B's row. Confirm A disconnects, the strip reads "Switching adapter…" naming
   B, and B then connects on its own.
2. Confirm the firmware, personality and controller shown after the switch are **B's**, with no
   flash of A's values underneath B's name.
3. Switch A → B → A several times in a row. Confirm each ends connected to the adapter tapped, and
   that no Android Forget was needed at any point.
4. Switch quickly, tapping a third adapter while a switch is still in progress. Confirm the app ends
   connected to the last one tapped and not to an intermediate one.
5. Power off B, then switch to it. Confirm B stays selected and reads "Selected, not connected · tap
   to retry" — **and that the app does not reconnect A**. This is the no-fallback rule.
6. Power B back on and tap its row again. Confirm it connects.
7. With Controller Link or the on-screen controller active on A, switch to B. Confirm the controller
   session stops cleanly and is not carried to B, and that BLE management ends up on B.
8. Start a switch from a screen other than Adapter (Keyboard or Amiibo). Confirm the app returns to
   the Adapter section rather than showing the previous adapter's contents.

## 13. Phase 3 hardware checklist

**Requires a reflash.** Phase 3 adds a firmware command.

1. Flash the new firmware. Pair a controller, then open Controllers → Saved pairings and refresh.
   Confirm the controller appears with a readable name and reads "Connected".
2. Compare against the adapter's own view: send `btpeers` over the UART diagnostic link. Confirm the
   ids, roles and connection states match what the app shows. **This is the phase gate** — the two
   renderings come from one computation, so a disagreement means the app is not reading what the
   adapter reports.
3. Confirm **this phone does not appear under saved controllers.** It should be under "This phone and
   unidentified peers", labelled management — and, if Controller Link has run, as one row on
   Classic + LE rather than two rows.
4. Power the controller off. Refresh. Confirm it still appears, now reading "Saved" rather than
   "Connected". Bonded and connected are different things.
5. Power-cycle the adapter and reconnect management without reconnecting the controller. Confirm the
   controller is still listed, and that peers the adapter has not seen since boot read "Saved
   pairing, not yet identified" rather than being presented as controllers.
6. Pair three or more controllers so the inventory needs more than one page. Confirm every one
   appears exactly once and none is missing.
7. Point the app at an adapter running older firmware. Confirm it says the firmware needs updating
   rather than showing an empty list.
8. Optional, if a controller with a non-ASCII name is available: confirm the name renders with
   replaced characters rather than breaking the row. That is the documented cost of ASCII-only
   sanitisation, and seeing it in practice is what would justify a real UTF-8 validator.

## 14. Phase 4 hardware checklist

**Requires a reflash.** Phase 4 adds fields to a firmware reply.

1. Flash the new firmware. Connect a controller the project has a driver for (DualSense, Switch Pro,
   Xbox). Open Settings → Paired controllers. Confirm the controller appears under **Connected**
   with its **driver identity** as the name — "Sony DualSense", not "Wireless Controller". **This is
   the phase gate for naming.**
2. Compare against the adapter: `btpeers` over the UART diagnostic link now carries `class` and
   `vid`/`pid` for the same peer. Confirm they match what the app shows.
3. Power the controller off and refresh. Confirm it moves to **Saved pairings**, keeps its name, and
   the row reads "Last connected" with a plausible time.
4. **Power-cycle the adapter** and reconnect management without reconnecting the controller. Confirm
   the controller still shows its name, the row says the identification is *remembered*, and it is
   still not called a live controller. This is the gap history exists to close; before Phase 4 this
   row read "Saved pairing, not yet identified" with no name.
5. Confirm **this phone is still not under saved controllers** after that power cycle — it is the
   case where the adapter cannot identify anything, so it is the case where exclusion has to come
   from memory.
6. Connect a controller with no matching driver (a generic BT gamepad). Confirm it falls back to its
   Bluetooth name, and if it has none, to `Controller • XXXX` rather than to a raw address.
7. Forget the controller from the adapter (physical/`bonds remove`), then refresh. Confirm it moves
   to **Recent**, reads "Not paired", and that its only action is **Remove from history**. Use it and
   confirm the row disappears and nothing on the adapter changed (`btpeers` unchanged).
8. Restart the app. Confirm Recent and the remembered names survive.
9. With two adapters registered, confirm history is per adapter: switching adapters must show the
   second adapter's controllers, not the first's. Remove one adapter from the app and confirm the
   other's history is untouched.
10. Point the app at an adapter running Phase 3 firmware. Confirm the list still renders — the
    envelope version did not move, so the only difference should be that names fall back to the
    remote-supplied ones.

## 15. Phase 5 hardware checklist

**Requires a reflash.** Phase 5 adds a mutating firmware command.

1. Flash, pair the phone, then pair **three** controllers. Confirm all three appear under Settings →
   Paired controllers with readable names.
2. Forget one that is **not** connected. Confirm the dialog says it will need pairing again and does
   **not** mention disconnecting. Confirm it disappears from Saved pairings and reappears under
   **Recent** — history is kept on purpose.
3. **The phase gate:** confirm the other two still connect normally, and that the forgotten one is
   refused until it is paired again. Compare against the adapter with `btpeers` over UART: it must
   show two peers, not three.
4. Forget a controller that is **connected**. Confirm the dialog adds "This will disconnect it now",
   that it does disconnect, and that the row moves to Recent.
5. **Confirm management survives every forget** — the app must stay connected throughout. This is the
   half that would be most damaging to get wrong.
6. Attempt to forget this phone. It is not offered in Paired controllers at all; if it can be reached
   any other way the adapter must refuse with the "this is this phone's own connection" message and
   the session must stay up.
7. Forget the same controller twice (tap Forget, then re-issue after a reconnect). The second attempt
   must report that it was already not paired, **not** an error.
8. Power-cycle the adapter. Confirm the forgotten controllers are still absent and the kept ones still
   reconnect — the deletion must be persistent, not just in RAM.
9. Point the app at an adapter running Phase 4 firmware. Confirm the Forget action reports the
   firmware does not support it rather than appearing to succeed.
