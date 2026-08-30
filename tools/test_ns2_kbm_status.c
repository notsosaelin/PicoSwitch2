/*
 * KB/M status JSON formatter.
 *
 * This exists because of a real defect, not a hypothetical one. The management
 * and UART surfaces each carried their own printf for this snapshot. Adding one
 * field to the struct updated the format string but not the argument list, in
 * BOTH of them, and printf format/argument drift is not a compile error. The
 * adapter reported:
 *
 *     {"kbm":"controller","override":"kb","profile":"false",...
 *      "native_mouse":, ... "recenters":3522559033}
 *
 * A diagnostic that lies is worse than no diagnostic: it invites wrong
 * conclusions about hardware. One formatter, one test, exact expected output.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

// The smallest buffer any real caller hands this formatter is
// ns2_uart_diag.c's 2048-byte trace_format_response; config.c's is 4096. The
// tests use the smaller of the two, so a reply that outgrows the firmware's own
// buffer fails here rather than silently truncating on hardware.
#define KBM_STATUS_TEST_BUFFER 2048

#include "ns2_kbm_status.h"

static void test_exact_output(void) {
    ns2_kbm_runtime_status_t status;
    memset(&status, 0, sizeof(status));
    status.mode = (uint8_t)NS2_KBM_MODE_KEYBOARD_MOUSE;
    status.mode_override = (uint8_t)NS2_KBM_MODE_AUTO;
    status.profile = (uint8_t)NS2_KBM_LAYOUT_KEYBOARD_MOUSE;
    status.keyboard_connected = 1u;
    status.mouse_connected = 0u;
    status.native_mouse_output = 0u;
    status.keyboard_conn = 4u;
    status.mouse_conn = 7u;
    status.group_id = 3u;
    status.source_id = 9u;
    status.keyboard_reports = 58u;
    status.mouse_reports = 0u;
    status.rejected_mode = 1u;
    status.rejected_duplicate = 1547u;
    status.rejected_not_owner = 2u;
    status.rejected_no_peer_key = 21u;
    status.rejected_unclassified = 22u;
    status.rejected_no_role = 23u;
    status.undecoded_reports = 24u;
    status.active_profile = 2u;
    memcpy(status.active_profile_name, "Splatoon", 9u);
    status.active_revision = 4u;
    status.active_fingerprint = 3735928559u;
    status.active_matches_source = 1u;
    status.rollover_reports = 3u;
    status.role_losses = 4u;
    status.config_generation = 5u;
    status.remap_neutralizations = 6u;
    status.publishes = 7u;
    status.stick_recenters = 8u;

    char out[KBM_STATUS_TEST_BUFFER];
    int written = ns2_kbm_status_format(&status, out, sizeof(out));

    static const char expected[] =
        "{"
        "\"mode\":\"kbmouse\","
        "\"override\":\"auto\","
        "\"profile\":\"kbm\","
        "\"keyboard\":true,"
        "\"mouse\":false,"
        "\"nativeMouse\":false,"
        "\"keyboardConn\":4,"
        "\"mouseConn\":7,"
        "\"group\":3,"
        "\"source\":9,"
        "\"keyboardReports\":58,"
        "\"mouseReports\":0,"
        "\"rejectedMode\":1,"
        "\"rejectedDuplicate\":1547,"
        "\"rejectedNotOwner\":2,"
        "\"rejectedNoPeerKey\":21,"
        "\"rejectedUnclassified\":22,"
        "\"rejectedNoRole\":23,"
        "\"undecodedReports\":24,"
        "\"activeProfile\":2,"
        "\"activeProfileName\":\"Splatoon\","
        "\"activeRevision\":4,"
        "\"activeFingerprint\":3735928559,"
        "\"activeMatchesSaved\":true,"
        "\"rollover\":3,"
        "\"roleLosses\":4,"
        "\"mapGeneration\":5,"
        "\"neutralizations\":6,"
        "\"publishes\":7,"
        "\"recenters\":8"
        "}";

    if (strcmp(out, expected) != 0) {
        // stderr, not stdout: assert() aborts, and an abort discards a buffered
        // stdout -- which left this test reporting only a line number.
        fprintf(stderr,
                "FAIL: formatter output drifted\n  expected: %s\n  actual:   %s\n",
                expected, out);
        assert(0);
    }
    assert(written == (int)strlen(expected));
    puts("  exact field order and types");
}

// Every distinct value must land in its own field. A shifted argument list
// still produces syntactically valid JSON, so identical values would hide it --
// give each counter a unique number and check each one individually.
static void test_every_field_is_distinct(void) {
    ns2_kbm_runtime_status_t status;
    memset(&status, 0, sizeof(status));
    status.mode = (uint8_t)NS2_KBM_MODE_KEYBOARD;
    status.mode_override = (uint8_t)NS2_KBM_MODE_CONTROLLER;
    status.profile = (uint8_t)NS2_KBM_LAYOUT_KEYBOARD;
    status.keyboard_conn = 11u;
    status.mouse_conn = 12u;
    status.group_id = 13u;
    status.source_id = 14u;
    status.keyboard_reports = 15u;
    status.mouse_reports = 16u;
    status.rejected_mode = 17u;
    status.rejected_duplicate = 18u;
    status.rejected_not_owner = 19u;
    status.rejected_no_peer_key = 26u;
    status.rejected_unclassified = 27u;
    status.rejected_no_role = 28u;
    status.undecoded_reports = 29u;
    status.active_profile = 30u;
    memcpy(status.active_profile_name, "Zelda", 6u);
    status.rollover_reports = 20u;
    status.role_losses = 21u;
    status.config_generation = 22u;
    status.remap_neutralizations = 23u;
    status.publishes = 24u;
    status.stick_recenters = 25u;

    char out[KBM_STATUS_TEST_BUFFER];
    (void)ns2_kbm_status_format(&status, out, sizeof(out));

    static const char *const pairs[] = {
        "\"mode\":\"keyboard\"",   "\"override\":\"controller\"",
        "\"profile\":\"kb\"",      "\"keyboardConn\":11",
        "\"mouseConn\":12",        "\"group\":13",
        "\"source\":14",           "\"keyboardReports\":15",
        "\"mouseReports\":16",     "\"rejectedMode\":17",
        "\"rejectedDuplicate\":18","\"rejectedNotOwner\":19",
        "\"rollover\":20",         "\"roleLosses\":21",
        "\"mapGeneration\":22",    "\"neutralizations\":23",
        "\"publishes\":24",        "\"recenters\":25",
        "\"rejectedNoPeerKey\":26", "\"rejectedUnclassified\":27",
        "\"rejectedNoRole\":28",    "\"undecodedReports\":29",
        "\"activeProfile\":30",     "\"activeProfileName\":\"Zelda\"",
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
        if (!strstr(out, pairs[i])) {
            printf("FAIL: missing %s in %s\n", pairs[i], out);
            assert(0);
        }
    }
    puts("  every field carries its own value");
}

static void test_bounds_and_null_safety(void) {
    ns2_kbm_runtime_status_t status;
    memset(&status, 0, sizeof(status));

    char tiny[8];
    int written = ns2_kbm_status_format(&status, tiny, sizeof(tiny));
    // snprintf semantics: reports what it would have needed, and truncates
    // safely rather than overrunning.
    assert(written > (int)sizeof(tiny));
    assert(tiny[sizeof(tiny) - 1u] == '\0');

    char out[KBM_STATUS_TEST_BUFFER];
    out[0] = 'x';
    assert(ns2_kbm_status_format(NULL, out, sizeof(out)) == 0);
    assert(out[0] == '\0');
    assert(ns2_kbm_status_format(&status, NULL, 16u) == 0);
    assert(ns2_kbm_status_format(&status, out, 0u) == 0);
    puts("  bounds and null safety");
}

// A default-constructed status must render as Controller/auto rather than as
// whatever enum value happens to sit at zero.
static void test_zeroed_status_is_sane(void) {
    ns2_kbm_runtime_status_t status;
    memset(&status, 0, sizeof(status));
    char out[KBM_STATUS_TEST_BUFFER];
    (void)ns2_kbm_status_format(&status, out, sizeof(out));
    assert(strstr(out, "\"mode\":\"controller\""));
    assert(strstr(out, "\"override\":\"controller\""));
    assert(strstr(out, "\"keyboard\":false"));
    assert(strstr(out, "\"mouse\":false"));
    puts("  zeroed status");
}

// ---------------------------------------------------------------------------
// Mouse-settings command surface
// ---------------------------------------------------------------------------
// One parser and one response schema serve the management/CDC command surface
// and the UART diagnostic channel. These tests exercise the literal command
// text both of them hand over, so the accepted field set and the accept/reject
// boundary cannot drift between the two surfaces or shift under a refactor.

// Mouse settings became profile-owned, so "the default mouse settings" is what
// the built-in Default template carries.
static ns2_kbm_mouse_config_t default_mouse(void) {
    ns2_kbm_content_t content;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    return content.mouse;
}

static void test_mouse_format_exact_output(void) {
    ns2_kbm_mouse_config_t mouse = default_mouse();
    mouse.sensitivity_x = 768u;
    mouse.sensitivity_y = 1024u;
    mouse.recenter_ms = 120u;
    mouse.invert_x = 0u;
    mouse.invert_y = 1u;

    char out[KBM_STATUS_TEST_BUFFER];
    int written = ns2_kbm_mouse_format(&mouse, out, sizeof(out));

    // The advertised limits travel with the values: a client must not carry its
    // own copy of the accepted range.
    static const char expected[] =
        "{\"sensitivityX\":768,\"sensitivityY\":1024,\"recenterMs\":120,"
        "\"invertX\":false,\"invertY\":true,\"antiDeadzone\":0,"
        "\"sensitivityMin\":16,\"sensitivityMax\":8192,"
        "\"recenterMinMs\":10,\"recenterMaxMs\":2000,"
        "\"antiDeadzoneMax\":50}";
    if (strcmp(out, expected) != 0) {
        printf("FAIL: mouse formatter drifted\n  expected: %s\n  actual:   %s\n",
               expected, out);
        assert(0);
    }
    assert(written == (int)strlen(expected));

    char tiny[8];
    assert(ns2_kbm_mouse_format(&mouse, tiny, sizeof(tiny)) > (int)sizeof(tiny));
    assert(tiny[sizeof(tiny) - 1u] == '\0');
    out[0] = 'x';
    assert(ns2_kbm_mouse_format(NULL, out, sizeof(out)) == 0);
    assert(out[0] == '\0');
    assert(ns2_kbm_mouse_format(&mouse, NULL, 16u) == 0);
    assert(ns2_kbm_mouse_format(&mouse, out, 0u) == 0);
    puts("  mouse settings formatter");
}

static void test_mouse_command_fields(void) {
    // `sensitivity` is both axes; the per-axis forms leave the other alone.
    ns2_kbm_mouse_config_t mouse = default_mouse();
    assert(ns2_kbm_mouse_command_apply(&mouse, "sensitivity 768"));
    assert(mouse.sensitivity_x == 768u && mouse.sensitivity_y == 768u);

    assert(ns2_kbm_mouse_command_apply(&mouse, "sensitivityx 1024"));
    assert(mouse.sensitivity_x == 1024u && mouse.sensitivity_y == 768u);

    assert(ns2_kbm_mouse_command_apply(&mouse, "sensitivityy 1536"));
    assert(mouse.sensitivity_x == 1024u && mouse.sensitivity_y == 1536u);

    // Every remaining field of the existing surface stays reachable: the UART
    // channel deliberately has no allowlist of its own.
    assert(ns2_kbm_mouse_command_apply(&mouse, "recenter 240"));
    assert(mouse.recenter_ms == 240u);
    assert(ns2_kbm_mouse_command_apply(&mouse, "invertx 1"));
    assert(mouse.invert_x == 1u);
    assert(ns2_kbm_mouse_command_apply(&mouse, "inverty 1"));
    assert(mouse.invert_y == 1u);
    assert(ns2_kbm_mouse_command_apply(&mouse, "invertx 0"));
    assert(mouse.invert_x == 0u);

    // Anti-deadzone across its whole configured range, on the same surface.
    for (unsigned percent = 0; percent <= NS2_KBM_MOUSE_ADZ_MAX; ++percent) {
        char line[32];
        snprintf(line, sizeof(line), "antideadzone %u", percent);
        assert(ns2_kbm_mouse_command_apply(&mouse, line));
        assert(mouse.anti_deadzone == (uint8_t)percent);
    }

    // One command sets exactly one field.
    ns2_kbm_mouse_config_t before = mouse;
    assert(ns2_kbm_mouse_command_apply(&mouse, "sensitivityx 2048"));
    assert(mouse.sensitivity_y == before.sensitivity_y);
    assert(mouse.recenter_ms == before.recenter_ms);
    assert(mouse.invert_x == before.invert_x);
    assert(mouse.invert_y == before.invert_y);
    puts("  mouse command fields");
}

static void test_mouse_command_rejects(void) {
    ns2_kbm_mouse_config_t mouse = default_mouse();
    const ns2_kbm_mouse_config_t original = mouse;

    static const char *const bad[] = {
        "",                    // nothing at all
        "sensitivity",         // field with no value
        "sensitivity abc",     // value is not a number
        "bogus 5",             // unknown field
        "sensitivity -1",      // negative
        "sensitivity 70000",   // beyond the persisted uint16_t
        "invertx 2",           // boolean field, non-boolean value
        "inverty 255",
        // Beyond a uint8_t. Without an explicit guard this would truncate to 0
        // and be accepted as "anti-deadzone off" -- a silently wrong setting is
        // worse than a rejected one.
        "antideadzone 256",
        "antideadzone 65535",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        if (ns2_kbm_mouse_command_apply(&mouse, bad[i])) {
            printf("FAIL: accepted \"%s\"\n", bad[i]);
            assert(0);
        }
    }
    // A rejected command must not have half-applied anything.
    assert(memcmp(&mouse, &original, sizeof(mouse)) == 0);
    assert(!ns2_kbm_mouse_command_apply(NULL, "sensitivity 768"));
    assert(!ns2_kbm_mouse_command_apply(&mouse, NULL));

    // Trailing text after the value has always been tolerated by this surface.
    // Tightening it would silently change which commands an existing client can
    // send, so it is pinned rather than left to drift.
    assert(ns2_kbm_mouse_command_apply(&mouse, "sensitivity 768 trailing"));
    assert(mouse.sensitivity_x == 768u);
    puts("  mouse command rejects");
}

// The parser deliberately does NOT enforce the configured range: that stays
// with ns2_kbm_runtime_set_mouse(), which rejects rather than clamps. A value
// that is representable but out of range must therefore pass the parser and be
// caught by sanitize, so both surfaces report it the same way.
static void test_range_enforcement_stays_with_sanitize(void) {
    ns2_kbm_mouse_config_t mouse;

    mouse = default_mouse();
    assert(ns2_kbm_mouse_command_apply(&mouse, "sensitivity 4"));  // below min
    assert(!ns2_kbm_mouse_sanitize(&mouse));

    mouse = default_mouse();
    assert(ns2_kbm_mouse_command_apply(&mouse, "recenter 9000"));  // above max
    assert(!ns2_kbm_mouse_sanitize(&mouse));

    // Anti-deadzone: representable but above the cap. The parser takes it, and
    // sanitize turns it OFF rather than clamping it -- an unusable value must
    // restore the validated linear response, not some compensation the user
    // never chose. The command therefore reports failure either way.
    mouse = default_mouse();
    assert(ns2_kbm_mouse_command_apply(&mouse, "antideadzone 51"));
    assert(!ns2_kbm_mouse_sanitize(&mouse));
    assert(mouse.anti_deadzone == 0u);

    mouse = default_mouse();
    assert(ns2_kbm_mouse_command_apply(&mouse, "antideadzone 200"));
    assert(!ns2_kbm_mouse_sanitize(&mouse));
    assert(mouse.anti_deadzone == 0u);

    // And values inside the range pass both.
    mouse = default_mouse();
    assert(ns2_kbm_mouse_command_apply(&mouse, "sensitivity 1536"));
    assert(ns2_kbm_mouse_command_apply(&mouse, "antideadzone 15"));
    assert(ns2_kbm_mouse_sanitize(&mouse));
    assert(mouse.sensitivity_x == 1536u);
    assert(mouse.anti_deadzone == 15u);
    assert(ns2_kbm_mouse_command_apply(&mouse, "antideadzone 50"));
    assert(ns2_kbm_mouse_sanitize(&mouse));
    assert(mouse.anti_deadzone == NS2_KBM_MOUSE_ADZ_MAX);

    // A whole-config sanitize agrees, because it uses the same clamp.
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    config.active[NS2_KBM_LAYOUT_KEYBOARD].content.mouse.sensitivity_x = 4u;
    assert(!ns2_kbm_config_sanitize(&config));
    puts("  range enforcement stays with sanitize");
}

int main(void) {
    puts("ns2_kbm status formatter:");
    test_exact_output();
    test_every_field_is_distinct();
    test_bounds_and_null_safety();
    test_zeroed_status_is_sane();
    test_mouse_format_exact_output();
    test_mouse_command_fields();
    test_mouse_command_rejects();
    test_range_enforcement_stays_with_sanitize();
    puts("ns2_kbm status formatter tests passed");
    return 0;
}
