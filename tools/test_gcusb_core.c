/*
 * Host-compilable tests for tools/gcusb/gcusb_core.c -- the pure, Windows-API-free logic behind
 * the gcusb PC-side USB protocol lab (see PROMPT.md). No hardware, no Windows headers:
 *
 *   gcc -I tools/gcusb -o test_gcusb_core tools/test_gcusb_core.c tools/gcusb/gcusb_core.c
 *   ./test_gcusb_core
 *
 * Exit code 0 = all assertions passed. Covers PROMPT.md's "Host tool tests" acceptance list:
 * deterministic device selection, bcdDevice mismatch refusal, allowlist enforcement, NDJSON
 * validity, replay script validation.
 */
#include <stdio.h>
#include <string.h>

#include "gcusb_core.h"

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
    // --- Target parsing / bcdDevice safety ---
    {
        CHECK(gcusb_parse_target("pico") == GCUSB_TARGET_PICO, "parse_target: 'pico' recognized");
        CHECK(gcusb_parse_target("genuine") == GCUSB_TARGET_GENUINE, "parse_target: 'genuine' recognized");
        CHECK(gcusb_parse_target("bogus") == GCUSB_TARGET_UNSPECIFIED, "parse_target: unknown string -> UNSPECIFIED, not a silent default");
        CHECK(gcusb_parse_target(NULL) == GCUSB_TARGET_UNSPECIFIED, "parse_target: NULL -> UNSPECIFIED");

        CHECK(gcusb_expected_bcddevice(GCUSB_TARGET_PICO) == 0x0111, "expected_bcddevice: Pico is 0x0111");
        CHECK(gcusb_expected_bcddevice(GCUSB_TARGET_GENUINE) == 0x0101, "expected_bcddevice: genuine is 0x0101");

        CHECK(gcusb_bcddevice_matches(GCUSB_TARGET_PICO, 0x0111), "bcddevice_matches: Pico target + 0x0111 actual -> match");
        CHECK(!gcusb_bcddevice_matches(GCUSB_TARGET_PICO, 0x0101), "bcddevice_matches: Pico target + genuine's 0x0101 -> REFUSED");
        CHECK(!gcusb_bcddevice_matches(GCUSB_TARGET_GENUINE, 0x0111), "bcddevice_matches: genuine target + Pico's 0x0111 -> REFUSED");
        CHECK(!gcusb_bcddevice_matches(GCUSB_TARGET_UNSPECIFIED, 0x0111), "bcddevice_matches: unspecified target never matches anything");
    }

    // --- bcdDevice extraction from a Windows hardware-ID string ---
    {
        uint16_t bcd = 0;
        CHECK(gcusb_parse_bcddevice_from_hwid("USB\\VID_057E&PID_2073&REV_0111", &bcd) && bcd == 0x0111,
              "parse_bcddevice_from_hwid: Pico-shaped hardware ID");
        CHECK(gcusb_parse_bcddevice_from_hwid("USB\\VID_057E&PID_2073&REV_0101", &bcd) && bcd == 0x0101,
              "parse_bcddevice_from_hwid: genuine-shaped hardware ID");
        CHECK(!gcusb_parse_bcddevice_from_hwid("USB\\VID_057E&PID_2073", &bcd),
              "parse_bcddevice_from_hwid: no REV_ field -> false, not a garbage value");
        CHECK(!gcusb_parse_bcddevice_from_hwid(NULL, &bcd), "parse_bcddevice_from_hwid: NULL input -> false");
    }

    // --- Command allowlist ---
    {
        uint8_t init_usb[8]        = {0x03, 0x91, 0x00, 0x0D, 0x00, 0x08, 0x00, 0x00};
        uint8_t select_report[8]   = {0x03, 0x91, 0x00, 0x0A, 0x00, 0x04, 0x00, 0x00};
        uint8_t mem_read[8]        = {0x02, 0x91, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00};
        uint8_t mem_write[8]       = {0x02, 0x91, 0x00, 0x05, 0x00, 0x08, 0x00, 0x00};
        uint8_t pairing[8]         = {0x15, 0x91, 0x00, 0x01, 0x00, 0x0e, 0x00, 0x00};
        uint8_t totally_unknown[8] = {0xEE, 0x91, 0x00, 0x99, 0x00, 0x00, 0x00, 0x00};

        CHECK(gcusb_classify_command(init_usb, 8) == GCUSB_CMD_ALLOWED, "allowlist: 0x03/0x0D Initialise USB allowed");
        CHECK(gcusb_classify_command(select_report, 8) == GCUSB_CMD_ALLOWED, "allowlist: 0x03/0x0A Select Input Report allowed");
        CHECK(gcusb_classify_command(mem_read, 8) == GCUSB_CMD_ALLOWED, "allowlist: 0x02/0x04 memory read allowed");
        CHECK(gcusb_classify_command(mem_write, 8) == GCUSB_CMD_REJECTED, "allowlist: 0x02/0x05 memory WRITE rejected by default");
        CHECK(gcusb_classify_command(pairing, 8) == GCUSB_CMD_REQUIRES_CONSOLE_CAPTURE_PROFILE,
              "allowlist: 0x15 pairing requires console-capture profile, not bare default");
        CHECK(gcusb_classify_command(totally_unknown, 8) == GCUSB_CMD_REJECTED, "allowlist: unrecognized command rejected by default");
        CHECK(gcusb_classify_command(NULL, 8) == GCUSB_CMD_REJECTED, "allowlist: NULL buffer rejected, not a crash");
        CHECK(gcusb_classify_command(init_usb, 4) == GCUSB_CMD_REJECTED, "allowlist: too-short buffer (<8 bytes) rejected");

        CHECK(strcmp(gcusb_command_name(select_report, 8), "0x03/0x0A Select Input Report") == 0,
              "command_name: known command gets its documented name");
        CHECK(strcmp(gcusb_command_name(totally_unknown, 8), "unknown") == 0,
              "command_name: unrecognized command named 'unknown', never NULL");
    }

    // --- Rumble safety clamps ---
    {
        CHECK(gcusb_clamp_rumble_amplitude(0xFF, false) == GCUSB_RUMBLE_MAX_FIRST_AMPLITUDE,
              "rumble clamp: 0xFF amplitude clamped down by default (never start full-range)");
        CHECK(gcusb_clamp_rumble_amplitude(0x10, false) == 0x10,
              "rumble clamp: a value already below the ceiling passes through unchanged");
        CHECK(gcusb_clamp_rumble_amplitude(0xFF, true) == 0xFF,
              "rumble clamp: --unsafe explicitly bypasses the ceiling");

        CHECK(gcusb_clamp_rumble_duration_ms(5000, false) == GCUSB_RUMBLE_MAX_PULSE_MS,
              "rumble clamp: a 5-second pulse is capped to the safe maximum by default");
        CHECK(gcusb_clamp_rumble_duration_ms(50, false) == 50,
              "rumble clamp: a short pulse passes through unchanged");
        CHECK(gcusb_clamp_rumble_duration_ms(5000, true) == 5000,
              "rumble clamp: --unsafe bypasses the duration cap too");

        uint8_t data[4];
        gcusb_build_rumble_data(0x40, data);
        CHECK(data[0] == 0 && data[1] == 1 && data[2] == 0 && data[3] == 0,
              "rumble data: nonzero request -> GC_RUMBLE_STATE_ON (1) in byte1, rest zero "
              "(matches switch_gc_hid_out_report()'s corrected state-based decode)");
        gcusb_build_rumble_data(0, data);
        CHECK(data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0,
              "rumble data: zero request -> GC_RUMBLE_STATE_OFF (0) in byte1");
        gcusb_build_rumble_stop_data(data);
        CHECK(data[0] == 0 && data[1] == 2 && data[2] == 0 && data[3] == 0,
              "rumble stop data: GC_RUMBLE_STATE_STOP (2) in byte1");
    }

    // --- NDJSON formatting ---
    {
        gcusb_log_event_t ev = {0};
        ev.ts_us = 123456789ull;
        ev.target = "pico";
        ev.iface = "vendor-bulk";
        ev.xfer_type = "bulk";
        ev.direction = "out";
        ev.setup_hex = NULL;
        ev.req_hex = "03910000";
        ev.resp_hex = "03010000";
        ev.status = "ok";
        ev.elapsed_us = 420;

        char buf[512];
        size_t n = gcusb_format_ndjson_line(&ev, buf, sizeof(buf));
        CHECK(n > 0, "ndjson: formats a non-empty line for a normal event");
        CHECK(buf[0] == '{' && buf[n - 1] == '}', "ndjson: line is a single well-formed JSON object");
        CHECK(strstr(buf, "\"target\":\"pico\"") != NULL, "ndjson: string field rendered correctly");
        CHECK(strstr(buf, "\"setup_hex\":null") != NULL, "ndjson: NULL string field rendered as JSON null, not \"(null)\"");
        CHECK(strstr(buf, "\"ts_us\":123456789") != NULL, "ndjson: integer field rendered without quotes");

        char tiny[8];
        size_t n2 = gcusb_format_ndjson_line(&ev, tiny, sizeof(tiny));
        CHECK(n2 == 0, "ndjson: too-small buffer reports 0 rather than emitting truncated/invalid JSON");
    }

    // --- Replay script parsing (no hardware) ---
    {
        gcusb_script_line_t line;

        gcusb_parse_script_line("# a comment\n", strlen("# a comment\n"), &line);
        CHECK(line.kind == GCUSB_SCRIPT_LINE_BLANK, "script: comment line is BLANK");

        gcusb_parse_script_line("\n", 1, &line);
        CHECK(line.kind == GCUSB_SCRIPT_LINE_BLANK, "script: empty line is BLANK");

        gcusb_parse_script_line("SLEEP 250\n", strlen("SLEEP 250\n"), &line);
        CHECK(line.kind == GCUSB_SCRIPT_LINE_SLEEP && line.sleep_ms == 250, "script: SLEEP parses its argument");

        gcusb_parse_script_line("SLEEP abc\n", strlen("SLEEP abc\n"), &line);
        CHECK(line.kind == GCUSB_SCRIPT_LINE_INVALID, "script: SLEEP with a non-numeric arg is INVALID");

        gcusb_parse_script_line("STOP-RUMBLE\n", strlen("STOP-RUMBLE\n"), &line);
        CHECK(line.kind == GCUSB_SCRIPT_LINE_STOP_RUMBLE, "script: STOP-RUMBLE recognized");

        gcusb_parse_script_line("SEND 03910000\n", strlen("SEND 03910000\n"), &line);
        CHECK(line.kind == GCUSB_SCRIPT_LINE_SEND && line.byte_count == 4 &&
              line.bytes[0] == 0x03 && line.bytes[1] == 0x91 && line.bytes[2] == 0x00 && line.bytes[3] == 0x00,
              "script: SEND decodes hex payload exactly");

        gcusb_parse_script_line("SEND 0391000\n", strlen("SEND 0391000\n"), &line);
        CHECK(line.kind == GCUSB_SCRIPT_LINE_INVALID, "script: SEND with odd-length hex payload is INVALID");

        // Real Initialise-USB request bytes (protocol.md, frame 370989):
        // 03 91 00 0d 00 08 00 00 01 00 f3 b9 34 8c 81 78
        gcusb_parse_script_line("SEND 0391000d000800000100f3b9348c8178\n",
                                 strlen("SEND 0391000d000800000100f3b9348c8178\n"), &line);
        CHECK(line.kind == GCUSB_SCRIPT_LINE_SEND && line.allowlisted,
              "script: a real, allowlisted Initialise-USB SEND is marked allowlisted");

        // 8-byte-minimum pairing-shaped request: id=0x15, dir=0x91, transport=0x00, sub=0x01.
        gcusb_parse_script_line("SEND 15910001000e0000\n", strlen("SEND 15910001000e0000\n"), &line);
        CHECK(line.kind == GCUSB_SCRIPT_LINE_SEND && !line.allowlisted &&
              line.cmd_class == GCUSB_CMD_REQUIRES_CONSOLE_CAPTURE_PROFILE,
              "script: a pairing (0x15) SEND parses but is NOT marked allowlisted by default");

        gcusb_parse_script_line("BOGUS 1234\n", strlen("BOGUS 1234\n"), &line);
        CHECK(line.kind == GCUSB_SCRIPT_LINE_INVALID, "script: unrecognized command word is INVALID");
    }

    printf("\n%s\n", failures == 0 ? "All checks passed." : "One or more checks FAILED.");
    return failures == 0 ? 0 : 1;
}
