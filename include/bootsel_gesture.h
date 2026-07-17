#ifndef _BOOTSEL_GESTURE_H_
#define _BOOTSEL_GESTURE_H_

#include <stdbool.h>
#include <stdint.h>

// Pure, host-testable BOOTSEL gesture recognizer. The Pico-specific code owns raw sampling and
// cross-core flash safety; this module only turns timestamped sampled button states into gestures.
#define BOOTSEL_TAP_WINDOW_MS 500u
#define BOOTSEL_HOLD_MS       5000u

typedef enum {
    BOOTSEL_NONE = 0,
    BOOTSEL_DOUBLE_TAP,
    BOOTSEL_TRIPLE_TAP,
    BOOTSEL_HOLD,
} bootsel_gesture_t;

typedef struct {
    bool was_pressed;
    uint32_t press_started_ms;
    bool hold_fired;
    uint8_t tap_count;
    uint32_t last_tap_ms;
} bootsel_gesture_state_t;

// Advance one recognizer step. A zero-initialized state is ready for use. `sample_valid=false`
// means the hardware sampler has not published its first value yet and must never fabricate an
// edge. Unsigned timestamp subtraction deliberately preserves behavior across uint32_t wrap.
bootsel_gesture_t bootsel_gesture_update(bootsel_gesture_state_t *state,
                                         bool sample_valid, bool pressed,
                                         uint32_t now_ms);

#endif  // _BOOTSEL_GESTURE_H_
