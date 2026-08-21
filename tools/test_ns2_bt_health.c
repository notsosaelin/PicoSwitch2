#include <assert.h>
#include <stdio.h>

#include "ns2_bt_health.h"

static void healthy_probe_returns_to_idle(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 100u);
    assert(ns2_bt_health_tick(&h, 10100u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_SEND_PROBE);
    ns2_bt_health_note_probe_complete(&h, 10110u, 0u);
    assert(h.phase == NS2_BT_HEALTH_IDLE);
    assert(h.probes_sent == 1u && h.probes_ok == 1u);
}

static void timed_out_probe_cycles_hci(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    assert(ns2_bt_health_tick(&h, 10000u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_SEND_PROBE);
    assert(ns2_bt_health_tick(&h, 22000u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_POWER_OFF);
    assert(h.probe_timeouts == 1u && h.recovery_attempts == 1u);
    assert(ns2_bt_health_tick(&h, 22001u, false, true, true, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
    assert(ns2_bt_health_tick(&h, 22251u, false, true, false, false) ==
           NS2_BT_HEALTH_ACTION_POWER_ON);
    assert(ns2_bt_health_tick(&h, 22252u, true, false, false, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
    assert(h.phase == NS2_BT_HEALTH_IDLE && h.recovery_completions == 1u);
}

static void missing_real_handle_repairs_stale_claim(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    assert(ns2_bt_health_tick(&h, 10000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_POWER_OFF);
}

static void failed_power_transition_requests_reboot_once(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    assert(ns2_bt_health_tick(&h, 10000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_POWER_OFF);
    assert(ns2_bt_health_tick(&h, 15000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_REQUEST_REBOOT);
    assert(ns2_bt_health_tick(&h, 16000u, true, false, true, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
}

static void idle_or_active_links_do_not_recover(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    assert(ns2_bt_health_tick(&h, 50000u, true, false, false, false) ==
           NS2_BT_HEALTH_ACTION_NONE);
    ns2_bt_health_note_hci_event(&h, 50000u);
    assert(ns2_bt_health_tick(&h, 59999u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_NONE);
}

static void unrelated_hci_progress_satisfies_liveness_probe(void)
{
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    assert(ns2_bt_health_tick(&h, 10000u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_SEND_PROBE);
    ns2_bt_health_note_hci_event(&h, 10001u);
    assert(ns2_bt_health_tick(&h, 10002u, true, false, true, true) ==
           NS2_BT_HEALTH_ACTION_NONE);
    assert(h.phase == NS2_BT_HEALTH_IDLE && h.probes_ok == 1u);
}

int main(void)
{
    healthy_probe_returns_to_idle();
    timed_out_probe_cycles_hci();
    missing_real_handle_repairs_stale_claim();
    failed_power_transition_requests_reboot_once();
    idle_or_active_links_do_not_recover();
    unrelated_hci_progress_satisfies_liveness_probe();
    puts("ns2_bt_health tests passed");
    return 0;
}
