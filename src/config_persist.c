#include "config_persist.h"

#include <string.h>

// The migration below is a prefix copy, which is only correct while schema 11
// begins with schema 10's exact layout. Assert that here rather than trusting a
// future edit to remember it: silently reinterpreting an existing adapter's
// stored bytes as different settings is precisely the failure this guards.
_Static_assert(offsetof(config_record_t, magic) ==
                   offsetof(config_record_v10_t, magic),
               "v11 must keep v10's magic offset");
_Static_assert(offsetof(config_record_t, version) ==
                   offsetof(config_record_v10_t, version),
               "v11 must keep v10's version offset");
_Static_assert(offsetof(config_record_t, body_color) ==
                   offsetof(config_record_v10_t, body_color),
               "v11 must keep v10's body_color offset");
_Static_assert(offsetof(config_record_t, joycon2_left_accent) ==
                   offsetof(config_record_v10_t, joycon2_left_accent),
               "v11 must keep v10's joycon2_left_accent offset");
_Static_assert(offsetof(config_record_t, joycon2_right_accent) ==
                   offsetof(config_record_v10_t, joycon2_right_accent),
               "v11 must keep v10's joycon2_right_accent offset");
_Static_assert(offsetof(config_record_t, wake_valid) ==
                   offsetof(config_record_v10_t, wake_valid),
               "v11 must keep v10's wake_valid offset");
_Static_assert(offsetof(config_record_t, wake_identity) ==
                   offsetof(config_record_v10_t, wake_identity),
               "v11 must keep v10's wake_identity offset");
// New fields must begin past the last byte any v10 field occupies. Comparing
// against sizeof(v10) instead would be wrong in the harmless direction (its
// trailing alignment padding never held a setting) but right in the direction
// that matters, so pin the exact end of the last field.
_Static_assert(offsetof(config_record_t, kbm) >=
                   offsetof(config_record_v10_t, wake_identity) +
                       sizeof(config_wake_identity_t),
               "v11 additions must not overlap any v10 field");

void config_persist_defaults(config_record_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->magic = CONFIG_PERSIST_MAGIC;
    out->version = CONFIG_PERSIST_VERSION;
    // Genuine retail Pro Controller 2 body colour. Users can replace this with
    // any RGB value; it drives Sony lights while Pro2 is active.
    out->body_color[0] = 0x23;
    out->body_color[1] = 0x23;
    out->body_color[2] = 0x23;
    // Genuine retail Joy-Con 2 accent colours from the project's L/R SPI dumps.
    out->joycon2_left_accent[0] = 0x9B;
    out->joycon2_left_accent[1] = 0xE1;
    out->joycon2_left_accent[2] = 0xE6;
    out->joycon2_right_accent[0] = 0xFF;
    out->joycon2_right_accent[1] = 0x8C;
    out->joycon2_right_accent[2] = 0x5F;
    ns2_kbm_config_defaults(&out->kbm);
}

config_persist_load_t config_persist_load(const void *stored, uint32_t stored_len,
                                          config_record_t *out) {
    if (!out) return CONFIG_PERSIST_DEFAULTED;
    config_persist_defaults(out);
    if (!stored || stored_len < sizeof(config_record_v10_t))
        return CONFIG_PERSIST_DEFAULTED;

    config_record_v10_t header;
    memcpy(&header, stored, sizeof(header));
    if (header.magic != CONFIG_PERSIST_MAGIC) return CONFIG_PERSIST_DEFAULTED;

    config_persist_load_t result;
    if (header.version == CONFIG_PERSIST_VERSION) {
        if (stored_len < sizeof(config_record_t)) return CONFIG_PERSIST_DEFAULTED;
        memcpy(out, stored, sizeof(*out));
        result = CONFIG_PERSIST_CURRENT;
    } else if (header.version == 10u) {
        // Upgrade in place: every unrelated setting the user already had is
        // preserved, and only the new block takes canonical defaults.
        out->body_color[0] = header.body_color[0];
        out->body_color[1] = header.body_color[1];
        out->body_color[2] = header.body_color[2];
        memcpy(out->joycon2_left_accent, header.joycon2_left_accent,
               sizeof(out->joycon2_left_accent));
        memcpy(out->joycon2_right_accent, header.joycon2_right_accent,
               sizeof(out->joycon2_right_accent));
        out->wake_valid = header.wake_valid;
        out->wake_identity = header.wake_identity;
        result = CONFIG_PERSIST_MIGRATED;
    } else {
        // An unknown (older or newer) schema. Falling back to defaults is the
        // only safe answer: reinterpreting its bytes would invent settings.
        return CONFIG_PERSIST_DEFAULTED;
    }

    out->magic = CONFIG_PERSIST_MAGIC;
    out->version = CONFIG_PERSIST_VERSION;
    if (!ns2_kbm_config_sanitize(&out->kbm) && result == CONFIG_PERSIST_CURRENT)
        result = CONFIG_PERSIST_REPAIRED;
    return result;
}
