// btstack_host.c - BTstack HID Host (BLE + Classic)
//
// Transport-agnostic BTstack integration for HID devices.
// Uses BTstack's SM (Security Manager) for LE Secure Connections,
// GATT client for HID over GATT Profile (HOGP), and
// HID Host for Classic BT HID devices.

#include "btstack_host.h"
#include "mgmt_bonds.h"
#include "mgmt_peers.h"
#include "mgmt_pairing.h"
#include "ns2_bt_lifecycle.h"
#include "ns2_ble_reconnect.h"
#include "ns2_owner_led.h"
#include "ns2_bt_health.h"
#include "ns2_bt_recovery_runtime.h"
#include "ds5_audio_bridge.h"
#include "ns2_pairing_crypto.h"
#include "mgmt_access.h"
#include "pico/time.h"
#include "hardware/sync.h"   // __dmb() for the cross-core bond snapshot seqlock

#ifdef BTSTACK_DEFER_SCAN
static bool btstack_host_scan_enabled = false;
void btstack_host_enable_scan(void) {
    btstack_host_scan_enabled = true;
    btstack_host_start_scan();
}
#endif
#include "btstack_config.h"
#include "bt_device_db.h"
// Include specific BTstack headers instead of umbrella btstack.h
// (btstack.h pulls in audio codecs which need sbc_encoder.h)
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"

// Run loop depends on transport: embedded for USB dongle, async_context for CYW43,
// FreeRTOS for ESP32
#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
#include "btstack_run_loop_embedded.h"
#endif

// Declare btstack_memory_init - can't include btstack_memory.h due to HID conflicts
extern void btstack_memory_init(void);

#include "bluetooth_data_types.h"
#include "bluetooth_company_id.h"
#include "bluetooth_sdp.h"
#include "ad_parser.h"
#include "gap.h"
#include "hci.h"
#include "hci_cmd.h"
#include "l2cap.h"
#include "ble/sm.h"
#include "ble/gatt_client.h"
#include "ble/att_db.h"
#include "ble/att_db_util.h"
#include "ble/att_server.h"
#include "ble/le_device_db.h"
#include "ble/gatt-service/hids_host.h"
#include "ble/gatt-service/device_information_service_client.h"
#include "ble/gatt-service/battery_service_client.h"
#include "classic/hid_host.h"
#include "classic/sdp_client.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "classic/device_id_server.h"

// Link key storage: TLV (flash) based for all builds
// USB dongle uses pico_flash_bank_instance(), CYW43/ESP32 use their own TLV setup
#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
#include "classic/btstack_link_key_db_tlv.h"
#include "ble/le_device_db_tlv.h"
#include "btstack_tlv_flash_bank.h"
#include "pico/btstack_flash_bank.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#endif

#include "btstack_tlv.h"
#include "hci_dump.h"
#include "hci_dump_embedded_stdout.h"

// BTHID callbacks - for classic BT HID devices
extern void bt_on_hid_ready(uint8_t conn_index);
extern void bt_on_disconnect(uint8_t conn_index);
extern void bt_on_disconnect_with_generation(uint8_t conn_index,
                                             uint32_t connection_generation);
extern void bt_on_hid_report(uint8_t conn_index, const uint8_t* data, uint16_t len);
extern void bt_on_hid_report_with_generation(uint8_t conn_index,
                                             uint32_t connection_generation,
                                             const uint8_t* data, uint16_t len);
extern void bthid_on_feature_report(uint8_t conn_index, const uint8_t* data,
                                    uint16_t len);
extern void bthid_update_device_info(uint8_t conn_index, const char* name,
                                      uint16_t vendor_id, uint16_t product_id);
extern void bthid_set_battery_level(uint8_t conn_index, uint8_t level);
extern void bthid_set_hid_descriptor(uint8_t conn_index, const uint8_t* desc, uint16_t desc_len);

// Platform HAL
extern void platform_reboot(void);

#include <stdio.h>
#include <string.h>

// For rumble feedback passthrough
// Note: manager.h includes tusb.h which conflicts with BTstack, so forward declare
extern int find_player_index(int dev_addr, int instance);
#include "core/services/players/feedback.h"

// Non-invasive raw-BLE-traffic capture (2026-07-10) — see sw2_capture.h. Off by default; adds
// no behavior when disabled. sw2_capture.c avoids tusb.h/BTstack header conflicts itself (it
// only needs tusb.h's tud_cdc_* calls, no BTstack types), so it's safe to include directly here.
#include "sw2_capture.h"
#include "switch2_pro2_audio_transport.h"
#include "switch2_pro2_audio_replay_fixture.h"
#include "ns2_bt_version_probe.h"
#include "ns2_nfc_mirror.h"
#include "ns2_native_motion.h"
#include "ns2_active_input.h"
#include "bt/bthid/bthid.h"
#include "bt_identity_log.h"
#include "config_wireless_bridge.h"
#include "config.h"
#include "usb.h" // read-only personality gate for automatic native Pro2 motion setup
#include "ns2_kbm_runtime.h" // KB/M role-aware Classic discovery admission

// ============================================================================
// FLASH HELPERS (for TLV storage)
// ============================================================================
#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
// Erase both BTstack flash banks (8KB total at end of flash)
static void __no_inline_not_in_flash_func(flash_erase_banks_func)(void* p) {
    (void)p;
    uint32_t flash_offset = PICO_FLASH_SIZE_BYTES - (FLASH_SECTOR_SIZE * 2);
    // Erase both 4KB sectors
    flash_range_erase(flash_offset, FLASH_SECTOR_SIZE * 2);
}

// Erase BTstack flash banks using flash_safe_execute
static void btstack_erase_flash_banks(void) {
    printf("[BTSTACK_HOST] Erasing BTstack flash banks at 0x%lX...\n",
           (unsigned long)(PICO_FLASH_SIZE_BYTES - (FLASH_SECTOR_SIZE * 2)));
    int result = flash_safe_execute(flash_erase_banks_func, NULL, UINT32_MAX);
    if (result == PICO_OK) {
        printf("[BTSTACK_HOST] Flash banks erased successfully\n");
    } else {
        printf("[BTSTACK_HOST] Flash erase failed: %d\n", result);
    }
}
#endif

// ============================================================================
// BLE HID REPORT ROUTING
// ============================================================================

// Deferred processing to avoid stack overflow in BTstack callback
static uint8_t pending_ble_report[64];  // 64 bytes for Switch 2 reports
static uint16_t pending_ble_report_len = 0;
static uint8_t pending_ble_conn_index = 0;
static uint32_t pending_ble_connection_generation = 0;
static volatile bool ble_report_pending = false;

// Forward declare the function to route BLE reports through bthid layer
static void route_ble_hid_report(uint8_t conn_index,
                                 uint32_t connection_generation,
                                 const uint8_t* data, uint16_t len);

// Forward declare Switch 2 functions (defined later with state machine)
static void switch2_retry_init_if_needed(void);
static void switch2_handle_feedback(void);

// ============================================================================
// CONFIGURATION
// ============================================================================

#define MAX_BLE_CONNECTIONS 2
#ifndef SCAN_INTERVAL
#define SCAN_INTERVAL 0x00A0  // 100ms (default)
#endif
#ifndef SCAN_WINDOW
#define SCAN_WINDOW   0x0050  // 50ms (default)
#endif

// ============================================================================
// STATE
// ============================================================================

typedef enum {
    BLE_STATE_IDLE,
    BLE_STATE_SCANNING,
    BLE_STATE_CONNECTING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCOVERING,
    BLE_STATE_READY
} ble_state_t;

typedef struct {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
    hci_con_handle_t handle;
    ble_state_t state;

    // GATT discovery state
    uint16_t hid_service_start;
    uint16_t hid_service_end;
    uint16_t report_char_handle;
    uint16_t report_ccc_handle;

    // Device info
    char name[48];
    const bt_device_profile_t* profile;
    uint16_t vid;
    uint16_t pid;

    // Connection index for bthid layer (offset by MAX_CLASSIC_CONNECTIONS)
    uint8_t conn_index;
    bool hid_ready;
    // Latched when an explicit pairing window admitted this connection. It
    // remains valid while the bounded handshake finishes, even if the scan
    // window closes after the raw ACL connects.
    bool fresh_pairing_admitted;
    // A loose RPA sighting is only permission to reach SM for cryptographic
    // identity resolution. It is never fresh-pair authority.
    bool rpa_trust_candidate;

    // Per-connection BLE HID client id. MUST be per-connection (not a single
    // global) so two BLE HID devices route reports + descriptors independently —
    // a shared cid cross-wires their reports/descriptors -> garbage.
    uint16_t hids_cid;
} ble_connection_t;

// BLE conn_index offset (BLE devices use conn_index >= this value)
#define BLE_CONN_INDEX_OFFSET MAX_CLASSIC_CONNECTIONS

typedef enum {
    GATT_IDLE,
    GATT_DISCOVERING_SERVICES,
    GATT_DISCOVERING_HID_CHARACTERISTICS,
    GATT_ENABLING_NOTIFICATIONS,
    GATT_READY
} gatt_state_t;

static struct {
    bool initialized;
    bool powered_on;
    ble_state_t state;

    // HCI transport (provided by caller)
    const hci_transport_t* hci_transport;

    // Scanning
    bool scan_active;

    // Pending connection
    bd_addr_t pending_addr;
    bd_addr_type_t pending_addr_type;
    char pending_name[48];
    const bt_device_profile_t* pending_profile;
    // Identity established by the parsed manufacturer advertisement. These
    // fields select the bthid driver, Switch 2 custom ATT reconnect path, and
    // native-motion eligibility. Do not overwrite them from an incompletely
    // decoded command/SPI response merely because two bytes resemble VID/PID.
    uint16_t pending_vid;
    uint16_t pending_pid;
    bool pending_fresh_pairing_admitted;
    bool pending_rpa_trust_candidate;

    // Last connected device (for reconnection)
    bd_addr_t last_connected_addr;
    bd_addr_type_t last_connected_addr_type;
    char last_connected_name[48];
    const bt_device_profile_t* last_connected_profile;
    // Durable copy of the validated connection identity above. In particular,
    // Pro Controller 2 gyro requires 057E:2069 and reboot reconnect requires
    // BT_BLE_CUSTOM. Changing either from speculative bytes breaks both paths.
    uint16_t last_connected_vid;
    uint16_t last_connected_pid;
    uint8_t last_connected_ltk[16];
    bool has_last_connected_ltk;
    bool has_last_connected;
    uint32_t reconnect_attempt_time;
    uint8_t reconnect_attempts;
    uint32_t scan_start_time;          // When current scan started (for periodic reconnect)
    uint32_t advertising_reports;
    uint32_t target_advertising_reports;
    uint32_t switch2_advertising_reports;
    uint32_t target_connect_attempts;
    uint32_t target_connect_successes;
    uint32_t target_connect_failures;
    uint32_t reencryption_started;
    uint32_t reencryption_successes;
    uint32_t reencryption_failures;
    uint8_t last_target_advertising_event_type;
    uint8_t last_target_connect_status;
    uint8_t last_reencryption_status;

    // Connections
    ble_connection_t connections[MAX_BLE_CONNECTIONS];

    // GATT discovery state
    gatt_state_t gatt_state;
    hci_con_handle_t gatt_handle;
    uint16_t hid_service_start;
    uint16_t hid_service_end;
    gatt_client_characteristic_t report_characteristic;  // Full HID Report characteristic

    // Callbacks
    btstack_host_report_callback_t report_callback;
    btstack_host_connect_callback_t connect_callback;

    // HIDS Client cid is now PER-CONNECTION (ble_connection_t.hids_cid) so two
    // BLE HID devices don't cross-wire. (Removed the single global hids_cid.)

    // Battery Service Client
    uint16_t bas_cid;

} hid_state;

// HID descriptor storage (shared across connections)
static uint8_t hid_descriptor_storage[1024];  // shared by all MAX_NR_HIDS_HOSTS connections

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;
static ns2_bt_health_t bt_health;
static hci_con_handle_t bt_health_probe_handle = HCI_CON_HANDLE_INVALID;

// Direct notification listener for Xbox HID reports (bypasses HIDS Host)
static gatt_client_notification_t xbox_hid_notification_listener;
static gatt_client_characteristic_t xbox_hid_characteristic;  // Fake characteristic for listener
static void xbox_hid_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// Direct notification listener for Switch 2 HID reports
static gatt_client_notification_t switch2_hid_notification_listener;
static gatt_client_characteristic_t switch2_hid_characteristic;
static void switch2_hid_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// Forward declaration for BLE disconnect cleanup (defined in Switch 2 section)
static void switch2_cleanup_on_disconnect(uint8_t source_conn_index,
                                          uint32_t source_generation);

// ============================================================================
// CLASSIC BT HID HOST STATE
// ============================================================================

#define MAX_CLASSIC_CONNECTIONS 4
#define INQUIRY_DURATION 5  // Inquiry duration in 1.28s units
#define CLASSIC_CONNECT_TIMEOUT_MS 15000  // Max time to establish HID connection

typedef struct {
    bool active;
    uint16_t hid_cid;           // BTstack HID connection ID
    bd_addr_t addr;
    char name[48];
    uint8_t class_of_device[3];
    uint16_t vendor_id;
    uint16_t product_id;
    bool hid_ready;
    const bt_device_profile_t* profile;
    uint32_t connect_time;      // When connection was initiated (for timeout detection)
} classic_connection_t;

static struct {
    bool inquiry_active;
    uint32_t inquiry_restart_at_ms;  // 0 = no deferred inquiry restart pending
    bool use_liac;  // Alternate between GIAC and LIAC for Wiimote/Wii U Pro discovery
    classic_connection_t connections[MAX_CLASSIC_CONNECTIONS];
    // Pending incoming connection info (from HCI_EVENT_CONNECTION_REQUEST)
    bd_addr_t pending_addr;
    uint32_t pending_cod;
    char pending_name[48];
    uint16_t pending_vid;
    uint16_t pending_pid;
    bool pending_valid;
    bool pending_outgoing;  // True if we initiated the connection (hid_host_connect)
    bool pending_trust_present;
    bool pending_fresh_pairing_admitted;
    bool pending_existing_link_key_valid;
    link_key_t pending_existing_link_key;
    link_key_type_t pending_existing_link_key_type;
    bool pending_notified_link_key_valid;
    bool pending_notified_link_key_admitted;
    link_key_t pending_notified_link_key;
    link_key_type_t pending_notified_link_key_type;
    bool pending_auth_succeeded;
    hci_con_handle_t pending_acl_handle;  // ACL handle for pending incoming connection
    const bt_device_profile_t* pending_profile;
    // Pending HID connect (deferred until encryption completes)
    bd_addr_t pending_hid_addr;
    hci_con_handle_t pending_hid_handle;
    bool pending_hid_connect;
    // Set after outgoing HID fails — wait for device to reconnect incoming
    uint32_t waiting_for_incoming_time;  // 0 = not waiting
    // Connection timeout recovery
    uint32_t recovery_start_time;        // When recovery started (0 = no recovery pending)
} classic_state;

// Pre-HID Classic pairing diagnostics. The normal identity log begins at
// bt_on_hid_ready(), which leaves no evidence when a controller is discovered
// but authentication or HID channel setup fails first. Record only the sparse
// state transitions needed to diagnose that boundary; this is RAM-only,
// pull-based through `btid dump`, and does not alter connection behavior.
static void classic_pair_diag(uint8_t conn_index, const char *name, uint32_t cod,
                              uint16_t vendor_id, uint16_t product_id,
                              const char *reason)
{
    uint8_t cod_bytes[3] = {
        (uint8_t)(cod & 0xFF),
        (uint8_t)((cod >> 8) & 0xFF),
        (uint8_t)((cod >> 16) & 0xFF),
    };
    bt_identity_log_record(conn_index, false, name,
                           vendor_id, product_id, 0,
                           (vendor_id || product_id) ? BTID_PROV_CLASSIC_SDP
                                                     : BTID_PROV_UNKNOWN,
                           cod_bytes, 0, NULL,
                           "BTstack pre-HID", reason, -1);
}

// ============================================================================
// WIIMOTE DIRECT L2CAP STATE
// ============================================================================
// Wiimotes don't work well with BTstack's hid_host layer.
// We bypass it and create L2CAP channels directly, like USB Host Shield does.

#ifndef PSM_HID_CONTROL
#define PSM_HID_CONTROL   0x0011
#endif
#ifndef PSM_HID_INTERRUPT
#define PSM_HID_INTERRUPT 0x0013
#endif

typedef enum {
    WIIMOTE_STATE_IDLE,
    WIIMOTE_STATE_W4_CONTROL_CONNECTED,
    WIIMOTE_STATE_W4_INTERRUPT_CONNECTED,
    WIIMOTE_STATE_CONNECTED
} wiimote_state_t;

typedef struct {
    bool active;
    wiimote_state_t state;
    bd_addr_t addr;
    hci_con_handle_t acl_handle;
    uint16_t control_cid;
    uint16_t interrupt_cid;
    char name[48];
    uint8_t class_of_device[3];
    uint16_t vendor_id;
    uint16_t product_id;
    int conn_index;  // Index in classic_state.connections for bthid routing
    bool using_hid_host;  // True if reconnected via HID Host (not direct L2CAP)
    uint16_t hid_host_cid;  // HID Host CID for sending (when using_hid_host is true)
    bool hid_host_ready;  // True when HID Host is ready to send (after DESCRIPTOR_AVAILABLE)
} wiimote_connection_t;

static wiimote_connection_t wiimote_conn;

#ifdef NS2_DS5_AUDIO
#include "bt_hid/bt/btstack/hid_host_long_report.h"
#include "ds5_reconnect_transport.h"
#endif

// Direct-L2CAP output is used by Sony controllers on CYW43 (to avoid their
// problematic SDP path) as well as the Wiimote family. l2cap_send() is not a
// queue: calling it outside a can-send window can reject and lose the report.
// Preserve promised reports and drain them from L2CAP_EVENT_CAN_SEND_NOW.
// Non-audio builds retain the original two-entry footprint. Live-audio builds
// use the same ten-entry depth as DS5Dongle so short radio scheduling stalls do
// not immediately become audible Opus holes.
#ifdef NS2_DS5_AUDIO
#define DIRECT_OUTPUT_QUEUE_DEPTH 10u
#else
#define DIRECT_OUTPUT_QUEUE_DEPTH 2u
#endif
#define DIRECT_OUTPUT_MAX_LEN 548u

typedef struct {
    uint16_t len;
    // DualSense audio report 0x39 is 547 bytes plus the 0xA2 transaction byte.
    // Ordinary controller reports remain <=80 bytes; the larger bound is used
    // only by Sony's direct-L2CAP path in live-audio builds.
    uint8_t data[DIRECT_OUTPUT_MAX_LEN];
} direct_output_entry_t;

typedef struct {
    uint8_t head;
    uint8_t count;
    direct_output_entry_t entries[DIRECT_OUTPUT_QUEUE_DEPTH];
} direct_output_queue_t;

static direct_output_queue_t direct_output_queue;

static void direct_output_clear(void)
{
    direct_output_queue.head = 0;
    direct_output_queue.count = 0;
}

static bool direct_output_pending(void)
{
    return direct_output_queue.count != 0;
}

static void direct_output_try_send(uint16_t cid)
{
    if (!direct_output_pending()) return;
    if (!l2cap_can_send_packet_now(cid)) {
        l2cap_request_can_send_now_event(cid);
        return;
    }

    direct_output_entry_t *entry =
        &direct_output_queue.entries[direct_output_queue.head];
    uint8_t status = l2cap_send(cid, entry->data, entry->len);
    if (status != ERROR_CODE_SUCCESS) {
        l2cap_request_can_send_now_event(cid);
        return;
    }

#ifdef NS2_DS5_AUDIO
    // Record the actual successful L2CAP submission, not the earlier direct
    // queue admission. This makes a send-only spike evidence of radio/L2CAP
    // backpressure while a flat core-1 heartbeat rules out a run-loop stall.
    if (entry->len >= 2u && entry->data[1] == 0x39u)
        ds5_audio_diag_note_l2cap_send(time_us_32());
#endif

    direct_output_queue.head =
        (uint8_t)((direct_output_queue.head + 1u) %
                  DIRECT_OUTPUT_QUEUE_DEPTH);
    direct_output_queue.count--;
    if (direct_output_pending()) {
        l2cap_request_can_send_now_event(cid);
    }
}

// Forward declaration
static void wiimote_l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// SDP query state
static uint8_t sdp_attribute_value[32];
static const uint16_t sdp_attribute_value_buffer_size = sizeof(sdp_attribute_value);
static struct {
    bool pending;
    bool active;
    bd_addr_t addr;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t attempts;
    uint32_t next_attempt_ms;
} classic_identity_query;

static void classic_identity_query_schedule(const bd_addr_t addr);
static void classic_identity_query_service(void);
static void btstack_host_record_fresh_admission(bool accepted);

// Classic HID descriptor storage
static uint8_t classic_hid_descriptor_storage[512];

// SDP Device ID record buffer (needed for DS4/DS5 reconnection)
static uint8_t device_id_sdp_service_buffer[100];

// Find classic connection by hid_cid
static classic_connection_t* find_classic_connection_by_cid(uint16_t hid_cid) {
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (classic_state.connections[i].active && classic_state.connections[i].hid_cid == hid_cid) {
            return &classic_state.connections[i];
        }
    }
    return NULL;
}

// Get conn_index for classic connection
static int get_classic_conn_index(uint16_t hid_cid) {
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (classic_state.connections[i].active && classic_state.connections[i].hid_cid == hid_cid) {
            return i;  // conn_index matches array index
        }
    }
    return -1;
}

// Find free classic connection slot
static classic_connection_t* find_free_classic_connection(void) {
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (!classic_state.connections[i].active) {
            return &classic_state.connections[i];
        }
    }
    return NULL;
}

// ============================================================================
// BLE CONNECTION HELPERS
// ============================================================================

// Get BLE connection by conn_index
static ble_connection_t* find_ble_connection_by_conn_index(uint8_t conn_index) {
    if (conn_index < BLE_CONN_INDEX_OFFSET) return NULL;
    uint8_t ble_index = conn_index - BLE_CONN_INDEX_OFFSET;
    if (ble_index >= MAX_BLE_CONNECTIONS) return NULL;
    if (hid_state.connections[ble_index].handle == HCI_CON_HANDLE_INVALID) return NULL;
    return &hid_state.connections[ble_index];
}

// Get conn_index for BLE connection
static int get_ble_conn_index_by_handle(hci_con_handle_t handle) {
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle == handle) {
            return BLE_CONN_INDEX_OFFSET + i;
        }
    }
    return -1;
}

// Route BLE HID report through bthid layer
static void route_ble_hid_report(uint8_t conn_index,
                                 uint32_t connection_generation,
                                 const uint8_t* data, uint16_t len)
{
    // Build BTHID-compatible packet: DATA|INPUT header + report
    // Buffer needs to hold 1 byte header + up to 64 bytes of report data
    static uint8_t hid_packet[65];
    hid_packet[0] = 0xA1;  // DATA | INPUT header
    if (len <= sizeof(hid_packet) - 1) {
        memcpy(hid_packet + 1, data, len);
        bt_on_hid_report_with_generation(conn_index, connection_generation,
                                         hid_packet, len + 1);
    }
}

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void hids_host_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void hid_host_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static ble_connection_t* find_connection_by_handle(hci_con_handle_t handle);
static ble_connection_t* find_connection_by_hids_cid(uint16_t hids_cid);
static ble_connection_t* find_free_connection(void);
static void start_hids_host(ble_connection_t *conn);
static void register_ble_hid_listener(hci_con_handle_t con_handle);
static void register_switch2_hid_listener(hci_con_handle_t con_handle);
enum {
    SW2_LINK_PHASE_SNAPSHOT = 1,
    SW2_LINK_PHASE_REQUEST  = 2,
    SW2_LINK_PHASE_COMPLETE = 3,
};
static void switch2_capture_link_params(uint8_t phase, uint8_t status,
                                        hci_con_handle_t con_handle, uint16_t interval,
                                        uint16_t latency, uint16_t supervision_timeout);
// MouthPad NUS client hooks (defined in the NUS section below)
static void mp_nus_mark_pending(hci_con_handle_t handle);
static void mp_nus_disconnected(hci_con_handle_t handle);
static void mp_nus_periodic(void);

// Deferred post-HID setup sequencer. After HID report notifications are
// enabled (0x1C), the hids_host needs a moment to return to CONNECTED before
// it will accept a protocol-mode write, and the other GATT clients must run one
// at a time. This runs from btstack_host_process(): phase 0 = write REPORT
// protocol mode (retry until accepted), phase 1 = start DIS/BAS + arm NUS.
// Per-connection so two BLE HID devices each get their own REPORT-mode write +
// notification-enable sequence (a single global would only set up the last device).
static struct {
    bool active;
    hci_con_handle_t handle;
    uint16_t hids_cid;
    uint8_t phase;
    uint32_t start_ms;
    uint32_t phase_ms;
} mp_hid_setup[MAX_BLE_CONNECTIONS];
static void mp_hid_setup_task(void);
static void start_battery_service_client(hci_con_handle_t handle);
static void dis_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// ============================================================================
// INITIALIZATION
// ============================================================================

// Internal function to set up HID handlers (used by both init paths)
// Minimal GATT server for the central. The MouthPad (and other Nordic-based
// HOGP devices built to talk to a companion app) act as a GATT *client* toward
// the host after connecting -- they discover/subscribe on our side. With no ATT
// server answering, that request hangs the full 30s ATT transaction timeout and
// the device terminates the link (reason 0x13) without ever streaming HID.
// Exposing GAP (device name/appearance) + GATT (Service Changed) services makes
// us look like a normal host so the device proceeds to stream.
#define PICO_SWITCH2_BLUETOOTH_NAME "PicoSwitch2"
#define PICO_SWITCH2_BLUETOOTH_NAME_LEN \
    ((uint8_t)(sizeof(PICO_SWITCH2_BLUETOOTH_NAME) - 1u))

// Keep the Classic GAP/EIR name, the ATT Device Name, and the management scan
// response on one product identity. PicoSwitch2 is 11 ASCII bytes; the LE
// complete-local-name AD below therefore has a 0x0C length byte (type + name).
_Static_assert(sizeof(PICO_SWITCH2_BLUETOOTH_NAME) - 1u == 11u,
               "PicoSwitch2 Bluetooth name length changed");
static uint8_t host_att_device_name[] = PICO_SWITCH2_BLUETOOTH_NAME;
// GAP Appearance for the LE management service.
//
// MUST NOT be a HID appearance (0x03C0..0x03FF). PicoSwitch2 is the HID *Host*;
// a HID appearance was always the wrong role, and it deterministically breaks
// the Controller Link on a freshly paired Android device:
//
//   btm_ble_read_remote_appearance_cmpl: Appearance 0x03c0,
//                                        Class of Device 0-5-0 found for ...
//   btif_update_remote_properties: CoD from storage was zero
//   btif_update_remote_properties: ... CoD: 0x001f00 -> 0x000500
//   btif_hd_upstreams_evt: BTA_HD_OPEN_EVT
//   btif_hd_upstreams_evt: remote device is not hid host, disconnecting
//
// Android reads this characteristic during LE pairing. When it has no stored
// Classic CoD ("CoD from storage was zero" -- true for any fresh pairing) it
// synthesises one from the appearance and PERSISTS it. 0x03C0 becomes major
// device class 5 (Peripheral). Fluoride's check_cod_hid() is
// (cod & 0x1F00) == 0x0500, so the phone then classifies this adapter as a HID
// peripheral and its HID Device profile refuses to run against it -- the ACL,
// authentication, encryption and both HID L2CAP channels all succeed first, and
// BTIF_HD drops the link ~40 ms later. 10 of 10 attempts, deterministic.
//
// A bond record created while the adapter was Classic-discoverable holds the
// real CoD instead, which is why this stayed hidden: it only bites on a fresh
// pairing, and the stale-but-correct record masked it indefinitely.
//
// 0x0080 is Generic Computer, matching the Classic CoD 0x000104
// (Computer/Desktop) set at init, so both transports report one coherent
// identity -- the rule the device name above already follows.
#define HOST_ATT_APPEARANCE 0x0080u
_Static_assert((HOST_ATT_APPEARANCE & 0xFFC0u) != 0x03C0u,
               "LE appearance is in the HID range: Android will synthesise a "
               "HID-peripheral Class of Device on a fresh pair and refuse the "
               "Controller Link (check_cod_hid). Keep this a host/computer "
               "appearance consistent with gap_set_class_of_device().");
static const uint8_t host_att_appearance[] = {
    (uint8_t)(HOST_ATT_APPEARANCE & 0xFFu),
    (uint8_t)(HOST_ATT_APPEARANCE >> 8),
};

// Config/in-band BLE management service. UUIDs are project-owned random UUIDs,
// deliberately distinct from Nordic UART so the browser cannot accidentally
// select an unrelated serial-like peripheral.
static const uint8_t config_ble_service_uuid[] = {
    0x7C, 0x5A, 0xD4, 0xED, 0x27, 0x31, 0x41, 0x7C,
    0xB3, 0x16, 0x05, 0x85, 0x05, 0xC7, 0xC0, 0x83,
};
static const uint8_t config_ble_rx_uuid[] = {
    0x52, 0x52, 0x18, 0x6A, 0x81, 0x7F, 0x48, 0x9F,
    0xAD, 0x75, 0x94, 0xC3, 0xBD, 0x44, 0x47, 0x69,
};
static const uint8_t config_ble_tx_uuid[] = {
    0x81, 0x46, 0x27, 0x06, 0x8E, 0x64, 0x40, 0x7A,
    0xBC, 0x3D, 0xD3, 0x03, 0x52, 0x9F, 0xBE, 0x1C,
};

// Advertisement UUID bytes use Bluetooth little-endian wire order. The full
// friendly name lives in scan response data so the primary advertisement stays
// below the 31-byte legacy limit while remaining service-filterable.
static uint8_t config_ble_adv_data[] = {
    2, BLUETOOTH_DATA_TYPE_FLAGS, 0x02,
    17, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
    0x83, 0xC0, 0xC7, 0x05, 0x85, 0x05, 0x16, 0xB3,
    0x7C, 0x41, 0x31, 0x27, 0xED, 0xD4, 0x5A, 0x7C,
};
static uint8_t config_ble_scan_response[] = {
    (uint8_t)(PICO_SWITCH2_BLUETOOTH_NAME_LEN + 1u),
    BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
    'P', 'i', 'c', 'o', 'S', 'w', 'i', 't', 'c', 'h', '2',
};

static struct {
    bool service_available;
    bool mode_active;
    bool advertising;
    bool closing;
    bool notifications_enabled;
    bool tx_requested;
    hci_con_handle_t handle;
    uint16_t rx_value_handle;
    uint16_t tx_value_handle;
    uint16_t tx_ccc_handle;
    btstack_context_callback_registration_t tx_request;
    uint8_t tx_chunk[512];
    // Identity of the connected management/companion client. Recorded because
    // the management link is a BONDED LE link (mgmt_session_authorized requires
    // client_bonded), and BTstack has ONE global le_device_db shared by both
    // roles -- so a companion bond sits in the same database the reconnect
    // selector enumerates. A companion that is connected must not be counted as
    // an absent peer. See btstack_host_pick_reconnect().
    // CONNECTION-LOCAL. This is the over-the-air address from
    // HCI_SUBEVENT_LE_CONNECTION_COMPLETE, which for a phone using LE privacy
    // is a Resolvable Private Address that changes roughly every 15 minutes.
    // It identifies THIS ACL and nothing more durable. Never compare it against
    // anything stored -- see client_identity_addr.
    bd_addr_t client_addr;
    bool client_addr_valid;
    // DURABLE. The bonded identity BTstack resolved that RPA to, taken from
    // SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED. This is the address the LE device
    // DB actually stores, so it is the only one that can be compared against a
    // bond record, a Classic BD_ADDR, or anything that outlives the connection.
    //
    // Everything durable used to compare client_addr instead. Because an RPA is
    // never equal to a stored identity, every one of those comparisons was
    // permanently false: the reconnect selector could not exclude the connected
    // companion, the Classic authentication path could never recognise the
    // companion's own Controller Link, an explicit forget could not drop the
    // management session, and the peer inventory reported the phone twice --
    // once as its bonded identity and once as an unbonded RPA.
    //
    // Only populated when the peer actually uses privacy AND resolution
    // succeeds. A peer connecting with its identity address directly leaves
    // this clear, and config_ble_durable_addr() then falls back to client_addr,
    // which in that case IS the identity.
    bd_addr_t client_identity_addr;
    uint8_t client_identity_addr_type;
    bool client_identity_valid;
    // Per-attempt fresh-bond admission, latched when this connection was
    // ACCEPTED. Controller BLE candidates already work this way
    // (conn->fresh_pairing_admitted, latched from
    // hid_state.pending_fresh_pairing_admitted at connection complete); the
    // management peripheral was the one path that instead re-read the live
    // window at SM confirmation time. That made admission depend on where the
    // 30 s PAIRING_WINDOW_MS deadline happened to fall relative to the user's
    // tap on Android's own pairing dialog, so authorization the user had
    // already given could expire mid-procedure. Latching here makes management
    // match the established per-attempt rule: the window authorizes an
    // ATTEMPT, and that attempt is bounded by its own connection.
    bool fresh_bond_admitted;
} config_ble = {
    .handle = HCI_CON_HANDLE_INVALID,
};

// True while the user's CONTROLLER/HID pairing window is open -- the BOOTSEL
// double-tap window, not a companion/management-specific one. Assigned only by
// btstack_host_set_pairing_window_open(), whose sole caller is the transport's
// set_pairing_mode hook:
//
//   open_pairing_window()            (ns2_bt_host.c)
//     -> bt_set_pairing_mode(true)   (bt_transport.h)
//     -> cyw43_transport_set_pairing_mode()
//          -> btstack_host_set_pairing_window_open(true)
//          -> btstack_host_start_scan()
//
// and symmetrically with false when ns2_bt_host.c closes the window (deadline
// reached, or the logical source became complete).
//
// It was previously named for management because mgmt_access consults it as the
// user-gesture authorization for accepting a new companion bond -- a READER of
// the HID pairing window, not a separate window. The old name made that look
// like companion-only state; it is not.
enum {  // btlife_event_t.code
    BTLIFE_NONE = 0,
    BTLIFE_SCAN_START, BTLIFE_SCAN_STOP, BTLIFE_SCAN_SUPPRESS,
    BTLIFE_ADV_START, BTLIFE_ADV_STOP,
    BTLIFE_MGMT_CONNECT, BTLIFE_MGMT_DISCONNECT,
    BTLIFE_CTRL_DISCONNECT, BTLIFE_HCI_DISCONNECT,
    // ---- Classic paging (added 2026-08-23, instrumentation only) ----------
    //
    // The 100-cycle soak put the failure entirely inside paging: 9 of 101 page
    // attempts ended in HCI_ERR_PAGE_TIMEOUT with no ACL, no authentication and
    // no encryption, while successful pages ranged 0.803-5.679 s against a fixed
    // 5.12 s Android page timeout. Android-side logs cannot say what this
    // adapter was doing at the time, which is what these record.
    //
    // HONEST LIMIT, stated here so nobody over-reads the data: page scan is a
    // BASEBAND function. The controller answers a page and only then raises
    // HCI_Connection_Request. A host therefore cannot distinguish "no page
    // arrived" from "a page arrived and the controller did not answer it" --
    // both appear as the absence of PAGE_RX. What these events DO settle is
    // whether the host ever saw the page (Case C vs A-or-B), whether page scan
    // was even enabled, and what else the radio was doing at that instant.
    BTLIFE_PAGE_SCAN_ON, BTLIFE_PAGE_SCAN_OFF,
    BTLIFE_INQUIRY_START, BTLIFE_INQUIRY_STOP,
    BTLIFE_PAGE_RX,          // HCI_Connection_Request: the page exchange succeeded
    BTLIFE_PAGE_ACCEPT,      // this host let it through the admission filter
    BTLIFE_PAGE_REJECT,      // this host refused it (a in the reject cause)
    BTLIFE_ACL_UP,           // HCI_Connection_Complete, status 0
    BTLIFE_ACL_FAIL,         // HCI_Connection_Complete, status != 0 (a = reason)
    // ---- post-page establishment phases (added 2026-08-23) ---------------
    //
    // The 40-cycle run split the device failures into two families that share
    // nothing but a symptom. One never reaches page_rx at all. The other, cycle
    // 3, recorded page_rx + page_accept at 10:02:08.846 and then NOTHING for
    // 20.3 s until acl_fail status 0x08 -- a phase boundary the previous events
    // could locate only by the size of the gap between them.
    //
    // These name each boundary so a failure says which phase it died in rather
    // than leaving it to be inferred from silence. No timers are added: the gaps
    // are computed by the analysis from the timestamps, so nothing here can
    // change when the stack acts.
    BTLIFE_LINK_KEY_REQ,     // controller asked us for a stored key (a = have it)
    BTLIFE_AUTH_DEFER,       // we declined to initiate authentication
    BTLIFE_AUTH_START,       // we requested security on this link
    BTLIFE_AUTH_DONE,        // Authentication Complete (a = HCI status)
    BTLIFE_ENC_CHANGE,       // Encryption Change (a = status, b = key size)
    BTLIFE_HID_READY,        // both HID channels usable
    BTLIFE_HID_FAIL,         // HID connection opened with a failure (a = status)
};

enum {  // BTLIFE_PAGE_REJECT cause (btlife_event_t.a)
    BTLIFE_REJECT_NONE = 0,
    BTLIFE_REJECT_NO_MGMT,    // cross-transport peer, no live management
    BTLIFE_REJECT_ADMISSION,  // unbonded peer outside a pairing window
    BTLIFE_REJECT_COUNT
};

enum {  // btlife_event_t.flags -- what the radio was doing when the event fired
    BTLIFE_F_INQUIRY   = 1u << 0,  // Classic inquiry round in progress
    BTLIFE_F_LE_SCAN   = 1u << 1,  // LE scan active
    BTLIFE_F_CONNECTING= 1u << 2,  // an LE connect attempt is outstanding
    BTLIFE_F_MGMT      = 1u << 3,  // management ACL present
    BTLIFE_F_MGMT_TRUST= 1u << 4,  // ...and bonded + encrypted
    BTLIFE_F_CLASSIC   = 1u << 5,  // at least one Classic connection present
    BTLIFE_F_WAKE_ADV  = 1u << 6,  // wake advertisement owns the radio
    BTLIFE_F_PAIRING   = 1u << 7,  // controller pairing window open
};
enum {  // SCAN_SUPPRESS cause (btlife_event_t.a) -- why a scan restart was refused
    BTLIFE_CAUSE_NONE = 0, BTLIFE_CAUSE_CONFIG_MODE, BTLIFE_CAUSE_MGMT_ARMED,
    BTLIFE_CAUSE_WAKE, BTLIFE_CAUSE_LOCKOUT, BTLIFE_CAUSE_APP_SUPPRESS,
    BTLIFE_CAUSE_NOT_POWERED, BTLIFE_CAUSE_ALREADY, BTLIFE_CAUSE_COUNT
};

// Sized for a full acceptance soak rather than a single incident. A Controller
// Link cycle costs roughly 6 events plus 2-3 inquiry transitions across its
// ~18 s idle gap, so ~9 per cycle; 1024 slots retains about 110 cycles. The
// previous 48 overflowed inside the first minute of the 100-cycle run
// (events.dropped passed 200), which is why a failure at cycle 93 could not be
// explained from the adapter side afterwards. 1024 * 12 B = 12 KB against
// 236 KB of BSS in a 520 KB part.
//
// Raised to 4096 on 2026-08-23 for the 200-cycle run. With the post-page phase
// events a Controller Link cycle costs roughly 16 slots, so 1024 would have
// retained about 65 cycles and lost any failure in the first two thirds of the
// run -- which is precisely the failure we would then be unable to explain.
// 4096 * 12 B = 48 KB.

// Defined with the ring below; declared here because the page-scan and
// inquiry call sites sit above it.
static void btlife_record(uint8_t code, uint8_t a, uint16_t b);

static bool hid_pairing_window_open;
/*
 * May a NEW management/companion bond be admitted right now?
 *
 * Split from hid_pairing_window_open by Phase 6. They used to be the same flag,
 * so opening a controller pairing window also admitted a new management bond.
 * Locally that is defensible: someone was holding the adapter and pressed the
 * button, and physical presence is the authority. Remotely it is not -- a "pair
 * a controller" request arriving over the air would open a window in which a
 * DIFFERENT phone could claim the management relationship, which is a much
 * larger grant than the user asked for.
 *
 * So management bonding follows the LOCAL gesture only. Remote pairing opens
 * controller discovery and grants no management authority whatsoever. Phase 0
 * recorded this as a product decision owed before Phase 6 (audit §7.3); this is
 * the decision.
 */
static bool mgmt_bond_window_open;
static bool pairing_lockout;
static bool install_reset_bootstrap_consumed;

// Bounded identity-refresh discoverability.
//
// Android stores a Class of Device per remote and its HID Device profile
// refuses any host whose stored class looks like a HID peripheral
// (btif_hd -> check_cod_hid: (cod & 0x1F00) == 0x0500). On a pairing where
// Android has no Classic CoD yet ("CoD from storage was zero") it synthesises
// one from our LE Appearance and persists it. Our real Classic CoD (0x000104,
// Computer/Desktop) only reaches Android through an inquiry response, and we
// are not discoverable outside a pairing window -- so a phone that learned a
// wrong class had no way to ever correct it, and the Controller Link failed
// deterministically forever after.
//
// This window exists purely so Android can re-learn our real class. It grants
// NO pairing authority: hid_pairing_window_open is untouched, so admission,
// SSP response and fresh-key commit all behave exactly as if it were closed.
// It only makes us answer inquiry, which is what carries the CoD.
/*
 * Opens CONTROLLER discovery only. Management bonding is a separate, explicit
 * grant -- see btstack_host_set_management_bond_window_open().
 *
 * This is the shared transport hook (`bt_set_pairing_mode`), reached by BOTH
 * the local gesture and remote pairing, so it must grant the SMALLER authority
 * and let the local path add to it. Phase 6 had it the other way round: this
 * function granted both, and the remote path called
 * btstack_host_set_controller_pairing_window_open(true) afterwards intending to
 * "downgrade" -- which does nothing, because that function only ever CLEARS the
 * management window on close. Remote pairing therefore opened management
 * bonding authority, the exact grant Phase 6 exists to withhold, and for the
 * whole 30 s window another phone could have claimed the management
 * relationship.
 *
 * Fail-safe by default: the shared path gives the least authority, and the
 * larger one has to be asked for by name.
 */
void btstack_host_set_pairing_window_open(bool open)
{
    btstack_host_set_controller_pairing_window_open(open);
}

// Physical presence at the adapter is what authorises a NEW management bond.
// Only the local gesture may call this with `true`; closing the controller
// window clears it unconditionally.
void btstack_host_set_management_bond_window_open(bool open)
{
    mgmt_bond_window_open = open;
}

void btstack_host_set_controller_pairing_window_open(bool open)
{
    hid_pairing_window_open = open;
    if (!open)
        mgmt_bond_window_open = false;
#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF) && !defined(CONFIG_USB2BLE)
    if (hid_state.powered_on) {
        // Bonded controllers remain connectable outside the window. New
        // candidates require this explicit discoverability window; SSP itself
        // is answered per admitted attempt in packet_handler().
        gap_discoverable_control(open && !pairing_lockout);
        gap_connectable_control(!pairing_lockout);
        // Page scan follows connectable. Recorded so a missing PAGE_RX can be
        // tested against "were we even listening" before anything subtler.
        btlife_record(pairing_lockout ? BTLIFE_PAGE_SCAN_OFF
                                      : BTLIFE_PAGE_SCAN_ON, 0u, 0u);
    }
#endif
}

bool btstack_host_pairing_window_open(void)
{
    return hid_pairing_window_open;
}

static bool config_ble_link_trusted(hci_con_handle_t handle)
{
    if (handle == HCI_CON_HANDLE_INVALID) return false;
    return mgmt_link_is_trusted(gap_bonded(handle), gap_encryption_key_size(handle));
}

// Evaluated at CONNECTION ADMISSION, not at SM confirmation.
// See config_ble.fresh_bond_admitted.
// Durable identity of the management companion. Defined after
// config_ble_identity_addr(), which does the authoritative lookup; declared
// here because the reconnect selector above needs it. See the definitions for
// why an RPA must never be used for a durable comparison.
static bool config_ble_durable_addr(bd_addr_t out);
static bool config_ble_addr_is_companion(const uint8_t addr[6]);

static bool config_ble_bond_admission_open(void)
{
    mgmt_state_t state = {
        .enabled = g_mgmt_enabled,
        .console_awake = true,
        .wake_active = false,
        .scanning = false,
        // The MANAGEMENT bonding window, not the controller one. These were the
        // same flag until Phase 6, so a remote "pair a controller" request would
        // have opened a window in which another phone could claim management.
        // See mgmt_bond_window_open.
        .pairing_window_open = mgmt_bond_window_open,
        .client_connected = config_ble.handle != HCI_CON_HANDLE_INVALID,
        .client_bonded = config_ble.handle != HCI_CON_HANDLE_INVALID &&
            gap_bonded(config_ble.handle),
        .client_encrypted = config_ble.handle != HCI_CON_HANDLE_INVALID &&
            gap_encryption_key_size(config_ble.handle) == 16u,
    };
    return mgmt_accept_bonding(&state);
}

static bool config_ble_accept_new_bond(void)
{
    // g_mgmt_enabled is re-read so the `mgmt off` escape hatch still revokes
    // admission for a connection that was latched before it was used.
    return mgmt_accept_latched_bonding(g_mgmt_enabled,
                                       config_ble.fresh_bond_admitted);
}

static void config_ble_start_advertising(void);

// The config/management BLE service (RX/TX GATT + wireless bridge) is authorized
// either in the explicit CDC Config personality (legacy, physically gated) OR
// when in-band management is enabled (g_mgmt_enabled, production default on). Both share
// the same service, bridge, and coexistence rules; only the arming trigger
// differs. When neither holds this returns false and the whole path is the
// proven zero-cost early return -- byte-identical to today's normal mode.
// docs/bluetooth/in-band-management-plan.md C1/C3.
static inline bool config_ble_authorized(void)
{
    return g_usb_config_mode || g_mgmt_enabled;
}

// ---------------------------------------------------------------------------
// In-band management / BLE coexistence diagnostics  (UART-only, always-on)
//
// The config/management BLE service reuses the single LE advertiser and, while
// armed, suppresses controller discovery (a config-mode assumption: config drops
// the console, so no controllers are needed). In-band management keeps the
// service armed DURING gameplay, so this ring + counters exist to isolate the
// observed "controller and management both drop and cannot recover without a
// power cycle" failure over the GP0/GP1 UART link -- the last-good/first-fail
// visibility the Web Portal cannot provide once it has dropped. Purely additive:
// record() only appends to a ring and bumps counters; it never changes BT logic.
// Events are written on core1 (BTstack thread) and read on core0 (UART task);
// the ring is diagnostic, so a benign torn read at most mis-renders one line.
// See docs/bluetooth/in-band-management-plan.md and STATUS.md.
// ---------------------------------------------------------------------------
#define BTLIFE_RING_SIZE 4096u
// Ring-only coalescing window for repeated scan-suppress entries.
#define BTLIFE_SUPPRESS_COALESCE_MS 10000u
typedef struct {
    uint32_t t_ms;
    uint8_t  code;
    uint8_t  a;        // cause / HCI reason / detail
    uint16_t b;        // connection handle, or 0
    uint8_t  flags;    // BTLIFE_F_* radio snapshot at the moment of the event
    uint8_t  addr3[3]; // last three octets of the peer, when one is involved
} btlife_event_t;
static btlife_event_t btlife_ring[BTLIFE_RING_SIZE];
static uint16_t btlife_head;    // next write slot
static uint16_t btlife_count;   // valid entries (<= BTLIFE_RING_SIZE)
static uint32_t btlife_dropped; // oldest events overwritten before being read
static struct {
    uint32_t scan_start, scan_stop, adv_start, adv_stop;
    uint32_t suppress[BTLIFE_CAUSE_COUNT];  // indexed by cause
    uint32_t mgmt_connect, mgmt_disconnect, ctrl_disconnect, hci_disconnect;
    uint16_t last_disc_handle;
    uint8_t last_disc_reason;
} btlife;

static uint32_t btlife_last_suppress_ms;
static uint8_t btlife_last_suppress_cause = 0xFF;

// What else the radio was doing, captured with the event rather than inferred
// from neighbouring entries.
//
// A page timeout is the ABSENCE of PAGE_RX, so it has no event of its own to
// carry context. Stamping every recorded event with this makes the surrounding
// state recoverable anyway: the INQUIRY_START/STOP pair bracketing a missing
// PAGE_RX is what tests whether inquiry occupancy is starving page scan, which
// is currently a hypothesis with no adapter-side evidence either way.
//
// Reads only. Called from the BTstack thread alongside the existing recorder.
static uint8_t btlife_radio_flags(void)
{
    uint8_t f = 0u;
    if (classic_state.inquiry_active)              f |= BTLIFE_F_INQUIRY;
    if (hid_state.scan_active)                     f |= BTLIFE_F_LE_SCAN;
    if (hid_state.state == BLE_STATE_CONNECTING)   f |= BTLIFE_F_CONNECTING;
    if (config_ble.handle != HCI_CON_HANDLE_INVALID) {
        f |= BTLIFE_F_MGMT;
        if (config_ble_link_trusted(config_ble.handle)) f |= BTLIFE_F_MGMT_TRUST;
    }
    if (btstack_classic_get_connection_count() > 0) f |= BTLIFE_F_CLASSIC;
    if (btstack_host_wake_advertisement_active())    f |= BTLIFE_F_WAKE_ADV;
    if (hid_pairing_window_open)                    f |= BTLIFE_F_PAIRING;
    return f;
}

static void btlife_record_addr(uint8_t code, uint8_t a, uint16_t b,
                               const bd_addr_t addr)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    // Counters are ALWAYS exact (they are the authoritative totals).
    switch (code) {
        case BTLIFE_SCAN_START:      btlife.scan_start++; break;
        case BTLIFE_SCAN_STOP:       btlife.scan_stop++; break;
        case BTLIFE_SCAN_SUPPRESS:   if (a < BTLIFE_CAUSE_COUNT) btlife.suppress[a]++; break;
        case BTLIFE_ADV_START:       btlife.adv_start++; break;
        case BTLIFE_ADV_STOP:        btlife.adv_stop++; break;
        case BTLIFE_MGMT_CONNECT:    btlife.mgmt_connect++; break;
        case BTLIFE_MGMT_DISCONNECT: btlife.mgmt_disconnect++; break;
        case BTLIFE_CTRL_DISCONNECT: btlife.ctrl_disconnect++; break;
        case BTLIFE_HCI_DISCONNECT:
            btlife.hci_disconnect++;
            btlife.last_disc_handle = b; btlife.last_disc_reason = a;
            break;
        default: break;
    }

    // A blocked scan can fire every ~32 ms tick; recording each one floods the
    // ring and evicts the meaningful connect/disconnect events (observed: 641
    // dropped, the disconnect ordering lost). Coalesce suppress entries in the
    // RING to a cause-change or this interval -- the counter above is
    // unaffected, so totals stay exact either way.
    //
    // Widened from 1 s on 2026-08-23. The 100-cycle soak logged 923 suppress
    // events, about 9 per Controller Link cycle, which is as many ring slots as
    // every paging event of that cycle combined and would have halved the
    // retention the enlarged ring was added to buy. A suppressed scan restart
    // is background noise for the paging question; the paging events are not.
    if (code == BTLIFE_SCAN_SUPPRESS) {
        if (a == btlife_last_suppress_cause &&
            (now - btlife_last_suppress_ms) < BTLIFE_SUPPRESS_COALESCE_MS) {
            return;
        }
        btlife_last_suppress_cause = a;
        btlife_last_suppress_ms = now;
    }

    btlife_event_t *e = &btlife_ring[btlife_head];
    e->t_ms = now;
    e->code = code; e->a = a; e->b = b;
    e->flags = btlife_radio_flags();
    if (addr != NULL) {
        e->addr3[0] = addr[3]; e->addr3[1] = addr[4]; e->addr3[2] = addr[5];
    } else {
        e->addr3[0] = e->addr3[1] = e->addr3[2] = 0u;
    }
    btlife_head = (uint16_t)((btlife_head + 1u) % BTLIFE_RING_SIZE);
    if (btlife_count < BTLIFE_RING_SIZE) btlife_count++;
    else btlife_dropped++;  // ring full: we just overwrote the oldest unread event
}

// Existing call sites keep their three-argument shape; only the paging events
// need to name a peer.
static void btlife_record(uint8_t code, uint8_t a, uint16_t b)
{
    btlife_record_addr(code, a, b, NULL);
}

static void config_ble_can_send(void *context)
{
    (void)context;
    config_ble.tx_requested = false;

    if (!config_ble.mode_active || !config_ble_authorized() ||
        config_ble.closing || !config_ble.notifications_enabled ||
        config_ble.handle == HCI_CON_HANDLE_INVALID ||
        !config_ble_link_trusted(config_ble.handle)) {
        return;
    }

    uint16_t mtu = att_server_get_mtu(config_ble.handle);
    if (mtu <= 3u) {
        return;
    }
    size_t capacity = mtu - 3u;
    if (capacity > sizeof(config_ble.tx_chunk)) {
        capacity = sizeof(config_ble.tx_chunk);
    }
    size_t length = config_wireless_bridge_peek_response(
        config_ble.tx_chunk, capacity);
    if (length == 0) {
        return;
    }

    uint8_t status = att_server_notify(
        config_ble.handle, config_ble.tx_value_handle,
        config_ble.tx_chunk, (uint16_t)length);
    if (status == ERROR_CODE_SUCCESS) {
        config_wireless_bridge_consume_response(length);
    }
}

static uint16_t host_att_read_callback(hci_con_handle_t con_handle, uint16_t att_handle,
                                       uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    (void)con_handle;
    if (att_handle == config_ble.tx_ccc_handle) {
        uint16_t value = config_ble.notifications_enabled ? 1u : 0u;
        return att_read_callback_handle_little_endian_16(
            value, offset, buffer, buffer_size);
    }
    (void)att_handle; (void)offset; (void)buffer; (void)buffer_size;
    return 0; // static values are served from the DB directly
}

static int host_att_write_callback(hci_con_handle_t con_handle, uint16_t att_handle,
                                   uint16_t transaction_mode, uint16_t offset,
                                   uint8_t *buffer, uint16_t buffer_size) {
    if (att_handle == config_ble.rx_value_handle) {
        if (transaction_mode != ATT_TRANSACTION_MODE_NONE) {
            return ATT_ERROR_WRITE_REQUEST_REJECTED;
        }
        if (!config_ble.mode_active || !config_ble_authorized() ||
            config_ble.closing || con_handle != config_ble.handle) {
            return ATT_ERROR_WRITE_NOT_PERMITTED;
        }
        if (!config_ble_link_trusted(con_handle)) {
            return ATT_ERROR_INSUFFICIENT_AUTHENTICATION;
        }
        if (offset != 0) {
            return ATT_ERROR_INVALID_OFFSET;
        }
        config_wireless_rx_result_t result =
            config_wireless_bridge_receive(buffer, buffer_size);
        if (result == CONFIG_WIRELESS_RX_BUSY) {
            return ATT_ERROR_INSUFFICIENT_RESOURCES;
        }
        if (result == CONFIG_WIRELESS_RX_TOO_LONG) {
            return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        }
        return ATT_ERROR_SUCCESS;
    }

    if (att_handle == config_ble.tx_ccc_handle) {
        if (transaction_mode != ATT_TRANSACTION_MODE_NONE) {
            return ATT_ERROR_WRITE_REQUEST_REJECTED;
        }
        if (!config_ble.mode_active || !config_ble_authorized() ||
            config_ble.closing || con_handle != config_ble.handle) {
            return ATT_ERROR_WRITE_NOT_PERMITTED;
        }
        if (!config_ble_link_trusted(con_handle)) {
            return ATT_ERROR_INSUFFICIENT_AUTHENTICATION;
        }
        if (offset != 0 || buffer_size != 2) {
            return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        }
        config_ble.notifications_enabled =
            (little_endian_read_16(buffer, 0) & 1u) != 0;
        return ATT_ERROR_SUCCESS;
    }

    (void)con_handle; (void)att_handle; (void)transaction_mode;
    (void)offset; (void)buffer; (void)buffer_size;
    return 0; // accept (e.g. CCCD writes) so the peer's setup completes
}

// Defined (strong) by ble_output.c when the BLE-peripheral path owns the ATT
// server with its full GATT profile (e.g. controller_btusb, usb2ble). In that
// case we must NOT init a second, minimal server -- it would clobber the rich
// profile. Central-only builds (bt2usb, mouthpad) don't link ble_output, so the
// weak default applies and we install the minimal server.
__attribute__((weak)) bool btstack_host_external_att_server(void) { return false; }

static void setup_att_server(void) {
    if (btstack_host_external_att_server()) {
        printf("[BTSTACK_HOST] ATT server owned externally (ble_output) -- skipping minimal server\n");
        return;
    }
    printf("[BTSTACK_HOST] Init ATT server (minimal GAP+GATT)...\n");
    att_db_util_init();
    // GAP service (0x1800)
    att_db_util_add_service_uuid16(0x1800);
    att_db_util_add_characteristic_uuid16(0x2A00, ATT_PROPERTY_READ,
        ATT_SECURITY_NONE, ATT_SECURITY_NONE,
        host_att_device_name, sizeof(host_att_device_name) - 1);
    att_db_util_add_characteristic_uuid16(0x2A01, ATT_PROPERTY_READ,
        ATT_SECURITY_NONE, ATT_SECURITY_NONE,
        (uint8_t *)host_att_appearance, sizeof(host_att_appearance));
    // GATT service (0x1801) with Service Changed (indicate)
    att_db_util_add_service_uuid16(0x1801);
    att_db_util_add_characteristic_uuid16(0x2A05, ATT_PROPERTY_INDICATE,
        ATT_SECURITY_NONE, ATT_SECURITY_ENCRYPTED, NULL, 0);

    // Project configuration service: browser -> Pico writes to RX; Pico ->
    // browser JSON-line replies are TX notifications. It is intentionally
    // present in the static ATT database. Advertising is armed by Config or
    // management mode; writes remain bonded/encrypted and allowlisted.
    att_db_util_add_service_uuid128(config_ble_service_uuid);
    config_ble.rx_value_handle = att_db_util_add_characteristic_uuid128(
        config_ble_rx_uuid,
        ATT_PROPERTY_WRITE | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE |
            ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE, ATT_SECURITY_ENCRYPTED, NULL, 0);
    config_ble.tx_value_handle = att_db_util_add_characteristic_uuid128(
        config_ble_tx_uuid,
        ATT_PROPERTY_NOTIFY | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE, ATT_SECURITY_ENCRYPTED, NULL, 0);
    config_ble.tx_ccc_handle = (uint16_t)(config_ble.tx_value_handle + 1u);
    config_ble.service_available = true;

    att_server_init(att_db_util_get_address(), host_att_read_callback, host_att_write_callback);
    printf("[BTSTACK_HOST] ATT server initialized (db size=%u)\n", att_db_util_get_size());
}

static void setup_hid_handlers(void)
{
    printf("[BTSTACK_HOST] Init L2CAP...\n");
    l2cap_init();

    printf("[BTSTACK_HOST] Init SM...\n");
    sm_init();

    // Configure SM - bonding + LE Secure Connections. Some HOGP devices (e.g.
    // Augmental MouthPad) accept a legacy-paired connection and even accept the
    // report CCC writes, but will NOT stream HID notifications unless the link
    // is secured with LE Secure Connections. Request SC (peers without SC fall
    // back to legacy automatically, so other controllers are unaffected).
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION);
    sm_set_encryption_key_size_range(7, 16);
    // REQUESTING Secure Connections and REQUIRING it are different policies,
    // and this host has always meant the first. BTstack 1.8.2 changed the
    // default: sm_init() now sets sm_sc_only_mode = true whenever
    // ENABLE_LE_SECURE_CONNECTIONS is defined (sm.c:5230), where 1.6.2 left it
    // false. SC-Only rejects any pairing whose peer does not offer Secure
    // Connections (sm.c:1292) and any link whose encryption key is shorter than
    // 16 bytes (sm.c:1276, 4956) -- which is exactly the legacy fallback the
    // comment above depends on, and would also invalidate existing bonds formed
    // under the old default.
    //
    // Say it out loud rather than inheriting either version's default. Raising
    // this to true is a deliberate product decision about which BLE peers are
    // still supported, not an SDK-migration side effect.
    sm_set_secure_connections_only_mode(false);

    printf("[BTSTACK_HOST] Init GATT client...\n");
    gatt_client_init();

    // Minimal ATT server so peers that act as GATT clients toward us don't hang
    // on the 30s ATT timeout (see setup_att_server comment).
    setup_att_server();

    printf("[BTSTACK_HOST] Init HIDS Host...\n");
    hids_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));

    printf("[BTSTACK_HOST] Init DIS client...\n");
    device_information_service_client_init();

    printf("[BTSTACK_HOST] Init Battery Service client...\n");
    battery_service_client_init();

    printf("[BTSTACK_HOST] Init LE Device DB...\n");
    le_device_db_init();

    // Initialize classic BT HID Host
    printf("[BTSTACK_HOST] Init Classic HID Host...\n");
    memset(&classic_state, 0, sizeof(classic_state));
    // Set security level BEFORE hid_host_init (it registers L2CAP services with this level)
    gap_set_security_level(LEVEL_0);  // DS3 doesn't support SSP
    hid_host_init(classic_hid_descriptor_storage, sizeof(classic_hid_descriptor_storage));
    hid_host_register_packet_handler(hid_host_packet_handler);

    // SDP server - needed for DS4/DS5 reconnection (they query Device ID)
    sdp_init();
    device_id_create_sdp_record(device_id_sdp_service_buffer, 0x10003,
                                DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH,
                                BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    sdp_register_service(device_id_sdp_service_buffer);
    printf("[BTSTACK_HOST] SDP server initialized\n");

    // Sniff mode improves idle-controller compatibility and power use, but its
    // periodic radio anchors cannot sustain a 548-byte DualSense audio report
    // every 20 ms. A regular beep/silence duty cycle on hardware showed the
    // link delivering one 20 ms payload roughly every 40 ms. Keep the
    // hardware-confirmed ordinary policy everywhere except live-audio builds.
#ifdef NS2_DS5_AUDIO
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
#else
    gap_set_default_link_policy_settings(
        LM_LINK_POLICY_ENABLE_SNIFF_MODE |
        LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
#endif

    // Register for HCI events
    printf("[BTSTACK_HOST] Register event handlers...\n");
    hci_event_callback_registration.callback = packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // Register for SM events
    sm_event_callback_registration.callback = sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    ns2_bt_health_init(&bt_health, btstack_run_loop_get_time_ms());
    bt_health_probe_handle = HCI_CON_HANDLE_INVALID;
    hid_state.initialized = true;
    printf("[BTSTACK_HOST] HID handlers initialized (BLE + Classic)\n");
}

// btstack_host_init is only used for USB dongle transport
// For CYW43/ESP32, use btstack_host_init_hid_handlers() after external BTstack init
#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)

// TLV context for flash-based link key storage (must be static/persistent)
static btstack_tlv_flash_bank_t btstack_tlv_flash_bank_context;

// Set up TLV (flash) storage for persistent link keys and BLE bonding
static void setup_tlv_storage(void) {
    printf("[BTSTACK_HOST] Setting up flash-based TLV storage...\n");

    // Check for corrupted flash banks and erase if needed
    // Flash bank 0 starts at end of flash - 8KB
    uint32_t bank0_offset = PICO_FLASH_SIZE_BYTES - (FLASH_SECTOR_SIZE * 2);
    const uint8_t* bank0_ptr = (const uint8_t*)(XIP_BASE + bank0_offset);

    // BTstack TLV expects clean flash (0xFF) or valid header
    // If we see our debug pattern (0xDEADBEEF) or other garbage, erase
    bool needs_erase = false;
    if (bank0_ptr[0] == 0xDE && bank0_ptr[1] == 0xAD &&
        bank0_ptr[2] == 0xBE && bank0_ptr[3] == 0xEF) {
        printf("[BTSTACK_HOST] Detected corrupted flash bank (debug pattern)\n");
        needs_erase = true;
    }

    if (needs_erase) {
        btstack_erase_flash_banks();
    }

    // Get the Pico SDK flash bank HAL instance
    const hal_flash_bank_t *hal_flash_bank_impl = pico_flash_bank_instance();
    printf("[BTSTACK_HOST] Flash bank instance: %p\n", hal_flash_bank_impl);

    // Initialize BTstack TLV with flash bank
    const btstack_tlv_t *btstack_tlv_impl = btstack_tlv_flash_bank_init_instance(
            &btstack_tlv_flash_bank_context,
            hal_flash_bank_impl,
            NULL);
    printf("[BTSTACK_HOST] TLV instance: %p\n", btstack_tlv_impl);

    if (!btstack_tlv_impl) {
        printf("[BTSTACK_HOST] ERROR: TLV init failed!\n");
        return;
    }

    // Set global TLV instance
    btstack_tlv_set_instance(btstack_tlv_impl, &btstack_tlv_flash_bank_context);

    // Set up Classic BT link key storage using TLV
    const btstack_link_key_db_t *btstack_link_key_db = btstack_link_key_db_tlv_get_instance(
            btstack_tlv_impl, &btstack_tlv_flash_bank_context);
    printf("[BTSTACK_HOST] Link key DB instance: %p\n", btstack_link_key_db);

    if (!btstack_link_key_db) {
        printf("[BTSTACK_HOST] ERROR: Link key DB init failed!\n");
        return;
    }

    hci_set_link_key_db(btstack_link_key_db);
    printf("[BTSTACK_HOST] Classic BT link key DB configured (flash)\n");

    // Configure BLE device DB for TLV storage
    le_device_db_tlv_configure(btstack_tlv_impl, &btstack_tlv_flash_bank_context);
    printf("[BTSTACK_HOST] BLE device DB configured (flash)\n");

    // Debug: check bank state
    printf("[BTSTACK_HOST] TLV context: current_bank=%d write_offset=0x%lX\n",
           btstack_tlv_flash_bank_context.current_bank,
           (unsigned long)btstack_tlv_flash_bank_context.write_offset);
}

void btstack_host_init(const void* transport)
{
    if (hid_state.initialized) {
        printf("[BTSTACK_HOST] Already initialized\n");
        return;
    }

    if (!transport) {
        printf("[BTSTACK_HOST] ERROR: No HCI transport provided\n");
        return;
    }

    printf("[BTSTACK_HOST] Initializing BTstack...\n");

    memset(&hid_state, 0, sizeof(hid_state));
    // Initialize BLE connection handles to invalid (handle 0 is valid in BLE)
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        hid_state.connections[i].handle = HCI_CON_HANDLE_INVALID;
    }
    hid_state.hci_transport = (const hci_transport_t*)transport;

    // HCI dump disabled - too verbose (logs every ACL packet)
    // printf("[BTSTACK_HOST] Init HCI dump (for logging)...\n");
    // hci_dump_init(hci_dump_embedded_stdout_get_instance());

    printf("[BTSTACK_HOST] Init memory pools...\n");
    btstack_memory_init();

    printf("[BTSTACK_HOST] Init run loop...\n");
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

    printf("[BTSTACK_HOST] Init HCI with provided transport...\n");
    hci_init(transport, NULL);

    // Set up flash-based TLV storage for persistent link keys and BLE bonds
    setup_tlv_storage();

    // Set up HID handlers
    setup_hid_handlers();
    printf("[BTSTACK_HOST] Initialized OK\n");
}
#endif

void btstack_host_init_hid_handlers(void)
{
    if (hid_state.initialized) {
        printf("[BTSTACK_HOST] HID handlers already initialized\n");
        return;
    }

    printf("[BTSTACK_HOST] Initializing HID handlers (BTstack already initialized externally)...\n");

    memset(&hid_state, 0, sizeof(hid_state));
    // Initialize BLE connection handles to invalid (handle 0 is valid in BLE)
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        hid_state.connections[i].handle = HCI_CON_HANDLE_INVALID;
    }
    // Note: hci_transport is not set here since BTstack was initialized externally

    // Set up HID handlers (BTstack core already initialized by btstack_cyw43_init or similar)
    setup_hid_handlers();
    printf("[BTSTACK_HOST] HID handlers initialized OK\n");
}

void btstack_host_power_on(void)
{
    printf("[BTSTACK_HOST] power_on called, initialized=%d\n", hid_state.initialized);

    if (!hid_state.initialized) {
        printf("[BTSTACK_HOST] ERROR: Not initialized\n");
        return;
    }

    printf("[BTSTACK_HOST] HCI state before power_on: %d\n", hci_get_state());
    printf("[BTSTACK_HOST] Calling hci_power_control(HCI_POWER_ON)...\n");
    int err = hci_power_control(HCI_POWER_ON);
    printf("[BTSTACK_HOST] hci_power_control returned %d, state now: %d\n", err, hci_get_state());
}

// ============================================================================
// LAST CONNECTED DEVICE PERSISTENCE (via BTstack TLV)
// ============================================================================

// TLV tags: 'JPLC' = Joypad Last Connected, 'JPLK' = post-wipe pairing lock.
#define TLV_TAG_LAST_CONNECTED (((uint32_t)'J' << 24) | ((uint32_t)'P' << 16) | ((uint32_t)'L' << 8) | 'C')
#define TLV_TAG_PAIRING_LOCKOUT (((uint32_t)'J' << 24) | ((uint32_t)'P' << 16) | ((uint32_t)'L' << 8) | 'K')

// This is global by design. Triple-tap means "forget everything," so a MAC
// denylist would miss powered-off devices, private-address rotation, and the
// Switch 2 custom ATT path that never creates a BTstack bond.
static bool switch2_force_fresh_custom_pairing;
static bool switch2_explicit_fresh_pairing_admitted;
static uint32_t fresh_admission_accepts;
static uint32_t fresh_admission_reject_window;
static uint32_t fresh_admission_reject_lockout;
static uint32_t wipe_completions;
static uint32_t hci_state_losses;

// Times gap_disconnect() was handed a handle the controller had already
// released. Under BTstack 1.6.2 this was invisible -- the stack answered with a
// synthetic disconnection-complete event and the ordinary teardown ran. Under
// 1.8.2 it is a real event with a real consequence, so it is counted and
// printed: a non-zero value says some record here outlived its ACL, which is
// worth knowing regardless of whether the convergence below papered over it.
static uint32_t disconnect_handle_already_gone;

// Request a disconnect for a handle held in a DURABLE LOCAL RECORD.
//
// Returns true when HCI_EVENT_DISCONNECTION_COMPLETE will follow and the
// ordinary teardown path owns the rest; false when no event is coming and the
// caller must release its own record now. See ns2_bt_disconnect_outcome() for
// the BTstack 1.6.2 -> 1.8.2 behaviour change this exists for.
//
// Event-driven call sites -- the ones that disconnect the very handle the event
// they are handling just delivered -- deliberately keep calling gap_disconnect()
// directly. That handle is live by construction, and giving those paths a
// second teardown mechanism is exactly the parallel-path problem
// classic_companion_release_on_mgmt_loss() warns about.
static bool btstack_host_request_disconnect(hci_con_handle_t handle,
                                            const char *what)
{
    uint8_t status = gap_disconnect(handle);
    if (ns2_bt_disconnect_outcome(status) == NS2_BT_DISCONNECT_EVENT_PENDING)
        return true;

    disconnect_handle_already_gone++;
    printf("[BTSTACK_HOST] %s: handle 0x%04X already released by the controller "
           "(gap_disconnect=0x%02X, count=%lu); converging locally\n",
           what ? what : "disconnect", handle, status,
           (unsigned long)disconnect_handle_already_gone);
    return false;
}

// Release a BLE controller slot for which no disconnection-complete event will
// arrive.
//
// HCI_EVENT_DISCONNECTION_COMPLETE stays the one ordinary teardown path, and it
// does considerably more than this: reconnect selection, stale-bond deletion,
// scan resumption. Those are decisions about a link that just died, and this is
// not that situation -- here the caller has already decided the peer should go
// away and the stack has just told us its ACL no longer exists. So this
// releases resources and nothing else. The host-global GATT/battery scratch
// state is deliberately left alone: it is shared with any other live BLE
// connection, and the ordinary path will clear it when its owner drops.
static void ble_connection_release_orphan(ble_connection_t *conn)
{
    if (conn == NULL || conn->handle == HCI_CON_HANDLE_INVALID) return;

    hci_con_handle_t handle = conn->handle;
    uint8_t conn_index = 0xFFu;
    uint32_t generation = 0u;
    uint16_t dcid = conn->hids_cid;

    if (conn->conn_index > 0) {
        conn_index = conn->conn_index;
        generation = bthid_get_connection_generation(conn_index);
        if (ble_report_pending && pending_ble_conn_index == conn_index &&
            (pending_ble_connection_generation == 0u ||
             pending_ble_connection_generation == generation)) {
            ble_report_pending = false;
        }
        bt_on_disconnect_with_generation(conn_index, generation);
    }

    memset(conn, 0, sizeof(*conn));
    conn->handle = HCI_CON_HANDLE_INVALID;

    if (dcid != 0) hids_host_disconnect(dcid);

    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (mp_hid_setup[i].handle == handle) mp_hid_setup[i].active = false;
    }
    mp_nus_disconnected(handle);
    switch2_cleanup_on_disconnect(conn_index, generation);
}

// Authoritative Switch 2 bond-key snapshot read from controller SPI during
// the custom ATT init sequence. Declared with reconnect persistence because
// successful init consumes it when updating the durable target record.
static uint8_t sw2_pairing_ltk_raw[16];
static uint8_t sw2_pairing_ltk_normalized[16];
static uint32_t sw2_pairing_ltk_reads;
static hci_con_handle_t sw2_pairing_ltk_handle = HCI_CON_HANDLE_INVALID;
static uint8_t sw2_pairing_ltk_phase;
static bool sw2_pairing_ltk_valid;
static bool sw2_pairing_ltk_matches_derived;
static bool sw2_pairing_ltk_raw_matches_derived;
static bool switch2_direct_reencrypt_active;
static hci_con_handle_t switch2_direct_reencrypt_handle = HCI_CON_HANDLE_INVALID;
static uint32_t switch2_direct_reencrypt_started_ms;
static uint32_t switch2_direct_cmd_status_events;
static uint32_t switch2_direct_cmd_complete_events;
static uint32_t switch2_direct_encrypt_events;
static uint32_t switch2_disconnect_events;
static uint16_t switch2_last_cmd_status_opcode;
static uint16_t switch2_last_cmd_complete_opcode;
static uint8_t switch2_last_cmd_status;
static uint8_t switch2_last_cmd_complete_status;
static uint8_t switch2_last_encrypt_status;
static uint8_t switch2_last_encrypt_enabled;
static uint8_t switch2_last_disconnect_reason;
// BTstack's SM key-size query remains zero because Switch 2 HOME encryption is
// started directly with HCI LE Start Encryption. Track the successful HCI
// Encryption Change event per link instead of consulting SM-owned metadata.
static bool switch2_link_encrypted;
static hci_con_handle_t switch2_link_encrypted_handle = HCI_CON_HANDLE_INVALID;
enum {
    SW2_ENCRYPT_NONE = 0,
    SW2_ENCRYPT_RECONNECT = 1,
};
static uint8_t switch2_direct_encrypt_phase;

// A1 is the fixed host key component sent by this implementation in
// SW2_SUBCMD_PAIRING_STEP2. B1 is returned by genuine Switch 2 controllers and
// is the same public device component used by the wired personalities. Their
// derived LTK is the key the controller requires for HOME reconnect link-layer
// encryption. Keep these beside the persistence code so changing the pairing
// command cannot silently desynchronise the reconnect key.
static const uint8_t SW2_BLE_HOST_A1[16] = {
    0xEA, 0xBD, 0x47, 0x13, 0x89, 0x35, 0x42, 0xC6,
    0x79, 0xEE, 0x07, 0xF2, 0x53, 0x2C, 0x6C, 0x31};
static const uint8_t SW2_BLE_DEVICE_B1[16] = {
    0x5C, 0xF6, 0xEE, 0x79, 0x2C, 0xDF, 0x05, 0xE1,
    0xBA, 0x2B, 0x63, 0x25, 0xC4, 0x1A, 0x5F, 0x10};

static void btstack_host_store_pairing_lockout(bool locked)
{
    const btstack_tlv_t *tlv_impl = NULL;
    void *tlv_context = NULL;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (!tlv_impl) return;

    if (locked) {
        const uint8_t value = 1;
        tlv_impl->store_tag(tlv_context, TLV_TAG_PAIRING_LOCKOUT, &value, sizeof(value));
    } else {
        tlv_impl->delete_tag(tlv_context, TLV_TAG_PAIRING_LOCKOUT);
    }
}

static void btstack_host_restore_pairing_lockout(void)
{
    const btstack_tlv_t *tlv_impl = NULL;
    void *tlv_context = NULL;
    uint8_t value = 0;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    pairing_lockout = tlv_impl &&
        tlv_impl->get_tag(tlv_context, TLV_TAG_PAIRING_LOCKOUT, &value, sizeof(value)) == sizeof(value) &&
        value == 1;
}

static void btstack_host_clear_last_connected(void)
{
    memset(hid_state.last_connected_addr, 0, sizeof(hid_state.last_connected_addr));
    memset(hid_state.last_connected_name, 0, sizeof(hid_state.last_connected_name));
    hid_state.last_connected_profile = NULL;
    hid_state.last_connected_vid = 0;
    hid_state.last_connected_pid = 0;
    memset(hid_state.last_connected_ltk, 0, sizeof(hid_state.last_connected_ltk));
    hid_state.has_last_connected_ltk = false;
    hid_state.has_last_connected = false;
    hid_state.reconnect_attempts = 0;
    hid_state.reconnect_attempt_time = 0;
    memset(sw2_pairing_ltk_raw, 0, sizeof(sw2_pairing_ltk_raw));
    memset(sw2_pairing_ltk_normalized, 0,
           sizeof(sw2_pairing_ltk_normalized));
    sw2_pairing_ltk_valid = false;
    sw2_pairing_ltk_handle = HCI_CON_HANDLE_INVALID;
    sw2_pairing_ltk_phase = SW2_ENCRYPT_NONE;
    switch2_force_fresh_custom_pairing = false;
    switch2_explicit_fresh_pairing_admitted = false;
    switch2_direct_reencrypt_active = false;
    switch2_direct_reencrypt_handle = HCI_CON_HANDLE_INVALID;
    switch2_direct_encrypt_phase = SW2_ENCRYPT_NONE;
    switch2_link_encrypted = false;
    switch2_link_encrypted_handle = HCI_CON_HANDLE_INVALID;

    const btstack_tlv_t *tlv_impl = NULL;
    void *tlv_context = NULL;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (tlv_impl) tlv_impl->delete_tag(tlv_context, TLV_TAG_LAST_CONNECTED);
}

typedef struct {
    bd_addr_t addr;
    uint8_t addr_type;
    char name[48];
} __attribute__((packed)) last_connected_record_v1_t;

// V2 appends identity required to choose the correct BLE transport after a
// dongle reboot. The restore path still accepts the original address/name-only
// record so existing non-Switch-2 bonds are not discarded on upgrade.
typedef struct {
    last_connected_record_v1_t v1;
    uint16_t vid;
    uint16_t pid;
} __attribute__((packed)) last_connected_record_v2_t;

typedef struct {
    last_connected_record_v2_t v2;
    uint8_t ltk[16];
    uint8_t ltk_valid;
} __attribute__((packed)) last_connected_record_v3_t;

static bool btstack_host_is_switch2_identity(uint16_t vid, uint16_t pid)
{
    return vid == 0x057E &&
        (pid == 0x2066 || pid == 0x2067 || pid == 0x2069 || pid == 0x2073);
}

static const bt_device_profile_t *btstack_host_reconnect_profile(
    const char *name, uint16_t vid, uint16_t pid)
{
    // Switch 2 display names are synthesized ("Switch 2 Pro", etc.) and do
    // not occur in bt_device_db's advertising-name table. VID/PID is therefore
    // the durable discriminator for its custom ATT transport.
    if (btstack_host_is_switch2_identity(vid, pid)) {
        return &BT_PROFILE_SWITCH2;
    }
    return bt_device_lookup_by_name(name);
}

static void btstack_host_save_last_connected(void)
{
    const btstack_tlv_t *tlv_impl = NULL;
    void *tlv_context = NULL;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (!tlv_impl) return;

    last_connected_record_v3_t record;
    memset(&record, 0, sizeof(record));
    memcpy(record.v2.v1.addr, hid_state.last_connected_addr, 6);
    record.v2.v1.addr_type = (uint8_t)hid_state.last_connected_addr_type;
    strncpy(record.v2.v1.name, hid_state.last_connected_name, sizeof(record.v2.v1.name) - 1);
    record.v2.v1.name[sizeof(record.v2.v1.name) - 1] = '\0';
    record.v2.vid = hid_state.last_connected_vid;
    record.v2.pid = hid_state.last_connected_pid;
    memcpy(record.ltk, hid_state.last_connected_ltk, sizeof(record.ltk));
    record.ltk_valid = hid_state.has_last_connected_ltk ? 1u : 0u;

    tlv_impl->store_tag(tlv_context, TLV_TAG_LAST_CONNECTED,
                        (const uint8_t *)&record, sizeof(record));
}

static bool btstack_host_le_bond_entry_at(void *context, int slot,
                                           int *address_type,
                                           uint8_t address[6])
{
    (void)context;
    if (slot < 0 || slot >= le_device_db_max_count()) return false;

    int stored_type = BD_ADDR_TYPE_UNKNOWN;
    bd_addr_t stored_addr;
    le_device_db_info(slot, &stored_type, stored_addr, NULL);
    if (stored_type == BD_ADDR_TYPE_UNKNOWN) return false;
    if (address_type) *address_type = stored_type;
    if (address) memcpy(address, stored_addr, sizeof(stored_addr));
    return true;
}

static int btstack_host_find_le_device(const bd_addr_t addr,
                                        bd_addr_type_t addr_type)
{
    return ns2_bt_find_bond_slot(
        btstack_host_le_bond_entry_at, NULL, le_device_db_max_count(), addr,
        (int)addr_type, true);
}

static bool btstack_host_install_switch2_ltk(void)
{
    if (!hid_state.has_last_connected || !hid_state.has_last_connected_ltk ||
        !btstack_host_is_switch2_identity(hid_state.last_connected_vid,
                                          hid_state.last_connected_pid)) {
        return false;
    }

    int index = btstack_host_find_le_device(hid_state.last_connected_addr,
                                             hid_state.last_connected_addr_type);
    if (index < 0) {
        sm_key_t zero_irk = {0};
        index = le_device_db_add(hid_state.last_connected_addr_type,
                                 hid_state.last_connected_addr, zero_irk);
    }
    if (index < 0) {
        printf("[SW2_BLE] Failed to allocate LE bond record for HOME reconnect\n");
        return false;
    }

    uint8_t zero_rand[8] = {0};
    le_device_db_encryption_set(index, 0, zero_rand,
                                hid_state.last_connected_ltk, 16,
                                0, 0, 1);
    printf("[SW2_BLE] Installed custom-pairing LTK in LE bond slot %d\n", index);
    return true;
}

// Low-level encryption helper retained as a diagnostic fallback. Production
// HOME reconnect uses BTstack's Security Manager so the stack restores both
// link encryption and its per-connection bonded/security state.
static bool btstack_host_start_switch2_encryption(hci_con_handle_t handle,
                                                   const uint8_t normalized_ltk[16],
                                                   uint8_t phase)
{
    if (!normalized_ltk || phase == SW2_ENCRYPT_NONE) return false;

    uint8_t raw_ltk[16];
    for (size_t i = 0; i < sizeof(raw_ltk); ++i) {
        raw_ltk[i] = normalized_ltk[15u - i];
    }

    uint8_t status = hci_send_cmd(&hci_le_start_encryption,
                                  handle, 0u, 0u, 0u, raw_ltk);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[SW2_BLE] Direct HCI re-encryption command rejected locally: 0x%02X\n",
               status);
        hid_state.last_reencryption_status = status;
        return false;
    }

    switch2_direct_reencrypt_active = true;
    switch2_direct_reencrypt_handle = handle;
    switch2_direct_encrypt_phase = phase;
    switch2_direct_reencrypt_started_ms = btstack_run_loop_get_time_ms();
    hid_state.reencryption_started++;
    hid_state.last_reencryption_status = ERROR_CODE_SUCCESS;
    printf("[SW2_BLE] Direct HCI LTK encryption started (phase=%u)\n", phase);
    return true;
}

static bool btstack_host_start_switch2_reencryption(hci_con_handle_t handle)
{
    if (!hid_state.has_last_connected_ltk) return false;
    return btstack_host_start_switch2_encryption(
        handle, hid_state.last_connected_ltk, SW2_ENCRYPT_RECONNECT);
}

static btstack_context_callback_registration_t switch2_force_fresh_cb;

static void btstack_host_force_switch2_fresh_pairing_run(void *context)
{
    (void)context;
    switch2_force_fresh_custom_pairing = true;
    switch2_explicit_fresh_pairing_admitted = true;

    // UART access is itself an explicit local diagnostic action, equivalent
    // to opening the bounded pairing window with BOOTSEL. A freshly wiped
    // device deliberately restores pairing_lockout across reboot, so merely
    // calling start_scan() below would otherwise be a silent no-op.
    btstack_host_clear_pairing_lockout();

    // `btfresh` deliberately abandons any in-flight HOME encryption attempt.
    // Do this before requesting the disconnect so a custom BLE connection that
    // has not reached bthid registration cannot leave the old handle/phase
    // blocking (or being mistaken for) the next clean pairing session.
    switch2_direct_reencrypt_active = false;
    switch2_direct_reencrypt_handle = HCI_CON_HANDLE_INVALID;
    switch2_direct_encrypt_phase = SW2_ENCRYPT_NONE;

    if (hid_state.has_last_connected &&
        btstack_host_is_switch2_identity(hid_state.last_connected_vid,
                                         hid_state.last_connected_pid)) {
        gap_delete_bonding(hid_state.last_connected_addr_type,
                           hid_state.last_connected_addr);
    }

    for (int i = 0; i < MAX_BLE_CONNECTIONS; ++i) {
        ble_connection_t *conn = &hid_state.connections[i];
        if (conn->handle == HCI_CON_HANDLE_INVALID || !conn->profile ||
            conn->profile->ble != BT_BLE_CUSTOM) continue;
        printf("[SW2_BLE] UART forcing fresh custom pairing; disconnecting handle 0x%04X\n",
               conn->handle);
        if (!btstack_host_request_disconnect(conn->handle, "btfresh custom link")) {
            // No disconnect event is coming, so nothing else will free this
            // slot -- and a stale slot would be mistaken for the live link the
            // next SYNC advertisement is supposed to replace.
            ble_connection_release_orphan(conn);
        }
        return;
    }

    if (hid_state.state == BLE_STATE_CONNECTING &&
        hid_state.reconnect_attempt_time != 0) {
        printf("[SW2_BLE] UART fresh pairing cancelling directed reconnect in flight\n");
        gap_connect_cancel();
        hid_state.state = BLE_STATE_IDLE;
        hid_state.reconnect_attempt_time = 0;
    }

    printf("[SW2_BLE] UART armed fresh custom pairing; scanning for SYNC advertisement\n");
    btstack_host_start_scan();
}

void btstack_host_force_switch2_fresh_pairing(void)
{
    switch2_force_fresh_cb.callback = btstack_host_force_switch2_fresh_pairing_run;
    switch2_force_fresh_cb.context = NULL;
    btstack_run_loop_execute_on_main_thread(&switch2_force_fresh_cb);
}

// True when this address already has a live BLE link. Used to keep the periodic
// bonded-reconnect from targeting a peer that is already connected, which would
// stop the scan for no gain -- see its call site in btstack_host_process().
static bool btstack_host_ble_addr_connected(const bd_addr_t addr)
{
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID &&
            memcmp(hid_state.connections[i].addr, addr, sizeof(bd_addr_t)) == 0) {
            return true;
        }
    }
    return false;
}

// Remember a BLE device only after its protocol-specific setup has actually
// succeeded. Switch 2 controllers use a custom ATT pairing sequence and never
// emit BTstack's SM_EVENT_PAIRING_COMPLETE, so relying on the SM callbacks alone
// silently left them without a durable reconnect target.
static void btstack_host_remember_ble_connection(ble_connection_t *conn)
{
    if (!conn) return;

    if (conn->fresh_pairing_admitted &&
        conn->profile == &BT_PROFILE_SWITCH2) {
        btstack_host_record_fresh_admission(true);
        conn->fresh_pairing_admitted = false;
    }

    const bool same_addr = hid_state.has_last_connected &&
        memcmp(hid_state.last_connected_addr, conn->addr, sizeof(bd_addr_t)) == 0;
    const char *name = conn->name;
    if (name[0] == '\0' && same_addr) {
        name = hid_state.last_connected_name;
    }

    bool changed = !same_addr ||
        hid_state.last_connected_addr_type != conn->addr_type ||
        hid_state.last_connected_vid != conn->vid ||
        hid_state.last_connected_pid != conn->pid ||
        strncmp(hid_state.last_connected_name, name,
                sizeof(hid_state.last_connected_name)) != 0;

    uint8_t switch2_ltk[16];
    bool switch2_ltk_valid = false;
    if (btstack_host_is_switch2_identity(conn->vid, conn->pid)) {
        // Prefer the controller's authoritative post-pairing SPI value. The
        // derived value remains a compatibility fallback for an older
        // controller/firmware that does not answer the read, never the source
        // of truth when the controller supplied its stored key.
        if (sw2_pairing_ltk_valid && sw2_pairing_ltk_handle == conn->handle) {
            memcpy(switch2_ltk, sw2_pairing_ltk_normalized, sizeof(switch2_ltk));
        } else {
            ns2_pairing_derive_ltk(SW2_BLE_HOST_A1, SW2_BLE_DEVICE_B1,
                                   switch2_ltk);
        }
        switch2_ltk_valid = true;
        if (!hid_state.has_last_connected_ltk ||
            memcmp(hid_state.last_connected_ltk, switch2_ltk,
                   sizeof(switch2_ltk)) != 0) {
            changed = true;
        }
    }

    memcpy(hid_state.last_connected_addr, conn->addr, sizeof(bd_addr_t));
    hid_state.last_connected_addr_type = conn->addr_type;
    hid_state.last_connected_profile = conn->profile;
    hid_state.last_connected_vid = conn->vid;
    hid_state.last_connected_pid = conn->pid;
    if (switch2_ltk_valid) {
        memcpy(hid_state.last_connected_ltk, switch2_ltk,
               sizeof(hid_state.last_connected_ltk));
        hid_state.has_last_connected_ltk = true;
    }
    strncpy(hid_state.last_connected_name, name,
            sizeof(hid_state.last_connected_name) - 1);
    hid_state.last_connected_name[sizeof(hid_state.last_connected_name) - 1] = '\0';
    hid_state.has_last_connected = true;
    hid_state.reconnect_attempts = 0;

    // Avoid rewriting flash on every successful reconnect when the target did
    // not change. A first successful custom-ATT pairing always writes it.
    if (changed) {
        btstack_host_save_last_connected();
    }
    if (conn->profile == &BT_PROFILE_SWITCH2 &&
        hid_state.has_last_connected_ltk) {
        btstack_host_install_switch2_ltk();
        switch2_force_fresh_custom_pairing = false;
    }

    printf("[BTSTACK_HOST] Reconnect target ready: %02X:%02X:%02X:%02X:%02X:%02X "
           "type=%u vid=0x%04X pid=0x%04X name='%s'%s\n",
           conn->addr[5], conn->addr[4], conn->addr[3], conn->addr[2],
           conn->addr[1], conn->addr[0], (unsigned)conn->addr_type,
           conn->vid, conn->pid, hid_state.last_connected_name,
           changed ? " (persisted)" : "");
}

static void btstack_host_restore_last_connected(void)
{
    if (hid_state.has_last_connected) return;  // Already have one (e.g., from same session)

    const btstack_tlv_t *tlv_impl = NULL;
    void *tlv_context = NULL;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (!tlv_impl) return;

    last_connected_record_v3_t record;
    memset(&record, 0, sizeof(record));
    int len = tlv_impl->get_tag(tlv_context, TLV_TAG_LAST_CONNECTED,
                                (uint8_t *)&record, sizeof(record));
    if (len != sizeof(last_connected_record_v1_t) &&
        len != sizeof(last_connected_record_v2_t) && len != sizeof(record)) return;

    // Validate — addr must not be all zeros
    bool valid = false;
    for (int i = 0; i < 6; i++) {
        if (record.v2.v1.addr[i] != 0) { valid = true; break; }
    }
    if (!valid) return;

    memcpy(hid_state.last_connected_addr, record.v2.v1.addr, 6);
    hid_state.last_connected_addr_type = (bd_addr_type_t)record.v2.v1.addr_type;
    strncpy(hid_state.last_connected_name, record.v2.v1.name, sizeof(hid_state.last_connected_name) - 1);
    hid_state.last_connected_name[sizeof(hid_state.last_connected_name) - 1] = '\0';
    if (len >= sizeof(last_connected_record_v2_t)) {
        hid_state.last_connected_vid = record.v2.vid;
        hid_state.last_connected_pid = record.v2.pid;
    }
    if (len == sizeof(record) && record.ltk_valid) {
        memcpy(hid_state.last_connected_ltk, record.ltk,
               sizeof(hid_state.last_connected_ltk));
        hid_state.has_last_connected_ltk = true;
    } else if (btstack_host_is_switch2_identity(hid_state.last_connected_vid,
                                                hid_state.last_connected_pid)) {
        // V2 migration: every Switch 2 record written by the preceding build
        // came from this exact fixed A1/B1 exchange, so its LTK is recoverable.
        ns2_pairing_derive_ltk(SW2_BLE_HOST_A1, SW2_BLE_DEVICE_B1,
                               hid_state.last_connected_ltk);
        hid_state.has_last_connected_ltk = true;
    }
    hid_state.last_connected_profile = btstack_host_reconnect_profile(
        hid_state.last_connected_name, hid_state.last_connected_vid,
        hid_state.last_connected_pid);
    hid_state.has_last_connected = true;
    hid_state.reconnect_attempts = 0;

    printf("[BTSTACK_HOST] Restored last connected: %02X:%02X:%02X:%02X:%02X:%02X name='%s'\n",
           record.v2.v1.addr[5], record.v2.v1.addr[4], record.v2.v1.addr[3],
           record.v2.v1.addr[2], record.v2.v1.addr[1], record.v2.v1.addr[0],
           hid_state.last_connected_name);
    if (hid_state.has_last_connected_ltk) {
        btstack_host_install_switch2_ltk();
        // Upgrade a V2 record in place so future boots do not depend on the
        // migration assumption above.
        if (len != sizeof(record)) btstack_host_save_last_connected();
    }
}

// ============================================================================
// SCANNING
// ============================================================================

static uint32_t scan_timeout_end = 0;  // 0 = no timeout (indefinite scan)
static bool scan_suppressed = false;   // App can suppress auto-restart (e.g. USB device connected)

// ---------------------------------------------------------------------------
// Multi-peer bonded-reconnect observability
// ---------------------------------------------------------------------------
// The existing reconnect counters are all scoped to the single `last_connected`
// target, which is exactly the blind spot when two peers are bonded: a second
// bonded peripheral that is power-cycled is invisible to every one of them.
// These three separate "that bond is gone" from "it is advertising and nothing
// targets it" from "its address rotates". Bounded counters only -- advertising
// reports arrive continuously, so no per-report logging.
static uint32_t bonded_adv_reports;
static uint32_t nontarget_adv_reports;
static uint32_t rpa_adv_reports;

// True when this advertised address matches a stored LE bond by raw address.
// Deliberately raw: if a peripheral rotates through resolvable private
// addresses this returns false, which is itself the diagnosis (see
// sightings_rpa) rather than something to paper over with name matching.
// Core 1 only: touches the LE device DB.
static bool btstack_host_addr_is_bonded(const bd_addr_t addr)
{
    for (int i = 0; i < le_device_db_max_count(); i++) {
        int stored_type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t stored_addr;
        memset(stored_addr, 0, sizeof(stored_addr));
        le_device_db_info(i, &stored_type, stored_addr, NULL);
        if (stored_type == BD_ADDR_TYPE_UNKNOWN) continue;
        if (memcmp(stored_addr, addr, sizeof(bd_addr_t)) == 0) return true;
    }
    return false;
}

// Build the bonded-candidate list from the LE device DB -- the authority on
// which identities are known -- and apply the reconnect policy. Core 1 only:
// touches the LE device DB and the live connection table.
//
// This replaces "always target hid_state.last_connected", which fired at a peer
// that was still connected whenever the surviving peer happened to be the most
// recent one. See ns2_ble_reconnect.h for the full rationale.
//
// WHAT THE LE DEVICE DB ACTUALLY CONTAINS
//
// It is NOT controller-only. sm_init() is global and configured with
// SM_AUTHREQ_BONDING, and BTstack keeps ONE le_device_db for the whole stack,
// shared by central-role controller links and peripheral-role management links
// alike. The management/companion path is explicitly a bonded link
// (mgmt_session_authorized() requires client_bonded), so a paired companion's
// identity IS stored in the same database enumerated here.
//
// Why an unrelated bond can never be dialled anyway:
//
//   * A direct connect requires decision.action == NS2_BLE_RECONNECT_DIRECT.
//   * The selector only ever returns DIRECT for a `preferred` candidate.
//   * `preferred` means the identity equals hid_state.last_connected_addr.
//   * last_connected_addr is written ONLY by btstack_host_remember_ble_connection(),
//     whose every caller passes a ble_connection_t from hid_state.connections[].
//   * That table is populated only by find_free_connection() in the CENTRAL-role
//     branch of HCI_SUBEVENT_LE_CONNECTION_COMPLETE. Peripheral-role ACLs are
//     routed to config_ble_accept_connection() and `break` before reaching it.
//
// So a management/companion bond cannot become a reconnect target: it is
// structurally excluded by role provenance, not by name matching. An
// unclassified bond can still make the selector answer SCAN rather than IDLE,
// which is harmless -- SCAN only means "leave discovery running" and never
// authorises a connect. Note that discovery lifetime is NOT driven from here:
// it is driven by whether the selected logical source is complete (see
// ns2_bt_host.c), so a stale bond cannot hold the scan open.
//
// A companion that is CONNECTED is excluded outright below, so a present peer
// is never mistaken for an absent one.
static ns2_ble_reconnect_decision_t btstack_host_pick_reconnect(void)
{
    ns2_ble_reconnect_candidate_t candidates[NS2_BLE_RECONNECT_MAX_CANDIDATES];
    uint8_t n = 0;

    for (int i = 0; i < le_device_db_max_count() && n < NS2_BLE_RECONNECT_MAX_CANDIDATES; i++) {
        int type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t a;
        memset(a, 0, sizeof(a));
        le_device_db_info(i, &type, a, NULL);
        if (type == BD_ADDR_TYPE_UNKNOWN) continue;

        // The connected management/companion client is present, not absent. It
        // is a peripheral-role link and so never appears in the central-role
        // connection table that `connected` is derived from, which would
        // otherwise make a live companion look like a missing controller.
        //
        // `a` is a bond-database identity, so this must compare against the
        // companion's IDENTITY. It used to compare config_ble.client_addr,
        // which under LE privacy is an RPA and therefore never equal to a
        // stored identity -- the exclusion silently never fired.
        if (config_ble_addr_is_companion(a))
            continue;

        memcpy(candidates[n].addr, a, sizeof(candidates[n].addr));
        candidates[n].addr_type = (uint8_t)type;
        candidates[n].connected = btstack_host_ble_addr_connected(a) ? 1u : 0u;
        candidates[n].preferred =
            (hid_state.has_last_connected &&
             memcmp(a, hid_state.last_connected_addr, sizeof(bd_addr_t)) == 0) ? 1u : 0u;
        n++;
    }

    // A user-opened pairing window owns discovery; a speculative direct connect
    // must not tear its scan down. See ns2_ble_reconnect.h.
    return ns2_ble_reconnect_select(candidates, n, hid_state.reconnect_attempts,
                                    hid_pairing_window_open);
}

// ---------------------------------------------------------------------------
// Bond inventory snapshot
// ---------------------------------------------------------------------------
// The LE device DB belongs to the BTstack thread on core 1, so core 0's UART
// diagnostics must never read it directly -- that is the same hazard config.c
// marshals around for `bonds list`. Core 1 republishes a plain snapshot here on
// its 30 ms process tick; core 0 reads it under a seqlock retry.
//
// This exists so a controlled reconnect test can record which bonds are present
// BEFORE a peer is power-cycled and compare afterwards, instead of inferring
// bond survival from whatever the adapter happens to be connected to.
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
} bond_snapshot_entry_t;

static volatile uint32_t bond_snapshot_seq;
static bond_snapshot_entry_t bond_snapshot[MAX_NR_LE_DEVICE_DB_ENTRIES];
static uint8_t bond_snapshot_count;

// Core 1 only.
static void bond_snapshot_refresh(void)
{
    uint32_t seq = bond_snapshot_seq + 1u;
    bond_snapshot_seq = seq;               // odd => write in progress
    __dmb();
    uint8_t n = 0;
    for (int i = 0; i < le_device_db_max_count() && n < MAX_NR_LE_DEVICE_DB_ENTRIES; i++) {
        int type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t a;
        memset(a, 0, sizeof(a));
        le_device_db_info(i, &type, a, NULL);
        if (type == BD_ADDR_TYPE_UNKNOWN) continue;
        memcpy(bond_snapshot[n].addr, a, sizeof(a));
        bond_snapshot[n].addr_type = (uint8_t)type;
        n++;
    }
    bond_snapshot_count = n;
    __dmb();
    bond_snapshot_seq = seq + 1u;          // even => complete
}

bool btstack_host_bond_snapshot_get(uint8_t index, btstack_host_bond_entry_t *out)
{
    if (!out) return false;
    for (int attempt = 0; attempt < 4; attempt++) {
        uint32_t before = bond_snapshot_seq;
        if (before & 1u) continue;         // write in progress
        __dmb();
        uint8_t count = bond_snapshot_count;
        if (index >= count) {
            __dmb();
            if (bond_snapshot_seq == before) return false;
            continue;
        }
        memcpy(out->addr, bond_snapshot[index].addr, sizeof(out->addr));
        out->addr_type = bond_snapshot[index].addr_type;
        __dmb();
        if (bond_snapshot_seq == before) return true;
    }
    return false;
}

// A random address is "resolvable private" when its two most significant bits
// are 0b01. Such an address changes periodically, so a raw comparison against a
// stored identity address can never match it.
static bool btstack_host_addr_is_rpa(const bd_addr_t addr, bd_addr_type_t type)
{
    return type == BD_ADDR_TYPE_LE_RANDOM && (addr[0] & 0xC0u) == 0x40u;
}

// ============================================================================
// TEMPORARY BLE WAKE ADVERTISING
// ============================================================================

// A known-good Switch 2 wake replay only needs about one second of legacy BLE
// advertising. Use 1.2 s for margin and a short disabled interval before the
// first burst / between payloads so HCI parameter and address changes cannot be
// coalesced with a still-enabled advertiser.
#define WAKE_ADV_BURST_MS 1200
#define WAKE_ADV_QUIESCE_MS 100

typedef enum {
    WAKE_ADV_PHASE_IDLE,
    WAKE_ADV_PHASE_PREPARE,
    WAKE_ADV_PHASE_BROADCAST,
    WAKE_ADV_PHASE_BETWEEN,
    WAKE_ADV_PHASE_RESTORE,
} wake_adv_phase_t;

static struct {
    bool active;
    bool resume_ble_scan;
    bool resume_classic_inquiry;
    bool scan_requested;
    wake_adv_phase_t phase;
    uint8_t index;
    uint8_t count;
    bd_addr_t advertiser_addr;
    uint8_t data[BTSTACK_HOST_WAKE_MAX_PAYLOADS][BTSTACK_HOST_WAKE_ADV_LEN];
    btstack_timer_source_t timer;
} wake_adv;

static void wake_adv_arm(uint32_t delay_ms);

static void wake_adv_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;
    bd_addr_t null_addr = {0};

    switch (wake_adv.phase) {
        case WAKE_ADV_PHASE_PREPARE:
            // The emulated controller address is replayed exactly. It is not
            // the CYW43's public address and must not alter Classic identity.
            hci_le_set_own_address_type(BD_ADDR_TYPE_LE_RANDOM);
            hci_le_random_address_set(wake_adv.advertiser_addr);
            gap_advertisements_set_params(0x0020, 0x0040, 0x03,
                                          BD_ADDR_TYPE_LE_PUBLIC, null_addr,
                                          0x07, 0x00);
            gap_advertisements_set_data(BTSTACK_HOST_WAKE_ADV_LEN,
                                        wake_adv.data[wake_adv.index]);
            gap_advertisements_enable(1);
            wake_adv.phase = WAKE_ADV_PHASE_BROADCAST;
            wake_adv_arm(WAKE_ADV_BURST_MS);
            break;

        case WAKE_ADV_PHASE_BROADCAST:
            gap_advertisements_enable(0);
            wake_adv.index++;
            if (wake_adv.index < wake_adv.count) {
                wake_adv.phase = WAKE_ADV_PHASE_BETWEEN;
            } else {
                wake_adv.phase = WAKE_ADV_PHASE_RESTORE;
            }
            wake_adv_arm(WAKE_ADV_QUIESCE_MS);
            break;

        case WAKE_ADV_PHASE_BETWEEN:
            gap_advertisements_set_data(BTSTACK_HOST_WAKE_ADV_LEN,
                                        wake_adv.data[wake_adv.index]);
            gap_advertisements_enable(1);
            wake_adv.phase = WAKE_ADV_PHASE_BROADCAST;
            wake_adv_arm(WAKE_ADV_BURST_MS);
            break;

        case WAKE_ADV_PHASE_RESTORE: {
            // All ordinary BLE-central operations in this host use the public
            // address. Restore it before allowing discovery/reconnect again.
            hci_le_set_own_address_type(BD_ADDR_TYPE_LE_PUBLIC);
            bool resume_scan = wake_adv.resume_ble_scan ||
                               wake_adv.resume_classic_inquiry ||
                               wake_adv.scan_requested ||
                               scan_timeout_end != 0;
            wake_adv.phase = WAKE_ADV_PHASE_IDLE;
            wake_adv.active = false;
            printf("[BTSTACK_HOST] Wake advertisement complete\n");
            if (resume_scan) btstack_host_start_scan();
            break;
        }

        case WAKE_ADV_PHASE_IDLE:
        default:
            break;
    }
}

static void wake_adv_arm(uint32_t delay_ms) {
    btstack_run_loop_set_timer_handler(&wake_adv.timer, wake_adv_timer_handler);
    btstack_run_loop_set_timer(&wake_adv.timer, delay_ms);
    btstack_run_loop_add_timer(&wake_adv.timer);
}

bool btstack_host_start_wake_advertisement(
    const uint8_t advertiser_addr[6],
    const uint8_t advertisements[][BTSTACK_HOST_WAKE_ADV_LEN],
    uint8_t advertisement_count) {
    if (!advertiser_addr || !advertisements || !hid_state.initialized ||
        !hid_state.powered_on || wake_adv.active || advertisement_count == 0 ||
        advertisement_count > BTSTACK_HOST_WAKE_MAX_PAYLOADS) {
        return false;
    }
    if (g_usb_config_mode) {
        return false; // Exclusive CDC Config owns advertising; no wake there.
    }
    // Under in-band management the config/management service also uses the single
    // advertiser, but wake OUTRANKS management: proceed here and let
    // config_ble_service_task drop the management advert once wake_adv is active
    // (it already gates advertising on !wake_adv.active). wake_adv_arm quiesces
    // before it actually advertises, so the handoff has a settling window.

    // Do not perturb a controller admission attempt. Existing HID links remain
    // active: this operation pauses discovery only, never HCI power or ACL.
    if (hid_state.state == BLE_STATE_CONNECTING || classic_state.pending_valid ||
        classic_state.pending_hid_connect) {
        return false;
    }

    // A completed sequence has no registered timer, but remove defensively
    // before reusing the embedded timer source.
    btstack_run_loop_remove_timer(&wake_adv.timer);
    memset(&wake_adv, 0, sizeof(wake_adv));
    wake_adv.active = true;  // gates every scan restart before scans are paused
    wake_adv.phase = WAKE_ADV_PHASE_PREPARE;
    wake_adv.count = advertisement_count;
    memcpy(wake_adv.advertiser_addr, advertiser_addr, 6);
    memcpy(wake_adv.data, advertisements,
           advertisement_count * BTSTACK_HOST_WAKE_ADV_LEN);

    wake_adv.resume_ble_scan = hid_state.scan_active;
    wake_adv.resume_classic_inquiry = classic_state.inquiry_active;
    if (hid_state.scan_active) {
        gap_stop_scan();
        hid_state.scan_active = false;
    }
#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
    if (classic_state.inquiry_active) {
        gap_inquiry_stop();
        classic_state.inquiry_active = false;
    }
#endif

    gap_advertisements_enable(0);
    printf("[BTSTACK_HOST] Starting wake advertisement as "
           "%02X:%02X:%02X:%02X:%02X:%02X (%u payload%s)\n",
           advertiser_addr[0], advertiser_addr[1], advertiser_addr[2],
           advertiser_addr[3], advertiser_addr[4], advertiser_addr[5],
           advertisement_count, advertisement_count == 1 ? "" : "s");
    wake_adv_arm(WAKE_ADV_QUIESCE_MS);
    return true;
}

bool btstack_host_wake_advertisement_active(void) {
    return wake_adv.active;
}

static void config_ble_stop_advertising(void)
{
    if (!config_ble.advertising) {
        return;
    }
    gap_advertisements_enable(0);
    config_ble.advertising = false;
    btlife_record(BTLIFE_ADV_STOP, 0, 0);
}

static void config_ble_start_advertising(void)
{
    if (!config_ble.service_available || !hid_state.powered_on ||
        !config_ble.mode_active ||
        !config_ble_authorized() || wake_adv.active || config_ble.advertising ||
        config_ble.handle != HCI_CON_HANDLE_INVALID) {
        return;
    }

    bd_addr_t null_addr = {0};
    hci_le_set_own_address_type(BD_ADDR_TYPE_LE_PUBLIC);
    gap_advertisements_set_params(
        0x00A0, 0x00F0, 0x00, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(
        sizeof(config_ble_adv_data), config_ble_adv_data);
    gap_scan_response_set_data(
        sizeof(config_ble_scan_response), config_ble_scan_response);
    gap_advertisements_enable(1);
    config_ble.advertising = true;
    btlife_record(BTLIFE_ADV_START, g_usb_config_mode ? 0u : 1u, 0);
    printf("[BTSTACK_HOST] Config/management BLE advertising enabled (%s)\n",
           g_usb_config_mode ? "CDC Config" : "in-band mgmt");
}

static bool config_ble_accept_connection(hci_con_handle_t handle,
                                         const bd_addr_t peer_addr)
{
    if (!config_ble.service_available || !config_ble.mode_active ||
        !config_ble_authorized() || config_ble.closing ||
        config_ble.handle != HCI_CON_HANDLE_INVALID) {
        return false;
    }

    config_ble.handle = handle;
    // Latch before anything else can move the window. The admission test reads
    // config_ble.handle, so it must follow the assignment above.
    config_ble.fresh_bond_admitted = config_ble_bond_admission_open();
    if (peer_addr) {
        memcpy(config_ble.client_addr, peer_addr, sizeof(config_ble.client_addr));
        config_ble.client_addr_valid = true;
    }
    config_ble.advertising = false; // controller stops connectable advertising on connect
    config_ble.notifications_enabled = false;
    config_ble.tx_requested = false;

    // Ask the central for supervision margin. We are the peripheral here, so
    // this is a request the phone may refuse -- best-effort, and nothing below
    // depends on it succeeding. See ns2_bt_mgmt_link_params() for the captured
    // peer-sleep failure this exists to ride through.
    ns2_bt_le_link_params_t link = ns2_bt_mgmt_link_params();
    if (ns2_bt_le_link_params_valid(link)) {
        int param_status = gap_request_connection_parameter_update(
            handle, link.interval_min_units, link.interval_max_units,
            link.latency, link.supervision_timeout_units);
        if (param_status != ERROR_CODE_SUCCESS) {
            printf("[BTSTACK_HOST] Config BLE link param request failed: 0x%02X\n",
                   param_status);
        }
    }
    config_wireless_bridge_reset_session();
    btlife_record(BTLIFE_MGMT_CONNECT, g_usb_config_mode ? 0u : 1u, handle);
    printf("[BTSTACK_HOST] Config BLE client connected: handle=0x%04X fresh_bond=%d\n",
           handle, config_ble.fresh_bond_admitted ? 1 : 0);
    return true;
}

// ---------------------------------------------------------------------------
// CONTROLLER LINK LIFECYCLE: bound to the management relationship
//
// See ns2_bt_companion_classic_admission_allowed() for the invariant. These
// three are its runtime half.
//
// The handle is the companion's Classic ACL, latched the moment we recognise
// the peer as the live management client. It is what makes the teardown exact:
// on management loss we must disconnect THAT link and nothing else, and by then
// config_ble no longer holds the identity to look it up with. Cleared on the
// link's own Disconnection Complete, so a reused handle can never inherit it.
// ---------------------------------------------------------------------------
static hci_con_handle_t classic_companion_acl_handle = HCI_CON_HANDLE_INVALID;
static uint32_t classic_companion_refused_no_mgmt;  // admission refused
static uint32_t classic_companion_mgmt_teardowns;   // torn down on mgmt loss

static void classic_companion_note_link(hci_con_handle_t handle)
{
    if (handle == HCI_CON_HANDLE_INVALID) return;
    if (classic_companion_acl_handle == handle) return;
    classic_companion_acl_handle = handle;
    printf("[BTSTACK_HOST] Controller Link bound to management session (handle=0x%04X)\n",
           handle);
}

// Management is gone: the Controller Link it authorised must go with it.
//
// This is a plain gap_disconnect, deliberately: the existing Classic teardown
// path (HID_SUBEVENT_CONNECTION_CLOSED -> bt_on_disconnect_with_generation ->
// slot memset -> scan resume, plus HCI_EVENT_DISCONNECTION_COMPLETE for the
// per-handle security state) already releases HID, input ownership and security
// state correctly. Adding a second teardown mechanism here would create a
// parallel path that could disagree with it.
static void classic_companion_release_on_mgmt_loss(void)
{
    hci_con_handle_t link = classic_companion_acl_handle;
    if (link == HCI_CON_HANDLE_INVALID) return;
    classic_companion_acl_handle = HCI_CON_HANDLE_INVALID;
    classic_companion_mgmt_teardowns++;
    printf("[BTSTACK_HOST] Management lost; tearing down Controller Link "
           "(handle=0x%04X, teardowns=%lu)\n",
           link, (unsigned long)classic_companion_mgmt_teardowns);
    // Nothing to converge on the false path: classic_companion_acl_handle was
    // already cleared above, and if the ACL is genuinely gone then HID Host was
    // torn down with it. The counter inside the helper is what makes the case
    // visible if it ever happens.
    (void)btstack_host_request_disconnect(link, "Controller Link teardown");
}

static bool config_ble_handle_disconnect(
    hci_con_handle_t handle, uint8_t reason)
{
    if (handle != config_ble.handle) {
        return false;
    }

    btlife_record(BTLIFE_MGMT_DISCONNECT, reason, handle);
    printf("[BTSTACK_HOST] Config BLE client disconnected: handle=0x%04X reason=0x%02X\n",
           handle, reason);
    config_ble.handle = HCI_CON_HANDLE_INVALID;
    config_ble.fresh_bond_admitted = false;
    config_ble.client_addr_valid = false;
    // The resolved identity belongs to the ACL that resolved it. A reused
    // handle must never inherit the previous companion's identity.
    config_ble.client_identity_valid = false;
    config_ble.closing = false;
    config_ble.notifications_enabled = false;
    config_ble.tx_requested = false;
    config_wireless_bridge_reset_session();

    // Controller Link is a facility of this relationship, so it does not
    // outlive it. Ordered before the scan decision on purpose: the Classic link
    // is still up at this instant, so discovery arbitration here is unchanged,
    // and the HID close path resumes the scan once the link is actually down.
    classic_companion_release_on_mgmt_loss();

    if (!config_ble.mode_active && !g_usb_config_mode &&
        !btstack_host_controller_connected()) {
        btstack_host_start_scan();
    }
    return true;
}

static void config_ble_service_task(bool in_config)
{
    if (!config_ble.service_available) {
        return;
    }

    if (!in_config) {
        if (!config_ble.mode_active) {
            return; // zero radio work in every normal controller personality
        }

        config_ble.mode_active = false;
        config_ble_stop_advertising();
        config_ble.notifications_enabled = false;
        config_ble.tx_requested = false;
        config_wireless_bridge_reset_session();
        printf("[BTSTACK_HOST] Config/management BLE service disabled (not authorized)\n");

        if (config_ble.handle != HCI_CON_HANDLE_INVALID) {
            hci_con_handle_t closing = config_ble.handle;
            config_ble.closing = true;
            if (!btstack_host_request_disconnect(closing, "management session")) {
                // `closing` latches until the disconnect event clears it. With
                // no event coming, management would stay permanently "connected
                // but closing": no advertising, no new session, no scan resume.
                config_ble_handle_disconnect(
                    closing, ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
            }
        } else if (!btstack_host_controller_connected()) {
            btstack_host_start_scan();
        }
        return;
    }

    if (!config_ble.mode_active) {
        config_ble.mode_active = true;
        config_ble.closing = false;
        config_wireless_bridge_reset_session();
        printf("[BTSTACK_HOST] Config BLE service armed\n");
    }

    // Don't perturb an in-flight controller connect attempt (a gap_connect is
    // outstanding); its completion path resolves per-link and the next tick
    // resumes advertiser arbitration.
    if (hid_state.state == BLE_STATE_CONNECTING) {
        return;
    }

    // The service owns ONLY the LE advertiser (peripheral role). It never stops
    // controller discovery (central-role scan / Classic inquiry) -- those are a
    // different radio function and run independently, so config/management and a
    // live controller coexist. Config mode and in-band management are identical
    // here; the old "stop the scan to advertise" arbitration (which starved
    // controller discovery -- HW-confirmed: scan.starts stayed 0 while
    // suppress.mgmt_armed climbed) is gone. Wake replay outranks the advertiser.
    // See docs/experiments/in-band-mgmt-coexistence-failure-2026-08-12.md.
    if (wake_adv.active) {
        config_ble_stop_advertising();
    } else {
        config_ble_start_advertising();
    }

    if (config_ble.handle != HCI_CON_HANDLE_INVALID &&
        config_ble.notifications_enabled &&
        !config_ble.tx_requested &&
        config_wireless_bridge_response_pending()) {
        config_ble.tx_request.callback = &config_ble_can_send;
        config_ble.tx_request.context = NULL;
        config_ble.tx_requested = true;
        uint8_t status = att_server_request_to_send_notification(
            &config_ble.tx_request, config_ble.handle);
        if (status != ERROR_CODE_SUCCESS) {
            config_ble.tx_requested = false;
        }
    }
}

// Pending BLE gamepad: when we see a gamepad appearance or HID UUID but no name in the
// ADV packet, stash the address and wait for the scan response (which typically contains
// the name). This prevents connecting to Xbox controllers as "Generic BLE Gamepad".
static struct {
    bool valid;
    bd_addr_t addr;
    bd_addr_type_t addr_type;
    uint16_t appearance;
    bool has_hid_uuid;
    uint32_t timestamp;  // For expiry
} pending_ble_gamepad;

#define BLE_RECONNECT_INTERVAL_MS 20000  // While scanning, try reconnecting to bonded device every 20s

void btstack_host_start_scan(void)
{
#ifdef CONFIG_USB2BLE
    return;  // USB2BLE is BLE peripheral only — no scanning for input devices
#endif
#ifdef BTSTACK_DEFER_SCAN
    if (!btstack_host_scan_enabled) return;
#endif
    // The controller BT link (core1) is INDEPENDENT of the USB face (core0).
    // Config, in-band management, and personality re-enumeration are all core0
    // concerns and must never gate, drop, or block controller discovery -- the
    // Switch re-enumeration does not require killing the BT link. Controller
    // discovery is a central-role LE scan / Classic inquiry; the config/
    // management service only owns the peripheral-role LE advertiser, a different
    // radio function, so the two coexist. (Config mode originally suppressed
    // discovery because it was a standalone mode with no controller; that coupling
    // is obsolete now.) Only wake replay, which needs the advertiser, still
    // outranks a scan restart -- and controller connects/disconnects are never
    // touched by config/management state.
    if (wake_adv.active) {
        btlife_record(BTLIFE_SCAN_SUPPRESS, BTLIFE_CAUSE_WAKE, 0);
        wake_adv.scan_requested = true;
        return;  // wake replay owns the LE advertiser until its restore phase
    }
    if (pairing_lockout) {
        btlife_record(BTLIFE_SCAN_SUPPRESS, BTLIFE_CAUSE_LOCKOUT, 0);
        return;  // A triple-tap wipe requires a new explicit pairing window.
    }
    if (scan_suppressed) {
        btlife_record(BTLIFE_SCAN_SUPPRESS, BTLIFE_CAUSE_APP_SUPPRESS, 0);
        return;  // App suppressed scanning (e.g. BT host disabled)
    }

    if (!hid_state.powered_on) {
        btlife_record(BTLIFE_SCAN_SUPPRESS, BTLIFE_CAUSE_NOT_POWERED, 0);
        printf("[BTSTACK_HOST] Not powered on yet\n");
        return;
    }

    if (hid_state.scan_active || classic_state.inquiry_active) {
        btlife_record(BTLIFE_SCAN_SUPPRESS, BTLIFE_CAUSE_ALREADY, 0);
        return;  // Already scanning
    }

    printf("[BTSTACK_HOST] Starting BLE scan...\n");
    btlife_record(BTLIFE_SCAN_START, 0, 0);
    gap_set_scan_params(1, SCAN_INTERVAL, SCAN_WINDOW, 0);
    gap_start_scan();
    hid_state.scan_active = true;
    hid_state.state = BLE_STATE_SCANNING;
    if (hid_state.has_last_connected && hid_state.scan_start_time == 0) {
        // First scan with a bonded device: offset start time so periodic reconnect
        // fires after ~3s instead of the full BLE_RECONNECT_INTERVAL_MS (20s)
        hid_state.scan_start_time = btstack_run_loop_get_time_ms() - BLE_RECONNECT_INTERVAL_MS + 3000;
    } else {
        hid_state.scan_start_time = btstack_run_loop_get_time_ms();
    }

#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF) && !defined(CONFIG_USB2BLE)
    // Also start classic BT inquiry (not available on ESP32-S3/nRF BLE-only)
    // Skip in USB2BLE mode — Classic inquiry interferes with BLE advertising
    // Alternate between GIAC (general) and LIAC (limited) to discover Wiimotes/Wii U Pro
    // which use Limited Discoverable mode when SYNC button is pressed
    uint32_t lap = classic_state.use_liac ? GAP_IAC_LIMITED_INQUIRY : GAP_IAC_GENERAL_INQUIRY;
    printf("[BTSTACK_HOST] Starting Classic inquiry (LAP=%s)...\n",
           classic_state.use_liac ? "LIAC" : "GIAC");
    gap_inquiry_set_lap(lap);
    gap_inquiry_start(INQUIRY_DURATION);
    classic_state.inquiry_active = true;
    // Inquiry and page scan are distinct baseband substates. Bracketing every
    // round is what allows a missing page to be tested against inquiry
    // occupancy instead of assumed to be caused by it.
    btlife_record(BTLIFE_INQUIRY_START, classic_state.use_liac ? 1u : 0u, 0u);
#endif
}

void btstack_host_stop_scan(void)
{
    // Always set state to idle to prevent scanning from restarting
    hid_state.state = BLE_STATE_IDLE;
    hid_state.scan_start_time = 0;

    if (hid_state.scan_active) {
        printf("[BTSTACK_HOST] Stopping BLE scan\n");
        btlife_record(BTLIFE_SCAN_STOP, 0, 0);
        gap_stop_scan();
        hid_state.scan_active = false;
    }

#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
    classic_state.inquiry_restart_at_ms = 0;
    if (classic_state.inquiry_active) {
        printf("[BTSTACK_HOST] Stopping Classic inquiry\n");
        gap_inquiry_stop();
        classic_state.inquiry_active = false;
    }
#endif
}

// See btstack_host_close_pairing_window()'s doc comment for the bug this
// guards against: hid_state.state is read in exactly six places (the
// BLE_CONNECT_TIMEOUT_MS watchdog, the idle-safety-net, the scan/state resync,
// periodic bonded-reconnect, and the two new-device admission gates at the BLE
// advertising and Classic inquiry handlers) — none of them past the raw
// gap_connect() phase. So once a candidate reaches BLE_STATE_CONNECTED,
// stop_scan()'s hid_state.state reset is inert; GATT discovery, SM
// pairing/bonding, HID setup, and Switch 2's GATT init all key off
// per-connection state instead and are already unaffected by scan/inquiry
// being stopped. The only phase where stop_scan() is destructive is CONNECTING:
// it silently disarms the BLE_CONNECT_TIMEOUT_MS watchdog for that specific
// attempt (the watchdog requires state==CONNECTING to fire), leaving recovery
// of a genuinely stuck attempt dependent on whatever timeout the HCI/radio
// layer applies on its own -- unbounded from this firmware's point of view.
//
// Classic BT was traced too and found NOT vulnerable: its connection-establish
// watchdog (CLASSIC_CONNECT_TIMEOUT_MS, ~1030 below) is keyed on each
// classic_connection_t's own connect_time, set the moment hid_host_connect()
// succeeds -- completely independent of hid_state.state/scan_active/
// inquiry_active. Stopping inquiry mid-Classic-connect is harmless. No Classic
// defer logic is needed or added.
static bool pairing_close_deferred;

// The Classic ACL handle on which this host requested security as part of a
// FRESH PAIRING, if any.
//
// Read the qualifier carefully. On an ordinary reconnect with a stored link key
// this host also calls gap_request_security_level(handle, LEVEL_2) (see the
// incoming-connection path), which stores a requested level and sets
// BONDING_SEND_AUTHENTICATE_REQUEST -- so "did we request security at all" is
// TRUE for essentially every companion link and would make the stand-down dead
// code. What actually distinguishes the two cases is ownership: during a fresh
// pairing we are driving the whole security establishment and must finish it
// ourselves; on a reconnect the peer requires and drives its own encryption,
// and the level we asked for is satisfied by the peer's Encryption Change
// through the identical hci.c:4135 path. Only the fresh-pairing site sets this.
static hci_con_handle_t classic_fresh_pairing_security_handle = HCI_CON_HANDLE_INVALID;

// Type C observability. Classic security is otherwise entirely invisible on a
// flashed build: there is no HCI trace, printf does not reach the UART diag
// channel, and the btlife ring records no security events.
static uint32_t classic_authentication_deferrals;  // we did not start auth
// 0x23 on the AUTHENTICATION procedure, counted separately from the encryption
// one. Android absorbed six of these on 2026-08-22 and two landed on encryption
// instead, where it does not -- so a build that only counts encryption
// collisions reports success while the actual race is still happening one
// procedure earlier. An acceptance run has to be able to see both.
static uint32_t classic_authentication_collisions;
static uint32_t classic_encryption_deferrals;      // we stood down
static uint32_t classic_encryption_peer_completed; // peer-led encryption came up
static uint32_t classic_encryption_collisions;     // 0x23 observed on our side
static uint32_t classic_encryption_unencrypted_active; // HID ready, not encrypted
// Handle we most recently stood down on, so peer-led completion can be
// attributed to the deferral rather than to an ordinary encrypted reconnect.
static hci_con_handle_t classic_encryption_deferred_handle = HCI_CON_HANDLE_INVALID;

// ---------------------------------------------------------------------------
// PINNED-BTSTACK COMPATIBILITY: stand down from the automatic encryption request
//
// This is the only place that reaches into BTstack's bonding state machine, and
// it exists because of one specific pinned behaviour:
//
//   hci.c:4240  HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT, status 0, not yet
//               encrypted  ->  conn->bonding_flags |= BONDING_SEND_ENCRYPTION_REQUEST
//   hci.c:7472  hci_run()  ->  hci_send_cmd(&hci_set_connection_encryption, ...)
//
// That is unconditional: it does not consider who started authentication, nor
// whether this host requires encryption (our HID Host registers LEVEL_0 and
// requires none). Both controllers report Authentication Complete for the same
// LMP authentication, so both hosts start the LMP encryption procedure and the
// loser is rejected with HCI_ERR_LMP_ERR_TRANS_COLLISION. BTstack itself
// tolerates that -- SET_CONNECTION_ENCRYPTION is not handled in
// handle_command_status_event() at all, and the Encryption Change failure path
// only drops to security level 0 -- but Android disconnects the ACL on
// encryption failure, which is what kills the Controller Link.
//
// Clearing the flag from here works because hci.c:4708 forwards the event to
// the application ("notify upper stack") AFTER the switch that sets it and
// BEFORE hci_run() consumes it. That ordering is the load-bearing assumption.
//
// The asserts below are deliberately noisy on an SDK bump: if BTstack renumbers
// the flag or the pinned version moves, this must be re-verified against the
// new hci.c rather than silently becoming a no-op. Upstream master still had
// the same unconditional behaviour when this was written (hci.c:5046), so a
// newer BTstack is not automatically a fix.
// ---------------------------------------------------------------------------
_Static_assert(BONDING_SEND_ENCRYPTION_REQUEST == 0x2000,
               "Pinned BTstack renumbered BONDING_SEND_ENCRYPTION_REQUEST; "
               "re-verify the Type C encryption-race stand-down against hci.c");
_Static_assert(BTSTACK_VERSION_MAJOR == 1 && BTSTACK_VERSION_MINOR == 6 &&
               BTSTACK_VERSION_PATCH == 2,
               "BTstack moved off the pinned 1.6.2; re-verify that hci.c still "
               "sets BONDING_SEND_ENCRYPTION_REQUEST unconditionally on "
               "Authentication Complete and still notifies the app before "
               "hci_run() consumes it (see docs/experiments/"
               "controller-link-cycling-failure-2026-08-22.md)");

// Why the last Classic Authentication Complete did or did not stand down.
//
// enc.deferrals staying zero while authentication and encryption both succeed
// is an integration contradiction, not a detail: it means the hook did not run,
// or one predicate was false, and the counters alone cannot say which. This
// records the deciding inputs from the most recent Authentication Complete so a
// single flashed attempt answers it instead of another diagnostic round.
static struct {
    bool valid;
    uint16_t handle;
    bool mgmt_connected;
    bool mgmt_addr_known;
    bool mgmt_addr_matches;
    bool mgmt_link_trusted;
    bool we_own_fresh_pairing;
    bool request_was_pending;
    bool stood_down;
    // Authentication side, recorded at HCI_EVENT_CONNECTION_COMPLETE, before
    // the Authentication Complete this struct's other fields describe.
    bool auth_deferred;
    bool auth_had_stored_key;
    // Tri-state, NOT a bool. HCI_Authentication_Complete is generated in
    // response to this host's own HCI_Authentication_Requested. When the
    // stand-down declines to send that command, the event never arrives -- the
    // peer's authentication reaches us as Link Key Request plus Encryption
    // Change instead. A plain `false` there reads as "authentication failed"
    // when the truth is "not locally observed, and not expected to be".
    ns2_bt_auth_observation_t auth_outcome;
    bool encrypted_ok;           // Encryption Change, enabled, no failure
    // Key size as read at the HID-ready acceptance gate, which is the only
    // point where it is valid. Reading it in the Encryption Change handler
    // returns 0: for Classic, BTstack issues HCI_Read_Encryption_Key_Size
    // AFTER that event and fills the value in on its completion.
    uint8_t encryption_key_size;
    bool key_size_valid;         // false => never sampled at a valid point
    bool hid_ready;              // HID channels usable for this link
    bool link_closed;            // this ACL is gone; record is post-mortem
} last_auth_decision;

// Start a record for one ACL.
//
// Every field here describes a single Classic link, so the record must be
// opened by that link and closed with it. Without this, two things went wrong:
// the connection-time fields (auth_deferred, auth_had_stored_key) were written
// with no handle of their own and so attached to whatever handle a PREVIOUS
// Authentication Complete had recorded, and a link that stood down and then
// never authenticated left the earlier link's encrypted_ok/hid_ready standing
// as if they described it. Handles are reused, so neither is hypothetical.
static void auth_decision_begin(hci_con_handle_t handle)
{
    memset(&last_auth_decision, 0, sizeof(last_auth_decision));
    last_auth_decision.valid = true;
    last_auth_decision.handle = handle;
}

// True while the record still describes the live link that opened it. Read
// before any late field (encryption, HID readiness) is attributed to it.
static bool auth_decision_owns(hci_con_handle_t handle)
{
    return last_auth_decision.valid && !last_auth_decision.link_closed &&
           last_auth_decision.handle == handle;
}

// Returns true when the automatic request was actually pending and cleared.
static bool btstack_host_stand_down_from_encryption(hci_connection_t *conn)
{
    if (conn == NULL) return false;
    if ((conn->bonding_flags & BONDING_SEND_ENCRYPTION_REQUEST) == 0) return false;
    conn->bonding_flags &= ~BONDING_SEND_ENCRYPTION_REQUEST;
    return true;
}

bool btstack_host_pairing_locked(void)
{
    return pairing_lockout;
}

// Does this incoming Classic peer hold a live, cryptographically proven
// management session right now?
//
// This is the identity binding behind cross-transport trust, and it is
// deliberately stricter than "some LE bond exists" -- and stricter than a bare
// address match against the bond database, which any device can claim by
// setting its BD_ADDR. All three must hold:
//
//   1. a management session is connected right now;
//   2. its peer address equals this Classic peer's address;
//   3. that session is bonded AND encrypted with a full-length key
//      (config_ble_link_trusted -> mgmt_link_is_trusted).
//
// (3) is what makes (2) meaningful: an impostor can spoof the address, but it
// cannot bring up an encrypted session as that identity without the LTK. Losing
// the management session revokes this immediately -- it is live state, not a
// stored grant, so it cannot outlive the proof that created it.
//
// Core 1 only: touches the live connection table.
// The RESOLVED IDENTITY of the live management client, or false if there is
// none yet.
//
// config_ble.client_addr is the address the phone actually connected WITH, taken
// from LE Connection Complete. On Android that is routinely a Resolvable Private
// Address, and an RPA never equals the public BD_ADDR the same phone pages us
// from on Classic. Comparing the connection address alone therefore fails to
// recognise the companion on exactly the connections where the RPA has not been
// resolved into an identity yet -- which is what refused a legitimate Controller
// Link 18 times after a fresh pairing on 2026-08-22.
//
// The identity here is not a claim, it is the outcome of cryptography: the LE
// device DB index is assigned by SM on successful bonding or IRK resolution, and
// nothing else writes it. It is also always available when the session is
// trusted at all -- pinned BTstack implements gap_bonded() for LE as exactly
// `sm_le_db_index >= 0` (hci.c:9844), the same index read here, and
// config_ble_link_trusted() already requires gap_bonded().
//
// Core 1 only: touches the LE device DB and the live connection table.
static bool config_ble_identity_addr(bd_addr_t out)
{
    if (config_ble.handle == HCI_CON_HANDLE_INVALID) return false;
    int index = sm_le_device_index(config_ble.handle);
    if (index < 0 || index >= le_device_db_max_count()) return false;
    int stored_type = BD_ADDR_TYPE_UNKNOWN;
    bd_addr_t stored_addr;
    memset(stored_addr, 0, sizeof(stored_addr));
    le_device_db_info(index, &stored_type, stored_addr, NULL);
    if (stored_type == BD_ADDR_TYPE_UNKNOWN) return false;
    memcpy(out, stored_addr, sizeof(bd_addr_t));
    return true;
}

/*
 * The management client's address for any DURABLE comparison.
 *
 * Preference order, most authoritative first:
 *
 *  1. config_ble_identity_addr() -- the LE device DB record this link is
 *     actually authenticated against, via sm_le_device_index(). This is the
 *     answer BTstack itself is using, so it cannot disagree with the bond.
 *  2. The identity captured from SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED. Covers
 *     the window before a db index is available.
 *  3. config_ble.client_addr -- correct only because a peer that produced no
 *     resolution connected with its identity address in the first place.
 *
 * Nothing here re-derives an identity. BTstack resolves the RPA (see
 * ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION); this only reads the result.
 *
 * Returns false when nothing is known, so a caller cannot silently compare
 * against a zeroed buffer.
 */
static bool config_ble_durable_addr(bd_addr_t out)
{
    if (config_ble_identity_addr(out)) return true;
    if (config_ble.client_identity_valid) {
        memcpy(out, config_ble.client_identity_addr, sizeof(bd_addr_t));
        return true;
    }
    if (config_ble.client_addr_valid) {
        memcpy(out, config_ble.client_addr, sizeof(bd_addr_t));
        return true;
    }
    return false;
}

/*
 * Is `addr` the management companion?
 *
 * Compared on durable identity, never on the connection address. An RPA rotates
 * roughly every 15 minutes and is never equal to a stored identity or to a
 * Classic BD_ADDR, so a raw client_addr comparison against anything durable is
 * not merely unreliable -- it is permanently false.
 */
static bool config_ble_addr_is_companion(const uint8_t addr[6])
{
    bd_addr_t durable;
    if (addr == NULL || !config_ble_durable_addr(durable)) return false;
    return memcmp(durable, addr, sizeof(bd_addr_t)) == 0;
}

// The individual terms of the companion-session conjunction, evaluated once.
//
// Extracted because three separate places need them and they must not drift:
// the predicate itself, the refusal record, and the per-ACL btauth record. When
// `clink.refused_no_mgmt` reached 18 on 2026-08-22 the only thing recorded was
// the count, and "some term was false" is not a diagnosis.
typedef struct {
    bool connected;
    bool addr_known;
    bool raw_matches;
    bool identity_known;
    bool identity_matches;
    bool link_trusted;
} companion_terms_t;

static companion_terms_t btstack_host_companion_terms(const bd_addr_t addr)
{
    companion_terms_t t;
    memset(&t, 0, sizeof(t));
    t.connected = config_ble.handle != HCI_CON_HANDLE_INVALID;
    t.addr_known = t.connected && config_ble.client_addr_valid;
    // Two ways to be the same peer. The identity comparison is the load-bearing
    // one and the only one that survives LE privacy; the connection-address
    // comparison is kept because it is correct whenever the phone connects with
    // its public address, and it costs nothing.
    t.raw_matches =
        t.addr_known &&
        memcmp(config_ble.client_addr, addr, sizeof(bd_addr_t)) == 0;
    bd_addr_t identity;
    t.identity_known = config_ble_identity_addr(identity);
    t.identity_matches =
        t.identity_known && memcmp(identity, addr, sizeof(bd_addr_t)) == 0;
    // Evaluated only for the matching peer: gap_bonded()/gap_encryption_key_size()
    // are keyed on the handle, so asking about a non-match proves nothing.
    t.link_trusted = (t.raw_matches || t.identity_matches) &&
                     config_ble_link_trusted(config_ble.handle);
    return t;
}

static bool btstack_host_classic_companion_session_trust(const bd_addr_t addr)
{
    companion_terms_t t = btstack_host_companion_terms(addr);
    return ns2_bt_companion_session_trust(t.connected,
                                          t.addr_known || t.identity_known,
                                          t.raw_matches || t.identity_matches,
                                          t.link_trusted);
}

// Is this Classic peer an identity that also exists on LE?
//
// The Android companion is the only peer this firmware creates that way: it
// bonds over LE to hold the management session, and its Classic link key is
// cross-transport-derived from that same LE bond under the same identity
// address. So an incoming Classic connection from an address that holds an LE
// bond is the companion arriving for its Controller Link.
//
// Physical controllers cannot satisfy this. Classic controllers (DS3/DS4/DS5,
// Switch Pro, Wiimote/Wii U Pro) never bond over LE, and BLE controllers
// (Xbox, Switch 2 Pro, MouthPad) never arrive on Classic at all -- so for every
// one of them this is false and admission is bit-for-bit what it was before.
//
// Raw address comparison, matching btstack_host_addr_is_bonded(): a Classic
// BD_ADDR is public, and the companion's LE identity address is the same public
// address (that equality is what btstack_host_classic_companion_session_trust()
// already relies on, and it is what 18 consecutive Controller Links validated
// on 2026-08-22).
//
// Core 1 only: touches the LE device DB.
static bool btstack_host_classic_peer_is_cross_transport(const bd_addr_t addr)
{
    return btstack_host_addr_is_bonded(addr);
}

// May this Classic peer be admitted at all, given the Controller Link's
// dependency on a live management session? See
// ns2_bt_companion_classic_admission_allowed().
static bool btstack_host_classic_admission_allowed(const bd_addr_t addr)
{
    return ns2_bt_companion_classic_admission_allowed(
        btstack_host_classic_peer_is_cross_transport(addr),
        btstack_host_classic_companion_session_trust(addr));
}

// A stored Classic link key is not the only proof this identity was admitted.
// See ns2_bt_classic_trust_present(): the Android companion pairs over LE and
// then arrives as a Classic HID Device, and pinned BTstack does not persist the
// cross-transport-derived Classic key atomically with the LE bond, so the LE
// bond can exist with no Classic key and reject the Controller Link for good.
static bool btstack_host_classic_has_trust(const bd_addr_t addr)
{
    link_key_t link_key;
    link_key_type_t key_type;
    bool classic_key =
        gap_get_link_key_for_bd_addr((uint8_t *)addr, link_key, &key_type);
    return ns2_bt_classic_trust_present(
        classic_key, btstack_host_classic_companion_session_trust(addr));
}

// Attribution for the most recent admission rejection.
//
// The counters alone are anonymous, which made a real question unanswerable in
// the field: `reject_window` reached 11 during one session with no way to tell
// whether the rejected peer was the phone whose Controller Link was failing or
// some unrelated Classic device. Recording the address and what the trust lookup
// actually returned is what turns "something was rejected" into evidence.
//
// Read-only, RAM-only, one slot; costs nothing until a rejection happens.
static bd_addr_t last_reject_addr;
static bool last_reject_trust_present;
static bool last_reject_valid;

// Why the most recent Controller Link refusal happened.
//
// The counter alone cost a real diagnosis: `clink.refused_no_mgmt` reached 18
// against a peer the bond database and `btreject` both showed as the companion,
// while the phone's own log said management was connected and identity-verified
// seconds earlier. "Some predicate was false" is not a finding, and the
// conjunction has four terms. Record which one, at the moment it decides.
static struct {
    bool valid;
    bd_addr_t addr;
    bool cross_transport;      // peer holds an LE bond => companion identity
    bool mgmt_connected;
    bool mgmt_addr_known;      // a connection address was recorded
    bool mgmt_raw_matches;     // ...and it equals this Classic peer
    bool mgmt_identity_known;  // SM resolved the session to a stored identity
    bool mgmt_identity_matches;// ...and that identity equals this Classic peer
    bool mgmt_link_trusted;    // bonded AND encrypted at full key length
} last_refusal;

static void btstack_host_note_link_refusal(const bd_addr_t addr,
                                           bool cross_transport)
{
    // Same evaluation the predicate itself used, so the recorded terms cannot
    // drift from the decision they are supposed to explain.
    companion_terms_t t = btstack_host_companion_terms(addr);
    last_refusal.valid = true;
    (void)memcpy(last_refusal.addr, addr, sizeof(bd_addr_t));
    last_refusal.cross_transport = cross_transport;
    last_refusal.mgmt_connected = t.connected;
    last_refusal.mgmt_addr_known = t.addr_known;
    last_refusal.mgmt_raw_matches = t.raw_matches;
    last_refusal.mgmt_identity_known = t.identity_known;
    last_refusal.mgmt_identity_matches = t.identity_matches;
    last_refusal.mgmt_link_trusted = t.link_trusted;
}

bool btstack_host_last_link_refusal(btstack_host_link_refusal_t *out)
{
    if (!out || !last_refusal.valid) return false;
    (void)memcpy(out->addr, last_refusal.addr, sizeof(out->addr));
    out->cross_transport = last_refusal.cross_transport;
    out->mgmt_connected = last_refusal.mgmt_connected;
    out->mgmt_addr_known = last_refusal.mgmt_addr_known;
    out->mgmt_raw_matches = last_refusal.mgmt_raw_matches;
    out->mgmt_identity_known = last_refusal.mgmt_identity_known;
    out->mgmt_identity_matches = last_refusal.mgmt_identity_matches;
    out->mgmt_link_trusted = last_refusal.mgmt_link_trusted;
    return true;
}

static void btstack_host_record_fresh_admission(bool accepted)
{
    if (accepted) {
        fresh_admission_accepts++;
    } else if (pairing_lockout) {
        fresh_admission_reject_lockout++;
    } else {
        fresh_admission_reject_window++;
    }
}

static void btstack_host_note_admission_reject(const bd_addr_t addr, bool trust_present)
{
    (void)memcpy(last_reject_addr, addr, sizeof(bd_addr_t));
    last_reject_trust_present = trust_present;
    last_reject_valid = true;
}

bool btstack_host_last_auth_decision(btstack_host_auth_decision_t *out)
{
    if (!out || !last_auth_decision.valid) return false;
    out->handle = last_auth_decision.handle;
    out->mgmt_connected = last_auth_decision.mgmt_connected;
    out->mgmt_addr_known = last_auth_decision.mgmt_addr_known;
    out->mgmt_addr_matches = last_auth_decision.mgmt_addr_matches;
    out->mgmt_link_trusted = last_auth_decision.mgmt_link_trusted;
    out->we_own_fresh_pairing = last_auth_decision.we_own_fresh_pairing;
    out->request_was_pending = last_auth_decision.request_was_pending;
    out->stood_down = last_auth_decision.stood_down;
    out->auth_deferred = last_auth_decision.auth_deferred;
    out->auth_had_stored_key = last_auth_decision.auth_had_stored_key;
    out->auth_outcome = last_auth_decision.auth_outcome;
    out->encrypted_ok = last_auth_decision.encrypted_ok;
    out->encryption_key_size = last_auth_decision.encryption_key_size;
    out->key_size_valid = last_auth_decision.key_size_valid;
    out->hid_ready = last_auth_decision.hid_ready;
    out->link_closed = last_auth_decision.link_closed;
    return true;
}

uint32_t btstack_host_authentication_deferrals(void)
{
    return classic_authentication_deferrals;
}

bool btstack_host_last_reject(uint8_t *addr_out, bool *trust_present)
{
    if (!last_reject_valid) return false;
    if (addr_out) (void)memcpy(addr_out, last_reject_addr, sizeof(bd_addr_t));
    if (trust_present) *trust_present = last_reject_trust_present;
    return true;
}

static void classic_pending_security_clear(void)
{
    classic_state.pending_trust_present = false;
    classic_state.pending_fresh_pairing_admitted = false;
    classic_state.pending_existing_link_key_valid = false;
    memset(classic_state.pending_existing_link_key, 0,
           sizeof(classic_state.pending_existing_link_key));
    classic_state.pending_existing_link_key_type = INVALID_LINK_KEY;
    classic_state.pending_notified_link_key_valid = false;
    classic_state.pending_notified_link_key_admitted = false;
    memset(classic_state.pending_notified_link_key, 0,
           sizeof(classic_state.pending_notified_link_key));
    classic_state.pending_notified_link_key_type = INVALID_LINK_KEY;
    classic_state.pending_auth_succeeded = false;
}

static void classic_pending_security_prepare(const bd_addr_t addr,
                                             bool fresh_pairing_admitted)
{
    classic_pending_security_clear();
    classic_state.pending_existing_link_key_valid =
        gap_get_link_key_for_bd_addr((uint8_t *)addr,
            classic_state.pending_existing_link_key,
            &classic_state.pending_existing_link_key_type);
    classic_state.pending_trust_present =
        classic_state.pending_existing_link_key_valid;
    classic_state.pending_fresh_pairing_admitted =
        fresh_pairing_admitted && !pairing_lockout;
}

// BTstack processes Link Key Notification before forwarding it to application
// handlers, so its default path may already have replaced the durable entry.
// Undo that write until Authentication Complete proves the candidate key.
static void classic_restore_existing_key(void)
{
    gap_drop_link_key_for_bd_addr(classic_state.pending_addr);
    if (classic_state.pending_existing_link_key_valid) {
        gap_store_link_key_for_bd_addr(
            classic_state.pending_addr,
            classic_state.pending_existing_link_key,
            classic_state.pending_existing_link_key_type);
    }
}

static void classic_commit_notified_key_if_authenticated(void)
{
    if (!ns2_bt_classic_key_commit_allowed(
            pairing_lockout, classic_state.pending_auth_succeeded,
            classic_state.pending_notified_link_key_valid,
            classic_state.pending_notified_link_key_admitted)) {
        return;
    }

    gap_store_link_key_for_bd_addr(
        classic_state.pending_addr,
        classic_state.pending_notified_link_key,
        classic_state.pending_notified_link_key_type);
    if (classic_state.pending_fresh_pairing_admitted) {
        btstack_host_record_fresh_admission(true);
    }
    classic_state.pending_trust_present = true;
    classic_state.pending_fresh_pairing_admitted = false;
    classic_state.pending_existing_link_key_valid = true;
    memcpy(classic_state.pending_existing_link_key,
           classic_state.pending_notified_link_key,
           sizeof(classic_state.pending_existing_link_key));
    classic_state.pending_existing_link_key_type =
        classic_state.pending_notified_link_key_type;
    classic_state.pending_notified_link_key_valid = false;
    classic_state.pending_notified_link_key_admitted = false;
    memset(classic_state.pending_notified_link_key, 0,
           sizeof(classic_state.pending_notified_link_key));
    printf("[BTSTACK_HOST] Authenticated Classic link-key update committed\n");
}

/*
 * Record proof that the pairing behind the parked link key succeeded, and
 * commit the key if that was the last thing it was waiting for.
 *
 * Called from BOTH Authentication Complete (this host drove authentication) and
 * Encryption Change (the peer drove it). See
 * ns2_bt_classic_authentication_proven() for why neither event can be relied on
 * alone -- and why a build that waited only for the local one never persisted a
 * DualSense's key, so every session re-ran SSP and the controller could only
 * ever reconnect through the pairing window.
 *
 * Matching is by ADDRESS, not by classic_state.pending_valid: HID open clears
 * that bookkeeping flag while this security exchange may still be in flight.
 * The parked key already carries its own admission proof and was only parked
 * after its identity matched pending_addr, so the address test is what keeps
 * this bound to the right peer.
 */
static void classic_note_security_proof(hci_con_handle_t handle,
                                        bool local_auth_complete_ok,
                                        bool encryption_enabled_ok)
{
    if (!ns2_bt_classic_authentication_proven(local_auth_complete_ok,
                                              encryption_enabled_ok)) {
        return;
    }
    hci_connection_t *conn = hci_connection_for_handle(handle);
    if (!conn ||
        bd_addr_cmp(conn->address, classic_state.pending_addr) != 0) {
        return;
    }
    classic_state.pending_auth_succeeded = true;
    classic_commit_notified_key_if_authenticated();
}

// Start the next Classic inquiry round, alternating GIAC/LIAC so both normally
// discoverable and SYNC-button (limited) devices stay reachable.
static void classic_restart_inquiry(void)
{
    classic_state.inquiry_restart_at_ms = 0;
    classic_state.use_liac = !classic_state.use_liac;
    uint32_t lap = classic_state.use_liac ? GAP_IAC_LIMITED_INQUIRY
                                          : GAP_IAC_GENERAL_INQUIRY;
    printf("[BTSTACK_HOST] Restarting inquiry (LAP=%s)...\n",
           classic_state.use_liac ? "LIAC" : "GIAC");
    gap_inquiry_set_lap(lap);
    gap_inquiry_start(INQUIRY_DURATION);
    classic_state.inquiry_active = true;
    btlife_record(BTLIFE_INQUIRY_START, classic_state.use_liac ? 1u : 0u, 0u);
}

static int btstack_host_classic_connection_filter(bd_addr_t addr,
                                                   hci_link_type_t link_type)
{
    UNUSED(link_type);
    // Refuse before an ACL exists. This is the "cleanly" in "Controller Link
    // establishment must be refused cleanly when management is disconnected":
    // no link, no security procedure, so no opportunity for both ends to start
    // the same one. Counted separately from the pairing-window rejections
    // because it is a lifecycle refusal, not an admission-policy failure.
    if (!btstack_host_classic_admission_allowed(addr)) {
        classic_companion_refused_no_mgmt++;
        btstack_host_note_admission_reject(addr, btstack_host_classic_has_trust(addr));
        btstack_host_note_link_refusal(addr, true);
        btlife_record_addr(BTLIFE_PAGE_REJECT, BTLIFE_REJECT_NO_MGMT, 0u, addr);
        printf("[BTSTACK_HOST] Refusing Controller Link: no live management session "
               "(refused=%lu)\n",
               (unsigned long)classic_companion_refused_no_mgmt);
        return 0;
    }
    ns2_bt_admission_t admission = ns2_bt_admission_decide(
        pairing_lockout, hid_pairing_window_open,
        btstack_host_classic_has_trust(addr));
    if (admission == NS2_BT_ADMISSION_REJECT) {
        btstack_host_record_fresh_admission(false);
        btstack_host_note_admission_reject(addr, btstack_host_classic_has_trust(addr));
        btlife_record_addr(BTLIFE_PAGE_REJECT, BTLIFE_REJECT_ADMISSION, 0u, addr);
        printf("[BTSTACK_HOST] Rejecting unbonded Classic ACL outside pairing window\n");
        return 0;
    }
    // Accepted: the page response proceeds. Paired with PAGE_RX this is the
    // adapter's half of "we answered", against which an Android-side
    // PAGE_TIMEOUT can be read.
    btlife_record_addr(BTLIFE_PAGE_ACCEPT, 0u, 0u, addr);
    return 1;
}

void btstack_host_clear_pairing_lockout(void)
{
    if (!pairing_lockout) return;

    pairing_lockout = false;
    btstack_host_store_pairing_lockout(false);
    classic_state.recovery_start_time = 0;
    printf("[BTSTACK_HOST] Pairing admission re-enabled by explicit pairing window\n");

#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF) && !defined(CONFIG_USB2BLE)
    gap_discoverable_control(hid_pairing_window_open ? 1 : 0);
    gap_connectable_control(1);
    btlife_record(BTLIFE_PAGE_SCAN_ON, 0u, 0u);
#endif
}

void btstack_host_close_pairing_window(void)
{
    if (hid_state.state == BLE_STATE_CONNECTING) {
        printf("[BTSTACK_HOST] Pairing window closing but a BLE connect is in flight -- "
               "deferring close until it resolves\n");
        pairing_close_deferred = true;
        return;
    }
    btstack_host_stop_scan();
}

bool btstack_host_pairing_close_deferred(void)
{
    return pairing_close_deferred;
}

// Resolve a deferred pairing-window close once the in-flight BLE connect
// attempt concludes (success -> BLE_STATE_CONNECTED, or failure). Called from
// both branches of HCI_SUBEVENT_LE_CONNECTION_COMPLETE. The BLE_CONNECT_TIMEOUT_MS
// watchdog's gap_connect_cancel() reaches this indirectly too: per the existing
// design (see that watchdog's own comment), the cancel itself generates a
// failure LE_CONNECTION_COMPLETE, which is handled by the failure branch here.
// No-op if nothing is deferred, so it's always safe to call.
static void resolve_deferred_pairing_close(void)
{
    if (!pairing_close_deferred) {
        return;
    }
    pairing_close_deferred = false;
    printf("[BTSTACK_HOST] Deferred pairing-window close: candidate resolved, closing now\n");
    btstack_host_stop_scan();
}

void btstack_host_start_timed_scan(uint32_t timeout_ms)
{
    scan_suppressed = false;  // Explicit scan request clears suppression
    scan_timeout_end = btstack_run_loop_get_time_ms() + timeout_ms;
    printf("[BTSTACK_HOST] Starting timed scan (%lums)\n", (unsigned long)timeout_ms);
    btstack_host_start_scan();
}

void btstack_host_suppress_scan(bool suppress)
{
    scan_suppressed = suppress;
    if (suppress && btstack_host_is_scanning()) {
        btstack_host_stop_scan();
    }
}

// True when a controller is fully connected (HID ready) on either transport.
// Keying on hid_ready (setup complete) rather than a raw connection count means
// callers never act on a mid-handshake connection.
static bool any_controller_hid_ready(void)
{
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (classic_state.connections[i].active &&
            classic_state.connections[i].hid_ready) {
            return true;
        }
    }
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID &&
            hid_state.connections[i].hid_ready) {
            return true;
        }
    }
    return false;
}

// 1 dongle : 1 controller. Used to retire the always-on multi-controller
// discovery: a controller finishing connection closes the pairing window (LED
// goes solid) and lets discovery idle. The host stays connectable/discoverable
// (set at init), so a bonded Classic controller still reconnects by paging in
// and a bonded BLE controller reconnects once discovery resumes at 0 connections
// (btstack_host_process safety-net).
bool btstack_host_controller_connected(void)
{
    return any_controller_hid_ready();
}

// Idle background BLE scan + Classic inquiry while a controller is connected.
// This is the steady-state / reconnect path (fresh pairing closes its window via
// btstack_host_controller_connected() instead). stop_scan() also leaves the BLE
// state machine IDLE, so the inquiry-complete handler stops re-arming. Discovery
// resumes automatically at 0 connections via btstack_host_process()'s safety-net.
// Caller gates out the pairing window so fresh pairing keeps scanning.
void btstack_host_idle_scan_if_connected(void)
{
    if (pairing_close_deferred) {
        return;  // let an in-flight pairing candidate resolve first
    }
    if (any_controller_hid_ready() &&
        (hid_state.scan_active || classic_state.inquiry_active)) {
        btstack_host_stop_scan();
    }
}

// Counterpart to btstack_host_idle_scan_if_connected(): keep discovery RUNNING
// while the selected source is still missing a peer, even though another peer is
// already connected.
//
// This exists because no other path can do it. A BLE HID peer reaching ready
// stops the scan unconditionally, and the only thing that would restore it --
// btstack_host_process()'s idle safety-net -- ends in
// btstack_classic_get_connection_count() == 0, which counts BLE links despite
// its name. With one peer connected that term is false, the predicate
// short-circuits, and start_scan() is never even reached (which is why the
// failing hardware showed scan starts == stops with no suppression counter
// climbing). The first peer to arrive shut the door behind itself.
//
// The caller owns the policy: this only executes the mechanics.
void btstack_host_scan_for_additional_peer(void)
{
    if (pairing_close_deferred) {
        return;  // let an in-flight pairing candidate resolve first
    }
    // Never disturb a connect already in flight: btstack_host_start_scan()
    // would overwrite hid_state.state out from under it.
    if (hid_state.state == BLE_STATE_CONNECTING) {
        return;
    }
    // Checked here rather than relying on start_scan()'s own guard so the
    // steady state does not increment the ALREADY suppression counter on every
    // 30 ms tick and drown the diagnostic.
    if (hid_state.scan_active || classic_state.inquiry_active) {
        return;
    }
    btstack_host_start_scan();
}

// ============================================================================
// CONNECTION
// ============================================================================

#define BLE_CONNECT_TIMEOUT_MS 10000   // 10s timeout for BLE connection attempts

static void btstack_host_connect_ble_candidate(bd_addr_t addr,
                                                bd_addr_type_t addr_type,
                                                bool rpa_trust_candidate)
{
    // No config/management gate here: the controller link is independent of the
    // USB face (see btstack_host_start_scan). A controller connects/reconnects
    // regardless of config mode, in-band management, or a pending personality
    // re-enumeration. Only wake replay (advertiser owner) is arbitrated elsewhere.
    printf("[BTSTACK_HOST] Connecting to %02X:%02X:%02X:%02X:%02X:%02X\n",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    // Stop scanning first
    btstack_host_stop_scan();

    // Save pending connection info
    memcpy(hid_state.pending_addr, addr, 6);
    hid_state.pending_addr_type = addr_type;
    hid_state.pending_rpa_trust_candidate = rpa_trust_candidate;
    hid_state.pending_fresh_pairing_admitted =
        !pairing_lockout &&
        (hid_pairing_window_open ||
         (switch2_explicit_fresh_pairing_admitted &&
          hid_state.pending_profile == &BT_PROFILE_SWITCH2));
    if (hid_state.pending_fresh_pairing_admitted &&
        switch2_explicit_fresh_pairing_admitted &&
        hid_state.pending_profile == &BT_PROFILE_SWITCH2) {
        // UART `btfresh` is a one-candidate diagnostic admission, not an
        // unbounded global pairing mode.
        switch2_explicit_fresh_pairing_admitted = false;
    }
    hid_state.state = BLE_STATE_CONNECTING;
    hid_state.reconnect_attempt_time = btstack_run_loop_get_time_ms();

    if (hid_state.has_last_connected &&
        memcmp(addr, hid_state.last_connected_addr, sizeof(bd_addr_t)) == 0) {
        hid_state.target_connect_attempts++;
    }

    // Create connection
    uint8_t status = gap_connect(addr, addr_type);
    printf("[BTSTACK_HOST] gap_connect returned status=%d\n", status);
}

void btstack_host_connect_ble(bd_addr_t addr, bd_addr_type_t addr_type)
{
    btstack_host_connect_ble_candidate(addr, addr_type, false);
}

// ============================================================================
// CALLBACKS
// ============================================================================

void btstack_host_register_report_callback(btstack_host_report_callback_t callback)
{
    hid_state.report_callback = callback;
}

void btstack_host_register_connect_callback(btstack_host_connect_callback_t callback)
{
    hid_state.connect_callback = callback;
}

// ============================================================================
// MAIN LOOP
// ============================================================================

static bool bt_health_claimed_acl(void)
{
    if (config_ble.handle != HCI_CON_HANDLE_INVALID) return true;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; ++i) {
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID) return true;
    }
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; ++i) {
        if (classic_state.connections[i].active) return true;
    }
    return false;
}

static hci_con_handle_t bt_health_find_probe_handle(void)
{
    btstack_linked_list_iterator_t iterator;
    hci_connections_get_iterator(&iterator);
    while (btstack_linked_list_iterator_has_next(&iterator)) {
        hci_connection_t *connection =
            (hci_connection_t *)btstack_linked_list_iterator_next(&iterator);
        if (connection->state == OPEN &&
            connection->con_handle != HCI_CON_HANDLE_INVALID) {
            return connection->con_handle;
        }
    }
    return HCI_CON_HANDLE_INVALID;
}

// True while an admitted pairing/security procedure owns the radio. Such a
// procedure is legitimately quiet on the HCI event path -- a peer is running
// SSP/SMP, inquiry is sweeping, or the user is reading a pairing dialog -- so
// liveness escalation is suppressed for a bounded interval rather than mistaking
// it for a wedge. See NS2_BT_HEALTH_SECURITY_SUPPRESS_MAX_MS.
static bool bt_health_security_in_flight(void)
{
    if (hid_pairing_window_open || pairing_close_deferred) return true;
    if (classic_state.pending_valid) return true;
    return config_ble.handle != HCI_CON_HANDLE_INVALID &&
           config_ble.fresh_bond_admitted;
}

static void bt_health_note_escalation(void)
{
    ns2_bt_recovery_escalation_t snapshot = {
        .phase = (uint8_t)bt_health.phase,
        .probes_sent = (uint8_t)bt_health.probes_sent,
        .probe_failures =
            (uint8_t)(bt_health.probes_failed + bt_health.probe_timeouts),
        .recovery_attempts = (uint8_t)bt_health.recovery_attempts,
        .uptime_s = (uint8_t)(btstack_run_loop_get_time_ms() / 1000u),
        .pairing_window_open = hid_pairing_window_open,
        .management_client = config_ble.handle != HCI_CON_HANDLE_INVALID,
        .classic_link = btstack_classic_get_connection_count() > 0,
        .ble_link = bt_health_find_probe_handle() != HCI_CON_HANDLE_INVALID,
        .discovery_active = hid_state.scan_active || classic_state.inquiry_active,
        .valid = true,
    };
    ns2_bt_recovery_note_escalation(&snapshot);
}

static void bt_health_service(void)
{
    uint32_t const now = btstack_run_loop_get_time_ms();
    HCI_STATE const state = hci_get_state();
    hci_con_handle_t const candidate = bt_health_find_probe_handle();
    ns2_bt_health_inputs_t const inputs = {
        .hci_working = state == HCI_STATE_WORKING,
        .hci_off = state == HCI_STATE_OFF,
        .claimed_acl = bt_health_claimed_acl(),
        .probe_handle_available = candidate != HCI_CON_HANDLE_INVALID,
        .security_in_flight = bt_health_security_in_flight(),
    };
    ns2_bt_health_action_t const action =
        ns2_bt_health_tick(&bt_health, now, &inputs);

    switch (action) {
        case NS2_BT_HEALTH_ACTION_SEND_PROBE:
            bt_health_probe_handle = candidate;
            printf("[BT_HEALTH] probing handle 0x%04X after quiet HCI interval\n",
                   candidate);
            if (!gap_read_rssi(candidate)) {
                ns2_bt_health_note_probe_complete(&bt_health, now, 0xFFu);
            }
            break;
        case NS2_BT_HEALTH_ACTION_POWER_OFF:
            bt_health_probe_handle = HCI_CON_HANDLE_INVALID;
            // Capture the radio context now, while it is still true. If the
            // power cycle itself times out this is the only description of the
            // event that survives the reboot.
            bt_health_note_escalation();
            printf("[BT_HEALTH] HCI liveness failed; requesting bounded power cycle\n");
            hci_power_control(HCI_POWER_OFF);
            break;
        case NS2_BT_HEALTH_ACTION_POWER_ON:
            printf("[BT_HEALTH] HCI is off; restoring controller power\n");
            hci_power_control(HCI_POWER_ON);
            break;
        case NS2_BT_HEALTH_ACTION_REQUEST_REBOOT:
            // Re-record so the surviving snapshot names the phase that actually
            // timed out (POWERING_OFF vs POWERING_ON), which the 2026-08-21
            // field event could not tell us.
            bt_health_note_escalation();
            printf("[BT_HEALTH] HCI power transition timed out in %s; requesting rate-limited reboot\n",
                   ns2_bt_health_phase_name(bt_health.phase));
            ns2_bt_recovery_request_reboot(NS2_BT_REBOOT_CAUSE_HCI_POWER_TIMEOUT);
            break;
        case NS2_BT_HEALTH_ACTION_NONE:
        default:
            break;
    }
}


// Transport-specific process function (weak, overridden by transport)
__attribute__((weak)) void btstack_host_transport_process(void) {
    // Default: no-op, transport should override
}

void btstack_host_process(void)
{
    if (!hid_state.initialized) return;

    bt_health_service();

    // Configuration/management is a BLE peripheral while USB is in the explicit
    // CDC Config personality OR when in-band management is enabled (production
    // default on; `mgmt off` is a current-boot escape hatch).
    // The false path is intentionally a state comparison only and performs no
    // advertising or ACL work -- byte-identical to today's normal mode.
    config_ble_service_task(config_ble_authorized());

    // Process transport-specific tasks (e.g., USB polling, CYW43 async context)
    btstack_host_transport_process();

#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
    // Process BTstack run loop multiple times to let packets flow through HCI->L2CAP->ATT->GATT
    // Note: CYW43 uses async_context, ESP32 uses FreeRTOS run loop - both process automatically
    for (int i = 0; i < 5; i++) {
        btstack_run_loop_embedded_execute_once();
    }
#endif

    // Process any pending BLE HID report (deferred from BTstack callback to avoid stack overflow)
    if (ble_report_pending) {
        ble_report_pending = false;
        route_ble_hid_report(pending_ble_conn_index,
                             pending_ble_connection_generation,
                             pending_ble_report, pending_ble_report_len);
    }

    // Retry Switch 2 init if stuck (no ACK received)
    switch2_retry_init_if_needed();

    // Advance deferred post-HID setup (REPORT protocol mode -> DIS/BAS/NUS)
    mp_hid_setup_task();

    // BTstack's HID host owns the SDP client until descriptor discovery has
    // fully unwound. Starting our PnP query directly from
    // HID_SUBEVENT_DESCRIPTOR_AVAILABLE can therefore return SDP_QUERY_BUSY.
    // Service the bounded identity request later from the normal run loop.
    classic_identity_query_service();

    // Kick off / advance MouthPad NUS discovery once HID has settled
    mp_nus_periodic();

    // Handle Switch 2 rumble/LED feedback passthrough
    switch2_handle_feedback();

    // Check scan timeout
    if (scan_timeout_end > 0 && btstack_host_is_scanning()) {
        if (btstack_run_loop_get_time_ms() >= scan_timeout_end) {
            printf("[BTSTACK_HOST] Timed scan expired\n");
            scan_timeout_end = 0;
            btstack_host_stop_scan();
        }
    }

    // BLE connection attempt timeout — gap_connect() has no built-in timeout,
    // so if the target device is powered off, we'd be stuck in CONNECTING forever.
    // Cancel after BLE_CONNECT_TIMEOUT_MS. Set state to IDLE immediately to prevent
    // this check from re-triggering on the next tick. The LE_CONNECTION_COMPLETE
    // error event from the cancel will handle retry/resume logic.
    if (hid_state.state == BLE_STATE_CONNECTING &&
        hid_state.reconnect_attempt_time != 0 &&
        (btstack_run_loop_get_time_ms() - hid_state.reconnect_attempt_time) >= BLE_CONNECT_TIMEOUT_MS) {
        printf("[BTSTACK_HOST] BLE connection attempt timed out after %dms\n", BLE_CONNECT_TIMEOUT_MS);
        gap_connect_cancel();
        hid_state.state = BLE_STATE_IDLE;
        hid_state.reconnect_attempt_time = 0;
    }

    // Deferred Classic inquiry restart. The gap between rounds is what leaves
    // the controller room to answer an incoming page; see
    // ns2_bt_inquiry_restart_delay_ms().
    if (classic_state.inquiry_restart_at_ms != 0 &&
        !classic_state.inquiry_active &&
        hid_state.state == BLE_STATE_SCANNING &&
        (int32_t)(btstack_run_loop_get_time_ms() -
                  classic_state.inquiry_restart_at_ms) >= 0) {
        classic_restart_inquiry();
    }

    // Timeout for "waiting for incoming reconnection" after outgoing Classic HID failure.
    // If the device doesn't reconnect within 30s, give up and resume scanning.
    if (classic_state.waiting_for_incoming_time != 0 &&
        (btstack_run_loop_get_time_ms() - classic_state.waiting_for_incoming_time) >= 30000) {
        printf("[BTSTACK_HOST] Incoming reconnection timeout, resuming scan\n");
        classic_state.waiting_for_incoming_time = 0;
    }

    // Classic connection establishment timeout.
    // If a connection doesn't reach hid_ready within CLASSIC_CONNECT_TIMEOUT_MS,
    // something went wrong (e.g., CYW43 SPI bus failure during SSP pairing,
    // incompatible device, or stuck SDP query). Clean up and try to recover.
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        classic_connection_t* conn = &classic_state.connections[i];
        if (conn->active && !conn->hid_ready && conn->connect_time != 0 &&
            (btstack_run_loop_get_time_ms() - conn->connect_time) >= CLASSIC_CONNECT_TIMEOUT_MS) {
            printf("[BTSTACK_HOST] Classic connection timeout after %lums (slot %d '%s'), cleaning up\n",
                   (unsigned long)(btstack_run_loop_get_time_ms() - conn->connect_time), i, conn->name);

            // Try to disconnect (may fail if BT transport is dead)
            if (conn->hid_cid != 0 && conn->hid_cid != 0xFFFF) {
                hid_host_disconnect(conn->hid_cid);
            }

            // Clean up wiimote state if this was a direct L2CAP device
            if (wiimote_conn.active && memcmp(wiimote_conn.addr, conn->addr, 6) == 0) {
                memset(&wiimote_conn, 0, sizeof(wiimote_conn));
            }

            // Clean up connection slot
            memset(conn, 0, sizeof(*conn));
            classic_state.pending_valid = false;
            classic_state.pending_hid_connect = false;
            classic_pending_security_clear();

            // Start recovery timer and try to resume scanning
            classic_state.recovery_start_time = btstack_run_loop_get_time_ms();
            btstack_host_start_scan();
            break;  // Only handle one timeout per tick
        }
    }

    // Recovery watchdog: if we cleaned up a stuck connection but BT transport
    // appears dead (no inquiry events received within 10s), force a reboot.
    if (!pairing_lockout && classic_state.recovery_start_time != 0 &&
        (btstack_run_loop_get_time_ms() - classic_state.recovery_start_time) >= 10000) {
        printf("[BTSTACK_HOST] No BT activity after connection timeout recovery, rebooting\n");
        platform_reboot();
    }

    // Safety net: if idle with no active connections and not scanning, resume scan.
    // This catches edge cases where the state machine gets stuck (e.g., gap_connect_cancel
    // doesn't generate an error event, or a disconnect handler didn't restart scanning).
    // Skip if:
    //   - scan_suppressed (app paused scanning, e.g. USB device connected)
    //   - waiting for incoming Classic reconnection (outgoing HID failed)
    //   - Classic connection setup in progress (name request, HID connect pending)
#ifndef CONFIG_USB2BLE
    if (hid_state.powered_on &&
        !wake_adv.active &&
        !pairing_lockout &&
        !scan_suppressed &&
        hid_state.state == BLE_STATE_IDLE &&
        hid_state.reconnect_attempt_time == 0 &&
        !hid_state.scan_active &&
        classic_state.waiting_for_incoming_time == 0 &&
        !classic_state.pending_valid &&
        // NOTE: despite its name this counts BLE links too (see its definition),
        // so this whole predicate is false while ANY BLE peer is connected. That
        // is deliberate for the 1-dongle-1-controller rule, but it means the
        // safety net can NEVER restore discovery for a second peer -- it is not
        // a fallback for a partial composite source. A KB/M source that is still
        // missing a role is re-armed explicitly from ns2_bt_host.c via
        // btstack_host_scan_for_additional_peer(); do not try to widen this term
        // instead, or a single connected controller would resume discovery and
        // undo the retired multi-controller scanning.
        btstack_classic_get_connection_count() == 0) {
        printf("[BTSTACK_HOST] Safety: idle with no connections, resuming scan\n");
        btstack_host_start_scan();
    }
#endif

    // Republish the bond inventory for core 0's diagnostics (see
    // bond_snapshot_refresh). Cheap and bounded: at most 16 DB entries every
    // 30 ms, and it is the only safe way core 0 can observe bond survival.
    bond_snapshot_refresh();

    // State/scan_active sync: if BLE scan is running but state is not SCANNING,
    // fix the desync so the advertising handler can auto-connect to devices.
    if (hid_state.scan_active && hid_state.state == BLE_STATE_IDLE) {
        printf("[BTSTACK_HOST] Safety: scan active but state IDLE, fixing to SCANNING\n");
        hid_state.state = BLE_STATE_SCANNING;
    }

    // Periodic reconnection to bonded device while scanning.
    // Many BLE devices (e.g. Stadia) don't advertise in discoverable mode after
    // bonding — they expect the central to connect directly via gap_connect().
    // After the rapid reconnect attempts (right after disconnect) are exhausted,
    // alternate between scanning and reconnection attempts.
    //
    // `last_connected` is a single slot holding the most recent BLE peer, which
    // is not necessarily a MISSING one. With a Keyboard + Mouse source the
    // keyboard connects second and therefore owns that slot; when the mouse
    // then powers off, this fired against the keyboard — which is still
    // connected — and btstack_host_connect_ble() tore down the scan window on
    // every attempt. Measured as balanced scan starts/stops with the bonded
    // mouse advertising and never being seen. Selection now runs over the bond
    // database and never yields a live identity, so the scan is left up for
    // whichever peer is actually absent.
    if (!wake_adv.active &&
        !switch2_force_fresh_custom_pairing &&
        hid_state.state == BLE_STATE_SCANNING &&
        hid_state.scan_start_time != 0 &&
        (btstack_run_loop_get_time_ms() - hid_state.scan_start_time) >= BLE_RECONNECT_INTERVAL_MS) {
        ns2_ble_reconnect_decision_t periodic = btstack_host_pick_reconnect();
        if (periodic.action == NS2_BLE_RECONNECT_DIRECT) {
            printf("[BTSTACK_HOST] Periodic reconnection to bonded device '%s'\n",
                   hid_state.last_connected_name);
            strncpy(hid_state.pending_name, hid_state.last_connected_name, sizeof(hid_state.pending_name) - 1);
            hid_state.pending_name[sizeof(hid_state.pending_name) - 1] = '\0';
            hid_state.pending_profile = hid_state.last_connected_profile;
            hid_state.pending_vid = hid_state.last_connected_vid;
            hid_state.pending_pid = hid_state.last_connected_pid;
            btstack_host_connect_ble(periodic.addr, periodic.addr_type);
        }
        // NS2_BLE_RECONNECT_SCAN / _IDLE: leave the scan running. For an absent
        // peer without stored metadata the advertising path is what reconnects
        // it, and it can only do that if the scan window survives.
    }
}

// ============================================================================
// SDP QUERY CALLBACK (for VID/PID detection)
// ============================================================================

static void sdp_query_vid_pid_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    // Debug: log connection-related HCI events for Wiimote troubleshooting
    if (wiimote_conn.active && event_type >= 0x01 && event_type <= 0x20) {
        printf("[BTSTACK_HOST] HCI event: 0x%02X\n", event_type);
    }

    switch (event_type) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE: {
            uint16_t attr_len = sdp_event_query_attribute_byte_get_attribute_length(packet);
            if (attr_len <= sdp_attribute_value_buffer_size) {
                uint16_t offset = sdp_event_query_attribute_byte_get_data_offset(packet);
                sdp_attribute_value[offset] = sdp_event_query_attribute_byte_get_data(packet);

                // Check if we got all bytes for this attribute
                if (offset + 1 == attr_len) {
                    uint16_t attr_id = sdp_event_query_attribute_byte_get_attribute_id(packet);
                    uint16_t value;
                    if (de_element_get_uint16(sdp_attribute_value, &value)) {
                        if (attr_id == BLUETOOTH_ATTRIBUTE_VENDOR_ID) {
                            classic_identity_query.vendor_id = value;
                            printf("[BTSTACK_HOST] SDP VID: 0x%04X\n", value);
                        } else if (attr_id == BLUETOOTH_ATTRIBUTE_PRODUCT_ID) {
                            classic_identity_query.product_id = value;
                            printf("[BTSTACK_HOST] SDP PID: 0x%04X\n", value);
                        }
                    }
                }
            }
            break;
        }
        case SDP_EVENT_QUERY_COMPLETE:
            printf("[BTSTACK_HOST] SDP query complete: VID=0x%04X PID=0x%04X\n",
                   classic_identity_query.vendor_id,
                   classic_identity_query.product_id);

            // Update the connection struct with VID/PID
            if (classic_identity_query.vendor_id ||
                classic_identity_query.product_id) {
                for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
                    classic_connection_t* conn = &classic_state.connections[i];
                    if (conn->active &&
                        memcmp(conn->addr, classic_identity_query.addr, 6) == 0) {
                        conn->vendor_id = classic_identity_query.vendor_id;
                        conn->product_id = classic_identity_query.product_id;
                        printf("[BTSTACK_HOST] Updated conn[%d] VID/PID: 0x%04X/0x%04X\n",
                               i, conn->vendor_id, conn->product_id);

                        // Notify bthid to re-evaluate driver selection with new VID/PID
                        bthid_update_device_info(i, conn->name,
                                                  classic_identity_query.vendor_id,
                                                  classic_identity_query.product_id);

                        // Re-send HID descriptor in case driver was re-evaluated to generic
                        // (descriptor was delivered earlier but ignored by the previous driver)
                        const uint8_t* hid_desc = hid_descriptor_storage_get_descriptor_data(conn->hid_cid);
                        uint16_t hid_desc_len = hid_descriptor_storage_get_descriptor_len(conn->hid_cid);
                        if (hid_desc && hid_desc_len > 0) {
                            bthid_set_hid_descriptor(i, hid_desc, hid_desc_len);
                        }
                        break;
                    }
                }

                // Also update wiimote_conn if active and address matches
                if (wiimote_conn.active &&
                    memcmp(wiimote_conn.addr, classic_identity_query.addr, 6) == 0) {
                    wiimote_conn.vendor_id = classic_identity_query.vendor_id;
                    wiimote_conn.product_id = classic_identity_query.product_id;
                    printf("[BTSTACK_HOST] Updated wiimote VID/PID: 0x%04X/0x%04X\n",
                           wiimote_conn.vendor_id, wiimote_conn.product_id);
                }
            } else if (classic_identity_query.attempts < 3u) {
                classic_identity_query.pending = true;
                classic_identity_query.next_attempt_ms =
                    btstack_run_loop_get_time_ms() + 250u;
            }
            classic_identity_query.active = false;
            break;
    }
}

static void classic_identity_query_schedule(const bd_addr_t addr)
{
    if (!addr) return;
    if ((classic_identity_query.pending || classic_identity_query.active) &&
        memcmp(classic_identity_query.addr, addr, sizeof(bd_addr_t)) == 0) {
        return;
    }
    memcpy(classic_identity_query.addr, addr, sizeof(bd_addr_t));
    classic_identity_query.vendor_id = 0;
    classic_identity_query.product_id = 0;
    classic_identity_query.attempts = 0;
    classic_identity_query.next_attempt_ms = btstack_run_loop_get_time_ms();
    classic_identity_query.pending = true;
    classic_identity_query.active = false;
}

/*
 * Is the PnP SDP identity query for this address still going to produce an
 * answer?
 *
 * True only while a query is scheduled or running for this exact peer, has not
 * exhausted its attempts, and has not already yielded a VID/PID. That is the
 * window in which a generic-fallback classification is PROVISIONAL rather than
 * final -- see mgmt_peers_classification_publishable().
 *
 * A BLE peer's address never matches this record, so BLE classification is
 * unaffected and is published exactly as before.
 */
static bool classic_identity_unresolved(const uint8_t addr[6])
{
    if (!addr || memcmp(classic_identity_query.addr, addr, sizeof(bd_addr_t)) != 0)
        return false;
    if (classic_identity_query.vendor_id || classic_identity_query.product_id)
        return false;
    return (classic_identity_query.pending || classic_identity_query.active) &&
           classic_identity_query.attempts < 3u;
}

static void classic_identity_query_service(void)
{
    if (!classic_identity_query.pending ||
        classic_identity_query.active ||
        classic_identity_query.attempts >= 3u ||
        (int32_t)(btstack_run_loop_get_time_ms() -
                  classic_identity_query.next_attempt_ms) < 0 ||
        !sdp_client_ready()) {
        return;
    }

    classic_identity_query.pending = false;
    classic_identity_query.active = true;
    classic_identity_query.vendor_id = 0;
    classic_identity_query.product_id = 0;
    classic_identity_query.attempts++;
    uint8_t const status = sdp_client_query_uuid16(
        &sdp_query_vid_pid_callback, classic_identity_query.addr,
        BLUETOOTH_SERVICE_CLASS_PNP_INFORMATION);
    if (status != ERROR_CODE_SUCCESS) {
        classic_identity_query.active = false;
        if (classic_identity_query.attempts < 3u) {
            classic_identity_query.pending = true;
            classic_identity_query.next_attempt_ms =
                btstack_run_loop_get_time_ms() + 100u;
        }
        printf("[BTSTACK_HOST] Deferred VID/PID SDP start failed: 0x%02X\n",
               status);
    } else {
        printf("[BTSTACK_HOST] Deferred VID/PID SDP query started (attempt %u)\n",
               classic_identity_query.attempts);
    }
}

static void btstack_host_clear_transient_radio_state(void)
{
    // HCI state loss does not reliably produce one disconnect event per link.
    // Retire every source generation mechanically so input ownership and the
    // owner LED cannot remain attached to stale transport slots.
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; ++i) {
        classic_connection_t *conn = &classic_state.connections[i];
        if (conn->active && conn->hid_ready) {
            bt_on_disconnect_with_generation(
                (uint8_t)i, bthid_get_connection_generation((uint8_t)i));
        }
        memset(conn, 0, sizeof(*conn));
    }
    for (int i = 0; i < MAX_BLE_CONNECTIONS; ++i) {
        ble_connection_t *conn = &hid_state.connections[i];
        if (conn->handle != HCI_CON_HANDLE_INVALID && conn->conn_index > 0u) {
            bt_on_disconnect_with_generation(
                conn->conn_index,
                bthid_get_connection_generation(conn->conn_index));
        }
        memset(conn, 0, sizeof(*conn));
        conn->handle = HCI_CON_HANDLE_INVALID;
    }

    switch2_cleanup_on_disconnect(0xFFu, 0u);
    memset(&wiimote_conn, 0, sizeof(wiimote_conn));
    classic_state.inquiry_active = false;
    classic_state.pending_valid = false;
    classic_state.pending_hid_connect = false;
    classic_state.pending_acl_handle = HCI_CON_HANDLE_INVALID;
    classic_state.waiting_for_incoming_time = 0u;
    classic_state.recovery_start_time = 0u;
    classic_pending_security_clear();

    hid_state.powered_on = false;
    hid_state.state = BLE_STATE_IDLE;
    hid_state.scan_active = false;
    hid_state.reconnect_attempt_time = 0u;
    hid_state.reconnect_attempts = 0u;
    hid_state.pending_fresh_pairing_admitted = false;
    hid_state.pending_rpa_trust_candidate = false;
    hid_state.gatt_state = GATT_IDLE;
    hid_state.gatt_handle = HCI_CON_HANDLE_INVALID;
    hid_state.bas_cid = 0u;
    ble_report_pending = false;
    pairing_close_deferred = false;

    config_ble.advertising = false;
    config_ble.handle = HCI_CON_HANDLE_INVALID;
    config_ble.fresh_bond_admitted = false;
    config_ble.client_addr_valid = false;
    // The resolved identity belongs to the ACL that resolved it. A reused
    // handle must never inherit the previous companion's identity.
    config_ble.client_identity_valid = false;
    config_ble.closing = false;
    config_ble.notifications_enabled = false;
    config_ble.tx_requested = false;
    config_wireless_bridge_reset_session();
    // Every ACL died with the HCI, so the Controller Link binding is stale
    // rather than something to tear down: forget it so a reused handle after
    // recovery cannot inherit it. No gap_disconnect here -- there is no link.
    classic_companion_acl_handle = HCI_CON_HANDLE_INVALID;
    // A transient HCI loss can arrive between any wake phases. Detach the
    // embedded timer before clearing its callback/list linkage so no stale
    // callback can run against reinitialized state after HCI recovery.
    btstack_run_loop_remove_timer(&wake_adv.timer);
    memset(&wake_adv, 0, sizeof(wake_adv));
}

// ============================================================================
// HCI EVENT HANDLER
// ============================================================================

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    ns2_bt_health_note_hci_event(&bt_health, btstack_run_loop_get_time_ms());

    // Verbose event traces are development-only. Production diagnostics use
    // bounded counters/snapshots so radio traffic cannot become log traffic.
#ifdef NS2_DIAG
    // Debug: log key HCI events to debug Wiimote reconnection
    // 0x04=CONNECTION_COMPLETE, 0x05=DISCONNECTION_COMPLETE, 0x06=AUTH_COMPLETE
    // 0x08=ENCRYPTION_CHANGE, 0x17=LINK_KEY_REQUEST, 0x18=LINK_KEY_NOTIFICATION
    // 0x16=PIN_CODE_REQUEST, 0x04=CONNECTION_REQUEST (offset differs)
    if (event_type == 0x17 || event_type == 0x18 || event_type == 0x06 ||
        event_type == 0x08 || event_type == 0x16) {
        printf("[BTSTACK_HOST] >>> HCI Event 0x%02X (size=%d)\n", event_type, size);
    }

    // Debug: catch GATT notifications at the global level
    if (event_type == GATT_EVENT_NOTIFICATION) {
        printf("[BTSTACK_HOST] >>> RAW GATT NOTIFICATION! len=%d\n", size);
    }
#endif

#ifdef NS2_DS5_AUDIO
    // l2cap_send() only confirms that BTstack/HCI accepted an ACL packet. The
    // controller reports actual radio-side completion later through this HCI
    // event, potentially batching several packets. Experimental audio builds
    // already enforce the project's one-controller invariant, so count every
    // post-stream ACL completion. Filtering on wiimote_conn.acl_handle is not
    // reliable across Sony's incoming HID-Host/direct-output reconnect path:
    // that path can have a valid interrupt CID while retaining a different
    // bookkeeping handle, which made the first completion meter read zero.
    if (event_type == HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS && size >= 3u) {
        uint8_t const num_handles = packet[2];
        uint16_t offset = 3u;
        for (uint8_t i = 0; i < num_handles && offset + 4u <= size; ++i) {
            uint16_t const completed =
                little_endian_read_16(packet, offset + 2u);
            offset += 4u;
            ds5_audio_diag_note_hci_completion(time_us_32(), completed);
        }
    }
#endif

    switch (event_type) {
        case BTSTACK_EVENT_STATE: {
            uint8_t stack_state = btstack_event_state_get_state(packet);
            if (stack_state != HCI_STATE_WORKING) {
                if (hid_state.powered_on ||
                    btstack_classic_get_connection_count() > 0u ||
                    config_ble.handle != HCI_CON_HANDLE_INVALID) {
                    hci_state_losses++;
                    printf("[BTSTACK_HOST] HCI left working state (%u); clearing transient links\n",
                           stack_state);
                    btstack_host_clear_transient_radio_state();
                }
                break;
            }
            {
                printf("[BTSTACK_HOST] HCI working\n");
                hid_state.powered_on = true;

                // Reset scan state (in case of reconnect)
                hid_state.scan_active = false;
                classic_state.inquiry_active = false;
                btstack_host_restore_pairing_lockout();
                const bool install_reset =
                    ns2_bt_install_reset_bootstrap_take(
                        config_install_reset_performed(),
                        &install_reset_bootstrap_consumed);
                pairing_lockout = ns2_bt_boot_pairing_locked(
                    pairing_lockout, install_reset);
                if (install_reset) {
                    // config_load() erased the complete persistence region
                    // before BTstack/TLV existed. Recreate only the lock tag
                    // now, before discovery or connectability can be enabled.
                    btstack_host_store_pairing_lockout(true);
                    printf("[BTSTACK_HOST] New firmware install: pairing admission locked until explicit gesture\n");
                }

#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
                // Set master role policy for incoming Classic connections
                // Wiimotes (including Wii U Pro) REQUIRE us to be master
                hci_set_master_slave_policy(0);  // 0 = always try to become master
                gap_register_classic_connection_filter(btstack_host_classic_connection_filter);
                printf("[BTSTACK_HOST] Set master role policy\n");
#endif

                // Print our local BD_ADDR
                bd_addr_t local_addr;
                gap_local_bd_addr(local_addr);
                printf("[BTSTACK_HOST] Local BD_ADDR: %02X:%02X:%02X:%02X:%02X:%02X\n",
                       local_addr[0], local_addr[1], local_addr[2],
                       local_addr[3], local_addr[4], local_addr[5]);

                // Print chip info (see hci_transport_h2_tinyusb.h for dongle compatibility guide)
                uint16_t manufacturer = hci_get_manufacturer();
                printf("[BTSTACK_HOST] Chip Manufacturer: 0x%04X", manufacturer);
                switch (manufacturer) {
                    case 0x000A: printf(" (CSR) - OK\n"); break;
                    case 0x000D: printf(" (TI)\n"); break;
                    case 0x000F: printf(" (Broadcom) - OK\n"); break;
                    case 0x001D: printf(" (Qualcomm)\n"); break;
                    case 0x0046: printf(" (MediaTek)\n"); break;
                    case 0x005D: printf(" (Realtek) - NEEDS FIRMWARE!\n"); break;
                    case 0x0002: printf(" (Intel)\n"); break;
                    default: printf("\n"); break;
                }

                // Set local name (for devices that want to see us)
                // Skip when acting as BLE peripheral — ble_output sets its own name
#ifndef CONFIG_USB2BLE
                gap_set_local_name(PICO_SWITCH2_BLUETOOTH_NAME);
#endif

                // Enable bonding (needed for both Classic and BLE)
                gap_set_bondable_mode(1);
                // Set IO capability for "just works" pairing (no PIN required)
                gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);

#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
                // Classic BT setup (not available on ESP32-S3 BLE-only)
#ifndef CONFIG_USB2BLE
                // Set class of device to Computer (Desktop Workstation)
                // Skip when acting as BLE peripheral — appearance is set in adv data
                gap_set_class_of_device(0x000104);  // Major: Computer, Minor: Desktop

                // Enable SSP (Secure Simple Pairing) on the controller
                extern const hci_cmd_t hci_write_simple_pairing_mode;
                hci_send_cmd(&hci_write_simple_pairing_mode, 1);

                // Request bonding during SSP (required for BTstack to store link keys!)
                gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_DEDICATED_BONDING);
                // Keep BTstack's global SSP auto-accept disabled. The app
                // answers each prompt from the admission latch captured for
                // that Classic attempt, so window expiry cannot revoke an
                // in-flight candidate or authorize a later one.
                gap_ssp_set_auto_accept(0);

                // Make host discoverable and connectable for incoming connections
                // Required for Sony controllers (DS3, DS4, DS5) which initiate connections
                gap_discoverable_control(
                    hid_pairing_window_open && !pairing_lockout);
                gap_connectable_control(pairing_lockout ? 0 : 1);
#endif
                // USB2BLE: Classic BT stays non-discoverable/non-connectable by default
#endif

#ifndef CONFIG_USB2BLE
                if (!pairing_lockout) {
                    // Restore last connected device from NVS (for reconnection after reboot)
                    btstack_host_restore_last_connected();

                    // Normal operation continuously scans for controllers.
                    btstack_host_start_scan();
                } else {
                    printf("[BTSTACK_HOST] Pairing admission locked after wipe; waiting for pairing gesture\n");
                }
#endif
            }
            break;
        }

        case GAP_EVENT_ADVERTISING_REPORT: {
            bd_addr_t addr;
            gap_event_advertising_report_get_address(packet, addr);
            bd_addr_type_t addr_type = gap_event_advertising_report_get_address_type(packet);
            int8_t rssi = gap_event_advertising_report_get_rssi(packet);
            uint8_t adv_len = gap_event_advertising_report_get_data_length(packet);
            const uint8_t *adv_data = gap_event_advertising_report_get_data(packet);
            uint8_t adv_event_type = gap_event_advertising_report_get_advertising_event_type(packet);

            hid_state.advertising_reports++;
            if (hid_state.has_last_connected &&
                memcmp(addr, hid_state.last_connected_addr, sizeof(bd_addr_t)) == 0) {
                hid_state.target_advertising_reports++;
                hid_state.last_target_advertising_event_type = adv_event_type;
            }

            // Parse name, appearance, and manufacturer data from advertising data
            char name[48] = {0};
            uint16_t mfr_company_id = 0;
            uint16_t sw2_vid = 0;
            uint16_t sw2_pid = 0;
            uint16_t appearance = 0;
            bool has_hid_uuid = false;

            ad_context_t context;
            for (ad_iterator_init(&context, adv_len, adv_data); ad_iterator_has_more(&context); ad_iterator_next(&context)) {
                uint8_t type = ad_iterator_get_data_type(&context);
                uint8_t len = ad_iterator_get_data_len(&context);
                const uint8_t *data = ad_iterator_get_data(&context);

                if (type == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME ||
                    type == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) {
                    uint8_t copy_len = (len < sizeof(name) - 1) ? len : sizeof(name) - 1;
                    memcpy(name, data, copy_len);
                    name[copy_len] = 0;
                }

                // Parse GAP Appearance (2 bytes, little-endian)
                if (type == BLUETOOTH_DATA_TYPE_APPEARANCE && len >= 2) {
                    appearance = data[0] | (data[1] << 8);
                }

                // Check for HID service UUID (0x1812) in service class lists
                if ((type == BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS ||
                     type == BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS) && len >= 2) {
                    for (int i = 0; i + 1 < len; i += 2) {
                        uint16_t uuid16 = data[i] | (data[i + 1] << 8);
                        if (uuid16 == 0x1812) {  // HID Service
                            has_hid_uuid = true;
                            break;
                        }
                    }
                }

                // Check for Switch 2 controller via manufacturer data
                // Company ID 0x0553 (Nintendo for Switch 2)
                // BlueRetro uses data[1] for company ID, data[6] for VID - their data includes length byte
                // BTstack iterator strips length+type, so we use data[0] for company ID, data[5] for VID
                if (type == BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA && len >= 2) {
                    mfr_company_id = data[0] | (data[1] << 8);
                    if (mfr_company_id == 0x0553) {
                        hid_state.switch2_advertising_reports++;
                        // Debug: print raw manufacturer data
#ifdef NS2_DIAG
                        printf("[SW2_BLE] Mfr data (%d bytes):", len);
                        for (int i = 0; i < len && i < 12; i++) {
                            printf(" %02X", data[i]);
                        }
                        printf("\n");
#endif
                        if (len >= 9) {
                            // VID at bytes 5-6, PID at bytes 7-8 (relative to after company ID)
                            // This matches BlueRetro's offsets accounting for length byte difference
                            sw2_vid = data[5] | (data[6] << 8);
                            sw2_pid = data[7] | (data[8] << 8);
                        }
#ifdef NS2_DIAG
                        printf("[BTSTACK_HOST] Switch 2 controller detected! VID=0x%04X PID=0x%04X\n",
                               sw2_vid, sw2_pid);
#endif
                    }
                }
            }

            // Log all BLE advertisements with names only in diagnostic builds.
#ifdef NS2_DIAG
            if (name[0] != 0) {
                printf("[BTSTACK_HOST] BLE adv: %02X:%02X:%02X:%02X:%02X:%02X name=\"%s\"\n",
                       addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], name);
            }
#endif

            // Merge with pending gamepad data: if we previously saw a gamepad appearance
            // or HID UUID for this address but no name (ADV packet), and now we have
            // the name (SCAN_RSP), use the merged data to identify the device properly.
            if (pending_ble_gamepad.valid &&
                memcmp(addr, pending_ble_gamepad.addr, 6) == 0) {
                // Carry appearance and HID UUID from the ADV packet if not in this packet
                if (appearance == 0) {
                    appearance = pending_ble_gamepad.appearance;
                }
                if (!has_hid_uuid) {
                    has_hid_uuid = pending_ble_gamepad.has_hid_uuid;
                }
                if (name[0]) {
                    // Got a name for the pending gamepad — clear pending and proceed
                    pending_ble_gamepad.valid = false;
                }
            }

            // Identify device by name and/or manufacturer company ID
            const bt_device_profile_t* profile = bt_device_lookup(name, mfr_company_id);
            bool is_known_controller = (profile != &BT_PROFILE_DEFAULT);

            // Generic BLE HID detection (fallback for unknown controllers)
            // Only triggers when no specific driver matched by name/manufacturer.
            // Primary signal: HID service UUID (0x1812) in advertisement
            // Fallback: GAP Appearance 0x03C3 (Joystick) or 0x03C4 (Gamepad)
            // Excludes controllers that use classic BT (DS4, DS3, DS5) — they advertise
            // BLE but must connect via classic for proper driver support.
            bool is_generic_ble_hid = false;
            if (!is_known_controller && !profile->classic_only &&
                (has_hid_uuid || appearance == 0x03C3 || appearance == 0x03C4)) {
                // If no name yet and this isn't already a scan response, defer connection
                // to wait for the scan response which typically contains the device name.
                // This prevents connecting to Xbox controllers as "Generic BLE HID".
                // Only defer once per address — if we already deferred and the scan response
                // didn't bring a name (or never arrived), connect as generic on the next ADV.
                if (!name[0] && adv_event_type != 0x04) {
                    if (!pending_ble_gamepad.valid ||
                        memcmp(addr, pending_ble_gamepad.addr, 6) != 0) {
                        pending_ble_gamepad.valid = true;
                        memcpy(pending_ble_gamepad.addr, addr, 6);
                        pending_ble_gamepad.addr_type = addr_type;
                        pending_ble_gamepad.appearance = appearance;
                        pending_ble_gamepad.has_hid_uuid = has_hid_uuid;
                        pending_ble_gamepad.timestamp = btstack_run_loop_get_time_ms();
                        printf("[BTSTACK_HOST] BLE HID (appearance=0x%04X hid_uuid=%d) with no name, waiting for scan response...\n",
                               appearance, has_hid_uuid);
                        break;
                    }
                    // Second ADV with no name for same address — proceed as generic
                    pending_ble_gamepad.valid = false;
                }
                is_generic_ble_hid = true;
                printf("[BTSTACK_HOST] Generic BLE HID detected: \"%s\" appearance=0x%04X hid_uuid=%d\n",
                       name, appearance, has_hid_uuid);
            }

            bool is_controller = is_known_controller || is_generic_ble_hid;
            bool ble_trust_present = is_controller &&
                btstack_host_find_le_device(addr, addr_type) >= 0;
            bool ble_rpa_trust_candidate = is_controller &&
                !ble_trust_present &&
                btstack_host_addr_is_rpa(addr, addr_type) &&
                le_device_db_count() > 0;
            bool fresh_pairing_authorized = hid_pairing_window_open ||
                (switch2_explicit_fresh_pairing_admitted &&
                 profile == &BT_PROFILE_SWITCH2);
            ns2_bt_admission_t ble_admission = ns2_bt_admission_decide(
                pairing_lockout, fresh_pairing_authorized,
                ble_trust_present);
            bool ble_candidate_admitted =
                ble_admission != NS2_BT_ADMISSION_REJECT ||
                (!pairing_lockout && ble_rpa_trust_candidate);

            // Multi-peer sighting bookkeeping. Counted for HID-looking peers
            // only, so unrelated BLE traffic does not drown the signal.
            if (is_controller || has_hid_uuid) {
                if (btstack_host_addr_is_bonded(addr)) {
                    bonded_adv_reports++;
                    // A bonded peer that is not the single reconnect target:
                    // if this climbs while the peer never rejoins, the peer is
                    // present and the target selection is what is wrong.
                    if (!hid_state.has_last_connected ||
                        memcmp(addr, hid_state.last_connected_addr, sizeof(bd_addr_t)) != 0)
                        nontarget_adv_reports++;
                } else if (btstack_host_addr_is_rpa(addr, addr_type)) {
                    // Rotating identity: a raw address compare can never match
                    // the stored bond, whatever the bond database contains.
                    rpa_adv_reports++;
                }
            }

            // Auto-connect to supported BLE controllers (skip classic-only devices)
            if (hid_state.state == BLE_STATE_SCANNING && is_controller &&
                (profile->ble != BT_BLE_NONE || is_generic_ble_hid) &&
                ble_candidate_admitted) {
                printf("[BTSTACK_HOST] BLE controller: %02X:%02X:%02X:%02X:%02X:%02X name=\"%s\"\n",
                       addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], name);
                // Determine display name from profile and PID
                const char* type_str;
                if (profile == &BT_PROFILE_SWITCH2) {
                    switch (sw2_pid) {
                        case 0x2067: type_str = "Switch 2 Joy-Con L"; break;
                        case 0x2066: type_str = "Switch 2 Joy-Con R"; break;
                        case 0x2069: type_str = "Switch 2 Pro"; break;
                        case 0x2073: type_str = "Switch 2 GameCube"; break;
                        default:     type_str = "Switch 2 Controller"; break;
                    }
                } else if (!is_generic_ble_hid) {
                    type_str = profile->name;
                } else if (appearance == 0x03C3 || appearance == 0x03C4) {
                    type_str = "Generic BLE Gamepad";
                } else {
                    type_str = "BLE HID Device";
                }
                printf("[BTSTACK_HOST] Connecting to %s...\n", type_str);
                // Use advertised name if available, otherwise use device type as fallback
                if (name[0]) {
                    strncpy(hid_state.pending_name, name, sizeof(hid_state.pending_name) - 1);
                } else {
                    strncpy(hid_state.pending_name, type_str, sizeof(hid_state.pending_name) - 1);
                }
                hid_state.pending_name[sizeof(hid_state.pending_name) - 1] = '\0';
                // This is the authoritative Switch 2 identity source currently
                // validated on hardware. Later protocol replies must not replace
                // it until their field layout is independently proven.
                hid_state.pending_profile = profile;
                hid_state.pending_vid = sw2_vid;
                hid_state.pending_pid = sw2_pid;
                btstack_host_connect_ble_candidate(
                    addr, addr_type, ble_rpa_trust_candidate);
            }
            break;
        }

        // Classic BT inquiry result
        case GAP_EVENT_INQUIRY_RESULT: {
            bd_addr_t addr;
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);

            // Parse name from extended inquiry response if available
            char name[240] = {0};
            if (gap_event_inquiry_result_get_name_available(packet)) {
                int name_len = gap_event_inquiry_result_get_name_len(packet);
                if (name_len > 0 && name_len < (int)sizeof(name)) {
                    memcpy(name, gap_event_inquiry_result_get_name(packet), name_len);
                    name[name_len] = 0;
                }
            }

            // Class of Device: Major=0x05 (Peripheral), Minor bits indicate type
            uint8_t major_class = (cod >> 8) & 0x1F;
            uint8_t minor_class = (cod >> 2) & 0x3F;
            bool is_gamepad = (major_class == 0x05) && ((minor_class & 0x0F) == 0x02);  // Gamepad
            bool is_joystick = (major_class == 0x05) && ((minor_class & 0x0F) == 0x01); // Joystick

            // Keyboard / pointing peripherals sit in the OTHER half of the
            // Peripheral minor class (bits 5-4: 01 keyboard, 10 pointing,
            // 11 combo), which the gamepad test above cannot see. They are
            // admitted only while a Keyboard / Keyboard + Mouse mode is
            // actually looking for that role -- Controller mode's admission
            // policy is deliberately left exactly as it was.
            uint8_t peripheral_type = (minor_class >> 4) & 0x03;
            bool cod_keyboard = (major_class == 0x05) &&
                                (peripheral_type == 0x01 || peripheral_type == 0x03);
            bool cod_pointing = (major_class == 0x05) &&
                                (peripheral_type == 0x02 || peripheral_type == 0x03);
            bool is_kbm_peripheral =
                ns2_kbm_runtime_wants_peripheral(cod_keyboard, cod_pointing);

            // Identify device by name
            const bt_device_profile_t* profile = bt_device_lookup_by_name(name);
            bool is_wiimote_family = (profile->classic == BT_CLASSIC_DIRECT_L2CAP);

            // Log all inquiry results for debugging (gamepads highlighted)
#ifdef NS2_DIAG
            const char* type_str = "";
            if (is_wiimote_family) type_str = " [WIIMOTE]";
            else if (is_gamepad || is_joystick) type_str = " [GAMEPAD]";
            else if (is_kbm_peripheral) type_str = " [KB/M]";
            printf("[BTSTACK_HOST] Inquiry: %02X:%02X:%02X:%02X:%02X:%02X COD=0x%06X%s %s\n",
                   addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
                   (unsigned)cod, type_str, name);
#endif

            // Auto-connect to gamepads, Wiimotes, and (only in a KB/M mode that
            // still needs the role) keyboards and pointing devices.
            if ((is_gamepad || is_joystick || is_wiimote_family ||
                 is_kbm_peripheral) && classic_state.inquiry_active) {
                bool classic_trust_present = btstack_host_classic_has_trust(addr);
                ns2_bt_admission_t classic_admission = ns2_bt_admission_decide(
                    pairing_lockout, hid_pairing_window_open,
                    classic_trust_present);
                if (classic_admission == NS2_BT_ADMISSION_REJECT)
                    break;
                // Skip if we already have an active incoming connection to this device
                // (the device connected to us before we found it in inquiry)
                if (classic_state.pending_valid && !classic_state.pending_outgoing &&
                    memcmp(classic_state.pending_addr, addr, 6) == 0) {
                    printf("[BTSTACK_HOST] Already have incoming connection from this device, skipping outgoing\n");
                    break;
                }

                printf("[BTSTACK_HOST] Classic %s found, connecting...\n",
                       is_kbm_peripheral && !is_gamepad && !is_joystick
                           ? "keyboard/pointing device" : "gamepad");
                classic_pair_diag(0xFF, name, cod, 0, 0,
                                  "classic-candidate-admitted");
                btstack_host_stop_scan();  // Stop inquiry

                // Save pending info for PIN code handler and deferred connection
                memcpy(classic_state.pending_addr, addr, 6);
                classic_state.pending_cod = cod;
                strncpy(classic_state.pending_name, name, sizeof(classic_state.pending_name) - 1);
                classic_state.pending_name[sizeof(classic_state.pending_name) - 1] = '\0';
                classic_state.pending_profile = profile;
                classic_state.pending_valid = true;
                classic_state.pending_outgoing = true;  // We initiated this connection
                classic_pending_security_prepare(
                    addr, hid_pairing_window_open && !pairing_lockout);

                // If name is unavailable, request it and defer connection to
                // REMOTE_NAME_REQUEST_COMPLETE. Wiimote-family devices (Wii U Pro,
                // Wiimote) need the name to route through the correct connection
                // path (direct L2CAP vs HID Host), and their name is not always
                // included in the Extended Inquiry Response.
                if (!name[0]) {
                    printf("[BTSTACK_HOST] Name unavailable at inquiry, requesting before connect...\n");
                    classic_state.pending_hid_connect = true;
                    gap_remote_name_request(addr, 0, 0);
                    break;
                }

                bool use_direct_l2cap = (profile->classic == BT_CLASSIC_DIRECT_L2CAP);
#ifdef BTSTACK_USE_CYW43
                // CYW43: Use direct L2CAP for Sony controllers to skip SDP.
                // SDP responses from DS4/DS5 crash the CYW43 SPI bus.
                if (profile->default_vid == 0x054C) {
                    use_direct_l2cap = true;
                    printf("[BTSTACK_HOST] CYW43: forcing direct L2CAP for Sony (skip SDP)\n");
                }
#endif
                if (use_direct_l2cap) {
                    // Direct L2CAP: skip SDP, create HID channels after encryption
                    printf("[BTSTACK_HOST] %s detected, using direct L2CAP approach\n", profile->name);
                    classic_state.pending_hid_connect = true;

                    // Initialize direct L2CAP connection state
                    memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                    wiimote_conn.active = true;
                    wiimote_conn.state = WIIMOTE_STATE_IDLE;
                    memcpy(wiimote_conn.addr, addr, 6);
                    strncpy(wiimote_conn.name, name, sizeof(wiimote_conn.name) - 1);
                    wiimote_conn.class_of_device[0] = cod & 0xFF;
                    wiimote_conn.class_of_device[1] = (cod >> 8) & 0xFF;
                    wiimote_conn.class_of_device[2] = (cod >> 16) & 0xFF;
                    wiimote_conn.vendor_id = profile->default_vid;
                    wiimote_conn.product_id = profile->default_pid;

                    // Allocate classic connection slot for bthid routing
                    classic_connection_t* conn = find_free_classic_connection();
                    if (conn) {
                        int conn_index = conn - classic_state.connections;
                        memset(conn, 0, sizeof(*conn));
                        conn->active = true;
                        conn->hid_cid = 0xFFFF;  // Special marker for direct L2CAP
                        memcpy(conn->addr, addr, 6);
                        strncpy(conn->name, name, sizeof(conn->name) - 1);
                        conn->class_of_device[0] = cod & 0xFF;
                        conn->class_of_device[1] = (cod >> 8) & 0xFF;
                        conn->class_of_device[2] = (cod >> 16) & 0xFF;
                        conn->profile = profile;
                        conn->connect_time = btstack_run_loop_get_time_ms();
                        wiimote_conn.conn_index = conn_index;
                        printf("[BTSTACK_HOST] %s conn_index=%d\n", profile->name, conn_index);
                    }

                    // Create ACL connection directly (gap_connect will trigger HCI connection)
                    // We'll create L2CAP channels after encryption completes
                    printf("[BTSTACK_HOST] Creating ACL connection to %s...\n", profile->name);
                    uint8_t status = gap_connect(addr, BD_ADDR_TYPE_ACL);
                    if (status != ERROR_CODE_SUCCESS && status != ERROR_CODE_COMMAND_DISALLOWED) {
                        printf("[BTSTACK_HOST] gap_connect failed: 0x%02X\n", status);
                        wiimote_conn.active = false;
                        classic_state.pending_hid_connect = false;
                    }
                } else {
                    // Non-Wiimote: use normal hid_host_connect.
                    // Always REPORT. BTstack 1.8.2 removed the per-profile
                    // fallback-to-boot mode; see bt_device_db.h for why nothing
                    // here wanted it.
                    uint16_t hid_cid;
                    uint8_t status = hid_host_connect(addr, HID_PROTOCOL_MODE_REPORT, &hid_cid);
                    if (status == ERROR_CODE_SUCCESS) {
                        printf("[BTSTACK_HOST] hid_host_connect started, cid=0x%04X\n", hid_cid);

                        // Allocate connection slot
                        classic_connection_t* conn = find_free_classic_connection();
                        if (conn) {
                            memset(conn, 0, sizeof(*conn));
                            conn->active = true;
                            conn->hid_cid = hid_cid;
                            memcpy(conn->addr, addr, 6);
                            strncpy(conn->name, name, sizeof(conn->name) - 1);
                            conn->class_of_device[0] = cod & 0xFF;
                            conn->class_of_device[1] = (cod >> 8) & 0xFF;
                            conn->class_of_device[2] = (cod >> 16) & 0xFF;
                            conn->profile = profile;
                            conn->connect_time = btstack_run_loop_get_time_ms();
                        }
                    } else {
                        printf("[BTSTACK_HOST] hid_host_connect failed: %d\n", status);
                    }
                }
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            classic_state.inquiry_active = false;
            btlife_record(BTLIFE_INQUIRY_STOP, 0u, 0u);
            classic_state.recovery_start_time = 0;  // BT transport is working
#ifndef CONFIG_USB2BLE
            // Restart inquiry after it completes (if we're still in scan mode)
            // Toggle between GIAC and LIAC to discover all device types
            if (hid_state.state == BLE_STATE_SCANNING) {
                // Not back to back: see ns2_bt_inquiry_restart_delay_ms().
                uint32_t delay =
                    ns2_bt_inquiry_restart_delay_ms(hid_pairing_window_open);
                if (delay == 0u) {
                    classic_restart_inquiry();
                } else {
                    classic_state.inquiry_restart_at_ms =
                        btstack_run_loop_get_time_ms() + delay;
                }
            }
#endif
            break;

        // Classic BT incoming connection request (DS3 connects this way)
        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
            uint8_t link_type = hci_event_connection_request_get_link_type(packet);
            printf("[BTSTACK_HOST] Incoming connection: %02X:%02X:%02X:%02X:%02X:%02X COD=0x%06X link=%d\n",
                   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], (unsigned)cod, link_type);
            // The controller answered a page and completed the FHS exchange.
            // Its ABSENCE next to a host-side page timeout is what rules Case C
            // out; its presence would rule Cases A and B out.
            btlife_record_addr(BTLIFE_PAGE_RX, link_type, 0u, addr);

            // Same lifecycle gate as the registered HCI filter, for the case
            // where BTstack still forwards the request. See
            // ns2_bt_companion_classic_admission_allowed().
            if (!btstack_host_classic_admission_allowed(addr)) {
                classic_companion_refused_no_mgmt++;
                btstack_host_note_admission_reject(
                    addr, btstack_host_classic_has_trust(addr));
                btstack_host_note_link_refusal(addr, true);
        btlife_record_addr(BTLIFE_PAGE_REJECT, BTLIFE_REJECT_NO_MGMT, 0u, addr);
                printf("[BTSTACK_HOST] Refusing Controller Link request: no live "
                       "management session (refused=%lu)\n",
                       (unsigned long)classic_companion_refused_no_mgmt);
                break;
            }

            bool classic_trust_present = btstack_host_classic_has_trust(addr);
            ns2_bt_admission_t classic_admission = ns2_bt_admission_decide(
                pairing_lockout, hid_pairing_window_open,
                classic_trust_present);
            if (classic_admission == NS2_BT_ADMISSION_REJECT) {
                // The registered HCI filter rejects this before admission. Do
                // not create pending bookkeeping if BTstack still forwards it.
                btstack_host_note_admission_reject(addr, classic_trust_present);
                printf("[BTSTACK_HOST] Rejecting unbonded Classic connection outside pairing window\n");
                break;
            }

            // Save pending connection info for use when HID connection is established
            // Note: device name is not available yet at CONNECTION_REQUEST time.
            // Wiimote detection is deferred to CONNECTION_COMPLETE or later when
            // name resolution completes. Global master role policy (set at startup)
            // already ensures we become master for all connections.
            memcpy(classic_state.pending_addr, addr, 6);
            classic_state.pending_cod = cod;
            classic_state.pending_name[0] = '\0';  // Clear, will be filled by remote name request
            classic_state.pending_vid = 0;
            classic_state.pending_pid = 0;
            classic_state.pending_valid = true;
            classic_state.pending_outgoing = false;  // Device initiated this connection
            // A peer holding a live encrypted management session may also
            // commit a Classic key; without that it is admitted but never
            // converges, re-running SSP on every reconnect. Gated on the same
            // proof as admission -- an address match alone must never buy the
            // authority to write a stored key outside a pairing window.
            classic_pending_security_prepare(
                addr,
                (hid_pairing_window_open ||
                 btstack_host_classic_companion_session_trust(addr)) &&
                    !pairing_lockout);
            classic_state.waiting_for_incoming_time = 0;  // Device reconnected
            // BTstack will auto-accept with the current master_slave_policy
            break;
        }

        case HCI_EVENT_CONNECTION_COMPLETE: {
            uint8_t status = hci_event_connection_complete_get_status(packet);
            hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
            bd_addr_t addr;
            hci_event_connection_complete_get_bd_addr(packet, addr);
            printf("[BTSTACK_HOST] Connection complete: status=%d handle=0x%04X addr=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   status, handle, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            btlife_record_addr(status == 0 ? BTLIFE_ACL_UP : BTLIFE_ACL_FAIL,
                               status, handle, addr);

            if (classic_state.pending_valid &&
                bd_addr_cmp(addr, classic_state.pending_addr) == 0) {
                char reason[BTID_REASON_LEN];
                snprintf(reason, sizeof(reason), "classic-acl-status-0x%02X", status);
                classic_pair_diag(0xFF, classic_state.pending_name,
                                  classic_state.pending_cod,
                                  classic_state.pending_vid,
                                  classic_state.pending_pid, reason);
            }

            // Handle connection complete for both incoming and outgoing connections
            if (status == 0) {
                if (pairing_lockout) {
                    printf("[BTSTACK_HOST] Disconnecting late Classic connection after pairing wipe\n");
                    gap_disconnect(handle);
                    classic_state.pending_valid = false;
                    classic_pending_security_clear();
                    break;
                }
                if (classic_state.pending_valid &&
                    bd_addr_cmp(addr, classic_state.pending_addr) == 0) {
                    uint32_t cod = classic_state.pending_cod;

                    if (classic_state.pending_outgoing) {
                        // Outgoing connection (we initiated)
                        printf("[BTSTACK_HOST] Outgoing ACL complete, COD=0x%06X\n", cod);

                        // For Wiimotes, store ACL handle and do L2CAP-specific setup
                        if (classic_state.pending_hid_connect && wiimote_conn.active) {
                            wiimote_conn.acl_handle = handle;
                            printf("[BTSTACK_HOST] Wiimote: stored ACL handle=0x%04X\n", handle);

                            // Request remote name if we don't have it from inquiry
                            if (wiimote_conn.name[0] == '\0') {
                                gap_remote_name_request(addr, 0, 0);
                            }

                            // Query VID/PID once the shared SDP client is idle.
                            classic_identity_query_schedule(addr);
                        }

                        // Request early authentication for direct L2CAP connections
                        // (Wiimote/Wii U Pro) where we manage channels ourselves and
                        // need PIN exchange before L2CAP setup.
                        // For hid_host_connect() connections: BTstack's HID Host handles
                        // authentication when creating HID L2CAP channels after SDP.
                        // Requesting auth here concurrently with SDP causes CYW43 SPI
                        // bus failures on devices with large HID descriptors (DS4 clones).
                        if (classic_state.pending_hid_connect && wiimote_conn.active) {
                            gap_request_security_level(handle, LEVEL_2);
                        } else if (strcmp(classic_state.pending_name,
                                          "Xbox Wireless Controller") == 0) {
                            // Retro Fighters BattlerGC Pro Bluetooth XInput mode uses
                            // this exact Classic name and valid gamepad COD (0x002508),
                            // but stalls after a successful ACL open: no auth,
                            // encryption, SDP, or HID-open event follows. BlueRetro's
                            // successful trace for the same model explicitly starts
                            // SSP authentication at this boundary before SDP/HID.
                            //
                            // Keep this exception name+transport scoped. Broad early
                            // auth is unsafe here because the Sony/CYW43 SDP race above
                            // is already hardware-evidenced.
                            classic_pair_diag(0xFF, classic_state.pending_name,
                                              classic_state.pending_cod,
                                              classic_state.pending_vid,
                                              classic_state.pending_pid,
                                              "classic-early-auth-request");
                            gap_request_security_level(handle, LEVEL_2);
                        }
                    } else {
                        // Incoming connection (device connected to us)
                        printf("[BTSTACK_HOST] Incoming ACL complete, COD=0x%06X\n", cod);
                        classic_state.pending_acl_handle = handle;

                        // Detect direct L2CAP device by pending profile (if available from prior inquiry).
                        // For incoming reconnections, pending_name is typically empty at
                        // this point — detection is deferred to HID_SUBEVENT_CONNECTION_OPENED
                        // or REMOTE_NAME_REQUEST_COMPLETE when the name becomes available.
                        const bt_device_profile_t* incoming_profile = classic_state.pending_profile;
                        if (!incoming_profile && classic_state.pending_name[0]) {
                            incoming_profile = bt_device_lookup_by_name(classic_state.pending_name);
                        }
                        bool is_direct_l2cap = (incoming_profile &&
                                                incoming_profile->classic == BT_CLASSIC_DIRECT_L2CAP);

                        if (is_direct_l2cap) {
                            // Wiimote/Wii U Pro reconnection - check role and link key
                            printf("[BTSTACK_HOST] %s detected (incoming reconnection)\n",
                                   incoming_profile->name);

                            // Wiimotes require master role - check and request if needed
                            hci_role_t current_role = gap_get_role(handle);
                            printf("[BTSTACK_HOST] Wiimote: role=%s\n",
                                   current_role == HCI_ROLE_MASTER ? "MASTER" :
                                   current_role == HCI_ROLE_SLAVE ? "SLAVE" : "UNKNOWN");
                            if (current_role != HCI_ROLE_MASTER) {
                                printf("[BTSTACK_HOST] Wiimote: requesting master role switch\n");
                                gap_request_role(addr, HCI_ROLE_MASTER);
                            }

                            // Check if we have a stored link key
                            link_key_t link_key;
                            link_key_type_t key_type;
                            bool have_key = gap_get_link_key_for_bd_addr(addr, link_key, &key_type);
                            printf("[BTSTACK_HOST] Wiimote: have_key=%d type=%d\n", have_key, have_key ? key_type : -1);

                            // Store info for when L2CAP events come in
                            memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                            wiimote_conn.active = true;
                            wiimote_conn.state = WIIMOTE_STATE_IDLE;
                            wiimote_conn.conn_index = -1;  // Not assigned yet
                            memcpy(wiimote_conn.addr, addr, 6);
                            wiimote_conn.acl_handle = handle;
                            memcpy(wiimote_conn.class_of_device, &cod, 3);
                            if (classic_state.pending_name[0]) {
                                strncpy(wiimote_conn.name, classic_state.pending_name, sizeof(wiimote_conn.name) - 1);
                            }

                            // Request remote name for driver matching (need to distinguish Wii U Pro from Wiimote)
                            gap_remote_name_request(addr, 0, 0);

                            // For incoming connections (reconnection), let HID Host handle L2CAP
                            // Don't create outgoing L2CAP - it can conflict with HID Host
                            if (have_key) {
                                printf("[BTSTACK_HOST] Wiimote: handle=0x%04X, have key, waiting for HID Host\n", handle);
                                // Stop scanning now - we have an incoming connection
                                btstack_host_stop_scan();
                                // HID Host will receive HID_SUBEVENT_INCOMING_CONNECTION
                                // and we'll accept it there
                            } else {
                                // No key - this is a new pairing, wait for device to initiate
                                printf("[BTSTACK_HOST] Wiimote: handle=0x%04X, no key, waiting for pairing\n", handle);
                            }
                        }

                        if (!is_direct_l2cap) {
                            // Standard incoming connection flow (DS3, DS4, DS5, or unknown device).
                            // If this is actually a Wiimote reconnection where the name wasn't
                            // available yet, it will be detected later when the name resolves
                            // (see REMOTE_NAME_REQUEST_COMPLETE and HID_SUBEVENT_CONNECTION_OPENED).

                            // Request remote name for driver matching (we don't have it from inquiry)
                            gap_remote_name_request(addr, 0, 0);

                            // Don't query VID/PID via SDP here — BTstack HID Host runs its
                            // own SDP query after accepting the incoming connection, and the
                            // SDP client only handles one query at a time. Our VID/PID query
                            // would delay HID Host's descriptor query. Instead, query VID/PID
                            // at HID_SUBEVENT_CONNECTION_OPENED after HID channels are established.

                            // Request authentication only if we have a stored key (reconnection).
                            // For new pairings (no key), defer auth to after name resolution
                            // to avoid concurrent SDP+auth on CYW43 and to let device type
                            // detection (Switch vs Sony) determine the connection path.
                            link_key_t incoming_link_key;
                            link_key_type_t incoming_key_type;
                            bool have_incoming_key = gap_get_link_key_for_bd_addr(
                                addr, incoming_link_key, &incoming_key_type);
                            // Stand down from STARTING authentication for the
                            // trusted live companion: Android's HID Device
                            // profile is guaranteed to start it, and both hosts
                            // running the same LMP procedure is what produced
                            // the captured 0x23 collisions. This changes who
                            // initiates, not what is required -- the HID-ready
                            // gate below still demands authenticated, encrypted
                            // transport at the required key size.
                            bool peer_is_companion =
                                btstack_host_classic_companion_session_trust(addr);
                            if (peer_is_companion) {
                                // Admission already proved the management
                                // session is live; bind the link to it so its
                                // loss can tear exactly this link down.
                                classic_companion_note_link(handle);
                            }
                            bool defer_auth = ns2_bt_defer_classic_authentication(
                                peer_is_companion,
                                have_incoming_key,
                                classic_fresh_pairing_security_handle == handle);
                            auth_decision_begin(handle);
                            last_auth_decision.auth_deferred = defer_auth;
                            last_auth_decision.auth_had_stored_key = have_incoming_key;
                            if (defer_auth) {
                                classic_authentication_deferrals++;
                                btlife_record_addr(BTLIFE_AUTH_DEFER, 1u, handle, addr);
                            } else if (have_incoming_key) {
                                gap_request_security_level(handle, LEVEL_2);
                                btlife_record_addr(BTLIFE_AUTH_START, 0u, handle, addr);
                            }
                        }
                    }
                }
            }
            break;
        }

        case L2CAP_EVENT_INCOMING_CONNECTION: {
            uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
            uint16_t cid = l2cap_event_incoming_connection_get_local_cid(packet);
            hci_con_handle_t handle = l2cap_event_incoming_connection_get_handle(packet);
            bd_addr_t addr;
            l2cap_event_incoming_connection_get_address(packet, addr);
            printf("[BTSTACK_HOST] L2CAP incoming: PSM=0x%04X cid=0x%04X handle=0x%04X\n", psm, cid, handle);

            // For Wiimotes during reconnection, we create outgoing L2CAP channels ourselves.
            // If the Wiimote also tries to create incoming channels, decline them at L2CAP level
            // to force the Wiimote to use our outgoing channels.
            if (wiimote_conn.active && wiimote_conn.acl_handle == handle &&
                (psm == PSM_HID_CONTROL || psm == PSM_HID_INTERRUPT)) {
                // If we're already creating outgoing channels (reconnection), decline incoming
                if (wiimote_conn.state >= WIIMOTE_STATE_W4_CONTROL_CONNECTED) {
                    printf("[BTSTACK_HOST] Wiimote: declining incoming L2CAP PSM=0x%04X (using outgoing channels)\n", psm);
                    l2cap_decline_connection(cid);
                    break;
                }
                // Fresh pairing or reconnection via HID Host - capture CID for direct L2CAP sending
                // HID Host will accept, but we need the CID to bypass hid_host_send_report
                printf("[BTSTACK_HOST] Wiimote: L2CAP incoming PSM=0x%04X cid=0x%04X (HID Host will accept)\n", psm, cid);
                if (psm == PSM_HID_CONTROL) {
                    wiimote_conn.control_cid = cid;
                    wiimote_conn.state = WIIMOTE_STATE_W4_CONTROL_CONNECTED;
                    printf("[BTSTACK_HOST] Wiimote: captured control CID=0x%04X from incoming\n", cid);
                } else {
                    wiimote_conn.interrupt_cid = cid;
                    wiimote_conn.state = WIIMOTE_STATE_W4_INTERRUPT_CONNECTED;
                    printf("[BTSTACK_HOST] Wiimote: captured interrupt CID=0x%04X from incoming\n", cid);
                }
            }
            break;
        }

        case L2CAP_EVENT_CHANNEL_OPENED: {
            uint8_t status = l2cap_event_channel_opened_get_status(packet);
            uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
            uint16_t cid = l2cap_event_channel_opened_get_local_cid(packet);
            bd_addr_t l2cap_addr;
            l2cap_event_channel_opened_get_address(packet, l2cap_addr);
            printf("[BTSTACK_HOST] L2CAP opened: status=%d PSM=0x%04X cid=0x%04X addr=%s\n",
                   status, psm, cid, bd_addr_to_str(l2cap_addr));

            // Capture L2CAP CIDs for Wiimote connections (for direct L2CAP sending)
            // HID Host handles receiving, but we need direct L2CAP CIDs for sending
            // Note: bt_on_hid_ready is called from HID_SUBEVENT_CONNECTION_OPENED
            if (status == 0 && wiimote_conn.active &&
                memcmp(l2cap_addr, wiimote_conn.addr, 6) == 0) {
                if (psm == PSM_HID_CONTROL) {
                    wiimote_conn.control_cid = cid;
                    printf("[BTSTACK_HOST] Wiimote: captured control CID=0x%04X for direct sending\n", cid);
                } else if (psm == PSM_HID_INTERRUPT) {
                    wiimote_conn.interrupt_cid = cid;
                    printf("[BTSTACK_HOST] Wiimote: captured interrupt CID=0x%04X for direct sending\n", cid);
                }
            }
            break;
        }

        case HCI_EVENT_LE_META: {
#ifdef CONFIG_USB2BLE
            break;  // USB2BLE is a BLE peripheral — ble_output handles LE events
#endif
            uint8_t subevent = hci_event_le_meta_get_subevent_code(packet);

            switch (subevent) {
                case HCI_SUBEVENT_LE_CONNECTION_COMPLETE: {
                    hci_con_handle_t handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                    uint8_t status = hci_subevent_le_connection_complete_get_status(packet);
                    uint8_t role = hci_subevent_le_connection_complete_get_role(packet);

                    // Controller links are initiated by this host, so the Pico
                    // is LE Central/Master. A browser connects in the opposite
                    // direction while Config advertising is active. Classify
                    // that peripheral-role ACL before it can consume a HID
                    // controller slot or enter SM/GATT-client setup.
                    if (status == ERROR_CODE_SUCCESS && role == HCI_ROLE_SLAVE) {
                        bd_addr_t peripheral_peer;
                        hci_subevent_le_connection_complete_get_peer_address(
                            packet, peripheral_peer);
                        if (!config_ble_accept_connection(handle, peripheral_peer)) {
                            printf("[BTSTACK_HOST] Rejecting unexpected LE peripheral-role connection "
                                   "outside Config service\n");
                            gap_disconnect(handle);
                        }
                        break;
                    }

                    bool target_attempt = hid_state.has_last_connected &&
                        memcmp(hid_state.pending_addr, hid_state.last_connected_addr,
                               sizeof(bd_addr_t)) == 0;
                    if (target_attempt) {
                        hid_state.last_target_connect_status = status;
                        if (status == ERROR_CODE_SUCCESS) {
                            hid_state.target_connect_successes++;
                        } else {
                            hid_state.target_connect_failures++;
                        }
                    }

                    if (status != 0) {
                        printf("[BTSTACK_HOST] Connection failed: 0x%02X\n", status);
                        hid_state.reconnect_attempt_time = 0;

                        // Was this failing attempt a candidate whose pairing window had
                        // already expired (btstack_host_close_pairing_window() deferred
                        // the close because this attempt was in flight)? If so, and it
                        // wasn't a reconnect to an already-bonded device, the window's
                        // deadline has genuinely passed -- close cleanly instead of
                        // falling into the generic "resume scanning" retry below, which
                        // would otherwise silently re-open discovery past the deadline.
                        // Bonded reconnects are explicitly exempted so their own
                        // has_last_connected retry logic is untouched either way.
                        bool pairing_window_expired = pairing_close_deferred;
                        bool is_bonded_reconnect = hid_state.has_last_connected &&
                            memcmp(hid_state.pending_addr, hid_state.last_connected_addr, 6) == 0;
                        resolve_deferred_pairing_close();
                        if (pairing_window_expired && !is_bonded_reconnect) {
                            printf("[BTSTACK_HOST] Pairing candidate failed after window expiry -- closing cleanly, not resuming scan\n");
                            hid_state.state = BLE_STATE_IDLE;
                            break;
                        }

                        // If scan is already running (e.g. safety net started it after
                        // gap_connect_cancel timeout), restore scanning state so the
                        // advertising handler can auto-connect to devices
                        if (hid_state.scan_active) {
                            hid_state.state = BLE_STATE_SCANNING;
                            printf("[BTSTACK_HOST] Scan already active, resuming scan state\n");
                            break;
                        }

                        hid_state.state = BLE_STATE_IDLE;

                        // If reconnection attempt failed, try again or resume
                        // scanning. Re-select rather than blindly retrying the
                        // same slot: the peer may have come back on its own
                        // during the attempt, in which case it must not be
                        // targeted again (that is what stopped the scan
                        // repeatedly and stranded the other peer).
                        ns2_ble_reconnect_decision_t retry = btstack_host_pick_reconnect();
                        if (retry.action == NS2_BLE_RECONNECT_DIRECT &&
                            !switch2_force_fresh_custom_pairing) {
                            hid_state.reconnect_attempts++;
                            printf("[BTSTACK_HOST] Retrying reconnection (attempt %d)...\n",
                                   hid_state.reconnect_attempts);
                            // Carry the stored name so conn->name is populated on
                            // reconnect — the MouthPad NUS relay arms on a name
                            // match, and an empty name leaves it stuck "scanning".
                            strncpy(hid_state.pending_name, hid_state.last_connected_name,
                                    sizeof(hid_state.pending_name) - 1);
                            hid_state.pending_name[sizeof(hid_state.pending_name) - 1] = '\0';
                            btstack_host_connect_ble(retry.addr, retry.addr_type);
                        } else {
                            printf("[BTSTACK_HOST] Reconnection failed after %d attempts, resuming scan\n",
                                   hid_state.reconnect_attempts);
                            btstack_host_start_scan();
                        }
                        break;
                    }

                    printf("[BTSTACK_HOST] Connected! handle=0x%04X\n", handle);
                    hid_state.reconnect_attempt_time = 0;

                    if (pairing_lockout) {
                        // Covers a connection-complete event already queued when the wipe
                        // occurred. No GATT, SM, or Switch 2 custom handshake may proceed.
                        printf("[BTSTACK_HOST] Disconnecting late BLE connection after pairing wipe\n");
                        gap_disconnect(handle);
                        hid_state.reconnect_attempt_time = 0;
                        hid_state.state = BLE_STATE_IDLE;
                        resolve_deferred_pairing_close();
                        break;
                    }

                    // Find or create connection entry
                    ble_connection_t *conn = find_free_connection();
                    if (conn) {
                        memcpy(conn->addr, hid_state.pending_addr, 6);
                        conn->addr_type = hid_state.pending_addr_type;
                        conn->handle = handle;
                        conn->state = BLE_STATE_CONNECTED;
                        // Copy the name from pending connection
                        strncpy(conn->name, hid_state.pending_name, sizeof(conn->name) - 1);
                        conn->name[sizeof(conn->name) - 1] = '\0';
                        conn->profile = hid_state.pending_profile;
                        conn->vid = hid_state.pending_vid;
                        conn->pid = hid_state.pending_pid;
                        conn->fresh_pairing_admitted =
                            hid_state.pending_fresh_pairing_admitted;
                        conn->rpa_trust_candidate =
                            hid_state.pending_rpa_trust_candidate;

                        printf("[BTSTACK_HOST] Connection stored: name='%s' profile=%s vid=0x%04X pid=0x%04X\n",
                               conn->name, conn->profile ? conn->profile->name : "default",
                               conn->vid, conn->pid);

                        // A saved Switch 2 target must first re-establish its
                        // encrypted BLE link with the LTK derived during the
                        // custom 0x15 exchange. SYNC-mode first pairing still
                        // uses direct ATT because no link key exists yet.
                        if (conn->profile && conn->profile->ble == BT_BLE_CUSTOM) {
                            bool encrypted_reconnect = target_attempt &&
                                hid_state.has_last_connected_ltk &&
                                !switch2_force_fresh_custom_pairing;
                            ns2_bt_custom_admission_t custom_admission =
                                ns2_bt_custom_admission_decide(
                                    pairing_lockout, encrypted_reconnect,
                                    conn->fresh_pairing_admitted,
                                    conn->rpa_trust_candidate);
                            if (custom_admission ==
                                NS2_BT_CUSTOM_ENCRYPTED_RECONNECT) {
                                printf("[SW2_BLE] Saved target connected; requesting bonded SM re-encryption\n");
                                if (!btstack_host_install_switch2_ltk()) {
                                    gap_disconnect(handle);
                                } else {
                                    sm_request_pairing(handle);
                                }
                            } else if (custom_admission ==
                                       NS2_BT_CUSTOM_VERIFY_RECONNECT) {
                                printf("[SW2_BLE] RPA candidate connected; requiring SM identity reuse\n");
                                sm_request_pairing(handle);
                            } else if (custom_admission == NS2_BT_CUSTOM_FRESH) {
                                printf("[BTSTACK_HOST] %s: fresh custom pairing via direct ATT setup\n",
                                       conn->profile->name);
                                register_switch2_hid_listener(handle);
                            } else {
                                printf("[SW2_BLE] Custom ATT admission missing; disconnecting\n");
                                gap_disconnect(handle);
                            }
                        } else {
                            // Request pairing (SM will handle Secure Connections)
                            printf("[BTSTACK_HOST] Requesting pairing...\n");
                            sm_request_pairing(handle);
                        }
                    }

                    hid_state.state = BLE_STATE_CONNECTED;
                    // Raw connect succeeded -- from here, GATT discovery, SM pairing,
                    // HID setup, and Switch 2 GATT init all key off per-connection state
                    // (conn->state / conn->hid_ready), not hid_state.state, so it's now
                    // safe to close a pairing window that was waiting on this attempt.
                    resolve_deferred_pairing_close();
                    break;
                }

                case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE: {
                    hci_con_handle_t handle =
                        hci_subevent_le_connection_update_complete_get_connection_handle(packet);
                    uint8_t status =
                        hci_subevent_le_connection_update_complete_get_status(packet);
                    uint16_t interval =
                        hci_subevent_le_connection_update_complete_get_conn_interval(packet);
                    uint16_t latency =
                        hci_subevent_le_connection_update_complete_get_conn_latency(packet);
                    uint16_t timeout =
                        hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);
                    printf("[BTSTACK_HOST] LE params: handle=0x%04X status=0x%02X "
                           "interval=%u.%02ums latency=%u timeout=%ums\n",
                           handle, status, interval * 125u / 100u,
                           25u * (interval & 3u), latency, timeout * 10u);
                    switch2_capture_link_params(SW2_LINK_PHASE_COMPLETE, status, handle,
                                                interval, latency, timeout);
                    break;
                }

            }
            break;
        }

        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
            bd_addr_t name_addr;
            hci_event_remote_name_request_complete_get_bd_addr(packet, name_addr);
            uint8_t name_status = hci_event_remote_name_request_complete_get_status(packet);

            if (name_status != 0) {
                printf("[BTSTACK_HOST] Remote name request failed: status=%d\n", name_status);

                // If we deferred a connection waiting for the name, fall back to
                // standard HID Host connect. This handles DS4, DS3, and other
                // controllers that may not respond to name requests.
                if (classic_state.pending_valid &&
                    classic_state.pending_outgoing &&
                    classic_state.pending_hid_connect &&
                    memcmp(name_addr, classic_state.pending_addr, 6) == 0) {
                    printf("[BTSTACK_HOST] Deferred connect: name failed, falling back\n");

#ifdef BTSTACK_USE_CYW43
                    // CYW43: if pending profile is Sony, use direct L2CAP to skip SDP
                    if (classic_state.pending_profile && classic_state.pending_profile->default_vid == 0x054C) {
                        printf("[BTSTACK_HOST] CYW43: forcing direct L2CAP for Sony (skip SDP)\n");
                        memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                        wiimote_conn.active = true;
                        wiimote_conn.state = WIIMOTE_STATE_IDLE;
                        memcpy(wiimote_conn.addr, name_addr, 6);
                        wiimote_conn.class_of_device[0] = classic_state.pending_cod & 0xFF;
                        wiimote_conn.class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                        wiimote_conn.class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                        wiimote_conn.vendor_id = classic_state.pending_profile->default_vid;
                        wiimote_conn.product_id = classic_state.pending_profile->default_pid;

                        classic_connection_t* conn = find_free_classic_connection();
                        if (conn) {
                            int conn_index = conn - classic_state.connections;
                            memset(conn, 0, sizeof(*conn));
                            conn->active = true;
                            conn->hid_cid = 0xFFFF;
                            memcpy(conn->addr, name_addr, 6);
                            conn->class_of_device[0] = classic_state.pending_cod & 0xFF;
                            conn->class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                            conn->class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                            conn->profile = classic_state.pending_profile;
                            conn->connect_time = btstack_run_loop_get_time_ms();
                            wiimote_conn.conn_index = conn_index;
                        }

                        uint8_t status = gap_connect(name_addr, BD_ADDR_TYPE_ACL);
                        if (status != ERROR_CODE_SUCCESS && status != ERROR_CODE_COMMAND_DISALLOWED) {
                            printf("[BTSTACK_HOST] gap_connect failed: 0x%02X\n", status);
                            wiimote_conn.active = false;
                        }
                        classic_state.pending_hid_connect = false;
                        break;
                    }
#endif
                    classic_state.pending_hid_connect = false;

                    uint16_t hid_cid;
                    uint8_t status = hid_host_connect(name_addr, HID_PROTOCOL_MODE_REPORT, &hid_cid);
                    if (status == ERROR_CODE_SUCCESS) {
                        printf("[BTSTACK_HOST] hid_host_connect started, cid=0x%04X\n", hid_cid);
                        classic_connection_t* conn = find_free_classic_connection();
                        if (conn) {
                            memset(conn, 0, sizeof(*conn));
                            conn->active = true;
                            conn->hid_cid = hid_cid;
                            memcpy(conn->addr, name_addr, 6);
                            conn->class_of_device[0] = classic_state.pending_cod & 0xFF;
                            conn->class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                            conn->class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                            conn->connect_time = btstack_run_loop_get_time_ms();
                        }
                    } else {
                        printf("[BTSTACK_HOST] hid_host_connect failed: %d\n", status);
                    }
                }
                break;
            }

            {
                const char* name = hci_event_remote_name_request_complete_get_remote_name(packet);
                printf("[BTSTACK_HOST] Remote name: %s\n", name);

                // Store name if this is our pending incoming connection
                if (classic_state.pending_valid &&
                    memcmp(name_addr, classic_state.pending_addr, 6) == 0) {
                    strncpy(classic_state.pending_name, name, sizeof(classic_state.pending_name) - 1);
                    classic_state.pending_name[sizeof(classic_state.pending_name) - 1] = '\0';
                }

                // Also update any active connection with this address
                for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
                    classic_connection_t* conn = &classic_state.connections[i];
                    if (conn->active && memcmp(conn->addr, name_addr, 6) == 0) {
                        if (conn->name[0] == '\0') {
                            strncpy(conn->name, name, sizeof(conn->name) - 1);
                            conn->name[sizeof(conn->name) - 1] = '\0';
                            printf("[BTSTACK_HOST] Updated conn[%d] name: %s\n", i, conn->name);

                            // Update profile from name if not already set
                            if (!conn->profile || conn->profile == &BT_PROFILE_DEFAULT) {
                                conn->profile = bt_device_lookup_by_name(name);
                            }

                            if (conn->hid_ready) {
                                // Set VID/PID from profile defaults (Wiimote-family lacks PnP SDP)
                                const bt_device_profile_t* name_profile = bt_device_lookup_by_name(name);
                                if (name_profile->default_vid) {
                                    conn->vendor_id = name_profile->default_vid;
                                    uint16_t pid = bt_device_wiimote_pid_from_name(name);
                                    if (pid) {
                                        conn->product_id = pid;
                                        printf("[BTSTACK_HOST] Late %s detection, PID=0x%04X\n",
                                               name_profile->name, pid);
                                    }
                                }
                                // Notify BTHID of late name arrival — allows driver
                                // re-evaluation for devices matched as generic because
                                // name wasn't available at connection time
                                bthid_update_device_info(i, conn->name,
                                                         conn->vendor_id, conn->product_id);
                            }
                        }
                        break;
                    }
                }

                // Also update wiimote_conn if active
                if (wiimote_conn.active && memcmp(wiimote_conn.addr, name_addr, 6) == 0) {
                    if (wiimote_conn.name[0] == '\0') {
                        strncpy(wiimote_conn.name, name, sizeof(wiimote_conn.name) - 1);
                        wiimote_conn.name[sizeof(wiimote_conn.name) - 1] = '\0';
                        printf("[BTSTACK_HOST] Updated wiimote name: %s\n", wiimote_conn.name);
                    }
                }

                // Late direct-L2CAP device detection for incoming reconnections: if the name
                // resolves to a direct-L2CAP device and wiimote_conn wasn't set up at
                // CONNECTION_COMPLETE (because name was unknown), set it up now so
                // ENCRYPTION_CHANGE can create outgoing L2CAP channels.
                const bt_device_profile_t* late_profile = bt_device_lookup_by_name(name);
                bool late_direct_l2cap = (late_profile->classic == BT_CLASSIC_DIRECT_L2CAP);

#ifdef BTSTACK_USE_CYW43
                // On CYW43, Sony incoming reconnections use HID Host (not direct L2CAP).
                // The controller initiates its own L2CAP channels; we just need to stop
                // scanning and set default VID so the connection slot gets Sony VID.
                // (Direct L2CAP is only used for outgoing initial pairing to skip SDP.)
                if (late_profile->default_vid == 0x054C &&
                    classic_state.pending_valid &&
                    !classic_state.pending_outgoing &&
                    memcmp(name_addr, classic_state.pending_addr, 6) == 0) {
                    printf("[BTSTACK_HOST] Late Sony detection (incoming) - using HID Host path\n");
                    if (classic_state.pending_vid == 0) {
                        classic_state.pending_vid = late_profile->default_vid;
                    }
                    btstack_host_stop_scan();
                }
#endif
                if (!wiimote_conn.active &&
                    late_direct_l2cap &&
                    classic_state.pending_valid &&
                    !classic_state.pending_outgoing &&
                    memcmp(name_addr, classic_state.pending_addr, 6) == 0) {
                    printf("[BTSTACK_HOST] Late %s detection from name resolution (incoming reconnection)\n",
                           late_profile->name);
                    memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                    wiimote_conn.active = true;
                    wiimote_conn.state = WIIMOTE_STATE_IDLE;
                    wiimote_conn.conn_index = -1;
                    memcpy(wiimote_conn.addr, name_addr, 6);
                    wiimote_conn.acl_handle = classic_state.pending_acl_handle;
                    memcpy(wiimote_conn.class_of_device, &classic_state.pending_cod, 3);
                    strncpy(wiimote_conn.name, name, sizeof(wiimote_conn.name) - 1);
                    wiimote_conn.name[sizeof(wiimote_conn.name) - 1] = '\0';
                    wiimote_conn.vendor_id = late_profile->default_vid;
                    wiimote_conn.product_id = classic_state.pending_pid ? classic_state.pending_pid : late_profile->default_pid;

                    // Stop scanning — we have an incoming connection to handle
                    btstack_host_stop_scan();

                    // Request auth if no stored key (first-time pairing).
                    // For reconnections, HCI auto-encrypts with the stored key.
                    link_key_t late_link_key;
                    link_key_type_t late_key_type;
                    if (!gap_get_link_key_for_bd_addr(name_addr, late_link_key, &late_key_type)) {
                        printf("[BTSTACK_HOST] No stored key, requesting auth for SSP pairing\n");
                        // Remember that WE drove security on this link: see
                        // ns2_bt_defer_classic_encryption().
                        classic_fresh_pairing_security_handle =
                            classic_state.pending_acl_handle;
                        gap_request_security_level(classic_state.pending_acl_handle, LEVEL_2);
                    }
                }

                // Deferred outgoing connection: name was unavailable at inquiry time,
                // so we requested it before connecting. Now that the name has resolved,
                // connect using the appropriate path (direct L2CAP vs HID Host).
                if (classic_state.pending_valid &&
                    classic_state.pending_outgoing &&
                    classic_state.pending_hid_connect &&
                    memcmp(name_addr, classic_state.pending_addr, 6) == 0) {
                    // Update pending name and re-lookup profile
                    strncpy(classic_state.pending_name, name, sizeof(classic_state.pending_name) - 1);
                    classic_state.pending_name[sizeof(classic_state.pending_name) - 1] = '\0';
                    classic_state.pending_hid_connect = false;

                    const bt_device_profile_t* deferred_profile = bt_device_lookup_by_name(name);
                    classic_state.pending_profile = deferred_profile;

                    bool deferred_direct_l2cap = (deferred_profile->classic == BT_CLASSIC_DIRECT_L2CAP);
#ifdef BTSTACK_USE_CYW43
                    if (deferred_profile->default_vid == 0x054C) {
                        deferred_direct_l2cap = true;
                        printf("[BTSTACK_HOST] CYW43: forcing direct L2CAP for Sony (skip SDP)\n");
                    }
#endif
                    if (deferred_direct_l2cap) {
                        printf("[BTSTACK_HOST] Deferred connect: %s detected, using direct L2CAP\n",
                               deferred_profile->name);
                        classic_state.pending_hid_connect = true;

                        memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                        wiimote_conn.active = true;
                        wiimote_conn.state = WIIMOTE_STATE_IDLE;
                        memcpy(wiimote_conn.addr, name_addr, 6);
                        strncpy(wiimote_conn.name, name, sizeof(wiimote_conn.name) - 1);
                        wiimote_conn.class_of_device[0] = classic_state.pending_cod & 0xFF;
                        wiimote_conn.class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                        wiimote_conn.class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                        wiimote_conn.vendor_id = deferred_profile->default_vid;
                        wiimote_conn.product_id = deferred_profile->default_pid;

                        classic_connection_t* conn = find_free_classic_connection();
                        if (conn) {
                            int conn_index = conn - classic_state.connections;
                            memset(conn, 0, sizeof(*conn));
                            conn->active = true;
                            conn->hid_cid = 0xFFFF;
                            memcpy(conn->addr, name_addr, 6);
                            strncpy(conn->name, name, sizeof(conn->name) - 1);
                            conn->class_of_device[0] = classic_state.pending_cod & 0xFF;
                            conn->class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                            conn->class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                            conn->profile = deferred_profile;
                            conn->connect_time = btstack_run_loop_get_time_ms();
                            wiimote_conn.conn_index = conn_index;
                        }

                        uint8_t status = gap_connect(name_addr, BD_ADDR_TYPE_ACL);
                        if (status != ERROR_CODE_SUCCESS && status != ERROR_CODE_COMMAND_DISALLOWED) {
                            printf("[BTSTACK_HOST] gap_connect failed: 0x%02X\n", status);
                            wiimote_conn.active = false;
                            classic_state.pending_hid_connect = false;
                        }
                    } else {
                        printf("[BTSTACK_HOST] Deferred connect: %s, using HID Host\n",
                               deferred_profile->name);
                        // Always REPORT -- see the sibling call site above and
                        // the retired-field note in bt_device_db.h.
                        uint16_t hid_cid;
                        uint8_t status = hid_host_connect(name_addr, HID_PROTOCOL_MODE_REPORT, &hid_cid);
                        if (status == ERROR_CODE_SUCCESS) {
                            printf("[BTSTACK_HOST] hid_host_connect started, cid=0x%04X\n", hid_cid);
                            classic_connection_t* conn = find_free_classic_connection();
                            if (conn) {
                                memset(conn, 0, sizeof(*conn));
                                conn->active = true;
                                conn->hid_cid = hid_cid;
                                memcpy(conn->addr, name_addr, 6);
                                strncpy(conn->name, name, sizeof(conn->name) - 1);
                                conn->class_of_device[0] = classic_state.pending_cod & 0xFF;
                                conn->class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                                conn->class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                                conn->profile = deferred_profile;
                                conn->connect_time = btstack_run_loop_get_time_ms();
                            }
                        } else {
                            printf("[BTSTACK_HOST] hid_host_connect failed: %d\n", status);
                        }
                    }
                }
            }
            break;
        }

        case HCI_EVENT_COMMAND_STATUS: {
            uint16_t opcode = hci_event_command_status_get_command_opcode(packet);
            if (opcode == HCI_OPCODE_HCI_LE_START_ENCRYPTION) {
                switch2_direct_cmd_status_events++;
                switch2_last_cmd_status_opcode = opcode;
                switch2_last_cmd_status = hci_event_command_status_get_status(packet);
                printf("[SW2_BLE] HCI Start Encryption command status=0x%02X\n",
                       switch2_last_cmd_status);
            }
            break;
        }

        case HCI_EVENT_COMMAND_COMPLETE: {
            uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            if (opcode == HCI_OPCODE_HCI_READ_RSSI && size >= 9u &&
                bt_health_probe_handle != HCI_CON_HANDLE_INVALID) {
                const uint8_t *params =
                    hci_event_command_complete_get_return_parameters(packet);
                hci_con_handle_t const handle = little_endian_read_16(params, 1u);
                if (handle == bt_health_probe_handle) {
                    ns2_bt_health_note_probe_complete(
                        &bt_health, btstack_run_loop_get_time_ms(), params[0]);
                    printf("[BT_HEALTH] RSSI probe complete handle=0x%04X status=0x%02X\n",
                           handle, params[0]);
                    bt_health_probe_handle = HCI_CON_HANDLE_INVALID;
                }
            }
            if (opcode == HCI_OPCODE_HCI_LE_START_ENCRYPTION) {
                switch2_direct_cmd_complete_events++;
                switch2_last_cmd_complete_opcode = opcode;
                const uint8_t *params =
                    hci_event_command_complete_get_return_parameters(packet);
                switch2_last_cmd_complete_status = size > 5 ? params[0] : 0xFF;
                printf("[SW2_BLE] HCI Start Encryption command complete status=0x%02X\n",
                       switch2_last_cmd_complete_status);
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            hci_con_handle_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);

            if (config_ble_handle_disconnect(handle, reason)) {
                break;
            }

            // Non-management (controller/other) ACL drop. Recorded with its HCI
            // reason so the UART trace shows which link died first and why.
            btlife_record(BTLIFE_HCI_DISCONNECT, reason, handle);

            // Handles are reused, so per-handle Classic security state must not
            // survive the connection that created it. Both of these are
            // fail-safe if missed (we simply do not defer next time), but a
            // stale match would silently change behaviour on an unrelated link.
            if (classic_fresh_pairing_security_handle == handle) {
                classic_fresh_pairing_security_handle = HCI_CON_HANDLE_INVALID;
            }
            if (classic_encryption_deferred_handle == handle) {
                classic_encryption_deferred_handle = HCI_CON_HANDLE_INVALID;
            }
            if (classic_companion_acl_handle == handle) {
                classic_companion_acl_handle = HCI_CON_HANDLE_INVALID;
            }
            // Keep the record readable for post-mortem, but retire it so a
            // reused handle cannot have its encryption or HID readiness
            // attributed to this dead link.
            if (last_auth_decision.valid && last_auth_decision.handle == handle) {
                last_auth_decision.link_closed = true;
            }
            printf("[BTSTACK_HOST] Disconnected: handle=0x%04X reason=0x%02X\n", handle, reason);

            ble_connection_t *conn = find_connection_by_handle(handle);
            if ((switch2_direct_reencrypt_active &&
                 switch2_direct_reencrypt_handle == handle) ||
                (conn && conn->profile && conn->profile->ble == BT_BLE_CUSTOM)) {
                switch2_disconnect_events++;
                switch2_last_disconnect_reason = reason;
            }
            if (conn) {
                uint8_t disconnected_conn_index = 0xFFu;
                uint32_t disconnected_generation = 0u;
                // Capture the departing peer's identity before the slot is torn
                // down. The stale-bond deletion below must act on THIS peer --
                // with two bonded peers, keying it off last_connected could
                // delete the bond of the peer that is still connected and
                // working.
                bd_addr_t disconnected_addr;
                uint8_t disconnected_addr_type = conn->addr_type;
                memcpy(disconnected_addr, conn->addr, sizeof(disconnected_addr));
                // An ACL is already a BLE connection as soon as it owns one of
                // our BLE slots. During Switch 2 HOME authentication it can
                // disconnect before bthid assigns conn_index; treating that as
                // Classic leaked the slot and left the global encryption/init
                // state attached to a dead handle. Notify bthid only when it
                // was registered, but always tear down the BLE slot/state.
                if (conn->conn_index > 0) {
                    // conn_index for BLE uses BLE_CONN_INDEX_OFFSET to distinguish from Classic
                    btlife_record(BTLIFE_CTRL_DISCONNECT, reason, handle);
                    printf("[BTSTACK_HOST] BLE disconnect: notifying bthid (conn_index=%d)\n", conn->conn_index);
                    disconnected_conn_index = conn->conn_index;
                    disconnected_generation =
                        bthid_get_connection_generation(disconnected_conn_index);
                    if (ble_report_pending &&
                        pending_ble_conn_index == disconnected_conn_index &&
                        (pending_ble_connection_generation == 0u ||
                         pending_ble_connection_generation == disconnected_generation)) {
                        ble_report_pending = false;
                    }
                    bt_on_disconnect_with_generation(
                        disconnected_conn_index, disconnected_generation);
                } else {
                    printf("[BTSTACK_HOST] BLE disconnect before bthid registration; cleaning handle 0x%04X\n",
                           handle);
                }
                uint16_t dcid = conn->hids_cid;   // capture before the memset clears it
                memset(conn, 0, sizeof(*conn));
                conn->handle = HCI_CON_HANDLE_INVALID;

                // Clean up THIS connection's HIDS Host (per-connection cid)
                if (dcid != 0) {
                    hids_host_disconnect(dcid);
                }
                if (hid_state.bas_cid != 0) {
                    battery_service_client_disconnect(hid_state.bas_cid);
                    hid_state.bas_cid = 0;
                }
                hid_state.gatt_state = GATT_IDLE;
                hid_state.gatt_handle = 0;

                // Unregister GATT notification listeners
                gatt_client_stop_listening_for_characteristic_value_updates(&xbox_hid_notification_listener);
                gatt_client_stop_listening_for_characteristic_value_updates(&switch2_hid_notification_listener);

                // Cancel any in-flight post-HID setup sequence for this device
                for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
                    if (mp_hid_setup[i].handle == handle) {
                        mp_hid_setup[i].active = false;
                    }
                }

                // Tear down MouthPad NUS client if this was the MouthPad
                mp_nus_disconnected(handle);

                // Clean up Switch 2 state (ACK listener, init state machine)
                switch2_cleanup_on_disconnect(
                    disconnected_conn_index, disconnected_generation);

                // BLE disconnect — manage BLE state and reconnection
                hid_state.state = BLE_STATE_IDLE;

                // Only chase a reconnect for reasons that indicate an unexpected link loss
                // (e.g. CONNECTION_TIMEOUT / supervision timeout, CONNECTION_FAILED_TO_BE_ESTABLISHED).
                // `reason` was captured above but never consulted before this fix — every
                // disconnect, including ones the peer initiated on purpose (controller powered
                // off, or the user explicitly disconnected it), triggered the same up-to-5×
                // blind gap_connect() cascade (BLE_CONNECT_TIMEOUT_MS=10s each, ~50s worst case)
                // to a device that has no intention of reconnecting. That's pure waste — it can
                // only ever occupy the connect-attempt slot with a doomed retry, never help, and
                // during that ~50s window `btstack_host_connect_ble()` calls `btstack_host_stop_scan()`
                // at the top of every attempt, so any *other* device (or the same one re-advertising
                // under a fresh session) also can't be discovered until the cascade exhausts. Real,
                // reproducible-from-code fit for the documented "reconnect sometimes needs multiple
                // tries" symptom — see docs/bluetooth/btstack-implementation.md "Reconnect reliability".
                bool reason_warrants_reconnect =
                    reason != ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION &&
                    reason != ERROR_CODE_REMOTE_DEVICE_TERMINATED_CONNECTION_DUE_TO_POWER_OFF;

                // NO AUTOMATIC BOND DELETION HERE.
                //
                // This block used to delete the dropped peer's bond on reason
                // 0x05/0x06, reasoning that those two statuses mean a stale key
                // rather than a link-quality problem, so deleting it made the
                // next attempt a real pairing instead of a repeat of the same
                // doomed handshake. The diagnosis was right and the remedy was
                // far too destructive: it turned one bad handshake into the
                // permanent loss of a pairing the user made, silently.
                //
                // Observed 2026-08-28: an adapter that held three bonds earlier
                // in the same powered session reported `btbonds: []` with no
                // reflash and no power cycle. This site, the LE re-encryption
                // site and the Classic authentication site each delete one bond
                // on 0x05/0x06 -- between them they can empty the database. The
                // trigger was never proven. The response did not need proving to
                // be wrong: an authentication error is not a mandate to mutate
                // persistent storage.
                //
                // The reconnect cascade below is already bounded and already
                // refuses to chase a peer that left deliberately, so a genuinely
                // stale bond now fails visibly and repeatedly rather than
                // erasing itself. The user resolves it with an explicit action --
                // Forget, `bonds remove`, or the BOOTSEL wipe -- which is where
                // authority over durable pairing state belongs.
                //
                // See ns2_bt_classic_auth_failure_forgets_existing() for the
                // same rule stated once, and tested.
                if (reason == ERROR_CODE_AUTHENTICATION_FAILURE ||
                    reason == ERROR_CODE_PIN_OR_KEY_MISSING) {
                    printf("[BTSTACK_HOST] Disconnect reason 0x%02X suggests a stale key; "
                           "bond PRESERVED, dropping the link only\n", reason);
                }

                // Reconnect a bonded peer that is actually ABSENT.
                //
                // This used to target hid_state.last_connected unconditionally.
                // With two bonded peers that is wrong whenever the survivor is
                // the more recent one: the reconnect fired at a peer that was
                // still connected, and because btstack_host_connect_ble() stops
                // the scan on every attempt, the 5-attempt cascade also erased
                // the scan windows in which the departed peer's advertisements
                // would have been seen. Selecting over the bond database and
                // excluding live identities fixes both halves.
                ns2_ble_reconnect_decision_t decision = btstack_host_pick_reconnect();
                if (decision.action == NS2_BLE_RECONNECT_DIRECT &&
                    !switch2_force_fresh_custom_pairing &&
                    reason_warrants_reconnect) {
                    hid_state.reconnect_attempts++;
                    printf("[BTSTACK_HOST] Attempting BLE reconnection to stored device (attempt %d)...\n",
                           hid_state.reconnect_attempts);
                    printf("[BTSTACK_HOST] Connecting to %02X:%02X:%02X:%02X:%02X:%02X name='%s'\n",
                           decision.addr[5], decision.addr[4], decision.addr[3],
                           decision.addr[2], decision.addr[1], decision.addr[0],
                           hid_state.last_connected_name);
                    // Copy stored name to pending so it's available when connection completes
                    strncpy(hid_state.pending_name, hid_state.last_connected_name, sizeof(hid_state.pending_name) - 1);
                    hid_state.pending_name[sizeof(hid_state.pending_name) - 1] = '\0';
                    btstack_host_connect_ble(decision.addr, decision.addr_type);
                } else if (btstack_classic_get_connection_count() == 0) {
                    if (hid_state.has_last_connected && !reason_warrants_reconnect) {
                        printf("[BTSTACK_HOST] Disconnect reason 0x%02X looks intentional (peer-initiated); "
                               "not chasing a reconnect, resuming scan instead\n", reason);
                    }
                    // Resume scanning only if no devices remain
                    btstack_host_start_scan();
                }
            } else {
                // Classic BT disconnect — don't touch BLE state.
                // Classic reconnection/scanning is handled by HID_SUBEVENT_CONNECTION_CLOSED
                // or the outgoing HID failure handler. If we're waiting for an incoming
                // reconnection, don't restart scanning here.
                printf("[BTSTACK_HOST] Classic disconnect: handle=0x%04X (BLE state unchanged)\n", handle);
                {
                    char diag_reason[BTID_REASON_LEN];
                    snprintf(diag_reason, sizeof(diag_reason),
                             "classic-acl-drop-0x%02X", reason);
                    classic_pair_diag(0xFF, classic_state.pending_name,
                                      classic_state.pending_cod,
                                      classic_state.pending_vid,
                                      classic_state.pending_pid, diag_reason);
                }

                // Clear pending connection state if this was the pending device.
                // Handles cases where ACL drops before HID opens (e.g., auth failure).
                if (classic_state.pending_valid) {
                    classic_state.pending_valid = false;
                    classic_state.pending_hid_connect = false;
                }
                // The security half outlives pending_valid (HID open clears that
                // while the exchange may still be running), so release it here
                // instead: an ACL that is gone can no longer prove the key it
                // parked. Scoped by "does this peer still have an ACL" rather
                // than by the disconnecting handle, because a second Classic
                // device dropping must not discard the first one's in-flight
                // key. Erring towards keeping it is harmless -- the key is RAM
                // only until proven, and prepare() clears it before any new
                // admission.
                if (!hci_connection_for_bd_addr_and_type(
                        classic_state.pending_addr, BD_ADDR_TYPE_ACL)) {
                    classic_pending_security_clear();
                }
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST: {
            bd_addr_t req_addr;
            reverse_bytes(&packet[2], req_addr, 6);

            // Check if we have a stored link key
            link_key_t link_key;
            link_key_type_t key_type;
            bool have_key = gap_get_link_key_for_bd_addr(req_addr, link_key, &key_type);

            hci_connection_t *conn = hci_connection_for_bd_addr_and_type(req_addr, BD_ADDR_TYPE_ACL);
            // The controller reached the point of asking for a stored key: a
            // phase boundary between "page answered" and "authenticating".
            btlife_record_addr(BTLIFE_LINK_KEY_REQ, have_key ? 1u : 0u,
                               conn ? conn->con_handle : 0u, req_addr);
            printf("[BTSTACK_HOST] Link key request: %02X:%02X:%02X:%02X:%02X:%02X conn=%s have_key=%d type=%d\n",
                   req_addr[0], req_addr[1], req_addr[2], req_addr[3], req_addr[4], req_addr[5],
                   conn ? "YES" : "NO", have_key, have_key ? key_type : -1);

            // BTstack's hci.c handles this automatically - it will look up the key and respond
            // If no key is found, it sends negative reply which triggers PIN request for legacy pairing
            break;
        }

        // Legacy PIN code request - needed for Wiimote/Wii U Pro Controller
        // These devices don't support SSP and require a PIN code derived from BD_ADDR
        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t pin_addr;
            hci_event_pin_code_request_get_bd_addr(packet, pin_addr);
            printf("[BTSTACK_HOST] PIN code request: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   pin_addr[0], pin_addr[1], pin_addr[2], pin_addr[3], pin_addr[4], pin_addr[5]);

            bool fresh_pairing_admitted = classic_state.pending_valid &&
                bd_addr_cmp(pin_addr, classic_state.pending_addr) == 0 &&
                classic_state.pending_fresh_pairing_admitted;
            if (!fresh_pairing_admitted) {
                btstack_host_record_fresh_admission(false);
                printf("[BTSTACK_HOST] PIN request rejected outside explicit pairing window\n");
                gap_pin_code_negative(pin_addr);
                break;
            }

            // Check if device needs BD_ADDR-based PIN (Wiimote/Wii U Pro)
            bool needs_bdaddr_pin = false;
            if (classic_state.pending_valid &&
                bd_addr_cmp(pin_addr, classic_state.pending_addr) == 0) {
                const bt_device_profile_t* pin_profile = classic_state.pending_profile;
                if (!pin_profile && classic_state.pending_name[0]) {
                    pin_profile = bt_device_lookup_by_name(classic_state.pending_name);
                }
                if (pin_profile && pin_profile->pin_type == BT_PIN_BDADDR) {
                    needs_bdaddr_pin = true;
                }
            }
            // Also check wiimote_conn state (may have been set up during inquiry)
            if (!needs_bdaddr_pin && wiimote_conn.active &&
                memcmp(pin_addr, wiimote_conn.addr, 6) == 0) {
                needs_bdaddr_pin = true;
            }

            if (needs_bdaddr_pin) {
                // Wiimote PIN: host's BD_ADDR reversed (when using SYNC button)
                // The PIN is 6 bytes, which is the BD_ADDR in reverse byte order
                bd_addr_t local_addr;
                gap_local_bd_addr(local_addr);
                uint8_t pin[6];
                pin[0] = local_addr[5];
                pin[1] = local_addr[4];
                pin[2] = local_addr[3];
                pin[3] = local_addr[2];
                pin[4] = local_addr[1];
                pin[5] = local_addr[0];
                printf("[BTSTACK_HOST] BD_ADDR PIN device detected, sending PIN (host BD_ADDR reversed)\n");
                gap_pin_code_response_binary(pin_addr, pin, 6);
            } else {
                // No BD_ADDR PIN needed - reject (SSP devices shouldn't ask for PIN)
                printf("[BTSTACK_HOST] PIN request rejected (no BD_ADDR PIN profile)\n");
                gap_pin_code_negative(pin_addr);
            }
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t ssp_addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, ssp_addr);
            bool pending_identity_matches = classic_state.pending_valid &&
                bd_addr_cmp(ssp_addr, classic_state.pending_addr) == 0;
            bool admitted = ns2_bt_classic_ssp_response_admitted(
                pairing_lockout, pending_identity_matches,
                classic_state.pending_fresh_pairing_admitted);
            if (admitted) {
                printf("[BTSTACK_HOST] Accepting SSP confirmation for admitted Classic attempt\n");
                gap_ssp_confirmation_response(ssp_addr);
            } else {
                btstack_host_record_fresh_admission(false);
                printf("[BTSTACK_HOST] Rejecting unadmitted Classic SSP confirmation\n");
                gap_ssp_confirmation_negative(ssp_addr);
            }
            break;
        }

        case HCI_EVENT_USER_PASSKEY_REQUEST: {
            bd_addr_t ssp_addr;
            hci_event_user_passkey_request_get_bd_addr(packet, ssp_addr);
            bool pending_identity_matches = classic_state.pending_valid &&
                bd_addr_cmp(ssp_addr, classic_state.pending_addr) == 0;
            bool admitted = ns2_bt_classic_ssp_response_admitted(
                pairing_lockout, pending_identity_matches,
                classic_state.pending_fresh_pairing_admitted);
            if (admitted) {
                printf("[BTSTACK_HOST] Answering SSP passkey for admitted Classic attempt\n");
                gap_ssp_passkey_response(ssp_addr, 0u);
            } else {
                btstack_host_record_fresh_admission(false);
                printf("[BTSTACK_HOST] Rejecting unadmitted Classic SSP passkey request\n");
                gap_ssp_passkey_negative(ssp_addr);
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_NOTIFICATION: {
            bd_addr_t notif_addr;
            reverse_bytes(&packet[2], notif_addr, 6);
            link_key_t link_key;
            memcpy(link_key, &packet[8], 16);
            link_key_type_t key_type = (link_key_type_t)packet[24];

            printf("[BTSTACK_HOST] Link key notification: %02X:%02X:%02X:%02X:%02X:%02X type=%d\n",
                   notif_addr[0], notif_addr[1], notif_addr[2], notif_addr[3], notif_addr[4], notif_addr[5], key_type);

            bool pending_identity_matches = classic_state.pending_valid &&
                bd_addr_cmp(notif_addr, classic_state.pending_addr) == 0;
            bool same_existing_key =
                classic_state.pending_existing_link_key_valid &&
                memcmp(link_key, classic_state.pending_existing_link_key,
                       sizeof(link_key_t)) == 0;
            bool authenticated_key_change =
                key_type == CHANGED_COMBINATION_KEY;
            bool trust_update_admitted = ns2_bt_classic_key_update_admitted(
                pairing_lockout, pending_identity_matches,
                classic_state.pending_fresh_pairing_admitted,
                classic_state.pending_trust_present,
                same_existing_key, authenticated_key_change);
            if (!trust_update_admitted) {
                // BTstack may already have auto-stored the event's key. Delete
                // it explicitly so stale local trust cannot authorize a new SSP
                // relationship. Restore the previous key when this identity had
                // one; a generic failed replacement must not destroy it.
                btstack_host_record_fresh_admission(false);
                printf("[BTSTACK_HOST] Dropping unadmitted Classic link key\n");
                if (pending_identity_matches) {
                    classic_restore_existing_key();
                } else {
                    gap_drop_link_key_for_bd_addr(notif_addr);
                }
                hci_connection_t *unadmitted =
                    hci_connection_for_bd_addr_and_type(notif_addr,
                                                        BD_ADDR_TYPE_ACL);
                if (unadmitted) gap_disconnect(unadmitted->con_handle);
                break;
            }

            memcpy(classic_state.pending_notified_link_key, link_key,
                   sizeof(classic_state.pending_notified_link_key));
            classic_state.pending_notified_link_key_type = key_type;
            classic_state.pending_notified_link_key_valid = true;
            classic_state.pending_notified_link_key_admitted = true;
            classic_restore_existing_key();
            classic_commit_notified_key_if_authenticated();
            break;
        }

        case HCI_EVENT_AUTHENTICATION_COMPLETE: {
            uint8_t status = packet[2];
            hci_con_handle_t handle = little_endian_read_16(packet, 3);
            hci_connection_t *auth_conn = hci_connection_for_handle(handle);
            btlife_record_addr(BTLIFE_AUTH_DONE, status, handle,
                               auth_conn ? auth_conn->address : NULL);
            bool pending_identity_matches = classic_state.pending_valid &&
                auth_conn &&
                bd_addr_cmp(auth_conn->address,
                            classic_state.pending_addr) == 0;
            printf("[BTSTACK_HOST] Authentication complete: handle=0x%04X status=0x%02X\n", handle, status);

            // Same classification as the encryption side; see
            // ns2_bt_encryption_collision(). The status code for "both ends ran
            // this procedure" is identical on either procedure.
            if (ns2_bt_encryption_collision(status)) {
                classic_authentication_collisions++;
                printf("[BTSTACK_HOST] Classic authentication collision 0x23 "
                       "(handle=0x%04X, collisions=%lu)\n",
                       handle, (unsigned long)classic_authentication_collisions);
            }

            // Stand down from BTstack's automatic encryption request for the
            // companion's Controller Link. BTstack set BONDING_SEND_ENCRYPTION_REQUEST
            // in the event handler that ran just before this one; hci_run() has
            // not consumed it yet, so clearing it here is what keeps both hosts
            // from starting the same LMP procedure. See
            // ns2_bt_defer_classic_encryption() for the captured collision.
            if (status == ERROR_CODE_SUCCESS && auth_conn != NULL) {
                bool we_own_fresh_pairing = (classic_fresh_pairing_security_handle == handle);
                // Capture the deciding inputs before acting on them; see
                // last_auth_decision for why the counters alone are not enough.
                // An outgoing link never passed through the incoming path that
                // opens the record, and a reused handle must not inherit the
                // previous link's fields, so open one here unless this ACL
                // already owns it.
                if (!auth_decision_owns(handle)) {
                    auth_decision_begin(handle);
                }
                last_auth_decision.mgmt_connected =
                    config_ble.handle != HCI_CON_HANDLE_INVALID;
                // Durable identity, not the LE connection address. auth_conn
                // ->address is a Classic BD_ADDR; an LE RPA can never equal
                // one, so the old raw client_addr comparison was permanently
                // false and this companion was never recognised as its own
                // Controller Link peer. With cross-transport key derivation the
                // phone's Classic address IS its LE identity, so comparing
                // against the identity is what makes the match possible at all.
                bd_addr_t mgmt_identity;
                last_auth_decision.mgmt_addr_known =
                    last_auth_decision.mgmt_connected &&
                    config_ble_durable_addr(mgmt_identity);
                last_auth_decision.mgmt_addr_matches =
                    last_auth_decision.mgmt_addr_known &&
                    memcmp(mgmt_identity, auth_conn->address,
                           sizeof(bd_addr_t)) == 0;
                last_auth_decision.mgmt_link_trusted =
                    last_auth_decision.mgmt_addr_matches &&
                    config_ble_link_trusted(config_ble.handle);
                last_auth_decision.we_own_fresh_pairing = we_own_fresh_pairing;
                last_auth_decision.request_was_pending =
                    (auth_conn->bonding_flags & BONDING_SEND_ENCRYPTION_REQUEST) != 0;
                last_auth_decision.stood_down = false;
                last_auth_decision.auth_outcome = NS2_BT_AUTH_OBSERVED_OK;
                if (ns2_bt_defer_classic_encryption(
                        btstack_host_classic_companion_session_trust(auth_conn->address),
                        we_own_fresh_pairing) &&
                    btstack_host_stand_down_from_encryption(auth_conn)) {
                    last_auth_decision.stood_down = true;
                    classic_encryption_deferrals++;
                    classic_encryption_deferred_handle = handle;
                    printf("[BTSTACK_HOST] Deferring Classic encryption to companion "
                           "(handle=0x%04X, deferrals=%lu)\n",
                           handle, (unsigned long)classic_encryption_deferrals);
                }
            }
            if (status != ERROR_CODE_SUCCESS && auth_decision_owns(handle)) {
                // A local Authentication Complete that failed IS an observation,
                // and a different thing from never having asked. See
                // ns2_bt_auth_observation_t.
                last_auth_decision.auth_outcome = NS2_BT_AUTH_OBSERVED_FAILED;
            }
            if (classic_fresh_pairing_security_handle == handle) {
                classic_fresh_pairing_security_handle = HCI_CON_HANDLE_INVALID;
            }
            {
                char reason[BTID_REASON_LEN];
                snprintf(reason, sizeof(reason), "classic-auth-status-0x%02X", status);
                classic_pair_diag(0xFF, classic_state.pending_name,
                                  classic_state.pending_cod,
                                  classic_state.pending_vid,
                                  classic_state.pending_pid, reason);
            }

            if (pending_identity_matches) {
                if (status == ERROR_CODE_SUCCESS) {
                    classic_state.pending_auth_succeeded = true;
                    classic_commit_notified_key_if_authenticated();
                    break;
                }

                // 0x05 Authentication Failure is generic: preserve the known
                // local key. 0x06 PIN_OR_KEY_MISSING used to invalidate it; it no
                // longer does, and the predicate is now false for every status.
                // See ns2_bt_classic_auth_failure_forgets_existing().
                //
                // The branch is kept rather than deleted so the policy has one
                // switch. Turning it back on means changing that one function,
                // past its comment and its test, rather than editing this site.
                if (ns2_bt_classic_auth_failure_forgets_existing(status)) {
                    printf("[BTSTACK_HOST] Peer reports PIN/key missing; deleting stale Classic key\n");
                    gap_drop_link_key_for_bd_addr(classic_state.pending_addr);
                    classic_state.pending_existing_link_key_valid = false;
                    classic_state.pending_trust_present = false;
                } else {
                    // Restores the key BTstack may already have overwritten from
                    // the notification. This is a REPLACE-BACK, not a delete:
                    // net effect is the durable key the peer had before.
                    classic_restore_existing_key();
                    printf("[BTSTACK_HOST] Classic authentication failed (0x%02X); "
                           "existing key PRESERVED\n", status);
                }
                classic_state.pending_notified_link_key_valid = false;
                classic_state.pending_notified_link_key_admitted = false;
                memset(classic_state.pending_notified_link_key, 0,
                       sizeof(classic_state.pending_notified_link_key));

                // Clean up wiimote state if auth failed before L2CAP channels were created
                if (wiimote_conn.active && wiimote_conn.acl_handle == handle) {
                    memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                }
                classic_state.pending_hid_connect = false;

                gap_disconnect(handle);
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE:
        case HCI_EVENT_ENCRYPTION_CHANGE_V2: {
            bool event_v2 = event_type == HCI_EVENT_ENCRYPTION_CHANGE_V2;
            hci_con_handle_t handle = event_v2 ?
                hci_event_encryption_change_v2_get_connection_handle(packet) :
                hci_event_encryption_change_get_connection_handle(packet);
            uint8_t status = event_v2 ?
                hci_event_encryption_change_v2_get_status(packet) :
                hci_event_encryption_change_get_status(packet);
            uint8_t enabled = event_v2 ?
                hci_event_encryption_change_v2_get_encryption_enabled(packet) :
                hci_event_encryption_change_get_encryption_enabled(packet);
            // b carries enabled + key size, so the acceptance gate's input is
            // visible in the ring and not only in the single-slot btauth record.
            btlife_record(BTLIFE_ENC_CHANGE, status,
                          (uint16_t)((enabled ? 0x100u : 0u) |
                                     gap_encryption_key_size(handle)));

            printf("[BTSTACK_HOST] Encryption change: handle=0x%04X status=0x%02X enabled=%d\n",
                   handle, status, enabled);

            // Type C observability. ERROR_CODE_LMP_ERROR_TRANSACTION_COLLISION
            // (0x23) is the collision itself; on the deferral handle a clean
            // enable is the peer completing encryption for us, which is the
            // whole point of standing down.
            if (ns2_bt_encryption_collision(status)) {
                classic_encryption_collisions++;
            } else if (ns2_bt_encryption_completed_for_deferral(
                           status, enabled != 0,
                           handle == classic_encryption_deferred_handle)) {
                classic_encryption_peer_completed++;
                classic_encryption_deferred_handle = HCI_CON_HANDLE_INVALID;
            }
            // Peer-led Classic pairings prove themselves here and nowhere else.
            // Without this the notified key stays parked until HID open discards
            // it, and the pairing is good for exactly one session.
            classic_note_security_proof(
                handle, false,
                (status == ERROR_CODE_SUCCESS) && (enabled != 0));

            if (auth_decision_owns(handle)) {
                last_auth_decision.encrypted_ok =
                    (status == ERROR_CODE_SUCCESS) && (enabled != 0);
                // Deliberately NOT sampling gap_encryption_key_size() here.
                // See last_auth_decision.encryption_key_size: the controller
                // has not been asked yet at this point, so it would record 0
                // on every peer-led link and look like a security failure.
            }

            if (switch2_direct_reencrypt_active &&
                switch2_direct_reencrypt_handle == handle) {
                switch2_direct_encrypt_events++;
                switch2_last_encrypt_status = status;
                switch2_last_encrypt_enabled = enabled;
                uint8_t encrypt_phase = switch2_direct_encrypt_phase;
                switch2_direct_reencrypt_active = false;
                switch2_direct_reencrypt_handle = HCI_CON_HANDLE_INVALID;
                switch2_direct_encrypt_phase = SW2_ENCRYPT_NONE;
                hid_state.last_reencryption_status = status;

                ble_connection_t *conn = find_connection_by_handle(handle);
                if (status == ERROR_CODE_SUCCESS && enabled != 0) {
                    hid_state.reencryption_successes++;
                    switch2_link_encrypted = true;
                    switch2_link_encrypted_handle = handle;
                    printf("[SW2_BLE] Direct HCI HOME re-encryption succeeded\n");
                    if (conn) {
                        btstack_host_remember_ble_connection(conn);
                        register_switch2_hid_listener(handle);
                    }
                } else {
                    hid_state.reencryption_failures++;
                    printf("[SW2_BLE] Direct HCI encryption failed: phase=%u status=0x%02X enabled=%u\n",
                           encrypt_phase, status, enabled);
                    // Aligned with the SM re-encryption path, which already
                    // preserves the custom bond and says why: an SM failure can
                    // be a local state or timing error and must not silently
                    // force the user back through SYNC pairing. The two halves
                    // of the same Switch 2 recovery disagreed -- this one
                    // deleted the bond, its sibling kept it -- so which one ran
                    // decided whether the pairing survived.
                    //
                    // The fresh-pairing latch is kept: the next attempt may
                    // establish new trust, and doing so REPLACES the key rather
                    // than requiring it to be absent first.
                    if (conn) {
                        switch2_force_fresh_custom_pairing = true;
                    }
                    gap_disconnect(handle);
                }
            }
            {
                char reason[BTID_REASON_LEN];
                snprintf(reason, sizeof(reason), "classic-encrypt-%02X-%u", status, enabled);
                classic_pair_diag(0xFF, classic_state.pending_name,
                                  classic_state.pending_cod,
                                  classic_state.pending_vid,
                                  classic_state.pending_pid, reason);
            }

            // For Wiimotes, create L2CAP control channel after encryption is enabled
            // This handles both initial pairing (state=IDLE) and reconnection (state=W4_CONTROL_CONNECTED)
            if (status == 0 && enabled && wiimote_conn.active &&
                wiimote_conn.acl_handle == handle &&
                (wiimote_conn.state == WIIMOTE_STATE_IDLE ||
                 wiimote_conn.state == WIIMOTE_STATE_W4_CONTROL_CONNECTED) &&
                wiimote_conn.control_cid == 0) {

                // For incoming reconnections, don't create outgoing L2CAP channels.
                // The controller will initiate its own channels via HID Host.
                // Creating outgoing channels conflicts with the incoming ones.
                if (classic_state.pending_valid && !classic_state.pending_outgoing) {
                    printf("[BTSTACK_HOST] Wiimote: incoming reconnection, waiting for HID Host channels\n");
                    break;
                }

                printf("[BTSTACK_HOST] Wiimote: encryption enabled, creating HID Control channel (PSM 0x11)...\n");

                uint16_t control_cid;
                uint8_t l2cap_status = l2cap_create_channel(wiimote_l2cap_packet_handler,
                                                            wiimote_conn.addr,
                                                            PSM_HID_CONTROL,
                                                            0xFFFF,  // MTU
                                                            &control_cid);
                if (l2cap_status == ERROR_CODE_SUCCESS) {
                    wiimote_conn.control_cid = control_cid;
                    wiimote_conn.state = WIIMOTE_STATE_W4_CONTROL_CONNECTED;
                    printf("[BTSTACK_HOST] Wiimote: L2CAP control channel request sent, cid=0x%04X\n", control_cid);
                } else {
                    printf("[BTSTACK_HOST] Wiimote: l2cap_create_channel failed: 0x%02X\n", l2cap_status);
                    wiimote_conn.active = false;
                    classic_state.pending_hid_connect = false;
                }
            }
            break;
        }

        case GAP_EVENT_SECURITY_LEVEL: {
            hci_con_handle_t handle = gap_event_security_level_get_handle(packet);
            gap_security_level_t level = gap_event_security_level_get_security_level(packet);
            printf("[BTSTACK_HOST] Security level update: handle=0x%04X level=%d\n", handle, level);
            break;
        }

        case HCI_EVENT_ROLE_CHANGE: {
            uint8_t status = hci_event_role_change_get_status(packet);
            bd_addr_t addr;
            hci_event_role_change_get_bd_addr(packet, addr);
            uint8_t role = hci_event_role_change_get_role(packet);
            printf("[BTSTACK_HOST] Role change: %02X:%02X:%02X:%02X:%02X:%02X status=%d role=%s\n",
                   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                   status, role == 0 ? "MASTER" : "SLAVE");
            break;
        }
    }
}

// ============================================================================
// SM EVENT HANDLER
// ============================================================================

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

#ifdef CONFIG_USB2BLE
    return;  // USB2BLE is a BLE peripheral — ble_output handles SM events
#endif

    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case SM_EVENT_JUST_WORKS_REQUEST:
        {
            hci_con_handle_t handle =
                sm_event_just_works_request_get_handle(packet);
            if (handle == config_ble.handle) {
                if (config_ble_accept_new_bond()) {
                    printf("[BTSTACK_HOST] SM: management Just Works accepted "
                           "(admitted when this connection was accepted)\n");
                    sm_just_works_confirm(handle);
                } else {
                    printf("[BTSTACK_HOST] SM: management Just Works declined -- this "
                           "connection was not admitted by an open pairing window\n");
                    sm_bonding_decline(handle);
                }
                break;
            }
            ble_connection_t *conn = find_connection_by_handle(handle);
            if (conn && conn->fresh_pairing_admitted) {
                btstack_host_record_fresh_admission(true);
                conn->fresh_pairing_admitted = false;
                printf("[BTSTACK_HOST] SM: controller Just Works accepted from explicit pairing window\n");
                sm_just_works_confirm(handle);
            } else {
                btstack_host_record_fresh_admission(false);
                printf("[BTSTACK_HOST] SM: controller Just Works declined outside pairing window\n");
                sm_bonding_decline(handle);
                gap_disconnect(handle);
            }
            break;
        }

        case SM_EVENT_PAIRING_STARTED:
            printf("[BTSTACK_HOST] SM: Pairing started\n");
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            hci_con_handle_t handle = sm_event_pairing_complete_get_handle(packet);
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            printf("[BTSTACK_HOST] SM: Pairing complete, handle=0x%04X status=0x%02X\n", handle, status);

            if (status == ERROR_CODE_SUCCESS) {
                printf("[BTSTACK_HOST] SM: Pairing successful!\n");
                ble_connection_t* conn = find_connection_by_handle(handle);
                if (conn) {
                    btstack_host_remember_ble_connection(conn);
                    printf("[BTSTACK_HOST] Stored device for reconnection: %02X:%02X:%02X:%02X:%02X:%02X name='%s'\n",
                           conn->addr[5], conn->addr[4], conn->addr[3], conn->addr[2], conn->addr[1], conn->addr[0],
                           hid_state.last_connected_name);

                    // Route based on BLE strategy
                    if (conn->profile && conn->profile->ble == BT_BLE_DIRECT_ATT) {
                        printf("[BTSTACK_HOST] %s detected - using fast-path HID listener\n",
                               conn->profile->name);
                        register_ble_hid_listener(handle);
                    } else if (conn->profile && conn->profile->ble == BT_BLE_CUSTOM) {
                        printf("[BTSTACK_HOST] %s detected - using fast-path notification enable\n",
                               conn->profile->name);
                        register_switch2_hid_listener(handle);
                    } else {
                        printf("[BTSTACK_HOST] BLE controller - starting GATT discovery\n");
                        start_hids_host(conn);
                        // MouthPad NUS is armed later, from the HID
                        // REPORTS_NOTIFICATION (0x1C) handler, so it doesn't
                        // contend with the HID notification enable.
                    }
                }
            } else {
                printf("[BTSTACK_HOST] SM: Pairing FAILED\n");
            }
            break;
        }

        case SM_EVENT_REENCRYPTION_STARTED:
            printf("[BTSTACK_HOST] SM: Re-encryption started\n");
            hid_state.reencryption_started++;
            break;

        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED: {
            // BTstack already did the resolution; this only records the answer.
            // ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION makes SM match a connecting
            // RPA against the IRKs in the LE device DB, and this event carries
            // the identity it matched. Nothing here re-derives it -- writing a
            // second resolver would be inventing a fact BTstack already has.
            //
            // Recorded for the management link only. Controller links already
            // carry their own identity in ble_connection_t, and the reconnect
            // and forget paths use that.
            hci_con_handle_t handle =
                sm_event_identity_resolving_succeeded_get_handle(packet);
            if (handle == config_ble.handle) {
                bd_addr_t identity;
                sm_event_identity_resolving_succeeded_get_identity_address(packet, identity);
                memcpy(config_ble.client_identity_addr, identity,
                       sizeof(config_ble.client_identity_addr));
                config_ble.client_identity_addr_type =
                    sm_event_identity_resolving_succeeded_get_identity_addr_type(packet);
                config_ble.client_identity_valid = true;
                printf("[BTSTACK_HOST] Management identity resolved: %s (type=%u)\n",
                       bd_addr_to_str(identity),
                       (unsigned)config_ble.client_identity_addr_type);
            }
            break;
        }

        case SM_EVENT_REENCRYPTION_COMPLETE: {
            hci_con_handle_t handle = sm_event_reencryption_complete_get_handle(packet);
            uint8_t status = sm_event_reencryption_complete_get_status(packet);
            hid_state.last_reencryption_status = status;
            if (status == ERROR_CODE_SUCCESS) {
                hid_state.reencryption_successes++;
            } else {
                hid_state.reencryption_failures++;
            }
            printf("[BTSTACK_HOST] SM: Re-encryption complete, handle=0x%04X status=0x%02X\n", handle, status);
            if (status == ERROR_CODE_SUCCESS) {
                printf("[BTSTACK_HOST] SM: Re-encryption successful!\n");
                ble_connection_t* conn = find_connection_by_handle(handle);
                if (conn) {
                    if (conn->profile && conn->profile->ble == BT_BLE_CUSTOM) {
                        switch2_link_encrypted = true;
                        switch2_link_encrypted_handle = handle;
                    }
                    // Refresh the durable target, including transport identity.
                    btstack_host_remember_ble_connection(conn);

                    // Route based on BLE strategy
                    if (conn->profile && conn->profile->ble == BT_BLE_DIRECT_ATT) {
                        printf("[BTSTACK_HOST] %s detected - using fast-path HID listener\n",
                               conn->profile->name);
                        register_ble_hid_listener(handle);
                    } else if (conn->profile && conn->profile->ble == BT_BLE_CUSTOM) {
                        printf("[BTSTACK_HOST] %s detected - using fast-path notification enable\n",
                               conn->profile->name);
                        register_switch2_hid_listener(handle);
                    } else {
                        printf("[BTSTACK_HOST] BLE controller - starting GATT discovery\n");
                        start_hids_host(conn);
                        // MouthPad NUS is armed later, from the HID
                        // REPORTS_NOTIFICATION (0x1C) handler, so it doesn't
                        // contend with the HID notification enable.
                    }
                }
            } else {
                ble_connection_t* conn = find_connection_by_handle(handle);
                if (handle == config_ble.handle) {
                    // THE MANAGEMENT LINK MUST NEVER LOSE ITS BOND HERE.
                    //
                    // Everything below this branch is controller-recovery
                    // policy: a peer whose stored key no longer works is a
                    // controller that was re-paired elsewhere, so deleting the
                    // stale bond is the only way to make the next attempt a
                    // real pairing. That reasoning does not transfer to the
                    // companion, and applying it to the companion is a one-way
                    // door -- the credential the app needs to reconnect is
                    // destroyed by the very failure it should have survived.
                    //
                    // It reached the companion by omission, not by intent. The
                    // management peripheral is a PERIPHERAL-role link and so is
                    // deliberately absent from the central-role connection
                    // table (see btstack_host_pick_reconnect(), which documents
                    // the same invariant). find_connection_by_handle() there-
                    // fore returns NULL for it, and NULL fell through to "an
                    // ordinary controller with a stale bond".
                    //
                    // Scope of this guard, stated exactly. It is NOT known to be
                    // the trigger of any observed failure: what has been traced
                    // is that reaching this branch is UNRECOVERABLE. Deleting
                    // the companion's key here means every later connect fails
                    // at the HCI layer with 0x05/0x06, and the companion treats
                    // that pair of statuses as terminal by design -- 0x05/0x06
                    // are deliberately excluded from GattRecoveryPolicy's retry
                    // set, so the app goes straight to RepairRequired and does
                    // not try again. One transient security failure would
                    // therefore cost the user a manual re-pair.
                    //
                    // Dropping the link without touching the bond keeps that
                    // failure recoverable instead of terminal. It does not by
                    // itself make the app retry -- the app's 0x05/0x06 policy is
                    // a separate question -- but it leaves a working credential
                    // for the next attempt, which is the difference between a
                    // hiccup and a re-pair. Clearing a companion bond stays a
                    // deliberate act: Repair pairing, `bonds remove`, and the
                    // BOOTSEL triple-tap wipe. None of those is this handler.
                    printf("[BTSTACK_HOST] Management re-encryption failed (status=0x%02X); "
                           "companion bond preserved, dropping link for retry\n", status);
                    gap_disconnect(handle);
                } else if (conn && conn->profile && conn->profile->ble == BT_BLE_CUSTOM) {
                    // Preserve the durable Nintendo custom bond. An SM failure
                    // can be a local state/timing error and must not silently
                    // force the user back through SYNC pairing.
                    printf("[SW2_BLE] SM re-encryption failed; preserving custom bond for retry\n");
                    btstack_host_install_switch2_ltk();
                    gap_disconnect(handle);
                } else {
                    // The bond is PRESERVED here too, for the same reason the
                    // companion's is: a re-encryption failure is a failed
                    // handshake, not a licence to destroy durable state. This
                    // branch used to delete it and then re-pair if the pairing
                    // window happened to be open -- so an ordinary transient
                    // failure with the window shut cost the user the pairing and
                    // gave nothing back.
                    //
                    // A peer whose key really is gone still cannot connect, and
                    // fails the same way every time until the user forgets it
                    // explicitly. That is bounded and visible; silent deletion
                    // was neither.
                    if (conn && conn->fresh_pairing_admitted) {
                        // The user has an open pairing window, which IS an
                        // explicit act: re-pairing is what they asked for. SM
                        // replaces the key on success; nothing is deleted first,
                        // so a failed re-pair leaves the old bond usable.
                        printf("[BTSTACK_HOST] LE re-encryption failed; pairing window open, "
                               "requesting fresh pairing with the bond intact\n");
                        sm_request_pairing(handle);
                    } else {
                        printf("[BTSTACK_HOST] LE re-encryption failed; bond PRESERVED, "
                               "dropping the link\n");
                        gap_disconnect(handle);
                    }
                }
            }
            break;
        }
    }
}

// ============================================================================
// MOUTHPAD NUS (Nordic UART Service) CLIENT
// ============================================================================
// Self-contained GATT client for the Augmental MouthPad's NUS stream. Acts
// ONLY on MouthPad connections (gated by mp_nus_mark_pending, which the
// connection-ready path calls only when the device name contains "MouthPad"),
// so it has no effect on any other controller. Discovery is dynamic by
// 128-bit UUID (no hardcoded handles) and is deferred ~1.5 s after connect so
// it runs after the HIDS Host has finished its own GATT discovery (the
// gatt_client allows one query at a time per connection).
//
// Device->host NUS notifications fire mp_nus_rx_cb; host->device writes go
// through btstack_host_mouthpad_nus_send(). The CDC<->NUS framing/relay glue
// lives in mp_bridge.c.

// NUS 128-bit UUIDs (textual / big-endian order, as BTstack uuid128 expects).
static const uint8_t nus_service_uuid128[16] = {
    0x6E,0x40,0x00,0x01,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x9E};
static const uint8_t nus_rx_uuid128[16] = {  // write  (host -> device)
    0x6E,0x40,0x00,0x02,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x9E};
static const uint8_t nus_tx_uuid128[16] = {  // notify (device -> host)
    0x6E,0x40,0x00,0x03,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x9E};

typedef enum {
    MP_NUS_IDLE = 0,
    MP_NUS_PENDING,        // connected, waiting for HID discovery to settle
    MP_NUS_DISC_SERVICE,
    MP_NUS_DISC_CHARS,
    MP_NUS_ENABLE_CCC,
    MP_NUS_READY,
} mp_nus_state_t;

static struct {
    mp_nus_state_t state;
    hci_con_handle_t handle;
    uint32_t pending_since;
    gatt_client_service_t service;
    gatt_client_characteristic_t tx_char;      // notify characteristic
    uint16_t rx_value_handle;                  // write characteristic value handle
    gatt_client_notification_t notify;
    uint8_t last_battery;                      // last BAS level seen (0 = unknown)
    char    firmware[24];                      // DIS firmware revision (for relay device_info)
} mp_nus = { .state = MP_NUS_IDLE, .handle = HCI_CON_HANDLE_INVALID };

// Host->device NUS write queue (drained on the BTstack run loop — see
// btstack_host_mouthpad_nus_send below). Declared here so mp_nus_reset() can
// flush it on disconnect.
#define MP_TX_SLOTS      8                  // host->device queue depth (low-rate)
#define MP_TX_SLOT_SIZE  247                // <= NUS MTU payload
typedef struct { uint16_t len; uint8_t data[MP_TX_SLOT_SIZE]; } mp_tx_slot_t;
static mp_tx_slot_t      mp_tx[MP_TX_SLOTS];
static volatile uint32_t mp_tx_head;        // write index (producer: main loop)
static volatile uint32_t mp_tx_tail;        // read index (consumer: run loop)
static volatile bool     mp_tx_scheduled;
static btstack_context_callback_registration_t mp_tx_cb;

static void (*mp_nus_rx_cb)(const uint8_t* data, uint16_t len) = NULL;

void btstack_host_set_mouthpad_nus_rx_cb(void (*cb)(const uint8_t*, uint16_t))
{
    mp_nus_rx_cb = cb;
}

bool btstack_host_mouthpad_nus_ready(void)
{
    return mp_nus.state == MP_NUS_READY;
}

// Fill `out` with the connected MouthPad's device info (for the dongle-level
// relay device_info_response / connection_status_response). Returns false if no
// MouthPad connection exists.
bool btstack_host_get_mouthpad_info(btstack_host_mouthpad_info_t* out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (mp_nus.handle == HCI_CON_HANDLE_INVALID) return false;
    ble_connection_t* c = find_connection_by_handle(mp_nus.handle);
    if (!c) return false;
    strncpy(out->name, c->name, sizeof(out->name) - 1);
    strncpy(out->firmware, mp_nus.firmware, sizeof(out->firmware) - 1);
    memcpy(out->addr, c->addr, 6);
    out->vid = c->vid;
    out->pid = c->pid;
    out->battery = mp_nus.last_battery;
    out->ready = (mp_nus.state == MP_NUS_READY);
    return true;
}

// Called from the connection-ready path only for MouthPad devices.
static void mp_nus_mark_pending(hci_con_handle_t handle)
{
    if (mp_nus.state != MP_NUS_IDLE) return;   // one MouthPad NUS at a time
    mp_nus.state = MP_NUS_PENDING;
    mp_nus.handle = handle;
    mp_nus.pending_since = btstack_run_loop_get_time_ms();
    mp_nus.tx_char.value_handle = 0;
    mp_nus.rx_value_handle = 0;
    printf("[MP_NUS] MouthPad connected (0x%04X) — NUS discovery pending\n", handle);
}

static void mp_nus_reset(void)
{
    if (mp_nus.state == MP_NUS_READY) {
        gatt_client_stop_listening_for_characteristic_value_updates(&mp_nus.notify);
    }
    mp_nus.state = MP_NUS_IDLE;
    mp_nus.handle = HCI_CON_HANDLE_INVALID;
    mp_nus.tx_char.value_handle = 0;
    mp_nus.rx_value_handle = 0;
    mp_nus.last_battery = 0;
    mp_nus.firmware[0] = '\0';
    // Discard any queued host->device writes for the gone MouthPad.
    mp_tx_tail = mp_tx_head;
    mp_tx_scheduled = false;
}

static void mp_nus_disconnected(hci_con_handle_t handle)
{
    if (mp_nus.handle == handle) {
        printf("[MP_NUS] MouthPad disconnected — NUS reset\n");
        mp_nus_reset();
    }
}

// Device -> host notifications on the NUS TX characteristic.
static void mp_nus_notify_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;
    uint16_t vh = gatt_event_notification_get_value_handle(packet);
    if (vh != mp_nus.tx_char.value_handle) return;
    uint16_t len = gatt_event_notification_get_value_length(packet);
    const uint8_t* val = gatt_event_notification_get_value(packet);
    if (mp_nus_rx_cb) mp_nus_rx_cb(val, len);
}

// GATT discovery state machine for the NUS service.
static void mp_nus_gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event = hci_event_packet_get_type(packet);

    switch (event) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            gatt_event_service_query_result_get_service(packet, &mp_nus.service);
            break;

        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            gatt_client_characteristic_t ch;
            gatt_event_characteristic_query_result_get_characteristic(packet, &ch);
            if (memcmp(ch.uuid128, nus_tx_uuid128, 16) == 0) {
                mp_nus.tx_char = ch;
            } else if (memcmp(ch.uuid128, nus_rx_uuid128, 16) == 0) {
                mp_nus.rx_value_handle = ch.value_handle;
            }
            break;
        }

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t status = gatt_event_query_complete_get_att_status(packet);
            if (status != 0) {
                printf("[MP_NUS] GATT query failed (state=%d status=0x%02X)\n", mp_nus.state, status);
                mp_nus_reset();
                break;
            }
            if (mp_nus.state == MP_NUS_DISC_SERVICE) {
                if (mp_nus.service.start_group_handle == 0) {
                    printf("[MP_NUS] No NUS service on device\n");
                    mp_nus_reset();
                    break;
                }
                mp_nus.state = MP_NUS_DISC_CHARS;
                gatt_client_discover_characteristics_for_service(
                    mp_nus_gatt_handler, mp_nus.handle, &mp_nus.service);
            } else if (mp_nus.state == MP_NUS_DISC_CHARS) {
                if (mp_nus.tx_char.value_handle == 0) {
                    printf("[MP_NUS] NUS TX characteristic not found\n");
                    mp_nus_reset();
                    break;
                }
                mp_nus.state = MP_NUS_ENABLE_CCC;
                gatt_client_write_client_characteristic_configuration(
                    mp_nus_gatt_handler, mp_nus.handle, &mp_nus.tx_char,
                    GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            } else if (mp_nus.state == MP_NUS_ENABLE_CCC) {
                gatt_client_listen_for_characteristic_value_updates(
                    &mp_nus.notify, mp_nus_notify_handler, mp_nus.handle, &mp_nus.tx_char);
                mp_nus.state = MP_NUS_READY;
                printf("[MP_NUS] NUS ready (tx=0x%04X rx=0x%04X)\n",
                       mp_nus.tx_char.value_handle, mp_nus.rx_value_handle);
            }
            break;
        }
    }
}

// Periodic: kick off discovery once the HID side has settled.
static void mp_nus_periodic(void)
{
    if (mp_nus.state != MP_NUS_PENDING) return;
    if ((btstack_run_loop_get_time_ms() - mp_nus.pending_since) < 1500) return;
    if (gatt_client_is_ready(mp_nus.handle) == 0) return;   // another query in flight
    mp_nus.state = MP_NUS_DISC_SERVICE;
    mp_nus.service.start_group_handle = 0;
    printf("[MP_NUS] Starting NUS discovery on 0x%04X\n", mp_nus.handle);
    gatt_client_discover_primary_services_by_uuid128(
        mp_nus_gatt_handler, mp_nus.handle, nus_service_uuid128);
}

// Deferred post-HID setup: REPORT protocol mode, then DIS/BAS + NUS arm.
static void mp_hid_setup_task(void)
{
    uint32_t now = btstack_run_loop_get_time_ms();

    // Run the REPORT-mode + notification-enable sequence for EACH connecting BLE
    // HID device independently (per-connection cid), so two devices both stream.
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (!mp_hid_setup[i].active) continue;
        uint16_t cid = mp_hid_setup[i].hids_cid;

        switch (mp_hid_setup[i].phase) {
            case 0: {
                // Write REPORT protocol mode FIRST. The MouthPad boots in BOOT mode
                // and BTstack's hids_host never writes the mode when REPORT is
                // requested. hids_host only accepts this once back in CONNECTED
                // state (returns 0x0C COMMAND_DISALLOWED until then), so retry.
                uint8_t st = hids_host_send_set_protocol_mode(cid, 0, HID_PROTOCOL_MODE_REPORT);
                if (st == ERROR_CODE_SUCCESS) {
                    printf("[MP] REPORT protocol-mode write initiated (cid=0x%04X)\n", cid);
                    mp_hid_setup[i].phase = 1;
                    mp_hid_setup[i].phase_ms = now;
                } else if ((now - mp_hid_setup[i].start_ms) > 3000) {
                    printf("[MP] protocol-mode write never accepted (last=0x%02X) — enabling anyway\n", st);
                    mp_hid_setup[i].phase = 1;
                    mp_hid_setup[i].phase_ms = now;
                }
                break;
            }
            case 1: {
                // After the write-without-response flushes, enable HID notifications
                // (NOW that the device is in REPORT mode, so the CCCs stick). Retry
                // until hids_host accepts it. DIS/BAS/NUS start from the 0x1C event.
                if ((now - mp_hid_setup[i].phase_ms) < 300) break;
                uint8_t r = hids_host_enable_notifications(cid);
                if (r == ERROR_CODE_SUCCESS) {
                    printf("[MP] notifications enabled after REPORT-mode switch (cid=0x%04X)\n", cid);
                    mp_hid_setup[i].active = false;
                } else if ((now - mp_hid_setup[i].start_ms) > 6000) {
                    printf("[MP] enable_notifications never accepted (last=0x%02X)\n", r);
                    mp_hid_setup[i].active = false;
                }
                break;
            }
            default:
                mp_hid_setup[i].active = false;
                break;
        }
    }
}

// Host -> device write (called from the bridge; safe in BTstack/run-loop context).
// ---------------------------------------------------------------------------
// Host->device NUS write, marshaled onto the BTstack run loop.
//
// btstack_host_mouthpad_nus_send() is called from the CDC/relay context (the
// main loop), but gatt_client_write MUST run on the BTstack thread: on nRF/ESP
// BTstack lives in its own RTOS task, and calling GATT APIs cross-thread races
// with BTstack's own processing and corrupts its state. So enqueue the payload
// into a lock-free SPSC ring and schedule a drain via
// btstack_run_loop_execute_on_main_thread() — the run-loop hop every platform
// HAL implements (nRF/ESP/CYW43) — which runs on the BTstack thread. On RP2040
// (cooperative run loop) this is the same context, just one iteration later.
// Single producer (main loop) + single consumer (run loop); both on one core,
// so volatile indices + a publish-after-copy are race-free under preemption.
// (The queue + indices are declared up by the mp_nus struct.)
// ---------------------------------------------------------------------------

// Runs on the BTstack run loop (BTstack thread) — drain the queue.
static void mp_tx_pump(void* ctx)
{
    (void)ctx;
    mp_tx_scheduled = false;                 // clear first so a late enqueue re-schedules
    while (mp_tx_head != mp_tx_tail) {
        mp_tx_slot_t* s = &mp_tx[mp_tx_tail % MP_TX_SLOTS];
        if (mp_nus.state == MP_NUS_READY && mp_nus.rx_value_handle != 0) {
            gatt_client_write_value_of_characteristic_without_response(
                mp_nus.handle, mp_nus.rx_value_handle, s->len, s->data);
        }
        mp_tx_tail++;
    }
}

bool btstack_host_mouthpad_nus_send(const uint8_t* data, uint16_t len)
{
    if (mp_nus.state != MP_NUS_READY || mp_nus.rx_value_handle == 0) return false;
    if (len == 0 || len > MP_TX_SLOT_SIZE) return false;
    if (mp_tx_head - mp_tx_tail >= MP_TX_SLOTS) return false;   // queue full

    mp_tx_slot_t* s = &mp_tx[mp_tx_head % MP_TX_SLOTS];
    memcpy(s->data, data, len);
    s->len = len;
    mp_tx_head++;                            // publish after copy

    if (!mp_tx_scheduled) {
        mp_tx_scheduled = true;
        mp_tx_cb.callback = &mp_tx_pump;
        mp_tx_cb.context  = NULL;
        btstack_run_loop_execute_on_main_thread(&mp_tx_cb);
    }
    return true;
}

// Forget the connected MouthPad's bond (the utility's clear_bonds relay command).
// forget_device touches gap_disconnect + le_device_db, so it must run on the
// BTstack thread — marshal it like the NUS write. Returns true if a connected
// MouthPad was captured for removal (the response success).
static btstack_context_callback_registration_t mp_clearbond_cb;
static bd_addr_t mp_clearbond_addr;
static volatile bool mp_clearbond_pending;

static void mp_clearbond_run(void* ctx)
{
    (void)ctx;
    mp_clearbond_pending = false;
    btstack_host_forget_device(mp_clearbond_addr);   // now on the BTstack thread
}

bool btstack_host_mouthpad_clear_bond(void)
{
    if (mp_nus.handle == HCI_CON_HANDLE_INVALID) return false;
    ble_connection_t* c = find_connection_by_handle(mp_nus.handle);
    if (!c) return false;
    memcpy(mp_clearbond_addr, c->addr, 6);
    if (!mp_clearbond_pending) {
        mp_clearbond_pending = true;
        mp_clearbond_cb.callback = &mp_clearbond_run;
        mp_clearbond_cb.context  = NULL;
        btstack_run_loop_execute_on_main_thread(&mp_clearbond_cb);
    }
    return true;
}

// ============================================================================
// BONDS LIST / REMOVE (management app, core0 -> core1 marshaled)
// ============================================================================
// Enumerating and mutating the LE device DB must run on the BTstack thread, so a
// core0 config command marshals a one-shot op to core1 (like mp_clearbond_run)
// and polls the *_done flag. Classic-BR/EDR bonds are managed by the triple-tap
// full wipe, not per-entry here.
static btstack_context_callback_registration_t bonds_cb;
static volatile bool bonds_op_pending;
static volatile bool bonds_op_done;
static volatile bool bonds_op_is_remove;
static volatile bool bonds_op_is_page;
static volatile int  bonds_remove_index;
static volatile int  bonds_page_start;
static volatile bool bonds_remove_ok;
static volatile bool bonds_list_complete;
static char bonds_list_json[MGMT_BONDS_RESPONSE_CAPACITY];
static bool btstack_host_forget_device_typed(const uint8_t bd_addr[6],
                                              int address_type,
                                              bool match_address_type);

static bool bonds_entry_at(void *context, int slot, mgmt_bond_entry_t *entry)
{
    if (!entry)
        return false;
    if (!btstack_host_le_bond_entry_at(context, slot, &entry->type,
                                       entry->address))
        return false;
    entry->index = slot;
    return true;
}

static void bonds_op_run(void *ctx)  // BTstack thread (core1)
{
    (void)ctx;
    if (bonds_op_is_remove) {
        int idx = bonds_remove_index;
        bonds_remove_ok = false;
        if (idx >= 0 && idx < le_device_db_max_count()) {
            int type = BD_ADDR_TYPE_UNKNOWN;
            bd_addr_t addr;
            le_device_db_info(idx, &type, addr, NULL);
            if (type != BD_ADDR_TYPE_UNKNOWN) {
                bonds_remove_ok = btstack_host_forget_device_typed(
                    addr, type, true);
            }
        }
    } else if (bonds_op_is_page) {
        mgmt_bonds_page_info_t info;
        size_t length = mgmt_bonds_format_page(
            bonds_entry_at, NULL, le_device_db_max_count(), bonds_page_start,
            bonds_list_json, sizeof(bonds_list_json), &info);
        if (length == 0) {
            // A page that cannot fit even one entry must not become an empty
            // successful reply: that would leave a client retrying the same
            // cursor forever.  Keep the failure compact for the wireless cap.
            strcpy(bonds_list_json,
                   "{\"error\":\"response_too_large\",\"code\":413}");
            bonds_list_complete = false;
        } else {
            bonds_list_complete = info.complete;
        }
    } else {
        bool complete = false;
        (void)mgmt_bonds_format_legacy(
            bonds_entry_at, NULL, le_device_db_max_count(),
            bonds_list_json, sizeof(bonds_list_json),
            &complete);
        bonds_list_complete = complete;
    }
    bonds_op_pending = false;
    bonds_op_done = true;
}

bool btstack_host_bonds_request(bool is_remove, int remove_index)
{
    if (bonds_op_pending) return false;
    bonds_op_is_remove = is_remove;
    bonds_op_is_page = false;
    bonds_remove_index = remove_index;
    bonds_op_done = false;
    bonds_op_pending = true;
    bonds_cb.callback = &bonds_op_run;
    bonds_cb.context = NULL;
    btstack_run_loop_execute_on_main_thread(&bonds_cb);
    return true;
}

bool btstack_host_bonds_request_list_page(int start_index)
{
    if (start_index < 0 || bonds_op_pending)
        return false;
    bonds_op_is_remove = false;
    bonds_op_is_page = true;
    bonds_page_start = start_index;
    bonds_op_done = false;
    bonds_op_pending = true;
    bonds_cb.callback = &bonds_op_run;
    bonds_cb.context = NULL;
    btstack_run_loop_execute_on_main_thread(&bonds_cb);
    return true;
}

bool btstack_host_bonds_done(void) { return bonds_op_done; }
const char *btstack_host_bonds_list_json(void) { return bonds_list_json; }
bool btstack_host_bonds_list_complete(void) { return bonds_list_complete; }
bool btstack_host_bonds_remove_ok(void) { return bonds_remove_ok; }

// ============================================================================
// PEER INVENTORY (management app, core0 -> core1 marshaled, read-only)
// ============================================================================
// Collects both security databases plus live connection state on the BTstack
// thread, hands them to mgmt_peers (which has no BTstack dependency) to be
// merged into logical peers, and formats one bounded page.
//
// The Classic link-key iterator hands back the link key itself. It is written
// into a local that is never read, never copied, and never leaves this
// function: mgmt_peer_bond_t has no field for it, which is what makes "no key
// bytes cross the management protocol" a property of the types rather than of
// this code remembering to be careful.
static btstack_context_callback_registration_t peers_cb;
static volatile bool peers_op_pending;
static volatile bool peers_op_done;
static volatile bool peers_op_ok;
static volatile int peers_page_start;
static volatile int peers_known_total;
static char peers_json[MGMT_PEERS_RESPONSE_CAPACITY];
// A forget rides the SAME single-slot operation as a list read, so the two can
// never run concurrently against the shared workspace, and a forget can never
// interleave with the enumeration that verifies it.
static volatile bool peers_op_is_forget;
static char peers_forget_id[MGMT_PEERS_ID_MAX];

static size_t peers_collect_bonds(mgmt_peer_bond_t *out, size_t capacity)
{
    size_t count = 0;

    for (int slot = 0; slot < le_device_db_max_count() && count < capacity; ++slot) {
        int type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t addr;
        if (!btstack_host_le_bond_entry_at(NULL, slot, &type, addr))
            continue;
        mgmt_peer_bond_t *bond = &out[count++];
        memset(bond, 0, sizeof(*bond));
        memcpy(bond->address, addr, sizeof(bond->address));
        bond->transport = MGMT_PEER_TRANSPORT_LE;
        bond->le_slot = (int16_t)slot;
        bond->le_address_type = (uint8_t)type;
        bond->classic_key_type = 0xFFu;
    }

#ifdef ENABLE_CLASSIC
    btstack_link_key_iterator_t it;
    if (gap_link_key_iterator_init(&it)) {
        bd_addr_t addr;
        link_key_t link_key;
        link_key_type_t key_type;
        while (count < capacity &&
               gap_link_key_iterator_get_next(&it, addr, link_key, &key_type)) {
            mgmt_peer_bond_t *bond = &out[count++];
            memset(bond, 0, sizeof(*bond));
            memcpy(bond->address, addr, sizeof(bond->address));
            bond->transport = MGMT_PEER_TRANSPORT_BREDR;
            bond->le_slot = -1;
            bond->le_address_type = 0xFFu;
            bond->classic_key_type = (uint8_t)key_type;
        }
        gap_link_key_iterator_done(&it);
        // The iterator requires somewhere to put the key; nothing here reads it,
        // and mgmt_peer_bond_t has no field it could be copied into. Wiped
        // anyway so a stack frame that held real key material does not simply
        // get reused with it still in place.
        memset(link_key, 0, sizeof(link_key));
    }
#endif
    return count;
}

/*
 * What the adapter's own driver stack decided a live peer IS.
 *
 * This is the second entry in the design's name hierarchy (HLD S20) and it
 * outranks the remote-supplied name for a reason: the remote name is whatever
 * the device chose to call itself, while the driver identity is the answer
 * this firmware reached from VID/PID, HID descriptor and class of device --
 * the same decision that determines how the controller is actually parsed.
 *
 * Nothing is invented when no driver has claimed the device. An empty
 * classification means "this adapter cannot say", which is the honest answer
 * for a peer that is bonded but not connected, and the caller omits the field
 * entirely rather than sending an empty one.
 *
 * Read on the BTstack thread, which is the same thread that owns the bthid
 * device table, so no snapshot or lock is needed here.
 */
static void peers_identity_for(const uint8_t addr[6], mgmt_peer_observation_t *o)
{
    for (uint8_t slot = 0; slot < BTHID_MAX_DEVICES; ++slot) {
        bthid_device_t *device = bthid_get_device_slot(slot);
        if (!device || !device->active)
            continue;
        if (memcmp(device->bd_addr, addr, 6) != 0)
            continue;
        const bthid_driver_t *driver = (const bthid_driver_t *)device->driver;
        // A generic-fallback name while the PnP SDP query is still outstanding
        // is provisional, and saying it out loud is what left a DualSense
        // recorded as the generic gamepad: the companion reads this inventory
        // once, the instant pairing completes, which is inside that window.
        if (driver && driver->name &&
            mgmt_peers_classification_publishable(
                bthid_device_is_generic_fallback(device),
                classic_identity_unresolved(addr))) {
            mgmt_peers_sanitize_name(driver->name, o->classification,
                                     sizeof(o->classification));
        }
        o->vendor_id = device->vendor_id;
        o->product_id = device->product_id;
        // The bthid table's name is the one SDP/inquiry actually resolved. Only
        // used when the transport's own connection record has none, so a
        // resolved name is never replaced by a staler copy of itself.
        if (o->name[0] == '\0')
            mgmt_peers_sanitize_name(device->name, o->name, sizeof(o->name));
        return;
    }
}

static void peers_add_observation(mgmt_peer_observation_t *out, size_t capacity,
                                  size_t *count, const uint8_t addr[6],
                                  const char *name, bool connected,
                                  bool management, bool bridge, bool direct,
                                  bool last_connected)
{
    if (*count >= capacity)
        return;
    mgmt_peer_observation_t *o = &out[(*count)++];
    memset(o, 0, sizeof(*o));
    memcpy(o->address, addr, sizeof(o->address));
    o->connected = connected ? 1u : 0u;
    o->is_management_client = management ? 1u : 0u;
    o->is_bridge_source = bridge ? 1u : 0u;
    o->is_direct_source = direct ? 1u : 0u;
    o->is_last_connected = last_connected ? 1u : 0u;
    if (name)
        mgmt_peers_sanitize_name(name, o->name, sizeof(o->name));
    peers_identity_for(addr, o);
}

// Is this address the Android Controller Bridge rather than a real controller?
// The arbiter is the authority: the bridge is identified from its HID
// descriptor, which only the input seam sees, and the answer is recorded there
// as a source class.
static bool peers_source_class_for(const uint8_t addr[6], bool *is_bridge)
{
    ns2_input_arbiter_status_t status;
    ns2_active_input_status(&status);
    for (unsigned i = 0; i < status.source_count &&
                         i < NS2_INPUT_ARBITER_MAX_SOURCES; ++i) {
        const ns2_input_source_info_t *source = &status.sources[i];
        if (!source->present || !source->key.stable_addr_valid)
            continue;
        if (memcmp(source->key.stable_addr, addr, 6) != 0)
            continue;
        if (is_bridge)
            *is_bridge = source->source_class == NS2_INPUT_SOURCE_CLASS_BRIDGE;
        return true;
    }
    return false;
}

static size_t peers_collect_observations(mgmt_peer_observation_t *out,
                                         size_t capacity)
{
    size_t count = 0;

    // (1) The management companion. Certain while it is connected, and checked
    //     first so a phone that is ALSO driving Controller Link is never
    //     reported as a controller.
    if (config_ble.handle != HCI_CON_HANDLE_INVALID) {
        // Reported under the companion's DURABLE identity, not the address this
        // ACL happens to be using. Under LE privacy those differ, and keying
        // the observation on the RPA split one phone into two logical peers:
        // its bonded identity (Classic + LE, role unknown, nothing connected)
        // and an unbonded RPA (role management, no stored key). The user saw
        // both, and the identity half landed under saved controllers because
        // nothing could prove it was the phone.
        //
        // Merging is by address, so emitting the identity is what collapses
        // them: the observation now lands on the same row as the bond record,
        // giving one peer with role=management and the real transport mask.
        bd_addr_t companion;
        if (config_ble_durable_addr(companion)) {
            peers_add_observation(out, capacity, &count, companion,
                                  NULL, true, true, false, false, false);
        }
    }

    // (2) Live Classic HID links. The arbiter says which of them is the bridge.
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; ++i) {
        const classic_connection_t *c = &classic_state.connections[i];
        if (!c->active)
            continue;
        bool bridge = false;
        bool known = peers_source_class_for(c->addr, &bridge);
        peers_add_observation(out, capacity, &count, c->addr, c->name, true,
                              false, known && bridge, !(known && bridge), false);
    }

    // (3) Live BLE HID links. Always controllers; the management peripheral is
    //     a different table and was handled above.
    for (int i = 0; i < MAX_BLE_CONNECTIONS; ++i) {
        const ble_connection_t *c = &hid_state.connections[i];
        if (c->handle == HCI_CON_HANDLE_INVALID)
            continue;
        peers_add_observation(out, capacity, &count, c->addr, c->name, true,
                              false, false, true, false);
    }

    // (4) The stored reconnect record. It only ever holds a controller, so it
    //     is real evidence about a peer that is not currently connected --
    //     which is otherwise the case where nothing can be said at all.
    if (hid_state.has_last_connected) {
        peers_add_observation(out, capacity, &count, hid_state.last_connected_addr,
                              hid_state.last_connected_name, false,
                              false, false, false, true);
    }
    return count;
}

/*
 * Peer-enumeration workspace, deliberately NOT on the call stack.
 *
 * A full inventory is three bounded arrays: 32 bond rows, 32 observations and
 * 32 merged peers. Since the peer record grew a classification and VID/PID
 * those total 6080 bytes, and as stack locals they made peers_op_run()'s frame
 * 6132 bytes.
 *
 * That is safe on Pico 2 W, which runs core 1 on an explicit 48 KiB stack, and
 * NOT safe on Pico W, where core 1 gets the SDK default PICO_CORE1_STACK_SIZE
 * of 2048 bytes in SCRATCH_X. Measured from the linked image: core 1 has 4096
 * bytes of SCRATCH_X plus a 2292-byte unallocated gap below it before the heap
 * begins, so 6388 bytes of headroom -- and the frame plus the nested
 * mgmt_peers_format_page() frame (408 bytes) and snprintf crossed it.
 *
 * Static storage is the right answer rather than a bigger stack: the arrays are
 * a fixed, known size, they are only ever touched from the BTstack thread (this
 * callback is the sole user, serialised by peers_op_pending), and moving them
 * costs the same RAM it would have cost transiently while making the depth
 * independent of which board is running. Peer semantics are unchanged --
 * identical arrays, identical capacities, identical calls.
 */
static mgmt_peer_bond_t peers_ws_bonds[MGMT_PEERS_MAX_ENTRIES];
static mgmt_peer_observation_t peers_ws_observed[MGMT_PEERS_MAX_ENTRIES];
static mgmt_peer_t peers_ws_peers[MGMT_PEERS_MAX_ENTRIES];

// The whole point of the workspace is that it is not on the stack; a future
// edit that shrinks it back onto the frame has to come past this.
_Static_assert(sizeof(peers_ws_bonds) + sizeof(peers_ws_observed) +
                   sizeof(peers_ws_peers) > 2048u,
               "peer workspace is larger than the Pico W core-1 stack and must "
               "stay in static storage");

// Rebuild the logical inventory into the shared workspace. Both the list read
// and the forget verification need exactly this, and a forget that verified
// against a differently-built view would not be verifying anything.
static size_t peers_build_inventory(void)
{
    size_t bond_count = peers_collect_bonds(peers_ws_bonds, MGMT_PEERS_MAX_ENTRIES);
    size_t observation_count =
        peers_collect_observations(peers_ws_observed, MGMT_PEERS_MAX_ENTRIES);
    return mgmt_peers_merge(peers_ws_bonds, bond_count, peers_ws_observed,
                            observation_count, peers_ws_peers,
                            MGMT_PEERS_MAX_ENTRIES);
}

/*
 * Forget one logical peer, atomically, on the BTstack thread.
 *
 * "Atomic" here means the whole sequence -- resolve, guard, disconnect, delete
 * both transports, verify -- runs inside one run-loop callback with nothing else
 * able to touch the security databases in between. That is the point of doing it
 * in firmware rather than as an app-issued disconnect-then-delete pair, which
 * another connection could race (design section 63).
 *
 * The peer id is a one-way hash of the identity address, so the only way to
 * resolve it is to rebuild the inventory and recompute each id. That is a
 * feature: a client can address only a peer the adapter itself just reported,
 * and it cannot smuggle in a raw address.
 */
static void peers_forget_run(void)
{
    size_t peer_count = peers_build_inventory();
    int index = mgmt_peers_find_by_id(peers_ws_peers, peer_count, peers_forget_id);

    if (index < 0) {
        // No record for that id. Idempotent success: a management reply can be
        // lost after the command has already run, and a retry must not report a
        // failure for work that is done.
        peers_op_ok = mgmt_peers_format_forget_result(
            peers_forget_id, MGMT_PEER_FORGET_ALREADY_ABSENT, 0,
            peers_json, sizeof(peers_json)) > 0;
        return;
    }

    const mgmt_peer_t *target = &peers_ws_peers[index];
    bd_addr_t addr;
    memcpy(addr, target->address, sizeof(addr));

    // Two independent guards for the same rule, because getting this wrong cuts
    // off the app issuing the command. The address test is structural and cannot
    // be fooled by a classification mistake; the role test also catches the
    // Controller Link relationship, which is the same phone wearing its other
    // hat. Clearing the companion's credential is a separate, explicitly named
    // action and is deliberately not reachable from the controller list.
    if (config_ble_addr_is_companion(addr) ||
        target->role == (uint8_t)MGMT_PEER_ROLE_MANAGEMENT_COMPANION ||
        target->role == (uint8_t)MGMT_PEER_ROLE_CONTROLLER_LINK) {
        printf("[BTSTACK_HOST] Refusing to forget the management companion\n");
        peers_op_ok = false;
        (void)mgmt_peers_format_forget_result(
            peers_forget_id, MGMT_PEER_FORGET_PROTECTED, target->transport,
            peers_json, sizeof(peers_json));
        return;
    }

    // Address-only form on purpose: one physical device, every credential it
    // holds. The typed form exists to remove ONE transport of a cross-transport
    // peer, which is not what "forget this controller" means -- a peer left with
    // half its keys reconnects on the surviving transport and looks like the
    // forget silently failed (design section 91).
    (void)btstack_host_forget_device_typed(addr, BD_ADDR_TYPE_UNKNOWN, false);

    // Verify by re-enumeration rather than trusting the delete. This is the step
    // that makes a partial failure visible instead of leaving the app's cache
    // claiming a pairing is gone while the adapter still holds one.
    size_t after_count = peers_build_inventory();
    int still = mgmt_peers_find_by_id(peers_ws_peers, after_count, peers_forget_id);
    unsigned remaining = 0;
    if (still >= 0)
        remaining = peers_ws_peers[still].transport;

    mgmt_peer_forget_outcome_t outcome = remaining == 0
        ? MGMT_PEER_FORGET_REMOVED
        : MGMT_PEER_FORGET_INCOMPLETE;
    if (outcome == MGMT_PEER_FORGET_INCOMPLETE) {
        printf("[BTSTACK_HOST] Forget incomplete: %s still holds transports 0x%02X\n",
               peers_forget_id, remaining);
    }
    peers_op_ok = outcome == MGMT_PEER_FORGET_REMOVED;
    if (mgmt_peers_format_forget_result(peers_forget_id, outcome, remaining,
                                        peers_json, sizeof(peers_json)) == 0) {
        peers_op_ok = false;
    }
}

// ============================================================================
// REMOTE CONTROLLER PAIRING (management -> the one pairing state machine)
// ============================================================================
//
// There is no second pairing flow here. `open_pairing_window()` in
// ns2_bt_host.c is the authoritative operation and the BOOTSEL gesture's only
// entry point; this records a request that the SAME state machine picks up on
// its next control tick, so the radio behaves identically whichever trigger
// started it (design §32).
//
// Everything below is core1-owned. The request flag is written from the
// run-loop callback dispatched by core0, and read by control_timer_handler(),
// which is itself a run-loop timer -- so both sit in one context and need no
// synchronisation beyond that.
static uint32_t pairing_operation_id;
static uint8_t pairing_state = MGMT_PAIRING_IDLE;
static uint8_t pairing_reason = MGMT_PAIRING_REASON_NONE;
static uint8_t pairing_candidates;
static uint32_t pairing_deadline_ms;
static bool pairing_remote_start_pending;
static bool pairing_remote_cancel_pending;

void btstack_host_pairing_note_state(uint8_t state, uint8_t reason,
                                     uint32_t deadline_ms)
{
    pairing_state = state;
    pairing_reason = reason;
    pairing_deadline_ms = deadline_ms;
    if (state == MGMT_PAIRING_DISCOVERING)
        pairing_candidates = 0;
}

void btstack_host_pairing_note_candidate(void)
{
    if (pairing_candidates < 255u &&
        (pairing_state == MGMT_PAIRING_DISCOVERING ||
         pairing_state == MGMT_PAIRING_CONNECTING)) {
        pairing_candidates++;
    }
}

bool btstack_host_pairing_take_remote_start(void)
{
    bool wanted = pairing_remote_start_pending;
    pairing_remote_start_pending = false;
    return wanted;
}

bool btstack_host_pairing_take_remote_cancel(void)
{
    bool wanted = pairing_remote_cancel_pending;
    pairing_remote_cancel_pending = false;
    return wanted;
}

uint32_t btstack_host_pairing_begin_operation(void)
{
    return ++pairing_operation_id;
}

void btstack_host_pairing_snapshot(mgmt_pairing_snapshot_t *out, uint32_t now_ms)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->operation = pairing_operation_id;
    out->state = pairing_state;
    out->reason = pairing_reason;
    out->candidates = pairing_candidates;
    bool active = pairing_state == MGMT_PAIRING_DISCOVERING ||
                  pairing_state == MGMT_PAIRING_CONNECTING;
    out->remaining_ms = mgmt_pairing_remaining_ms(pairing_deadline_ms, now_ms, active);
}

bool btstack_host_pairing_active(void)
{
    return pairing_state == MGMT_PAIRING_DISCOVERING ||
           pairing_state == MGMT_PAIRING_CONNECTING;
}

/*
 * Can this adapter still store a bond of ANY kind?
 *
 * "Full" means both stores are full: a Classic-only controller can still be
 * paired while the LE database is full and vice versa, so refusing on either
 * alone would block pairings that would have worked. Only when neither
 * transport has a free slot is there genuinely nowhere to put a new
 * relationship (design §90).
 *
 * Counted from the same enumeration the peer inventory uses, so this answer
 * cannot drift from what the user is shown.
 */
static bool btstack_host_security_storage_full(void)
{
    mgmt_peer_bond_t bonds[MGMT_PEERS_MAX_ENTRIES];
    size_t count = peers_collect_bonds(bonds, MGMT_PEERS_MAX_ENTRIES);
    size_t le_used = 0;
    size_t classic_used = 0;
    for (size_t i = 0; i < count; ++i) {
        if (bonds[i].transport & MGMT_PEER_TRANSPORT_LE) le_used++;
        if (bonds[i].transport & MGMT_PEER_TRANSPORT_BREDR) classic_used++;
    }
    bool le_full = le_used >= (size_t)le_device_db_max_count();
#ifdef ENABLE_CLASSIC
    bool classic_full = classic_used >= (size_t)NVM_NUM_LINK_KEYS;
#else
    bool classic_full = true;
#endif
    return le_full && classic_full;
}

// Deferred management reply, same single-slot shape as peers/bonds.
static btstack_context_callback_registration_t pairing_cb;
static volatile bool pairing_op_pending;
static volatile bool pairing_op_done;
static volatile bool pairing_op_ok;
static volatile uint8_t pairing_op_action;
static char pairing_json[MGMT_PAIRING_RESPONSE_CAPACITY];

static void pairing_op_run(void *ctx)   // BTstack thread (core1)
{
    (void)ctx;
    uint32_t now = btstack_run_loop_get_time_ms();
    mgmt_pairing_action_t action = (mgmt_pairing_action_t)pairing_op_action;

    if (action == MGMT_PAIRING_START) {
        mgmt_pairing_reason_t why = MGMT_PAIRING_REASON_NONE;
        // `already_active` covers a window the USER opened with the gesture as
        // well as one a client started: it is the same window, and re-arming it
        // remotely would silently extend something the user did physically.
        if (!mgmt_pairing_start_allowed(g_mgmt_enabled,
                                        btstack_host_pairing_active(),
                                        pairing_lockout,
                                        btstack_host_security_storage_full(),
                                        &why)) {
            mgmt_pairing_snapshot_t blocked;
            btstack_host_pairing_snapshot(&blocked, now);
            blocked.state = MGMT_PAIRING_BLOCKED;
            blocked.reason = (uint8_t)why;
            pairing_op_ok = false;
            (void)mgmt_pairing_format_status(&blocked, pairing_json,
                                             sizeof(pairing_json));
        } else {
            // The control tick performs the actual open, so one code path --
            // the gesture's -- still owns every radio side effect.
            pairing_remote_start_pending = true;
            pairing_operation_id++;
            pairing_state = MGMT_PAIRING_DISCOVERING;
            pairing_reason = MGMT_PAIRING_REASON_NONE;
            pairing_candidates = 0;
            pairing_deadline_ms = now + MGMT_PAIRING_WINDOW_MS;
            mgmt_pairing_snapshot_t started;
            btstack_host_pairing_snapshot(&started, now);
            pairing_op_ok = mgmt_pairing_format_status(&started, pairing_json,
                                                       sizeof(pairing_json)) > 0;
        }
    } else if (action == MGMT_PAIRING_CANCEL) {
        // Idempotent by design (§64): cancelling when idle is a success that
        // reports idle, because the caller asked for an end state and that is
        // the end state.
        if (btstack_host_pairing_active()) {
            pairing_remote_cancel_pending = true;
            pairing_state = MGMT_PAIRING_CANCELLED;
            pairing_reason = MGMT_PAIRING_REASON_NONE;
            pairing_deadline_ms = now;
        }
        mgmt_pairing_snapshot_t cancelled;
        btstack_host_pairing_snapshot(&cancelled, now);
        pairing_op_ok = mgmt_pairing_format_status(&cancelled, pairing_json,
                                                   sizeof(pairing_json)) > 0;
    } else {
        mgmt_pairing_snapshot_t status;
        btstack_host_pairing_snapshot(&status, now);
        pairing_op_ok = mgmt_pairing_format_status(&status, pairing_json,
                                                   sizeof(pairing_json)) > 0;
    }

    if (!pairing_op_ok && pairing_json[0] == '\0')
        strcpy(pairing_json, "{\"error\":\"response_too_large\",\"code\":413}");
    pairing_op_pending = false;
    pairing_op_done = true;
}

bool btstack_host_pairing_request(uint8_t action)
{
    if (pairing_op_pending) return false;
    pairing_op_action = action;
    pairing_op_done = false;
    pairing_op_ok = false;
    pairing_op_pending = true;
    pairing_cb.callback = &pairing_op_run;
    pairing_cb.context = NULL;
    btstack_run_loop_execute_on_main_thread(&pairing_cb);
    return true;
}

bool btstack_host_pairing_done(void) { return pairing_op_done; }
const char *btstack_host_pairing_json(void) { return pairing_json; }

static void peers_op_run(void *ctx)  // BTstack thread (core1)
{
    (void)ctx;
    if (peers_op_is_forget) {
        peers_forget_run();
        peers_op_pending = false;
        peers_op_done = true;
        return;
    }

    mgmt_peer_t *peers = peers_ws_peers;
    size_t peer_count = peers_build_inventory();
    peers_known_total = (int)peer_count;

    mgmt_peers_page_info_t info;
    size_t length = mgmt_peers_format_page(peers, peer_count, peers_page_start,
                                           peers_json, sizeof(peers_json), &info);
    if (length == 0) {
        strcpy(peers_json, "{\"error\":\"response_too_large\",\"code\":413}");
        peers_op_ok = false;
    } else {
        peers_op_ok = true;
    }
    peers_op_pending = false;
    peers_op_done = true;
}

bool btstack_host_peers_request_forget(const char *peer_id)
{
    if (!mgmt_peers_id_valid(peer_id) || peers_op_pending)
        return false;
    snprintf(peers_forget_id, sizeof(peers_forget_id), "%s", peer_id);
    peers_op_is_forget = true;
    peers_op_done = false;
    peers_op_ok = false;
    peers_op_pending = true;
    peers_cb.callback = &peers_op_run;
    peers_cb.context = NULL;
    btstack_run_loop_execute_on_main_thread(&peers_cb);
    return true;
}

bool btstack_host_peers_request_page(int start_peer)
{
    if (start_peer < 0 || peers_op_pending)
        return false;
    peers_op_is_forget = false;
    peers_page_start = start_peer;
    peers_op_done = false;
    peers_op_ok = false;
    peers_op_pending = true;
    peers_cb.callback = &peers_op_run;
    peers_cb.context = NULL;
    btstack_run_loop_execute_on_main_thread(&peers_cb);
    return true;
}

bool btstack_host_peers_done(void) { return peers_op_done; }
const char *btstack_host_peers_json(void) { return peers_json; }
bool btstack_host_peers_ok(void) { return peers_op_ok; }
int btstack_host_peers_total(void) { return peers_known_total; }

// ============================================================================
// GATT CLIENT
// ============================================================================

static void gatt_client_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case GATT_EVENT_SERVICE_QUERY_RESULT: {
            gatt_client_service_t service;
            gatt_event_service_query_result_get_service(packet, &service);
            printf("[BTSTACK_HOST] GATT: Service 0x%04X-0x%04X UUID=0x%04X\n",
                   service.start_group_handle, service.end_group_handle,
                   service.uuid16);
            // Save HID service handles (UUID 0x1812)
            if (service.uuid16 == 0x1812) {
                hid_state.hid_service_start = service.start_group_handle;
                hid_state.hid_service_end = service.end_group_handle;
                printf("[BTSTACK_HOST] Found HID Service!\n");
            }
            break;
        }

        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            gatt_client_characteristic_t characteristic;
            gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
            printf("[BTSTACK_HOST] GATT: Char handle=0x%04X value=0x%04X end=0x%04X props=0x%02X UUID=0x%04X\n",
                   characteristic.start_handle, characteristic.value_handle,
                   characteristic.end_handle, characteristic.properties, characteristic.uuid16);
            // Save first Report characteristic (UUID 0x2A4D) with Notify property
            if (characteristic.uuid16 == 0x2A4D && (characteristic.properties & 0x10) &&
                hid_state.report_characteristic.value_handle == 0) {
                hid_state.report_characteristic = characteristic;
                printf("[BTSTACK_HOST] Found HID Report characteristic!\n");
            }
            break;
        }

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t status = gatt_event_query_complete_get_att_status(packet);
            printf("[BTSTACK_HOST] GATT: Query complete, status=0x%02X, gatt_state=%d\n",
                   status, hid_state.gatt_state);

            if (status != 0) break;

            // State machine for GATT discovery
            if (hid_state.gatt_state == GATT_DISCOVERING_SERVICES) {
                if (hid_state.hid_service_start != 0) {
                    // Found HID, now discover its characteristics
                    printf("[BTSTACK_HOST] Discovering HID characteristics...\n");
                    hid_state.gatt_state = GATT_DISCOVERING_HID_CHARACTERISTICS;
                    gatt_client_discover_characteristics_for_handle_range_by_uuid16(
                        gatt_client_callback, hid_state.gatt_handle,
                        hid_state.hid_service_start, hid_state.hid_service_end,
                        0x2A4D);  // HID Report UUID
                } else {
                    printf("[BTSTACK_HOST] No HID service found!\n");
                }
            } else if (hid_state.gatt_state == GATT_DISCOVERING_HID_CHARACTERISTICS) {
                if (hid_state.report_characteristic.value_handle != 0) {
                    // Found Report char, enable notifications
                    printf("[BTSTACK_HOST] Enabling notifications on 0x%04X (end=0x%04X)...\n",
                           hid_state.report_characteristic.value_handle,
                           hid_state.report_characteristic.end_handle);
                    hid_state.gatt_state = GATT_ENABLING_NOTIFICATIONS;
                    gatt_client_write_client_characteristic_configuration(
                        gatt_client_callback, hid_state.gatt_handle,
                        &hid_state.report_characteristic,
                        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                } else {
                    printf("[BTSTACK_HOST] No HID Report characteristic found!\n");
                }
            } else if (hid_state.gatt_state == GATT_ENABLING_NOTIFICATIONS) {
                printf("[BTSTACK_HOST] Notifications enabled! Ready for HID reports.\n");
                hid_state.gatt_state = GATT_READY;
            }
            break;
        }

        case GATT_EVENT_NOTIFICATION: {
            hci_con_handle_t con_handle = gatt_event_notification_get_handle(packet);
            uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
            uint16_t value_length = gatt_event_notification_get_value_length(packet);
            const uint8_t *value = gatt_event_notification_get_value(packet);

            // BLE HID Report characteristic (Xbox uses handle 0x001E)
            // Route through bthid layer
            if (value_handle == 0x001E && value_length >= 1) {
                int conn_index = get_ble_conn_index_by_handle(con_handle);
                if (conn_index >= 0) {
                    route_ble_hid_report(
                        (uint8_t)conn_index,
                        bthid_get_connection_generation((uint8_t)conn_index),
                        value, value_length);
                }
            }
            break;
        }
    }
}

// ============================================================================
// DIRECT XBOX HID NOTIFICATION HANDLER
// ============================================================================

// Handle notifications directly from gatt_client listener API
static void ble_hid_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    hci_con_handle_t con_handle = gatt_event_notification_get_handle(packet);
    uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
    uint16_t value_length = gatt_event_notification_get_value_length(packet);
    const uint8_t *value = gatt_event_notification_get_value(packet);

    // Debug: log notification shapes only in diagnostic builds. Alternating
    // report lengths otherwise turn this into an unbounded production path.
#ifdef NS2_DIAG
    static uint16_t last_handle = 0;
    static uint16_t last_len = 0;
    if (value_handle != last_handle || value_length != last_len) {
        printf("[BTSTACK_HOST] BLE notif: handle=0x%04X len=%d data=%02X %02X %02X %02X\n",
               value_handle, value_length,
               value_length > 0 ? value[0] : 0,
               value_length > 1 ? value[1] : 0,
               value_length > 2 ? value[2] : 0,
               value_length > 3 ? value[3] : 0);
        last_handle = value_handle;
        last_len = value_length;
    }
#endif

    // Accept HID report notifications - filter by reasonable gamepad report length
    if (value_length < 10 || value_length > sizeof(pending_ble_report)) return;

    // Get conn_index for this BLE connection
    int conn_index = get_ble_conn_index_by_handle(con_handle);
    if (conn_index < 0) return;

    // Register the source before deferring its report so the arbiter can gate
    // it even when a normalized event has not arrived yet.
    ns2_active_input_note_connection((uint8_t)conn_index);
    const uint32_t connection_generation =
        ns2_active_input_connection_generation((uint8_t)conn_index);
    if (!ns2_active_input_connection_is_active_generation(
            (uint8_t)conn_index, connection_generation)) return;

    // Defer processing to main loop to avoid stack overflow
    memcpy(pending_ble_report, value, value_length);
    pending_ble_report_len = value_length;
    pending_ble_conn_index = (uint8_t)conn_index;
    pending_ble_connection_generation = connection_generation;
    ble_report_pending = true;
}

// Register direct listener for BLE HID notifications and notify bthid layer
static void register_ble_hid_listener(hci_con_handle_t con_handle)
{
    printf("[BTSTACK_HOST] Registering BLE HID listener for handle 0x%04X\n", con_handle);

    // Find the BLE connection
    ble_connection_t* conn = find_connection_by_handle(con_handle);
    if (!conn) {
        printf("[BTSTACK_HOST] ERROR: No connection for handle 0x%04X\n", con_handle);
        return;
    }

    // Assign conn_index if not already set
    int ble_index = -1;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (&hid_state.connections[i] == conn) {
            ble_index = i;
            break;
        }
    }
    if (ble_index < 0) return;

    conn->conn_index = BLE_CONN_INDEX_OFFSET + ble_index;
    conn->hid_ready = true;

    // Set up a fake characteristic structure with just the value_handle
    // Xbox BLE HID Report characteristic value handle is 0x001E
    memset(&xbox_hid_characteristic, 0, sizeof(xbox_hid_characteristic));
    xbox_hid_characteristic.value_handle = 0x001E;
    xbox_hid_characteristic.end_handle = 0x001F;  // Approximate

    // Register to listen for notifications on the HID report characteristic
    gatt_client_listen_for_characteristic_value_updates(
        &xbox_hid_notification_listener,
        ble_hid_notification_handler,
        con_handle,
        &xbox_hid_characteristic);

    printf("[BTSTACK_HOST] BLE HID listener registered, conn_index=%d\n", conn->conn_index);

    // Notify bthid layer that device is ready
    btstack_host_stop_scan();
    scan_timeout_end = 0;
    printf("[BTSTACK_HOST] Calling bt_on_hid_ready(%d) for BLE device '%s'\n", conn->conn_index, conn->name);
    bt_on_hid_ready(conn->conn_index);
}

// ============================================================================
// SWITCH 2 BLE HID NOTIFICATION HANDLER
// ============================================================================

// Switch 2 ATT handles (from protocol documentation)
#define SW2_INPUT_REPORT_HANDLE     0x000A  // Input reports via notification
#define SW2_CCC_HANDLE              0x000B  // Client Characteristic Configuration
#define SW2_OUTPUT_REPORT_HANDLE    0x0012  // Rumble output
#define SW2_CMD_HANDLE              0x0014  // Command output
#define SW2_ACK_CCC_HANDLE          0x001B  // ACK notification CCC
#define SW2_NFC_COMMAND_HANDLE      0x0016  // Extended NFC command write
#define SW2_NFC_RESPONSE_HANDLE     0x001E  // Extended NFC response notification
#define SW2_NFC_CCC_HANDLE          0x001F  // Extended NFC response CCC

// Declared with the ATT-handle block because the primary 0x000A notification
// callback appears before the later report-selection state machine.
static volatile bool s_sw2_imuref_requested;
static volatile bool s_sw2_imuref_active;
static volatile bool s_sw2_imuref_transition_pending;
static bool s_sw2_imuref_transition_target;
static bool s_sw2_imuref_awaiting_unsubscribe;
static volatile uint8_t s_sw2_imuref_last_att_status;
static volatile bool s_sw2_imuref_dual_requested;
static volatile bool s_sw2_imuref_dual_active;
static volatile bool s_sw2_imuref_dual_transition_pending;
static volatile uint8_t s_sw2_imuref_dual_att_status;
static volatile uint16_t s_sw2_imuref_interval_pending_units;
static volatile uint16_t s_sw2_imuref_interval_target_units;
static volatile uint8_t s_sw2_imuref_interval_request_status;
static volatile uint32_t s_sw2_imuref_common_notifications;
static volatile uint32_t s_sw2_imuref_native_notifications;

// Opt-in motion-enable experiment (see sw2_capture.h) — UNVERIFIED for this device. Handle
// numbering guessed from switch2_input_viewer.py's second input-report path plus this repo's own
// observed characteristic/CCC handle pairing pattern (0x000A/0x000B, 0x0019/0x001A); never
// independently confirmed via GATT discovery against a real Pro Controller 2.
#define SW2_MOTION_HANDLE           0x000E  // UNVERIFIED: candidate richer/IMU input report
#define SW2_MOTION_CCC_HANDLE       0x000F  // ASSUMED via the value_handle+1 CCC pattern

// Pro Controller 2 firmware 2.x headset transport. These handles were found by
// this repo's own live GATT discovery and independently match decrypted
// controller traffic: 0x002C is host->controller speaker/haptic audio, 0x002E
// is the extended controller->host input/mic report, and 0x002F is its CCC.
#define SW2_PRO2_AUDIO_OUTPUT_HANDLE 0x002C
#define SW2_PRO2_AUDIO_INPUT_HANDLE  0x002E
#define SW2_PRO2_AUDIO_CCC_HANDLE    0x002F

// Switch 2 command constants
#define SW2_CMD_PAIRING             0x15
#define SW2_CMD_SET_LED             0x09
#define SW2_CMD_READ_SPI            0x02
#define SW2_REQ_TYPE_REQ            0x91
#define SW2_REQ_INT_BLE             0x01
#define SW2_SUBCMD_SET_LED          0x07
#define SW2_SUBCMD_READ_SPI         0x04
// Pairing subcmds - sent in order: STEP1 -> STEP2 -> STEP3 -> STEP4
// Note: Response ACK contains same subcmd as request
#define SW2_SUBCMD_PAIRING_STEP1    0x01  // Send BD address
#define SW2_SUBCMD_PAIRING_STEP2    0x04  // Send magic bytes 1
#define SW2_SUBCMD_PAIRING_STEP3    0x02  // Send magic bytes 2
#define SW2_SUBCMD_PAIRING_STEP4    0x03  // Complete pairing

// Fresh-SYNC custom ATT initialization states, confirmed against live hardware.
typedef enum {
    SW2_INIT_IDLE = 0,
    SW2_INIT_READ_INFO,             // Read device info from SPI
    SW2_INIT_READ_LTK,              // Read the controller's existing bond key
    SW2_INIT_PAIR_STEP1,            // Pairing step 1 (BD addr)
    SW2_INIT_PAIR_STEP2,            // Pairing step 2
    SW2_INIT_PAIR_STEP3,            // Pairing step 3
    SW2_INIT_PAIR_STEP4,            // Pairing step 4
    SW2_INIT_READ_NEW_LTK,          // Read authoritative key written by pairing
    SW2_INIT_SET_LED,               // Set player LED
    SW2_INIT_DONE                   // Init complete
} sw2_init_state_t;

// Handle Switch 2 HID notifications
static void switch2_hid_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    hci_con_handle_t con_handle = gatt_event_notification_get_handle(packet);
    uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
    uint16_t value_length = gatt_event_notification_get_value_length(packet);
    const uint8_t *value = gatt_event_notification_get_value(packet);

    // Non-invasive raw capture (see sw2_capture.h) — the COMPLETE, unmodified notification,
    // ahead of any parsing/filtering below. This is what removes the "bytes 16-59 discarded"
    // blind spot: every byte this callback ever receives is now capturable, not just the
    // buttons/sticks window process_report() happens to read.
    sw2_capture_record(SW2_CAP_INPUT_NOTIFY, value_handle, value, value_length);

    // Debug first notification
    static bool sw2_notif_debug = false;
    if (!sw2_notif_debug) {
        printf("[SW2_BLE] Notification: handle=0x%04X len=%d data=%02X %02X %02X %02X\n",
               value_handle, value_length,
               value_length > 0 ? value[0] : 0,
               value_length > 1 ? value[1] : 0,
               value_length > 2 ? value[2] : 0,
               value_length > 3 ? value[3] : 0);
        sw2_notif_debug = true;
    }

    if (__atomic_load_n(&s_sw2_imuref_requested, __ATOMIC_ACQUIRE) &&
        value_handle == SW2_INPUT_REPORT_HANDLE)
        __atomic_add_fetch(
            &s_sw2_imuref_common_notifications, 1u, __ATOMIC_RELAXED);

    // Switch 2 input reports are 64 bytes on handle 0x000A
    if (value_handle != SW2_INPUT_REPORT_HANDLE) return;
    if (value_length < 16 || value_length > sizeof(pending_ble_report)) return;

    // Get conn_index for this BLE connection
    int conn_index = get_ble_conn_index_by_handle(con_handle);
    if (conn_index < 0) return;

    ns2_active_input_note_connection((uint8_t)conn_index);
    const uint32_t connection_generation =
        ns2_active_input_connection_generation((uint8_t)conn_index);
    if (!ns2_active_input_connection_is_active_generation(
            (uint8_t)conn_index, connection_generation)) return;

    // Defer processing to main loop to avoid stack overflow
    memcpy(pending_ble_report, value, value_length);
    pending_ble_report_len = value_length;
    pending_ble_conn_index = (uint8_t)conn_index;
    pending_ble_connection_generation = connection_generation;
    ble_report_pending = true;
}

// Forward declarations for Switch 2
static void switch2_send_next_init_cmd(hci_con_handle_t con_handle);
static bool switch2_resume_encrypted_session(hci_con_handle_t con_handle);
static void switch2_send_player_led(hci_con_handle_t con_handle, uint8_t pattern);
static uint8_t sw2_last_player_led;

static void switch2_publish_hid_ready(hci_con_handle_t con_handle)
{
    ble_connection_t *conn = find_connection_by_handle(con_handle);
    if (!conn) return;

    // Driver selection must see the resolved identity before readiness.
    printf("[SW2_BLE] Updating device info: VID=0x%04X PID=0x%04X\n",
           conn->vid, conn->pid);
    bthid_update_device_info(conn->conn_index, conn->name, conn->vid, conn->pid);
    btstack_host_stop_scan();
    scan_timeout_end = 0;
    printf("[SW2_BLE] Calling bt_on_hid_ready(%d) for Switch 2 device\n",
           conn->conn_index);
    bt_on_hid_ready(conn->conn_index);
}

// CCC write completion handler for Switch 2 input reports
static void switch2_ccc_write_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) return;

    uint8_t status = gatt_event_query_complete_get_att_status(packet);
    hci_con_handle_t handle = gatt_event_query_complete_get_handle(packet);

    if (status == ATT_ERROR_SUCCESS) {
        printf("[SW2_BLE] Input notifications enabled for handle 0x%04X\n", handle);

        if (switch2_link_encrypted && switch2_link_encrypted_handle == handle) {
            // Player LEDs are controller-local state and return to the running
            // search pattern after a controller power cycle even though the
            // bonded SM session, input, and motion have resumed. Reassert the
            // one-controller dongle's P1 assignment after the input CCC
            // transaction completes and before native setup begins.
            sw2_last_player_led = 0x01;
            switch2_send_player_led(handle, 0x01);
        }

        switch2_publish_hid_ready(handle);
    } else {
        printf("[SW2_BLE] Failed to enable input notifications: status=0x%02X\n", status);
    }
}

// CCC write completion handler for Switch 2 ACK notifications
static void switch2_ack_ccc_write_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) return;

    uint8_t status = gatt_event_query_complete_get_att_status(packet);
    hci_con_handle_t handle = gatt_event_query_complete_get_handle(packet);

    if (status == ATT_ERROR_SUCCESS) {
        printf("[SW2_BLE] ACK notifications enabled for handle 0x%04X\n", handle);

        if (switch2_resume_encrypted_session(handle)) {
            return;
        }

        // Do not overlap another ATT request with the command state machine.
        // The prior code launched the input-CCC write and then emitted the
        // first write-command ~sub-millisecond later while the CCC transaction
        // was still outstanding. Fresh links happened to tolerate it; captured
        // encrypted HOME links silently discarded every command. Complete the
        // command/ACK sequence first and enable input only at its terminal ACK.
        printf("[SW2_BLE] Starting serialized controller init\n");
        switch2_send_next_init_cmd(handle);
    } else {
        printf("[SW2_BLE] Failed to enable ACK notifications: status=0x%02X\n", status);
    }
}

// Switch 2 init state machine
static sw2_init_state_t sw2_init_state = SW2_INIT_IDLE;
static hci_con_handle_t sw2_init_handle = 0;

// Switch 2's custom ATT pairing writes the link-layer key into its SPI bond
// table. Live reads confirm the authoritative 16 bytes at 0x1FA01A after
// pairing. BTstack's LE database uses
// the opposite (human/crypto) byte order and reverses it again when formatting
// that HCI command, so `normalized` below is always reverse(`raw`). The live
// bytes remain private to this translation unit. Ordinary diagnostics expose
// only validity and comparison results, never either key representation.
static void switch2_record_pairing_ltk(hci_con_handle_t con_handle,
                                       const uint8_t raw_ltk[16], uint8_t phase)
{
    uint8_t derived[16];
    ns2_pairing_derive_ltk(SW2_BLE_HOST_A1, SW2_BLE_DEVICE_B1, derived);

    memcpy(sw2_pairing_ltk_raw, raw_ltk, sizeof(sw2_pairing_ltk_raw));
    for (size_t i = 0; i < sizeof(sw2_pairing_ltk_normalized); ++i) {
        sw2_pairing_ltk_normalized[i] = raw_ltk[15u - i];
    }
    sw2_pairing_ltk_handle = con_handle;
    sw2_pairing_ltk_phase = phase;
    sw2_pairing_ltk_valid = true;
    sw2_pairing_ltk_matches_derived =
        memcmp(sw2_pairing_ltk_normalized, derived, sizeof(derived)) == 0;
    sw2_pairing_ltk_raw_matches_derived =
        memcmp(sw2_pairing_ltk_raw, derived, sizeof(derived)) == 0;
    sw2_pairing_ltk_reads++;

    printf("[SW2_BLE] SPI LTK %s: normalized=%s derived, raw=%s derived\n",
           phase == 2 ? "after pairing" : "before pairing",
           sw2_pairing_ltk_matches_derived ? "matches" : "differs from",
           sw2_pairing_ltk_raw_matches_derived ? "matches" : "differs from");
}

// Retry timing for the init state machine (see switch2_send_init_cmd()/switch2_retry_init_if_needed()
// below). SW2_INIT_RETRY_INTERVAL_MS is this project's pre-existing ~500ms retry intent — no primary
// source or capture was found to establish that
// value; it is preserved as an existing project policy, not promoted to a measured fact. What *is*
// fixed here: the interval used to be a raw call-count modulo (assumed ~120Hz caller; the real
// caller — ns2_bt_host.c's 30ms control_timer — runs at ~33Hz, so the old check fired every ~1.8s,
// not ~500ms), and the counter was never reset on a state transition, disconnect, or new session, so
// it also carried an essentially random phase into each new pairing step. Replaced with a real
// monotonic deadline (btstack_run_loop_get_time_ms(), the same clock this file already uses for the
// BLE reconnect-attempt timeout) that starts fresh every time a command is actually sent, plus a
// bounded retry count with an explicit recovery transition (see switch2_retry_init_if_needed()).
#define SW2_INIT_RETRY_INTERVAL_MS 500
#define SW2_INIT_MAX_RETRIES 10  // ~5s of retries at the interval above before forcing a recovery disconnect
static uint32_t sw2_init_cmd_sent_ms = 0;
static uint8_t sw2_init_retry_count = 0;
static sw2_init_state_t sw2_init_last_sent_state = SW2_INIT_IDLE;
static uint32_t s_sw2_init_done_ms;

static bool switch2_resume_encrypted_session(hci_con_handle_t con_handle)
{
    if (!switch2_link_encrypted || switch2_link_encrypted_handle != con_handle) {
        return false;
    }

    /*
     * HOME reconnect resumes an established controller session. Captures show
     * the encrypted controller ignores first-connection READ_INFO and pairing
     * commands, while current BLE clients restore subscriptions and consume
     * input immediately. The full init sequence remains exclusive to a fresh
     * SYNC connection.
     */
    sw2_init_state = SW2_INIT_DONE;
    s_sw2_init_done_ms = btstack_run_loop_get_time_ms();
    uint8_t state = (uint8_t)SW2_INIT_DONE;
    sw2_capture_record(SW2_CAP_STATE, 0, &state, 1);

    static uint8_t input_ccc_enable[] = { 0x01, 0x00 };
    printf("[SW2_BLE] Resuming encrypted session; enabling input notifications\n");
    sw2_capture_record(SW2_CAP_CCC_WRITE, SW2_CCC_HANDLE,
                       input_ccc_enable, sizeof(input_ccc_enable));
    gatt_client_write_value_of_characteristic(
        switch2_ccc_write_callback, con_handle, SW2_CCC_HANDLE,
        sizeof(input_ccc_enable), input_ccc_enable);
    return true;
}

// ACK notification listener for Switch 2 commands
static gatt_client_notification_t switch2_ack_notification_listener;
static gatt_client_characteristic_t switch2_ack_characteristic;

// Read-only command-0x10 version probe requested by the out-of-band UART
// tooling. The request is marshalled onto the BTstack run loop; the raw reply
// is published before READY with release/acquire ordering for core0.
static uint8_t s_sw2_version_state = NS2_BT_VERSION_IDLE;
static uint8_t s_sw2_version_length;
static uint8_t s_sw2_version_raw[12];
static btstack_context_callback_registration_t s_sw2_version_cb;

static void switch2_version_probe_run(void *context) {
    (void)context;
    if (sw2_init_state != SW2_INIT_DONE || sw2_init_handle == 0) {
        __atomic_store_n(&s_sw2_version_state, NS2_BT_VERSION_NO_CONTROLLER,
                         __ATOMIC_RELEASE);
        return;
    }

    uint8_t command[] = {
        0x10, SW2_REQ_TYPE_REQ, SW2_REQ_INT_BLE, 0x01, 0x00, 0x00, 0x00, 0x00};
    s_sw2_version_length = 0;
    __atomic_store_n(&s_sw2_version_state, NS2_BT_VERSION_SENT, __ATOMIC_RELEASE);
    sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, command, sizeof(command));
    gatt_client_write_value_of_characteristic_without_response(
        sw2_init_handle, SW2_CMD_HANDLE, sizeof(command), command);
}

void ns2_bt_version_probe_request(void) {
    __atomic_store_n(&s_sw2_version_state, NS2_BT_VERSION_REQUESTED,
                     __ATOMIC_RELEASE);
    s_sw2_version_cb.callback = switch2_version_probe_run;
    s_sw2_version_cb.context = NULL;
    btstack_run_loop_execute_on_main_thread(&s_sw2_version_cb);
}

void ns2_bt_version_probe_snapshot(ns2_bt_version_result_t *out) {
    ns2_bt_version_state_t state = (ns2_bt_version_state_t)__atomic_load_n(
        &s_sw2_version_state, __ATOMIC_ACQUIRE);
    out->state = state;
    out->length = 0;
    memset(out->raw, 0, sizeof(out->raw));
    if (state == NS2_BT_VERSION_READY || state == NS2_BT_VERSION_PROTOCOL_ERROR) {
        out->length = s_sw2_version_length;
        memcpy(out->raw, s_sw2_version_raw, sizeof(out->raw));
    }
}

const char *ns2_bt_version_state_name(ns2_bt_version_state_t state) {
    switch (state) {
        case NS2_BT_VERSION_IDLE: return "idle";
        case NS2_BT_VERSION_REQUESTED: return "requested";
        case NS2_BT_VERSION_SENT: return "sent";
        case NS2_BT_VERSION_READY: return "ready";
        case NS2_BT_VERSION_NO_CONTROLLER: return "no_controller";
        case NS2_BT_VERSION_PROTOCOL_ERROR: return "protocol_error";
        default: return "unknown";
    }
}

// ============================================================================
// ONE-SHOT GATT DISCOVERY — ground truth for raw ATT handle numbering (see sw2_capture.h)
// ============================================================================

#define SW2_GATT_DISC_MAX_SERVICES 8
#define SW2_GATT_DISC_MAX_CHARS    32

typedef enum {
    SW2_GATT_DISC_IDLE = 0,
    SW2_GATT_DISC_SERVICES,
    SW2_GATT_DISC_CHARS,
    SW2_GATT_DISC_DESCS,
    SW2_GATT_DISC_DONE,
} sw2_gatt_disc_state_t;

static volatile bool s_sw2_gatt_disc_enabled = false;
static bool s_sw2_gatt_disc_fired = false;  // per-connection one-shot guard
static sw2_gatt_disc_state_t s_gatt_disc_state = SW2_GATT_DISC_IDLE;
static gatt_client_service_t s_gatt_disc_services[SW2_GATT_DISC_MAX_SERVICES];
static uint8_t s_gatt_disc_num_services, s_gatt_disc_svc_idx;
static gatt_client_characteristic_t s_gatt_disc_chars[SW2_GATT_DISC_MAX_CHARS];
static uint8_t s_gatt_disc_num_chars, s_gatt_disc_char_idx;

void sw2_set_gatt_discovery_enabled(bool on) {
    s_sw2_gatt_disc_enabled = on;
    if (!on) s_sw2_gatt_disc_fired = false;
}

bool sw2_get_gatt_discovery_enabled(void) {
    return s_sw2_gatt_disc_enabled;
}

static void sw2_gatt_disc_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event = hci_event_packet_get_type(packet);

    switch (event) {
        case GATT_EVENT_SERVICE_QUERY_RESULT: {
            if (s_gatt_disc_num_services < SW2_GATT_DISC_MAX_SERVICES) {
                gatt_client_service_t svc;
                gatt_event_service_query_result_get_service(packet, &svc);
                s_gatt_disc_services[s_gatt_disc_num_services++] = svc;
                uint8_t data[20];
                data[0] = (uint8_t)(svc.end_group_handle & 0xFF);
                data[1] = (uint8_t)(svc.end_group_handle >> 8);
                data[2] = (uint8_t)(svc.uuid16 & 0xFF);
                data[3] = (uint8_t)(svc.uuid16 >> 8);
                memcpy(&data[4], svc.uuid128, 16);
                sw2_capture_record(SW2_CAP_GATT_SVC, svc.start_group_handle, data, sizeof(data));
            }
            break;
        }
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            gatt_client_characteristic_t ch;
            gatt_event_characteristic_query_result_get_characteristic(packet, &ch);
            if (s_gatt_disc_num_chars < SW2_GATT_DISC_MAX_CHARS) {
                s_gatt_disc_chars[s_gatt_disc_num_chars++] = ch;
            }
            uint8_t data[24];
            data[0] = (uint8_t)(ch.start_handle & 0xFF);
            data[1] = (uint8_t)(ch.start_handle >> 8);
            data[2] = (uint8_t)(ch.end_handle & 0xFF);
            data[3] = (uint8_t)(ch.end_handle >> 8);
            data[4] = (uint8_t)(ch.properties & 0xFF);
            data[5] = (uint8_t)(ch.properties >> 8);
            data[6] = (uint8_t)(ch.uuid16 & 0xFF);
            data[7] = (uint8_t)(ch.uuid16 >> 8);
            memcpy(&data[8], ch.uuid128, 16);
            sw2_capture_record(SW2_CAP_GATT_CHAR, ch.value_handle, data, sizeof(data));
            break;
        }
        case GATT_EVENT_ALL_CHARACTERISTIC_DESCRIPTORS_QUERY_RESULT: {
            gatt_client_characteristic_descriptor_t d;
            gatt_event_all_characteristic_descriptors_query_result_get_characteristic_descriptor(packet, &d);
            uint8_t data[18];
            data[0] = (uint8_t)(d.uuid16 & 0xFF);
            data[1] = (uint8_t)(d.uuid16 >> 8);
            memcpy(&data[2], d.uuid128, 16);
            sw2_capture_record(SW2_CAP_GATT_DESC, d.handle, data, sizeof(data));
            break;
        }
        case GATT_EVENT_QUERY_COMPLETE: {
            hci_con_handle_t con = gatt_event_query_complete_get_handle(packet);
            uint8_t status = gatt_event_query_complete_get_att_status(packet);
            if (s_gatt_disc_state == SW2_GATT_DISC_SERVICES) {
                printf("[SW2_GATT_DISC] %d services discovered (status=0x%02X)\n",
                       s_gatt_disc_num_services, status);
                s_gatt_disc_svc_idx = 0;
                s_gatt_disc_num_chars = 0;
                if (s_gatt_disc_num_services > 0) {
                    s_gatt_disc_state = SW2_GATT_DISC_CHARS;
                    gatt_client_discover_characteristics_for_service(
                        sw2_gatt_disc_handler, con, &s_gatt_disc_services[0]);
                } else {
                    s_gatt_disc_state = SW2_GATT_DISC_DONE;
                }
            } else if (s_gatt_disc_state == SW2_GATT_DISC_CHARS) {
                s_gatt_disc_svc_idx++;
                if (s_gatt_disc_svc_idx < s_gatt_disc_num_services) {
                    gatt_client_discover_characteristics_for_service(
                        sw2_gatt_disc_handler, con, &s_gatt_disc_services[s_gatt_disc_svc_idx]);
                } else {
                    printf("[SW2_GATT_DISC] %d characteristics discovered\n", s_gatt_disc_num_chars);
                    s_gatt_disc_char_idx = 0;
                    if (s_gatt_disc_num_chars > 0) {
                        s_gatt_disc_state = SW2_GATT_DISC_DESCS;
                        gatt_client_discover_characteristic_descriptors(
                            sw2_gatt_disc_handler, con, &s_gatt_disc_chars[0]);
                    } else {
                        s_gatt_disc_state = SW2_GATT_DISC_DONE;
                    }
                }
            } else if (s_gatt_disc_state == SW2_GATT_DISC_DESCS) {
                s_gatt_disc_char_idx++;
                if (s_gatt_disc_char_idx < s_gatt_disc_num_chars) {
                    gatt_client_discover_characteristic_descriptors(
                        sw2_gatt_disc_handler, con, &s_gatt_disc_chars[s_gatt_disc_char_idx]);
                } else {
                    s_gatt_disc_state = SW2_GATT_DISC_DONE;
                    printf("[SW2_GATT_DISC] Done: %d services, %d characteristics (+ their descriptors)\n",
                           s_gatt_disc_num_services, s_gatt_disc_num_chars);
                }
            }
            break;
        }
        default:
            break;
    }
}

// One-shot: walk every primary service -> every characteristic in it -> every descriptor of each
// characteristic, capturing raw ATT handles + UUIDs as ground truth. See sw2_capture.h.
static void switch2_run_gatt_discovery(hci_con_handle_t con_handle)
{
    printf("[SW2_GATT_DISC] Starting full GATT discovery on handle 0x%04X\n", con_handle);
    s_gatt_disc_num_services = 0;
    s_gatt_disc_num_chars = 0;
    s_gatt_disc_svc_idx = 0;
    s_gatt_disc_char_idx = 0;
    s_gatt_disc_state = SW2_GATT_DISC_SERVICES;
    gatt_client_discover_primary_services(sw2_gatt_disc_handler, con_handle);
}

// ============================================================================
// V2 FEATURE-ENABLE EXPERIMENT MATRIX (see sw2_capture.h)
// ============================================================================

// The console writes {0x85, 0x00} to descriptor 0x0010 immediately before enabling the native
// 0x000E input stream. Our earlier 0x000C arithmetic guess accepted a write but left the stream at
// 30 ms; the server research export identifies 0x0010 explicitly, and live GATT discovery is now
// exposed over UART and confirmed against the controller's actual attribute table.
#define SW2_NATIVE_REPORT_RATE_HANDLE 0x0010

typedef struct {
    uint32_t address;
    uint8_t  size;
} sw2_spi_read_t;

// Exact address/size list from switch2_input_viewer.py's connection sequence, in its exact order
// (primary stick cal, secondary stick cal, user cal, gyro cal, accel/mag cal, pairing data).
static const sw2_spi_read_t SW2_V2_CAL_READS[] = {
    { 0x13080,  0x40 },
    { 0x130C0,  0x40 },
    { 0x1FC040, 0x40 },
    { 0x13040,  0x10 },
    { 0x13100,  0x18 },
    { 0x1FA000, 0x40 },
};
#define SW2_V2_CAL_COUNT (sizeof(SW2_V2_CAL_READS) / sizeof(SW2_V2_CAL_READS[0]))

// Motion-relevant reads observed directly over this project's console UART bridge during a
// genuine Pro Controller 2 / report-0x09 enumeration. This deliberately excludes the BLE bond
// table read used by the reference PC viewer and includes the console's 0x13060 read instead.
static const sw2_spi_read_t SW2_V2_CONSOLE_CAL_READS[] = {
    { 0x13080,  0x40 },
    { 0x130C0,  0x40 },
    { 0x1FC040, 0x40 },
    { 0x13040,  0x10 },
    { 0x13100,  0x18 },
    { 0x13060,  0x20 },
};
#define SW2_V2_CONSOLE_CAL_COUNT \
    (sizeof(SW2_V2_CONSOLE_CAL_READS) / sizeof(SW2_V2_CONSOLE_CAL_READS[0]))

typedef struct {
    uint8_t     id;
    const char *name;
    uint8_t     configure_flags;
    uint8_t     enable_flags;
    bool        do_cal_reads;
    bool        do_handle_write;
    bool        disable_all_first; // send 0x0C/0x05 mask 0xFF before configuring this run
    bool        defer_ccc_subscribe;  // if true, the 0x000E CCC subscribe happens LAST, not first
    bool        use_console_cal_reads; // use the reads captured from console report-0x09 init
    bool        request_fast_link; // request a 7.5ms BLE interval after the final CCC write
    bool        subscribe_common_input; // target report-0x05 CCC 0x000B instead of native 0x000F
    uint8_t     cal_read_limit; // zero = complete selected list; raw reference uses first three
    bool        send_common_report_select; // exact 0x0A/0x02 payload 0x00000003
} sw2_v2_variant_t;

static const sw2_v2_variant_t SW2_V2_VARIANTS[] = {
    { 1, "control",              0x07, 0x07, false, false, false, false, false, false },
    { 2, "mask_ff",              0xFF, 0xFF, false, false, false, false, false, false },
    { 3, "handle_write_only",    0x07, 0x07, false, true,  false, false, false, false },
    { 4, "mask_ff_handle_write", 0xFF, 0xFF, false, true,  false, false, false, false },
    { 5, "calibration_seq",      0x07, 0x07, true,  false, false, false, false, false },
    { 6, "full_sequence",        0xFF, 0x07, true,  true,  false, true,  false, false },
    // Establish a clean feature state before requesting buttons + sticks + IMU. The bitmap
    // selects the length-0x1E live-orientation payload over the length-0x28 packed multi-sample
    // IMU payload; earlier variants only added feature bits and could inherit the 0x28 form from
    // a prior run. (0x28 was long believed to be magnetometer data. That is refuted -- it is a
    // packed multi-sample IMU record. See docs/experiments/pro2-carrier-unknown-fields-2026-07-31.md.)
    { 7, "reset_then_imu",       0x07, 0x07, false, false, true,  false, false, false },
    // Grounded in the console-side UART trace, not a third-party viewer: configure and enable
    // exactly 0x27, perform the exact six calibration reads the console made, write 0x0085 to
    // the report-rate descriptor, then subscribe to the native Pro2 report last.
    { 8, "console_motion_core",   0x27, 0x27, true,  true,  false, true,  true,  false },
    // Same console-grounded controller setup as variant 8. The only added variable is a
    // central-side HCI update to the minimum standard BLE interval (6 * 1.25ms = 7.5ms), made
    // after the native-report CCC write completes. The real console reportedly uses a vendor
    // path below the standard minimum; this test deliberately does not depend on that quirk.
    { 9, "console_motion_fast_link", 0x27, 0x27, true, true, false, true, true, true },
};
#define SW2_V2_VARIANT_COUNT (sizeof(SW2_V2_VARIANTS) / sizeof(SW2_V2_VARIANTS[0]))

// Production profile for a genuine Pro Controller 2 source. Keep this named separately from
// the UART-selectable RE matrix: variant 9 remains available for repeatable captures, but normal
// controller operation must not depend on an array position or an armed experiment.
static const sw2_v2_variant_t SW2_NATIVE_PRO2_PROFILE = {
    9, "native_pro2", 0x27, 0x27, true, true, false, true, true, true
};

// Exact control shape from btle_procon2_motion_0x000A.pcapng: feature 0x2F,
// the first three reference calibration reads, report-rate descriptor 0x0010,
// and finally the common report-0x05 CCC at 0x000B. This remains UART-only;
// production startup continues to select SW2_NATIVE_PRO2_PROFILE above.
static const sw2_v2_variant_t SW2_RAW_IMU_REFERENCE_PROFILE = {
    10, "raw_imu_reference", 0x2F, 0x2F, true, true, false, true, false, false,
    true, 3, true
};

typedef enum {
    SW2_V2_IDLE = 0,
    SW2_V2_CCC_SUBSCRIBED,       // waiting for the (non-deferred) CCC write to complete
    SW2_V2_DISABLE_SENT,          // waiting for disable-all ACK (cmd=0x0C, subcmd=0x05)
    SW2_V2_REPORT_SELECT_SENT,    // waiting for exact common-report selector ACK (0x0A/0x02)
    SW2_V2_CONFIGURE_SENT,       // waiting for the configure ACK (cmd=0x0C, subcmd=0x02)
    SW2_V2_CAL_READ,             // waiting for a SPI-read ACK (cmd=0x02, subcmd=0x04)
    SW2_V2_ENABLE_SENT,          // waiting for the enable ACK (cmd=0x0C, subcmd=0x04)
    SW2_V2_HANDLE_WRITE_SENT,    // waiting for the descriptor-write completion
    SW2_V2_CCC_SUBSCRIBED_LATE,  // waiting for the deferred (variant 6) CCC write to complete
    SW2_V2_DONE,
} sw2_v2_state_t;

static volatile uint8_t s_sw2_v2_armed_variant = 0;  // 0 = off
static bool s_sw2_v2_fired = false;                  // per-connection one-shot guard
static bool s_sw2_native_auto_fired = false;         // production Pro2 path, reset on disconnect
static volatile uint32_t s_sw2_native_auto_checks;
static volatile uint32_t s_sw2_native_auto_starts;
static volatile uint32_t s_sw2_native_auto_wait_elapsed_ms;
static volatile uint16_t s_sw2_native_auto_source_pid;
static volatile uint8_t s_sw2_native_auto_personality;
static volatile uint8_t s_sw2_native_auto_block_mask;
static sw2_v2_state_t s_sw2_v2_state = SW2_V2_IDLE;
static const sw2_v2_variant_t *s_sw2_v2_active = NULL;
static uint8_t s_sw2_v2_cal_index = 0;
static volatile bool s_sw2_magraw_requested;
static volatile bool s_sw2_magraw_active;
static volatile bool s_sw2_magraw_transition_pending;
static bool s_sw2_magraw_transition_target;
static bool s_sw2_magraw_reference_running;
static bool s_sw2_magraw_awaiting_input_ccc;
static uint8_t s_sw2_magraw_reference_step;
static uint8_t s_sw2_magraw_reference_result;
static uint8_t s_sw2_magraw_last_response_status;
static uint8_t s_sw2_magraw_input_ccc_status;
static uint32_t s_sw2_magraw_command_sent_ms;
static gatt_client_notification_t sw2_motion_notification_listener;
static gatt_client_characteristic_t sw2_motion_characteristic;
static void sw2_motion_notification_handler(uint8_t packet_type, uint16_t channel,
                                            uint8_t *packet, uint16_t size);

typedef enum {
    SW2_PRO2_AUDIO_OFF = 0,
    SW2_PRO2_AUDIO_SUBSCRIBE_PENDING,
    SW2_PRO2_AUDIO_ACTIVE,
    SW2_PRO2_AUDIO_UNSUBSCRIBE_PENDING,
    SW2_PRO2_AUDIO_RESTORE_MOTION_PENDING,
    SW2_PRO2_AUDIO_ERROR,
} sw2_pro2_audio_state_t;

static volatile bool s_sw2_pro2_audio_requested;
static volatile sw2_pro2_audio_state_t s_sw2_pro2_audio_state = SW2_PRO2_AUDIO_OFF;
static volatile uint8_t s_sw2_pro2_audio_last_att_status;
static volatile uint8_t s_sw2_pro2_audio_last_headset_raw;
static volatile uint8_t s_sw2_pro2_audio_last_data_len;
static volatile uint16_t s_sw2_pro2_audio_last_report_len;
static volatile uint16_t s_sw2_pro2_audio_max_report_len;
static volatile uint32_t s_sw2_pro2_audio_notifications;
static volatile uint32_t s_sw2_pro2_audio_compact_failures;
static volatile uint32_t s_sw2_motion_last_notification_us;
static gatt_client_notification_t sw2_pro2_audio_notification_listener;
static gatt_client_characteristic_t sw2_pro2_audio_characteristic;

// UART-gated genuine NFC mirror. The handles above come from this
// repository's own live GATT discovery. This path only captures the genuine
// response; it does not replace the console-facing response yet.
static volatile bool s_sw2_nfc_mirror_requested;
static volatile bool s_sw2_nfc_mirror_initiator;
static volatile ns2_nfc_mirror_state_t s_sw2_nfc_mirror_state =
    NS2_NFC_MIRROR_OFF;
static volatile uint8_t s_sw2_nfc_mirror_last_att_status;
static volatile uint8_t s_sw2_nfc_mirror_last_send_status;
static volatile uint8_t s_sw2_nfc_mirror_last_command;
static volatile uint8_t s_sw2_nfc_mirror_last_subcommand;
static volatile uint8_t s_sw2_nfc_mirror_report_state;
static volatile uint16_t s_sw2_nfc_mirror_last_response_length;
static volatile uint32_t s_sw2_nfc_mirror_commands_submitted;
static volatile uint32_t s_sw2_nfc_mirror_commands_sent;
static volatile uint32_t s_sw2_nfc_mirror_notifications;
static volatile uint32_t s_sw2_nfc_mirror_report_state_transitions;
static volatile uint32_t s_sw2_nfc_mirror_rejected;
static volatile bool s_sw2_nfc_mirror_slot_claimed;
static volatile bool s_sw2_nfc_mirror_command_pending;
static volatile bool s_sw2_nfc_mirror_awaiting_response;
static volatile bool s_sw2_nfc_mirror_response_ready;
static uint8_t s_sw2_nfc_mirror_command[NS2_NFC_MIRROR_COMMAND_MAX];
static size_t s_sw2_nfc_mirror_command_length;
static uint8_t s_sw2_nfc_mirror_response[NS2_NFC_MIRROR_RESPONSE_MAX];
static size_t s_sw2_nfc_mirror_response_length;
static uint8_t s_sw2_nfc_mirror_send_failures;
static uint32_t s_sw2_nfc_mirror_next_send_ms;
static uint32_t s_sw2_nfc_mirror_awaiting_since_ms;
static volatile uint32_t s_sw2_nfc_mirror_response_timeouts;
static gatt_client_notification_t sw2_nfc_mirror_notification_listener;
static gatt_client_characteristic_t sw2_nfc_mirror_characteristic;

typedef enum {
    SW2_PRO2_REPLAY_IDLE = 0,
    SW2_PRO2_REPLAY_SETUP_ONE,
    SW2_PRO2_REPLAY_SETUP_TWO,
    SW2_PRO2_REPLAY_PRIME,
    SW2_PRO2_REPLAY_AUDIO,
    SW2_PRO2_REPLAY_TAIL,
    SW2_PRO2_REPLAY_DONE,
    SW2_PRO2_REPLAY_ERROR,
} sw2_pro2_replay_state_t;

static volatile bool s_sw2_pro2_replay_requested;
static volatile sw2_pro2_replay_state_t s_sw2_pro2_replay_state;
static volatile uint8_t s_sw2_pro2_replay_last_send_status;
static volatile uint16_t s_sw2_pro2_replay_frames_sent;
static uint8_t s_sw2_pro2_replay_index;
static uint8_t s_sw2_pro2_replay_silence_count;
static uint8_t s_sw2_pro2_replay_send_failures;
static bool s_sw2_pro2_replay_stream4_next;
static uint32_t s_sw2_pro2_replay_next_us;

typedef enum {
    SW2_PRO2_LIVE_IDLE = 0,
    SW2_PRO2_LIVE_SETUP_ONE,
    SW2_PRO2_LIVE_SETUP_TWO,
    SW2_PRO2_LIVE_STREAM4,
    SW2_PRO2_LIVE_STREAM2,
    SW2_PRO2_LIVE_ERROR,
} sw2_pro2_live_state_t;

static volatile bool s_sw2_pro2_live_requested;
static volatile sw2_pro2_live_state_t s_sw2_pro2_live_state;
static volatile uint8_t s_sw2_pro2_live_last_send_status;
static volatile uint8_t s_sw2_pro2_live_last_toc;
static uint8_t s_sw2_pro2_live_prefix[6];
static volatile uint32_t s_sw2_pro2_live_frames_sent;
static volatile uint32_t s_sw2_pro2_live_underruns;
static volatile uint8_t s_sw2_pro2_live_prime_count;
static uint8_t s_sw2_pro2_live_send_failures;
static uint32_t s_sw2_pro2_live_next_us;

void btstack_host_request_switch2_pro2_audio_live(bool enabled)
{
    __atomic_store_n(&s_sw2_pro2_live_requested, enabled, __ATOMIC_RELEASE);
    if (enabled) {
        // Diagnostic replay and live PCM own the same 0x002C stream and must
        // never be interleaved.
        __atomic_store_n(&s_sw2_pro2_replay_requested, false,
                         __ATOMIC_RELEASE);
        s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_IDLE;
    } else {
        s_sw2_pro2_live_state = SW2_PRO2_LIVE_IDLE;
        ds5_audio_bridge_set_switch2_pro2_active(false);
    }
}

void btstack_host_request_switch2_pro2_audio_replay(bool enabled)
{
    if (enabled) btstack_host_request_switch2_pro2_audio_live(false);
    __atomic_store_n(&s_sw2_pro2_replay_requested, enabled, __ATOMIC_RELEASE);
    if (!enabled || s_sw2_pro2_replay_state == SW2_PRO2_REPLAY_DONE ||
        s_sw2_pro2_replay_state == SW2_PRO2_REPLAY_ERROR)
        s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_IDLE;
}

void btstack_host_set_switch2_pro2_audio_capture(bool enabled)
{
    __atomic_store_n(&s_sw2_pro2_audio_requested, enabled, __ATOMIC_RELEASE);
    // A failed ATT operation is deliberately latched so the 1 ms feedback
    // service cannot hammer the GATT queue. An explicit off command rearms it.
    if (!enabled && s_sw2_pro2_audio_state == SW2_PRO2_AUDIO_ERROR)
        s_sw2_pro2_audio_state = SW2_PRO2_AUDIO_OFF;
    if (!enabled) {
        btstack_host_request_switch2_pro2_audio_replay(false);
        btstack_host_request_switch2_pro2_audio_live(false);
    }
}

void btstack_host_get_switch2_pro2_audio_diag(btstack_host_pro2_audio_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    ble_connection_t *conn = find_connection_by_handle(sw2_init_handle);
    out->requested = __atomic_load_n(&s_sw2_pro2_audio_requested, __ATOMIC_ACQUIRE);
    out->active = s_sw2_pro2_audio_state == SW2_PRO2_AUDIO_ACTIVE;
    out->state = (uint8_t)s_sw2_pro2_audio_state;
    out->last_att_status = s_sw2_pro2_audio_last_att_status;
    out->last_headset_raw = s_sw2_pro2_audio_last_headset_raw;
    out->last_audio_length = s_sw2_pro2_audio_last_data_len;
    out->source_pid = conn ? conn->pid : 0;
    out->connection_handle = sw2_init_handle;
    out->last_report_length = s_sw2_pro2_audio_last_report_len;
    out->max_report_length = s_sw2_pro2_audio_max_report_len;
    out->notifications = s_sw2_pro2_audio_notifications;
    out->compact_failures = s_sw2_pro2_audio_compact_failures;
    out->replay_requested = __atomic_load_n(
        &s_sw2_pro2_replay_requested, __ATOMIC_ACQUIRE);
    out->replay_active = s_sw2_pro2_replay_state != SW2_PRO2_REPLAY_IDLE &&
                         s_sw2_pro2_replay_state != SW2_PRO2_REPLAY_DONE &&
                         s_sw2_pro2_replay_state != SW2_PRO2_REPLAY_ERROR;
    out->replay_state = (uint8_t)s_sw2_pro2_replay_state;
    out->replay_last_send_status = s_sw2_pro2_replay_last_send_status;
    out->replay_frames_sent = s_sw2_pro2_replay_frames_sent;
    out->live_requested = __atomic_load_n(
        &s_sw2_pro2_live_requested, __ATOMIC_ACQUIRE);
    out->live_active = s_sw2_pro2_live_state != SW2_PRO2_LIVE_IDLE &&
                       s_sw2_pro2_live_state != SW2_PRO2_LIVE_ERROR;
    out->live_state = (uint8_t)s_sw2_pro2_live_state;
    out->live_last_send_status = s_sw2_pro2_live_last_send_status;
    out->live_last_toc = s_sw2_pro2_live_last_toc;
    memcpy(out->live_prefix, s_sw2_pro2_live_prefix,
           sizeof(out->live_prefix));
    out->live_frames_sent = s_sw2_pro2_live_frames_sent;
    out->live_underruns = s_sw2_pro2_live_underruns;
    out->live_prime_count = s_sw2_pro2_live_prime_count;
}

void ns2_nfc_mirror_request(bool enabled)
{
    if (enabled) {
        __atomic_store_n(
            &s_sw2_nfc_mirror_report_state, 0, __ATOMIC_RELEASE);
        __atomic_store_n(
            &s_sw2_nfc_mirror_command_pending, false, __ATOMIC_RELEASE);
        __atomic_store_n(
            &s_sw2_nfc_mirror_awaiting_response, false, __ATOMIC_RELEASE);
        __atomic_store_n(
            &s_sw2_nfc_mirror_response_ready, false, __ATOMIC_RELEASE);
        __atomic_store_n(
            &s_sw2_nfc_mirror_slot_claimed, false, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(
            &s_sw2_nfc_mirror_initiator, false, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s_sw2_nfc_mirror_requested, enabled, __ATOMIC_RELEASE);
}

void ns2_nfc_mirror_snapshot(ns2_nfc_mirror_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    ble_connection_t *conn = find_connection_by_handle(sw2_init_handle);
    out->requested = __atomic_load_n(
        &s_sw2_nfc_mirror_requested, __ATOMIC_ACQUIRE);
    out->state = (uint8_t)__atomic_load_n(
        &s_sw2_nfc_mirror_state, __ATOMIC_ACQUIRE);
    out->active = out->state == NS2_NFC_MIRROR_ACTIVE;
    out->initiator = __atomic_load_n(
        &s_sw2_nfc_mirror_initiator, __ATOMIC_ACQUIRE);
    out->response_ready = __atomic_load_n(
        &s_sw2_nfc_mirror_response_ready, __ATOMIC_ACQUIRE);
    out->command_pending = __atomic_load_n(
        &s_sw2_nfc_mirror_slot_claimed, __ATOMIC_ACQUIRE);
    out->awaiting_response = __atomic_load_n(
        &s_sw2_nfc_mirror_awaiting_response, __ATOMIC_ACQUIRE);
    out->last_att_status = s_sw2_nfc_mirror_last_att_status;
    out->last_send_status = s_sw2_nfc_mirror_last_send_status;
    out->last_command = s_sw2_nfc_mirror_last_command;
    out->last_subcommand = s_sw2_nfc_mirror_last_subcommand;
    out->report_state = __atomic_load_n(
        &s_sw2_nfc_mirror_report_state, __ATOMIC_ACQUIRE);
    out->source_pid = conn ? conn->pid : 0;
    out->connection_handle = sw2_init_handle;
    out->last_response_length = s_sw2_nfc_mirror_last_response_length;
    out->commands_submitted = __atomic_load_n(
        &s_sw2_nfc_mirror_commands_submitted, __ATOMIC_ACQUIRE);
    out->commands_sent = __atomic_load_n(
        &s_sw2_nfc_mirror_commands_sent, __ATOMIC_ACQUIRE);
    out->notifications = __atomic_load_n(
        &s_sw2_nfc_mirror_notifications, __ATOMIC_ACQUIRE);
    out->report_state_transitions = __atomic_load_n(
        &s_sw2_nfc_mirror_report_state_transitions, __ATOMIC_ACQUIRE);
    out->response_timeouts = __atomic_load_n(
        &s_sw2_nfc_mirror_response_timeouts, __ATOMIC_ACQUIRE);
    out->rejected = __atomic_load_n(
        &s_sw2_nfc_mirror_rejected, __ATOMIC_ACQUIRE);
}

void ns2_nfc_mirror_set_initiator(bool enabled)
{
    __atomic_store_n(&s_sw2_nfc_mirror_initiator, enabled, __ATOMIC_RELEASE);
    // Same BLE subscription either way; arming the initiator must not require
    // remembering to arm the bridge first.
    if (enabled) ns2_nfc_mirror_request(true);
}

static bool sw2_nfc_mirror_submit_slot(const uint8_t *command, size_t length)
{
    if (!__atomic_load_n(
            &s_sw2_nfc_mirror_requested, __ATOMIC_ACQUIRE)) {
        return false;
    }
    if (!command || length < 8u || length > NS2_NFC_MIRROR_COMMAND_MAX ||
        command[0] != 0x01u ||
        __atomic_load_n(
            &s_sw2_nfc_mirror_response_ready, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_sw2_nfc_mirror_state, __ATOMIC_ACQUIRE) !=
            NS2_NFC_MIRROR_ACTIVE) {
        __atomic_add_fetch(&s_sw2_nfc_mirror_rejected, 1u, __ATOMIC_RELAXED);
        return false;
    }

    bool expected = false;
    if (!__atomic_compare_exchange_n(
            &s_sw2_nfc_mirror_slot_claimed, &expected, true, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&s_sw2_nfc_mirror_rejected, 1u, __ATOMIC_RELAXED);
        return false;
    }

    memcpy(s_sw2_nfc_mirror_command, command, length);
    s_sw2_nfc_mirror_command_length = length;
    s_sw2_nfc_mirror_last_command = command[0];
    s_sw2_nfc_mirror_last_subcommand = command[3];
    s_sw2_nfc_mirror_send_failures = 0;
    s_sw2_nfc_mirror_next_send_ms = 0;
    __atomic_add_fetch(
        &s_sw2_nfc_mirror_commands_submitted, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(
        &s_sw2_nfc_mirror_command_pending, true, __ATOMIC_RELEASE);
    return true;
}

bool ns2_nfc_mirror_submit(const uint8_t *command, size_t length)
{
    if (__atomic_load_n(&s_sw2_nfc_mirror_initiator, __ATOMIC_ACQUIRE))
        return false;
    return sw2_nfc_mirror_submit_slot(command, length);
}

bool ns2_nfc_mirror_initiator_submit(const uint8_t *command, size_t length)
{
    if (!__atomic_load_n(&s_sw2_nfc_mirror_initiator, __ATOMIC_ACQUIRE))
        return false;
    return sw2_nfc_mirror_submit_slot(command, length);
}

bool ns2_nfc_mirror_accept_ble_response(
    const uint8_t *response, size_t length)
{
    if (!response || length < 8u ||
        !__atomic_load_n(
            &s_sw2_nfc_mirror_awaiting_response, __ATOMIC_ACQUIRE) ||
        response[0] != 0x01u ||
        response[3] != s_sw2_nfc_mirror_last_subcommand ||
        __atomic_load_n(
            &s_sw2_nfc_mirror_response_ready, __ATOMIC_ACQUIRE)) {
        return false;
    }

    const size_t usb_length = ns2_nfc_mirror_translate_ble_response(
        s_sw2_nfc_mirror_response, sizeof(s_sw2_nfc_mirror_response),
        response, length);
    if (usb_length == 0) {
        __atomic_add_fetch(
            &s_sw2_nfc_mirror_rejected, 1u, __ATOMIC_RELAXED);
        return false;
    }

    s_sw2_nfc_mirror_last_response_length = (uint16_t)length;
    s_sw2_nfc_mirror_response_length = usb_length;
    __atomic_add_fetch(
        &s_sw2_nfc_mirror_notifications, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(
        &s_sw2_nfc_mirror_response_ready, true, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_nfc_mirror_awaiting_response, false, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_nfc_mirror_slot_claimed, false, __ATOMIC_RELEASE);
    return true;
}

static bool sw2_nfc_mirror_take_slot(
    uint8_t *response, size_t capacity, size_t *length)
{
    if (!response || !length ||
        !__atomic_load_n(
            &s_sw2_nfc_mirror_response_ready, __ATOMIC_ACQUIRE) ||
        capacity < s_sw2_nfc_mirror_response_length) {
        return false;
    }

    *length = s_sw2_nfc_mirror_response_length;
    memcpy(response, s_sw2_nfc_mirror_response, *length);
    __atomic_store_n(
        &s_sw2_nfc_mirror_response_ready, false, __ATOMIC_RELEASE);
    return true;
}

bool ns2_nfc_mirror_take_usb_response(
    uint8_t *response, size_t capacity, size_t *length)
{
    if (__atomic_load_n(&s_sw2_nfc_mirror_initiator, __ATOMIC_ACQUIRE))
        return false;
    return sw2_nfc_mirror_take_slot(response, capacity, length);
}

bool ns2_nfc_mirror_initiator_take(
    uint8_t *response, size_t capacity, size_t *length)
{
    if (!__atomic_load_n(&s_sw2_nfc_mirror_initiator, __ATOMIC_ACQUIRE))
        return false;
    return sw2_nfc_mirror_take_slot(response, capacity, length);
}

bool ns2_nfc_mirror_active(void)
{
    return __atomic_load_n(&s_sw2_nfc_mirror_requested, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&s_sw2_nfc_mirror_initiator, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&s_sw2_nfc_mirror_state, __ATOMIC_ACQUIRE) ==
               NS2_NFC_MIRROR_ACTIVE;
}

uint8_t ns2_nfc_mirror_report_state(void)
{
    if (__atomic_load_n(
            &s_sw2_nfc_mirror_state, __ATOMIC_ACQUIRE) !=
        NS2_NFC_MIRROR_ACTIVE) {
        return 0;
    }
    return __atomic_load_n(
        &s_sw2_nfc_mirror_report_state, __ATOMIC_ACQUIRE);
}

static void switch2_capture_link_params(uint8_t phase, uint8_t status,
                                        hci_con_handle_t con_handle, uint16_t interval,
                                        uint16_t latency, uint16_t supervision_timeout)
{
    uint8_t data[8] = {
        phase, status,
        (uint8_t)interval, (uint8_t)(interval >> 8),
        (uint8_t)latency, (uint8_t)(latency >> 8),
        (uint8_t)supervision_timeout, (uint8_t)(supervision_timeout >> 8),
    };
    sw2_capture_record(SW2_CAP_LINK_PARAMS, con_handle, data, sizeof(data));
}

static void switch2_v2_request_fast_link(hci_con_handle_t con_handle)
{
    // The Pico is the LE central, so use HCI LE Connection Update rather than the L2CAP
    // parameter-request procedure intended for a peripheral asking its central. Six units is
    // the Bluetooth-spec minimum (7.5ms); latency 0 prevents skipped connection events.
    const uint16_t interval = 6;
    const uint16_t latency = 0;
    const uint16_t supervision_timeout = 400; // 4s, comfortably valid for a 7.5ms link
    switch2_capture_link_params(SW2_LINK_PHASE_SNAPSHOT, 0, con_handle,
                                gap_le_connection_interval(con_handle), 0, 0);
    int status = gap_update_connection_parameters(con_handle, interval, interval, latency,
                                                   supervision_timeout);
    switch2_capture_link_params(SW2_LINK_PHASE_REQUEST, (uint8_t)status, con_handle,
                                interval, latency, supervision_timeout);
    printf("[SW2_V2] Fast-link request status=0x%02X interval=7.5ms\n", status);
}

void sw2_set_v2_variant(uint8_t variant) {
    s_sw2_v2_armed_variant = variant;
    if (variant == 0) s_sw2_v2_fired = false;
}

uint8_t sw2_get_v2_variant(void) {
    return s_sw2_v2_armed_variant;
}

void btstack_host_request_switch2_magraw(bool enabled)
{
    __atomic_store_n(&s_sw2_magraw_requested, enabled, __ATOMIC_RELEASE);
}

void btstack_host_get_switch2_magraw_diag(btstack_host_magraw_diag_t *out)
{
    if (!out) return;
    ble_connection_t *conn = find_connection_by_handle(sw2_init_handle);
    out->requested = __atomic_load_n(&s_sw2_magraw_requested, __ATOMIC_ACQUIRE);
    out->active = __atomic_load_n(&s_sw2_magraw_active, __ATOMIC_ACQUIRE);
    out->transition_pending = __atomic_load_n(
        &s_sw2_magraw_transition_pending, __ATOMIC_ACQUIRE);
    out->v2_state = (uint8_t)s_sw2_v2_state;
    out->reference_step = s_sw2_magraw_reference_step;
    out->reference_steps = 12u;
    out->reference_result = s_sw2_magraw_reference_result;
    out->last_response_status = s_sw2_magraw_last_response_status;
    out->input_ccc_status = s_sw2_magraw_input_ccc_status;
    out->source_pid = conn ? conn->pid : 0u;
    out->connection_handle = sw2_init_handle;
}

void btstack_host_request_switch2_imuref(bool enabled)
{
    __atomic_store_n(&s_sw2_imuref_requested, enabled, __ATOMIC_RELEASE);
    if (!enabled)
        __atomic_store_n(
            &s_sw2_imuref_dual_requested, false, __ATOMIC_RELEASE);
}

void btstack_host_request_switch2_imuref_dual(bool enabled)
{
    __atomic_store_n(
        &s_sw2_imuref_dual_requested, enabled, __ATOMIC_RELEASE);
}

void btstack_host_request_switch2_imuref_interval(uint16_t interval_units)
{
    __atomic_store_n(
        &s_sw2_imuref_interval_target_units, interval_units, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_imuref_interval_pending_units, interval_units, __ATOMIC_RELEASE);
}

void btstack_host_get_switch2_imuref_diag(btstack_host_imuref_diag_t *out)
{
    if (!out) return;
    ble_connection_t *conn = find_connection_by_handle(sw2_init_handle);
    out->requested = __atomic_load_n(&s_sw2_imuref_requested, __ATOMIC_ACQUIRE);
    out->active = __atomic_load_n(&s_sw2_imuref_active, __ATOMIC_ACQUIRE);
    out->transition_pending = __atomic_load_n(
        &s_sw2_imuref_transition_pending, __ATOMIC_ACQUIRE);
    out->dual_requested = __atomic_load_n(
        &s_sw2_imuref_dual_requested, __ATOMIC_ACQUIRE);
    out->dual_active = __atomic_load_n(
        &s_sw2_imuref_dual_active, __ATOMIC_ACQUIRE);
    out->dual_transition_pending = __atomic_load_n(
        &s_sw2_imuref_dual_transition_pending, __ATOMIC_ACQUIRE);
    out->v2_state = (uint8_t)s_sw2_v2_state;
    out->last_att_status = __atomic_load_n(
        &s_sw2_imuref_last_att_status, __ATOMIC_ACQUIRE);
    out->dual_att_status = __atomic_load_n(
        &s_sw2_imuref_dual_att_status, __ATOMIC_ACQUIRE);
    out->interval_request_status = __atomic_load_n(
        &s_sw2_imuref_interval_request_status, __ATOMIC_ACQUIRE);
    out->interval_target_units = __atomic_load_n(
        &s_sw2_imuref_interval_target_units, __ATOMIC_ACQUIRE);
    out->interval_actual_units = sw2_init_handle
        ? gap_le_connection_interval(sw2_init_handle) : 0u;
    out->common_notifications = __atomic_load_n(
        &s_sw2_imuref_common_notifications, __ATOMIC_ACQUIRE);
    out->native_notifications = __atomic_load_n(
        &s_sw2_imuref_native_notifications, __ATOMIC_ACQUIRE);
    out->source_pid = conn ? conn->pid : 0u;
    out->connection_handle = sw2_init_handle;
}

void sw2_native_auto_diag_snapshot(sw2_native_auto_diag_t *out)
{
    if (!out) return;
    out->checks = __atomic_load_n(&s_sw2_native_auto_checks, __ATOMIC_ACQUIRE);
    out->starts = __atomic_load_n(&s_sw2_native_auto_starts, __ATOMIC_ACQUIRE);
    out->wait_elapsed_ms = __atomic_load_n(&s_sw2_native_auto_wait_elapsed_ms, __ATOMIC_ACQUIRE);
    out->source_pid = __atomic_load_n(&s_sw2_native_auto_source_pid, __ATOMIC_ACQUIRE);
    out->personality = __atomic_load_n(&s_sw2_native_auto_personality, __ATOMIC_ACQUIRE);
    out->block_mask = __atomic_load_n(&s_sw2_native_auto_block_mask, __ATOMIC_ACQUIRE);
    out->init_state = (uint8_t)sw2_init_state;
    out->v2_state = (uint8_t)s_sw2_v2_state;
    out->auto_fired = s_sw2_native_auto_fired ? 1u : 0u;
    out->armed_variant = s_sw2_v2_armed_variant;
    out->gatt_discovery = s_sw2_gatt_disc_enabled ? 1u : 0u;
}

// Translate Pro/GC-format 0x000E buttons/sticks into the common 0x000A layout consumed by the
// existing Switch 2 BLE driver. Enabling this report makes genuine Pro Controller 2 firmware stop
// sending 0x000A completely, so treating 0x000E as motion-only freezes all controller input.
static bool switch2_normalize_pro_000e(const uint8_t *src, uint16_t len, uint8_t dst[63])
{
    if (!src || len < 15) return false;
    memset(dst, 0, 63);
    dst[0] = src[0];
    dst[1] = src[1];

    const uint8_t b0 = src[2];
    const uint8_t b1 = src[3];
    const uint8_t b2 = src[4];

    if (b0 & 0x01) dst[4] |= 0x04; // B
    if (b0 & 0x02) dst[4] |= 0x08; // A
    if (b0 & 0x04) dst[4] |= 0x01; // Y
    if (b0 & 0x08) dst[4] |= 0x02; // X
    if (b0 & 0x10) dst[4] |= 0x40; // R
    if (b0 & 0x20) dst[4] |= 0x80; // ZR
    if (b0 & 0x40) dst[5] |= 0x02; // Plus
    if (b0 & 0x80) dst[5] |= 0x04; // Right stick

    if (b1 & 0x01) dst[6] |= 0x01; // Down
    if (b1 & 0x02) dst[6] |= 0x04; // Right
    if (b1 & 0x04) dst[6] |= 0x08; // Left
    if (b1 & 0x08) dst[6] |= 0x02; // Up
    if (b1 & 0x10) dst[6] |= 0x40; // L
    if (b1 & 0x20) dst[6] |= 0x80; // ZL
    if (b1 & 0x40) dst[5] |= 0x01; // Minus
    if (b1 & 0x80) dst[5] |= 0x08; // Left stick

    if (b2 & 0x01) dst[5] |= 0x10; // Home
    if (b2 & 0x02) dst[5] |= 0x20; // Capture
    if (b2 & 0x04) dst[7] |= 0x01; // GR
    if (b2 & 0x08) dst[7] |= 0x02; // GL
    if (b2 & 0x10) dst[5] |= 0x40; // C

#ifdef NS2_DS5_AUDIO
    // Native Pro2 report 0x000E and extended report 0x002E both carry the
    // physical jack state at 0x0D. Preserve both occupancy (synthetic bit 28)
    // and microphone capability (synthetic bit 29) in the common-format
    // report so the normal input seam can advertise the exact jack type.
    const uint8_t headset_state = switch2_pro2_audio_headset_state(src[0x0D]);
    if (headset_state != CONTROLLER_HEADSET_NONE) dst[7] |= 0x10;
    if (headset_state == CONTROLLER_HEADSET_HEADSET) dst[7] |= 0x20;
#endif

    memcpy(&dst[10], &src[5], 6);
    return true;
}

static void sw2_motion_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
    uint16_t value_length = gatt_event_notification_get_value_length(packet);
    const uint8_t *value = gatt_event_notification_get_value(packet);

    sw2_capture_record(SW2_CAP_INPUT_NOTIFY, value_handle, value, value_length);
    if (__atomic_load_n(&s_sw2_imuref_requested, __ATOMIC_ACQUIRE))
        __atomic_add_fetch(
            &s_sw2_imuref_native_notifications, 1u, __ATOMIC_RELAXED);
    s_sw2_motion_last_notification_us = time_us_32();

    hci_con_handle_t con_handle = gatt_event_notification_get_handle(packet);
    ble_connection_t *conn = find_connection_by_handle(con_handle);
    if (!conn || conn->pid != 0x2069) return; // Nintendo Switch 2 Pro Controller

    if (value_length > 0x0C &&
        __atomic_load_n(
            &s_sw2_nfc_mirror_requested, __ATOMIC_ACQUIRE)) {
        const uint8_t state = value[0x0C];
        const uint8_t previous = __atomic_exchange_n(
            &s_sw2_nfc_mirror_report_state, state, __ATOMIC_ACQ_REL);
        if (state != previous) {
            __atomic_add_fetch(
                &s_sw2_nfc_mirror_report_state_transitions, 1u,
                __ATOMIC_RELAXED);
            sw2_capture_record(
                SW2_CAP_NFC_STATE, SW2_MOTION_HANDLE, &state, 1);
        }
    }

    int conn_index = get_ble_conn_index_by_handle(con_handle);
    if (conn_index < 0) return;

    // Preserve the genuine controller's opaque native motion PDU for the console-facing USB
    // report builder. This is intentionally separate from button/stick normalization below:
    // no quaternion decoding, axis conversion, or generic gamepad structure can alter it.
    ns2_active_input_note_connection((uint8_t)conn_index);
    const uint32_t connection_generation =
        ns2_active_input_connection_generation((uint8_t)conn_index);
    if (!ns2_active_input_connection_is_active_generation(
            (uint8_t)conn_index, connection_generation)) return;
    ns2_native_motion_publish_generation((uint8_t)conn_index,
                                         connection_generation,
                                         conn->vid, conn->pid,
                                         value, value_length, time_us_32());

    if (ble_report_pending) return;

    uint8_t normalized[63];
    if (!switch2_normalize_pro_000e(value, value_length, normalized)) return;
    memcpy(pending_ble_report, normalized, sizeof(normalized));
    pending_ble_report_len = sizeof(normalized);
    pending_ble_conn_index = (uint8_t)conn_index;
    pending_ble_connection_generation = connection_generation;
    ble_report_pending = true;
}

static void sw2_pro2_audio_notification_handler(uint8_t packet_type, uint16_t channel,
                                                uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET ||
        hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    const uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
    const uint16_t value_length = gatt_event_notification_get_value_length(packet);
    const uint8_t *value = gatt_event_notification_get_value(packet);
    if (value_handle != SW2_PRO2_AUDIO_INPUT_HANDLE) return;

    sw2_capture_record(SW2_CAP_INPUT_NOTIFY, value_handle, value, value_length);
    s_sw2_pro2_audio_notifications++;
    s_sw2_pro2_audio_last_report_len = value_length;
    if (value_length > s_sw2_pro2_audio_max_report_len)
        s_sw2_pro2_audio_max_report_len = value_length;
    if (value_length > 0x0D) s_sw2_pro2_audio_last_headset_raw = value[0x0D];
    if (value_length > 0x0E) s_sw2_pro2_audio_last_data_len = value[0x0E];

    hci_con_handle_t con_handle = gatt_event_notification_get_handle(packet);
    ble_connection_t *conn = find_connection_by_handle(con_handle);
    if (!conn || conn->pid != 0x2069) return;
    int conn_index = get_ble_conn_index_by_handle(con_handle);
    if (conn_index < 0) return;

    const uint32_t now_us = time_us_32();
    if (!switch2_pro2_audio_needs_input_fallback(
            now_us, s_sw2_motion_last_notification_us)) return;

    uint8_t compact[SW2_PRO2_AUDIO_COMPACT_LEN];
    if (!switch2_pro2_audio_compact_input(value, value_length, compact)) {
        s_sw2_pro2_audio_compact_failures++;
        return;
    }

    ns2_active_input_note_connection((uint8_t)conn_index);
    const uint32_t connection_generation =
        ns2_active_input_connection_generation((uint8_t)conn_index);
    if (!ns2_active_input_connection_is_active_generation(
            (uint8_t)conn_index, connection_generation)) return;

    // If ordinary 0x000E actually stops, feed 0x002E's relocated native-motion
    // block and controls into the exact same validated paths as a fallback.
    ns2_native_motion_publish_generation((uint8_t)conn_index,
                                         connection_generation,
                                         conn->vid, conn->pid,
                                         compact, sizeof(compact), now_us);
    if (ble_report_pending) return;
    uint8_t normalized[63];
    if (!switch2_normalize_pro_000e(compact, sizeof(compact), normalized)) return;
    memcpy(pending_ble_report, normalized, sizeof(normalized));
    pending_ble_report_len = sizeof(normalized);
    pending_ble_conn_index = (uint8_t)conn_index;
    pending_ble_connection_generation = connection_generation;
    ble_report_pending = true;
}

static void sw2_nfc_mirror_notification_handler(
    uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET ||
        hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    const uint16_t value_handle =
        gatt_event_notification_get_value_handle(packet);
    if (value_handle != SW2_NFC_RESPONSE_HANDLE) return;

    const uint16_t value_length =
        gatt_event_notification_get_value_length(packet);
    const uint8_t *value = gatt_event_notification_get_value(packet);
    s_sw2_nfc_mirror_last_response_length = value_length;
    sw2_capture_record(
        SW2_CAP_NFC_NOTIFY, value_handle, value, value_length);
    if (value_length > 14u) {
        (void)ns2_nfc_mirror_accept_ble_response(
            &value[14], value_length - 14u);
    }
}

static void sw2_nfc_mirror_ccc_callback(
    uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET ||
        hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) return;

    const uint8_t status = gatt_event_query_complete_get_att_status(packet);
    s_sw2_nfc_mirror_last_att_status = status;
    sw2_capture_record(
        SW2_CAP_WRITE_STATUS, SW2_NFC_CCC_HANDLE, &status, 1);

    if (status != ATT_ERROR_SUCCESS) {
        s_sw2_nfc_mirror_state = NS2_NFC_MIRROR_ERROR;
        printf("[SW2_NFC] Extended-response CCC operation failed: 0x%02X\n",
               status);
        return;
    }

    if (s_sw2_nfc_mirror_state == NS2_NFC_MIRROR_SUBSCRIBE_PENDING) {
        s_sw2_nfc_mirror_state = NS2_NFC_MIRROR_ACTIVE;
        printf("[SW2_NFC] Genuine-reader diagnostic mirror active\n");
    } else if (
        s_sw2_nfc_mirror_state == NS2_NFC_MIRROR_UNSUBSCRIBE_PENDING) {
        gatt_client_stop_listening_for_characteristic_value_updates(
            &sw2_nfc_mirror_notification_listener);
        s_sw2_nfc_mirror_state = NS2_NFC_MIRROR_OFF;
        printf("[SW2_NFC] Genuine-reader diagnostic mirror disabled\n");
    }
}

static void switch2_service_nfc_mirror(void)
{
    const bool requested = __atomic_load_n(
        &s_sw2_nfc_mirror_requested, __ATOMIC_ACQUIRE);
    ble_connection_t *conn = find_connection_by_handle(sw2_init_handle);

    if (!requested && s_sw2_nfc_mirror_state == NS2_NFC_MIRROR_ERROR) {
        gatt_client_stop_listening_for_characteristic_value_updates(
            &sw2_nfc_mirror_notification_listener);
        s_sw2_nfc_mirror_state = NS2_NFC_MIRROR_OFF;
    }

    if (sw2_init_state != SW2_INIT_DONE || sw2_init_handle == 0 ||
        !conn || conn->pid != 0x2069) {
        return;
    }

    if (requested && s_sw2_nfc_mirror_state == NS2_NFC_MIRROR_OFF) {
        // Preserve the validated initialization and audio paths: claim the
        // one outstanding GATT procedure only after their setup operations
        // have finished.
        if (s_sw2_v2_active != NULL ||
            (s_sw2_v2_state != SW2_V2_IDLE &&
             s_sw2_v2_state != SW2_V2_DONE) ||
            (s_gatt_disc_state != SW2_GATT_DISC_IDLE &&
             s_gatt_disc_state != SW2_GATT_DISC_DONE) ||
            (s_sw2_pro2_audio_state != SW2_PRO2_AUDIO_OFF &&
             s_sw2_pro2_audio_state != SW2_PRO2_AUDIO_ACTIVE)) {
            return;
        }

        memset(&sw2_nfc_mirror_characteristic, 0,
               sizeof(sw2_nfc_mirror_characteristic));
        sw2_nfc_mirror_characteristic.value_handle =
            SW2_NFC_RESPONSE_HANDLE;
        sw2_nfc_mirror_characteristic.end_handle = 0x0020;
        gatt_client_listen_for_characteristic_value_updates(
            &sw2_nfc_mirror_notification_listener,
            sw2_nfc_mirror_notification_handler, sw2_init_handle,
            &sw2_nfc_mirror_characteristic);

        static uint8_t ccc_enable[] = {0x01, 0x00};
        s_sw2_nfc_mirror_state = NS2_NFC_MIRROR_SUBSCRIBE_PENDING;
        sw2_capture_record(
            SW2_CAP_CCC_WRITE, SW2_NFC_CCC_HANDLE,
            ccc_enable, sizeof(ccc_enable));
        const uint8_t status = gatt_client_write_value_of_characteristic(
            sw2_nfc_mirror_ccc_callback, sw2_init_handle,
            SW2_NFC_CCC_HANDLE, sizeof(ccc_enable), ccc_enable);
        if (status != ERROR_CODE_SUCCESS) {
            s_sw2_nfc_mirror_last_att_status = status;
            s_sw2_nfc_mirror_state = NS2_NFC_MIRROR_ERROR;
        }
        return;
    }

    if (!requested &&
        s_sw2_nfc_mirror_state == NS2_NFC_MIRROR_ACTIVE) {
        static uint8_t ccc_disable[] = {0x00, 0x00};
        s_sw2_nfc_mirror_state = NS2_NFC_MIRROR_UNSUBSCRIBE_PENDING;
        sw2_capture_record(
            SW2_CAP_CCC_WRITE, SW2_NFC_CCC_HANDLE,
            ccc_disable, sizeof(ccc_disable));
        const uint8_t status = gatt_client_write_value_of_characteristic(
            sw2_nfc_mirror_ccc_callback, sw2_init_handle,
            SW2_NFC_CCC_HANDLE, sizeof(ccc_disable), ccc_disable);
        if (status != ERROR_CODE_SUCCESS) {
            s_sw2_nfc_mirror_last_att_status = status;
            s_sw2_nfc_mirror_state = NS2_NFC_MIRROR_ERROR;
        }
        return;
    }

    if (s_sw2_nfc_mirror_state != NS2_NFC_MIRROR_ACTIVE) return;
    const uint32_t now_ms = btstack_run_loop_get_time_ms();

    if (__atomic_load_n(
            &s_sw2_nfc_mirror_awaiting_response, __ATOMIC_ACQUIRE)) {
        if ((uint32_t)(now_ms - s_sw2_nfc_mirror_awaiting_since_ms) >
            1000u) {
            const uint8_t fallback_ble[] = {
                0x01, 0x01, 0x01, s_sw2_nfc_mirror_last_subcommand,
                0x10, 0x78, 0x00, 0x00,
            };
            s_sw2_nfc_mirror_response_length =
                ns2_nfc_mirror_translate_ble_response(
                    s_sw2_nfc_mirror_response,
                    sizeof(s_sw2_nfc_mirror_response),
                    fallback_ble, sizeof(fallback_ble));
            s_sw2_nfc_mirror_last_response_length =
                sizeof(fallback_ble);
            __atomic_add_fetch(
                &s_sw2_nfc_mirror_response_timeouts, 1u,
                __ATOMIC_RELAXED);
            __atomic_store_n(
                &s_sw2_nfc_mirror_response_ready, true,
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &s_sw2_nfc_mirror_awaiting_response, false,
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &s_sw2_nfc_mirror_slot_claimed, false,
                __ATOMIC_RELEASE);
        }
        return;
    }

    if (!__atomic_load_n(
            &s_sw2_nfc_mirror_command_pending, __ATOMIC_ACQUIRE)) {
        return;
    }

    if ((int32_t)(now_ms - s_sw2_nfc_mirror_next_send_ms) < 0) return;

    uint8_t command[NS2_NFC_MIRROR_FRAME_MAX];
    const size_t source_length = s_sw2_nfc_mirror_command_length;
    const size_t length = ns2_nfc_mirror_prepare_extended_ble_command(
        command, sizeof(command), s_sw2_nfc_mirror_command, source_length);
    if (length == 0) {
        __atomic_add_fetch(
            &s_sw2_nfc_mirror_rejected, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(
            &s_sw2_nfc_mirror_command_pending, false, __ATOMIC_RELEASE);
        __atomic_store_n(
            &s_sw2_nfc_mirror_slot_claimed, false, __ATOMIC_RELEASE);
        return;
    }

    const uint8_t status =
        gatt_client_write_value_of_characteristic_without_response(
            sw2_init_handle, SW2_NFC_COMMAND_HANDLE,
            (uint16_t)length, command);
    s_sw2_nfc_mirror_last_send_status = status;
    if (status != ERROR_CODE_SUCCESS) {
        if (++s_sw2_nfc_mirror_send_failures < 8u) {
            s_sw2_nfc_mirror_next_send_ms = now_ms + 2u;
            return;
        }
        __atomic_add_fetch(
            &s_sw2_nfc_mirror_rejected, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(
            &s_sw2_nfc_mirror_slot_claimed, false, __ATOMIC_RELEASE);
    } else {
        sw2_capture_record(
            SW2_CAP_CMD_OUT, SW2_NFC_COMMAND_HANDLE, command,
            (uint16_t)length);
        __atomic_add_fetch(
            &s_sw2_nfc_mirror_commands_sent, 1u, __ATOMIC_RELAXED);
        s_sw2_nfc_mirror_awaiting_since_ms = now_ms;
        __atomic_store_n(
            &s_sw2_nfc_mirror_awaiting_response, true,
            __ATOMIC_RELEASE);
    }

    __atomic_store_n(
        &s_sw2_nfc_mirror_command_pending, false, __ATOMIC_RELEASE);
}

static void sw2_pro2_audio_ccc_callback(uint8_t packet_type, uint16_t channel,
                                       uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET ||
        hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) return;

    const uint8_t status = gatt_event_query_complete_get_att_status(packet);
    const hci_con_handle_t con_handle = gatt_event_query_complete_get_handle(packet);
    s_sw2_pro2_audio_last_att_status = status;
    const uint16_t completed_handle =
        s_sw2_pro2_audio_state == SW2_PRO2_AUDIO_RESTORE_MOTION_PENDING
            ? SW2_MOTION_CCC_HANDLE
            : SW2_PRO2_AUDIO_CCC_HANDLE;
    sw2_capture_record(SW2_CAP_WRITE_STATUS, completed_handle, &status, 1);

    if (status != ATT_ERROR_SUCCESS) {
        s_sw2_pro2_audio_state = SW2_PRO2_AUDIO_ERROR;
        printf("[SW2_PRO2_AUDIO] CCC operation failed: 0x%02X\n", status);
        return;
    }

    if (s_sw2_pro2_audio_state == SW2_PRO2_AUDIO_SUBSCRIBE_PENDING) {
        s_sw2_pro2_audio_state = SW2_PRO2_AUDIO_ACTIVE;
        printf("[SW2_PRO2_AUDIO] Extended input/audio notifications active\n");
    } else if (s_sw2_pro2_audio_state == SW2_PRO2_AUDIO_UNSUBSCRIBE_PENDING) {
        static uint8_t ccc_enable[] = { 0x01, 0x00 };
        s_sw2_pro2_audio_state = SW2_PRO2_AUDIO_RESTORE_MOTION_PENDING;
        sw2_capture_record(SW2_CAP_CCC_WRITE, SW2_MOTION_CCC_HANDLE,
                           ccc_enable, sizeof(ccc_enable));
        gatt_client_write_value_of_characteristic(
            sw2_pro2_audio_ccc_callback, con_handle, SW2_MOTION_CCC_HANDLE,
            sizeof(ccc_enable), ccc_enable);
    } else if (s_sw2_pro2_audio_state == SW2_PRO2_AUDIO_RESTORE_MOTION_PENDING) {
        s_sw2_pro2_audio_state = SW2_PRO2_AUDIO_OFF;
        printf("[SW2_PRO2_AUDIO] Ordinary native-motion notifications restored\n");
    }
}

static void switch2_service_pro2_audio_capture(void)
{
    const bool requested =
        __atomic_load_n(&s_sw2_pro2_audio_requested, __ATOMIC_ACQUIRE);
    ble_connection_t *conn = find_connection_by_handle(sw2_init_handle);
    if (!conn || conn->pid != 0x2069) return;

    if (requested && s_sw2_pro2_audio_state == SW2_PRO2_AUDIO_OFF) {
        // Do not overlap the production native-motion setup's outstanding GATT
        // procedure. UART may arm this at any time; service it only once that
        // already-validated sequence is fully idle.
        if (s_sw2_v2_active != NULL ||
            (s_sw2_v2_state != SW2_V2_IDLE && s_sw2_v2_state != SW2_V2_DONE)) return;

        memset(&sw2_pro2_audio_characteristic, 0,
               sizeof(sw2_pro2_audio_characteristic));
        sw2_pro2_audio_characteristic.value_handle = SW2_PRO2_AUDIO_INPUT_HANDLE;
        sw2_pro2_audio_characteristic.end_handle = 0x0030;
        gatt_client_listen_for_characteristic_value_updates(
            &sw2_pro2_audio_notification_listener,
            sw2_pro2_audio_notification_handler, sw2_init_handle,
            &sw2_pro2_audio_characteristic);

        static uint8_t ccc_enable[] = { 0x01, 0x00 };
        s_sw2_pro2_audio_state = SW2_PRO2_AUDIO_SUBSCRIBE_PENDING;
        sw2_capture_record(SW2_CAP_CCC_WRITE, SW2_PRO2_AUDIO_CCC_HANDLE,
                           ccc_enable, sizeof(ccc_enable));
        gatt_client_write_value_of_characteristic(
            sw2_pro2_audio_ccc_callback, sw2_init_handle,
            SW2_PRO2_AUDIO_CCC_HANDLE, sizeof(ccc_enable), ccc_enable);
        printf("[SW2_PRO2_AUDIO] Enabling 0x002E notifications\n");
    } else if (!requested && s_sw2_pro2_audio_state == SW2_PRO2_AUDIO_ACTIVE) {
        static uint8_t ccc_disable[] = { 0x00, 0x00 };
        s_sw2_pro2_audio_state = SW2_PRO2_AUDIO_UNSUBSCRIBE_PENDING;
        sw2_capture_record(SW2_CAP_CCC_WRITE, SW2_PRO2_AUDIO_CCC_HANDLE,
                           ccc_disable, sizeof(ccc_disable));
        gatt_client_write_value_of_characteristic(
            sw2_pro2_audio_ccc_callback, sw2_init_handle,
            SW2_PRO2_AUDIO_CCC_HANDLE, sizeof(ccc_disable), ccc_disable);
        printf("[SW2_PRO2_AUDIO] Disabling 0x002E notifications\n");
    }
}

static bool switch2_pro2_replay_due(uint32_t now_us, uint32_t target_us)
{
    return (int32_t)(now_us - target_us) >= 0;
}

static bool switch2_pro2_replay_write(hci_con_handle_t con_handle,
                                      uint16_t value_handle,
                                      uint8_t *data, uint16_t len)
{
    const uint8_t status = gatt_client_write_value_of_characteristic_without_response(
        con_handle, value_handle, len, data);
    s_sw2_pro2_replay_last_send_status = status;
    if (status != ERROR_CODE_SUCCESS) {
        // A busy controller can reject a write-without-response transiently.
        // Retry at the audio cadence, but latch an error instead of hammering
        // BTstack forever if the transport never becomes writable.
        s_sw2_pro2_replay_next_us = time_us_32() + 8000u;
        if (++s_sw2_pro2_replay_send_failures >= 16) {
            s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_ERROR;
            __atomic_store_n(&s_sw2_pro2_replay_requested, false,
                             __ATOMIC_RELEASE);
        }
        return false;
    }
    s_sw2_pro2_replay_send_failures = 0;
    sw2_capture_record(SW2_CAP_CMD_OUT, value_handle, data, len);
    return true;
}

// UART-only historical endpoint discriminator: pair captured second halves
// with the fixed idle first half. This proved that stream 0x02 reaches the
// headphone decoder, but it is deliberately not a reconstruction of the
// original 240-byte Opus packets. The live path below is the valid codec path.
void btstack_host_service_switch2_pro2_audio_replay(void)
{
    const bool requested = __atomic_load_n(
        &s_sw2_pro2_replay_requested, __ATOMIC_ACQUIRE);
    if (!requested) return;

    ble_connection_t *conn = find_connection_by_handle(sw2_init_handle);
    if (!conn || conn->pid != 0x2069) {
        s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_ERROR;
        return;
    }

    if (s_sw2_pro2_replay_state == SW2_PRO2_REPLAY_IDLE) {
        // Require the extended report and a physically occupied jack. This
        // prevents accidental replay through a controller with no headphones.
        if (s_sw2_pro2_audio_state != SW2_PRO2_AUDIO_ACTIVE ||
            switch2_pro2_audio_headset_state(s_sw2_pro2_audio_last_headset_raw) ==
                CONTROLLER_HEADSET_NONE) return;
        s_sw2_pro2_replay_index = 0;
        s_sw2_pro2_replay_silence_count = 0;
        s_sw2_pro2_replay_send_failures = 0;
        s_sw2_pro2_replay_stream4_next = true;
        s_sw2_pro2_replay_frames_sent = 0;
        s_sw2_pro2_replay_last_send_status = 0;
        s_sw2_pro2_replay_next_us = time_us_32();
        s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_SETUP_ONE;
    }

    const uint32_t now_us = time_us_32();
    if (!switch2_pro2_replay_due(now_us, s_sw2_pro2_replay_next_us)) return;

    static uint8_t setup_one[] = {
        0x00, 0x18, 0x91, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x07};
    static uint8_t setup_two[] = {
        0x00, 0x17, 0x91, 0x01, 0x02, 0x00, 0x07, 0x00,
        0x00, 0x80, 0xBB, 0x00, 0x00, 0x02, 0xF0, 0x00};
    // The original Total Phase USB capture retained its real timestamps even
    // though the derived pcapng did not. One 240-byte Opus packet repeats every
    // 20 ms: stream 0x04 carries bytes 0..119 about 5 ms before stream 0x02
    // carries bytes 120..239. The fixture is the exact contiguous
    // 49.206..49.766-second sequence of second halves, including two all-zero
    // idle continuations. This replay intentionally supplies idle first halves.
    uint8_t stream4_packet[3 + SW2_PRO2_REPLAY_FRAME_BYTES] = {
        0x00, 0x04, SW2_PRO2_REPLAY_FRAME_BYTES, 0xFC, 0xFF, 0xFE};
    uint8_t stream2_packet[3 + SW2_PRO2_REPLAY_FRAME_BYTES] = {
        0x00, 0x02, SW2_PRO2_REPLAY_FRAME_BYTES};

    switch (s_sw2_pro2_replay_state) {
        case SW2_PRO2_REPLAY_SETUP_ONE:
            if (!switch2_pro2_replay_write(sw2_init_handle, 0x0032,
                                            setup_one, sizeof(setup_one))) return;
            s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_SETUP_TWO;
            s_sw2_pro2_replay_next_us += 25000u;
            break;

        case SW2_PRO2_REPLAY_SETUP_TWO:
            if (!switch2_pro2_replay_write(sw2_init_handle, 0x0032,
                                            setup_two, sizeof(setup_two))) return;
            s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_PRIME;
            s_sw2_pro2_replay_next_us += 10000u;
            break;

        case SW2_PRO2_REPLAY_PRIME:
        case SW2_PRO2_REPLAY_AUDIO:
        case SW2_PRO2_REPLAY_TAIL:
            if (s_sw2_pro2_replay_stream4_next) {
                if (!switch2_pro2_replay_write(
                        sw2_init_handle, SW2_PRO2_AUDIO_OUTPUT_HANDLE,
                        stream4_packet, sizeof(stream4_packet))) return;
                s_sw2_pro2_replay_stream4_next = false;
                s_sw2_pro2_replay_next_us += 5000u;
                return;
            }

            if (s_sw2_pro2_replay_state == SW2_PRO2_REPLAY_AUDIO)
                memcpy(&stream2_packet[3],
                       switch2_pro2_replay_frames[s_sw2_pro2_replay_index],
                       SW2_PRO2_REPLAY_FRAME_BYTES);
            if (!switch2_pro2_replay_write(
                    sw2_init_handle, SW2_PRO2_AUDIO_OUTPUT_HANDLE,
                    stream2_packet, sizeof(stream2_packet))) return;
            s_sw2_pro2_replay_stream4_next = true;
            s_sw2_pro2_replay_next_us += 15000u;

            if (s_sw2_pro2_replay_state == SW2_PRO2_REPLAY_PRIME) {
                if (++s_sw2_pro2_replay_silence_count >= 8) {
                    s_sw2_pro2_replay_silence_count = 0;
                    s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_AUDIO;
                }
            } else if (s_sw2_pro2_replay_state == SW2_PRO2_REPLAY_AUDIO) {
                s_sw2_pro2_replay_index++;
                s_sw2_pro2_replay_frames_sent++;
                if (s_sw2_pro2_replay_index >= SW2_PRO2_REPLAY_FRAME_COUNT)
                    s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_TAIL;
            } else {
                if (++s_sw2_pro2_replay_silence_count >= 8) {
                    s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_DONE;
                    __atomic_store_n(&s_sw2_pro2_replay_requested, false,
                                     __ATOMIC_RELEASE);
                }
            }
            break;

        case SW2_PRO2_REPLAY_DONE:
        case SW2_PRO2_REPLAY_ERROR:
        case SW2_PRO2_REPLAY_IDLE:
        default:
            break;
    }
}

static bool switch2_pro2_live_write(hci_con_handle_t con_handle,
                                    uint16_t value_handle,
                                    uint8_t *data, uint16_t len)
{
    const uint8_t status =
        gatt_client_write_value_of_characteristic_without_response(
            con_handle, value_handle, len, data);
    s_sw2_pro2_live_last_send_status = status;
    if (status != ERROR_CODE_SUCCESS) {
        s_sw2_pro2_live_next_us = time_us_32() + 2000u;
        if (++s_sw2_pro2_live_send_failures >= 16u) {
            s_sw2_pro2_live_state = SW2_PRO2_LIVE_ERROR;
            __atomic_store_n(&s_sw2_pro2_live_requested, false,
                             __ATOMIC_RELEASE);
            ds5_audio_bridge_set_switch2_pro2_active(false);
        }
        return false;
    }
    s_sw2_pro2_live_send_failures = 0;
    sw2_capture_record(SW2_CAP_CMD_OUT, value_handle, data, len);
    return true;
}

static void switch2_pro2_live_advance(uint32_t now_us, uint32_t interval_us)
{
    uint32_t const scheduled = s_sw2_pro2_live_next_us + interval_us;
    // Preserve the measured 5/15 ms interleave during normal timer jitter, but
    // never catch up a long Bluetooth stall by bursting stale packets.
    s_sw2_pro2_live_next_us =
        (int32_t)(now_us - scheduled) >= 20000
            ? now_us + interval_us
            : scheduled;
}

// UART-gated production-shaped path: console USB PCM is encoded into one
// 240-byte stereo Opus packet per 20 ms interval. The real transport divides
// that packet into a 120-byte 0x04 first chunk and a 120-byte 0x02 second
// chunk. These are packet halves, not independent audio and haptic codecs.
void btstack_host_service_switch2_pro2_audio_live(void)
{
    const bool requested = __atomic_load_n(
        &s_sw2_pro2_live_requested, __ATOMIC_ACQUIRE);
    if (!requested) {
        if (s_sw2_pro2_live_state != SW2_PRO2_LIVE_IDLE ||
            ds5_audio_bridge_switch2_pro2_active()) {
            s_sw2_pro2_live_state = SW2_PRO2_LIVE_IDLE;
            ds5_audio_bridge_set_switch2_pro2_active(false);
        }
        return;
    }

    ble_connection_t *conn = find_connection_by_handle(sw2_init_handle);
    const bool ready = conn && conn->pid == 0x2069 &&
        s_sw2_pro2_audio_state == SW2_PRO2_AUDIO_ACTIVE &&
        switch2_pro2_audio_headset_state(
            s_sw2_pro2_audio_last_headset_raw) != CONTROLLER_HEADSET_NONE;
    if (!ready) {
        if (s_sw2_pro2_live_state != SW2_PRO2_LIVE_IDLE ||
            ds5_audio_bridge_switch2_pro2_active()) {
            s_sw2_pro2_live_state = SW2_PRO2_LIVE_IDLE;
            ds5_audio_bridge_set_switch2_pro2_active(false);
        }
        return;
    }

    if (s_sw2_pro2_live_state == SW2_PRO2_LIVE_IDLE) {
        s_sw2_pro2_live_send_failures = 0;
        s_sw2_pro2_live_last_send_status = 0;
        s_sw2_pro2_live_last_toc = 0;
        memset(s_sw2_pro2_live_prefix, 0,
               sizeof(s_sw2_pro2_live_prefix));
        s_sw2_pro2_live_frames_sent = 0;
        s_sw2_pro2_live_underruns = 0;
        s_sw2_pro2_live_prime_count = 0;
        // Match the successful capture replay: establish the controller's
        // transport with eight exact idle stream pairs before codec work can
        // contend with setup writes or their ACL completions.
        ds5_audio_bridge_set_switch2_pro2_active(false);
        s_sw2_pro2_live_next_us = time_us_32();
        s_sw2_pro2_live_state = SW2_PRO2_LIVE_SETUP_ONE;
    }

    const uint32_t now_us = time_us_32();
    if (!switch2_pro2_replay_due(now_us, s_sw2_pro2_live_next_us)) return;

    static uint8_t setup_one[] = {
        0x00, 0x18, 0x91, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x07};
    static uint8_t setup_two[] = {
        0x00, 0x17, 0x91, 0x01, 0x02, 0x00, 0x07, 0x00,
        0x00, 0x80, 0xBB, 0x00, 0x00, 0x02, 0xF0, 0x00};
    static uint8_t stream4_packet[3 + SWITCH2_PRO2_AUDIO_CHUNK_LEN];
    static uint8_t stream2_packet[3 + SWITCH2_PRO2_AUDIO_CHUNK_LEN];
    static uint8_t opus_packet[SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN];
    static bool opus_packet_encoded;

    switch (s_sw2_pro2_live_state) {
        case SW2_PRO2_LIVE_SETUP_ONE:
            if (!switch2_pro2_live_write(sw2_init_handle, 0x0032,
                                          setup_one, sizeof(setup_one))) return;
            s_sw2_pro2_live_state = SW2_PRO2_LIVE_SETUP_TWO;
            switch2_pro2_live_advance(now_us, 25000u);
            break;

        case SW2_PRO2_LIVE_SETUP_TWO:
            if (!switch2_pro2_live_write(sw2_init_handle, 0x0032,
                                          setup_two, sizeof(setup_two))) return;
            s_sw2_pro2_live_state = SW2_PRO2_LIVE_STREAM4;
            switch2_pro2_live_advance(now_us, 10000u);
            break;

        case SW2_PRO2_LIVE_STREAM4: {
            stream4_packet[0] = 0x00;
            stream4_packet[1] = 0x04;
            stream4_packet[2] = SWITCH2_PRO2_AUDIO_CHUNK_LEN;
            // Exact steady-state 240-byte idle Opus packet observed from the
            // real console. Its first half begins FC FF FE; the rest of both
            // chunks is zero.
            memset(opus_packet, 0, sizeof(opus_packet));
            opus_packet[0] = 0xFC;
            opus_packet[1] = 0xFF;
            opus_packet[2] = 0xFE;
            opus_packet_encoded = s_sw2_pro2_live_prime_count >= 8u &&
                ds5_audio_bridge_peek_switch2_pro2_frame(opus_packet);
            memcpy(&stream4_packet[3], opus_packet,
                   SWITCH2_PRO2_AUDIO_CHUNK_LEN);
            if (!switch2_pro2_live_write(
                    sw2_init_handle, SW2_PRO2_AUDIO_OUTPUT_HANDLE,
                    stream4_packet, sizeof(stream4_packet))) return;
            s_sw2_pro2_live_last_toc = stream4_packet[3];
            memcpy(s_sw2_pro2_live_prefix, &stream4_packet[3],
                   sizeof(s_sw2_pro2_live_prefix));
            s_sw2_pro2_live_state = SW2_PRO2_LIVE_STREAM2;
            switch2_pro2_live_advance(now_us, 5000u);
            break;
        }

        case SW2_PRO2_LIVE_STREAM2:
            memset(stream2_packet, 0, sizeof(stream2_packet));
            stream2_packet[1] = 0x02;
            stream2_packet[2] = SWITCH2_PRO2_AUDIO_CHUNK_LEN;
            memcpy(&stream2_packet[3],
                   &opus_packet[SWITCH2_PRO2_AUDIO_CHUNK_LEN],
                   SWITCH2_PRO2_AUDIO_CHUNK_LEN);
            if (!switch2_pro2_live_write(
                    sw2_init_handle, SW2_PRO2_AUDIO_OUTPUT_HANDLE,
                    stream2_packet, sizeof(stream2_packet))) return;
            if (opus_packet_encoded)
                ds5_audio_bridge_commit_switch2_pro2_frame();
            else {
                // The controller consumed one complete fixed idle packet.
                // Advance our stateful encoder through matching silence before
                // the next live packet so CELT prediction starts in phase.
                ds5_audio_bridge_note_switch2_pro2_idle_frame();
                if (s_sw2_pro2_live_prime_count >= 8u &&
                    ds5_audio_bridge_usb_speaker_active())
                    s_sw2_pro2_live_underruns++;
            }
            s_sw2_pro2_live_frames_sent++;
            if (s_sw2_pro2_live_prime_count < 8u &&
                ++s_sw2_pro2_live_prime_count == 8u)
                ds5_audio_bridge_set_switch2_pro2_active(true);
            s_sw2_pro2_live_state = SW2_PRO2_LIVE_STREAM4;
            switch2_pro2_live_advance(now_us, 15000u);
            break;

        case SW2_PRO2_LIVE_ERROR:
            ds5_audio_bridge_set_switch2_pro2_active(false);
            break;

        case SW2_PRO2_LIVE_IDLE:
        default:
            break;
    }
}

static void switch2_build_spi_read_cmd(uint8_t *out, uint32_t address, uint8_t size)
{
    // Byte-for-byte copy of switch2_input_viewer.py's read_spi_memory() command construction --
    // deliberately not re-derived, since replicating a proven-working reference exactly is the
    // whole point of this variant.
    out[0] = SW2_CMD_READ_SPI;
    out[1] = SW2_REQ_TYPE_REQ;
    out[2] = SW2_REQ_INT_BLE;
    out[3] = SW2_SUBCMD_READ_SPI;
    out[4] = 0x00; out[5] = 0x08; out[6] = 0x00; out[7] = 0x00;
    out[8] = size;
    out[9] = 0x7E; out[10] = 0x00; out[11] = 0x00;
    out[12] = (uint8_t)(address & 0xFF);
    out[13] = (uint8_t)((address >> 8) & 0xFF);
    out[14] = (uint8_t)((address >> 16) & 0xFF);
    out[15] = (uint8_t)((address >> 24) & 0xFF);
}

static void switch2_v2_send_configure(hci_con_handle_t con_handle)
{
    uint8_t cmd[] = { 0x0c, SW2_REQ_TYPE_REQ, SW2_REQ_INT_BLE, 0x02, 0x00, 0x04, 0x00, 0x00,
                      s_sw2_v2_active->configure_flags, 0x00, 0x00, 0x00 };
    sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, cmd, sizeof(cmd));
    gatt_client_write_value_of_characteristic_without_response(con_handle, SW2_CMD_HANDLE, sizeof(cmd), cmd);
}

static void switch2_v2_send_common_report_select(hci_con_handle_t con_handle)
{
    // Exact frame 1998 from btle_procon2_motion_0x000A.pcapng.
    uint8_t cmd[] = {
        0x0A, SW2_REQ_TYPE_REQ, SW2_REQ_INT_BLE, 0x02,
        0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00
    };
    sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, cmd, sizeof(cmd));
    gatt_client_write_value_of_characteristic_without_response(
        con_handle, SW2_CMD_HANDLE, sizeof(cmd), cmd);
}

static void switch2_v2_send_disable_all(hci_con_handle_t con_handle)
{
    uint8_t cmd[] = { 0x0c, SW2_REQ_TYPE_REQ, SW2_REQ_INT_BLE, 0x05, 0x00, 0x04, 0x00, 0x00,
                      0xFF, 0x00, 0x00, 0x00 };
    sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, cmd, sizeof(cmd));
    gatt_client_write_value_of_characteristic_without_response(con_handle, SW2_CMD_HANDLE,
                                                                 sizeof(cmd), cmd);
}

static void switch2_v2_send_next_cal_read(hci_con_handle_t con_handle)
{
    const sw2_spi_read_t *reads = s_sw2_v2_active->use_console_cal_reads
        ? SW2_V2_CONSOLE_CAL_READS : SW2_V2_CAL_READS;
    const sw2_spi_read_t *r = &reads[s_sw2_v2_cal_index];
    uint8_t cmd[16];
    switch2_build_spi_read_cmd(cmd, r->address, r->size);
    sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, cmd, sizeof(cmd));
    gatt_client_write_value_of_characteristic_without_response(con_handle, SW2_CMD_HANDLE, sizeof(cmd), cmd);
}

static void switch2_v2_send_enable(hci_con_handle_t con_handle)
{
    uint8_t cmd[] = { 0x0c, SW2_REQ_TYPE_REQ, SW2_REQ_INT_BLE, 0x04, 0x00, 0x04, 0x00, 0x00,
                      s_sw2_v2_active->enable_flags, 0x00, 0x00, 0x00 };
    sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, cmd, sizeof(cmd));
    gatt_client_write_value_of_characteristic_without_response(con_handle, SW2_CMD_HANDLE, sizeof(cmd), cmd);
}

static void switch2_v2_handle_write_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void switch2_v2_ccc_write_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void switch2_v2_send_handle_write(hci_con_handle_t con_handle)
{
    // Exact reference-viewer bytes. Write-with-response is intentional so the capture records a
    // definitive ATT result for the unconfirmed candidate handle.
    uint8_t data[] = { 0x85, 0x00 };
    sw2_capture_record(SW2_CAP_CMD_OUT, SW2_NATIVE_REPORT_RATE_HANDLE, data, sizeof(data));
    gatt_client_write_value_of_characteristic(
        switch2_v2_handle_write_callback, con_handle, SW2_NATIVE_REPORT_RATE_HANDLE,
        sizeof(data), data);
}

static void switch2_v2_send_ccc_subscribe(hci_con_handle_t con_handle)
{
    const bool common = s_sw2_v2_active &&
        s_sw2_v2_active->subscribe_common_input;
    const uint16_t ccc_handle = common
        ? SW2_CCC_HANDLE : SW2_MOTION_CCC_HANDLE;

    if (common) {
        // The ordinary 0x000A listener is installed by the primary Switch 2
        // connection path. Reassert only its CCC after the feature transition.
        static uint8_t ccc_enable[] = { 0x01, 0x00 };
        sw2_capture_record(
            SW2_CAP_CCC_WRITE, ccc_handle, ccc_enable, sizeof(ccc_enable));
        gatt_client_write_value_of_characteristic(
            switch2_v2_ccc_write_callback, con_handle, ccc_handle,
            sizeof(ccc_enable), ccc_enable);
        return;
    }

    memset(&sw2_motion_characteristic, 0, sizeof(sw2_motion_characteristic));
    sw2_motion_characteristic.value_handle = SW2_MOTION_HANDLE;
    // Live discovery confirms this characteristic spans declaration 0x000D through descriptor
    // 0x0010 (value 0x000E, CCC 0x000F, report-rate descriptor 0x0010).
    sw2_motion_characteristic.end_handle = SW2_NATIVE_REPORT_RATE_HANDLE;

    gatt_client_listen_for_characteristic_value_updates(
        &sw2_motion_notification_listener, sw2_motion_notification_handler, con_handle,
        &sw2_motion_characteristic);

    static uint8_t ccc_enable[] = { 0x01, 0x00 };
    sw2_capture_record(SW2_CAP_CCC_WRITE, SW2_MOTION_CCC_HANDLE, ccc_enable, sizeof(ccc_enable));
    gatt_client_write_value_of_characteristic(
        switch2_v2_ccc_write_callback, con_handle, SW2_MOTION_CCC_HANDLE, sizeof(ccc_enable), ccc_enable);
}

static void switch2_v2_ccc_write_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) return;

    uint8_t status = gatt_event_query_complete_get_att_status(packet);
    hci_con_handle_t con_handle = gatt_event_query_complete_get_handle(packet);
    const uint16_t ccc_handle =
        s_sw2_v2_active && s_sw2_v2_active->subscribe_common_input
            ? SW2_CCC_HANDLE : SW2_MOTION_CCC_HANDLE;
    printf("[SW2_V2] CCC write (0x%04X) status=0x%02X state=%d\n",
           ccc_handle, status, s_sw2_v2_state);
    // Closes a gap found analyzing the first v2 hardware run: this completion status previously
    // only reached printf(), never the capture pipeline. Logged here, not before -- this is the
    // actual ATT-level result of the write BTstack already issued; nothing about when this
    // callback fires or what it does next is changed by adding this one line.
    sw2_capture_record(SW2_CAP_WRITE_STATUS, ccc_handle, &status, 1);
    if (!s_sw2_v2_active) return;

    if (s_sw2_v2_state == SW2_V2_CCC_SUBSCRIBED) {
        // Non-deferred variants: CCC first, then start the command sequence.
        if (s_sw2_v2_active->disable_all_first) {
            s_sw2_v2_state = SW2_V2_DISABLE_SENT;
            switch2_v2_send_disable_all(con_handle);
        } else if (s_sw2_v2_active->send_common_report_select) {
            s_sw2_v2_state = SW2_V2_REPORT_SELECT_SENT;
            switch2_v2_send_common_report_select(con_handle);
        } else {
            s_sw2_v2_state = SW2_V2_CONFIGURE_SENT;
            switch2_v2_send_configure(con_handle);
        }
    } else if (s_sw2_v2_state == SW2_V2_CCC_SUBSCRIBED_LATE) {
        // Deferred variants: this is the last GATT step. Variant 9 adds one asynchronous HCI
        // connection update after it, keeping the controller command sequence identical to v8.
        printf("[SW2_V2] Variant %d complete (deferred CCC subscribe was last step)\n",
               s_sw2_v2_active->id);
        bool request_fast_link = s_sw2_v2_active->request_fast_link;
        s_sw2_v2_state = SW2_V2_DONE;
        s_sw2_v2_active = NULL;
        if (request_fast_link && status == ERROR_CODE_SUCCESS) {
            switch2_v2_request_fast_link(con_handle);
        }
    }
}

static void switch2_v2_handle_write_callback(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) return;

    uint8_t status = gatt_event_query_complete_get_att_status(packet);
    hci_con_handle_t con_handle = gatt_event_query_complete_get_handle(packet);
    printf("[SW2_V2] Handle-write (0x%04X) status=0x%02X\n", SW2_NATIVE_REPORT_RATE_HANDLE, status);
    // Same fix as switch2_v2_ccc_write_callback above -- log the ATT status that was already
    // computed, without changing this callback's timing or subsequent behavior.
    sw2_capture_record(SW2_CAP_WRITE_STATUS, SW2_NATIVE_REPORT_RATE_HANDLE, &status, 1);
    if (!s_sw2_v2_active) return;

    if (s_sw2_v2_active->defer_ccc_subscribe) {
        s_sw2_v2_state = SW2_V2_CCC_SUBSCRIBED_LATE;
        switch2_v2_send_ccc_subscribe(con_handle);
    } else {
        printf("[SW2_V2] Variant %d complete (handle write was last step)\n", s_sw2_v2_active->id);
        s_sw2_v2_state = SW2_V2_DONE;
        s_sw2_v2_active = NULL;
    }
}

// Called from switch2_ack_notification_handler() for every ACK while a v2 variant is active.
// Advances the variant's state machine; a cmd/subcmd that doesn't match the step currently being
// waited on is silently ignored (it belongs to something else — e.g. the primary init sequence
// finishing up, or an unrelated ACK — not an error).
static void switch2_v2_handle_ack(hci_con_handle_t con_handle, uint8_t cmd, uint8_t subcmd)
{
    if (!s_sw2_v2_active) return;

    switch (s_sw2_v2_state) {
        case SW2_V2_DISABLE_SENT:
            if (cmd == 0x0c && subcmd == 0x05) {
                if (s_sw2_v2_active->send_common_report_select) {
                    s_sw2_v2_state = SW2_V2_REPORT_SELECT_SENT;
                    switch2_v2_send_common_report_select(con_handle);
                } else {
                    s_sw2_v2_state = SW2_V2_CONFIGURE_SENT;
                    switch2_v2_send_configure(con_handle);
                }
            }
            break;
        case SW2_V2_REPORT_SELECT_SENT:
            if (cmd == 0x0A && subcmd == 0x02) {
                s_sw2_v2_state = SW2_V2_CONFIGURE_SENT;
                switch2_v2_send_configure(con_handle);
            }
            break;
        case SW2_V2_CONFIGURE_SENT:
            if (cmd == 0x0c && subcmd == 0x02) {
                if (s_sw2_v2_active->do_cal_reads) {
                    s_sw2_v2_cal_index = 0;
                    s_sw2_v2_state = SW2_V2_CAL_READ;
                    switch2_v2_send_next_cal_read(con_handle);
                } else {
                    s_sw2_v2_state = SW2_V2_ENABLE_SENT;
                    switch2_v2_send_enable(con_handle);
                }
            }
            break;
        case SW2_V2_CAL_READ:
            if (cmd == SW2_CMD_READ_SPI && subcmd == SW2_SUBCMD_READ_SPI) {
                s_sw2_v2_cal_index++;
                size_t cal_count = s_sw2_v2_active->use_console_cal_reads
                    ? SW2_V2_CONSOLE_CAL_COUNT : SW2_V2_CAL_COUNT;
                if (s_sw2_v2_active->cal_read_limit != 0u &&
                    s_sw2_v2_active->cal_read_limit < cal_count)
                    cal_count = s_sw2_v2_active->cal_read_limit;
                if (s_sw2_v2_cal_index < cal_count) {
                    switch2_v2_send_next_cal_read(con_handle);
                } else {
                    s_sw2_v2_state = SW2_V2_ENABLE_SENT;
                    switch2_v2_send_enable(con_handle);
                }
            }
            break;
        case SW2_V2_ENABLE_SENT:
            if (cmd == 0x0c && subcmd == 0x04) {
                if (s_sw2_v2_active->do_handle_write) {
                    s_sw2_v2_state = SW2_V2_HANDLE_WRITE_SENT;
                    switch2_v2_send_handle_write(con_handle);
                } else {
                    printf("[SW2_V2] Variant %d complete (enable was last step)\n", s_sw2_v2_active->id);
                    s_sw2_v2_state = SW2_V2_DONE;
                    s_sw2_v2_active = NULL;
                }
            }
            break;
        default:
            break;  // SW2_V2_HANDLE_WRITE_SENT/CCC_SUBSCRIBED_LATE finish via their own callbacks
    }
}

// Run one controller-side native-report setup profile. UART-selected variants use this for
// repeatable RE; the production Pro2 path uses the separately named profile above. The sequence
// intentionally changes the source's active BLE report, so 0x000E input normalization and the
// native report-0x09 side channel are both part of normal operation once it completes.
static void switch2_start_v2_variant(hci_con_handle_t con_handle,
                                     const sw2_v2_variant_t *v, bool automatic)
{
    printf("[SW2_V2] Starting %svariant %d (%s) on handle 0x%04X\n",
           automatic ? "automatic " : "", v->id, v->name, con_handle);
    ns2_native_motion_clear();
    uint8_t vid = v->id;
    sw2_capture_record(SW2_CAP_VARIANT, 0, &vid, 1);

    s_sw2_v2_active = v;
    s_sw2_v2_cal_index = 0;

    if (v->defer_ccc_subscribe) {
        // Variant 6: subscribe LAST, mirroring the reference tool's actual operation order.
        if (v->send_common_report_select) {
            s_sw2_v2_state = SW2_V2_REPORT_SELECT_SENT;
            switch2_v2_send_common_report_select(con_handle);
        } else {
            s_sw2_v2_state = SW2_V2_CONFIGURE_SENT;
            switch2_v2_send_configure(con_handle);
        }
    } else {
        s_sw2_v2_state = SW2_V2_CCC_SUBSCRIBED;
        switch2_v2_send_ccc_subscribe(con_handle);
    }
}

typedef struct {
    uint8_t command;
    uint8_t subcommand;
    uint8_t length;
    const uint8_t *data;
} sw2_magraw_reference_command_t;

static const uint8_t SW2_MAGRAW_INIT_USB[] =
    { 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t SW2_MAGRAW_PAIR_COMPLETE[] = { 0x00 };
static const uint8_t SW2_MAGRAW_FLAGS[] = { 0x94, 0x00, 0x00, 0x00 };
static const uint8_t SW2_MAGRAW_CONFIG[] = {
    0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x35,
    0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t SW2_MAGRAW_REPORT_09[] = { 0x09, 0x00, 0x00, 0x00 };
static const uint8_t SW2_MAGRAW_LED[] =
    { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// Byte-for-byte command list used by the current public Windows implementation
// for a Pro Controller 2, excluding 0x01/0x01: that same implementation
// explicitly skips 0x01/0x01 for PID 0x2069 because the controller returns
// status 4 while all required functions work without it.
static const sw2_magraw_reference_command_t SW2_MAGRAW_REFERENCE[] = {
    { 0x03, 0x0D, sizeof(SW2_MAGRAW_INIT_USB), SW2_MAGRAW_INIT_USB },
    { 0x07, 0x01, 0, NULL },
    { 0x16, 0x01, 0, NULL },
    { 0x15, 0x03, sizeof(SW2_MAGRAW_PAIR_COMPLETE), SW2_MAGRAW_PAIR_COMPLETE },
    { 0x0C, 0x02, sizeof(SW2_MAGRAW_FLAGS), SW2_MAGRAW_FLAGS },
    { 0x11, 0x03, 0, NULL },
    { 0x0A, 0x08, sizeof(SW2_MAGRAW_CONFIG), SW2_MAGRAW_CONFIG },
    { 0x0C, 0x04, sizeof(SW2_MAGRAW_FLAGS), SW2_MAGRAW_FLAGS },
    { 0x03, 0x0A, sizeof(SW2_MAGRAW_REPORT_09), SW2_MAGRAW_REPORT_09 },
    { 0x10, 0x01, 0, NULL },
    { 0x01, 0x0C, 0, NULL },
    { 0x09, 0x07, sizeof(SW2_MAGRAW_LED), SW2_MAGRAW_LED },
};
#define SW2_MAGRAW_REFERENCE_COUNT \
    (sizeof(SW2_MAGRAW_REFERENCE) / sizeof(SW2_MAGRAW_REFERENCE[0]))

static void switch2_magraw_input_ccc_callback(
    uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void switch2_magraw_send_reference_command(hci_con_handle_t con_handle)
{
    if (s_sw2_magraw_reference_step >= SW2_MAGRAW_REFERENCE_COUNT) return;
    const sw2_magraw_reference_command_t *entry =
        &SW2_MAGRAW_REFERENCE[s_sw2_magraw_reference_step];
    uint8_t command[8u + sizeof(SW2_MAGRAW_CONFIG)];
    command[0] = entry->command;
    command[1] = SW2_REQ_TYPE_REQ;
    command[2] = SW2_REQ_INT_BLE;
    command[3] = entry->subcommand;
    command[4] = 0;
    command[5] = entry->length;
    command[6] = 0;
    command[7] = 0;
    if (entry->length) memcpy(&command[8], entry->data, entry->length);

    sw2_capture_record(
        SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, command, 8u + entry->length);
    gatt_client_write_value_of_characteristic_without_response(
        con_handle, SW2_CMD_HANDLE, 8u + entry->length, command);
    s_sw2_magraw_command_sent_ms = btstack_run_loop_get_time_ms();
}

static void switch2_magraw_start_reference(hci_con_handle_t con_handle)
{
    printf("[SW2_MAGRAW] Starting exact reference initialization\n");
    ns2_native_motion_clear();
    s_sw2_magraw_reference_running = true;
    s_sw2_magraw_awaiting_input_ccc = false;
    s_sw2_magraw_reference_step = 0;
    s_sw2_magraw_reference_result = 1; // running
    s_sw2_magraw_last_response_status = 0;
    s_sw2_magraw_input_ccc_status = 0xFF;
    __atomic_store_n(&s_sw2_magraw_transition_pending, true, __ATOMIC_RELEASE);
    switch2_magraw_send_reference_command(con_handle);
}

static void switch2_magraw_begin_restore(hci_con_handle_t con_handle)
{
    printf("[SW2_MAGRAW] Restoring full validated native profile\n");
    s_sw2_magraw_reference_running = false;
    s_sw2_magraw_awaiting_input_ccc = false;
    s_sw2_magraw_transition_target = false;
    __atomic_store_n(&s_sw2_magraw_transition_pending, true, __ATOMIC_RELEASE);
    switch2_start_v2_variant(con_handle, &SW2_NATIVE_PRO2_PROFILE, false);
}

static void switch2_magraw_handle_ack(
    hci_con_handle_t con_handle, const uint8_t *value, uint16_t value_length)
{
    if (!s_sw2_magraw_reference_running || value_length < 4u) return;
    if (s_sw2_magraw_reference_step >= SW2_MAGRAW_REFERENCE_COUNT) return;
    const sw2_magraw_reference_command_t *expected =
        &SW2_MAGRAW_REFERENCE[s_sw2_magraw_reference_step];
    if (value[0] != expected->command || value[3] != expected->subcommand) return;

    s_sw2_magraw_last_response_status = value_length > 1u ? value[1] : 0u;
    if (s_sw2_magraw_last_response_status != 0x01u) {
        printf("[SW2_MAGRAW] Reference step %u rejected, response=0x%02X\n",
               s_sw2_magraw_reference_step,
               s_sw2_magraw_last_response_status);
        s_sw2_magraw_reference_running = false;
        s_sw2_magraw_awaiting_input_ccc = false;
        s_sw2_magraw_reference_result = 3; // rejected
        __atomic_store_n(&s_sw2_magraw_requested, false, __ATOMIC_RELEASE);
        __atomic_store_n(&s_sw2_magraw_active, true, __ATOMIC_RELEASE);
        __atomic_store_n(&s_sw2_magraw_transition_pending, false, __ATOMIC_RELEASE);
        return;
    }

    s_sw2_magraw_reference_step++;
    if (s_sw2_magraw_reference_step < SW2_MAGRAW_REFERENCE_COUNT) {
        switch2_magraw_send_reference_command(con_handle);
        return;
    }

    // Bleak's start_notify(INPUT_REPORT_UUID) runs after the command loop and
    // writes the 0x000A characteristic's CCC at 0x000B. Native profile setup
    // can leave that stream quiescent even if the CCC was written earlier, so
    // reproduce the source order rather than assuming CCC state survives the
    // feature transition.
    printf("[SW2_MAGRAW] Reference commands complete; subscribing input CCC\n");
    s_sw2_magraw_awaiting_input_ccc = true;
    static uint8_t ccc_enable[] = { 0x01, 0x00 };
    sw2_capture_record(
        SW2_CAP_CCC_WRITE, SW2_CCC_HANDLE, ccc_enable, sizeof(ccc_enable));
    gatt_client_write_value_of_characteristic(
        switch2_magraw_input_ccc_callback, con_handle, SW2_CCC_HANDLE,
        sizeof(ccc_enable), ccc_enable);
    s_sw2_magraw_command_sent_ms = btstack_run_loop_get_time_ms();
}

static void switch2_magraw_input_ccc_callback(
    uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET ||
        hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) return;
    if (!s_sw2_magraw_reference_running ||
        !s_sw2_magraw_awaiting_input_ccc) return;

    s_sw2_magraw_input_ccc_status =
        gatt_event_query_complete_get_att_status(packet);
    sw2_capture_record(
        SW2_CAP_WRITE_STATUS, SW2_CCC_HANDLE,
        &s_sw2_magraw_input_ccc_status, 1);
    s_sw2_magraw_reference_running = false;
    s_sw2_magraw_awaiting_input_ccc = false;
    if (s_sw2_magraw_input_ccc_status == ERROR_CODE_SUCCESS) {
        printf("[SW2_MAGRAW] Exact reference initialization complete\n");
        s_sw2_magraw_reference_result = 2; // success
        __atomic_store_n(&s_sw2_magraw_active, true, __ATOMIC_RELEASE);
        __atomic_store_n(
            &s_sw2_magraw_transition_pending, false, __ATOMIC_RELEASE);
    } else {
        printf("[SW2_MAGRAW] Input CCC rejected, ATT status=0x%02X\n",
               s_sw2_magraw_input_ccc_status);
        s_sw2_magraw_reference_result = 5; // input CCC failure
        __atomic_store_n(&s_sw2_magraw_requested, false, __ATOMIC_RELEASE);
        __atomic_store_n(&s_sw2_magraw_active, true, __ATOMIC_RELEASE);
        __atomic_store_n(
            &s_sw2_magraw_transition_pending, false, __ATOMIC_RELEASE);
    }
}

static void switch2_run_v2_experiment(hci_con_handle_t con_handle)
{
    uint8_t n = s_sw2_v2_armed_variant;
    if (n < 1 || n > SW2_V2_VARIANT_COUNT) return;
    switch2_start_v2_variant(con_handle, &SW2_V2_VARIANTS[n - 1], false);
}

static void switch2_service_magraw_probe(void)
{
    if (sw2_init_state != SW2_INIT_DONE || sw2_init_handle == 0) return;
    ble_connection_t *conn = find_connection_by_handle(sw2_init_handle);
    if (!conn || conn->pid != 0x2069) return;
    if (__atomic_load_n(&s_sw2_imuref_requested, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_sw2_imuref_active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_sw2_imuref_transition_pending, __ATOMIC_ACQUIRE))
        return;

    if (s_sw2_magraw_reference_running) {
        const uint32_t now_ms = btstack_run_loop_get_time_ms();
        if ((uint32_t)(now_ms - s_sw2_magraw_command_sent_ms) > 2000u) {
            printf("[SW2_MAGRAW] Reference step %u timed out\n",
                   s_sw2_magraw_reference_step);
            s_sw2_magraw_reference_running = false;
            s_sw2_magraw_awaiting_input_ccc = false;
            s_sw2_magraw_reference_result = 4; // timeout
            __atomic_store_n(&s_sw2_magraw_requested, false, __ATOMIC_RELEASE);
            __atomic_store_n(&s_sw2_magraw_active, true, __ATOMIC_RELEASE);
            __atomic_store_n(
                &s_sw2_magraw_transition_pending, false, __ATOMIC_RELEASE);
        }
        return;
    }

    if (__atomic_load_n(&s_sw2_magraw_transition_pending, __ATOMIC_ACQUIRE)) {
        if (s_sw2_v2_active == NULL && s_sw2_v2_state == SW2_V2_DONE) {
            __atomic_store_n(
                &s_sw2_magraw_active, s_sw2_magraw_transition_target,
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &s_sw2_magraw_transition_pending, false, __ATOMIC_RELEASE);
        }
        return;
    }

    const bool requested = __atomic_load_n(
        &s_sw2_magraw_requested, __ATOMIC_ACQUIRE);
    const bool active = __atomic_load_n(
        &s_sw2_magraw_active, __ATOMIC_ACQUIRE);
    if (requested == active) return;
    if (s_sw2_v2_active != NULL ||
        (s_sw2_v2_state != SW2_V2_IDLE && s_sw2_v2_state != SW2_V2_DONE)) return;

    s_sw2_magraw_transition_target = requested;
    if (requested) {
        switch2_magraw_start_reference(sw2_init_handle);
    } else {
        switch2_magraw_begin_restore(sw2_init_handle);
    }
}

static void switch2_imuref_unsubscribe_callback(
    uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET ||
        hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE)
        return;
    if (!s_sw2_imuref_awaiting_unsubscribe) return;

    const uint8_t status = gatt_event_query_complete_get_att_status(packet);
    const hci_con_handle_t con_handle =
        gatt_event_query_complete_get_handle(packet);
    const uint16_t ccc_handle = s_sw2_imuref_transition_target
        ? SW2_MOTION_CCC_HANDLE : SW2_CCC_HANDLE;
    __atomic_store_n(&s_sw2_imuref_last_att_status, status, __ATOMIC_RELEASE);
    sw2_capture_record(SW2_CAP_WRITE_STATUS, ccc_handle, &status, 1);
    s_sw2_imuref_awaiting_unsubscribe = false;

    if (status != ERROR_CODE_SUCCESS && s_sw2_imuref_transition_target) {
        // Fail closed: native reporting is still intact, so do not layer the
        // raw profile on top of an input stream we failed to disable.
        printf("[SW2_IMUREF] Native CCC unsubscribe failed status=0x%02X\n",
               status);
        __atomic_store_n(&s_sw2_imuref_requested, false, __ATOMIC_RELEASE);
        __atomic_store_n(
            &s_sw2_imuref_transition_pending, false, __ATOMIC_RELEASE);
        return;
    }

    if (s_sw2_imuref_transition_target) {
        printf("[SW2_IMUREF] Native CCC disabled; selecting raw-IMU profile\n");
        switch2_start_v2_variant(
            con_handle, &SW2_RAW_IMU_REFERENCE_PROFILE, false);
    } else {
        // Restore native even if the common unsubscribe returned an error.
        // Selecting the production profile is the safest terminal state.
        printf("[SW2_IMUREF] Common CCC disabled; restoring native profile\n");
        switch2_start_v2_variant(
            con_handle, &SW2_NATIVE_PRO2_PROFILE, false);
    }
}

static void switch2_imuref_disable_current_ccc(
    hci_con_handle_t con_handle, bool select_raw)
{
    static uint8_t ccc_disable[] = { 0x00, 0x00 };
    const uint16_t ccc_handle = select_raw
        ? SW2_MOTION_CCC_HANDLE : SW2_CCC_HANDLE;
    s_sw2_imuref_awaiting_unsubscribe = true;
    __atomic_store_n(&s_sw2_imuref_last_att_status, 0xFF, __ATOMIC_RELEASE);
    sw2_capture_record(
        SW2_CAP_CCC_WRITE, ccc_handle, ccc_disable, sizeof(ccc_disable));
    gatt_client_write_value_of_characteristic(
        switch2_imuref_unsubscribe_callback, con_handle, ccc_handle,
        sizeof(ccc_disable), ccc_disable);
}

static void switch2_imuref_dual_ccc_callback(
    uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET ||
        hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE)
        return;
    if (!__atomic_load_n(
            &s_sw2_imuref_dual_transition_pending, __ATOMIC_ACQUIRE))
        return;

    const uint8_t status = gatt_event_query_complete_get_att_status(packet);
    __atomic_store_n(
        &s_sw2_imuref_dual_att_status, status, __ATOMIC_RELEASE);
    sw2_capture_record(
        SW2_CAP_WRITE_STATUS, SW2_MOTION_CCC_HANDLE, &status, 1);
    if (status == ERROR_CODE_SUCCESS) {
        const bool requested = __atomic_load_n(
            &s_sw2_imuref_dual_requested, __ATOMIC_ACQUIRE);
        __atomic_store_n(
            &s_sw2_imuref_dual_active, requested, __ATOMIC_RELEASE);
    }
    __atomic_store_n(
        &s_sw2_imuref_dual_transition_pending, false, __ATOMIC_RELEASE);
}

static void switch2_service_imuref_dual_probe(hci_con_handle_t con_handle)
{
    if (!__atomic_load_n(&s_sw2_imuref_active, __ATOMIC_ACQUIRE))
        return;
    if (__atomic_load_n(
            &s_sw2_imuref_dual_transition_pending, __ATOMIC_ACQUIRE))
        return;

    const bool requested = __atomic_load_n(
        &s_sw2_imuref_dual_requested, __ATOMIC_ACQUIRE);
    const bool active = __atomic_load_n(
        &s_sw2_imuref_dual_active, __ATOMIC_ACQUIRE);
    if (requested == active) return;

    static uint8_t ccc_enable[] = { 0x01, 0x00 };
    static uint8_t ccc_disable[] = { 0x00, 0x00 };
    uint8_t *value = requested ? ccc_enable : ccc_disable;
    __atomic_store_n(
        &s_sw2_imuref_dual_att_status, 0xFF, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_imuref_dual_transition_pending, true, __ATOMIC_RELEASE);
    sw2_capture_record(
        SW2_CAP_CCC_WRITE, SW2_MOTION_CCC_HANDLE, value, 2u);
    gatt_client_write_value_of_characteristic(
        switch2_imuref_dual_ccc_callback, con_handle,
        SW2_MOTION_CCC_HANDLE, 2u, value);
}

static void switch2_service_imuref_interval_probe(
    hci_con_handle_t con_handle)
{
    const uint16_t interval = __atomic_exchange_n(
        &s_sw2_imuref_interval_pending_units, 0u, __ATOMIC_ACQ_REL);
    if (interval == 0u) return;

    // UART constrains this to 7.5--30 ms. Latency zero keeps every connection
    // event observable; the production profile restores six units (7.5 ms).
    const int status = gap_update_connection_parameters(
        con_handle, interval, interval, 0u, 400u);
    __atomic_store_n(
        &s_sw2_imuref_interval_request_status, (uint8_t)status,
        __ATOMIC_RELEASE);
    switch2_capture_link_params(
        SW2_LINK_PHASE_REQUEST, (uint8_t)status, con_handle,
        interval, 0u, 400u);
}

static void switch2_service_imuref_probe(void)
{
    if (sw2_init_state != SW2_INIT_DONE || sw2_init_handle == 0) return;
    ble_connection_t *conn = find_connection_by_handle(sw2_init_handle);
    if (!conn || conn->pid != 0x2069) return;

    // Never let two UART-only report-selection experiments contend for the
    // shared controller command/CCC state machine.
    if (__atomic_load_n(&s_sw2_magraw_requested, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_sw2_magraw_active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_sw2_magraw_transition_pending, __ATOMIC_ACQUIRE))
        return;

    if (__atomic_load_n(&s_sw2_imuref_transition_pending, __ATOMIC_ACQUIRE)) {
        if (s_sw2_imuref_awaiting_unsubscribe) return;
        if (s_sw2_v2_active == NULL && s_sw2_v2_state == SW2_V2_DONE) {
            __atomic_store_n(
                &s_sw2_imuref_active, s_sw2_imuref_transition_target,
                __ATOMIC_RELEASE);
            if (!s_sw2_imuref_transition_target) {
                __atomic_store_n(
                    &s_sw2_imuref_dual_requested, false, __ATOMIC_RELEASE);
                __atomic_store_n(
                    &s_sw2_imuref_dual_active, false, __ATOMIC_RELEASE);
                __atomic_store_n(
                    &s_sw2_imuref_dual_transition_pending, false,
                    __ATOMIC_RELEASE);
                // The production restore profile has already requested the
                // validated six-unit (7.5 ms) interval. Keep diagnostics
                // aligned with that restored state instead of displaying the
                // last UART-only experiment target.
                __atomic_store_n(
                    &s_sw2_imuref_interval_target_units, 6u,
                    __ATOMIC_RELEASE);
                __atomic_store_n(
                    &s_sw2_imuref_interval_pending_units, 0u,
                    __ATOMIC_RELEASE);
            }
            __atomic_store_n(
                &s_sw2_imuref_transition_pending, false, __ATOMIC_RELEASE);
        }
        return;
    }

    const bool requested = __atomic_load_n(
        &s_sw2_imuref_requested, __ATOMIC_ACQUIRE);
    const bool active = __atomic_load_n(
        &s_sw2_imuref_active, __ATOMIC_ACQUIRE);
    if (requested == active) {
        if (active)
            switch2_service_imuref_interval_probe(sw2_init_handle);
        switch2_service_imuref_dual_probe(sw2_init_handle);
        return;
    }
    if (__atomic_load_n(
            &s_sw2_imuref_dual_transition_pending, __ATOMIC_ACQUIRE))
        return;
    if (s_sw2_v2_active != NULL ||
        (s_sw2_v2_state != SW2_V2_IDLE && s_sw2_v2_state != SW2_V2_DONE))
        return;

    s_sw2_imuref_transition_target = requested;
    __atomic_store_n(
        &s_sw2_imuref_transition_pending, true, __ATOMIC_RELEASE);
    if (requested) {
        __atomic_store_n(&s_sw2_imuref_common_notifications, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_sw2_imuref_native_notifications, 0, __ATOMIC_RELEASE);
        printf("[SW2_IMUREF] Disabling native CCC before raw-IMU selection\n");
        switch2_imuref_disable_current_ccc(sw2_init_handle, true);
    } else {
        printf("[SW2_IMUREF] Disabling common CCC before native restore\n");
        switch2_imuref_disable_current_ccc(sw2_init_handle, false);
    }
}

// Cleanup Switch 2 state on BLE disconnect (called from disconnect handler)
static void switch2_cleanup_on_disconnect(uint8_t source_conn_index,
                                          uint32_t source_generation) {
    gatt_client_stop_listening_for_characteristic_value_updates(&switch2_ack_notification_listener);
    gatt_client_stop_listening_for_characteristic_value_updates(&sw2_motion_notification_listener);
    gatt_client_stop_listening_for_characteristic_value_updates(&sw2_pro2_audio_notification_listener);
    gatt_client_stop_listening_for_characteristic_value_updates(
        &sw2_nfc_mirror_notification_listener);
    sw2_init_state = SW2_INIT_IDLE;
    sw2_init_handle = 0;
    // Explicit reset (belt-and-suspenders with the state-change check in switch2_send_init_cmd()):
    // a new session must never inherit a retry count or deadline phase from whatever this
    // connection's init sequence last did, even in the edge case where the new session happens to
    // get stuck at the exact same sw2_init_state as the old one.
    sw2_init_retry_count = 0;
    sw2_init_cmd_sent_ms = 0;
    sw2_init_last_sent_state = SW2_INIT_IDLE;
    switch2_link_encrypted = false;
    switch2_link_encrypted_handle = HCI_CON_HANDLE_INVALID;
    sw2_pairing_ltk_handle = HCI_CON_HANDLE_INVALID;
    switch2_direct_reencrypt_active = false;
    switch2_direct_reencrypt_handle = HCI_CON_HANDLE_INVALID;
    switch2_direct_encrypt_phase = SW2_ENCRYPT_NONE;
    s_sw2_v2_fired = false;
    s_sw2_native_auto_fired = false;
    __atomic_store_n(&s_sw2_magraw_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_magraw_active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_magraw_transition_pending, false, __ATOMIC_RELEASE);
    s_sw2_magraw_reference_running = false;
    s_sw2_magraw_awaiting_input_ccc = false;
    s_sw2_magraw_reference_step = 0;
    s_sw2_magraw_reference_result = 0;
    s_sw2_magraw_last_response_status = 0;
    s_sw2_magraw_input_ccc_status = 0;
    s_sw2_magraw_command_sent_ms = 0;
    __atomic_store_n(&s_sw2_imuref_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_imuref_active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_imuref_transition_pending, false, __ATOMIC_RELEASE);
    s_sw2_imuref_awaiting_unsubscribe = false;
    __atomic_store_n(&s_sw2_imuref_last_att_status, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_imuref_dual_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_imuref_dual_active, false, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_imuref_dual_transition_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_imuref_dual_att_status, 0, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_imuref_interval_pending_units, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_imuref_interval_target_units, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_imuref_interval_request_status, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_imuref_common_notifications, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_imuref_native_notifications, 0, __ATOMIC_RELEASE);
    s_sw2_init_done_ms = 0;
    __atomic_store_n(&s_sw2_native_auto_checks, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_native_auto_starts, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_native_auto_wait_elapsed_ms, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_native_auto_source_pid, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_native_auto_personality, 0xFF, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sw2_native_auto_block_mask, 0, __ATOMIC_RELEASE);
    s_sw2_v2_state = SW2_V2_IDLE;
    s_sw2_v2_active = NULL;
    s_sw2_gatt_disc_fired = false;
    s_gatt_disc_state = SW2_GATT_DISC_IDLE;
    s_sw2_pro2_audio_state = SW2_PRO2_AUDIO_OFF;
    s_sw2_pro2_audio_last_att_status = 0;
    s_sw2_pro2_audio_last_headset_raw = 0;
    s_sw2_pro2_audio_last_data_len = 0;
    s_sw2_pro2_audio_last_report_len = 0;
    s_sw2_pro2_audio_max_report_len = 0;
    s_sw2_pro2_audio_notifications = 0;
    s_sw2_pro2_audio_compact_failures = 0;
    __atomic_store_n(&s_sw2_nfc_mirror_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_nfc_mirror_state, NS2_NFC_MIRROR_OFF, __ATOMIC_RELEASE);
    s_sw2_nfc_mirror_last_att_status = 0;
    s_sw2_nfc_mirror_last_send_status = 0;
    s_sw2_nfc_mirror_last_command = 0;
    s_sw2_nfc_mirror_last_subcommand = 0;
    s_sw2_nfc_mirror_report_state = 0;
    s_sw2_nfc_mirror_last_response_length = 0;
    s_sw2_nfc_mirror_commands_submitted = 0;
    s_sw2_nfc_mirror_commands_sent = 0;
    s_sw2_nfc_mirror_notifications = 0;
    s_sw2_nfc_mirror_report_state_transitions = 0;
    s_sw2_nfc_mirror_response_timeouts = 0;
    s_sw2_nfc_mirror_rejected = 0;
    s_sw2_nfc_mirror_command_length = 0;
    s_sw2_nfc_mirror_response_length = 0;
    s_sw2_nfc_mirror_send_failures = 0;
    s_sw2_nfc_mirror_next_send_ms = 0;
    s_sw2_nfc_mirror_awaiting_since_ms = 0;
    __atomic_store_n(
        &s_sw2_nfc_mirror_command_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_nfc_mirror_awaiting_response, false, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_nfc_mirror_response_ready, false, __ATOMIC_RELEASE);
    __atomic_store_n(
        &s_sw2_nfc_mirror_slot_claimed, false, __ATOMIC_RELEASE);
    s_sw2_motion_last_notification_us = 0;
    __atomic_store_n(&s_sw2_pro2_replay_requested, false, __ATOMIC_RELEASE);
    s_sw2_pro2_replay_state = SW2_PRO2_REPLAY_IDLE;
    s_sw2_pro2_replay_last_send_status = 0;
    s_sw2_pro2_replay_frames_sent = 0;
    ds5_audio_bridge_set_switch2_pro2_active(false);
    s_sw2_pro2_live_state = SW2_PRO2_LIVE_IDLE;
    s_sw2_pro2_live_last_send_status = 0;
    s_sw2_pro2_live_last_toc = 0;
    memset(s_sw2_pro2_live_prefix, 0, sizeof(s_sw2_pro2_live_prefix));
    s_sw2_pro2_live_frames_sent = 0;
    s_sw2_pro2_live_underruns = 0;
    s_sw2_pro2_live_prime_count = 0;
    if (source_conn_index != 0xFFu) {
        (void)ns2_native_motion_source_disconnected_generation(
            source_conn_index, source_generation, time_us_32());
    }
    uint8_t version_state = __atomic_load_n(&s_sw2_version_state, __ATOMIC_ACQUIRE);
    if (version_state == NS2_BT_VERSION_REQUESTED || version_state == NS2_BT_VERSION_SENT) {
        __atomic_store_n(&s_sw2_version_state, NS2_BT_VERSION_NO_CONTROLLER,
                         __ATOMIC_RELEASE);
    }
}

// Forward declare
static void switch2_send_init_cmd(hci_con_handle_t con_handle);

static void switch2_ack_notification_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
    uint16_t value_length = gatt_event_notification_get_value_length(packet);
    const uint8_t *value = gatt_event_notification_get_value(packet);
    hci_con_handle_t con_handle = gatt_event_notification_get_handle(packet);

    // Non-invasive raw capture (see sw2_capture.h), ahead of the 0x001A filter below — this
    // listener has been observed receiving notifications on handles other than 0x001A (see the
    // pre-existing debug log just below), so capturing here, not after the filter, is what
    // makes "every observed handle" actually true rather than assumed.
    sw2_capture_record(SW2_CAP_ACK_NOTIFY, value_handle, value, value_length);

    // Debug: print all notifications (not just 0x001A) to see what's coming in
    static bool ack_notif_debug = false;
    if (!ack_notif_debug && value_handle != SW2_INPUT_REPORT_HANDLE) {
        printf("[SW2_BLE] ACK listener got notification: handle=0x%04X len=%d\n",
               value_handle, value_length);
        ack_notif_debug = true;
    }

    if (value_handle != 0x001A) return;  // ACK handle

    if (value_length < 4) return;
    uint8_t cmd = value[0];
    uint8_t subcmd = value[3];
    if (cmd == 0x01) {
        (void)ns2_nfc_mirror_accept_ble_response(value, value_length);
    }

    if (cmd == 0x10 && subcmd == 0x01 &&
        __atomic_load_n(&s_sw2_version_state, __ATOMIC_ACQUIRE) == NS2_BT_VERSION_SENT) {
        if (value_length >= 20) {
            memcpy(s_sw2_version_raw, &value[8], sizeof(s_sw2_version_raw));
            s_sw2_version_length = sizeof(s_sw2_version_raw);
            __atomic_store_n(&s_sw2_version_state, NS2_BT_VERSION_READY,
                             __ATOMIC_RELEASE);
        } else {
            s_sw2_version_length = value_length > 8 ? (uint8_t)(value_length - 8) : 0;
            if (s_sw2_version_length > sizeof(s_sw2_version_raw))
                s_sw2_version_length = sizeof(s_sw2_version_raw);
            memset(s_sw2_version_raw, 0, sizeof(s_sw2_version_raw));
            if (s_sw2_version_length)
                memcpy(s_sw2_version_raw, &value[8], s_sw2_version_length);
            __atomic_store_n(&s_sw2_version_state, NS2_BT_VERSION_PROTOCOL_ERROR,
                             __ATOMIC_RELEASE);
        }
    }

    printf("[SW2_BLE] ACK: cmd=0x%02X subcmd=0x%02X state=%d len=%d\n",
           cmd, subcmd, sw2_init_state, value_length);

    // Route to the v2 experiment's state machine too, if one is active (see sw2_capture.h). Safe
    // to call unconditionally — it no-ops immediately when no variant is running, and only ever
    // runs after SW2_INIT_DONE, so it can never observe an ACK the primary switch() below is
    // still using to drive pairing.
    switch2_v2_handle_ack(con_handle, cmd, subcmd);
    switch2_magraw_handle_ack(con_handle, value, value_length);

    // Handle ACK based on current init state
    switch (cmd) {
        case SW2_CMD_READ_SPI:
            if (sw2_init_state == SW2_INIT_READ_INFO) {
                // Got device info, extract VID/PID if needed
                if (value_length >= 34) {
                    uint16_t vid = value[30] | (value[31] << 8);
                    uint16_t pid = value[32] | (value[33] << 8);
                    printf("[SW2_BLE] Device info: VID=0x%04X PID=0x%04X\n", vid, pid);
                    // These legacy offsets are retained as an investigation
                    // log only. Hardware returned 0x3238/0x0000 here for a
                    // confirmed 0x057E/0x2069 Pro Controller 2, so they must
                    // not replace the validated advertising identity.
                }
                // Read the controller's existing bond key before deciding
                // whether custom pairing is necessary. This read does not
                // alter controller state.
                sw2_init_state = SW2_INIT_READ_LTK;
                switch2_send_init_cmd(con_handle);
            } else if (sw2_init_state == SW2_INIT_READ_LTK) {
                bool valid_ltk_response = value_length >= 32 && value[8] >= 16 &&
                    value[12] == 0x1A && value[13] == 0xA0 &&
                    value[14] == 0x1F && value[15] == 0x00;
                if (valid_ltk_response) {
                    switch2_record_pairing_ltk(con_handle, &value[16], 1);
                }

                // An already encrypted HOME reconnect with the same SPI key
                // must not rewrite the controller's bond. A SYNC/fresh link is
                // unencrypted here and intentionally runs the custom pairing
                // exchange even if stale local storage happens to match.
                bool encrypted_key_matches = valid_ltk_response &&
                    gap_encryption_key_size(con_handle) > 0 &&
                    hid_state.has_last_connected_ltk &&
                    memcmp(hid_state.last_connected_ltk,
                           sw2_pairing_ltk_normalized, 16) == 0;
                sw2_init_state = encrypted_key_matches ?
                    SW2_INIT_SET_LED : SW2_INIT_PAIR_STEP1;
                printf("[SW2_BLE] Existing bond %s; %s custom pairing\n",
                       encrypted_key_matches ? "matches encrypted link" : "not reusable",
                       encrypted_key_matches ? "skipping" : "running");
                switch2_send_init_cmd(con_handle);
            } else if (sw2_init_state == SW2_INIT_READ_NEW_LTK) {
                bool valid_ltk_response = value_length >= 32 && value[8] >= 16 &&
                    value[12] == 0x1A && value[13] == 0xA0 &&
                    value[14] == 0x1F && value[15] == 0x00;
                if (valid_ltk_response) {
                    switch2_record_pairing_ltk(con_handle, &value[16], 2);
                } else {
                    printf("[SW2_BLE] Post-pairing SPI LTK response malformed (len=%u)\n",
                           value_length);
                }
                if (valid_ltk_response) {
                    // The custom 0x15 exchange establishes and stores the
                    // relationship, but this initial SYNC link remains
                    // unencrypted. The LTK is first exercised through BTstack
                    // SM re-encryption on a later HOME reconnect. Hardware
                    // returns 0x06 if encryption is forced here immediately,
                    // even though the SPI key is readable.
                    sw2_init_state = SW2_INIT_SET_LED;
                    switch2_send_init_cmd(con_handle);
                } else {
                    // Retain a bounded compatibility path for firmware that
                    // answers the custom exchange but not the SPI read.
                    sw2_init_state = SW2_INIT_SET_LED;
                    switch2_send_init_cmd(con_handle);
                }
            }
            break;

        case SW2_CMD_PAIRING:
            switch (subcmd) {
                case SW2_SUBCMD_PAIRING_STEP1:
                    if (sw2_init_state == SW2_INIT_PAIR_STEP1) {
                        sw2_init_state = SW2_INIT_PAIR_STEP2;
                        switch2_send_init_cmd(con_handle);
                    }
                    break;
                case SW2_SUBCMD_PAIRING_STEP2:
                    if (sw2_init_state == SW2_INIT_PAIR_STEP2) {
                        sw2_init_state = SW2_INIT_PAIR_STEP3;
                        switch2_send_init_cmd(con_handle);
                    }
                    break;
                case SW2_SUBCMD_PAIRING_STEP3:
                    if (sw2_init_state == SW2_INIT_PAIR_STEP3) {
                        sw2_init_state = SW2_INIT_PAIR_STEP4;
                        switch2_send_init_cmd(con_handle);
                    }
                    break;
                case SW2_SUBCMD_PAIRING_STEP4:
                    if (sw2_init_state == SW2_INIT_PAIR_STEP4) {
                        printf("[SW2_BLE] Pairing complete! Reading stored LTK...\n");
                        sw2_init_state = SW2_INIT_READ_NEW_LTK;
                        switch2_send_init_cmd(con_handle);
                    }
                    break;
            }
            break;

        case SW2_CMD_SET_LED:
            if (sw2_init_state == SW2_INIT_SET_LED) {
                ble_connection_t *conn =
                    find_connection_by_handle(con_handle);
                bool encrypted_reconnect = conn && switch2_link_encrypted &&
                    switch2_link_encrypted_handle == con_handle &&
                    gap_encryption_key_size(con_handle) > 0u;
                bool fresh_custom =
                    conn && conn->fresh_pairing_admitted;
                if (pairing_lockout || (!encrypted_reconnect && !fresh_custom)) {
                    btstack_host_record_fresh_admission(false);
                    printf("[SW2_BLE] Final custom admission missing; disconnecting before readiness\n");
                    sw2_init_state = SW2_INIT_IDLE;
                    gap_disconnect(con_handle);
                    break;
                }
                printf("[SW2_BLE] LED set! Init done.\n");
                sw2_init_state = SW2_INIT_DONE;
                s_sw2_init_done_ms = btstack_run_loop_get_time_ms();
                // The Nintendo custom ATT path deliberately skips BTstack SM,
                // so this successful terminal ACK is its equivalent of a
                // pairing-complete event. Persist the peer now, never merely
                // from an advertisement or raw ACL connection.
                btstack_host_remember_ble_connection(conn);
                // Terminal state — not followed by switch2_send_init_cmd(), so captured here
                // directly (every other transition is captured at the top of that function).
                uint8_t s = (uint8_t)SW2_INIT_DONE;
                sw2_capture_record(SW2_CAP_STATE, 0, &s, 1);

                // Init is now terminal and no GATT command request is in
                // flight. Enable the input CCC as its own serialized request;
                // its completion callback publishes the device to bthid.
                static uint8_t ccc_enable[] = { 0x01, 0x00 };
                printf("[SW2_BLE] Enabling input notifications on CCC handle 0x%04X\n",
                       SW2_CCC_HANDLE);
                sw2_capture_record(SW2_CAP_CCC_WRITE, SW2_CCC_HANDLE,
                                   ccc_enable, sizeof(ccc_enable));
                gatt_client_write_value_of_characteristic(
                    switch2_ccc_write_callback, con_handle, SW2_CCC_HANDLE,
                    sizeof(ccc_enable), ccc_enable);
            }
            break;
    }
}

static void switch2_send_init_cmd(hci_con_handle_t con_handle)
{
    printf("[SW2_BLE] Sending init cmd, state=%d\n", sw2_init_state);

    // Every send (whether the first for this state or a retry) refreshes the deadline. A genuinely
    // NEW state (different from the last one this function was called for) also resets the retry
    // count, so a slow-to-arrive step never inherits a near-exhausted budget from the step before it.
    if (sw2_init_state != sw2_init_last_sent_state) {
        sw2_init_retry_count = 0;
        sw2_init_last_sent_state = sw2_init_state;
    }
    sw2_init_cmd_sent_ms = btstack_run_loop_get_time_ms();

    // Non-invasive capture (see sw2_capture.h): every SW2_INIT_* state this host acts on. Every
    // sw2_init_state transition in switch2_ack_notification_handler() is immediately followed
    // by a call to this function (except the terminal SW2_INIT_DONE, captured separately at its
    // assignment site) or by switch2_send_next_init_cmd(), so capturing here covers the sequence.
    {
        uint8_t s = (uint8_t)sw2_init_state;
        sw2_capture_record(SW2_CAP_STATE, 0, &s, 1);
    }

    switch (sw2_init_state) {
        case SW2_INIT_READ_INFO: {
            // Read the controller identity block from SPI.
            uint8_t read_info[] = {
                SW2_CMD_READ_SPI,       // 0x02
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_READ_SPI,    // 0x04
                0x00, 0x08, 0x00, 0x00,
                0x40,                   // Read length
                0x7e, 0x00, 0x00,       // Address type
                0x00, 0x30, 0x01, 0x00  // SPI address
            };
            sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, read_info, sizeof(read_info));
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(read_info), read_info);
            printf("[SW2_BLE] READ_INFO sent\n");
            break;
        }

        case SW2_INIT_READ_LTK:
        case SW2_INIT_READ_NEW_LTK: {
            // Read only the key field from the first bond record. Response
            // data begins at ACK byte 16.
            uint8_t read_ltk[] = {
                SW2_CMD_READ_SPI,
                SW2_REQ_TYPE_REQ,
                SW2_REQ_INT_BLE,
                SW2_SUBCMD_READ_SPI,
                0x00, 0x08, 0x00, 0x00,
                0x10,
                0x7e, 0x00, 0x00,
                0x1a, 0xa0, 0x1f, 0x00
            };
            sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE,
                               read_ltk, sizeof(read_ltk));
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(read_ltk), read_ltk);
            printf("[SW2_BLE] READ_%s_LTK sent\n",
                   sw2_init_state == SW2_INIT_READ_NEW_LTK ? "NEW" : "EXISTING");
            break;
        }

        case SW2_INIT_PAIR_STEP1: {
            // Pairing step 1 consumes the controller address in the raw order
            // returned by HCI Read BD_ADDR. BTstack exposes
            // gap_local_bd_addr() in display order, so convert at this API
            // boundary. A prior SPI
            // table read showed that the controller transforms this field when
            // storing its bond record; that stored representation must not be
            // mistaken for the ATT command's byte order. Using display order
            // here produced a readable LTK and working fresh input, but HOME
            // reconnect repeatedly stalled after LE Start Encryption because
            // the bond belonged to the wrong serialized host identity.
            bd_addr_t local_addr;
            gap_local_bd_addr(local_addr);
            uint8_t hci_addr[6] = {
                local_addr[5], local_addr[4], local_addr[3],
                local_addr[2], local_addr[1], local_addr[0],
            };
            printf("[SW2_BLE] Pair Step 1: BD addr = %02X:%02X:%02X:%02X:%02X:%02X\n",
                   local_addr[0], local_addr[1], local_addr[2],
                   local_addr[3], local_addr[4], local_addr[5]);

            uint8_t pair1[] = {
                SW2_CMD_PAIRING,        // 0x15
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_PAIRING_STEP1, // 0x01
                0x00, 0x0e, 0x00, 0x00, 0x00, 0x02,
                // 6 bytes: our raw HCI-order BD addr
                hci_addr[0], hci_addr[1], hci_addr[2],
                hci_addr[3], hci_addr[4], hci_addr[5],
                // 6 bytes: the second controller bond identity decrements raw byte 0
                (uint8_t)(hci_addr[0] - 1), hci_addr[1], hci_addr[2],
                hci_addr[3], hci_addr[4], hci_addr[5],
            };
            sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, pair1, sizeof(pair1));
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(pair1), pair1);
            break;
        }

        case SW2_INIT_PAIR_STEP2: {
            // Pairing step 2: captured controller handshake bytes.
            uint8_t pair2[] = {
                SW2_CMD_PAIRING,        // 0x15
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_PAIRING_STEP2, // 0x04
                0x00, 0x11, 0x00, 0x00, 0x00,
                0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42, 0xc6,
                0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31
            };
            sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, pair2, sizeof(pair2));
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(pair2), pair2);
            printf("[SW2_BLE] Pair Step 2 sent\n");
            break;
        }

        case SW2_INIT_PAIR_STEP3: {
            // Pairing step 3: More magic bytes
            uint8_t pair3[] = {
                SW2_CMD_PAIRING,        // 0x15
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_PAIRING_STEP3, // 0x02
                0x00, 0x11, 0x00, 0x00, 0x00,
                0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b, 0x41,
                0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73
            };
            sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, pair3, sizeof(pair3));
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(pair3), pair3);
            printf("[SW2_BLE] Pair Step 3 sent\n");
            break;
        }

        case SW2_INIT_PAIR_STEP4: {
            // Pairing step 4: Completion
            uint8_t pair4[] = {
                SW2_CMD_PAIRING,        // 0x15
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_PAIRING_STEP4, // 0x03
                0x00, 0x01, 0x00, 0x00, 0x00
            };
            sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, pair4, sizeof(pair4));
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(pair4), pair4);
            printf("[SW2_BLE] Pair Step 4 sent\n");
            break;
        }

        case SW2_INIT_SET_LED: {
            // Set player LED
            uint8_t led_cmd[] = {
                SW2_CMD_SET_LED,        // 0x09
                SW2_REQ_TYPE_REQ,       // 0x91
                SW2_REQ_INT_BLE,        // 0x01
                SW2_SUBCMD_SET_LED,     // 0x07
                0x00, 0x08, 0x00, 0x00,
                0x01,  // Player 1 LED pattern
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, led_cmd, sizeof(led_cmd));
            gatt_client_write_value_of_characteristic_without_response(
                con_handle, SW2_CMD_HANDLE, sizeof(led_cmd), led_cmd);
            printf("[SW2_BLE] LED command sent\n");
            break;
        }

        default:
            printf("[SW2_BLE] Unknown init state: %d\n", sw2_init_state);
            break;
    }
}

static void switch2_send_next_init_cmd(hci_con_handle_t con_handle)
{
    // ACK notification setup is complete before this begins. On a HOME
    // reconnect it also runs only after HCI encryption succeeded.
    if (sw2_init_state == SW2_INIT_IDLE) {
        printf("[SW2_BLE] Starting init sequence with READ_INFO...\n");
        sw2_init_state = SW2_INIT_READ_INFO;
        switch2_send_init_cmd(con_handle);
    } else if (sw2_init_state == SW2_INIT_DONE) {
        printf("[SW2_BLE] Init already done\n");
    } else {
        // Init in progress, wait for ACK
        printf("[SW2_BLE] Init in progress (state=%d)\n", sw2_init_state);
    }
}

// Retry init if stuck (called from main loop, ~every CONTROL_TICK_MS via btstack_host_process() —
// see the retry-timing comment above sw2_init_cmd_sent_ms for why this used to be call-count-based).
static void switch2_retry_init_if_needed(void)
{
    if (sw2_init_state == SW2_INIT_IDLE ||
        sw2_init_state == SW2_INIT_DONE || sw2_init_handle == 0) {
        return;
    }

    uint32_t now = btstack_run_loop_get_time_ms();
    // Wrap-safe: unsigned subtraction is correct across a 32-bit ms rollover (~49.7 days) as long
    // as the true elapsed time never exceeds ~24.8 days, which no single init attempt can.
    if (now - sw2_init_cmd_sent_ms < SW2_INIT_RETRY_INTERVAL_MS) {
        return;
    }

    if (sw2_init_retry_count >= SW2_INIT_MAX_RETRIES) {
        // Recovery: the controller never advanced past this state after
        // SW2_INIT_MAX_RETRIES real attempts, ~SW2_INIT_MAX_RETRIES*SW2_INIT_RETRY_INTERVAL_MS of
        // wall-clock time. Retrying forever would leave a permanently stuck, silently-undiagnosable
        // connection (no prior recovery path existed for this state machine at all — the link-layer
        // reconnect cascade this project fixed separately only engages on an actual disconnect).
        // Force one: gap_disconnect() here is a locally-initiated disconnect, which reports a
        // reason code distinct from the two "peer disconnected on purpose" codes that BLE reconnect
        // fix now excludes — so this composes correctly with that fix and triggers a fresh
        // reconnect attempt rather than requiring the user to notice and manually intervene.
        printf("[SW2_BLE] Init stuck in state=%d after %d retries (~%lums), forcing reconnect\n",
               sw2_init_state, SW2_INIT_MAX_RETRIES,
               (unsigned long)SW2_INIT_MAX_RETRIES * SW2_INIT_RETRY_INTERVAL_MS);
        hci_con_handle_t stuck_handle = sw2_init_handle;
        if (!btstack_host_request_disconnect(stuck_handle, "Switch 2 init recovery")) {
            // sw2_init_handle is not cleared by anything except
            // switch2_cleanup_on_disconnect(), which the disconnect event
            // drives. With no event coming, this recovery would re-run every
            // SW2_INIT_RETRY_INTERVAL_MS forever and never rearm -- the exact
            // permanently-stuck state it was added to escape. Converge here.
            ble_connection_t *stuck = find_connection_by_handle(stuck_handle);
            if (stuck != NULL) {
                ble_connection_release_orphan(stuck);
            } else {
                switch2_cleanup_on_disconnect(0xFFu, 0u);
            }
        }
        return;
    }

    sw2_init_retry_count++;
    printf("[SW2_BLE] Retrying init cmd (state=%d, attempt=%d/%d, %lums since last send)\n",
           sw2_init_state, sw2_init_retry_count, SW2_INIT_MAX_RETRIES,
           (unsigned long)(now - sw2_init_cmd_sent_ms));
    switch2_send_init_cmd(sw2_init_handle);
}

// ============================================================================
// SWITCH 2 RUMBLE/HAPTICS
// ============================================================================
// Switch 2 Pro Controller uses LRA (Linear Resonant Actuator) haptics.
// Output goes to ATT handle 0x0012.
// LRA ops format: 5 bytes per op (4-byte bitfield + 1-byte hf_amp)
// Each side (L/R) has 1 state byte + 3 ops = 16 bytes
// Total output: 1 + 16 + 16 + 9 padding = 42 bytes

// Rumble state tracking
static uint8_t sw2_last_rumble_left = 0;
static uint8_t sw2_last_rumble_right = 0;
static uint8_t sw2_rumble_tid = 0;
static uint32_t sw2_rumble_send_counter = 0;

// Player LED patterns (cumulative, matching joypad-web)
static const uint8_t SW2_PLAYER_LED_PATTERNS[] = {
    0x01,  // Player 1: 1 LED
    0x03,  // Player 2: 2 LEDs
    0x07,  // Player 3: 3 LEDs
    0x0F,  // Player 4: 4 LEDs
};

// Send player LED command to Switch 2 controller
static void switch2_send_player_led(hci_con_handle_t con_handle, uint8_t pattern)
{
    uint8_t led_cmd[] = {
        SW2_CMD_SET_LED,        // 0x09
        SW2_REQ_TYPE_REQ,       // 0x91
        SW2_REQ_INT_BLE,        // 0x01
        SW2_SUBCMD_SET_LED,     // 0x07
        0x00, 0x08, 0x00, 0x00,
        pattern,  // Player LED pattern
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    sw2_capture_record(SW2_CAP_CMD_OUT, SW2_CMD_HANDLE, led_cmd, sizeof(led_cmd));
    gatt_client_write_value_of_characteristic_without_response(
        con_handle, SW2_CMD_HANDLE, sizeof(led_cmd), led_cmd);
}

// Encode haptic data for one motor (5 bytes)
// Based on joypad-web's encodeSwitch2Haptic() function
// Format: [amplitude, frequency, amplitude, frequency, flags]
// Key: Lower frequency = more felt, higher frequency = audible tones
// freq 0x60 = felt rumble, freq 0xFE = audible (avoid this)
static void encode_haptic(uint8_t* out, uint8_t intensity)
{
    if (intensity == 0) {
        // Off state
        out[0] = 0x00;
        out[1] = 0x00;
        out[2] = 0x00;
        out[3] = 0x00;
        out[4] = 0x00;
    } else {
        // Active rumble - use low frequency for felt vibration
        // Amplitude: scale from 0x40 to 0xFF based on intensity
        uint8_t amp = 0x40 + ((intensity * 0xBF) / 255);
        // Frequency: use 0x40-0x60 range for low rumble (more felt, less audible)
        // Lower values = lower frequency = more physical sensation
        uint8_t freq = 0x40;  // Low frequency for maximum felt rumble
        out[0] = amp;   // High band amplitude
        out[1] = freq;  // High band frequency (low value = felt)
        out[2] = amp;   // Low band amplitude
        out[3] = freq;  // Low band frequency
        out[4] = 0x00;  // Flags
    }
}

// Send rumble command to Switch 2 controller via BLE
// Pro uses HD haptics, GameCube uses simple on/off
static void switch2_send_rumble(hci_con_handle_t con_handle, uint8_t left, uint8_t right)
{
    // Get connection to check PID
    ble_connection_t* conn = find_connection_by_handle(con_handle);

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    // Counter with state bits
    uint8_t counter = 0x50 | (sw2_rumble_tid & 0x0F);
    sw2_rumble_tid++;

    if (conn && conn->pid == 0x2073) {
        // GameCube controller: simple on/off rumble
        // Format: byte 1 = counter, byte 2 = on/off state
        buf[1] = counter;
        buf[2] = (left || right) ? 0x01 : 0x00;

        gatt_client_write_value_of_characteristic_without_response(
            con_handle, SW2_OUTPUT_REPORT_HANDLE, 21, buf);
    } else {
        // Pro controller: HD haptics format
        // [1]: Counter (0x5X)
        // [2-6]: Left haptic (5 bytes)
        // [17]: Counter duplicate
        // [18-22]: Right haptic (5 bytes)
        buf[1] = counter;
        buf[17] = counter;  // Duplicate counter

        // Encode left motor haptic (bytes 2-6)
        encode_haptic(&buf[2], left);

        // Encode right motor haptic (bytes 18-22)
        encode_haptic(&buf[18], right);

        gatt_client_write_value_of_characteristic_without_response(
            con_handle, SW2_OUTPUT_REPORT_HANDLE, sizeof(buf), buf);
    }
}

// Check feedback system and send rumble/LED if needed (called from task loop)
static void switch2_handle_feedback(void)
{
    // Only process if we have an active Switch 2 connection
    if (sw2_init_state != SW2_INIT_DONE || sw2_init_handle == 0) return;

    // One-shot GATT discovery and v2 feature-enable experiment (see sw2_capture.h): each fires at
    // most once per connection, after normal init has completed unchanged, only when explicitly
    // armed via `sw2cap gattdisc on` / `sw2cap variant <n>`. Neither affects anything below this
    // point. Do not arm both in the same session (see sw2_capture.h) -- if a user does, discovery
    // is checked first and gets first use of the connection's single outstanding GATT query.
    if (s_sw2_gatt_disc_enabled && !s_sw2_gatt_disc_fired) {
        s_sw2_gatt_disc_fired = true;
        switch2_run_gatt_discovery(sw2_init_handle);
    }
    if (s_sw2_v2_armed_variant != 0 && !s_sw2_v2_fired) {
        s_sw2_v2_fired = true;
        switch2_run_v2_experiment(sw2_init_handle);
    }

#ifdef NS2_PRO
    // Production native-motion path, promoted only after hardware validation of variant 9:
    // genuine Pro Controller 2 -> Pro2 USB personality, console-captured feature sequence,
    // verified 7.5ms/133Hz BLE link, then byte-exact opaque PDU pass-through. Explicit RE
    // variants and GATT discovery retain priority and suppress this automatic path so diagnostic
    // sessions remain isolated and attributable.
    if (!s_sw2_native_auto_fired) {
        ble_connection_t *motion_source = find_connection_by_handle(sw2_init_handle);
        uint32_t elapsed = btstack_run_loop_get_time_ms() - s_sw2_init_done_ms;
        uint8_t blocked = 0;
        if (elapsed < 250u) blocked |= 0x01;
        if (s_sw2_v2_armed_variant != 0) blocked |= 0x02;
        if (s_sw2_gatt_disc_enabled) blocked |= 0x04;
        if (g_usb_personality != USB_PERSONALITY_SWITCH2_PRO2) blocked |= 0x08;
        if (!motion_source) blocked |= 0x10;
        // conn->pid comes from the validated Nintendo manufacturer advertisement.
        // Keep this strict: native opaque motion is Pro Controller 2-specific,
        // but never feed conn->pid from speculative SPI/command offsets.
        if (motion_source && motion_source->pid != 0x2069) blocked |= 0x20;
        if (s_sw2_v2_active != NULL ||
            (s_sw2_v2_state != SW2_V2_IDLE && s_sw2_v2_state != SW2_V2_DONE)) blocked |= 0x40;

        __atomic_add_fetch(&s_sw2_native_auto_checks, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&s_sw2_native_auto_wait_elapsed_ms, elapsed, __ATOMIC_RELEASE);
        __atomic_store_n(&s_sw2_native_auto_source_pid,
                         motion_source ? motion_source->pid : 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&s_sw2_native_auto_personality, (uint8_t)g_usb_personality,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_sw2_native_auto_block_mask, blocked, __ATOMIC_RELEASE);

        if (blocked == 0) {
            s_sw2_native_auto_fired = true;
            __atomic_add_fetch(&s_sw2_native_auto_starts, 1u, __ATOMIC_RELEASE);
            switch2_start_v2_variant(sw2_init_handle, &SW2_NATIVE_PRO2_PROFILE, true);
        }
    }
#endif

    // Off-by-default UART experiment. It runs only after the ordinary init/
    // production native-motion state machine is idle and always has an
    // explicit restore path back to the validated 0x27 profile.
    switch2_service_magraw_probe();
    switch2_service_imuref_probe();

    switch2_service_pro2_audio_capture();
    switch2_service_nfc_mirror();

    sw2_rumble_send_counter++;

    // Get conn_index from HCI handle
    int conn_index = get_ble_conn_index_by_handle(sw2_init_handle);
    if (conn_index < 0) return;

    // Find player index for this device
    int player_idx = find_player_index(conn_index, 0);
    if (player_idx < 0) return;

    // Get feedback state
    feedback_state_t* fb = feedback_get_state(player_idx);
    if (!fb) return;

    // --- Handle Player LED ---
    if (fb->led_dirty) {
        // Determine LED pattern from feedback
        uint8_t led_pattern = 0x01;  // Default to player 1

        if (fb->led.pattern != 0) {
            // Use pattern bits directly (0x01=P1, 0x02=P2, 0x04=P3, 0x08=P4)
            // Convert to cumulative pattern for Switch 2
            if (fb->led.pattern & 0x08) led_pattern = SW2_PLAYER_LED_PATTERNS[3];
            else if (fb->led.pattern & 0x04) led_pattern = SW2_PLAYER_LED_PATTERNS[2];
            else if (fb->led.pattern & 0x02) led_pattern = SW2_PLAYER_LED_PATTERNS[1];
            else led_pattern = SW2_PLAYER_LED_PATTERNS[0];
        } else {
            // Use player index if no explicit pattern
            int idx = (player_idx >= 0 && player_idx < 4) ? player_idx : 0;
            led_pattern = SW2_PLAYER_LED_PATTERNS[idx];
        }

        if (led_pattern != sw2_last_player_led) {
            sw2_last_player_led = led_pattern;
            switch2_send_player_led(sw2_init_handle, led_pattern);
        }
    }

    // --- Handle Rumble ---
    bool value_changed = (fb->rumble.left != sw2_last_rumble_left ||
                          fb->rumble.right != sw2_last_rumble_right);

    // Send rumble if:
    // 1. Values changed, OR
    // 2. Rumble is active and we need periodic refresh (every ~50ms at 120Hz = 6 ticks)
    bool need_refresh = (sw2_last_rumble_left > 0 || sw2_last_rumble_right > 0) &&
                        (sw2_rumble_send_counter % 6 == 0);

    if (fb->rumble_dirty || value_changed || need_refresh) {
        sw2_last_rumble_left = fb->rumble.left;
        sw2_last_rumble_right = fb->rumble.right;

        switch2_send_rumble(sw2_init_handle, fb->rumble.left, fb->rumble.right);
    }

    // Clear dirty flags after processing
    if (fb->rumble_dirty || fb->led_dirty) {
        feedback_clear_dirty(player_idx);
    }
}

// Register Switch 2 notification listener and enable notifications
static void register_switch2_hid_listener(hci_con_handle_t con_handle)
{
    printf("[SW2_BLE] Registering Switch 2 HID listener for handle 0x%04X\n", con_handle);

    // Find the BLE connection
    ble_connection_t* conn = find_connection_by_handle(con_handle);
    if (!conn) {
        printf("[SW2_BLE] ERROR: No connection for handle 0x%04X\n", con_handle);
        return;
    }

    // Assign conn_index if not already set
    int ble_index = -1;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (&hid_state.connections[i] == conn) {
            ble_index = i;
            break;
        }
    }
    if (ble_index < 0) return;

    conn->conn_index = BLE_CONN_INDEX_OFFSET + ble_index;
    conn->hid_ready = true;
    sw2_init_handle = con_handle;
    sw2_init_state = SW2_INIT_IDLE;
    sw2_pairing_ltk_valid = false;
    sw2_pairing_ltk_handle = HCI_CON_HANDLE_INVALID;
    sw2_pairing_ltk_phase = 0;

    printf("[SW2_BLE] Connection: VID=0x%04X PID=0x%04X conn_index=%d\n",
           conn->vid, conn->pid, conn->conn_index);

    // Set up ACK notification listener (handle 0x001A)
    memset(&switch2_ack_characteristic, 0, sizeof(switch2_ack_characteristic));
    switch2_ack_characteristic.value_handle = 0x001A;
    switch2_ack_characteristic.end_handle = 0x001A + 1;

    gatt_client_listen_for_characteristic_value_updates(
        &switch2_ack_notification_listener,
        switch2_ack_notification_handler,
        con_handle,
        &switch2_ack_characteristic);

    // Set up input report notification listener (handle 0x000A)
    memset(&switch2_hid_characteristic, 0, sizeof(switch2_hid_characteristic));
    switch2_hid_characteristic.value_handle = SW2_INPUT_REPORT_HANDLE;
    switch2_hid_characteristic.end_handle = SW2_INPUT_REPORT_HANDLE + 1;

    gatt_client_listen_for_characteristic_value_updates(
        &switch2_hid_notification_listener,
        switch2_hid_notification_handler,
        con_handle,
        &switch2_hid_characteristic);

    printf("[SW2_BLE] Notification listeners registered\n");

    // Enable notifications on ACK handle first (0x001B) - wait for confirmation
    static uint8_t ccc_enable[] = { 0x01, 0x00 };
    printf("[SW2_BLE] Enabling ACK notifications on CCC handle 0x%04X\n", SW2_ACK_CCC_HANDLE);
    sw2_capture_record(SW2_CAP_CCC_WRITE, SW2_ACK_CCC_HANDLE, ccc_enable, sizeof(ccc_enable));
    gatt_client_write_value_of_characteristic(
        switch2_ack_ccc_write_callback, con_handle, SW2_ACK_CCC_HANDLE, sizeof(ccc_enable), ccc_enable);
}

static void start_hids_host(ble_connection_t *conn)
{
    printf("[BTSTACK_HOST] Connecting HIDS Host...\n");

    conn->state = BLE_STATE_DISCOVERING;
    hid_state.gatt_handle = conn->handle;

    uint8_t status = hids_host_connect(conn->handle, hids_host_handler,
                                         HID_PROTOCOL_MODE_REPORT, &conn->hids_cid);

    printf("[BTSTACK_HOST] hids_host_connect returned %d, cid=0x%04X\n",
           status, conn->hids_cid);
}

// ============================================================================
// BAS (BATTERY SERVICE) CLIENT HANDLER
// ============================================================================

static void bas_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_BATTERY_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_battery_service_connected_get_status(packet);
            uint8_t num_instances = gattservice_subevent_battery_service_connected_get_num_instances(packet);
            printf("[BTSTACK_HOST] BAS connected: status=%d instances=%d\n", status, num_instances);
            break;
        }

        case GATTSERVICE_SUBEVENT_BATTERY_SERVICE_LEVEL: {
            uint8_t att_status = gattservice_subevent_battery_service_level_get_att_status(packet);
            uint8_t level = gattservice_subevent_battery_service_level_get_level(packet);

            if (att_status != ATT_ERROR_SUCCESS) break;

            // Find conn_index for the current BLE connection
            int conn_index = get_ble_conn_index_by_handle(hid_state.gatt_handle);
            if (conn_index >= 0) {
                bthid_set_battery_level((uint8_t)conn_index, level);
            }
            // Cache for the MouthPad relay's connection-status response.
            if (hid_state.gatt_handle == mp_nus.handle) {
                mp_nus.last_battery = level;
            }
            break;
        }

        default:
            break;
    }
}

static void start_battery_service_client(hci_con_handle_t handle)
{
    uint8_t status = battery_service_client_connect(handle, bas_client_handler, 60000, &hid_state.bas_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[BTSTACK_HOST] BAS connect failed: status=%d\n", status);
    } else {
        printf("[BTSTACK_HOST] BAS connect started: cid=0x%04X\n", hid_state.bas_cid);
    }
}

// ============================================================================
// DIS (DEVICE INFORMATION SERVICE) CLIENT HANDLER
// ============================================================================

static void dis_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_PNP_ID: {
            hci_con_handle_t handle = gattservice_subevent_device_information_pnp_id_get_con_handle(packet);
            uint16_t vid = gattservice_subevent_device_information_pnp_id_get_vendor_id(packet);
            uint16_t pid = gattservice_subevent_device_information_pnp_id_get_product_id(packet);
            uint8_t vendor_source = gattservice_subevent_device_information_pnp_id_get_vendor_source_id(packet);

            printf("[BTSTACK_HOST] DIS PnP ID: vendor_source=%d VID=0x%04X PID=0x%04X handle=0x%04X\n",
                   vendor_source, vid, pid, handle);

            ble_connection_t *conn = find_connection_by_handle(handle);
            if (conn && (vid || pid)) {
                bool changed = conn->vid != vid || conn->pid != pid;
                conn->vid = vid;
                conn->pid = pid;

                // Always deliver a valid DIS result to the consuming layer,
                // even when advertisement data happened to populate the same
                // IDs first. This makes the late authoritative handoff
                // idempotent and prevents connection-cache timing from
                // suppressing driver/quirk re-evaluation. HID binding and
                // report notifications remain earlier in the sequence.
                printf("[BTSTACK_HOST] DIS: %s device info for conn_index=%d\n",
                       changed ? "updating" : "confirming", conn->conn_index);
                bthid_update_device_info(conn->conn_index, conn->name, vid, pid);
            }
            // Recognize the MouthPad by its Augmental DIS PnP ID (0x1915:0xEEEE)
            // and arm the NUS relay — names can be reset to dev values that lack
            // "MouthPad" (the name gate at the 0x1C handler then misses it, leaving
            // the relay stuck "scanning"). mp_nus_mark_pending is a no-op if already
            // armed by the name gate.
            if (vid == 0x1915 && pid == 0xEEEE) {
                mp_nus_mark_pending(handle);
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_FIRMWARE_REVISION: {
            hci_con_handle_t handle = gattservice_subevent_device_information_firmware_revision_get_con_handle(packet);
            // Store for the active DIS connection, NOT gated on mp_nus being armed:
            // PnP ID (which arms mp_nus by VID/PID) comes LAST in DIS, after the
            // firmware-revision read — gating on mp_nus.handle here would miss it.
            if (gattservice_subevent_device_information_firmware_revision_get_att_status(packet) == ATT_ERROR_SUCCESS
                && handle == hid_state.gatt_handle) {
                const char* fw = gattservice_subevent_device_information_firmware_revision_get_value(packet);
                if (fw) {
                    strncpy(mp_nus.firmware, fw, sizeof(mp_nus.firmware) - 1);
                    mp_nus.firmware[sizeof(mp_nus.firmware) - 1] = '\0';
                    printf("[BTSTACK_HOST] DIS firmware revision: %s\n", mp_nus.firmware);
                }
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_DONE: {
            hci_con_handle_t handle = gattservice_subevent_device_information_done_get_con_handle(packet);
            uint8_t att_status = gattservice_subevent_device_information_done_get_att_status(packet);
            printf("[BTSTACK_HOST] DIS query done: handle=0x%04X status=0x%02X\n", handle, att_status);
            // Start Battery Service client after DIS completes (avoids GATT procedure contention)
            start_battery_service_client(handle);
            break;
        }

        default:
            break;
    }
}

static void hids_host_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(packet_type);  // hids_host passes HCI_EVENT_GATTSERVICE_META, not HCI_EVENT_PACKET
    UNUSED(channel);
    UNUSED(size);

    // Check the event type in the packet itself
    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            uint8_t num_instances = gattservice_subevent_hid_service_connected_get_num_instances(packet);
            printf("[BTSTACK_HOST] HIDS connected! status=%d instances=%d\n", status, num_instances);

            if (status == ERROR_CODE_SUCCESS) {
                // Route by the event's own cid so two BLE HID devices stay separate
                // (a shared global handle/cid cross-wired their descriptors/reports).
                uint16_t cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
                ble_connection_t *conn = find_connection_by_hids_cid(cid);
                if (!conn) conn = find_connection_by_handle(hid_state.gatt_handle);  // fallback
                if (conn) {
                    conn->state = BLE_STATE_READY;
                    conn->hid_ready = true;
                    conn->hids_cid = cid;

                    // Assign conn_index if not already set
                    int slot = -1;
                    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
                        if (&hid_state.connections[i] == conn) {
                            conn->conn_index = BLE_CONN_INDEX_OFFSET + i;
                            slot = i;
                            break;
                        }
                    }

                    // Notify bthid layer that device is ready
                    btstack_host_stop_scan();
                    scan_timeout_end = 0;
                    printf("[BTSTACK_HOST] Calling bt_on_hid_ready(%d) for BLE device '%s'\n",
                           conn->conn_index, conn->name);
                    bt_on_hid_ready(conn->conn_index);

                    // Pass THIS device's HID descriptor to bthid (per-connection cid)
                    const uint8_t* hid_desc = hids_host_descriptor_storage_get_descriptor_data(conn->hids_cid, 0);
                    uint16_t hid_desc_len = hids_host_descriptor_storage_get_descriptor_len(conn->hids_cid, 0);
                    if (hid_desc && hid_desc_len > 0) {
                        printf("[BTSTACK_HOST] BLE HID descriptor: %d bytes\n", hid_desc_len);
                        // Raw hex dump for the 2026-07-12 BLE-rumble investigation: we've
                        // never actually inspected a real Xbox Series BLE report map in
                        // this repo. xbox_ble.c's rumble output assumes report ID 0x03
                        // (copied from the documented Classic-BT format) has an Output
                        // usage in THIS descriptor too — that's unverified. This dump
                        // lets a hex decode confirm whether an Output report exists at
                        // all, and at which report ID, instead of guessing again.
                        printf("[BTSTACK_HOST] BLE HID descriptor bytes:");
                        for (uint16_t i = 0; i < hid_desc_len; i++) {
                            if ((i % 16) == 0) printf("\n  ");
                            printf("%02X ", hid_desc[i]);
                        }
                        printf("\n");
                        bthid_set_hid_descriptor(conn->conn_index, hid_desc, hid_desc_len);
                    }

                    // NOTE: DIS (PnP VID/PID) and BAS (battery) are intentionally
                    // NOT started here. Each is a gatt_client query, and running
                    // them concurrently with hids_host_enable_notifications()
                    // starves the HID notification enabling on devices with many
                    // report characteristics (e.g. Augmental MouthPad, 4+ reports):
                    // the CCC writes never complete, no reports flow, and the
                    // device drops the link. They are deferred to the
                    // REPORTS_NOTIFICATION (0x1C) handler below, so only one GATT
                    // client uses the connection at a time.

                    // Switch to REPORT protocol mode BEFORE enabling notifications
                    // (mirrors the working mouthpad-usb order). The MouthPad boots
                    // in BOOT mode; switching mode AFTER subscribing makes it drop
                    // the report CCCs, so it never streams. The sequencer writes
                    // the mode (once hids_host is back in CONNECTED), THEN
                    // enables notifications, then the 0x1C handler starts DIS/BAS/NUS.
                    if (slot >= 0) {
                        mp_hid_setup[slot].active   = true;
                        mp_hid_setup[slot].handle   = conn->handle;
                        mp_hid_setup[slot].hids_cid = conn->hids_cid;
                        mp_hid_setup[slot].phase    = 0;
                        mp_hid_setup[slot].start_ms = btstack_run_loop_get_time_ms();
                    }
                }
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_SERVICE_REPORTS_NOTIFICATION: {
            uint8_t configuration = gattservice_subevent_hid_service_reports_notification_get_configuration(packet);
            printf("[BTSTACK_HOST] HID Reports Notification configured: %d\n", configuration);
            printf("[BTSTACK_HOST] Ready to receive HID reports!\n");

            // Mode is already REPORT and notifications are now enabled. Start the
            // remaining GATT clients one at a time: DIS -> BAS, then arm NUS.
            // Resolve THIS device by the event's cid (not the global handle).
            {
                uint16_t cid = gattservice_subevent_hid_service_reports_notification_get_hids_cid(packet);
                ble_connection_t* nconn = find_connection_by_hids_cid(cid);
                hci_con_handle_t nhandle = nconn ? nconn->handle : hid_state.gatt_handle;

                uint8_t dis = device_information_service_client_query(
                    nhandle, dis_client_handler);
                if (dis != ERROR_CODE_SUCCESS) {
                    start_battery_service_client(nhandle);
                }
                // Recognize the MouthPad by the live conn name OR the stored
                // last-connected name (a reconnect can leave conn->name empty,
                // which previously left the NUS relay stuck unarmed -> the app
                // shows "scanning" despite a paired MouthPad).
                if (nconn && (strstr(nconn->name, "MouthPad") != NULL ||
                              strstr(hid_state.last_connected_name, "MouthPad") != NULL)) {
                    mp_nus_mark_pending(nhandle);
                }
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_PROTOCOL_MODE: {
            uint8_t pm = gattservice_subevent_hid_protocol_mode_get_protocol_mode(packet);
            printf("[MP] device protocol mode now = %s (%d)\n",
                   pm == 0 ? "BOOT" : "REPORT", pm);
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            uint16_t report_len = gattservice_subevent_hid_report_get_report_len(packet);
            const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);

            // Route by the report's OWN cid -> the owning connection. Using the
            // global gatt_handle here sent BOTH devices' reports to the last one
            // connected (the haywire merge). This is the core 2-BLE-HID fix.
            uint16_t cid = gattservice_subevent_hid_report_get_hids_cid(packet);
            ble_connection_t* rconn = find_connection_by_hids_cid(cid);
            int conn_index = rconn ? rconn->conn_index : get_ble_conn_index_by_handle(hid_state.gatt_handle);
            if (conn_index >= 0) {
                route_ble_hid_report(
                    (uint8_t)conn_index,
                    bthid_get_connection_generation((uint8_t)conn_index),
                    report, report_len);
            }

            // Forward to callback if set
            if (hid_state.report_callback) {
                hid_state.report_callback(hid_state.gatt_handle, report, report_len);
            }
            break;
        }

        default:
            printf("[BTSTACK_HOST] GATT service subevent: 0x%02X\n",
                   hci_event_gattservice_meta_get_subevent_code(packet));
            break;
    }
}

// ============================================================================
// HELPERS
// ============================================================================

static ble_connection_t* find_connection_by_handle(hci_con_handle_t handle)
{
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle == handle) {
            return &hid_state.connections[i];
        }
    }
    return NULL;
}

// Find the connection that owns a given BLE HID client id. Used to route
// hids_host events (connect/notification/report) to the right device when
// more than one BLE HID device is connected.
static ble_connection_t* find_connection_by_hids_cid(uint16_t hids_cid)
{
    if (hids_cid == 0) return NULL;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].hids_cid == hids_cid) {
            return &hid_state.connections[i];
        }
    }
    return NULL;
}

static ble_connection_t* find_free_connection(void)
{
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle == HCI_CON_HANDLE_INVALID) {
            return &hid_state.connections[i];
        }
    }
    return NULL;
}

// ============================================================================
// STATUS
// ============================================================================

bool btstack_host_is_initialized(void)
{
    return hid_state.initialized;
}

bool btstack_host_is_powered_on(void)
{
    return hid_state.powered_on;
}

bool btstack_host_is_scanning(void)
{
    return hid_state.scan_active || classic_state.inquiry_active;
}

void btstack_host_get_reconnect_diag(btstack_host_reconnect_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->powered_on = hid_state.powered_on;
    out->scan_active = hid_state.scan_active;
    out->has_last_connected = hid_state.has_last_connected;
    out->has_last_connected_ltk = hid_state.has_last_connected_ltk;
    out->force_fresh_custom_pairing = switch2_force_fresh_custom_pairing;
    out->connect_attempt_active = hid_state.reconnect_attempt_time != 0;
    out->state = (uint8_t)hid_state.state;
    out->reconnect_attempts = hid_state.reconnect_attempts;
    out->last_connected_addr_type = (uint8_t)hid_state.last_connected_addr_type;
    out->last_connected_ble_strategy = hid_state.last_connected_profile ?
        (uint8_t)hid_state.last_connected_profile->ble : (uint8_t)BT_BLE_NONE;
    out->last_connected_vid = hid_state.last_connected_vid;
    out->last_connected_pid = hid_state.last_connected_pid;
    out->advertising_reports = hid_state.advertising_reports;
    out->target_advertising_reports = hid_state.target_advertising_reports;
    out->switch2_advertising_reports = hid_state.switch2_advertising_reports;
    out->bonded_advertising_reports = bonded_adv_reports;
    out->nontarget_advertising_reports = nontarget_adv_reports;
    out->rpa_advertising_reports = rpa_adv_reports;
    // Snapshot, not a live DB read: this getter is called from core 0.
    out->bond_count = bond_snapshot_count;
    out->bond_capacity = (uint8_t)MAX_NR_LE_DEVICE_DB_ENTRIES;
    out->target_connect_attempts = hid_state.target_connect_attempts;
    out->target_connect_successes = hid_state.target_connect_successes;
    out->target_connect_failures = hid_state.target_connect_failures;
    out->reencryption_started = hid_state.reencryption_started;
    out->reencryption_successes = hid_state.reencryption_successes;
    out->reencryption_failures = hid_state.reencryption_failures;
    out->pairing_ltk_reads = sw2_pairing_ltk_reads;
    out->direct_reencrypt_active = switch2_direct_reencrypt_active;
    out->direct_reencrypt_handle = switch2_direct_reencrypt_handle;
    out->direct_reencrypt_elapsed_ms = switch2_direct_reencrypt_active ?
        btstack_run_loop_get_time_ms() - switch2_direct_reencrypt_started_ms : 0;
    out->hci_command_ready = hci_can_send_command_packet_now();
    out->direct_link_key_size = switch2_direct_reencrypt_handle != HCI_CON_HANDLE_INVALID ?
        gap_encryption_key_size(switch2_direct_reencrypt_handle) : 0;
    out->direct_encrypt_phase = switch2_direct_encrypt_phase;
    out->direct_cmd_status_events = switch2_direct_cmd_status_events;
    out->direct_cmd_complete_events = switch2_direct_cmd_complete_events;
    out->direct_encrypt_events = switch2_direct_encrypt_events;
    out->switch2_disconnect_events = switch2_disconnect_events;
    out->last_direct_cmd_status_opcode = switch2_last_cmd_status_opcode;
    out->last_direct_cmd_complete_opcode = switch2_last_cmd_complete_opcode;
    out->last_direct_cmd_status = switch2_last_cmd_status;
    out->last_direct_cmd_complete_status = switch2_last_cmd_complete_status;
    out->last_direct_encrypt_status = switch2_last_encrypt_status;
    out->last_direct_encrypt_enabled = switch2_last_encrypt_enabled;
    out->last_switch2_disconnect_reason = switch2_last_disconnect_reason;
    out->last_target_advertising_event_type = hid_state.last_target_advertising_event_type;
    out->last_target_connect_status = hid_state.last_target_connect_status;
    out->last_reencryption_status = hid_state.last_reencryption_status;
    out->pairing_ltk_phase = sw2_pairing_ltk_phase;
    out->pairing_ltk_valid = sw2_pairing_ltk_valid;
    out->pairing_ltk_matches_derived = sw2_pairing_ltk_matches_derived;
    out->pairing_ltk_raw_matches_derived = sw2_pairing_ltk_raw_matches_derived;
    gap_local_bd_addr(out->local_addr);
    memcpy(out->last_connected_addr, hid_state.last_connected_addr,
           sizeof(out->last_connected_addr));
    strncpy(out->last_connected_name, hid_state.last_connected_name,
            sizeof(out->last_connected_name) - 1);
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID) {
            out->connected_ble_count++;
        }
    }
}

// --- In-band management / BLE coexistence diagnostics (UART) ----------------

const char *btstack_host_life_code_name(uint8_t code)
{
    switch (code) {
        case BTLIFE_SCAN_START:      return "scan_start";
        case BTLIFE_SCAN_STOP:       return "scan_stop";
        case BTLIFE_SCAN_SUPPRESS:   return "scan_suppress";
        case BTLIFE_ADV_START:       return "adv_start";
        case BTLIFE_ADV_STOP:        return "adv_stop";
        case BTLIFE_MGMT_CONNECT:    return "mgmt_connect";
        case BTLIFE_MGMT_DISCONNECT: return "mgmt_disconnect";
        case BTLIFE_CTRL_DISCONNECT: return "ctrl_disconnect";
        case BTLIFE_HCI_DISCONNECT:  return "hci_disconnect";
        case BTLIFE_PAGE_SCAN_ON:    return "page_scan_on";
        case BTLIFE_PAGE_SCAN_OFF:   return "page_scan_off";
        case BTLIFE_INQUIRY_START:   return "inquiry_start";
        case BTLIFE_INQUIRY_STOP:    return "inquiry_stop";
        case BTLIFE_PAGE_RX:         return "page_rx";
        case BTLIFE_PAGE_ACCEPT:     return "page_accept";
        case BTLIFE_PAGE_REJECT:     return "page_reject";
        case BTLIFE_ACL_UP:          return "acl_up";
        case BTLIFE_ACL_FAIL:        return "acl_fail";
        case BTLIFE_LINK_KEY_REQ:    return "link_key_req";
        case BTLIFE_AUTH_DEFER:      return "auth_defer";
        case BTLIFE_AUTH_START:      return "auth_start";
        case BTLIFE_AUTH_DONE:       return "auth_done";
        case BTLIFE_ENC_CHANGE:      return "enc_change";
        case BTLIFE_HID_READY:       return "hid_ready";
        case BTLIFE_HID_FAIL:        return "hid_fail";
        default:                     return "none";
    }
}

void btstack_host_life_flag_names(uint8_t flags, char out[9])
{
    static const char letters[8] = { 'i', 's', 'c', 'm', 't', 'C', 'w', 'p' };
    for (int bit = 0; bit < 8; bit++) {
        out[bit] = (flags & (1u << bit)) ? letters[bit] : '-';
    }
    out[8] = '\0';
}

const char *btstack_host_life_cause_name(uint8_t cause)
{
    switch (cause) {
        case BTLIFE_CAUSE_CONFIG_MODE:  return "config_mode";
        case BTLIFE_CAUSE_MGMT_ARMED:   return "mgmt_armed";
        case BTLIFE_CAUSE_WAKE:         return "wake";
        case BTLIFE_CAUSE_LOCKOUT:      return "lockout";
        case BTLIFE_CAUSE_APP_SUPPRESS: return "app_suppress";
        case BTLIFE_CAUSE_NOT_POWERED:  return "not_powered";
        case BTLIFE_CAUSE_ALREADY:      return "already";
        default:                        return "none";
    }
}

bool btstack_host_life_get(uint16_t index, btstack_host_life_record_t *out)
{
    if (!out || index >= btlife_count) return false;
    // Oldest-first logical order. Reads statics the BTstack thread may be
    // writing; a benign torn read at most mis-renders a single diagnostic line.
    uint16_t oldest = (uint16_t)((btlife_head + BTLIFE_RING_SIZE - btlife_count)
                                 % BTLIFE_RING_SIZE);
    const btlife_event_t *e = &btlife_ring[(oldest + index) % BTLIFE_RING_SIZE];
    out->t_ms = e->t_ms;
    out->code = e->code;
    out->a = e->a;
    out->b = e->b;
    out->flags = e->flags;
    out->addr3[0] = e->addr3[0];
    out->addr3[1] = e->addr3[1];
    out->addr3[2] = e->addr3[2];
    return true;
}

void btstack_host_life_clear(void)
{
    btlife_head = 0;
    btlife_count = 0;
    btlife_dropped = 0;
    memset(&btlife, 0, sizeof(btlife));
}

void btstack_host_get_mgmt_diag(btstack_host_mgmt_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    // Feature / personality
    out->mgmt_enabled = g_mgmt_enabled;
    out->config_mode = g_usb_config_mode;
    out->personality = (uint8_t)g_usb_personality;
    // Radio / host state
    out->powered_on = hid_state.powered_on;
    out->hid_state = (uint8_t)hid_state.state;
    out->scan_active = hid_state.scan_active;
    out->inquiry_active = classic_state.inquiry_active;
    out->wake_adv_active = wake_adv.active;
    out->controller_connected = btstack_host_controller_connected();
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (classic_state.connections[i].active)
            out->connected_classic_count++;
        if (classic_state.connections[i].active &&
            classic_state.connections[i].hid_ready)
            out->ready_classic_count++;
    }
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID)
            out->connected_ble_count++;
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID &&
            hid_state.connections[i].hid_ready)
            out->ready_ble_count++;
    }
    out->pairing_window_open = hid_pairing_window_open;
    out->pairing_close_deferred = pairing_close_deferred;
    out->pairing_lockout = pairing_lockout;
    // config/management BLE service
    out->cble_service_available = config_ble.service_available;
    out->cble_mode_active = config_ble.mode_active;
    out->cble_advertising = config_ble.advertising;
    out->cble_has_client = config_ble.handle != HCI_CON_HANDLE_INVALID;
    out->cble_closing = config_ble.closing;
    out->cble_notifications = config_ble.notifications_enabled;
    out->cble_fresh_bond_admitted = config_ble.fresh_bond_admitted;
    // Counters (ring-independent totals)
    out->event_count = btlife_count;
    out->event_dropped = btlife_dropped;
    out->scan_starts = btlife.scan_start;
    out->scan_stops = btlife.scan_stop;
    out->adv_starts = btlife.adv_start;
    out->adv_stops = btlife.adv_stop;
    out->suppress_config_mode = btlife.suppress[BTLIFE_CAUSE_CONFIG_MODE];
    out->suppress_mgmt_armed = btlife.suppress[BTLIFE_CAUSE_MGMT_ARMED];
    out->suppress_wake = btlife.suppress[BTLIFE_CAUSE_WAKE];
    out->suppress_other = btlife.suppress[BTLIFE_CAUSE_LOCKOUT] +
                          btlife.suppress[BTLIFE_CAUSE_APP_SUPPRESS] +
                          btlife.suppress[BTLIFE_CAUSE_NOT_POWERED] +
                          btlife.suppress[BTLIFE_CAUSE_ALREADY];
    out->mgmt_connects = btlife.mgmt_connect;
    out->mgmt_disconnects = btlife.mgmt_disconnect;
    out->ctrl_disconnects = btlife.ctrl_disconnect;
    out->hci_disconnects = btlife.hci_disconnect;
    out->hci_state_losses = hci_state_losses;
    out->hci_state = (uint8_t)hci_get_state();
    out->hci_health_phase = (uint8_t)bt_health.phase;
    out->hci_probe_handle = bt_health_probe_handle;
    out->hci_last_event_age_ms =
        btstack_run_loop_get_time_ms() - bt_health.last_hci_event_ms;
    out->hci_probes_sent = bt_health.probes_sent;
    out->hci_probes_ok = bt_health.probes_ok;
    out->hci_probes_failed = bt_health.probes_failed;
    out->hci_probe_timeouts = bt_health.probe_timeouts;
    out->hci_recovery_attempts = bt_health.recovery_attempts;
    out->hci_recovery_completions = bt_health.recovery_completions;
    out->fresh_admission_accepts = fresh_admission_accepts;
    out->classic_encryption_deferrals = classic_encryption_deferrals;
    out->classic_encryption_peer_completed = classic_encryption_peer_completed;
    out->classic_encryption_collisions = classic_encryption_collisions;
    out->classic_encryption_unencrypted_active = classic_encryption_unencrypted_active;
    out->classic_authentication_deferrals = classic_authentication_deferrals;
    out->classic_authentication_collisions = classic_authentication_collisions;
    out->classic_companion_handle = classic_companion_acl_handle;
    out->classic_companion_refused_no_mgmt = classic_companion_refused_no_mgmt;
    out->classic_companion_mgmt_teardowns = classic_companion_mgmt_teardowns;
    out->fresh_admission_reject_window = fresh_admission_reject_window;
    out->fresh_admission_reject_lockout = fresh_admission_reject_lockout;
    out->wipe_completions = wipe_completions;
    out->disconnect_handle_already_gone = disconnect_handle_already_gone;
    out->last_disc_handle = btlife.last_disc_handle;
    out->last_disc_reason = btlife.last_disc_reason;
    ns2_owner_led_diag_t led_diag;
    ns2_owner_led_diag_snapshot(&led_diag);
    out->owner_led_reason = led_diag.reason;
    out->owner_led_output_on = led_diag.output_on;
    out->owner_led_last_transition_ms = led_diag.last_transition_ms;
    out->owner_led_timer_max_gap_ms = led_diag.timer_max_gap_ms;
}

// ============================================================================
// CLASSIC BT HID HOST PACKET HANDLER
// ============================================================================

static bool btstack_report_debug_done = false;

static void hid_host_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    if (event_type != HCI_EVENT_HID_META) return;

    uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);

    switch (subevent) {
        case HID_SUBEVENT_INCOMING_CONNECTION: {
            // Accept incoming HID connections from devices
            uint16_t hid_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
            bd_addr_t incoming_addr;
            hid_subevent_incoming_connection_get_address(packet, incoming_addr);

            if (pairing_lockout) {
                printf("[BTSTACK_HOST] Declining incoming HID connection while pairing is locked\n");
                hid_host_decline_connection(hid_cid);
                break;
            }

            // For Wiimotes/Wii U Pro: accept HID Host connection for reconnection
            if (wiimote_conn.active && memcmp(incoming_addr, wiimote_conn.addr, 6) == 0) {
                printf("[BTSTACK_HOST] Wiimote HID incoming - accepting\n");
                wiimote_conn.using_hid_host = true;
                wiimote_conn.hid_host_cid = hid_cid;
                hid_host_accept_connection(hid_cid, HID_PROTOCOL_MODE_REPORT);

                // Allocate classic_connection slot for HID_SUBEVENT_CONNECTION_OPENED to find
                classic_connection_t* conn = find_free_classic_connection();
                if (conn) {
                    memset(conn, 0, sizeof(*conn));
                    conn->active = true;
                    conn->hid_cid = hid_cid;
                    memcpy(conn->addr, wiimote_conn.addr, 6);
                    memcpy(conn->class_of_device, wiimote_conn.class_of_device, 3);
                    strncpy(conn->name, wiimote_conn.name, sizeof(conn->name) - 1);
                    conn->vendor_id = 0x057E;  // Nintendo
                    conn->connect_time = btstack_run_loop_get_time_ms();
                    // Get index for wiimote_conn
                    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
                        if (&classic_state.connections[i] == conn) {
                            wiimote_conn.conn_index = i;
                            printf("[BTSTACK_HOST] Wiimote: allocated conn_index=%d for HID Host\n", i);
                            break;
                        }
                    }
                }
                break;
            }

            // REPORT for every incoming peer. This previously defaulted to
            // REPORT_WITH_FALLBACK_TO_BOOT for any device whose name was not yet
            // known, but on an INCOMING connection 1.6.2 overwrote the requested
            // mode with REPORT as soon as SDP returned a HID record -- so the two
            // only ever differed when SDP failed. BTstack 1.8.2 removed the
            // enumerator; see bt_device_db.h.
            printf("[BTSTACK_HOST] HID incoming connection, cid=0x%04X - accepting (mode=REPORT)\n",
                   hid_cid);
            hid_host_accept_connection(hid_cid, HID_PROTOCOL_MODE_REPORT);

            // Allocate connection slot if needed
            if (!find_classic_connection_by_cid(hid_cid)) {
                classic_connection_t* conn = find_free_classic_connection();
                if (conn) {
                    memset(conn, 0, sizeof(*conn));
                    conn->active = true;
                    conn->hid_cid = hid_cid;
                    hid_subevent_incoming_connection_get_address(packet, conn->addr);

                    // Use pending COD and name if address matches (from HCI_EVENT_CONNECTION_REQUEST)
                    if (classic_state.pending_valid &&
                        memcmp(conn->addr, classic_state.pending_addr, 6) == 0) {
                        conn->class_of_device[0] = classic_state.pending_cod & 0xFF;
                        conn->class_of_device[1] = (classic_state.pending_cod >> 8) & 0xFF;
                        conn->class_of_device[2] = (classic_state.pending_cod >> 16) & 0xFF;
                        // Copy name if we got it from remote name request
                        if (classic_state.pending_name[0]) {
                            strncpy(conn->name, classic_state.pending_name, sizeof(conn->name) - 1);
                            conn->name[sizeof(conn->name) - 1] = '\0';
                            printf("[BTSTACK_HOST] Using pending name: %s\n", conn->name);
                        }
                        // Copy VID/PID if we got them from SDP query
                        if (classic_state.pending_vid || classic_state.pending_pid) {
                            conn->vendor_id = classic_state.pending_vid;
                            conn->product_id = classic_state.pending_pid;
                            printf("[BTSTACK_HOST] Using pending VID/PID: 0x%04X/0x%04X\n",
                                   conn->vendor_id, conn->product_id);
                        }
                        // DON'T clear pending_valid here - PIN code request may come after this
                        // It will be cleared in HID_SUBEVENT_CONNECTION_OPENED
                        printf("[BTSTACK_HOST] Using pending COD: 0x%06X\n", (unsigned)classic_state.pending_cod);
                    }
                    conn->connect_time = btstack_run_loop_get_time_ms();
                }
            }
            break;
        }

        case HID_SUBEVENT_CONNECTION_OPENED: {
            uint16_t hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            uint8_t status = hid_subevent_connection_opened_get_status(packet);
            classic_connection_t* opening_conn = find_classic_connection_by_cid(hid_cid);
            {
                char reason[BTID_REASON_LEN];
                snprintf(reason, sizeof(reason), "classic-hid-open-0x%02X", status);
                classic_pair_diag(
                    opening_conn ? (uint8_t)(opening_conn - classic_state.connections) : 0xFF,
                    opening_conn && opening_conn->name[0] ? opening_conn->name
                                                          : classic_state.pending_name,
                    opening_conn
                        ? ((uint32_t)opening_conn->class_of_device[0] |
                           ((uint32_t)opening_conn->class_of_device[1] << 8) |
                           ((uint32_t)opening_conn->class_of_device[2] << 16))
                        : classic_state.pending_cod,
                    opening_conn ? opening_conn->vendor_id : classic_state.pending_vid,
                    opening_conn ? opening_conn->product_id : classic_state.pending_pid,
                    reason);
            }

            // Reset security level if we elevated it for Wiimote
            if (classic_state.pending_hid_connect) {
                printf("[BTSTACK_HOST] Resetting security level to 0\n");
                gap_set_security_level(LEVEL_0);
                classic_state.pending_hid_connect = false;
            }

            // Clear pending CONNECTION bookkeeping now that HID is established.
            //
            // The SECURITY half deliberately survives this point. HID channels
            // are registered at LEVEL_0, so they can open before -- or without
            // -- encryption, and discarding the parked link key here threw away
            // a first pairing that was still one Encryption Change away from
            // being durable. It is cleared when the ACL that owns it drops, and
            // classic_pending_security_prepare() clears it before any new
            // admission, so nothing can inherit it.
            classic_state.pending_valid = false;

            if (status != ERROR_CODE_SUCCESS) {
                btlife_record(BTLIFE_HID_FAIL, status, hid_cid);
                printf("[BTSTACK_HOST] HID connection failed, cid=0x%04X status=0x%02X\n", hid_cid, status);
                // Remove connection slot
                classic_connection_t* conn = find_classic_connection_by_cid(hid_cid);
                if (conn) {
                    memset(conn, 0, sizeof(*conn));
                }

                // If this was an outgoing connection, disconnect ACL and wait for
                // the device to reconnect via the incoming path. Some controllers
                // (certain DS4 HW revisions) don't accept HID L2CAP channels from
                // the host but work when they initiate the connection themselves.
                // A link key was exchanged during the failed attempt, so when the
                // device reconnects (incoming), authentication will use the stored key.
                // Don't resume scanning — otherwise we'll rediscover the device
                // still in pairing mode and loop endlessly.
                if (!hid_subevent_connection_opened_get_incoming(packet)) {
                    hci_con_handle_t con_handle = hid_subevent_connection_opened_get_con_handle(packet);
                    printf("[BTSTACK_HOST] Outgoing HID failed, disconnecting to allow incoming reconnect\n");
                    gap_disconnect(con_handle);
                    classic_state.pending_outgoing = false;
                    classic_state.pending_valid = false;
                    classic_pending_security_clear();
                    // Don't scan — stay connectable, wait for incoming reconnection
                    classic_state.waiting_for_incoming_time = btstack_run_loop_get_time_ms();
                }
                return;
            }

            printf("[BTSTACK_HOST] HID connection opened, cid=0x%04X\n", hid_cid);

            // Mark connection as ready (HID channels established)
            classic_connection_t* conn = find_classic_connection_by_cid(hid_cid);
            if (conn) {
                // FAIL CLOSED. Standing down from initiating authentication and
                // encryption changes WHO STARTS them, never whether they are
                // required. If the companion did not in fact drive security to
                // completion, this link must not become usable -- an
                // unauthenticated or unencrypted Controller Link is a worse
                // outcome than a failed one.
                hci_connection_t *acl =
                    hci_connection_for_bd_addr_and_type(conn->addr, BD_ADDR_TYPE_ACL);
                bool is_companion =
                    btstack_host_classic_companion_session_trust(conn->addr);
                uint8_t key_size = acl ? gap_encryption_key_size(acl->con_handle) : 0u;
                if (is_companion && key_size < NS2_BT_REQUIRED_CLASSIC_KEY_SIZE) {
                    classic_encryption_unencrypted_active++;
                    btlife_record_addr(BTLIFE_HID_FAIL, key_size,
                                       acl ? acl->con_handle : 0u, conn->addr);
                    printf("[BTSTACK_HOST] Refusing companion HID: Classic security "
                           "incomplete (handle=0x%04X, key_size=%u, count=%lu)\n",
                           acl ? acl->con_handle : 0xFFFFu, key_size,
                           (unsigned long)classic_encryption_unencrypted_active);
                    if (acl != NULL) {
                        gap_disconnect(acl->con_handle);
                    }
                    break;
                }

                conn->hid_ready = true;
                // Final phase boundary: both HID channels usable.
                btlife_record_addr(BTLIFE_HID_READY, key_size,
                                   acl ? acl->con_handle : 0u, conn->addr);
                if (acl != NULL && auth_decision_owns(acl->con_handle)) {
                    last_auth_decision.hid_ready = true;
                    // The only valid sampling point for the Classic key size,
                    // and the exact value the acceptance gate above judged. By
                    // now BTstack's HCI_Read_Encryption_Key_Size has completed;
                    // in the Encryption Change handler it has not even been
                    // sent, which is why this used to report 0 on every link.
                    last_auth_decision.encryption_key_size = key_size;
                    last_auth_decision.key_size_valid = true;
                }
                if (is_companion && acl != NULL) {
                    // Late binding for a companion link that reached HID
                    // readiness without passing the incoming-connection latch
                    // (outgoing/direct-L2CAP paths). Idempotent.
                    classic_companion_note_link(acl->con_handle);
                }

                // Check if this is a direct-L2CAP device by profile or name
                bool is_direct_l2cap = (conn->profile &&
                                        conn->profile->classic == BT_CLASSIC_DIRECT_L2CAP);
                // Also check by name if profile wasn't set (late name resolution)
                if (!is_direct_l2cap && conn->name[0]) {
                    const bt_device_profile_t* conn_profile = bt_device_lookup_by_name(conn->name);
                    if (conn_profile->classic == BT_CLASSIC_DIRECT_L2CAP) {
                        is_direct_l2cap = true;
                    }
                    if (!conn->profile) {
                        conn->profile = conn_profile;
                    }
                }
                // Also check wiimote_conn state (may have been set up during inquiry)
                if (!is_direct_l2cap && wiimote_conn.active &&
                    memcmp(conn->addr, wiimote_conn.addr, 6) == 0) {
                    is_direct_l2cap = true;
                }

                if (is_direct_l2cap) {
                    // Direct L2CAP devices: HID Host handles receiving, we send via direct L2CAP
                    printf("[BTSTACK_HOST] %s HID connected via HID Host (receive via HID Host, send via L2CAP)\n",
                           conn->profile ? conn->profile->name : "Wiimote");

                    // Set default VID/PID from profile if not already set
                    if (conn->vendor_id == 0 && conn->profile && conn->profile->default_vid) {
                        conn->vendor_id = conn->profile->default_vid;
                    }
                    if (conn->product_id == 0) {
                        uint16_t pid = bt_device_wiimote_pid_from_name(conn->name);
                        if (pid) {
                            conn->product_id = pid;
                            printf("[BTSTACK_HOST] Detected %s by name, PID=0x%04X\n",
                                   conn->profile ? conn->profile->name : "device", pid);
                        }
                    }

                    // Initialize wiimote_conn if not already active (e.g., incoming
                    // reconnection where name wasn't available at CONNECTION_COMPLETE)
                    if (!wiimote_conn.active) {
                        memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                        wiimote_conn.active = true;
                        wiimote_conn.state = WIIMOTE_STATE_IDLE;
                        memcpy(wiimote_conn.addr, conn->addr, 6);
                        memcpy(wiimote_conn.class_of_device, conn->class_of_device, 3);
                        wiimote_conn.using_hid_host = true;
                        wiimote_conn.hid_host_cid = hid_cid;
                    }

                    // Link wiimote_conn to this classic_connection slot for routing
                    int conn_index = get_classic_conn_index(hid_cid);
                    if (conn_index >= 0) {
                        wiimote_conn.conn_index = conn_index;
                        wiimote_conn.vendor_id = conn->vendor_id;
                        wiimote_conn.product_id = conn->product_id;
                        strncpy(wiimote_conn.name, conn->name, sizeof(wiimote_conn.name) - 1);

                        bthid_update_device_info(conn_index, conn->name,
                                                 conn->vendor_id, conn->product_id);

                        // NOTE: We previously called hid_host_get_l2cap_cids() here to get L2CAP CIDs
                        // from HID Host for direct L2CAP sending (Wiimotes don't support SET_PROTOCOL).
                        // This required a custom BTstack patch (see .dev/docs/btstack-patches.md).
                        // Removed because CI uses upstream BTstack without the patch.
                        // If Wiimote HID Host mode has issues, consider re-adding the patch.

                        printf("[BTSTACK_HOST] Wiimote: conn_index=%d control_cid=0x%04X interrupt_cid=0x%04X using_hid_host=%d\n",
                               conn_index, wiimote_conn.control_cid, wiimote_conn.interrupt_cid, wiimote_conn.using_hid_host);

                        if (wiimote_conn.using_hid_host) {
                            // Using HID Host — set up state but defer bt_on_hid_ready
                            // to DESCRIPTOR_AVAILABLE. BTstack's HID Host immediately
                            // starts SDP after CONNECTION_OPENED (state → W2_SEND_SDP_QUERY),
                            // and hid_host_send_report() fails with COMMAND_DISALLOWED
                            // until SDP + SET_PROTOCOL complete. Deferring ensures the
                            // driver's init subcommands (SET_INPUT_MODE etc.) succeed.
                            wiimote_conn.hid_host_ready = true;
                            wiimote_conn.state = WIIMOTE_STATE_CONNECTED;
                            btstack_host_stop_scan();
                            scan_timeout_end = 0;
                            printf("[BTSTACK_HOST] Wiimote: HID Host ready, deferring bt_on_hid_ready to DESCRIPTOR_AVAILABLE\n");
                        } else if (wiimote_conn.control_cid != 0 && wiimote_conn.interrupt_cid != 0) {
                            printf("[BTSTACK_HOST] Wiimote: calling bt_on_hid_ready(%d) via direct L2CAP\n", conn_index);
                            bt_on_hid_ready(conn_index);
                        } else {
                            printf("[BTSTACK_HOST] Wiimote: waiting for L2CAP CIDs before ready\n");
                        }
                    }
                } else {
                    // Set default VID/PID from profile if available
                    if (conn->vendor_id == 0 && conn->profile && conn->profile->default_vid) {
                        conn->vendor_id = conn->profile->default_vid;
                        printf("[BTSTACK_HOST] Set VID=0x%04X from %s profile\n",
                               conn->vendor_id, conn->profile->name);
                    }

                    // Non-Wiimote: wait for HID_SUBEVENT_DESCRIPTOR_AVAILABLE
                    // NOTE: Do NOT issue SDP queries here — BTstack HID Host starts its
                    // own SDP query (for HID descriptor) immediately after CONNECTION_OPENED.
                    // sdp_client only handles one query at a time, so issuing ours here
                    // would block BTstack's, preventing DESCRIPTOR_AVAILABLE from firing.
                    // VID/PID SDP query is deferred to DESCRIPTOR_AVAILABLE instead.
                }
            }
            break;
        }

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
            uint16_t hid_cid = hid_subevent_descriptor_available_get_hid_cid(packet);
            uint8_t status = hid_subevent_descriptor_available_get_status(packet);

            printf("[BTSTACK_HOST] HID descriptor available, cid=0x%04X status=0x%02X\n", hid_cid, status);

            // Notify bthid layer that device is ready
            // This fires after SDP + SET_PROTOCOL complete, so BTstack's state
            // is CONNECTION_ESTABLISHED and hid_host_send_report() will succeed.
            int conn_index = get_classic_conn_index(hid_cid);
            if (conn_index >= 0) {
                // Pass HID descriptor to bthid for generic gamepad parsing
                const uint8_t* hid_desc = hid_descriptor_storage_get_descriptor_data(hid_cid);
                uint16_t hid_desc_len = hid_descriptor_storage_get_descriptor_len(hid_cid);
                if (hid_desc && hid_desc_len > 0) {
                    printf("[BTSTACK_HOST] Classic HID descriptor: %d bytes\n", hid_desc_len);
                    bthid_set_hid_descriptor(conn_index, hid_desc, hid_desc_len);
                }

                btstack_host_stop_scan();
                scan_timeout_end = 0;
                printf("[BTSTACK_HOST] Calling bt_on_hid_ready(%d)\n", conn_index);
                bt_on_hid_ready(conn_index);

                // Query VID/PID via SDP if not yet known (deferred from CONNECTION_OPENED
                // to avoid conflicting with BTstack's internal HID descriptor SDP query)
                classic_connection_t* desc_conn = find_classic_connection_by_cid(hid_cid);
                if (desc_conn &&
                    (desc_conn->vendor_id == 0 ||
                     desc_conn->product_id == 0)) {
                    printf("[BTSTACK_HOST] Queueing VID/PID SDP query after HID descriptor\n");
                    classic_identity_query_schedule(desc_conn->addr);
                }
            }
            break;
        }

        case HID_SUBEVENT_GET_REPORT_RESPONSE: {
            uint16_t hid_cid =
                hid_subevent_get_report_response_get_hid_cid(packet);
            uint8_t status =
                hid_subevent_get_report_response_get_handshake_status(packet);
            uint16_t report_len =
                hid_subevent_get_report_response_get_report_len(packet);
            const uint8_t *report =
                hid_subevent_get_report_response_get_report(packet);
            int conn_index = get_classic_conn_index(hid_cid);
            if (status == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL &&
                conn_index >= 0 && report_len > 0) {
                bthid_on_feature_report((uint8_t)conn_index, report,
                                        report_len);
            } else {
                printf("[BTSTACK_HOST] GET_REPORT failed: cid=0x%04X status=%u len=%u\n",
                       hid_cid, status, report_len);
            }
            break;
        }

        case HID_SUBEVENT_REPORT: {
            uint16_t hid_cid = hid_subevent_report_get_hid_cid(packet);
            const uint8_t* report = hid_subevent_report_get_report(packet);
            uint16_t report_len = hid_subevent_report_get_report_len(packet);

            // Debug: show raw BTstack report
            if (!btstack_report_debug_done && report_len >= 4) {
                printf("[BTSTACK_HOST] Raw report len=%d: %02X %02X %02X %02X\n",
                       report_len, report[0], report[1], report[2], report[3]);
                btstack_report_debug_done = true;
            }

            // Route to bthid layer
            // BTstack report already includes 0xA1 header (DATA|INPUT)
            int conn_index = get_classic_conn_index(hid_cid);
            if (conn_index >= 0 && report_len > 0) {
                bt_on_hid_report_with_generation(
                    (uint8_t)conn_index,
                    bthid_get_connection_generation((uint8_t)conn_index),
                    report, report_len);
            }
            break;
        }

        case HID_SUBEVENT_CONNECTION_CLOSED: {
            uint16_t hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);
            printf("[BTSTACK_HOST] HID connection closed, cid=0x%04X\n", hid_cid);
            classic_connection_t* closing_conn = find_classic_connection_by_cid(hid_cid);
            if (closing_conn) {
                classic_pair_diag(
                    (uint8_t)(closing_conn - classic_state.connections),
                    closing_conn->name,
                    (uint32_t)closing_conn->class_of_device[0] |
                        ((uint32_t)closing_conn->class_of_device[1] << 8) |
                        ((uint32_t)closing_conn->class_of_device[2] << 16),
                    closing_conn->vendor_id, closing_conn->product_id,
                    "classic-hid-closed");
            }

            // Reset debug flag so reconnections produce debug output
            btstack_report_debug_done = false;

            // Notify bthid layer
            int conn_index = get_classic_conn_index(hid_cid);
            if (conn_index >= 0) {
                bt_on_disconnect_with_generation(
                    (uint8_t)conn_index,
                    bthid_get_connection_generation((uint8_t)conn_index));
            }

            // Free connection slot
            classic_connection_t* conn = find_classic_connection_by_cid(hid_cid);
            if (conn) {
                memset(conn, 0, sizeof(*conn));
            }

            // Resume scanning if no devices remain
            if (btstack_classic_get_connection_count() == 0) {
                printf("[BTSTACK_HOST] No devices connected, resuming scan\n");
                btstack_host_start_scan();
            }
            break;
        }

        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE: {
            uint16_t hid_cid = hid_subevent_set_protocol_response_get_hid_cid(packet);
            uint8_t handshake = hid_subevent_set_protocol_response_get_handshake_status(packet);
            hid_protocol_mode_t mode = hid_subevent_set_protocol_response_get_protocol_mode(packet);
            printf("[BTSTACK_HOST] HID set protocol response: cid=0x%04X handshake=%d mode=%d\n",
                   hid_cid, handshake, mode);
            break;
        }

        default:
            printf("[BTSTACK_HOST] HID subevent: 0x%02X\n", subevent);
            break;
    }
}

// ============================================================================
// WIIMOTE DIRECT L2CAP PACKET HANDLER
// ============================================================================

static void wiimote_l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);

    switch (packet_type) {
        case HCI_EVENT_PACKET: {
            uint8_t event_type = hci_event_packet_get_type(packet);

            if (event_type == L2CAP_EVENT_CHANNEL_OPENED) {
                uint8_t status = l2cap_event_channel_opened_get_status(packet);
                uint16_t local_cid = l2cap_event_channel_opened_get_local_cid(packet);
                uint16_t psm = l2cap_event_channel_opened_get_psm(packet);

                printf("[BTSTACK_HOST] Wiimote L2CAP opened: status=%d PSM=0x%04X cid=0x%04X\n",
                       status, psm, local_cid);

                if (status != 0) {
                    printf("[BTSTACK_HOST] Wiimote: L2CAP channel failed: 0x%02X\n", status);
                    // Don't deactivate - wait for HID Host to handle via HID_SUBEVENT_INCOMING_CONNECTION
                    // (timing varies: HID incoming may come before or after L2CAP failure)
                    printf("[BTSTACK_HOST] Wiimote: waiting for HID Host fallback\n");
                    return;
                }

                if (psm == PSM_HID_CONTROL && wiimote_conn.state == WIIMOTE_STATE_W4_CONTROL_CONNECTED) {
                    // Control channel opened, now create interrupt channel
                    printf("[BTSTACK_HOST] Wiimote: Control channel connected, creating Interrupt channel (PSM 0x13)...\n");

                    uint16_t interrupt_cid;
                    uint8_t l2cap_status = l2cap_create_channel(wiimote_l2cap_packet_handler,
                                                                wiimote_conn.addr,
                                                                PSM_HID_INTERRUPT,
                                                                0xFFFF,
                                                                &interrupt_cid);
                    if (l2cap_status == ERROR_CODE_SUCCESS) {
                        wiimote_conn.interrupt_cid = interrupt_cid;
                        wiimote_conn.state = WIIMOTE_STATE_W4_INTERRUPT_CONNECTED;
                        printf("[BTSTACK_HOST] Wiimote: L2CAP interrupt channel request sent, cid=0x%04X\n", interrupt_cid);
                    } else {
                        printf("[BTSTACK_HOST] Wiimote: l2cap_create_channel (interrupt) failed: 0x%02X\n", l2cap_status);
                        wiimote_conn.active = false;
                        classic_state.pending_hid_connect = false;
                    }

                } else if (psm == PSM_HID_INTERRUPT && wiimote_conn.state == WIIMOTE_STATE_W4_INTERRUPT_CONNECTED) {
                    // Interrupt channel opened - connection complete!
                    printf("[BTSTACK_HOST] Wiimote: Interrupt channel connected - HID READY!\n");
                    wiimote_conn.state = WIIMOTE_STATE_CONNECTED;
                    direct_output_clear();
                    classic_state.pending_hid_connect = false;

                    // Stop scanning now that we have a connected device
                    btstack_host_stop_scan();
                    scan_timeout_end = 0;

                    // Allocate classic connection slot if not already allocated (reconnection case)
                    if (wiimote_conn.conn_index < 0) {
                        classic_connection_t* conn = find_free_classic_connection();
                        if (conn) {
                            memset(conn, 0, sizeof(*conn));
                            conn->active = true;
                            conn->hid_cid = 0xFFFF;  // Mark as Wiimote (no HID Host CID)
                            memcpy(conn->addr, wiimote_conn.addr, 6);
                            strncpy(conn->name, wiimote_conn.name, sizeof(conn->name) - 1);
                            conn->vendor_id = 0x057E;  // Nintendo
                            conn->product_id = bt_device_wiimote_pid_from_name(wiimote_conn.name);
                            conn->hid_ready = true;

                            // Get index
                            for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
                                if (&classic_state.connections[i] == conn) {
                                    wiimote_conn.conn_index = i;
                                    wiimote_conn.vendor_id = conn->vendor_id;
                                    wiimote_conn.product_id = conn->product_id;
                                    printf("[BTSTACK_HOST] Wiimote: allocated conn_index=%d\n", i);
                                    break;
                                }
                            }
                        }
                    }

                    // Update the classic connection slot
                    if (wiimote_conn.conn_index >= 0 && wiimote_conn.conn_index < MAX_CLASSIC_CONNECTIONS) {
                        classic_connection_t* conn = &classic_state.connections[wiimote_conn.conn_index];
                        conn->hid_ready = true;

                        // Update bthid with device info
                        // Use SDP VID/PID if available, otherwise default to Nintendo (0x057E)
                        uint16_t vid = wiimote_conn.vendor_id ? wiimote_conn.vendor_id : 0x057E;
                        uint16_t pid = wiimote_conn.product_id;
                        printf("[BTSTACK_HOST] Wiimote: updating bthid with name='%s' VID=0x%04X PID=0x%04X\n",
                               wiimote_conn.name, vid, pid);
                        bthid_update_device_info(wiimote_conn.conn_index, wiimote_conn.name, vid, pid);

                        // Notify bthid layer
                        printf("[BTSTACK_HOST] Wiimote: calling bt_on_hid_ready(%d)\n", wiimote_conn.conn_index);
                        bt_on_hid_ready(wiimote_conn.conn_index);
                    }
                }

            } else if (event_type == L2CAP_EVENT_CAN_SEND_NOW) {
                uint16_t local_cid = l2cap_event_can_send_now_get_local_cid(packet);
                if (local_cid == wiimote_conn.interrupt_cid &&
                    direct_output_pending()) {
                    direct_output_try_send(local_cid);
                }
            } else if (event_type == L2CAP_EVENT_CHANNEL_CLOSED) {
                uint16_t local_cid = l2cap_event_channel_closed_get_local_cid(packet);
                printf("[BTSTACK_HOST] Wiimote L2CAP closed: cid=0x%04X\n", local_cid);

                if (wiimote_conn.active &&
                    (local_cid == wiimote_conn.control_cid || local_cid == wiimote_conn.interrupt_cid)) {
                    // Notify disconnect
                    if (wiimote_conn.conn_index >= 0) {
                        bt_on_disconnect_with_generation(
                            (uint8_t)wiimote_conn.conn_index,
                            bthid_get_connection_generation(
                                (uint8_t)wiimote_conn.conn_index));
                        // Clear connection slot
                        if (wiimote_conn.conn_index < MAX_CLASSIC_CONNECTIONS) {
                            memset(&classic_state.connections[wiimote_conn.conn_index], 0, sizeof(classic_connection_t));
                        }
                    }
                    direct_output_clear();
                    memset(&wiimote_conn, 0, sizeof(wiimote_conn));
                }
            }
            break;
        }

        case L2CAP_DATA_PACKET: {
            // HID data from Wiimote interrupt channel
            // Data already includes HID header (0xA1 for DATA|INPUT)
            if (wiimote_conn.active && wiimote_conn.state == WIIMOTE_STATE_CONNECTED) {
                // Route to bthid layer
                if (wiimote_conn.conn_index >= 0 && size > 0) {
                    bt_on_hid_report(wiimote_conn.conn_index, packet, size);
                }
            } else {
#ifdef NS2_DIAG
                printf("[BTSTACK_HOST] Wiimote data dropped: active=%d state=%d\n",
                       wiimote_conn.active, wiimote_conn.state);
#endif
            }
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// CLASSIC BT OUTPUT REPORTS
// ============================================================================

// Send SET_REPORT on control channel with specified report type
// report_type: 1=Input, 2=Output, 3=Feature
bool btstack_classic_send_set_report_type(uint8_t conn_index, uint8_t report_type,
                                           uint8_t report_id, const uint8_t* data, uint16_t len)
{
    if (conn_index >= MAX_CLASSIC_CONNECTIONS) return false;

    classic_connection_t* conn = &classic_state.connections[conn_index];
    if (!conn->active || !conn->hid_ready) return false;

    // Check if this is a Wiimote/direct L2CAP connection (marked with hid_cid = 0xFFFF)
    if (conn->hid_cid == 0xFFFF && wiimote_conn.active &&
        wiimote_conn.conn_index == conn_index &&
        wiimote_conn.state == WIIMOTE_STATE_CONNECTED) {
        // Send SET_REPORT on control channel via raw L2CAP
        // HID transaction format: [SET_REPORT | report_type] [report_id] [data...]
        static uint8_t wiimote_setreport_buf[80];
        uint16_t total = len + 2;
        if (total > sizeof(wiimote_setreport_buf)) return false;
        wiimote_setreport_buf[0] = 0x50 | (report_type & 0x03);  // SET_REPORT | type
        wiimote_setreport_buf[1] = report_id;
        if (len > 0) memcpy(wiimote_setreport_buf + 2, data, len);
        uint8_t status = l2cap_send(wiimote_conn.control_cid, wiimote_setreport_buf, total);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[BTSTACK_HOST] wiimote send_set_report failed: type=%d id=0x%02X status=%d\n",
                   report_type, report_id, status);
        }
        return status == ERROR_CODE_SUCCESS;
    }

    // Map report type to BTstack enum
    hid_report_type_t hid_type;
    switch (report_type) {
        case 1: hid_type = HID_REPORT_TYPE_INPUT; break;
        case 2: hid_type = HID_REPORT_TYPE_OUTPUT; break;
        case 3: hid_type = HID_REPORT_TYPE_FEATURE; break;
        default: hid_type = HID_REPORT_TYPE_OUTPUT; break;
    }

    // hid_host_send_set_report stores a pointer to the data and sends asynchronously.
    // Copy into static buffer so the data persists until the actual L2CAP send completes.
    static uint8_t hid_host_set_report_buf[80];
    if (len > sizeof(hid_host_set_report_buf)) return false;
    if (len > 0) memcpy(hid_host_set_report_buf, data, len);

    uint8_t status = hid_host_send_set_report(conn->hid_cid, hid_type, report_id, hid_host_set_report_buf, len);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[BTSTACK_HOST] send_set_report failed: type=%d id=0x%02X status=%d\n",
               report_type, report_id, status);
    }
    return status == ERROR_CODE_SUCCESS;
}

bool btstack_classic_get_feature_report(uint8_t conn_index, uint8_t report_id)
{
    if (conn_index >= MAX_CLASSIC_CONNECTIONS) return false;

    classic_connection_t *conn = &classic_state.connections[conn_index];
    if (!conn->active || !conn->hid_ready || conn->hid_cid == 0xFFFF)
        return false;

    uint8_t status = hid_host_send_get_report(
        conn->hid_cid, HID_REPORT_TYPE_FEATURE, report_id);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[BTSTACK_HOST] send_get_report failed: id=0x%02X status=%u\n",
               report_id, status);
    }
    return status == ERROR_CODE_SUCCESS;
}

// Send SET_REPORT on control channel (default to OUTPUT type)
bool btstack_classic_send_set_report(uint8_t conn_index, uint8_t report_id,
                                      const uint8_t* data, uint16_t len)
{
    return btstack_classic_send_set_report_type(conn_index, 2, report_id, data, len);
}

// Send DATA on interrupt channel (for regular output reports)
bool btstack_classic_send_report(uint8_t conn_index, uint8_t report_id,
                                  const uint8_t* data, uint16_t len)
{
    // BLE connection — use GATT HIDS Host
    if (conn_index >= BLE_CONN_INDEX_OFFSET) {
        uint8_t ble_index = conn_index - BLE_CONN_INDEX_OFFSET;
        if (ble_index >= MAX_BLE_CONNECTIONS) return false;
        ble_connection_t* conn = &hid_state.connections[ble_index];
        if (conn->handle == HCI_CON_HANDLE_INVALID || !conn->hid_ready) return false;
        if (conn->hids_cid == 0) return false;
        uint8_t status = hids_host_send_write_report(conn->hids_cid, report_id,
                                                        HID_REPORT_TYPE_OUTPUT,
                                                        data, len);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[BTSTACK_HOST] BLE send_write_report failed: report_id=0x%02X status=0x%02X\n",
                   report_id, status);
        }
        return status == ERROR_CODE_SUCCESS;
    }

    if (conn_index >= MAX_CLASSIC_CONNECTIONS) return false;

    classic_connection_t* conn = &classic_state.connections[conn_index];
    if (!conn->active || !conn->hid_ready) return false;

#ifdef NS2_DS5_AUDIO
    // Fresh Sony pairing uses the raw direct-L2CAP connection below. A bonded
    // controller reconnects through HID Host and therefore has a real hid_cid.
    // Route only the exact oversized DualSense protocol shapes through the
    // 16-bit-length extension. Do not depend on name/VID metadata: HID becomes
    // ready before late identity resolution during a normal bonded reconnect.
    if (conn->hid_cid != 0xFFFF &&
        ds5_reconnect_uses_long_hid_report(report_id, len)) {
        uint8_t const status = ns2_hid_host_send_long_report(
            conn->hid_cid, report_id, data, len);
        return status == ERROR_CODE_SUCCESS;
    }
#endif

    // Check if this is a Wiimote (direct L2CAP, marked with hid_cid = 0xFFFF)
    if (conn->hid_cid == 0xFFFF && wiimote_conn.active &&
        wiimote_conn.conn_index == conn_index &&
        wiimote_conn.state == WIIMOTE_STATE_CONNECTED) {
        // Build HID packet: 0xA2 (DATA|OUTPUT) + report_id + data. Accept it
        // into a bounded/coalescing queue even when L2CAP cannot send this instant;
        // the packet handler drains it from L2CAP_EVENT_CAN_SEND_NOW.
        if (len + 2 > DIRECT_OUTPUT_MAX_LEN) return false;
        // A 0x39 audio packet needs a 548-byte interrupt MTU. Detect a
        // permanently undersized negotiated channel here instead of accepting
        // the report and retrying L2CAP_DATA_LEN_EXCEEDS_REMOTE_MTU forever.
        uint16_t const remote_mtu =
            l2cap_get_remote_mtu_for_local_cid(wiimote_conn.interrupt_cid);
        if (remote_mtu != 0 && len + 2 > remote_mtu) {
            printf("[BTSTACK_HOST] Direct output len=%u exceeds interrupt MTU=%u\n",
                   (unsigned)(len + 2), (unsigned)remote_mtu);
            return false;
        }
        uint8_t entry_index;
        if (direct_output_queue.count < DIRECT_OUTPUT_QUEUE_DEPTH) {
            entry_index =
                (uint8_t)((direct_output_queue.head +
                           direct_output_queue.count) %
                          DIRECT_OUTPUT_QUEUE_DEPTH);
            direct_output_queue.count++;
        } else {
            // Preserve audio losslessly. Ordinary LED/rumble churn may still
            // coalesce into the newest queued entry when the FIFO is full.
            bool const incoming_audio = report_id == 0x39u;
            entry_index =
                (uint8_t)((direct_output_queue.head +
                           direct_output_queue.count - 1u) %
                          DIRECT_OUTPUT_QUEUE_DEPTH);
            bool const queued_audio =
                direct_output_queue.entries[entry_index].data[1] == 0x39u;
            if (incoming_audio || queued_audio) return false;
        }
        direct_output_entry_t *entry =
            &direct_output_queue.entries[entry_index];
        uint8_t *dst = entry->data;
        entry->len = len + 2;
        dst[0] = 0xA2;
        dst[1] = report_id;
        memcpy(dst + 2, data, len);
        direct_output_try_send(wiimote_conn.interrupt_cid);
        return true;
    }

    // hid_host_send_report stores a pointer to the data and sends asynchronously.
    // Copy into static buffer so the data persists until the actual L2CAP send completes.
    static uint8_t hid_host_report_buf[80];
    if (len > sizeof(hid_host_report_buf)) return false;
    if (len > 0) memcpy(hid_host_report_buf, data, len);

    return hid_host_send_report(conn->hid_cid, report_id, hid_host_report_buf, len) == ERROR_CODE_SUCCESS;
}

// Check if a connection is a Wiimote (using direct L2CAP)
bool btstack_wiimote_is_connection(uint8_t conn_index)
{
    if (conn_index >= MAX_CLASSIC_CONNECTIONS) return false;
    classic_connection_t* conn = &classic_state.connections[conn_index];
    // Wiimote connections are marked with hid_cid = 0xFFFF
    return conn->active && conn->hid_cid == 0xFFFF &&
           wiimote_conn.active && wiimote_conn.conn_index == conn_index;
}

// Check if we can send on Wiimote L2CAP channel
bool btstack_wiimote_can_send(uint8_t conn_index)
{
    if (!wiimote_conn.active) {
        return false;
    }

    // Prefer direct L2CAP when we have the interrupt CID
    if (wiimote_conn.interrupt_cid != 0) {
        return l2cap_can_send_packet_now(wiimote_conn.interrupt_cid) != 0;
    }

    // Fallback to HID Host path
    if (wiimote_conn.using_hid_host && wiimote_conn.hid_host_ready) {
        return true;  // HID Host handles flow control internally
    }

    return false;
}

// Send raw L2CAP data to Wiimote on INTERRUPT channel
bool btstack_wiimote_send_raw(uint8_t conn_index, const uint8_t* data, uint16_t len)
{
    UNUSED(conn_index);
#ifdef NS2_DIAG
    printf("[BTSTACK_HOST] wiimote_send_raw: active=%d using_hid=%d hid_ready=%d int_cid=0x%04X\n",
           wiimote_conn.active, wiimote_conn.using_hid_host, wiimote_conn.hid_host_ready, wiimote_conn.interrupt_cid);
#endif

    if (!wiimote_conn.active) {
#ifdef NS2_DIAG
        printf("[BTSTACK_HOST] wiimote_send_raw: no active connection\n");
#endif
        return false;
    }
    if (len == 0 || len > 80) {
#ifdef NS2_DIAG
        printf("[BTSTACK_HOST] wiimote_send_raw: bad len=%d\n", len);
#endif
        return false;
    }

    // Prefer direct L2CAP when we have the interrupt CID (works even with HID Host)
    // This bypasses hid_host_send_report which can fail with 0x0C if HID Host state isn't ready
    if (wiimote_conn.interrupt_cid != 0) {
        if (!l2cap_can_send_packet_now(wiimote_conn.interrupt_cid)) {
#ifdef NS2_DIAG
            printf("[BTSTACK_HOST] wiimote_send_raw: L2CAP not ready to send\n");
#endif
            return false;
        }

        uint8_t status = l2cap_send(wiimote_conn.interrupt_cid, data, len);
        if (status != ERROR_CODE_SUCCESS) {
#ifdef NS2_DIAG
            printf("[BTSTACK_HOST] wiimote_send_raw: l2cap_send failed status=0x%02X\n", status);
#endif
        } else {
#ifdef NS2_DIAG
            printf("[BTSTACK_HOST] wiimote_send_raw: sent %d bytes on INTR cid=0x%04X (0x%02X 0x%02X...)\n",
                   len, wiimote_conn.interrupt_cid, data[0], len > 1 ? data[1] : 0);
#endif
        }
        return status == ERROR_CODE_SUCCESS;
    }

    // Fallback to HID Host when using_hid_host but no direct CID (shouldn't happen normally)
    if (wiimote_conn.using_hid_host && wiimote_conn.hid_host_ready) {
        // Data format: first byte is 0xA2, second is report ID, rest is data
        if (len < 2) return false;
        uint8_t report_id = data[1];
        uint16_t payload_len = len - 2;
        // hid_host_send_report stores a pointer — copy to static buffer for async send
        static uint8_t wiimote_hid_report_buf[80];
        if (payload_len > sizeof(wiimote_hid_report_buf)) return false;
        if (payload_len > 0) memcpy(wiimote_hid_report_buf, &data[2], payload_len);
#ifdef NS2_DIAG
        printf("[BTSTACK_HOST] wiimote_send_raw via HID Host: cid=0x%04X report=0x%02X len=%d\n",
               wiimote_conn.hid_host_cid, report_id, payload_len);
#endif
        uint8_t status = hid_host_send_report(wiimote_conn.hid_host_cid, report_id, wiimote_hid_report_buf, payload_len);
        if (status == ERROR_CODE_SUCCESS) {
#ifdef NS2_DIAG
            printf("[BTSTACK_HOST] wiimote_send_raw: sent %d bytes via HID Host\n", len);
#endif
        } else {
#ifdef NS2_DIAG
            printf("[BTSTACK_HOST] wiimote_send_raw: HID Host send failed status=0x%02X\n", status);
#endif
        }
        return status == ERROR_CODE_SUCCESS;
    }

#ifdef NS2_DIAG
    printf("[BTSTACK_HOST] wiimote_send_raw: no interrupt CID and HID Host not ready\n");
#endif
    return false;
}

// Send raw L2CAP data to Wiimote on CONTROL channel
bool btstack_wiimote_send_control(uint8_t conn_index, const uint8_t* data, uint16_t len)
{
    UNUSED(conn_index);
#ifdef NS2_DIAG
    printf("[BTSTACK_HOST] wiimote_send_control: idx=%d len=%d control_cid=0x%04X using_hid_host=%d\n",
           conn_index, len, wiimote_conn.control_cid, wiimote_conn.using_hid_host);
#endif

    if (!wiimote_conn.active) {
#ifdef NS2_DIAG
        printf("[BTSTACK_HOST] wiimote_send_control: no active connection\n");
#endif
        return false;
    }
    if (len == 0 || len > 64) {
#ifdef NS2_DIAG
        printf("[BTSTACK_HOST] wiimote_send_control: bad len=%d\n", len);
#endif
        return false;
    }

    // Prefer direct L2CAP when we have the control CID (works even with HID Host)
    if (wiimote_conn.control_cid != 0) {
        if (!l2cap_can_send_packet_now(wiimote_conn.control_cid)) {
#ifdef NS2_DIAG
            printf("[BTSTACK_HOST] wiimote_send_control: L2CAP not ready to send\n");
#endif
            return false;
        }

        // Convert DATA format (0xA2) to SET_REPORT format (0x52) for control channel
        // Some Wii U Pro Controllers are strict and reject DATA transactions on control channel
        uint8_t send_buf[64];
        memcpy(send_buf, data, len);
        if (send_buf[0] == 0xA2) {
            send_buf[0] = 0x52;  // SET_REPORT | OUTPUT
        }

#ifdef NS2_DIAG
        printf("[BTSTACK_HOST] wiimote_send_control via L2CAP: cid=0x%04X len=%d hdr=0x%02X\n",
               wiimote_conn.control_cid, len, send_buf[0]);
#endif
        uint8_t status = l2cap_send(wiimote_conn.control_cid, send_buf, len);
        if (status != ERROR_CODE_SUCCESS) {
#ifdef NS2_DIAG
            printf("[BTSTACK_HOST] wiimote_send_control: l2cap_send failed status=0x%02X\n", status);
#endif
        }
        return status == ERROR_CODE_SUCCESS;
    }

    // Fallback to HID Host when using_hid_host but no direct CID
    if (wiimote_conn.using_hid_host && wiimote_conn.hid_host_ready) {
        // Data format: first byte is 0x52 (SET_REPORT), second is report type+ID
        if (len < 2) return false;
        uint8_t report_id = data[1];
        uint16_t payload_len = len - 2;
        // hid_host_send_set_report stores a pointer — copy to static buffer for async send
        static uint8_t wiimote_hid_setreport_buf[80];
        if (payload_len > sizeof(wiimote_hid_setreport_buf)) return false;
        if (payload_len > 0) memcpy(wiimote_hid_setreport_buf, &data[2], payload_len);
        uint8_t status = hid_host_send_set_report(wiimote_conn.hid_host_cid, HID_REPORT_TYPE_OUTPUT,
                                                   report_id, wiimote_hid_setreport_buf, payload_len);
        if (status == ERROR_CODE_SUCCESS) {
#ifdef NS2_DIAG
            printf("[BTSTACK_HOST] wiimote_send_control: sent %d bytes via HID Host\n", len);
#endif
        }
        return status == ERROR_CODE_SUCCESS;
    }

#ifdef NS2_DIAG
    printf("[BTSTACK_HOST] wiimote_send_control: no control CID and HID Host not ready\n");
#endif
    return false;
}

// Get connection info for bthid driver matching (Classic or BLE)
bool btstack_classic_get_connection(uint8_t conn_index, btstack_classic_conn_info_t* info)
{
    if (!info) return false;

    // Check if this is a BLE connection (conn_index >= BLE_CONN_INDEX_OFFSET)
    if (conn_index >= BLE_CONN_INDEX_OFFSET) {
        uint8_t ble_index = conn_index - BLE_CONN_INDEX_OFFSET;
        if (ble_index >= MAX_BLE_CONNECTIONS) return false;

        ble_connection_t* conn = &hid_state.connections[ble_index];
        if (conn->handle == HCI_CON_HANDLE_INVALID) return false;

        info->active = true;
        memcpy(info->bd_addr, conn->addr, 6);
        strncpy(info->name, conn->name, sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
        // BLE devices don't have class_of_device, set to zeros
        memset(info->class_of_device, 0, 3);
        // Use VID/PID from BLE manufacturer data (e.g., Switch 2)
        info->vendor_id = conn->vid;
        info->product_id = conn->pid;
        info->hid_ready = conn->hid_ready;
        info->is_ble = true;

        return true;
    }

    // Classic connection
    if (conn_index >= MAX_CLASSIC_CONNECTIONS) return false;

    classic_connection_t* conn = &classic_state.connections[conn_index];
    if (!conn->active) return false;

    info->active = conn->active;
    memcpy(info->bd_addr, conn->addr, 6);
    strncpy(info->name, conn->name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    memcpy(info->class_of_device, conn->class_of_device, 3);
    info->vendor_id = conn->vendor_id;
    info->product_id = conn->product_id;
    info->hid_ready = conn->hid_ready;
    info->is_ble = false;

    return true;
}

// Get number of active connections (Classic + BLE)
uint8_t btstack_classic_get_connection_count(void)
{
    uint8_t count = 0;
    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        if (classic_state.connections[i].active) {
            count++;
        }
    }
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (hid_state.connections[i].handle != HCI_CON_HANDLE_INVALID) {
            count++;
        }
    }
    return count;
}

// ============================================================================
// DISCONNECT ALL
// ============================================================================

void btstack_host_disconnect_all_devices(void)
{
    printf("[BTSTACK_HOST] Disconnecting all devices...\n");

    for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; i++) {
        classic_connection_t* c = &classic_state.connections[i];
        if (!c->active) continue;
        // Drop the underlying ACL link, not just the HID profile. Closing
        // only HID (hid_host_disconnect) leaves the ACL up and some pads
        // (notably the DS4) hold their "connected to host" state past the
        // HID drop -- lightbar stays solid blue and the controller never
        // sleeps. Disconnecting the ACL via gap_disconnect() forces the
        // pad into its post-disconnect state where idle-sleep kicks in.
        hci_connection_t* hci_conn = hci_connection_for_bd_addr_and_type(
            c->addr, BD_ADDR_TYPE_ACL);
        // hci_connection_for_bd_addr_and_type() already answered NULL for a
        // released link, so this branch is only reached with a live handle; the
        // helper is used for the counter and for symmetry with the BLE loop.
        if (hci_conn) {
            if (!btstack_host_request_disconnect(hci_conn->con_handle,
                                                 "disconnect-all Classic") &&
                c->hid_cid != 0 && c->hid_cid != 0xFFFF) {
                hid_host_disconnect(c->hid_cid);
            }
        } else if (c->hid_cid != 0 && c->hid_cid != 0xFFFF) {
            hid_host_disconnect(c->hid_cid);  // fallback
        }
    }
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        ble_connection_t *c = &hid_state.connections[i];
        if (c->handle == HCI_CON_HANDLE_INVALID) continue;
        // "Disconnect all" must actually end with no connections. Before
        // BTstack 1.8.2 a slot whose ACL had already gone was cleaned up by the
        // synthetic disconnect event; now it would survive this call and keep
        // occupying a controller slot for the rest of the boot.
        if (!btstack_host_request_disconnect(c->handle, "disconnect-all BLE"))
            ble_connection_release_orphan(c);
    }

    // Clear reconnection state so we don't try to reconnect to cleared devices
    hid_state.has_last_connected = false;
    hid_state.reconnect_attempts = 0;
}

// ============================================================================
// BOND MANAGEMENT
// ============================================================================

void btstack_host_delete_all_bonds(void)
{
    printf("[BTSTACK_HOST] Deleting all Bluetooth bonds...\n");

    // Close admission before touching databases so asynchronous disconnect/
    // connection-complete events cannot race the wipe and immediately re-admit a pad.
    pairing_lockout = true;
    pairing_close_deferred = false;
    scan_timeout_end = 0;
    classic_state.recovery_start_time = 0;
    classic_state.waiting_for_incoming_time = 0;
    classic_state.pending_valid = false;
    classic_pending_security_clear();
    hid_state.pending_fresh_pairing_admitted = false;
    switch2_explicit_fresh_pairing_admitted = false;
    if (hid_state.state == BLE_STATE_CONNECTING) gap_connect_cancel();
    btstack_host_stop_scan();

#if !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF) && !defined(CONFIG_USB2BLE)
    gap_discoverable_control(0);
    gap_connectable_control(0);
#endif

    // Wipe is global, including the management relationship, so terminate at
    // the HCI owner's complete connection list instead of relying only on the
    // controller slot tables below. Hardware validation on 2026-08-20 showed
    // that the slot-only sweep could leave a controller transport connected
    // even though input ownership and durable trust had already been removed.
    // hci_disconnect_all() also covers raw/in-flight Classic, BLE, and SCO
    // links which have not reached a HID-ready project slot. The lock and radio
    // admission controls above are established first, so completion events or
    // a new page cannot turn this teardown into an automatic re-admission.
    hci_disconnect_all();

#if !defined(BTSTACK_USE_CYW43) && !defined(BTSTACK_USE_ESP32) && !defined(BTSTACK_USE_NRF)
    // Erase BTstack flash banks to force clean re-initialization
    // This is more reliable than using BTstack's delete APIs when flash was corrupted
    btstack_erase_flash_banks();

    // Re-initialize the TLV context to pick up the erased banks
    const hal_flash_bank_t *flash_bank = pico_flash_bank_instance();
    btstack_tlv_flash_bank_init_instance(&btstack_tlv_flash_bank_context,
                                          flash_bank, NULL);
    printf("[BTSTACK_HOST] TLV re-initialized with clean flash banks\n");
#else
    // For CYW43/ESP32, use BTstack's standard APIs
    gap_delete_all_link_keys();
    printf("[BTSTACK_HOST] Classic BT link keys deleted\n");

    int ble_count = le_device_db_count();
    // le_device_db_init() is a no-op for the TLV backend used by CYW43. Walk
    // every slot explicitly because the backend permits holes, and use GAP's
    // public deletion API so its resolving-list side effects are retained.
    for (int i = le_device_db_max_count() - 1; i >= 0; --i) {
        int type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t addr;
        le_device_db_info(i, &type, addr, NULL);
        if (type != BD_ADDR_TYPE_UNKNOWN)
            gap_delete_bonding((bd_addr_type_t)type, addr);
    }
    printf("[BTSTACK_HOST] BLE bonds deleted (was %d devices, now %d)\n",
           ble_count, le_device_db_count());
#endif

    btstack_host_clear_last_connected();
    // Store after a flash-bank erase/re-init so the post-wipe lock survives reboot.
    btstack_host_store_pairing_lockout(true);

    // The shared LE DB also owns the management peripheral bond. Wipe-all is
    // intentionally global, so terminate that link without touching unrelated
    // controller/management lifecycles during ordinary disconnects.
    if (config_ble.handle != HCI_CON_HANDLE_INVALID) {
        hci_con_handle_t mgmt = config_ble.handle;
        if (!btstack_host_request_disconnect(mgmt, "management session (wipe)"))
            config_ble_handle_disconnect(
                mgmt, ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
    }

    wipe_completions++;
    printf("[BTSTACK_HOST] All bonds cleared. Devices will need to re-pair.\n");
}

bool btstack_host_get_last_connected(uint8_t bd_addr_out[6], char name_out[48])
{
    if (!hid_state.has_last_connected) return false;
    bool nonzero = false;
    for (int i = 0; i < 6; i++) if (hid_state.last_connected_addr[i]) { nonzero = true; break; }
    if (!nonzero) return false;
    memcpy(bd_addr_out, hid_state.last_connected_addr, 6);
    strncpy(name_out, hid_state.last_connected_name, 47);
    name_out[47] = '\0';
    return true;
}

static bool btstack_host_forget_device_typed(const uint8_t bd_addr[6],
                                              int address_type,
                                              bool match_address_type)
{
    if (!hid_state.initialized || !bd_addr) return false;

    bd_addr_t addr;
    memcpy(addr, bd_addr, 6);
    bool affected = false;

    printf("[BTSTACK_HOST] Forgetting device %02X:%02X:%02X:%02X:%02X:%02X\n",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    // Cancel an in-flight connect to this identity before mutating trust.
    if (hid_state.state == BLE_STATE_CONNECTING &&
        memcmp(hid_state.pending_addr, addr, sizeof(addr)) == 0 &&
        (!match_address_type ||
         hid_state.pending_addr_type == (bd_addr_type_t)address_type)) {
        gap_connect_cancel();
        hid_state.pending_fresh_pairing_admitted = false;
        affected = true;
    }

    // Disconnect controller-role BLE links for this identity.
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        ble_connection_t *c = &hid_state.connections[i];
        if (c->handle != HCI_CON_HANDLE_INVALID &&
            memcmp(c->addr, addr, 6) == 0 &&
            (!match_address_type ||
             c->addr_type == (bd_addr_type_t)address_type)) {
            // A forgotten peer must not keep a slot. Same reasoning as
            // btstack_host_disconnect_all_devices().
            if (!btstack_host_request_disconnect(c->handle, "forget BLE link"))
                ble_connection_release_orphan(c);
            affected = true;
        }
    }

    // The management peripheral shares the LE DB but not the controller table.
    //
    // `addr` is the identity the caller asked to forget, so the match is on the
    // companion's durable identity. Comparing the live RPA meant an explicit
    // forget of the companion's bond left its session running.
    if (config_ble.handle != HCI_CON_HANDLE_INVALID &&
        config_ble_addr_is_companion(addr)) {
        hci_con_handle_t mgmt = config_ble.handle;
        if (!btstack_host_request_disconnect(mgmt, "management session (forget)"))
            config_ble_handle_disconnect(
                mgmt, ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
        affected = true;
    }

    // A typed LE management removal MUST NOT disturb a same-address Classic
    // relationship. The public address-only helper intentionally covers both.
    if (ns2_bt_forget_matches_address_type(
            match_address_type, address_type, BD_ADDR_TYPE_ACL)) {
        for (int i = 0; i < MAX_CLASSIC_CONNECTIONS; ++i) {
            classic_connection_t *conn = &classic_state.connections[i];
            if (!conn->active || memcmp(conn->addr, addr, sizeof(addr)) != 0)
                continue;
            hci_connection_t *hci_conn = hci_connection_for_bd_addr_and_type(
                conn->addr, BD_ADDR_TYPE_ACL);
            if (hci_conn)
                (void)btstack_host_request_disconnect(hci_conn->con_handle,
                                                      "forget Classic link");
            affected = true;
        }
    }

    // Search the full capacity: count() is not a slot bound when the TLV DB is
    // sparse. Use GAP deletion to refresh the controller resolving list.
    int bond_slot = ns2_bt_find_bond_slot(
        btstack_host_le_bond_entry_at, NULL, le_device_db_max_count(), addr,
        address_type, match_address_type);
    if (bond_slot >= 0) {
        int stored_type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t stored_addr;
        le_device_db_info(bond_slot, &stored_type, stored_addr, NULL);
        if (stored_type != BD_ADDR_TYPE_UNKNOWN) {
            gap_delete_bonding((bd_addr_type_t)stored_type, stored_addr);
            printf("[BTSTACK_HOST] Removed BLE bond at index %d\n", bond_slot);
            affected = true;
        }
    }

    // Remove Classic link key only for the address-only cross-transport helper.
    if (ns2_bt_forget_matches_address_type(
            match_address_type, address_type, BD_ADDR_TYPE_ACL)) {
#ifdef ENABLE_CLASSIC
        gap_drop_link_key_for_bd_addr(addr);
#endif
    }

    // Clear last-connected if it matches
    if (hid_state.has_last_connected &&
        memcmp(hid_state.last_connected_addr, addr, 6) == 0 &&
        (!match_address_type ||
         hid_state.last_connected_addr_type ==
             (bd_addr_type_t)address_type)) {
        btstack_host_clear_last_connected();
        affected = true;
    }
    return affected;
}

void btstack_host_forget_device(const uint8_t bd_addr[6])
{
    (void)btstack_host_forget_device_typed(
        bd_addr, BD_ADDR_TYPE_UNKNOWN, false);
}
