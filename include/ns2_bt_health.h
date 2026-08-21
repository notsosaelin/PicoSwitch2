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
    bool probe_failed;
    bool reboot_requested;
} ns2_bt_health_t;

void ns2_bt_health_init(ns2_bt_health_t *health, uint32_t now_ms);
void ns2_bt_health_note_hci_event(ns2_bt_health_t *health, uint32_t now_ms);
void ns2_bt_health_note_probe_complete(ns2_bt_health_t *health,
                                       uint32_t now_ms, uint8_t status);
ns2_bt_health_action_t ns2_bt_health_tick(ns2_bt_health_t *health,
                                          uint32_t now_ms,
                                          bool hci_working,
                                          bool hci_off,
                                          bool claimed_acl,
                                          bool probe_handle_available);
const char *ns2_bt_health_phase_name(ns2_bt_health_phase_t phase);

#ifdef __cplusplus
}
#endif

#endif
