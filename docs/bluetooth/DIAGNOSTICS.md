# Bluetooth diagnostics

Status: current UART and management surfaces as of 2026-08-20.

Diagnostics are pull-based and bounded so they can be used during real console operation without
turning logging load into a timing variable.

## Commands

| Command | Purpose |
|---|---|
| `btstate` | live radio, management, pairing/admission and disconnect snapshot |
| `btbonds` | core-1-published LE bond address/type inventory |
| `btlife status` | same live state plus lifecycle ring summary |
| `btlife read N` | one bounded ring event, oldest first |
| `btlife clear` | clear lifecycle ring and its existing counters |
| `btreconnect` | preferred target and LE reconnect/security counters |
| `btdev` | live bthid connection/driver/classification inventory |
| `btfresh` | explicit UART-only Switch 2 fresh-pair diagnostic action |

The CDC/BLE management command surface additionally provides `btid dump|stat|clear|desc` for
identity/classification transitions. Its `bonds list`/paged form reads the same LE DB on the owning
core; `bonds remove` deletes one typed LE entry. It is not a Classic inventory.

## `btstate` lifecycle fields

Relevant JSON groups include:

| Field | Meaning |
|---|---|
| `scan_active`, `inquiry_active` | LE and Classic discovery mechanics |
| `wake_adv` | temporary wake advertisement owns the radio |
| `connections.classic_raw`, `connections.ble_raw` | allocated transport slots; not readiness |
| `connections.classic_ready`, `connections.ble_ready` | HID/protocol-ready controller links |
| `pairing.window_open` | explicit fresh-pair authority is open |
| `pairing.close_deferred` | UI window expired while an LE connection was in flight |
| `pairing.lockout` | post-wipe/install global admission lock |
| `cble.*` | management peripheral availability, advertising and live client |
| `admission.fresh_accepted` | fresh controller trust decisions accepted |
| `admission.reject_window` | fresh trust/security attempts rejected because no window was open |
| `admission.reject_lockout` | attempts rejected by the stronger wipe/install lock |
| `wipe_completions` | synchronous trust-store wipe sequences completed; disconnect events may follow |
| `disc.*` | aggregate controller/HCI disconnects, state losses and last HCI reason |
| `owner_led.reason`, `owner_led.on` | selected owner-LED policy reason and current output |
| `owner_led.last_transition_ms` | wall-clock time of the most recent actual LED on/off edge; zero until the initially-off output first changes |
| `owner_led.timer_max_gap_ms` | largest observed gap between owner-LED timer service calls |

Counters are monotonic for the boot and may count more than one defensive rejection in a single
remote attempt. They explain policy activity; they are not a unique-peer count.

The owner LED uses this fixed priority: mode acknowledgement, GameCube diagnostic, Config mode,
wipe acknowledgement, pairing, controller ready, idle. “Connected” is driven only by a ready
controller count, never by a raw ACL/LE slot. Every cadence uses elapsed milliseconds, so callback
frequency cannot speed it up. Idle is one 90 ms pulse per 10 seconds; a solid LED means a controller
is protocol-ready unless a higher-priority state is active.

The reason/pattern epoch is internal and separate from `last_transition_ms`; changing reason without
changing electrical output does not advance the diagnostic timestamp. Packet-rate Bluetooth and
Wiimote transport logging is diagnostic-gated or bounded; production investigation should prefer
the pull snapshots and counters above.

## Distinguishing persistence from automatic re-pairing

Use this order. Do not power the remote on until step 4.

1. Record firmware identity and `btbonds` while the relationship exists.
2. Power the controller off.
3. Perform wipe or the named UF2 flash path, reboot, then capture `btstate`, `btbonds`, and
   `btreconnect` before the remote returns.
4. Confirm zero applicable bonds, no `JPLC` target, `pairing.window_open=false`, and
   `pairing.lockout=true` for wipe/install reset.
5. Power the remote on without opening a window.
6. Capture the same snapshots plus `btlife` and note whether `admission.reject_*` increases.
7. Open one explicit window, re-pair, and confirm `fresh_accepted` and bond inventory increase.

If a bond exists at step 4, investigate persistence/deletion. If it does not exist but a new bond
appears at step 6, investigate admission. A connected LED by itself proves neither.

## Failure localization

Combine snapshots to distinguish:

- advertisement never arrived (`advertising_reports` unchanged);
- peer advertised but was not the preferred target (`nontarget_advertising_reports`);
- RPA could not be raw-address matched (`rpa_advertising_reports`);
- connection was attempted and failed (`target_connect_*`);
- link connected but security failed (`reencryption_*` and last status);
- management occupied or changed its own role (`cble.*`, management events);
- discovery restart was suppressed (`suppress.*`);
- trust creation was refused by policy (`admission.*`).

If `disc.state_losses` increases, correlate it with ready-count retirement and HCI recovery. A raw
slot without a ready count must not select the solid-connected LED state.

## Secret-handling policy

Ordinary diagnostics MUST NOT emit Classic link keys, LTKs, IRKs, Switch 2 A1/B1 material, PIN
bytes, or raw TLV entries containing them. Address/type, key-present booleans, phases, counts,
statuses and hashes of non-secret capture artifacts are allowed. Raw secret inspection, if ever
required for a bounded laboratory experiment, MUST be an explicitly gated non-production path and
MUST NOT be committed in captures or logs.
