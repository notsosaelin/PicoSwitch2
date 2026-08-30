#include "bt/bthid/devices/generic/bthid_keyboard_report.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Minimal HID report-descriptor item walk
// ---------------------------------------------------------------------------
// Short-item prefix: [bTag:4][bType:2][bSize:2], bSize 3 meaning 4 data bytes.
#define HID_ITEM_SIZE_MASK 0x03u
#define HID_ITEM_TYPE_MASK 0x0Cu
#define HID_ITEM_TAG_MASK 0xF0u

#define HID_TYPE_MAIN 0x00u
#define HID_TYPE_GLOBAL 0x04u
#define HID_TYPE_LOCAL 0x08u

#define HID_TAG_INPUT 0x80u
#define HID_TAG_OUTPUT 0x90u
#define HID_TAG_FEATURE 0xB0u
#define HID_TAG_COLLECTION 0xA0u
#define HID_TAG_END_COLLECTION 0xC0u

#define HID_TAG_USAGE_PAGE 0x00u
#define HID_TAG_REPORT_SIZE 0x70u
#define HID_TAG_REPORT_ID 0x80u
#define HID_TAG_REPORT_COUNT 0x90u
#define HID_TAG_PUSH 0xA0u
#define HID_TAG_POP 0xB0u

#define HID_TAG_USAGE 0x00u
#define HID_TAG_USAGE_MIN 0x10u
#define HID_TAG_USAGE_MAX 0x20u

// Input item data bits.
#define HID_IO_CONSTANT 0x01u
#define HID_IO_VARIABLE 0x02u

#define HID_USAGE_PAGE_KEYBOARD 0x07u
#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01u

// Generic Desktop application usages that mean "this peer is a controller".
#define HID_USAGE_JOYSTICK 0x04u
#define HID_USAGE_GAME_PAD 0x05u

#define HID_COLLECTION_APPLICATION 0x01u

// Local Usage items pending for the next main item. Only enough to answer
// "what does this collection declare itself to be"; the keyboard fields
// themselves are addressed by Usage Minimum/Maximum, not by this list.
#define KB_MAX_PENDING_USAGES 8u

// Keyboard usages 0x01..0x03 are the error codes (rollover, POST fail, undefined
// error), never physical keys.
#define HID_KEY_ERROR_ROLLOVER 0x01u
#define HID_KEY_FIRST_REAL 0x04u

#define KB_MAX_REPORT_IDS 8u

// Push/Pop nesting depth. The HID spec sets no limit; real descriptors nest one
// or two levels, and overflowing simply stops saving rather than corrupting the
// state that is already correct.
#define KB_MAX_STATE_DEPTH 4u

// The Global item state Push/Pop saves and restores. Report ID is a Global item
// too, so it belongs here: a descriptor may Push, switch report id for a
// secondary block, and Pop back to the first one.
typedef struct {
    uint16_t usage_page;
    uint16_t report_size;
    uint16_t report_count;
    uint8_t report_id;
} kb_global_state_t;

typedef struct {
    uint8_t report_id;
    uint16_t input_bits;
} kb_report_offset_t;

static uint16_t *report_offset(kb_report_offset_t *table, uint8_t *count,
                               uint8_t report_id) {
    for (uint8_t i = 0; i < *count; ++i) {
        if (table[i].report_id == report_id) return &table[i].input_bits;
    }
    if (*count >= KB_MAX_REPORT_IDS) return NULL;
    table[*count].report_id = report_id;
    table[*count].input_bits = 0;
    return &table[(*count)++].input_bits;
}

static uint32_t item_data(const uint8_t *desc, uint16_t offset, uint8_t size) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; ++i)
        value |= (uint32_t)desc[offset + i] << (8u * i);
    return value;
}

bool bthid_keyboard_parse_descriptor(const uint8_t *desc, uint16_t desc_len,
                                     bthid_keyboard_report_map_t *out) {
    if (!desc || desc_len == 0 || !out) return false;
    memset(out, 0, sizeof(*out));

    uint16_t usage_page = 0;
    uint16_t report_size = 0;
    uint16_t report_count = 0;
    uint8_t report_id = 0;
    uint32_t usage_min = 0;
    uint32_t usage_max = 0;
    bool have_usage_range = false;
    bool found = false;
    bool seen_application_collection = false;
    uint8_t keyboard_report_id = 0;

    uint32_t pending_usages[KB_MAX_PENDING_USAGES];
    uint8_t pending_usage_count = 0;

    kb_report_offset_t offsets[KB_MAX_REPORT_IDS];
    uint8_t offset_count = 0;
    memset(offsets, 0, sizeof(offsets));

    kb_global_state_t saved[KB_MAX_STATE_DEPTH];
    uint8_t state_depth = 0;
    memset(saved, 0, sizeof(saved));

    uint16_t index = 0;
    while (index < desc_len) {
        uint8_t prefix = desc[index];
        if (prefix == 0xFEu) {
            // Long item: [0xFE][bDataSize][bLongItemTag][data...]
            if ((uint32_t)index + 3u > desc_len) break;
            uint16_t data_size = desc[index + 1u];
            index = (uint16_t)(index + 3u + data_size);
            continue;
        }
        uint8_t size_code = prefix & HID_ITEM_SIZE_MASK;
        uint8_t size = size_code == 3u ? 4u : size_code;
        if ((uint32_t)index + 1u + size > desc_len) break;
        uint32_t data = item_data(desc, (uint16_t)(index + 1u), size);
        uint8_t type = prefix & HID_ITEM_TYPE_MASK;
        uint8_t tag = prefix & HID_ITEM_TAG_MASK;
        index = (uint16_t)(index + 1u + size);

        if (type == HID_TYPE_GLOBAL) {
            switch (tag) {
                case HID_TAG_USAGE_PAGE: usage_page = (uint16_t)data; break;
                case HID_TAG_REPORT_SIZE: report_size = (uint16_t)data; break;
                case HID_TAG_REPORT_COUNT: report_count = (uint16_t)data; break;
                case HID_TAG_REPORT_ID:
                    report_id = (uint8_t)data;
                    out->using_report_ids = true;
                    break;
                case HID_TAG_PUSH:
                    // Save the Global state. A descriptor uses this to borrow
                    // the state for a nested block -- most often an LED output
                    // report between the modifier byte and the key array -- and
                    // expects Pop to put back the Usage Page and field sizes the
                    // keys are declared under. Ignoring it silently reads every
                    // later field under the wrong page at the wrong offsets,
                    // which looks exactly like a keyboard that types nothing.
                    if (state_depth < KB_MAX_STATE_DEPTH) {
                        saved[state_depth].usage_page = usage_page;
                        saved[state_depth].report_size = report_size;
                        saved[state_depth].report_count = report_count;
                        saved[state_depth].report_id = report_id;
                        state_depth++;
                    }
                    break;
                case HID_TAG_POP:
                    // An unmatched Pop is a malformed descriptor; keep the
                    // current state rather than inventing one.
                    if (state_depth > 0u) {
                        state_depth--;
                        usage_page = saved[state_depth].usage_page;
                        report_size = saved[state_depth].report_size;
                        report_count = saved[state_depth].report_count;
                        report_id = saved[state_depth].report_id;
                    }
                    break;
                default: break;  // Logical/Physical ranges and units are not needed here
            }
            continue;
        }
        if (type == HID_TYPE_LOCAL) {
            if (tag == HID_TAG_USAGE_MIN) {
                usage_min = data;
                have_usage_range = true;
            } else if (tag == HID_TAG_USAGE_MAX) {
                usage_max = data;
            } else if (tag == HID_TAG_USAGE) {
                if (pending_usage_count < KB_MAX_PENDING_USAGES) {
                    // A 4-byte Usage carries its page in the high half; a short
                    // one inherits the current global page.
                    uint32_t qualified = size == 4u
                        ? data
                        : (((uint32_t)usage_page << 16) | (data & 0xFFFFu));
                    pending_usages[pending_usage_count++] = qualified;
                }
            }
            continue;
        }
        if (type != HID_TYPE_MAIN) continue;

        if (tag == HID_TAG_COLLECTION) {
            // Does this peer declare itself a controller? Secondary guard only
            // -- a supported controller is recognized by the quirk table long
            // before this runs (see the header's SCOPE note). This catches an
            // UNKNOWN pad that declares a keyboard collection for its
            // share/profile button, which claiming as a keyboard would strand.
            if (data == HID_COLLECTION_APPLICATION) {
                for (uint8_t i = 0; i < pending_usage_count; ++i) {
                    uint16_t page = (uint16_t)(pending_usages[i] >> 16);
                    uint16_t usage = (uint16_t)(pending_usages[i] & 0xFFFFu);
                    if (page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
                        (usage == HID_USAGE_JOYSTICK || usage == HID_USAGE_GAME_PAD))
                        out->has_gamepad_collection = true;

                    // FIRST application collection only. What a descriptor opens
                    // with is the device stating what it primarily is -- a
                    // keyboard opens with Usage(Keyboard), a mouse with
                    // Usage(Mouse) and declares its macro keys after. Later
                    // collections are extras and must not overwrite it.
                    if (!seen_application_collection &&
                        page == HID_USAGE_PAGE_GENERIC_DESKTOP && usage <= 0xFFu) {
                        out->primary_application_usage = (uint8_t)usage;
                        seen_application_collection = true;
                    }
                }
                seen_application_collection = true;
            }
            usage_min = usage_max = 0;
            have_usage_range = false;
            pending_usage_count = 0;
            continue;
        }
        if (tag == HID_TAG_END_COLLECTION) {
            usage_min = usage_max = 0;
            have_usage_range = false;
            pending_usage_count = 0;
            continue;
        }
        if (tag == HID_TAG_OUTPUT || tag == HID_TAG_FEATURE) {
            usage_min = usage_max = 0;
            have_usage_range = false;
            pending_usage_count = 0;
            continue;
        }
        if (tag != HID_TAG_INPUT) continue;

        uint16_t *bits = report_offset(offsets, &offset_count, report_id);
        uint16_t field_offset = bits ? *bits : 0u;
        uint32_t field_bits = (uint32_t)report_size * report_count;
        if (bits) {
            uint32_t next = (uint32_t)*bits + field_bits;
            *bits = next > 0xFFFFu ? 0xFFFFu : (uint16_t)next;
        }

        bool constant = (data & HID_IO_CONSTANT) != 0u;
        bool variable = (data & HID_IO_VARIABLE) != 0u;

        if (usage_page == HID_USAGE_PAGE_KEYBOARD && !constant &&
            report_count > 0u) {
            // The report-id offset table is what makes this field addressable;
            // without it the offsets would be wrong, so skip rather than guess.
            if (!bits) {
                usage_min = usage_max = 0;
                have_usage_range = false;
                continue;
            }
            if (!found) {
                keyboard_report_id = report_id;
                found = true;
            }
            if (report_id != keyboard_report_id) {
                // A second keyboard report id (e.g. a vendor NKRO alternate).
                // This build follows exactly one, chosen deterministically as
                // the first declared, so decoding can never depend on which
                // report happens to arrive first.
                usage_min = usage_max = 0;
                have_usage_range = false;
                continue;
            }
            if (variable && report_size == 1u && have_usage_range &&
                usage_min <= 0xFFu && usage_max >= usage_min) {
                // The declared usage range bounds the run: a descriptor may
                // pad a bitmap with more bits than it names usages for, and
                // those trailing bits are not keys.
                uint32_t range = usage_max - usage_min + 1u;
                uint32_t count = report_count < range ? report_count : range;
                if (usage_min + count > 0x100u) count = 0x100u - usage_min;
                if (count > 0u &&
                    out->bitmap_count < BTHID_KEYBOARD_MAX_BITMAP_FIELDS) {
                    out->bitmaps[out->bitmap_count].bit_offset = field_offset;
                    out->bitmaps[out->bitmap_count].count = (uint16_t)count;
                    out->bitmaps[out->bitmap_count].usage_min = (uint8_t)usage_min;
                    out->bitmap_count++;
                }
            } else if (!variable && report_size == 8u &&
                       out->array_count < BTHID_KEYBOARD_MAX_ARRAY_FIELDS &&
                       (field_offset & 7u) == 0u) {
                out->arrays[out->array_count].bit_offset = field_offset;
                out->arrays[out->array_count].count =
                    report_count > 255u ? 255u : (uint8_t)report_count;
                out->array_count++;
            }
        }

        usage_min = usage_max = 0;
        have_usage_range = false;
        pending_usage_count = 0;
    }

    if (!found || (out->bitmap_count == 0u && out->array_count == 0u))
        return false;
    // A controller that also carries keyboard usages stays a controller. The
    // parsed keyboard fields are left in `out` so a caller can see what was
    // found, but this is not a keyboard.
    if (out->has_gamepad_collection) return false;
    out->report_id = keyboard_report_id;
    return true;
}

static void set_usage(uint8_t bitmap[BTHID_KEYBOARD_USAGE_BYTES], uint8_t usage) {
    bitmap[usage >> 3] |= (uint8_t)(1u << (usage & 7u));
}

static bool read_bit(const uint8_t *data, uint16_t len, uint32_t bit) {
    if ((bit >> 3) >= len) return false;
    return (data[bit >> 3] & (1u << (bit & 7u))) != 0u;
}

bthid_keyboard_shape_t bthid_keyboard_shape(const bthid_keyboard_report_map_t *map) {
    bthid_keyboard_shape_t shape;
    memset(&shape, 0, sizeof(shape));
    if (!map) return shape;

    for (uint8_t i = 0; i < map->array_count; ++i)
        shape.rollover_slots = (uint16_t)(shape.rollover_slots + map->arrays[i].count);

    for (uint8_t i = 0; i < map->bitmap_count; ++i) {
        const bthid_keyboard_bitmap_field_t *field = &map->bitmaps[i];
        // The modifier byte is exactly the eight flags over 0xE0..0xE7. Every
        // keyboard has it; a macro collection generally does not, and when it
        // does it is not on its own enough (see below).
        if (field->usage_min == 0xE0u && field->count == 8u) {
            shape.has_modifier_byte = true;
            continue;
        }
        shape.key_bitmap_bits = (uint16_t)(shape.key_bitmap_bits + field->count);
    }

    shape.keyboard_is_primary_collection =
        map->primary_application_usage == BTHID_HID_USAGE_KEYBOARD ||
        map->primary_application_usage == BTHID_HID_USAGE_KEYPAD;

    /*
     * Three facts must agree, and each rules out a different wrong answer.
     *
     * 1. The descriptor OPENS with a keyboard application collection. This is
     *    the device's own statement about what it is, and it is what keeps a
     *    gaming mouse out: an ASUS ROG KERIS II opens with Usage(Mouse) and
     *    declares its macro keys afterwards, so it can never pass this clause
     *    no matter how complete that macro collection is.
     * 2. A standard modifier byte. Shift/Ctrl/Alt/GUI as eight flags is
     *    something a keyboard has and a handful of macro buttons does not.
     * 3. Enough key capacity to be a keyboard: a rollover array of several
     *    slots, OR -- for an NKRO board that has no array at all -- a wide key
     *    bitmap. Requiring the array alone would fail every NKRO keyboard.
     *
     * A controller that carries keyboard usages is already excluded upstream by
     * has_gamepad_collection, and is excluded again here for the same reason
     * clause 1 exists.
     */
    shape.strong_keyboard =
        shape.keyboard_is_primary_collection &&
        shape.has_modifier_byte &&
        !map->has_gamepad_collection &&
        (shape.rollover_slots >= BTHID_KEYBOARD_STRONG_ROLLOVER_SLOTS ||
         shape.key_bitmap_bits >= BTHID_KEYBOARD_STRONG_BITMAP_BITS);

    return shape;
}

bthid_keyboard_decode_t bthid_keyboard_decode_report(
    const bthid_keyboard_report_map_t *map, const uint8_t *data, uint16_t len,
    uint8_t usage_bitmap[BTHID_KEYBOARD_USAGE_BYTES]) {
    if (!map || !data || !usage_bitmap || len == 0) return BTHID_KEYBOARD_DECODE_FAIL;
    if (map->using_report_ids && data[0] != map->report_id)
        return BTHID_KEYBOARD_DECODE_FAIL;

    // Offsets recorded by the parser exclude the report-id byte, exactly as the
    // descriptor describes the report; add it back for the on-wire buffer.
    uint32_t base = map->using_report_ids ? 8u : 0u;
    memset(usage_bitmap, 0, BTHID_KEYBOARD_USAGE_BYTES);

    for (uint8_t f = 0; f < map->bitmap_count; ++f) {
        const bthid_keyboard_bitmap_field_t *field = &map->bitmaps[f];
        for (uint16_t i = 0; i < field->count; ++i) {
            uint32_t usage = (uint32_t)field->usage_min + i;
            if (usage > 0xFFu) break;
            if (usage < HID_KEY_FIRST_REAL) continue;
            if (read_bit(data, len, base + field->bit_offset + i))
                set_usage(usage_bitmap, (uint8_t)usage);
        }
    }

    for (uint8_t f = 0; f < map->array_count; ++f) {
        const bthid_keyboard_array_field_t *field = &map->arrays[f];
        uint32_t byte = (base + field->bit_offset) >> 3;
        for (uint8_t i = 0; i < field->count; ++i) {
            if (byte + i >= len) break;
            uint8_t usage = data[byte + i];
            if (usage == HID_KEY_ERROR_ROLLOVER) return BTHID_KEYBOARD_DECODE_ROLLOVER;
            if (usage < HID_KEY_FIRST_REAL) continue;
            set_usage(usage_bitmap, usage);
        }
    }

    return BTHID_KEYBOARD_DECODE_OK;
}

bthid_keyboard_decode_t bthid_keyboard_decode_boot(
    const uint8_t *data, uint16_t len,
    uint8_t usage_bitmap[BTHID_KEYBOARD_USAGE_BYTES]) {
    if (!data || !usage_bitmap || len < 3u) return BTHID_KEYBOARD_DECODE_FAIL;
    memset(usage_bitmap, 0, BTHID_KEYBOARD_USAGE_BYTES);

    uint8_t modifiers = data[0];
    for (uint8_t bit = 0; bit < 8u; ++bit) {
        if (modifiers & (1u << bit)) set_usage(usage_bitmap, (uint8_t)(0xE0u + bit));
    }
    for (uint16_t i = 2; i < len && i < 8u; ++i) {
        uint8_t usage = data[i];
        if (usage == HID_KEY_ERROR_ROLLOVER) return BTHID_KEYBOARD_DECODE_ROLLOVER;
        if (usage < HID_KEY_FIRST_REAL) continue;
        set_usage(usage_bitmap, usage);
    }
    return BTHID_KEYBOARD_DECODE_OK;
}
