// sw2_capture.c — see sw2_capture.h for purpose/design. Producer: core1 (BT stack callbacks in
// btstack_host.c). Consumer: core0, pulled on demand by config.c's `sw2cap drain` handler.
// Cross-core safety uses pico/critical_section.h (the same primitive report.c already uses for
// its cross-core gamepad state) — a few cycles of spinlock contention is not a meaningful stall
// for BT timing; the producer never does I/O (CDC/USB) inside the lock, only a memcpy.

#include "sw2_capture.h"

#include <string.h>

#include "pico/time.h"
#include "pico/critical_section.h"

#define SW2_CAP_RING 128  // ~128 * ~144B =~ 18KB RAM. Full 0x002E audio reports fit without
                          // truncation while preserving the capture facility's prior RAM budget.

static sw2_cap_entry_t s_ring[SW2_CAP_RING];
static uint32_t s_head;  // next write index
static uint32_t s_tail;  // next read index
static uint32_t s_dropped;
static volatile bool s_enabled;
static volatile bool s_pair_enabled;
static volatile sw2_capture_filter_t s_filter = SW2_CAPTURE_FILTER_ALL;
static critical_section_t s_lock;
static bool s_lock_init_done;

static void ensure_lock(void) {
    if (!s_lock_init_done) {
        critical_section_init(&s_lock);
        s_lock_init_done = true;
    }
}

void sw2_capture_set_enabled(bool on) {
    ensure_lock();
    critical_section_enter_blocking(&s_lock);
    s_enabled = on;
    if (on) {
        s_pair_enabled = false;
        // Fresh session: start from an empty ring and a clean drop counter, so a capture
        // session's stats describe only that session, not whatever accumulated before it.
        s_head = 0;
        s_tail = 0;
        s_dropped = 0;
    }
    critical_section_exit(&s_lock);
}

void sw2_capture_set_filter(sw2_capture_filter_t filter) {
    ensure_lock();
    critical_section_enter_blocking(&s_lock);
    s_filter = filter == SW2_CAPTURE_FILTER_NFC
        ? SW2_CAPTURE_FILTER_NFC
        : SW2_CAPTURE_FILTER_ALL;
    critical_section_exit(&s_lock);
}

sw2_capture_filter_t sw2_capture_get_filter(void) {
    return s_filter;
}

void sw2_capture_pair_set_enabled(bool on) {
    ensure_lock();
    critical_section_enter_blocking(&s_lock);
    s_pair_enabled = on;
    if (on) {
        s_enabled = false;
        s_head = 0;
        s_tail = 0;
        s_dropped = 0;
    }
    critical_section_exit(&s_lock);
}

bool sw2_capture_pair_get_enabled(void) { return s_pair_enabled; }

static void record_locked(sw2_capture_kind_t kind, uint16_t handle,
                          const uint8_t *data, uint16_t len) {
    uint32_t next = (s_head + 1) % SW2_CAP_RING;
    if (next == s_tail) {
        s_tail = (s_tail + 1) % SW2_CAP_RING;
        s_dropped++;
    }
    sw2_cap_entry_t *e = &s_ring[s_head];
    e->us = time_us_64();
    e->handle = handle;
    e->kind = (uint8_t)kind;
    e->orig_len = len;
    uint16_t n = len > SW2_CAP_MAX_DATA ? SW2_CAP_MAX_DATA : len;
    e->len = (uint8_t)n;
    if (data && n) memcpy(e->data, data, n);
    s_head = next;
}

bool sw2_capture_get_enabled(void) { return s_enabled; }
uint32_t sw2_capture_dropped_count(void) { return s_dropped; }

uint16_t sw2_capture_buffered_count(void) {
    ensure_lock();
    critical_section_enter_blocking(&s_lock);
    uint32_t count = (s_head >= s_tail) ? (s_head - s_tail) :
                                          (SW2_CAP_RING - s_tail + s_head);
    critical_section_exit(&s_lock);
    return (uint16_t)count;
}

void sw2_capture_record(sw2_capture_kind_t kind, uint16_t handle, const uint8_t *data, uint16_t len) {
    if (!s_enabled) return;
    if (s_filter == SW2_CAPTURE_FILTER_NFC &&
        handle != 0x0014 && handle != 0x001A && handle != 0x001B &&
        handle != 0x0016 && handle != 0x001E && handle != 0x001F &&
        kind != SW2_CAP_NFC_STATE && kind != SW2_CAP_MARKER) {
        return;
    }
    ensure_lock();

    critical_section_enter_blocking(&s_lock);
    // Recheck under the lock: core0 can switch capture modes after the fast
    // path above but before core1 acquires the lock.
    if (s_enabled) {
        // Keep the newest traffic. Human-directed captures commonly run longer
        // than the ring; overwriting the oldest preserves the event of interest.
        record_locked(kind, handle, data, len);
    }
    critical_section_exit(&s_lock);
}

void sw2_capture_pair_record(const uint8_t *data, uint16_t len) {
    if (!s_pair_enabled || !data || len > SW2_CAP_MAX_DATA) return;
    ensure_lock();
    critical_section_enter_blocking(&s_lock);
    if (s_pair_enabled)
        record_locked(SW2_CAP_MOTION_PAIR, 0, data, len);
    critical_section_exit(&s_lock);
}

bool sw2_capture_drain_one(sw2_cap_entry_t *out) {
    ensure_lock();
    critical_section_enter_blocking(&s_lock);
    if (s_tail == s_head) {
        critical_section_exit(&s_lock);
        return false;
    }
    *out = s_ring[s_tail];
    s_tail = (s_tail + 1) % SW2_CAP_RING;
    critical_section_exit(&s_lock);
    return true;
}

const char *sw2_capture_kind_name(uint8_t k) {
    switch ((sw2_capture_kind_t)k) {
        case SW2_CAP_INPUT_NOTIFY: return "input";
        case SW2_CAP_ACK_NOTIFY:   return "ack";
        case SW2_CAP_CMD_OUT:      return "cmd_out";
        case SW2_CAP_CCC_WRITE:    return "ccc_write";
        case SW2_CAP_STATE:        return "state";
        case SW2_CAP_GATT_SVC:     return "gatt_svc";
        case SW2_CAP_GATT_CHAR:    return "gatt_char";
        case SW2_CAP_GATT_DESC:    return "gatt_desc";
        case SW2_CAP_VARIANT:      return "variant";
        case SW2_CAP_WRITE_STATUS: return "write_status";
        case SW2_CAP_MARKER:       return "marker";
        case SW2_CAP_LINK_PARAMS:  return "link_params";
        case SW2_CAP_MOTION_PAIR:  return "motion_pair";
        case SW2_CAP_NFC_NOTIFY:   return "nfc_notify";
        case SW2_CAP_NFC_STATE:    return "nfc_state";
        default:                   return "?";
    }
}

void sw2_capture_mark(const uint8_t *label, uint16_t len) {
    sw2_capture_record(SW2_CAP_MARKER, 0, label, len);
}
