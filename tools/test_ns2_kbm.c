// Keyboard / Keyboard + Mouse model tests.
//
// Host-only: no Pico SDK, no BTstack, no console, no connected hardware. This
// is the authority for the mapping, merge, opposing-direction, duplicate
// binding, remap-neutralization, and mouse-translation contracts.

#include <assert.h>
#include <math.h>
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

// Mirrors the private KBM_STICK_LIMIT in src/ns2_kbm.c. test_mouse_translation()
// pins it against SWITCH_STICK_MAX, so the two cannot drift apart silently.
#define KBM_STICK_FULL_SCALE 2048

// ---------------------------------------------------------------------------
// Mouse-to-stick temporal harness
// ---------------------------------------------------------------------------
// The mouse-to-stick translator is a TIME-domain component, so testing it one
// report at a time proves nothing about how it feels. This drives it exactly the
// way the firmware does: mouse reports on a Bluetooth-like cadence, plus the
// production 3 ms core-1 service tick (RUMBLE_TICK_MS in ns2_bt_host.c), and
// samples the deflection every simulated millisecond.
#define SIM_TICK_MS 3u

typedef struct {
    ns2_kbm_config_t config;
    ns2_kbm_state_t state;
    uint32_t now_ms;
} mouse_sim_t;

typedef struct {
    int32_t min_x, max_x;
    int32_t min_y, max_y;
    unsigned centred;  // samples with the translated stick at exact neutral
    unsigned samples;
} sim_stats_t;

static void sim_init(mouse_sim_t *sim) {
    ns2_kbm_config_defaults(&sim->config);
    ns2_kbm_state_init(&sim->state);
    sim->now_ms = 0u;
}

static void stats_init(sim_stats_t *stats) {
    stats->min_x = stats->min_y = INT32_MAX;
    stats->max_x = stats->max_y = INT32_MIN;
    stats->centred = 0u;
    stats->samples = 0u;
}

// Run for `duration_ms`, delivering (dx, dy) every `period_ms` (0 = no reports
// at all, i.e. the mouse is not moving). Samples taken after `settle_ms` are
// folded into `stats`, which may be NULL.
static void sim_run(mouse_sim_t *sim, int dx, int dy, unsigned period_ms,
                    unsigned duration_ms, unsigned settle_ms,
                    sim_stats_t *stats) {
    uint32_t start = sim->now_ms;
    for (unsigned step = 1u; step <= duration_ms; ++step) {
        sim->now_ms = start + step;
        if (period_ms && (step % period_ms) == 0u)
            ns2_kbm_state_mouse_report(&sim->state, 0, (int16_t)dx, (int16_t)dy,
                                       0, &sim->config.mouse, sim->now_ms);
        if ((step % SIM_TICK_MS) == 0u)
            ns2_kbm_state_service(&sim->state, &sim->config.mouse, sim->now_ms);
        if (!stats || step < settle_ms) continue;
        if (sim->state.stick_x < stats->min_x) stats->min_x = sim->state.stick_x;
        if (sim->state.stick_x > stats->max_x) stats->max_x = sim->state.stick_x;
        if (sim->state.stick_y < stats->min_y) stats->min_y = sim->state.stick_y;
        if (sim->state.stick_y > stats->max_y) stats->max_y = sim->state.stick_y;
        if (sim->state.stick_x == 0 && sim->state.stick_y == 0) stats->centred++;
        stats->samples++;
    }
}

// Continuous movement must look to the console like a stick being HELD, not
// like one being tapped.
//
// This is the regression for the model that shipped before it. That one was a
// leaky position accumulator -- `stick += delta * sensitivity` against a
// constant friction of KBM_STICK_LIMIT / recenter_ms per millisecond -- so
// deflection existed only while the mouse outran the friction. On the shipped
// defaults the break-even was 8.53 counts/ms (~8533 counts/s): measured, the
// three slower cases below sat at EXACT CENTRE 50%, 75% and 80% of the time and
// never exceeded 40, 10 and 2 units of a 2048 full scale. Everything asserted
// here about sustained deflection therefore failed under that model, which is
// what the user felt in Splatoon as move / brief turn / stop / move / brief
// turn / stop.
static void test_mouse_sustained_motion_holds_deflection(void) {
    mouse_sim_t sim;
    sim_stats_t slow, medium, fast;

    // Slow: 5 counts every 8 ms = 0.62 counts/ms, an order of magnitude below
    // the old model's break-even speed.
    sim_init(&sim);
    stats_init(&slow);
    sim_run(&sim, 5, 0, 8u, 300u, 150u, &slow);
    assert(slow.samples > 100u);
    assert(slow.centred == 0u);   // never collapses between reports
    assert(slow.min_x > 100);     // and stays meaningfully deflected
    assert(slow.max_x < 300);

    // Medium: 20 counts every 8 ms.
    sim_init(&sim);
    stats_init(&medium);
    sim_run(&sim, 20, 0, 8u, 300u, 150u, &medium);
    assert(medium.centred == 0u);
    assert(medium.min_x > 450);

    // Faster movement means MORE deflection, with no overlap between speeds.
    assert(medium.min_x > slow.max_x);

    // Fast: 50 counts every 8 ms = 6.25 counts/ms, still under the old
    // break-even, so the old model produced pulses here too.
    sim_init(&sim);
    stats_init(&fast);
    sim_run(&sim, 50, 0, 8u, 300u, 150u, &fast);
    assert(fast.centred == 0u);
    assert(fast.min_x > medium.max_x);
    assert(fast.max_x <= KBM_STICK_FULL_SCALE);

    // Ripple between reports must stay small: the console sees a level, not a
    // sawtooth. (Old model: the trough was zero in every one of these cases.)
    assert(medium.min_x * 4 > medium.max_x * 3);  // trough > 75% of peak
    puts("  mouse sustained motion holds deflection");
}

// The old model's hidden velocity threshold must be gone: useful deflection has
// to exist far below the rate that used to be needed merely to beat the decay.
static void test_mouse_low_speed_has_no_threshold(void) {
    mouse_sim_t sim;
    sim_stats_t stats;

    // 1 count every 10 ms = 0.1 counts/ms, 85x below the old break-even. The
    // old model produced 0 for 80% of samples and never more than 2 units.
    sim_init(&sim);
    stats_init(&stats);
    sim_run(&sim, 1, 0, 10u, 400u, 200u, &stats);
    assert(stats.centred == 0u);
    assert(stats.min_x > 10);

    // Sub-unit contributions survive at minimum sensitivity too: the Q8.8 scale
    // is divided out at the output, not truncated per report. The old model
    // computed delta * 16 / 256 == 0 for every report of fewer than 16 counts,
    // so the minimum sensitivity was not merely insensitive, it was deaf.
    sim_init(&sim);
    sim.config.mouse.sensitivity_x = NS2_KBM_MOUSE_SENS_MIN;
    stats_init(&stats);
    sim_run(&sim, 4, 0, 8u, 400u, 200u, &stats);
    assert(stats.min_x > 0);
    puts("  mouse low speed has no threshold");
}

// Stopping must end the camera command promptly and at EXACT centre.
static void test_mouse_release_returns_to_exact_centre(void) {
    mouse_sim_t sim;
    ns2_kbm_output_t out;

    sim_init(&sim);
    sim_run(&sim, 40, 0, 8u, 200u, 0u, NULL);
    assert(sim.state.stick_x > 500);

    // Silence. The deflection must be gone by the inactivity deadline plus one
    // service tick, and it must be exactly neutral rather than nearly so.
    sim_run(&sim, 0, 0, 0u, NS2_KBM_MOUSE_IDLE_MS + SIM_TICK_MS, 0u, NULL);
    assert(sim.state.stick_x == 0 && sim.state.stick_y == 0);
    assert(sim.state.motion_x == 0 && sim.state.motion_y == 0);
    assert(!ns2_kbm_state_mouse_motion_pending(&sim.state));
    ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE, false,
                    &out);
    assert(out.right_x == SWITCH_STICK_MID && out.right_y == SWITCH_STICK_MID);

    // It must also already be DECELERATING well before the deadline, so the
    // release reads as the camera stopping rather than as a hard cut.
    sim_init(&sim);
    sim_run(&sim, 40, 0, 8u, 200u, 0u, NULL);
    int32_t held = sim.state.stick_x;
    sim_run(&sim, 0, 0, 0u, NS2_KBM_MOUSE_VELOCITY_MS, 0u, NULL);
    assert(sim.state.stick_x < held / 2);

    // No wind-up: a violent flick releases on the same deadline as a gentle
    // one. An unclamped velocity estimate would keep the stick pinned for as
    // long as it took to bleed off, which is post-motion camera drift.
    sim_init(&sim);
    sim_run(&sim, 4000, 0, 4u, 400u, 0u, NULL);
    assert(sim.state.stick_x == KBM_STICK_FULL_SCALE);
    sim_run(&sim, 0, 0, 0u, NS2_KBM_MOUSE_IDLE_MS + SIM_TICK_MS, 0u, NULL);
    assert(sim.state.stick_x == 0);
    puts("  mouse release returns to exact centre");
}

// A gesture is a span of time, not a run of non-empty reports.
static void test_mouse_gesture_continuity(void) {
    mouse_sim_t sim;
    sim_stats_t stats;

    // Sparse reports well inside the inactivity allowance must not produce
    // visible right / centre / right pulses.
    sim_init(&sim);
    stats_init(&stats);
    sim_run(&sim, 30, 0, 20u, 400u, 200u, &stats);
    assert(stats.centred == 0u);
    assert(stats.min_x > 200);

    // Neither may an empty report inside a live gesture: mouse report timing and
    // quantization put dx == dy == 0 reports in the middle of continuous
    // physical motion all the time.
    sim_init(&sim);
    sim_run(&sim, 30, 0, 8u, 200u, 0u, NULL);
    int32_t before = sim.state.stick_x;
    ns2_kbm_state_mouse_report(&sim.state, 0, 0, 0, 0, &sim.config.mouse,
                               ++sim.now_ms);
    assert(sim.state.stick_x > before / 2);

    // Past the allowance, exact centre -- and a button-only report does not
    // extend the gesture, because only real movement does.
    sim_init(&sim);
    sim_run(&sim, 30, 0, 8u, 200u, 0u, NULL);
    for (unsigned i = 0; i < 4u; ++i) {
        sim.now_ms += NS2_KBM_MOUSE_IDLE_MS / 2u;
        ns2_kbm_state_mouse_report(&sim.state, 1u << 0, 0, 0, 0,
                                   &sim.config.mouse, sim.now_ms);
    }
    assert(sim.state.stick_x == 0 && sim.state.motion_x == 0);
    puts("  mouse gesture continuity");
}

// Reversing direction must reorient the stick, not fight a stale tail.
static void test_mouse_direction_reversal(void) {
    mouse_sim_t sim;
    sim_stats_t stats;

    sim_init(&sim);
    sim_run(&sim, 30, 0, 8u, 200u, 0u, NULL);
    assert(sim.state.stick_x > 500);

    // Crossing centre must take on the order of the estimator time constant,
    // not the length of the gesture that preceded it.
    sim_run(&sim, -30, 0, 8u, NS2_KBM_MOUSE_VELOCITY_MS, 0u, NULL);
    assert(sim.state.stick_x < 0);

    // And it settles at the mirror image of the original deflection.
    stats_init(&stats);
    sim_run(&sim, -30, 0, 8u, 200u, 100u, &stats);
    assert(stats.max_x < -500);
    assert(stats.centred == 0u);
    puts("  mouse direction reversal");
}

// Speed maps to magnitude monotonically until the axis runs out of range, and
// the axes are independent.
static void test_mouse_speed_scaling_and_axes(void) {
    mouse_sim_t sim;
    sim_stats_t stats;

    // Above the speed that maps to full scale, the output stays at full scale.
    // A faster mouse cannot make the game turn faster than its own maximum
    // analog turn rate; that is a property of the destination, not a defect.
    sim_init(&sim);
    stats_init(&stats);
    sim_run(&sim, 2000, 0, 8u, 300u, 150u, &stats);
    assert(stats.max_x == KBM_STICK_FULL_SCALE);
    assert(stats.min_x > KBM_STICK_FULL_SCALE / 2);

    // Diagonal movement: equal speeds give equal magnitudes, and the axes carry
    // their own estimates rather than sharing one.
    sim_init(&sim);
    stats_init(&stats);
    sim_run(&sim, 20, -20, 8u, 300u, 150u, &stats);
    assert(stats.min_x > 450);
    assert(stats.max_y < -450);
    assert(stats.min_x == -stats.max_y);
    assert(stats.max_x <= KBM_STICK_FULL_SCALE);

    // One axis moving leaves the other at exact centre.
    sim_init(&sim);
    stats_init(&stats);
    sim_run(&sim, 20, 0, 8u, 300u, 0u, &stats);
    assert(stats.min_y == 0 && stats.max_y == 0);

    // The reference interval is live configuration, so it can change with a
    // gesture in flight. Both extremes of the range have to stay bounded: the
    // estimate is held in scaled counts sized against the interval in force
    // when it was stored, and the largest one is 200x the smallest.
    sim_init(&sim);
    sim.config.mouse.recenter_ms = NS2_KBM_MOUSE_RECENTER_MIN_MS;
    sim_run(&sim, 4000, 4000, 4u, 200u, 0u, NULL);
    assert(sim.state.stick_x == KBM_STICK_FULL_SCALE);
    sim.config.mouse.recenter_ms = NS2_KBM_MOUSE_RECENTER_MAX_MS;
    stats_init(&stats);
    sim_run(&sim, 4000, 4000, 4u, 200u, 0u, &stats);
    assert(stats.max_x == KBM_STICK_FULL_SCALE);
    assert(stats.min_x >= 0 && stats.min_y >= 0);   // no wrapped product
    sim_run(&sim, 0, 0, 0u, NS2_KBM_MOUSE_IDLE_MS + SIM_TICK_MS, 0u, NULL);
    assert(sim.state.stick_x == 0 && sim.state.stick_y == 0);
    puts("  mouse speed scaling and axes");
}

// The translated stick is for personalities WITHOUT a pointer. The Joy-Con 2
// native mouse path is hardware validated and takes relative deltas unchanged,
// with no sensitivity, velocity estimate, continuity or release behavior
// applied to it.
static void test_mouse_native_path_untouched(void) {
    mouse_sim_t sim;
    ns2_kbm_output_t out;

    sim_init(&sim);
    sim_run(&sim, 30, -10, 8u, 200u, 0u, NULL);
    ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE, true,
                    &out);
    assert(out.has_mouse == 1);
    // Whatever accumulated since the last publish, verbatim and unscaled: 25
    // reports of (30, -10) over the 200 ms gesture, with no sensitivity, no
    // velocity estimate and no release applied to any of it.
    assert(out.mouse_delta_x == 25 * 30 && out.mouse_delta_y == 25 * -10);
    // Mid-gesture, with a large velocity estimate live, the stick is untouched.
    assert(sim.state.stick_x > 500);
    assert(out.right_x == SWITCH_STICK_MID && out.right_y == SWITCH_STICK_MID);

    // Relative movement stays an event: a republish with no new motion must not
    // replay it, however deflected the translated stick happens to be.
    ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE, true,
                    &out);
    assert(out.has_mouse == 1);
    assert(out.mouse_delta_x == 0 && out.mouse_delta_y == 0);
    assert(out.mouse_delta_wheel == 0);
    puts("  mouse native path untouched");
}

// A held digital right-stick binding remains authoritative.
static void test_mouse_digital_stick_precedence(void) {
    mouse_sim_t sim;
    ns2_kbm_output_t out;

    sim_init(&sim);
    // IJKL are deliberately unbound in the Keyboard + Mouse profile, so bind one
    // explicitly rather than relying on a default that does not exist.
    assert(ns2_kbm_set_binding(&sim.config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                               key(KEY_I), NS2_DST_RSTICK_UP));
    sim_run(&sim, 30, 30, 8u, 200u, 0u, NULL);
    assert(sim.state.stick_x > 500);

    const uint8_t aim_up[] = {KEY_I};
    press_keys(&sim.state, aim_up, 1);
    ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE, false,
                    &out);
    // The digital binding owns the whole right stick while it is held: the
    // translated deflection does not leak onto the other axis either.
    assert(out.right_y == SWITCH_STICK_MAX);
    assert(out.right_x == SWITCH_STICK_MID);

    // Releasing it hands the axis back to the live gesture.
    release_all(&sim.state);
    ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE, false,
                    &out);
    assert(out.right_x > SWITCH_STICK_MID);
    puts("  mouse digital stick precedence");
}

// ---------------------------------------------------------------------------
// Radial anti-deadzone (output response mapping)
// ---------------------------------------------------------------------------
// Compensation for a destination that discards the bottom of its stick range.
// It is applied AFTER the velocity estimator and must not disturb any temporal
// property: at 0 it is a byte-for-byte no-op, and at any value a zero vector
// stays exactly zero.

static void adz(int32_t *x, int32_t *y, uint8_t percent) {
    ns2_kbm_mouse_anti_deadzone(x, y, percent);
}

// Magnitude, computed in floating point HERE on purpose: the implementation's
// integer sqrt is the thing under test, so the test must not reuse it.
static double magnitude_of(int32_t x, int32_t y) {
    return sqrt((double)x * x + (double)y * y);
}

static void test_anti_deadzone_zero_and_identity(void) {
    // Off is an exact no-op at every magnitude, including the extremes.
    const int32_t samples[][2] = {{0, 0},       {1, 0},      {-1, 0},
                                  {0, 1},       {37, -11},   {1024, 1024},
                                  {2048, 0},    {-2048, -2048}, {5, 5}};
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        int32_t x = samples[i][0], y = samples[i][1];
        adz(&x, &y, 0);
        assert(x == samples[i][0] && y == samples[i][1]);
    }

    // A zero vector stays EXACTLY zero at every configured value. No setting may
    // turn "not moving the mouse" into stick deflection.
    for (unsigned percent = 0; percent <= NS2_KBM_MOUSE_ADZ_MAX; ++percent) {
        int32_t x = 0, y = 0;
        adz(&x, &y, (uint8_t)percent);
        assert(x == 0 && y == 0);
    }
    // Including values above the cap, which clamp rather than misbehave.
    int32_t x = 0, y = 0;
    adz(&x, &y, 255u);
    assert(x == 0 && y == 0);
    puts("  anti-deadzone zero and identity");
}

static void test_anti_deadzone_magnitude_mapping(void) {
    const int32_t full = KBM_STICK_FULL_SCALE;
    for (unsigned percent = 5; percent <= NS2_KBM_MOUSE_ADZ_MAX; percent += 5) {
        double floor_units = (double)full * percent / 100.0;

        // The smallest possible movement lands on the floor, not above it.
        int32_t x = 1, y = 0;
        adz(&x, &y, (uint8_t)percent);
        assert(x >= (int32_t)floor_units - 2 && x <= (int32_t)floor_units + 2);
        assert(y == 0);

        // Full scale stays full scale: this may only ever raise a deflection.
        x = full;
        y = 0;
        adz(&x, &y, (uint8_t)percent);
        assert(x == full && y == 0);

        // A magnitude already past full scale (diagonal corners reach 1.41x) is
        // left alone rather than rescaled down.
        x = full;
        y = full;
        adz(&x, &y, (uint8_t)percent);
        assert(x == full && y == full);

        // Halfway in maps halfway between the floor and full.
        x = full / 2;
        y = 0;
        adz(&x, &y, (uint8_t)percent);
        double expected = floor_units + (full - floor_units) * 0.5;
        assert(fabs((double)x - expected) <= 2.0);

        // Monotonic and bounded across the whole input range.
        int32_t previous = -1;
        for (int32_t in = 0; in <= full; in += 16) {
            x = in;
            y = 0;
            adz(&x, &y, (uint8_t)percent);
            assert(x >= previous);        // never folds back on itself
            assert(x <= full);            // never leaves the axis range
            assert(in == 0 || x >= in);   // never shrinks a real deflection
            previous = x;
        }
    }
    puts("  anti-deadzone magnitude mapping");
}

// The reason this is radial and not a pair of per-axis floors: independent
// floors rotate the vector, turning a slow nearly-horizontal sweep into a
// diagonal one.
static void test_anti_deadzone_preserves_direction(void) {
    const uint8_t percent = 15u;

    // Cardinals stay exactly cardinal, in all four directions.
    const int32_t cardinals[][2] = {{300, 0}, {-300, 0}, {0, 300}, {0, -300}};
    for (unsigned i = 0; i < 4; ++i) {
        int32_t x = cardinals[i][0], y = cardinals[i][1];
        adz(&x, &y, percent);
        assert((cardinals[i][0] == 0) == (x == 0));
        assert((cardinals[i][1] == 0) == (y == 0));
        // Sign is never inverted.
        assert(cardinals[i][0] <= 0 ? x <= 0 : x >= 0);
        assert(cardinals[i][1] <= 0 ? y <= 0 : y >= 0);
    }

    // Exact diagonal stays exact.
    int32_t x = 400, y = -400;
    adz(&x, &y, percent);
    assert(x == -y);

    // The case that motivated the radial form: 20% right, 1% up. A per-axis
    // floor would lift the tiny component to the floor too and swing this from
    // ~3 degrees to ~26. Radial keeps the angle.
    const double tolerance_degrees = 1.0;
    const int32_t vectors[][2] = {
        {410, 20}, {410, -20}, {-410, 20}, {20, 410}, {1000, 3}, {60, 5},
    };
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        int32_t ax = vectors[i][0], ay = vectors[i][1];
        double before = atan2((double)ay, (double)ax);
        adz(&ax, &ay, percent);
        double after = atan2((double)ay, (double)ax);
        double drift = fabs(before - after) * 180.0 / 3.14159265358979;
        if (drift > tolerance_degrees) {
            printf("FAIL: (%d,%d) rotated by %.2f degrees\n", vectors[i][0],
                   vectors[i][1], drift);
            assert(0);
        }
        // And the magnitude really was raised over the floor.
        assert(magnitude_of(ax, ay) >=
               (double)KBM_STICK_FULL_SCALE * percent / 100.0 - 2.0);
    }
    puts("  anti-deadzone preserves direction");
}

// The configured percentage must mean that percentage of RADIAL magnitude at
// every angle -- which is a statement about the precision of the divisor, not
// about the shape of the mapping.
//
// This is a regression for a real defect. The first implementation took the
// magnitude as floor(sqrt(x^2 + y^2)) in whole stick units and then divided by
// it, so any vector whose true magnitude was not an integer had its output
// inflated by the truncation. The smallest diagonal was the worst case: |(1,1)|
// is 1.414, floored to 1, and a configured 15% radial floor came out as 21.2%
// (50% came out as 70.7%) while a cardinal vector of the same magnitude got its
// 15% exactly. Direction was fine; the magnitude was not. The magnitude is now
// computed in sixteenths of a unit.
static void test_anti_deadzone_radial_precision(void) {
    const uint8_t percents[] = {5, 10, 15, 20, 25, 50};
    const int32_t vectors[][2] = {
        {1, 0}, {0, 1}, {1, 1},  {-1, 1}, {2, 1},  {1, 2},
        {3, 3}, {5, 5}, {7, 7},  {7, 0},  {10, 0}, {2, 0},
    };
    for (unsigned p = 0; p < sizeof(percents) / sizeof(percents[0]); ++p) {
        double floor_units =
            (double)(KBM_STICK_FULL_SCALE * percents[p] / 100);
        for (unsigned v = 0; v < sizeof(vectors) / sizeof(vectors[0]); ++v) {
            int32_t x = vectors[v][0], y = vectors[v][1];
            double before = magnitude_of(x, y);
            adz(&x, &y, percents[p]);
            double after = magnitude_of(x, y);
            // floor, plus the linear ramp's contribution for this input.
            double expected =
                floor_units +
                before * (KBM_STICK_FULL_SCALE - floor_units) /
                    KBM_STICK_FULL_SCALE;
            double tolerance = expected * 0.05 + 3.0;
            if (fabs(after - expected) > tolerance) {
                printf("FAIL: (%d,%d) at %u%% -> magnitude %.2f, expected "
                       "%.2f (+/- %.2f)\n",
                       vectors[v][0], vectors[v][1], percents[p], after,
                       expected, tolerance);
                assert(0);
            }
        }
    }

    // Equal physical magnitude at different angles must receive comparable
    // compensation. |(7,0)| = 7.00 and |(5,5)| = 7.07; the broken version gave
    // 15.23% and 16.85% for these at a configured 15%.
    for (unsigned p = 0; p < sizeof(percents) / sizeof(percents[0]); ++p) {
        int32_t cx = 7, cy = 0, dx = 5, dy = 5;
        adz(&cx, &cy, percents[p]);
        adz(&dx, &dy, percents[p]);
        double cardinal = magnitude_of(cx, cy);
        double diagonal = magnitude_of(dx, dy);
        assert(fabs(cardinal - diagonal) <= cardinal * 0.03 + 2.0);
    }

    // And the specific numbers from the defect report, stated as bounds rather
    // than as a formula, so a future rewrite has to reproduce the behaviour and
    // not merely the arithmetic.
    int32_t x = 1, y = 1;
    adz(&x, &y, 15u);
    double m = magnitude_of(x, y);
    assert(m > (double)KBM_STICK_FULL_SCALE * 0.14);
    assert(m < (double)KBM_STICK_FULL_SCALE * 0.17);  // was 0.212
    x = 1;
    y = 1;
    adz(&x, &y, 50u);
    m = magnitude_of(x, y);
    assert(m > (double)KBM_STICK_FULL_SCALE * 0.48);
    assert(m < (double)KBM_STICK_FULL_SCALE * 0.54);  // was 0.707
    puts("  anti-deadzone radial precision");
}

// The integer sqrt is the one piece of new arithmetic; exercise its edges and
// prove nothing overflows at the extremes of the axis range.
static void test_anti_deadzone_bounds_and_overflow(void) {
    const int32_t full = KBM_STICK_FULL_SCALE;
    for (uint8_t percent = 0; percent <= NS2_KBM_MOUSE_ADZ_MAX; ++percent) {
        const int32_t extremes[][2] = {
            {full, full},   {-full, -full}, {full, -full}, {-full, full},
            {full, 1},      {1, full},      {full - 1, full - 1},
            {1, 1},         {-1, -1},       {2, 1},        {0, full},
        };
        for (unsigned i = 0; i < sizeof(extremes) / sizeof(extremes[0]); ++i) {
            int32_t x = extremes[i][0], y = extremes[i][1];
            adz(&x, &y, percent);
            // Bounded, sign-preserving, and never NaN-ish garbage from a bad
            // division.
            assert(x >= -full && x <= full);
            assert(y >= -full && y <= full);
            assert(extremes[i][0] < 0 ? x <= 0 : x >= 0);
            assert(extremes[i][1] < 0 ? y <= 0 : y >= 0);
        }
    }
    assert((ns2_kbm_mouse_anti_deadzone(NULL, NULL, 15u), true));
    int32_t only_x = 100;
    ns2_kbm_mouse_anti_deadzone(&only_x, NULL, 15u);
    assert(only_x == 100);
    puts("  anti-deadzone bounds and overflow");
}

// End to end through the real translator: the mapping reaches the wire, the
// temporal model is untouched by it, and the paths that must not see it do not.
static void test_anti_deadzone_through_resolve(void) {
    mouse_sim_t sim;
    ns2_kbm_output_t out;

    // A slow sweep that lands well under a game deadzone today.
    sim_init(&sim);
    sim_run(&sim, 1, 0, 10u, 400u, 0u, NULL);
    int32_t slow_state = sim.state.stick_x;
    assert(slow_state > 0 && slow_state < 60);  // ~1% of full scale

    ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE, false,
                    &out);
    uint16_t linear = out.right_x;

    sim.config.mouse.anti_deadzone = 15u;
    ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE, false,
                    &out);
    uint16_t compensated = out.right_x;
    assert(compensated > linear);
    // Above 15% of full scale, i.e. out of a 15% deadzone.
    assert(compensated - SWITCH_STICK_MID >= KBM_STICK_FULL_SCALE * 15 / 100);
    // The estimator itself was not touched by the mapping.
    assert(sim.state.stick_x == slow_state);

    // Stationary mouse: exact centre at every value, even mid-gesture state.
    for (unsigned percent = 0; percent <= NS2_KBM_MOUSE_ADZ_MAX; percent += 5) {
        sim_init(&sim);
        sim.config.mouse.anti_deadzone = (uint8_t)percent;
        sim_run(&sim, 30, 30, 8u, 200u, 0u, NULL);
        sim_run(&sim, 0, 0, 0u, NS2_KBM_MOUSE_IDLE_MS + SIM_TICK_MS, 0u, NULL);
        ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE,
                        false, &out);
        assert(out.right_x == SWITCH_STICK_MID);
        assert(out.right_y == SWITCH_STICK_MID);
    }

    // A fresh state with no mouse activity at all publishes exact centre.
    sim_init(&sim);
    sim.config.mouse.anti_deadzone = NS2_KBM_MOUSE_ADZ_MAX;
    ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE, false,
                    &out);
    assert(out.right_x == SWITCH_STICK_MID && out.right_y == SWITCH_STICK_MID);

    // Native Joy-Con pointer output never sees it: deltas verbatim, stick centred.
    sim_init(&sim);
    sim.config.mouse.anti_deadzone = NS2_KBM_MOUSE_ADZ_MAX;
    sim_run(&sim, 3, -2, 8u, 200u, 0u, NULL);
    ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE, true,
                    &out);
    assert(out.has_mouse == 1);
    assert(out.mouse_delta_x == 25 * 3 && out.mouse_delta_y == 25 * -2);
    assert(out.right_x == SWITCH_STICK_MID && out.right_y == SWITCH_STICK_MID);

    // A held digital right-stick binding still wins outright.
    sim_init(&sim);
    sim.config.mouse.anti_deadzone = 20u;
    assert(ns2_kbm_set_binding(&sim.config, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                               key(KEY_I), NS2_DST_RSTICK_UP));
    sim_run(&sim, 30, 0, 8u, 200u, 0u, NULL);
    const uint8_t aim_up[] = {KEY_I};
    press_keys(&sim.state, aim_up, 1);
    ns2_kbm_resolve(&sim.state, &sim.config, NS2_KBM_MODE_KEYBOARD_MOUSE, false,
                    &out);
    assert(out.right_y == SWITCH_STICK_MAX);
    assert(out.right_x == SWITCH_STICK_MID);
    puts("  anti-deadzone through resolve");
}

// The tuning tradeoff, pinned as data rather than left to a comment: while the
// velocity estimate decays after the user stops, anti-deadzone holds the output
// at or above its floor until the idle deadline forces exact centre. A value
// above the game's real deadzone therefore shows up as low-speed creep for that
// window. This does not assert a "correct" value -- only that the behaviour is
// bounded, ends in exact zero, and is what the tuning rule says it is.
static void test_anti_deadzone_release_tail(void) {
    const uint8_t values[] = {0, 5, 10, 15, 20};
    for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); ++v) {
        mouse_sim_t sim;
        ns2_kbm_output_t out;
        sim_init(&sim);
        sim.config.mouse.anti_deadzone = values[v];
        sim_run(&sim, 20, 0, 8u, 200u, 0u, NULL);

        int32_t floor_units = KBM_STICK_FULL_SCALE * values[v] / 100;
        int32_t last = 0;
        unsigned centred_at = 0;
        // Stepped one millisecond at a time so the deflection can be sampled at
        // every publish, with the service tick on its production 3 ms cadence.
        for (unsigned ms = 1; ms <= NS2_KBM_MOUSE_IDLE_MS + SIM_TICK_MS; ++ms) {
            sim.now_ms++;
            if ((sim.now_ms % SIM_TICK_MS) == 0u)
                ns2_kbm_state_service(&sim.state, &sim.config.mouse, sim.now_ms);
            ns2_kbm_resolve(&sim.state, &sim.config,
                            NS2_KBM_MODE_KEYBOARD_MOUSE, false, &out);
            int32_t deflection = (int32_t)out.right_x - SWITCH_STICK_MID;
            if (deflection == 0) {
                centred_at = ms;
                break;
            }
            // Never below the floor while the estimate is still alive, and
            // never rising on its own.
            assert(deflection >= floor_units - 2);
            assert(last == 0 || deflection <= last);
            last = deflection;
        }
        // Always reaches EXACT centre, on the same deadline as anti-deadzone 0.
        assert(centred_at != 0);
        assert(centred_at <= NS2_KBM_MOUSE_IDLE_MS + SIM_TICK_MS);
    }
    puts("  anti-deadzone release tail");
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
    assert(slow > 0);
    ns2_kbm_state_init(&state);
    config.mouse.sensitivity_x = (uint16_t)(NS2_KBM_MOUSE_SENS_DEFAULT * 2u);
    ns2_kbm_state_mouse_report(&state, 0, 20, 0, 0, &config.mouse, 0);
    assert(state.stick_x == slow * 2);
    config.mouse.sensitivity_x = NS2_KBM_MOUSE_SENS_DEFAULT;

    // The velocity reference interval scales it the same way, in the same
    // direction the old constant-rate `recenter_ms` did: a larger value holds
    // more deflection for the same movement.
    ns2_kbm_state_init(&state);
    config.mouse.recenter_ms =
        (uint16_t)(NS2_KBM_MOUSE_RECENTER_DEFAULT_MS * 2u);
    ns2_kbm_state_mouse_report(&state, 0, 20, 0, 0, &config.mouse, 0);
    assert(state.stick_x == slow * 2);
    config.mouse.recenter_ms = NS2_KBM_MOUSE_RECENTER_DEFAULT_MS;

    // High-DPI bursts clamp instead of accumulating without bound.
    ns2_kbm_state_init(&state);
    for (unsigned i = 0; i < 100; ++i)
        ns2_kbm_state_mouse_report(&state, 0, 30000, 0, 0, &config.mouse, 0);
    assert(state.stick_x == KBM_STICK_FULL_SCALE);
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
    assert(!ns2_kbm_state_mouse_motion_pending(&state));

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
// Role resolution once the transport's self-declaration is taken into account.
//
// The regression boundary for the BLE-keyboard-as-mouse defect, and it has to
// hold in BOTH directions: a genuine keyboard with a pointer must reach COMBO,
// and a gaming mouse with macro keys must still reach MOUSE.
static void test_primary_from_evidence(void) {
    ns2_kbm_peer_caps_t both = caps(true, true);

    // Capability alone is unchanged: still MOUSE. This is the clause that keeps
    // the ASUS ROG KERIS II out of the keyboard role, and nothing here weakens it.
    assert(ns2_kbm_primary_from_evidence(both, false, false) == MS);

    // A Classic combo peripheral declares itself. Unchanged behaviour.
    assert(ns2_kbm_primary_from_evidence(both, true, false) == CB);

    // A BLE keyboard whose DESCRIPTOR says keyboard. The defect this fixes:
    // BLE has no Class of Device, so before this the peer fell to precedence
    // and lost the keyboard role.
    assert(ns2_kbm_primary_from_evidence(both, false, true) == CB);

    // Strong keyboard evidence is only ever a tie-breaker between two present
    // capabilities. It cannot invent a role the device does not have.
    assert(ns2_kbm_primary_from_evidence(caps(true, false), false, true) == KB);
    assert(ns2_kbm_primary_from_evidence(caps(false, true), false, true) == MS);
    assert(ns2_kbm_primary_from_evidence(caps(false, false), true, true) ==
           NS2_KBM_PRIMARY_NONE);

    // A mouse that declares no keyboard capability at all is untouched by
    // either flag.
    assert(ns2_kbm_primary_from_evidence(caps(false, true), true, true) == MS);
}

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
    test_mouse_sustained_motion_holds_deflection();
    test_mouse_low_speed_has_no_threshold();
    test_mouse_release_returns_to_exact_centre();
    test_mouse_gesture_continuity();
    test_mouse_direction_reversal();
    test_mouse_speed_scaling_and_axes();
    test_mouse_native_path_untouched();
    test_mouse_digital_stick_precedence();
    test_anti_deadzone_zero_and_identity();
    test_anti_deadzone_magnitude_mapping();
    test_anti_deadzone_preserves_direction();
    test_anti_deadzone_radial_precision();
    test_anti_deadzone_bounds_and_overflow();
    test_anti_deadzone_through_resolve();
    test_anti_deadzone_release_tail();
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
    test_primary_from_evidence();
    test_roles_and_admission();
    test_role_reconnect_while_partner_live();
    test_freed_role_is_not_absorbed();
    puts("ns2_kbm tests passed");
    return 0;
}
