// Keyboard / Keyboard + Mouse model tests.
//
// Host-only: no Pico SDK, no BTstack, no console, no connected hardware. This
// is the authority for the mapping, merge, opposing-direction, duplicate
// binding, remap-neutralization, and mouse-translation contracts.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ns2_kbm.h"
#include "switch_pro.h"

#define KEY_W 0x1Au
#define KEY_A 0x04u
#define KEY_S 0x16u
#define KEY_D 0x07u
#define KEY_F 0x09u
#define KEY_E 0x08u
#define KEY_I 0x0Cu
#define KEY_K 0x0Eu
#define KEY_1 0x1Eu
#define KEY_3 0x20u
#define KEY_SPACE 0x2Cu
#define KEY_ESCAPE 0x29u
#define KEY_UP 0x52u
#define KEY_DOWN 0x51u
#define KEY_LSHIFT 0xE1u

static ns2_kbm_source_t key(uint8_t usage) {
    ns2_kbm_source_t source = {NS2_KBM_SRC_KEY, usage};
    return source;
}

static ns2_kbm_source_t mouse_button(uint8_t number) {
    ns2_kbm_source_t source = {NS2_KBM_SRC_MOUSE, number};
    return source;
}

static void hold(uint8_t bitmap[NS2_KBM_KEY_BITMAP_BYTES], uint8_t usage) {
    bitmap[usage >> 3] |= (uint8_t)(1u << (usage & 7u));
}

static void press_keys(ns2_kbm_state_t *state, const uint8_t *usages,
                       unsigned count) {
    uint8_t bitmap[NS2_KBM_KEY_BITMAP_BYTES];
    memset(bitmap, 0, sizeof(bitmap));
    for (unsigned i = 0; i < count; ++i) hold(bitmap, usages[i]);
    ns2_kbm_state_set_keys(state, bitmap);
}

static void release_all(ns2_kbm_state_t *state) {
    press_keys(state, NULL, 0);
}

// ---------------------------------------------------------------------------

static void test_identifier_validation(void) {
    assert(ns2_kbm_source_valid(key(KEY_W)));
    assert(ns2_kbm_source_valid(key(0xE7u)));
    assert(!ns2_kbm_source_valid(key(0x00u)));   // "no event"
    assert(!ns2_kbm_source_valid(key(0x01u)));   // ErrorRollOver, not a key
    assert(!ns2_kbm_source_valid(key(0xE8u)));   // beyond the supported range
    assert(ns2_kbm_source_valid(mouse_button(1)));
    assert(ns2_kbm_source_valid(mouse_button(NS2_KBM_MOUSE_BUTTONS)));
    assert(!ns2_kbm_source_valid(mouse_button(0)));
    assert(!ns2_kbm_source_valid(mouse_button(NS2_KBM_MOUSE_BUTTONS + 1u)));

    ns2_kbm_source_t parsed;
    assert(ns2_kbm_source_parse("key:1A", &parsed) &&
           ns2_kbm_source_equal(parsed, key(KEY_W)));
    assert(ns2_kbm_source_parse("mouse:3", &parsed) &&
           ns2_kbm_source_equal(parsed, mouse_button(3)));
    assert(!ns2_kbm_source_parse("key:00", &parsed));
    assert(!ns2_kbm_source_parse("key:1AZ", &parsed));
    assert(!ns2_kbm_source_parse("mouse:9", &parsed));
    assert(!ns2_kbm_source_parse("dpad:up", &parsed));

    char text[12];
    ns2_kbm_source_format(key(KEY_W), text, sizeof(text));
    assert(strcmp(text, "key:1A") == 0);
    ns2_kbm_source_format(mouse_button(2), text, sizeof(text));
    assert(strcmp(text, "mouse:2") == 0);

    uint8_t destination = 0xFF;
    assert(ns2_kbm_destination_from_name("lstick_up", &destination) &&
           destination == NS2_DST_LSTICK_UP);
    assert(!ns2_kbm_destination_from_name("nonsense", &destination));
    assert(strcmp(ns2_kbm_destination_name(NS2_DST_ZR), "zr") == 0);
    puts("  identifiers");
}

// ns2_kbm_apply_destination() is the single authority both mapping systems use
// (the locked physical-controller map in ns2_seam.c calls it too), so pin the
// exact wire bit each destination produces. A silent change here would move a
// physical controller's buttons, not just a keyboard's.
static void test_destination_wire_bits(void) {
    static const struct {
        uint8_t destination;
        uint8_t buttons[3];
        uint8_t extra;
    } expected[] = {
        {NS2_DST_B, {SWITCH_MASK_B, 0, 0}, 0},
        {NS2_DST_A, {SWITCH_MASK_A, 0, 0}, 0},
        {NS2_DST_Y, {SWITCH_MASK_Y, 0, 0}, 0},
        {NS2_DST_X, {SWITCH_MASK_X, 0, 0}, 0},
        {NS2_DST_L, {0, 0, SWITCH_MASK_L}, 0},
        {NS2_DST_R, {SWITCH_MASK_R, 0, 0}, 0},
        {NS2_DST_ZL, {0, 0, SWITCH_MASK_ZL}, 0},
        {NS2_DST_ZR, {SWITCH_MASK_ZR, 0, 0}, 0},
        {NS2_DST_L3, {0, SWITCH_MASK_L3, 0}, 0},
        {NS2_DST_R3, {0, SWITCH_MASK_R3, 0}, 0},
        {NS2_DST_MINUS, {0, SWITCH_MASK_MINUS, 0}, 0},
        {NS2_DST_PLUS, {0, SWITCH_MASK_PLUS, 0}, 0},
        {NS2_DST_HOME, {0, SWITCH_MASK_HOME, 0}, 0},
        {NS2_DST_CAPTURE, {0, SWITCH_MASK_CAPTURE, 0}, 0},
        {NS2_DST_DUP, {0, 0, SWITCH_MASK_DPAD_UP}, 0},
        {NS2_DST_DDOWN, {0, 0, SWITCH_MASK_DPAD_DOWN}, 0},
        {NS2_DST_DLEFT, {0, 0, SWITCH_MASK_DPAD_LEFT}, 0},
        {NS2_DST_DRIGHT, {0, 0, SWITCH_MASK_DPAD_RIGHT}, 0},
        {NS2_DST_GL, {0, 0, 0}, SWITCH_EXTRA_GL},
        {NS2_DST_GR, {0, 0, 0}, SWITCH_EXTRA_GR},
        {NS2_DST_C, {0, 0, 0}, SWITCH_EXTRA_C},
    };
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        uint8_t buttons[3] = {0, 0, 0};
        uint8_t extra = 0;
        ns2_kbm_apply_destination(expected[i].destination, buttons, &extra);
        assert(memcmp(buttons, expected[i].buttons, sizeof(buttons)) == 0);
        assert(extra == expected[i].extra);
    }

    // NONE and the digital stick directions are not buttons and must set no bit.
    static const uint8_t not_buttons[] = {
        NS2_DST_NONE,
        NS2_DST_LSTICK_UP, NS2_DST_LSTICK_DOWN,
        NS2_DST_LSTICK_LEFT, NS2_DST_LSTICK_RIGHT,
        NS2_DST_RSTICK_UP, NS2_DST_RSTICK_DOWN,
        NS2_DST_RSTICK_LEFT, NS2_DST_RSTICK_RIGHT,
    };
    for (unsigned i = 0; i < sizeof(not_buttons) / sizeof(not_buttons[0]); ++i) {
        uint8_t buttons[3] = {0, 0, 0};
        uint8_t extra = 0;
        ns2_kbm_apply_destination(not_buttons[i], buttons, &extra);
        assert(buttons[0] == 0 && buttons[1] == 0 && buttons[2] == 0 && extra == 0);
    }

    // Every destination the enum defines must be named and round-trip.
    for (uint8_t destination = 0; destination < NS2_DST_COUNT; ++destination) {
        const char *name = ns2_kbm_destination_name(destination);
        uint8_t parsed = 0xFF;
        assert(ns2_kbm_destination_from_name(name, &parsed));
        assert(parsed == destination);
    }
    puts("  destination wire bits");
}

static void test_canonical_defaults(void) {
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    // Auto by default: an ordinary HID device must work without a mode command.
    assert(config.mode == NS2_KBM_MODE_AUTO);
    assert(config.profiles[NS2_KBM_PROFILE_KEYBOARD].count == 0);
    assert(config.mouse.sensitivity_x == NS2_KBM_MOUSE_SENS_DEFAULT);

    // Keyboard profile: WASD walk, IJKL aim, the documented face layout.
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_W)) ==
           NS2_DST_LSTICK_UP);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_I)) ==
           NS2_DST_RSTICK_UP);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_SPACE)) ==
           NS2_DST_B);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F)) ==
           NS2_DST_A);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_ESCAPE)) ==
           NS2_DST_HOME);

    // Keyboard + Mouse profile: the mouse owns the right stick, so IJKL are
    // deliberately unassigned there and mouse buttons carry the triggers.
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE, key(KEY_W)) ==
           NS2_DST_LSTICK_UP);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE, key(KEY_I)) ==
           NS2_DST_NONE);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           mouse_button(1)) == NS2_DST_ZR);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           mouse_button(2)) == NS2_DST_ZL);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           mouse_button(3)) == NS2_DST_R3);
    // The standard five-button mouse contract: all five are ordinary mouse
    // inputs and all five are bound. Back/Forward keep the destinations the
    // Controller-mode base map already gives them, so the same physical button
    // does the same thing in both modes.
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           mouse_button(4)) == NS2_DST_Y);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           mouse_button(5)) == NS2_DST_B);
    for (uint8_t button = 1u; button <= NS2_KBM_MOUSE_BUTTONS; ++button) {
        assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                               mouse_button(button)) != NS2_DST_NONE);
    }

    // Every canonical binding must name a legal destination.
    ns2_kbm_effective_t effective[NS2_KBM_MAX_EFFECTIVE];
    for (unsigned p = 0; p < NS2_KBM_PROFILE_COUNT; ++p) {
        uint16_t count = ns2_kbm_effective_bindings(
            &config, (ns2_kbm_profile_t)p, effective, NS2_KBM_MAX_EFFECTIVE);
        assert(count > 0 && count < NS2_KBM_MAX_EFFECTIVE);
        for (uint16_t i = 0; i < count; ++i) {
            assert(ns2_kbm_source_valid(effective[i].source));
            assert(ns2_kbm_destination_valid(effective[i].destination));
            assert(effective[i].destination != NS2_DST_NONE);
            assert(!effective[i].overridden);
        }
    }
    puts("  canonical defaults");
}

static void test_keyboard_mapping(void) {
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_output_t out;

    // Nothing held: neutral.
    press_keys(&state, NULL, 0);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.buttons[0] == 0 && out.buttons[1] == 0 && out.buttons[2] == 0);
    assert(out.left_x == SWITCH_STICK_MID && out.left_y == SWITCH_STICK_MID);

    // Key down / key up.
    const uint8_t space[] = {KEY_SPACE};
    press_keys(&state, space, 1);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.buttons[0] & SWITCH_MASK_B);
    release_all(&state);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert((out.buttons[0] & SWITCH_MASK_B) == 0);

    // Simultaneous keys, including a modifier used as an ordinary binding.
    const uint8_t combo[] = {KEY_W, KEY_F, KEY_LSHIFT};
    press_keys(&state, combo, 3);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.left_y == SWITCH_STICK_MAX);
    assert(out.buttons[0] & SWITCH_MASK_A);
    assert(out.buttons[0] & SWITCH_MASK_Y);

    // Report order must not matter.
    ns2_kbm_output_t reordered;
    const uint8_t combo_reversed[] = {KEY_LSHIFT, KEY_F, KEY_W};
    press_keys(&state, combo_reversed, 3);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &reordered);
    assert(memcmp(&out, &reordered, sizeof(out)) == 0);

    // An unmapped key changes nothing.
    const uint8_t combo_plus_unmapped[] = {KEY_LSHIFT, KEY_F, KEY_W, 0x30u};
    press_keys(&state, combo_plus_unmapped, 4);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &reordered);
    assert(memcmp(&out, &reordered, sizeof(out)) == 0);
    puts("  keyboard mapping");
}

static void test_opposing_directions(void) {
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_output_t out;

    const uint8_t wasd_vertical[] = {KEY_W, KEY_S};
    press_keys(&state, wasd_vertical, 2);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.left_y == SWITCH_STICK_MID);

    const uint8_t wasd_horizontal[] = {KEY_A, KEY_D};
    press_keys(&state, wasd_horizontal, 2);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.left_x == SWITCH_STICK_MID);

    // Same rule for digital right stick and for the D-pad.
    const uint8_t aim[] = {KEY_I, KEY_K};
    press_keys(&state, aim, 2);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.right_y == SWITCH_STICK_MID);

    const uint8_t dpad[] = {KEY_UP, KEY_DOWN};
    press_keys(&state, dpad, 2);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert((out.buttons[2] & (SWITCH_MASK_DPAD_UP | SWITCH_MASK_DPAD_DOWN)) == 0);

    // Releasing one side restores the other immediately; nothing accumulates.
    const uint8_t only_w[] = {KEY_W};
    press_keys(&state, only_w, 1);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.left_y == SWITCH_STICK_MAX);
    release_all(&state);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.left_y == SWITCH_STICK_MID);
    puts("  opposing directions");
}

static void test_overrides_and_profile_independence(void) {
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);

    // Remap F -> X in the Keyboard profile only.
    assert(ns2_kbm_set_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE, key(KEY_F)) ==
           NS2_DST_A);

    // And the reverse direction.
    assert(ns2_kbm_set_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                               mouse_button(1), NS2_DST_A));
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);

    // Clearing a binding makes it unassigned but keeps it distinct from
    // "restore the default".
    assert(ns2_kbm_set_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_SPACE),
                               NS2_DST_NONE));
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_SPACE)) ==
           NS2_DST_NONE);
    assert(ns2_kbm_clear_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_SPACE)));
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_SPACE)) ==
           NS2_DST_B);

    // Requesting exactly the default stores no override.
    uint8_t before = config.profiles[NS2_KBM_PROFILE_KEYBOARD].count;
    assert(ns2_kbm_set_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_E),
                               NS2_DST_X));
    assert(config.profiles[NS2_KBM_PROFILE_KEYBOARD].count == before);

    // A new binding for a source with no canonical default shows up in the
    // effective listing, so a UI never has to guess it exists.
    assert(ns2_kbm_set_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(0x2Bu),
                               NS2_DST_GL));
    ns2_kbm_effective_t effective[NS2_KBM_MAX_EFFECTIVE];
    uint16_t count = ns2_kbm_effective_bindings(&config, NS2_KBM_PROFILE_KEYBOARD,
                                                effective, NS2_KBM_MAX_EFFECTIVE);
    bool found_tab = false;
    bool found_f_override = false;
    for (uint16_t i = 0; i < count; ++i) {
        if (ns2_kbm_source_equal(effective[i].source, key(0x2Bu))) {
            found_tab = effective[i].destination == NS2_DST_GL &&
                        effective[i].overridden;
        }
        if (ns2_kbm_source_equal(effective[i].source, key(KEY_F))) {
            found_f_override = effective[i].destination == NS2_DST_X &&
                               effective[i].overridden;
        }
    }
    assert(found_tab && found_f_override);

    // Resetting one profile leaves the other untouched.
    ns2_kbm_config_reset_profile(&config, NS2_KBM_PROFILE_KEYBOARD);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F)) ==
           NS2_DST_A);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           mouse_button(1)) == NS2_DST_A);
    ns2_kbm_config_reset_profile(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           mouse_button(1)) == NS2_DST_ZR);

    // Restoring defaults must reproduce the canonical profile exactly.
    ns2_kbm_config_t pristine;
    ns2_kbm_config_defaults(&pristine);
    assert(memcmp(&config.profiles, &pristine.profiles, sizeof(config.profiles)) == 0);

    // The override table is bounded and refuses to overflow.
    ns2_kbm_config_t full;
    ns2_kbm_config_defaults(&full);
    unsigned added = 0;
    for (uint8_t usage = 0x04u; usage <= 0xE7u && added < NS2_KBM_MAX_OVERRIDES + 4u;
         ++usage) {
        if (ns2_kbm_binding(&full, NS2_KBM_PROFILE_KEYBOARD, key(usage)) !=
            NS2_DST_NONE)
            continue;
        if (ns2_kbm_set_binding(&full, NS2_KBM_PROFILE_KEYBOARD, key(usage),
                                NS2_DST_A))
            added++;
        else
            break;
    }
    assert(added == NS2_KBM_MAX_OVERRIDES);
    assert(full.profiles[NS2_KBM_PROFILE_KEYBOARD].count == NS2_KBM_MAX_OVERRIDES);
    puts("  overrides and profile independence");
}

static void test_duplicate_destinations(void) {
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_output_t out;

    // Two keyboard sources naming one destination.
    assert(ns2_kbm_set_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_E),
                               NS2_DST_A));
    const uint8_t both[] = {KEY_F, KEY_E};
    press_keys(&state, both, 2);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.buttons[0] & SWITCH_MASK_A);

    // Releasing one must NOT release the shared destination.
    const uint8_t only_e[] = {KEY_E};
    press_keys(&state, only_e, 1);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.buttons[0] & SWITCH_MASK_A);

    // Releasing the last one does.
    release_all(&state);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert((out.buttons[0] & SWITCH_MASK_A) == 0);

    // The same rule across roles: key 3 and mouse button 1 both reach ZR in the
    // canonical Keyboard + Mouse profile.
    ns2_kbm_config_defaults(&config);
    ns2_kbm_state_init(&state);
    const uint8_t key_three[] = {KEY_3};
    press_keys(&state, key_three, 1);
    ns2_kbm_state_mouse_report(&state, 1u << 0, 0, 0, 0, &config.mouse, 0);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.buttons[0] & SWITCH_MASK_ZR);
    // Mouse button up, key still held.
    ns2_kbm_state_mouse_report(&state, 0, 0, 0, 0, &config.mouse, 10);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.buttons[0] & SWITCH_MASK_ZR);
    // Key up too.
    release_all(&state);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert((out.buttons[0] & SWITCH_MASK_ZR) == 0);

    // One source must not reach more than one destination.
    ns2_kbm_config_defaults(&config);
    ns2_kbm_state_init(&state);
    const uint8_t only_f[] = {KEY_F};
    press_keys(&state, only_f, 1);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.buttons[0] == SWITCH_MASK_A);
    assert(out.buttons[1] == 0 && out.buttons[2] == 0 && out.extra == 0);
    puts("  duplicate destinations");
}

static void test_remap_while_held(void) {
    // The resolved output is recomputed from the held set every time, so a
    // binding change can never leave the previous destination pressed.
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_output_t out;

    const uint8_t only_f[] = {KEY_F};
    press_keys(&state, only_f, 1);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.buttons[0] & SWITCH_MASK_A);

    assert(ns2_kbm_set_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert((out.buttons[0] & SWITCH_MASK_A) == 0);
    assert(out.buttons[0] & SWITCH_MASK_X);

    // The runtime additionally drops the held state at the boundary; verify the
    // model supports that without leaving anything behind.
    ns2_kbm_state_clear_keyboard(&state);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert(out.buttons[0] == 0);
    puts("  remap while held");
}

static void test_mouse_translation(void) {
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_output_t out;

    // Non-native output: movement becomes right-stick deflection.
    ns2_kbm_state_mouse_report(&state, 0, 100, 0, 0, &config.mouse, 0);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.right_x > SWITCH_STICK_MID);
    assert(out.right_y == SWITCH_STICK_MID);
    assert(out.has_mouse == 0);
    assert(out.mouse_delta_x == 0);

    // Mouse down is stick down (a lower 12-bit value).
    ns2_kbm_state_init(&state);
    ns2_kbm_state_mouse_report(&state, 0, 0, 100, 0, &config.mouse, 0);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.right_y < SWITCH_STICK_MID);

    // Inversion.
    ns2_kbm_state_init(&state);
    config.mouse.invert_y = 1;
    ns2_kbm_state_mouse_report(&state, 0, 0, 100, 0, &config.mouse, 0);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.right_y > SWITCH_STICK_MID);
    config.mouse.invert_y = 0;

    // Sensitivity scales the same movement.
    ns2_kbm_state_init(&state);
    config.mouse.sensitivity_x = NS2_KBM_MOUSE_SENS_DEFAULT;
    ns2_kbm_state_mouse_report(&state, 0, 20, 0, 0, &config.mouse, 0);
    int32_t slow = state.stick_x;
    ns2_kbm_state_init(&state);
    config.mouse.sensitivity_x = (uint16_t)(NS2_KBM_MOUSE_SENS_DEFAULT * 2u);
    ns2_kbm_state_mouse_report(&state, 0, 20, 0, 0, &config.mouse, 0);
    assert(state.stick_x == slow * 2);
    config.mouse.sensitivity_x = NS2_KBM_MOUSE_SENS_DEFAULT;

    // High-DPI bursts clamp instead of accumulating without bound.
    ns2_kbm_state_init(&state);
    for (unsigned i = 0; i < 100; ++i)
        ns2_kbm_state_mouse_report(&state, 0, 30000, 0, 0, &config.mouse, 0);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.right_x == SWITCH_STICK_MAX);

    // And it always comes back to neutral with no further motion.
    for (uint32_t t = 1; t <= 400u; ++t)
        ns2_kbm_state_service(&state, &config.mouse, t);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(state.stick_x == 0);
    assert(out.right_x == SWITCH_STICK_MID);

    // A single long gap recenters completely rather than partially.
    ns2_kbm_state_init(&state);
    ns2_kbm_state_mouse_report(&state, 0, 500, 500, 0, &config.mouse, 0);
    ns2_kbm_state_service(&state, &config.mouse, 100000u);
    assert(state.stick_x == 0 && state.stick_y == 0);

    // Native mouse output takes the relative deltas untouched and leaves the
    // right stick alone.
    ns2_kbm_state_init(&state);
    ns2_kbm_state_mouse_report(&state, 0, 42, -7, 3, &config.mouse, 0);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, true, &out);
    assert(out.has_mouse == 1);
    assert(out.mouse_delta_x == 42 && out.mouse_delta_y == -7);
    assert(out.mouse_delta_wheel == 3);
    assert(out.right_x == SWITCH_STICK_MID && out.right_y == SWITCH_STICK_MID);

    // Relative movement is one-shot: a republish must not replay it.
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, true, &out);
    assert(out.mouse_delta_x == 0 && out.mouse_delta_y == 0);
    puts("  mouse translation");
}

static void test_state_ownership(void) {
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_output_t out;

    // Keyboard and mouse contributions merge; neither erases the other.
    const uint8_t walk[] = {KEY_W};
    press_keys(&state, walk, 1);
    ns2_kbm_state_mouse_report(&state, 1u << 1, 0, 0, 0, &config.mouse, 0);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.left_y == SWITCH_STICK_MAX);            // keyboard-owned
    assert(out.buttons[2] & SWITCH_MASK_ZL);            // mouse-owned

    // A keyboard report does not clear the mouse button.
    const uint8_t walk_and_jump[] = {KEY_W, KEY_SPACE};
    press_keys(&state, walk_and_jump, 2);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.buttons[2] & SWITCH_MASK_ZL);
    assert(out.buttons[0] & SWITCH_MASK_B);

    // A mouse report does not clear held keys.
    ns2_kbm_state_mouse_report(&state, 0, 0, 0, 0, &config.mouse, 5);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.left_y == SWITCH_STICK_MAX);
    assert(out.buttons[0] & SWITCH_MASK_B);
    assert((out.buttons[2] & SWITCH_MASK_ZL) == 0);

    // Mouse loss clears mouse-owned state only, including the translated stick.
    ns2_kbm_state_mouse_report(&state, 1u << 0, 400, 0, 0, &config.mouse, 10);
    ns2_kbm_state_clear_mouse(&state);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.right_x == SWITCH_STICK_MID);
    assert((out.buttons[0] & SWITCH_MASK_ZR) == 0);
    assert(out.left_y == SWITCH_STICK_MAX);   // keyboard survives
    assert(out.buttons[0] & SWITCH_MASK_B);

    // A missing mouse must NOT turn the Keyboard + Mouse profile into the
    // Keyboard profile: IJKL stay unassigned.
    const uint8_t aim[] = {KEY_I};
    press_keys(&state, aim, 1);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.right_y == SWITCH_STICK_MID);

    // Keyboard loss clears keyboard-owned state only.
    ns2_kbm_state_init(&state);
    press_keys(&state, walk_and_jump, 2);
    ns2_kbm_state_mouse_report(&state, 1u << 1, 0, 0, 0, &config.mouse, 0);
    ns2_kbm_state_clear_keyboard(&state);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.left_y == SWITCH_STICK_MID);
    assert((out.buttons[0] & SWITCH_MASK_B) == 0);
    assert(out.buttons[2] & SWITCH_MASK_ZL);

    // Reconnect starts neutral: a fresh state produces nothing at all.
    ns2_kbm_state_init(&state);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.buttons[0] == 0 && out.buttons[1] == 0 && out.buttons[2] == 0);
    assert(out.left_x == SWITCH_STICK_MID && out.right_x == SWITCH_STICK_MID);

    // Controller mode contributes nothing even with state held.
    press_keys(&state, walk_and_jump, 2);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_CONTROLLER, false, &out);
    assert(out.buttons[0] == 0 && out.left_y == SWITCH_STICK_MID);

    // Keyboard mode ignores a mouse that is somehow still marked present.
    ns2_kbm_state_mouse_report(&state, 1u << 0, 0, 0, 0, &config.mouse, 0);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD, false, &out);
    assert((out.buttons[0] & SWITCH_MASK_ZR) == 0);
    puts("  state ownership");
}

static void test_config_validation(void) {
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    assert(ns2_kbm_config_sanitize(&config));

    // Unknown mode falls back to the canonical default (auto).
    config.mode = 200u;
    assert(!ns2_kbm_config_sanitize(&config));
    // Auto by default: an ordinary HID device must work without a mode command.
    assert(config.mode == NS2_KBM_MODE_AUTO);

    // An impossible override count discards the whole table rather than
    // reinterpreting whatever bytes survive.
    ns2_kbm_config_defaults(&config);
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].count = 200u;
    assert(!ns2_kbm_config_sanitize(&config));
    assert(config.profiles[NS2_KBM_PROFILE_KEYBOARD].count == 0);

    // Individual bad entries are dropped, good ones survive.
    ns2_kbm_config_defaults(&config);
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].count = 3;
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[0].source = key(KEY_F);
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[0].destination = NS2_DST_X;
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[1].source.kind = 99u;
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[1].source.code = 5u;
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[1].destination = NS2_DST_A;
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[2].source = key(KEY_E);
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[2].destination = 250u;
    assert(!ns2_kbm_config_sanitize(&config));
    assert(config.profiles[NS2_KBM_PROFILE_KEYBOARD].count == 1);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_E)) ==
           NS2_DST_X);  // canonical default, not the rejected 250

    // Duplicate sources collapse to the first.
    ns2_kbm_config_defaults(&config);
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].count = 2;
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[0].source = key(KEY_F);
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[0].destination = NS2_DST_X;
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[1].source = key(KEY_F);
    config.profiles[NS2_KBM_PROFILE_KEYBOARD].entries[1].destination = NS2_DST_Y;
    assert(!ns2_kbm_config_sanitize(&config));
    assert(config.profiles[NS2_KBM_PROFILE_KEYBOARD].count == 1);
    assert(ns2_kbm_binding(&config, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);

    // Out-of-range mouse settings fall back to their canonical values.
    ns2_kbm_config_defaults(&config);
    config.mouse.sensitivity_x = 0;
    config.mouse.recenter_ms = 60000u;
    config.mouse.invert_x = 7u;
    assert(!ns2_kbm_config_sanitize(&config));
    assert(config.mouse.sensitivity_x == NS2_KBM_MOUSE_SENS_DEFAULT);
    assert(config.mouse.recenter_ms == NS2_KBM_MOUSE_RECENTER_DEFAULT_MS);
    assert(config.mouse.invert_x == 0);

    // Fully random bytes must produce a usable configuration, not a crash and
    // not arbitrary controller destinations.
    uint8_t noise[sizeof(ns2_kbm_config_t)];
    for (size_t i = 0; i < sizeof(noise); ++i) noise[i] = (uint8_t)(i * 37u + 11u);
    memcpy(&config, noise, sizeof(config));
    (void)ns2_kbm_config_sanitize(&config);
    assert(config.mode < NS2_KBM_MODE_COUNT);
    for (unsigned p = 0; p < NS2_KBM_PROFILE_COUNT; ++p) {
        assert(config.profiles[p].count <= NS2_KBM_MAX_OVERRIDES);
        for (uint8_t i = 0; i < config.profiles[p].count; ++i) {
            assert(ns2_kbm_source_valid(config.profiles[p].entries[i].source));
            assert(ns2_kbm_destination_valid(
                config.profiles[p].entries[i].destination));
        }
    }
    ns2_kbm_state_t state;
    ns2_kbm_state_init(&state);
    ns2_kbm_output_t out;
    uint8_t bitmap[NS2_KBM_KEY_BITMAP_BYTES];
    memset(bitmap, 0xFF, sizeof(bitmap));
    ns2_kbm_state_set_keys(&state, bitmap);
    ns2_kbm_state_mouse_report(&state, 0xFFFFu, 32767, -32768, 127,
                               &config.mouse, 0);
    ns2_kbm_resolve(&state, &config, NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
    assert(out.left_x <= SWITCH_STICK_MAX && out.right_y <= SWITCH_STICK_MAX);
    puts("  configuration validation");
}

static ns2_kbm_peer_caps_t caps(bool keyboard, bool pointer) {
    ns2_kbm_peer_caps_t c = {keyboard ? 1u : 0u, pointer ? 1u : 0u};
    return c;
}

static ns2_kbm_role_policy_t policy(bool keyboard, bool mouse) {
    ns2_kbm_role_policy_t p = {keyboard ? 1u : 0u, mouse ? 1u : 0u};
    return p;
}

// Short aliases: these appear on nearly every admission call, and the primary
// kind is the thing under test, so it should be the part that reads loudest.
#define KB NS2_KBM_PRIMARY_KEYBOARD
#define MS NS2_KBM_PRIMARY_MOUSE
#define CB NS2_KBM_PRIMARY_COMBO

// Capability precedence for an unresolved peer: pointer wins, and "has both"
// is never combo.
static void test_primary_from_caps(void) {
    assert(ns2_kbm_primary_from_caps(caps(false, false)) == NS2_KBM_PRIMARY_NONE);
    assert(ns2_kbm_primary_from_caps(caps(true, false)) == NS2_KBM_PRIMARY_KEYBOARD);
    assert(ns2_kbm_primary_from_caps(caps(false, true)) == NS2_KBM_PRIMARY_MOUSE);
    // The KERIS II case: keyboard usages present, still a mouse.
    assert(ns2_kbm_primary_from_caps(caps(true, true)) == NS2_KBM_PRIMARY_MOUSE);
    // COMBO is never inferred from capabilities.
    assert(ns2_kbm_primary_from_caps(caps(true, true)) != NS2_KBM_PRIMARY_COMBO);

    // THE HAZARD this function must never be applied to.
    //
    // A BLE peer reaches the keyboard driver through descriptor
    // reclassification, and for a moment the only capability on record is the
    // driver binding's narrow view: {keyboard, no pointer}. Fed to this
    // function that yields KEYBOARD -- correct for the inputs, catastrophic for
    // a gaming mouse, and latched for the life of the connection.
    //
    // The value below is therefore CORRECT and is exactly why
    // ns2_kbm_runtime.c's primary_authority_pending() refuses to call this for
    // a BLE peer whose descriptor classification has not landed yet. That
    // transport gate is firmware-side and is verified on hardware; this pins
    // the reason it has to exist.
    assert(ns2_kbm_primary_from_caps(caps(true, false)) == NS2_KBM_PRIMARY_KEYBOARD);
    // Once the descriptor lands, the same peer classifies correctly.
    assert(ns2_kbm_primary_from_caps(caps(true, true)) == NS2_KBM_PRIMARY_MOUSE);
    puts("  primary from capabilities");
}

// The mode is inferred from the admitted composition. Pairing an ordinary HID
// device must not require the user to predict a mode first.
// Discovery lifetime: the confirmed "first peer wins" regression.
//
// A BLE HID peer reaching ready stops the scan unconditionally, and the host's
// idle safety-net cannot restore it while any link is up (its final term,
// btstack_classic_get_connection_count() == 0, counts BLE links despite the
// name). So this predicate is the ONLY thing that keeps discovery alive for a
// second peer. Answering "complete" while a role is still missing strands the
// absent peripheral until it is physically power-cycled.
static void test_logical_source_completeness(void) {
    // Nothing connected at all: not complete, so discovery runs.
    assert(!ns2_kbm_logical_source_complete(false, false, false));

    // Keyboard arrives first -- the exact hardware case. Source is PARTIAL, so
    // discovery must stay available for the mouse.
    assert(!ns2_kbm_logical_source_complete(true, false, false));

    // Mouse arrives first -- mirrored, and equally partial.
    assert(!ns2_kbm_logical_source_complete(false, true, false));

    // Both roles held: the composite is complete and discovery may idle.
    assert(ns2_kbm_logical_source_complete(true, true, false));

    // Ordinary single controller: complete, discovery idles. This is the
    // historical 1-dongle-1-controller behaviour and must not regress.
    assert(ns2_kbm_logical_source_complete(false, false, true));

    // A KB/M role outranks a stray connected controller: a partial composite
    // stays partial, so the missing role remains discoverable.
    assert(!ns2_kbm_logical_source_complete(true, false, true));
    assert(!ns2_kbm_logical_source_complete(false, true, true));
    assert(ns2_kbm_logical_source_complete(true, true, true));

    printf("OK:   partial KB/M source keeps discovery alive; complete source idles\n");
}

// Role loss must reopen discovery, and the returning role must close it again.
static void test_discovery_reopens_on_role_loss(void) {
    // Complete -> mouse powers off -> partial: discovery must resume for it
    // while the keyboard is left untouched.
    assert(ns2_kbm_logical_source_complete(true, true, false));
    assert(!ns2_kbm_logical_source_complete(true, false, false));
    // Mouse returns: complete again, discovery may stop.
    assert(ns2_kbm_logical_source_complete(true, true, false));

    // Mirror: keyboard powers off, mouse survives.
    assert(!ns2_kbm_logical_source_complete(false, true, false));
    assert(ns2_kbm_logical_source_complete(true, true, false));

    printf("OK:   losing a role reopens discovery; regaining it lets discovery idle\n");
}

// Under AUTO the effective mode is derived from the roles present, so it reports
// a coherent mode for a half-built composite. Completeness must NOT be keyed off
// it, or one peer would always look like a finished source.
static void test_completeness_not_derived_from_effective_mode(void) {
    // Keyboard alone under AUTO resolves to a KEYBOARD mode...
    ns2_kbm_mode_t kb_only = ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, true, false);
    assert(kb_only == NS2_KBM_MODE_KEYBOARD);
    // ...yet the source is still incomplete.
    assert(!ns2_kbm_logical_source_complete(true, false, false));

    // Mouse alone under AUTO resolves to KEYBOARD_MOUSE...
    ns2_kbm_mode_t mouse_only = ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, false, true);
    assert(mouse_only == NS2_KBM_MODE_KEYBOARD_MOUSE);
    // ...and is likewise incomplete.
    assert(!ns2_kbm_logical_source_complete(false, true, false));

    printf("OK:   completeness is independent of the AUTO-derived effective mode\n");
}

// ---------------------------------------------------------------------------
// Partial-source completion window
// ---------------------------------------------------------------------------
// A partial KB/M source holds discovery open so the complementary role can join.
// That must stay bounded: keyboard-only and mouse-only are legitimate, and
// scanning forever would keep the radio hunting for as long as someone uses a
// keyboard. Expiry settles the source; it does NOT lock the topology.

#define W NS2_KBM_COMPLETION_WINDOW_MS

static void test_completion_keyboard_first(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    // Nothing connected: ordinary always-on discovery, no window.
    assert(ns2_kbm_completion_update(&s, false, false, false, 1000) == NS2_KBM_DISCOVERY_SEEK);
    assert(!s.windowing);

    // Keyboard joins -> partial, window opens.
    assert(ns2_kbm_completion_update(&s, true, false, false, 1000) == NS2_KBM_DISCOVERY_SEEK);
    assert(s.windowing && s.started_ms == 1000);

    // Inside the window discovery keeps running.
    assert(ns2_kbm_completion_update(&s, true, false, false, 1000 + W - 1) == NS2_KBM_DISCOVERY_SEEK);

    // At the deadline the partial source is treated as intentional.
    assert(ns2_kbm_completion_update(&s, true, false, false, 1000 + W) == NS2_KBM_DISCOVERY_IDLE);
    assert(!s.windowing);

    // ...and stays settled rather than oscillating back into seeking.
    assert(ns2_kbm_completion_update(&s, true, false, false, 1000 + W + 5000) == NS2_KBM_DISCOVERY_IDLE);
    printf("OK:   keyboard-only seeks for the completion window, then settles\n");
}

static void test_completion_mouse_first(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    assert(ns2_kbm_completion_update(&s, false, true, false, 500) == NS2_KBM_DISCOVERY_SEEK);
    assert(s.windowing && s.started_ms == 500);
    assert(ns2_kbm_completion_update(&s, false, true, false, 500 + W - 1) == NS2_KBM_DISCOVERY_SEEK);
    assert(ns2_kbm_completion_update(&s, false, true, false, 500 + W) == NS2_KBM_DISCOVERY_IDLE);
    printf("OK:   mouse-only seeks for the completion window, then settles\n");
}

// The hardware-validated fast path: both powered together, complement arrives
// inside the window. This must never regress to "first peer wins".
static void test_completion_complement_joins(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    assert(ns2_kbm_completion_update(&s, true, false, false, 0) == NS2_KBM_DISCOVERY_SEEK);
    assert(ns2_kbm_completion_update(&s, true, true, false, W / 2) == NS2_KBM_DISCOVERY_IDLE);
    assert(!s.windowing);   // cancelled immediately on completion

    memset(&s, 0, sizeof(s));
    assert(ns2_kbm_completion_update(&s, false, true, false, 0) == NS2_KBM_DISCOVERY_SEEK);
    assert(ns2_kbm_completion_update(&s, true, true, false, W / 2) == NS2_KBM_DISCOVERY_IDLE);
    assert(!s.windowing);
    printf("OK:   complement joining inside the window completes the source and cancels it\n");
}

// Role loss from a complete pair opens a NEW window, so a power-cycled
// peripheral has a grace period to rejoin without re-pairing.
static void test_completion_role_loss_reopens(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    assert(ns2_kbm_completion_update(&s, true, true, false, 0) == NS2_KBM_DISCOVERY_IDLE);
    assert(!s.windowing);

    // Mouse powers off -> partial -> new window from that moment.
    assert(ns2_kbm_completion_update(&s, true, false, false, 8000) == NS2_KBM_DISCOVERY_SEEK);
    assert(s.windowing && s.started_ms == 8000);

    // Mouse returns inside the window -> complete again, window cancelled.
    assert(ns2_kbm_completion_update(&s, true, true, false, 8000 + W - 1) == NS2_KBM_DISCOVERY_IDLE);
    assert(!s.windowing);

    // Mirror: keyboard powers off and returns.
    assert(ns2_kbm_completion_update(&s, false, true, false, 20000) == NS2_KBM_DISCOVERY_SEEK);
    assert(s.windowing && s.started_ms == 20000);
    assert(ns2_kbm_completion_update(&s, true, true, false, 20100) == NS2_KBM_DISCOVERY_IDLE);
    printf("OK:   losing a role reopens the window; the role returning cancels it\n");
}

// Keyed to logical-source transitions, never to traffic. Servicing repeatedly --
// what happens while a keyboard is actively used -- must not push the deadline.
static void test_completion_not_extended_by_service(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    assert(ns2_kbm_completion_update(&s, true, false, false, 100) == NS2_KBM_DISCOVERY_SEEK);
    for (uint32_t t = 100; t < 100 + W; t += 250) {
        assert(ns2_kbm_completion_update(&s, true, false, false, t) == NS2_KBM_DISCOVERY_SEEK);
        assert(s.started_ms == 100);   // deadline never moves
    }
    assert(ns2_kbm_completion_update(&s, true, false, false, 100 + W) == NS2_KBM_DISCOVERY_IDLE);
    printf("OK:   repeated servicing cannot extend the completion window\n");
}

// A settled partial source is NOT a locked topology. Expiry only ends background
// hunting; the surviving role keeps working and the complement may still join
// when discovery is re-opened explicitly. This is the transition the bounded
// window made first-class, and the one that exposed the reconnect regression.
static void test_completion_settled_then_complement_joins(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    // Keyboard settles alone.
    assert(ns2_kbm_completion_update(&s, true, false, false, 0) == NS2_KBM_DISCOVERY_SEEK);
    assert(ns2_kbm_completion_update(&s, true, false, false, W) == NS2_KBM_DISCOVERY_IDLE);
    assert(!s.windowing);
    // The keyboard role is untouched by expiry -- settling is a discovery
    // decision, not a source change.
    assert(s.held == (uint8_t)NS2_KBM_HELD_KEYBOARD);

    // Long after settling, the mouse joins via an explicitly re-opened window.
    // The source completes and discovery idles for the right reason.
    assert(ns2_kbm_completion_update(&s, true, true, false, W + 600000u) == NS2_KBM_DISCOVERY_IDLE);
    assert(!s.windowing);
    assert(s.held == (uint8_t)(NS2_KBM_HELD_KEYBOARD | NS2_KBM_HELD_MOUSE));

    // Mirror: mouse settles alone, keyboard joins much later.
    memset(&s, 0, sizeof(s));
    assert(ns2_kbm_completion_update(&s, false, true, false, 0) == NS2_KBM_DISCOVERY_SEEK);
    assert(ns2_kbm_completion_update(&s, false, true, false, W) == NS2_KBM_DISCOVERY_IDLE);
    assert(s.held == (uint8_t)NS2_KBM_HELD_MOUSE);
    assert(ns2_kbm_completion_update(&s, true, true, false, W + 600000u) == NS2_KBM_DISCOVERY_IDLE);
    assert(s.held == (uint8_t)(NS2_KBM_HELD_KEYBOARD | NS2_KBM_HELD_MOUSE));
    printf("OK:   a settled partial source still accepts its complement later\n");
}

static void test_completion_all_roles_gone(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    assert(ns2_kbm_completion_update(&s, true, false, false, 0) == NS2_KBM_DISCOVERY_SEEK);
    assert(s.windowing);
    assert(ns2_kbm_completion_update(&s, false, false, false, 1000) == NS2_KBM_DISCOVERY_SEEK);
    assert(!s.windowing && s.started_ms == 0);
    printf("OK:   losing the last KB/M role clears the window\n");
}

static void test_completion_role_swap_restarts(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    assert(ns2_kbm_completion_update(&s, true, false, false, 0) == NS2_KBM_DISCOVERY_SEEK);
    assert(s.started_ms == 0);
    // Keyboard-only becoming mouse-only is a new source, not a continuation.
    assert(ns2_kbm_completion_update(&s, false, true, false, 4000) == NS2_KBM_DISCOVERY_SEEK);
    assert(s.started_ms == 4000);
    assert(ns2_kbm_completion_update(&s, false, true, false, 4000 + W - 1) == NS2_KBM_DISCOVERY_SEEK);
    printf("OK:   swapping which single role is held restarts the window\n");
}

static void test_completion_ordinary_controller(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    assert(ns2_kbm_completion_update(&s, false, false, true, 0) == NS2_KBM_DISCOVERY_IDLE);
    assert(!s.windowing);
    // A controller never gets timed multi-peer scanning.
    assert(ns2_kbm_completion_update(&s, false, false, true, 10u * W) == NS2_KBM_DISCOVERY_IDLE);
    assert(!s.windowing);
    printf("OK:   an ordinary controller idles discovery immediately, with no window\n");
}

static void test_completion_independent_of_effective_mode(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, true, false) == NS2_KBM_MODE_KEYBOARD);
    assert(ns2_kbm_completion_update(&s, true, false, false, 0) == NS2_KBM_DISCOVERY_SEEK);

    memset(&s, 0, sizeof(s));
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, false, true) == NS2_KBM_MODE_KEYBOARD_MOUSE);
    assert(ns2_kbm_completion_update(&s, false, true, false, 0) == NS2_KBM_DISCOVERY_SEEK);
    printf("OK:   the completion window is independent of the AUTO effective mode\n");
}

static void test_completion_wrap_and_null(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    const uint32_t near_wrap = 0xFFFFFFFFu - (W / 2u);
    assert(ns2_kbm_completion_update(&s, true, false, false, near_wrap) == NS2_KBM_DISCOVERY_SEEK);
    assert(ns2_kbm_completion_update(&s, true, false, false,
                                     (uint32_t)(near_wrap + W - 1u)) == NS2_KBM_DISCOVERY_SEEK);
    assert(ns2_kbm_completion_update(&s, true, false, false,
                                     (uint32_t)(near_wrap + W)) == NS2_KBM_DISCOVERY_IDLE);

    assert(ns2_kbm_completion_update(NULL, true, false, false, 0) == NS2_KBM_DISCOVERY_SEEK);
    printf("OK:   completion window is wrap-safe and NULL-safe\n");
}

// Discovery ownership matrix. Hardware-proven regression: with the completion
// window evaluated only while no pairing window was open, the first peer to
// finish connecting inside an explicit pairing window stopped the scan (every
// HID_READY path calls btstack_host_stop_scan()) and nothing re-armed it --
// keyboard connected, source still partial, yet hid_state=0 / scan_active=false
// with scan starts == stops. Explicit pairing is authoritative.
static void test_discovery_ownership_matrix(void) {
    // 1. pairing open + partial KB/M -> discovery ON.
    //    True regardless of the completion window's verdict, including after it
    //    has expired: the user's explicit request outranks it.
    assert(ns2_kbm_discovery_policy(true, false, NS2_KBM_DISCOVERY_SEEK) ==
           NS2_KBM_DISCOVERY_ARM);
    assert(ns2_kbm_discovery_policy(true, false, NS2_KBM_DISCOVERY_IDLE) ==
           NS2_KBM_DISCOVERY_ARM);

    // 2. pairing open + complete -> the window closes itself; never retire here,
    //    or controller replacement (which keeps scanning with a controller
    //    connected) would break.
    assert(ns2_kbm_discovery_policy(true, true, NS2_KBM_DISCOVERY_IDLE) ==
           NS2_KBM_DISCOVERY_LEAVE);
    assert(ns2_kbm_discovery_policy(true, true, NS2_KBM_DISCOVERY_SEEK) ==
           NS2_KBM_DISCOVERY_LEAVE);

    // 3. pairing closed + partial + completion window running -> discovery ON.
    assert(ns2_kbm_discovery_policy(false, false, NS2_KBM_DISCOVERY_SEEK) ==
           NS2_KBM_DISCOVERY_ARM);

    // 4. pairing closed + completion window expired -> discovery OFF.
    assert(ns2_kbm_discovery_policy(false, false, NS2_KBM_DISCOVERY_IDLE) ==
           NS2_KBM_DISCOVERY_RETIRE);

    // 5. pairing closed + complete source (KB/M pair, or one controller) -> OFF.
    assert(ns2_kbm_discovery_policy(false, true, NS2_KBM_DISCOVERY_IDLE) ==
           NS2_KBM_DISCOVERY_RETIRE);

    printf("OK:   discovery ownership matrix: explicit pairing outranks the window\n");
}

// End-to-end lifecycle over the same two functions production calls, in the
// order production calls them: advance the window, then apply the policy.
static void test_discovery_pairing_window_survives_first_peer(void) {
    ns2_kbm_completion_t s;
    memset(&s, 0, sizeof(s));

    // Pairing window open, nothing connected yet -> discovery on.
    ns2_kbm_discovery_t t = ns2_kbm_completion_update(&s, false, false, false, 0);
    assert(ns2_kbm_discovery_policy(true,
               ns2_kbm_logical_source_complete(false, false, false), t) ==
           NS2_KBM_DISCOVERY_ARM);

    // Keyboard finishes connecting INSIDE the pairing window. Its HID_READY
    // stopped the scan; the source is still partial, so discovery must be
    // re-armed on this very tick.
    t = ns2_kbm_completion_update(&s, true, false, false, 100);
    assert(ns2_kbm_discovery_policy(true,
               ns2_kbm_logical_source_complete(true, false, false), t) ==
           NS2_KBM_DISCOVERY_ARM);

    // Still partial much later in the window, after the completion window has
    // expired: explicit pairing keeps discovery on so the mouse can still join.
    t = ns2_kbm_completion_update(&s, true, false, false, 100 + W + 5000);
    assert(t == NS2_KBM_DISCOVERY_IDLE);        // window itself has expired
    assert(ns2_kbm_discovery_policy(true,
               ns2_kbm_logical_source_complete(true, false, false), t) ==
           NS2_KBM_DISCOVERY_ARM);              // ...but pairing outranks it

    // Mouse joins -> source complete -> the window may close and discovery stop.
    t = ns2_kbm_completion_update(&s, true, true, false, 100 + W + 6000);
    assert(ns2_kbm_discovery_policy(true,
               ns2_kbm_logical_source_complete(true, true, false), t) ==
           NS2_KBM_DISCOVERY_LEAVE);
    assert(ns2_kbm_discovery_policy(false,
               ns2_kbm_logical_source_complete(true, true, false), t) ==
           NS2_KBM_DISCOVERY_RETIRE);

    // Mirror: mouse first inside the pairing window.
    memset(&s, 0, sizeof(s));
    t = ns2_kbm_completion_update(&s, false, true, false, 0);
    assert(ns2_kbm_discovery_policy(true,
               ns2_kbm_logical_source_complete(false, true, false), t) ==
           NS2_KBM_DISCOVERY_ARM);
    t = ns2_kbm_completion_update(&s, true, true, false, 500);
    assert(ns2_kbm_discovery_policy(false,
               ns2_kbm_logical_source_complete(true, true, false), t) ==
           NS2_KBM_DISCOVERY_RETIRE);

    printf("OK:   a pairing window keeps discovery up after its first peer connects\n");
}

#undef W

static void test_effective_mode_inference(void) {
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, false, false) ==
           NS2_KBM_MODE_CONTROLLER);
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, true, false) ==
           NS2_KBM_MODE_KEYBOARD);
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, true, true) ==
           NS2_KBM_MODE_KEYBOARD_MOUSE);
    // A mouse alone bootstraps the composite: it is half of a KB/M source.
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, false, true) ==
           NS2_KBM_MODE_KEYBOARD_MOUSE);

    // An explicit Controller override stops inference dead.
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_CONTROLLER, true, true) ==
           NS2_KBM_MODE_CONTROLLER);

    // An explicit Keyboard + Mouse keeps the KB/M profile while the mouse is
    // merely absent, so a missing mouse never demotes the profile.
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_KEYBOARD_MOUSE, true, false) ==
           NS2_KBM_MODE_KEYBOARD_MOUSE);
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_KEYBOARD, true, false) ==
           NS2_KBM_MODE_KEYBOARD);
    puts("  effective mode inference");
}

static void test_roles_and_admission(void) {
    ns2_kbm_roles_t roles;
    ns2_kbm_roles_init(&roles);

    ns2_kbm_peer_key_t keyboard = {1u, {0, 0, 0, 0, 0, 0xA1u}, 1u, 0u, 10u};
    ns2_kbm_peer_key_t mouse = {1u, {0, 0, 0, 0, 0, 0xB2u}, 1u, 1u, 11u};
    ns2_kbm_peer_key_t keyboard2 = {1u, {0, 0, 0, 0, 0, 0xC3u}, 1u, 2u, 12u};
    ns2_kbm_peer_key_t mouse2 = {1u, {0, 0, 0, 0, 0, 0xD4u}, 1u, 3u, 13u};

    // A policy allowing nothing refuses everything.
    assert(ns2_kbm_roles_admit(&roles, policy(false, false), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_REJECT_MODE);
    // A peer that can do nothing is refused too.
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), KB, caps(false, false),
                               &keyboard) == NS2_KBM_ADMIT_REJECT_MODE);
    // An unclassified peer is refused rather than guessed at.
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), NS2_KBM_PRIMARY_NONE,
                               caps(true, true), &keyboard) ==
           NS2_KBM_ADMIT_REJECT_MODE);

    // Keyboard-only policy: one keyboard, no mouse role at all.
    assert(ns2_kbm_roles_admit(&roles, policy(true, false), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    assert(roles.group_id != 0u);
    uint32_t group = roles.group_id;
    assert(ns2_kbm_roles_admit(&roles, policy(true, false), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    assert(roles.group_id == group);
    assert(ns2_kbm_roles_admit(&roles, policy(true, false), MS, caps(false, true),
                               &mouse) == NS2_KBM_ADMIT_REJECT_MODE);
    assert(!roles.mouse.valid);

    // Keyboard-first then mouse: one composite.
    ns2_kbm_roles_init(&roles);
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    group = roles.group_id;
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), MS, caps(false, true),
                               &mouse) == NS2_KBM_ADMIT_MOUSE);
    assert(roles.group_id == group);

    // MOUSE-FIRST must work identically: group creation is not keyboard-biased.
    ns2_kbm_roles_t mouse_first;
    ns2_kbm_roles_init(&mouse_first);
    assert(ns2_kbm_roles_admit(&mouse_first, policy(true, true), MS, caps(false, true),
                               &mouse) == NS2_KBM_ADMIT_MOUSE);
    assert(mouse_first.group_id != 0u);
    assert(mouse_first.mouse.valid && !mouse_first.keyboard.valid);
    uint32_t mouse_group = mouse_first.group_id;
    assert(ns2_kbm_roles_admit(&mouse_first, policy(true, true), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    assert(mouse_first.group_id == mouse_group);
    assert(mouse_first.keyboard.valid && mouse_first.mouse.valid);

    // Duplicates are still refused.
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), KB, caps(true, false),
                               &keyboard2) == NS2_KBM_ADMIT_REJECT_DUPLICATE);
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), MS, caps(false, true),
                               &mouse2) == NS2_KBM_ADMIT_REJECT_DUPLICATE);
    assert(roles.rejected_duplicate == 2u);

    // --- Capability is not role ownership -------------------------------------
    // Hardware case: an ASUS ROG KERIS II gaming mouse reports kbcap=true AND
    // mousecap=true, because its macro buttons put a keyboard collection in its
    // descriptor. It is a MOUSE.
    ns2_kbm_peer_caps_t gaming_mouse = caps(true, true);
    assert(ns2_kbm_primary_from_caps(gaming_mouse) == NS2_KBM_PRIMARY_MOUSE);

    // Mouse-first: it takes the mouse role ONLY, leaving the keyboard role free
    // for the user's actual keyboard. If it took both, the composite would look
    // complete and the real keyboard could never join.
    ns2_kbm_roles_t keris;
    ns2_kbm_roles_init(&keris);
    assert(ns2_kbm_roles_admit(&keris, policy(true, true), MS, gaming_mouse,
                               &mouse) == NS2_KBM_ADMIT_MOUSE);
    assert(keris.mouse.valid && !keris.keyboard.valid);
    // ...and the real keyboard then joins the same composite.
    uint32_t keris_group = keris.group_id;
    assert(ns2_kbm_roles_admit(&keris, policy(true, true), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    assert(keris.keyboard.valid && keris.mouse.valid);
    assert(keris.group_id == keris_group);
    assert(keris.rejected_duplicate == 0u);

    // Keyboard-first, gaming mouse second: the mouse role is free, so it is
    // taken. This is the case the old asymmetric rule rejected 1547 times.
    ns2_kbm_roles_init(&keris);
    assert(ns2_kbm_roles_admit(&keris, policy(true, true), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    assert(ns2_kbm_roles_admit(&keris, policy(true, true), MS, gaming_mouse,
                               &mouse) == NS2_KBM_ADMIT_MOUSE);
    assert(keris.mouse.valid);
    assert(keris.rejected_duplicate == 0u);

    // A second gaming mouse while the mouse role is occupied is a DUPLICATE. It
    // must not fall back to the free keyboard role on the strength of auxiliary
    // macro-key usages.
    ns2_kbm_roles_init(&keris);
    assert(ns2_kbm_roles_admit(&keris, policy(true, true), MS, gaming_mouse,
                               &mouse) == NS2_KBM_ADMIT_MOUSE);
    assert(ns2_kbm_roles_admit(&keris, policy(true, true), MS, gaming_mouse,
                               &mouse2) == NS2_KBM_ADMIT_REJECT_DUPLICATE);
    assert(!keris.keyboard.valid);

    // The mirror: a keyboard with an auxiliary trackpad takes the keyboard role
    // only, and while the keyboard role is occupied it does not become a mouse.
    ns2_kbm_peer_caps_t keyboard_with_trackpad = caps(true, true);
    ns2_kbm_roles_init(&keris);
    assert(ns2_kbm_roles_admit(&keris, policy(true, true), KB,
                               keyboard_with_trackpad, &keyboard) ==
           NS2_KBM_ADMIT_KEYBOARD);
    assert(keris.keyboard.valid && !keris.mouse.valid);
    assert(ns2_kbm_roles_admit(&keris, policy(true, true), KB,
                               keyboard_with_trackpad, &keyboard2) ==
           NS2_KBM_ADMIT_REJECT_DUPLICATE);
    assert(!keris.mouse.valid);

    // --- Genuine COMBO --------------------------------------------------------
    // Only a device that positively declares itself a combined keyboard/pointing
    // peripheral may occupy both roles.
    ns2_kbm_roles_t combo_case;
    ns2_kbm_roles_init(&combo_case);
    ns2_kbm_peer_key_t combo = {1u, {0, 0, 0, 0, 0, 0xE5u}, 1u, 0u, 20u};
    assert(ns2_kbm_roles_admit(&combo_case, policy(true, true), CB, caps(true, true),
                               &combo) == NS2_KBM_ADMIT_BOTH);
    assert(ns2_kbm_roles_contains(&combo_case, &combo));
    assert(ns2_kbm_roles_admit(&combo_case, policy(true, true), MS, caps(false, true),
                               &mouse) == NS2_KBM_ADMIT_REJECT_DUPLICATE);

    // A combo arriving with the keyboard role taken takes the free mouse role.
    ns2_kbm_roles_init(&combo_case);
    assert(ns2_kbm_roles_admit(&combo_case, policy(true, true), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    assert(ns2_kbm_roles_admit(&combo_case, policy(true, true), CB, caps(true, true),
                               &combo) == NS2_KBM_ADMIT_MOUSE);
    assert(combo_case.mouse.valid);

    // ...and with the mouse role taken, the free keyboard role.
    ns2_kbm_roles_init(&combo_case);
    assert(ns2_kbm_roles_admit(&combo_case, policy(true, true), MS, caps(false, true),
                               &mouse) == NS2_KBM_ADMIT_MOUSE);
    assert(ns2_kbm_roles_admit(&combo_case, policy(true, true), CB, caps(true, true),
                               &combo) == NS2_KBM_ADMIT_KEYBOARD);
    assert(combo_case.keyboard.valid && combo_case.mouse.valid);

    // --- Lifetime -------------------------------------------------------------
    // Losing one role keeps the composite identity and the survivor role.
    ns2_kbm_roles_init(&roles);
    (void)ns2_kbm_roles_admit(&roles, policy(true, true), KB, caps(true, false), &keyboard);
    group = roles.group_id;
    (void)ns2_kbm_roles_admit(&roles, policy(true, true), MS, caps(false, true), &mouse);
    bool released_keyboard = false;
    bool released_mouse = false;
    assert(ns2_kbm_roles_release(&roles, &mouse, &released_keyboard, &released_mouse));
    assert(!released_keyboard && released_mouse);
    assert(roles.group_id == group);
    assert(roles.keyboard.valid && !roles.mouse.valid);

    // A stale disconnect carrying an older generation matches nothing.
    ns2_kbm_peer_key_t stale_keyboard = keyboard;
    stale_keyboard.connection_generation = 9u;
    assert(!ns2_kbm_roles_release(&roles, &stale_keyboard, NULL, NULL));
    assert(roles.keyboard.valid);

    // A reconnecting role with a NEW generation rejoins the same composite
    // while the other role stays live.
    ns2_kbm_peer_key_t mouse_reconnected = mouse;
    mouse_reconnected.connection_generation = 77u;
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), MS, caps(false, true),
                               &mouse_reconnected) == NS2_KBM_ADMIT_MOUSE);
    assert(roles.group_id == group);
    // The stale generation has no authority over the replacement.
    assert(!ns2_kbm_roles_release(&roles, &mouse, NULL, NULL));
    assert(roles.mouse.valid);

    // Losing every role retires the composite handle.
    assert(ns2_kbm_roles_release(&roles, &keyboard, NULL, NULL));
    assert(ns2_kbm_roles_release(&roles, &mouse_reconnected, NULL, NULL));
    assert(roles.group_id == 0u);
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    assert(roles.group_id != group && roles.group_id != 0u);

    ns2_kbm_roles_release_all(&roles);
    assert(!roles.keyboard.valid && !roles.mouse.valid && roles.group_id == 0u);
    puts("  roles and admission");
}

// The exact hardware sequence that failed: a complete KB/M source loses one
// role, the partner keeps working, and the bonded peer comes back.
//
// The Bluetooth half of that (re-arming discovery so the missing peer can be
// found) lives in btstack_host.c and is only verifiable on hardware. This pins
// the half that is testable: role lifetime, composite identity, generation
// safety, and the effective-mode transitions around it.
static void test_role_reconnect_while_partner_live(void) {
    ns2_kbm_roles_t roles;
    ns2_kbm_roles_init(&roles);
    ns2_kbm_peer_key_t keyboard = {1u, {0, 0, 0, 0, 0, 0xA1u}, 1u, 5u, 30u};
    ns2_kbm_peer_key_t mouse = {1u, {0, 0, 0, 0, 0, 0xB2u}, 1u, 4u, 31u};

    // 1-3. Mouse first, keyboard joins, both live -> Keyboard + Mouse.
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), MS, caps(true, true),
                               &mouse) == NS2_KBM_ADMIT_MOUSE);
    uint32_t group = roles.group_id;
    assert(group != 0u);
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, roles.keyboard.valid,
                                  roles.mouse.valid) ==
           NS2_KBM_MODE_KEYBOARD_MOUSE);
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    assert(roles.keyboard.valid && roles.mouse.valid);
    assert(roles.group_id == group);

    // 4-5. Mouse powers off. The keyboard survives, the composite identity
    // survives, and the mode falls back to Keyboard.
    bool released_keyboard = false;
    bool released_mouse = false;
    assert(ns2_kbm_roles_release(&roles, &mouse, &released_keyboard,
                                 &released_mouse));
    assert(!released_keyboard && released_mouse);
    assert(roles.keyboard.valid && !roles.mouse.valid);
    assert(roles.group_id == group);
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, roles.keyboard.valid,
                                  roles.mouse.valid) == NS2_KBM_MODE_KEYBOARD);

    // The mouse role must be genuinely free -- a role left occupied by a dead
    // generation is what would block the return even with discovery working.
    assert(!roles.mouse.valid);

    // 6-7. The bonded mouse returns on a NEW connection generation and rejoins
    // the SAME logical relationship. No re-pair, no new group, keyboard
    // untouched.
    ns2_kbm_peer_key_t mouse_back = mouse;
    mouse_back.connection_generation = 99u;
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), MS, caps(true, true),
                               &mouse_back) == NS2_KBM_ADMIT_MOUSE);
    assert(roles.mouse.valid && roles.keyboard.valid);
    assert(roles.group_id == group);
    assert(roles.keyboard.connection_generation == 30u);  // never disturbed
    assert(roles.mouse.connection_generation == 99u);
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, roles.keyboard.valid,
                                  roles.mouse.valid) ==
           NS2_KBM_MODE_KEYBOARD_MOUSE);
    assert(roles.rejected_duplicate == 0u);

    // The dead generation has no authority over the peer that replaced it.
    assert(!ns2_kbm_roles_release(&roles, &mouse, NULL, NULL));
    assert(roles.mouse.valid);
    assert(roles.mouse.connection_generation == 99u);

    // Symmetric: the keyboard can do the same while the mouse stays live.
    assert(ns2_kbm_roles_release(&roles, &keyboard, &released_keyboard,
                                 &released_mouse));
    assert(released_keyboard && !released_mouse);
    assert(roles.mouse.valid && roles.group_id == group);
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, roles.keyboard.valid,
                                  roles.mouse.valid) ==
           NS2_KBM_MODE_KEYBOARD_MOUSE);  // mouse-only is a partial KB/M source
    ns2_kbm_peer_key_t keyboard_back = keyboard;
    keyboard_back.connection_generation = 100u;
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), KB, caps(true, false),
                               &keyboard_back) == NS2_KBM_ADMIT_KEYBOARD);
    assert(roles.keyboard.valid && roles.mouse.valid);
    assert(roles.group_id == group);
    puts("  role reconnect while partner live");
}

// A freed role must never be absorbed by a surviving peer.
//
// Hardware failure this reproduces: a KERIS II gaming mouse (primary MOUSE,
// caps {keyboard, pointer}, bound to the keyboard driver) held the mouse role
// alongside a real keyboard. The keyboard powered off, freeing the keyboard
// role. Repeated re-admission of the mouse -- ns2_kbm_runtime_note_ready() runs
// on every raw report -- then handed it the keyboard role as well:
//
//     keyboardConn == mouseConn == 5, keyboardReports frozen at 93
//
// The source looked complete, so discovery stopped and the real keyboard could
// never return.
static void test_freed_role_is_not_absorbed(void) {
    ns2_kbm_roles_t roles;
    ns2_kbm_roles_init(&roles);
    ns2_kbm_peer_caps_t keris = caps(true, true);   // macro keys + pointer
    ns2_kbm_peer_key_t mouse = {1u, {0, 0, 0, 0, 0, 0xB2u}, 1u, 5u, 3u};
    ns2_kbm_peer_key_t keyboard = {1u, {0, 0, 0, 0, 0, 0xA1u}, 1u, 4u, 1u};

    assert(ns2_kbm_roles_admit(&roles, policy(true, true), MS, keris, &mouse) ==
           NS2_KBM_ADMIT_MOUSE);
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), KB, caps(true, false),
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    uint32_t group = roles.group_id;

    // The real keyboard powers off. Its role frees; the mouse keeps its own.
    assert(ns2_kbm_roles_release(&roles, &keyboard, NULL, NULL));
    assert(!roles.keyboard.valid && roles.mouse.valid);

    // Now hammer re-admission the way the runtime does on every raw report.
    // The mouse must stay mouse-only through all of it.
    for (unsigned i = 0; i < 64u; ++i) {
        ns2_kbm_admit_t admit =
            ns2_kbm_roles_admit(&roles, policy(true, true), MS, keris, &mouse);
        assert(admit == NS2_KBM_ADMIT_MOUSE);
        assert(!roles.keyboard.valid);
    }

    // Even if something re-derives its primary WRONGLY as KEYBOARD -- which is
    // exactly what a lost classification did on hardware -- a peer already
    // holding the mouse role must not expand into the free keyboard role.
    for (unsigned i = 0; i < 8u; ++i) {
        (void)ns2_kbm_roles_admit(&roles, policy(true, true), KB, keris, &mouse);
        assert(!roles.keyboard.valid);
        assert(roles.mouse.valid);
        assert(roles.mouse.conn_index == 5u);
    }

    // The real keyboard returns on a NEW generation and takes the free role.
    ns2_kbm_peer_key_t keyboard_back = keyboard;
    keyboard_back.conn_index = 6u;
    keyboard_back.connection_generation = 9u;
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), KB, caps(true, false),
                               &keyboard_back) == NS2_KBM_ADMIT_KEYBOARD);
    assert(roles.keyboard.valid && roles.mouse.valid);
    assert(roles.group_id == group);
    // Distinct peers, which is what keyboardConn == mouseConn proved was wrong.
    assert(roles.keyboard.conn_index != roles.mouse.conn_index);
    assert(ns2_kbm_effective_mode(NS2_KBM_MODE_AUTO, roles.keyboard.valid,
                                  roles.mouse.valid) ==
           NS2_KBM_MODE_KEYBOARD_MOUSE);

    // Mirror: a keyboard-primary peer with auxiliary pointer capability must not
    // absorb a freed mouse role.
    ns2_kbm_roles_init(&roles);
    ns2_kbm_peer_caps_t trackpad_keyboard = caps(true, true);
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), KB, trackpad_keyboard,
                               &keyboard) == NS2_KBM_ADMIT_KEYBOARD);
    assert(ns2_kbm_roles_admit(&roles, policy(true, true), MS, caps(false, true),
                               &mouse) == NS2_KBM_ADMIT_MOUSE);
    assert(ns2_kbm_roles_release(&roles, &mouse, NULL, NULL));
    for (unsigned i = 0; i < 32u; ++i) {
        (void)ns2_kbm_roles_admit(&roles, policy(true, true), KB,
                                  trackpad_keyboard, &keyboard);
        assert(!roles.mouse.valid);
    }
    puts("  freed role is not absorbed");
}

int main(void) {
    puts("ns2_kbm:");
    test_identifier_validation();
    test_destination_wire_bits();
    test_canonical_defaults();
    test_keyboard_mapping();
    test_opposing_directions();
    test_overrides_and_profile_independence();
    test_duplicate_destinations();
    test_remap_while_held();
    test_mouse_translation();
    test_state_ownership();
    test_config_validation();
    test_effective_mode_inference();
    test_logical_source_completeness();
    test_discovery_reopens_on_role_loss();
    test_completeness_not_derived_from_effective_mode();
    test_completion_keyboard_first();
    test_completion_mouse_first();
    test_completion_complement_joins();
    test_completion_role_loss_reopens();
    test_completion_not_extended_by_service();
    test_completion_settled_then_complement_joins();
    test_completion_all_roles_gone();
    test_completion_role_swap_restarts();
    test_completion_ordinary_controller();
    test_completion_independent_of_effective_mode();
    test_completion_wrap_and_null();
    test_discovery_ownership_matrix();
    test_discovery_pairing_window_survives_first_peer();
    test_primary_from_caps();
    test_roles_and_admission();
    test_role_reconnect_while_partner_live();
    test_freed_role_is_not_absorbed();
    puts("ns2_kbm tests passed");
    return 0;
}
