// bthid.c - Bluetooth HID Layer Implementation
// Handles Bluetooth HID devices and routes reports to device-specific drivers

#include "bthid.h"
#include "core/input_event.h"
#include "bt/transport/bt_transport.h"
#include "devices/generic/bthid_gamepad.h"
#include "devices/generic/bthid_mouse.h"
#include "devices/vendors/sony/ds3_bt.h"
#include "devices/vendors/sony/ds4_bt.h"
#include "devices/vendors/sony/ds5_bt.h"
#include "core/services/storage/flash.h"
#include "bt_identity_log.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// SONY REPORT IDS (for reclassification)
// ============================================================================

#define SONY_REPORT_ID_BASIC    0x01    // DS3/DS4 basic mode
#define SONY_REPORT_ID_DS4      0x11    // DS4 full BT report
#define SONY_REPORT_ID_DS5      0x31    // DS5 full BT report

// ============================================================================
// CONFIGURATION
// ============================================================================

#define BTHID_MAX_DRIVERS       16  // Max registered drivers

// Raw-report debug hook: weak no-op default (PicoSwitch's seam provides the real one).
__attribute__((weak)) void bthid_on_raw_report(uint8_t conn_index,
                                               uint32_t connection_generation,
                                               const uint8_t *data, uint16_t len) {
    (void)conn_index; (void)connection_generation; (void)data; (void)len;
}

// Battery-publish hook. PicoSwitch's seam republishes the cached controller
// state immediately; standalone/host users can leave this as a no-op.
__attribute__((weak)) void bthid_on_battery_update(input_event_t *event) {
    (void)event;
}

// Platform safe-point hook. PicoSwitch uses this to service its cooperative
// BOOTSEL flash-sampling handshake. Keeping the hook here means sustained HID
// traffic creates safe points instead of starving the timer that normally
// supplies them. Other bthid users retain a zero-cost no-op.
__attribute__((weak)) void bthid_on_report_boundary(void) {}

// Optional lifecycle hook.  PicoSwitch uses it to expose an idle-but-connected
// controller in the active-input source registry before its first gameplay
// report.  Standalone bthid users keep the zero-cost weak default.
__attribute__((weak)) void bthid_on_hid_ready(uint8_t conn_index) {
    (void)conn_index;
}

__attribute__((weak)) void bthid_on_hid_rebind(uint8_t conn_index) {
    (void)conn_index;
}

// ============================================================================
// STATIC DATA
// ============================================================================

static bthid_device_t devices[BTHID_MAX_DEVICES];
static const bthid_driver_t* drivers[BTHID_MAX_DRIVERS];
static uint8_t driver_count = 0;
static uint32_t next_connection_generation = 1u;
static uint8_t rebind_depth;

// HID descriptor cache — stored when a vendor driver is active so the generic
// driver can use it if a fallback is triggered later
#define BTHID_MAX_DESC_LEN 512
static uint8_t cached_hid_desc[BTHID_MAX_DESC_LEN];
static uint16_t cached_hid_desc_len = 0;
static uint8_t cached_hid_desc_conn = 0xFF;  // conn_index of cached descriptor

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static bthid_device_t* find_or_create_device(uint8_t conn_index);
static const bthid_driver_t* find_driver(const char* name, const uint8_t* cod,
                                          uint16_t vendor_id, uint16_t product_id,
                                          bool is_ble);
static bool driver_transport_ok(const bthid_driver_t* driver, bool is_ble);
static bt_identity_provenance_t identity_provenance_guess(bool is_ble, uint16_t vendor_id);
static bthid_device_type_t classify_device(const uint8_t* class_of_device);
static bool try_reclassify_sony_device(bthid_device_t* device, uint8_t report_id);

static void bind_driver_event_generation(bthid_device_t *device)
{
    // Every production bthid driver keeps input_event_t as the first member of
    // driver_data (the battery path relies on this existing contract).
    if (device && device->driver_data)
        ((input_event_t *)device->driver_data)->connection_generation =
            device->connection_generation;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void bthid_init(void)
{
    memset(devices, 0, sizeof(devices));
    driver_count = 0;
    next_connection_generation = 1u;
    rebind_depth = 0u;
    printf("[BTHID] Initialized\n");
}

// ============================================================================
// DRIVER REGISTRATION
// ============================================================================

void bthid_register_driver(const bthid_driver_t* driver)
{
    if (driver_count < BTHID_MAX_DRIVERS) {
        drivers[driver_count++] = driver;
        printf("[BTHID] Registered driver: %s\n", driver->name);
    } else {
        printf("[BTHID] Driver registry full, cannot add: %s\n", driver->name);
    }
}

// ============================================================================
// TASK
// ============================================================================

static bool bthid_task_debug_done = false;
static bool bt_on_hid_report_debug_done = false;

void bthid_task(void)
{
    // Run device driver tasks
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (devices[i].active && devices[i].driver) {
            if (!bthid_task_debug_done) {
                printf("[BTHID] Task loop: dev %d active, driver=%p, drv->task=%p\n",
                       i, devices[i].driver,
                       ((const bthid_driver_t*)devices[i].driver)->task);
                bthid_task_debug_done = true;
            }
            const bthid_driver_t* drv = (const bthid_driver_t*)devices[i].driver;
            if (drv->task) {
                drv->task(&devices[i]);
            }
        }
    }
}

// ============================================================================
// DEVICE MANAGEMENT
// ============================================================================

static bthid_device_t* find_or_create_device(uint8_t conn_index)
{
    // Look for existing device
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (devices[i].active && devices[i].conn_index == conn_index) {
            return &devices[i];
        }
    }

    // Find free slot
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (!devices[i].active) {
            memset(&devices[i], 0, sizeof(devices[i]));
            devices[i].active = true;
            devices[i].conn_index = conn_index;
            devices[i].player_index = 0xFF;  // Unassigned
            devices[i].connection_generation = next_connection_generation++;
            if (devices[i].connection_generation == 0u)
                devices[i].connection_generation = next_connection_generation++;
            return &devices[i];
        }
    }

    return NULL;
}

static void remove_device(uint8_t conn_index)
{
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (devices[i].active && devices[i].conn_index == conn_index) {
            if (devices[i].driver) {
                const bthid_driver_t* drv = (const bthid_driver_t*)devices[i].driver;
                if (drv->disconnect) {
                    drv->disconnect(&devices[i]);
                }
            }
            memset(&devices[i], 0, sizeof(devices[i]));
            printf("[BTHID] Device removed from slot %d\n", i);
            break;
        }
    }
}

bthid_device_t* bthid_get_device(uint8_t conn_index)
{
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (devices[i].active && devices[i].conn_index == conn_index) {
            return &devices[i];
        }
    }
    return NULL;
}

uint8_t bthid_get_device_count(void)
{
    uint8_t count = 0;
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (devices[i].active) {
            count++;
        }
    }
    return count;
}

uint32_t bthid_get_connection_generation(uint8_t conn_index)
{
    bthid_device_t *device = bthid_get_device(conn_index);
    return device ? device->connection_generation : 0u;
}

void bthid_rebind_begin(void)
{
    if (rebind_depth != UINT8_MAX)
        rebind_depth++;
}

void bthid_rebind_end(void)
{
    if (rebind_depth > 0u)
        rebind_depth--;
}

bool bthid_rebind_in_progress(void)
{
    return rebind_depth != 0u;
}

// ============================================================================
// DEVICE INFO UPDATE (VID/PID available after SDP query)
// ============================================================================

void bthid_update_device_info(uint8_t conn_index, const char* name,
                               uint16_t vendor_id, uint16_t product_id)
{
    bthid_device_t* device = bthid_get_device(conn_index);
    if (!device) {
        return;
    }

    // Update name if provided
    if (name && name[0]) {
        strncpy(device->name, name, BTHID_MAX_NAME_LEN - 1);
        device->name[BTHID_MAX_NAME_LEN - 1] = '\0';
    }

    // Update VID/PID if provided
    if (vendor_id) device->vendor_id = vendor_id;
    if (product_id) device->product_id = product_id;

    // Re-evaluate driver when new info arrives.
    // Two cases:
    // 1. Generic driver → try to find a better vendor-specific match
    // 2. Vendor driver no longer matches (e.g., PID exclusion) → find correct driver
    const bthid_driver_t* current = (const bthid_driver_t*)device->driver;
    const bt_connection_t* conn = bt_get_connection(conn_index);
    const uint8_t* cod = conn ? conn->class_of_device : NULL;

    bool needs_reval = (current == &bthid_gamepad_driver);

    // Check if current vendor driver still matches with updated info
    // A generic mouse may have been selected from its HID descriptor because
    // BLE has no COD or because a Classic device advertises the keyboard+mouse
    // combo class. That structural evidence is stronger than the name/COD
    // heuristics accepted by match(); do not let a late VID/PID update undo it
    // and bind the device back to the generic gamepad parser.
    if (!needs_reval && current != &bthid_mouse_driver && current && current->match) {
        if (!current->match(device->name, cod, device->vendor_id,
                            device->product_id, device->is_ble)) {
            needs_reval = true;
        }
    }

    if (needs_reval) {
        const bthid_driver_t* new_driver = NULL;
        for (int i = 0; i < driver_count; i++) {
            if (drivers[i] != current &&
                drivers[i]->match && driver_transport_ok(drivers[i], device->is_ble) &&
                drivers[i]->match(device->name, cod,
                                   device->vendor_id, device->product_id,
                                   device->is_ble)) {
                new_driver = drivers[i];
                break;
            }
        }

        // If no vendor driver matches, fall back to generic
        if (!new_driver && current != &bthid_gamepad_driver) {
            new_driver = &bthid_gamepad_driver;
        }

        if (new_driver) {
            printf("[BTHID] Re-selecting driver: %s -> %s (name=%s VID=0x%04X PID=0x%04X)\n",
                   current->name, new_driver->name, device->name,
                   device->vendor_id, device->product_id);

            bthid_rebind_begin();
            if (current->disconnect) {
                current->disconnect(device);
            }
            bthid_rebind_end();
            device->driver_data = NULL;
            device->driver = new_driver;
            if (new_driver->init) {
                new_driver->init(device);
            }
            bind_driver_event_generation(device);

            bt_identity_log_record(conn_index, device->is_ble, device->name,
                                    device->vendor_id, device->product_id, 0,
                                    identity_provenance_guess(device->is_ble, device->vendor_id),
                                    cod, 0, NULL, new_driver->name, "reeval-rebind",
                                    (int8_t)device->player_index);

            // Pass cached HID descriptor through the central classifier. A
            // vendor fallback can still turn out to be a descriptor-defined
            // mouse and must not bypass mouse reclassification.
            if (new_driver == &bthid_gamepad_driver &&
                cached_hid_desc_len > 0 && cached_hid_desc_conn == conn_index) {
                bthid_set_hid_descriptor(conn_index, cached_hid_desc, cached_hid_desc_len);
            }
            bthid_on_hid_rebind(conn_index);
        } else if (current == &bthid_gamepad_driver) {
            // Still generic but VID/PID changed — update VID-dependent flags.
            //
            // Found 2026-07-12 hardware-testing the identity log against a real 8BitDo NGC
            // Modkit: this branch previously logged nothing at all, so the exact moment SDP/DIS
            // resolved a real VID/PID (while staying correctly on the generic driver, since no
            // more specific driver claims it) was invisible in btid dump — only inferable after
            // the fact from whatever event happened to log next. Log it explicitly now.
            bthid_gamepad_update_vid(device);
            bt_identity_log_record(conn_index, device->is_ble, device->name,
                                    device->vendor_id, device->product_id, 0,
                                    identity_provenance_guess(device->is_ble, device->vendor_id),
                                    cod, 0, NULL, current->name, "vid-resolved-stayed-generic",
                                    (int8_t)device->player_index);
        }
    }
}

// ============================================================================
// DRIVER MATCHING
// ============================================================================

// A driver whose declared transports don't include this connection's actual
// transport can never legitimately match it, regardless of what its match()
// function would otherwise return (see bthid_transport_mask_t in bthid.h for why
// this is checked centrally instead of inside each driver's match()).
static bool driver_transport_ok(const bthid_driver_t* driver, bool is_ble)
{
    bthid_transport_mask_t mask = driver->transports ? driver->transports : BTHID_TRANSPORT_BOTH;
    return is_ble ? (mask & BTHID_TRANSPORT_BLE) : (mask & BTHID_TRANSPORT_CLASSIC);
}

// Best-effort provenance inference shared by every bt_identity_log_record() call site (see
// bt_identity_log.h for the full rationale). A single helper, not three copies, specifically
// because a 2026-07-12 hardware test found one of the three original inline copies (the
// descriptor-arrival hook) had drifted to a hardcoded BTID_PROV_UNKNOWN regardless of actual
// state — exactly the kind of duplication bug a shared helper prevents from recurring.
static bt_identity_provenance_t identity_provenance_guess(bool is_ble, uint16_t vendor_id)
{
    if (vendor_id == 0) return BTID_PROV_UNKNOWN;
    return is_ble ? BTID_PROV_BLE_DIS_PNP : BTID_PROV_CLASSIC_SDP;
}

static const bthid_driver_t* find_driver(const char* name, const uint8_t* cod,
                                          uint16_t vendor_id, uint16_t product_id,
                                          bool is_ble)
{
    // First try registered drivers
    for (int i = 0; i < driver_count; i++) {
        if (drivers[i]->match && driver_transport_ok(drivers[i], is_ble) &&
            drivers[i]->match(name, cod, vendor_id, product_id, is_ble)) {
            return drivers[i];
        }
    }
    return NULL;
}

static bthid_device_type_t classify_device(const uint8_t* class_of_device)
{
    if (!class_of_device) {
        return BTHID_DEVICE_UNKNOWN;
    }

    // Class of Device format:
    // cod[0]: Minor Device Class + Format Type
    // cod[1]: Major Service Class (low byte) + Major Device Class
    // cod[2]: Major Service Class (high byte)

    uint8_t major_class = (class_of_device[1] >> 0) & 0x1F;
    uint8_t minor_class = (class_of_device[0] >> 2) & 0x3F;

    // Major class 0x05 = Peripheral
    if (major_class == 0x05) {
        // Minor class bits 7-6 indicate device type
        uint8_t peripheral_type = (minor_class >> 4) & 0x03;

        switch (peripheral_type) {
            case 0x01:  // Keyboard
                return BTHID_DEVICE_KEYBOARD;
            case 0x02:  // Pointing device
                return BTHID_DEVICE_MOUSE;
            case 0x03:  // Combo keyboard/pointing
                return BTHID_DEVICE_KEYBOARD;
            default:
                break;
        }

        // Check minor bits 5-0 for gamepad/joystick
        uint8_t device_subtype = minor_class & 0x0F;
        if (device_subtype == 0x01) {
            return BTHID_DEVICE_JOYSTICK;
        } else if (device_subtype == 0x02) {
            return BTHID_DEVICE_GAMEPAD;
        }
    }

    return BTHID_DEVICE_UNKNOWN;
}

// ============================================================================
// SONY DEVICE RECLASSIFICATION
// Detect DS4 vs DS5 by report ID and swap drivers if needed
// ============================================================================

static bool try_reclassify_sony_device(bthid_device_t* device, uint8_t report_id)
{
    const bthid_driver_t* current = (const bthid_driver_t*)device->driver;
    const bthid_driver_t* new_driver = NULL;

    // Check if reclassification is needed
    if (report_id == SONY_REPORT_ID_DS5 && current != &ds5_bt_driver) {
        // Got DS5 report but not using DS5 driver
        new_driver = &ds5_bt_driver;
        printf("[BTHID] Reclassify: report 0x%02X -> DS5 driver\n", report_id);
    } else if (report_id == SONY_REPORT_ID_DS4 && current != &ds4_bt_driver) {
        // Got DS4 full report but not using DS4 driver
        new_driver = &ds4_bt_driver;
        printf("[BTHID] Reclassify: report 0x%02X -> DS4 driver\n", report_id);
    }

    if (new_driver) {
        // Reclassification is a same-connection driver replacement, not a
        // physical source loss.  The seam observes this explicit guard.
        bthid_rebind_begin();
        if (current && current->disconnect) {
            current->disconnect(device);
        }
        bthid_rebind_end();

        // Clear driver data
        device->driver_data = NULL;

        // Initialize new driver
        device->driver = new_driver;
        if (new_driver->init) {
            new_driver->init(device);
        }
        bind_driver_event_generation(device);
        bthid_on_hid_rebind(device->conn_index);

        printf("[BTHID] Reclassification complete: now using %s\n", new_driver->name);
        return true;
    }

    return false;
}

// ============================================================================
// TRANSPORT CALLBACKS
// Override weak implementations in bt_transport.c
// ============================================================================

void bt_on_hid_ready(uint8_t conn_index)
{
    printf("[BTHID] HID ready on connection %d\n", conn_index);

    const bt_connection_t* conn = bt_get_connection(conn_index);
    if (!conn) {
        return;
    }

    bthid_device_t* device = find_or_create_device(conn_index);
    if (!device) {
        printf("[BTHID] No free device slots\n");
        return;
    }

    // Copy device info
    memcpy(device->bd_addr, conn->bd_addr, 6);
    strncpy(device->name, conn->name, BTHID_MAX_NAME_LEN - 1);
    device->name[BTHID_MAX_NAME_LEN - 1] = '\0';
    device->type = classify_device(conn->class_of_device);
    device->vendor_id = conn->vendor_id;
    device->product_id = conn->product_id;
    device->is_ble = conn->is_ble;

    char addr_str[18];
    sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
            device->bd_addr[5], device->bd_addr[4], device->bd_addr[3],
            device->bd_addr[2], device->bd_addr[1], device->bd_addr[0]);

    printf("[BTHID] Device: %s (%s), type=%d, VID=0x%04X PID=0x%04X\n",
           device->name[0] ? device->name : "Unknown",
           addr_str, device->type, conn->vendor_id, conn->product_id);

    // Find matching driver (VID/PID takes priority over name/COD)
    const bthid_driver_t* driver = find_driver(device->name, conn->class_of_device,
                                               conn->vendor_id, conn->product_id,
                                               device->is_ble);
    // Best-effort provenance for the identity log: DIS PnP ID hasn't run yet at this point
    // for BLE (see the Gate 2 timing trace in btstack-implementation.md — it's deferred until
    // after HID notifications enable), so a nonzero BLE VID/PID here can only have come from
    // pre-connection advertisement manufacturer data (Switch 2's mechanism). Classic BT SDP
    // typically completes before HID ready, so a nonzero Classic VID/PID here is attributed to
    // SDP. This is a reasonable inference from the connection's own timing, not a guarantee.
    bt_identity_provenance_t provenance = conn->vendor_id == 0 ? BTID_PROV_UNKNOWN
        : (device->is_ble ? BTID_PROV_BLE_ADV_MFR_DATA : BTID_PROV_CLASSIC_SDP);
    if (driver) {
        printf("[BTHID] Using driver: %s\n", driver->name);
        device->driver = driver;
        if (driver->init) {
            driver->init(device);
        }
        bind_driver_event_generation(device);
    } else {
        printf("[BTHID] No specific driver found, using generic gamepad\n");
        device->driver = &bthid_gamepad_driver;
        if (bthid_gamepad_driver.init) {
            bthid_gamepad_driver.init(device);
        }
        bind_driver_event_generation(device);
    }
    bt_identity_log_record(conn_index, device->is_ble, device->name,
                            conn->vendor_id, conn->product_id, 0, provenance,
                            conn->class_of_device, 0, NULL,
                            ((const bthid_driver_t*)device->driver)->name,
                            driver ? "initial-bind" : "initial-bind-generic-fallback", -1);

    // Pass cached HID descriptor if available (often arrives before device creation).
    // Route through the central hook so an unnamed BLE mouse initially claimed
    // by the catch-all gamepad fallback can be reclassified structurally.
    const bthid_driver_t *bound = (const bthid_driver_t*)device->driver;
    if ((bound == &bthid_gamepad_driver || bound == &bthid_mouse_driver) &&
        cached_hid_desc_len > 0 && cached_hid_desc_conn == conn_index) {
        bthid_set_hid_descriptor(conn_index, cached_hid_desc, cached_hid_desc_len);
    }

    // Debug: confirm device state directly from array
    printf("[BTHID] Setup complete: devices[0].active=%d, devices[0].driver=%p\n",
           devices[0].active, devices[0].driver);
    bthid_on_hid_ready(conn_index);
}

void bt_on_disconnect(uint8_t conn_index)
{
    bt_on_disconnect_with_generation(
        conn_index, bthid_get_connection_generation(conn_index));
}

void bt_on_disconnect_with_generation(uint8_t conn_index,
                                      uint32_t connection_generation)
{
    printf("[BTHID] Disconnect on connection %d\n", conn_index);
    bthid_device_t *device = bthid_get_device(conn_index);
    if (connection_generation != 0u &&
        (!device || device->connection_generation != connection_generation)) {
        // A delayed disconnect for an older occupant must not remove the new
        // device that reused this transport index.
        return;
    }
    if (cached_hid_desc_conn == conn_index) {
        cached_hid_desc_len = 0;
        cached_hid_desc_conn = 0xFF;
    }
    remove_device(conn_index);

    // Reset one-shot debug flags so reconnections produce debug output
    bt_on_hid_report_debug_done = false;
    bthid_task_debug_done = false;

    // Check if we have pending flash writes now that BT may be idle
    flash_on_bt_disconnect();
}

void bt_on_hid_report(uint8_t conn_index, const uint8_t* data, uint16_t len)
{
    bt_on_hid_report_with_generation(
        conn_index, bthid_get_connection_generation(conn_index), data, len);
}

void bt_on_hid_report_with_generation(uint8_t conn_index,
                                      uint32_t connection_generation,
                                      const uint8_t* data, uint16_t len)
{
    // Run before validation/routing: even an unbound or malformed high-rate
    // source must not be able to starve platform maintenance indefinitely.
    bthid_on_report_boundary();

    if (len < 1) {
        return;
    }

    bthid_device_t* device = bthid_get_device(conn_index);
    if (connection_generation != 0u &&
        (!device || device->connection_generation != connection_generation)) {
        // Transport callbacks may be deferred past a disconnect/reconnect.
        // Reject a captured old lifecycle before invoking any driver.
        return;
    }
    if (!device) {
        static uint8_t unknown_report_count = 0;
        if (unknown_report_count < 3) {
            printf("[BTHID] Report for unknown device on conn %d (len=%d)\n", conn_index, len);
            unknown_report_count++;
        }
        return;
    }

    // Debug first report after (re)connection
    if (!bt_on_hid_report_debug_done) {
        printf("[BTHID] First report: conn=%d, len=%d, data[0]=0x%02X\n",
               conn_index, len, data[0]);
        bt_on_hid_report_debug_done = true;
    }

    // Parse HID transaction header
    uint8_t header = data[0];
    uint8_t trans_type = header & 0xF0;
    uint8_t param = header & 0x0F;

    switch (trans_type) {
        case BTHID_TRANS_DATA: {
            // Data report - param indicates report type
            uint8_t report_type = param;
            const uint8_t* report_data = data + 1;
            uint16_t report_len = len - 1;

            if (report_type == BTHID_REPORT_TYPE_INPUT && report_len >= 1) {
                // Check report ID for Sony device reclassification
                // Only attempt if currently using a Sony driver or generic gamepad
                // This prevents Xbox data (which may contain 0x11/0x31 bytes) from triggering reclassification
                uint8_t report_id = report_data[0];
                const bthid_driver_t* drv = (const bthid_driver_t*)device->driver;
                bool is_sony_or_generic = (drv == &ds3_bt_driver || drv == &ds4_bt_driver ||
                                           drv == &ds5_bt_driver || drv == &bthid_gamepad_driver);
                if (is_sony_or_generic && (report_id == SONY_REPORT_ID_DS4 || report_id == SONY_REPORT_ID_DS5)) {
                    // Try to reclassify based on actual report ID
                    if (try_reclassify_sony_device(device, report_id)) {
                        // Driver was swapped - it will process this report on next iteration
                        // after its init sequence completes
                        return;
                    }
                }

                // Raw-report debug hook (config-mode view); weak no-op by default.
                bthid_on_raw_report(device->conn_index,
                                    device->connection_generation,
                                    report_data, report_len);

                // Input report - route to driver
                if (device->driver) {
                    const bthid_driver_t* drv = (const bthid_driver_t*)device->driver;
                    if (drv->process_report) {
                        drv->process_report(device, report_data, report_len);
                    }
                }
            }
            break;
        }

        case BTHID_TRANS_HANDSHAKE:
            printf("[BTHID] Handshake: result=%d\n", param);
            break;

        default:
            printf("[BTHID] Unhandled transaction: 0x%02X\n", trans_type);
            break;
    }
}

// ============================================================================
// HID DESCRIPTOR
// ============================================================================

void bthid_set_hid_descriptor(uint8_t conn_index, const uint8_t* desc, uint16_t desc_len)
{
    // Always cache — descriptor often arrives before device is created (Classic BT)
    if (desc_len <= BTHID_MAX_DESC_LEN) {
        memmove(cached_hid_desc, desc, desc_len);
        cached_hid_desc_len = desc_len;
        cached_hid_desc_conn = conn_index;
    }

    bthid_device_t* device = bthid_get_device(conn_index);
    if (!device || !device->driver) return;

    // HID report descriptor carries no VID/PID of its own — log it as its own event (fingerprint
    // + length) rather than folding into the last identity event, since it often arrives on a
    // separate timeline from VID/PID resolution (see the Gate 2 timing trace).
    //
    // Fixed 2026-07-12, found hardware-testing against a real 8BitDo NGC Modkit: this
    // previously hardcoded BTID_PROV_UNKNOWN and NULL class_of_device unconditionally,
    // regardless of device->vendor_id/product_id already being resolved and the connection's
    // real COD being available — the log showed a stale/wrong snapshot instead of the device's
    // actual state at this point in time.
    const bt_connection_t* desc_conn = bt_get_connection(conn_index);
    bt_identity_log_record(conn_index, device->is_ble, device->name,
                            device->vendor_id, device->product_id, 0,
                            identity_provenance_guess(device->is_ble, device->vendor_id),
                            desc_conn ? desc_conn->class_of_device : NULL, desc_len, desc,
                            ((const bthid_driver_t*)device->driver)->name,
                            "descriptor-arrived", (int8_t)device->player_index);

    const bthid_driver_t *current = (const bthid_driver_t*)device->driver;
    if (current == &bthid_gamepad_driver &&
        bthid_mouse_descriptor_is_mouse(desc, desc_len)) {
        // BLE has no Class-of-Device, and many mice advertise opaque product
        // names. The relative X+Y descriptor is the authoritative discriminator.
        printf("[BTHID] Descriptor reclassify: generic gamepad -> generic mouse (%s)\n",
               device->name);
        bthid_rebind_begin();
        if (current->disconnect) current->disconnect(device);
        bthid_rebind_end();
        device->driver_data = NULL;
        device->driver = &bthid_mouse_driver;
        device->type = BTHID_DEVICE_MOUSE;
        if (bthid_mouse_driver.init && bthid_mouse_driver.init(device))
            bthid_mouse_set_descriptor(device, desc, desc_len);
        bind_driver_event_generation(device);
        bthid_on_hid_rebind(conn_index);
    } else if (current == &bthid_mouse_driver) {
        bthid_mouse_set_descriptor(device, desc, desc_len);
    } else if (current == &bthid_gamepad_driver) {
        bthid_gamepad_set_descriptor(device, desc, desc_len);
    }
}

bool bthid_get_cached_descriptor(const uint8_t** out_data, uint16_t* out_len, uint8_t* out_conn)
{
    if (cached_hid_desc_len == 0) return false;
    if (out_data) *out_data = cached_hid_desc;
    if (out_len) *out_len = cached_hid_desc_len;
    if (out_conn) *out_conn = cached_hid_desc_conn;
    return true;
}

void bthid_fallback_to_generic(uint8_t conn_index)
{
    bthid_device_t* device = bthid_get_device(conn_index);
    if (!device || !device->driver) return;

    const bthid_driver_t* current = (const bthid_driver_t*)device->driver;
    if (current == &bthid_gamepad_driver) return;  // Already generic

    printf("[BTHID] Fallback to generic: %s -> %s (name=%s)\n",
           current->name, bthid_gamepad_driver.name, device->name);

    // Same-connection fallback: preserve logical source ownership while
    // resetting only the vendor parser state.
    bthid_rebind_begin();
    if (current->disconnect) {
        current->disconnect(device);
    }
    bthid_rebind_end();
    device->driver_data = NULL;

    // Switch to generic
    device->driver = &bthid_gamepad_driver;
    if (bthid_gamepad_driver.init) {
        bthid_gamepad_driver.init(device);
    }
    bind_driver_event_generation(device);

    // Pass cached HID descriptor if available for this device
    if (cached_hid_desc_len > 0 && cached_hid_desc_conn == conn_index) {
        bthid_set_hid_descriptor(conn_index, cached_hid_desc, cached_hid_desc_len);
    }
    bthid_on_hid_rebind(conn_index);
}

// ============================================================================
// BATTERY
// ============================================================================

void bthid_set_battery_level(uint8_t conn_index, uint8_t level)
{
    bthid_device_t* device = bthid_get_device(conn_index);
    if (!device || !device->driver_data) {
        return;
    }

    // All driver data structs have input_event_t as their first field
    input_event_t* event = (input_event_t*)device->driver_data;

    // BAS remains a live fallback: accept 0% and recurring notifications, but
    // never overwrite controller-native HID telemetry (which can also include
    // a reliable charging state).
    if (input_event_set_bas_battery(event, level)) {
        printf("[BTHID] BAS battery: %d%% (conn %d)\n", level, conn_index);
        // Battery notifications may arrive while the controller is otherwise
        // idle, so publish the cached buttons/axes with the new power state.
        bthid_on_battery_update(event);
    }
}

// ============================================================================
// OUTPUT REPORTS
// ============================================================================

bool bthid_send_output_report(uint8_t conn_index, uint8_t report_id,
                               const uint8_t* data, uint16_t len)
{
    uint8_t buf[64];
    if (len + 2 > sizeof(buf)) {
        return false;
    }

    // Build DATA | OUTPUT header
    buf[0] = BTHID_TRANS_DATA | BTHID_REPORT_TYPE_OUTPUT;
    buf[1] = report_id;
    memcpy(&buf[2], data, len);

    // Diagnostic for the 2026-07-12 BLE-rumble investigation: this is the one
    // choke point every non-DS4/DS5 driver's output report passes through
    // before it reaches btstack_classic_send_report() (which, despite the
    // name, branches to hids_client_send_write_report() for BLE conn_index
    // values). Logging here proves whether a send was even attempted and
    // what conn_index/report_id it used, independent of whatever the BTstack
    // call itself reports back.
    bool ok = bt_send_interrupt(conn_index, buf, len + 2);
    printf("[BTHID] send_output_report conn=%d report_id=0x%02X len=%d -> %s\n",
           conn_index, report_id, len, ok ? "queued" : "FAILED");
    return ok;
}

bool bthid_send_feature_report(uint8_t conn_index, uint8_t report_id,
                                const uint8_t* data, uint16_t len)
{
    uint8_t buf[64];
    if (len + 2 > sizeof(buf)) {
        return false;
    }

    // Build SET_REPORT | FEATURE header
    buf[0] = BTHID_TRANS_SET_REPORT | BTHID_REPORT_TYPE_FEATURE;
    buf[1] = report_id;
    memcpy(&buf[2], data, len);

    return bt_send_control(conn_index, buf, len + 2);
}

bool bthid_request_feature_report(uint8_t conn_index, uint8_t report_id)
{
    bthid_device_t *device = bthid_get_device(conn_index);
    if (!device || !device->active) return false;
    return bt_request_feature_report(conn_index, report_id);
}

void bthid_on_feature_report(uint8_t conn_index, const uint8_t *data,
                             uint16_t len)
{
    bthid_device_t *device = bthid_get_device(conn_index);
    if (!device || !device->active || !device->driver || !data || len < 1)
        return;

    const bthid_driver_t *driver = (const bthid_driver_t *)device->driver;
    if (driver->process_feature_report)
        driver->process_feature_report(device, data, len);
}
