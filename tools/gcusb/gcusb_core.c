#include "gcusb_core.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ------------------------------------------------------------------------------------------- */

gcusb_target_t gcusb_parse_target(const char *arg) {
    if (!arg) return GCUSB_TARGET_UNSPECIFIED;
    if (strcmp(arg, "pico") == 0 || strcmp(arg, "Pico") == 0 || strcmp(arg, "PICO") == 0)
        return GCUSB_TARGET_PICO;
    if (strcmp(arg, "genuine") == 0 || strcmp(arg, "Genuine") == 0 || strcmp(arg, "GENUINE") == 0)
        return GCUSB_TARGET_GENUINE;
    return GCUSB_TARGET_UNSPECIFIED;
}

uint16_t gcusb_expected_bcddevice(gcusb_target_t target) {
    switch (target) {
        case GCUSB_TARGET_PICO:    return GCUSB_BCDDEVICE_PICO;
        case GCUSB_TARGET_GENUINE: return GCUSB_BCDDEVICE_GENUINE;
        default:                  return 0;
    }
}

bool gcusb_bcddevice_matches(gcusb_target_t target, uint16_t actual_bcddevice) {
    if (target == GCUSB_TARGET_UNSPECIFIED) return false;  /* never silently accept */
    return gcusb_expected_bcddevice(target) == actual_bcddevice;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool gcusb_parse_bcddevice_from_hwid(const char *hwid, uint16_t *out_bcddevice) {
    if (!hwid || !out_bcddevice) return false;
    const char *rev = strstr(hwid, "REV_");
    if (!rev) return false;
    rev += 4;  /* skip "REV_" */
    uint16_t value = 0;
    int digits = 0;
    for (; digits < 4; digits++) {
        int v = hexval(rev[digits]);
        if (v < 0) break;
        value = (uint16_t)((value << 4) | (uint16_t)v);
    }
    if (digits != 4) return false;
    *out_bcddevice = value;
    return true;
}

/* ------------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t id;
    int sub;             /* -1 = matches any sub for this id */
    gcusb_cmd_class_t cls;
    const char *name;
} gcusb_allow_entry_t;

/* Mirrors switch_gc_vendor_dispatch()'s own switch(id)/sub structure exactly (src/switch_gc/
 * switch_gc.c) -- every entry here corresponds to a case that dispatcher actually implements a
 * real (non-generic-bare-ACK) response for for. Anything NOT listed here classifies as REJECTED,
 * matching PROMPT.md's "reject unknown vendor writes by default" requirement -- including this
 * dispatcher's own `default:` bare-ACK case (0x06/0x0A-vibration/etc.), which is intentionally
 * NOT allowlisted here: this tool exists partly to find out whether one of those "unknown, gets
 * a bare ACK" commands is what's actually triggering the rumble bug, so the safe path is to
 * require --unsafe for anything not already a known-good, named, evidenced command. */
static const gcusb_allow_entry_t k_allowlist[] = {
    {0x03, 0x0D, GCUSB_CMD_ALLOWED, "0x03/0x0D Initialise USB"},
    {0x03, 0x03, GCUSB_CMD_ALLOWED, "0x03/0x03 Enable USB HID Reports"},
    {0x03, 0x0A, GCUSB_CMD_ALLOWED, "0x03/0x0A Select Input Report"},
    {0x02, 0x04, GCUSB_CMD_ALLOWED, "0x02/0x04 Memory read (variable length)"},
    {0x02, 0x01, GCUSB_CMD_ALLOWED, "0x02/0x01 Memory read (fixed 0x28)"},
    /* 0x02/0x05 (memory WRITE) deliberately absent -- SPI/factory writes rejected by default. */
    {0x0C, 0x01, GCUSB_CMD_ALLOWED, "0x0C/0x01 Get feature info"},
    {0x0C, 0x06, GCUSB_CMD_ALLOWED, "0x0C/0x06 Configure features"},
    {0x16, -1,   GCUSB_CMD_ALLOWED, "0x16 Unknown status (24 zero bytes)"},
    {0x07, -1,   GCUSB_CMD_ALLOWED, "0x07 First-init command"},
    {0x09, -1,   GCUSB_CMD_ALLOWED, "0x09 Player LEDs"},
    {0x10, -1,   GCUSB_CMD_ALLOWED, "0x10 Firmware info"},
    {0x0B, 0x03, GCUSB_CMD_ALLOWED, "0x0B/0x03 Battery"},
    {0x0B, 0x04, GCUSB_CMD_ALLOWED, "0x0B/0x04 Battery"},
    {0x11, 0x01, GCUSB_CMD_ALLOWED, "0x11/0x01 Unknown (shared-constant form)"},
    {0x11, 0x03, GCUSB_CMD_ALLOWED, "0x11/0x03 Unknown (shared-constant form)"},
    {0x01, -1,   GCUSB_CMD_ALLOWED, "0x01 NFC-shaped bare ack"},
    {0x18, 0x01, GCUSB_CMD_ALLOWED, "0x18/0x01 Unknown"},
    {0x18, 0x03, GCUSB_CMD_ALLOWED, "0x18/0x03 Unknown"},
    /* Pairing-shaped: real AES-128 session-key derivation state (s_gc_ltk). Not a stored
     * identity/bond write (nothing persists across power cycles), but per PROMPT.md's explicit
     * "pairing-key or identity writes" rejection and its own instruction to gate this behind an
     * explicit profile, this family is NEVER in the bare default allowlist. */
    {0x15, -1,   GCUSB_CMD_REQUIRES_CONSOLE_CAPTURE_PROFILE, "0x15 Bluetooth-pairing-shaped (real crypto)"},
};
#define K_ALLOWLIST_COUNT (sizeof(k_allowlist) / sizeof(k_allowlist[0]))

static const gcusb_allow_entry_t *find_entry(const uint8_t *cmd, uint32_t len) {
    if (!cmd || len < 8) return NULL;
    uint8_t id = cmd[0];
    uint8_t sub = cmd[3];
    for (size_t i = 0; i < K_ALLOWLIST_COUNT; i++) {
        if (k_allowlist[i].id != id) continue;
        if (k_allowlist[i].sub == -1 || (uint8_t)k_allowlist[i].sub == sub) return &k_allowlist[i];
    }
    return NULL;
}

gcusb_cmd_class_t gcusb_classify_command(const uint8_t *cmd, uint32_t len) {
    const gcusb_allow_entry_t *e = find_entry(cmd, len);
    if (!e) return GCUSB_CMD_REJECTED;
    /* Extra restriction for Select Input Report: only 0x05/0x0A payload values are meaningful
     * (switch_gc_select_report()'s own contract) -- anything else is still a harmless query per
     * the firmware (it just won't arm streaming), so classification doesn't need to special-case
     * the payload value beyond what the firmware itself already tolerates safely. */
    return e->cls;
}

const char *gcusb_command_name(const uint8_t *cmd, uint32_t len) {
    const gcusb_allow_entry_t *e = find_entry(cmd, len);
    return e ? e->name : "unknown";
}

/* ------------------------------------------------------------------------------------------- */

uint8_t gcusb_clamp_rumble_amplitude(uint8_t requested, bool allow_unsafe) {
    if (allow_unsafe) return requested;
    return (requested > GCUSB_RUMBLE_MAX_FIRST_AMPLITUDE) ? GCUSB_RUMBLE_MAX_FIRST_AMPLITUDE
                                                            : requested;
}

uint32_t gcusb_clamp_rumble_duration_ms(uint32_t requested_ms, bool allow_unsafe) {
    if (allow_unsafe) return requested_ms;
    return (requested_ms > GCUSB_RUMBLE_MAX_PULSE_MS) ? GCUSB_RUMBLE_MAX_PULSE_MS : requested_ms;
}

/* GC_RUMBLE_STATE_ON=1 -- must match src/switch_gc/switch_gc.c's gc_rumble_state_t exactly
 * (duplicated, not shared, since this tool intentionally has zero dependency on firmware headers
 * -- see this file's own top-of-file comment on the pure/impure split). */
#define GCUSB_WIRE_GC_RUMBLE_STATE_ON 1u

void gcusb_build_rumble_data(uint8_t requested, uint8_t out[4]) {
    out[0] = 0;  /* sequence/command byte -- Unknown exact formula, firmware doesn't read it */
    out[1] = (requested != 0) ? GCUSB_WIRE_GC_RUMBLE_STATE_ON : 0;  /* GC_RUMBLE_STATE_OFF/ON */
    out[2] = 0;
    out[3] = 0;
}

void gcusb_build_rumble_stop_data(uint8_t out[4]) {
    out[0] = 0;
    out[1] = 2;  /* GC_RUMBLE_STATE_STOP -- distinct from modulation OFF (0) */
    out[2] = 0;
    out[3] = 0;
}

/* ------------------------------------------------------------------------------------------- */

static size_t append_str(char *out, size_t cap, size_t pos, const char *s) {
    size_t slen = strlen(s);
    if (pos + slen >= cap) return cap;  /* signal overflow by returning cap (caller checks) */
    memcpy(out + pos, s, slen);
    return pos + slen;
}

static size_t append_json_escaped(char *out, size_t cap, size_t pos, const char *s) {
    pos = append_str(out, cap, pos, "\"");
    if (pos >= cap) return cap;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char buf[8];
        size_t n;
        if (*p == '"' || *p == '\\') { buf[0] = '\\'; buf[1] = (char)*p; n = 2; }
        else if (*p == '\n') { buf[0] = '\\'; buf[1] = 'n'; n = 2; }
        else if (*p < 0x20) { snprintf(buf, sizeof(buf), "\\u%04x", *p); n = strlen(buf); }
        else { buf[0] = (char)*p; n = 1; }
        if (pos + n >= cap) return cap;
        memcpy(out + pos, buf, n);
        pos += n;
    }
    if (pos + 1 >= cap) return cap;
    out[pos++] = '"';
    return pos;
}

static size_t append_field_str(char *out, size_t cap, size_t pos, const char *key,
                                const char *value, bool first) {
    if (!first) pos = append_str(out, cap, pos, ",");
    if (pos >= cap) return cap;
    pos = append_str(out, cap, pos, "\"");
    if (pos >= cap) return cap;
    pos = append_str(out, cap, pos, key);
    if (pos >= cap) return cap;
    pos = append_str(out, cap, pos, "\":");
    if (pos >= cap) return cap;
    if (value) pos = append_json_escaped(out, cap, pos, value);
    else pos = append_str(out, cap, pos, "null");
    return pos;
}

static size_t append_field_uint(char *out, size_t cap, size_t pos, const char *key,
                                 uint64_t value, bool first) {
    if (!first) pos = append_str(out, cap, pos, ",");
    if (pos >= cap) return cap;
    pos = append_str(out, cap, pos, "\"");
    if (pos >= cap) return cap;
    pos = append_str(out, cap, pos, key);
    if (pos >= cap) return cap;
    pos = append_str(out, cap, pos, "\":");
    if (pos >= cap) return cap;
    char numbuf[32];
    snprintf(numbuf, sizeof(numbuf), "%llu", (unsigned long long)value);
    pos = append_str(out, cap, pos, numbuf);
    return pos;
}

size_t gcusb_format_ndjson_line(const gcusb_log_event_t *ev, char *out, size_t out_cap) {
    if (!ev || !out || out_cap == 0) return 0;
    size_t pos = 0;
    pos = append_str(out, out_cap, pos, "{");
    if (pos >= out_cap) return 0;
    pos = append_field_uint(out, out_cap, pos, "ts_us", ev->ts_us, true);
    if (pos >= out_cap) return 0;
    pos = append_field_str(out, out_cap, pos, "target", ev->target, false);
    if (pos >= out_cap) return 0;
    pos = append_field_str(out, out_cap, pos, "iface", ev->iface, false);
    if (pos >= out_cap) return 0;
    pos = append_field_str(out, out_cap, pos, "xfer_type", ev->xfer_type, false);
    if (pos >= out_cap) return 0;
    pos = append_field_str(out, out_cap, pos, "direction", ev->direction, false);
    if (pos >= out_cap) return 0;
    pos = append_field_str(out, out_cap, pos, "setup_hex", ev->setup_hex, false);
    if (pos >= out_cap) return 0;
    pos = append_field_str(out, out_cap, pos, "req_hex", ev->req_hex, false);
    if (pos >= out_cap) return 0;
    pos = append_field_str(out, out_cap, pos, "resp_hex", ev->resp_hex, false);
    if (pos >= out_cap) return 0;
    pos = append_field_str(out, out_cap, pos, "status", ev->status, false);
    if (pos >= out_cap) return 0;
    pos = append_field_uint(out, out_cap, pos, "elapsed_us", ev->elapsed_us, false);
    if (pos >= out_cap) return 0;
    pos = append_str(out, out_cap, pos, "}");
    if (pos >= out_cap) return 0;
    if (pos >= out_cap) return 0;
    out[pos] = '\0';
    return pos;
}

/* ------------------------------------------------------------------------------------------- */

static void trim(const char *line, size_t line_len, const char **start, size_t *len) {
    size_t s = 0, e = line_len;
    while (e > 0 && (line[e - 1] == '\n' || line[e - 1] == '\r' || line[e - 1] == ' ' ||
                     line[e - 1] == '\t' || line[e - 1] == '\0'))
        e--;
    while (s < e && (line[s] == ' ' || line[s] == '\t')) s++;
    *start = line + s;
    *len = e - s;
}

static bool token_is(const char *s, size_t len, const char *word) {
    size_t wl = strlen(word);
    return len == wl && strncmp(s, word, wl) == 0;
}

void gcusb_parse_script_line(const char *line, size_t line_len, gcusb_script_line_t *out) {
    memset(out, 0, sizeof(*out));
    if (!line || !out) return;

    const char *s;
    size_t len;
    trim(line, line_len, &s, &len);

    if (len == 0 || s[0] == '#') {
        out->kind = GCUSB_SCRIPT_LINE_BLANK;
        return;
    }

    /* Find first whitespace to split command word from argument. */
    size_t cmd_len = 0;
    while (cmd_len < len && s[cmd_len] != ' ' && s[cmd_len] != '\t') cmd_len++;

    if (token_is(s, cmd_len, "STOP-RUMBLE")) {
        out->kind = GCUSB_SCRIPT_LINE_STOP_RUMBLE;
        return;
    }

    size_t arg_start = cmd_len;
    while (arg_start < len && (s[arg_start] == ' ' || s[arg_start] == '\t')) arg_start++;
    const char *arg = s + arg_start;
    size_t arg_len = len - arg_start;

    if (token_is(s, cmd_len, "SLEEP")) {
        if (arg_len == 0) { out->kind = GCUSB_SCRIPT_LINE_INVALID; return; }
        uint32_t ms = 0;
        for (size_t i = 0; i < arg_len; i++) {
            if (arg[i] < '0' || arg[i] > '9') { out->kind = GCUSB_SCRIPT_LINE_INVALID; return; }
            ms = ms * 10 + (uint32_t)(arg[i] - '0');
        }
        out->kind = GCUSB_SCRIPT_LINE_SLEEP;
        out->sleep_ms = ms;
        return;
    }

    if (token_is(s, cmd_len, "SEND")) {
        if (arg_len == 0 || (arg_len % 2) != 0 || (arg_len / 2) > sizeof(out->bytes)) {
            out->kind = GCUSB_SCRIPT_LINE_INVALID;
            return;
        }
        uint32_t count = 0;
        for (size_t i = 0; i + 1 < arg_len + 1 && i < arg_len; i += 2) {
            int hi = hexval(arg[i]);
            int lo = hexval(arg[i + 1]);
            if (hi < 0 || lo < 0) { out->kind = GCUSB_SCRIPT_LINE_INVALID; return; }
            out->bytes[count++] = (uint8_t)((hi << 4) | lo);
        }
        out->kind = GCUSB_SCRIPT_LINE_SEND;
        out->byte_count = count;
        out->cmd_class = gcusb_classify_command(out->bytes, count);
        out->allowlisted = (out->cmd_class == GCUSB_CMD_ALLOWED);
        return;
    }

    out->kind = GCUSB_SCRIPT_LINE_INVALID;
}
