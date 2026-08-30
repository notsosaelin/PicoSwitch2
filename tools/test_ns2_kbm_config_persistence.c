// Persisted settings record: schema migration, the KB/M profile library, and
// the realized active mapping.
//
// Host-only. Two failures this guards against:
//
//   1. a schema change silently reinterpreting an existing adapter's stored
//      bytes as different settings -- which a firmware build cannot catch,
//      because both layouts compile fine;
//   2. Save quietly behaving like Apply. The separation between the profile
//      LIBRARY and the REALIZED mapping is the whole point of schema 14, and it
//      has to be structural rather than a UI convention, so it is asserted here
//      at the model layer.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config_persist.h"

#define KEY_F 0x09u
#define KEY_E 0x08u
#define KEY_SPACE 0x2Cu

static ns2_kbm_source_t key(uint8_t usage) {
    ns2_kbm_source_t source = {NS2_KBM_SRC_KEY, usage};
    return source;
}

static ns2_kbm_source_t mouse_button(uint8_t number) {
    ns2_kbm_source_t source = {NS2_KBM_SRC_MOUSE, number};
    return source;
}

// The mapping a layout is actually resolving against.
static const ns2_kbm_content_t *realized(const config_record_t *record,
                                         ns2_kbm_layout_t layout) {
    return ns2_kbm_active_content(&record->kbm, layout);
}

static uint8_t realized_binding(const config_record_t *record,
                                ns2_kbm_layout_t layout,
                                ns2_kbm_source_t source) {
    return ns2_kbm_binding(realized(record, layout), layout, source);
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

    // No custom profiles, and both layouts realizing their built-in Default.
    // Default is a TEMPLATE: it consumes no slot, which is what leaves all six
    // for the user.
    for (unsigned i = 0; i < NS2_KBM_MAX_PROFILES; ++i)
        assert(!record.kbm.profiles[i].used);
    for (unsigned i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i) {
        assert(record.kbm.active[i].source_id == NS2_KBM_PROFILE_ID_DEFAULT);
        assert(record.kbm.active[i].content.overrides.count == 0);
        assert(record.kbm.active[i].content.mouse.sensitivity_x ==
               NS2_KBM_MOUSE_SENS_DEFAULT);
    }
    // A fresh adapter is by definition running exactly its Default.
    assert(ns2_kbm_active_matches_source(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD));
    puts("  defaults");
}

static void test_record_fits_its_programmed_region(void) {
    // The record is programmed as CONFIG_RECORD_BYTES (2048) inside a sector
    // that is erased whole either way. Widening further is a deliberate
    // decision, not something a struct edit should reach by accident.
    assert(sizeof(config_record_t) <= 2048u);
    printf("  record is %u of 2048 bytes\n", (unsigned)sizeof(config_record_t));
}

static void test_blank_and_foreign_records(void) {
    config_record_t record;
    uint8_t erased[2048];
    memset(erased, 0xFF, sizeof(erased));
    assert(config_persist_load(erased, sizeof(erased), &record) ==
           CONFIG_PERSIST_DEFAULTED);
    assert(record.version == CONFIG_PERSIST_VERSION);

    // A future schema. Reinterpreting its bytes would invent settings.
    config_record_v10_t future;
    make_v10(&future);
    future.version = 250u;
    uint8_t sector[2048];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &future, sizeof(future));
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_DEFAULTED);

    // Foreign magic.
    config_record_v10_t alien;
    make_v10(&alien);
    alien.magic = 0xDEADBEEFu;
    memcpy(sector, &alien, sizeof(alien));
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_DEFAULTED);

    assert(config_persist_load(NULL, 0, &record) == CONFIG_PERSIST_DEFAULTED);
    puts("  blank and foreign records");
}

static void test_v10_migration(void) {
    config_record_v10_t old;
    make_v10(&old);
    uint8_t sector[2048];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &old, sizeof(old));

    config_record_t record;
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_MIGRATED);
    assert(record.version == CONFIG_PERSIST_VERSION);
    assert(record.body_color[0] == 0x11);
    assert(record.wake_valid == 0xA5u);
    assert(record.wake_identity.product_id == 0x2069u);
    // v10 predates KB/M entirely, so both layouts realize Default.
    for (unsigned i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i)
        assert(record.kbm.active[i].source_id == NS2_KBM_PROFILE_ID_DEFAULT);
    puts("  schema 10 -> current migration");
}

// Fill a frozen v13 KB/M block with a customized mapping.
static void make_v13_kbm(ns2_kbm_config_v13_t *kbm, bool customize) {
    memset(kbm, 0, sizeof(*kbm));
    kbm->mode = (uint8_t)NS2_KBM_MODE_KEYBOARD_MOUSE;
    kbm->mouse.sensitivity_x = 777u;
    kbm->mouse.sensitivity_y = 777u;
    kbm->mouse.recenter_ms = 90u;
    kbm->mouse.anti_deadzone = 12u;
    if (!customize) return;
    kbm->profiles[NS2_KBM_LAYOUT_KEYBOARD].count = 1u;
    kbm->profiles[NS2_KBM_LAYOUT_KEYBOARD].entries[0].source = key(KEY_F);
    kbm->profiles[NS2_KBM_LAYOUT_KEYBOARD].entries[0].destination = NS2_DST_X;
    kbm->profiles[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].count = 1u;
    kbm->profiles[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].entries[0].source =
        mouse_button(3);
    kbm->profiles[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].entries[0].destination =
        NS2_DST_B;
}

static void test_v11_migration(void) {
    config_record_v11_t old;
    memset(&old, 0, sizeof(old));
    old.magic = CONFIG_PERSIST_MAGIC;
    old.version = 11u;
    old.body_color[0] = 0x11;
    old.wake_valid = 0xA5u;
    old.kbm.mode = (uint8_t)NS2_KBM_MODE_KEYBOARD_MOUSE;
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD].count = 1u;
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD].entries[0].source = key(KEY_F);
    old.kbm.profiles[NS2_KBM_LAYOUT_KEYBOARD].entries[0].destination = NS2_DST_X;
    old.kbm.mouse.sensitivity_x = 1024u;
    old.kbm.mouse.invert_y = 1u;

    uint8_t sector[2048];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &old, sizeof(old));

    config_record_t record;
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_MIGRATED);
    assert(record.version == CONFIG_PERSIST_VERSION);
    // Deliberate tripwire: bumping the schema must bring whoever did it here to
    // confirm every older layout still has a migration. v11 upgrades five
    // steps in one load.
    assert(CONFIG_PERSIST_VERSION == 16u);

    assert(record.body_color[0] == 0x11);
    assert(record.wake_valid == 0xA5u);
    assert(record.kbm.mode == (uint8_t)NS2_KBM_MODE_KEYBOARD_MOUSE);
    // The user's mapping survives and is what the console runs.
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    assert(realized(&record, NS2_KBM_LAYOUT_KEYBOARD)->mouse.sensitivity_x ==
           1024u);
    assert(realized(&record, NS2_KBM_LAYOUT_KEYBOARD)->mouse.invert_y == 1u);
    // Anti-deadzone did not exist in v11 and must arrive OFF, so upgrading
    // cannot change how an existing adapter already feels.
    assert(realized(&record, NS2_KBM_LAYOUT_KEYBOARD)->mouse.anti_deadzone == 0u);

    assert(config_persist_load(sector, sizeof(config_record_v11_t) - 1u,
                               &record) == CONFIG_PERSIST_DEFAULTED);
    puts("  schema 11 -> 14 migration");
}

static void test_v12_migration(void) {
    config_record_v12_t old;
    memset(&old, 0, sizeof(old));
    old.magic = CONFIG_PERSIST_MAGIC;
    old.version = 12u;
    old.body_color[0] = 0x71;
    old.wake_valid = 0x5Au;
    make_v13_kbm(&old.kbm, /*customize=*/true);

    uint8_t sector[2048];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &old, sizeof(old));

    config_record_t record;
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_MIGRATED);
    assert(record.body_color[0] == 0x71);
    assert(record.wake_valid == 0x5Au);
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    assert(realized(&record, NS2_KBM_LAYOUT_KEYBOARD)->mouse.anti_deadzone ==
           12u);
    // The companion table did not exist in v12 and must start EMPTY: inventing
    // membership from an existing bond would assert a role never observed.
    for (unsigned i = 0; i < CONFIG_MGMT_COMPANIONS_MAX; ++i)
        assert(!record.mgmt_companions[i].valid);

    assert(config_persist_load(sector, sizeof(config_record_v12_t) - 1u,
                               &record) == CONFIG_PERSIST_DEFAULTED);
    puts("  schema 12 -> 14 migration");
}

// The migration that matters most: a v13 adapter must upgrade with its
// management companions intact, its mapping intact, and its console behaviour
// unchanged.
static void test_v13_migration(void) {
    config_record_v13_t old;
    memset(&old, 0, sizeof(old));
    old.magic = CONFIG_PERSIST_MAGIC;
    old.version = 13u;
    old.body_color[0] = 0x61;
    old.wake_valid = 0x5Au;
    old.wake_identity.product_id = 0x2069u;
    make_v13_kbm(&old.kbm, /*customize=*/true);
    // Two remembered management companions -- the durable role evidence added
    // in v13 so an offline management bond stops appearing as a fake Paired
    // Controller.
    old.mgmt_companions[0].valid = 1u;
    old.mgmt_companions[0].addr_type = 1u;
    old.mgmt_companions[0].addr[0] = 0xC0;
    old.mgmt_companions[0].addr[5] = 0x01;
    old.mgmt_companions[1].valid = 1u;
    old.mgmt_companions[1].addr[0] = 0xC1;

    uint8_t sector[2048];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &old, sizeof(old));

    config_record_t record;
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_MIGRATED);

    // (1) Unrelated settings.
    assert(record.body_color[0] == 0x61);
    assert(record.wake_valid == 0x5Au);
    assert(record.wake_identity.product_id == 0x2069u);
    assert(record.kbm.mode == (uint8_t)NS2_KBM_MODE_KEYBOARD_MOUSE);

    // (2) THE MANAGEMENT COMPANION TABLE SURVIVES. Regressing this would bring
    // back fake Paired Controllers and let a front end be forgotten through the
    // wrong companion.
    assert(config_mgmt_companion_known(record.mgmt_companions,
                                       CONFIG_MGMT_COMPANIONS_MAX,
                                       old.mgmt_companions[0].addr));
    assert(config_mgmt_companion_known(record.mgmt_companions,
                                       CONFIG_MGMT_COMPANIONS_MAX,
                                       old.mgmt_companions[1].addr));
    assert(record.mgmt_companions[0].addr_type == 1u);

    // (3) Both mappings resolve exactly as before.
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                            mouse_button(3)) == NS2_DST_B);
    // ...including every binding the user did NOT change.
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_SPACE)) ==
           ns2_kbm_default_binding(NS2_KBM_LAYOUT_KEYBOARD, key(KEY_SPACE)));
    // ...and the mouse settings, which were global and are now profile-owned.
    for (unsigned i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i) {
        assert(record.kbm.active[i].content.mouse.sensitivity_x == 777u);
        assert(record.kbm.active[i].content.mouse.anti_deadzone == 12u);
    }

    // (4) The customized mapping was KEPT as a named profile the user can see,
    // and it is the one being realized.
    const ns2_kbm_profile_slot_t *kb = ns2_kbm_profile_find(
        &record.kbm, record.kbm.active[NS2_KBM_LAYOUT_KEYBOARD].source_id);
    assert(kb != NULL);
    assert(strcmp(kb->name, "Current Keyboard") == 0);
    assert(kb->layout == NS2_KBM_LAYOUT_KEYBOARD);
    assert(ns2_kbm_active_matches_source(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD));

    const ns2_kbm_profile_slot_t *kbm = ns2_kbm_profile_find(
        &record.kbm, record.kbm.active[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].source_id);
    assert(kbm != NULL);
    assert(strcmp(kbm->name, "Current KB + Mouse") == 0);

    // Four of the six custom slots remain free for the user.
    unsigned used = 0;
    for (unsigned i = 0; i < NS2_KBM_MAX_PROFILES; ++i)
        if (record.kbm.profiles[i].used) used++;
    assert(used == 2);

    assert(config_persist_load(sector, sizeof(config_record_v13_t) - 1u,
                               &record) == CONFIG_PERSIST_DEFAULTED);
    puts("  schema 13 -> 14 preserves companions, mappings and behaviour");
}

// An adapter whose mapping was never customized must NOT burn a custom slot.
static void test_v13_canonical_mapping_consumes_no_slot(void) {
    config_record_v13_t old;
    memset(&old, 0, sizeof(old));
    old.magic = CONFIG_PERSIST_MAGIC;
    old.version = 13u;
    // Default mouse settings and no overrides: exactly the canonical Default.
    ns2_kbm_config_t fresh;
    ns2_kbm_config_defaults(&fresh);
    old.kbm.mode = fresh.mode;
    old.kbm.mouse = fresh.active[0].content.mouse;

    uint8_t sector[2048];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &old, sizeof(old));

    config_record_t record;
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_MIGRATED);
    for (unsigned i = 0; i < NS2_KBM_MAX_PROFILES; ++i)
        assert(!record.kbm.profiles[i].used);
    for (unsigned i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i) {
        assert(record.kbm.active[i].source_id == NS2_KBM_PROFILE_ID_DEFAULT);
        assert(ns2_kbm_active_matches_source(&record.kbm, (ns2_kbm_layout_t)i));
    }
    puts("  an unmodified v13 mapping migrates to Default, consuming no slot");
}

static void test_profile_library(void) {
    config_record_t record;
    config_persist_defaults(&record);
    ns2_kbm_config_t *c = &record.kbm;

    uint8_t work = ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "Work",
                                          NULL);
    assert(work >= NS2_KBM_PROFILE_ID_FIRST);
    const ns2_kbm_profile_slot_t *slot = ns2_kbm_profile_find(c, work);
    assert(slot && slot->revision == 1u);
    // A new profile IS its layout's Default until something changes.
    assert(slot->content.overrides.count == 0);

    // Creating it does not change what the console is running.
    assert(c->active[NS2_KBM_LAYOUT_KEYBOARD].source_id ==
           NS2_KBM_PROFILE_ID_DEFAULT);

    // Names are unique within a layout, and free in the other one.
    assert(ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "Work", NULL) ==
           NS2_KBM_PROFILE_ID_NONE);
    assert(ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, "Work",
                                  NULL) != NS2_KBM_PROFILE_ID_NONE);

    // A name that sanitizes to nothing is refused rather than stored blank.
    assert(ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "\x01\x02",
                                  NULL) == NS2_KBM_PROFILE_ID_NONE);

    // THREE POSITIONS PER LAYOUT, not six in a flat pool.
    //
    // Six records is exactly three positions in each of two layouts. A layout
    // whose bank is full is full even while records remain free, because the
    // user addresses "Keyboard Profile 1..3", never a storage slot -- and
    // reporting a Keyboard refusal as "storage full" would be a fact they could
    // not act on.
    //
    // One profile exists in each layout at this point, both called "Work".
    assert(ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "KB2", NULL) !=
           NS2_KBM_PROFILE_ID_NONE);
    assert(ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "KB3", NULL) !=
           NS2_KBM_PROFILE_ID_NONE);
    assert(ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "KB4", NULL) ==
           NS2_KBM_PROFILE_ID_NONE);
    assert(ns2_kbm_free_position(c, NS2_KBM_LAYOUT_KEYBOARD) == 0u);

    // The other layout's bank is untouched by that and holds its own three.
    assert(ns2_kbm_free_position(c, NS2_KBM_LAYOUT_KEYBOARD_MOUSE) == 2u);
    assert(ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, "KBM2",
                                  NULL) != NS2_KBM_PROFILE_ID_NONE);
    assert(ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, "KBM3",
                                  NULL) != NS2_KBM_PROFILE_ID_NONE);
    assert(ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, "KBM4",
                                  NULL) == NS2_KBM_PROFILE_ID_NONE);

    // Every record is now spoken for: three positions in each bank.
    for (uint8_t p = 1u; p <= NS2_KBM_POSITIONS_PER_LAYOUT; ++p) {
        assert(ns2_kbm_profile_at(c, NS2_KBM_LAYOUT_KEYBOARD, p));
        assert(ns2_kbm_profile_at(c, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, p));
    }

    // Rename is metadata only and does not bump the revision, because no
    // draft's mapping content became stale.
    uint16_t before = ns2_kbm_profile_find(c, work)->revision;
    assert(ns2_kbm_profile_rename(c, work, "Work 2"));
    assert(ns2_kbm_profile_find(c, work)->revision == before);
    assert(strcmp(ns2_kbm_profile_find(c, work)->name, "Work 2") == 0);
    // ...and cannot collide within the layout.
    assert(!ns2_kbm_profile_rename(c, work, "KB2"));

    // Deleting frees a slot.
    assert(ns2_kbm_profile_delete(c, work));
    assert(ns2_kbm_profile_find(c, work) == NULL);
    assert(!ns2_kbm_profile_delete(c, work));
    assert(ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "Reuse", NULL) !=
           NS2_KBM_PROFILE_ID_NONE);
    puts("  profile library: create, name rules, rename, delete, capacity");
}

// A reused storage slot must not inherit the deleted profile's identity, or a
// companion's cached draft would silently address an unrelated mapping.
static void test_stable_ids_do_not_alias_reused_slots(void) {
    config_record_t record;
    config_persist_defaults(&record);
    ns2_kbm_config_t *c = &record.kbm;

    uint8_t first = ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "First",
                                           NULL);
    assert(ns2_kbm_profile_delete(c, first));
    uint8_t second = ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD,
                                            "Second", NULL);
    assert(second != first);
    // The stale id addresses nothing at all, which is the point.
    assert(ns2_kbm_profile_find(c, first) == NULL);
    puts("  stable ids do not alias a reused slot");
}

// The central contract: SAVE stores, APPLY realizes, and they are not the same
// operation even for the profile that is currently applied.
static void test_save_is_not_apply(void) {
    config_record_t record;
    config_persist_defaults(&record);
    ns2_kbm_config_t *c = &record.kbm;

    // Work revision 1, binding F -> X.
    ns2_kbm_content_t content;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    assert(ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    uint8_t work = ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "Work",
                                          &content);
    assert(work != NS2_KBM_PROFILE_ID_NONE);

    // Creating it changed nothing about the console.
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           ns2_kbm_default_binding(NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)));

    bool changed = false;
    assert(ns2_kbm_apply(c, NS2_KBM_LAYOUT_KEYBOARD, work, &changed));
    assert(changed);
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    assert(ns2_kbm_active_matches_source(c, NS2_KBM_LAYOUT_KEYBOARD));

    // Now edit and SAVE the applied profile: revision 2, E -> Y as well.
    ns2_kbm_content_t edited = content;
    assert(ns2_kbm_set_binding(&edited, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_E),
                               NS2_DST_Y));
    assert(ns2_kbm_profile_save(c, work, 1u, NULL, &edited) == 2u);

    // THE POINT: the console has not changed.
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_E)) !=
           NS2_DST_Y);
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    // ...and the divergence is DETECTABLE, so a client cannot claim "Active".
    assert(!ns2_kbm_active_matches_source(c, NS2_KBM_LAYOUT_KEYBOARD));

    // Apply realizes the saved revision, atomically.
    assert(ns2_kbm_apply(c, NS2_KBM_LAYOUT_KEYBOARD, work, &changed));
    assert(changed);
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_E)) ==
           NS2_DST_Y);
    assert(ns2_kbm_active_matches_source(c, NS2_KBM_LAYOUT_KEYBOARD));

    // Re-applying identical content reports no change, so the caller can skip
    // the flash write entirely.
    assert(ns2_kbm_apply(c, NS2_KBM_LAYOUT_KEYBOARD, work, &changed));
    assert(!changed);

    // The other layout was never touched.
    assert(c->active[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].source_id ==
           NS2_KBM_PROFILE_ID_DEFAULT);
    puts("  save stores, apply realizes, and they are different operations");
}

// A legacy per-binding write mutates the realized mapping directly, which its
// clients expect -- and must then be reported as no longer matching the saved
// profile rather than letting a companion keep claiming it is applied.
static void test_legacy_binding_creates_truthful_divergence(void) {
    config_record_t record;
    config_persist_defaults(&record);
    ns2_kbm_config_t *c = &record.kbm;

    uint8_t work = ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "Work",
                                          NULL);
    assert(ns2_kbm_apply(c, NS2_KBM_LAYOUT_KEYBOARD, work, NULL));
    assert(ns2_kbm_active_matches_source(c, NS2_KBM_LAYOUT_KEYBOARD));

    assert(ns2_kbm_set_binding(&c->active[NS2_KBM_LAYOUT_KEYBOARD].content,
                               NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F), NS2_DST_X));
    // The console changed immediately, as the legacy command promises.
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    // The saved profile did NOT, and the mismatch is visible.
    assert(ns2_kbm_binding(&ns2_kbm_profile_find(c, work)->content,
                           NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) != NS2_DST_X);
    assert(!ns2_kbm_active_matches_source(c, NS2_KBM_LAYOUT_KEYBOARD));
    puts("  a legacy bind diverges the realized mapping, truthfully");
}

static void test_save_conflict_and_layout_rules(void) {
    config_record_t record;
    config_persist_defaults(&record);
    ns2_kbm_config_t *c = &record.kbm;

    uint8_t work = ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "Work",
                                          NULL);
    ns2_kbm_content_t content;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    assert(ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F),
                               NS2_DST_X));

    // A draft built against revision 1 saves once...
    assert(ns2_kbm_profile_save(c, work, 1u, NULL, &content) == 2u);
    // ...and a second draft still claiming revision 1 is a CONFLICT, not a
    // silent overwrite of whatever the other companion just stored.
    ns2_kbm_content_t stale = content;
    assert(ns2_kbm_set_binding(&stale, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_E),
                               NS2_DST_Y));
    assert(ns2_kbm_profile_save(c, work, 1u, NULL, &stale) == 0u);
    // The rejected save changed nothing.
    assert(ns2_kbm_profile_find(c, work)->revision == 2u);
    assert(ns2_kbm_binding(&ns2_kbm_profile_find(c, work)->content,
                           NS2_KBM_LAYOUT_KEYBOARD, key(KEY_E)) != NS2_DST_Y);
    // Unknown ids are refused too.
    assert(ns2_kbm_profile_save(c, 200u, 1u, NULL, &content) == 0u);

    // A profile belongs to ONE layout and cannot be realized on the other:
    // every unoverridden key would resolve against the wrong canonical map.
    assert(!ns2_kbm_apply(c, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, work, NULL));
    assert(c->active[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].source_id ==
           NS2_KBM_PROFILE_ID_DEFAULT);
    puts("  stale revisions and wrong-layout applies are refused intact");
}

// Deleting the profile behind the realized mapping must leave the adapter
// running something that exists.
static void test_deleting_the_applied_profile_falls_back_to_default(void) {
    config_record_t record;
    config_persist_defaults(&record);
    ns2_kbm_config_t *c = &record.kbm;

    ns2_kbm_content_t content;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    assert(ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    uint8_t work = ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "Work",
                                          &content);
    assert(ns2_kbm_apply(c, NS2_KBM_LAYOUT_KEYBOARD, work, NULL));

    assert(ns2_kbm_profile_delete(c, work));
    assert(c->active[NS2_KBM_LAYOUT_KEYBOARD].source_id ==
           NS2_KBM_PROFILE_ID_DEFAULT);
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           ns2_kbm_default_binding(NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)));
    assert(ns2_kbm_active_matches_source(c, NS2_KBM_LAYOUT_KEYBOARD));
    puts("  deleting the applied profile realizes Default");
}

static void test_fingerprint(void) {
    ns2_kbm_content_t a, b;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &a);
    b = a;
    assert(ns2_kbm_content_fingerprint(&a, NS2_KBM_LAYOUT_KEYBOARD) ==
           ns2_kbm_content_fingerprint(&b, NS2_KBM_LAYOUT_KEYBOARD));

    // One binding difference moves it.
    assert(ns2_kbm_set_binding(&b, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    assert(ns2_kbm_content_fingerprint(&a, NS2_KBM_LAYOUT_KEYBOARD) !=
           ns2_kbm_content_fingerprint(&b, NS2_KBM_LAYOUT_KEYBOARD));

    // A profile-owned SETTING moves it too -- the mapping's behaviour is more
    // than its bindings.
    ns2_kbm_content_t c = a;
    c.mouse.sensitivity_x = (uint16_t)(a.mouse.sensitivity_x + 64u);
    assert(ns2_kbm_content_fingerprint(&a, NS2_KBM_LAYOUT_KEYBOARD) !=
           ns2_kbm_content_fingerprint(&c, NS2_KBM_LAYOUT_KEYBOARD));

    // Order of entry must NOT: two profiles that behave the same fingerprint
    // the same however they were built.
    ns2_kbm_content_t forward, backward;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &forward);
    backward = forward;
    assert(ns2_kbm_set_binding(&forward, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    assert(ns2_kbm_set_binding(&forward, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_E),
                               NS2_DST_Y));
    assert(ns2_kbm_set_binding(&backward, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_E),
                               NS2_DST_Y));
    assert(ns2_kbm_set_binding(&backward, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    assert(ns2_kbm_content_fingerprint(&forward, NS2_KBM_LAYOUT_KEYBOARD) ==
           ns2_kbm_content_fingerprint(&backward, NS2_KBM_LAYOUT_KEYBOARD));

    // The layout is part of the digest, because identical overrides mean
    // different mappings over different canonical tables.
    assert(ns2_kbm_content_fingerprint(&a, NS2_KBM_LAYOUT_KEYBOARD) !=
           ns2_kbm_content_fingerprint(&a, NS2_KBM_LAYOUT_KEYBOARD_MOUSE));

    // An override that merely restates the default is not a difference.
    ns2_kbm_content_t redundant;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &redundant);
    redundant.overrides.count = 1u;
    redundant.overrides.entries[0].source = key(KEY_SPACE);
    redundant.overrides.entries[0].destination =
        ns2_kbm_default_binding(NS2_KBM_LAYOUT_KEYBOARD, key(KEY_SPACE));
    assert(ns2_kbm_content_fingerprint(&redundant, NS2_KBM_LAYOUT_KEYBOARD) ==
           ns2_kbm_content_fingerprint(&a, NS2_KBM_LAYOUT_KEYBOARD));

    // Pinned values, so the C#, Kotlin and C implementations can be checked
    // against one another rather than against each other's opinions.
    printf("  fingerprint: default kb=%lu kbm=%lu\n",
           (unsigned long)ns2_kbm_content_fingerprint(&a,
                                                      NS2_KBM_LAYOUT_KEYBOARD),
           (unsigned long)ns2_kbm_content_fingerprint(
               &a, NS2_KBM_LAYOUT_KEYBOARD_MOUSE));
}

static void test_mgmt_companion_membership(void) {
    config_mgmt_companion_t table[CONFIG_MGMT_COMPANIONS_MAX];
    memset(table, 0, sizeof(table));

    const uint8_t a[6] = {1, 0, 0, 0, 0, 0};
    const uint8_t b[6] = {2, 0, 0, 0, 0, 0};
    const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};

    assert(!config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, a));
    assert(config_mgmt_companion_remember(table, CONFIG_MGMT_COMPANIONS_MAX, a,
                                          0u));
    // Re-registering is not a change, so a companion reconnecting costs no
    // flash write.
    assert(!config_mgmt_companion_remember(table, CONFIG_MGMT_COMPANIONS_MAX, a,
                                           0u));
    assert(config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, a));
    assert(!config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX, b));

    // A zero address is not an identity: BTstack hands one out for unresolved
    // peers, and storing it would match every other unresolved peer.
    assert(!config_mgmt_companion_remember(table, CONFIG_MGMT_COMPANIONS_MAX,
                                           zero, 0u));

    assert(config_mgmt_companion_forget(table, CONFIG_MGMT_COMPANIONS_MAX, a));
    assert(!config_mgmt_companion_forget(table, CONFIG_MGMT_COMPANIONS_MAX, a));

    // A full table evicts the OLDEST, never the newest.
    for (unsigned i = 0; i < CONFIG_MGMT_COMPANIONS_MAX; ++i) {
        uint8_t addr[6] = {(uint8_t)(0x10u + i), 0, 0, 0, 0, 0};
        assert(config_mgmt_companion_remember(table, CONFIG_MGMT_COMPANIONS_MAX,
                                              addr, 0u));
    }
    const uint8_t oldest[6] = {0x10u, 0, 0, 0, 0, 0};
    const uint8_t newest[6] = {
        (uint8_t)(0x10u + CONFIG_MGMT_COMPANIONS_MAX - 1u), 0, 0, 0, 0, 0};
    const uint8_t extra[6] = {0x99u, 0, 0, 0, 0, 0};
    assert(config_mgmt_companion_remember(table, CONFIG_MGMT_COMPANIONS_MAX,
                                          extra, 0u));
    assert(!config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX,
                                        oldest));
    assert(config_mgmt_companion_known(table, CONFIG_MGMT_COMPANIONS_MAX,
                                       newest));
    puts("  management-companion membership");
}

static void test_round_trip(void) {
    config_record_t record;
    config_persist_defaults(&record);

    // A companion, a custom profile, and an applied mapping.
    const uint8_t phone[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    assert(config_mgmt_companion_remember(record.mgmt_companions,
                                          CONFIG_MGMT_COMPANIONS_MAX, phone,
                                          1u));
    ns2_kbm_content_t content;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    assert(ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    content.mouse.sensitivity_x = 900u;
    uint8_t work = ns2_kbm_profile_create(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD,
                                          "Work", &content);
    // SET BOOT, not merely apply. Since v15 those are different acts: apply is
    // the runtime change an app's Activate and a profile-switch key both make,
    // and it deliberately does not survive a power cycle. Only an explicit boot
    // choice does -- otherwise whichever profile a hotkey happened to select
    // would become the boot profile the next time anything was saved.
    // Position 1 of the Keyboard bank, which is where "Work" landed.
    assert(ns2_kbm_set_boot_position(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD, 1u,
                                     NULL));

    uint8_t sector[2048];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &record, sizeof(record));

    config_record_t again;
    assert(config_persist_load(sector, sizeof(sector), &again) ==
           CONFIG_PERSIST_CURRENT);
    assert(config_mgmt_companion_known(again.mgmt_companions,
                                       CONFIG_MGMT_COMPANIONS_MAX, phone));
    const ns2_kbm_profile_slot_t *slot = ns2_kbm_profile_find(&again.kbm, work);
    assert(slot && strcmp(slot->name, "Work") == 0);
    // A reboot preserves the REALIZED mapping, which is what the console runs.
    assert(realized_binding(&again, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);
    assert(realized(&again, NS2_KBM_LAYOUT_KEYBOARD)->mouse.sensitivity_x ==
           900u);
    assert(ns2_kbm_active_matches_source(&again.kbm, NS2_KBM_LAYOUT_KEYBOARD));
    puts("  profiles, active mapping and companions survive a reboot");
}

static void test_sanitize_rejects_malformed_state(void) {
    // Sanitize rejects malformed or impossible state. It is NOT torn-write
    // detection: the record is single-bank, erase-then-program and carries no
    // CRC, so a power loss during the final programming remains an existing
    // durability limitation. What it guarantees is that whatever is read back
    // produces a usable mapping or a safe fallback, never an invented binding.
    config_record_t record;
    config_persist_defaults(&record);
    ns2_kbm_config_t *c = &record.kbm;

    uint8_t work = ns2_kbm_profile_create(c, NS2_KBM_LAYOUT_KEYBOARD, "Work",
                                          NULL);
    assert(ns2_kbm_apply(c, NS2_KBM_LAYOUT_KEYBOARD, work, NULL));

    // A profile claiming a layout that does not exist.
    c->profiles[3].used = 1u;
    c->profiles[3].layout = 9u;
    c->profiles[3].profile_id = 40u;
    c->profiles[3].revision = 1u;
    // Two profiles sharing one id, which would let a cached draft address the
    // wrong mapping.
    c->profiles[4].used = 1u;
    c->profiles[4].layout = 0u;
    c->profiles[4].profile_id = work;
    c->profiles[4].revision = 1u;
    // A live profile carrying the reserved revision-0 sentinel.
    c->profiles[5].used = 1u;
    c->profiles[5].layout = 0u;
    c->profiles[5].profile_id = 41u;
    c->profiles[5].revision = 0u;
    // An override table longer than the table.
    c->active[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].content.overrides.count = 200u;
    // An out-of-range profile-owned setting.
    c->active[NS2_KBM_LAYOUT_KEYBOARD].content.mouse.anti_deadzone = 250u;

    assert(!ns2_kbm_config_sanitize(c));

    assert(ns2_kbm_profile_find(c, 40u) == NULL);  // unreadable layout dropped
    assert(!c->profiles[4].used);                  // duplicate id dropped
    assert(ns2_kbm_profile_find(c, 41u) != NULL &&
           ns2_kbm_profile_find(c, 41u)->revision != 0u);
    assert(c->active[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].content.overrides.count ==
           0);
    assert(c->active[NS2_KBM_LAYOUT_KEYBOARD].content.mouse.anti_deadzone ==
           NS2_KBM_MOUSE_ADZ_DEFAULT);
    // The surviving profile is untouched.
    assert(ns2_kbm_profile_find(c, work) != NULL);

    // An active snapshot naming a source that no longer exists KEEPS its
    // content -- the user's console behaviour does not change because a profile
    // vanished -- and is simply re-labelled as matching nothing saved.
    config_record_t orphan;
    config_persist_defaults(&orphan);
    orphan.kbm.active[NS2_KBM_LAYOUT_KEYBOARD].source_id = 77u;
    assert(!ns2_kbm_config_sanitize(&orphan.kbm));
    assert(orphan.kbm.active[NS2_KBM_LAYOUT_KEYBOARD].source_id ==
           NS2_KBM_PROFILE_ID_NONE);
    assert(!ns2_kbm_active_matches_source(&orphan.kbm,
                                          NS2_KBM_LAYOUT_KEYBOARD));
    puts("  sanitize rejects malformed profile state");
}

static void test_arbitrary_bytes_are_survivable(void) {
    // Every byte pattern must sanitize into something usable, because the
    // record has no integrity check to tell a good one from a bad one.
    static uint8_t noise[sizeof(config_record_t)];
    for (unsigned i = 0; i < sizeof(noise); ++i)
        noise[i] = (uint8_t)(i * 37u + 11u);

    config_record_t record;
    memcpy(&record, noise, sizeof(record));
    (void)ns2_kbm_config_sanitize(&record.kbm);

    assert(record.kbm.mode < NS2_KBM_MODE_COUNT);
    for (unsigned i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i) {
        const ns2_kbm_content_t *content = &record.kbm.active[i].content;
        assert(content->overrides.count <= NS2_KBM_MAX_OVERRIDES);
        for (uint8_t j = 0; j < content->overrides.count; ++j) {
            assert(ns2_kbm_source_valid(content->overrides.entries[j].source));
            assert(ns2_kbm_destination_valid(
                content->overrides.entries[j].destination));
        }
        assert(content->mouse.anti_deadzone <= NS2_KBM_MOUSE_ADZ_MAX);
    }
    for (unsigned i = 0; i < NS2_KBM_MAX_PROFILES; ++i) {
        if (!record.kbm.profiles[i].used) continue;
        assert(record.kbm.profiles[i].layout < NS2_KBM_LAYOUT_COUNT);
        assert(record.kbm.profiles[i].profile_id >= NS2_KBM_PROFILE_ID_FIRST);
        assert(record.kbm.profiles[i].revision != 0u);
        assert(record.kbm.profiles[i].content.overrides.count <=
               NS2_KBM_MAX_OVERRIDES);
    }
    puts("  arbitrary bytes sanitize into a usable configuration");
}

// v14 -> v15. The adapter has ALREADY RUN v14 during development, so this is a
// real installation being upgraded, not a hypothetical one.
//
// v15 adds the persisted boot-active slot and the profile-switch key table. The
// six stored profiles are reinterpreted as the adapter's RESIDENT SLOTS -- a
// change of meaning in the documentation, not of bytes -- so nothing about them
// may move, and the console must behave identically after the upgrade.
static void test_v14_migration(void) {
    config_record_v14_t old;
    memset(&old, 0, sizeof(old));
    old.magic = CONFIG_PERSIST_MAGIC;
    old.version = 14u;
    old.body_color[0] = 0x77;
    old.wake_valid = 0x5Au;
    old.wake_identity.product_id = 0x2069u;
    old.mgmt_companions[0].valid = 1u;
    old.mgmt_companions[0].addr[0] = 0xC0;

    // A v14 library: two resident profiles, and the Keyboard layout realizing
    // the custom one rather than Default.
    ns2_kbm_config_t seed;
    ns2_kbm_config_defaults(&seed);
    ns2_kbm_content_t work;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &work);
    assert(ns2_kbm_set_binding(&work, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F),
                               NS2_DST_X));
    uint8_t work_id = ns2_kbm_profile_create(&seed, NS2_KBM_LAYOUT_KEYBOARD,
                                             "Work", &work);
    assert(work_id != NS2_KBM_PROFILE_ID_NONE);
    ns2_kbm_content_t desk;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD_MOUSE, &desk);
    uint8_t desk_id = ns2_kbm_profile_create(&seed, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                                             "Desktop", &desk);
    assert(desk_id != NS2_KBM_PROFILE_ID_NONE);
    bool changed = false;
    assert(ns2_kbm_apply(&seed, NS2_KBM_LAYOUT_KEYBOARD, work_id, &changed));

    // Copy the v14 prefix of that config into the frozen v14 container.
    old.kbm.mode = seed.mode;
    old.kbm.next_profile_id = seed.next_profile_id;
    memcpy(old.kbm.profiles, seed.profiles, sizeof(old.kbm.profiles));
    memcpy(old.kbm.active, seed.active, sizeof(old.kbm.active));

    uint8_t sector[2048];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &old, sizeof(old));

    config_record_t record;
    assert(config_persist_load(sector, sizeof(sector), &record) ==
           CONFIG_PERSIST_MIGRATED);
    assert(record.version == 16u);

    // (1) Unrelated settings and the companion table survive.
    assert(record.body_color[0] == 0x77);
    assert(record.wake_identity.product_id == 0x2069u);
    assert(config_mgmt_companion_known(record.mgmt_companions,
                                       CONFIG_MGMT_COMPANIONS_MAX,
                                       old.mgmt_companions[0].addr));

    // (2) EVERY RESIDENT SLOT SURVIVES, with its id, name, layout and mapping.
    const ns2_kbm_profile_slot_t *work_slot =
        ns2_kbm_profile_find(&record.kbm, work_id);
    assert(work_slot != NULL);
    assert(strcmp(work_slot->name, "Work") == 0);
    assert(work_slot->layout == (uint8_t)NS2_KBM_LAYOUT_KEYBOARD);
    assert(ns2_kbm_binding(&work_slot->content, NS2_KBM_LAYOUT_KEYBOARD,
                           key(KEY_F)) == NS2_DST_X);
    const ns2_kbm_profile_slot_t *desk_slot =
        ns2_kbm_profile_find(&record.kbm, desk_id);
    assert(desk_slot != NULL);
    assert(strcmp(desk_slot->name, "Desktop") == 0);

    // (3) THE CONSOLE BEHAVES IDENTICALLY. The boot choice takes the profile the
    // layout was ALREADY realizing, so a firmware update does not silently
    // change what the adapter does at power-up.
    // Positions are assigned deterministically, in slot order within each
    // layout, and a profile is never moved between layouts.
    assert(work_slot->position == 1u);
    assert(desk_slot->position == 1u);  // first of ITS OWN layout's bank
    assert(record.kbm.boot_position[NS2_KBM_LAYOUT_KEYBOARD] == 1u);
    assert(record.kbm.boot_position[NS2_KBM_LAYOUT_KEYBOARD_MOUSE] ==
           NS2_KBM_POSITION_DEFAULT);
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_X);

    // (4) No switch key is invented. Reserving F1..F4 on an upgrade would
    // silently steal keys out of the user's existing mapping.
    for (unsigned i = 0; i < NS2_KBM_SWITCH_BINDINGS_MAX; ++i)
        assert(record.kbm.switches[i].used == 0u);
}

// Boot-active and runtime-active are different facts, and only one is persisted.
static void test_boot_active_is_what_power_up_realizes(void) {
    config_record_t record;
    config_persist_defaults(&record);
    ns2_kbm_content_t content;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    assert(ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F),
                               NS2_DST_Y));
    uint8_t id = ns2_kbm_profile_create(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD,
                                        "Halo", &content);
    assert(id != NS2_KBM_PROFILE_ID_NONE);

    // A hotkey switch changes the RUNTIME snapshot only; the persisted boot
    // choice stays Default. Simulated here by applying without touching
    // boot_position, which is exactly what the runtime does.
    bool changed = false;
    assert(ns2_kbm_apply(&record.kbm, NS2_KBM_LAYOUT_KEYBOARD, id, &changed));
    assert(realized_binding(&record, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_Y);
    assert(record.kbm.boot_position[NS2_KBM_LAYOUT_KEYBOARD] ==
           NS2_KBM_POSITION_DEFAULT);

    // Now persist and reload: power-up follows the BOOT choice, not the runtime
    // one. Without this, an unrelated save would silently promote whichever
    // profile a switch key happened to select into the boot profile.
    uint8_t sector[2048];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &record, sizeof(record));
    config_record_t reloaded;
    assert(config_persist_load(sector, sizeof(sector), &reloaded) !=
           CONFIG_PERSIST_DEFAULTED);
    assert(reloaded.kbm.active[NS2_KBM_LAYOUT_KEYBOARD].source_id ==
           NS2_KBM_PROFILE_ID_DEFAULT);
    assert(realized_binding(&reloaded, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           ns2_kbm_default_binding(NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)));

    // Setting the boot POSITION explicitly is what makes it survive.
    assert(ns2_kbm_set_boot_position(&reloaded.kbm, NS2_KBM_LAYOUT_KEYBOARD, 1u,
                                     NULL));
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &reloaded, sizeof(reloaded));
    config_record_t again;
    assert(config_persist_load(sector, sizeof(sector), &again) !=
           CONFIG_PERSIST_DEFAULTED);
    assert(realized_binding(&again, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F)) ==
           NS2_DST_Y);
}

int main(void) {
    puts("config persistence:");
    test_defaults();
    test_record_fits_its_programmed_region();
    test_blank_and_foreign_records();
    test_v10_migration();
    test_v11_migration();
    test_v12_migration();
    test_v13_migration();
    test_v14_migration();
    test_boot_active_is_what_power_up_realizes();
    test_v13_canonical_mapping_consumes_no_slot();
    test_profile_library();
    test_stable_ids_do_not_alias_reused_slots();
    test_save_is_not_apply();
    test_legacy_binding_creates_truthful_divergence();
    test_save_conflict_and_layout_rules();
    test_deleting_the_applied_profile_falls_back_to_default();
    test_fingerprint();
    test_mgmt_companion_membership();
    test_round_trip();
    test_sanitize_rejects_malformed_state();
    test_arbitrary_bytes_are_survivable();
    puts("config persistence tests passed");
    return 0;
}
