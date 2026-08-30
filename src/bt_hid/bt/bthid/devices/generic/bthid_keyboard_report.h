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

    // The Generic Desktop usage of the FIRST top-level application collection,
    // or 0 when the descriptor opens with something else.
    //
    // This is the closest a descriptor comes to a Class of Device: the device
    // stating what it primarily is. A keyboard opens with Usage(Keyboard); a
    // mouse opens with Usage(Mouse) and declares its macro keys afterwards.
    // BLE has no Class of Device, so this is the only self-declaration
    // available there.
    uint8_t primary_application_usage;
} bthid_keyboard_report_map_t;

// Generic Desktop application usages worth naming.
#define BTHID_HID_USAGE_MOUSE 0x02u
#define BTHID_HID_USAGE_KEYBOARD 0x06u
#define BTHID_HID_USAGE_KEYPAD 0x07u

// How strongly a descriptor argues that its owner is a real keyboard.
//
// Exists to answer one question the capability bits cannot: a peer declaring
// BOTH keyboard and pointer capability is either a keyboard with a pointing
// extra, or a mouse with macro keys, and those need opposite roles.
typedef struct {
    // Total key slots across the rollover array fields. A boot keyboard
    // declares six; a macro pad declares one or two, or none at all.
    uint16_t rollover_slots;

    // Key-bitmap bits EXCLUDING the modifier byte. An NKRO keyboard reports
    // one bit per usage here and may have no array at all.
    uint16_t key_bitmap_bits;

    // A standard 8-bit modifier field over usages 0xE0..0xE7.
    bool has_modifier_byte;

    // The descriptor opens with Usage(Keyboard) or Usage(Keypad).
    bool keyboard_is_primary_collection;

    // All of the above agreeing. See bthid_keyboard_shape().
    bool strong_keyboard;
} bthid_keyboard_shape_t;

// A real keyboard's shape needs at least this many rollover slots, or this many
// key-bitmap bits for an NKRO board that has no array.
#define BTHID_KEYBOARD_STRONG_ROLLOVER_SLOTS 4u
#define BTHID_KEYBOARD_STRONG_BITMAP_BITS 32u

// Measure the keyboard evidence in a parsed map.
//
// Pure and host-testable, and deliberately conservative: everything it needs is
// already recorded by bthid_keyboard_parse_descriptor(), and an ambiguous
// descriptor must produce strong_keyboard = false so the caller keeps its
// existing behaviour.
bthid_keyboard_shape_t bthid_keyboard_shape(const bthid_keyboard_report_map_t *map);

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
