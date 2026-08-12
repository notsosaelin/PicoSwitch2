/*
 * Host regression coverage for late Bluetooth VID/PID resolution.
 *
 * This links the production bthid.c state machine against mock transports and
 * drivers. It verifies that HID can bind and consume reports before DIS, then
 * rebind from authoritative identity without losing subsequent input.
 *
 *   gcc -std=c11 -Wall -Wextra -ffunction-sections -fdata-sections \
 *       -Isrc -Isrc/bt_hid -Isrc/bt_hid/bt/bthid -Iinclude \
 *       tools/test_bthid_late_identity.c \
 *       src/bt_hid/bt/bthid/bthid.c src/bt_hid/bt/bthid/bthid_identity.c \
 *       -Wl,--gc-sections -o build/test_bthid_late_identity.exe
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt/bthid/bthid.h"
#include "bt/bthid/bthid_identity.h"
#include "bt/transport/bt_transport.h"
#include "bt_identity_log.h"

static int failures;
static bt_connection_t connection;
static unsigned provisional_init_count;
static unsigned provisional_disconnect_count;
static unsigned provisional_report_count;
static unsigned exact_init_count;
static unsigned exact_disconnect_count;
static unsigned exact_report_count;
static unsigned generic_init_count;
static unsigned generic_disconnect_count;
static unsigned generic_report_count;
static unsigned generic_vid_update_count;
static unsigned report_boundary_count;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            printf("FAIL: %s\n", message);                                   \
            failures++;                                                       \
        } else {                                                               \
            printf("OK:   %s\n", message);                                   \
        }                                                                      \
    } while (0)

static const bt_connection_t *fake_get_connection(uint8_t index)
{
    return index == 0 ? &connection : NULL;
}

static const bt_transport_t fake_transport = {
    .name = "late-identity-host-test",
    .get_connection = fake_get_connection,
};

const bt_transport_t *bt_transport = &fake_transport;

static bool init_provisional(bthid_device_t *device)
{
    provisional_init_count++;
    device->driver_data = &provisional_init_count;
    return true;
}

static void disconnect_provisional(bthid_device_t *device)
{
    (void)device;
    provisional_disconnect_count++;
}

static void report_provisional(bthid_device_t *device, const uint8_t *data,
                               uint16_t len)
{
    (void)device;
    (void)data;
    (void)len;
    provisional_report_count++;
}

static bool match_provisional_name(const char *name, const uint8_t *cod,
                                   uint16_t vid, uint16_t pid, bool is_ble)
{
    static const uint16_t expected_pids[] = { 0x0001 };
    (void)cod;
    return is_ble && name && strstr(name, "Provisional Pad") &&
           bthid_name_fallback_allowed(vid, pid, 0x1111, expected_pids, 1);
}

static bool init_exact(bthid_device_t *device)
{
    exact_init_count++;
    device->driver_data = &exact_init_count;
    return true;
}

static void disconnect_exact(bthid_device_t *device)
{
    (void)device;
    exact_disconnect_count++;
}

static void report_exact(bthid_device_t *device, const uint8_t *data,
                         uint16_t len)
{
    (void)device;
    (void)data;
    (void)len;
    exact_report_count++;
}

static bool match_exact(const char *name, const uint8_t *cod,
                        uint16_t vid, uint16_t pid, bool is_ble)
{
    (void)name;
    (void)cod;
    return is_ble && vid == 0x2222 && pid == 0x0002;
}

static const bthid_driver_t provisional_driver = {
    .name = "Provisional Name Driver",
    .transports = BTHID_TRANSPORT_BLE,
    .match = match_provisional_name,
    .init = init_provisional,
    .process_report = report_provisional,
    .disconnect = disconnect_provisional,
};

static const bthid_driver_t exact_driver = {
    .name = "Exact Identity Driver",
    .transports = BTHID_TRANSPORT_BLE,
    .match = match_exact,
    .init = init_exact,
    .process_report = report_exact,
    .disconnect = disconnect_exact,
};

static const bthid_driver_t classic_only_exact_driver = {
    .name = "Wrong Transport Driver",
    .transports = BTHID_TRANSPORT_CLASSIC,
    .match = match_exact,
};

static bool match_generic(const char *name, const uint8_t *cod,
                          uint16_t vid, uint16_t pid, bool is_ble)
{
    (void)name;
    (void)cod;
    (void)vid;
    (void)pid;
    (void)is_ble;
    return true;
}

static bool init_generic(bthid_device_t *device)
{
    generic_init_count++;
    device->driver_data = &generic_init_count;
    return true;
}

static void report_generic(bthid_device_t *device, const uint8_t *data,
                           uint16_t len)
{
    (void)device;
    (void)data;
    (void)len;
    generic_report_count++;
}

static void disconnect_generic(bthid_device_t *device)
{
    (void)device;
    generic_disconnect_count++;
}

const bthid_driver_t bthid_gamepad_driver = {
    .name = "Generic BT Gamepad",
    .transports = BTHID_TRANSPORT_BOTH,
    .match = match_generic,
    .init = init_generic,
    .process_report = report_generic,
    .disconnect = disconnect_generic,
};

const bthid_driver_t bthid_mouse_driver = { .name = "Generic BT Mouse test stub" };

bool bthid_mouse_descriptor_is_mouse(const uint8_t *desc, uint16_t desc_len)
{
    (void)desc;
    (void)desc_len;
    return false;
}

void bthid_mouse_set_descriptor(bthid_device_t *device, const uint8_t *desc,
                                uint16_t desc_len)
{
    (void)device;
    (void)desc;
    (void)desc_len;
}

// bthid.c keeps report-ID-based Sony reclassification available. These inert
// definitions satisfy that production path without participating in the tests.
const bthid_driver_t ds3_bt_driver = { .name = "DS3 test stub" };
const bthid_driver_t ds4_bt_driver = { .name = "DS4 test stub" };
const bthid_driver_t ds5_bt_driver = { .name = "DS5 test stub" };

void bthid_gamepad_set_descriptor(bthid_device_t *device, const uint8_t *desc,
                                  uint16_t desc_len)
{
    (void)device;
    (void)desc;
    (void)desc_len;
}

void bthid_gamepad_update_vid(bthid_device_t *device)
{
    (void)device;
    generic_vid_update_count++;
}

void flash_on_bt_disconnect(void) {}

void bthid_on_report_boundary(void)
{
    report_boundary_count++;
}

void bt_identity_log_record(uint8_t conn_index, bool is_ble, const char *name,
                            uint16_t vendor_id, uint16_t product_id,
                            uint16_t product_version,
                            bt_identity_provenance_t provenance,
                            const uint8_t *class_of_device, uint16_t desc_len,
                            const uint8_t *desc_bytes, const char *driver_name,
                            const char *reason, int8_t player_slot)
{
    (void)conn_index;
    (void)is_ble;
    (void)name;
    (void)vendor_id;
    (void)product_id;
    (void)product_version;
    (void)provenance;
    (void)class_of_device;
    (void)desc_len;
    (void)desc_bytes;
    (void)driver_name;
    (void)reason;
    (void)player_slot;
}

static void reset_fixture(const char *name)
{
    memset(&connection, 0, sizeof(connection));
    strncpy(connection.name, name, sizeof(connection.name) - 1);
    connection.connected = true;
    connection.hid_ready = true;
    connection.is_ble = true;

    provisional_init_count = 0;
    provisional_disconnect_count = 0;
    provisional_report_count = 0;
    exact_init_count = 0;
    exact_disconnect_count = 0;
    exact_report_count = 0;
    generic_init_count = 0;
    generic_disconnect_count = 0;
    generic_report_count = 0;
    generic_vid_update_count = 0;
    report_boundary_count = 0;
    bthid_init();
}

static void send_input_report(void)
{
    const uint8_t report[] = { 0xA1, 0x01, 0x55 };
    bt_on_hid_report(0, report, sizeof(report));
}

static void test_name_fallback_evidence(void)
{
    const uint16_t one_pid[] = { 0x9400 };

    CHECK(bthid_name_fallback_allowed(0, 0, 0x18D1, one_pid, 1),
          "unknown pre-DIS identity permits provisional name matching");
    CHECK(bthid_name_fallback_allowed(0x18D1, 0x9400, 0x18D1, one_pid, 1),
          "expected late VID/PID preserves the provisional match");
    CHECK(!bthid_name_fallback_allowed(0x9999, 0x9400, 0x18D1, one_pid, 1),
          "known contradictory VID rejects a provisional name match");
    CHECK(!bthid_name_fallback_allowed(0x18D1, 0x9999, 0x18D1, one_pid, 1),
          "known contradictory PID rejects a model-specific name match");
    CHECK(bthid_name_fallback_allowed(0x045E, 0x1234, 0x045E, NULL, 0),
          "vendor-wide driver accepts any PID from its expected vendor");
}

static void test_provisional_bind_reselects_and_keeps_input_flowing(void)
{
    reset_fixture("Provisional Pad");
    bthid_register_driver(&provisional_driver);
    bthid_register_driver(&exact_driver);
    bt_on_hid_ready(0);

    bthid_device_t *device = bthid_get_device(0);
    CHECK(device && device->driver == &provisional_driver,
          "HID binds immediately by name before DIS resolves identity");

    send_input_report();
    CHECK(provisional_report_count == 1,
          "provisional driver consumes input before late identity arrives");

    connection.vendor_id = 0x2222;
    connection.product_id = 0x0002;
    bthid_update_device_info(0, connection.name,
                             connection.vendor_id, connection.product_id);

    CHECK(device->driver == &exact_driver,
          "late authoritative VID/PID replaces a contradicted name binding");
    CHECK(provisional_disconnect_count == 1 && exact_init_count == 1,
          "late rebind cleanly disconnects old driver and initializes new driver once");
    CHECK(device->vendor_id == 0x2222 && device->product_id == 0x0002,
          "resolved identity is stored on the active HID device");

    send_input_report();
    CHECK(exact_report_count == 1,
          "the next notification reaches the newly selected driver");
    CHECK(report_boundary_count == 2,
          "both pre- and post-resolution reports retain maintenance safe points");

    bthid_update_device_info(0, connection.name,
                             connection.vendor_id, connection.product_id);
    CHECK(exact_init_count == 1 && exact_disconnect_count == 0,
          "repeated DIS confirmation is idempotent for a stable exact driver");
}

static void test_generic_upgrade_and_transport_filter(void)
{
    reset_fixture("Unknown BLE Pad");
    bthid_register_driver(&classic_only_exact_driver);
    bthid_register_driver(&exact_driver);
    bt_on_hid_ready(0);

    bthid_device_t *device = bthid_get_device(0);
    CHECK(device && device->driver == &bthid_gamepad_driver,
          "unknown BLE device begins on generic without waiting for DIS");

    send_input_report();
    CHECK(generic_report_count == 1,
          "generic fallback consumes notifications during identity resolution");

    connection.vendor_id = 0x2222;
    connection.product_id = 0x0002;
    bthid_update_device_info(0, connection.name,
                             connection.vendor_id, connection.product_id);

    CHECK(device->driver == &exact_driver,
          "late identity upgrades generic to the matching BLE driver");
    CHECK(device->driver != &classic_only_exact_driver,
          "late re-evaluation still enforces the physical transport mask");
}

static void test_contradicted_name_falls_back_to_generic(void)
{
    reset_fixture("Provisional Pad");
    bthid_register_driver(&provisional_driver);
    bt_on_hid_ready(0);

    bthid_update_device_info(0, connection.name, 0x9999, 0x9999);
    bthid_device_t *device = bthid_get_device(0);
    CHECK(device && device->driver == &bthid_gamepad_driver,
          "contradicted name binding falls back safely when no exact driver claims it");

    send_input_report();
    CHECK(generic_report_count == 1,
          "generic fallback continues consuming input after correction");
}

static void test_android_initiated_classic_phone_falls_back_to_generic(void)
{
    reset_fixture("PicoSwitch2 Android Bridge");
    connection.is_ble = false;
    // Major Class 0x02 (Phone), deliberately not Peripheral/Gamepad. This
    // models an OEM that keeps its phone CoD after registering HID Device.
    connection.class_of_device[0] = 0x0C;
    connection.class_of_device[1] = 0x02;
    connection.class_of_device[2] = 0x00;

    bt_on_hid_ready(0);
    bthid_device_t *device = bthid_get_device(0);
    CHECK(device && device->driver == &bthid_gamepad_driver,
          "Android-initiated Classic HID binds generic despite phone Class of Device");
    CHECK(device && !device->is_ble && device->type == BTHID_DEVICE_UNKNOWN,
          "phone Class of Device is retained as unknown rather than rewritten as a gamepad");

    send_input_report();
    CHECK(generic_report_count == 1,
          "bound Classic Android HID input reaches the generic driver");

    // Late Classic SDP delivers the phone's own (non-gamepad) VID/PID. No vendor
    // driver claims it, so the re-evaluation path must keep it on generic without
    // a disconnect/rebind churn -- otherwise built-in controls would hiccup mid-session.
    connection.vendor_id = 0x1234;
    connection.product_id = 0x5678;
    bthid_update_device_info(0, connection.name,
                             connection.vendor_id, connection.product_id);
    CHECK(device->driver == &bthid_gamepad_driver && generic_disconnect_count == 0,
          "late phone VID/PID keeps the generic binding without a rebind churn");
    CHECK(generic_vid_update_count == 1,
          "generic stays bound and only refreshes VID-dependent flags on late identity");

    send_input_report();
    CHECK(generic_report_count == 2,
          "input keeps flowing to generic after late phone identity resolves");

    bt_on_disconnect(0);
    CHECK(generic_disconnect_count == 1 && bthid_get_device(0) == NULL,
          "Classic Android link teardown invokes generic cleanup and removes the device");
}

int main(void)
{
    test_name_fallback_evidence();
    test_provisional_bind_reselects_and_keeps_input_flowing();
    test_generic_upgrade_and_transport_filter();
    test_contradicted_name_falls_back_to_generic();
    test_android_initiated_classic_phone_falls_back_to_generic();

    if (failures) {
        printf("bthid_late_identity: %d failure(s)\n", failures);
        return 1;
    }
    puts("bthid_late_identity: all tests passed");
    return 0;
}
