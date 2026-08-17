#include "bt/bthid/devices/generic/bthid_mouse.h"

#include <stdio.h>
#include <string.h>

#include "bt/bthid/devices/generic/bthid_mouse_report.h"
#include "core/buttons.h"
#include "core/input_event.h"
#include "core/router/router.h"
#include "core/services/players/manager.h"

typedef struct {
    input_event_t event;
    bthid_mouse_report_map_t map;
    bool initialized;
    bool has_report_map;
} bthid_mouse_data_t;

static bthid_mouse_data_t mouse_data[BTHID_MAX_DEVICES];

static bool classic_cod_is_mouse(const uint8_t *cod) {
    if (!cod) return false;
    uint8_t major_class = cod[1] & 0x1F;
    uint8_t minor_class = (cod[0] >> 2) & 0x3F;
    uint8_t peripheral_type = (minor_class >> 4) & 0x03;
    return major_class == 0x05 && peripheral_type == 0x02;
}

static bool contains_mouse_name(const char *name) {
    return name && (strstr(name, "Mouse") || strstr(name, "mouse"));
}

static bool mouse_match(const char *device_name, const uint8_t *class_of_device,
                        uint16_t vendor_id, uint16_t product_id, bool is_ble) {
    (void)vendor_id;
    (void)product_id;
    return is_ble ? contains_mouse_name(device_name) : classic_cod_is_mouse(class_of_device);
}

static bool mouse_init(bthid_device_t *device) {
    for (int i = 0; i < BTHID_MAX_DEVICES; ++i) {
        if (mouse_data[i].initialized) continue;
        init_input_event(&mouse_data[i].event);
        mouse_data[i].initialized = true;
        mouse_data[i].event.type = INPUT_TYPE_MOUSE;
        mouse_data[i].event.transport = device->is_ble
            ? INPUT_TRANSPORT_BT_BLE : INPUT_TRANSPORT_BT_CLASSIC;
        mouse_data[i].event.dev_addr = device->conn_index;
        mouse_data[i].event.instance = 0;
        device->type = BTHID_DEVICE_MOUSE;
        device->driver_data = &mouse_data[i];
        printf("[BTHID_MOUSE] Init: %s\n", device->name);
        return true;
    }
    return false;
}

bool bthid_mouse_descriptor_is_mouse(const uint8_t *desc, uint16_t desc_len) {
    bthid_mouse_report_map_t map;
    return bthid_mouse_parse_descriptor(desc, desc_len, &map);
}

void bthid_mouse_set_descriptor(bthid_device_t *device,
                                const uint8_t *desc, uint16_t desc_len) {
    bthid_mouse_data_t *mouse = device ? device->driver_data : NULL;
    if (!mouse) return;
    mouse->has_report_map = bthid_mouse_parse_descriptor(desc, desc_len, &mouse->map);
    if (mouse->has_report_map) {
        printf("[BTHID_MOUSE] Parsed report %u: %u buttons, X@%u/%u Y@%u/%u wheel=%d\n",
               mouse->map.report_id, mouse->map.button_count,
               mouse->map.x.bit_offset, mouse->map.x.bit_size,
               mouse->map.y.bit_offset, mouse->map.y.bit_size,
               mouse->map.wheel.present);
    }
}

static uint32_t normalize_mouse_buttons(uint16_t buttons) {
    uint32_t out = 0;
    // Match the existing single-Joy-Con generic profile: left/right click are
    // the top shoulder and trigger; middle is Home. Back/Forward become B/A on
    // the common Right Joy-Con personality and remain remappable later.
    if (buttons & (1u << 0)) out |= JP_BUTTON_L2;
    if (buttons & (1u << 1)) out |= JP_BUTTON_R2;
    if (buttons & (1u << 2)) out |= JP_BUTTON_A1;
    if (buttons & (1u << 3)) out |= JP_BUTTON_B3;
    if (buttons & (1u << 4)) out |= JP_BUTTON_B1;
    if (buttons & (1u << 5)) out |= JP_BUTTON_L4;
    if (buttons & (1u << 6)) out |= JP_BUTTON_R4;
    if (buttons & (1u << 7)) out |= JP_BUTTON_A3;
    return out;
}

static void mouse_process_report(bthid_device_t *device,
                                 const uint8_t *data, uint16_t len) {
    bthid_mouse_data_t *mouse = device ? device->driver_data : NULL;
    if (!mouse || !data) return;

    bthid_mouse_report_t report;
    bool decoded = mouse->has_report_map &&
                   bthid_mouse_decode_report(&mouse->map, data, len, &report);
    if (!decoded) {
        // Boot-report fallback for simple Classic mice whose descriptor could
        // not be obtained: [buttons][dx][dy][optional wheel].
        if (mouse->has_report_map || len < 3) return;
        report.buttons = data[0] & 0x07;
        report.delta_x = (int8_t)data[1];
        report.delta_y = (int8_t)data[2];
        report.wheel = len > 3 ? (int8_t)data[3] : 0;
    }

    mouse->event.buttons = normalize_mouse_buttons(report.buttons);
    // Preserve the HID button numbering alongside the lossy gamepad projection
    // above: the KB/M mapping model binds mouse controls by button number, and
    // recovering that from JP_BUTTON_* would be an inverse of a mapping that is
    // free to change.
    mouse->event.hid_buttons = report.buttons;
    mouse->event.button_count = mouse->has_report_map ? mouse->map.button_count : 3;
    mouse->event.delta_x = report.delta_x;
    mouse->event.delta_y = report.delta_y;
    mouse->event.delta_wheel = report.wheel;
    router_submit_input(&mouse->event);
    // Relative fields are one-shot. A later BAS update or other republish must
    // not replay this movement.
    mouse->event.delta_x = 0;
    mouse->event.delta_y = 0;
    mouse->event.delta_wheel = 0;
}

static void mouse_disconnect(bthid_device_t *device) {
    bthid_mouse_data_t *mouse = device ? device->driver_data : NULL;
    if (!mouse) return;
    router_device_disconnected_with_generation(mouse->event.dev_addr,
                                               mouse->event.instance,
                                               mouse->event.connection_generation);
    remove_players_by_address(mouse->event.dev_addr, mouse->event.instance);
    init_input_event(&mouse->event);
    memset(&mouse->map, 0, sizeof(mouse->map));
    mouse->has_report_map = false;
    mouse->initialized = false;
}

const bthid_driver_t bthid_mouse_driver = {
    .name = "Generic BT Mouse",
    .transports = BTHID_TRANSPORT_BOTH,
    .match = mouse_match,
    .init = mouse_init,
    .process_report = mouse_process_report,
    .task = NULL,
    .disconnect = mouse_disconnect,
};

void bthid_mouse_register(void) {
    bthid_register_driver(&bthid_mouse_driver);
}
