// Profile-switch keys: selecting a RESIDENT SLOT while the adapter runs
// standalone.
//
// This is the reason the adapter stores profiles at all. Without it the six
// resident slots are only reachable from a companion, which defeats the point:
// the user wants to change mapping mid-session with no phone or PC attached.
//
// The properties here are the ones whose absence would be dangerous rather than
// merely wrong -- a stuck button, a key that both switches and fires, a switch
// that erases flash on every press, or a hotkey that reaches across layouts.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ns2_kbm.h"

#define KEY_F1 0x3Au
#define KEY_F2 0x3Bu
#define KEY_F3 0x3Cu
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

// A library with two Keyboard slots and one Keyboard+Mouse slot, each mapping
// the same key to a different destination so a switch is observable.
typedef struct {
    ns2_kbm_config_t config;
    uint8_t work;     // Keyboard
    uint8_t halo;     // Keyboard
    uint8_t desktop;  // Keyboard + Mouse
} fixture_t;

static void build(fixture_t *f) {
    memset(f, 0, sizeof(*f));
    ns2_kbm_config_defaults(&f->config);

    ns2_kbm_content_t content;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    assert(ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_A),
                               NS2_DST_X));
    content.mouse.sensitivity_x = 700u;
    f->work = ns2_kbm_profile_create(&f->config, NS2_KBM_LAYOUT_KEYBOARD, "Work",
                                     &content);

    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    assert(ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_A),
                               NS2_DST_B));
    content.mouse.sensitivity_x = 1400u;
    f->halo = ns2_kbm_profile_create(&f->config, NS2_KBM_LAYOUT_KEYBOARD, "Halo",
                                     &content);

    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD_MOUSE, &content);
    f->desktop = ns2_kbm_profile_create(&f->config,
                                        NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                                        "Desktop", &content);

    assert(f->work != NS2_KBM_PROFILE_ID_NONE);
    assert(f->halo != NS2_KBM_PROFILE_ID_NONE);
    assert(f->desktop != NS2_KBM_PROFILE_ID_NONE);
}

static uint8_t realized_binding(const ns2_kbm_config_t *config,
                                ns2_kbm_layout_t layout, uint8_t usage) {
    const ns2_kbm_content_t *content = ns2_kbm_active_content(config, layout);
    return content ? ns2_kbm_binding(content, layout, key(usage)) : NS2_DST_NONE;
}

/* ------------------------------------------------------------------ tests */

static void test_binding_a_switch_key(void) {
    printf("switch keys bind to resident slots of their own layout\n");
    fixture_t f;
    build(&f);

    CHECK(ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F1),
                              f.work),
          "F1 -> Work should bind");
    CHECK(ns2_kbm_switch_target(&f.config, NS2_KBM_LAYOUT_KEYBOARD,
                                key(KEY_F1)) == f.work,
          "F1 should select Work");

    // WRONG LAYOUT. A Keyboard key cannot select a Keyboard+Mouse slot: that
    // layout can never realize it, so the key would be a control that silently
    // does nothing.
    CHECK(!ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F2),
                               f.desktop),
          "a Keyboard key must not select a KB+M slot");

    // Default is the fallback the layout already returns to, not a slot.
    CHECK(!ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F2),
                               NS2_KBM_PROFILE_ID_DEFAULT),
          "Default is not a resident slot to switch into");

    // Rebinding the same source replaces rather than duplicates, so one key can
    // never carry two meanings.
    CHECK(ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F1),
                              f.halo),
          "rebinding F1 should succeed");
    CHECK(ns2_kbm_switch_target(&f.config, NS2_KBM_LAYOUT_KEYBOARD,
                                key(KEY_F1)) == f.halo,
          "F1 should now select Halo");

    // Clearing always succeeds, so a UI can unbind without first checking.
    CHECK(ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F1),
                              NS2_KBM_PROFILE_ID_NONE),
          "clearing should succeed");
    CHECK(ns2_kbm_switch_target(&f.config, NS2_KBM_LAYOUT_KEYBOARD,
                                key(KEY_F1)) == NS2_KBM_PROFILE_ID_NONE,
          "F1 should be unbound");
}

static void test_no_key_is_reserved(void) {
    printf("no usage is hardcoded or reserved\n");
    fixture_t f;
    build(&f);

    // Function keys are a UI convenience, not a firmware rule. An ordinary
    // letter must be bindable, and F-keys must be free until chosen.
    CHECK(ns2_kbm_switch_target(&f.config, NS2_KBM_LAYOUT_KEYBOARD,
                                key(KEY_F1)) == NS2_KBM_PROFILE_ID_NONE,
          "F1 must not be reserved by default");
    CHECK(ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_W),
                              f.work),
          "any valid usage must be bindable");
    CHECK(ns2_kbm_switch_target(&f.config, NS2_KBM_LAYOUT_KEYBOARD,
                                key(KEY_W)) == f.work,
          "W should select Work");
}

static void test_key_down_edge_only(void) {
    printf("a switch fires once on key-down and never while held\n");
    fixture_t f;
    build(&f);
    assert(ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F1),
                               f.work));

    uint8_t empty[NS2_KBM_KEY_BITMAP_BYTES] = {0};
    uint8_t down[NS2_KBM_KEY_BITMAP_BYTES] = {0};
    hold(down, KEY_F1);

    CHECK(ns2_kbm_switch_edge(&f.config, NS2_KBM_LAYOUT_KEYBOARD, empty, down) ==
              f.work,
          "the down edge must fire");

    // STILL HELD. At keyboard report rate a repeat would be a neutralization
    // storm and the console would be unplayable while the key was down.
    CHECK(ns2_kbm_switch_edge(&f.config, NS2_KBM_LAYOUT_KEYBOARD, down, down) ==
              NS2_KBM_PROFILE_ID_NONE,
          "a held key must not fire again");

    // Release fires nothing.
    CHECK(ns2_kbm_switch_edge(&f.config, NS2_KBM_LAYOUT_KEYBOARD, down, empty) ==
              NS2_KBM_PROFILE_ID_NONE,
          "release must not fire");

    // Press again after a release fires again.
    CHECK(ns2_kbm_switch_edge(&f.config, NS2_KBM_LAYOUT_KEYBOARD, empty, down) ==
              f.work,
          "a second press must fire");
}

static void test_the_switch_key_is_consumed(void) {
    printf("a switch key does not also emit its gameplay binding\n");
    fixture_t f;
    build(&f);
    // Bind F1 as a switch key AND give it a gameplay mapping in the profile, so
    // the two would collide if the key were not consumed.
    ns2_kbm_content_t content;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    assert(ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F1),
                               NS2_DST_A));
    bool changed = false;
    assert(ns2_kbm_apply(&f.config, NS2_KBM_LAYOUT_KEYBOARD,
                         NS2_KBM_PROFILE_ID_DEFAULT, &changed));
    assert(ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F1),
                               f.work));

    uint8_t bitmap[NS2_KBM_KEY_BITMAP_BYTES] = {0};
    hold(bitmap, KEY_F1);
    hold(bitmap, KEY_A);

    ns2_kbm_switch_mask(&f.config, NS2_KBM_LAYOUT_KEYBOARD, bitmap);

    CHECK(!held(bitmap, KEY_F1), "the switch key must be masked out of gameplay");
    CHECK(held(bitmap, KEY_A), "other keys must be untouched");
}

static void test_switching_changes_the_whole_profile(void) {
    printf("a switch changes bindings AND profile-owned mouse settings\n");
    fixture_t f;
    build(&f);
    bool changed = false;
    assert(ns2_kbm_apply(&f.config, NS2_KBM_LAYOUT_KEYBOARD, f.work, &changed));

    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              NS2_DST_X,
          "Work maps A to X");
    CHECK(ns2_kbm_active_content(&f.config, NS2_KBM_LAYOUT_KEYBOARD)
                  ->mouse.sensitivity_x == 700u,
          "Work's mouse settings are realized");

    assert(ns2_kbm_apply(&f.config, NS2_KBM_LAYOUT_KEYBOARD, f.halo, &changed));

    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              NS2_DST_B,
          "Halo maps A to B");
    // The whole profile switches, not just its keys. Leaving the previous
    // profile's sensitivity behind would make a switch feel broken in a way that
    // is very hard to attribute.
    CHECK(ns2_kbm_active_content(&f.config, NS2_KBM_LAYOUT_KEYBOARD)
                  ->mouse.sensitivity_x == 1400u,
          "Halo's mouse settings are realized too");
}

static void test_layouts_are_independent(void) {
    printf("a Keyboard switch key cannot disturb Keyboard+Mouse\n");
    fixture_t f;
    build(&f);
    assert(ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F1),
                               f.work));
    assert(ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                               key(KEY_F2), f.desktop));

    uint8_t empty[NS2_KBM_KEY_BITMAP_BYTES] = {0};
    uint8_t f1[NS2_KBM_KEY_BITMAP_BYTES] = {0};
    hold(f1, KEY_F1);

    // F1 is a Keyboard switch key and means nothing in Keyboard+Mouse.
    CHECK(ns2_kbm_switch_edge(&f.config, NS2_KBM_LAYOUT_KEYBOARD_MOUSE, empty,
                              f1) == NS2_KBM_PROFILE_ID_NONE,
          "F1 must not fire in the other layout");

    uint8_t before = f.config.active[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].source_id;
    bool changed = false;
    assert(ns2_kbm_apply(&f.config, NS2_KBM_LAYOUT_KEYBOARD, f.work, &changed));
    CHECK(f.config.active[NS2_KBM_LAYOUT_KEYBOARD_MOUSE].source_id == before,
          "switching Keyboard must leave Keyboard+Mouse alone");
}

static void test_empty_and_invalid_targets_are_refused(void) {
    printf("an empty or deleted slot is refused safely\n");
    fixture_t f;
    build(&f);
    assert(ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_F1),
                               f.work));

    // Delete the target out from under the key. The binding is dropped by
    // sanitize rather than left pointing at nothing.
    assert(ns2_kbm_profile_delete(&f.config, f.work));
    (void)ns2_kbm_config_sanitize(&f.config);
    CHECK(ns2_kbm_switch_target(&f.config, NS2_KBM_LAYOUT_KEYBOARD,
                                key(KEY_F1)) == NS2_KBM_PROFILE_ID_NONE,
          "a binding to a deleted slot must not survive");

    // And applying an id that does not exist is refused outright, so even a
    // stale target could not change the runtime.
    bool changed = false;
    CHECK(!ns2_kbm_apply(&f.config, NS2_KBM_LAYOUT_KEYBOARD, 200u, &changed),
          "an unknown slot must be refused");
    CHECK(!changed, "a refused switch changes nothing");
}

static void test_the_switch_table_is_bounded(void) {
    printf("the switch table is bounded and cannot be overfilled\n");
    fixture_t f;
    build(&f);

    // Only two Keyboard slots exist, so bind both repeatedly across distinct
    // sources until the table is full.
    unsigned bound = 0;
    for (uint8_t usage = 0x3Au; usage < 0x3Au + 20u; ++usage) {
        if (ns2_kbm_switch_bind(&f.config, NS2_KBM_LAYOUT_KEYBOARD, key(usage),
                                f.work)) {
            bound++;
        }
    }
    CHECK(bound == NS2_KBM_SWITCH_BINDINGS_MAX,
          "expected exactly %u bindings, got %u",
          (unsigned)NS2_KBM_SWITCH_BINDINGS_MAX, bound);
}

static void test_boot_and_runtime_are_separate(void) {
    printf("runtime activation does not move the boot choice\n");
    fixture_t f;
    build(&f);

    // A hotkey switch, or an app Activate: runtime only.
    bool changed = false;
    assert(ns2_kbm_apply(&f.config, NS2_KBM_LAYOUT_KEYBOARD, f.halo, &changed));
    CHECK(f.config.active[NS2_KBM_LAYOUT_KEYBOARD].source_id == f.halo,
          "the runtime slot moved");
    CHECK(f.config.boot_profile_id[NS2_KBM_LAYOUT_KEYBOARD] ==
              NS2_KBM_PROFILE_ID_DEFAULT,
          "the boot choice must NOT move");

    // The explicit boot action moves both.
    assert(ns2_kbm_set_boot_profile(&f.config, NS2_KBM_LAYOUT_KEYBOARD, f.work,
                                    &changed));
    CHECK(f.config.boot_profile_id[NS2_KBM_LAYOUT_KEYBOARD] == f.work,
          "the boot choice moved");
    CHECK(f.config.active[NS2_KBM_LAYOUT_KEYBOARD].source_id == f.work,
          "setting the boot choice also realizes it");

    // Re-running boot realization is what init does; it must land on the boot
    // choice regardless of what the runtime last selected.
    assert(ns2_kbm_apply(&f.config, NS2_KBM_LAYOUT_KEYBOARD, f.halo, &changed));
    ns2_kbm_realize_boot_profiles(&f.config);
    CHECK(f.config.active[NS2_KBM_LAYOUT_KEYBOARD].source_id == f.work,
          "power-up realizes the boot choice, not the last runtime switch");
}

static void test_updating_a_resident_slot_does_not_mutate_the_runtime(void) {
    printf("updating a resident slot leaves the running mapping alone\n");
    fixture_t f;
    build(&f);
    bool changed = false;
    assert(ns2_kbm_apply(&f.config, NS2_KBM_LAYOUT_KEYBOARD, f.work, &changed));
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              NS2_DST_X,
          "Work is running");

    // The user uploads a newer copy of Work into its slot mid-session.
    ns2_kbm_content_t updated;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &updated);
    assert(ns2_kbm_set_binding(&updated, NS2_KBM_LAYOUT_KEYBOARD, key(KEY_A),
                               NS2_DST_Y));
    const ns2_kbm_profile_slot_t *slot = ns2_kbm_profile_find(&f.config, f.work);
    assert(slot);
    uint16_t revision = ns2_kbm_profile_save(&f.config, f.work, slot->revision,
                                             "Work", &updated);
    CHECK(revision != 0u, "the slot update should be accepted");

    // NO SILENT RUNTIME MUTATION. The console keeps doing what it was doing
    // until the user activates the updated slot.
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              NS2_DST_X,
          "the running mapping must not change under the user");
    CHECK(!ns2_kbm_active_matches_source(&f.config, NS2_KBM_LAYOUT_KEYBOARD),
          "the divergence must be reported truthfully");

    // Activating picks up the new content.
    assert(ns2_kbm_apply(&f.config, NS2_KBM_LAYOUT_KEYBOARD, f.work, &changed));
    CHECK(realized_binding(&f.config, NS2_KBM_LAYOUT_KEYBOARD, KEY_A) ==
              NS2_DST_Y,
          "activating uses the updated content");
    CHECK(ns2_kbm_active_matches_source(&f.config, NS2_KBM_LAYOUT_KEYBOARD),
          "and the divergence clears");
}

int main(void) {
    printf("== ns2_kbm switch keys ==\n");
    test_binding_a_switch_key();
    test_no_key_is_reserved();
    test_key_down_edge_only();
    test_the_switch_key_is_consumed();
    test_switching_changes_the_whole_profile();
    test_layouts_are_independent();
    test_empty_and_invalid_targets_are_refused();
    test_the_switch_table_is_bounded();
    test_boot_and_runtime_are_separate();
    test_updating_a_resident_slot_does_not_mutate_the_runtime();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks,
           failures);
    return failures ? 1 : 0;
}
