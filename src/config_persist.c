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

// Schema 12 extends the KB/M block's mouse settings, so every v11 field that
// precedes it must still sit where v11 left it -- including inside the block.
_Static_assert(offsetof(config_record_t, kbm) ==
                   offsetof(config_record_v11_t, kbm),
               "v12 must keep v11's kbm offset");
_Static_assert(offsetof(ns2_kbm_config_v13_t, profiles) ==
                   offsetof(ns2_kbm_config_v11_t, profiles),
               "v12 must keep v11's profile-table offset");
_Static_assert(offsetof(ns2_kbm_config_v13_t, mouse) ==
                   offsetof(ns2_kbm_config_v11_t, mouse),
               "v12 must keep v11's mouse-block offset");
_Static_assert(sizeof(((ns2_kbm_config_v13_t *)0)->profiles) ==
                   sizeof(((ns2_kbm_config_v11_t *)0)->profiles),
               "v11 and v12/v13 share the override table; a resize would "
               "redefine what stored bytes mean for both");
// The frozen v11 mouse block is 8 bytes and must stay that way; the live one
// grew to 10. Pinning both is what makes the field-by-field migration below
// provably a migration rather than a reinterpretation.
_Static_assert(sizeof(ns2_kbm_mouse_config_v11_t) == 8u,
               "v11 mouse block is frozen at 8 bytes");
_Static_assert(sizeof(ns2_kbm_mouse_config_t) == 10u,
               "v12 mouse block is 10 bytes; update the migration if it changes");
// NOT a strict size increase, and that is the point. v11's record ended with 2
// bytes of trailing alignment padding, and v12's two new mouse bytes land
// exactly in them: both records are the same length. So a stored v11 record is
// indistinguishable from a v12 one BY SIZE, and only the version field
// separates them. Anything that keyed migration off the stored length would
// read an old adapter's padding as a live anti-deadzone setting -- which is
// precisely why this got a version bump instead of a spare-byte claim.
_Static_assert(sizeof(config_record_t) >= sizeof(config_record_v11_t),
               "v12 must not shrink v11");
_Static_assert(offsetof(config_record_v12_t, kbm) +
                       sizeof(ns2_kbm_config_v13_t) <=
                   sizeof(config_record_v12_t),
               "the v12 KB/M block must fit inside the record it is stored in");

// Schema 14 replaces the KB/M container entirely: one override set per layout
// becomes a library of six NAMED CUSTOM profiles plus a separately stored
// REALIZED mapping per layout. The override table inside them is byte-identical
// to what v13 stored per layout, which is what makes the migration below a
// field-by-field move rather than a reinterpretation.
_Static_assert(sizeof(((ns2_kbm_content_t *)0)->overrides) ==
                   sizeof(((ns2_kbm_config_v13_t *)0)->profiles[0]),
               "v14 content must carry v13's override table unchanged");
_Static_assert(sizeof(((ns2_kbm_content_t *)0)->mouse) ==
                   sizeof(((ns2_kbm_config_v13_t *)0)->mouse),
               "v14 content must carry v13's mouse block unchanged");
_Static_assert(sizeof(config_record_t) > sizeof(config_record_v13_t),
               "v14 appends to v13");

// v15 appends the boot-active slot and the switch table to v14's KB/M block.
// Every v14 field keeps its offset, which is what makes the migration a copy.
_Static_assert(offsetof(ns2_kbm_config_t, profiles) ==
                   offsetof(ns2_kbm_config_v14_t, profiles),
               "v15 must not move v14's profile library");
_Static_assert(offsetof(ns2_kbm_config_t, active) ==
                   offsetof(ns2_kbm_config_v14_t, active),
               "v15 must not move v14's realized snapshots");
_Static_assert(sizeof(ns2_kbm_config_t) > sizeof(ns2_kbm_config_v14_t),
               "v15 appends to v14");
_Static_assert(sizeof(config_record_t) > sizeof(config_record_v14_t),
               "v15 appends to v14's record");
// The whole feature has to fit the widened record. If this ever fails, compact
// the representation before widening further -- the sector is 4096 but the
// programmed region is deliberately the smallest thing that holds the model.
_Static_assert(sizeof(config_record_t) <= 2048u,
               "the settings record must fit CONFIG_RECORD_BYTES (2048)");

// Schema 13 appends the management-companion table. Unlike v12 this IS a strict
// size increase -- v12's record had no trailing room left -- so a stored v12
// record is shorter, and the version field still decides, never the length.
_Static_assert(offsetof(config_record_t, kbm) ==
                   offsetof(config_record_v12_t, kbm),
               "v13 must keep v12's kbm offset");
_Static_assert(offsetof(config_record_t, wake_identity) ==
                   offsetof(config_record_v12_t, wake_identity),
               "v13 must keep v12's wake_identity offset");
_Static_assert(offsetof(config_record_t, mgmt_companions) >=
                   sizeof(config_record_v12_t),
               "v13's additions must not overlap any v12 field");
_Static_assert(sizeof(config_record_t) > sizeof(config_record_v12_t),
               "v13 appends to v12");

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
    // No remembered companions. An adapter that has never held a management
    // session has no role evidence, and zero is that statement.
    memset(out->mgmt_companions, 0, sizeof(out->mgmt_companions));
}

// A zeroed address is not an identity. BTstack hands out all-zero addresses for
// unresolved or absent peers, and remembering one would match every other
// unresolved peer for the life of the table.
static bool companion_addr_usable(const uint8_t addr[6]) {
    if (!addr) return false;
    for (unsigned i = 0; i < 6u; ++i)
        if (addr[i] != 0u) return true;
    return false;
}

static int companion_slot(const config_mgmt_companion_t *table, uint8_t capacity,
                          const uint8_t addr[6]) {
    for (uint8_t i = 0; i < capacity; ++i) {
        if (!table[i].valid) continue;
        if (memcmp(table[i].addr, addr, 6u) == 0) return (int)i;
    }
    return -1;
}

bool config_mgmt_companion_remember(config_mgmt_companion_t *table,
                                    uint8_t capacity, const uint8_t addr[6],
                                    uint8_t addr_type) {
    if (!table || capacity == 0u || !companion_addr_usable(addr)) return false;

    int slot = companion_slot(table, capacity, addr);
    if (slot >= 0) {
        // Already known. Only an address-type correction is a change worth a
        // flash write.
        if (table[slot].addr_type == addr_type) return false;
        table[slot].addr_type = addr_type;
        return true;
    }

    for (uint8_t i = 0; i < capacity; ++i) {
        if (table[i].valid) continue;
        table[i].valid = 1u;
        table[i].addr_type = addr_type;
        memcpy(table[i].addr, addr, 6u);
        return true;
    }

    // Full: evict the oldest by shifting down, so slot order IS recency and no
    // separate timestamp has to be stored or kept consistent.
    for (uint8_t i = 1; i < capacity; ++i) table[i - 1u] = table[i];
    table[capacity - 1u].valid = 1u;
    table[capacity - 1u].addr_type = addr_type;
    memcpy(table[capacity - 1u].addr, addr, 6u);
    return true;
}

bool config_mgmt_companion_known(const config_mgmt_companion_t *table,
                                 uint8_t capacity, const uint8_t addr[6]) {
    if (!table || capacity == 0u || !companion_addr_usable(addr)) return false;
    return companion_slot(table, capacity, addr) >= 0;
}

bool config_mgmt_companion_forget(config_mgmt_companion_t *table,
                                  uint8_t capacity, const uint8_t addr[6]) {
    if (!table || capacity == 0u || !companion_addr_usable(addr)) return false;
    int slot = companion_slot(table, capacity, addr);
    if (slot < 0) return false;
    // Close the gap so recency order survives a removal.
    for (uint8_t i = (uint8_t)slot + 1u; i < capacity; ++i) table[i - 1u] = table[i];
    memset(&table[capacity - 1u], 0, sizeof(table[capacity - 1u]));
    return true;
}

// Lift a v12/v13 KB/M block into the v14 library + realized-mapping model.
//
// The migration's whole job is that an upgraded adapter resolves EXACTLY the
// bindings it resolved before, and that no user work is thrown away. Two cases,
// decided per layout by what the user actually had:
//
//   the stored mapping is the canonical default
//       -> realize the built-in Default template. No custom slot is consumed,
//          because there is nothing of the user's to keep.
//
//   the stored mapping differs
//       -> keep it as a NAMED CUSTOM PROFILE and realize it. The user's mapping
//          survives, gains a name they can see, and is immediately the one in
//          use -- so nothing about their console changes on upgrade.
//
// Mouse settings were global in v13 and are profile-owned in v14, so they are
// copied into BOTH layouts. That preserves behaviour exactly: whichever layout
// the adapter resolves, it finds the settings the user had.
static void migrate_kbm_from_v13(const ns2_kbm_config_v13_t *old,
                                 ns2_kbm_config_t *out) {
    static const char *const MIGRATED_NAME[NS2_KBM_LAYOUT_COUNT] = {
        "Current Keyboard",
        "Current KB + Mouse",
    };

    ns2_kbm_config_defaults(out);
    out->mode = old->mode;

    for (uint8_t i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i) {
        ns2_kbm_layout_t layout = (ns2_kbm_layout_t)i;

        ns2_kbm_content_t content;
        memset(&content, 0, sizeof(content));
        content.overrides = old->profiles[i];
        content.mouse = old->mouse;
        ns2_kbm_content_canonicalize(&content, layout);

        ns2_kbm_content_t canonical;
        ns2_kbm_template_default(layout, &canonical);
        ns2_kbm_content_canonicalize(&canonical, layout);

        if (memcmp(&content, &canonical, sizeof(content)) == 0) {
            // Identical to Default in every profile-owned respect. Realizing
            // the template is the same mapping and costs no slot.
            (void)ns2_kbm_apply(out, layout, NS2_KBM_PROFILE_ID_DEFAULT, NULL);
            continue;
        }

        uint8_t id = ns2_kbm_profile_create(out, layout, MIGRATED_NAME[i],
                                            &content);
        if (id == NS2_KBM_PROFILE_ID_NONE) {
            // Cannot happen with six free slots and two layouts, but a silent
            // fallback to Default would discard the user's mapping. Realize the
            // content directly instead: behaviour is preserved even if the
            // library row could not be created.
            out->active[i].source_id = (uint8_t)NS2_KBM_PROFILE_ID_NONE;
            out->active[i].source_revision = 0u;
            out->active[i].content = content;
            continue;
        }
        (void)ns2_kbm_apply(out, layout, id, NULL);
    }
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
    } else if (header.version == 11u) {
        if (stored_len < sizeof(config_record_v11_t))
            return CONFIG_PERSIST_DEFAULTED;
        config_record_v11_t v11;
        memcpy(&v11, stored, sizeof(v11));
        // v11 keeps v10's header layout, so those fields come from the prefix
        // already read.
        out->body_color[0] = header.body_color[0];
        out->body_color[1] = header.body_color[1];
        out->body_color[2] = header.body_color[2];
        memcpy(out->joycon2_left_accent, header.joycon2_left_accent,
               sizeof(out->joycon2_left_accent));
        memcpy(out->joycon2_right_accent, header.joycon2_right_accent,
               sizeof(out->joycon2_right_accent));
        out->wake_valid = header.wake_valid;
        out->wake_identity = header.wake_identity;
        // The KB/M block is rebuilt FIELD BY FIELD, not copied as a blob: v12
        // resized the mouse settings, so a memcpy would land the old bytes on
        // the new layout and invent a setting out of the user's inversion flags.
        //
        // Lifted to v13's shape first, then through the one v13 -> v14
        // migration, so a v11 adapter and a v13 adapter with the same mapping
        // reach byte-identical v14 state by the same code path.
        ns2_kbm_config_v13_t lifted;
        memset(&lifted, 0, sizeof(lifted));
        lifted.mode = v11.kbm.mode;
        for (uint8_t i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i)
            lifted.profiles[i] = v11.kbm.profiles[i];
        lifted.mouse.sensitivity_x = v11.kbm.mouse.sensitivity_x;
        lifted.mouse.sensitivity_y = v11.kbm.mouse.sensitivity_y;
        lifted.mouse.recenter_ms = v11.kbm.mouse.recenter_ms;
        lifted.mouse.invert_x = v11.kbm.mouse.invert_x;
        lifted.mouse.invert_y = v11.kbm.mouse.invert_y;
        // The one genuinely new setting. OFF, so upgrading an existing adapter
        // cannot change how it already feels: anti-deadzone 0 is exactly the
        // hardware-validated linear response.
        lifted.mouse.anti_deadzone = (uint8_t)NS2_KBM_MOUSE_ADZ_DEFAULT;
        migrate_kbm_from_v13(&lifted, &out->kbm);
        result = CONFIG_PERSIST_MIGRATED;
    } else if (header.version == 12u) {
        if (stored_len < sizeof(config_record_v12_t))
            return CONFIG_PERSIST_DEFAULTED;
        config_record_v12_t v12;
        memcpy(&v12, stored, sizeof(v12));
        out->body_color[0] = header.body_color[0];
        out->body_color[1] = header.body_color[1];
        out->body_color[2] = header.body_color[2];
        memcpy(out->joycon2_left_accent, header.joycon2_left_accent,
               sizeof(out->joycon2_left_accent));
        memcpy(out->joycon2_right_accent, header.joycon2_right_accent,
               sizeof(out->joycon2_right_accent));
        out->wake_valid = header.wake_valid;
        out->wake_identity = header.wake_identity;
        migrate_kbm_from_v13(&v12.kbm, &out->kbm);
        // The companion table starts EMPTY on an upgraded adapter, and must.
        // Inventing membership from an existing bond would do exactly what this
        // field exists to stop: assert a role the adapter never observed. Each
        // companion re-registers itself on its next authenticated session.
        memset(out->mgmt_companions, 0, sizeof(out->mgmt_companions));
        result = CONFIG_PERSIST_MIGRATED;
    } else if (header.version == 14u) {
        // v14 -> v15. The six stored profiles ARE the adapter's resident slots;
        // nothing about them changes. What is added is the persisted boot-active
        // slot and the profile-switch key table.
        if (stored_len < sizeof(config_record_v14_t))
            return CONFIG_PERSIST_DEFAULTED;
        config_record_v14_t v14;
        memcpy(&v14, stored, sizeof(v14));
        out->body_color[0] = header.body_color[0];
        out->body_color[1] = header.body_color[1];
        out->body_color[2] = header.body_color[2];
        memcpy(out->joycon2_left_accent, header.joycon2_left_accent,
               sizeof(out->joycon2_left_accent));
        memcpy(out->joycon2_right_accent, header.joycon2_right_accent,
               sizeof(out->joycon2_right_accent));
        out->wake_valid = header.wake_valid;
        out->wake_identity = header.wake_identity;
        memcpy(out->mgmt_companions, v14.mgmt_companions,
               sizeof(out->mgmt_companions));

        // The v14 KB/M prefix moves across verbatim: ids, names, layouts,
        // mappings, revisions and both realized snapshots.
        out->kbm.mode = v14.kbm.mode;
        out->kbm.next_profile_id = v14.kbm.next_profile_id;
        memcpy(out->kbm.profiles, v14.kbm.profiles, sizeof(out->kbm.profiles));
        memcpy(out->kbm.active, v14.kbm.active, sizeof(out->kbm.active));
        // boot_profile_id and the switch table are filled in below, by the rule
        // that applies to every migrated schema.
        result = CONFIG_PERSIST_MIGRATED;
    } else if (header.version == 13u) {
        if (stored_len < sizeof(config_record_v13_t))
            return CONFIG_PERSIST_DEFAULTED;
        config_record_v13_t v13;
        memcpy(&v13, stored, sizeof(v13));
        out->body_color[0] = header.body_color[0];
        out->body_color[1] = header.body_color[1];
        out->body_color[2] = header.body_color[2];
        memcpy(out->joycon2_left_accent, header.joycon2_left_accent,
               sizeof(out->joycon2_left_accent));
        memcpy(out->joycon2_right_accent, header.joycon2_right_accent,
               sizeof(out->joycon2_right_accent));
        out->wake_valid = header.wake_valid;
        out->wake_identity = header.wake_identity;
        memcpy(out->mgmt_companions, v13.mgmt_companions,
               sizeof(out->mgmt_companions));
        migrate_kbm_from_v13(&v13.kbm, &out->kbm);
        result = CONFIG_PERSIST_MIGRATED;
    } else {
        // An unknown (older or newer) schema. Falling back to defaults is the
        // only safe answer: reinterpreting its bytes would invent settings.
        return CONFIG_PERSIST_DEFAULTED;
    }

    out->magic = CONFIG_PERSIST_MAGIC;
    out->version = CONFIG_PERSIST_VERSION;

    // EVERY migrated schema predates the boot/runtime split, so its record says
    // what the layout was realizing but not what it should realize at power-up.
    // The answer is the same thing: an upgraded adapter must come back doing
    // exactly what it did before. Defaulting these to Default instead would
    // silently discard a migrated mapping -- which is precisely what happened
    // when only the v14 branch set them.
    //
    // No switch key is invented on any path. Reserving F1..F6 would steal six
    // keys out of a mapping the user already has.
    if (result == CONFIG_PERSIST_MIGRATED) {
        for (uint8_t i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i)
            out->kbm.boot_profile_id[i] = out->kbm.active[i].source_id;
        memset(out->kbm.switches, 0, sizeof(out->kbm.switches));
    }

    if (!ns2_kbm_config_sanitize(&out->kbm) && result == CONFIG_PERSIST_CURRENT)
        result = CONFIG_PERSIST_REPAIRED;
    // AFTER sanitize, so a boot choice that had to fall back to Default is the
    // one realized. Power-up follows the persisted choice, never whatever the
    // runtime snapshot held when the record was last written -- a profile-switch
    // key changes the runtime without a flash write, and an unrelated save must
    // not turn that into the boot profile.
    ns2_kbm_realize_boot_profiles(&out->kbm);
    return result;
}
