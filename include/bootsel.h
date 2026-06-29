#ifndef _BOOTSEL_H_
#define _BOOTSEL_H_

#include <stdint.h>
#include <stdbool.h>

// BOOTSEL-button gestures. The Pico's only button doubles as the flash chip
// select, so reading it at runtime requires briefly tri-stating that pin with
// the *other* core parked (see bootsel.c). Poll from the Bluetooth core; the USB
// core must have called multicore_lockout_victim_init() first.
typedef enum {
    BOOTSEL_NONE = 0,
    BOOTSEL_DOUBLE_TAP,   // enter pairing window
    BOOTSEL_TRIPLE_TAP,   // wipe saved Bluetooth devices
    BOOTSEL_HOLD,         // enter configuration mode (>= 5s)
} bootsel_gesture_t;

// Run the gesture state machine once. Call periodically (~30 ms) with the
// current millisecond timestamp. Returns a recognized gesture, else BOOTSEL_NONE.
bootsel_gesture_t bootsel_poll(uint32_t now_ms);

#endif  // _BOOTSEL_H_
