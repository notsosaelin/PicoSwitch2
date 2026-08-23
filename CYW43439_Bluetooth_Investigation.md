# CYW43439 / CYW43xxx Bluetooth Investigation

## Purpose

Investigation notes for PicoSwitch2 Classic Bluetooth reconnect failures
involving:

- Raspberry Pi Pico 2 W
- Infineon CYW43439
- Pico SDK 2.2.0
- BTstack 1.6.2 (pinned in the SDK)
- Simultaneous BLE GATT management + Bluetooth Classic HID

This document is written so a future engineer can reconstruct the whole
investigation without any conversation history. It records what was
observed, what was tried, what was eliminated, and what is still open.

**Status of the tree**: the Arm A baseline is the default and only
runtime behaviour. The Arm B treatment described below was an experiment
and has been removed from the active code path. It survives as history in
this document and in commit `0a20da9` on `experiment/inquiry-suppression`.

---

## 1. Original symptom

Intermittent Classic Bluetooth failures during repeated Controller Link
reconnect cycling, in two distinct families.

### CLASSIC_ACL_TIMEOUT (HCI 0x08 CONNECTION_TIMEOUT)

The adapter answers the page and the ACL never completes:

```
page_rx -> page_accept -> ~20 s of silence -> HCI 0x08
```

The stall duration is the defining feature. Across every soak run it
clusters tightly in **20.2-20.6 s**, and never anywhere else:

```
20.203  20.212  20.274  20.378  20.380  20.446  20.453  20.633
```

This is not the tail of the successful distribution. Successful
establishments are bounded and quantised: across 99 pooled successes they
fall on a ~90 ms grid (mean residual 9.2% of the step, against the 25%
expected from no grid), spanning 0.179 s to 0.996 s. Nothing at all
occupies the space between 1 s and 20 s. Two mechanisms, not one
distribution.

Rate: ~2.4% of establishment windows.

### CLASSIC_PAGE_TIMEOUT

Android pages, the adapter never reports receiving it. No `page_rx`, so
nothing to correlate against on the adapter side beyond radio state.

Rate: ~4% of cycles in the Arm A baseline.

These two are separate failures with separate evidence, and are kept
separate throughout. This document is primarily about the first.

---

## 2. Hypotheses considered

| # | Hypothesis | Status |
|---|---|---|
| 1 | Inquiry restart interference inside the establishment window | **Disproven** — section 5 |
| 2 | Inquiry restart consumes the designed idle gap | **Disproven** — section 5 |
| 3 | Page scan suppressed by local state | **Eliminated by source review** — section 3 |
| 4 | Host-side BTstack timing gap | **Partially eliminated** — section 3 |
| 5 | CYW43439 / controller-side scheduling | **Leading** — section 7 |
| 6 | Android Bluetooth stack | Open, low priority |

---

## 3. What source review established before any experiment

**Nothing in this firmware emits 0x08.** The reason code arrives from the
controller.

**BTstack arms no host-side timer covering `ACCEPTED_CONNECTION_REQUEST`.**
The state machine moves `RECEIVED_CONNECTION_REQUEST` ->
`ACCEPTED_CONNECTION_REQUEST` via `hci_accept_connection_request` and then
simply waits for `HCI_EVENT_CONNECTION_COMPLETE`. There is no host timeout
on that wait. Whatever produces the ~20.2-20.6 s bound is below the host.

**Page scan cannot be suppressed by local state.** `hci_stack->connectable`
has exactly one writer (`gap_connectable_control`), so no combination of
LE scan, management, HID, wake advertising or inquiry state can turn page
scan off behind our back. Consistent with the event ring: `page_scan_off`
appears once in 1303 events, at boot.

**What this firmware does own** is the inquiry restart. Both restart sites
gate only on `hid_state.state == BLE_STATE_SCANNING` — the BLE state
machine — so neither is aware of a Classic establishment in progress.
That is a genuine omission, and it is what made hypothesis 1 attractive.

---

## 4. Experimental setup

### Arm A — baseline (production behaviour, current default)

Unmodified. Classic inquiry restarts are scheduled purely off the BLE
state machine, with a deliberate `NS2_BT_INQUIRY_IDLE_GAP_MS` (2000 ms)
gap between rounds whose stated purpose is to leave the controller room to
answer an incoming page.

### Arm B — treatment (removed from the tree)

Classic inquiry *restarts* postponed — never cancelled — for the duration
of an incoming Classic ACL establishment:

- window opened by `gap_classic_accept_callback` returning 1, ACL link
  type only. hci.c consults that callback for incoming requests only,
  which is what kept the outgoing `SENT_CREATE_CONNECTION` path out of
  the experiment.
- window closed at `HCI_EVENT_CONNECTION_COMPLETE`, both statuses, before
  any status branching so no early break could skip it.
- bounded by `EXP_CLASSIC_SETUP_MAX_MS` (30 s), enforced on the periodic
  loop rather than only inside the hold predicate, so a Connection
  Complete that never arrived could not take discovery with it.
- `inquiry_restart_at_ms` still armed throughout, so the deferred site
  fired as soon as the hold cleared.

No inquiry stop, no page scan change, no retry, no timing change.

### How the arms were kept comparable

Arm selection was a runtime UART toggle (`expmode inquiry on|off`), so
**both arms ran from one binary against one pairing**:

```
uf2 sha256 B667B0879C8AC1E3E34332CAF632DEEC709ADC17001122279712D84F673458A5
build      0a20da9a      git tree clean
```

Build, flash and bond were therefore not variables. 150 cycles per arm,
workload A. The arm was recorded in each run's identity file and queried
over UART before and after, so the artifacts state which arm produced
them.

Not controlled: time of day. Arm A ran 13:21, Arm B 15:22. RF environment
and phone state are not held constant by a shared binary.

### Measurement

Per-establishment measurement built from the firmware `btlife` event ring
**alone** — no Android timestamps, therefore no clock alignment, therefore
no alignment residual can move a latency figure. Each window runs from
`page_accept` to the next `acl_up`/`acl_fail`, and records duration,
outcome, failure reason and the number of `inquiry_start` events falling
inside it. Windows that never closed are counted and reported rather than
dropped, so a wrapped ring cannot be averaged in as if it were fast.

### Ring contamination, and how it was corrected

The soak runner drains the `btlife` ring after a run but **never clears
it**. Arm B's 150 cycles overflowed the 4096-entry ring, so its dump came
back at exactly capacity and retained 155 minutes of history — all of Arm
A's run as well as its own.

Correlating that dump wholesale reported 210 windows and 6 stalls for Arm
B, two of which (20.212 s and 20.633 s, two inquiry restarts each) were
Arm A's cycles 89 and 113 counted a second time. The tell was that those
two latencies matched Arm A's to the millisecond.

Correction: both runs share one uninterrupted adapter uptime clock, so
Arm A's own last drained event (`t = 4880.5 s`) is an exact boundary.
Cutting there gives Arm B's true 136 windows and 4 stalls. Every Arm B
figure in this document is post-correction.

**Rule for future A/B runs**: clear the ring at run start or size it to
the run, and treat any dump returning exactly ring capacity as truncated
and contaminated until proven otherwise.

---

## 5. Results

### The treatment applied exactly

| | Arm A | Arm B |
|---|---|---|
| establishment windows | 167 | 136 |
| inquiry restarts inside windows | 9 across 4 windows | **0 across 0 windows** |
| hold / resume events | n/a | 5 / 5, matched, no leaks |

### The failure did not move

| | Arm A | Arm B |
|---|---|---|
| 0x08 stalls | 4 (2.4% of windows) | 4 (2.9% of windows) |
| stall durations | 20.203 / 20.453 / 20.212 / 20.633 s | 20.183 / 20.380 / 20.378 / 20.446 s |
| inquiry restarts inside each stall | 3 / 2 / 2 / 2 | **0 / 0 / 0 / 0** |
| `acl_up` latency p50 | 0.634 s | 0.635 s |
| `acl_up` latency p90 / max | 0.818 / 1.075 s | 0.810 / 0.910 s |

Fisher exact on 0x08 per establishment window: **p = 1.000**.

Per cycle: ACL timeout 4/149 (2.7%) vs 5/148 (3.4%), p = 0.750.

The Arm A stall sequences look like
`[inquiry_stop, inquiry_start, inquiry_stop, inquiry_start]`. The Arm B
stall sequences are just `[exp_inquiry_hold]` — the restart genuinely did
not happen, and the stall happened anyway, for the same length of time.

### `acl_up` -> next `inquiry_start`

```
Arm A   n=162   min=0.438   p10=8.174   p50=8.946   p90=11.413   max=12.054
Arm B   n=130   min=1.988   p10=8.171   p50=9.032   p90=11.258   max=11.720
```

Effectively indistinguishable. The p10/p50/p90 agree to within ~0.1 s and
the shapes are the same.

This also disproves hypothesis 2. The concern was that a hold outliving
the 2000 ms idle gap would make the restart fire immediately on Connection
Complete, consuming the gap that exists to leave the controller room to
answer a page. It did not: Arm B's **minimum** was 1.988 s, so it never
went below the designed gap, and only 1 of Arm A's 162 samples was below
it. Cycle cadence (~9 s between link-up and the next round), not the
restart policy, sets that spacing.

### Inquiry duty overall

```
Arm A   6.3 inquiry rounds/min over a 78.5 min active span
Arm B   5.8 inquiry rounds/min over a 61.8 min active span
```

Arm B ran *less* inquiry overall, not more.

---

## 6. Conclusion: hypothesis 1 is disproven

**Removing inquiry restart behaviour from the establishment window did not
reduce CLASSIC_ACL_TIMEOUT.** The rate is statistically identical
(p = 1.000), the duration is identical, and the cluster is identical.
Inquiry restarts inside the window are **not necessary** for the failure.

This is a successful experiment, not a failed fix. It eliminated the most
plausible host-side cause, and it is the reason the controller-side
direction can now be called leading rather than merely remaining.

### Why the correlation looked total beforehand

Before the experiment the evidence appeared overwhelming: across 162
successful establishments **zero** contained an inquiry restart, while
**all four** stalls contained two or three.

That is a windowing confound with no causal content. A success completes
in ~0.63 s and inquiry rounds are 6.4 s apart, so a success window
essentially *cannot* contain a restart; a 20 s stall *will* contain two or
three no matter what caused it. Observational data of this shape is
structurally incapable of separating cause from consequence, which is
precisely why an interventional test was required.

### Why the treatment could not plausibly have worked, in hindsight

136 establishment windows at ~0.7 s each is about 95 s out of a 61.8 min
run — roughly **2.6% of the timeline**. Removing inquiry from 2.6% of the
run was never going to move a ~2-3% failure rate unless the coupling were
exact. Worth remembering when scoping the next intervention: a treatment
must cover enough of the timeline to be able to show an effect.

### Do not re-open without new evidence

Specifically, do not re-derive "inquiry restarts land inside every stall
and none of the successes" and treat it as a finding. It is true, it is
reproducible, and it means nothing.

---

## 7. Hypothesis ranking after the experiment

### Leading: CYW43439 / controller-side scheduling

BTstack arms no host-side timer covering `ACCEPTED_CONNECTION_REQUEST`,
and nothing in this firmware explains a repeated, tightly clustered
20.2-20.6 s failure window. The one local scheduling behaviour that was
plausibly implicated has now been removed from that window with no effect
on rate or duration.

**This does not prove the CYW hypothesis.** It is the leading explanation
by elimination of the host-side candidates plus the positive evidence
above (the reason code originates below the host; the duration is
controller-consistent and host-inexplicable; successes are slot-quantised
while stalls are not). Proving it requires evidence this investigation has
not yet gathered.

### Remaining

2. BTstack / CYW43 host integration timing issue — not eliminated, only
   narrowed. The absence of a host timer rules out a host *timeout*, not a
   host-driven race into a controller state.
3. Android Bluetooth stack — open, low priority. Android's role in the
   0x08 case is passive: it pages, the adapter answers, and both sides
   then wait.

---

## 8. Unresolved observations

These are recorded so they are not mistaken for findings later.

- **PAGE_TIMEOUT differences are not significant.** Arm A 6/149 (4.0%) vs
  Arm B 13/148 (8.8%), Fisher p = 0.103. An interim p = 0.048 computed on
  a partial Arm B run did not survive the complete data. There is also no
  mechanism on offer, since Arm B ran *less* inquiry overall. Treat as
  unresolved noise, not as evidence that suppression harms paging.
- **Single-instance Arm B failure classes are noise.** 1 STALE_LINK, 2
  C3_ACL_UP_THEN_DISCONNECTED, and an 0x0E at 7.566 s (against 1.586 s in
  Arm A). All single-digit counts, none conclusive.
- **More data is required before claiming controller scheduling impact.**
  At ~2.4% of windows, distinguishing "eliminated" from "reduced" needs on
  the order of 270+ establishment windows per arm; 150 cycles supports
  directional statements only.
- **Page scan starvation is not fully excluded.** The instrumentation
  records host-side scan-enable *transitions*, and there were none during
  either run. Controller-internal starvation without a host state change
  would not be visible to it.

---

## 9. Tooling kept from this investigation

All of this is independent of the rejected Arm B runtime behaviour and
remains in the tree:

- **`page_accept` -> Connection Complete measurement**
  (`tools/correlate_paging.py`, `establishment_windows`). Adapter-side
  only, no clock alignment, reports latency distribution, 0x08 count,
  other `acl_fail` reasons so a new failure class is visible, unterminated
  windows, and inquiry occupancy.
- **Paging and post-page instrumentation** (`btlife` event ring):
  page scan on/off, inquiry start/stop, `page_rx`, `page_accept`,
  `page_reject`, `acl_up`, `acl_fail`, `link_key_req`, `auth_defer`,
  `auth_start`, `auth_done`, `enc_change`, `hid_ready`, `hid_fail`, plus
  the compact radio-flag snapshot.
- **Wiring guards** (`tools/test_bluetooth_closeout_wiring.py`), including
  the guard that paging instrumentation stays observation-only and never
  acquires the radio or changes timing.
- **Soak harness** (`tools/controller_link_cycle.py`) with domain-split
  outcomes (`device:` / `app:` / `harness:` / `EXCLUDED`), so a dependency
  outage can never be reported as a device failure.
- **Ring contamination methodology** — section 4, above.

---

## 10. Suggested next experiment

Not started. Listed so the next session does not have to re-derive it.

Compare Pico SDK 2.2.0 against 2.3.0. It is the only remaining lever that
changes controller-adjacent code (BTstack version, cyw43 driver, HCI
processing) without changing this project's own logic. Note that an SDK
update does not necessarily carry a CYW43439 controller firmware change,
so a null result would not be conclusive — check what actually differs in
the bundled firmware blob before running cycles.

Size any A/B at 270+ establishment windows per arm, clear the `btlife`
ring at run start, and control time of day if practical.
