#include <assert.h>
#include <stdio.h>

#include "ds5_native_haptics.h"

int main(void) {
    ds5_native_haptic_state_t state;
    ds5_native_haptics_reset(&state);
    assert(!ds5_native_haptics_stream_requested(&state));

    // A nonzero command starts native streaming and packet sends cannot age it
    // out while the console still holds that command.
    ds5_native_haptics_note_rumble(&state, 20, 0);
    assert(ds5_native_haptics_stream_requested(&state));
    ds5_native_haptics_packet_sent(&state);
    assert(ds5_native_haptics_stream_requested(&state));

    // STOP schedules exactly two packets. Repeated zero reports must not keep
    // extending the tail indefinitely.
    ds5_native_haptics_note_rumble(&state, 0, 0);
    assert(state.stop_packets == DS5_NATIVE_HAPTIC_STOP_PACKETS);
    ds5_native_haptics_note_rumble(&state, 0, 0);
    assert(state.stop_packets == DS5_NATIVE_HAPTIC_STOP_PACKETS);
    ds5_native_haptics_packet_sent(&state);
    assert(ds5_native_haptics_stream_requested(&state));
    ds5_native_haptics_packet_sent(&state);
    assert(!ds5_native_haptics_stream_requested(&state));

    // A new effect during the stop tail cancels the tail and remains live.
    ds5_native_haptics_note_rumble(&state, 0, 30);
    ds5_native_haptics_note_rumble(&state, 0, 0);
    ds5_native_haptics_packet_sent(&state);
    ds5_native_haptics_note_rumble(&state, 8, 9);
    assert(state.rumble_active);
    assert(state.stop_packets == 0);
    assert(ds5_native_haptics_stream_requested(&state));

    ds5_native_haptics_reset(&state);
    assert(!ds5_native_haptics_stream_requested(&state));

    puts("ds5_native_haptics: all tests passed");
    return 0;
}
