# Two controllers paired to the adapter both drive the console

**Status:** root cause identified and fixed in the source arbiter; reproduced
and pinned by a host regression test. **Hardware validation pending** — no
adapter was flashed for this pass.

**Reported:** 2026-09-06, by a tester. A DualSense and an Xbox Elite were paired
to the adapter at the same time, and **both moved the console simultaneously**,
while the companion's Input page showed a single controller driving it.

The one-owner rule was never violated *as the arbiter saw it*. Ownership was
being handed back and forth between the two controllers hundreds of times a
second, and every individual status snapshot the companion read was internally
consistent — one active source, exactly as the app displayed.

## Is this the app or the firmware?

**Firmware.** The companion renders `input sources` / `input status` verbatim;
it neither routes reports nor decides ownership. Nothing about the app's
rendering was wrong — it was reporting a state that was true for the microsecond
it was sampled.

## The exact path

`ns2_input_arbiter` classifies every source (`NS2_INPUT_SOURCE_CLASS_*`) and,
while the user has made no explicit choice, gives the console to the
highest-class source. `UNKNOWN` is the lowest class and means *"the caller that
registered this peer could not see what it is"* — a connection hook has no
report to classify from.

`register_source()` treated **any** class difference on an already-registered
source as a genuine change of standing and re-ran the automatic ownership
policy. That rule was written for the upgrade direction (`UNKNOWN` -> `DIRECT`
on a peer's first report, which is how a controller reclaims the console from
the companion bridge). The downgrade direction was never considered — and the
firmware performs it constantly:

1. `bthid.c::bt_on_hid_report_with_generation()` hands **every** input report,
   from every connected peer, to `bthid_on_raw_report()` before the bound driver
   parses it (`src/bt_hid/bt/bthid/bthid.c:688`).
2. `ns2_seam.c::bthid_on_raw_report()` calls
   `ns2_active_input_note_connection()` (`src/bt_hid/ns2_seam.c:483`).
3. That registers the peer with `NS2_INPUT_SOURCE_CLASS_UNKNOWN`
   (`src/bt_hid/ns2_active_input.c:188`) — correct for a peer that has never
   reported, destructive for one that has.
4. The owner was therefore demoted to `UNKNOWN` **on every one of its own
   reports**, dropping it below every other connected peer, and the policy
   handed the console to the rival controller.
5. Microseconds later the bound driver parsed the same report into an
   `input_event_t`, `router_submit_input()` re-registered the peer as `DIRECT`,
   and the policy handed the console back.

With one controller connected this is invisible: there is nobody to hand the
console to, so `apply_auto_policy()` finds the same owner and returns early.
With two paired controllers it means ownership follows **whichever controller
reported most recently**, and a report the bound driver does not turn into an
event — a mode/battery/paddle report, a report id the driver skips — leaves the
owner parked at `UNKNOWN` until its next parsed report, which is the window
where the second controller publishes to console slot 0 in its own right.

The secondary damage was a `report_neutralize_slot(0)` on both halves of every
report (each policy handover is an `auto_switched` decision, and the seam owes
the console a neutral boundary for one), so the console was also being fed
neutral state between real reports.

## Why the app showed one controller

`ns2_input_arbiter_get_status()` is a seqlock snapshot of `active_id`. Every
snapshot was valid; they simply disagreed with each other at report rate. A
companion polling once a second can only ever show one of them. **The status
surface is not a witness for this class of bug** — the `transitions` counter that
`input sources` reports (`ns2_input_arbiter_status_t::transition_count`) is, and it was climbing
at roughly the combined report rate of the connected controllers.

## Reproduction

`tools/test_ns2_input_arbiter.c::test_connection_hook_cannot_demote_a_classified_source()`
drives the pure arbiter through the exact call sequence the firmware produces
per report (raw hook `UNKNOWN`, then parsed event `DIRECT`) for two
directly-paired controllers. Against the pre-fix arbiter it fails on the first
raw hook of the owning controller, with `decision.auto_switched` set — the
console handed to the other controller by a hook that learned nothing.

## The fix

Both changes are in `src/bt_hid/ns2_input_arbiter.c`; no transport, seam, or
companion change was required.

1. **Ignorance never overwrites knowledge.** `register_source()` ignores an
   `UNKNOWN` class for a source that has already been classified. Only a report
   can reclassify a source, in either direction (`DIRECT` <-> `BRIDGE` still
   works, because those classes come from a report's declared provenance). A
   brand-new source registered by a connection hook is unaffected: it is created
   `UNKNOWN`, as before, and its first report identifies it.

2. **Ties go to the lower source id, not the lower registry slot.** A second,
   independent defect in the same policy function: `preferred_index()` broke
   equal-class ties by array index, but a registry slot is reused the moment its
   occupant disconnects, while ids are monotonic and never reused within a boot.
   With two controllers paired, a peer reconnecting into the slot its
   predecessor vacated outranked the peer that had inherited the console and
   took it away mid-game. Pinned by
   `test_equal_class_arrival_cannot_take_a_live_console()`.

## Keyboard + Mouse is unaffected

Keyboard + Mouse is the one place where two Bluetooth peers are deliberately ONE
console owner, so both changed rules were checked against it specifically.

**The composite never takes the UNKNOWN path.** A peer holding (or eligible for)
a KB/M role is registered exclusively by `ns2_kbm_runtime`'s own submit, which
always carries the composite `group_id` and always resolves to class `DIRECT`.
`ns2_kbm_runtime_gates_connection()` (`src/bt_hid/ns2_kbm_runtime.c:512`) keeps
the UNKNOWN-registering connection hook off those peers entirely — that gate is
what change 1 makes redundant for them, not something it depends on. The
upgrade direction, `UNKNOWN -> DIRECT`/`BRIDGE`, is untouched, so the mode-change
path (`ns2_active_input_reset()` + `reevaluate_connected_peers()`, which
re-registers every live peer as UNKNOWN into an empty registry) still classifies
each peer on its first report exactly as before.

**The composite was already immune to the original defect**, which is why the
tester saw it on two controllers and not on a keyboard and mouse:
`apply_auto_policy()` refuses to move the token to a peer of the group that
already owns the console, so demoting one half could never hand the console to
the other half. Two independent controllers share no group, which is precisely
what left them exposed.

**The tie-break change strengthens the composite rather than perturbing it.** A
half that reconnects into the registry slot its own predecessor vacated used to
be selected by `preferred_index()` and then rejected by that group guard; with
ties decided by source id it is not selected in the first place, and the
surviving half keeps the token.

Verified, not just argued: `test_composite_survives_the_two_controller_fix()`
drives the composite through an UNKNOWN re-registration of the token-holding
half, a member disconnect and rejoin into the vacated slot, an unrelated
controller attempting to displace it, and whole-source loss. It passes against
the fixed arbiter **and against the pre-fix arbiter** — composite behaviour is
unchanged in both directions, which is the point. `test_kbm_runtime_lifecycle`
covers the role/admission half of the same feature and is unaffected (it stubs
the arbiter); all seven `kbm` host targets pass.

## Evidence and confidence

| Claim | Confidence |
|---|---|
| The demotion path exists and fires on every report | **Confirmed** — call chain above, reproduced in a host test |
| It is what the tester observed | **Strong evidence** — it is the only path that produces both symptoms together (two controllers driving, one shown active), and it requires exactly the reported configuration (two same-class sources) to become visible |
| The fix restores a single stable owner | **Confirmed in software** — 80/80 host tests pass, including the new regressions; **not yet hardware-validated** |

## What would falsify the second claim

A hardware run with both controllers paired on the fixed firmware in which the
non-owning controller still moves the console. The discriminator is the
`transitions` counter in `input sources`: on the fixed firmware it must stay
**flat** while both controllers are moved, and rise only on a real connect,
disconnect, or user selection. A climbing count with the fix in place means a
second publisher exists on a path the arbiter does not gate.

## Related

- [`controller-link-console-slot-misroute-2026-08-21.md`](controller-link-console-slot-misroute-2026-08-21.md)
  — the other two-sources-connected defect, in the publish path rather than the
  ownership policy. Both stayed latent for the same reason: two same-class
  sources connected at once is a configuration the project rarely exercised.
