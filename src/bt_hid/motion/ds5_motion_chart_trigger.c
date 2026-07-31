#include "ds5_motion_chart_trigger.h"

#include <string.h>

void ds5_motion_chart_trigger_reset(ds5_motion_chart_trigger_t *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

void ds5_motion_chart_trigger_arm(ds5_motion_chart_trigger_t *state)
{
    ds5_motion_chart_trigger_arm_mask(
        state, DS5_MOTION_CHART_ALL_STATES_MASK);
}

void ds5_motion_chart_trigger_arm_mask(
    ds5_motion_chart_trigger_t *state,
    uint8_t target_mask)
{
    if (!state) return;
    ds5_motion_chart_trigger_reset(state);
    state->armed = true;
    state->target_mask =
        target_mask & DS5_MOTION_CHART_ALL_STATES_MASK;
}

ds5_motion_chart_trigger_event_t ds5_motion_chart_trigger_observe(
    ds5_motion_chart_trigger_t *state,
    const uint8_t *native,
    uint8_t native_length)
{
    ds5_motion_chart_trigger_event_t event = {0};
    if (!state || !state->armed || !native ||
        (native_length != 0x1Eu && native_length != 0x28u)) {
        return event;
    }

    if (!state->triggered) {
        if (native_length != 0x1Eu) return event;

        uint8_t const chart = native[4] & 0x03u;
        if (!state->baseline_valid) {
            state->baseline_valid = true;
            state->baseline_state = chart;
            return event;
        }
        if (chart == state->baseline_state) return event;
        if ((state->target_mask & (1u << chart)) == 0u &&
            (state->target_mask & (1u << state->baseline_state)) == 0u) {
            state->baseline_state = chart;
            return event;
        }

        state->triggered = true;
        state->trigger_state = chart;
        state->post_records = 1u;  // The transition PDU is the first post record.
        event.trim_pretrigger_window = true;
        return event;
    }

    if (state->post_records < UINT16_MAX) state->post_records++;
    if (state->post_records >= DS5_MOTION_CHART_POST_RECORDS) {
        state->armed = false;
        state->complete = true;
        event.stop_after_record = true;
    }
    return event;
}

void ds5_motion_chart_trigger_set_pre_records(
    ds5_motion_chart_trigger_t *state,
    uint16_t retained)
{
    if (state) state->pre_records = retained;
}
