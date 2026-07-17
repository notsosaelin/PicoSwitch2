#include "bootsel_gesture.h"

bootsel_gesture_t bootsel_gesture_update(bootsel_gesture_state_t *state,
                                         bool sample_valid, bool pressed,
                                         uint32_t now_ms) {
    if (!sample_valid)
        return BOOTSEL_NONE;

    if (pressed && !state->was_pressed) {
        state->press_started_ms = now_ms;
        state->hold_fired = false;
    }

    if (pressed && !state->hold_fired &&
        (now_ms - state->press_started_ms) >= BOOTSEL_HOLD_MS) {
        state->hold_fired = true;
        state->tap_count = 0;
        state->was_pressed = pressed;
        return BOOTSEL_HOLD;
    }

    if (!pressed && state->was_pressed) {
        if (!state->hold_fired) {
            state->tap_count++;
            state->last_tap_ms = now_ms;
        }
        state->hold_fired = false;
    }
    state->was_pressed = pressed;

    if (state->tap_count > 0 &&
        (now_ms - state->last_tap_ms) >= BOOTSEL_TAP_WINDOW_MS) {
        uint8_t count = state->tap_count;
        state->tap_count = 0;
        if (count == 2)
            return BOOTSEL_DOUBLE_TAP;
        if (count >= 3)
            return BOOTSEL_TRIPLE_TAP;
    }
    return BOOTSEL_NONE;
}
