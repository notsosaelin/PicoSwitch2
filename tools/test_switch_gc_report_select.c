/*
 * Host-compilable golden tests for the pure NSO GameCube report-select decision logic
 * (src/switch_gc/switch_gc_report_select.c). No pico-sdk/TinyUSB dependency:
 *
 *   gcc -I include -o test_switch_gc_report_select \
 *       tools/test_switch_gc_report_select.c src/switch_gc/switch_gc_report_select.c
 *   ./test_switch_gc_report_select
 *
 * Exit code 0 = all assertions passed. Covers: selecting 0x05 arms 0x05, selecting 0x0A arms
 * 0x0A, unsupported IDs remain unarmed, switching selections changes the armed ID. (Reset/mount
 * clearing this state to GC_REPORT_ID_NONE is a one-line fact in switch_gc.c's
 * switch_gc_init()/reset()/mount() -- verified by code inspection, not a separate runtime test
 * here, since it requires linking the whole TinyUSB-dependent module.)
 */
#include <stdio.h>

#include "switch_gc_report_select.h"

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                      \
            printf("FAIL: %s\n", msg);                                      \
            failures++;                                                    \
        } else {                                                            \
            printf("OK:   %s\n", msg);                                     \
        }                                                                   \
    } while (0)

// Build a minimal 0x03/0x0A "Select Input Report" command payload with the given requested ID
// at c[8], matching ndeadly's documented request layout (id/dir/transport/sub/len header, then
// 1-byte report ID + 3 unused bytes).
static void build_select_cmd(uint8_t *c, uint8_t requested_id) {
    c[0] = 0x03; c[1] = 0x91; c[2] = 0x00; c[3] = 0x0A;
    c[4] = 0x00; c[5] = 0x04; c[6] = 0x00; c[7] = 0x00;
    c[8] = requested_id; c[9] = 0; c[10] = 0; c[11] = 0;
}

int main(void) {
    uint8_t cmd[12];

    // Selecting 0x05 arms 0x05.
    build_select_cmd(cmd, 0x05);
    CHECK(switch_gc_select_report(GC_REPORT_ID_NONE, cmd, sizeof(cmd)) == 0x05,
          "selecting 0x05 from unarmed arms 0x05");

    // Selecting 0x0A arms 0x0A.
    build_select_cmd(cmd, 0x0A);
    CHECK(switch_gc_select_report(GC_REPORT_ID_NONE, cmd, sizeof(cmd)) == 0x0A,
          "selecting 0x0A from unarmed arms 0x0A");

    // Unsupported IDs remain unarmed.
    build_select_cmd(cmd, 0x09);
    CHECK(switch_gc_select_report(GC_REPORT_ID_NONE, cmd, sizeof(cmd)) == GC_REPORT_ID_NONE,
          "selecting an unsupported ID (0x09) from unarmed stays unarmed");
    build_select_cmd(cmd, 0x00);
    CHECK(switch_gc_select_report(GC_REPORT_ID_NONE, cmd, sizeof(cmd)) == GC_REPORT_ID_NONE,
          "selecting ID 0x00 from unarmed stays unarmed");

    // Switching selections changes the emitted report ID.
    build_select_cmd(cmd, 0x0A);
    uint8_t after_0a = switch_gc_select_report(0x05, cmd, sizeof(cmd));
    CHECK(after_0a == 0x0A, "switching from 0x05 to 0x0A actually switches");
    build_select_cmd(cmd, 0x05);
    uint8_t after_05 = switch_gc_select_report(0x0A, cmd, sizeof(cmd));
    CHECK(after_05 == 0x05, "switching from 0x0A to 0x05 actually switches");

    // An unsupported ID does NOT clear an already-armed selection (matches "invalid report IDs
    // are ignored" -- the previous valid selection stays active).
    build_select_cmd(cmd, 0x09);
    CHECK(switch_gc_select_report(0x0A, cmd, sizeof(cmd)) == 0x0A,
          "an unsupported ID does not clear an already-armed 0x0A selection");

    // Short/malformed command (no report-ID byte present) leaves the current state unchanged.
    uint8_t short_cmd[8] = {0x03, 0x91, 0x00, 0x0A, 0x00, 0x04, 0x00, 0x00};
    CHECK(switch_gc_select_report(0x05, short_cmd, sizeof(short_cmd)) == 0x05,
          "a short command with no report-ID byte leaves the current selection unchanged");

    printf("\n%s\n", failures == 0 ? "All checks passed." : "One or more checks FAILED.");
    return failures == 0 ? 0 : 1;
}
