// Persistent settings + configuration-mode command protocol.
//
// Settings live in one flash sector placed safely below btstack's own flash
// region (it uses the last 2-3 sectors depending on the chip). The flash write
// is performed on core1 (which already owns the multicore-lockout requester role
// used for BOOTSEL), so it can park core0 during the erase/program without any
// risk of a bidirectional lockout. Commands execute on core0 whether they arrive
// over USB CDC or the Config-only BLE bridge; this preserves that ownership and
// keeps parsing/flash waits out of BTstack callbacks.

#include "config.h"
#include "report.h"      // get_global_raw_buttons / get_global_gamepad_input (live view)
#include "switch_pro.h"  // switch_pro_input_t
#include "switch_pro2.h" // ns2_dbg_* getters (report-0x09 motion/gyro debug instrumentation)
#include "ns2_firmware_profile.h" // firmware prompt read-address diagnostics
#include "sw2_capture.h" // genuine Switch 2 BLE raw-traffic capture/export (2026-07-10)
#include "bt_identity_log.h" // controller identity/driver-binding event log (Gate 2, 2026-07-12)
#include "bt/bthid/bthid.h" // bthid_get_cached_descriptor (btid desc command)
#include "ds5_audio_bridge.h" // DualSense audio stall diagnostics
#include "bt/bthid/devices/generic/bthid_gamepad.h" // bthid_gamepad_dump_map (btid desc command)
#include "virtual_amiibo_store.h"
#include "config_wireless_bridge.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>

#include "tusb.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "hardware/flash.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"

#define CONFIG_MAGIC 0x50535731u  // 'PSW1'
#define CONFIG_VERSION 10
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 4 * FLASH_SECTOR_SIZE)
#define PERSISTENT_FLASH_START \
    (PICO_FLASH_SIZE_BYTES - 5u * FLASH_SECTOR_SIZE)
#define PERSISTENT_FLASH_SIZE (5u * FLASH_SECTOR_SIZE)
#define CONFIG_WAKE_VALID 0xA5
#define CONFIG_WAKE_SAVE_DELAY_MS 5000
#define INSTALL_MARKER_LENGTH 19u

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t body_color[3];                          // Pro2 body/lightbar R,G,B
    uint8_t joycon2_left_accent[3];                 // Joy-Con 2 L highlight/lightbar
    uint8_t joycon2_right_accent[3];                // Joy-Con 2 R highlight/lightbar
    uint8_t wake_valid;
    config_wake_identity_t wake_identity;
} pico_config_t;

// Every UF2 contains this pending marker in its own dedicated flash page.
// First boot consumes it with a 1->0 page program after erasing all five
// PicoSwitch2 persistence sectors. Reflashing even the same UF2 rewrites the
// application sector and restores the pending marker, while an ordinary reboot
// leaves the consumed page untouched.
static const volatile uint8_t firmware_install_marker[FLASH_PAGE_SIZE]
    __attribute__((aligned(FLASH_PAGE_SIZE),
                   section(".rodata.install_marker"), used)) =
    {'P', 'S', '2', '-', 'I', 'N', 'S', 'T', 'A', 'L',
     'L', '-', 'R', 'E', 'S', 'E', 'T', '-', '1'};

_Static_assert(sizeof(pico_config_t) <= FLASH_PAGE_SIZE, "config must fit in one flash page");
_Static_assert(sizeof(firmware_install_marker) == FLASH_PAGE_SIZE,
               "install marker must occupy one flash page");
_Static_assert(INSTALL_MARKER_LENGTH < FLASH_PAGE_SIZE,
               "install marker magic must fit in its page");

static pico_config_t cfg;
static critical_section_t cfg_lock;
static volatile bool save_requested;
static volatile uint32_t save_not_before_ms;

static bool firmware_install_reset_pending(void)
{
    // Volatile byte reads are intentional: this page is programmed to zero
    // after first boot, so the compiler must not constant-fold the initializer.
    return firmware_install_marker[0] == 'P' &&
           firmware_install_marker[1] == 'S' &&
           firmware_install_marker[2] == '2' &&
           firmware_install_marker[3] == '-' &&
           firmware_install_marker[4] == 'I' &&
           firmware_install_marker[5] == 'N' &&
           firmware_install_marker[6] == 'S' &&
           firmware_install_marker[7] == 'T' &&
           firmware_install_marker[8] == 'A' &&
           firmware_install_marker[9] == 'L' &&
           firmware_install_marker[10] == 'L' &&
           firmware_install_marker[11] == '-' &&
           firmware_install_marker[12] == 'R' &&
           firmware_install_marker[13] == 'E' &&
           firmware_install_marker[14] == 'S' &&
           firmware_install_marker[15] == 'E' &&
           firmware_install_marker[16] == 'T' &&
           firmware_install_marker[17] == '-' &&
           firmware_install_marker[18] == '1';
}

static bool consume_install_marker_and_erase_persistence(void)
{
    const uintptr_t marker_address = (uintptr_t)firmware_install_marker;
    if (marker_address < XIP_BASE ||
        marker_address + FLASH_PAGE_SIZE > XIP_BASE + PERSISTENT_FLASH_START ||
        ((marker_address - XIP_BASE) & (FLASH_PAGE_SIZE - 1u)) != 0u) {
        printf("[CONFIG] Refusing install reset: marker placement is invalid\n");
        return false;
    }

    uint8_t consumed[FLASH_PAGE_SIZE] = {0};
    const uint32_t marker_offset = (uint32_t)(marker_address - XIP_BASE);

    // This runs on core0 before core1 is launched and before USB/CYW43 start.
    // Erase durable state first; a power loss before consuming the marker
    // simply repeats the safe erase on the next boot.
    uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(PERSISTENT_FLASH_START, PERSISTENT_FLASH_SIZE);
    flash_range_program(marker_offset, consumed, sizeof(consumed));
    restore_interrupts(interrupts);
    printf("[CONFIG] New firmware image: settings, amiibo slots, wake identity, "
           "and Bluetooth bonds reset\n");
    return true;
}

static void load_defaults(void) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC;
    cfg.version = CONFIG_VERSION;
    // Genuine retail Pro Controller 2 body colour. Users can replace this with
    // any RGB value in config mode; it drives Sony lights while Pro2 is active.
    cfg.body_color[0] = 0x23;
    cfg.body_color[1] = 0x23;
    cfg.body_color[2] = 0x23;
    // Genuine retail Joy-Con 2 accent colours from the project's L/R SPI dumps.
    cfg.joycon2_left_accent[0] = 0x9B;
    cfg.joycon2_left_accent[1] = 0xE1;
    cfg.joycon2_left_accent[2] = 0xE6;
    cfg.joycon2_right_accent[0] = 0xFF;
    cfg.joycon2_right_accent[1] = 0x8C;
    cfg.joycon2_right_accent[2] = 0x5F;
}

void config_load(void) {
    critical_section_init(&cfg_lock);
    config_wireless_bridge_init();
    if (firmware_install_reset_pending())
        (void)consume_install_marker_and_erase_persistence();

    const uint8_t *flash = (const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    const pico_config_t *f = (const pico_config_t *)flash;
    if (f->magic == CONFIG_MAGIC && f->version == CONFIG_VERSION) {
        memcpy(&cfg, f, sizeof(cfg));
    } else {
        load_defaults();
    }
    virtual_amiibo_store_init();
}

void config_get_body_color(uint8_t rgb[3]) {
    critical_section_enter_blocking(&cfg_lock);
    memcpy(rgb, cfg.body_color, sizeof(cfg.body_color));
    critical_section_exit(&cfg_lock);
}

void config_get_joycon2_accent(bool right, uint8_t rgb[3]) {
    critical_section_enter_blocking(&cfg_lock);
    const uint8_t *accent = right ? cfg.joycon2_right_accent : cfg.joycon2_left_accent;
    memcpy(rgb, accent, 3);
    critical_section_exit(&cfg_lock);
}

static void set_body_color(uint8_t r, uint8_t g, uint8_t b) {
    critical_section_enter_blocking(&cfg_lock);
    cfg.body_color[0] = r;
    cfg.body_color[1] = g;
    cfg.body_color[2] = b;
    critical_section_exit(&cfg_lock);
}

static void set_joycon2_accent(bool right, uint8_t r, uint8_t g, uint8_t b) {
    critical_section_enter_blocking(&cfg_lock);
    uint8_t *accent = right ? cfg.joycon2_right_accent : cfg.joycon2_left_accent;
    accent[0] = r;
    accent[1] = g;
    accent[2] = b;
    critical_section_exit(&cfg_lock);
}

static bool address_is_nonzero(const uint8_t addr[6]) {
    uint8_t any = 0;
    for (int i = 0; i < 6; i++) any |= addr[i];
    return any != 0;
}

bool config_get_wake_identity(config_wake_identity_t *out) {
    if (!out) return false;

    bool valid;
    critical_section_enter_blocking(&cfg_lock);
    valid = cfg.wake_valid == CONFIG_WAKE_VALID &&
            cfg.wake_identity.host_count > 0 &&
            cfg.wake_identity.host_count <= CONFIG_WAKE_MAX_HOSTS &&
            address_is_nonzero(cfg.wake_identity.controller_addr_wire);
    if (valid) {
        for (uint8_t i = 0; i < cfg.wake_identity.host_count; i++) {
            if (!address_is_nonzero(cfg.wake_identity.host_addr_wire[i])) {
                valid = false;
                break;
            }
        }
    }
    if (valid) *out = cfg.wake_identity;
    critical_section_exit(&cfg_lock);
    return valid;
}

void config_store_wake_identity(const config_wake_identity_t *identity) {
    if (!identity || identity->host_count == 0 ||
        identity->host_count > CONFIG_WAKE_MAX_HOSTS ||
        !address_is_nonzero(identity->controller_addr_wire)) {
        return;
    }
    for (uint8_t i = 0; i < identity->host_count; i++) {
        if (!address_is_nonzero(identity->host_addr_wire[i])) return;
    }

    critical_section_enter_blocking(&cfg_lock);
    bool changed = cfg.wake_valid != CONFIG_WAKE_VALID ||
                   memcmp(&cfg.wake_identity, identity, sizeof(*identity)) != 0;
    if (changed) {
        cfg.wake_identity = *identity;
        cfg.wake_valid = CONFIG_WAKE_VALID;
    }
    critical_section_exit(&cfg_lock);

    // The console may repeat its pairing exchange on later USB sessions. Do
    // not erase/program flash again when the learned identity is unchanged.
    if (!changed) return;

    // Pairing continues for several USB commands after 0x15/03. A flash erase
    // parks core0, so postpone it until that timing-sensitive exchange is over.
    save_not_before_ms = to_ms_since_boot(get_absolute_time()) + CONFIG_WAKE_SAVE_DELAY_MS;
    save_requested = true;
}

void config_service_save(void) {
    // The virtual-tag journal has its own sector and request flag, but shares
    // this core1-only flash/lockout execution point.
    virtual_amiibo_store_service_save();

    if (!save_requested)
        return;

    uint32_t not_before = save_not_before_ms;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (not_before != 0 && (int32_t)(now - not_before) < 0)
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
    save_not_before_ms = 0;
}

//--------------------------------------------------------------------+
// Configuration command protocol
//--------------------------------------------------------------------+

#define LINE_MAX 128
static char line[LINE_MAX];
static uint16_t line_len;
// 4096: sized for "sw2cap drain"'s batch reply (up to SW2CAP_DRAIN_MAX=16 entries, each up to
// ~235 B with 64 hex-encoded payload bytes) — the largest reply this protocol produces. Also
// comfortably covers "imuanom" (30 hex-encoded motion bytes + a 4-entry trail + per-axis
// context), which no longer fits in the 256 B every simpler reply uses.
static char out[4096];

typedef enum {
    CONFIG_REPLY_CDC = 0,
    CONFIG_REPLY_WIRELESS,
} config_reply_transport_t;

static config_reply_transport_t reply_transport = CONFIG_REPLY_CDC;
static uint32_t wireless_reply_session;

static void reply(const char *s) {
    if (reply_transport == CONFIG_REPLY_WIRELESS) {
        (void)config_wireless_bridge_publish_response(
            wireless_reply_session, s);
        return;
    }
    tud_cdc_write_str(s);
    tud_cdc_write_str("\r\n");
    tud_cdc_write_flush();
}

static void cmd_get(void) {
    uint8_t body[3];
    uint8_t joy_l[3];
    uint8_t joy_r[3];
    critical_section_enter_blocking(&cfg_lock);
    memcpy(body, cfg.body_color, sizeof(body));
    memcpy(joy_l, cfg.joycon2_left_accent, sizeof(joy_l));
    memcpy(joy_r, cfg.joycon2_right_accent, sizeof(joy_r));
    critical_section_exit(&cfg_lock);
    // Keep the old lightbar shape as a read-only compatibility alias for
    // pre-v7 portal clients. All four entries intentionally name one value.
    snprintf(out, sizeof(out),
             "{\"body_color\":[%u,%u,%u],\"joycon2_left_accent\":[%u,%u,%u],"
             "\"joycon2_right_accent\":[%u,%u,%u],\"lightbar\":[[%u,%u,%u],[%u,%u,%u],"
             "[%u,%u,%u],[%u,%u,%u]]}",
             body[0], body[1], body[2],
             joy_l[0], joy_l[1], joy_l[2], joy_r[0], joy_r[1], joy_r[2],
             body[0], body[1], body[2], body[0], body[1], body[2],
             body[0], body[1], body[2], body[0], body[1], body[2]);
    reply(out);
}

static void reply_amiibo_result(virtual_amiibo_result_t result) {
    if (result == VIRTUAL_AMIIBO_OK) {
        reply("{\"ok\":true}");
        return;
    }
    snprintf(out, sizeof(out), "{\"error\":\"%s\",\"code\":%u}",
             virtual_amiibo_result_string(result), (unsigned)result);
    reply(out);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex_bytes(const char *hex, uint8_t *bytes,
                            size_t capacity, size_t *length) {
    size_t chars = strlen(hex);
    if ((chars & 1u) != 0 || chars / 2u > capacity) return false;
    for (size_t i = 0; i < chars / 2u; ++i) {
        int high = hex_nibble(hex[i * 2u]);
        int low = hex_nibble(hex[i * 2u + 1u]);
        if (high < 0 || low < 0) return false;
        bytes[i] = (uint8_t)((high << 4) | low);
    }
    *length = chars / 2u;
    return true;
}

#define AMIIBO_CDC_CHUNK_MAX 32u
static void cmd_amiibo(char *arg) {
    if (strcmp(arg, "status") == 0) {
        virtual_amiibo_status_t status;
        virtual_amiibo_store_status(&status);
        snprintf(out, sizeof(out),
                 "{\"loaded\":%s,\"dirty\":%s,"
                 "\"presented\":%s,"
                 "\"persisted\":%s,\"persistPending\":%s,\"size\":%u,"
                 "\"signature\":%s,\"hasSave2\":%s,\"usingSave2\":%s,"
                 "\"generation\":%lu,"
                 "\"uid\":\"%02X%02X%02X%02X%02X%02X%02X\","
                 "\"upload\":{\"active\":%s,\"received\":%u,\"size\":%u}}",
                 status.loaded ? "true" : "false",
                 status.dirty ? "true" : "false",
                 status.presented ? "true" : "false",
                 status.persisted ? "true" : "false",
                 virtual_amiibo_store_persist_pending() ? "true" : "false",
                 status.size,
                 status.has_originality_signature ? "true" : "false",
                 status.has_used_copy ? "true" : "false",
                 status.using_used_copy ? "true" : "false",
                 (unsigned long)status.generation,
                 status.uid[0], status.uid[1], status.uid[2], status.uid[3],
                 status.uid[4], status.uid[5], status.uid[6],
                 status.upload_active ? "true" : "false",
                 status.upload_received, status.upload_size);
        reply(out);
        return;
    }

    if (strncmp(arg, "begin ", 6) == 0) {
        unsigned long size;
        unsigned long crc;
        char trailing;
        if (sscanf(arg + 6, "%lu %lx %c", &size, &crc, &trailing) != 2) {
            reply("{\"error\":\"usage: amiibo begin <540|572> <crc32>\"}");
            return;
        }
        reply_amiibo_result(virtual_amiibo_store_upload_begin(
            (size_t)size, (uint32_t)crc));
        return;
    }

    if (strncmp(arg, "chunk ", 6) == 0) {
        char *hex = strchr(arg + 6, ' ');
        if (!hex) {
            reply("{\"error\":\"usage: amiibo chunk <offset> <hex>\"}");
            return;
        }
        *hex++ = '\0';
        char *end;
        unsigned long offset = strtoul(arg + 6, &end, 10);
        if (*end != '\0') {
            reply("{\"error\":\"bad chunk offset\"}");
            return;
        }
        uint8_t bytes[AMIIBO_CDC_CHUNK_MAX];
        size_t length;
        if (!parse_hex_bytes(hex, bytes, sizeof(bytes), &length) ||
            length == 0) {
            reply("{\"error\":\"chunk must contain 1-32 hex bytes\"}");
            return;
        }
        reply_amiibo_result(virtual_amiibo_store_upload_chunk(
            (size_t)offset, bytes, length));
        return;
    }

    if (strcmp(arg, "commit") == 0) {
        reply_amiibo_result(virtual_amiibo_store_upload_commit());
        return;
    }
    if (strcmp(arg, "commit save2") == 0 ||
        strcmp(arg, "commit used") == 0) {
        reply_amiibo_result(virtual_amiibo_store_upload_commit_used());
        return;
    }
    if (strcmp(arg, "cancel") == 0) {
        virtual_amiibo_store_upload_cancel();
        reply("{\"ok\":true}");
        return;
    }

    const char *read_args = NULL;
    int read_copy = -1;
    if (strncmp(arg, "read save1 ", 11) == 0 ||
        strncmp(arg, "read clean ", 11) == 0) {
        read_args = arg + 11;
        read_copy = 0;
    } else if (strncmp(arg, "read save2 ", 11) == 0) {
        read_args = arg + 11;
        read_copy = 1;
    } else if (strncmp(arg, "read used ", 10) == 0) {
        read_args = arg + 10;
        read_copy = 1;
    } else if (strncmp(arg, "read ", 5) == 0) {
        read_args = arg + 5;
    }
    if (read_args) {
        unsigned long offset;
        unsigned long length;
        char trailing;
        if (sscanf(read_args, "%lu %lu %c",
                   &offset, &length, &trailing) != 2 ||
            length == 0 || length > AMIIBO_CDC_CHUNK_MAX) {
            reply("{\"error\":\"usage: amiibo read [save1|save2] "
                  "<offset> <1-32>\"}");
            return;
        }
        uint8_t bytes[AMIIBO_CDC_CHUNK_MAX];
        virtual_amiibo_result_t result =
            read_copy < 0
                ? virtual_amiibo_store_read(
                      (size_t)offset, bytes, (size_t)length)
                : virtual_amiibo_store_read_copy(
                      read_copy != 0, (size_t)offset,
                      bytes, (size_t)length);
        if (result != VIRTUAL_AMIIBO_OK) {
            reply_amiibo_result(result);
            return;
        }
        int used = snprintf(out, sizeof(out),
                            "{\"offset\":%lu,\"data\":\"", offset);
        for (unsigned long i = 0; i < length; ++i)
            used += snprintf(out + used, sizeof(out) - used,
                             "%02X", bytes[i]);
        snprintf(out + used, sizeof(out) - used, "\"}");
        reply(out);
        return;
    }

    if (strcmp(arg, "downloaded") == 0) {
        virtual_amiibo_store_acknowledge_download();
        reply("{\"ok\":true}");
        return;
    }

    if (strcmp(arg, "select save1") == 0 ||
        strcmp(arg, "select clean") == 0) {
        reply_amiibo_result(virtual_amiibo_store_select_used(false));
        return;
    }
    if (strcmp(arg, "select save2") == 0 ||
        strcmp(arg, "select used") == 0) {
        reply_amiibo_result(virtual_amiibo_store_select_used(true));
        return;
    }

    if (strcmp(arg, "present") == 0) {
        reply_amiibo_result(virtual_amiibo_store_set_presented(true));
        return;
    }
    if (strcmp(arg, "eject") == 0) {
        reply_amiibo_result(virtual_amiibo_store_set_presented(false));
        return;
    }

    if (strcmp(arg, "persist") == 0) {
        virtual_amiibo_store_request_persist();
        absolute_time_t deadline = make_timeout_time_ms(2000);
        while (virtual_amiibo_store_persist_pending() &&
               !time_reached(deadline))
            tud_task();
        reply(virtual_amiibo_store_persist_pending()
                  ? "{\"error\":\"persist timeout\"}"
                  : "{\"ok\":true}");
        return;
    }

    reply("{\"error\":\"usage: amiibo status|begin|chunk|commit|"
           "commit save2|cancel|read [save1|save2]|downloaded|"
           "select save1|select save2|"
           "present|eject|persist\"}");
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

static void cmd_audiostat(bool reset) {
    if (reset) {
        ds5_audio_diag_reset();
        reply("{\"ok\":true}");
        return;
    }

    ds5_audio_diag_t d;
    ds5_audio_diag_get(&d);
    snprintf(out, sizeof(out),
             "{\"sysClockKhz\":%lu,"
             "\"core1MaxGapUs\":%lu,\"core1GapsOver10ms\":%lu,"
             "\"sendMaxGapUs\":%lu,\"sendGapsOver40ms\":%lu,\"sends\":%lu,"
             "\"hciMaxGapUs\":%lu,\"hciGapsOver40ms\":%lu,"
             "\"hciEvents\":%lu,\"hciPackets\":%lu,\"hciMaxBatch\":%lu,"
             "\"pcmPackets\":%lu,\"pcmNonzero\":%lu,\"pcmShort\":%lu,"
             "\"pcmDropped\":%lu,\"pcmMaxGapUs\":%lu,\"pcmGapsOver2ms\":%lu,"
             "\"pcmQueueMax\":%lu,\"opusFrames\":%lu,\"opusErrors\":%lu,"
             "\"opusMaxGapUs\":%lu,\"opusGapsOver20ms\":%lu,"
             "\"opusEncodeMaxUs\":%lu,\"pipelineResets\":%lu,"
             "\"codecCalls\":%lu,\"codecNoEncoder\":%lu,"
             "\"codecDisconnected\":%lu,\"codecUsbInactive\":%lu,"
             "\"codecNoPcm\":%lu,\"codecBlocks\":%lu,"
             "\"codecMaxGapUs\":%lu,\"codecGapsOver10ms\":%lu,"
             "\"codecGapLe3ms\":%lu,\"codecGapLe7ms\":%lu,"
             "\"codecGapLe12ms\":%lu,\"codecGapLe25ms\":%lu,"
             "\"codecGapOver25ms\":%lu,\"usbSpeakerOnEdges\":%lu,"
             "\"usbSpeakerOffEdges\":%lu,\"usbSpeakerActiveUs\":%lu,"
             "\"usbSpeakerActive\":%u}",
             (unsigned long)(clock_get_hz(clk_sys) / 1000u),
             (unsigned long)d.core1_max_gap_us,
             (unsigned long)d.core1_gaps_over_10ms,
             (unsigned long)d.send_max_gap_us,
             (unsigned long)d.send_gaps_over_40ms,
             (unsigned long)d.sends_total,
             (unsigned long)d.hci_complete_max_gap_us,
             (unsigned long)d.hci_complete_gaps_over_40ms,
             (unsigned long)d.hci_complete_events,
             (unsigned long)d.hci_completed_packets,
             (unsigned long)d.hci_complete_max_batch,
             (unsigned long)d.pcm_packets_total,
             (unsigned long)d.pcm_nonzero_packets,
             (unsigned long)d.pcm_short_packets,
             (unsigned long)d.pcm_dropped_packets,
             (unsigned long)d.pcm_max_gap_us,
             (unsigned long)d.pcm_gaps_over_2ms,
             (unsigned long)d.pcm_queue_max_depth,
             (unsigned long)d.opus_frames_total,
             (unsigned long)d.opus_encode_errors,
             (unsigned long)d.opus_max_gap_us,
             (unsigned long)d.opus_gaps_over_20ms,
             (unsigned long)d.opus_encode_max_us,
             (unsigned long)d.pipeline_resets,
             (unsigned long)d.codec_calls_total,
             (unsigned long)d.codec_no_encoder,
             (unsigned long)d.codec_disconnected,
             (unsigned long)d.codec_usb_inactive,
             (unsigned long)d.codec_no_pcm,
             (unsigned long)d.codec_blocks_dequeued,
             (unsigned long)d.codec_call_max_gap_us,
             (unsigned long)d.codec_call_gaps_over_10ms,
             (unsigned long)d.codec_gap_le_3ms,
             (unsigned long)d.codec_gap_le_7ms,
             (unsigned long)d.codec_gap_le_12ms,
             (unsigned long)d.codec_gap_le_25ms,
             (unsigned long)d.codec_gap_over_25ms,
             (unsigned long)d.usb_speaker_on_edges,
             (unsigned long)d.usb_speaker_off_edges,
             (unsigned long)d.usb_speaker_active_us,
             d.usb_speaker_active ? 1u : 0u);
    reply(out);
}

// Connected controller identity for the "Current Input Type" panel. Slot 0.
static void cmd_device(void) {
    char name[40];
    uint16_t vid = 0, pid = 0;
    switch_pro_input_t in;
    get_global_device(0, name, sizeof(name), &vid, &pid);
    get_global_gamepad_input(0, &in);
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
    snprintf(out, sizeof(out),
             "{\"name\":\"%s\",\"vid\":%u,\"pid\":%u,"
             "\"batteryValid\":%u,\"battery\":%u,\"charging\":%u}",
             esc, vid, pid, in.battery_valid, in.battery_level,
             in.battery_charging);
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
// NS2_PRO-only: the ns2_dbg_* bodies only exist in switch_pro2.c, which is entirely #ifdef
// NS2_PRO. Found 2026-07-12: this command (and cmd_imuanom below) had no guard at all, so
// -DNS2_PRO=OFF (the plain Switch-1 build) failed to link ever since these commands were added
// 2026-07-10 — a real, unnoticed regression, not something introduced by this pass.
#ifdef NS2_PRO
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

static void cmd_fwreads(void) {
    ns2_firmware_diagnostics_format_json(out, sizeof(out));
    reply(out);
}
#endif  // NS2_PRO

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
// `sw2cap variant <0-9>` arms one of the v2 feature-enable experiment variants (0 = off);
// `sw2cap variant stat` reports which is currently armed (see sw2_capture.h for what each
// variant does). Independent of plain capture on/off, so a session can capture normal traffic
// first and only arm an experiment when explicitly asked. Do not arm gattdisc and a variant in
// the same session (see sw2_capture.h).
#define SW2CAP_DRAIN_MAX 16
static void cmd_sw2cap(const char *arg) {
    if (strcmp(arg, "on") == 0) {
        sw2_capture_set_filter(SW2_CAPTURE_FILTER_ALL);
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
        if (n < 0 || n > 9) {
            reply("{\"error\":\"variant must be 0-9 (0=off)\"}");
        } else {
            sw2_set_v2_variant((uint8_t)n);
            snprintf(out, sizeof(out), "{\"ok\":true,\"variant\":%d}", n);
            reply(out);
        }
    } else if (strncmp(arg, "mark ", 5) == 0) {
        // Capture-session annotation (see sw2_capture.h's sw2_capture_mark()) -- a pure logging
        // call, does not touch the BLE connection/init/report path at all. Truncated to
        // SW2_CAP_MAX_DATA same as every other capture entry's payload.
        const char *label = arg + 5;
        size_t len = strlen(label);
        if (len > 64) len = 64;
        sw2_capture_mark((const uint8_t *)label, (uint16_t)len);
        snprintf(out, sizeof(out), "{\"ok\":true,\"marked\":\"%.64s\"}", label);
        reply(out);
    } else {
        reply("{\"error\":\"usage: sw2cap on|off|stat|drain|gattdisc on|off|stat|variant <0-9>|variant stat|mark <text>\"}");
    }
}

// `btid dump` — drains the controller identity/driver-binding event log (bt_identity_log.h,
// Gate 2). Always recording (no on/off toggle — one event per binding decision is low-frequency
// enough that there's no cost to leaving it on, unlike sw2cap's per-packet traffic capture).
// `btid stat` reports the dropped count; `btid clear` resets the ring for a fresh test session.
// Pull-based for the same reason as sw2cap — see that command's comment.
#define BTID_DRAIN_MAX 8
static void cmd_btid(const char *arg) {
    if (strcmp(arg, "clear") == 0) {
        bt_identity_log_clear();
        reply("{\"ok\":true}");
    } else if (strcmp(arg, "stat") == 0) {
        snprintf(out, sizeof(out), "{\"dropped\":%lu}",
                 (unsigned long)bt_identity_log_dropped_count());
        reply(out);
    } else if (strcmp(arg, "dump") == 0) {
        int j = snprintf(out, sizeof(out), "{\"dropped\":%lu,\"entries\":[",
                          (unsigned long)bt_identity_log_dropped_count());
        bt_identity_event_t e;
        int n = 0;
        while (n < BTID_DRAIN_MAX) {
            if (!bt_identity_log_drain_one(&e)) break;
            j += snprintf(out + j, sizeof(out) - j,
                "%s{\"ms\":%lu,\"conn\":%u,\"transport\":\"%s\",\"name\":\"%.32s\","
                "\"vid\":\"0x%04X\",\"pid\":\"0x%04X\",\"provenance\":\"%s\","
                "\"cod\":\"%02X%02X%02X\",\"desc_len\":%u,\"desc_fp\":\"0x%04X\","
                "\"driver\":\"%.24s\",\"reason\":\"%.32s\",\"slot\":%d}",
                n ? "," : "", (unsigned long)e.timestamp_ms, e.conn_index,
                e.is_ble ? "ble" : "classic", e.name, e.vendor_id, e.product_id,
                bt_identity_provenance_name(e.provenance),
                e.class_of_device[0], e.class_of_device[1], e.class_of_device[2],
                e.desc_len, e.desc_fingerprint, e.driver_name, e.reason, e.player_slot);
            n++;
        }
        bool more = (n == BTID_DRAIN_MAX);
        snprintf(out + j, sizeof(out) - j, "],\"more\":%s}", more ? "true" : "false");
        reply(out);
    } else if (strcmp(arg, "desc") == 0) {
        // Raw cached HID descriptor + generic driver's parsed field map — added 2026-07-12 to
        // inspect real descriptor bytes/parse results directly instead of guessing from input
        // symptoms (see the 8BitDo NGC trigger/shoulder investigation in
        // docs/bluetooth/8bitdo-ngc-diy-profile.md). Single-slot cache, same as bthid.c's
        // internal one — only useful for whichever one connection most recently had a
        // descriptor arrive.
        const uint8_t* desc = NULL;
        uint16_t desc_len = 0;
        uint8_t desc_conn = 0xFF;
        int j = snprintf(out, sizeof(out), "{");
        if (bthid_get_cached_descriptor(&desc, &desc_len, &desc_conn)) {
            j += snprintf(out + j, sizeof(out) - j, "\"conn\":%u,\"len\":%u,\"bytes\":\"",
                          desc_conn, desc_len);
            for (int i = 0; i < desc_len && j < (int)sizeof(out) - 200; i++)
                j += snprintf(out + j, sizeof(out) - j, "%02X", desc[i]);
            j += snprintf(out + j, sizeof(out) - j, "\",\"map\":");
            char mapbuf[1024];
            bthid_gamepad_dump_map(desc_conn, mapbuf, sizeof(mapbuf));
            j += snprintf(out + j, sizeof(out) - j, "%s", mapbuf);
        } else {
            j += snprintf(out + j, sizeof(out) - j, "\"error\":\"no descriptor cached\"");
        }
        snprintf(out + j, sizeof(out) - j, "}");
        reply(out);
    } else {
        reply("{\"error\":\"usage: btid dump|stat|clear|desc\"}");
    }
}

static void handle_line(char *cmd) {
    if (reply_transport == CONFIG_REPLY_WIRELESS &&
        !config_wireless_command_allowed(cmd)) {
        reply("{\"error\":\"command unavailable over Bluetooth\"}");
        return;
    }

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
    } else if (strcmp(cmd, "audiostat") == 0) {
        cmd_audiostat(false);
    } else if (strcmp(cmd, "audiostat reset") == 0) {
        cmd_audiostat(true);
    } else if (strcmp(cmd, "raw") == 0) {
        cmd_raw();
#ifdef NS2_PRO
    } else if (strcmp(cmd, "imu") == 0) {
        cmd_imu();
    } else if (strcmp(cmd, "imuanom") == 0) {
        cmd_imuanom();
    } else if (strcmp(cmd, "fwreads") == 0) {
        cmd_fwreads();
#endif  // NS2_PRO
    } else if (strncmp(cmd, "sw2cap ", 7) == 0) {
        cmd_sw2cap(cmd + 7);
    } else if (strncmp(cmd, "btid ", 5) == 0) {
        cmd_btid(cmd + 5);
    } else if (strncmp(cmd, "amiibo ", 7) == 0) {
        cmd_amiibo(cmd + 7);
    } else if (strncmp(cmd, "body ", 5) == 0) {
        int r, g, b;
        if (sscanf(cmd + 5, "%d %d %d", &r, &g, &b) == 3 &&
            r >= 0 && r < 256 && g >= 0 && g < 256 && b >= 0 && b < 256) {
            set_body_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
            reply("{\"ok\":true}");
        } else {
            reply("{\"error\":\"bad args\"}");
        }
    } else if (strncmp(cmd, "jcl ", 4) == 0 || strncmp(cmd, "jcr ", 4) == 0) {
        int r, g, b;
        if (sscanf(cmd + 4, "%d %d %d", &r, &g, &b) == 3 &&
            r >= 0 && r < 256 && g >= 0 && g < 256 && b >= 0 && b < 256) {
            set_joycon2_accent(cmd[2] == 'r', (uint8_t)r, (uint8_t)g, (uint8_t)b);
            reply("{\"ok\":true}");
        } else {
            reply("{\"error\":\"bad args\"}");
        }
    } else if (strncmp(cmd, "lb ", 3) == 0) {
        // Backward-compatible alias for a cached v6 config page. Only slot 0
        // ever affected this single-controller firmware, so reject stale
        // attempts to configure the now-removed independent player colours.
        int p, r, g, b;
        if (sscanf(cmd + 3, "%d %d %d %d", &p, &r, &g, &b) == 4 && p == 0 &&
            r >= 0 && r < 256 && g >= 0 && g < 256 && b >= 0 && b < 256) {
            set_body_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
            reply("{\"ok\":true}");
        } else {
            reply("{\"error\":\"bad args\"}");
        }
    } else if (strcmp(cmd, "save") == 0) {
        // An explicit config-mode save overrides any deferred automatic save.
        save_not_before_ms = 0;
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
    reply_transport = CONFIG_REPLY_CDC;
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

    // BLE writes arrive on core1. Execute at most one complete command here on
    // core0, using the same parser and persistence behavior as CDC. The browser
    // waits for each JSON-line response before sending another command, so a
    // bounded one-command bridge is intentional.
    char wireless_command[CONFIG_WIRELESS_COMMAND_CAPACITY];
    uint32_t session;
    if (config_wireless_bridge_take_command(
            wireless_command, sizeof(wireless_command), &session)) {
        reply_transport = CONFIG_REPLY_WIRELESS;
        wireless_reply_session = session;
        handle_line(wireless_command);
        reply_transport = CONFIG_REPLY_CDC;
    }

    // BLE capture entries (see sw2_capture.h) are pulled explicitly via `sw2cap drain`, not
    // auto-streamed here — a client (the web UI) polls it like any other command.
}
