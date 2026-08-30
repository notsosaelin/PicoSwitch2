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
    assert(record.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD].count == 0);
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
    assert(record.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD].count == 0);
    assert(record.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].count == 0);
    assert(record.kbm.mouse.sensitivity_x == NS2_KBM_MOUSE_SENS_DEFAULT);
    assert(record.kbm.mouse.recenter_ms == NS2_KBM_MOUSE_RECENTER_DEFAULT_MS);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_A);

    // Migration is deterministic: the same stored bytes always produce the same
    // record.
    config_record_t again;
    assert(config_persist_load(sector, sizeof(sector), &again) ==
           CONFIG_PERSIST_MIGRATED);
    assert(memcmp(&record, &again, sizeof(record)) == 0);
    puts("  schema 10 -> current migration");
}

// Schema 12 appends the mouse anti-deadzone INSIDE the KB/M block, which
// resizes the mouse settings. A v11 adapter's stored bytes therefore cannot be
// read as a v12 record, and the migration must land every existing field on its
// new home while the one new setting takes its OFF default -- so upgrading an
// installed adapter cannot change how it already feels.
static void test_v11_migration(void) {
    config_record_v11_t old;
    memset(&old, 0, sizeof(old));
    old.magic = CONFIG_PERSIST_MAGIC;
    old.version = 11u;
    old.body_color[0] = 0x11;
    old.body_color[1] = 0x22;
    old.body_color[2] = 0x33;
    old.joycon2_left_accent[0] = 0x44;
    old.joycon2_right_accent[0] = 0x55;
    old.wake_valid = 0xA5u;
    old.wake_identity.product_id = 0x2069u;
    old.wake_identity.host_count = 1u;
    old.wake_identity.controller_addr_wire[0] = 0xAB;
    old.wake_identity.host_addr_wire[0][0] = 0xCD;

    // A user who had customized their KB/M setup on v11.
    old.kbm.mode = (uint8_t)NS2_KBM_MODE_KEYBOARD_MOUSE;
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD].count = 1u;
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD].entries[0].source.kind =
        NS2_KBM_SRC_KEY;
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD].entries[0].source.code = KEY_F;
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD].entries[0].destination =
        NS2_DST_X;
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].count = 1u;
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].entries[0].source.kind =
        NS2_KBM_SRC_MOUSE;
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].entries[0].source.code = 3u;
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].entries[0].destination =
        NS2_DST_B;
    old.kbm.mouse.sensitivity_x = 1024u;
    old.kbm.mouse.sensitivity_y = 1536u;
    old.kbm.mouse.recenter_ms = 240u;
    old.kbm.mouse.invert_x = 1u;
    old.kbm.mouse.invert_y = 0u;

    uint8_t sector[sizeof(config_record_t) + 64];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &old, sizeof(old));

    config_record_t record;
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_MIGRATED);
    assert(record.version == CONFIG_PERSIST_VERSION);
    // Deliberate tripwire: bumping the schema must bring whoever did it here to
    // confirm every older layout still has a migration. v11 now upgrades two
    // steps in one load, so this test covers 11 -> 13, not 11 -> 12.
    assert(CONFIG_PERSIST_VERSION == 13u);

    // Unrelated settings survive.
    assert(record.body_color[0] == 0x11 && record.body_color[1] == 0x22 &&
           record.body_color[2] == 0x33);
    assert(record.joycon2_left_accent[0] == 0x44);
    assert(record.joycon2_right_accent[0] == 0x55);
    assert(record.wake_valid == 0xA5u);
    assert(record.wake_identity.product_id == 0x2069u);
    assert(record.wake_identity.host_count == 1u);
    assert(record.wake_identity.controller_addr_wire[0] == 0xAB);
    assert(record.wake_identity.host_addr_wire[0][0] == 0xCD);

    // The whole KB/M block survives, mode, both mapping profiles and every
    // mouse setting alike.
    assert(record.kbm.mode == (uint8_t)NS2_KBM_MODE_KEYBOARD_MOUSE);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    ns2_kbm_source_t middle = {NS2_KBM_SRC_MOUSE, 3u};
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                           middle) == NS2_DST_B);
    assert(record.kbm.mouse.sensitivity_x == 1024u);
    assert(record.kbm.mouse.sensitivity_y == 1536u);
    assert(record.kbm.mouse.recenter_ms == 240u);
    assert(record.kbm.mouse.invert_x == 1u);
    assert(record.kbm.mouse.invert_y == 0u);

    // The one new setting is OFF, and is NOT whatever bytes followed the v11
    // record in flash (0xFF here, which would be a wildly out-of-range value).
    assert(record.kbm.mouse.anti_deadzone == 0u);
    assert(record.kbm.mouse.anti_deadzone == NS2_KBM_MOUSE_ADZ_DEFAULT);

    // Deterministic.
    config_record_t again;
    assert(config_persist_load(sector, sizeof(sector), &again) ==
           CONFIG_PERSIST_MIGRATED);
    assert(memcmp(&record, &again, sizeof(record)) == 0);

    // A truncated v11 record is refused rather than partially interpreted.
    assert(config_persist_load(sector, sizeof(config_record_v11_t) - 1u,
                               &record) == CONFIG_PERSIST_DEFAULTED);
    puts("  schema 11 -> 13 migration");
}

// A v12 record round-trips the new setting, and an out-of-range stored value is
// repaired to OFF by the existing sanitize policy rather than being applied.
static void test_anti_deadzone_persistence(void) {
    config_record_t stored;
    config_persist_defaults(&stored);
    assert(stored.kbm.mouse.anti_deadzone == 0u);

    stored.kbm.mouse.anti_deadzone = 15u;
    stored.kbm.mouse.sensitivity_x = 1024u;

    config_record_t loaded;
    assert(config_persist_load(&stored, sizeof(stored), &loaded) ==
           CONFIG_PERSIST_CURRENT);
    assert(loaded.kbm.mouse.anti_deadzone == 15u);
    assert(loaded.kbm.mouse.sensitivity_x == 1024u);

    // Every value across the configured range survives a save/reload.
    for (unsigned percent = 0; percent <= NS2_KBM_MOUSE_ADZ_MAX; ++percent) {
        stored.kbm.mouse.anti_deadzone = (uint8_t)percent;
        assert(config_persist_load(&stored, sizeof(stored), &loaded) ==
               CONFIG_PERSIST_CURRENT);
        assert(loaded.kbm.mouse.anti_deadzone == (uint8_t)percent);
    }

    // Corrupt/out-of-range persisted values fail closed to OFF, and the record
    // is reported as repaired rather than silently accepted.
    stored.kbm.mouse.anti_deadzone = 0xFFu;
    assert(config_persist_load(&stored, sizeof(stored), &loaded) ==
           CONFIG_PERSIST_REPAIRED);
    assert(loaded.kbm.mouse.anti_deadzone == 0u);
    // The rest of the record is untouched by that repair.
    assert(loaded.kbm.mouse.sensitivity_x == 1024u);

    stored.kbm.mouse.anti_deadzone = (uint8_t)(NS2_KBM_MOUSE_ADZ_MAX + 1u);
    assert(config_persist_load(&stored, sizeof(stored), &loaded) ==
           CONFIG_PERSIST_REPAIRED);
    assert(loaded.kbm.mouse.anti_deadzone == 0u);
    puts("  anti-deadzone persistence");
}

static void test_mapping_round_trip(void) {
    config_record_t stored;
    config_persist_defaults(&stored);

    // A user customizes both profiles independently, plus the mode and the
    // mouse translation settings.
    stored.kbm.mode = (uint8_t)NS2_KBM_MODE_KEYBOARD_MOUSE;
    assert(ns2_kbm_set_binding(&stored.kbm, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    assert(ns2_kbm_set_binding(&stored.kbm, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                               mouse_button(1), NS2_DST_A));
    assert(ns2_kbm_set_binding(&stored.kbm, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                               key(KEY_SPACE), NS2_DST_NONE));
    stored.kbm.mouse.sensitivity_x = 1024u;
    stored.kbm.mouse.invert_y = 1u;
    stored.body_color[0] = 0x99;

    // Reboot.
    config_record_t record;
    assert(config_persist_load(&stored, sizeof(stored), &record) ==
           CONFIG_PERSIST_CURRENT);
    assert(record.kbm.mode == NS2_KBM_MODE_KEYBOARD_MOUSE);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                           mouse_button(1)) == NS2_DST_A);
    // An explicit unassign survives as an unassign, not as "restore default".
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                           key(KEY_SPACE)) == NS2_DST_NONE);
    // The profiles stayed independent across the round trip.
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                           key(KEY_F)) == NS2_DST_A);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD,
                           key(KEY_SPACE)) == NS2_DST_B);
    assert(record.kbm.mouse.sensitivity_x == 1024u);
    assert(record.kbm.mouse.invert_y == 1u);
    assert(record.body_color[0] == 0x99);

    // Resetting one profile leaves the other, the mode, the mouse settings, and
    // unrelated adapter settings untouched.
    ns2_kbm_config_reset_profile(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_A);
    assert(ns2_kbm_binding(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
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
    for (unsigned p = 0; p < NS2_KBM_LAYOUT_COUNT; ++p) {
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


static void test_v12_migration(void) {
    config_record_v12_t old;
    memset(&old, 0, sizeof(old));
    old.magic = CONFIG_PERSIST_MAGIC;
    old.version = 12u;
    old.body_color[0] = 0x71;
    old.joycon2_left_accent[1] = 0x72;
    old.joycon2_right_accent[2] = 0x73;
    old.wake_valid = 0x5Au;
    old.wake_identity.product_id = 0x2069u;
    ns2_kbm_config_defaults(&old.kbm);
    old.kbm.mode = (uint8_t)NS2_KBM_MODE_KEYBOARD;
    old.kbm.mouse.sensitivity_x = 777u;
    old.kbm.mouse.anti_deadzone = 12u;

    uint8_t sector[512];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &old, sizeof(old));

    config_record_t record;
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_MIGRATED);

    // Every v12 setting survives.
    assert(record.version == CONFIG_PERSIST_VERSION);
    assert(record.body_color[0] == 0x71);
    assert(record.joycon2_left_accent[1] == 0x72);
    assert(record.joycon2_right_accent[2] == 0x73);
    assert(record.wake_valid == 0x5Au);
    assert(record.wake_identity.product_id == 0x2069u);
    assert(record.kbm.mode == (uint8_t)NS2_KBM_MODE_KEYBOARD);
    assert(record.kbm.mouse.sensitivity_x == 777u);
    assert(record.kbm.mouse.anti_deadzone == 12u);

    // And the new table starts EMPTY. An upgraded adapter must not claim a role
    // it never observed -- each companion re-registers on its next session.
    for (unsigned i = 0; i < CONFIG_MGMT_COMPANIONS_MAX; ++i)
        assert(!record.mgmt_companions[i].valid);

    // A truncated v12 record is refused rather than partially interpreted.
    assert(config_persist_load(sector, sizeof(config_record_v12_t) - 1u,
                               &record) == CONFIG_PERSIST_DEFAULTED);
    puts("  schema 12 -> 13 migration");
}

static void test_mgmt_companion_membership(void) {
    config_mgmt_companion_t table[CONFIG_MGMT_COMPANIONS_MAX];
    memset(table, 0, sizeof(table));

    const uint8_t a[6] = {1, 0, 0, 0, 0, 0};
    const uint8_t b[6] = {2, 0, 0, 0, 0, 0};
    const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};

    assert(!config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, a));

    // Registering is a change; re-registering the same peer is not, so a
    // companion reconnecting costs no flash write.
    assert(config_mgmt_companion_remember(table, CONFIG_MGMT_COMPANIONS_MAX, a, 0u));
    assert(!config_mgmt_companion_remember(table, CONFIG_MGMT_COMPANIONS_MAX, a, 0u));
    assert(config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, a));
    assert(!config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, b));

    // A zero address is not an identity: BTstack hands one out for unresolved
    // peers, and storing it would match every other unresolved peer.
    assert(!config_mgmt_companion_remember(table, CONFIG_MGMT_COMPANIONS_MAX, zero, 0u));
    assert(!config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, zero));

    // Forgetting removes exactly one, and only reports a change when it did.
    assert(config_mgmt_companion_forget(table, CONFIG_MGMT_COMPANIONS_MAX, a));
    assert(!config_mgmt_companion_forget(table, CONFIG_MGMT_COMPANIONS_MAX, a));
    assert(!config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, a));

    // Full table evicts the OLDEST, never the newest.
    for (unsigned i = 0; i < CONFIG_MGMT_COMPANIONS_MAX; ++i) {
        uint8_t addr[6] = {(uint8_t)(0x10u + i), 0, 0, 0, 0, 0};
        assert(config_mgmt_companion_remember(table, CONFIG_MGMT_COMPANIONS_MAX,
                                              addr, 0u));
    }
    const uint8_t oldest[6] = {0x10u, 0, 0, 0, 0, 0};
    const uint8_t newest[6] = {(uint8_t)(0x10u + CONFIG_MGMT_COMPANIONS_MAX - 1u),
                               0, 0, 0, 0, 0};
    const uint8_t extra[6] = {0x99u, 0, 0, 0, 0, 0};
    assert(config_mgmt_companion_remember(table, CONFIG_MGMT_COMPANIONS_MAX,
                                          extra, 0u));
    assert(!config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, oldest));
    assert(config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, newest));
    assert(config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, extra));
    puts("  management-companion membership");
}

static void test_mgmt_companions_round_trip(void) {
    config_record_t record;
    config_persist_defaults(&record);
    const uint8_t phone[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    assert(config_mgmt_companion_remember(record.mgmt_companions,
                                          CONFIG_MGMT_COMPANIONS_MAX, phone, 1u));

    uint8_t sector[512];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &record, sizeof(record));

    config_record_t again;
    assert(config_persist_load(sector, sizeof(sector), &again) ==
           CONFIG_PERSIST_CURRENT);
    assert(config_mgmt_companion_known(again.mgmt_companions,
                                       CONFIG_MGMT_COMPANIONS_MAX, phone));
    assert(again.mgmt_companions[0].addr_type == 1u);
    puts("  management companions survive a reboot");
}

int main(void) {
    puts("config persistence:");
    test_defaults();
    test_blank_and_foreign_records();
    test_v10_migration();
    test_v11_migration();
    test_v12_migration();
    test_mgmt_companion_membership();
    test_mgmt_companions_round_trip();
    test_anti_deadzone_persistence();
    test_mapping_round_trip();
    test_corrupt_mapping_fails_safe();
    puts("config persistence tests passed");
    return 0;
}
