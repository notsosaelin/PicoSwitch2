#ifndef NS2_BT_RECOVERY_RUNTIME_H
#define NS2_BT_RECOVERY_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A recovery reboot destroys every RAM counter that would explain it. On
// 2026-08-21 a watchdog reboot fired in the field with cause
// HCI_POWER_TIMEOUT, and the surviving evidence could not say WHICH power
// transition timed out, whether a probe had been attempted, or what the radio
// was doing at the time -- all of it lived in RAM. This snapshot is written
// into a watchdog scratch register immediately before the reboot and read back
// on the next boot, so one event is classifiable without asking the maintainer
// to reproduce it. Deliberately one 32-bit word, not a log.
typedef struct {
    uint8_t phase;             // ns2_bt_health_phase_t at escalation
    uint8_t probes_sent;       // saturating, 0..15
    uint8_t probe_failures;    // saturating, 0..15 (failed + timed out)
    uint8_t recovery_attempts; // saturating, 0..15
    uint8_t uptime_s;          // saturating, 0..127
    bool pairing_window_open;
    bool management_client;
    bool classic_link;
    bool ble_link;
    bool discovery_active;
    bool valid;
} ns2_bt_recovery_escalation_t;

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
    // What the radio was doing when the PREVIOUS boot escalated, if it did.
    ns2_bt_recovery_escalation_t last_escalation;
} ns2_bt_recovery_runtime_diag_t;

enum {
    NS2_BT_REBOOT_CAUSE_HCI_POWER_TIMEOUT = 1,
};

// Record the escalation context for the next boot. Called on the same code path
// as the reboot request, before it, so the snapshot always describes the event
// that caused the reboot rather than whatever followed.
void ns2_bt_recovery_note_escalation(const ns2_bt_recovery_escalation_t *state);

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
