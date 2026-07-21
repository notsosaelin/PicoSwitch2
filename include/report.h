#ifndef _REPORT_H_
#define _REPORT_H_

#include <stdint.h>
#include <stdbool.h>

#include "switch_pro.h"

// Cross-core shared state, serialized with a hardware critical section.
//   Input  (BT core -> USB core): each controller's button/stick/IMU state.
//   Rumble (USB core -> BT core): each controller's rumble amplitude (0..255),
//                                 forwarded to the physical controller.

// Must be called once from core0 before launching core1.
void report_init(void);

// Input: published by the Bluetooth core, consumed by the USB core.
void set_global_gamepad_input(uint8_t idx, const switch_pro_input_t *in);
void get_global_gamepad_input(uint8_t idx, switch_pro_input_t *out);

// Mouse reports are relative events, not persistent axes. Accumulate producer
// deltas under the cross-core lock and consume them exactly once from the USB
// report loop.
void accumulate_global_mouse_input(uint8_t idx, const switch_pro_input_t *in);
void take_global_gamepad_input(uint8_t idx, switch_pro_input_t *out);

// Raw controller buttons (unified JP_BUTTON_* bitmap, before remap) — published by
// the BT core alongside the mapped input, read by config mode's live-view for the
// "controller input" column. 0 when no controller is connected to that slot.
void set_global_raw_buttons(uint8_t idx, uint32_t jp_buttons);
uint32_t get_global_raw_buttons(uint8_t idx);

// Connected controller identity (name + USB VID/PID from SDP), published by the BT
// core, read by config mode to label the "Current Input Type" panel. Empty name =
// nothing connected in that slot.
void set_global_device(uint8_t idx, const char *name, uint16_t vid, uint16_t pid);
void get_global_device(uint8_t idx, char *name_out, uint16_t name_len, uint16_t *vid, uint16_t *pid);

// Raw HID input report (first RAW_REPORT_BYTES bytes) as received from the controller,
// captured by the BT stack before the driver parses it. For config mode's raw-report
// debug view — used to reverse-engineer inputs a driver doesn't parse yet (e.g. Xbox
// Elite paddles). Published by the BT core, read by config mode.
#define RAW_REPORT_BYTES 48
void set_global_raw_report(uint8_t idx, const uint8_t *data, uint16_t len);
uint16_t get_global_raw_report(uint8_t idx, uint8_t *out, uint16_t maxlen);

// Rumble: published by the USB core (decoded from console reports), consumed by
// the Bluetooth core (forwarded to the controller). Left/right carry each HD-rumble
// motor's amplitude independently — both Switch 1 and Switch 2 rumble packets encode
// two distinct motors; forwarding them separately lets joypad-os drivers that support
// independent per-motor amplitude (feedback_set_rumble()'s left/right, e.g. DualSense,
// Xbox) preserve stereo separation instead of both motors buzzing identically.
void report_set_rumble(uint8_t idx, uint8_t left, uint8_t right);
void report_get_rumble(uint8_t idx, uint8_t *left, uint8_t *right);

// Generation-counted variant -- Confirmed necessary 2026-07-14 (docs/experiments/
// gcusb-rumble-lab-2026-07-14.md): plain left/right value comparison in a downstream consumer
// (ns2_seam.c's feedback_get_state()) cannot distinguish "no change happened" from "it changed
// and changed back before I last polled" -- both look identical if the value ends up equal to
// what the consumer last saw. If an intervening nonzero excursion was ever forwarded to a
// BT-connected controller with a long hardware sustain (e.g. Xbox's ~10-minute
// pulse_sustain_10ms/loop_count, see bthid_gamepad.c), a same-value "no visible change" read can
// silently mean a stop was never re-affirmed. `generation` increments on every
// report_set_rumble() call regardless of whether the value actually differs from the previous
// one, so a consumer comparing generations (not raw values) can never miss a transition merely
// because it happened to poll after the value had already round-tripped back to something it
// had seen before.
void report_get_rumble_gen(uint8_t idx, uint8_t *left, uint8_t *right, uint32_t *generation);

// Audio-native haptics run at a slower packet cadence than Switch 2 report
// 0x02. Return and reset the maximum scalar observed since the previous audio
// packet so short Nintendo peaks are not discarded by latest-value sampling.
void report_take_rumble_audio_peak(uint8_t idx, uint8_t *left, uint8_t *right);
void report_reset_rumble_audio_peak(uint8_t idx);

// Player LEDs: raw Switch 2 command-0x09 wire bitfield, published by the active
// USB personality and consumed by the Bluetooth feedback seam. Generation is
// incremented for every received assignment, including repeated values.
void report_set_player_leds(uint8_t idx, uint8_t wire_mask);
void report_get_player_leds(uint8_t idx, uint8_t *wire_mask, uint32_t *generation);

// True if any controller currently has a button pressed (used to wake the
// console from sleep via USB remote wakeup).
bool report_any_button_pressed(void);

#endif  // _REPORT_H_
