// bthid_gamepad_quirk_xbox_elite2.c - Xbox Elite Series 2 (PID 0x0B05 wired / 0x0B22 Bluetooth)
// quirk profile. Shares QUIRK_XBOX's button-map selection and rumble implementation (it's still
// an ordinary Xbox pad for those purposes) and only adds its own 4 back paddles.
//
// Reserved naming note: a future Xbox Elite 1 or Elite 3 gets its own
// bthid_gamepad_quirk_xbox_elite1.c / _elite3.c file with its own confirmed byte layout --
// never folded into this file or into "xbox" just because they're all "Elite" or all "Xbox".

#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"

extern void xbox_select_button_map(uint8_t button_count, bool has_sim_triggers,
                                    const uint32_t **out_map, uint8_t *out_size);
extern void xbox_extract_extra(const ble_report_map_t *map, const uint8_t *data, uint16_t len,
                               input_event_t *event);
extern bool xbox_send_rumble(uint8_t conn_index, uint8_t left, uint8_t right);

// Xbox Elite Series 2: the 4 back paddles live in the last report byte (bits 0-3).
// They report raw ONLY when left UNMAPPED in the active on-board profile (a mapped
// paddle sends its assigned button instead). Captured on hardware in byte 19 of a
// 20-byte report: R4=0x01, R5=0x02, L4=0x04, L5=0x08. Left paddles -> GL, right -> GR.
// (Byte 17 = active profile 0-3; not mapped — it's a mode selector, not a button.)
//
// Xbox Elite Series 2: 20-byte report with the 4 paddles in byte 19 (R4=0x01,
// R5=0x02, L4=0x04, L5=0x08). Detected by "Xbox + 20-byte report" rather than the
// exact PID (which the BLE PnP query often fails to resolve) in QUIRK_XBOX's own
// extract_extra() fallback; this function is the single shared implementation both quirks
// call, so they can't drift apart on the byte layout. Regular Xbox pads send 16-byte reports
// so they never hit this. Paddles report raw only when the active on-board profile leaves
// them unmapped.
void xbox_elite2_extract_paddles(const uint8_t *data, uint16_t len, input_event_t *event)
{
    if (len < 20) return;
    uint8_t pad = data[19];
    if (pad & 0x04) event->buttons |= JP_BUTTON_L4;  // upper-left  paddle -> GL
    if (pad & 0x08) event->buttons |= JP_BUTTON_L5;  // lower-left  paddle -> GL
    if (pad & 0x01) event->buttons |= JP_BUTTON_R4;  // upper-right paddle -> GR
    if (pad & 0x02) event->buttons |= JP_BUTTON_R5;  // lower-right paddle -> GR
}

const gamepad_quirk_t QUIRK_XBOX_ELITE2 = {
    .name = "xbox_elite2",
    .select_button_map = xbox_select_button_map,
    .extract_extra = xbox_extract_extra,
    .send_rumble = xbox_send_rumble,
};
