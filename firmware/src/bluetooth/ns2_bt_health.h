#ifndef NS2_BT_HEALTH_H
#define NS2_BT_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NS2_BT_HEALTH_QUIET_BEFORE_PROBE_MS 10000u
#define NS2_BT_HEALTH_PROBE_TIMEOUT_MS      12000u
#define NS2_BT_HEALTH_POWER_OFF_TIMEOUT_MS   5000u
#define NS2_BT_HEALTH_POWER_ON_DELAY_MS       250u
#define NS2_BT_HEALTH_POWER_ON_TIMEOUT_MS    8000u

// A claimed ACL with no OPEN HCI handle is NORMAL, not a wedge: a link in
// connection setup, authentication, or teardown legitimately has no OPEN handle
// yet, and the owning state is cleared by an event that has not arrived. The
// original code escalated straight to a power cycle in that case, skipping the
// cheap probe that exists precisely to distinguish the two. Require a separate,
// longer confirmation instead, so only a condition that persists escalates.
#define NS2_BT_HEALTH_NO_HANDLE_GRACE_MS    10000u

// An admitted pairing/security procedure is deliberately quiet on the HCI event
// path for long stretches (the user is reading a pairing dialog, a peer is
// running SSP/SMP, inquiry is running). Suppress liveness escalation while one
// is in flight, but BOUND the suppression so a genuine wedge during pairing
// still recovers. Longer than PAIRING_WINDOW_MS (30 s) so an ordinary pairing
// never trips it, short enough that a wedge is still caught inside a minute.
#define NS2_BT_HEALTH_SECURITY_SUPPRESS_MAX_MS 45000u

typedef enum {
    NS2_BT_HEALTH_IDLE = 0,
    NS2_BT_HEALTH_PROBE_PENDING,
    NS2_BT_HEALTH_POWERING_OFF,
    NS2_BT_HEALTH_WAIT_POWER_ON,
    NS2_BT_HEALTH_POWERING_ON,
    NS2_BT_HEALTH_FAILED,
} ns2_bt_health_phase_t;

typedef enum {
    NS2_BT_HEALTH_ACTION_NONE = 0,
    NS2_BT_HEALTH_ACTION_SEND_PROBE,
    NS2_BT_HEALTH_ACTION_POWER_OFF,
    NS2_BT_HEALTH_ACTION_POWER_ON,
    NS2_BT_HEALTH_ACTION_REQUEST_REBOOT,
} ns2_bt_health_action_t;

typedef struct {
    ns2_bt_health_phase_t phase;
    uint32_t phase_started_ms;
    uint32_t last_hci_event_ms;
    uint32_t hci_event_sequence;
    uint32_t probe_event_sequence;
    uint32_t probes_sent;
    uint32_t probes_ok;
    uint32_t probes_failed;
    uint32_t probe_timeouts;
    uint32_t recovery_attempts;
    uint32_t recovery_completions;
    // Explicit "armed" flags rather than a 0 timestamp sentinel: now_ms is a
    // free-running millisecond clock that is legitimately 0 at boot and wraps
    // every 49.7 days, so 0 cannot mean "not set".
    uint32_t no_handle_since_ms;
    uint32_t security_suppress_since_ms;
    uint32_t security_suppressions;
    bool no_handle_armed;
    bool security_suppress_armed;
    bool probe_failed;
    bool reboot_requested;
} ns2_bt_health_t;

// Everything the escalation decision depends on, gathered by the caller.
// Passing a struct keeps the tick signature stable as inputs are added; the
// 2026-08-21 field incident showed the decision needs more context, not more
// positional booleans.
typedef struct {
    bool hci_working;
    bool hci_off;
    bool claimed_acl;
    bool probe_handle_available;
    // An admitted pairing/security procedure is running. Its HCI quiet is
    // expected, so it suppresses escalation for a bounded interval.
    bool security_in_flight;
} ns2_bt_health_inputs_t;

void ns2_bt_health_init(ns2_bt_health_t *health, uint32_t now_ms);
void ns2_bt_health_note_hci_event(ns2_bt_health_t *health, uint32_t now_ms);
void ns2_bt_health_note_probe_complete(ns2_bt_health_t *health,
                                       uint32_t now_ms, uint8_t status);
ns2_bt_health_action_t ns2_bt_health_tick(ns2_bt_health_t *health,
                                          uint32_t now_ms,
                                          const ns2_bt_health_inputs_t *in);
const char *ns2_bt_health_phase_name(ns2_bt_health_phase_t phase);

#ifdef __cplusplus
}
#endif

#endif
