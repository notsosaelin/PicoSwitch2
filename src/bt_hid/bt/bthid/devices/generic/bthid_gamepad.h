// bthid_gamepad.h - Generic Bluetooth Gamepad Driver
// Handles basic HID gamepads over Bluetooth

#ifndef BTHID_GAMEPAD_H
#define BTHID_GAMEPAD_H

#include "bt/bthid/bthid.h"

// Generic gamepad driver
extern const bthid_driver_t bthid_gamepad_driver;

// Register the generic gamepad driver
void bthid_gamepad_register(void);

// Parse BLE HID descriptor for dynamic report field extraction
void bthid_gamepad_set_descriptor(bthid_device_t* device, const uint8_t* desc, uint16_t desc_len);

// Update VID-dependent flags (e.g., is_xbox) when VID/PID arrives after descriptor parsing
void bthid_gamepad_update_vid(bthid_device_t* device);

// Debug: format the parsed report-field map for a connection into a compact JSON fragment
// (no surrounding braces) for the `btid desc` config command — added 2026-07-12 to inspect
// real HID descriptor parsing results without guessing from symptomatic input behavior.
// Returns false (out set to "not-generic-or-no-map") if the device isn't on this driver or
// hasn't parsed a descriptor yet.
bool bthid_gamepad_dump_map(uint8_t conn_index, char* out, unsigned out_size);

#endif // BTHID_GAMEPAD_H
