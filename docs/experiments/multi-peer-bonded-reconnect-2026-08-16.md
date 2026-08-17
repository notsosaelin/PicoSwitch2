# Multi-peer bonded BLE reconnect failure

Date: 2026-08-16
Status: **Resolved — hardware validated 2026-08-16**
Branch: `ns2-testing`

## Outcome

Hardware: ASUS ROG FALCHION RX keyboard + ROG KERIS II ACE mouse, Pico W.
No re-pairing at any point in the sequence.

| Step | Result |
|---|---|
| Both fresh | `keyboard=true mouse=true`, `keyboardConn=4 mouseConn=5`, one group, `ble_conns=2`, `scan_active=false` |
| Mouse OFF | keyboard survives and stays usable; mouse role clears; `roleLosses` increments; no false absorption |
| Both OFF | `group=0`, `source=0`, `ble_conns=0`, `scan_active=true`, `inquiry_active=true` |
| Mouse ON alone | `mouse=true`, `mouseConn=4`, `ble_conns=1`, **`scan_active=true`** — the previously broken state |
| Keyboard ON second | joins automatically; `keyboard=true mouse=true`, `keyboardConn=5 mouseConn=4`, `ble_conns=2`, discovery retires |

Confirmed lifecycle: first bonded role reconnects → partial source → discovery stays available →
second role joins → complete source → discovery retires. Single-peer bonded reconnect was separately
confirmed for the keyboard alone and the mouse alone.

## Question

Why does a previously working bonded BLE keyboard or mouse fail to automatically
reconnect after being power-cycled, while its partner peer remains connected?

## Background

Keyboard + Mouse is a composite logical source: two Bluetooth peers joined under
one `group_id`, owning one console seat. Hardware-confirmed working: both peers
pair and coexist, either order; AUTO mode; role assignment; independent role loss
with partial-source survival in both directions. The remaining failure was
strictly automatic bonded reconnect after a peripheral power cycle, in **both**
directions, recoverable only by full manual re-pairing.

## Root cause 2 — "first peer wins" — Confirmed (code + hardware)

Hardware established that single-peer bonded reconnect works for the keyboard
alone and the mouse alone. With both bonded and powered, **whichever peer
connects first is the only one that connects**, symmetric in both directions, and
the waiting peripheral is only noticed if it is physically power-cycled *after*
the first peer drops.

The mechanism, traced end to end:

1. A BLE HID peer reaches ready and the host calls `btstack_host_stop_scan()`
   unconditionally — the legacy 1-dongle-1-controller rule. This also sets
   `hid_state.state = BLE_STATE_IDLE` and `scan_active = false`.
2. The only path that would restore discovery is the idle safety-net in
   `btstack_host_process()`. Its final term is
   `btstack_classic_get_connection_count() == 0`.
3. **That helper counts BLE links too, despite its name** — it walks
   `classic_state.connections[]` *and* `hid_state.connections[]`
   (`btstack_host.c`). With one BLE peer connected it returns 1.
4. The safety-net predicate therefore short-circuits and never reaches
   `btstack_host_start_scan()`.

This is confirmed rather than inferred by the counters: `scan starts == stops`
(5/5 and 6/6) and `suppress.other == 1` for the whole session. Had the safety net
been calling `start_scan()` and being rejected by one of its gates, the matching
suppression counter would have climbed on every 30 ms tick. It did not — so the
call never happened. `suppress.other` was a red herring; it aggregates
LOCKOUT + APP_SUPPRESS + NOT_POWERED + ALREADY and reflects one unrelated event.

Two hypotheses were tested and **rejected** on the way, and are recorded so they
are not revisited:

- *`reconnect_attempt_time` left stale by a successful connect.* Rejected: it is
  cleared unconditionally on the LE connection-complete success path.
- *`scan_suppressed` holding discovery off.* Rejected: `btstack_host_suppress_scan()`
  has no callers anywhere in the tree, so the flag is always false.

### Fix

The policy question is not "are there zero Bluetooth connections?" but "should
discovery be available?" — and `ns2_bt_host.c` already answers it. Its
`logical_source_complete()` correctly reported **false** in both failing hardware
snapshots; nothing acted on that answer.

- The completeness rule moved to a pure, host-tested function,
  `ns2_kbm_logical_source_complete()`. It is deliberately **not** keyed off the
  effective mode: under AUTO that mode is derived from whichever roles are
  already filled, so it would report "complete" the moment one peer arrived.
- `ns2_bt_host.c` now acts on both branches. Complete → `btstack_host_idle_scan_if_connected()`
  (unchanged). Incomplete → `btstack_host_scan_for_additional_peer()`, which
  positively re-arms discovery while another peer is connected, skipping an
  in-flight connect and not spamming the ALREADY counter.

`btstack_classic_get_connection_count()` was **not** changed. Its "all controller
links" semantics is depended on by the BLE disconnect resume-scan path and by the
transport's connection count (LED state), so altering it would have changed
unrelated behaviour. The safety-net term is left intact and documented instead;
widening it would have made a single connected controller resume discovery and
undone the retired multi-controller scanning.

No HID peer roster was introduced. An earlier attempt at persistent "known HID
peer" provenance was reverted in full: "known HID peer" is not the same as
"expected member of the current logical source", and a historical roster would
have made every previously bonded gamepad an expected peer.

## Root cause 1 — reconnect targeting — Confirmed (code)

`hid_state.last_connected_*` is a **single** reconnect identity, a one-peer-era
abstraction. Three sites in `src/bt_hid/bt/btstack/btstack_host.c` targeted it
unconditionally:

1. the BLE disconnect handler,
2. the connection-failure retry cascade,
3. the periodic bonded reconnect.

`last_connected` names whichever peer connected **most recently**, which is not
necessarily the one that went away. With two bonded peers this is wrong whenever
the survivor is the more recent one:

> Keyboard connects second, so it owns the slot. The **mouse** powers off. The
> disconnect handler fires a reconnect at the **keyboard** — which is still
> connected.

The damage is not merely a wasted attempt. `btstack_host_connect_ble()` calls
`btstack_host_stop_scan()` at the top of every attempt, and the failure path
retries up to five times. So each cycle **tears down the very scan windows** in
which the absent peer's advertisements would have been seen. The absent peer can
then never be discovered, and only re-pairing restores it.

**Scope of what this explains.** The mechanism is asymmetric, and it must not be
overstated. It bites only when `last_connected` is the peer that *stayed*: then
the host dials a live peer and stops the scan the absent one needed. When the
peer that dropped **is** `last_connected`, the old code targeted the correct
address, and this mechanism does not by itself explain a failure in that
direction. Both directions were reported as failing on hardware, so at least one
of those observations is not accounted for here — candidates include the
five-attempt cascade exhausting its scan windows before the peer returned, or
`reason_warrants_reconnect` classification. The single-slot design is the correct
architectural defect to fix regardless, but this record does not claim it explains
every observation.

Corroborating hardware counters captured earlier in the failing configuration:
balanced `scan {starts:64, stops:64}` — one scan teardown per reconnect cycle —
with the bonded peer advertising and never being seen (`target_adv:0`).

### Supporting static facts

| # | Property | Confidence |
|---|----------|------------|
| 1 | The reconnect target is a single slot; the scheduler never enumerated bonds. | Confirmed |
| 2 | BTstack's LE device DB already stores per-peer bonds (`MAX_NR_LE_DEVICE_DB_ENTRIES` = 16), so the identity list already existed. | Confirmed |
| 3 | `btstack_host_connect_ble()` stops the scan on every attempt. | Confirmed |
| 4 | `btstack_host_start_scan()` has no BLE-connection guard — discovery can run while a peer is connected. | Confirmed |
| 5 | The advertising auto-connect path resolves name, profile and VID/PID from the advertisement, so a peer reached by discovery gets full identity. | Confirmed |
| 6 | Multiple simultaneous BLE HID links already work. This was never a BTstack capability limit. | Confirmed (hardware) |
| 7 | The LE device DB is **not** controller-only: `sm_init()` is global with `SM_AUTHREQ_BONDING`, one DB serves both roles, and the management link is bonded (`mgmt_session_authorized()` requires `client_bonded`). A companion bond therefore sits in the same DB the selector enumerates. | Confirmed |
| 8 | An unrelated bond still cannot be dialled: DIRECT requires `preferred`; `preferred` requires equality with `last_connected_addr`; that field is written only by `btstack_host_remember_ble_connection()`, whose callers all pass a `ble_connection_t` from `hid_state.connections[]`; and that table is filled only in the **central-role** branch of `HCI_SUBEVENT_LE_CONNECTION_COMPLETE` — peripheral-role ACLs are routed to `config_ble_accept_connection()` and `break` first. | Confirmed |

## Evidence that is permanently unavailable

After the failure the devices were **manually re-paired** to restore operation.
Re-pairing rewrites the bond table and the `last_connected` slot. Therefore
whether both bonds survived the original power cycle, and which peer held the
target slot at that moment, **cannot be reconstructed**. A snapshot taken
2026-08-16 (both peers connected, `has_target` naming only the mouse) is
post-repair state and is **not** evidence about the original failure.

This does not block the fix. A host that supports multiple bonded HID peers must
not depend on one global `last_connected` slot as its sole reconnect identity —
an architectural fact independent of any particular bond's survival.

## Fix

`include/ns2_ble_reconnect.h` + `src/ns2_ble_reconnect.c` — a pure, host-testable
selector over bonded candidates. Deliberately free of BTstack and Pico SDK
dependencies so the policy is unit-testable.

Inputs per candidate: address, address type, `connected`, `preferred` (matches
the stored `last_connected` record, so name/profile/VID/PID are available).

Decision:

- **IDLE** — no bonds, or every bonded identity is live. Issue nothing.
- **DIRECT** — an absent *preferred* candidate exists and is within the attempt
  bound. The host holds its metadata, so a direct `gap_connect` is safe.
- **SCAN** — absent identities exist but none is directly targetable. Keep
  discovery running; the advertising path supplies identity that a blind connect
  to a bare bonded address would lack.

**Invariant: a candidate that is already connected is never selected.** That is
what fixes the failure.

### Candidate universe and unrelated bonds

The DB is shared with the bonded management/companion path (property 7), so the
enumeration is not controller-only. Two things keep that safe:

- **Structural, not name-based:** only a `preferred` candidate can ever be
  direct-connected, and `preferred` can only originate from a central-role HID
  connection (property 8). A companion bond can therefore never be dialled.
- **A connected companion is excluded outright.** Its peripheral-role ACL never
  appears in the central-role connection table that `connected` is derived from,
  so without this it would look like a missing controller. Its address is
  captured on connect and cleared on disconnect.

A *stale* companion bond (paired previously, not currently connected) remains in
the candidate list and can make the selector answer SCAN instead of IDLE. That is
harmless by construction: SCAN only means "leave discovery running" and never
authorises a connect. It is pinned by test — no arrangement of non-preferred
candidates, at any attempt count, can produce DIRECT.

Note that BTstack's LE DB records no role or purpose per bond, so there is no
existing metadata that could filter stale companion bonds directly; the exclusion
is by role provenance instead. No device-name matching is used anywhere in this
path.

`btstack_host_pick_reconnect()` builds the candidate list from the LE device DB —
the existing authority — and all three sites now consult it. No KB/M-specific
addresses were introduced; no bond data was duplicated into KB/M configuration.

Direct attempts remain bounded (`NS2_BLE_RECONNECT_DIRECT_ATTEMPT_LIMIT` = 5,
mirroring the previous cascade bound), after which the policy yields SCAN so a
stuck preferred peer cannot starve the others.

### Adjacent correctness fix

The stale-bond deletion on `AUTHENTICATION_FAILURE` / `PIN_OR_KEY_MISSING` also
keyed off `last_connected`. With two bonded peers that could delete the bond of
the peer **still connected and working**. It now acts on the address of the peer
that actually dropped, captured before the connection slot is torn down.

## Observability retained

- `btbonds` (UART) — bond inventory: index, address type, address. `bonds list`
  reaches the same data but only over CDC or the BLE management bridge, neither
  reachable while the adapter's USB-C is on the console.
- `btreconnect` gains `bonds`, `bond_cap`, `bonded_adv`, `nontarget_adv`,
  `rpa_adv` — sightings matching any stored bond, matching a bond that is not the
  current target, and sightings under a resolvable private address.

Cross-core note: the LE device DB belongs to the BTstack thread on core 1 and the
UART diagnostics run on core 0. Core 1 republishes a plain bond snapshot on its
30 ms `btstack_host_process()` tick; core 0 reads it under a seqlock retry and
never touches the DB — the same hazard `config.c` marshals around.

`tools/reconnect_lab.ps1` is an optional developer harness for capturing labelled
checkpoints around a power cycle. It is **not** required to validate this fix.

## Automated validation

`tools/test_ns2_ble_reconnect.c`, 11 cases: both live; A live/B absent; B live/A
absent; neither live (deterministic, then SCAN for the remainder); attempt bound;
removed bond excluded; empty bond DB; legacy single-controller preserved; absent
peers without stored metadata reached by discovery; a connected identity never
returned even when pathologically flagged preferred; no arrangement of
non-preferred bonds yields a direct connect; NULL and oversized-count safety.

`tools/test_ns2_kbm.c` additionally pins the discovery-lifetime rule: partial KB/M
in either direction keeps discovery available, both roles retire it, a lone
controller retires it, role loss reopens it, and completeness stays independent of
the AUTO-derived effective mode.

Management/host suite: 18 passed, 0 failed. Both boards build clean; install-reset
markers verified.

## Remaining unknowns

- Whether either peer uses a rotating resolvable private address is still unknown;
  `rpa_adv` would reveal it. The fix does not depend on the answer, because
  discovery matches on the advertisement rather than a raw bond compare.
- Discovery remains active indefinitely while a KB/M source is deliberately
  partial (intentional keyboard-only or mouse-only use). That is the behaviour
  that makes rejoin work; bounding it is deferred future work, not a defect.

## Follow-on: bounded partial-source discovery (2026-08-17)

Bounding a partial KB/M source's discovery to a 10 s completion window created a
state this investigation never exercised — **settled partial**: one role live,
discovery idle, source still incomplete. Two latent defects surfaced there, both
fixed and hardware validated.

**1. HID_READY stops the scan, and nothing re-asserted it.** Every BLE HID peer
reaching ready calls `btstack_host_stop_scan()` (three sites in `btstack_host.c`).
That is legitimate low-level behaviour. The defect was at the policy layer: the
partial-source re-arm lived inside `if (pairing_until_ms == 0)`, so the first peer
to finish connecting *inside* an explicit pairing window killed discovery for the
rest of that window. Measured: keyboard connected, source partial, `hid_state=0`,
`scan_active=false`, `scan starts == stops`.

The durable lesson: **discovery ownership must be re-asserted every tick, not
assumed.** `stop_scan()` has many legitimate callers, so "who stopped the scan?"
is the wrong question. `ns2_kbm_discovery_policy()` now evaluates intent on every
control tick and re-arms idempotently. The zero-connection idle safety net is not
a substitute — its final term counts BLE links, so it cannot fire while a partial
source has one peer connected.

**2. Speculative DIRECT reconnect could consume an explicit pairing window.**
`stop_scan()` clears `scan_start_time`; the pairing window's `start_scan()` then
takes the "first scan with a bonded device" fast path and backdates it, making the
periodic reconnect eligible ~3 s into the window. `btstack_host_connect_ble()`
stops the scan for the whole attempt. `ns2_ble_reconnect_select()` now takes
`pairing_window_open` and never returns DIRECT while it is set.

That flag's provenance was verified from production code, because its old name was
misleading: `management_pairing_window_open` was **not** a companion/management
window. It is the HID/BOOTSEL pairing window —
`open_pairing_window()` → `bt_set_pairing_mode(true)` →
`cyw43_transport_set_pairing_mode()` → `btstack_host_set_pairing_window_open(true)`,
one assignment site, cleared on the same path when the window closes. The
management bond gate is a *reader* of it, not a separate window. Renamed to
`hid_pairing_window_open`.

### Rejected designs — do not resurrect

| Idea | Why rejected |
|---|---|
| Persistent HID peer roster (keyboard/mouse addresses, expected-peer database) | "Known HID peer" is not "expected member of the current logical source"; it would make every historical gamepad an expected peer and keep discovery alive. BTstack remains the bond authority. |
| AUTO effective mode as the completeness authority | AUTO is *derived* from the roles currently present, so it reports "complete" the moment one peer arrives and can never ask discovery to keep looking. |
| Extending the completion window on report traffic | An actively used keyboard would hold discovery open forever. The window is keyed to logical-source transitions and expires from the original transition time. |
| Reverting the bounded window to hide the exposed bug | The settled-partial state is part of the feature contract; the exposed defects were real and were fixed. |

### Final hardware result — 2026-08-17, PASS

FALCHION RX + KERIS II ACE on Pico W. Zero peers → normal discovery restored.
First role joins → discovery stays active. ~30 s later → completion window expired,
discovery retired, keyboard still connected and working. BOOTSEL double-tap →
discovery re-armed despite the background window being long gone. Mouse powered on
inside that pairing window → joined as the second role (`keyboard=true mouse=true`,
`keyboardConn=4 mouseConn=5`, `ble_conns=2`, mouse input confirmed), then discovery
retired because the source was complete. No reboot, no bond clearing, no
disconnecting the keyboard, no manual mode change.

## Do not

- Do not treat post-repair bond state as evidence about the original failure.
- Do not reintroduce a single reconnect identity, or stack conditions around one
  slot until it approximates a list. The bond database is the list.
- Do not clear or recreate bonds automatically because re-pairing fixes it.
- Do not weaken pairing security, and do not make the device name the security
  identity.
