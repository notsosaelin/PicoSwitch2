// bthid_gamepad_quirk_bitdo_m30.c - 8BitDo M30: Genesis/Saturn-style pad with NO analog
// triggers -- its L2/R2 are digital shoulder buttons that the HID report also exposes as
// trigger axes. Reporting them as analog L2/R2 makes the trigger output bypass button
// remapping (the analog axis isn't remapped), so users can't reassign L2/R2. Identity matching
// (name-primary, VID/PID-secondary) lives in bthid_gamepad_quirks.c's gamepad_quirks_identify()
// alongside every other quirk's match rule.
//
// M30 always reports <=14 buttons (no paddles), so it uses the plain sequential table directly
// -- never bthid_gamepad_quirk_bitdo_paddle.c's own buttonCnt-conditional selection, since this
// quirk's button count is fixed, not something that needs runtime branching.

#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"

extern const uint32_t SEQ_BUTTON_MAP[16];  // defined in bthid_gamepad_quirks.c

const gamepad_quirk_t QUIRK_BITDO_M30 = {
    .name = "bitdo_m30",
    .button_map = SEQ_BUTTON_MAP,
    .button_map_size = 16,
    // Controllers whose "triggers" are really digital shoulder buttons: the core engine drops
    // the synthesized analog value so L2/R2 come only from the digital buttons, which stay
    // subject to button remapping.
    .digital_shoulder_triggers = true,
};
