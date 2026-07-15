#include "switch_gc_rumble_decode.h"

switch_gc_rumble_command_t switch_gc_decode_rumble(uint8_t report_id,
                                                    const uint8_t *data,
                                                    uint16_t len) {
    // Empty interrupt slots carry no new command. Pulse expiry in switch_gc.c
    // provides the bounded stop; a ZLP must never prolong an active pulse.
    if (report_id == 0 && len == 0) return SWITCH_GC_RUMBLE_NO_CHANGE;
    if (report_id != 0x03) return SWITCH_GC_RUMBLE_NO_CHANGE;
    if (!data || len < 3) return SWITCH_GC_RUMBLE_STOP;

    // STOP wins if either candidate state byte requests it.
    if (data[1] == 2 || data[2] == 2) return SWITCH_GC_RUMBLE_STOP;

    // The genuine USB capture's active packets are `seq 00 01 00`.
    // Also retain `seq 01 00 00`, the previously documented host form.
    if (data[1] == 1 || data[2] == 1) return SWITCH_GC_RUMBLE_ON;

    if (data[1] == 0 && data[2] == 0) return SWITCH_GC_RUMBLE_OFF;
    return SWITCH_GC_RUMBLE_STOP;
}
