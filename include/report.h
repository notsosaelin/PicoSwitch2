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

// Raw controller buttons (unified JP_BUTTON_* bitmap, before remap) — published by
// the BT core alongside the mapped input, read by config mode's live-view for the
// "controller input" column. 0 when no controller is connected to that slot.
void set_global_raw_buttons(uint8_t idx, uint32_t jp_buttons);
uint32_t get_global_raw_buttons(uint8_t idx);

// Rumble: published by the USB core (decoded from console reports), consumed by
// the Bluetooth core (forwarded to the controller).
void report_set_rumble(uint8_t idx, uint8_t amplitude);
uint8_t report_get_rumble(uint8_t idx);

// True if any controller currently has a button pressed (used to wake the
// console from sleep via USB remote wakeup).
bool report_any_button_pressed(void);

#endif  // _REPORT_H_
