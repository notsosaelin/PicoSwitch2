#ifndef BTHID_KEYBOARD_REPORT_H
#define BTHID_KEYBOARD_REPORT_H

#include <stdbool.h>
#include <stdint.h>

// Bluetooth HID keyboard report-descriptor scanner.
//
// This deliberately does NOT use the shared LUFA-derived parser in
// usb/usbh/hid/devices/generic/hid_parser.c, for two concrete reasons found
// while implementing keyboard support:
//
//  1. That parser's report-item filter (CALLBACK_HIDParser_FilterHIDReportItem)
//     rejects Usage Page 0x07 outright, so a keyboard descriptor produces zero
//     items and parses as a failure.
//  2. It expands every element of a Report Count into its own pooled item, and
//     the pool holds 50. A full NKRO keyboard declares one bit per usage --
//     232 items -- which would exhaust the pool and trip its assert().
//
// A keyboard descriptor is simple enough that a direct item walk is both
// smaller and safer than widening a shared parser used by every other device.

#define BTHID_KEYBOARD_USAGE_BYTES 32u  // covers HID usage page 0x07 ids 0x00..0xFF
#define BTHID_KEYBOARD_MAX_BITMAP_FIELDS 4u
#define BTHID_KEYBOARD_MAX_ARRAY_FIELDS 2u

// A run of one-bit key flags (boot-protocol modifier byte, or an NKRO bitmap).
typedef struct {
    uint16_t bit_offset;
    uint16_t count;
    uint8_t usage_min;
} bthid_keyboard_bitmap_field_t;

// A run of 8-bit key slots carrying the usage of a held key (boot-protocol
// 6-key rollover array).
typedef struct {
    uint16_t bit_offset;
    uint8_t count;
} bthid_keyboard_array_field_t;

typedef struct {
    uint8_t report_id;
    bool using_report_ids;
    uint8_t bitmap_count;
    uint8_t array_count;
    bthid_keyboard_bitmap_field_t bitmaps[BTHID_KEYBOARD_MAX_BITMAP_FIELDS];
    bthid_keyboard_array_field_t arrays[BTHID_KEYBOARD_MAX_ARRAY_FIELDS];
    // The descriptor also declares a Generic Desktop Joystick or Game Pad
    // application collection. Such a peer is a CONTROLLER that happens to carry
    // keyboard usages as well, not a keyboard -- see
    // bthid_keyboard_parse_descriptor().
    bool has_gamepad_collection;
} bthid_keyboard_report_map_t;

typedef enum {
    BTHID_KEYBOARD_DECODE_FAIL = 0,      // not a keyboard report / malformed
    BTHID_KEYBOARD_DECODE_OK = 1,
    // The keyboard reported ErrorRollOver: more keys are down than it can
    // encode, and it is not telling us which. The previous held set must be
    // retained rather than replaced with a wrong one.
    BTHID_KEYBOARD_DECODE_ROLLOVER = 2,
} bthid_keyboard_decode_t;

// True when the descriptor declares at least one keyboard input field and does
// not declare itself a controller.
//
// SCOPE: this is a discriminator for UNRESOLVED generic HID peers only. Whether
// a peer is a supported controller is decided by gamepad_quirks_identify(), the
// project's established identification machinery, and bthid.c consults that
// FIRST -- see bthid_gamepad_identity_unresolved(). Nothing supported is
// identified by this function.
//
// The Joystick (0x04) / Game Pad (0x05) application-collection check below is a
// secondary hazard guard for peers the quirk table cannot help with: an unknown
// controller may declare keyboard usages alongside its Game Pad collection (the
// share/profile-button pattern), and claiming it as a keyboard would strand it.
// `out->has_gamepad_collection` reports that case so a caller can tell "not a
// keyboard" from "a controller that also has keys".
bool bthid_keyboard_parse_descriptor(const uint8_t *desc, uint16_t desc_len,
                                     bthid_keyboard_report_map_t *out);

// Decode one input report into a held-usage bitmap (bit n = HID usage n held).
bthid_keyboard_decode_t bthid_keyboard_decode_report(
    const bthid_keyboard_report_map_t *map, const uint8_t *data, uint16_t len,
    uint8_t usage_bitmap[BTHID_KEYBOARD_USAGE_BYTES]);

// Boot-protocol fallback for a Classic keyboard whose report descriptor never
// arrived: [modifier][reserved][6 key slots].
bthid_keyboard_decode_t bthid_keyboard_decode_boot(
    const uint8_t *data, uint16_t len,
    uint8_t usage_bitmap[BTHID_KEYBOARD_USAGE_BYTES]);

#endif  // BTHID_KEYBOARD_REPORT_H
