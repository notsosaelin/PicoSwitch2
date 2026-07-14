// bt_identity_log.h — bounded, pull-based controller-identity event log, added 2026-07-12
// for Gate 2 (BT/BLE identity and driver-binding architecture).
//
// Purpose: DATA.md's Gate 2 asks for "one bounded event per connection" recording transport,
// address/identity status, name, VID:PID + provenance, class of device, HID descriptor
// fingerprint, selected driver, match/fallback reason, and slot — so a real hardware session
// (Switch 2 Pro, DualSense, Xbox, Switch 1 Pro, Wiimote +/- attachment, generic XInput, 8BitDo
// NGC DIY) can be captured and read back to build a real compatibility/identity inventory,
// instead of re-deriving it from printf() output that has no backend in this firmware (see
// docs/bluetooth/btstack-implementation.md's rumble-regression section for why that's a dead
// end) or from unsolicited CDC lines the config UI's request/reply protocol can't tolerate.
//
// Design, deliberately modeled on sw2_capture.c's already-solved version of this exact problem
// (pull-based ring buffer, config-mode drain command, never blocks the BT stack):
//   - One entry is appended per *binding decision*, not per connection — initial driver
//     selection (bt_on_hid_ready), any later re-selection when better identity arrives
//     (bthid_update_device_info()'s re-eval path), and HID descriptor arrival (which carries its
//     own length/fingerprint and often arrives after initial binding for BLE devices, per the
//     Gate 2 timing trace). A connection that gets re-bound naturally produces a short history in
//     the log, which is exactly the debugging signal Gate 2 needs (it's what would have made the
//     switch_pro_bt/switch2_ble shadowing bug and the ds4_bt name-fallback gap visible from a
//     hardware session, instead of requiring a code audit to find).
//   - Producer (core1, BT stack callbacks via bthid.c) never blocks: a full ring drops the new
//     entry and counts it, same policy as sw2_capture.
//   - Pull-based drain only — see sw2_capture.h's revision note on why unsolicited CDC output
//     doesn't work with the config web UI's request/reply protocol. Drained via the `btid dump`
//     config-mode command (config.c).
//   - No link keys or other secrets recorded, per DATA.md's explicit instruction.
#ifndef BT_IDENTITY_LOG_H
#define BT_IDENTITY_LOG_H

#include <stdint.h>
#include <stdbool.h>

// Best-effort provenance of vendor_id/product_id at the moment this event was recorded. See
// docs/bluetooth/btstack-implementation.md "Gate 2" for the full evidence-ranking discussion —
// this enum records which tier actually supplied the value on THIS connection, not a fixed
// priority order.
typedef enum {
    BTID_PROV_UNKNOWN = 0,        // VID/PID still 0 at this point (not yet resolved)
    BTID_PROV_BLE_ADV_MFR_DATA,   // pre-connection BLE advertisement manufacturer data (Switch 2)
    BTID_PROV_BLE_DIS_PNP,        // GATT DIS PnP ID (0x2A50), arrives after HID notifications
    BTID_PROV_CLASSIC_SDP,        // Classic BT SDP PnP information
    BTID_PROV_HARDCODED_DEFAULT,  // driver-assigned default (e.g. Wiimote-family lacks PnP SDP)
} bt_identity_provenance_t;

#define BTID_NAME_LEN   32
#define BTID_DRIVER_LEN 24
// 32, not 24: found 2026-07-12 hardware-testing against a real 8BitDo NGC Modkit — the reason
// strings actually used (e.g. "initial-bind-generic-fallback", "vid-resolved-stayed-generic")
// silently truncated at 24. Sized with headroom above the longest string in use.
#define BTID_REASON_LEN 32

typedef struct {
    uint32_t timestamp_ms;             // platform_time_ms() at record time
    uint8_t  conn_index;
    bool     is_ble;
    char     name[BTID_NAME_LEN];      // device name at record time, truncated
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t product_version;
    uint8_t  provenance;               // bt_identity_provenance_t
    uint8_t  class_of_device[3];       // Classic only; zero for BLE
    uint16_t desc_len;                 // HID report descriptor length, 0 if not yet known
    uint16_t desc_fingerprint;         // cheap additive checksum of the descriptor bytes
    char     driver_name[BTID_DRIVER_LEN];  // selected driver's .name
    char     reason[BTID_REASON_LEN];  // short free-text: "initial-bind" / "reeval-upgrade" /
                                        // "reeval-generic-to-specific" / "descriptor-arrived" / etc.
    int8_t   player_slot;              // -1 if not yet assigned
} bt_identity_event_t;

// Record one identity/binding event. Safe to call from core1's BT-stack context; never blocks.
void bt_identity_log_record(uint8_t conn_index, bool is_ble, const char* name,
                             uint16_t vendor_id, uint16_t product_id, uint16_t product_version,
                             bt_identity_provenance_t provenance, const uint8_t* class_of_device,
                             uint16_t desc_len, const uint8_t* desc_bytes,
                             const char* driver_name, const char* reason, int8_t player_slot);

// Pop the oldest buffered entry into *out. Returns true if one was available, false if empty.
bool bt_identity_log_drain_one(bt_identity_event_t* out);

uint32_t bt_identity_log_dropped_count(void);  // entries lost to a full ring since last clear
void bt_identity_log_clear(void);              // drop all buffered entries + reset drop counter

const char* bt_identity_provenance_name(uint8_t provenance);

#endif  // BT_IDENTITY_LOG_H
