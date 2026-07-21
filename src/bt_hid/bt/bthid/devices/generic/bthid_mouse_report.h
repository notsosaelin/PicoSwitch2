#ifndef BTHID_MOUSE_REPORT_H
#define BTHID_MOUSE_REPORT_H

#include <stdbool.h>
#include <stdint.h>

#define BTHID_MOUSE_MAX_BUTTONS 16

typedef struct {
    uint16_t bit_offset;
    uint8_t bit_size;
    bool present;
} bthid_mouse_field_t;

typedef struct {
    uint8_t report_id;
    bool using_report_ids;
    bthid_mouse_field_t x;
    bthid_mouse_field_t y;
    bthid_mouse_field_t wheel;
    bthid_mouse_field_t buttons[BTHID_MOUSE_MAX_BUTTONS];
    uint8_t button_count;
} bthid_mouse_report_map_t;

typedef struct {
    int16_t delta_x;
    int16_t delta_y;
    int8_t wheel;
    uint16_t buttons;
} bthid_mouse_report_t;

// A mouse report is identified structurally: one report ID contains relative
// Generic Desktop X and Y fields. This works for both Classic HID and BLE HOGP
// descriptors and avoids trusting device names.
bool bthid_mouse_parse_descriptor(const uint8_t *desc, uint16_t desc_len,
                                  bthid_mouse_report_map_t *out);

bool bthid_mouse_decode_report(const bthid_mouse_report_map_t *map,
                               const uint8_t *data, uint16_t len,
                               bthid_mouse_report_t *out);

#endif
