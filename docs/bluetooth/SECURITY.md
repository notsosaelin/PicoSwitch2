# Bluetooth security contract

Status: source-confirmed against Pico SDK 2.2.0 and pinned BTstack revision
`501e6d2b86e6c92bfb9c390bcf55709938e25ac1` on 2026-08-20. The controller-family
runtime matrix remains pending.

The BLE management row and the "LE admission and privacy" facts were re-read
against Pico SDK 2.3.0 / BTstack 1.8.2 source on 2026-11-10. The other rows
have not been re-confirmed since the SDK migration.

This document separates admission, authentication, encryption and durable trust. A raw radio
connection is not controller readiness, and a globally bondable stack is not permission to create
new trust. PicoSwitch2 grants fresh trust only through an explicit pairing window and independently
checks that authority at the protocol boundary that can persist or activate the relationship.

## Security matrix

| Path | Fresh admission | Authentication and encryption | Durable trust | MITM / key size |
|---|---|---|---|---|
| Classic SSP HID | Explicit window latches one admitted attempt; discoverability closes with the window, while that matching in-flight SSP prompt may finish | Dedicated bonding with NoInputNoOutput; global auto-accept is disabled and application responses require the attempt latch; applicable connection paths request `LEVEL_2` | A candidate link key is held in RAM and committed only after matching successful authentication | Just Works, no MITM. Classic link keys are 16 bytes; negotiated encryption is not currently exposed in `btstate` |
| Classic legacy PIN / direct L2CAP | Explicit window for a new key; stored key permits reconnect | PIN handling and selected direct-L2CAP paths request `LEVEL_2`; HID service registration remains `LEVEL_0` for DS3 compatibility | Same deferred link-key commit rule as SSP | Legacy device-specific PIN, not a general MITM guarantee. Classic link key is 16 bytes |
| Standard LE HOGP | Per-connection fresh latch for Just Works, or existing SM identity for reconnect | Bonding and LE Secure Connections requested; NoInputNoOutput; stale re-encryption cannot silently become fresh pairing | BTstack LE device DB | No MITM. Negotiated key-size range is 7-16 bytes. Secure Connections is preferred, not strict-only |
| Switch 2 custom ATT, fresh | Explicit window or the bounded UART-only `btfresh` laboratory action | Initial custom exchange may be unencrypted; the public protocol derives the 16-byte LTK before readiness | Normalized LTK in BTstack LE DB plus `JPLC` reconnect metadata | No independent MITM proof. Final ready transition requires the fresh latch |
| Switch 2 custom ATT, reconnect | Exact saved identity or a resolvable-address candidate may reach SM; neither grants fresh trust | Existing 16-byte bond must encrypt successfully before custom reconnect readiness | Existing BTstack bond and `JPLC`; failed reconnect preserves material for bounded retry | No new trust is created. An RPA candidate is verified cryptographically by SM |
| BLE management | New Just Works bond only in the explicit window, and only with LE Secure Connections (since 2026-11-10; hardware validation pending) | ATT writes require encryption; session authorization also requires a durable bond and a 16-byte encryption key | Shared BTstack LE device DB, never `JPLC` | No MITM because the endpoint is NoInputNoOutput. A legacy pairing offer is declined for a new bond; an existing bond is not yet checked for it |

The matrix states current source contracts. Hardware confirmation still requires the controller
family cases in [`VALIDATION.md`](VALIDATION.md).

## Classic link-key transition

Pinned BTstack stores a `LINK_KEY_NOTIFICATION` internally before it emits that event to the
application. PicoSwitch2 therefore interposes on the default write: it restores an existing key or
removes an unadmitted new entry, keeps the candidate only in RAM, and commits it after the matching
address/handle receives successful `AUTHENTICATION_COMPLETE`.

Outside a fresh window, only an identical existing key or a `CHANGED_COMBINATION_KEY` transition is
eligible. An unrelated replacement key is rejected and the old key is restored. Generic
authentication failure preserves existing trust; only `PIN_OR_KEY_MISSING` removes the affected
stale key. These rules prevent a failed or unauthorized attempt from replacing working trust.

The HID Host service is registered at `LEVEL_0` because DS3 does not support SSP. This is a
compatibility setting, not evidence that every live Classic link is unencrypted: direct-L2CAP,
stored-key reconnect and selected profile paths request `LEVEL_2`. Conversely, HID readiness alone
does not prove the negotiated security level. Physical validation must record the controller and
the observed link behavior rather than infer it from service registration.

## LE admission and privacy

The shared LE device DB contains controller and management identities. An exact identity with
usable trust may reconnect while the window is closed. A resolvable private address cannot be
matched by raw address before connection, so a bounded RPA candidate may reach SM when the DB is
non-empty. Its fresh-admission latch remains false. Successful identity resolution and encryption
can complete a reconnect; a Just Works request or custom-ready transition without valid existing
security is rejected.

`SM_AUTHREQ_SECURE_CONNECTION` is set with bonding, but the pinned stack permits legacy fallback;
PicoSwitch2 does not call `sm_set_secure_connections_only_mode(true)`. The configured LE encryption
key-size range is 7 through 16 bytes. Management deliberately requires exactly 16 bytes in addition
to bond presence, while ordinary HOGP accepts the stack-negotiated range.

## Management pairing method

Management is the one link that carries authority over the adapter: it can read
and replace the amiibo, forget controllers, open the controller pairing window,
feed controller input and receive console audio. Until 2026-11-10 it inherited
the stack-wide policy of requesting Secure Connections and accepting legacy as a
fallback. With LE legacy Just Works, a captured pairing exchange is enough to
recover the long-term key offline.

A new management bond is now accepted only when the Just Works request reports
Secure Connections (`mgmt_accept_new_bond_pairing()`, rule 3c in
`mgmt_access.h`; applied in the `SM_EVENT_JUST_WORKS_REQUEST` handler). A legacy
offer is declined and counted as `cble.legacy_declined` in `btstate`. Controller
pairing is unchanged.

Status: source and host-test validated (`test_mgmt_access`). Hardware validation
is pending: STATUS.md gate 14.

Known limits:

- **Existing bonds are not re-checked.** `mgmt_link_is_trusted()` still accepts
  any bonded 16-byte link. `gap_secure_connection()` exposes the stored flag,
  but it is not yet established how BTstack 1.8.2 marks an LE key derived from a
  Classic bond (CTKD), which is how the Android companion may hold its key.
  Requiring the flag before that is known could lock a working companion out.
- **Just Works has no MITM protection.** An attacker active during the pairing
  window can still be the peer that bonds. The window needs the physical
  gesture and admits one attempt.
- **Bench commands are not reachable over BLE.** `amiibo reader ...` and
  `caudio lock ...` are refused by `config_wireless_command_allowed()` although
  their families are allowed.

## Reset and recovery

An HCI transition away from `HCI_STATE_WORKING` retires live ready-source generations, clears raw
connection slots, listeners and transient admission/reconnect state, and leaves durable Classic/LE
bonds and `JPLC` intact. This prevents stale raw state from impersonating a connected controller
after radio recovery without turning a controller reset into a trust wipe.

Global bondability remains enabled because the stack needs to support both Classic and LE bonding.
It is a mechanism, not the policy authority. Attempt-scoped SSP responses, LE Just Works checks,
custom final-admission checks, key-transition interposition and the post-wipe lock enforce policy.

The install-reset fact is consumed once on the first HCI working transition of that Pico boot. It
can create the initial empty-store lock before discovery, but a later HCI restart cannot replay the
sticky fact and undo a user's explicit pairing unlock.

Wipe is stronger than input neutralization or project-slot cleanup. After closing admission and
radio discovery/page scan, it terminates the pinned stack's complete HCI connection inventory,
including raw links that have not reached a project HID slot. This boundary was added after
physical testing showed that slot-only teardown could stop input while a controller still appeared
connected. The strict Xbox Elite Series 2 retest of `c6d53e7` confirmed that the corrected wipe
forces the peer disconnected and prevents another controller session until pairing is reopened;
that result does not generalize to every controller family.

## Diagnostic boundary

Ordinary diagnostics may expose identities, address types, key-presence booleans, negotiated key
size, phases, statuses and counters. They MUST NOT expose Classic link keys, LTKs, IRKs, PIN bytes,
Switch 2 derived key material or raw TLV records containing them. See
[`DIAGNOSTICS.md`](DIAGNOSTICS.md).
