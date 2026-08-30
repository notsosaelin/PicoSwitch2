// Persistent settings + configuration-mode command protocol.
//
// Settings live in one flash sector placed safely below btstack's own flash
// region (it uses the last 2-3 sectors depending on the chip). The flash write
// is performed on core1 (which already owns the multicore-lockout requester role
// used for BOOTSEL), so it can park core0 during the erase/program without any
// risk of a bidirectional lockout. Commands execute on core0 whether they arrive
// over USB CDC or the bonded/encrypted Config/in-band BLE bridge; this preserves that ownership and
// keeps parsing/flash waits out of BTstack callbacks.

#include "config.h"
#include "config_persist.h"  // persisted record layout, defaults, migration
#include "config_save_tracker.h"
#include "report.h"      // get_global_raw_buttons / get_global_gamepad_input (live view)
#include "switch_pro.h"  // switch_pro_input_t
#include "switch_pro2.h" // ns2_dbg_* getters (report-0x09 motion/gyro debug instrumentation)
#include "ns2_firmware_profile.h" // firmware prompt read-address diagnostics
#include "sw2_capture.h" // genuine Switch 2 BLE raw-traffic capture/export (2026-07-10)
#include "bt_identity_log.h" // controller identity/driver-binding event log (Gate 2, 2026-07-12)
#include "bt/bthid/bthid.h" // bthid_get_cached_descriptor (btid desc command)
#include "ds5_audio_bridge.h" // DualSense audio stall diagnostics
#include "bt/bthid/devices/generic/bthid_gamepad.h" // bthid_gamepad_dump_map (btid desc command)
#include "fixtures/android_controller_hid.h" // ANDROID_BRIDGE_CONTRACT_VERSION (runtime skew detection)
#include "virtual_amiibo_store.h"
#include "config_wireless_bridge.h"
#include "mgmt_bonds.h"
#include "mgmt_peers.h"
#include "mgmt_pairing.h"
#include "usb.h"  // g_usb_personality (personality query command)
#include "ns2_wake.h"  // ns2_wake_manual_request (wake command)
#include "ns2_active_input.h" // source registry / explicit active input
#include "ns2_kbm.h"           // Bluetooth keyboard / KB+M mapping model
#include "ns2_kbm_commands.h"  // shared, host-tested KB/M read formatters
#include "ns2_kbm_runtime.h"   // live KB/M configuration + status
#include "ns2_kbm_status.h"    // shared KB/M status JSON formatter
#include "bt/btstack/btstack_host.h"  // bonds list/remove (management)
#ifdef NS2_PRO
#include "ns2_nfc_mirror.h"  // amiibo reader (controller-as-reader backup)
#endif

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

#define CONFIG_MAGIC CONFIG_PERSIST_MAGIC
#define CONFIG_VERSION CONFIG_PERSIST_VERSION
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 4 * FLASH_SECTOR_SIZE)
#define PERSISTENT_FLASH_START \
    (PICO_FLASH_SIZE_BYTES - 6u * FLASH_SECTOR_SIZE)
#define PERSISTENT_FLASH_SIZE (6u * FLASH_SECTOR_SIZE)
#define CONFIG_WAKE_VALID 0xA5
#define CONFIG_WAKE_SAVE_DELAY_MS 5000
#define INSTALL_MARKER_LENGTH 19u

// The record layout, its factory defaults, and schema migration live in
// config_persist.c so both can be exercised by a host test; this file owns
// where they are stored and when they are written.
typedef config_record_t pico_config_t;

// The record no longer fits one flash page. The settings sector is 4 KiB and is
// erased whole on every save, so widening the programmed region costs nothing
// but a slightly longer program; keep it a whole number of pages.
//
// Widened to 8 pages for the KB/M profile table (schema 14). Verified against
// the persistence map in virtual_amiibo_store.c before doing so: this sector
// (SIZE-4S) is ours alone, its neighbours are BTstack's TLV bank A above and
// the amiibo journal below, and the erase already covers all 4096 bytes
// whatever this value is -- so the window in which a power loss costs the
// settings is unchanged. What does grow is the number of pages a torn program
// can leave half-written, and the record carries no CRC; ns2_kbm_config_sanitize()
// is what fails that closed.
#define CONFIG_RECORD_BYTES (8u * FLASH_PAGE_SIZE)

// Every UF2 contains this pending marker in its own dedicated flash page.
// First boot consumes it with a 1->0 page program after erasing all six
// PicoSwitch2 persistence sectors. Reflashing even the same UF2 rewrites the
// application sector and restores the pending marker, while an ordinary reboot
// leaves the consumed page untouched.
static const volatile uint8_t firmware_install_marker[FLASH_PAGE_SIZE]
    __attribute__((aligned(FLASH_PAGE_SIZE),
                   section(".rodata.install_marker"), used)) =
    {'P', 'S', '2', '-', 'I', 'N', 'S', 'T', 'A', 'L',
     'L', '-', 'R', 'E', 'S', 'E', 'T', '-', '1'};

_Static_assert(sizeof(pico_config_t) <= CONFIG_RECORD_BYTES,
               "config must fit in the programmed settings record");
_Static_assert(CONFIG_RECORD_BYTES <= FLASH_SECTOR_SIZE,
               "settings record must fit in its own erase sector");
_Static_assert((CONFIG_RECORD_BYTES % FLASH_PAGE_SIZE) == 0u,
               "flash_range_program requires whole pages");
_Static_assert(sizeof(firmware_install_marker) == FLASH_PAGE_SIZE,
               "install marker must occupy one flash page");
_Static_assert(INSTALL_MARKER_LENGTH < FLASH_PAGE_SIZE,
               "install marker magic must fit in its page");

static pico_config_t cfg;
static critical_section_t cfg_lock;
static config_save_tracker_t save_tracker;
static volatile uint32_t save_not_before_ms;
static bool install_reset_performed;

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
    config_persist_defaults(&cfg);
}

void config_load(void) {
    critical_section_init(&cfg_lock);
    config_wireless_bridge_init();
    install_reset_performed = firmware_install_reset_pending() &&
        consume_install_marker_and_erase_persistence();

    const uint8_t *flash = (const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    config_persist_load_t loaded =
        config_persist_load(flash, CONFIG_RECORD_BYTES, &cfg);
    switch (loaded) {
        case CONFIG_PERSIST_MIGRATED:
            printf("[CONFIG] Settings upgraded to schema %u; existing colours "
                   "and wake identity preserved\n", (unsigned)CONFIG_VERSION);
            // Persist the upgraded record so the migration runs exactly once.
            (void)config_request_save();
            break;
        case CONFIG_PERSIST_REPAIRED:
            printf("[CONFIG] Keyboard/mouse mapping data was unusable; "
                   "canonical defaults restored\n");
            (void)config_request_save();
            break;
        default:
            break;
    }
    // The KB/M runtime owns the live copy from here on; it validates again on
    // adoption so a management write can never install something the loader
    // would have rejected.
    (void)ns2_kbm_runtime_config_load(&cfg.kbm);
    virtual_amiibo_store_init();
}

bool config_install_reset_performed(void) {
    return install_reset_performed;
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
    (void)config_save_tracker_request(&save_tracker);
}

void config_note_management_companion(const uint8_t addr[6], uint8_t addr_type) {
    critical_section_enter_blocking(&cfg_lock);
    bool changed = config_mgmt_companion_remember(
        cfg.mgmt_companions, CONFIG_MGMT_COMPANIONS_MAX, addr, addr_type);
    critical_section_exit(&cfg_lock);
    // A companion reconnects constantly. Only a genuinely new membership is
    // worth an erase/program cycle.
    if (changed) (void)config_save_tracker_request(&save_tracker);
}

bool config_is_management_companion(const uint8_t addr[6]) {
    critical_section_enter_blocking(&cfg_lock);
    bool known = config_mgmt_companion_known(cfg.mgmt_companions,
                                             CONFIG_MGMT_COMPANIONS_MAX, addr);
    critical_section_exit(&cfg_lock);
    return known;
}

void config_forget_management_companion(const uint8_t addr[6]) {
    critical_section_enter_blocking(&cfg_lock);
    bool changed = config_mgmt_companion_forget(
        cfg.mgmt_companions, CONFIG_MGMT_COMPANIONS_MAX, addr);
    critical_section_exit(&cfg_lock);
    if (changed) (void)config_save_tracker_request(&save_tracker);
}

bool config_management_companion_at(uint8_t index, uint8_t addr[6],
                                    uint8_t *addr_type) {
    if (index >= CONFIG_MGMT_COMPANIONS_MAX || !addr) return false;
    critical_section_enter_blocking(&cfg_lock);
    bool valid = cfg.mgmt_companions[index].valid != 0u;
    if (valid) {
        memcpy(addr, cfg.mgmt_companions[index].addr, 6u);
        if (addr_type) *addr_type = cfg.mgmt_companions[index].addr_type;
    }
    critical_section_exit(&cfg_lock);
    return valid;
}

uint32_t config_request_save(void) {
    // An explicit save overrides any deferred automatic save.
    //
    // Exists so a surface that is not the command parser -- the UART diagnostic
    // channel -- arms the SAME deferred write rather than growing a second
    // persistence path. The record is composed and written by
    // config_service_save() on core1 either way, which is also what makes the
    // live KB/M configuration part of the saved record.
    save_not_before_ms = 0;
    return config_save_tracker_request(&save_tracker);
}

void config_service_save(void) {
    // The virtual-tag journal has its own sector and request flag, but shares
    // this core1-only flash/lockout execution point.
    virtual_amiibo_store_service_save();

    if (!config_save_tracker_pending(&save_tracker))
        return;

    uint32_t not_before = save_not_before_ms;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (not_before != 0 && (int32_t)(now - not_before) < 0)
        return;

    // Snapshot the newest request before composing the record. A request that
    // arrives during the write remains newer than completed and is serviced on
    // a later control tick rather than being lost when this write finishes.
    uint32_t completing_request = config_save_tracker_requested(&save_tracker);

    // The KB/M runtime owns the live mapping state; take its current snapshot
    // rather than a copy this module would have to keep in step by hand. Taken
    // before cfg_lock deliberately: it spins on the runtime's own seqlock, and
    // nesting two independent spin waits is how a lock-order bug starts.
    ns2_kbm_config_t kbm_snapshot;
    ns2_kbm_runtime_config_get(&kbm_snapshot);

    pico_config_t snap;
    critical_section_enter_blocking(&cfg_lock);
    cfg.kbm = kbm_snapshot;
    snap = cfg;
    critical_section_exit(&cfg_lock);

    static uint8_t record[CONFIG_RECORD_BYTES];
    memset(record, 0xFF, sizeof(record));
    memcpy(record, &snap, sizeof(snap));

    // Park core0 (USB) so it can't touch flash, then erase+program with our
    // interrupts off (an ISR could otherwise execute from now-disabled flash).
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, record, CONFIG_RECORD_BYTES);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();

    config_save_tracker_complete(&save_tracker, completing_request);
    save_not_before_ms = 0;
}

//--------------------------------------------------------------------+
// Configuration command protocol
//--------------------------------------------------------------------+

#define LINE_MAX 128
static char line[LINE_MAX];
static uint16_t line_len;
// 4096: sized for "sw2cap drain"'s batch reply (up to SW2CAP_DRAIN_MAX=16 entries, each up to
// ~235 B with 64 hex-encoded payload bytes) — the largest reply this protocol produces.
static char out[4096];

typedef enum {
    CONFIG_REPLY_CDC = 0,
    CONFIG_REPLY_WIRELESS,
    // A capture buffer supplied by the caller, used by the UART diagnostic
    // passthrough. See config_execute_captured().
    CONFIG_REPLY_CAPTURE,
} config_reply_transport_t;

static config_reply_transport_t reply_transport = CONFIG_REPLY_CDC;
static char *capture_buffer;
static size_t capture_capacity;
static size_t capture_length;
static uint32_t wireless_reply_session;

static struct {
    bool active;
    bool is_remove;
    bool is_page;
    uint32_t session;
    absolute_time_t deadline;
} wireless_bonds;

// The peer inventory uses the same deferred-reply shape as `bonds`, and for the
// same reason: the databases live on core1 and core0 must keep servicing the
// console while the answer is built. Kept as its own record rather than folded
// into wireless_bonds so a peer read and a bond read can never be mistaken for
// one another when both are in flight across a reconnect.
static struct {
    bool active;
    uint32_t session;
    absolute_time_t deadline;
} wireless_peers;

// Remote pairing uses the same deferred shape again, and its own record for the
// same reason: a pairing reply and a peer read must never be mistaken for one
// another when both are outstanding across a reconnect.
static struct {
    bool active;
    uint32_t session;
    absolute_time_t deadline;
} wireless_pairing;

static void reply(const char *s) {
    if (reply_transport == CONFIG_REPLY_CAPTURE) {
        // The TRUE length is recorded even when the copy is truncated: a
        // diagnostic that reported the clipped size would hide an oversized
        // reply, which is the exact class of bug this path exists to expose.
        capture_length = strlen(s);
        if (capture_buffer && capture_capacity) {
            size_t copied = capture_length < capture_capacity - 1u
                                ? capture_length : capture_capacity - 1u;
            memcpy(capture_buffer, s, copied);
            capture_buffer[copied] = '\0';
        }
        return;
    }
    if (reply_transport == CONFIG_REPLY_WIRELESS) {
        if (!config_wireless_bridge_publish_response(
                wireless_reply_session, s)) {
            // A response that exceeds the wireless slot must never become a
            // silent timeout or a syntactically valid partial result.  The
            // compact fallback fits even when the original command did not.
            (void)config_wireless_bridge_publish_response(
                wireless_reply_session,
                "{\"error\":\"response_too_large\",\"code\":413}");
        }
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
        // Amiibo model/figure ID: the 8 plaintext bytes at tag offset 0x54 (game/character/
        // variant/type/model/series). It is NOT encrypted, so we can surface the amiibo's
        // identity for the app (e.g. AmiiboAPI lookup) without any keys. The identity block sits
        // at 0x54 for BOTH the 540/572 NTAG215 store AND the 2048-byte NTAG I2C 2K (v3) store —
        // the only difference is the format byte 0x5B (0x02 standard, 0x03 v3). See
        // docs/switch2/amiibo-crypto-research-2026-08.md and docs/Amiibo-v3.md.
        char figure_id[17] = "";
        if (status.loaded || status.v3_loaded) {
            uint8_t fig[8];
            virtual_amiibo_result_t fr = status.v3_loaded
                ? virtual_amiibo_store_v3_read(0x54u, fig, sizeof(fig))
                : virtual_amiibo_store_read(0x54u, fig, sizeof(fig));
            if (fr == VIRTUAL_AMIIBO_OK)
                for (int i = 0; i < 8; i++)
                    snprintf(figure_id + i * 2, 3, "%02X", fig[i]);
        }
        snprintf(out, sizeof(out),
                 "{\"loaded\":%s,\"dirty\":%s,"
                 "\"presented\":%s,\"v3loaded\":%s,"
                 "\"persisted\":%s,\"persistPending\":%s,\"size\":%u,"
                 "\"signature\":%s,\"hasSave2\":%s,\"usingSave2\":%s,"
                 "\"generation\":%lu,\"payloadCrc\":\"%08lX\","
                 "\"uid\":\"%02X%02X%02X%02X%02X%02X%02X\",\"figureId\":\"%s\","
                 "\"upload\":{\"active\":%s,\"received\":%u,\"size\":%u}}",
                 status.loaded ? "true" : "false",
                 status.dirty ? "true" : "false",
                 status.presented ? "true" : "false",
                 status.v3_loaded ? "true" : "false",
                 status.persisted ? "true" : "false",
                 virtual_amiibo_store_persist_pending() ? "true" : "false",
                 status.size,
                 status.has_originality_signature ? "true" : "false",
                 status.has_used_copy ? "true" : "false",
                 status.using_used_copy ? "true" : "false",
                 (unsigned long)status.generation,
                 (unsigned long)status.payload_crc,
                 status.uid[0], status.uid[1], status.uid[2], status.uid[3],
                 status.uid[4], status.uid[5], status.uid[6], figure_id,
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
            reply("{\"error\":\"usage: amiibo begin <540|572|2048> <crc32>\"}");
            return;
        }
        // A 2048-byte image routes to the isolated NTAG I2C 2K (v3) slot; all
        // other sizes use the validated NTAG215 store unchanged.
        reply_amiibo_result(size == 2048u
            ? virtual_amiibo_store_v3_upload_begin((size_t)size, (uint32_t)crc)
            : virtual_amiibo_store_upload_begin((size_t)size, (uint32_t)crc));
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
        // Require a bare decimal run: strtoul() alone would accept an empty
        // field ("amiibo chunk  AABB" splits to offset ""), leading whitespace,
        // and a sign, silently landing the write at offset 0 or at a wrapped
        // huge offset instead of reporting the malformed command.
        if (arg[6] < '0' || arg[6] > '9') {
            reply("{\"error\":\"bad chunk offset\"}");
            return;
        }
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
        reply_amiibo_result(virtual_amiibo_store_v3_upload_active()
            ? virtual_amiibo_store_v3_upload_chunk((size_t)offset, bytes, length)
            : virtual_amiibo_store_upload_chunk((size_t)offset, bytes, length));
        return;
    }

    if (strcmp(arg, "commit") == 0) {
        reply_amiibo_result(virtual_amiibo_store_v3_upload_active()
            ? virtual_amiibo_store_v3_upload_commit()
            : virtual_amiibo_store_upload_commit());
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
        virtual_amiibo_status_t status;
        virtual_amiibo_store_status(&status);
        virtual_amiibo_result_t result;
        if (status.v3_loaded) {
            result = read_copy < 0
                ? virtual_amiibo_store_v3_read(
                      (size_t)offset, bytes, (size_t)length)
                : VIRTUAL_AMIIBO_ERROR_NOT_LOADED;
        } else {
            result = read_copy < 0
                ? virtual_amiibo_store_read(
                      (size_t)offset, bytes, (size_t)length)
                : virtual_amiibo_store_read_copy(
                      read_copy != 0, (size_t)offset,
                      bytes, (size_t)length);
        }
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
        reply_amiibo_result(virtual_amiibo_store_v3_loaded()
            ? virtual_amiibo_store_v3_set_presented(true)
            : virtual_amiibo_store_set_presented(true));
        return;
    }
    if (strcmp(arg, "eject") == 0) {
        reply_amiibo_result(virtual_amiibo_store_v3_loaded()
            ? virtual_amiibo_store_v3_set_presented(false)
            : virtual_amiibo_store_set_presented(false));
        return;
    }

    if (strcmp(arg, "clear") == 0) {
        virtual_amiibo_store_request_clear();
        if (reply_transport == CONFIG_REPLY_WIRELESS) {
            // Deferred: never stall core0 during gameplay (see `save` / C6).
            reply("{\"ok\":true,\"queued\":true}");
            return;
        }
        absolute_time_t deadline = make_timeout_time_ms(2000);
        while (virtual_amiibo_store_clear_pending() &&
               !time_reached(deadline))
            tud_task();
        reply(virtual_amiibo_store_clear_pending()
                  ? "{\"error\":\"clear timeout\"}"
                  : "{\"ok\":true}");
        return;
    }

    if (strcmp(arg, "persist") == 0) {
        virtual_amiibo_store_request_persist();
        if (reply_transport == CONFIG_REPLY_WIRELESS) {
            // Deferred: never stall core0 during gameplay (see `save` / C6).
            reply("{\"ok\":true,\"queued\":true}");
            return;
        }
        absolute_time_t deadline = make_timeout_time_ms(2000);
        while (virtual_amiibo_store_persist_pending() &&
               !time_reached(deadline))
            tud_task();
        reply(virtual_amiibo_store_persist_pending()
                  ? "{\"error\":\"persist timeout\"}"
                  : "{\"ok\":true}");
        return;
    }

#ifdef NS2_PRO
    // Controller-as-reader backup (interface-audit G4 Path B): drive NFC commands
    // at a connected genuine Pro Controller 2 to read a physical amiibo, so the app
    // can back it up. Low-level, mirroring the UART `nfcmirror initiator` surface —
    // the app (like tools/nfc_probe.ps1) sequences the reads. NTAG215 reads the full
    // 540; v3 sector-aware reads (sector 1) are the app's responsibility. Requires a
    // genuine Pro2 connected and the reader armed. See amiibo-crypto-research and
    // docs/Amiibo-v3.md §11.1.
    if (strcmp(arg, "reader on") == 0) {
        ns2_nfc_mirror_set_initiator(true);
        reply("{\"ok\":true,\"reader\":true}");
        return;
    }
    if (strcmp(arg, "reader off") == 0) {
        ns2_nfc_mirror_set_initiator(false);
        reply("{\"ok\":true,\"reader\":false}");
        return;
    }
    if (strncmp(arg, "reader send ", 12) == 0) {
        uint8_t command[40];
        size_t length = 0;
        if (!parse_hex_bytes(arg + 12, command, sizeof(command), &length) || length < 8u) {
            reply("{\"error\":\"reader send: expected >=8 bytes of hex\"}");
        } else if (!ns2_nfc_mirror_initiator_submit(command, length)) {
            reply("{\"error\":\"reader not armed, no genuine Pro2, or busy\"}");
        } else {
            snprintf(out, sizeof(out), "{\"ok\":true,\"length\":%u}", (unsigned)length);
            reply(out);
        }
        return;
    }
    if (strcmp(arg, "reader reply") == 0) {
        uint8_t resp[NS2_NFC_MIRROR_RESPONSE_MAX];
        size_t length = 0;
        if (!ns2_nfc_mirror_initiator_take(resp, sizeof(resp), &length)) {
            reply("{\"ready\":false}");
        } else {
            int j = snprintf(out, sizeof(out),
                             "{\"ready\":true,\"length\":%u,\"data\":\"", (unsigned)length);
            for (size_t i = 0; i < length && j < (int)sizeof(out) - 4; i++)
                j += snprintf(out + j, sizeof(out) - j, "%02X", resp[i]);
            snprintf(out + j, sizeof(out) - j, "\"}");
            reply(out);
        }
        return;
    }
#endif  // NS2_PRO

    reply("{\"error\":\"usage: amiibo status|begin|chunk|commit|"
           "commit save2|cancel|read [save1|save2]|downloaded|"
           "select save1|select save2|"
           "present|eject|clear|persist|reader on|off|send <hex>|reply\"}");
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

// Bounded source registry snapshot for the management app.  The wireless
// bridge has a 512-byte response slot, so all four current BTHID sources use a
// compact identity (opaque id, connection, transport, generation, short name).
static void cmd_input_sources(void) {
    ns2_input_arbiter_status_t status;
    ns2_active_input_status(&status);
    int j = snprintf(out, sizeof(out),
                     "{\"active\":%lu,\"pending\":%lu,\"explicit\":%s,"
                     "\"fresh\":%s,\"transitions\":%lu,\"sources\":[",
                     (unsigned long)status.active_id,
                     (unsigned long)status.pending_id,
                     status.explicit_active ? "true" : "false",
                     status.awaiting_fresh ? "true" : "false",
                     (unsigned long)status.transition_count);
    unsigned shown = status.source_count < 4u ? status.source_count : 4u;
    for (unsigned i = 0; i < shown && j < (int)sizeof(out) - 96; ++i) {
        const ns2_input_source_info_t *source = &status.sources[i];
        // 16 characters, not 12: "Controller Link" is the truthful name for a
        // source with no Bluetooth name of its own (ns2_input_source_display_name),
        // and 12 cut it to "Controller L". Four sources cost at most 16 extra
        // bytes, well inside this reply's remaining room in the 512-byte
        // wireless slot, and the per-entry headroom check below is unchanged.
        char name[17];
        unsigned n = 0;
        for (; source->name[n] && n < sizeof(name) - 1u; ++n) {
            unsigned char c = (unsigned char)source->name[n];
            name[n] = (c < 0x20u || c == '"' || c == '\\') ? ' ' : (char)c;
        }
        name[n] = '\0';
        j += snprintf(out + j, sizeof(out) - (size_t)j,
                      "%s{\"id\":%lu,\"conn\":%u,\"transport\":%u,"
                      "\"generation\":%lu,\"name\":\"%.16s\"}",
                      i ? "," : "", (unsigned long)source->id,
                      source->key.dev_addr, source->key.transport,
                      (unsigned long)source->generation,
                      name);
        if (j < 0) j = 0;
        if ((size_t)j >= sizeof(out)) {
            j = (int)sizeof(out) - 1;
            break;
        }
    }
    snprintf(out + j, sizeof(out) - (size_t)j,
             "],\"more\":%s}", status.source_count > shown ? "true" : "false");
    reply(out);
}

static void cmd_input_active(const char *arg) {
    if (!arg || !arg[0]) {
        reply("{\"error\":\"usage: input active <source-id|none>\"}");
        return;
    }
    uint32_t id = 0;
    if (strcmp(arg, "none") != 0) {
        char *end = NULL;
        unsigned long parsed = strtoul(arg, &end, 10);
        if (*arg == '\0' || !end || *end != '\0' || parsed > UINT32_MAX)
            id = UINT32_MAX;
        else
            id = (uint32_t)parsed;
    }
    if (id == UINT32_MAX || !ns2_active_input_request(id)) {
        reply("{\"error\":\"unknown or disconnected input source\"}");
        return;
    }
    snprintf(out, sizeof(out), "{\"ok\":true,\"queued\":true,\"active\":%lu}",
             (unsigned long)id);
    reply(out);
}

// ---------------------------------------------------------------------------
// Bluetooth Keyboard / Keyboard + Mouse configuration
// ---------------------------------------------------------------------------
// This is the complete configuration surface a graphical remapping editor needs:
// read the effective profile, change/clear a binding, restore defaults, and
// read/write the mouse translation settings. It deliberately exposes stable
// textual identifiers ("key:1A", "mouse:1", "lstick_up") rather than firmware
// constants, so a UI never has to know a report layout or reconstruct defaults
// from source.
//
// Responses stay inside the wireless slot. The READ formatters live in
// src/ns2_kbm_commands.c, not here: this file cannot be compiled on the host, so
// pagination written here was only ever checked by hand-written client fixtures
// -- and a page-index bug shipped that way. See that file for the cursor
// contract; this one only dispatches.
static void cmd_kbm_status(void) {
    ns2_kbm_runtime_status_t status;
    ns2_kbm_runtime_status(&status);
    // One shared, host-tested formatter. This surface and the UART one each had
    // their own printf, and adding a field shifted every argument in both --
    // silently, because format/argument drift is not a compile error.
    (void)ns2_kbm_status_format(&status, out, sizeof(out));
    reply(out);
}

// Render one mapping, whether it is a layout's realized snapshot or a stored
// profile's content. `profile_id` is NS2_KBM_PROFILE_ID_NONE for the realized
// one, which is what `kbm map kb` reads.
//
// Formatted into a buffer sized to the WIRE limit, not to `out`. `out` is 4096
// because the UART console can take that, and formatting a management reply
// into it is exactly how `kbm status` came to be refused over Bluetooth. A reply
// that will not fit is therefore impossible to produce here by construction.
static void cmd_kbm_map(const ns2_kbm_content_t *content,
                        ns2_kbm_layout_t layout, uint8_t profile_id,
                        unsigned cursor) {
    char wire[NS2_KBM_REPLY_MAX_BYTES + 1u];
    int len = ns2_kbm_format_map(content, layout, profile_id, (uint16_t)cursor,
                                 wire, sizeof(wire));
    if (len < 0) {
        reply("{\"error\":\"mapping does not fit a reply\"}");
        return;
    }
    reply(wire);
}

// Both of these delegate to ns2_kbm_status.c so this surface and the UART
// diagnostic channel cannot drift apart -- the same reason `kbm status` is
// rendered there. Parsing, the response schema, and the accepted field set live
// in exactly one place; range validation stays in ns2_kbm_runtime_set_mouse().
static void cmd_kbm_mouse_get(void) {
    ns2_kbm_mouse_config_t mouse;
    ns2_kbm_runtime_get_mouse(&mouse);
    (void)ns2_kbm_mouse_format(&mouse, out, sizeof(out));
    reply(out);
}

static bool kbm_mouse_apply(const char *args) {
    ns2_kbm_mouse_config_t mouse;
    ns2_kbm_runtime_get_mouse(&mouse);
    if (!ns2_kbm_mouse_command_apply(&mouse, args)) return false;
    return ns2_kbm_runtime_set_mouse(&mouse);
}

// Resolve a legacy mapping target.
//
// `kb` and `kbm` name a LAYOUT and always have. They now mean that layout's
// REALIZED mapping -- what the console is actually running -- which is exactly
// what their existing clients expect from `kbm map` and `kbm bind`. Keeping the
// spelling is what makes the profile system invisible to anyone who does not
// want it.
static bool kbm_layout_arg(const char *name, ns2_kbm_layout_t *out) {
    return name && out && ns2_kbm_layout_from_name(name, out);
}

// Parse a profile id, accepting the reserved word `default`.
static bool kbm_profile_arg(const char *text, uint8_t *out) {
    if (!text || !out) return false;
    if (strcmp(text, "default") == 0) {
        *out = (uint8_t)NS2_KBM_PROFILE_ID_DEFAULT;
        return true;
    }
    unsigned value = 0;
    if (sscanf(text, "%u", &value) != 1) return false;
    if (value < NS2_KBM_PROFILE_ID_FIRST || value > NS2_KBM_PROFILE_ID_MAX)
        return false;
    *out = (uint8_t)value;
    return true;
}

// A user-facing PROFILE POSITION: `default`, or 1..3 within a layout's bank.
//
// Deliberately not a profile id. The user thinks "Profile 2 of the layout I am
// using"; the id is an internal handle, and making the switch surface speak ids
// would force them to know which record lives where.
static bool kbm_position_arg(const char *text, uint8_t *out) {
    if (!text || !out) return false;
    if (strcmp(text, "default") == 0) {
        *out = (uint8_t)NS2_KBM_POSITION_DEFAULT;
        return true;
    }
    unsigned value = 0;
    if (sscanf(text, "%u", &value) != 1) return false;
    if (value < 1u || value > NS2_KBM_POSITIONS_PER_LAYOUT) return false;
    *out = (uint8_t)value;
    return true;
}

// ---------------------------------------------------------------------------
// Staged profile write
// ---------------------------------------------------------------------------
// A profile does not fit one management frame, and looping `kbm bind` is not a
// transaction: a disconnect halfway leaves the adapter running half of one
// mapping and half of another, and every step erases flash.
//
// So a draft is assembled in RAM, entry by entry, and becomes real in ONE
// operation. Nothing before `commit` touches stored or realized state.
//
// One staging buffer is enough: management already admits a single trusted
// session, so there is never a second writer to interleave with. It is static
// rather than stack-allocated for the same reason peers_op_run's workspace is --
// core 1's stack on Pico W is 2048 bytes and this structure is larger than that.
//
// NOTE: this protects against a partial or abandoned TRANSFER. It is not
// protection against power loss during the final config-sector write, which
// remains an existing durability limitation of the single-bank record.
typedef struct {
    bool open;
    bool overflowed;   // an entry did not fit; commit must refuse
    bool creating;     // target is a new profile rather than an existing one
    ns2_kbm_layout_t layout;
    // The bank position an explicit assignment asked for, or 0 for "wherever
    // there is room" (an ordinary New).
    uint8_t position;
    uint8_t profile_id;
    uint16_t expected_revision;
    char name[NS2_KBM_PROFILE_NAME_MAX];
    ns2_kbm_content_t content;
} kbm_draft_t;

static kbm_draft_t kbm_draft;

static void kbm_draft_reset(void) { memset(&kbm_draft, 0, sizeof(kbm_draft)); }

static void cmd_kbm_draft(const char *arg) {
    char verb[10] = {0};
    char rest[64] = {0};
    int fields = sscanf(arg, "%9s %63[^\n]", verb, rest);
    if (fields < 1) {
        reply("{\"error\":\"usage: kbm draft begin|bind|mouse|commit|abort\"}");
        return;
    }

    if (strcmp(verb, "begin") == 0) {
        char layout_name[8] = {0};
        char target[12] = {0};
        unsigned revision = 0;
        char name[NS2_KBM_PROFILE_NAME_MAX] = {0};
        if (sscanf(rest, "%7s %11s %u %19[^\n]", layout_name, target, &revision,
                   name) != 4) {
            reply("{\"error\":\"usage: kbm draft begin <kb|kbm> "
                  "<id|new|pos:N> <baseRevision> <name>\"}");
            return;
        }
        ns2_kbm_layout_t layout;
        if (!kbm_layout_arg(layout_name, &layout)) {
            reply("{\"error\":\"unknown layout\"}");
            return;
        }
        // Beginning again replaces the staging buffer rather than failing: a
        // client that reconnected after a dropped session must be able to start
        // over without a stuck transaction it cannot see or clear.
        kbm_draft_reset();
        kbm_draft.layout = layout;
        unsigned wanted = 0;
        if (strcmp(target, "new") == 0) {
            kbm_draft.creating = true;
        } else if (sscanf(target, "pos:%u", &wanted) == 1) {
            // ASSIGN INTO A BANK POSITION. If that position already holds a
            // profile the upload REPLACES its content, keeping its stable id so
            // a switch key bound to the position keeps working; if it is empty
            // the profile is created there. Either way the user's choice of
            // position is honoured exactly.
            if (wanted < 1u || wanted > NS2_KBM_POSITIONS_PER_LAYOUT) {
                reply("{\"error\":\"position out of range\"}");
                return;
            }
            ns2_kbm_config_t snapshot;
            ns2_kbm_runtime_config_snapshot(&snapshot);
            const ns2_kbm_profile_slot_t *occupant =
                ns2_kbm_profile_at(&snapshot, layout, (uint8_t)wanted);
            if (occupant) {
                kbm_draft.profile_id = occupant->profile_id;
            } else {
                kbm_draft.creating = true;
                kbm_draft.position = (uint8_t)wanted;
            }
        } else if (!kbm_profile_arg(target, &kbm_draft.profile_id) ||
                   kbm_draft.profile_id == NS2_KBM_PROFILE_ID_DEFAULT) {
            // Default is a template. It has no stored content to overwrite, and
            // saving "into" it would make it mutable, which is exactly what the
            // built-in fallback must never become.
            reply("{\"error\":\"invalid profile\"}");
            return;
        }
        kbm_draft.expected_revision = (uint16_t)revision;
        // A draft starts from the layout's Default and is built up by `bind`
        // entries, so what commits is exactly what the client sent -- never a
        // merge with whatever happened to be stored.
        ns2_kbm_template_default(layout, &kbm_draft.content);
        (void)snprintf(kbm_draft.name, sizeof(kbm_draft.name), "%s", name);
        kbm_draft.open = true;
        reply("{\"ok\":true}");
        return;
    }

    if (!kbm_draft.open) {
        reply("{\"error\":\"no draft\"}");
        return;
    }

    if (strcmp(verb, "bind") == 0) {
        char source_text[16] = {0};
        char dest_text[16] = {0};
        if (sscanf(rest, "%15s %15s", source_text, dest_text) != 2) {
            reply("{\"error\":\"usage: kbm draft bind <src> <dst|none>\"}");
            return;
        }
        ns2_kbm_source_t source;
        if (!ns2_kbm_source_parse(source_text, &source)) {
            reply("{\"error\":\"unknown source input\"}");
            return;
        }
        uint8_t destination;
        if (!ns2_kbm_destination_from_name(dest_text, &destination)) {
            reply("{\"error\":\"unknown destination\"}");
            return;
        }
        if (!ns2_kbm_set_binding(&kbm_draft.content, kbm_draft.layout, source,
                                 destination)) {
            // Remembered rather than reported-and-forgotten: a client that
            // ignored this error must not end up committing a mapping that
            // silently lost an entry.
            kbm_draft.overflowed = true;
            reply("{\"error\":\"mapping storage full\"}");
            return;
        }
        reply("{\"ok\":true}");
        return;
    }

    if (strcmp(verb, "mouse") == 0) {
        if (!ns2_kbm_mouse_command_apply(&kbm_draft.content.mouse, rest)) {
            reply("{\"error\":\"bad value\"}");
            return;
        }
        reply("{\"ok\":true}");
        return;
    }

    if (strcmp(verb, "abort") == 0) {
        kbm_draft_reset();
        reply("{\"ok\":true}");
        return;
    }

    if (strcmp(verb, "commit") == 0) {
        if (kbm_draft.overflowed) {
            kbm_draft_reset();
            reply("{\"error\":\"incomplete transaction\"}");
            return;
        }
        ns2_kbm_mouse_config_t verify = kbm_draft.content.mouse;
        if (!ns2_kbm_mouse_sanitize(&verify)) {
            kbm_draft_reset();
            reply("{\"error\":\"invalid settings\"}");
            return;
        }

        if (kbm_draft.creating) {
            uint8_t id = ns2_kbm_runtime_profile_create_at(
                kbm_draft.layout, kbm_draft.position, kbm_draft.name,
                &kbm_draft.content);
            if (id == NS2_KBM_PROFILE_ID_NONE) {
                kbm_draft_reset();
                // Storage full and a duplicate name both land here; the client
                // asked for a new profile and did not get one either way.
                reply("{\"error\":\"profile storage full or name in use\"}");
                return;
            }
            kbm_draft_reset();
            snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%u,\"revision\":1}",
                     id);
            reply(out);
            return;
        }

        uint16_t revision = ns2_kbm_runtime_profile_save(
            kbm_draft.profile_id, kbm_draft.expected_revision, kbm_draft.name,
            &kbm_draft.content);
        uint8_t id = kbm_draft.profile_id;
        kbm_draft_reset();
        if (revision == 0u) {
            // The overwhelmingly common cause is a draft built against an older
            // revision, which is a conflict the client must resolve rather than
            // a failure it should retry.
            reply("{\"error\":\"stale revision\"}");
            return;
        }
        // Saving stores the profile. It deliberately does NOT change what the
        // console is running; that is `kbm apply`.
        snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%u,\"revision\":%u}", id,
                 revision);
        reply(out);
        return;
    }

    reply("{\"error\":\"usage: kbm draft begin|bind|mouse|commit|abort\"}");
}

static void cmd_kbm_profiles(unsigned cursor) {
    ns2_kbm_config_t snapshot;
    ns2_kbm_runtime_config_snapshot(&snapshot);
    char wire[NS2_KBM_REPLY_MAX_BYTES + 1u];
    int len = ns2_kbm_format_profiles(&snapshot, (uint16_t)cursor, wire,
                                      sizeof(wire));
    if (len < 0) {
        reply("{\"error\":\"profile list does not fit a reply\"}");
        return;
    }
    reply(wire);
}

// The realized mapping of each layout, and whether it still matches the profile
// that produced it. This is the reply a client must believe over any local flag.
static void cmd_kbm_active(void) {
    ns2_kbm_config_t snapshot;
    ns2_kbm_runtime_config_snapshot(&snapshot);
    char wire[NS2_KBM_REPLY_MAX_BYTES + 1u];
    int len = ns2_kbm_format_active(&snapshot, wire, sizeof(wire));
    if (len < 0) {
        reply("{\"error\":\"active mappings do not fit a reply\"}");
        return;
    }
    reply(wire);
}

static void cmd_kbm_profile(const char *arg) {
    char verb[10] = {0};
    char rest[64] = {0};
    if (sscanf(arg, "%9s %63[^\n]", verb, rest) < 1) {
        reply("{\"error\":\"usage: kbm profile rename|delete|dup ...\"}");
        return;
    }

    if (strcmp(verb, "rename") == 0) {
        uint8_t id = 0;
        char target[12] = {0};
        char name[NS2_KBM_PROFILE_NAME_MAX] = {0};
        if (sscanf(rest, "%11s %19[^\n]", target, name) != 2 ||
            !kbm_profile_arg(target, &id) ||
            id == NS2_KBM_PROFILE_ID_DEFAULT) {
            reply("{\"error\":\"usage: kbm profile rename <id> <name>\"}");
            return;
        }
        if (!ns2_kbm_runtime_profile_rename(id, name)) {
            reply("{\"error\":\"profile not found or name in use\"}");
            return;
        }
        reply("{\"ok\":true}");
        return;
    }

    if (strcmp(verb, "dup") == 0) {
        uint8_t id = 0;
        char target[12] = {0};
        char name[NS2_KBM_PROFILE_NAME_MAX] = {0};
        if (sscanf(rest, "%11s %19[^\n]", target, name) != 2 ||
            !kbm_profile_arg(target, &id)) {
            reply("{\"error\":\"usage: kbm profile dup <id|default> <name>\"}");
            return;
        }
        ns2_kbm_config_t snapshot;
        ns2_kbm_runtime_config_snapshot(&snapshot);
        ns2_kbm_content_t content;
        ns2_kbm_layout_t layout;
        if (id == NS2_KBM_PROFILE_ID_DEFAULT) {
            // Duplicating Default needs a layout, which the id cannot carry.
            // The name is expected to be qualified by the caller's own layout
            // context; require it explicitly instead of guessing.
            reply("{\"error\":\"use kbm draft begin <layout> new for Default\"}");
            return;
        }
        const ns2_kbm_profile_slot_t *slot = ns2_kbm_profile_find(&snapshot, id);
        if (!slot) {
            reply("{\"error\":\"profile not found\"}");
            return;
        }
        content = slot->content;
        layout = (ns2_kbm_layout_t)slot->layout;
        uint8_t copy = ns2_kbm_runtime_profile_create(layout, name, &content);
        if (copy == NS2_KBM_PROFILE_ID_NONE) {
            reply("{\"error\":\"profile storage full or name in use\"}");
            return;
        }
        snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%u}", copy);
        reply(out);
        return;
    }

    if (strcmp(verb, "delete") == 0) {
        uint8_t id = 0;
        if (!kbm_profile_arg(rest, &id) || id == NS2_KBM_PROFILE_ID_DEFAULT) {
            reply("{\"error\":\"usage: kbm profile delete <id>\"}");
            return;
        }
        if (!ns2_kbm_runtime_profile_delete(id)) {
            reply("{\"error\":\"profile not found\"}");
            return;
        }
        // Deleting the profile that produced a realized mapping falls that
        // layout back to Default deliberately; report the result so a client
        // cannot keep showing a mapping that no longer exists.
        cmd_kbm_active();
        return;
    }

    reply("{\"error\":\"usage: kbm profile rename|delete|dup ...\"}");
}

static void cmd_kbm_apply(const char *arg) {
    char layout_name[8] = {0};
    char target[12] = {0};
    if (sscanf(arg, "%7s %11s", layout_name, target) != 2) {
        reply("{\"error\":\"usage: kbm apply <kb|kbm> <id|default>\"}");
        return;
    }
    ns2_kbm_layout_t layout;
    if (!kbm_layout_arg(layout_name, &layout)) {
        reply("{\"error\":\"unknown layout\"}");
        return;
    }
    uint8_t id = 0;
    if (!kbm_profile_arg(target, &id)) {
        reply("{\"error\":\"invalid profile\"}");
        return;
    }
    bool changed = false;
    if (!ns2_kbm_runtime_apply(layout, id, &changed)) {
        // Almost always a profile belonging to the OTHER layout; say which,
        // because "not found" would send the user looking for the wrong thing.
        reply("{\"error\":\"profile not found for that layout\"}");
        return;
    }
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"layout\":\"%s\",\"id\":%u,\"changed\":%s}",
             ns2_kbm_layout_name(layout), id, changed ? "true" : "false");
    reply(out);
}

static void cmd_kbm(char *arg) {
    if (!arg || !arg[0] || strcmp(arg, "status") == 0) {
        cmd_kbm_status();
        return;
    }

    if (strcmp(arg, "mode") == 0) {
        // `mode` is the EFFECTIVE mode, inferred from the admitted roles;
        // `override` is the persisted user choice, "auto" when inferring.
        ns2_kbm_runtime_status_t status;
        ns2_kbm_runtime_status(&status);
        snprintf(out, sizeof(out),
                 "{\"mode\":\"%s\",\"override\":\"%s\",\"available\":"
                 "[\"auto\",\"controller\",\"keyboard\",\"kbmouse\"]}",
                 ns2_kbm_mode_name((ns2_kbm_mode_t)status.mode),
                 ns2_kbm_mode_name((ns2_kbm_mode_t)status.mode_override));
        reply(out);
        return;
    }
    if (strncmp(arg, "mode ", 5) == 0) {
        ns2_kbm_mode_t mode;
        if (!ns2_kbm_mode_from_name(arg + 5, &mode) ||
            !ns2_kbm_runtime_set_mode(mode)) {
            reply("{\"error\":\"usage: kbm mode auto|controller|keyboard|kbmouse\"}");
            return;
        }
        snprintf(out, sizeof(out), "{\"ok\":true,\"mode\":\"%s\"}",
                 ns2_kbm_mode_name(mode));
        reply(out);
        return;
    }

    if (strncmp(arg, "profile ", 8) == 0) {
        cmd_kbm_profile(arg + 8);
        return;
    }

    // `cursor` is the index of the first item to return, echoed back with a
    // `next` telling the client where to resume. NOT a page index: rows are
    // variable width, so the number that fits a reply is not a constant, and a
    // fixed stride silently drops whatever did not fit. See ns2_kbm_commands.h.
    if (strcmp(arg, "profiles") == 0) {
        cmd_kbm_profiles(0);
        return;
    }

    if (strncmp(arg, "profiles ", 9) == 0) {
        unsigned cursor = 0;
        if (sscanf(arg + 9, "%u", &cursor) != 1 ||
            cursor > NS2_KBM_MAX_PROFILES) {
            reply("{\"error\":\"usage: kbm profiles [cursor]\"}");
            return;
        }
        cmd_kbm_profiles(cursor);
        return;
    }

    if (strcmp(arg, "counters") == 0) {
        ns2_kbm_runtime_status_t status;
        ns2_kbm_runtime_status(&status);
        (void)ns2_kbm_counters_format(&status, out, sizeof(out));
        reply(out);
        return;
    }

    if (strcmp(arg, "active") == 0) {
        cmd_kbm_active();
        return;
    }

    // --- resident-slot selection and standalone switching ------------------
    //
    // `apply` is the RUNTIME change (see cmd_kbm_apply): it costs no flash and
    // does not survive a power cycle. `boot` is the persisted choice. They are
    // deliberately separate commands because conflating them would either put a
    // flash erase on every activation or make a runtime switch permanent.

    // A POSITION, not a slot id: `default` or 1..3, resolved through the layout.
    if (strncmp(arg, "boot ", 5) == 0) {
        char name[8] = {0};
        char target[12] = {0};
        ns2_kbm_layout_t layout;
        uint8_t position = 0;
        if (sscanf(arg + 5, "%7s %11s", name, target) != 2 ||
            !kbm_layout_arg(name, &layout) ||
            !kbm_position_arg(target, &position)) {
            reply("{\"error\":\"usage: kbm boot <kb|kbm> <default|1-3>\"}");
            return;
        }
        bool changed = false;
        if (!ns2_kbm_runtime_set_boot_position(layout, position, &changed)) {
            reply("{\"error\":\"that position is empty for this layout\"}");
            return;
        }
        // Only a real change is worth persisting.
        if (changed) config_request_save();
        snprintf(out, sizeof(out), "{\"ok\":true,\"changed\":%s}",
                 changed ? "true" : "false");
        reply(out);
        return;
    }

    if (strcmp(arg, "switches") == 0) {
        ns2_kbm_config_t snapshot;
        ns2_kbm_runtime_config_snapshot(&snapshot);
        char wire[NS2_KBM_REPLY_MAX_BYTES + 1u];
        if (ns2_kbm_format_switches(&snapshot, wire, sizeof(wire)) < 0) {
            reply("{\"error\":\"switch list does not fit a reply\"}");
            return;
        }
        reply(wire);
        return;
    }

    // No layout argument: one physical key means one ACTION in both layouts.
    if (strncmp(arg, "switch ", 7) == 0) {
        char source_text[16] = {0};
        char target[12] = {0};
        ns2_kbm_source_t source;
        uint8_t position = 0;
        if (sscanf(arg + 7, "%15s %11s", source_text, target) != 2 ||
            !ns2_kbm_source_parse(source_text, &source)) {
            reply("{\"error\":\"usage: kbm switch <key:NN> "
                  "<default|1-3|none>\"}");
            return;
        }
        if (strcmp(target, "none") == 0) {
            position = (uint8_t)NS2_KBM_SWITCH_NONE;
        } else if (!kbm_position_arg(target, &position)) {
            reply("{\"error\":\"usage: kbm switch <key:NN> "
                  "<default|1-3|none>\"}");
            return;
        }
        if (!ns2_kbm_runtime_switch_bind(source, position)) {
            reply("{\"error\":\"invalid source or switch table full\"}");
            return;
        }
        config_request_save();
        reply("{\"ok\":true}");
        return;
    }

    if (strncmp(arg, "apply ", 6) == 0) {
        cmd_kbm_apply(arg + 6);
        return;
    }

    if (strncmp(arg, "draft ", 6) == 0) {
        cmd_kbm_draft(arg + 6);
        return;
    }

    // --- legacy surface -------------------------------------------------
    // `map`, `bind` and `reset` name a LAYOUT and act on its REALIZED mapping,
    // which is what the console is running and what their existing clients have
    // always meant. A profile's stored content is read with `kbm pmap`.

    if (strncmp(arg, "map ", 4) == 0) {
        char name[8] = {0};
        unsigned cursor = 0;
        ns2_kbm_layout_t layout;
        if (sscanf(arg + 4, "%7s %u", name, &cursor) < 1 ||
            !kbm_layout_arg(name, &layout) || cursor > NS2_KBM_MAX_EFFECTIVE) {
            reply("{\"error\":\"usage: kbm map <kb|kbm> [cursor]\"}");
            return;
        }
        ns2_kbm_config_t snapshot;
        ns2_kbm_runtime_config_snapshot(&snapshot);
        cmd_kbm_map(&snapshot.active[layout].content, layout,
                    (uint8_t)NS2_KBM_PROFILE_ID_NONE, cursor);
        return;
    }

    // Read one STORED profile's mapping, as opposed to the realized one.
    if (strncmp(arg, "pmap ", 5) == 0) {
        char target[12] = {0};
        unsigned cursor = 0;
        uint8_t id = 0;
        if (sscanf(arg + 5, "%11s %u", target, &cursor) < 1 ||
            !kbm_profile_arg(target, &id) ||
            cursor > NS2_KBM_MAX_EFFECTIVE) {
            reply("{\"error\":\"usage: kbm pmap <id> [cursor]\"}");
            return;
        }
        ns2_kbm_config_t snapshot;
        ns2_kbm_runtime_config_snapshot(&snapshot);
        const ns2_kbm_profile_slot_t *slot = ns2_kbm_profile_find(&snapshot, id);
        if (!slot) {
            reply("{\"error\":\"profile not found\"}");
            return;
        }
        cmd_kbm_map(&slot->content, (ns2_kbm_layout_t)slot->layout,
                    slot->profile_id, cursor);
        return;
    }

    if (strncmp(arg, "bind ", 5) == 0) {
        char name[8] = {0};
        char source_text[16] = {0};
        char dest_text[16] = {0};
        if (sscanf(arg + 5, "%7s %15s %15s", name, source_text, dest_text) != 3) {
            reply("{\"error\":\"usage: kbm bind <kb|kbm> <key:NN|mouse:N> "
                  "<dest|none|default>\"}");
            return;
        }
        ns2_kbm_layout_t layout;
        ns2_kbm_source_t source;
        if (!kbm_layout_arg(name, &layout)) {
            reply("{\"error\":\"unknown layout\"}");
            return;
        }
        if (!ns2_kbm_source_parse(source_text, &source)) {
            reply("{\"error\":\"unknown source input\"}");
            return;
        }
        bool ok;
        if (strcmp(dest_text, "default") == 0) {
            ok = ns2_kbm_runtime_clear_binding(layout, source);
        } else {
            uint8_t destination;
            if (!ns2_kbm_destination_from_name(dest_text, &destination)) {
                reply("{\"error\":\"unknown destination\"}");
                return;
            }
            ok = ns2_kbm_runtime_set_binding(layout, source, destination);
        }
        if (!ok) {
            reply("{\"error\":\"mapping storage full\"}");
            return;
        }
        snprintf(out, sizeof(out),
                 "{\"ok\":true,\"layout\":\"%s\",\"src\":\"%s\",\"dst\":\"%s\"}",
                 ns2_kbm_layout_name(layout), source_text, dest_text);
        reply(out);
        return;
    }

    if (strncmp(arg, "reset", 5) == 0) {
        const char *what = arg[5] == ' ' ? arg + 6 : "";
        if (strcmp(what, "all") == 0 || what[0] == '\0') {
            ns2_kbm_runtime_reset_all();
            reply("{\"ok\":true,\"reset\":\"all\"}");
            return;
        }
        ns2_kbm_layout_t layout;
        if (!kbm_layout_arg(what, &layout)) {
            reply("{\"error\":\"usage: kbm reset kb|kbm|all\"}");
            return;
        }
        ns2_kbm_runtime_reset_layout(layout);
        snprintf(out, sizeof(out), "{\"ok\":true,\"reset\":\"%s\"}",
                 ns2_kbm_layout_name(layout));
        reply(out);
        return;
    }

    if (strcmp(arg, "mouse") == 0) {
        cmd_kbm_mouse_get();
        return;
    }
    if (strncmp(arg, "mouse ", 6) == 0) {
        if (!kbm_mouse_apply(arg + 6)) {
            reply("{\"error\":\"usage: kbm mouse <sensitivity|sensitivityx|"
                  "sensitivityy|recenter|invertx|inverty|antideadzone> "
                  "<value>\"}");
            return;
        }
        cmd_kbm_mouse_get();
        return;
    }

    reply("{\"error\":\"usage: kbm status|mode|map|bind|reset|mouse\"}");
}

// Current output personality (read-only). Lets the management app display the mode and gate
// mode-specific controls (e.g. amiibo controls are only meaningful in Pro2). The switch action
// itself is a separate future command (see docs/bluetooth/app-interface-audit.md G2). "config" is
// omitted from the controller list since it is the configuration personality, not a controller.
static const char *personality_short_name(usb_personality_t p) {
    switch (p) {
        case USB_PERSONALITY_SWITCH2_PRO2: return "pro2";
        case USB_PERSONALITY_NSO_GAMECUBE: return "gc";
        case USB_PERSONALITY_JOYCON2_L:    return "jcl";
        case USB_PERSONALITY_JOYCON2_R:    return "jcr";
        case USB_PERSONALITY_CDC_CONFIG:   return "config";
        default:                           return "unknown";
    }
}

static void cmd_personality(void) {
    snprintf(out, sizeof(out),
             "{\"current\":\"%s\",\"available\":[\"pro2\",\"gc\",\"jcl\",\"jcr\"]}",
             personality_short_name(g_usb_personality));
    reply(out);
}

// Request a switch to a specific controller output personality. Queues the same
// re-enumeration the BOOTSEL single-tap cycle uses (owner hardware-confirmed on a
// live Switch 2 — the console drops the old controller and detects the new one);
// see docs/bluetooth/app-interface-audit.md G2. The USB re-enumeration briefly
// disconnects the console (and, over USB CDC, this reply); over BLE the reply is
// unaffected. CDC_CONFIG is intentionally not a valid target.
static void cmd_personality_set(const char *target) {
    usb_personality_t p;
    if (strcmp(target, "pro2") == 0)      p = USB_PERSONALITY_SWITCH2_PRO2;
    else if (strcmp(target, "gc") == 0)   p = USB_PERSONALITY_NSO_GAMECUBE;
    else if (strcmp(target, "jcl") == 0)  p = USB_PERSONALITY_JOYCON2_L;
    else if (strcmp(target, "jcr") == 0)  p = USB_PERSONALITY_JOYCON2_R;
    else { reply("{\"error\":\"usage: personality <pro2|gc|jcl|jcr>\"}"); return; }
    if (p == g_usb_personality) { reply("{\"ok\":true,\"unchanged\":true}"); return; }
    g_usb_requested_personality = p;
    g_usb_personality_request_pending = true;
    reply("{\"ok\":true,\"switching\":true}");
}

// Apply host-visible configuration that is sampled during USB enumeration
// without changing personality. The acknowledgement is queued before core0's
// USB task consumes the edge, so an in-band BLE client keeps its management
// relationship while the console sees a clean detach/reconnect.
static void cmd_reenumerate(void) {
    if (g_usb_personality == USB_PERSONALITY_CDC_CONFIG) {
        reply("{\"error\":\"unavailable in config personality\"}");
        return;
    }
    g_usb_reenumerate_request_pending = true;
    reply("{\"ok\":true,\"reenumerating\":true}");
}

// In-band BLE management gate (docs/bluetooth/in-band-management-plan.md). When
// enabled, the config BLE service arms and stays connectable in a normal
// controller personality, so a phone/web portal can manage the adapter without
// the CDC Config re-enumeration that drops the console. `mgmt on/off` flips the
// runtime gate; `mgmt`/`mgmt status` reports it. RAM-only by design: production
// builds boot ON, `mgmt off` disables it for the current boot, and a reboot
// restores ON. Writes require a stored LE bond plus active 16-byte encryption;
// creating a new management bond requires the physical pairing window.
static void cmd_mgmt(const char *arg) {
    if (arg == NULL || arg[0] == '\0' || strcmp(arg, "status") == 0) {
        // report only
    } else if (strcmp(arg, "on") == 0) {
        g_mgmt_enabled = true;
    } else if (strcmp(arg, "off") == 0) {
        g_mgmt_enabled = false;
    } else {
        reply("{\"error\":\"usage: mgmt status|on|off\"}");
        return;
    }
    snprintf(out, sizeof(out), "{\"ok\":true,\"enabled\":%s}",
             g_mgmt_enabled ? "true" : "false");
    reply(out);
}

// Saved-pairing management for the app: list the stored LE bonds and remove one
// by index. The LE device DB is owned by the BTstack thread, so the op is
// marshaled to core1. Wireless callers complete asynchronously so core0 keeps
// servicing the console; CDC Config may synchronously pump USB while waiting.
// Classic-BT bonds are managed via the triple-tap full wipe, not per-entry.
static void reply_bonds_result(bool is_remove, bool is_page) {
    if (is_remove) {
        reply(btstack_host_bonds_remove_ok() ? "{\"ok\":true}"
                                             : "{\"error\":\"no such bond\"}");
    } else if (!is_page && !btstack_host_bonds_list_complete()) {
        // The old spelling is intentionally all-or-error. New clients use the
        // v2 cursor form after receiving this compact signal.
        reply("{\"error\":\"response_too_large\",\"code\":413}");
    } else {
        // Both legacy and v2 operations return a bounded JSON envelope. The
        // legacy envelope retains `bonds`, so older clients ignore metadata.
        reply(btstack_host_bonds_list_json());
    }
}

static void cmd_bonds(const char *arg) {
    mgmt_bonds_action_t action;
    int value;
    if (!mgmt_bonds_parse_command(arg, &action, &value)) {
        reply("{\"error\":\"usage: bonds list|list v2 [cursor]|remove <index>\"}");
        return;
    }
    bool is_remove = action == MGMT_BONDS_REMOVE;
    bool is_page = action == MGMT_BONDS_LIST_PAGE;
    bool requested = is_page
        ? btstack_host_bonds_request_list_page(value)
        : btstack_host_bonds_request(is_remove, is_remove ? value : -1);
    if (!requested) {
        reply("{\"error\":\"busy\"}");
        return;
    }
    if (reply_transport == CONFIG_REPLY_WIRELESS) {
        wireless_bonds.active = true;
        wireless_bonds.is_remove = is_remove;
        wireless_bonds.is_page = is_page;
        wireless_bonds.session = wireless_reply_session;
        wireless_bonds.deadline = make_timeout_time_ms(1000);
        return;
    }
    absolute_time_t deadline = make_timeout_time_ms(1000);
    while (!btstack_host_bonds_done() && !time_reached(deadline))
        tud_task();
    if (!btstack_host_bonds_done()) {
        reply("{\"error\":\"timeout\"}");
        return;
    }
    reply_bonds_result(is_remove, is_page);
}

// Read-only logical peer inventory for the management app.
//
// Distinct from `bonds`, which enumerates raw LE device-DB slots. This merges
// the Classic link-key store and the LE device DB into one row per physical
// device, annotates each with the role the adapter can currently PROVE, and
// carries no key material of any kind. A peer whose owner has not been seen
// since boot is reported with role "unknown" rather than assumed to be a
// controller; persistent role metadata is deliberately not part of this pass.
// Success and failure are both already fully described by the buffer core1
// produced -- a page, or the compact response_too_large error the formatter
// writes when it cannot make progress. Re-deriving a second wording here would
// give the same condition two descriptions.
static void reply_peers_result(void) {
    reply(btstack_host_peers_json());
}

static void cmd_peers(const char *arg) {
    mgmt_peers_action_t action;
    int cursor;
    char peer_id[MGMT_PEERS_ID_MAX];
    if (!mgmt_peers_parse_command(arg, &action, &cursor,
                                  peer_id, sizeof(peer_id))) {
        reply("{\"error\":\"usage: peers list [cursor]|forget <id>\"}");
        return;
    }
    // Both forms use the same deferred reply: the answer is built on core1 and
    // collected on a later poll, so core0 keeps servicing the console. `forget`
    // is the mutating form and carries its own verified result, including the
    // refusal when the target is this adapter's management companion.
    bool requested = action == MGMT_PEERS_FORGET
        ? btstack_host_peers_request_forget(peer_id)
        : btstack_host_peers_request_page(cursor);
    if (!requested) {
        reply("{\"error\":\"busy\"}");
        return;
    }
    if (reply_transport == CONFIG_REPLY_WIRELESS) {
        wireless_peers.active = true;
        wireless_peers.session = wireless_reply_session;
        wireless_peers.deadline = make_timeout_time_ms(1000);
        return;
    }
    absolute_time_t deadline = make_timeout_time_ms(1000);
    while (!btstack_host_peers_done() && !time_reached(deadline))
        tud_task();
    if (!btstack_host_peers_done()) {
        reply("{\"error\":\"timeout\"}");
        return;
    }
    reply_peers_result();
}

// Remote controller pairing. Drives the SAME pairing state machine the BOOTSEL
// gesture drives -- there is no second flow -- and differs only in authority:
// a request arriving over the air opens controller discovery without admitting
// a new management bond. The firmware owns the deadline, so losing the app can
// never leave the adapter discoverable.
static void cmd_pairing(const char *arg) {
    mgmt_pairing_action_t action;
    if (!mgmt_pairing_parse_command(arg, &action)) {
        reply("{\"error\":\"usage: pairing start|status|cancel\"}");
        return;
    }
    if (!btstack_host_pairing_request((uint8_t)action)) {
        reply("{\"error\":\"busy\"}");
        return;
    }
    if (reply_transport == CONFIG_REPLY_WIRELESS) {
        wireless_pairing.active = true;
        wireless_pairing.session = wireless_reply_session;
        wireless_pairing.deadline = make_timeout_time_ms(1000);
        return;
    }
    absolute_time_t deadline = make_timeout_time_ms(1000);
    while (!btstack_host_pairing_done() && !time_reached(deadline))
        tud_task();
    if (!btstack_host_pairing_done()) {
        reply("{\"error\":\"timeout\"}");
        return;
    }
    reply(btstack_host_pairing_json());
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
// `src` is the motion source class (SWITCH_MOTION_SOURCE_*), which selects the seam row that
// rotated this sample onto the carrier frame — the field to read first for any axis complaint.
// `enc` is the single motion encoder's live state. (Getter prototypes come from switch_pro2.h.)
// NS2_PRO-only: the ns2_dbg_* bodies only exist in switch_pro2.c, which is entirely #ifdef
// NS2_PRO. Found 2026-07-12: this command had no guard at all, so
// -DNS2_PRO=OFF (the plain Switch-1 build) failed to link ever since these commands were added
// 2026-07-10 — a real, unnoticed regression, not something introduced by this pass.
#ifdef NS2_PRO
static void cmd_imu(void) {
    switch_pro_input_t in;
    get_global_gamepad_input(0, &in);
    uint8_t rid = 0, st = 0, mlen = 0;
    ns2_dbg_report_state(&rid, &st, &mlen);
    ns2_ds5_motion_diag_t m;
    ns2_dbg_ds5_motion(&m);
    snprintf(out, sizeof(out),
             "{\"hm\":%u,\"src\":%u,\"a\":[%d,%d,%d],\"g\":[%d,%d,%d],"
             "\"rid\":%u,\"stream\":%u,\"mlen\":%u,"
             "\"enc\":{\"on\":%u,\"active\":%u,\"warm\":%u,\"updates\":%lu,"
             "\"bias\":[%ld,%ld,%ld],\"corr\":[%ld,%ld,%ld],"
             "\"q\":[%ld,%ld,%ld,%ld],\"reject\":%lu,\"dt_us\":%lu}}",
             in.has_motion, in.motion_source,
             in.accel[0], in.accel[1], in.accel[2],
             in.gyro[0], in.gyro[1], in.gyro[2], rid, st, mlen,
             m.enabled, m.source_active, m.has_sample,
             (unsigned long)m.updates,
             (long)m.bias_gyro[0], (long)m.bias_gyro[1], (long)m.bias_gyro[2],
             (long)m.corrected_gyro[0], (long)m.corrected_gyro[1],
             (long)m.corrected_gyro[2],
             (long)m.quaternion_million[0], (long)m.quaternion_million[1],
             (long)m.quaternion_million[2], (long)m.quaternion_million[3],
             (unsigned long)m.representation_rejects,
             (unsigned long)m.host_dt_us);
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
        // bridge_contract and build are how the companion detects that the
        // FLASHED firmware is older than the installed app. Source-level parity
        // checks compare source tree to source tree and cannot see this; the
        // resulting skew silently disables battery/motion/rumble while leaving
        // buttons working. See tools/fixtures/android_controller_hid.h.
        char info[192];
        snprintf(info, sizeof(info),
                 "{\"id\":\"picoswitch\",\"product\":\"PicoSwitch Config\","
                 "\"version\":\"2.0\",\"bridge_contract\":%u,\"build\":\"%s\"}",
                 (unsigned)ANDROID_BRIDGE_CONTRACT_VERSION, PICOSWITCH_BUILD_ID);
        reply(info);
    } else if (strcmp(cmd, "ping") == 0) {
        reply("{\"ok\":true}");
    } else if (strcmp(cmd, "get") == 0) {
        cmd_get();
    } else if (strcmp(cmd, "state") == 0) {
        cmd_state();
    } else if (strcmp(cmd, "device") == 0) {
        cmd_device();
    } else if (strcmp(cmd, "input sources") == 0) {
        cmd_input_sources();
    } else if (strncmp(cmd, "input active ", 13) == 0) {
        // Wireless RX has already passed mgmt_allow_write(): bonded,
        // encrypted, enabled, and allowlisted.  The command was intentionally
        // blocked before management authorization landed; keep one shared
        // mutation path now that the transport supplies that security gate.
        cmd_input_active(cmd + 13);
    } else if (strcmp(cmd, "kbm") == 0) {
        cmd_kbm(NULL);
    } else if (strncmp(cmd, "kbm ", 4) == 0) {
        cmd_kbm(cmd + 4);
    } else if (strcmp(cmd, "personality") == 0) {
        cmd_personality();
    } else if (strncmp(cmd, "personality ", 12) == 0) {
        cmd_personality_set(cmd + 12);
    } else if (strcmp(cmd, "reenumerate") == 0) {
        cmd_reenumerate();
    } else if (strncmp(cmd, "bonds ", 6) == 0) {
        cmd_bonds(cmd + 6);
    } else if (strncmp(cmd, "peers ", 6) == 0) {
        cmd_peers(cmd + 6);
    } else if (strncmp(cmd, "pairing ", 8) == 0) {
        cmd_pairing(cmd + 8);
    } else if (strcmp(cmd, "mgmt") == 0) {
        cmd_mgmt(NULL);
    } else if (strncmp(cmd, "mgmt ", 5) == 0) {
        cmd_mgmt(cmd + 5);
#ifdef NS2_PRO
    } else if (strcmp(cmd, "wake") == 0) {
        // Queue an app-initiated console wake. core1's wake service performs it if
        // the console is asleep and a wake identity exists (paired once while on).
        // This reply can only ever confirm the command was DELIVERED -- the work
        // happens later on core1 -- so it deliberately does not claim success.
        // The caller polls `wake status` for the real outcome.
        ns2_wake_manual_request();
        reply("{\"ok\":true,\"queued\":true,\"result\":\"pending\"}");
    } else if (strcmp(cmd, "wake status") == 0) {
        ns2_wake_status_t st;
        ns2_wake_get_status(&st);
        snprintf(out, sizeof(out),
                 "{\"result\":\"%s\",\"consoleAsleep\":%s,\"identityValid\":%s,"
                 "\"attempts\":%lu,\"lastAttemptMs\":%lu}",
                 ns2_wake_result_name(st.result),
                 st.console_asleep ? "true" : "false",
                 st.identity_valid ? "true" : "false",
                 (unsigned long)st.attempts,
                 (unsigned long)st.last_attempt_ms);
        reply(out);
#endif
    } else if (strcmp(cmd, "audiostat") == 0) {
        cmd_audiostat(false);
    } else if (strcmp(cmd, "audiostat reset") == 0) {
        cmd_audiostat(true);
    } else if (strcmp(cmd, "raw") == 0) {
        cmd_raw();
#ifdef NS2_PRO
    } else if (strcmp(cmd, "imu") == 0) {
        cmd_imu();
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
    } else if (strcmp(cmd, "save status") == 0) {
        uint32_t requested = config_save_tracker_requested(&save_tracker);
        uint32_t completed = config_save_tracker_completed(&save_tracker);
        snprintf(out, sizeof(out),
                 "{\"pending\":%s,\"requested\":%lu,\"completed\":%lu}",
                 requested != completed ? "true" : "false",
                 (unsigned long)requested, (unsigned long)completed);
        reply(out);
    } else if (strcmp(cmd, "save") == 0) {
        uint32_t request_id = config_request_save();
        if (reply_transport == CONFIG_REPLY_WIRELESS) {
            // In-band management runs WHILE a controller drives the console, so
            // core0 must never busy-wait for the flash write here -- an up-to-2 s
            // stall would hitch the controller report loop. core1's control tick
            // performs the deferred write at a safe point; ack immediately.
            // docs/bluetooth/in-band-management-plan.md C6.
            snprintf(out, sizeof(out),
                     "{\"ok\":true,\"queued\":true,\"requested\":%lu}",
                     (unsigned long)request_id);
            reply(out);
        } else {
            // CDC Config drops the console for its session, so a synchronous
            // confirmation (pumping USB) is fine and nicer for the wired UI.
            absolute_time_t deadline = make_timeout_time_ms(2000);
            while (!config_save_tracker_reached(
                       config_save_tracker_completed(&save_tracker), request_id) &&
                   !time_reached(deadline))
                tud_task();
            if (!config_save_tracker_reached(
                    config_save_tracker_completed(&save_tracker), request_id)) {
                reply("{\"error\":\"save timeout\"}");
            } else {
                snprintf(out, sizeof(out),
                         "{\"ok\":true,\"requested\":%lu}",
                         (unsigned long)request_id);
                reply(out);
            }
        }
    } else {
        reply("{\"error\":\"unknown command\"}");
    }
}

void config_wireless_task(void) {
    // BLE writes arrive on core1 and are handed across via the wireless bridge.
    // Execute at most one complete command here on core0, using the same parser
    // and persistence behavior as CDC. The browser waits for each JSON-line
    // response before sending another command, so a bounded one-command bridge is
    // intentional. Self-gating: take_command returns false unless the config/
    // management BLE service is armed AND a client has written a full line, so in
    // a normal personality with management off this is a single cheap check.
    if (wireless_bonds.active) {
        if (!config_wireless_bridge_session_active(wireless_bonds.session)) {
            // The client disconnected. The core1 database operation may finish
            // harmlessly, but its result must never cross into a new session.
            wireless_bonds.active = false;
        } else if (!btstack_host_bonds_done() &&
                   !time_reached(wireless_bonds.deadline)) {
            return;
        } else {
            config_reply_transport_t previous = reply_transport;
            reply_transport = CONFIG_REPLY_WIRELESS;
            wireless_reply_session = wireless_bonds.session;
            bool timed_out = !btstack_host_bonds_done();
            bool is_remove = wireless_bonds.is_remove;
            bool is_page = wireless_bonds.is_page;
            wireless_bonds.active = false;
            if (timed_out) {
                reply("{\"error\":\"timeout\"}");
            } else {
                reply_bonds_result(is_remove, is_page);
            }
            reply_transport = previous;
            return;
        }
    }

    if (wireless_peers.active) {
        if (!config_wireless_bridge_session_active(wireless_peers.session)) {
            // Same rule as the bond read: a late core1 result must never cross
            // into a session that did not ask for it.
            wireless_peers.active = false;
        } else if (!btstack_host_peers_done() &&
                   !time_reached(wireless_peers.deadline)) {
            return;
        } else {
            config_reply_transport_t previous = reply_transport;
            reply_transport = CONFIG_REPLY_WIRELESS;
            wireless_reply_session = wireless_peers.session;
            bool timed_out = !btstack_host_peers_done();
            wireless_peers.active = false;
            if (timed_out) {
                reply("{\"error\":\"timeout\"}");
            } else {
                reply_peers_result();
            }
            reply_transport = previous;
            return;
        }
    }

    if (wireless_pairing.active) {
        if (!config_wireless_bridge_session_active(wireless_pairing.session)) {
            wireless_pairing.active = false;
        } else if (!btstack_host_pairing_done() &&
                   !time_reached(wireless_pairing.deadline)) {
            return;
        } else {
            config_reply_transport_t previous = reply_transport;
            reply_transport = CONFIG_REPLY_WIRELESS;
            wireless_reply_session = wireless_pairing.session;
            bool timed_out = !btstack_host_pairing_done();
            wireless_pairing.active = false;
            if (timed_out) {
                // The pairing WINDOW is unaffected: the firmware owns its
                // deadline, so a lost reply costs the client a status read and
                // nothing else. The next `pairing status` reports the truth.
                reply("{\"error\":\"timeout\"}");
            } else {
                reply(btstack_host_pairing_json());
            }
            reply_transport = previous;
            return;
        }
    }

    char wireless_command[CONFIG_WIRELESS_COMMAND_CAPACITY];
    uint32_t session;
    if (config_wireless_bridge_take_command(
            wireless_command, sizeof(wireless_command), &session)) {
        config_reply_transport_t previous = reply_transport;
        reply_transport = CONFIG_REPLY_WIRELESS;
        wireless_reply_session = session;
        handle_line(wireless_command);
        reply_transport = previous;
    }

    // BLE capture entries (see sw2_capture.h) are pulled explicitly via `sw2cap drain`, not
    // auto-streamed here — a client (the web UI) polls it like any other command.
}

// Run one management command and capture its reply, instead of writing it to a
// transport.
//
// EXISTS FOR DIAGNOSIS. The UART console (src/ns2_uart_diag.c) has its own small
// dispatcher that knows a handful of `kbm` verbs; the full management surface
// lives here and is reachable only over BLE or the CDC Config personality. So
// the one channel always available on the bench could not read the commands that
// were failing, and a wire-format defect had to be reasoned about from source
// and confirmed by a user with a companion app open.
//
// Returns the reply length. The reply is truncated into `out_buffer` rather than
// refused, because a diagnostic that hides an oversized reply would conceal
// exactly the class of bug this exists to find; callers report the true length.
size_t config_execute_captured(const char *command, char *out_buffer,
                               size_t capacity) {
    if (!command || !out_buffer || capacity == 0) return 0;
    // handle_line() writes through `line`, which the CDC path also owns. This
    // runs on core0 from the same task loop, so there is no concurrent user.
    size_t length = strlen(command);
    if (length >= LINE_MAX) return 0;
    memcpy(line, command, length + 1u);

    config_reply_transport_t previous = reply_transport;
    reply_transport = CONFIG_REPLY_CAPTURE;
    capture_buffer = out_buffer;
    capture_capacity = capacity;
    capture_length = 0;
    out_buffer[0] = '\0';
    handle_line(line);
    reply_transport = previous;
    capture_buffer = NULL;
    capture_capacity = 0;
    line_len = 0;
    return capture_length;
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
}
