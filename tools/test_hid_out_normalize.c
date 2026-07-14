/*
 * Host-compilable tests for the TinyUSB HID-OUT report-ID normalization
 * (src/hid_out_normalize.c). No pico-sdk/TinyUSB dependency:
 *
 *   gcc -I include -o test_hid_out_normalize \
 *       tools/test_hid_out_normalize.c src/hid_out_normalize.c
 *   ./test_hid_out_normalize
 *
 * Exit code 0 = all assertions passed. Covers PROMPT.md's Phase 4 list:
 * inline-ID (interrupt OUT), separate-ID (control SET_REPORT), short packet,
 * all-zero, nonzero, and ZLP cases.
 */
#include <stdio.h>
#include <string.h>

#include "hid_out_normalize.h"

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

int main(void) {
    // Interrupt OUT, inline ID: report_id==0 from TinyUSB, real ID at buffer[0].
    // GameCube rumble sample: 03 50 02 00 00 (ID + 4 data bytes).
    {
        uint8_t buf[] = {0x03, 0x50, 0x02, 0x00, 0x00};
        hid_out_normalized_t n = hid_out_normalize(0, buf, sizeof(buf));
        CHECK(n.report_id == 0x03, "interrupt-OUT inline-ID: report_id extracted from buffer[0]");
        CHECK(n.data == buf + 1, "interrupt-OUT inline-ID: data points past the ID byte");
        CHECK(n.data_len == 4, "interrupt-OUT inline-ID: data_len excludes the ID byte");
        CHECK(memcmp(n.data, "\x50\x02\x00\x00", 4) == 0, "interrupt-OUT inline-ID: data bytes exact");
    }

    // Control SET_REPORT, separate ID: TinyUSB already stripped the leading ID byte and
    // passes report_id != 0 alongside the now-ID-less buffer.
    {
        uint8_t buf[] = {0x50, 0x02, 0x00, 0x00};  // ID already stripped by TinyUSB
        hid_out_normalized_t n = hid_out_normalize(0x03, buf, sizeof(buf));
        CHECK(n.report_id == 0x03, "control-SET_REPORT separate-ID: report_id passed through");
        CHECK(n.data == buf, "control-SET_REPORT separate-ID: data is the buffer as-is (no shift)");
        CHECK(n.data_len == 4, "control-SET_REPORT separate-ID: data_len unchanged");
        CHECK(memcmp(n.data, "\x50\x02\x00\x00", 4) == 0, "control-SET_REPORT separate-ID: data bytes exact");
    }

    // Zero-length OUT packet (ZLP): report_id==0, bufsize==0 -- Confirmed genuine idle/
    // no-rumble behavior for the GameCube rumble endpoint.
    {
        hid_out_normalized_t n = hid_out_normalize(0, NULL, 0);
        CHECK(n.report_id == 0, "ZLP: report_id is 0");
        CHECK(n.data_len == 0, "ZLP: data_len is 0");
    }

    // Short packet: interrupt OUT with only the ID byte and nothing else (data_len should be 0,
    // not underflow).
    {
        uint8_t buf[] = {0x03};
        hid_out_normalized_t n = hid_out_normalize(0, buf, sizeof(buf));
        CHECK(n.report_id == 0x03, "short packet: report_id still extracted from the lone byte");
        CHECK(n.data_len == 0, "short packet: data_len is 0, not underflowed");
    }

    // All-zero rumble data (interrupt OUT): report ID present, all 4 data bytes zero -- the
    // "explicit motor-off report" case, distinct from a ZLP but semantically equivalent for
    // switch_gc_hid_out_report()'s own any_nonzero check (tested separately in
    // tools/test_switch_gc_report.c-adjacent coverage; this file only checks normalization).
    {
        uint8_t buf[] = {0x03, 0x00, 0x00, 0x00, 0x00};
        hid_out_normalized_t n = hid_out_normalize(0, buf, sizeof(buf));
        CHECK(n.report_id == 0x03, "all-zero rumble: report_id still 0x03");
        CHECK(n.data_len == 4, "all-zero rumble: data_len still 4");
        CHECK((n.data[0] | n.data[1] | n.data[2] | n.data[3]) == 0, "all-zero rumble: data is all zero");
    }

    // Nonzero rumble data via control SET_REPORT (separate-ID path), proving both transports
    // agree on the final normalized shape.
    {
        uint8_t buf[] = {0x61, 0x00, 0x01, 0x00};  // ID already stripped
        hid_out_normalized_t n = hid_out_normalize(0x03, buf, sizeof(buf));
        CHECK(n.report_id == 0x03, "nonzero via control path: report_id 0x03");
        CHECK((n.data[0] | n.data[1] | n.data[2] | n.data[3]) != 0, "nonzero via control path: data is nonzero");
    }

    // Pro2 rumble report (0x02), interrupt OUT, proving the same normalization serves both
    // personalities identically (no GameCube-specific special-casing inside the normalizer).
    {
        uint8_t buf[64] = {0x02};
        buf[1] = 0x50;  // arbitrary payload marker
        hid_out_normalized_t n = hid_out_normalize(0, buf, sizeof(buf));
        CHECK(n.report_id == 0x02, "Pro2 interrupt-OUT: report_id 0x02");
        CHECK(n.data_len == 63, "Pro2 interrupt-OUT: data_len is 64-1=63");
        CHECK(n.data[0] == 0x50, "Pro2 interrupt-OUT: data[0] is the byte after the ID");
    }

    printf("\n%s\n", failures == 0 ? "All checks passed." : "One or more checks FAILED.");
    return failures == 0 ? 0 : 1;
}
