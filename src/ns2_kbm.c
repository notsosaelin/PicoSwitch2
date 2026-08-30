// Bluetooth Keyboard / Keyboard + Mouse input model.
//
// Pure logic: no Pico SDK, no BTstack, no bthid, no report.c. Everything this
// file decides is reproducible in a host test, which is why the whole mapping,
// merge, opposing-direction, and mouse-translation contract lives here rather
// than inside the Bluetooth report parsers.
//
// See docs/bluetooth/keyboard-mouse-input.md for the behavioral contract and
// include/ns2_kbm.h for the layering.

#include "ns2_kbm.h"

#include <string.h>

#include "switch_pro.h"  // SWITCH_MASK_*, SWITCH_EXTRA_*, SWITCH_STICK_*

// ---------------------------------------------------------------------------
// HID Usage Page 0x07 identifiers used by the canonical profiles
// ---------------------------------------------------------------------------
// Spelled out rather than computed so the default tables read as the physical
// keys a user sees, and so a later UX editor can quote the same numbers.
#define KEY_A 0x04u
#define KEY_C 0x06u
#define KEY_D 0x07u
#define KEY_E 0x08u
#define KEY_F 0x09u
#define KEY_I 0x0Cu
#define KEY_J 0x0Du
#define KEY_K 0x0Eu
#define KEY_L 0x0Fu
#define KEY_Q 0x14u
#define KEY_R 0x15u
#define KEY_S 0x16u
#define KEY_W 0x1Au
#define KEY_1 0x1Eu
#define KEY_3 0x20u
#define KEY_ENTER 0x28u
#define KEY_ESCAPE 0x29u
#define KEY_BACKSPACE 0x2Au
#define KEY_SPACE 0x2Cu
#define KEY_F12 0x45u
#define KEY_ARROW_RIGHT 0x4Fu
#define KEY_ARROW_LEFT 0x50u
#define KEY_ARROW_DOWN 0x51u
#define KEY_ARROW_UP 0x52u
#define KEY_LEFT_CTRL 0xE0u
#define KEY_LEFT_SHIFT 0xE1u
#define KEY_LEFT_ALT 0xE2u

typedef struct {
    uint8_t kind;
    uint8_t code;
    uint8_t destination;
} kbm_default_binding_t;

// Canonical Keyboard profile.
//
// The keyboard is the entire controller here, so it carries both sticks: WASD
// walks, IJKL aims. Everything else follows the layout in PLAN.md's KB/M brief.
static const kbm_default_binding_t KBM_DEFAULT_KEYBOARD[] = {
    {NS2_KBM_SRC_KEY, KEY_W, NS2_DST_LSTICK_UP},
    {NS2_KBM_SRC_KEY, KEY_S, NS2_DST_LSTICK_DOWN},
    {NS2_KBM_SRC_KEY, KEY_A, NS2_DST_LSTICK_LEFT},
    {NS2_KBM_SRC_KEY, KEY_D, NS2_DST_LSTICK_RIGHT},

    {NS2_KBM_SRC_KEY, KEY_I, NS2_DST_RSTICK_UP},
    {NS2_KBM_SRC_KEY, KEY_K, NS2_DST_RSTICK_DOWN},
    {NS2_KBM_SRC_KEY, KEY_J, NS2_DST_RSTICK_LEFT},
    {NS2_KBM_SRC_KEY, KEY_L, NS2_DST_RSTICK_RIGHT},

    {NS2_KBM_SRC_KEY, KEY_ARROW_UP, NS2_DST_DUP},
    {NS2_KBM_SRC_KEY, KEY_ARROW_DOWN, NS2_DST_DDOWN},
    {NS2_KBM_SRC_KEY, KEY_ARROW_LEFT, NS2_DST_DLEFT},
    {NS2_KBM_SRC_KEY, KEY_ARROW_RIGHT, NS2_DST_DRIGHT},

    {NS2_KBM_SRC_KEY, KEY_SPACE, NS2_DST_B},
    {NS2_KBM_SRC_KEY, KEY_F, NS2_DST_A},
    {NS2_KBM_SRC_KEY, KEY_E, NS2_DST_X},
    {NS2_KBM_SRC_KEY, KEY_LEFT_SHIFT, NS2_DST_Y},

    {NS2_KBM_SRC_KEY, KEY_Q, NS2_DST_L},
    {NS2_KBM_SRC_KEY, KEY_R, NS2_DST_R},
    {NS2_KBM_SRC_KEY, KEY_1, NS2_DST_ZL},
    {NS2_KBM_SRC_KEY, KEY_3, NS2_DST_ZR},

    {NS2_KBM_SRC_KEY, KEY_LEFT_CTRL, NS2_DST_L3},
    {NS2_KBM_SRC_KEY, KEY_LEFT_ALT, NS2_DST_R3},

    {NS2_KBM_SRC_KEY, KEY_ENTER, NS2_DST_PLUS},
    {NS2_KBM_SRC_KEY, KEY_BACKSPACE, NS2_DST_MINUS},
    {NS2_KBM_SRC_KEY, KEY_ESCAPE, NS2_DST_HOME},
    {NS2_KBM_SRC_KEY, KEY_F12, NS2_DST_CAPTURE},
};

// Canonical Keyboard + Mouse profile.
//
// Deliberately NOT the keyboard profile plus mouse buttons: the mouse owns the
// right stick / pointer here, so IJKL are unassigned and R3 moves to the middle
// mouse button. This is what makes the two profiles genuinely independent
// rather than one being a superset of the other, and it is why a disconnected
// mouse must not silently fall back to the Keyboard profile.
static const kbm_default_binding_t KBM_DEFAULT_KEYBOARD_MOUSE[] = {
    {NS2_KBM_SRC_KEY, KEY_W, NS2_DST_LSTICK_UP},
    {NS2_KBM_SRC_KEY, KEY_S, NS2_DST_LSTICK_DOWN},
    {NS2_KBM_SRC_KEY, KEY_A, NS2_DST_LSTICK_LEFT},
    {NS2_KBM_SRC_KEY, KEY_D, NS2_DST_LSTICK_RIGHT},

    {NS2_KBM_SRC_KEY, KEY_ARROW_UP, NS2_DST_DUP},
    {NS2_KBM_SRC_KEY, KEY_ARROW_DOWN, NS2_DST_DDOWN},
    {NS2_KBM_SRC_KEY, KEY_ARROW_LEFT, NS2_DST_DLEFT},
    {NS2_KBM_SRC_KEY, KEY_ARROW_RIGHT, NS2_DST_DRIGHT},

    {NS2_KBM_SRC_KEY, KEY_SPACE, NS2_DST_B},
    {NS2_KBM_SRC_KEY, KEY_F, NS2_DST_A},
    {NS2_KBM_SRC_KEY, KEY_E, NS2_DST_X},
    {NS2_KBM_SRC_KEY, KEY_LEFT_SHIFT, NS2_DST_Y},

    {NS2_KBM_SRC_KEY, KEY_Q, NS2_DST_L},
    {NS2_KBM_SRC_KEY, KEY_R, NS2_DST_R},
    // 1/3 keep the keyboard route to ZL/ZR while the mouse triggers also reach
    // them. Two sources naming one destination is legal and is exactly the case
    // the recompute-from-held-set design makes safe.
    {NS2_KBM_SRC_KEY, KEY_1, NS2_DST_ZL},
    {NS2_KBM_SRC_KEY, KEY_3, NS2_DST_ZR},

    {NS2_KBM_SRC_KEY, KEY_LEFT_CTRL, NS2_DST_L3},
    {NS2_KBM_SRC_KEY, KEY_C, NS2_DST_C},

    {NS2_KBM_SRC_KEY, KEY_ENTER, NS2_DST_PLUS},
    {NS2_KBM_SRC_KEY, KEY_BACKSPACE, NS2_DST_MINUS},
    {NS2_KBM_SRC_KEY, KEY_ESCAPE, NS2_DST_HOME},
    {NS2_KBM_SRC_KEY, KEY_F12, NS2_DST_CAPTURE},

    // The standard five-button mouse contract: left, right, middle, back,
    // forward. All five are ordinary mouse inputs and all five are bound by
    // default -- an unbound Back/Forward would be a control that works in
    // Controller mode and silently does nothing here.
    //
    // 1/2 are swapped relative to the Controller-mode base map on purpose:
    // left click is the fire trigger (ZR) in a keyboard+mouse context. Back and
    // Forward keep the destinations the existing Controller-mode mapping gives
    // them (JP_BUTTON_B3 -> Y, JP_BUTTON_B1 -> B) so the same physical button
    // does the same thing in both modes.
    {NS2_KBM_SRC_MOUSE, 1u, NS2_DST_ZR},
    {NS2_KBM_SRC_MOUSE, 2u, NS2_DST_ZL},
    {NS2_KBM_SRC_MOUSE, 3u, NS2_DST_R3},
    {NS2_KBM_SRC_MOUSE, 4u, NS2_DST_Y},
    {NS2_KBM_SRC_MOUSE, 5u, NS2_DST_B},
};

static const kbm_default_binding_t *default_table(ns2_kbm_profile_t profile,
                                                  uint16_t *count) {
    if (profile == NS2_KBM_PROFILE_KEYBOARD_MOUSE) {
        *count = (uint16_t)(sizeof(KBM_DEFAULT_KEYBOARD_MOUSE) /
                            sizeof(KBM_DEFAULT_KEYBOARD_MOUSE[0]));
        return KBM_DEFAULT_KEYBOARD_MOUSE;
    }
    *count = (uint16_t)(sizeof(KBM_DEFAULT_KEYBOARD) /
                        sizeof(KBM_DEFAULT_KEYBOARD[0]));
    return KBM_DEFAULT_KEYBOARD;
}

// ---------------------------------------------------------------------------
// Identifier validation
// ---------------------------------------------------------------------------

bool ns2_kbm_source_valid(ns2_kbm_source_t source) {
    switch (source.kind) {
        case NS2_KBM_SRC_KEY:
            // Usage 0 is "no event" and 1..3 are the rollover/POST-fail error
            // codes; none of them is a physical key and none may be bound.
            return source.code >= 0x04u && source.code <= NS2_KBM_KEY_USAGE_MAX;
        case NS2_KBM_SRC_MOUSE:
            return source.code >= 1u && source.code <= NS2_KBM_MOUSE_BUTTONS;
        default:
            return false;
    }
}

bool ns2_kbm_destination_valid(uint8_t destination) {
    return destination < NS2_DST_COUNT;
}

// ---------------------------------------------------------------------------
// Destination -> normalized wire bits
// ---------------------------------------------------------------------------
// Single authority for what a NS2_DST_* means in the Pro Controller button
// layout. ns2_seam.c's locked physical map calls the same function so the two
// mapping systems can never disagree about what "ZR" is.
void ns2_kbm_apply_destination(uint8_t destination, uint8_t buttons[3],
                               uint8_t *extra) {
    if (!buttons || !extra) return;
    switch (destination) {
        case NS2_DST_B:       buttons[0] |= SWITCH_MASK_B; break;
        case NS2_DST_A:       buttons[0] |= SWITCH_MASK_A; break;
        case NS2_DST_Y:       buttons[0] |= SWITCH_MASK_Y; break;
        case NS2_DST_X:       buttons[0] |= SWITCH_MASK_X; break;
        case NS2_DST_L:       buttons[2] |= SWITCH_MASK_L; break;
        case NS2_DST_R:       buttons[0] |= SWITCH_MASK_R; break;
        case NS2_DST_ZL:      buttons[2] |= SWITCH_MASK_ZL; break;
        case NS2_DST_ZR:      buttons[0] |= SWITCH_MASK_ZR; break;
        case NS2_DST_L3:      buttons[1] |= SWITCH_MASK_L3; break;
        case NS2_DST_R3:      buttons[1] |= SWITCH_MASK_R3; break;
        case NS2_DST_MINUS:   buttons[1] |= SWITCH_MASK_MINUS; break;
        case NS2_DST_PLUS:    buttons[1] |= SWITCH_MASK_PLUS; break;
        case NS2_DST_HOME:    buttons[1] |= SWITCH_MASK_HOME; break;
        case NS2_DST_CAPTURE: buttons[1] |= SWITCH_MASK_CAPTURE; break;
        case NS2_DST_DUP:     buttons[2] |= SWITCH_MASK_DPAD_UP; break;
        case NS2_DST_DDOWN:   buttons[2] |= SWITCH_MASK_DPAD_DOWN; break;
        case NS2_DST_DLEFT:   buttons[2] |= SWITCH_MASK_DPAD_LEFT; break;
        case NS2_DST_DRIGHT:  buttons[2] |= SWITCH_MASK_DPAD_RIGHT; break;
        case NS2_DST_GL:      *extra |= SWITCH_EXTRA_GL; break;
        case NS2_DST_GR:      *extra |= SWITCH_EXTRA_GR; break;
        case NS2_DST_C:       *extra |= SWITCH_EXTRA_C; break;
        default: break;  // NS2_DST_NONE and the digital stick directions
    }
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static void mouse_defaults(ns2_kbm_mouse_config_t *mouse) {
    mouse->sensitivity_x = NS2_KBM_MOUSE_SENS_DEFAULT;
    mouse->sensitivity_y = NS2_KBM_MOUSE_SENS_DEFAULT;
    mouse->recenter_ms = NS2_KBM_MOUSE_RECENTER_DEFAULT_MS;
    mouse->invert_x = 0;
    mouse->invert_y = 0;
    mouse->anti_deadzone = (uint8_t)NS2_KBM_MOUSE_ADZ_DEFAULT;
    mouse->reserved = 0;
}

void ns2_kbm_config_defaults(ns2_kbm_config_t *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    // Auto is the default: pairing an ordinary HID device must work without
    // the user first predicting and selecting a mode.
    config->mode = (uint8_t)NS2_KBM_MODE_AUTO;
    mouse_defaults(&config->mouse);
}

void ns2_kbm_config_reset_profile(ns2_kbm_config_t *config,
                                  ns2_kbm_profile_t profile) {
    if (!config || profile >= NS2_KBM_PROFILE_COUNT) return;
    memset(&config->profiles[profile], 0, sizeof(config->profiles[profile]));
}

static bool sanitize_profile(ns2_kbm_profile_overrides_t *profile) {
    bool clean = true;
    if (profile->count > NS2_KBM_MAX_OVERRIDES) {
        // A count larger than the table cannot be trusted to describe anything;
        // reinterpreting the surviving bytes would be inventing bindings.
        memset(profile, 0, sizeof(*profile));
        return false;
    }
    uint8_t kept = 0;
    for (uint8_t i = 0; i < profile->count; ++i) {
        ns2_kbm_override_t entry = profile->entries[i];
        entry.reserved = 0;
        if (!ns2_kbm_source_valid(entry.source) ||
            !ns2_kbm_destination_valid(entry.destination)) {
            clean = false;
            continue;  // drop rather than remap to an arbitrary destination
        }
        bool duplicate = false;
        for (uint8_t j = 0; j < kept; ++j) {
            if (ns2_kbm_source_equal(profile->entries[j].source, entry.source)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            clean = false;
            continue;
        }
        profile->entries[kept++] = entry;
    }
    if (kept != profile->count) clean = false;
    profile->count = kept;
    for (uint8_t i = kept; i < NS2_KBM_MAX_OVERRIDES; ++i)
        memset(&profile->entries[i], 0, sizeof(profile->entries[i]));
    profile->reserved[0] = profile->reserved[1] = profile->reserved[2] = 0;
    return clean;
}

static bool clamp_u16(uint16_t *value, uint16_t lo, uint16_t hi,
                      uint16_t fallback) {
    if (*value >= lo && *value <= hi) return true;
    *value = fallback;
    return false;
}

bool ns2_kbm_config_sanitize(ns2_kbm_config_t *config) {
    if (!config) return false;
    bool clean = true;

    if (config->mode >= (uint8_t)NS2_KBM_MODE_COUNT) {
        // Fall back to the canonical default, which is inference -- not to a
        // mode that would silently disable KB/M input.
        config->mode = (uint8_t)NS2_KBM_MODE_AUTO;
        clean = false;
    }
    config->reserved[0] = config->reserved[1] = config->reserved[2] = 0;

    for (unsigned p = 0; p < NS2_KBM_PROFILE_COUNT; ++p) {
        if (!sanitize_profile(&config->profiles[p])) clean = false;
    }

    if (!clamp_u16(&config->mouse.sensitivity_x, NS2_KBM_MOUSE_SENS_MIN,
                   NS2_KBM_MOUSE_SENS_MAX, NS2_KBM_MOUSE_SENS_DEFAULT))
        clean = false;
    if (!clamp_u16(&config->mouse.sensitivity_y, NS2_KBM_MOUSE_SENS_MIN,
                   NS2_KBM_MOUSE_SENS_MAX, NS2_KBM_MOUSE_SENS_DEFAULT))
        clean = false;
    if (!clamp_u16(&config->mouse.recenter_ms, NS2_KBM_MOUSE_RECENTER_MIN_MS,
                   NS2_KBM_MOUSE_RECENTER_MAX_MS,
                   NS2_KBM_MOUSE_RECENTER_DEFAULT_MS))
        clean = false;
    if (config->mouse.invert_x > 1u) { config->mouse.invert_x = 0; clean = false; }
    if (config->mouse.invert_y > 1u) { config->mouse.invert_y = 0; clean = false; }
    // Fails closed to OFF, not to a clamped value: an unusable anti-deadzone
    // must restore the validated linear response rather than apply some
    // arbitrary compensation the user never chose.
    if (config->mouse.anti_deadzone > NS2_KBM_MOUSE_ADZ_MAX) {
        config->mouse.anti_deadzone = (uint8_t)NS2_KBM_MOUSE_ADZ_DEFAULT;
        clean = false;
    }
    config->mouse.reserved = 0;

    return clean;
}

uint8_t ns2_kbm_default_binding(ns2_kbm_profile_t profile,
                                ns2_kbm_source_t source) {
    if (profile >= NS2_KBM_PROFILE_COUNT) return NS2_DST_NONE;
    uint16_t count = 0;
    const kbm_default_binding_t *table = default_table(profile, &count);
    for (uint16_t i = 0; i < count; ++i) {
        if (table[i].kind == source.kind && table[i].code == source.code)
            return table[i].destination;
    }
    return NS2_DST_NONE;
}

uint8_t ns2_kbm_binding(const ns2_kbm_config_t *config,
                        ns2_kbm_profile_t profile, ns2_kbm_source_t source) {
    if (!config || profile >= NS2_KBM_PROFILE_COUNT) return NS2_DST_NONE;
    const ns2_kbm_profile_overrides_t *overrides = &config->profiles[profile];
    uint8_t count = overrides->count <= NS2_KBM_MAX_OVERRIDES
                        ? overrides->count : 0u;
    for (uint8_t i = 0; i < count; ++i) {
        if (ns2_kbm_source_equal(overrides->entries[i].source, source))
            return overrides->entries[i].destination;
    }
    return ns2_kbm_default_binding(profile, source);
}

bool ns2_kbm_set_binding(ns2_kbm_config_t *config, ns2_kbm_profile_t profile,
                         ns2_kbm_source_t source, uint8_t destination) {
    if (!config || profile >= NS2_KBM_PROFILE_COUNT) return false;
    if (!ns2_kbm_source_valid(source)) return false;
    if (!ns2_kbm_destination_valid(destination)) return false;

    ns2_kbm_profile_overrides_t *overrides = &config->profiles[profile];
    if (overrides->count > NS2_KBM_MAX_OVERRIDES) return false;

    // Requesting exactly the canonical default is stored as "no override" so a
    // user who reverts a binding by hand reaches the same state as a reset.
    if (destination == ns2_kbm_default_binding(profile, source))
        return ns2_kbm_clear_binding(config, profile, source);

    for (uint8_t i = 0; i < overrides->count; ++i) {
        if (ns2_kbm_source_equal(overrides->entries[i].source, source)) {
            overrides->entries[i].destination = destination;
            return true;
        }
    }
    if (overrides->count >= NS2_KBM_MAX_OVERRIDES) return false;
    overrides->entries[overrides->count].source = source;
    overrides->entries[overrides->count].destination = destination;
    overrides->entries[overrides->count].reserved = 0;
    overrides->count++;
    return true;
}

bool ns2_kbm_clear_binding(ns2_kbm_config_t *config, ns2_kbm_profile_t profile,
                           ns2_kbm_source_t source) {
    if (!config || profile >= NS2_KBM_PROFILE_COUNT) return false;
    if (!ns2_kbm_source_valid(source)) return false;
    ns2_kbm_profile_overrides_t *overrides = &config->profiles[profile];
    if (overrides->count > NS2_KBM_MAX_OVERRIDES) return false;
    for (uint8_t i = 0; i < overrides->count; ++i) {
        if (!ns2_kbm_source_equal(overrides->entries[i].source, source))
            continue;
        for (uint8_t j = (uint8_t)(i + 1u); j < overrides->count; ++j)
            overrides->entries[j - 1u] = overrides->entries[j];
        overrides->count--;
        memset(&overrides->entries[overrides->count], 0,
               sizeof(overrides->entries[overrides->count]));
        return true;
    }
    return true;  // already at the canonical default
}

uint16_t ns2_kbm_effective_bindings(const ns2_kbm_config_t *config,
                                    ns2_kbm_profile_t profile,
                                    ns2_kbm_effective_t *out,
                                    uint16_t capacity) {
    if (!config || !out || profile >= NS2_KBM_PROFILE_COUNT) return 0;
    uint16_t written = 0;
    uint16_t default_count = 0;
    const kbm_default_binding_t *table = default_table(profile, &default_count);
    const ns2_kbm_profile_overrides_t *overrides = &config->profiles[profile];
    uint8_t override_count = overrides->count <= NS2_KBM_MAX_OVERRIDES
                                 ? overrides->count : 0u;

    for (uint16_t i = 0; i < default_count && written < capacity; ++i) {
        ns2_kbm_source_t source = {table[i].kind, table[i].code};
        uint8_t destination = ns2_kbm_binding(config, profile, source);
        if (destination == NS2_DST_NONE) continue;  // explicitly unassigned
        out[written].source = source;
        out[written].destination = destination;
        out[written].overridden =
            destination != table[i].destination ? 1u : 0u;
        written++;
    }
    // Overrides that name a source with no canonical binding are new bindings
    // and must appear too.
    for (uint8_t i = 0; i < override_count && written < capacity; ++i) {
        if (overrides->entries[i].destination == NS2_DST_NONE) continue;
        if (ns2_kbm_default_binding(profile, overrides->entries[i].source) !=
            NS2_DST_NONE)
            continue;  // already emitted from the default table
        out[written].source = overrides->entries[i].source;
        out[written].destination = overrides->entries[i].destination;
        out[written].overridden = 1u;
        written++;
    }
    return written;
}

// ---------------------------------------------------------------------------
// Live state
// ---------------------------------------------------------------------------

void ns2_kbm_state_init(ns2_kbm_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

void ns2_kbm_state_clear_keyboard(ns2_kbm_state_t *state) {
    if (!state) return;
    memset(state->keys, 0, sizeof(state->keys));
    state->keyboard_present = 0;
}

void ns2_kbm_state_clear_mouse(ns2_kbm_state_t *state) {
    if (!state) return;
    state->mouse_buttons = 0;
    state->mouse_present = 0;
    state->mouse_delta_x = 0;
    state->mouse_delta_y = 0;
    state->mouse_delta_wheel = 0;
    state->motion_x = 0;
    state->motion_y = 0;
    state->stick_x = 0;
    state->stick_y = 0;
    state->motion_clock_valid = 0;
    state->motion_clock_ms = 0;
    state->last_motion_ms = 0;
}

void ns2_kbm_state_set_keys(ns2_kbm_state_t *state, const uint8_t *bitmap) {
    if (!state || !bitmap) return;
    memcpy(state->keys, bitmap, NS2_KBM_KEY_BITMAP_BYTES);
    state->keyboard_present = 1;
}

bool ns2_kbm_state_key_held(const ns2_kbm_state_t *state, uint8_t usage) {
    if (!state) return false;
    return (state->keys[usage >> 3] & (uint8_t)(1u << (usage & 7u))) != 0u;
}

static int32_t clamp_i32(int32_t value, int32_t lo, int32_t hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int16_t clamp_i16(int32_t value) {
    return (int16_t)clamp_i32(value, -32768, 32767);
}

static int8_t clamp_i8(int32_t value) {
    return (int8_t)clamp_i32(value, -127, 127);
}

// Largest deflection the mouse translator may produce on one axis. The
// 12-bit centre is not the midpoint of the range, so the positive side clamps
// one count short of the limit; ns2_seam.c's ns2_to12() handles the same
// asymmetry for analog sources.
#define KBM_STICK_LIMIT 2048

// ---------------------------------------------------------------------------
// Mouse-to-stick translation
// ---------------------------------------------------------------------------
// A relative mouse is a VELOCITY device and an analog stick is a POSITION
// device, so the translator's whole job is turning "how fast is the mouse
// moving right now" into "how far is the stick being held".
//
//   deflection = mouse_counts_per_ms * (sensitivity / 256) * recenter_ms
//
// Continuous movement therefore holds a continuous deflection: the level is a
// function of current SPEED, not of accumulated distance, so it does not have
// to be re-earned on every report.
//
// DISPROVEN PREDECESSOR -- do not reintroduce. This was originally a leaky
// position accumulator: each report added `delta * sensitivity` to the
// deflection and a constant-rate friction subtracted KBM_STICK_LIMIT /
// recenter_ms per millisecond. That is `stick += delta; stick -= friction`,
// which makes deflection depend on whether the mouse can OUTRUN the friction,
// and so it silently imposed a minimum speed. Measured on the shipped defaults
// (sensitivity 512 = 2 units/count, recenter 120 ms = 17.07 units/ms of decay):
// break-even was 8.53 counts/ms (~8533 counts/s). Below it the stick sat at
// exact centre 50-80% of the time and never exceeded ~2% deflection -- the
// console saw a train of pulses, not a held stick (measured trace at 20 counts
// per 8 ms: 40 40 40 0 0 0 0 0 40 23 23 23 0 0 0 0 ...). Above it the stick
// slammed to ~full scale. That cliff is what made stick-camera games feel like
// "move, brief turn, stop, move, brief turn, stop". Slowing the friction down
// only trades the pulsing for post-motion coasting; the model itself was wrong.
//
// `motion_*` is that velocity estimate, held as a first-order low pass over
// SCALED COUNTS: each report adds `delta * sensitivity` (Q8.8, undivided -- the
// 1/256 is applied once at the output, so sub-unit movement at low sensitivity
// accumulates instead of being truncated away report by report), and the value
// leaks with time constant NS2_KBM_MOUSE_VELOCITY_MS. Under sustained movement
// at rate R counts/ms it settles at R * sensitivity * NS2_KBM_MOUSE_VELOCITY_MS,
// which is why the output scaling below divides that constant back out.

// Largest velocity estimate worth holding: the one that already maps to full
// deflection. Clamping HERE and not only at the output is what prevents wind-up
// -- without it a fast flick would leave an estimate far above full scale that
// kept the stick pinned after the mouse stopped, which is exactly the
// post-motion camera drift this translator must never produce.
static int32_t motion_limit(uint32_t recenter_ms) {
    // Rounded UP, so that a clamped estimate maps to exactly full scale rather
    // than one unit short of it after the output division truncates.
    uint32_t scale = (uint32_t)KBM_STICK_LIMIT * NS2_KBM_MOUSE_VELOCITY_MS * 256u;
    return (int32_t)((scale + recenter_ms - 1u) / recenter_ms);
}

// Deflection for a velocity estimate. Integer division truncates toward zero,
// so the two directions are treated identically and neither gains a bias.
static int32_t motion_to_stick(int32_t motion, uint32_t recenter_ms) {
    // Re-clamp against the CURRENT reference interval before scaling. The
    // estimate is already bounded when it is stored, but `recenter_ms` is live
    // configuration: raising it mid-gesture would otherwise scale an estimate
    // bounded for the old, smaller interval and overflow the product.
    int32_t limit = motion_limit(recenter_ms);
    motion = clamp_i32(motion, -limit, limit);
    // So the product cannot exceed KBM_STICK_LIMIT *
    // NS2_KBM_MOUSE_VELOCITY_MS * 256 (~21e6) plus one interval's rounding,
    // whatever recenter_ms is -- well inside int32.
    int32_t stick = (motion * (int32_t)recenter_ms) /
                    (int32_t)(NS2_KBM_MOUSE_VELOCITY_MS * 256u);
    return clamp_i32(stick, -KBM_STICK_LIMIT, KBM_STICK_LIMIT);
}

// One first-order decay step over `elapsed_ms`, as tau / (tau + elapsed).
//
// That is the exponential to first order at the 3 ms service tick this actually
// runs on (0.930 vs 0.928 at tau = 40 ms) while staying monotone and strictly
// positive for any gap length, so an unusually long interval between two
// reports of one continuous gesture merely decays the level further instead of
// dropping it to centre. Integer truncation brings small values to exactly zero
// on its own; exact centre is guaranteed by the inactivity deadline regardless.
static int32_t motion_decay(int32_t motion, uint32_t elapsed_ms) {
    if (motion == 0) return 0;
    int32_t magnitude = motion < 0 ? -motion : motion;
    magnitude = (int32_t)(((int64_t)magnitude * NS2_KBM_MOUSE_VELOCITY_MS) /
                          (int64_t)(NS2_KBM_MOUSE_VELOCITY_MS + elapsed_ms));
    return motion < 0 ? -magnitude : magnitude;
}

// ---------------------------------------------------------------------------
// Radial anti-deadzone (output response, NOT part of the temporal model)
// ---------------------------------------------------------------------------
// Exact floor(sqrt(value)). Integer because everything else on this path is,
// and because the result feeds a ratio that must be reproducible on the host
// test as well as on the MCU. Runs at most once per publish (~350/s).
static uint32_t isqrt_u32(uint32_t value) {
    uint32_t result = 0;
    uint32_t bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit != 0u) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

// Fixed-point resolution of the radial magnitude, in sixteenths of a stick unit.
//
// The magnitude is the DIVISOR in the rescale below, so truncating it to a whole
// stick unit is not a rounding detail -- it is a systematic overshoot of the
// configured floor at every angle whose true magnitude is not an integer, and it
// is worst exactly where the compensation matters most. Measured against the
// whole-unit version: |(1,1)| is 1.414 but floored to 1, inflating the ratio by
// 41%, so a configured 15% radial floor came out as 21.2% at 45 degrees (and 50%
// came out as 70.7%) while a cardinal vector got its 15% exactly. Direction was
// preserved throughout -- the bug was purely magnitude.
//
// Squaring in a sixteenths domain removes it: the largest possible input is
// 2 * 2048^2 * 16^2 = 2^31, which is the most precision available while the
// intermediate still fits a uint32, so this stays one 32-bit integer sqrt.
#define KBM_ADZ_SCALE 16

void ns2_kbm_mouse_anti_deadzone(int32_t *x, int32_t *y, uint8_t percent) {
    if (!x || !y) return;
    // Exact zero is the whole safety property: no configuration may turn "the
    // user is not moving the mouse" into stick deflection.
    if (percent == 0u || (*x == 0 && *y == 0)) return;
    if (percent > NS2_KBM_MOUSE_ADZ_MAX) percent = (uint8_t)NS2_KBM_MOUSE_ADZ_MAX;

    // Bounded by construction: each axis is clamped to KBM_STICK_LIMIT, so the
    // sum of squares cannot exceed 2 * 2048^2 = 2^23, and the scaled square
    // cannot exceed 2^31.
    uint32_t squared = (uint32_t)(*x * *x) + (uint32_t)(*y * *y);
    uint32_t magnitude =
        isqrt_u32(squared * (uint32_t)(KBM_ADZ_SCALE * KBM_ADZ_SCALE));
    if (magnitude == 0u) return;
    // A vector already at or past full scale (the diagonal corners reach
    // 1.41x) is left alone. Anti-deadzone may only ever RAISE a deflection;
    // rescaling here would quietly cut the top of the range instead.
    if (magnitude >= (uint32_t)KBM_STICK_LIMIT * KBM_ADZ_SCALE) return;

    int32_t floor_units =
        (int32_t)(((uint32_t)KBM_STICK_LIMIT * percent) / 100u);
    // magnitude is in sixteenths, so the linear term divides by the scaled
    // limit. Bounded by 32768 * 2048 = 2^26.
    int32_t mapped =
        floor_units +
        (int32_t)((magnitude * (uint32_t)(KBM_STICK_LIMIT - floor_units)) /
                  ((uint32_t)KBM_STICK_LIMIT * KBM_ADZ_SCALE));
    // |axis| < 2048 and mapped <= 2048, so the product is under 2^22 and the
    // scaled numerator under 2^26; no axis can leave the clamped range because
    // mapped never exceeds KBM_STICK_LIMIT.
    *x = (*x * mapped * KBM_ADZ_SCALE) / (int32_t)magnitude;
    *y = (*y * mapped * KBM_ADZ_SCALE) / (int32_t)magnitude;
}

static uint16_t mouse_recenter_ms(const ns2_kbm_mouse_config_t *mouse) {
    if (!mouse || mouse->recenter_ms < NS2_KBM_MOUSE_RECENTER_MIN_MS ||
        mouse->recenter_ms > NS2_KBM_MOUSE_RECENTER_MAX_MS)
        return NS2_KBM_MOUSE_RECENTER_DEFAULT_MS;
    return mouse->recenter_ms;
}

static uint16_t mouse_sensitivity(uint16_t value) {
    if (value < NS2_KBM_MOUSE_SENS_MIN || value > NS2_KBM_MOUSE_SENS_MAX)
        return NS2_KBM_MOUSE_SENS_DEFAULT;
    return value;
}

bool ns2_kbm_state_mouse_motion_pending(const ns2_kbm_state_t *state) {
    if (!state) return false;
    return state->motion_x != 0 || state->motion_y != 0 ||
           state->stick_x != 0 || state->stick_y != 0;
}

void ns2_kbm_state_service(ns2_kbm_state_t *state,
                           const ns2_kbm_mouse_config_t *mouse,
                           uint32_t now_ms) {
    if (!state) return;
    if (!state->motion_clock_valid) {
        state->motion_clock_valid = 1;
        state->motion_clock_ms = now_ms;
        state->last_motion_ms = now_ms;
        return;
    }
    uint32_t elapsed = now_ms - state->motion_clock_ms;  // wrap-safe
    if (elapsed == 0u) return;
    state->motion_clock_ms = now_ms;

    // Gesture over. The deadline is deliberately separate from the decay: the
    // decay shapes how the camera slows down, this guarantees it actually stops
    // at EXACT centre rather than asymptotically near it.
    if ((uint32_t)(now_ms - state->last_motion_ms) >= NS2_KBM_MOUSE_IDLE_MS) {
        state->motion_x = 0;
        state->motion_y = 0;
        state->stick_x = 0;
        state->stick_y = 0;
        return;
    }
    if (state->motion_x == 0 && state->motion_y == 0) return;

    uint32_t recenter = mouse_recenter_ms(mouse);
    int32_t limit = motion_limit(recenter);
    // Clamping on the way through absorbs a live reference-interval change
    // instead of carrying an estimate bounded for the previous one.
    state->motion_x =
        clamp_i32(motion_decay(state->motion_x, elapsed), -limit, limit);
    state->motion_y =
        clamp_i32(motion_decay(state->motion_y, elapsed), -limit, limit);
    state->stick_x = motion_to_stick(state->motion_x, recenter);
    state->stick_y = motion_to_stick(state->motion_y, recenter);
}

void ns2_kbm_state_mouse_report(ns2_kbm_state_t *state, uint16_t buttons,
                                int16_t delta_x, int16_t delta_y,
                                int8_t delta_wheel,
                                const ns2_kbm_mouse_config_t *mouse,
                                uint32_t now_ms) {
    if (!state) return;
    // Decay over the interval that just elapsed BEFORE folding in this report,
    // so the estimate is a function of elapsed time and never of report rate: a
    // 1000 Hz mouse and a 125 Hz mouse moving at the same speed settle on the
    // same deflection.
    ns2_kbm_state_service(state, mouse, now_ms);
    state->mouse_present = 1;
    state->mouse_buttons =
        (uint16_t)(buttons & ((1u << NS2_KBM_MOUSE_BUTTONS) - 1u));
    state->mouse_delta_x = clamp_i16((int32_t)state->mouse_delta_x + delta_x);
    state->mouse_delta_y = clamp_i16((int32_t)state->mouse_delta_y + delta_y);
    state->mouse_delta_wheel =
        clamp_i8((int32_t)state->mouse_delta_wheel + delta_wheel);

    // Scaled counts, NOT stick units: the Q8.8 sensitivity is applied whole and
    // divided out at the output, so a slow mouse at low sensitivity contributes
    // a fraction of a stick unit per report instead of truncating to nothing.
    int32_t step_x = (int32_t)delta_x * mouse_sensitivity(
                         mouse ? mouse->sensitivity_x
                               : NS2_KBM_MOUSE_SENS_DEFAULT);
    int32_t step_y = (int32_t)delta_y * mouse_sensitivity(
                         mouse ? mouse->sensitivity_y
                               : NS2_KBM_MOUSE_SENS_DEFAULT);
    if (mouse && mouse->invert_x) step_x = -step_x;
    if (mouse && mouse->invert_y) step_y = -step_y;

    uint32_t recenter = mouse_recenter_ms(mouse);
    int32_t limit = motion_limit(recenter);
    state->motion_x = clamp_i32(state->motion_x + step_x, -limit, limit);
    state->motion_y = clamp_i32(state->motion_y + step_y, -limit, limit);
    state->stick_x = motion_to_stick(state->motion_x, recenter);
    state->stick_y = motion_to_stick(state->motion_y, recenter);

    // Only real movement extends the gesture. A button-only or empty report
    // must not keep a stale deflection alive, and equally must not end a
    // gesture that is merely between reports -- which is why the inactivity
    // deadline is a time bound and not "this report had no movement".
    if (delta_x != 0 || delta_y != 0) state->last_motion_ms = now_ms;
}

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------

ns2_kbm_profile_t ns2_kbm_mode_profile(ns2_kbm_mode_t mode) {
    // Callers pass an EFFECTIVE mode; AUTO has no profile of its own and is
    // mapped to the keyboard profile only so an index can never go out of range.
    return mode == NS2_KBM_MODE_KEYBOARD_MOUSE ? NS2_KBM_PROFILE_KEYBOARD_MOUSE
                                               : NS2_KBM_PROFILE_KEYBOARD;
}

// Opposing digital directions neutralize. This is the simplest rule that is
// independent of report order, cannot latch, and produces a state a physical
// controller could actually be in: no analog stick reaches full left and full
// right at once, and no D-pad presses two opposite edges.
static uint16_t resolve_axis(bool negative, bool positive) {
    if (negative == positive) return SWITCH_STICK_MID;
    return positive ? SWITCH_STICK_MAX : SWITCH_STICK_MIN;
}

void ns2_kbm_resolve(ns2_kbm_state_t *state, const ns2_kbm_config_t *config,
                     ns2_kbm_mode_t mode, bool native_mouse,
                     ns2_kbm_output_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->left_x = SWITCH_STICK_MID;
    out->left_y = SWITCH_STICK_MID;
    out->right_x = SWITCH_STICK_MID;
    out->right_y = SWITCH_STICK_MID;
    // AUTO is not a runtime mode: the caller resolves it to an effective mode
    // first. Treating it as KB/M here would publish input the composition has
    // not actually justified.
    if (!state || !config || mode == NS2_KBM_MODE_CONTROLLER ||
        mode == NS2_KBM_MODE_AUTO || mode >= NS2_KBM_MODE_COUNT)
        return;

    ns2_kbm_profile_t profile = ns2_kbm_mode_profile(mode);
    bool held[NS2_DST_COUNT];
    memset(held, 0, sizeof(held));

    // Keyboard-owned contribution. Walking the held bitmap (not every usage)
    // keeps this proportional to the number of keys actually down.
    if (state->keyboard_present) {
        for (unsigned byte = 0; byte < NS2_KBM_KEY_BITMAP_BYTES; ++byte) {
            uint8_t bits = state->keys[byte];
            if (!bits) continue;
            for (unsigned bit = 0; bit < 8u; ++bit) {
                if (!(bits & (1u << bit))) continue;
                ns2_kbm_source_t source = {NS2_KBM_SRC_KEY,
                                           (uint8_t)(byte * 8u + bit)};
                if (!ns2_kbm_source_valid(source)) continue;
                uint8_t destination = ns2_kbm_binding(config, profile, source);
                if (destination < NS2_DST_COUNT) held[destination] = true;
            }
        }
    }

    // Mouse-owned contribution. Only Keyboard + Mouse mode has a mouse role, so
    // a stale mouse can never leak into Keyboard mode.
    if (state->mouse_present && mode == NS2_KBM_MODE_KEYBOARD_MOUSE) {
        for (uint8_t button = 1u; button <= NS2_KBM_MOUSE_BUTTONS; ++button) {
            if (!(state->mouse_buttons & (uint16_t)(1u << (button - 1u))))
                continue;
            ns2_kbm_source_t source = {NS2_KBM_SRC_MOUSE, button};
            uint8_t destination = ns2_kbm_binding(config, profile, source);
            if (destination < NS2_DST_COUNT) held[destination] = true;
        }
    }

    // Buttons. held[] is a set, so several sources naming one destination press
    // it exactly once and releasing one of them cannot release the destination
    // while another is still down.
    for (uint8_t destination = 1u; destination < NS2_DST_COUNT; ++destination) {
        if (held[destination])
            ns2_kbm_apply_destination(destination, out->buttons, &out->extra);
    }

    // Opposing D-pad edges cancel, matching the stick rule.
    if (held[NS2_DST_DUP] && held[NS2_DST_DDOWN])
        out->buttons[2] &= (uint8_t)~(SWITCH_MASK_DPAD_UP | SWITCH_MASK_DPAD_DOWN);
    if (held[NS2_DST_DLEFT] && held[NS2_DST_DRIGHT])
        out->buttons[2] &= (uint8_t)~(SWITCH_MASK_DPAD_LEFT | SWITCH_MASK_DPAD_RIGHT);

    out->left_x = resolve_axis(held[NS2_DST_LSTICK_LEFT],
                               held[NS2_DST_LSTICK_RIGHT]);
    out->left_y = resolve_axis(held[NS2_DST_LSTICK_DOWN],
                               held[NS2_DST_LSTICK_UP]);

    uint16_t digital_right_x = resolve_axis(held[NS2_DST_RSTICK_LEFT],
                                            held[NS2_DST_RSTICK_RIGHT]);
    uint16_t digital_right_y = resolve_axis(held[NS2_DST_RSTICK_DOWN],
                                            held[NS2_DST_RSTICK_UP]);
    out->right_x = digital_right_x;
    out->right_y = digital_right_y;

    if (mode == NS2_KBM_MODE_KEYBOARD_MOUSE && state->mouse_present) {
        if (native_mouse) {
            // The selected output personality exposes a real pointer, so the
            // relative deltas go there unchanged and the right stick is left to
            // the digital bindings. This is the existing hardware-validated
            // Joy-Con 2 mouse path; nothing is converted to a stick first.
            out->has_mouse = 1;
            out->mouse_delta_x = state->mouse_delta_x;
            out->mouse_delta_y = state->mouse_delta_y;
            out->mouse_delta_wheel = state->mouse_delta_wheel;
        } else if (digital_right_x == SWITCH_STICK_MID &&
                   digital_right_y == SWITCH_STICK_MID) {
            // No native pointer: translate movement to the right stick. A
            // digital right-stick binding that is actually held wins, so the
            // two never fight over the same axis.
            //
            // Anti-deadzone is applied HERE, on a copy, and nowhere else. It is
            // compensation for the destination -- the same kind of concern as
            // the Y negation below -- so the velocity estimate in state->stick_*
            // stays a pure function of mouse speed and every temporal property
            // validated on hardware is untouched by it.
            int32_t stick_x = state->stick_x;
            int32_t stick_y = state->stick_y;
            ns2_kbm_mouse_anti_deadzone(&stick_x, &stick_y,
                                        config->mouse.anti_deadzone);
            out->right_x = (uint16_t)clamp_i32(
                (int32_t)SWITCH_STICK_MID + stick_x, 0, SWITCH_STICK_MAX);
            out->right_y = (uint16_t)clamp_i32(
                (int32_t)SWITCH_STICK_MID - stick_y, 0, SWITCH_STICK_MAX);
        }
    }

    // Relative motion is one-shot by definition: consume it here so a republish
    // (a battery notification, a keyboard report) cannot replay the same
    // movement.
    state->mouse_delta_x = 0;
    state->mouse_delta_y = 0;
    state->mouse_delta_wheel = 0;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

const char *ns2_kbm_mode_name(ns2_kbm_mode_t mode) {
    switch (mode) {
        case NS2_KBM_MODE_KEYBOARD: return "keyboard";
        case NS2_KBM_MODE_KEYBOARD_MOUSE: return "kbmouse";
        case NS2_KBM_MODE_CONTROLLER: return "controller";
        case NS2_KBM_MODE_AUTO: return "auto";
        default: return "auto";
    }
}

bool ns2_kbm_mode_from_name(const char *name, ns2_kbm_mode_t *out) {
    if (!name || !out) return false;
    if (strcmp(name, "controller") == 0) { *out = NS2_KBM_MODE_CONTROLLER; return true; }
    if (strcmp(name, "keyboard") == 0) { *out = NS2_KBM_MODE_KEYBOARD; return true; }
    if (strcmp(name, "kbmouse") == 0) { *out = NS2_KBM_MODE_KEYBOARD_MOUSE; return true; }
    if (strcmp(name, "auto") == 0) { *out = NS2_KBM_MODE_AUTO; return true; }
    return false;
}

const char *ns2_kbm_profile_name(ns2_kbm_profile_t profile) {
    return profile == NS2_KBM_PROFILE_KEYBOARD_MOUSE ? "kbm" : "kb";
}

bool ns2_kbm_profile_from_name(const char *name, ns2_kbm_profile_t *out) {
    if (!name || !out) return false;
    if (strcmp(name, "kb") == 0) { *out = NS2_KBM_PROFILE_KEYBOARD; return true; }
    if (strcmp(name, "kbm") == 0) { *out = NS2_KBM_PROFILE_KEYBOARD_MOUSE; return true; }
    return false;
}

static const struct {
    const char *name;
    uint8_t destination;
} KBM_DESTINATION_NAMES[] = {
    {"none", NS2_DST_NONE},
    {"a", NS2_DST_A},
    {"b", NS2_DST_B},
    {"x", NS2_DST_X},
    {"y", NS2_DST_Y},
    {"l", NS2_DST_L},
    {"r", NS2_DST_R},
    {"zl", NS2_DST_ZL},
    {"zr", NS2_DST_ZR},
    {"l3", NS2_DST_L3},
    {"r3", NS2_DST_R3},
    {"minus", NS2_DST_MINUS},
    {"plus", NS2_DST_PLUS},
    {"home", NS2_DST_HOME},
    {"capture", NS2_DST_CAPTURE},
    {"dup", NS2_DST_DUP},
    {"ddown", NS2_DST_DDOWN},
    {"dleft", NS2_DST_DLEFT},
    {"dright", NS2_DST_DRIGHT},
    {"gl", NS2_DST_GL},
    {"gr", NS2_DST_GR},
    {"c", NS2_DST_C},
    {"lstick_up", NS2_DST_LSTICK_UP},
    {"lstick_down", NS2_DST_LSTICK_DOWN},
    {"lstick_left", NS2_DST_LSTICK_LEFT},
    {"lstick_right", NS2_DST_LSTICK_RIGHT},
    {"rstick_up", NS2_DST_RSTICK_UP},
    {"rstick_down", NS2_DST_RSTICK_DOWN},
    {"rstick_left", NS2_DST_RSTICK_LEFT},
    {"rstick_right", NS2_DST_RSTICK_RIGHT},
};

const char *ns2_kbm_destination_name(uint8_t destination) {
    for (unsigned i = 0;
         i < sizeof(KBM_DESTINATION_NAMES) / sizeof(KBM_DESTINATION_NAMES[0]);
         ++i) {
        if (KBM_DESTINATION_NAMES[i].destination == destination)
            return KBM_DESTINATION_NAMES[i].name;
    }
    return "none";
}

bool ns2_kbm_destination_from_name(const char *name, uint8_t *out) {
    if (!name || !out) return false;
    for (unsigned i = 0;
         i < sizeof(KBM_DESTINATION_NAMES) / sizeof(KBM_DESTINATION_NAMES[0]);
         ++i) {
        if (strcmp(name, KBM_DESTINATION_NAMES[i].name) == 0) {
            *out = KBM_DESTINATION_NAMES[i].destination;
            return true;
        }
    }
    return false;
}

static char hex_digit(unsigned value) {
    return (char)(value < 10u ? '0' + value : 'A' + (value - 10u));
}

void ns2_kbm_source_format(ns2_kbm_source_t source, char *out, uint16_t len) {
    if (!out || len == 0) return;
    out[0] = '\0';
    if (source.kind == NS2_KBM_SRC_KEY && len >= 8u) {
        out[0] = 'k'; out[1] = 'e'; out[2] = 'y'; out[3] = ':';
        out[4] = hex_digit((unsigned)(source.code >> 4));
        out[5] = hex_digit((unsigned)(source.code & 0x0Fu));
        out[6] = '\0';
    } else if (source.kind == NS2_KBM_SRC_MOUSE && len >= 9u) {
        out[0] = 'm'; out[1] = 'o'; out[2] = 'u'; out[3] = 's';
        out[4] = 'e'; out[5] = ':';
        out[6] = (char)('0' + (source.code % 10u));
        out[7] = '\0';
    }
}

static int parse_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ns2_kbm_source_parse(const char *text, ns2_kbm_source_t *out) {
    if (!text || !out) return false;
    if (strncmp(text, "key:", 4) == 0) {
        const char *digits = text + 4;
        int high = parse_hex_digit(digits[0]);
        if (high < 0) return false;
        int low = parse_hex_digit(digits[1]);
        unsigned code;
        if (low < 0) {
            if (digits[1] != '\0') return false;
            code = (unsigned)high;
        } else {
            if (digits[2] != '\0') return false;
            code = (unsigned)((high << 4) | low);
        }
        out->kind = NS2_KBM_SRC_KEY;
        out->code = (uint8_t)code;
        return ns2_kbm_source_valid(*out);
    }
    if (strncmp(text, "mouse:", 6) == 0) {
        const char *digits = text + 6;
        if (digits[0] < '1' || digits[0] > '9' || digits[1] != '\0')
            return false;
        out->kind = NS2_KBM_SRC_MOUSE;
        out->code = (uint8_t)(digits[0] - '0');
        return ns2_kbm_source_valid(*out);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Peer roles and admission
// ---------------------------------------------------------------------------

ns2_kbm_primary_t ns2_kbm_primary_from_evidence(ns2_kbm_peer_caps_t caps,
                                                bool declares_combo,
                                                bool strong_keyboard) {
    // COMBO is never INFERRED from having both capabilities -- see below. It is
    // granted only when the device has said so, through its Class of Device or
    // through a report descriptor that is unmistakably a keyboard's.
    if ((declares_combo || strong_keyboard) && caps.keyboard && caps.pointer)
        return NS2_KBM_PRIMARY_COMBO;
    return ns2_kbm_primary_from_caps(caps);
}

ns2_kbm_primary_t ns2_kbm_primary_from_caps(ns2_kbm_peer_caps_t caps) {
    // Pointer wins. A gaming mouse declaring keyboard usages for its macro
    // buttons is still a mouse, and treating it as a combo would let it consume
    // the keyboard role the user's actual keyboard needs. COMBO is never
    // inferred here; it requires the device to declare itself one.
    if (caps.pointer) return NS2_KBM_PRIMARY_MOUSE;
    if (caps.keyboard) return NS2_KBM_PRIMARY_KEYBOARD;
    return NS2_KBM_PRIMARY_NONE;
}

void ns2_kbm_roles_init(ns2_kbm_roles_t *roles) {
    if (!roles) return;
    memset(roles, 0, sizeof(*roles));
}

// Two peer keys name the same physical HID occupant only when they agree on the
// stable identity AND the connection generation. Generation equality is what
// stops a stale callback from a recycled transport index from releasing or
// re-admitting a replacement peer.
static bool peer_equal(const ns2_kbm_peer_key_t *a,
                       const ns2_kbm_peer_key_t *b) {
    if (!a || !b || !a->valid || !b->valid) return false;
    if (a->connection_generation != b->connection_generation) return false;
    if (a->addr_valid != b->addr_valid) return false;
    if (a->addr_valid) return memcmp(a->addr, b->addr, sizeof(a->addr)) == 0;
    return a->conn_index == b->conn_index;
}

static void roles_assign_group_if_empty(ns2_kbm_roles_t *roles) {
    if (roles->keyboard.valid || roles->mouse.valid) return;
    roles->next_group_id++;
    if (roles->next_group_id == 0u) roles->next_group_id = 1u;
    roles->group_id = roles->next_group_id;
}

ns2_kbm_mode_t ns2_kbm_effective_mode(ns2_kbm_mode_t override_mode,
                                      bool keyboard_present,
                                      bool mouse_present) {
    // An explicit "Controller" is the user saying KB/M is off; nothing infers
    // past it. It is also what the role policy uses to refuse both roles, so in
    // practice neither role is ever present here.
    if (override_mode == NS2_KBM_MODE_CONTROLLER) return NS2_KBM_MODE_CONTROLLER;

    if (keyboard_present && mouse_present) return NS2_KBM_MODE_KEYBOARD_MOUSE;

    if (keyboard_present) {
        // A mouse that is merely absent must not silently demote the profile:
        // if the user pinned Keyboard + Mouse, the KB/M profile stays effective
        // with the mouse role empty.
        return override_mode == NS2_KBM_MODE_KEYBOARD_MOUSE
                   ? NS2_KBM_MODE_KEYBOARD_MOUSE : NS2_KBM_MODE_KEYBOARD;
    }

    // A mouse alone bootstraps the composite: it is half of a Keyboard + Mouse
    // source whose keyboard has not arrived yet. (The native Joy-Con mouse
    // exception is applied earlier, by refusing the mouse role at all, so a
    // mouse-only source never reaches this point on those personalities.)
    if (mouse_present) return NS2_KBM_MODE_KEYBOARD_MOUSE;

    return NS2_KBM_MODE_CONTROLLER;
}

ns2_kbm_discovery_t ns2_kbm_completion_update(ns2_kbm_completion_t *state,
                                              bool keyboard_connected,
                                              bool mouse_connected,
                                              bool controller_connected,
                                              uint32_t now_ms) {
    if (!state) return NS2_KBM_DISCOVERY_SEEK;

    uint8_t held = 0u;
    if (keyboard_connected) held |= (uint8_t)NS2_KBM_HELD_KEYBOARD;
    if (mouse_connected) held |= (uint8_t)NS2_KBM_HELD_MOUSE;

    const bool partial = (held == (uint8_t)NS2_KBM_HELD_KEYBOARD) ||
                         (held == (uint8_t)NS2_KBM_HELD_MOUSE);

    if (!partial) {
        // Complete, or no KB/M role at all. Either way there is no window: a
        // complete source idles discovery, and a source with nothing connected
        // falls back to the ordinary always-on discovery behavior, which stays
        // authoritative.
        state->held = held;
        state->windowing = 0u;
        state->started_ms = 0u;
        return ns2_kbm_logical_source_complete(keyboard_connected, mouse_connected,
                                               controller_connected)
                   ? NS2_KBM_DISCOVERY_IDLE : NS2_KBM_DISCOVERY_SEEK;
    }

    // Partial. Start a window only on a TRANSITION into this partial state --
    // from complete (a role was lost), from empty (the first role arrived), or
    // from the other partial state (keyboard-only became mouse-only, a new
    // source rather than a continuation).
    if (state->held != held) {
        state->held = held;
        state->windowing = 1u;
        state->started_ms = now_ms;
        return NS2_KBM_DISCOVERY_SEEK;
    }

    // Unchanged partial state. Nothing here can restart the window: this branch
    // is reached on every service tick regardless of how much keyboard or mouse
    // traffic arrived, so an actively used peripheral cannot keep discovery
    // alive.
    if (!state->windowing) return NS2_KBM_DISCOVERY_IDLE;

    // Unsigned elapsed comparison, wrap-safe across the 32-bit ms rollover.
    if ((uint32_t)(now_ms - state->started_ms) >= NS2_KBM_COMPLETION_WINDOW_MS) {
        // The complementary role did not arrive. Treat the partial source as
        // intentional: it keeps working, the radio just stops hunting. An
        // explicit pairing request can still re-open discovery later.
        state->windowing = 0u;
        return NS2_KBM_DISCOVERY_IDLE;
    }
    return NS2_KBM_DISCOVERY_SEEK;
}

ns2_kbm_discovery_action_t ns2_kbm_discovery_policy(bool pairing_window_open,
                                                    bool source_complete,
                                                    ns2_kbm_discovery_t timed) {
    if (pairing_window_open) {
        // The user explicitly asked to discover. That outranks the bounded
        // window entirely: keep re-asserting until the source is complete, so a
        // peer finishing its connection mid-window cannot end discovery for the
        // rest of it.
        return source_complete ? NS2_KBM_DISCOVERY_LEAVE : NS2_KBM_DISCOVERY_ARM;
    }
    return (timed == NS2_KBM_DISCOVERY_SEEK) ? NS2_KBM_DISCOVERY_ARM
                                             : NS2_KBM_DISCOVERY_RETIRE;
}

bool ns2_kbm_logical_source_complete(bool keyboard_connected,
                                     bool mouse_connected,
                                     bool controller_connected) {
    if (keyboard_connected || mouse_connected) {
        // Deliberately NOT keyed off the effective mode: under AUTO that mode is
        // derived from whichever roles are already filled, so it would report
        // "complete" the moment one peer arrived and could never ask discovery
        // to keep looking for the other.
        return keyboard_connected && mouse_connected;
    }
    return controller_connected;
}

ns2_kbm_admit_t ns2_kbm_roles_admit(ns2_kbm_roles_t *roles,
                                    ns2_kbm_role_policy_t policy,
                                    ns2_kbm_primary_t primary,
                                    ns2_kbm_peer_caps_t caps,
                                    const ns2_kbm_peer_key_t *key) {
    if (!roles || !key || !key->valid) return NS2_KBM_ADMIT_REJECT_MODE;

    // Which roles this peer is OFFERING. A device that merely has the capability
    // does not offer the role: a gaming mouse with macro keys is a mouse, and
    // must leave the keyboard role for the actual keyboard. Only a device that
    // genuinely represents both may offer both.
    bool offers_keyboard = false;
    bool offers_mouse = false;
    switch (primary) {
        case NS2_KBM_PRIMARY_KEYBOARD:
            offers_keyboard = caps.keyboard != 0u;
            break;
        case NS2_KBM_PRIMARY_MOUSE:
            offers_mouse = caps.pointer != 0u;
            break;
        case NS2_KBM_PRIMARY_COMBO:
            offers_keyboard = caps.keyboard != 0u;
            offers_mouse = caps.pointer != 0u;
            break;
        default:
            roles->rejected_mode++;
            return NS2_KBM_ADMIT_REJECT_MODE;
    }
    if (!offers_keyboard && !offers_mouse) {
        roles->rejected_mode++;
        return NS2_KBM_ADMIT_REJECT_MODE;
    }

    bool may_keyboard = offers_keyboard && policy.allow_keyboard;
    bool may_mouse = offers_mouse && policy.allow_mouse;
    if (!may_keyboard && !may_mouse) {
        roles->rejected_mode++;
        return NS2_KBM_ADMIT_REJECT_MODE;
    }

    bool already_keyboard = peer_equal(&roles->keyboard, key);
    bool already_mouse = peer_equal(&roles->mouse, key);

    // A role becoming free must NEVER let a surviving peer expand its ownership.
    //
    // Hardware failure this prevents: the KERIS II mouse held the mouse role;
    // the real keyboard powered off, freeing the keyboard role; a later
    // re-admission of the mouse re-derived its primary as KEYBOARD (its
    // classification slot had been lost to connection-index reuse, leaving only
    // the keyboard driver's narrow capability view) and it took the keyboard
    // role too. `keyboardConn == mouseConn == 5`, the source looked complete,
    // and the real keyboard could never come back.
    //
    // Once a peer holds a role, that IS its role for this connection
    // generation, whatever a later classification claims. Only a peer
    // positively classified COMBO may hold both.
    if (primary != NS2_KBM_PRIMARY_COMBO && (already_keyboard || already_mouse)) {
        offers_keyboard = already_keyboard;
        offers_mouse = already_mouse;
        may_keyboard = offers_keyboard && policy.allow_keyboard;
        may_mouse = offers_mouse && policy.allow_mouse;
        if (!may_keyboard && !may_mouse) {
            roles->rejected_mode++;
            return NS2_KBM_ADMIT_REJECT_MODE;
        }
    }

    bool keyboard_free = !roles->keyboard.valid || already_keyboard;
    bool mouse_free = !roles->mouse.valid || already_mouse;

    // Take every offered role that is free. For a single-primary peer that is
    // at most its own role, so a mouse whose role is taken is a duplicate and
    // never silently becomes a second keyboard. For a genuine combo it is both
    // when both are free and whichever one is free otherwise -- which is the
    // case that fixes the original bug.
    bool take_keyboard = may_keyboard && keyboard_free;
    bool take_mouse = may_mouse && mouse_free;

    if (!take_keyboard && !take_mouse) {
        roles->rejected_duplicate++;
        return NS2_KBM_ADMIT_REJECT_DUPLICATE;
    }

    roles_assign_group_if_empty(roles);
    if (take_keyboard) roles->keyboard = *key;
    if (take_mouse) roles->mouse = *key;

    if (take_keyboard && take_mouse) return NS2_KBM_ADMIT_BOTH;
    return take_keyboard ? NS2_KBM_ADMIT_KEYBOARD : NS2_KBM_ADMIT_MOUSE;
}

bool ns2_kbm_roles_release(ns2_kbm_roles_t *roles,
                           const ns2_kbm_peer_key_t *key,
                           bool *released_keyboard, bool *released_mouse) {
    if (released_keyboard) *released_keyboard = false;
    if (released_mouse) *released_mouse = false;
    if (!roles || !key) return false;

    bool any = false;
    if (peer_equal(&roles->keyboard, key)) {
        memset(&roles->keyboard, 0, sizeof(roles->keyboard));
        roles->role_losses++;
        if (released_keyboard) *released_keyboard = true;
        any = true;
    }
    if (peer_equal(&roles->mouse, key)) {
        memset(&roles->mouse, 0, sizeof(roles->mouse));
        roles->role_losses++;
        if (released_mouse) *released_mouse = true;
        any = true;
    }
    // The composite identity survives while any role remains, so a reconnecting
    // peer rejoins the same logical source instead of appearing as a new one.
    if (any && !roles->keyboard.valid && !roles->mouse.valid)
        roles->group_id = 0u;
    return any;
}

void ns2_kbm_roles_release_all(ns2_kbm_roles_t *roles) {
    if (!roles) return;
    memset(&roles->keyboard, 0, sizeof(roles->keyboard));
    memset(&roles->mouse, 0, sizeof(roles->mouse));
    roles->group_id = 0u;
}

bool ns2_kbm_roles_contains(const ns2_kbm_roles_t *roles,
                            const ns2_kbm_peer_key_t *key) {
    if (!roles || !key) return false;
    return peer_equal(&roles->keyboard, key) || peer_equal(&roles->mouse, key);
}
