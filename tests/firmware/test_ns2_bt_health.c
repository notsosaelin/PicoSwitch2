#include <assert.h>
#include <stdio.h>

#include "ns2_bt_health.h"

// Shorthand for the ordinary "a link is claimed and a real OPEN handle exists"
// case, which is what almost every test varies from.
static ns2_bt_health_inputs_t inputs(bool working, bool off, bool claimed,
                                     bool handle)
{
    ns2_bt_health_inputs_t in = {
        .hci_working = working,
        .hci_off = off,
        .claimed_acl = claimed,
        .probe_handle_available = handle,
        .security_in_flight = false,
    };
    return in;
}

static ns2_bt_health_action_t tick(ns2_bt_health_t *h, uint32_t now,
                                   bool working, bool off, bool claimed,
                                   bool handle)
{
    ns2_bt_health_inputs_t in = inputs(working, off, claimed, handle);
    return ns2_bt_health_tick(h, now, &in);
}

static void healthy_probe_returns_to_idle(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 100u);
    assert(tick(&h, 10100u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_SEND_PROBE);
    ns2_bt_health_note_probe_complete(&h, 10110u, 0u);
    assert(h.phase == NS2_BT_HEALTH_IDLE);
    assert(h.probes_sent == 1u && h.probes_ok == 1u);
}

static void timed_out_probe_cycles_hci(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    assert(tick(&h, 10000u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_SEND_PROBE);
    assert(tick(&h, 22000u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_POWER_OFF);
    assert(h.probe_timeouts == 1u && h.recovery_attempts == 1u);
    assert(tick(&h, 22001u, false, true, true, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
    assert(tick(&h, 22251u, false, true, false, false) ==
           NS2_BT_HEALTH_ACTION_POWER_ON);
    assert(tick(&h, 22252u, true, false, false, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
    assert(h.phase == NS2_BT_HEALTH_IDLE && h.recovery_completions == 1u);
}

// A claimed ACL with no OPEN handle is what a link in setup, authentication, or
// teardown looks like. It used to escalate straight to a power cycle, skipping
// the cheap probe that exists to tell the two apart. It must now persist for its
// own grace interval first.
static void missing_handle_needs_a_confirmation_interval(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);

    // Quiet long enough to consider escalating, but the missing handle has only
    // just been observed.
    assert(tick(&h, 10000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
    assert(tick(&h, 19999u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
    assert(h.recovery_attempts == 0u);

    // Still missing after the grace interval: now it is evidence.
    assert(tick(&h, 20000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_POWER_OFF);
}

static void a_handle_appearing_cancels_the_missing_handle_timer(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    assert(tick(&h, 10000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
    // The link finished coming up. That is the probe path, never a power cycle.
    assert(tick(&h, 12000u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_SEND_PROBE);
    assert(h.recovery_attempts == 0u);
    assert(!h.no_handle_armed);
}

// The 2026-08-21 field case: a pairing/security procedure is legitimately quiet
// on the HCI event path. Escalating through it would power-cycle the radio in
// the middle of the user's own pairing.
static void an_admitted_security_procedure_suppresses_escalation(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    ns2_bt_health_inputs_t in = inputs(true, false, true, true);
    in.security_in_flight = true;

    // Far past the quiet threshold, but pairing owns the radio.
    assert(ns2_bt_health_tick(&h, 10000u, &in) == NS2_BT_HEALTH_ACTION_NONE);
    assert(ns2_bt_health_tick(&h, 40000u, &in) == NS2_BT_HEALTH_ACTION_NONE);
    assert(h.probes_sent == 0u && h.recovery_attempts == 0u);
    assert(h.security_suppressions == 1u);

    // Suppression is bounded from when the procedure was first observed, so a
    // wedge DURING pairing still recovers.
    assert(ns2_bt_health_tick(&h, 54999u, &in) == NS2_BT_HEALTH_ACTION_NONE);
    assert(ns2_bt_health_tick(&h, 55000u, &in) ==
           NS2_BT_HEALTH_ACTION_SEND_PROBE);
}

static void suppression_window_restarts_per_procedure(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    ns2_bt_health_inputs_t busy = inputs(true, false, true, true);
    busy.security_in_flight = true;

    assert(ns2_bt_health_tick(&h, 10000u, &busy) == NS2_BT_HEALTH_ACTION_NONE);
    // Procedure ends; a later one gets its own full allowance rather than
    // inheriting the first one's elapsed time.
    ns2_bt_health_note_hci_event(&h, 11000u);
    assert(tick(&h, 11001u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_NONE);
    assert(!h.security_suppress_armed);

    assert(ns2_bt_health_tick(&h, 30000u, &busy) == NS2_BT_HEALTH_ACTION_NONE);
    assert(h.security_suppressions == 2u);
    assert(ns2_bt_health_tick(&h, 74000u, &busy) == NS2_BT_HEALTH_ACTION_NONE);
    assert(ns2_bt_health_tick(&h, 75000u, &busy) ==
           NS2_BT_HEALTH_ACTION_SEND_PROBE);
}

static void failed_power_transition_requests_reboot_once(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    assert(tick(&h, 10000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
    assert(tick(&h, 20000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_POWER_OFF);
    assert(tick(&h, 25000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_REQUEST_REBOOT);
    assert(h.phase == NS2_BT_HEALTH_FAILED);
    assert(tick(&h, 26000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
}

static void idle_or_active_links_do_not_recover(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    assert(tick(&h, 50000u, true, false, false, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
    ns2_bt_health_note_hci_event(&h, 50000u);
    assert(tick(&h, 59999u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_NONE);
}

static void unrelated_hci_progress_satisfies_liveness_probe(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    assert(tick(&h, 10000u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_SEND_PROBE);
    ns2_bt_health_note_hci_event(&h, 10001u);
    assert(tick(&h, 10002u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_NONE);
    assert(h.phase == NS2_BT_HEALTH_IDLE && h.probes_ok == 1u);
}

// Whatever the inputs do, recovery must be bounded: every escalation reaches a
// terminal state and no path emits a second reboot request.
static void escalation_is_always_bounded(void)
{
    for (unsigned mask = 0; mask < 32u; ++mask) {
        ns2_bt_health_t h;
        ns2_bt_health_init(&h, 0u);
        ns2_bt_health_inputs_t in = {
            .hci_working = (mask & 1u) != 0u,
            .hci_off = (mask & 2u) != 0u,
            .claimed_acl = (mask & 4u) != 0u,
            .probe_handle_available = (mask & 8u) != 0u,
            .security_in_flight = (mask & 16u) != 0u,
        };
        unsigned reboots = 0;
        for (uint32_t t = 0; t < 400000u; t += 1000u) {
            ns2_bt_health_action_t a = ns2_bt_health_tick(&h, t, &in);
            if (a == NS2_BT_HEALTH_ACTION_REQUEST_REBOOT) reboots++;
            assert(reboots <= 1u);
        }
        // A steady input set never oscillates: it either stayed idle or came to
        // rest in a terminal/awaiting phase.
        assert(h.phase == NS2_BT_HEALTH_IDLE ||
               h.phase == NS2_BT_HEALTH_PROBE_PENDING ||
               h.phase == NS2_BT_HEALTH_POWERING_OFF ||
               h.phase == NS2_BT_HEALTH_WAIT_POWER_ON ||
               h.phase == NS2_BT_HEALTH_POWERING_ON ||
               h.phase == NS2_BT_HEALTH_FAILED);
    }
}

int main(void)
{
    healthy_probe_returns_to_idle();
    timed_out_probe_cycles_hci();
    missing_handle_needs_a_confirmation_interval();
    a_handle_appearing_cancels_the_missing_handle_timer();
    an_admitted_security_procedure_suppresses_escalation();
    suppression_window_restarts_per_procedure();
    failed_power_transition_requests_reboot_once();
    idle_or_active_links_do_not_recover();
    unrelated_hci_progress_satisfies_liveness_probe();
    escalation_is_always_bounded();
    puts("ns2_bt_health tests passed");
    return 0;
}
