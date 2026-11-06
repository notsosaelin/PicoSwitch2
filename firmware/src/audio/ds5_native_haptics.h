#ifndef DS5_NATIVE_HAPTICS_H
#define DS5_NATIVE_HAPTICS_H

#include <stdbool.h>
#include <stdint.h>

// A STOP can arrive after the peak accumulator has already retained the prior
// nonzero command. The first native packet may therefore carry that final peak;
// the second is guaranteed to carry zero.
#define DS5_NATIVE_HAPTIC_STOP_PACKETS 2u

typedef struct {
    bool rumble_active;
    uint8_t stop_packets;
} ds5_native_haptic_state_t;

static inline void ds5_native_haptics_reset(
    ds5_native_haptic_state_t *state) {
    if (!state) return;
    state->rumble_active = false;
    state->stop_packets = 0;
}

static inline void ds5_native_haptics_note_rumble(
    ds5_native_haptic_state_t *state, uint8_t left, uint8_t right) {
    if (!state) return;
    bool const was_active = state->rumble_active;
    state->rumble_active = left != 0 || right != 0;
    if (state->rumble_active) {
        state->stop_packets = 0;
    } else if (was_active) {
        state->stop_packets = DS5_NATIVE_HAPTIC_STOP_PACKETS;
    }
}

static inline bool ds5_native_haptics_stream_requested(
    const ds5_native_haptic_state_t *state) {
    return state &&
           (state->rumble_active || state->stop_packets != 0);
}

static inline void ds5_native_haptics_packet_sent(
    ds5_native_haptic_state_t *state) {
    if (!state || state->rumble_active || state->stop_packets == 0) return;
    state->stop_packets--;
}

#endif  // DS5_NATIVE_HAPTICS_H
