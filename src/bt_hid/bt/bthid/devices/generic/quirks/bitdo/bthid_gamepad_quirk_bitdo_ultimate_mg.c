// bthid_gamepad_quirk_bitdo_ultimate_mg.c - 8BitDo Ultimate 2 (Mobile Gaming/"MG" variant),
// PID 0x200B specifically. Shares bthid_gamepad_quirk_bitdo_paddle.c's buttonCnt-conditional
// button-map selection (it's still a generic 8BitDo pad for that purpose) and adds its own 2
// back paddles, read from a fixed raw-report byte rather than bthid_gamepad_quirk_bitdo_paddle.c's
// usage 3/6 convention -- confirmed 2026-07-15 directly from the project owner's own raw-report
// capture, the same evidence-first approach already used for the Xbox Elite Series 2's back
// paddles. Kept as its own PID-specific quirk (not folded into bitdo_paddle) since this model's
// descriptor-derived button layout was not independently re-verified to match that other
// convention -- same "don't assume one 8BitDo model's layout for another" caution the NGC
// Modkit's own quirk already established.

#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"

extern void bitdo_select_button_map(uint8_t button_count, bool has_sim_triggers,
                                    const uint32_t **out_map, uint8_t *out_size);

// 8BitDo Ultimate 2 (Mobile Gaming/"MG", PID 0x200B): 2 back paddles, confirmed 2026-07-15
// via the project owner's own raw-report capture -- byte 8, bit 0x20 = left paddle, bit 0x04
// = right paddle (both bytes 0-7 and 9-10 identical between the two captures, only byte 8
// differs). Mapped to JP_BUTTON_L4/R4, the same generic "extra paddle" destinations the Xbox
// Elite Series 2 quirk already uses -- NS2_DEFAULT_MAP routes them to GL/GR in Pro2 mode.
static void extract_extra(const ble_report_map_t *map, const uint8_t *data, uint16_t len,
                          input_event_t *event)
{
    (void)map;
    if (len < 9) return;
    if (data[8] & 0x20) event->buttons |= JP_BUTTON_L4;
    if (data[8] & 0x04) event->buttons |= JP_BUTTON_R4;
}

const gamepad_quirk_t QUIRK_BITDO_ULTIMATE_MG = {
    .name = "bitdo_ultimate_mg",
    .select_button_map = bitdo_select_button_map,
    .extract_extra = extract_extra,
};
