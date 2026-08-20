#ifndef NS2_OWNER_LED_H
#define NS2_OWNER_LED_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NS2_OWNER_LED_MODE_ACK = 0,
    NS2_OWNER_LED_GC_DIAG,
    NS2_OWNER_LED_CONFIG,
    NS2_OWNER_LED_WIPE,
    NS2_OWNER_LED_PAIRING,
    NS2_OWNER_LED_CONNECTED,
    NS2_OWNER_LED_IDLE,
} ns2_owner_led_reason_t;

typedef struct {
    bool mode_ack;
    bool gc_diag;
    bool config_mode;
    bool wipe_active;
    bool pairing_active;
    bool controller_ready;
} ns2_owner_led_inputs_t;

typedef struct {
    uint8_t reason;
    bool output_on;
    uint32_t last_transition_ms;
    uint32_t timer_max_gap_ms;
} ns2_owner_led_diag_t;

ns2_owner_led_reason_t ns2_owner_led_decide(ns2_owner_led_inputs_t inputs);
bool ns2_owner_led_render(ns2_owner_led_reason_t reason,
                          uint32_t elapsed_ms,
                          uint8_t flash_count,
                          uint8_t gc_stage,
                          uint8_t gc_bad_report_id);
const char *ns2_owner_led_reason_name(ns2_owner_led_reason_t reason);

// Core 1 publishes one bounded snapshot; core 0 diagnostics consume it.
void ns2_owner_led_diag_publish(ns2_owner_led_reason_t reason,
                                bool output_on,
                                uint32_t last_transition_ms,
                                uint32_t timer_max_gap_ms);
void ns2_owner_led_diag_snapshot(ns2_owner_led_diag_t *out);

#endif
