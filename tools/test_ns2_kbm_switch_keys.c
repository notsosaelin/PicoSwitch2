// Profile-switch keys: selecting a PROFILE POSITION while the adapter runs
// standalone.
//
// This is the reason the adapter stores profiles at all. Without it the resident
// profiles are only reachable from a companion, which defeats the point: the
// user wants to change mapping mid-session with no phone or PC attached.
//
// THE CENTRAL PROPERTY, and the one the whole design turns on: a switch key
// names a SEMANTIC POSITION -- Default, Profile 1, Profile 2, Profile 3 -- not a
// storage slot. One set of keys serves both layouts, and the layout derived from
// what is actually connected decides which bank the position is read from. So
// F2 means "Profile 1" always, and selects Halo on a keyboard-only session and
// Metroid Prime once a mouse joins, with no reconfiguration.
//
// The other properties here are the ones whose absence would be dangerous rather
// than merely wrong: a stuck button across a switch, a key that both switches
// and fires, a switch that erases flash on every press, or a key that reaches
// into the wrong layout's bank.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ns2_kbm.h"

#define KEY_F1 0x3Au
#define KEY_F2 0x3Bu
#define KEY_F3 0x3Cu
#define KEY_F4 0x3Du
#define KEY_A 0x04u
#define KEY_W 0x1Au

static int failures;
static int checks;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static ns2_kbm_source_t key(uint8_t usage) {
    ns2_kbm_source_t s = {NS2_KBM_SRC_KEY, usage};
    return s;
}

static void hold(uint8_t *bitmap, uint8_t usage) {
    bitmap[usage >> 3] |= (uint8_t)(1u << (usage & 7u));
}

static bool held(const uint8_t *bitmap, uint8_t usage) {
    return (bitmap[usage >> 3] & (uint8_t)(1u << (usage & 7u))) != 0u;
}

// The two banks from the product specification.
//
//     Keyboard:        Profile 1 Halo, Profile 2 Zelda, Profile 3 Tekken
//     Keyboard+Mouse:  Profile 1 Metroid, Profile 2 Emblem, Profile 3 Testing
//
// Each maps KEY_A to a different destination so a switch is observable, and each
// carries a distinct mouse sensitivity so a WHOLE-profile switch is too.
typedef struct {
    ns2_kbm_config_t config;
    uint8_t kb[4];   // [1..3] Keyboard bank ids
    uint8_t kbm[4];  // [1..3] Keyboard+Mouse bank ids
} fixture_t;

static uint8_t make(fixture_t *f, ns2_kbm_layout_t layout, const char *name,
                    uint8_t destination, uint16_t sensitivity) {
    ns2_kbm_content_t content;
    ns2_kbm_template_default(layout, &content);
    assert(ns2_kbm_set_binding(&content, layout, key(KEY_A), destination));
    content.mouse.sensitivity_x = sensitivity;
    uint8_t id = ns2_kbm_profile_create(&f->config, layout, name, &content);
    assert(id != NS2_KBM_PROFILE_ID_NONE);
    return id;
}

static void build(fixture_t *f) {
    memset(f, 0, sizeof(*f));
    ns2_kbm_config_defaults(&f->config);

    f->kb[1] = make(f, NS2_KBM_LAYOUT_KEYBOARD, "Halo", NS2_DST_X, 700u);
    f->kb[2] = make(f, NS2_KBM_LAYOUT_KEYBOARD, "Zelda", NS2_DST_B, 800u);
    f->kb[3] = make(f, NS2_KBM_LAYOUT_KEYBOARD, "Tekken", NS2_DST_Y, 900u);

    f->kbm[1] = make(f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, "Metroid", NS2_DST_L,
                     1100u);
    f->kbm[2] = make(f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, "Emblem", NS2_DST_R,
                     1200u);
    f->kbm[3] = make(f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, "Testing", NS2_DST_A,
                     1300u);

    // Three positions per layout, and both banks full: six records exactly.
    for (uint8_t p = 1; p <= NS2_KBM_POSITIONS_PER_LAYOUT; ++p) {
        assert(ns2_kbm_profile_at(&f->config, NS2_KBM_LAYOUT_KEYBOARD, p));
        assert(ns2_kbm_profile_at(&f->config, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, p));
    }
}

// The user's four keys, exactly as the specification describes them.
static void bind_the_four_keys(fixture_t *f) {
    assert(ns2_kbm_switch_bind(&f->config, key(KEY_F1), NS2_KBM_POSITION_DEFAULT));
    assert(ns2_kbm_switch_bind(&f->config, key(KEY_F2), 1u));
    assert(ns2_kbm_switch_bind(&f->config, key(KEY_F3), 2u));
    assert(ns2_kbm_switch_bind(&f->config, key(KEY_F4), 3u));
}

// Press one key and resolve it exactly as the runtime does.
static uint8_t press(fixture_t *f, ns2_kbm_layout_t layout, uint8_t usage) {
    uint8_t before[NS2_KBM_KEY_BITMAP_BYTES] = {0};
    uint8_t after[NS2_KBM_KEY_BITMAP_BYTES] = {0};
    hold(after, usage);
    uint8_t action = ns2_kbm_switch_edge(&f->config, before, after);
    if (action == NS2_KBM_SWITCH_NONE) return NS2_KBM_PROFILE_ID_NONE;
    uint8_t id = ns2_kbm_position_profile_id(&f->config, layout, action);
    if (id == NS2_KBM_PROFILE_ID_NONE) return NS2_KBM_PROFILE_ID_NONE;
    bool changed = false;
    if (!ns2_kbm_apply(&f->config, layout, id, &changed))
        return NS2_KBM_PROFILE_ID_NONE;
    return id;
}

static uint8_t realized_binding(const ns2_kbm_config_t *config,
                                ns2_kbm_layout_t layout, uint8_t usage) {
    const ns2_kbm_content_t *content = ns2_kbm_active_content(config, layout);
    return content ? ns2_kbm_binding(content, layout, key(usage)) : NS2_DST_NONE;
}

static uint16_t realized_sensitivity(const ns2_kbm_config_t *config,
                                     ns2_kbm_layout_t layout) {
    const ns2_kbm_content_t *content = ns2_kbm_active_content(config, layout);
    return content ? content->mouse.sensitivity_x : 0u;
}

/* ------------------------------------------------------------------ tests */

// THE SPECIFICATION, verbatim. The same four keys, both layouts.
static void test_the_same_keys_select_the_layout_appropriate_profile(void) {
    printf("the same key selects the layout-appropriate profile\n");
    fixture_t f;
    build(&f);
    bind_the_four_keys(&f);

    // Keyboard-only session.
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F2) == f.kb[1],
          "F2 must select Keyboard Profile 1 (Halo)");
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              NS2_DST_X, "Halo is realized");
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F3) == f.kb[2],
          "F3 must select Keyboard Profile 2 (Zelda)");
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F4) == f.kb[3],
          "F4 must select Keyboard Profile 3 (Tekken)");
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F1) ==
              NS2_KBM_PROFILE_ID_DEFAULT,
          "F1 must select the built-in Default of the Keyboard layout");

    // A mouse joins. THE SAME KEYS, without any reconfiguration, now address the
    // Keyboard+Mouse bank.
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, KEY_F2) == f.kbm[1],
          "F2 must select KB+M Profile 1 (Metroid)");
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, KEY_A) ==
              NS2_DST_L, "Metroid is realized");
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, KEY_F3) == f.kbm[2],
          "F3 must select KB+M Profile 2 (Emblem)");
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, KEY_F4) == f.kbm[3],
          "F4 must select KB+M Profile 3 (Testing)");
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, KEY_F1) ==
              NS2_KBM_PROFILE_ID_DEFAULT,
          "F1 must select the built-in Default of the KB+M layout");

    // And back. The Keyboard bank's meaning is restored, unchanged by anything
    // that happened while the mouse was connected.
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F2) == f.kb[1],
          "returning to Keyboard restores F2 -> Halo");
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              NS2_DST_X, "Halo is realized again");
}

static void test_one_action_table_serves_both_layouts(void) {
    printf("switch bindings are layout-independent\n");
    fixture_t f;
    build(&f);
    bind_the_four_keys(&f);

    // The lookup itself carries no layout: that is what stops the user having to
    // configure two disjoint key ranges.
    CHECK(ns2_kbm_switch_action(&f.config, key(KEY_F2)) == 1u,
          "F2 is Profile 1, everywhere");
    CHECK(ns2_kbm_switch_action(&f.config, key(KEY_F1)) ==
              NS2_KBM_POSITION_DEFAULT, "F1 is Default, everywhere");
    CHECK(ns2_kbm_switch_action(&f.config, key(KEY_W)) == NS2_KBM_SWITCH_NONE,
          "an unbound key is not a switch key");
}

static void test_no_key_is_reserved(void) {
    printf("no usage is hardcoded or reserved\n");
    fixture_t f;
    build(&f);

    // Function keys are a UI convenience, not a firmware rule.
    CHECK(ns2_kbm_switch_action(&f.config, key(KEY_F1)) == NS2_KBM_SWITCH_NONE,
          "F1 must not be reserved by default");
    CHECK(ns2_kbm_switch_bind(&f.config, key(KEY_W), 1u),
          "any valid usage must be bindable");
    CHECK(ns2_kbm_switch_action(&f.config, key(KEY_W)) == 1u,
          "W selects Profile 1");
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_W) == f.kb[1],
          "and it works");
}

static void test_binding_rules(void) {
    printf("binding rules keep one key to one meaning\n");
    fixture_t f;
    build(&f);

    CHECK(!ns2_kbm_switch_bind(&f.config, key(KEY_F1), 4u),
          "a position beyond the bank must be refused");

    // Rebinding a source replaces its action.
    assert(ns2_kbm_switch_bind(&f.config, key(KEY_F2), 1u));
    assert(ns2_kbm_switch_bind(&f.config, key(KEY_F2), 2u));
    CHECK(ns2_kbm_switch_action(&f.config, key(KEY_F2)) == 2u,
          "rebinding replaces the action");

    // Assigning a second key to the same action MOVES it, so a bounded table
    // cannot fill up with duplicates of one action.
    assert(ns2_kbm_switch_bind(&f.config, key(KEY_F3), 2u));
    CHECK(ns2_kbm_switch_action(&f.config, key(KEY_F2)) == NS2_KBM_SWITCH_NONE,
          "the old key for that action is released");
    CHECK(ns2_kbm_switch_action(&f.config, key(KEY_F3)) == 2u,
          "the new key holds it");

    // Clearing always succeeds.
    CHECK(ns2_kbm_switch_bind(&f.config, key(KEY_F3), NS2_KBM_SWITCH_NONE),
          "clearing succeeds");
    CHECK(ns2_kbm_switch_action(&f.config, key(KEY_F3)) == NS2_KBM_SWITCH_NONE,
          "and unbinds");

    // Keys may be configured BEFORE the profiles exist: the order must not
    // matter, and an empty position is simply rejected at press time.
    ns2_kbm_config_t empty;
    ns2_kbm_config_defaults(&empty);
    CHECK(ns2_kbm_switch_bind(&empty, key(KEY_F2), 1u),
          "binding a key for an empty position is allowed");
    CHECK(ns2_kbm_position_profile_id(&empty, NS2_KBM_LAYOUT_KEYBOARD, 1u) ==
              NS2_KBM_PROFILE_ID_NONE,
          "but the position resolves to nothing");
}

static void test_key_down_edge_only(void) {
    printf("a switch fires once on key-down and never while held\n");
    fixture_t f;
    build(&f);
    bind_the_four_keys(&f);

    uint8_t empty[NS2_KBM_KEY_BITMAP_BYTES] = {0};
    uint8_t down[NS2_KBM_KEY_BITMAP_BYTES] = {0};
    hold(down, KEY_F2);

    CHECK(ns2_kbm_switch_edge(&f.config, empty, down) == 1u,
          "the down edge must fire");

    // STILL HELD. At keyboard report rate a repeat would be a neutralization
    // storm and the console would be unplayable while the key was down.
    CHECK(ns2_kbm_switch_edge(&f.config, down, down) == NS2_KBM_SWITCH_NONE,
          "a held key must not fire again");
    CHECK(ns2_kbm_switch_edge(&f.config, down, empty) == NS2_KBM_SWITCH_NONE,
          "release must not fire");
    CHECK(ns2_kbm_switch_edge(&f.config, empty, down) == 1u,
          "a second press must fire");
}

static void test_the_switch_key_is_consumed(void) {
    printf("a switch key does not also emit its gameplay binding\n");
    fixture_t f;
    build(&f);
    bind_the_four_keys(&f);

    uint8_t bitmap[NS2_KBM_KEY_BITMAP_BYTES] = {0};
    hold(bitmap, KEY_F1);
    hold(bitmap, KEY_F2);
    hold(bitmap, KEY_A);

    ns2_kbm_switch_mask(&f.config, bitmap);

    CHECK(!held(bitmap, KEY_F1), "Default's key is masked out of gameplay");
    CHECK(!held(bitmap, KEY_F2), "Profile 1's key is masked out of gameplay");
    CHECK(held(bitmap, KEY_A), "other keys are untouched");
}

static void test_switching_changes_the_whole_profile(void) {
    printf("a switch changes bindings AND profile-owned mouse settings\n");
    fixture_t f;
    build(&f);
    bind_the_four_keys(&f);

    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, KEY_F2) == f.kbm[1],
          "select KB+M Profile 1");
    CHECK(realized_sensitivity(&f.config, NS2_KBM_LAYOUT_KEYBOARD_MOUSE) ==
              1100u, "Metroid's mouse settings are realized");

    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, KEY_F3) == f.kbm[2],
          "select KB+M Profile 2");
    // The WHOLE profile switches. Leaving the previous profile's sensitivity
    // behind would make a switch feel broken in a way that is very hard to
    // attribute to the switch at all.
    CHECK(realized_sensitivity(&f.config, NS2_KBM_LAYOUT_KEYBOARD_MOUSE) ==
              1200u, "Emblem's mouse settings are realized too");
}

static void test_layouts_are_independent(void) {
    printf("switching one layout never disturbs the other\n");
    fixture_t f;
    build(&f);
    bind_the_four_keys(&f);

    assert(press(&f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, KEY_F4) == f.kbm[3]);
    uint8_t kbm_before = f.config.active[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].source_id;

    assert(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F2) == f.kb[1]);

    CHECK(f.config.active[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].source_id == kbm_before,
          "switching Keyboard must leave Keyboard+Mouse alone");
    CHECK(f.config.active[NS2_KBM_LAYOUT_KEYBOARD].source_id == f.kb[1],
          "and Keyboard did move");
}

static void test_an_empty_position_is_rejected_safely(void) {
    printf("an empty position is rejected without changing anything\n");
    fixture_t f;
    memset(&f, 0, sizeof(f));
    ns2_kbm_config_defaults(&f.config);
    // Only Keyboard Profile 1 exists; positions 2 and 3 are empty, and the whole
    // KB+M bank is empty.
    f.kb[1] = make(&f, NS2_KBM_LAYOUT_KEYBOARD, "Halo", NS2_DST_X, 700u);
    bind_the_four_keys(&f);

    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F3) == NS2_KBM_PROFILE_ID_NONE,
          "an empty Keyboard position must not activate");
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, KEY_F2) ==
              NS2_KBM_PROFILE_ID_NONE,
          "an empty KB+M position must not activate");
    // NEVER the other layout's profile. That would be the worst possible
    // outcome: a mapping for the wrong device silently taking effect.
    CHECK(f.config.active[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].source_id ==
              NS2_KBM_PROFILE_ID_DEFAULT,
          "the wrong layout's profile must never be activated");
}

static void test_positions_are_bounded_per_layout(void) {
    printf("each layout's bank holds exactly three custom positions\n");
    fixture_t f;
    build(&f);  // both banks already full

    ns2_kbm_content_t content;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    CHECK(ns2_kbm_profile_create(&f.config, NS2_KBM_LAYOUT_KEYBOARD, "Extra",
                                 &content) == NS2_KBM_PROFILE_ID_NONE,
          "a fourth Keyboard profile must be refused");
    CHECK(ns2_kbm_free_position(&f.config, NS2_KBM_LAYOUT_KEYBOARD) == 0u,
          "the Keyboard bank reports itself full");

    // Freeing a position makes exactly that position available again.
    assert(ns2_kbm_profile_delete(&f.config, f.kb[2]));
    CHECK(ns2_kbm_free_position(&f.config, NS2_KBM_LAYOUT_KEYBOARD) == 2u,
          "the freed position is the one offered");
    uint8_t replacement = ns2_kbm_profile_create(&f.config,
                                                 NS2_KBM_LAYOUT_KEYBOARD,
                                                 "Replacement", &content);
    CHECK(replacement != NS2_KBM_PROFILE_ID_NONE, "and can be filled");
    const ns2_kbm_profile_slot_t *slot =
        ns2_kbm_profile_at(&f.config, NS2_KBM_LAYOUT_KEYBOARD, 2u);
    CHECK(slot && slot->profile_id == replacement,
          "the new profile occupies position 2");
}

static void test_boot_and_runtime_are_separate(void) {
    printf("runtime activation does not move the boot position\n");
    fixture_t f;
    build(&f);
    bind_the_four_keys(&f);

    // A hotkey switch, or an app Activate: runtime only.
    assert(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F3) == f.kb[2]);
    CHECK(f.config.boot_position[NS2_KBM_LAYOUT_KEYBOARD] ==
              NS2_KBM_POSITION_DEFAULT,
          "the boot position must NOT move");

    // The explicit boot action moves both.
    bool changed = false;
    CHECK(ns2_kbm_set_boot_position(&f.config, NS2_KBM_LAYOUT_KEYBOARD, 1u,
                                    &changed),
          "setting the boot position succeeds");
    CHECK(changed, "and reports the change, so the caller can skip a no-op save");
    CHECK(f.config.boot_position[NS2_KBM_LAYOUT_KEYBOARD] == 1u,
          "the boot position moved");
    CHECK(f.config.active[NS2_KBM_LAYOUT_KEYBOARD].source_id == f.kb[1],
          "and it is realized now, not only after a reboot");

    // Setting the same position again changes nothing, so it costs no write.
    changed = true;
    assert(ns2_kbm_set_boot_position(&f.config, NS2_KBM_LAYOUT_KEYBOARD, 1u,
                                     &changed));
    CHECK(!changed, "a no-op boot selection must not report a change");

    // Power-up realizes the boot position regardless of the last hotkey.
    assert(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F4) == f.kb[3]);
    ns2_kbm_realize_boot_profiles(&f.config);
    CHECK(f.config.active[NS2_KBM_LAYOUT_KEYBOARD].source_id == f.kb[1],
          "power-up realizes the boot position, not the last runtime switch");

    // An empty boot position cannot be set.
    assert(ns2_kbm_profile_delete(&f.config, f.kbm[1]));
    CHECK(!ns2_kbm_set_boot_position(&f.config, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                                     1u, &changed),
          "an empty position cannot become the boot position");
}

static void test_updating_a_resident_profile_does_not_mutate_the_runtime(void) {
    printf("updating a resident profile leaves the running mapping alone\n");
    fixture_t f;
    build(&f);
    bind_the_four_keys(&f);
    assert(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F2) == f.kb[1]);
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              NS2_DST_X, "Halo is running");

    // The user uploads a newer copy of Halo into its position mid-session.
    ns2_kbm_content_t updated;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &updated);
    assert(ns2_kbm_set_binding(&updated, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_A),
                               NS2_DST_ZR));
    const ns2_kbm_profile_slot_t *slot = ns2_kbm_profile_find(&f.config, f.kb[1]);
    assert(slot);
    CHECK(ns2_kbm_profile_save(&f.config, f.kb[1], slot->revision, "Halo",
                               &updated) != 0u,
          "the resident update should be accepted");

    // NO SILENT RUNTIME MUTATION. The console keeps doing what it was doing
    // until the user activates the updated profile.
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              NS2_DST_X, "the running mapping must not change under the user");
    CHECK(!ns2_kbm_active_matches_source(&f.config, NS2_KBM_LAYOUT_KEYBOARD),
          "the divergence must be reported truthfully");

    // Reselecting the position picks up the new content.
    assert(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F2) == f.kb[1]);
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              NS2_DST_ZR, "reactivating uses the updated content");
    CHECK(ns2_kbm_active_matches_source(&f.config, NS2_KBM_LAYOUT_KEYBOARD),
          "and the divergence clears");
}

// REMOVE FROM ADAPTER. The user-facing action, addressed by position.
static void test_removing_a_position_falls_back_safely(void) {
    printf("removing a position leaves no dangling runtime or boot selection\n");
    fixture_t f;
    build(&f);
    bind_the_four_keys(&f);

    // Position 2 is both running and the startup choice -- the worst case for a
    // removal, because two separate references have to be cleaned up.
    bool changed = false;
    assert(ns2_kbm_set_boot_position(&f.config, NS2_KBM_LAYOUT_KEYBOARD, 2u,
                                     &changed));
    assert(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F3) == f.kb[2]);
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) == NS2_DST_B,
          "Zelda is running before the removal");

    CHECK(ns2_kbm_position_clear(&f.config, NS2_KBM_LAYOUT_KEYBOARD, 2u),
          "the position should clear");

    CHECK(ns2_kbm_profile_at(&f.config, NS2_KBM_LAYOUT_KEYBOARD, 2u) == NULL,
          "the position is empty afterwards");
    // Both references fall back to Default rather than dangling.
    CHECK(f.config.active[NS2_KBM_LAYOUT_KEYBOARD].source_id ==
              NS2_KBM_PROFILE_ID_DEFAULT,
          "the running mapping falls back to Default");
    CHECK(f.config.boot_position[NS2_KBM_LAYOUT_KEYBOARD] ==
              NS2_KBM_POSITION_DEFAULT,
          "the startup choice falls back to Default");
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              ns2_kbm_default_binding(NS2_KBM_LAYOUT_KEYBOARD, key(KEY_A)),
          "and the console is running the Default mapping");

    // The OTHER layout is untouched, and the other positions of this one remain.
    CHECK(ns2_kbm_profile_at(&f.config, NS2_KBM_LAYOUT_KEYBOARD, 1u) != NULL,
          "Profile 1 survives");
    CHECK(ns2_kbm_profile_at(&f.config, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, 2u) != NULL,
          "the other layout's Profile 2 survives");

    // The switch key that pointed at it stays bound: the ACTION is still valid,
    // the position is simply empty, and pressing it is safely refused.
    CHECK(ns2_kbm_switch_action(&f.config, key(KEY_F3)) == 2u,
          "the switch key keeps its action");
    CHECK(press(&f, NS2_KBM_LAYOUT_KEYBOARD, KEY_F3) == NS2_KBM_PROFILE_ID_NONE,
          "and pressing it now does nothing");

    // Clearing an already-empty position succeeds: the caller asked for it to
    // hold nothing, and it does.
    CHECK(ns2_kbm_position_clear(&f.config, NS2_KBM_LAYOUT_KEYBOARD, 2u),
          "clearing an empty position is not an error");
    CHECK(!ns2_kbm_position_clear(&f.config, NS2_KBM_LAYOUT_KEYBOARD, 9u),
          "an out-of-range position is refused");

    // And the freed position can be assigned again.
    CHECK(ns2_kbm_free_position(&f.config, NS2_KBM_LAYOUT_KEYBOARD) == 2u,
          "the freed position is offered next");
}

int main(void) {
    printf("== ns2_kbm switch keys ==\n");
    test_the_same_keys_select_the_layout_appropriate_profile();
    test_one_action_table_serves_both_layouts();
    test_no_key_is_reserved();
    test_binding_rules();
    test_key_down_edge_only();
    test_the_switch_key_is_consumed();
    test_switching_changes_the_whole_profile();
    test_layouts_are_independent();
    test_an_empty_position_is_rejected_safely();
    test_positions_are_bounded_per_layout();
    test_boot_and_runtime_are_separate();
    test_updating_a_resident_profile_does_not_mutate_the_runtime();
    test_removing_a_position_falls_back_safely();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks,
           failures);
    return failures ? 1 : 0;
}
