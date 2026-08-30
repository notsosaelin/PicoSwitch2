// Keyboard input, end to end: a raw HID report becomes a pressed controller
// button.
//
// WHY THIS EXISTS
//
// The KB/M suite proved classification and slot ownership, and both were correct
// on hardware while the keyboard produced no input at all. Every test stopped at
// a boolean -- "is this peer a keyboard", "does it hold the keyboard slot" --
// and none of them started from bytes a keyboard actually sends or ended at the
// controller state the console receives. A whole pipeline can be dead between
// two green assertions.
//
// So this one starts at a REPORT and ends at OUTPUT, through the real parser,
// the real decoder, the real config and the real resolve. It deliberately does
// not use the runtime (that needs the Bluetooth stack), so it cannot prove
// admission; it proves everything on either side of it, which is what narrows a
// hardware failure to one stage instead of a subsystem.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bt/bthid/devices/generic/bthid_keyboard_report.h"
#include "ns2_kbm.h"

// This test edits and reads "the mapping a layout resolves against", which under
// the profile model is that layout's REALIZED content -- the same thing
// ns2_kbm_resolve() reads. Shimmed rather than rewritten at every call site
// because what changed is where a mapping lives, not any stage of the pipeline
// below. A function-like macro is not re-expanded inside its own replacement
// list, so each of these is one ordinary call.
#define KBM_LAYOUT_CONTENT(cfg, layout) (&(cfg)->active[(layout)].content)

#define ns2_kbm_binding(cfg, layout, src) \
    ns2_kbm_binding(KBM_LAYOUT_CONTENT(cfg, layout), (layout), (src))
#define ns2_kbm_set_binding(cfg, layout, src, dst) \
    ns2_kbm_set_binding(KBM_LAYOUT_CONTENT(cfg, layout), (layout), (src), (dst))
#define ns2_kbm_clear_binding(cfg, layout, src) \
    ns2_kbm_clear_binding(KBM_LAYOUT_CONTENT(cfg, layout), (layout), (src))

// Usage 0x05 is the letter B -- the exact binding used in the failing hardware
// test (`kbm bind kb key:05 -> b`).
#define USAGE_B 0x05u
#define USAGE_A 0x04u

// Boot keyboard, no report ID: [modifiers][reserved][6 key slots].
static const uint8_t DESC_BOOT[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0
};

// The same keyboard behind report ID 1, plus a mouse collection on ID 2 -- the
// shape a keyboard-with-pointer composite presents, and the one the 8BitDo is.
static const uint8_t DESC_KEYBOARD_WITH_POINTER[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xC0, 0xC0
};

static bool button_b_pressed(const ns2_kbm_output_t *out) {
    // NS2_DST_B is what `kbm bind ... b` stores; apply_destination puts it in
    // the console button block. Comparing against a neutral resolve keeps this
    // honest about WHICH bit without hard-coding the wire layout here.
    ns2_kbm_output_t neutral;
    ns2_kbm_state_t empty;
    ns2_kbm_config_t config;
    ns2_kbm_state_init(&empty);
    ns2_kbm_config_defaults(&config);
    ns2_kbm_resolve(&empty, &config, NS2_KBM_MODE_KEYBOARD, false, &neutral);

    for (unsigned i = 0; i < sizeof(out->buttons); ++i)
        if (out->buttons[i] != neutral.buttons[i]) return true;
    return false;
}

// Build the exact bytes a boot keyboard sends while `usage` is held.
static void boot_report(uint8_t *report, uint8_t usage) {
    memset(report, 0, 8u);
    if (usage) report[2] = usage;
}

// One press, all the way through.
static void test_keypress_becomes_a_button(void) {
    bthid_keyboard_report_map_t map;
    uint8_t usages[BTHID_KEYBOARD_USAGE_BYTES];
    uint8_t report[8];

    assert(bthid_keyboard_parse_descriptor(DESC_BOOT, sizeof(DESC_BOOT), &map));

    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    ns2_kbm_source_t key_b = {NS2_KBM_SRC_KEY, USAGE_B};
    assert(ns2_kbm_set_binding(&config, NS2_KBM_LAYOUT_KEYBOARD, key_b,
                               NS2_DST_B));
    assert(ns2_kbm_binding(&config, NS2_KBM_LAYOUT_KEYBOARD, key_b) ==
           NS2_DST_B);

    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_output_t out;

    // --- press -------------------------------------------------------------
    boot_report(report, USAGE_B);
    assert(bthid_keyboard_decode_report(&map, report, sizeof(report), usages) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(usages[USAGE_B >> 3] & (1u << (USAGE_B & 7u)));

    ns2_kbm_state_set_keys(&state, usages);
    assert(state.keyboard_present);

    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(button_b_pressed(&out));

    // --- release -----------------------------------------------------------
    boot_report(report, 0u);
    assert(bthid_keyboard_decode_report(&map, report, sizeof(report), usages) ==
           BTHID_KEYBOARD_DECODE_OK);
    ns2_kbm_state_set_keys(&state, usages);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(!button_b_pressed(&out));

    printf("  keypress -> mapped button, and released\n");
}

// An unbound key must do nothing at all.
static void test_unbound_key_does_nothing(void) {
    bthid_keyboard_report_map_t map;
    uint8_t usages[BTHID_KEYBOARD_USAGE_BYTES];
    uint8_t report[8];
    assert(bthid_keyboard_parse_descriptor(DESC_BOOT, sizeof(DESC_BOOT), &map));

    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    ns2_kbm_source_t key_a = {NS2_KBM_SRC_KEY, USAGE_A};
    (void)ns2_kbm_clear_binding(&config, NS2_KBM_LAYOUT_KEYBOARD, key_a);

    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_output_t out;

    boot_report(report, USAGE_A);
    assert(bthid_keyboard_decode_report(&map, report, sizeof(report), usages) ==
           BTHID_KEYBOARD_DECODE_OK);
    ns2_kbm_state_set_keys(&state, usages);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(!button_b_pressed(&out));

    printf("  unbound key changes nothing\n");
}

// THE COMPOSITE CASE. A keyboard-with-pointer puts its keyboard behind a report
// ID, and its mouse behind another. If the decoder took the wrong report, or
// ignored the ID, a real keypress would decode as nothing -- which is exactly
// the hardware symptom.
static void test_composite_report_id_routing(void) {
    bthid_keyboard_report_map_t map;
    uint8_t usages[BTHID_KEYBOARD_USAGE_BYTES];

    assert(bthid_keyboard_parse_descriptor(DESC_KEYBOARD_WITH_POINTER,
                                           sizeof(DESC_KEYBOARD_WITH_POINTER),
                                           &map));
    assert(map.using_report_ids);
    // The KEYBOARD's report id, not the mouse's.
    assert(map.report_id == 1u);

    // A keyboard report: [id=1][modifiers][reserved][6 slots].
    uint8_t keyboard_report[9];
    memset(keyboard_report, 0, sizeof(keyboard_report));
    keyboard_report[0] = 1u;
    keyboard_report[3] = USAGE_B;
    assert(bthid_keyboard_decode_report(&map, keyboard_report,
                                        sizeof(keyboard_report), usages) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(usages[USAGE_B >> 3] & (1u << (USAGE_B & 7u)));

    // A MOUSE report on the other id must not decode as keyboard input. A byte
    // that happens to look like a usage must not become a keypress.
    uint8_t mouse_report[5] = {2u, 0x01u, 0x05u, 0x00u, 0x00u};
    assert(bthid_keyboard_decode_report(&map, mouse_report,
                                        sizeof(mouse_report), usages) ==
           BTHID_KEYBOARD_DECODE_FAIL);

    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    ns2_kbm_source_t key_b = {NS2_KBM_SRC_KEY, USAGE_B};
    assert(ns2_kbm_set_binding(&config, NS2_KBM_LAYOUT_KEYBOARD, key_b,
                               NS2_DST_B));

    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_output_t out;

    assert(bthid_keyboard_decode_report(&map, keyboard_report,
                                        sizeof(keyboard_report), usages) ==
           BTHID_KEYBOARD_DECODE_OK);
    ns2_kbm_state_set_keys(&state, usages);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(button_b_pressed(&out));

    printf("  composite report ids route keyboard and mouse separately\n");
}

// The binding namespace must be the SAME one the decoder produces. `kbm bind kb
// key:05` stores usage 5, the decoder sets bit 5, and resolve looks up source
// {KEY, 5}. A mismatch anywhere here is invisible to every other test: the bind
// succeeds, the map reads back, and the key does nothing.
static void test_binding_namespace_is_hid_usage(void) {
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);

    ns2_kbm_source_t parsed;
    assert(ns2_kbm_source_parse("key:05", &parsed));
    assert(parsed.kind == NS2_KBM_SRC_KEY);
    assert(parsed.code == USAGE_B);

    assert(ns2_kbm_set_binding(&config, NS2_KBM_LAYOUT_KEYBOARD, parsed,
                               NS2_DST_B));

    // And the decoder sets exactly that bit for a report carrying usage 5.
    bthid_keyboard_report_map_t map;
    uint8_t usages[BTHID_KEYBOARD_USAGE_BYTES];
    uint8_t report[8];
    assert(bthid_keyboard_parse_descriptor(DESC_BOOT, sizeof(DESC_BOOT), &map));
    boot_report(report, USAGE_B);
    assert(bthid_keyboard_decode_report(&map, report, sizeof(report), usages) ==
           BTHID_KEYBOARD_DECODE_OK);

    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_state_set_keys(&state, usages);
    assert(ns2_kbm_state_key_held(&state, parsed.code));

    ns2_kbm_output_t out;
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(button_b_pressed(&out));

    printf("  bind namespace and decoded usage are the same number\n");
}

// Keyboard mode must resolve the KEYBOARD profile, and a binding that exists
// only in the other profile must not fire. The reverse of the mismatch that
// makes a saved binding look correct while nothing happens.
static void test_profile_selection_follows_mode(void) {
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    ns2_kbm_source_t key_b = {NS2_KBM_SRC_KEY, USAGE_B};

    assert(ns2_kbm_mode_layout(NS2_KBM_MODE_KEYBOARD) ==
           NS2_KBM_LAYOUT_KEYBOARD);
    assert(ns2_kbm_mode_layout(NS2_KBM_MODE_KEYBOARD_MOUSE) ==
           NS2_KBM_LAYOUT_KEYBOARD_MOUSE);

    // Bound ONLY in the keyboard-and-mouse profile.
    assert(ns2_kbm_clear_binding(&config, NS2_KBM_LAYOUT_KEYBOARD, key_b) ||
           ns2_kbm_binding(&config, NS2_KBM_LAYOUT_KEYBOARD, key_b) ==
               NS2_DST_NONE);
    assert(ns2_kbm_set_binding(&config, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, key_b,
                               NS2_DST_B));

    bthid_keyboard_report_map_t map;
    uint8_t usages[BTHID_KEYBOARD_USAGE_BYTES];
    uint8_t report[8];
    assert(bthid_keyboard_parse_descriptor(DESC_BOOT, sizeof(DESC_BOOT), &map));
    boot_report(report, USAGE_B);
    assert(bthid_keyboard_decode_report(&map, report, sizeof(report), usages) ==
           BTHID_KEYBOARD_DECODE_OK);

    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_state_set_keys(&state, usages);
    ns2_kbm_output_t out;

    // Keyboard mode resolves the keyboard profile, where it is NOT bound.
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(!button_b_pressed(&out));

    // The other mode finds it.
    state.mouse_present = 1u;
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(button_b_pressed(&out));

    printf("  mode selects the profile, and only that profile resolves\n");
}

// A held key must survive a report that carries no keyboard payload at all.
static void test_rollover_does_not_release_everything(void) {
    bthid_keyboard_report_map_t map;
    uint8_t usages[BTHID_KEYBOARD_USAGE_BYTES];
    uint8_t report[8];
    assert(bthid_keyboard_parse_descriptor(DESC_BOOT, sizeof(DESC_BOOT), &map));

    // ErrorRollOver (0x01) in every slot: the keyboard cannot say what is down.
    memset(report, 0, sizeof(report));
    for (unsigned i = 2; i < 8u; ++i) report[i] = 0x01u;
    assert(bthid_keyboard_decode_report(&map, report, sizeof(report), usages) ==
           BTHID_KEYBOARD_DECODE_ROLLOVER);

    printf("  rollover is reported, not decoded as an empty key set\n");
}

// THE BLE TRANSPORT SHAPE.
//
// Pins what the BLE path actually delivers, because a plausible-sounding and
// WRONG theory about it nearly got implemented: that HOGP notifications carry
// no Report ID and the transport must prepend one.
//
// BTstack's hids_host already inserts it. hids_host_setup_report_event_with_report_id()
// writes the id at the start of the report payload, which is why
// gattservice_subevent_hid_report_get_report() points at &event[9] with
// report_len = value_len + 1. route_ble_hid_report() then prepends only the
// 0xA1 transaction header, and bt_on_hid_report_with_generation() strips exactly
// that one byte. A driver therefore receives [report_id][payload] on BLE,
// identical to Classic.
//
// Prepending the ID again would shift every BLE report by one byte and break
// every BLE HID device. This test exists so that idea dies here, not on
// hardware.
static void test_ble_transport_shape_matches_classic(void) {
    bthid_keyboard_report_map_t map;
    uint8_t usages[BTHID_KEYBOARD_USAGE_BYTES];

    assert(bthid_keyboard_parse_descriptor(DESC_KEYBOARD_WITH_POINTER,
                                           sizeof(DESC_KEYBOARD_WITH_POINTER),
                                           &map));
    assert(map.using_report_ids);
    assert(map.report_id == 1u);

    // What a driver sees once the transaction header is stripped: the report id
    // followed by the report data. The same on both transports.
    uint8_t as_driver_sees_it[9];
    memset(as_driver_sees_it, 0, sizeof(as_driver_sees_it));
    as_driver_sees_it[0] = 1u;
    as_driver_sees_it[3] = USAGE_B;

    assert(bthid_keyboard_decode_report(&map, as_driver_sees_it,
                                        sizeof(as_driver_sees_it), usages) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(usages[USAGE_B >> 3] & (1u << (USAGE_B & 7u)));

    // The double-prepend the disproven theory would have produced, checked on
    // the MODIFIER byte.
    //
    // Not on a key: a boot report is six interchangeable key slots, so shifting
    // by one byte merely moves the key into the next slot and it still decodes.
    // That is precisely why this class of bug is hard to see from behaviour --
    // it can look almost right. The modifier field has exactly one position, so
    // a shift turns LeftShift into whatever the previous byte held.
    uint8_t shifted[9];
    memset(shifted, 0, sizeof(shifted));
    shifted[0] = 1u;
    shifted[1] = 0x02u;  // LeftShift, usage 0xE1
    assert(bthid_keyboard_decode_report(&map, shifted, sizeof(shifted), usages) ==
           BTHID_KEYBOARD_DECODE_OK);
    assert(usages[0xE1u >> 3] & (1u << (0xE1u & 7u)));

    uint8_t doubled[10];
    doubled[0] = 1u;
    memcpy(doubled + 1, shifted, sizeof(shifted));
    (void)bthid_keyboard_decode_report(&map, doubled, sizeof(doubled), usages);
    assert(!(usages[0xE1u >> 3] & (1u << (0xE1u & 7u))));

    printf("  BLE delivers the same [report_id][payload] shape as Classic\n");
}

int main(void) {
    printf("kbm_keyboard_pipeline:\n");
    test_ble_transport_shape_matches_classic();
    test_keypress_becomes_a_button();
    test_unbound_key_does_nothing();
    test_composite_report_id_routing();
    test_binding_namespace_is_hid_usage();
    test_profile_selection_follows_mode();
    test_rollover_does_not_release_everything();
    printf("kbm_keyboard_pipeline tests passed\n");
    return 0;
}
