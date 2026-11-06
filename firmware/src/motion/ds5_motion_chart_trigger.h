#ifndef DS5_MOTION_CHART_TRIGGER_H
#define DS5_MOTION_CHART_TRIGGER_H

#include <stdbool.h>
#include <stdint.h>

// A diagnostic-only one-shot trigger for the genuine Pro Controller 2
// length-0x1E two-bit carrier chart. It deliberately knows nothing
// about the capture ring or UART so its transition behavior is host-testable.
#define DS5_MOTION_CHART_PRE_RECORDS 63u
#define DS5_MOTION_CHART_POST_RECORDS 64u
#define DS5_MOTION_CHART_ALL_STATES_MASK 0x0Fu
// States 0/1/3 now have direct transition and sign-branch evidence. State 2
// has never appeared in a retained genuine-controller capture.
#define DS5_MOTION_CHART_UNRESOLVED_STATES_MASK 0x04u

typedef struct {
    bool armed;
    bool baseline_valid;
    bool triggered;
    bool complete;
    uint8_t target_mask;
    uint8_t baseline_state;
    uint8_t trigger_state;
    uint16_t pre_records;
    uint16_t post_records;
} ds5_motion_chart_trigger_t;

typedef struct {
    bool trim_pretrigger_window;
    bool stop_after_record;
} ds5_motion_chart_trigger_event_t;

void ds5_motion_chart_trigger_reset(ds5_motion_chart_trigger_t *state);
void ds5_motion_chart_trigger_arm(ds5_motion_chart_trigger_t *state);
void ds5_motion_chart_trigger_arm_mask(
    ds5_motion_chart_trigger_t *state,
    uint8_t target_mask);

// Observe one exact native PDU before it is copied into the capture ring.
// Chart selection is carried only by length-0x1E byte 4 bits 0..1. Once the
// first target-state change is observed, this counts every following native
// PDU so the resulting window contains both 0x1E and interleaved 0x28 context.
// Ignored changes advance the baseline; they are not allowed to create a
// synthetic transition across an intervening chart.
ds5_motion_chart_trigger_event_t ds5_motion_chart_trigger_observe(
    ds5_motion_chart_trigger_t *state,
    const uint8_t *native,
    uint8_t native_length);

void ds5_motion_chart_trigger_set_pre_records(
    ds5_motion_chart_trigger_t *state,
    uint16_t retained);

#endif  // DS5_MOTION_CHART_TRIGGER_H
