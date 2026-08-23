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
#include "ns2_bt_lifecycle.h"  // ns2_bt_auth_observation_t

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

// Management first-bond admission follows the same explicit BOOTSEL pairing
// window as controller discovery. The transport sets this edge when pairing
// mode opens/closes; bonded management reconnects do not require it.
void btstack_host_set_pairing_window_open(bool open);
bool btstack_host_pairing_window_open(void);

// Start scanning with a timeout (auto-stops after timeout_ms)
void btstack_host_start_timed_scan(uint32_t timeout_ms);

// Suppress/unsuppress automatic scan restart (e.g. when USB device connected).
// Explicit start_timed_scan clears suppression.
void btstack_host_suppress_scan(bool suppress);

// Address and trust-lookup outcome of the most recent admission rejection.
// The reject COUNTERS are anonymous, which left a real field question
// unanswerable: `reject_window` reached 11 with no way to tell whether the
// rejected peer was the phone whose Controller Link was failing or an unrelated
// Classic device. Returns false until a rejection has occurred.
bool btstack_host_last_reject(uint8_t *addr_out, bool *trust_present);

// Deciding inputs of the most recent Controller Link refusal.
//
// `clink.refused_no_mgmt` is a count, and the predicate behind it is a
// four-term conjunction. On 2026-08-22 that counter reached 18 against a peer
// the bond database and `btreject` both identified as the companion, while the
// phone's own log showed management connected and identity-verified seconds
// earlier -- unanswerable from counters. These are the terms themselves.
typedef struct {
    uint8_t addr[6];
    bool cross_transport;
    bool mgmt_connected;
    bool mgmt_addr_known;
    bool mgmt_raw_matches;
    bool mgmt_identity_known;
    bool mgmt_identity_matches;
    bool mgmt_link_trusted;
} btstack_host_link_refusal_t;

bool btstack_host_last_link_refusal(btstack_host_link_refusal_t *out);

// Deciding inputs of the most recent Classic Authentication Complete. Exists
// because enc.deferrals staying zero while security succeeds is ambiguous on
// its own: it cannot distinguish "the hook never ran" from "one predicate was
// false", and guessing between those costs a flash cycle.
typedef struct {
    uint16_t handle;
    bool mgmt_connected;
    bool mgmt_addr_known;
    bool mgmt_addr_matches;
    bool mgmt_link_trusted;
    bool we_own_fresh_pairing;
    bool request_was_pending;
    bool stood_down;
    bool auth_deferred;
    bool auth_had_stored_key;
    // Tri-state: NOT_OBSERVED is the expected value on the peer-led path,
    // where this host deliberately never sends HCI_Authentication_Requested
    // and so never receives the completion event. Never read it as failure.
    ns2_bt_auth_observation_t auth_outcome;
    bool encrypted_ok;
    uint8_t encryption_key_size;  // valid only when key_size_valid
    bool key_size_valid;          // sampled at the HID-ready acceptance gate
    bool hid_ready;
    // The ACL this record describes has disconnected. Every field above is a
    // post-mortem of that link and none of them will be updated again.
    bool link_closed;
} btstack_host_auth_decision_t;

// Count of companion links where this host did NOT initiate authentication.
uint32_t btstack_host_authentication_deferrals(void);

bool btstack_host_last_auth_decision(btstack_host_auth_decision_t *out);



// True when a controller is fully connected (HID ready) on either transport.
// Used for the 1-dongle-1-controller discovery gating.
bool btstack_host_controller_connected(void);

// Idle background scan/inquiry while a controller is connected (1 dongle : 1
// controller). Call periodically, outside an open pairing window. Discovery
// resumes automatically at 0 connections.
void btstack_host_idle_scan_if_connected(void);

// Keep discovery running while the selected source is still missing a peer, even
// though another peer is already connected. Counterpart to
// btstack_host_idle_scan_if_connected(); never disturbs an in-flight connect.
void btstack_host_scan_for_additional_peer(void);

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
    // Multi-peer reconnect: every counter above is scoped to the single
    // `last_connected` target, so a second bonded peer is invisible to them.
    // These distinguish "the other bond is gone" from "it is advertising but
    // nothing targets it" from "its address rotates so no raw match is
    // possible".
    uint32_t bonded_advertising_reports;     // sighting matched ANY stored bond
    uint32_t nontarget_advertising_reports;  // ...a bond that is not the target
    uint32_t rpa_advertising_reports;        // unmatchable resolvable private addr
    uint8_t bond_count;                      // stored LE bonds
    uint8_t bond_capacity;                   // MAX_NR_LE_DEVICE_DB_ENTRIES
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
    char last_connected_name[48];
} btstack_host_reconnect_diag_t;

void btstack_host_get_reconnect_diag(btstack_host_reconnect_diag_t *out);

// In-band management / BLE coexistence diagnostics (UART-only). A lifecycle event
// ring + live snapshot + scan-suppression cause counters, added to isolate the
// "controller and management both drop and cannot recover without a power cycle"
// failure over the GP0/GP1 UART link (last-good/first-fail visibility the Web
// Portal cannot give once dropped). All read-only; recording never alters BT
// behavior. See docs/bluetooth/in-band-management-plan.md.
typedef struct {
    uint32_t t_ms;   // ms since boot
    uint8_t  code;   // btstack_host_life_code_name()
    uint8_t  a;      // scan-suppress cause (btstack_host_life_cause_name()) or HCI reason
    uint16_t b;      // connection handle (0 when not applicable)
} btstack_host_life_record_t;

typedef struct {
    // Feature / personality
    bool mgmt_enabled;
    bool config_mode;
    uint8_t personality;              // usb_personality_t
    // Radio / host state
    bool powered_on;
    uint8_t hid_state;                // internal BLE state machine (diagnostic only)
    bool scan_active;
    bool inquiry_active;
    bool wake_adv_active;
    bool controller_connected;
    uint8_t connected_classic_count;
    uint8_t ready_classic_count;
    uint8_t connected_ble_count;
    uint8_t ready_ble_count;
    bool pairing_window_open;
    bool pairing_close_deferred;
    bool pairing_lockout;
    // config/management BLE service
    bool cble_service_available;
    bool cble_mode_active;            // service armed (Config OR mgmt)
    bool cble_advertising;
    bool cble_has_client;
    bool cble_closing;
    bool cble_notifications;
    bool cble_fresh_bond_admitted;
    // Ring + counters
    uint16_t event_count;
    uint32_t event_dropped;
    uint32_t scan_starts;
    uint32_t scan_stops;
    uint32_t adv_starts;
    uint32_t adv_stops;
    uint32_t suppress_config_mode;    // scan restart refused: in CDC Config
    uint32_t suppress_mgmt_armed;     // scan restart refused: in-band mgmt armed (the coexistence bug)
    uint32_t suppress_wake;
    uint32_t suppress_other;          // lockout + app-suppress + not-powered + already
    uint32_t mgmt_connects;
    uint32_t mgmt_disconnects;
    uint32_t ctrl_disconnects;
    uint32_t hci_disconnects;
    uint32_t hci_state_losses;
    uint8_t hci_state;
    uint8_t hci_health_phase;
    uint16_t hci_probe_handle;
    uint32_t hci_last_event_age_ms;
    uint32_t hci_probes_sent;
    uint32_t hci_probes_ok;
    uint32_t hci_probes_failed;
    uint32_t hci_probe_timeouts;
    uint32_t hci_recovery_attempts;
    uint32_t hci_recovery_completions;
    uint32_t fresh_admission_accepts;
    uint32_t fresh_admission_reject_window;
    uint32_t fresh_admission_reject_lockout;
    // Times this host stood down from BTstack's automatic Classic encryption
    // request so it would not race the companion. Non-zero on the next flashed
    // build is what confirms the Type C mechanism on hardware.
    uint32_t classic_encryption_deferrals;
    uint32_t classic_encryption_peer_completed;
    uint32_t classic_encryption_collisions;
    uint32_t classic_encryption_unencrypted_active;
    // Stand-downs and collisions on the AUTHENTICATION procedure. Counted
    // separately from the encryption ones: the captured race happens on
    // authentication first, and a run that only watches encryption can
    // report success while it is still occurring.
    uint32_t classic_authentication_deferrals;
    uint32_t classic_authentication_collisions;
    // Controller Link lifecycle. See
    // ns2_bt_companion_classic_admission_allowed(): the companion's Classic
    // link exists only for the duration of its management session.
    uint16_t classic_companion_handle;          // live Controller Link ACL
    uint32_t classic_companion_refused_no_mgmt; // refused: management not live
    uint32_t classic_companion_mgmt_teardowns;  // torn down on management loss
    uint32_t wipe_completions;
    uint16_t last_disc_handle;
    uint8_t last_disc_reason;
    uint8_t owner_led_reason;
    bool owner_led_output_on;
    uint32_t owner_led_last_transition_ms;
    uint32_t owner_led_timer_max_gap_ms;
} btstack_host_mgmt_diag_t;

void btstack_host_get_mgmt_diag(btstack_host_mgmt_diag_t *out);

// One entry of the bond inventory snapshot published by core 1 (see
// bond_snapshot_refresh). Safe to call from core 0; false when `index` is past
// the current bond count. Lets a controlled reconnect test record which bonds
// exist BEFORE a peer is power-cycled, so bond survival is observed rather than
// inferred from the adapter's connected state.
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
} btstack_host_bond_entry_t;

bool btstack_host_bond_snapshot_get(uint8_t index, btstack_host_bond_entry_t *out);
// Read lifecycle event at logical index (0 = oldest). False when index >= count.
bool btstack_host_life_get(uint16_t index, btstack_host_life_record_t *out);
void btstack_host_life_clear(void);
const char *btstack_host_life_code_name(uint8_t code);
const char *btstack_host_life_cause_name(uint8_t cause);

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

// UART-only, off-by-default Pro Controller 2 raw-magnetometer probe. Requests
// the separate feature-0x80/handle-0x000A layout used by PC-side tools, then
// restores the validated 0x27 native-motion profile. Requests are marshaled
// onto the BTstack thread; production startup behavior is unchanged.
typedef struct {
    bool requested;
    bool active;
    bool transition_pending;
    uint8_t v2_state;
    uint8_t reference_step;
    uint8_t reference_steps;
    uint8_t reference_result;
    uint8_t last_response_status;
    uint8_t input_ccc_status;
    uint16_t source_pid;
    uint16_t connection_handle;
} btstack_host_magraw_diag_t;

void btstack_host_request_switch2_magraw(bool enabled);
void btstack_host_get_switch2_magraw_diag(btstack_host_magraw_diag_t *out);

// UART-only, off-by-default raw-IMU reference selector. It replays the exact
// feature-0x2F/calibration/report-rate/0x000A CCC shape recovered from the
// decrypted genuine Pro2 PCAP, then restores the validated native profile.
typedef struct {
    bool requested;
    bool active;
    bool transition_pending;
    bool dual_requested;
    bool dual_active;
    bool dual_transition_pending;
    uint8_t v2_state;
    uint8_t last_att_status;
    uint8_t dual_att_status;
    uint8_t interval_request_status;
    uint16_t interval_target_units;
    uint16_t interval_actual_units;
    uint32_t common_notifications;
    uint32_t native_notifications;
    uint16_t source_pid;
    uint16_t connection_handle;
} btstack_host_imuref_diag_t;

void btstack_host_request_switch2_imuref(bool enabled);
void btstack_host_request_switch2_imuref_dual(bool enabled);
void btstack_host_request_switch2_imuref_interval(uint16_t interval_units);
void btstack_host_get_switch2_imuref_diag(btstack_host_imuref_diag_t *out);

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
bool btstack_classic_get_feature_report(uint8_t conn_index, uint8_t report_id);

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

// Management-app LE bond list/remove. A core0 caller marshals a one-shot op onto
// the BTstack thread (core1) and polls btstack_host_bonds_done(); the LE device
// DB is not touched from core0. is_remove=false enumerates a complete,
// backward-compatible v2 envelope (result in btstack_host_bonds_list_json());
// is_remove=true removes the LE bond at index (result in
// btstack_host_bonds_remove_ok()). Returns false if an op is pending. A legacy
// list that cannot fit is reported incomplete and must be answered with the
// compact response_too_large error rather than a partial array.
bool btstack_host_bonds_request(bool is_remove, int remove_index);
// Request one explicitly versioned page beginning at a le_device_db slot
// cursor. The result has a "next" cursor, or null when complete.
bool btstack_host_bonds_request_list_page(int start_index);
bool btstack_host_bonds_done(void);
const char *btstack_host_bonds_list_json(void);
bool btstack_host_bonds_list_complete(void);
bool btstack_host_bonds_remove_ok(void);

#ifdef __cplusplus
}
#endif

#endif // BTSTACK_HOST_H
