// Bluetooth HID keyboard report-descriptor and report-decode tests.
//
// Host-only. The descriptors below are the two shapes real Bluetooth keyboards
// actually ship: the 8+1+6 boot layout (with and without a report ID) and an
// NKRO bitmap.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bt/bthid/devices/generic/bthid_keyboard_report.h"
#include "fixtures/composite_gamepad_keyboard_hid.h"

#define KEY_A 0x04u
#define KEY_B 0x05u
#define KEY_W 0x1Au
#define KEY_LCTRL 0xE0u
#define KEY_LSHIFT 0xE1u

// Standard boot keyboard, no report IDs.
static const uint8_t DESC_BOOT[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0xE0,        //   Usage Minimum (LeftControl)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Cnst)  -- reserved byte
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array)
    0xC0               // End Collection
};

// Same, behind report ID 1.
static const uint8_t DESC_BOOT_ID[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0
};

// NKRO bitmap: modifiers plus one bit per usage 0x00..0x67.
static const uint8_t DESC_NKRO[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x67,        //   Usage Maximum (0x67)
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x68,        //   Report Count (104)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0xC0
};

// A gamepad: relative and absolute axes plus buttons, no keyboard usages.
static const uint8_t DESC_GAMEPAD[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0xC0
};

static bool held(const uint8_t *bitmap, uint8_t usage) {
    return (bitmap[usage >> 3] & (uint8_t)(1u << (usage & 7u))) != 0u;
}

static unsigned held_count(const uint8_t *bitmap) {
    unsigned count = 0;
    for (unsigned i = 0; i < BTHID_KEYBOARD_USAGE_BYTES; ++i) {
        for (unsigned bit = 0; bit < 8u; ++bit)
            if (bitmap[i] & (1u << bit)) count++;
    }
    return count;
}

static void test_classification(void) {
    bthid_keyboard_report_map_t map;
    assert(bthid_keyboard_parse_descriptor(DESC_BOOT, sizeof(DESC_BOOT), &map));
    assert(!map.using_report_ids);
    assert(map.bitmap_count == 1 && map.array_count == 1);
    assert(map.bitmaps[0].bit_offset == 0 && map.bitmaps[0].count == 8);
    assert(map.bitmaps[0].usage_min == 0xE0u);
    assert(map.arrays[0].bit_offset == 16 && map.arrays[0].count == 6);

    assert(bthid_keyboard_parse_descriptor(DESC_BOOT_ID, sizeof(DESC_BOOT_ID),
                                           &map));
    assert(map.using_report_ids && map.report_id == 1u);
    // Offsets exclude the report-id byte; the decoder adds it back.
    assert(map.bitmaps[0].bit_offset == 0 && map.arrays[0].bit_offset == 16);

    assert(bthid_keyboard_parse_descriptor(DESC_NKRO, sizeof(DESC_NKRO), &map));
    assert(map.bitmap_count == 2 && map.array_count == 0);
    assert(map.bitmaps[1].bit_offset == 8 && map.bitmaps[1].usage_min == 0x00u);
    assert(map.bitmaps[1].count == 0x68u);

    // A gamepad must never be claimed as a keyboard.
    assert(!bthid_keyboard_parse_descriptor(DESC_GAMEPAD, sizeof(DESC_GAMEPAD),
                                            &map));

    // Regression (Xbox Elite): a controller that ALSO declares a keyboard
    // collection is still a controller. Claiming it as a keyboard replaced its
    // gamepad driver and left the whole pad producing nothing.
    assert(!bthid_keyboard_parse_descriptor(COMPOSITE_GAMEPAD_KEYBOARD_HID,
                                            sizeof(COMPOSITE_GAMEPAD_KEYBOARD_HID),
                                            &map));
    // ...and the reason must be the controller collection, not a failure to see
    // the keyboard fields. Both are true here, which is exactly the trap.
    assert(map.has_gamepad_collection);
    assert(map.bitmap_count > 0 || map.array_count > 0);

    assert(!bthid_keyboard_parse_descriptor(COMPOSITE_JOYSTICK_KEYBOARD_HID,
                                            sizeof(COMPOSITE_JOYSTICK_KEYBOARD_HID),
                                            &map));
    assert(map.has_gamepad_collection);

    // A real keyboard is unaffected: no controller collection, still claimed.
    assert(bthid_keyboard_parse_descriptor(DESC_BOOT, sizeof(DESC_BOOT), &map));
    assert(!map.has_gamepad_collection);
    assert(bthid_keyboard_parse_descriptor(DESC_NKRO, sizeof(DESC_NKRO), &map));
    assert(!map.has_gamepad_collection);

    // Malformed / truncated descriptors are rejected, not guessed at.
    assert(!bthid_keyboard_parse_descriptor(NULL, 4, &map));
    assert(!bthid_keyboard_parse_descriptor(DESC_BOOT, 0, &map));
    for (uint16_t len = 1; len < sizeof(DESC_BOOT); ++len) {
        bthid_keyboard_report_map_t partial;
        (void)bthid_keyboard_parse_descriptor(DESC_BOOT, len, &partial);
        // No assertion on the result: a prefix may legitimately contain a
        // complete keyboard field. The requirement is only that it terminates
        // and never reports fields it did not see.
        assert(partial.bitmap_count <= BTHID_KEYBOARD_MAX_BITMAP_FIELDS);
        assert(partial.array_count <= BTHID_KEYBOARD_MAX_ARRAY_FIELDS);
    }
    puts("  classification");
}

static void test_boot_decode(void) {
    bthid_keyboard_report_map_t map;
    assert(bthid_keyboard_parse_descriptor(DESC_BOOT, sizeof(DESC_BOOT), &map));
    uint8_t bitmap[BTHID_KEYBOARD_USAGE_BYTES];

    // Single key down.
    const uint8_t down[8] = {0, 0, KEY_A, 0, 0, 0, 0, 0};
    assert(bthid_keyboard_decode_report(&map, down, sizeof(down), bitmap) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(held(bitmap, KEY_A) && held_count(bitmap) == 1);

    // Key up.
    const uint8_t up[8] = {0};
    assert(bthid_keyboard_decode_report(&map, up, sizeof(up), bitmap) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(held_count(bitmap) == 0);

    // Modifiers land in the same usage space as ordinary keys.
    const uint8_t modified[8] = {0x03, 0, KEY_W, 0, 0, 0, 0, 0};
    assert(bthid_keyboard_decode_report(&map, modified, sizeof(modified),
                                        bitmap) == BTHID_KEYBOARD_DECODE_OK);
    assert(held(bitmap, KEY_LCTRL) && held(bitmap, KEY_LSHIFT));
    assert(held(bitmap, KEY_W) && held_count(bitmap) == 3);

    // Simultaneous keys; slot order must not matter.
    const uint8_t three[8] = {0, 0, KEY_A, KEY_B, KEY_W, 0, 0, 0};
    const uint8_t three_reordered[8] = {0, 0, KEY_W, KEY_A, KEY_B, 0, 0, 0};
    uint8_t other[BTHID_KEYBOARD_USAGE_BYTES];
    assert(bthid_keyboard_decode_report(&map, three, sizeof(three), bitmap) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(bthid_keyboard_decode_report(&map, three_reordered,
                                        sizeof(three_reordered), other) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(memcmp(bitmap, other, sizeof(bitmap)) == 0);
    assert(held_count(bitmap) == 3);

    // ErrorRollOver is reported as such, never as "nothing held".
    const uint8_t rollover[8] = {0, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
    assert(bthid_keyboard_decode_report(&map, rollover, sizeof(rollover),
                                        bitmap) == BTHID_KEYBOARD_DECODE_ROLLOVER);

    // Truncated reports decode what is present and never read past the buffer.
    const uint8_t truncated[3] = {0x02, 0, KEY_A};
    assert(bthid_keyboard_decode_report(&map, truncated, sizeof(truncated),
                                        bitmap) == BTHID_KEYBOARD_DECODE_OK);
    assert(held(bitmap, KEY_LSHIFT) && held(bitmap, KEY_A));
    assert(bthid_keyboard_decode_report(&map, down, 0, bitmap) ==
           BTHID_KEYBOARD_DECODE_FAIL);

    // Report-ID form: a foreign report id is rejected outright.
    assert(bthid_keyboard_parse_descriptor(DESC_BOOT_ID, sizeof(DESC_BOOT_ID),
                                           &map));
    const uint8_t with_id[9] = {0x01, 0x00, 0, KEY_A, 0, 0, 0, 0, 0};
    assert(bthid_keyboard_decode_report(&map, with_id, sizeof(with_id), bitmap) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(held(bitmap, KEY_A) && held_count(bitmap) == 1);
    const uint8_t wrong_id[9] = {0x02, 0x00, 0, KEY_A, 0, 0, 0, 0, 0};
    assert(bthid_keyboard_decode_report(&map, wrong_id, sizeof(wrong_id),
                                        bitmap) == BTHID_KEYBOARD_DECODE_FAIL);
    puts("  boot report decode");
}

static void test_nkro_decode(void) {
    bthid_keyboard_report_map_t map;
    assert(bthid_keyboard_parse_descriptor(DESC_NKRO, sizeof(DESC_NKRO), &map));
    uint8_t bitmap[BTHID_KEYBOARD_USAGE_BYTES];

    // 1 modifier byte + 13 bitmap bytes (104 bits).
    uint8_t report[14];
    memset(report, 0, sizeof(report));
    report[0] = 0x02;  // LeftShift
    // Usage KEY_W sits at bit offset 8 + KEY_W.
    unsigned bit = 8u + KEY_W;
    report[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
    bit = 8u + KEY_A;
    report[bit >> 3] |= (uint8_t)(1u << (bit & 7u));

    assert(bthid_keyboard_decode_report(&map, report, sizeof(report), bitmap) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(held(bitmap, KEY_LSHIFT) && held(bitmap, KEY_W) && held(bitmap, KEY_A));
    assert(held_count(bitmap) == 3);

    // More than six keys at once, which is the whole point of NKRO.
    memset(report, 0, sizeof(report));
    for (uint8_t usage = KEY_A; usage < KEY_A + 10u; ++usage) {
        bit = 8u + usage;
        report[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
    }
    assert(bthid_keyboard_decode_report(&map, report, sizeof(report), bitmap) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(held_count(bitmap) == 10);

    // Usages below 0x04 are error codes, never keys, even in a bitmap.
    memset(report, 0, sizeof(report));
    report[1] = 0x0F;  // usages 0x00..0x03
    assert(bthid_keyboard_decode_report(&map, report, sizeof(report), bitmap) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(held_count(bitmap) == 0);
    puts("  NKRO decode");
}

static void test_boot_fallback(void) {
    uint8_t bitmap[BTHID_KEYBOARD_USAGE_BYTES];
    const uint8_t report[8] = {0x01, 0, KEY_W, 0, 0, 0, 0, 0};
    assert(bthid_keyboard_decode_boot(report, sizeof(report), bitmap) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(held(bitmap, KEY_LCTRL) && held(bitmap, KEY_W));

    const uint8_t rollover[8] = {0, 0, 0x01, 0, 0, 0, 0, 0};
    assert(bthid_keyboard_decode_boot(rollover, sizeof(rollover), bitmap) ==
           BTHID_KEYBOARD_DECODE_ROLLOVER);

    const uint8_t tiny[2] = {0x01, 0};
    assert(bthid_keyboard_decode_boot(tiny, sizeof(tiny), bitmap) ==
           BTHID_KEYBOARD_DECODE_FAIL);
    puts("  boot fallback");
}

int main(void) {
    puts("bthid_keyboard_report:");
    test_classification();
    test_boot_decode();
    test_nkro_decode();
    test_boot_fallback();
    puts("bthid_keyboard_report tests passed");
    return 0;
}
