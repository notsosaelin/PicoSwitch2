# Bluetooth persistence and reset contract

Status: exact source and linker/build-layout audit on 2026-08-20. Logical deletion and builds are
source-tested; physical flash/reboot observations for this correction are pending.

## Physical flash map

Flash sectors are 4 KiB. The production SDK supplies two alternating BTstack TLV banks. RP2350 A2
reserves the final sector, so its BTstack base is one sector lower than RP2040.

| Purpose | Pico W, 2 MiB | Pico 2 W, 4 MiB |
|---|---:|---:|
| Virtual Amiibo bank 0 | `0x1FA000` | `0x3FA000` |
| Virtual Amiibo bank 1 | `0x1FB000` | `0x3FB000` |
| PicoSwitch2 configuration | `0x1FC000` | `0x3FC000` |
| Unused by these stores / BTstack TLV A | `0x1FD000` | `0x3FD000` |
| BTstack TLV A / TLV B | `0x1FE000` | `0x3FE000` |
| BTstack TLV B / SDK-reserved final sector | `0x1FF000` | `0x3FF000` |

The table's slash-separated row reflects the board difference: Pico W TLV occupies the final two
sectors; Pico 2 W TLV occupies sectors `SIZE-3S` and `SIZE-2S`.

## Logical stores in the BTstack TLV banks

The Pico SDK `btstack_cyw43_init()` installs one TLV instance, then attaches both databases:

| Tag family | Owner | Contents | Capacity |
|---|---|---|---:|
| `BTL0`…`BTL15` | BTstack Classic link-key DB | address, link key, key type, sequence | 16 |
| `BTD0`…`BTD15` | BTstack LE device DB | identity/type, IRK, LTK, EDIV/RAND, security flags | 16 |
| `JPLC` | PicoSwitch2 | preferred BLE address/type/name, VID/PID, optional Switch 2 LTK | 1 |
| `JPLK` | PicoSwitch2 | post-wipe/install admission lock byte | 1 |

`JPLC` is a reconnect hint plus Switch 2 custom-key record; it is not a substitute for the BTstack
bond database. Global wipe and forget-one for the matching identity MUST clear the whole record and
all in-memory copies. A zero-address record with residual key bytes MUST NOT be written.

The LE backend permits sparse slots. `le_device_db_count()` is a count, not a traversal bound.
Enumeration and deletion MUST visit `0..le_device_db_max_count()-1` and skip empty slots.

## Public deletion semantics

PicoSwitch2 uses:

- `gap_drop_link_key_for_bd_addr()` / `gap_delete_all_link_keys()` for Classic trust;
- `gap_delete_bonding(address_type, address)` for LE trust.

Direct `le_device_db_remove()` calls are not the product deletion boundary. In the pinned SM,
`gap_delete_bonding()` removes the database entry and reloads the controller resolving list when LE
privacy resolution is enabled. Wipe and forget MUST preserve those side effects.

## Operation matrix

| Operation | Settings | Amiibo | Classic keys | LE bonds | `JPLC` / Switch 2 key | `JPLK` | Admission afterward |
|---|---|---|---|---|---|---|---|
| Ordinary reboot | preserve | preserve | preserve | preserve | preserve | preserve | existing policy |
| Disconnect all | preserve | preserve | preserve | preserve | target remains | preserve | no new trust without window |
| Forget one LE identity | preserve | preserve | preserve | delete matching typed entry | clear if matching | preserve | closed unless window open |
| Triple-tap wipe all | preserve | preserve | delete all | delete all, including management | delete | store locked | closed |
| New UF2 first boot | reset | reset both banks | erase | erase | erase, then absent | recreate locked | closed |
| Reboot of same installed image | preserve | preserve | preserve | preserve | preserve | preserve | existing policy |
| Full-chip erase | erase | erase | erase | erase | erase | erase | closed by default fresh-admission policy |

The management `bonds remove <index>` surface is LE-only. Classic per-device deletion is reached
through `peers forget <id>`, which is deliberately address-only and removes every credential one
physical device holds — LE bond and Classic link key together — because "forget this controller"
does not mean "forget half of it". Triple-tap remains the user-facing global operation.

## Classic link keys are committed on proof, not on notification

A Classic Link Key Notification is not proof of anything: BTstack stores the key from that event
before the application sees it, so an unadmitted replacement would silently overwrite a good
credential. The firmware therefore undoes that store, parks the key, and commits it only once the
pairing behind it is known to have succeeded.

**Two events prove that, and neither is guaranteed to arrive.**

| Proof | When it arrives | Who drives the pairing |
|---|---|---|
| `HCI_Authentication_Complete` | only in response to this host's own `HCI_Authentication_Requested` | this adapter |
| `HCI_Encryption_Change`, encryption enabled | whenever the link becomes encrypted | either end |

This firmware requests authentication only for the Wiimote family and for the one Classic device
that reports the name `Xbox Wireless Controller`; BTstack's HID Host registers its L2CAP services at
`LEVEL_0`, so it never asks either. **Every other Classic controller drives SSP itself**, and its
authentication reaches this host as Link Key Notification plus Encryption Change with no
Authentication Complete at all.

Waiting only for the local event is why a DualSense paired, worked, and was never durable: the key
was parked, BTstack's copy had already been deleted, and HID open discarded the parked one. The
peer inventory had no BR/EDR bond to report, the app showed *completing pairing* indefinitely, and
the next connection was an unknown Classic device that `ns2_bt_admission_decide()` correctly refused
with the pairing window closed. Reopening the window re-ran SSP and produced the same
non-durable session.

Encryption enabled is exactly as conclusive as the local event: a Classic link cannot be encrypted
except with a link key both ends hold and have authenticated against. It is now accepted as proof
(`ns2_bt_classic_authentication_proven()`).

**This did not relax admission.** The key being proven was already admitted by
`ns2_bt_classic_key_update_admitted()` — a fresh pairing window, or existing trust plus an identical
or authenticated-changed key. Proof answers only *did that pairing succeed*.

The parked security state also outlives `pending_valid`. HID channels are registered at `LEVEL_0`
and can open before or without encryption, so clearing the parked key at HID open threw away a
pairing that was one event short of durable. It is released when the peer's ACL is gone.

**Rule: a Classic controller is remembered when its link key is committed. Admission decides
whether a key may be written; proof decides whether it is. Do not conflate them, and do not treat
the locally observed Authentication Complete as a requirement — for most controllers it never
happens.**

## Peer roles

A bond entry is one security record for one transport. A **peer** is one physical device, and may
own several entries — with `ENABLE_CROSS_TRANSPORT_KEY_DERIVATION` configured, the management phone
normally owns two. `peers list` (see [management/PROTOCOL.md](../management/PROTOCOL.md)) reports
peers, merged by identity address, read-only, and without key material of any kind.

| Role | Evidence that establishes it | Persistent? |
|---|---|---|
| `management` | Connected as the BLE peripheral's management client (`config_ble.client_addr`) | No |
| `controller_link` | A connected Classic HID peer the arbiter classified as the Android bridge from its HID descriptor | No |
| `controller` | A connected controller source, or a match against the `JPLC` reconnect record | Partly — `JPLC` survives reboot |
| `unknown` | Everything else | n/a |

The roles are ordered: `management` wins over `controller_link`, which wins over `controller`. One
phone can hold both relationships simultaneously, and reporting it as a controller would put the
user's own phone in a list of things to forget.

**Role classification is live evidence only.** There is no persistent per-peer role store, so after
a reboot a stored bond whose owner has not yet reconnected is `unknown`. That is the correct answer,
not a gap to be papered over with a guess: the security databases record an address, a key and a key
type, and nothing about what the device is.

What carries a controller's identity across that gap is the **companion's per-adapter peer history**
(Phase 4), not the adapter. The app remembers the last name and classification it was told for each
peer id and shows them for a peer that is bonded but offline. That is why the adapter must never
publish a provisional classification as though it were an answer: the app has no way to tell a
first guess from a settled one, and it keeps whatever it was given until something better arrives.
While Classic identity is still resolving — the generic HID-descriptor driver is bound and the PnP
SDP query has not returned — the classification is omitted entirely
(`mgmt_peers_classification_publishable()`), which the app reads as "not yet" rather than "generic".

Since Phase 4 the adapter also reports, for a **connected** peer only, the bthid driver identity it
derived (`class`) and the resolved `vid`/`pid`. Those are live too, and are absent for a peer that is
merely bonded.

### Management identity is the resolved identity, never the connection address

`config_ble.client_addr` is the over-the-air address from
`HCI_SUBEVENT_LE_CONNECTION_COMPLETE`. For a phone using LE privacy that is a Resolvable Private
Address which rotates roughly every quarter hour, and it is **never** equal to the identity the LE
device DB stores or to the phone's Classic BD_ADDR.

Durable comparisons therefore use `config_ble_durable_addr()`, which prefers
`config_ble_identity_addr()` (the record `sm_le_device_index()` says this link is authenticated
against), then the identity captured from `SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED`, and only then the
connection address — which is correct in that last case precisely because no resolution happened, so
the peer connected under its identity. BTstack does the resolution
(`ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION`); nothing here re-derives it.

Three comparisons were previously made against the raw connection address and so were permanently
false: the reconnect selector could not exclude the connected companion, the Classic authentication
path could never recognise the companion's own Controller Link, and an explicit forget of the
companion's bond left its session running. `btstack_host_companion_terms()` was already correct — it
tests `raw_matches || identity_matches`.

The same mistake produced the peer inventory's most visible symptom: one phone appeared as two
logical peers, its bonded identity (Classic + LE, role `unknown`) and an unbonded RPA (role
`management`). Peers merge by address, so the observation is now emitted under the durable identity
and the two collapse into one row.

**Rule: the live RPA is connection-local metadata. Anything durable — a bond record, a Classic
address, a peer row, history — uses the resolved identity.**

### Automatic recovery may never delete a durable bond

**Destroying persistent pairing state is an explicit user action, never an error handler's idea.**

Four sites used to delete a bond automatically when authentication failed, each with locally
reasonable logic and no view of the others:

| Site | Trigger | Was |
|---|---|---|
| LE disconnect handler | reason 0x05 / 0x06 | deleted the dropped peer's bond |
| LE `SM_EVENT_REENCRYPTION_COMPLETE` | any failure status | deleted the peer's bond, then re-paired if the window was open |
| Switch 2 direct-HCI re-encryption | any failure status | deleted the custom bond and forced fresh pairing |
| Classic authentication complete | 0x06 only | dropped the durable link key |

Together they can empty the database. Observed 2026-08-28: an adapter holding three bonds reported
`btbonds: []` in the same powered session, with no reflash and no power cycle. The trigger was never
proven and did not need to be — the response was wrong regardless.

All four now preserve the credential and drop only the link. A genuinely stale bond therefore fails
the same way every attempt: bounded, visible, and recoverable by an explicit act. The reconnect
cascade is already bounded and already declines to chase a peer that left deliberately, so nothing
loops.

Note the Switch 2 pair specifically: the SM path already preserved the custom bond ("an SM failure
can be a local state/timing error") while the direct-HCI path deleted it. Which half of the same
recovery ran decided whether the pairing survived. They agree now.

Destructive paths that remain, all explicitly user-driven: Phase 5 `peers forget`, `bonds remove`,
the BOOTSEL triple-tap wipe, the `btfresh` diagnostic, the MouthPad clear-bond command, and the
first-boot install reset.

`ns2_bt_classic_auth_failure_forgets_existing()` states the rule once and is pinned across all 256
status values. Reintroducing automatic deletion has to come past it.

### The TLV store is transaction-safe; one interrupted write cannot empty it

Worth recording, because "every bond vanished" invites the theory that a half-finished flash write
corrupted the database. It cannot, and that matters: it is what rules out storage corruption and
points at deletion-by-code instead.

BTstack's `btstack_tlv_flash_bank` is A/B with an epoch. Deleting one tag writes zeros into that
entry's delete field **in place** — no bank rewrite. Compaction erases the *other* bank, copies the
live entries, and writes the new bank's header **last**. The header is the commit marker: until it
exists the new bank fails its magic check and `get_latest_bank()` keeps selecting the old one. An
interruption at any point leaves the previous bank intact and selected.

The project's runtime `btstack_erase_flash_banks()` is compiled out entirely on CYW43
(`#if !defined(BTSTACK_USE_CYW43)`), so no runtime mass-erase path exists on this hardware either.

### Flash writes park the other core, so error paths should not write flash

`flash_safe_execute()` is called from core 1 (the BTstack worker) by every TLV mutation, and parks
core 0 through `multicore_lockout_victim_init()`, registered in `usb.c` inside the TinyUSB loop —
where an existing in-tree note already records that core 0 "cannot grant a lockout promptly from
this tight TinyUSB loop while streaming". `pico_flash_bank` passes `UINT32_MAX` as the enter/exit
timeout, so neither core gives up.

No lock-order defect was proven: core 0 waits on the async-context mutex with interrupts enabled, so
the lockout IRQ can still park it, and the sequence resolves. But the exposure is real and scales
with how often error handling writes flash. Removing automatic bond deletion removes that class of
write almost entirely — normal authentication and reconnect failures are now RAM and state-machine
operations. That is the containment actually available without redesigning persistence.

### Controller bond-recovery must never reach the companion

`SM_EVENT_REENCRYPTION_COMPLETE` with a failure status deletes the peer's bond so the next attempt
becomes a real pairing. That is correct **for controllers**: a controller whose stored key no longer
works has usually been re-paired elsewhere.

It is wrong for the **management companion**, and it reached it by omission. The management
peripheral is a *peripheral-role* link and is deliberately absent from the central-role connection
table (the invariant `btstack_host_pick_reconnect()` also relies on), so
`find_connection_by_handle()` returns NULL for it — and NULL fell through to "an ordinary controller
with a stale bond".

Reaching that branch is **unrecoverable**, which is what makes it worth guarding regardless of how
often it fires: the companion excludes HCI 0x05/0x06 from its retry set by design and goes straight
to a terminal repair state, so one transient security failure would cost a manual re-pair.

**Invariant: no automatic recovery path may delete the management companion's bond.** Clearing it is
always deliberate — Repair pairing, `bonds remove`, or the BOOTSEL triple-tap wipe. When adding any
future automatic `gap_delete_bonding()`, check the handle against `config_ble.handle` first;
`find_connection_by_handle() == NULL` does **not** mean "unknown controller".

### Persisting peer metadata was tried, and withdrawn

> **Do not reimplement this without new evidence.** Storing non-secret peer metadata on the adapter
> — a project-owned TLV tag beside `JPLC`/`JPLK`, advisory rather than authoritative, erased by the
> install reset exactly as bonds are — is the obvious way to make role and name survive a reboot,
> and it is what the design document recommends (`BT_MANAGEMENT_UPGRADE_PASS.md` §24.2). It was
> implemented after Phase 4's first pass and it **destabilised the Bluetooth core**. It was
> withdrawn, and Phase 4 shipped with the companion remembering peer metadata instead.
>
> The storage audit's capacity finding still stands; capacity was never what failed. What actually
> destabilised the core has **not** been established. The untested first hypothesis is TLV write
> contention with the security databases, which share the same flash banks. Establish the cause
> before rebuilding the same shape.

The consequence to accept meanwhile: peer names and roles that survive a reboot live on the
management phone, so a different phone sees only the adapter's live answers. See
[`../android-companion.md`](../android-companion.md).

### Known limitation

Merging is by identity address. A dual-mode device whose LE identity address differs from its
Classic BD_ADDR appears as two `unknown` peers rather than one. That is incomplete rather than
wrong, and is preferred over asserting an association the adapter cannot demonstrate.

## Install-reset marker

Every UF2 carries a dedicated pending marker page in the application image. On the first boot after
that UF2 is written, core 0 runs before USB, CYW43 and core 1:

1. Validate marker placement below the reserved persistence boundary.
2. Erase the complete six-sector region (`SIZE-6S` through end of flash).
3. Program the marker page from its pending magic to zero.
4. Load default settings and an empty Amiibo store.
5. Initialize BTstack/TLV on core 1.
6. Recreate `JPLK=1` before enabling discovery or Classic connectability.

Erase precedes marker consumption. Power loss before consumption repeats the erase safely. Power
loss after consumption but before `JPLK` recreation still leaves no trust material; the independent
fresh-admission rule rejects new trust while no explicit window is open.

The previous implementation erased only five sectors while the verifier and reserved layout began
at `SIZE-6S`. That left Amiibo bank 0 outside the reset despite a log claiming all slots were reset.
The 2026-08-20 correction makes the runtime erase match the six-sector contract.

## Firmware flashing terminology

“UF2 flashed” is not enough evidence to identify what happened. For the release UF2 path above, a
newly written marker deliberately resets persistent state once. A debug/programmer operation that
does not rewrite the marker page may preserve state. A full-chip erase removes everything. Test
reports MUST name the exact flashing method and record bond state before the remote is powered on.

## Security and observability

Diagnostics MAY expose slot count, address/type, reconnect phase and security outcome. They MUST NOT
expose Classic keys, LTKs, IRKs, Switch 2 key components, PIN bytes, or raw TLV records containing
secrets.
