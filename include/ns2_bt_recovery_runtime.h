#ifndef NS2_BT_RECOVERY_RUNTIME_H
#define NS2_BT_RECOVERY_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool reboot_pending;
    bool reboot_suppressed;
    uint8_t consecutive_recovery_boots;
    uint8_t last_boot_cause;
    uint32_t reboot_requests;
    uint32_t core1_heartbeat_sequence;
    uint32_t core1_heartbeat_age_ms;
    uint32_t control_tick_age_ms;
    uint32_t control_tick_max_gap_ms;
} ns2_bt_recovery_runtime_diag_t;

enum {
    NS2_BT_REBOOT_CAUSE_HCI_POWER_TIMEOUT = 1,
};

void ns2_bt_recovery_runtime_init(void);
void ns2_bt_recovery_note_core1_activity(uint32_t now_ms);
void ns2_bt_recovery_note_control_tick(uint32_t now_ms);
void ns2_bt_recovery_request_reboot(uint8_t cause);
void ns2_bt_recovery_core0_service(void);
void ns2_bt_recovery_runtime_get_diag(ns2_bt_recovery_runtime_diag_t *out);

#ifdef __cplusplus
}
#endif

#endif
