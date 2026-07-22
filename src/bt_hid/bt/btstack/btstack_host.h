// btstack_host.h - BTstack HID Host (BLE + Classic)
//
// Transport-agnostic BTstack integration for HID devices.
// Supports both BLE HID over GATT and Classic BT HID.
//
// Usage:
//   btstack_host_init(hci_transport);  // Pass HCI transport (USB dongle or CYW43)
//   btstack_host_process();            // Call from main loop

#ifndef BTSTACK_HOST_H
#define BTSTACK_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include "bluetooth.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// INITIALIZATION
// ============================================================================

// Initialize BTstack with specified HCI transport
// For USB dongle: pass hci_transport_h2_tinyusb_instance()
// For Pico W: pass hci_transport_cyw43_instance()
// Note: transport is actually hci_transport_t* but we use void* to avoid header conflicts
void btstack_host_init(const void* transport);

// Initialize only the HID handlers (callbacks, state) without BTstack init
// Use this when BTstack was already initialized externally (e.g., btstack_cyw43_init)
void btstack_host_init_hid_handlers(void);

// Power on the Bluetooth controller
void btstack_host_power_on(void);

// ============================================================================
// SCANNING / PAIRING
// ============================================================================

// Start scanning for BLE and Classic BT devices
void btstack_host_start_scan(void);

// Clear the persistent admission lock installed by a full pairing wipe. Call
// only for an explicit user-opened pairing window.
void btstack_host_clear_pairing_lockout(void);

// True after a full wipe and until an explicit pairing window is opened.
bool btstack_host_pairing_locked(void);

// Stop scanning
void btstack_host_stop_scan(void);

// Close an explicit pairing-discovery window. Unlike btstack_host_stop_scan(),
// this is safe to call at any time: if a BLE connect attempt is genuinely in
// flight (gap_connect() outstanding), the close is deferred until that attempt
// resolves (success or failure) instead of corrupting its watchdog state. Use
// this for user-facing "pairing window expired" closure; use stop_scan()
// directly for anything that should close immediately regardless (app-level
// scan suppression, timed-scan expiry). See btstack_host.c for the trace that
// motivated this split.
void btstack_host_close_pairing_window(void);

// True while a pairing-window close is deferred, waiting for an in-flight
// connect attempt to resolve. For UI (e.g. keep the pairing LED blinking
// through the grace period instead of going idle prematurely).
bool btstack_host_pairing_close_deferred(void);

// Start scanning with a timeout (auto-stops after timeout_ms)
void btstack_host_start_timed_scan(uint32_t timeout_ms);

// Suppress/unsuppress automatic scan restart (e.g. when USB device connected).
// Explicit start_timed_scan clears suppression.
void btstack_host_suppress_scan(bool suppress);

// True when a controller is fully connected (HID ready) on either transport.
// Used for the 1-dongle-1-controller discovery gating.
bool btstack_host_controller_connected(void);

// Idle background scan/inquiry while a controller is connected (1 dongle : 1
// controller). Call periodically, outside an open pairing window. Discovery
// resumes automatically at 0 connections.
void btstack_host_idle_scan_if_connected(void);

// Temporarily pause discovery and emit one or two non-connectable legacy BLE
// advertisements from an exact random address. Used for Switch 2 wake replay;
// the sequence is timer-driven and never blocks the BTstack run loop. Returns
// false if the radio is not ready or a connection/wake operation is in flight.
#define BTSTACK_HOST_WAKE_ADV_LEN 31
#define BTSTACK_HOST_WAKE_MAX_PAYLOADS 2
bool btstack_host_start_wake_advertisement(
    const uint8_t advertiser_addr[6],
    const uint8_t advertisements[][BTSTACK_HOST_WAKE_ADV_LEN],
    uint8_t advertisement_count);
bool btstack_host_wake_advertisement_active(void);

// Connect to a BLE device
void btstack_host_connect_ble(bd_addr_t addr, bd_addr_type_t addr_type);

// ============================================================================
// CALLBACKS
// ============================================================================

typedef void (*btstack_host_report_callback_t)(uint16_t handle, const uint8_t *report, uint16_t len);
typedef void (*btstack_host_connect_callback_t)(uint16_t handle, bool connected);

void btstack_host_register_report_callback(btstack_host_report_callback_t callback);
void btstack_host_register_connect_callback(btstack_host_connect_callback_t callback);

// ============================================================================
// MAIN LOOP
// ============================================================================

// Process BTstack events - call from main loop
void btstack_host_process(void);

// ============================================================================
// STATUS
// ============================================================================

bool btstack_host_is_initialized(void);
bool btstack_host_is_powered_on(void);
bool btstack_host_is_scanning(void);

// Read-only bonded-reconnect state for the out-of-band UART diagnostics.
// `state` is the host's internal BLE state-machine value and is intentionally
// diagnostic-only; callers must not use it to drive behavior.
typedef struct {
    bool powered_on;
    bool scan_active;
    bool has_last_connected;
    bool has_last_connected_ltk;
    bool force_fresh_custom_pairing;
    bool connect_attempt_active;
    uint8_t state;
    uint8_t reconnect_attempts;
    uint8_t connected_ble_count;
    uint8_t local_addr[6];
    uint8_t last_connected_addr[6];
    uint8_t last_connected_addr_type;
    uint8_t last_connected_ble_strategy;
    uint16_t last_connected_vid;
    uint16_t last_connected_pid;
    uint32_t advertising_reports;
    uint32_t target_advertising_reports;
    uint32_t switch2_advertising_reports;
    uint32_t target_connect_attempts;
    uint32_t target_connect_successes;
    uint32_t target_connect_failures;
    uint32_t reencryption_started;
    uint32_t reencryption_successes;
    uint32_t reencryption_failures;
    uint32_t pairing_ltk_reads;
    uint32_t direct_reencrypt_elapsed_ms;
    uint32_t direct_cmd_status_events;
    uint32_t direct_cmd_complete_events;
    uint32_t direct_encrypt_events;
    uint32_t switch2_disconnect_events;
    uint16_t direct_reencrypt_handle;
    uint16_t last_direct_cmd_status_opcode;
    uint16_t last_direct_cmd_complete_opcode;
    uint8_t last_target_advertising_event_type;
    uint8_t last_target_connect_status;
    uint8_t last_reencryption_status;
    uint8_t pairing_ltk_phase;
    bool pairing_ltk_valid;
    bool pairing_ltk_matches_derived;
    bool pairing_ltk_raw_matches_derived;
    bool direct_reencrypt_active;
    bool hci_command_ready;
    uint8_t direct_link_key_size;
    uint8_t direct_encrypt_phase;
    uint8_t last_direct_cmd_status;
    uint8_t last_direct_cmd_complete_status;
    uint8_t last_direct_encrypt_status;
    uint8_t last_direct_encrypt_enabled;
    uint8_t last_switch2_disconnect_reason;
    uint8_t pairing_ltk_raw[16];
    uint8_t pairing_ltk_normalized[16];
    char last_connected_name[48];
} btstack_host_reconnect_diag_t;

void btstack_host_get_reconnect_diag(btstack_host_reconnect_diag_t *out);

// UART diagnostic control: discard only the saved target's BTstack LE bond,
// disconnect its current link, and arm Nintendo custom ATT pairing. The durable
// target identity is retained so the next SYNC advertisement is recognized.
void btstack_host_force_switch2_fresh_pairing(void);

// Off-by-default genuine Pro Controller 2 headset transport experiment. This
// switches the controller from its ordinary 0x000E native report to the
// firmware-2.x 0x002E extended input/audio report while preserving buttons and
// native motion. The replay command transmits a captured genuine haptic burst;
// the separately gated live path encodes console PCM onto stream 0x04.
typedef struct {
    bool requested;
    bool active;
    uint8_t state;
    uint8_t last_att_status;
    uint8_t last_headset_raw;
    uint8_t last_audio_length;
    uint16_t source_pid;
    uint16_t connection_handle;
    uint16_t last_report_length;
    uint16_t max_report_length;
    uint32_t notifications;
    uint32_t compact_failures;
    bool replay_requested;
    bool replay_active;
    uint8_t replay_state;
    uint8_t replay_last_send_status;
    uint16_t replay_frames_sent;
    bool live_requested;
    bool live_active;
    uint8_t live_state;
    uint8_t live_last_send_status;
    uint8_t live_last_toc;
    uint8_t live_prefix[6];
    uint8_t live_prime_count;
    uint32_t live_frames_sent;
    uint32_t live_underruns;
} btstack_host_pro2_audio_diag_t;

void btstack_host_set_switch2_pro2_audio_capture(bool enabled);
void btstack_host_get_switch2_pro2_audio_diag(btstack_host_pro2_audio_diag_t *out);
void btstack_host_request_switch2_pro2_audio_replay(bool enabled);
void btstack_host_request_switch2_pro2_audio_live(bool enabled);
// Service the diagnostic replay from the existing 2 ms audio timer. Its wire
// format requires a 5 ms stream offset and 20 ms long-term packet cadence, so
// the ordinary 30 ms Bluetooth control tick cannot drive it correctly.
void btstack_host_service_switch2_pro2_audio_replay(void);
void btstack_host_service_switch2_pro2_audio_live(void);

// ============================================================================
// CLASSIC BT CONNECTION INFO (for bthid driver matching)
// ============================================================================

typedef struct {
    bool active;
    uint8_t bd_addr[6];
    char name[48];
    uint8_t class_of_device[3];
    uint16_t vendor_id;
    uint16_t product_id;
    bool hid_ready;
    bool is_ble;
} btstack_classic_conn_info_t;

bool btstack_classic_get_connection(uint8_t conn_index, btstack_classic_conn_info_t* info);
uint8_t btstack_classic_get_connection_count(void);

// Get last-connected bonded device (returns false if none stored)
bool btstack_host_get_last_connected(uint8_t bd_addr_out[6], char name_out[48]);

// Classic BT output (for bthid drivers)
bool btstack_classic_send_set_report_type(uint8_t conn_index, uint8_t report_type,
                                           uint8_t report_id, const uint8_t* data, uint16_t len);
bool btstack_classic_send_set_report(uint8_t conn_index, uint8_t report_id,
                                      const uint8_t* data, uint16_t len);
bool btstack_classic_send_report(uint8_t conn_index, uint8_t report_id,
                                  const uint8_t* data, uint16_t len);

// Wiimote raw L2CAP output (Wiimotes use raw protocol, not HID transport headers)
bool btstack_wiimote_is_connection(uint8_t conn_index);
bool btstack_wiimote_can_send(uint8_t conn_index);
bool btstack_wiimote_send_raw(uint8_t conn_index, const uint8_t* data, uint16_t len);
bool btstack_wiimote_send_control(uint8_t conn_index, const uint8_t* data, uint16_t len);

// ============================================================================
// MOUTHPAD NUS CLIENT (Augmental MouthPad relay)
// ============================================================================

// Register a callback for device->host NUS bytes (fires in BTstack context).
void btstack_host_set_mouthpad_nus_rx_cb(void (*cb)(const uint8_t* data, uint16_t len));

// Write host->device NUS bytes. Returns false if no MouthPad NUS is ready.
// Must be called from BTstack/run-loop context (e.g. the bridge task).
bool btstack_host_mouthpad_nus_send(const uint8_t* data, uint16_t len);

// True once NUS service discovery has completed for a connected MouthPad.
bool btstack_host_mouthpad_nus_ready(void);

// Connected MouthPad device info, for the dongle-level relay responses
// (device_info_response / ble_connection_status_response).
typedef struct {
    char     name[48];
    char     firmware[24];  // DIS firmware revision ("" if unknown)
    uint8_t  addr[6];
    uint16_t vid;
    uint16_t pid;
    uint8_t  battery;   // last BAS level (0 = unknown)
    bool     ready;     // NUS service ready (vs still connecting)
} btstack_host_mouthpad_info_t;

// Fills `out` with the connected MouthPad's info; false if none connected.
bool btstack_host_get_mouthpad_info(btstack_host_mouthpad_info_t* out);

// Forget the connected MouthPad's bond (the relay clear_bonds command). The
// removal is marshaled onto the BTstack thread; returns true if a connected
// MouthPad was captured for removal.
bool btstack_host_mouthpad_clear_bond(void);

// ============================================================================
// BOND MANAGEMENT
// ============================================================================

// Disconnect all active BT devices (Classic and BLE)
void btstack_host_disconnect_all_devices(void);

// Delete all stored BT bonds (Classic and BLE)
// Devices will need to re-pair after this
void btstack_host_delete_all_bonds(void);

// Forget a specific device by address — disconnects if connected, removes bond
void btstack_host_forget_device(const uint8_t bd_addr[6]);

#ifdef __cplusplus
}
#endif

#endif // BTSTACK_HOST_H
