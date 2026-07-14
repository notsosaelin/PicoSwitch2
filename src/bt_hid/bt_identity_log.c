// bt_identity_log.c — see bt_identity_log.h for purpose/design. Producer: core1 (bthid.c, driver
// binding decisions). Consumer: core0, pulled on demand by config.c's `btid dump` handler.
// Cross-core safety mirrors sw2_capture.c exactly (same critical_section_t pattern).

#include "bt_identity_log.h"

#include <string.h>

#include "platform/platform.h"
#include "pico/critical_section.h"

#define BTID_RING 16  // one event per binding decision, not per packet — connections are rare
                       // relative to sw2_capture's traffic volume, so a small ring is enough to
                       // hold a full multi-controller test session between drains

static bt_identity_event_t s_ring[BTID_RING];
static uint32_t s_head;
static uint32_t s_tail;
static uint32_t s_dropped;
static critical_section_t s_lock;
static bool s_lock_init_done;

static void ensure_lock(void) {
    if (!s_lock_init_done) {
        critical_section_init(&s_lock);
        s_lock_init_done = true;
    }
}

static uint16_t desc_fingerprint(const uint8_t* data, uint16_t len) {
    // Cheap additive checksum, not a cryptographic hash — good enough to notice "this
    // descriptor differs from the last one seen for this device," not to prove uniqueness.
    if (!data || !len) return 0;
    uint16_t sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        sum = (uint16_t)(sum + data[i] + (i & 0xFF));
    }
    return sum;
}

void bt_identity_log_record(uint8_t conn_index, bool is_ble, const char* name,
                             uint16_t vendor_id, uint16_t product_id, uint16_t product_version,
                             bt_identity_provenance_t provenance, const uint8_t* class_of_device,
                             uint16_t desc_len, const uint8_t* desc_bytes,
                             const char* driver_name, const char* reason, int8_t player_slot) {
    ensure_lock();
    critical_section_enter_blocking(&s_lock);
    uint32_t next = (s_head + 1) % BTID_RING;
    if (next == s_tail) {
        s_dropped++;
        critical_section_exit(&s_lock);
        return;  // ring full -- drop, never wait for the consumer
    }
    bt_identity_event_t* e = &s_ring[s_head];
    memset(e, 0, sizeof(*e));
    e->timestamp_ms = platform_time_ms();
    e->conn_index = conn_index;
    e->is_ble = is_ble;
    if (name) {
        strncpy(e->name, name, BTID_NAME_LEN - 1);
    }
    e->vendor_id = vendor_id;
    e->product_id = product_id;
    e->product_version = product_version;
    e->provenance = (uint8_t)provenance;
    if (class_of_device) {
        memcpy(e->class_of_device, class_of_device, 3);
    }
    e->desc_len = desc_len;
    e->desc_fingerprint = desc_fingerprint(desc_bytes, desc_len);
    if (driver_name) {
        strncpy(e->driver_name, driver_name, BTID_DRIVER_LEN - 1);
    }
    if (reason) {
        strncpy(e->reason, reason, BTID_REASON_LEN - 1);
    }
    e->player_slot = player_slot;
    s_head = next;
    critical_section_exit(&s_lock);
}

bool bt_identity_log_drain_one(bt_identity_event_t* out) {
    ensure_lock();
    critical_section_enter_blocking(&s_lock);
    if (s_tail == s_head) {
        critical_section_exit(&s_lock);
        return false;
    }
    *out = s_ring[s_tail];
    s_tail = (s_tail + 1) % BTID_RING;
    critical_section_exit(&s_lock);
    return true;
}

uint32_t bt_identity_log_dropped_count(void) { return s_dropped; }

void bt_identity_log_clear(void) {
    ensure_lock();
    critical_section_enter_blocking(&s_lock);
    s_head = 0;
    s_tail = 0;
    s_dropped = 0;
    critical_section_exit(&s_lock);
}

const char* bt_identity_provenance_name(uint8_t provenance) {
    switch ((bt_identity_provenance_t)provenance) {
        case BTID_PROV_UNKNOWN:            return "unknown";
        case BTID_PROV_BLE_ADV_MFR_DATA:   return "ble_adv_mfr_data";
        case BTID_PROV_BLE_DIS_PNP:        return "ble_dis_pnp";
        case BTID_PROV_CLASSIC_SDP:        return "classic_sdp";
        case BTID_PROV_HARDCODED_DEFAULT:  return "hardcoded_default";
        default:                           return "?";
    }
}
