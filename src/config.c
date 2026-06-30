// Persistent settings + configuration-mode command protocol over USB CDC serial.
//
// Settings live in one flash sector placed safely below bluepad32/btstack's own
// flash region (it uses the last 2-3 sectors depending on the chip). The flash
// write is performed on core1 (which already owns the multicore-lockout requester
// role used for BOOTSEL), so it can park core0 during the erase/program without
// any risk of a bidirectional lockout.

#include "config.h"
#include "remap.h"

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
#define CONFIG_VERSION 3
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 4 * FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t lightbar[4][3];                       // per-player-position R,G,B
    uint8_t button_map[FAMILY_COUNT][SRC_COUNT];  // per-platform remap (v3)
} pico_config_t;

// Default position-faithful map (reproduces the built-in behaviour).
static const uint8_t REMAP_DEFAULT[SRC_COUNT] = {
    [SRC_SOUTH] = DST_B, [SRC_EAST] = DST_A, [SRC_WEST] = DST_Y, [SRC_NORTH] = DST_X,
    [SRC_L] = DST_L, [SRC_R] = DST_R, [SRC_ZL] = DST_ZL, [SRC_ZR] = DST_ZR,
    [SRC_L3] = DST_L3, [SRC_R3] = DST_R3,
    [SRC_MINUS] = DST_MINUS, [SRC_PLUS] = DST_PLUS, [SRC_HOME] = DST_HOME, [SRC_CAPTURE] = DST_CAPTURE,
    [SRC_DPAD_UP] = DST_DPAD_UP, [SRC_DPAD_DOWN] = DST_DPAD_DOWN,
    [SRC_DPAD_LEFT] = DST_DPAD_LEFT, [SRC_DPAD_RIGHT] = DST_DPAD_RIGHT,
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
    for (int fam = 0; fam < FAMILY_COUNT; fam++)
        memcpy(cfg.button_map[fam], REMAP_DEFAULT, SRC_COUNT);
}

void config_load(void) {
    critical_section_init(&cfg_lock);
    const uint8_t *flash = (const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    const pico_config_t *f = (const pico_config_t *)flash;
    if (f->magic == CONFIG_MAGIC && f->version == CONFIG_VERSION) {
        memcpy(&cfg, f, sizeof(cfg));
    } else if (f->magic == CONFIG_MAGIC && f->version == 2) {
        // v2 had a single shared map (at the same offset as family 0's map):
        // keep colours and apply that map to every family.
        load_defaults();
        memcpy(cfg.lightbar, f->lightbar, sizeof(cfg.lightbar));
        const uint8_t *v2map = flash + offsetof(pico_config_t, button_map);
        for (int fam = 0; fam < FAMILY_COUNT; fam++)
            memcpy(cfg.button_map[fam], v2map, SRC_COUNT);
    } else if (f->magic == CONFIG_MAGIC && f->version == 1) {
        // v1 had lightbar only: keep colours, default the maps.
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

void config_get_button_map(uint8_t family, uint8_t map_out[]) {
    if (family >= FAMILY_COUNT)
        family = FAMILY_GENERIC;
    critical_section_enter_blocking(&cfg_lock);
    memcpy(map_out, cfg.button_map[family], SRC_COUNT);
    critical_section_exit(&cfg_lock);
}

static void set_button_map(uint8_t family, const uint8_t map_in[]) {
    if (family >= FAMILY_COUNT)
        return;
    critical_section_enter_blocking(&cfg_lock);
    memcpy(cfg.button_map[family], map_in, SRC_COUNT);
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
static char out[256];

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

static void cmd_getmap(int family) {
    if (family < 0 || family >= FAMILY_COUNT) {
        reply("{\"error\":\"bad family\"}");
        return;
    }
    uint8_t m[SRC_COUNT];
    critical_section_enter_blocking(&cfg_lock);
    memcpy(m, cfg.button_map[family], sizeof(m));
    critical_section_exit(&cfg_lock);
    int n = snprintf(out, sizeof(out), "{\"map\":[");
    for (int i = 0; i < SRC_COUNT; i++)
        n += snprintf(out + n, sizeof(out) - n, "%s%u", i ? "," : "", m[i]);
    snprintf(out + n, sizeof(out) - n, "]}");
    reply(out);
}

// Parse "setmap <family> d0 d1 ... d17" and store the family's map.
static void cmd_setmap(char *args) {
    char *p = args, *end;
    long family = strtol(p, &end, 10);
    if (end == p || family < 0 || family >= FAMILY_COUNT) {
        reply("{\"error\":\"bad family\"}");
        return;
    }
    p = end;
    uint8_t m[SRC_COUNT];
    for (int i = 0; i < SRC_COUNT; i++) {
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v >= DST_COUNT) {
            reply("{\"error\":\"bad map\"}");
            return;
        }
        m[i] = (uint8_t)v;
        p = end;
    }
    set_button_map((uint8_t)family, m);
    reply("{\"ok\":true}");
}

static void handle_line(char *cmd) {
    if (strcmp(cmd, "info") == 0) {
        reply("{\"id\":\"picoswitch\",\"product\":\"PicoSwitch Config\",\"version\":\"2.0\"}");
    } else if (strcmp(cmd, "ping") == 0) {
        reply("{\"ok\":true}");
    } else if (strcmp(cmd, "get") == 0) {
        cmd_get();
    } else if (strncmp(cmd, "getmap ", 7) == 0) {
        cmd_getmap(atoi(cmd + 7));
    } else if (strncmp(cmd, "setmap ", 7) == 0) {
        cmd_setmap(cmd + 7);
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
}
