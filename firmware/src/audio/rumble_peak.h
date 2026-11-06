#ifndef RUMBLE_PEAK_H
#define RUMBLE_PEAK_H

#include <stdint.h>

typedef struct {
    uint8_t left;
    uint8_t right;
} rumble_peak_t;

static inline void rumble_peak_push(rumble_peak_t *peak,
                                    uint8_t left,
                                    uint8_t right) {
    if (!peak) return;
    if (left > peak->left) peak->left = left;
    if (right > peak->right) peak->right = right;
}

static inline void rumble_peak_reset(rumble_peak_t *peak,
                                     uint8_t current_left,
                                     uint8_t current_right) {
    if (!peak) return;
    peak->left = current_left;
    peak->right = current_right;
}

// Return every peak accumulated since the previous take. Reset to the current
// rumble state rather than zero so a sustained command remains sustained even
// when no new console report arrives before the next audio packet.
static inline rumble_peak_t rumble_peak_take(rumble_peak_t *peak,
                                             uint8_t current_left,
                                             uint8_t current_right) {
    rumble_peak_t result = {current_left, current_right};
    if (!peak) return result;
    result = *peak;
    rumble_peak_reset(peak, current_left, current_right);
    return result;
}

#endif  // RUMBLE_PEAK_H
