#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ds5_motion_chart_trigger.h"

static void make_motion30(uint8_t pdu[0x1E], uint8_t chart)
{
    for (unsigned i = 0; i < 0x1Eu; ++i) pdu[i] = 0;
    pdu[4] = chart & 3u;
}

int main(void)
{
    ds5_motion_chart_trigger_t state;
    uint8_t pdu30[0x1E];
    uint8_t pdu40[0x28] = {0};

    ds5_motion_chart_trigger_arm(&state);
    assert(state.armed && !state.triggered);

    make_motion30(pdu30, 1);
    ds5_motion_chart_trigger_event_t event =
        ds5_motion_chart_trigger_observe(&state, pdu30, sizeof(pdu30));
    assert(!event.trim_pretrigger_window);
    assert(state.baseline_valid && state.baseline_state == 1);

    // A 0x28 packet has no chart selector and must not trigger.
    event = ds5_motion_chart_trigger_observe(&state, pdu40, sizeof(pdu40));
    assert(!event.trim_pretrigger_window && !state.triggered);

    make_motion30(pdu30, 2);
    event = ds5_motion_chart_trigger_observe(&state, pdu30, sizeof(pdu30));
    assert(event.trim_pretrigger_window);
    assert(state.triggered && state.trigger_state == 2);
    assert(state.post_records == 1);
    ds5_motion_chart_trigger_set_pre_records(&state, 63);

    for (unsigned i = 1; i < DS5_MOTION_CHART_POST_RECORDS; ++i) {
        event = ds5_motion_chart_trigger_observe(
            &state, (i & 1u) ? pdu40 : pdu30,
            (i & 1u) ? sizeof(pdu40) : sizeof(pdu30));
        assert(event.stop_after_record ==
               (i + 1u == DS5_MOTION_CHART_POST_RECORDS));
    }

    assert(state.complete && !state.armed);
    assert(state.pre_records == 63);
    assert(state.post_records == 64);

    // An opportunistic unresolved-state trigger ignores states 0/1/3,
    // advances its baseline, and freezes only when unseen state 2 participates.
    ds5_motion_chart_trigger_arm_mask(
        &state, DS5_MOTION_CHART_UNRESOLVED_STATES_MASK);
    assert(state.target_mask == DS5_MOTION_CHART_UNRESOLVED_STATES_MASK);
    make_motion30(pdu30, 0);
    event = ds5_motion_chart_trigger_observe(
        &state, pdu30, sizeof(pdu30));
    assert(state.baseline_valid && state.baseline_state == 0);
    make_motion30(pdu30, 3);
    event = ds5_motion_chart_trigger_observe(
        &state, pdu30, sizeof(pdu30));
    assert(!event.trim_pretrigger_window && !state.triggered);
    assert(state.baseline_state == 3);
    make_motion30(pdu30, 0);
    event = ds5_motion_chart_trigger_observe(
        &state, pdu30, sizeof(pdu30));
    assert(!event.trim_pretrigger_window && !state.triggered);
    assert(state.baseline_state == 0);
    make_motion30(pdu30, 1);
    event = ds5_motion_chart_trigger_observe(
        &state, pdu30, sizeof(pdu30));
    assert(!event.trim_pretrigger_window && !state.triggered);
    assert(state.baseline_state == 1);
    make_motion30(pdu30, 3);
    event = ds5_motion_chart_trigger_observe(
        &state, pdu30, sizeof(pdu30));
    assert(!event.trim_pretrigger_window && !state.triggered);
    assert(state.baseline_state == 3);
    make_motion30(pdu30, 2);
    event = ds5_motion_chart_trigger_observe(
        &state, pdu30, sizeof(pdu30));
    assert(event.trim_pretrigger_window && state.triggered);
    assert(state.baseline_state == 3 && state.trigger_state == 2);

    puts("ds5 motion chart trigger tests passed");
    return 0;
}
