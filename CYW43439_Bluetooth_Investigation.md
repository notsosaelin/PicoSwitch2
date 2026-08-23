# CYW43439 / CYW43xxx Bluetooth Investigation

## Purpose

Investigation notes for PicoSwitch2 Classic Bluetooth reconnect failures
involving:

-   Raspberry Pi Pico 2 W
-   Infineon CYW43439
-   Pico SDK 2.2.0
-   BTstack
-   Simultaneous BLE GATT management + Bluetooth Classic HID

## Observed Failure Modes

### CLASSIC_PAGE_TIMEOUT

Android attempts Classic paging but the adapter does not report
receiving the page.

Investigate: - Page scan starvation - Inquiry/page scan interaction -
Controller scheduling behavior

### CLASSIC_ACL_TIMEOUT (HCI 0x08 CONNECTION_TIMEOUT)

Observed sequence:

page_rx -\> page_accept -\> \~20 second stall -\> HCI 0x08

This suggests the failure occurs after acceptance, during ACL
establishment.

Possible causes: - CYW43439 controller firmware state machine edge
case - Classic/BLE coexistence scheduling race - BTstack/CYW43 timing
issue

## Concurrent Classic + BLE

Stress case:

BLE GATT Management + Classic HID Controller Link + Rapid reconnect
cycling

Investigate: - Shared controller scheduling - LE + BR/EDR coexistence -
Same BD_ADDR multi-transport behavior

## Pico SDK

Current: - Pico SDK 2.2.0

Comparison: - Pico SDK 2.3.0

Relevant areas: - BTstack updates - cyw43 driver updates - HCI
processing behavior

A SDK update does not necessarily mean a CYW43439 firmware fix.

## BTstack

BTstack controls: - HCI commands - Host state machines - Event
processing

BTstack does not control: - RF scheduling - Page scan timing - Baseband
scheduling

Potential tests: - Compare SDK versions - Analyze inquiry scheduling -
Measure HCI event timing

## Disproven: inquiry restart inside the ACCEPTED_CONNECTION_REQUEST window

Status: **Disproven** (2026-08-23). Do not re-open without new evidence.

### Why it was attractive

Both inquiry restart sites gate only on `hid_state.state ==
BLE_STATE_SCANNING`, so neither is aware of a Classic establishment in
progress -- an omission this firmware owns. And in every soak up to that
point the correlation looked total: across 162 successful establishments,
**zero** contained an inquiry restart, while **all four** 0x08 stalls
contained two or three.

That correlation is an artifact of window duration and carries no causal
content. A success completes in ~0.63 s (p50) and inquiry rounds are
6.4 s apart, so a success window essentially cannot contain a restart; a
20 s stall will contain two or three no matter what caused it. The
observational data is structurally incapable of separating the two, which
is what forced an interventional test.

### Method

Runtime-toggleable suppression of Classic inquiry *restarts* between page
acceptance and HCI Connection Complete, default OFF, both arms from one
binary (uf2 sha256 `B667B087...`, build `0a20da9a`) against one pairing,
150 cycles per arm, workload A. Restarts were postponed, never cancelled;
no inquiry stop, no page scan change, no retry, no timing change.

### Result

The treatment applied exactly as intended and the failure did not move.

| | Arm A (production) | Arm B (suppressed) |
|---|---|---|
| establishment windows | 167 | 136 |
| inquiry restarts inside windows | 9 across 4 windows | **0 across 0 windows** |
| 0x08 stalls | 4 (2.4%) | 4 (2.9%) |
| stall durations | 20.203 / 20.453 / 20.212 / 20.633 s | 20.183 / 20.380 / 20.378 / 20.446 s |
| restarts inside each stall | 3 / 2 / 2 / 2 | **0 / 0 / 0 / 0** |
| acl_up latency p50 | 0.634 s | 0.635 s |

Fisher exact on 0x08 per establishment window: **p = 1.000**.

With inquiry restarts entirely absent from the window, the stall occurs at
the same rate, with the same duration, in the same 20.2-20.6 s cluster.
Inquiry restarts inside the establishment window are therefore **not
necessary** for CLASSIC_ACL_TIMEOUT.

A secondary hypothesis -- that suppression would consume the deliberate
`NS2_BT_INQUIRY_IDLE_GAP_MS` (2000 ms) gap by making the restart fire
immediately on Connection Complete -- was tested in the same data and is
also disproven. `acl_up` to next `inquiry_start` is p50 8.946 s in Arm A
and 9.032 s in Arm B; Arm B's minimum is 1.988 s and it never went below
the designed gap. Cycle cadence, not the restart policy, sets that
spacing.

Why the treatment could not plausibly have worked, in hindsight: 136
establishment windows at ~0.7 s each is ~95 s out of a 61.8 min run,
about 2.6% of the timeline. Removing inquiry from 2.6% of the run was
never going to move a 2.4% failure rate unless the coupling were exact.

### Not established either way

Arm B showed more CLASSIC_PAGE_TIMEOUT per cycle (13/148 = 8.8% vs 6/149
= 4.0%), but p = 0.103 -- not significant, and overall inquiry duty was
if anything *lower* in Arm B (5.8 vs 6.3 rounds/min), so there is no
mechanism on offer. Treat as unresolved noise, not as evidence that
suppression harms paging.

## Current Hypothesis Ranking

1.  CYW43439 controller scheduling edge case -- now the leading
    explanation for CLASSIC_ACL_TIMEOUT by elimination as well as by
    positive evidence. BTstack arms no host timer covering
    `ACCEPTED_CONNECTION_REQUEST`, nothing in this firmware emits 0x08,
    the stall duration clusters tightly at 20.2-20.6 s across both arms
    and every soak, and the one local scheduling behaviour that was
    plausibly implicated has now been removed from the window with no
    effect.
2.  BTstack/CYW43 host integration timing issue
3.  Android Bluetooth stack issue

## Methodology Note: the btlife ring is not cleared between runs

Arm B's 150 cycles overflowed the 4096-entry ring, and because the runner
drains but never clears it, the dump retained 155 minutes of history --
all of Arm A's run as well as its own. Correlating that dump wholesale
reported 210 windows and 6 stalls for Arm B, two of which (20.212 s and
20.633 s, two inquiry restarts each) were Arm A's cycles 89 and 113
counted a second time.

Separating on Arm A's own drain timestamp -- exact, since both runs share
one uninterrupted uptime clock -- gives the 136 windows and 4 stalls used
above. Any future A/B must clear the ring at run start or size it to the
run, and any dump that comes back at exactly ring capacity should be
treated as truncated and contaminated until proven otherwise.

## Production Consideration

This appears to require a stress workflow: - Active BLE management -
Repeated Classic reconnects - Hundreds of cycles

Possible production strategy: - Keep stable behavior - Add recovery
paths - Avoid degrading normal user behavior while chasing rare soak
failures
