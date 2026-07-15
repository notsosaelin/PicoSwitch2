#ifndef SWITCH_GC_RUMBLE_DECODE_H
#define SWITCH_GC_RUMBLE_DECODE_H

#include <stdint.h>

typedef enum {
    SWITCH_GC_RUMBLE_NO_CHANGE = 0,
    SWITCH_GC_RUMBLE_OFF,
    SWITCH_GC_RUMBLE_ON,
    SWITCH_GC_RUMBLE_STOP,
} switch_gc_rumble_command_t;

// Decode the four data bytes following USB report ID 0x03. The project's
// genuine capture contains both 02 00 (STOP), 00 01 (active), and 00 00 (OFF)
// in bytes 1/2 after the sequence byte, while host-side documentation also
// describes 01 00 as ON. Accept both active placements until a controlled
// capture resolves why the USB forms differ.
switch_gc_rumble_command_t switch_gc_decode_rumble(uint8_t report_id,
                                                    const uint8_t *data,
                                                    uint16_t len);

#endif  // SWITCH_GC_RUMBLE_DECODE_H
