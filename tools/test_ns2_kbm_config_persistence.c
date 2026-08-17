// Persisted settings record: schema migration and KB/M mapping persistence.
//
// Host-only. The failure this guards against is a schema change silently
// reinterpreting an existing adapter's stored bytes as different settings --
// which a firmware build cannot catch, because both layouts compile fine.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config_persist.h"

#define KEY_F 0x09u
#define KEY_SPACE 0x2Cu

static ns2_kbm_source_t key(uint8_t usage) {
    ns2_kbm_source_t source = {NS2_KBM_SRC_KEY, usage};
    return source;
}

static ns2_kbm_source_t mouse_button(uint8_t number) {
    ns2_kbm_source_t source = {NS2_KBM_SRC_MOUSE, number};
    return source;
}

// A stored schema-10 record from an adapter with customized colours and a
// learned wake identity.
static void make_v10(config_record_v10_t *record) {
    memset(record, 0, sizeof(*record));
    record->magic = CONFIG_PERSIST_MAGIC;
    record->version = 10u;
    record->body_color[0] = 0x11;
    record->body_color[1] = 0x22;
    record->body_color[2] = 0x33;
    record->joycon2_left_accent[0] = 0x44;
    record->joycon2_right_accent[0] = 0x55;
    record->wake_valid = 0xA5u;
    record->wake_identity.product_id = 0x2069u;
    record->wake_identity.host_count = 1u;
    record->wake_identity.controller_addr_wire[0] = 0xAB;
    record->wake_identity.host_addr_wire[0][0] = 0xCD;
}

static void test_defaults(void) {
    config_record_t record;
    config_persist_defaults(&record);
    assert(record.magic == CONFIG_PERSIST_MAGIC);
    assert(record.version == CONFIG_PERSIST_VERSION);
    assert(record.body_color[0] == 0x23);
    assert(record.kbm.mode == NS2_KBM_MODE_AUTO);
    assert(record.kbm.profiles[NS2_KBM_PROFILE_KEYBOARD].count == 0);
    assert(record.kbm.mouse.sensitivity_x == NS2_KBM_MOUSE_SENS_DEFAULT);
    puts("  defaults");
}

static void test_blank_and_foreign_records(void) {
    config_record_t record;

    // An erased sector.
    uint8_t erased[sizeof(config_record_t)];
    memset(erased, 0xFF, sizeof(erased));
    assert(config_persist_load(erased, sizeof(erased), &record) ==
           CONFIG_PERSIST_DEFAULTED);
    assert(record.version == CONFIG_PERSIST_VERSION);
    assert(record.kbm.mode == NS2_KBM_MODE_AUTO);

    // Wrong magic.
    config_record_t stored;
    config_persist_defaults(&stored);
    stored.magic = 0xDEADBEEFu;
    assert(config_persist_load(&stored, sizeof(stored), &record) ==
           CONFIG_PERSIST_DEFAULTED);

    // A schema this build has never seen -- older or newer -- must not have its
    // bytes reinterpreted.
    config_persist_defaults(&stored);
    stored.version = 9u;
    stored.body_color[0] = 0x77;
    assert(config_persist_load(&stored, sizeof(stored), &record) ==
           CONFIG_PERSIST_DEFAULTED);
    assert(record.body_color[0] == 0x23);
    config_persist_defaults(&stored);
    stored.version = 250u;
    assert(config_persist_load(&stored, sizeof(stored), &record) ==
           CONFIG_PERSIST_DEFAULTED);

    // A truncated read is never allowed to produce a partially-populated record.
    config_persist_defaults(&stored);
    assert(config_persist_load(&stored, sizeof(config_record_v10_t), &record) ==
           CONFIG_PERSIST_DEFAULTED);
    assert(config_persist_load(NULL, 0, &record) == CONFIG_PERSIST_DEFAULTED);
    puts("  blank and foreign records");
}

static void test_v10_migration(void) {
    config_record_v10_t old;
    make_v10(&old);

    // A schema-10 record is padded into the (larger) flash region it was
    // stored in; the loader must not depend on what follows it.
    uint8_t sector[sizeof(config_record_t) + 64];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &old, sizeof(old));

    config_record_t record;
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_MIGRATED);

    // Every unrelated setting survives the version bump.
    assert(record.body_color[0] == 0x11 && record.body_color[1] == 0x22 &&
           record.body_color[2] == 0x33);
    assert(record.joycon2_left_accent[0] == 0x44);
    assert(record.joycon2_right_accent[0] == 0x55);
    assert(record.wake_valid == 0xA5u);
    assert(record.wake_identity.product_id == 0x2069u);
    assert(record.wake_identity.host_count == 1u);
    assert(record.wake_identity.controller_addr_wire[0] == 0xAB);
    assert(record.wake_identity.host_addr_wire[0][0] == 0xCD);

    // The new block gets deterministic canonical defaults, NOT whatever bytes
    // happened to follow the old record in flash.
    assert(record.version == CONFIG_PERSIST_VERSION);
    assert(record.kbm.mode == NS2_KBM_MODE_AUTO);
    assert(record.kbm.profiles[NS2_KBM_PROFILE_KEYBOARD].count == 0);
    assert(record.kbm.profiles[NS2_KBM_PROFILE_KEYBOARD_MOUSE].count == 0);
    assert(record.kbm.mouse.sensitivity_x == NS2_KBM_MOUSE_SENS_DEFAULT);
    assert(record.kbm.mouse.recenter_ms == NS2_KBM_MOUSE_RECENTER_DEFAULT_MS);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F)) ==
           NS2_DST_A);

    // Migration is deterministic: the same stored bytes always produce the same
    // record.
    config_record_t again;
    assert(config_persist_load(sector, sizeof(sector), &again) ==
           CONFIG_PERSIST_MIGRATED);
    assert(memcmp(&record, &again, sizeof(record)) == 0);
    puts("  schema 10 -> 11 migration");
}

static void test_mapping_round_trip(void) {
    config_record_t stored;
    config_persist_defaults(&stored);

    // A user customizes both profiles independently, plus the mode and the
    // mouse translation settings.
    stored.kbm.mode = (uint8_t)NS2_KBM_MODE_KEYBOARD_MOUSE;
    assert(ns2_kbm_set_binding(&stored.kbm, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    assert(ns2_kbm_set_binding(&stored.kbm, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                               mouse_button(1), NS2_DST_A));
    assert(ns2_kbm_set_binding(&stored.kbm, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                               key(KEY_SPACE), NS2_DST_NONE));
    stored.kbm.mouse.sensitivity_x = 1024u;
    stored.kbm.mouse.invert_y = 1u;
    stored.body_color[0] = 0x99;

    // Reboot.
    config_record_t record;
    assert(config_persist_load(&stored, sizeof(stored), &record) ==
           CONFIG_PERSIST_CURRENT);
    assert(record.kbm.mode == NS2_KBM_MODE_KEYBOARD_MOUSE);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           mouse_button(1)) == NS2_DST_A);
    // An explicit unassign survives as an unassign, not as "restore default".
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           key(KEY_SPACE)) == NS2_DST_NONE);
    // The profiles stayed independent across the round trip.
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           key(KEY_F)) == NS2_DST_A);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_PROFILE_KEYBOARD,
                           key(KEY_SPACE)) == NS2_DST_B);
    assert(record.kbm.mouse.sensitivity_x == 1024u);
    assert(record.kbm.mouse.invert_y == 1u);
    assert(record.body_color[0] == 0x99);

    // Resetting one profile leaves the other, the mode, the mouse settings, and
    // unrelated adapter settings untouched.
    ns2_kbm_config_reset_profile(&record.kbm, NS2_KBM_PROFILE_KEYBOARD);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_PROFILE_KEYBOARD, key(KEY_F)) ==
           NS2_DST_A);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_PROFILE_KEYBOARD_MOUSE,
                           mouse_button(1)) == NS2_DST_A);
    assert(record.kbm.mode == NS2_KBM_MODE_KEYBOARD_MOUSE);
    assert(record.kbm.mouse.sensitivity_x == 1024u);
    assert(record.body_color[0] == 0x99);
    puts("  mapping round trip");
}

static void test_corrupt_mapping_fails_safe(void) {
    config_record_t stored;
    config_persist_defaults(&stored);
    stored.body_color[0] = 0x88;
    stored.wake_valid = 0xA5u;
    // Corrupt only the mapping block.
    memset(&stored.kbm, 0x5A, sizeof(stored.kbm));

    config_record_t record;
    config_persist_load_t result = config_persist_load(&stored, sizeof(stored),
                                                       &record);
    assert(result == CONFIG_PERSIST_REPAIRED);
    // Unrelated settings are untouched by a mapping repair.
    assert(record.body_color[0] == 0x88);
    assert(record.wake_valid == 0xA5u);
    // And the mapping block is usable, with no arbitrary destinations.
    assert(record.kbm.mode < NS2_KBM_MODE_COUNT);
    for (unsigned p = 0; p < NS2_KBM_PROFILE_COUNT; ++p) {
        assert(record.kbm.profiles[p].count <= NS2_KBM_MAX_OVERRIDES);
        for (uint8_t i = 0; i < record.kbm.profiles[p].count; ++i) {
            assert(ns2_kbm_source_valid(record.kbm.profiles[p].entries[i].source));
            assert(ns2_kbm_destination_valid(
                record.kbm.profiles[p].entries[i].destination));
        }
    }
    assert(record.kbm.mouse.sensitivity_x >= NS2_KBM_MOUSE_SENS_MIN &&
           record.kbm.mouse.sensitivity_x <= NS2_KBM_MOUSE_SENS_MAX);
    assert(record.kbm.mouse.recenter_ms >= NS2_KBM_MOUSE_RECENTER_MIN_MS &&
           record.kbm.mouse.recenter_ms <= NS2_KBM_MOUSE_RECENTER_MAX_MS);
    puts("  corrupt mapping fails safe");
}

int main(void) {
    puts("config persistence:");
    test_defaults();
    test_blank_and_foreign_records();
    test_v10_migration();
    test_mapping_round_trip();
    test_corrupt_mapping_fails_safe();
    puts("config persistence tests passed");
    return 0;
}
