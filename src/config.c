// Persistent settings + configuration-mode command protocol over USB CDC serial.
//
// Settings live in one flash sector placed safely below btstack's own flash
// region (it uses the last 2-3 sectors depending on the chip). The flash write
// is performed on core1 (which already owns the multicore-lockout requester role
// used for BOOTSEL), so it can park core0 during the erase/program without any
// risk of a bidirectional lockout.

#include "config.h"
#include "ns2_remap.h"   // NS2_FAM_COUNT / NS2_SRC_COUNT / NS2_DST_*
#include "report.h"      // get_global_raw_buttons / get_global_gamepad_input (live view)
#include "switch_pro.h"  // switch_pro_input_t
#include "switch_pro2.h" // ns2_dbg_* getters (report-0x09 motion/gyro debug instrumentation)
#include "sw2_capture.h" // genuine Switch 2 BLE raw-traffic capture/export (2026-07-10)

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>

#include "tusb.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define CONFIG_MAGIC 0x50535731u  // 'PSW1'
#define CONFIG_VERSION 5
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 4 * FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t lightbar[4][3];                         // per-player-position R,G,B
    uint8_t ns2_map[NS2_FAM_COUNT][NS2_SRC_COUNT];  // per-family button remap
} pico_config_t;

// Built-in joypad remap (source index -> NS2_DST_*). Reproduces the seam's hardcoded
// mapping exactly, so an all-defaults config behaves identically to no remapping.
// Order matches SRC_TO_JP[] in ns2_seam.c: B1 B2 B3 B4 L1 R1 L2 R2 S1 S2 L3 R3
// DU DD DL DR A1 A2 A3 A4 L4 R4 A5 L5 R5.
static const uint8_t NS2_DEFAULT_MAP[NS2_SRC_COUNT] = {
    NS2_DST_B, NS2_DST_A, NS2_DST_Y, NS2_DST_X,
    NS2_DST_L, NS2_DST_R, NS2_DST_ZL, NS2_DST_ZR,
    NS2_DST_MINUS, NS2_DST_PLUS, NS2_DST_L3, NS2_DST_R3,
    NS2_DST_DUP, NS2_DST_DDOWN, NS2_DST_DLEFT, NS2_DST_DRIGHT,
    NS2_DST_HOME, NS2_DST_CAPTURE, NS2_DST_C, NS2_DST_CAPTURE,
    NS2_DST_GL, NS2_DST_GR, NS2_DST_C, NS2_DST_GL, NS2_DST_GR,
};

_Static_assert(sizeof(pico_config_t) <= FLASH_PAGE_SIZE, "config must fit in one flash page");

static pico_config_t cfg;
static critical_section_t cfg_lock;
static volatile bool save_requested;

static void load_defaults(void) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC;
    cfg.version = CONFIG_VERSION;
    static const uint8_t def[4][3] = {{0, 0, 255}, {255, 0, 0}, {0, 255, 0}, {255, 192, 0}};
    memcpy(cfg.lightbar, def, sizeof(def));
    for (int fam = 0; fam < NS2_FAM_COUNT; fam++)
        memcpy(cfg.ns2_map[fam], NS2_DEFAULT_MAP, NS2_SRC_COUNT);
}

void config_load(void) {
    critical_section_init(&cfg_lock);
    const uint8_t *flash = (const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    const pico_config_t *f = (const pico_config_t *)flash;
    if (f->magic == CONFIG_MAGIC && f->version == CONFIG_VERSION) {
        memcpy(&cfg, f, sizeof(cfg));
    } else if (f->magic == CONFIG_MAGIC) {
        // Any older layout (pre-v5 carried the now-removed bluepad32 button_map):
        // the lightbar is at a stable offset, so keep it and default the remap.
        load_defaults();
        memcpy(cfg.lightbar, f->lightbar, sizeof(cfg.lightbar));
    } else {
        load_defaults();
    }
}

void config_get_lightbar(uint8_t player, uint8_t rgb[3]) {
    critical_section_enter_blocking(&cfg_lock);
    if (player < 4) {
        rgb[0] = cfg.lightbar[player][0];
        rgb[1] = cfg.lightbar[player][1];
        rgb[2] = cfg.lightbar[player][2];
    } else {
        rgb[0] = rgb[1] = rgb[2] = 0;
    }
    critical_section_exit(&cfg_lock);
}

static void set_lightbar(uint8_t player, uint8_t r, uint8_t g, uint8_t b) {
    if (player >= 4)
        return;
    critical_section_enter_blocking(&cfg_lock);
    cfg.lightbar[player][0] = r;
    cfg.lightbar[player][1] = g;
    cfg.lightbar[player][2] = b;
    critical_section_exit(&cfg_lock);
}

void config_get_ns2_map(uint8_t family, uint8_t map_out[]) {
    if (family >= NS2_FAM_COUNT)
        family = NS2_FAM_COUNT - 1;  // generic
    critical_section_enter_blocking(&cfg_lock);
    memcpy(map_out, cfg.ns2_map[family], NS2_SRC_COUNT);
    critical_section_exit(&cfg_lock);
}

static void set_ns2_map(uint8_t family, const uint8_t map_in[]) {
    if (family >= NS2_FAM_COUNT)
        return;
    critical_section_enter_blocking(&cfg_lock);
    memcpy(cfg.ns2_map[family], map_in, NS2_SRC_COUNT);
    critical_section_exit(&cfg_lock);
}

void config_service_save(void) {
    if (!save_requested)
        return;

    pico_config_t snap;
    critical_section_enter_blocking(&cfg_lock);
    snap = cfg;
    critical_section_exit(&cfg_lock);

    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &snap, sizeof(snap));

    // Park core0 (USB) so it can't touch flash, then erase+program with our
    // interrupts off (an ISR could otherwise execute from now-disabled flash).
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();

    save_requested = false;
}

//--------------------------------------------------------------------+
// CDC command protocol
//--------------------------------------------------------------------+

#define LINE_MAX 128
static char line[LINE_MAX];
static uint16_t line_len;
// 4096: sized for "sw2cap drain"'s batch reply (up to SW2CAP_DRAIN_MAX=16 entries, each up to
// ~235 B with 64 hex-encoded payload bytes) — the largest reply this protocol produces. Also
// comfortably covers "imuanom" (30 hex-encoded motion bytes + a 4-entry trail + per-axis
// context), which no longer fits in the 256 B every simpler reply uses.
static char out[4096];

static void reply(const char *s) {
    tud_cdc_write_str(s);
    tud_cdc_write_str("\r\n");
    tud_cdc_write_flush();
}

static void cmd_get(void) {
    uint8_t lb[4][3];
    critical_section_enter_blocking(&cfg_lock);
    memcpy(lb, cfg.lightbar, sizeof(lb));
    critical_section_exit(&cfg_lock);
    snprintf(out, sizeof(out),
             "{\"lightbar\":[[%u,%u,%u],[%u,%u,%u],[%u,%u,%u],[%u,%u,%u]]}",
             lb[0][0], lb[0][1], lb[0][2], lb[1][0], lb[1][1], lb[1][2],
             lb[2][0], lb[2][1], lb[2][2], lb[3][0], lb[3][1], lb[3][2]);
    reply(out);
}

// Per-family remap (NS2_SRC_COUNT entries).
static void cmd_getns2map(int family) {
    if (family < 0 || family >= NS2_FAM_COUNT) {
        reply("{\"error\":\"bad family\"}");
        return;
    }
    uint8_t m[NS2_SRC_COUNT];
    config_get_ns2_map((uint8_t)family, m);
    int n = snprintf(out, sizeof(out), "{\"map\":[");
    for (int i = 0; i < NS2_SRC_COUNT; i++)
        n += snprintf(out + n, sizeof(out) - n, "%s%u", i ? "," : "", m[i]);
    snprintf(out + n, sizeof(out) - n, "]}");
    reply(out);
}

// Parse "setns2map <family> d0 d1 ... d24" and store the family's joypad map.
static void cmd_setns2map(char *args) {
    char *p = args, *end;
    long family = strtol(p, &end, 10);
    if (end == p || family < 0 || family >= NS2_FAM_COUNT) {
        reply("{\"error\":\"bad family\"}");
        return;
    }
    p = end;
    uint8_t m[NS2_SRC_COUNT];
    for (int i = 0; i < NS2_SRC_COUNT; i++) {
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v >= NS2_DST_COUNT) {
            reply("{\"error\":\"bad map\"}");
            return;
        }
        m[i] = (uint8_t)v;
        p = end;
    }
    set_ns2_map((uint8_t)family, m);
    reply("{\"ok\":true}");
}

// Live input snapshot for the config-mode 2-column view: the connected controller's
// raw buttons (unified JP_BUTTON_* bitmap) and the resulting Switch 2 output (the three
// Pro-Controller button bytes + the C/GL/GR extras). Slot 0 (single-controller milestone).
static void cmd_state(void) {
    uint32_t raw = get_global_raw_buttons(0);
    switch_pro_input_t in;
    get_global_gamepad_input(0, &in);
    snprintf(out, sizeof(out), "{\"raw\":%lu,\"out\":[%u,%u,%u,%u]}",
             (unsigned long)raw, in.buttons[0], in.buttons[1], in.buttons[2], in.extra);
    reply(out);
}

// Connected controller identity for the "Current Input Type" panel. Slot 0.
static void cmd_device(void) {
    char name[40];
    uint16_t vid = 0, pid = 0;
    get_global_device(0, name, sizeof(name), &vid, &pid);
    // Escape the SDP-supplied name for JSON (quotes, backslashes, control chars).
    char esc[96];
    int j = 0;
    for (int i = 0; name[i] && j < (int)sizeof(esc) - 2; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '"' || c == '\\')
            esc[j++] = '\\';
        esc[j++] = (c < 0x20) ? ' ' : (char)c;
    }
    esc[j] = '\0';
    snprintf(out, sizeof(out), "{\"name\":\"%s\",\"vid\":%u,\"pid\":%u}", esc, vid, pid);
    reply(out);
}

// Raw HID report of the connected controller (hex) for the debug view. Lets us
// reverse-engineer inputs a driver doesn't parse yet (e.g. Xbox Elite paddles).
static void cmd_raw(void) {
    uint8_t buf[RAW_REPORT_BYTES];
    uint16_t n = get_global_raw_report(0, buf, sizeof(buf));
    int j = snprintf(out, sizeof(out), "{\"len\":%u,\"bytes\":\"", n);
    for (uint16_t i = 0; i < n && j < (int)sizeof(out) - 4; i++)
        j += snprintf(out + j, sizeof(out) - j, "%02x", buf[i]);
    snprintf(out + j, sizeof(out) - j, "\"}");
    reply(out);
}

// Debug: the live IMU state the report builder consumes (has_motion + accel/gyro from
// the shared cross-core state) plus the USB report state (active report id, streaming,
// and the motion-length report 0x09 last emitted). Bisects the gyro pipeline: if accel/gyro
// move live here, the break is downstream (report format / Steam); if frozen, it's upstream.
// (Getter prototypes come from switch_pro2.h; bias/still added 2026-07-10 for the stillness-gate
// hardware check, phase added the same day after the symptom was reclassified from gradual drift
// to abrupt jumps, anom added the same day again for the mathematically-derived discontinuity
// detector — see switch_pro2.c's NS2_MAX_PHASE_DELTA derivation.)
static void cmd_imu(void) {
    switch_pro_input_t in;
    get_global_gamepad_input(0, &in);
    uint8_t rid = 0, st = 0, mlen = 0;
    ns2_dbg_report_state(&rid, &st, &mlen);
    int32_t bias[3] = {0, 0, 0};
    uint8_t still = 0;
    ns2_dbg_motion_bias(bias, &still);
    int32_t phase[3] = {0, 0, 0};
    ns2_dbg_motion_phase(phase);
    uint32_t anom_seq = ns2_dbg_motion_anomaly()->seq;
    snprintf(out, sizeof(out),
             "{\"hm\":%u,\"a\":[%d,%d,%d],\"g\":[%d,%d,%d],\"rid\":%u,\"stream\":%u,\"mlen\":%u,"
             "\"bias\":[%ld,%ld,%ld],\"still\":%u,\"phase\":[%ld,%ld,%ld],\"anom\":%lu}",
             in.has_motion, in.accel[0], in.accel[1], in.accel[2],
             in.gyro[0], in.gyro[1], in.gyro[2], rid, st, mlen,
             (long)bias[0], (long)bias[1], (long)bias[2], still,
             (long)phase[0], (long)phase[1], (long)phase[2], (unsigned long)anom_seq);
    reply(out);
}

// Full context for the most recent report-0x09 phase-discontinuity anomaly (see
// switch_pro2.c's NS2_MAX_PHASE_DELTA — a bound derived from the encoder's own arithmetic
// limits, not a heuristic threshold). Call after `imu`'s "anom" count is nonzero; the reply is
// large (trail + 30 motion bytes), so it is its own command rather than folded into the
// frequently-polled `imu` line.
static void cmd_imuanom(void) {
    const ns2_anom_capture_t *a = ns2_dbg_motion_anomaly();
    if (!a->valid) {
        reply("{\"valid\":0}");
        return;
    }
    int j = snprintf(out, sizeof(out),
        "{\"valid\":1,\"seq\":%lu,\"gyro\":[%d,%d,%d],\"accel\":[%d,%d,%d],"
        "\"g\":[%ld,%ld,%ld],\"bias\":[%ld,%ld,%ld],\"still\":%u,\"dt_us\":%lu,"
        "\"phase_before\":[%ld,%ld,%ld],\"phase_after\":[%ld,%ld,%ld],\"delta\":[%ld,%ld,%ld],"
        "\"imu_tick\":%u,\"tick_count\":%u,\"imu_enabled\":%u,\"motion_len\":%u,\"bytes\":\"",
        (unsigned long)a->seq, a->gyro[0], a->gyro[1], a->gyro[2],
        a->accel[0], a->accel[1], a->accel[2],
        (long)a->g[0], (long)a->g[1], (long)a->g[2],
        (long)a->bias[0], (long)a->bias[1], (long)a->bias[2], a->still, (unsigned long)a->dt_us,
        (long)a->phase_before[0], (long)a->phase_before[1], (long)a->phase_before[2],
        (long)a->phase_after[0], (long)a->phase_after[1], (long)a->phase_after[2],
        (long)a->delta[0], (long)a->delta[1], (long)a->delta[2],
        a->imu_tick, a->tick_count, a->imu_enabled, a->motion_len);
    for (int i = 0; i < 30 && j < (int)sizeof(out) - 6; i++)
        j += snprintf(out + j, sizeof(out) - j, "%02x", a->motion_bytes[i]);
    j += snprintf(out + j, sizeof(out) - j, "\",\"trail\":[");
    for (int i = 0; i < NS2_ANOM_TRAIL && j < (int)sizeof(out) - 96; i++) {
        const ns2_anom_trail_t *t = &a->trail[i];
        j += snprintf(out + j, sizeof(out) - j,
            "%s{\"gyro\":[%d,%d,%d],\"delta\":[%ld,%ld,%ld],\"still\":%u,\"dt_us\":%lu}",
            i ? "," : "", t->gyro[0], t->gyro[1], t->gyro[2],
            (long)t->delta[0], (long)t->delta[1], (long)t->delta[2],
            t->still, (unsigned long)t->dt_us);
    }
    snprintf(out + j, sizeof(out) - j, "]}");
    reply(out);
}

// Genuine Switch 2 BLE raw-traffic capture (2026-07-10) — see sw2_capture.h. Off by default;
// `sw2cap on` starts a fresh session (clears the ring + drop counter), `sw2cap off` stops it,
// `sw2cap stat` reports whether it's running and how many entries have been dropped (ring
// overrun — meaningful only as "something didn't get read fast enough", not a data-quality
// signal about the controller itself), `sw2cap drain` pops up to SW2CAP_DRAIN_MAX buffered
// entries into one JSON reply (plus the same capturing/dropped fields `stat` reports, so a
// client can poll `drain` alone and get full status + data in one round trip). Pull-based by
// design — see sw2_capture.h's revision note on why this replaced auto-streaming.
//
// `sw2cap gattdisc on|off|stat` controls the separate, off-by-default one-shot GATT discovery
// tool (see sw2_capture.h) — ground truth for raw ATT handle numbering.
//
// `sw2cap variant <0-6>` arms one of the v2 feature-enable experiment's six variants (0 = off);
// `sw2cap variant stat` reports which is currently armed (see sw2_capture.h for what each
// variant does). Independent of plain capture on/off, so a session can capture normal traffic
// first and only arm an experiment when explicitly asked. Do not arm gattdisc and a variant in
// the same session (see sw2_capture.h).
#define SW2CAP_DRAIN_MAX 16
static void cmd_sw2cap(const char *arg) {
    if (strcmp(arg, "on") == 0) {
        sw2_capture_set_enabled(true);
        reply("{\"ok\":true,\"capturing\":true}");
    } else if (strcmp(arg, "off") == 0) {
        sw2_capture_set_enabled(false);
        reply("{\"ok\":true,\"capturing\":false}");
    } else if (strcmp(arg, "stat") == 0) {
        snprintf(out, sizeof(out), "{\"capturing\":%s,\"dropped\":%lu}",
                 sw2_capture_get_enabled() ? "true" : "false",
                 (unsigned long)sw2_capture_dropped_count());
        reply(out);
    } else if (strcmp(arg, "drain") == 0) {
        int j = snprintf(out, sizeof(out), "{\"capturing\":%s,\"dropped\":%lu,\"entries\":[",
                          sw2_capture_get_enabled() ? "true" : "false",
                          (unsigned long)sw2_capture_dropped_count());
        sw2_cap_entry_t e;
        int n = 0;
        bool more = false;
        while (n < SW2CAP_DRAIN_MAX) {
            if (!sw2_capture_drain_one(&e)) break;
            int entry_j = snprintf(out + j, sizeof(out) - j,
                "%s{\"us\":%llu,\"kind\":\"%s\",\"handle\":\"0x%04X\",\"len\":%u,\"orig_len\":%u,\"bytes\":\"",
                n ? "," : "", (unsigned long long)e.us, sw2_capture_kind_name(e.kind),
                e.handle, e.len, e.orig_len);
            j += entry_j;
            for (int i = 0; i < e.len && j < (int)sizeof(out) - 8; i++)
                j += snprintf(out + j, sizeof(out) - j, "%02x", e.data[i]);
            j += snprintf(out + j, sizeof(out) - j, "\"}");
            n++;
        }
        // Report whether more remain buffered right now (for adaptive client polling), without
        // consuming an entry to check — has_more would need its own peek; cheaply approximated
        // by "we stopped because we hit the cap, not because the ring was empty" is not quite
        // right either, so just try one more non-destructive check via a drain+not-found probe
        // is unnecessary complexity here: SW2CAP_DRAIN_MAX draining every ~40ms comfortably
        // outpaces realistic BLE notification rates (see the protocol inventory doc), so treat
        // "hit the cap" as the practical signal to poll again immediately.
        more = (n == SW2CAP_DRAIN_MAX);
        snprintf(out + j, sizeof(out) - j, "],\"more\":%s}", more ? "true" : "false");
        reply(out);
    } else if (strcmp(arg, "gattdisc on") == 0) {
        sw2_set_gatt_discovery_enabled(true);
        reply("{\"ok\":true,\"gattdisc\":true}");
    } else if (strcmp(arg, "gattdisc off") == 0) {
        sw2_set_gatt_discovery_enabled(false);
        reply("{\"ok\":true,\"gattdisc\":false}");
    } else if (strcmp(arg, "gattdisc stat") == 0) {
        snprintf(out, sizeof(out), "{\"gattdisc\":%s}",
                 sw2_get_gatt_discovery_enabled() ? "true" : "false");
        reply(out);
    } else if (strcmp(arg, "variant stat") == 0) {
        snprintf(out, sizeof(out), "{\"variant\":%d}", sw2_get_v2_variant());
        reply(out);
    } else if (strncmp(arg, "variant ", 8) == 0) {
        int n = atoi(arg + 8);
        if (n < 0 || n > 6) {
            reply("{\"error\":\"variant must be 0-6 (0=off)\"}");
        } else {
            sw2_set_v2_variant((uint8_t)n);
            snprintf(out, sizeof(out), "{\"ok\":true,\"variant\":%d}", n);
            reply(out);
        }
    } else if (strncmp(arg, "mark ", 5) == 0) {
        // Capture-session annotation (see sw2_capture.h's sw2_capture_mark()) -- a pure logging
        // call, does not touch the BLE connection/init/report path at all. Truncated to
        // SW2_CAP_MAX_DATA (64 bytes) same as every other capture entry's payload.
        const char *label = arg + 5;
        size_t len = strlen(label);
        if (len > 64) len = 64;
        sw2_capture_mark((const uint8_t *)label, (uint16_t)len);
        snprintf(out, sizeof(out), "{\"ok\":true,\"marked\":\"%.64s\"}", label);
        reply(out);
    } else {
        reply("{\"error\":\"usage: sw2cap on|off|stat|drain|gattdisc on|off|stat|variant <0-6>|variant stat|mark <text>\"}");
    }
}

static void handle_line(char *cmd) {
    if (strcmp(cmd, "info") == 0) {
        reply("{\"id\":\"picoswitch\",\"product\":\"PicoSwitch Config\",\"version\":\"2.0\"}");
    } else if (strcmp(cmd, "ping") == 0) {
        reply("{\"ok\":true}");
    } else if (strcmp(cmd, "get") == 0) {
        cmd_get();
    } else if (strcmp(cmd, "state") == 0) {
        cmd_state();
    } else if (strcmp(cmd, "device") == 0) {
        cmd_device();
    } else if (strcmp(cmd, "raw") == 0) {
        cmd_raw();
    } else if (strcmp(cmd, "imu") == 0) {
        cmd_imu();
    } else if (strcmp(cmd, "imuanom") == 0) {
        cmd_imuanom();
    } else if (strncmp(cmd, "sw2cap ", 7) == 0) {
        cmd_sw2cap(cmd + 7);
    } else if (strncmp(cmd, "getns2map ", 10) == 0) {
        cmd_getns2map(atoi(cmd + 10));
    } else if (strncmp(cmd, "setns2map ", 10) == 0) {
        cmd_setns2map(cmd + 10);
    } else if (strncmp(cmd, "lb ", 3) == 0) {
        int p, r, g, b;
        if (sscanf(cmd + 3, "%d %d %d %d", &p, &r, &g, &b) == 4 && p >= 0 && p < 4 && r >= 0 &&
            r < 256 && g >= 0 && g < 256 && b >= 0 && b < 256) {
            set_lightbar((uint8_t)p, (uint8_t)r, (uint8_t)g, (uint8_t)b);
            reply("{\"ok\":true}");
        } else {
            reply("{\"error\":\"bad args\"}");
        }
    } else if (strcmp(cmd, "save") == 0) {
        save_requested = true;
        // Wait (pumping USB) for core1's control tick to perform the flash write.
        absolute_time_t deadline = make_timeout_time_ms(2000);
        while (save_requested && !time_reached(deadline))
            tud_task();
        reply(save_requested ? "{\"error\":\"save timeout\"}" : "{\"ok\":true}");
    } else {
        reply("{\"error\":\"unknown command\"}");
    }
}

void config_cdc_task(void) {
    while (tud_cdc_available()) {
        int32_t c = tud_cdc_read_char();
        if (c < 0)
            break;
        if (c == '\n' || c == '\r') {
            if (line_len > 0) {
                line[line_len] = '\0';
                handle_line(line);
                line_len = 0;
            }
        } else if (line_len < LINE_MAX - 1) {
            line[line_len++] = (char)c;
        }
    }
    // BLE capture entries (see sw2_capture.h) are pulled explicitly via `sw2cap drain`, not
    // auto-streamed here — a client (the web UI) polls it like any other command.
}
