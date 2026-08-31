# KB/M resident upload stalls: the management reply was never notified

**Date:** 2026-08-31
**Status:** Root cause **Confirmed** by source audit plus a live-hardware control.
Fix implemented; end-to-end hardware validation of the fixed build pending.

## Question

Assigning a local KB/M profile to an adapter bank position killed the management
session. The companion reported the assignment as done, the bank still showed
Empty, the app could not reconnect without being force-closed, and one occurrence
escalated to "identity changed / repair dongle" — after which a power cycle and
an ordinary reconnect worked over the same pairing, with no repair.

Which command in the staged upload first goes unanswered, and why?

## Background

A resident upload is one staged transaction over the in-band BLE management
carrier:

```
kbm draft begin kb pos:N <baseRevision> <name>
kbm draft bind <src> <dst>          × one per SPARSE override
kbm draft mouse <field> <value>     × 6
kbm draft commit
kbm profiles 0                      readback
kbm active                          readback
```

The reported failure named the first command that never answered:

```
14:12:10.593 INFO  [kbm] mode=controller profile=kb keyboard=False mouse=False bindings=53
14:12:35.655 DEBUG [ble] retire reason=reply-timeout gen=1
14:12:35.782 ERROR [ui] The adapter did not answer 'kbm draft bind key:1D a' within 10000 ms.
```

`reply-timeout`, not `command-failed`, is the load-bearing detail. The Windows
client writes with `GattWriteOption.WriteWithResponse` and turns any ATT error
into a `GattTransportException`, which retires with `command-failed`. So **the
GATT write succeeded** and no reply came back. That eliminates the obvious
suspect — the firmware's one-slot bridge dropping a command it cannot accept,
which returns `ATT_ERROR_INSUFFICIENT_RESOURCES` and is therefore visible to the
client, not silent.

## Hypotheses

1. The firmware rejected, mis-parsed, or deadlocked on `kbm draft bind`.
2. Core 0 never ran `config_wireless_task()`, so the reply was never published.
3. The reply was published and core 1 never notified it.
4. The Windows client wrote the command but dropped or mis-correlated the reply.

## Method and results

### Control: replay the whole transaction through the real dispatcher

The UART console's `cfg <management command>` runs a command through
`config_execute_captured()` — the same `handle_line()` parser, the same KB/M
draft state, the same persistence — so the firmware half can be exercised with no
BLE involved. Run against the live adapter (`build 71373293+dirty`, bank empty):

```
15 ms  cfg kbm draft begin kb pos:1 0 Bench Test   {"ok":true}
 9 ms  cfg kbm draft bind key:1D a                 {"ok":true}
 9 ms  cfg kbm draft bind key:1B b                 {"ok":true}
 9 ms  cfg kbm draft bind key:06 x                 {"ok":true}
 9 ms  cfg kbm draft bind key:19 y                 {"ok":true}
 9 ms  cfg kbm draft mouse sensitivityx 100        {"ok":true}
       … five more mouse fields …
10 ms  cfg kbm draft commit                        {"ok":true,"id":3,"revision":1}
22 ms  cfg kbm profiles 0                          total:1, position:1, "Bench Test",
                                                   revision:1, overrides:4
```

**Hypothesis 1 is disproven.** The exact command from the failure log — including
`key:1D a` — is accepted, parsed, staged and committed, at roughly 10 ms per
command, and the profile lands in the bank position asked for. Nothing about the
KB/M command surface, the allowlist, the draft session, or config persistence is
implicated.

That also weakens hypothesis 2: `cfg` is dispatched from
`ns2_uart_diag_task()`, which sits in the same core-0 loop iteration as
`config_wireless_task()`. A starved core 0 would have made these round trips slow
too, and they were not.

### The audit that follows: who arms the notification?

`att_server_request_to_send_notification()` for the management link had exactly
one call site, at the end of `config_ble_service_task()` in
`src/bt_hid/bt/btstack/btstack_host.c`. Above it sat:

```c
// Don't perturb an in-flight controller connect attempt (a gap_connect is
// outstanding); its completion path resolves per-link and the next tick
// resumes advertiser arbitration.
if (hid_state.state == BLE_STATE_CONNECTING) {
    return;
}
```

That guard is about **advertising** — its own comment says so. But it returned
before the notification block, so while a controller connect attempt was
outstanding, a management reply that core 0 had already published was never
scheduled for transmission.

And:

```c
#define BLE_CONNECT_TIMEOUT_MS 10000   // 10s timeout for BLE connection attempts
```

The companion's per-command budget is `ManagementChannel.DefaultTimeoutMillis`,
which is also **10 000 ms**. A single overlapping connect attempt is therefore
sufficient, by itself, to guarantee the client times out. `btstack_host.c:6018`
notes a "blind `gap_connect()` cascade (BLE_CONNECT_TIMEOUT_MS=10s each, ~50s
worst case)", so the window can be far longer than one attempt.

**Hypothesis 3 confirmed.**

### Why it only happens with no controller connected

`BLE_STATE_CONNECTING` is entered by `gap_connect()` against a candidate found by
the central-role scan. An adapter with a controller attached is not scanning and
not connecting. An adapter with **no** controller is doing exactly that, on a
loop.

The failure log says so: `personality=pro2 controller=No controller peers=0`.
That is not incidental — it is the precondition.

### The same return explains the other two symptoms

`config_ble_start_advertising()` also has exactly one call site, immediately
below the same guard, and `config_ble_handle_disconnect()` deliberately does not
advertise itself — it relies on the next service tick. So while the connect
cascade runs:

- a published management reply is not notified → **`reply-timeout`**;
- after the client retires and the link drops, the management advertiser is not
  restarted → **the adapter is invisible to a reconnect**;
- two `Unreachable` connect results in one logical attempt then satisfy
  `AdapterResetSignature` → **a false "identity changed / repair dongle"**.

One defect, three reported symptoms, and a power cycle clears all three because
it ends the cascade.

## Interpretation

Answering the management client is not a radio-role decision. It is one ATT
notification on a link that is already established, and it must not be gated by
anything that gates advertising. The ordering error was invisible in every
ordinary state and produced a symptom — "the adapter did not answer" — that
points at the command rather than at the carrier.

## Conclusion

`config_ble_pump_response()` is now a named function called **before** the
`BLE_STATE_CONNECTING` return, and it is the sole site that arms
`config_ble_can_send`. The advertiser arbitration below is unchanged: it is an
existing, hardware-informed decision and this pass had no evidence to revisit it.

The pump also measures the interval between a reply becoming sendable and its
notification completing, published as `cble.tx_wait_max_ms` in the UART
`btstate` snapshot. Nothing previously distinguished "the command never arrived"
from "the answer was never sent", which is why this took a source audit rather
than a reading.

`tools/test_bluetooth_closeout_wiring.py::check_management_notify_is_not_gated_by_controller_connect`
pins both halves — the call ordering, and that there is exactly one arming site —
and was mutation-verified: restoring the pre-fix ordering fails it, and the fixed
ordering passes.

## Client-side findings from the same incident

Independent of the firmware defect, three client defects made the consequences
worse than they had to be. All three are fixed with regressions.

1. **The relationship stayed `Connected` with no carrier.** The transport retires
   its GATT session correctly, but that was invisible above it, and
   `RequestReconnect` is deliberately inert while `Connected`. Reconnect did
   nothing; only killing the process cleared it. `ReconcileCarrierLoss()` now runs
   after every exclusive operation and returns the relationship to `Idle` —
   touching neither the registry, the pairing, nor the local library.

2. **`AdapterResetSignature` was reachable from a proven session.**
   `RequireSuccess` counted an `Unreachable` at any stage other than `Connect`,
   including `Command` on a session whose service discovery and CCC write had
   already succeeded over encrypted handles. `TransportTrustSnapshot.BondProven`
   now records that proof and disqualifies the signature outright, and only
   `Services` and `Subscribe` feed the corroboration counter. The genuine
   stale-bond path is untouched: a stale bond fails below the attribute layer and
   can never set `BondProven`.

3. **The page reported success unconditionally.** `OnAssignToAdapter` used the
   void `SafeAsync` overload and then printed its success message over the error
   banner `SafeAsync` had just raised. It now uses the result-carrying overload
   and returns early, and `AdapterRepository.AssignKbmPositionAsync` verifies the
   readback — the position must be occupied and its content fingerprint must equal
   what was sent — before returning at all.

`kbm draft bind` and `kbm draft mouse` were also added to the repeatable-command
allowlist on both clients. Both are absolute writes into a RAM-held draft, so a
repeat leaves it byte-identical; `begin`, `commit` and `abort` remain excluded.
This is defence against the one-slot bridge's documented BUSY drop, **not** the
fix for this defect — under the stall every retry would have timed out too.

## Confidence

**Confirmed** for the mechanism: the code path is unambiguous by inspection, the
two timeout constants are equal, the firmware half was cleared by a live control
run on the affected adapter, and the "no controller connected" precondition
matches the reported state exactly.

**Not yet measured:** the fixed build's `cble.tx_wait_max_ms` during a real
upload with no controller attached. That is what the hardware smoke test reads.

## Remaining unknowns

- Whether the advertiser should also be exempted from the `BLE_STATE_CONNECTING`
  guard. Skipping the restart leaves the adapter undiscoverable for up to
  `BLE_CONNECT_TIMEOUT_MS` per attempt after a management disconnect, which is
  what produced the false-repair escalation. It self-heals, and the client no
  longer misreads it, so this pass left the arbitration alone — but a bounded
  exemption is worth designing if the window is ever measured to be long.
- Whether the one-slot bridge's BUSY drop should return a distinguishable ATT
  error the client can retry on directly, rather than relying on an allowlist.

## Suggested follow-up

Read `btstate` after the smoke test and record `cble.tx_wait_max_ms`. On a
healthy carrier it should be single-digit to low-tens of milliseconds. A value
near 10 000 would mean a pump is still being gated somewhere this audit missed.
