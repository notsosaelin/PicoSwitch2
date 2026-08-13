// bthid.h - Bluetooth HID Layer
// Handles Bluetooth HID devices and routes reports to device-specific drivers

#ifndef BTHID_H
#define BTHID_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define BTHID_MAX_DEVICES       4   // Max simultaneous BT HID devices
#define BTHID_MAX_NAME_LEN      48  // Max device name length

// ============================================================================
// HID REPORT TYPES (Bluetooth HID spec)
// ============================================================================

#define BTHID_REPORT_TYPE_INPUT     0x01
#define BTHID_REPORT_TYPE_OUTPUT    0x02
#define BTHID_REPORT_TYPE_FEATURE   0x03

// HID Transaction header types (high nibble)
#define BTHID_TRANS_HANDSHAKE       0x00
#define BTHID_TRANS_HID_CONTROL     0x10
#define BTHID_TRANS_GET_REPORT      0x40
#define BTHID_TRANS_SET_REPORT      0x50
#define BTHID_TRANS_GET_PROTOCOL    0x60
#define BTHID_TRANS_SET_PROTOCOL    0x70
#define BTHID_TRANS_DATA            0xA0

// Handshake result codes
#define BTHID_HANDSHAKE_SUCCESS     0x00
#define BTHID_HANDSHAKE_NOT_READY   0x01
#define BTHID_HANDSHAKE_ERR_INVALID 0x02
#define BTHID_HANDSHAKE_ERR_UNSUPPORTED 0x03
#define BTHID_HANDSHAKE_ERR_INVALID_PARAM 0x04
#define BTHID_HANDSHAKE_ERR_UNKNOWN 0x0E
#define BTHID_HANDSHAKE_ERR_FATAL   0x0F

// Protocol modes
#define BTHID_PROTOCOL_BOOT         0x00
#define BTHID_PROTOCOL_REPORT       0x01

// ============================================================================
// DEVICE TYPES (based on Class of Device)
// ============================================================================

typedef enum {
    BTHID_DEVICE_UNKNOWN = 0,
    BTHID_DEVICE_KEYBOARD,
    BTHID_DEVICE_MOUSE,
    BTHID_DEVICE_GAMEPAD,
    BTHID_DEVICE_JOYSTICK,
} bthid_device_type_t;

// ============================================================================
// DEVICE STATE
// ============================================================================

typedef struct {
    bool active;                        // Device slot in use
    uint8_t conn_index;                 // Transport connection index
    uint8_t bd_addr[6];                 // Device address
    char name[BTHID_MAX_NAME_LEN];      // Device name
    bthid_device_type_t type;           // Device type
    uint8_t player_index;               // Assigned player slot (-1 if none)
    uint16_t vendor_id;                 // USB VID (from SDP/manufacturer data)
    uint16_t product_id;                // USB PID (from SDP/manufacturer data)

    bool is_ble;                        // True if BLE (not Classic BT)

    // Monotonic HID lifecycle token.  BTstack reuses connection indices after
    // disconnect; the NS2 input arbiter includes this token in its source key
    // so a stale report/disconnect cannot target a newer occupant of the slot.
    uint32_t connection_generation;

    // Device driver info
    const void* driver;                 // Pointer to device driver interface
    void* driver_data;                  // Driver-specific data
} bthid_device_t;

// ============================================================================
// DEVICE DRIVER INTERFACE
// ============================================================================

// Which physical transport(s) a driver's real hardware family can ever use.
// Found 2026-07-12 during the Gate 2 reachability audit: switch_pro_bt.c (Switch 1,
// Classic-BT-only hardware) had a name-based fallback ("Pro Controller" substring)
// with no transport check, registered *before* switch2_ble.c — a BLE-connecting
// Switch 2 Pro Controller (whose advertised name plausibly also contains "Pro
// Controller") could be incorrectly claimed by the Switch 1 driver before
// switch2_ble_match() ever ran. Worse, bthid_update_device_info()'s re-evaluation
// logic (which re-checks a driver's own match() once accurate VID/PID arrives async)
// would NOT catch this specific case, because switch_match() still returns true via
// the name path even when called again with the correct Switch-2 PID — the bug
// survives its own self-healing mechanism.
//
// Rather than patch that one driver with an ad-hoc `if (is_ble) return false;` (which
// a future driver author could easily forget to replicate), every driver now declares
// which transport(s) its real hardware can use, checked centrally in find_driver() and
// the re-eval loop in bthid_update_device_info() *before* match() is ever called. This
// closes the whole bug class at once, for present and future drivers. Default (0, i.e.
// a driver that doesn't set this field) is BTHID_TRANSPORT_BOTH — deliberately
// fail-open to "no new restriction" rather than fail-closed to "driver never matches
// anything," since an accidentally-omitted field should not silently disable a
// previously-working driver.
typedef enum {
    BTHID_TRANSPORT_CLASSIC = 1 << 0,
    BTHID_TRANSPORT_BLE     = 1 << 1,
    BTHID_TRANSPORT_BOTH    = BTHID_TRANSPORT_CLASSIC | BTHID_TRANSPORT_BLE,
} bthid_transport_mask_t;

typedef struct {
    const char* name;

    // Real hardware transport(s) this driver's controller family can ever use.
    // See bthid_transport_mask_t above. Checked before match() is called.
    bthid_transport_mask_t transports;

    // Check if this driver handles a device (by VID/PID, name, COD, or transport)
    // Priority: VID/PID match > name match > COD match
    bool (*match)(const char* device_name, const uint8_t* class_of_device,
                  uint16_t vendor_id, uint16_t product_id, bool is_ble);

    // Initialize driver for a device
    bool (*init)(bthid_device_t* device);

    // Process incoming HID report
    void (*process_report)(bthid_device_t* device, const uint8_t* data, uint16_t len);

    // Periodic task (for output reports, rumble, etc.)
    void (*task)(bthid_device_t* device);

    // Device disconnected
    void (*disconnect)(bthid_device_t* device);

    // Optional HID feature-report response. `data` begins with report_id and
    // contains exactly the bytes returned by GET_REPORT (no HID transaction
    // header). Ordinary input routing is deliberately separate.
    void (*process_feature_report)(bthid_device_t* device,
                                   const uint8_t* data, uint16_t len);

} bthid_driver_t;

// ============================================================================
// BTHID API
// ============================================================================

// Initialize BTHID layer
void bthid_init(void);

// Periodic task (call from main loop)
void bthid_task(void);

// Get device by connection index
bthid_device_t* bthid_get_device(uint8_t conn_index);

// Get device count
uint8_t bthid_get_device_count(void);

// Optional hook: invoked with every raw input report before the driver parses it
// (conn_index identifies the device). Default is a weak no-op; PicoSwitch overrides
// it to expose the raw report to config mode's debug view for controller RE.
void bthid_on_raw_report(uint8_t conn_index, uint32_t connection_generation,
                         const uint8_t *data, uint16_t len);

// Lifecycle notification after a same-connection driver rebind.  This is
// distinct from HID-ready: the physical source remains the same owner and
// must not be removed/re-added as a new source.
void bthid_on_hid_rebind(uint8_t conn_index);

// Return the immutable token assigned to the current HID occupant, or zero if
// this transport index is not currently registered.
uint32_t bthid_get_connection_generation(uint8_t conn_index);

// Rebind guard used by the router seam while a driver is being replaced.  A
// driver's ordinary disconnect callback is reused for cleanup, but it must not
// be interpreted as physical source loss during this bounded interval.
void bthid_rebind_begin(void);
void bthid_rebind_end(void);
bool bthid_rebind_in_progress(void);

// Re-evaluate driver for a device (call when VID/PID or name becomes available)
void bthid_update_device_info(uint8_t conn_index, const char* name,
                               uint16_t vendor_id, uint16_t product_id);

// ============================================================================
// DRIVER REGISTRATION
// ============================================================================

// Register a device driver
void bthid_register_driver(const bthid_driver_t* driver);

// ============================================================================
// OUTPUT REPORTS
// ============================================================================

// Send output report (rumble, LEDs, etc.)
bool bthid_send_output_report(uint8_t conn_index, uint8_t report_id,
                               const uint8_t* data, uint16_t len);

// Send feature report
bool bthid_send_feature_report(uint8_t conn_index, uint8_t report_id,
                                const uint8_t* data, uint16_t len);

// Request and route one feature report from a Classic Bluetooth HID device.
bool bthid_request_feature_report(uint8_t conn_index, uint8_t report_id);
void bthid_on_feature_report(uint8_t conn_index, const uint8_t* data,
                             uint16_t len);

// ============================================================================
// BATTERY
// ============================================================================

// Update battery level from BLE Battery Service. Accepts recurring updates and
// a valid 0%; controller-native HID telemetry remains authoritative.
void bthid_set_battery_level(uint8_t conn_index, uint8_t level);

// ============================================================================
// HID DESCRIPTOR
// ============================================================================

// Pass BLE HID descriptor to driver for report parsing
void bthid_set_hid_descriptor(uint8_t conn_index, const uint8_t* desc, uint16_t desc_len);

// Debug: retrieve the single-slot cached raw HID descriptor for the `btid desc` config command
// (added 2026-07-12 to inspect real descriptor bytes instead of guessing from input symptoms).
// Returns false if nothing is cached. This mirrors the existing internal single-slot cache
// (bthid.c's cached_hid_desc/_len/_conn) used to hand descriptors to newly-created devices —
// same one-at-a-time-connection assumption, fine for a single-controller test session.
bool bthid_get_cached_descriptor(const uint8_t** out_data, uint16_t* out_len, uint8_t* out_conn);

// Switch a device from its current vendor driver to the generic gamepad driver.
// Used when a vendor driver detects an incompatible report format at runtime.
void bthid_fallback_to_generic(uint8_t conn_index);

#endif // BTHID_H
