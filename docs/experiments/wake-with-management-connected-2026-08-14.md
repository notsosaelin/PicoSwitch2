# Console wake while the companion app is connected — investigation

Status: 🟡 one root cause **found and fixed** (controller wake); the app's false success **fixed**;
the app-initiated radio question remains **hardware-gated**
Date: 2026-08-14
Related: [`../bluetooth/android-controller-bridge.md`](../bluetooth/android-controller-bridge.md),
[`../bluetooth/wake-from-sleep-design.md`](../bluetooth/wake-from-sleep-design.md)

## Reported behaviour

| Scenario | Observed |
|---|---|
| App only, app Wake action | App reports the signal was sent; console does **not** wake |
| Controller only | Wake works |
| App **and** controller connected | The physical controller can **no longer** wake the console; removing the app restores it |

Leading hypothesis offered: the BLE management connection conflicts with the BLE wake procedure.

## Finding 1 — controller wake with the app connected: ROOT CAUSE FOUND (code-proven)

This is **not** a radio conflict. It is the active-input arbiter silently swallowing wake intent.

`ns2_seam.c router_submit_input()` gates every incoming report on the arbiter:

```c
if (!ns2_active_input_submit(e, &route))
    return;                 // <-- returns here
...
ns2_wake_note_controller_input(...)   // <-- wake latch lived here, after the gate
```

The gate's own comment stated the intent: *"An inactive source must not affect slot 0, identity, raw
buttons, **wake**, motion, or mouse state."* Including wake in that list is the defect.

When the companion connects as a second input source, only one source is *active*. Every report from
the non-active physical controller returns at the gate, so it can never latch the neutral→pressed
edge that arms wake. Disconnecting the companion frees the slot, the controller becomes active again,
and wake works — exactly the reported behaviour, including its reversibility.

**Fix.** Wake intent is now latched *before* the gate; output remains gated. Rationale, recorded at
the call site: waking a sleeping console is not publication — it writes no slot-0 state, identity,
motion, or mouse data, and while the console sleeps nothing is being published by anyone. Pressing a
button on any connected controller unambiguously means "wake up". Sticks deliberately do not count,
so a drifting idle stick cannot wake the console; the non-neutral test mirrors the publishing path's
button/trigger fold.

Both paths now share one `seam_note_wake_intent()` implementation, so the neutral-baseline rules
(reconnect quarantine, `suppress_wake_input`, per-source sessions) cannot drift apart.

## Finding 2 — the app's false success: ROOT CAUSE FOUND (code-proven)

`config.c` handled `wake` as:

```c
ns2_wake_manual_request();                    // latch a flag on core0
reply("{\"ok\":true,\"queued\":true}");       // ...and immediately claim ok
```

The request is *performed later on core1*. The reply therefore only ever proved that the **command
was delivered**, and the app surfaced that as "Wake request queued" / signal sent. Worse, core1
discarded the outcome entirely (`(void)ns2_wake_request();`), so no layer knew whether the wake ran.

**Fix — the adapter now records what actually happened**, and the app polls for it:

| Outcome | Meaning |
|---|---|
| `advertised` | a wake advertisement really started on the radio |
| `console_awake` | console was not asleep; nothing to do |
| `no_identity` | never completed a USB pairing while the console was on |
| `radio_busy` | the advertiser refused / a wake burst was already running |
| `pending` | latched, not yet serviced |

New `wake status` command (allowlisted for wireless). The app polls it briefly after `wake` and
reports each outcome distinctly; an adapter too old to answer maps to `Unknown` → *"sent, this
firmware cannot report whether it ran"*. **No path reports success merely because a command was
transmitted**, and even `advertised` is phrased as "broadcast", not "the console woke", because the
console's response is not observable from here.

## Finding 3 — app-initiated wake failing: NOT yet root-caused (hardware-gated)

With Finding 2 fixed, the next reproduction will *say which branch it took*, which decides this:

- `no_identity` → the adapter never captured a wake identity; a pairing/persistence issue, unrelated
  to BLE.
- `console_awake` → the adapter thinks the console is awake (USB suspend/mount detection), unrelated
  to BLE.
- `radio_busy` → the advertiser refused, which is the first real evidence for a radio conflict.
- `advertised` → the adapter *did* broadcast and the console ignored it — the interesting case,
  pointing at the advertisement payload/address rather than at management coexistence.

**The leading BLE-conflict hypothesis is not confirmed and is not treated as the cause.** What the
code shows so far:

- `btstack_host_start_wake_advertisement()` does **not** reject on an active management connection.
  It rejects only for exclusive CDC Config, an in-flight controller admission, or an already-running
  wake burst. Management advertising is separately dropped while `wake_adv.active`, so wake outranks
  management by design.
- The wake burst sets a **random advertiser address** (`hci_le_random_address_set`) to replay the
  controller's identity. Per the Core Spec this command is disallowed while advertising, scanning, or
  initiating — all of which the wake path explicitly stops first — but **not** by a peripheral
  connection existing. So there is no spec-level reason a management connection blocks it; whether a
  particular CYW43 firmware agrees is exactly what `radio_busy` vs `advertised` will reveal.

## What was fixed vs what remains

| Item | State |
|---|---|
| Controller cannot wake while the companion is connected | ✅ **fixed** (wake intent latched before the arbiter gate) |
| App reports success without evidence | ✅ **fixed** (real outcome recorded + polled; no transmitted-means-success path) |
| App-initiated wake not waking the console | 🔴 **hardware-gated** — the new outcome reporting is the instrument that identifies it |

## Tests

`tools/test_ns2_wake_policy.c` (extended, green): the pre-existing automatic-wake policy is
unchanged, plus new coverage that a latched request reads `pending`; a console-awake request reports
`console_awake` and not success; an asleep console with a free radio reports `advertised` **and**
actually starts one advertisement; a busy radio reports `radio_busy`; and every serviced request is
counted.

`ManagementProtocolTest` (Android, green): every adapter outcome maps to its enum, and an
unrecognised or absent result maps to `Unknown` — never to success.

## Deliberately not done

No delays, disconnect/reconnect dances, or management teardown were added to "make wake work". The
instruction was explicit and the evidence does not yet justify touching radio arbitration.
