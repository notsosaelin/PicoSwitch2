#include "ns2_owner_led.h"

#include <string.h>

static volatile uint8_t diag_reason = NS2_OWNER_LED_IDLE;
static volatile uint8_t diag_output_on;
static volatile uint32_t diag_last_transition_ms;
static volatile uint32_t diag_timer_max_gap_ms;

ns2_owner_led_reason_t ns2_owner_led_decide(ns2_owner_led_inputs_t inputs)
{
    if (inputs.mode_ack) return NS2_OWNER_LED_MODE_ACK;
    if (inputs.gc_diag) return NS2_OWNER_LED_GC_DIAG;
    if (inputs.config_mode) return NS2_OWNER_LED_CONFIG;
    if (inputs.wipe_active) return NS2_OWNER_LED_WIPE;
    if (inputs.pairing_active) return NS2_OWNER_LED_PAIRING;
    if (inputs.controller_ready) return NS2_OWNER_LED_CONNECTED;
    return NS2_OWNER_LED_IDLE;
}

static bool render_digit_burst(uint32_t elapsed_ms, uint8_t count)
{
    uint32_t burst_ms = (uint32_t)count * 300u;
    return elapsed_ms < burst_ms && (elapsed_ms % 300u) < 150u;
}

bool ns2_owner_led_render(ns2_owner_led_reason_t reason,
                          uint32_t elapsed_ms,
                          uint8_t flash_count,
                          uint8_t gc_stage,
                          uint8_t gc_bad_report_id)
{
    switch (reason) {
        case NS2_OWNER_LED_MODE_ACK: {
            uint32_t slot = elapsed_ms / 150u;
            return slot < (uint32_t)flash_count * 2u && (slot & 1u) == 0u;
        }
        case NS2_OWNER_LED_GC_DIAG:
            if (gc_stage == 255u) return true;
            if (gc_stage == 0u) return ((elapsed_ms / 750u) & 1u) == 0u;
            if (gc_stage == 21u) {
                uint8_t tens = gc_bad_report_id / 10u;
                uint8_t ones = gc_bad_report_id % 10u;
                uint32_t tens_ms = (uint32_t)tens * 300u;
                uint32_t ones_start = tens_ms + 1000u;
                uint32_t cycle_ms = ones_start + (uint32_t)ones * 300u + 1800u;
                uint32_t pos = cycle_ms ? elapsed_ms % cycle_ms : 0u;
                if (pos < tens_ms) return render_digit_burst(pos, tens);
                if (pos >= ones_start)
                    return render_digit_burst(pos - ones_start, ones);
                return false;
            } else {
                uint32_t burst_ms = (uint32_t)gc_stage * 1500u;
                uint32_t cycle_ms = burst_ms + 10000u;
                uint32_t pos = elapsed_ms % cycle_ms;
                return pos < burst_ms && (pos % 1500u) < 300u;
            }
        case NS2_OWNER_LED_CONFIG:
            return ((elapsed_ms / 500u) & 1u) == 0u;
        case NS2_OWNER_LED_WIPE:
            return ((elapsed_ms / 60u) & 1u) == 0u;
        case NS2_OWNER_LED_PAIRING:
            return ((elapsed_ms / 120u) & 1u) == 0u;
        case NS2_OWNER_LED_CONNECTED:
            return true;
        case NS2_OWNER_LED_IDLE:
        default:
            return (elapsed_ms % 10000u) < 90u;
    }
}

const char *ns2_owner_led_reason_name(ns2_owner_led_reason_t reason)
{
    switch (reason) {
        case NS2_OWNER_LED_MODE_ACK: return "mode_ack";
        case NS2_OWNER_LED_GC_DIAG: return "gc_diag";
        case NS2_OWNER_LED_CONFIG: return "config";
        case NS2_OWNER_LED_WIPE: return "wipe";
        case NS2_OWNER_LED_PAIRING: return "pairing";
        case NS2_OWNER_LED_CONNECTED: return "controller_ready";
        case NS2_OWNER_LED_IDLE: return "idle";
        default: return "unknown";
    }
}

void ns2_owner_led_track_output(ns2_owner_led_output_state_t *state,
                                bool output_on,
                                uint32_t now_ms)
{
    if (!state || state->output_on == output_on) return;
    state->output_on = output_on;
    state->last_transition_ms = now_ms;
}

void ns2_owner_led_diag_publish(ns2_owner_led_reason_t reason,
                                bool output_on,
                                uint32_t last_transition_ms,
                                uint32_t timer_max_gap_ms)
{
    __atomic_store_n(&diag_reason, (uint8_t)reason, __ATOMIC_RELAXED);
    __atomic_store_n(&diag_output_on, output_on ? 1u : 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&diag_last_transition_ms, last_transition_ms,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&diag_timer_max_gap_ms, timer_max_gap_ms,
                     __ATOMIC_RELEASE);
}

void ns2_owner_led_diag_snapshot(ns2_owner_led_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->timer_max_gap_ms =
        __atomic_load_n(&diag_timer_max_gap_ms, __ATOMIC_ACQUIRE);
    out->reason = __atomic_load_n(&diag_reason, __ATOMIC_RELAXED);
    out->output_on =
        __atomic_load_n(&diag_output_on, __ATOMIC_RELAXED) != 0u;
    out->last_transition_ms =
        __atomic_load_n(&diag_last_transition_ms, __ATOMIC_RELAXED);
}
