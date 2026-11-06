#include "ns2_bt_health.h"

#include <string.h>

static bool elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t interval_ms)
{
    return (uint32_t)(now_ms - since_ms) >= interval_ms;
}

void ns2_bt_health_init(ns2_bt_health_t *health, uint32_t now_ms)
{
    if (!health) return;
    memset(health, 0, sizeof(*health));
    health->last_hci_event_ms = now_ms;
}

void ns2_bt_health_note_hci_event(ns2_bt_health_t *health, uint32_t now_ms)
{
    if (!health) return;
    health->last_hci_event_ms = now_ms;
    health->hci_event_sequence++;
}

void ns2_bt_health_note_probe_complete(ns2_bt_health_t *health,
                                       uint32_t now_ms, uint8_t status)
{
    if (!health || health->phase != NS2_BT_HEALTH_PROBE_PENDING) return;
    health->last_hci_event_ms = now_ms;
    if (status == 0u) {
        health->probes_ok++;
        health->phase = NS2_BT_HEALTH_IDLE;
        health->phase_started_ms = now_ms;
    } else {
        health->probes_failed++;
        health->probe_failed = true;
    }
}

static ns2_bt_health_action_t start_power_off(ns2_bt_health_t *health,
                                               uint32_t now_ms)
{
    health->probe_failed = false;
    health->recovery_attempts++;
    health->phase = NS2_BT_HEALTH_POWERING_OFF;
    health->phase_started_ms = now_ms;
    return NS2_BT_HEALTH_ACTION_POWER_OFF;
}

static ns2_bt_health_action_t fail_to_reboot(ns2_bt_health_t *health,
                                              uint32_t now_ms)
{
    health->phase = NS2_BT_HEALTH_FAILED;
    health->phase_started_ms = now_ms;
    if (health->reboot_requested) return NS2_BT_HEALTH_ACTION_NONE;
    health->reboot_requested = true;
    return NS2_BT_HEALTH_ACTION_REQUEST_REBOOT;
}

ns2_bt_health_action_t ns2_bt_health_tick(ns2_bt_health_t *health,
                                          uint32_t now_ms,
                                          const ns2_bt_health_inputs_t *in)
{
    if (!health || !in) return NS2_BT_HEALTH_ACTION_NONE;

    switch (health->phase) {
        case NS2_BT_HEALTH_IDLE:
            if (!in->hci_working || !in->claimed_acl) {
                health->no_handle_armed = false;
                health->security_suppress_armed = false;
                return NS2_BT_HEALTH_ACTION_NONE;
            }
            // An admitted pairing/security procedure owns the radio and is
            // expected to be quiet on this path. Suppress escalation while it
            // runs, but bound the suppression so a wedge DURING pairing -- the
            // exact 2026-08-21 field case -- still recovers.
            if (in->security_in_flight) {
                if (!health->security_suppress_armed) {
                    health->security_suppress_armed = true;
                    health->security_suppress_since_ms = now_ms;
                    health->security_suppressions++;
                }
                if (!elapsed(now_ms, health->security_suppress_since_ms,
                             NS2_BT_HEALTH_SECURITY_SUPPRESS_MAX_MS)) {
                    health->no_handle_armed = false;
                    return NS2_BT_HEALTH_ACTION_NONE;
                }
            } else {
                health->security_suppress_armed = false;
            }
            if (!elapsed(now_ms, health->last_hci_event_ms,
                         NS2_BT_HEALTH_QUIET_BEFORE_PROBE_MS)) {
                health->no_handle_armed = false;
                return NS2_BT_HEALTH_ACTION_NONE;
            }
            if (!in->probe_handle_available) {
                // Not evidence of a wedge on its own. Confirm it persists.
                if (!health->no_handle_armed) {
                    health->no_handle_armed = true;
                    health->no_handle_since_ms = now_ms;
                    return NS2_BT_HEALTH_ACTION_NONE;
                }
                if (!elapsed(now_ms, health->no_handle_since_ms,
                             NS2_BT_HEALTH_NO_HANDLE_GRACE_MS)) {
                    return NS2_BT_HEALTH_ACTION_NONE;
                }
                return start_power_off(health, now_ms);
            }
            health->no_handle_armed = false;
            health->phase = NS2_BT_HEALTH_PROBE_PENDING;
            health->phase_started_ms = now_ms;
            health->probe_event_sequence = health->hci_event_sequence;
            health->probes_sent++;
            return NS2_BT_HEALTH_ACTION_SEND_PROBE;

        case NS2_BT_HEALTH_PROBE_PENDING:
            if (health->probe_failed) return start_power_off(health, now_ms);
            // The command is an active nudge, not the only acceptable proof.
            // Any later HCI event establishes that the controller path is
            // progressing and avoids a disruptive recovery during a benign
            // command-queue delay under mixed audio/controller traffic.
            if (health->hci_event_sequence != health->probe_event_sequence) {
                health->probes_ok++;
                health->phase = NS2_BT_HEALTH_IDLE;
                health->phase_started_ms = now_ms;
                return NS2_BT_HEALTH_ACTION_NONE;
            }
            if (elapsed(now_ms, health->phase_started_ms,
                        NS2_BT_HEALTH_PROBE_TIMEOUT_MS)) {
                health->probe_timeouts++;
                return start_power_off(health, now_ms);
            }
            return NS2_BT_HEALTH_ACTION_NONE;

        case NS2_BT_HEALTH_POWERING_OFF:
            if (in->hci_off) {
                health->phase = NS2_BT_HEALTH_WAIT_POWER_ON;
                health->phase_started_ms = now_ms;
                return NS2_BT_HEALTH_ACTION_NONE;
            }
            if (elapsed(now_ms, health->phase_started_ms,
                        NS2_BT_HEALTH_POWER_OFF_TIMEOUT_MS)) {
                return fail_to_reboot(health, now_ms);
            }
            return NS2_BT_HEALTH_ACTION_NONE;

        case NS2_BT_HEALTH_WAIT_POWER_ON:
            if (elapsed(now_ms, health->phase_started_ms,
                        NS2_BT_HEALTH_POWER_ON_DELAY_MS)) {
                health->phase = NS2_BT_HEALTH_POWERING_ON;
                health->phase_started_ms = now_ms;
                return NS2_BT_HEALTH_ACTION_POWER_ON;
            }
            return NS2_BT_HEALTH_ACTION_NONE;

        case NS2_BT_HEALTH_POWERING_ON:
            if (in->hci_working) {
                health->phase = NS2_BT_HEALTH_IDLE;
                health->phase_started_ms = now_ms;
                health->last_hci_event_ms = now_ms;
                health->recovery_completions++;
                return NS2_BT_HEALTH_ACTION_NONE;
            }
            if (elapsed(now_ms, health->phase_started_ms,
                        NS2_BT_HEALTH_POWER_ON_TIMEOUT_MS)) {
                return fail_to_reboot(health, now_ms);
            }
            return NS2_BT_HEALTH_ACTION_NONE;

        case NS2_BT_HEALTH_FAILED:
        default:
            return NS2_BT_HEALTH_ACTION_NONE;
    }
}

const char *ns2_bt_health_phase_name(ns2_bt_health_phase_t phase)
{
    switch (phase) {
        case NS2_BT_HEALTH_IDLE: return "idle";
        case NS2_BT_HEALTH_PROBE_PENDING: return "probe";
        case NS2_BT_HEALTH_POWERING_OFF: return "powering_off";
        case NS2_BT_HEALTH_WAIT_POWER_ON: return "wait_power_on";
        case NS2_BT_HEALTH_POWERING_ON: return "powering_on";
        case NS2_BT_HEALTH_FAILED: return "failed";
        default: return "unknown";
    }
}
