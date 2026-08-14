/*
 * Transport-integrated active-input lifecycle regression.
 *
 * This links the production bthid lifecycle callbacks to the production
 * source arbiter.  It covers same-connection parser rebinds (late identity,
 * Sony DS4/DS5 report reclassification, descriptor mouse classification, and
 * vendor fallback) plus a recycled transport index where an old report and
 * old disconnect are delivered after a new occupant is ready.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt/bthid/bthid.h"
#include "bt/transport/bt_transport.h"
#include "bt_identity_log.h"
#include "core/router/router.h"
#include "ns2_input_arbiter.h"

static int failures;
static bt_connection_t connection;
static input_event_t event_storage;
static ns2_input_arbiter_t arbiter;
static bool descriptor_is_mouse;
static unsigned generic_reports;
static unsigned provisional_reports;
static unsigned exact_reports;
static unsigned vendor_reports;
static unsigned mouse_reports;
static unsigned ds4_reports;
static unsigned ds5_reports;
static unsigned generic_disconnects;
static unsigned provisional_disconnects;
static unsigned exact_disconnects;
static unsigned vendor_disconnects;
static unsigned mouse_disconnects;
static unsigned ds4_disconnects;
static unsigned ds5_disconnects;
static unsigned ready_hooks;
static unsigned rebind_hooks;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) printf("OK:   %s\n", message);                         \
        else { printf("FAIL: %s\n", message); failures++; }                  \
    } while (0)

static const bt_connection_t *fake_get_connection(uint8_t index)
{
    return index == 0u ? &connection : NULL;
}

static const bt_transport_t fake_transport = {
    .name = "active-input-lifecycle-host-test",
    .get_connection = fake_get_connection,
};

const bt_transport_t *bt_transport = &fake_transport;

static void fixture_source_key(uint32_t generation,
                               ns2_input_source_key_t *key)
{
    memset(key, 0, sizeof(*key));
    key->transport = INPUT_TRANSPORT_BT_BLE;
    key->dev_addr = 0u;
    key->instance = 0;
    memcpy(key->stable_addr, connection.bd_addr, sizeof(key->stable_addr));
    key->stable_addr_valid = 1u;
    key->connection_generation = generation;
}

static void register_current_source(void)
{
    bthid_device_t *device = bthid_get_device(0u);
    CHECK(device != NULL, "HID lifecycle exposes the current device to the source registry");
    if (!device) return;

    ns2_input_source_key_t key;
    fixture_source_key(device->connection_generation, &key);
    ns2_input_route_decision_t decision;
    (void)ns2_input_arbiter_submit(&arbiter, &key, device->name,
                                   device->vendor_id, device->product_id,
                                   NS2_INPUT_SOURCE_CLASS_DIRECT, &decision);
}

void bthid_on_hid_ready(uint8_t conn_index)
{
    CHECK(conn_index == 0u, "HID-ready hook carries the transport index");
    ready_hooks++;
    register_current_source();
}

void bthid_on_hid_rebind(uint8_t conn_index)
{
    CHECK(conn_index == 0u, "rebind hook carries the original transport index");
    rebind_hooks++;
    register_current_source();
}

void bthid_on_report_boundary(void) {}
void bthid_on_battery_update(input_event_t *event) { (void)event; }
void flash_on_bt_disconnect(void) {}

void bt_identity_log_record(uint8_t conn_index, bool is_ble, const char *name,
                            uint16_t vendor_id, uint16_t product_id,
                            uint16_t product_version,
                            bt_identity_provenance_t provenance,
                            const uint8_t *class_of_device, uint16_t desc_len,
                            const uint8_t *desc_bytes, const char *driver_name,
                            const char *reason, int8_t player_slot)
{
    (void)conn_index; (void)is_ble; (void)name; (void)vendor_id;
    (void)product_id; (void)product_version; (void)provenance;
    (void)class_of_device; (void)desc_len; (void)desc_bytes;
    (void)driver_name; (void)reason; (void)player_slot;
}

static void route_event(void)
{
    ns2_input_source_key_t key;
    fixture_source_key(event_storage.connection_generation, &key);
    ns2_input_route_decision_t decision;
    if (ns2_input_arbiter_submit(&arbiter, &key, connection.name,
                                 connection.vendor_id, connection.product_id,
                                 NS2_INPUT_SOURCE_CLASS_DIRECT, &decision)) {
        // The driver-specific counter is incremented by the caller; this
        // marker makes the common route observable in the fixture.
    }
}

static bool init_event(bthid_device_t *device)
{
    init_input_event(&event_storage);
    event_storage.dev_addr = device->conn_index;
    event_storage.instance = 0;
    event_storage.transport = INPUT_TRANSPORT_BT_BLE;
    device->driver_data = &event_storage;
    return true;
}

static void disconnect_event(bthid_device_t *device, unsigned *counter)
{
    (void)device;
    (*counter)++;
    router_device_disconnected_with_generation(
        event_storage.dev_addr, event_storage.instance,
        event_storage.connection_generation);
}

static void generic_disconnect(bthid_device_t *device)
{
    disconnect_event(device, &generic_disconnects);
}

static void provisional_disconnect(bthid_device_t *device)
{
    disconnect_event(device, &provisional_disconnects);
}

static void exact_disconnect(bthid_device_t *device)
{
    disconnect_event(device, &exact_disconnects);
}

static void vendor_disconnect(bthid_device_t *device)
{
    disconnect_event(device, &vendor_disconnects);
}

static void mouse_disconnect(bthid_device_t *device)
{
    disconnect_event(device, &mouse_disconnects);
}

static void ds4_disconnect(bthid_device_t *device)
{
    disconnect_event(device, &ds4_disconnects);
}

static void ds5_disconnect(bthid_device_t *device)
{
    disconnect_event(device, &ds5_disconnects);
}

static void process_common(bthid_device_t *device, unsigned *counter,
                           const uint8_t *data, uint16_t len)
{
    (void)device; (void)data; (void)len;
    (*counter)++;
    route_event();
}

static void process_generic(bthid_device_t *device, const uint8_t *data, uint16_t len)
{
    process_common(device, &generic_reports, data, len);
}

static void process_provisional(bthid_device_t *device, const uint8_t *data, uint16_t len)
{
    process_common(device, &provisional_reports, data, len);
}

static void process_exact(bthid_device_t *device, const uint8_t *data, uint16_t len)
{
    process_common(device, &exact_reports, data, len);
}

static void process_vendor(bthid_device_t *device, const uint8_t *data, uint16_t len)
{
    process_common(device, &vendor_reports, data, len);
}

static void process_mouse(bthid_device_t *device, const uint8_t *data, uint16_t len)
{
    process_common(device, &mouse_reports, data, len);
}

static void process_ds4(bthid_device_t *device, const uint8_t *data, uint16_t len)
{
    process_common(device, &ds4_reports, data, len);
}

static void process_ds5(bthid_device_t *device, const uint8_t *data, uint16_t len)
{
    process_common(device, &ds5_reports, data, len);
}

static bool match_provisional(const char *name, const uint8_t *cod,
                              uint16_t vid, uint16_t pid, bool is_ble)
{
    (void)cod; (void)pid;
    return is_ble && vid == 0u && name && strstr(name, "Provisional") != NULL;
}

static bool match_exact(const char *name, const uint8_t *cod,
                        uint16_t vid, uint16_t pid, bool is_ble)
{
    (void)name; (void)cod;
    return is_ble && vid == 0x2222u && pid == 0x0002u;
}

static bool match_vendor(const char *name, const uint8_t *cod,
                         uint16_t vid, uint16_t pid, bool is_ble)
{
    (void)cod; (void)vid; (void)pid;
    return is_ble && name && strstr(name, "Fallback") != NULL;
}

static bool match_never(const char *name, const uint8_t *cod,
                       uint16_t vid, uint16_t pid, bool is_ble)
{
    (void)name; (void)cod; (void)vid; (void)pid; (void)is_ble;
    return false;
}

static bool init_generic(bthid_device_t *device) { return init_event(device); }
static bool init_provisional(bthid_device_t *device) { return init_event(device); }
static bool init_exact(bthid_device_t *device) { return init_event(device); }
static bool init_vendor(bthid_device_t *device) { return init_event(device); }
static bool init_mouse(bthid_device_t *device) { return init_event(device); }
static bool init_ds4(bthid_device_t *device) { return init_event(device); }
static bool init_ds5(bthid_device_t *device) { return init_event(device); }

const bthid_driver_t bthid_gamepad_driver = {
    .name = "Generic lifecycle gamepad",
    .transports = BTHID_TRANSPORT_BOTH,
    .match = match_never,
    .init = init_generic,
    .process_report = process_generic,
    .disconnect = generic_disconnect,
};

const bthid_driver_t bthid_mouse_driver = {
    .name = "Generic lifecycle mouse",
    .transports = BTHID_TRANSPORT_BOTH,
    .match = match_never,
    .init = init_mouse,
    .process_report = process_mouse,
    .disconnect = mouse_disconnect,
};

const bthid_driver_t ds3_bt_driver = { .name = "DS3 lifecycle stub" };
const bthid_driver_t ds4_bt_driver = {
    .name = "DS4 lifecycle stub",
    .transports = BTHID_TRANSPORT_BLE,
    .init = init_ds4,
    .process_report = process_ds4,
    .disconnect = ds4_disconnect,
};
const bthid_driver_t ds5_bt_driver = {
    .name = "DS5 lifecycle stub",
    .transports = BTHID_TRANSPORT_BLE,
    .init = init_ds5,
    .process_report = process_ds5,
    .disconnect = ds5_disconnect,
};

static const bthid_driver_t provisional_driver = {
    .name = "Provisional lifecycle driver",
    .transports = BTHID_TRANSPORT_BLE,
    .match = match_provisional,
    .init = init_provisional,
    .process_report = process_provisional,
    .disconnect = provisional_disconnect,
};

static const bthid_driver_t exact_driver = {
    .name = "Exact lifecycle driver",
    .transports = BTHID_TRANSPORT_BLE,
    .match = match_exact,
    .init = init_exact,
    .process_report = process_exact,
    .disconnect = exact_disconnect,
};

static const bthid_driver_t vendor_driver = {
    .name = "Vendor lifecycle driver",
    .transports = BTHID_TRANSPORT_BLE,
    .match = match_vendor,
    .init = init_vendor,
    .process_report = process_vendor,
    .disconnect = vendor_disconnect,
};

void bthid_gamepad_set_descriptor(bthid_device_t *device,
                                  const uint8_t *desc, uint16_t desc_len)
{
    (void)device; (void)desc; (void)desc_len;
}

void bthid_gamepad_update_vid(bthid_device_t *device) { (void)device; }

bool bthid_mouse_descriptor_is_mouse(const uint8_t *desc, uint16_t desc_len)
{
    (void)desc; (void)desc_len;
    return descriptor_is_mouse;
}

void bthid_mouse_set_descriptor(bthid_device_t *device,
                                const uint8_t *desc, uint16_t desc_len)
{
    (void)device; (void)desc; (void)desc_len;
}

void remove_players_by_address(int dev_addr, int instance)
{
    (void)dev_addr; (void)instance;
}

void router_device_disconnected(uint8_t dev_addr, int8_t instance)
{
    router_device_disconnected_with_generation(dev_addr, instance, 0u);
}

void router_device_disconnected_with_generation(uint8_t dev_addr,
                                                int8_t instance,
                                                uint32_t connection_generation)
{
    if (bthid_rebind_in_progress()) return;
    ns2_input_source_key_t key;
    fixture_source_key(connection_generation, &key);
    key.dev_addr = dev_addr;
    key.instance = instance;
    bool was_active = false;
    (void)ns2_input_arbiter_disconnect(&arbiter, &key, &was_active);
}

int find_player_index(int dev_addr, int instance)
{
    (void)dev_addr; (void)instance;
    return 0;
}

static uint32_t active_id(void)
{
    ns2_input_arbiter_status_t status;
    ns2_input_arbiter_get_status(&arbiter, &status);
    return status.active_id;
}

static uint32_t transition_count(void)
{
    ns2_input_arbiter_status_t status;
    ns2_input_arbiter_get_status(&arbiter, &status);
    return status.transition_count;
}

static bool owner_preserved(uint32_t id, uint32_t transitions,
                            const char *message)
{
    ns2_input_arbiter_status_t status;
    ns2_input_arbiter_get_status(&arbiter, &status);
    bool ok = status.active_id == id && status.source_count == 1u &&
              status.transition_count == transitions &&
              status.awaiting_fresh == 0u;
    CHECK(ok, message);
    return ok;
}

static void reset_fixture(const char *name)
{
    memset(&connection, 0, sizeof(connection));
    connection.connected = true;
    connection.hid_ready = true;
    connection.is_ble = true;
    connection.bd_addr[0] = 0x10;
    connection.bd_addr[1] = 0x20;
    connection.bd_addr[2] = 0x30;
    connection.bd_addr[3] = 0x40;
    connection.bd_addr[4] = 0x50;
    connection.bd_addr[5] = 0x60;
    strncpy(connection.name, name, sizeof(connection.name) - 1u);
    descriptor_is_mouse = false;
    generic_reports = provisional_reports = exact_reports = 0u;
    vendor_reports = mouse_reports = ds4_reports = ds5_reports = 0u;
    generic_disconnects = provisional_disconnects = exact_disconnects = 0u;
    vendor_disconnects = mouse_disconnects = ds4_disconnects = ds5_disconnects = 0u;
    ready_hooks = rebind_hooks = 0u;
    ns2_input_arbiter_init(&arbiter);
    bthid_init();
}

static void send_report(uint32_t generation, uint8_t report_id)
{
    const uint8_t report[] = { 0xA1, report_id, 0x55 };
    bt_on_hid_report_with_generation(0u, generation, report, sizeof(report));
}

static void begin_ready(void)
{
    bt_on_hid_ready(0u);
    CHECK(ready_hooks >= 1u, "HID-ready lifecycle callback registers the source");
}

static void test_late_identity_rebind_preserves_owner(void)
{
    reset_fixture("Provisional Pad");
    bthid_register_driver(&provisional_driver);
    bthid_register_driver(&exact_driver);
    begin_ready();
    bthid_device_t *device = bthid_get_device(0u);
    uint32_t generation = device ? device->connection_generation : 0u;
    uint32_t id = active_id();
    uint32_t transitions = transition_count();
    send_report(generation, 0x01u);
    connection.vendor_id = 0x2222u;
    connection.product_id = 0x0002u;
    bthid_update_device_info(0u, connection.name,
                             connection.vendor_id, connection.product_id);
    CHECK(device && device->driver == &exact_driver,
          "late VID/PID selects the exact replacement driver");
    CHECK(provisional_disconnects == 1u && rebind_hooks == 1u,
          "late VID/PID uses rebind cleanup and one source refresh");
    owner_preserved(id, transitions,
                    "late VID/PID rebind preserves the active source identity");
    send_report(generation, 0x01u);
    CHECK(exact_reports == 1u, "input continues after late VID/PID rebind");
}

static void test_sony_reclassification_preserves_owner(void)
{
    reset_fixture("Sony Pad");
    begin_ready();
    bthid_device_t *device = bthid_get_device(0u);
    uint32_t generation = device ? device->connection_generation : 0u;
    uint32_t id = active_id();
    uint32_t transitions = transition_count();
    send_report(generation, 0x31u);
    CHECK(device && device->driver == &ds5_bt_driver,
          "Sony report 0x31 reclassifies to the DS5 driver");
    owner_preserved(id, transitions,
                    "DS5 reclassification preserves the active source identity");
    send_report(generation, 0x31u);
    CHECK(ds5_reports == 1u, "input continues through the DS5 replacement driver");
    send_report(generation, 0x11u);
    CHECK(device && device->driver == &ds4_bt_driver,
          "Sony report 0x11 reclassifies to the DS4 driver");
    owner_preserved(id, transitions,
                    "DS4 reclassification preserves the active source identity");
    send_report(generation, 0x11u);
    CHECK(ds4_reports == 1u, "input continues through the DS4 replacement driver");
}

static void test_descriptor_rebind_preserves_owner(void)
{
    reset_fixture("Descriptor Mouse");
    begin_ready();
    bthid_device_t *device = bthid_get_device(0u);
    uint32_t id = active_id();
    uint32_t transitions = transition_count();
    descriptor_is_mouse = true;
    const uint8_t descriptor[] = { 0x05, 0x01, 0x09, 0x02 };
    bthid_set_hid_descriptor(0u, descriptor, sizeof(descriptor));
    CHECK(device && device->driver == &bthid_mouse_driver,
          "mouse descriptor reclassifies generic gamepad to mouse");
    owner_preserved(id, transitions,
                    "descriptor reclassification preserves the active source identity");
    uint32_t generation = device ? device->connection_generation : 0u;
    send_report(generation, 0x01u);
    CHECK(mouse_reports == 1u, "input continues through descriptor-selected mouse driver");
}

static void test_fallback_rebind_preserves_owner(void)
{
    reset_fixture("Fallback Pad");
    bthid_register_driver(&vendor_driver);
    begin_ready();
    bthid_device_t *device = bthid_get_device(0u);
    uint32_t id = active_id();
    uint32_t transitions = transition_count();
    bthid_fallback_to_generic(0u);
    CHECK(device && device->driver == &bthid_gamepad_driver,
          "vendor fallback selects the generic gamepad driver");
    owner_preserved(id, transitions,
                    "vendor fallback preserves the active source identity");
    uint32_t generation = device ? device->connection_generation : 0u;
    send_report(generation, 0x01u);
    CHECK(generic_reports == 1u, "input continues after vendor fallback");
}

static void test_stale_transport_callbacks_are_rejected(void)
{
    reset_fixture("Original Pad");
    begin_ready();
    bthid_device_t *old_device = bthid_get_device(0u);
    uint32_t old_generation = old_device ? old_device->connection_generation : 0u;
    send_report(old_generation, 0x01u);
    unsigned reports_before_stale = generic_reports;
    bt_on_disconnect_with_generation(0u, old_generation);

    strncpy(connection.name, "Replacement Pad", sizeof(connection.name) - 1u);
    connection.bd_addr[0] = 0xAA;
    begin_ready();
    bthid_device_t *new_device = bthid_get_device(0u);
    uint32_t new_generation = new_device ? new_device->connection_generation : 0u;
    CHECK(new_generation != 0u && new_generation != old_generation,
          "transport index reuse receives a new lifecycle generation");
    uint32_t replacement_id = 0u;
    ns2_input_arbiter_status_t status;
    ns2_input_arbiter_get_status(&arbiter, &status);
    if (status.source_count == 1u) replacement_id = status.sources[0].id;

    send_report(old_generation, 0x01u);
    CHECK(generic_reports == reports_before_stale,
          "stale report token cannot publish through the replacement occupant");
    bt_on_disconnect_with_generation(0u, old_generation);
    CHECK(bthid_get_device(0u) == new_device,
          "stale disconnect token cannot remove the replacement occupant");
    ns2_input_arbiter_get_status(&arbiter, &status);
    CHECK(status.source_count == 1u && status.sources[0].id == replacement_id,
          "stale disconnect leaves the replacement source registered");

    CHECK(ns2_input_arbiter_request_active(&arbiter, replacement_id),
          "replacement can be explicitly selected after the old lifecycle loss");
    send_report(new_generation, 0x01u);
    ns2_input_arbiter_get_status(&arbiter, &status);
    CHECK(status.active_id == replacement_id && status.awaiting_fresh == 0u,
          "replacement selection keeps the neutral/fresh boundary after reuse");
}

int main(void)
{
    test_late_identity_rebind_preserves_owner();
    test_sony_reclassification_preserves_owner();
    test_descriptor_rebind_preserves_owner();
    test_fallback_rebind_preserves_owner();
    test_stale_transport_callbacks_are_rejected();
    if (failures) {
        printf("ns2 active-input lifecycle: %d failure(s)\n", failures);
        return 1;
    }
    puts("ns2 active-input lifecycle: all tests passed");
    return 0;
}
