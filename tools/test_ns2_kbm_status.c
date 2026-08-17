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

#include "ns2_kbm_status.h"

static void test_exact_output(void) {
    ns2_kbm_runtime_status_t status;
    memset(&status, 0, sizeof(status));
    status.mode = (uint8_t)NS2_KBM_MODE_KEYBOARD_MOUSE;
    status.mode_override = (uint8_t)NS2_KBM_MODE_AUTO;
    status.profile = (uint8_t)NS2_KBM_PROFILE_KEYBOARD_MOUSE;
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
    status.rollover_reports = 3u;
    status.role_losses = 4u;
    status.config_generation = 5u;
    status.remap_neutralizations = 6u;
    status.publishes = 7u;
    status.stick_recenters = 8u;

    char out[512];
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
        "\"rollover\":3,"
        "\"roleLosses\":4,"
        "\"mapGeneration\":5,"
        "\"neutralizations\":6,"
        "\"publishes\":7,"
        "\"recenters\":8"
        "}";

    if (strcmp(out, expected) != 0) {
        printf("FAIL: formatter output drifted\n  expected: %s\n  actual:   %s\n",
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
    status.profile = (uint8_t)NS2_KBM_PROFILE_KEYBOARD;
    status.keyboard_conn = 11u;
    status.mouse_conn = 12u;
    status.group_id = 13u;
    status.source_id = 14u;
    status.keyboard_reports = 15u;
    status.mouse_reports = 16u;
    status.rejected_mode = 17u;
    status.rejected_duplicate = 18u;
    status.rejected_not_owner = 19u;
    status.rollover_reports = 20u;
    status.role_losses = 21u;
    status.config_generation = 22u;
    status.remap_neutralizations = 23u;
    status.publishes = 24u;
    status.stick_recenters = 25u;

    char out[512];
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

    char out[512];
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
    char out[512];
    (void)ns2_kbm_status_format(&status, out, sizeof(out));
    assert(strstr(out, "\"mode\":\"controller\""));
    assert(strstr(out, "\"override\":\"controller\""));
    assert(strstr(out, "\"keyboard\":false"));
    assert(strstr(out, "\"mouse\":false"));
    puts("  zeroed status");
}

int main(void) {
    puts("ns2_kbm status formatter:");
    test_exact_output();
    test_every_field_is_distinct();
    test_bounds_and_null_safety();
    test_zeroed_status_is_sane();
    puts("ns2_kbm status formatter tests passed");
    return 0;
}
