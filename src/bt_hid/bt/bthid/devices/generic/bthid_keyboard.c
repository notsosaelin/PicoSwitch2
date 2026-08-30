// Generic Bluetooth HID keyboard driver.
//
// The driver's only job is to decide WHICH KEYS ARE HELD. What those keys mean
// is decided one layer up, by the KB/M mapping model — a Bluetooth report
// parser must never contain a controller mapping.
//
// A peer whose descriptor also declares a pointer collection is a combo
// keyboard+mouse. Rather than forcing such a device to occupy two Bluetooth
// connections, this driver decodes both report shapes and hands the pointer
// half to the ordinary mouse path, so one peer can fill both KB/M roles.

#include "bt/bthid/devices/generic/bthid_keyboard.h"

#include <stdio.h>
#include <string.h>

#include "bt/bthid/devices/generic/bthid_keyboard_report.h"
#include "bt/bthid/devices/generic/bthid_mouse_report.h"
#include "bt/transport/bt_transport.h"  // Class of Device for combo detection
#include "core/buttons.h"
#include "core/input_event.h"
#include "core/router/router.h"
#include "core/services/players/manager.h"
#include "ns2_kbm.h"
#include "ns2_kbm_runtime.h"

typedef struct {
    input_event_t event;
    bthid_keyboard_report_map_t map;
    bthid_mouse_report_map_t pointer;
    uint8_t usages[BTHID_KEYBOARD_USAGE_BYTES];
    bool initialized;
    bool has_report_map;
    bool has_pointer;
} bthid_keyboard_data_t;

static bthid_keyboard_data_t keyboard_data[BTHID_MAX_DEVICES];

// Class of Device peripheral types: 0x01 keyboard, 0x03 combo keyboard/pointer.
// BLE has no Class of Device, so BLE keyboards are claimed structurally by
// bthid.c's descriptor reclassifier instead of by this match().
static bool classic_cod_is_keyboard(const uint8_t *cod) {
    if (!cod) return false;
    uint8_t major_class = cod[1] & 0x1F;
    uint8_t minor_class = (cod[0] >> 2) & 0x3F;
    uint8_t peripheral_type = (minor_class >> 4) & 0x03;
    return major_class == 0x05 &&
           (peripheral_type == 0x01 || peripheral_type == 0x03);
}

// Peripheral minor class 0x03 is "combo keyboard/pointing": the device stating
// it supplies both roles. This is the ONLY positive combo evidence available,
// and it is Classic-only -- BLE has no Class of Device.
static bool classic_cod_declares_combo(const bthid_device_t *device) {
    if (!device || device->is_ble) return false;
    const bt_connection_t *conn = bt_get_connection(device->conn_index);
    if (!conn) return false;
    const uint8_t *cod = conn->class_of_device;
    uint8_t major_class = cod[1] & 0x1F;
    uint8_t minor_class = (cod[0] >> 2) & 0x3F;
    return major_class == 0x05 && ((minor_class >> 4) & 0x03) == 0x03;
}

static bool keyboard_match(const char *device_name, const uint8_t *class_of_device,
                           uint16_t vendor_id, uint16_t product_id, bool is_ble) {
    (void)device_name;
    (void)vendor_id;
    (void)product_id;
    // Deliberately never name-matched. A gamepad whose descriptor happens to
    // contain unusual usages, or whose product name contains "board", must not
    // be claimed here.
    return is_ble ? false : classic_cod_is_keyboard(class_of_device);
}

static bool keyboard_init(bthid_device_t *device) {
    for (int i = 0; i < BTHID_MAX_DEVICES; ++i) {
        if (keyboard_data[i].initialized) continue;
        bthid_keyboard_data_t *kb = &keyboard_data[i];
        memset(kb, 0, sizeof(*kb));
        init_input_event(&kb->event);
        kb->initialized = true;
        kb->event.type = INPUT_TYPE_KEYBOARD;
        kb->event.transport = device->is_ble ? INPUT_TRANSPORT_BT_BLE
                                             : INPUT_TRANSPORT_BT_CLASSIC;
        kb->event.dev_addr = device->conn_index;
        kb->event.instance = 0;
        kb->event.connection_generation = device->connection_generation;
        device->type = BTHID_DEVICE_KEYBOARD;
        device->driver_data = kb;
        printf("[BTHID_KEYBOARD] Init: %s\n", device->name);
        return true;
    }
    return false;
}

bool bthid_keyboard_descriptor_is_keyboard(const uint8_t *desc, uint16_t desc_len) {
    bthid_keyboard_report_map_t map;
    return bthid_keyboard_parse_descriptor(desc, desc_len, &map);
}

void bthid_keyboard_set_descriptor(bthid_device_t *device, const uint8_t *desc,
                                   uint16_t desc_len) {
    bthid_keyboard_data_t *kb = device ? device->driver_data : NULL;
    if (!kb) return;
    kb->has_report_map = bthid_keyboard_parse_descriptor(desc, desc_len, &kb->map);
    kb->has_pointer = bthid_mouse_parse_descriptor(desc, desc_len, &kb->pointer);
    bthid_keyboard_shape_t shape = bthid_keyboard_shape(&kb->map);
    if (kb->has_report_map) {
        printf("[BTHID_KEYBOARD] Parsed report %u: %u bitmap field(s), "
               "%u array field(s), pointer=%d\n",
               kb->map.report_id, kb->map.bitmap_count, kb->map.array_count,
               kb->has_pointer ? 1 : 0);
    }
    // Report capability, and separately whether the device POSITIVELY declares
    // itself a combined keyboard+pointing peripheral.
    //
    // "Has both capabilities" is emphatically not that declaration. An ASUS ROG
    // KERIS II gaming mouse parses as kbcap=true AND mousecap=true, because its
    // macro buttons put a keyboard collection in its descriptor -- yet it is a
    // mouse, and treating it as a combo would let it occupy the keyboard role
    // and lock out the user's actual keyboard. Only the Class-of-Device combo
    // peripheral type is a statement by the device itself; BLE has no Class of
    // Device, so a BLE peer is never classified combo here.
    bool declares_combo = classic_cod_declares_combo(device);

    // One line, only when the answer was not obvious -- a keyboard with no
    // pointer needs no explanation, and this runs on every descriptor arrival.
    // Says WHY, because a support report that only shows the outcome cannot
    // distinguish "misclassified" from "correctly classified, wrong device".
    if (kb->has_pointer) {
        printf("[BTHID_KEYBOARD] kb+pointer: primary_usage=0x%02X strong=%d "
               "modifier=%d rollover=%u bitmap=%u cod_combo=%d -> %s\n",
               kb->map.primary_application_usage,
               shape.strong_keyboard ? 1 : 0,
               shape.has_modifier_byte ? 1 : 0,
               shape.rollover_slots, shape.key_bitmap_bits,
               declares_combo ? 1 : 0,
               (declares_combo || shape.strong_keyboard)
                   ? "COMBO"
                   : "MOUSE (macro-safe fallback)");
    }

    ns2_kbm_runtime_note_classification(device->conn_index,
                                        device->connection_generation,
                                        true, kb->has_pointer,
                                        declares_combo, shape.strong_keyboard);
}

// Mouse buttons are reported in HID Usage Page 0x09 order. The generic mouse
// driver's own normalization is reused verbatim so a combo peer and a separate
// mouse reach the KB/M model through exactly the same button encoding.
static uint32_t normalize_pointer_buttons(uint16_t buttons) {
    uint32_t out = 0;
    if (buttons & (1u << 0)) out |= JP_BUTTON_L2;
    if (buttons & (1u << 1)) out |= JP_BUTTON_R2;
    if (buttons & (1u << 2)) out |= JP_BUTTON_A1;
    if (buttons & (1u << 3)) out |= JP_BUTTON_B3;
    if (buttons & (1u << 4)) out |= JP_BUTTON_B1;
    return out;
}

static void keyboard_process_report(bthid_device_t *device, const uint8_t *data,
                                    uint16_t len) {
    bthid_keyboard_data_t *kb = device ? device->driver_data : NULL;
    if (!kb || !data || len == 0) return;
    kb->event.connection_generation = device->connection_generation;

    bthid_keyboard_decode_t decoded = BTHID_KEYBOARD_DECODE_FAIL;
    if (kb->has_report_map)
        decoded = bthid_keyboard_decode_report(&kb->map, data, len, kb->usages);
    if (decoded == BTHID_KEYBOARD_DECODE_FAIL && !kb->has_report_map)
        decoded = bthid_keyboard_decode_boot(data, len, kb->usages);

    if (decoded == BTHID_KEYBOARD_DECODE_ROLLOVER) {
        // The keyboard cannot say which keys are down. Retaining the previous
        // held set is the only safe answer: replacing it with an empty one
        // would release every control mid-input.
        ns2_kbm_runtime_note_rollover();
        return;
    }
    if (decoded == BTHID_KEYBOARD_DECODE_OK) {
        router_submit_keyboard_input(&kb->event, kb->usages);
        return;
    }

    // Not a keyboard report. A combo peer interleaves pointer reports on the
    // same connection; route those through the ordinary mouse path.
    if (!kb->has_pointer) return;
    bthid_mouse_report_t pointer;
    if (!bthid_mouse_decode_report(&kb->pointer, data, len, &pointer)) return;

    input_event_t mouse_event = kb->event;
    mouse_event.type = INPUT_TYPE_MOUSE;
    mouse_event.buttons = normalize_pointer_buttons(pointer.buttons);
    mouse_event.hid_buttons = pointer.buttons;
    mouse_event.delta_x = pointer.delta_x;
    mouse_event.delta_y = pointer.delta_y;
    mouse_event.delta_wheel = pointer.wheel;
    router_submit_input(&mouse_event);
}

static void keyboard_disconnect(bthid_device_t *device) {
    bthid_keyboard_data_t *kb = device ? device->driver_data : NULL;
    if (!kb) return;
    router_device_disconnected_with_generation(kb->event.dev_addr,
                                               kb->event.instance,
                                               kb->event.connection_generation);
    remove_players_by_address(kb->event.dev_addr, kb->event.instance);
    memset(kb, 0, sizeof(*kb));
}

const bthid_driver_t bthid_keyboard_driver = {
    .name = "Generic BT Keyboard",
    .transports = BTHID_TRANSPORT_BOTH,
    .match = keyboard_match,
    .init = keyboard_init,
    .process_report = keyboard_process_report,
    .task = NULL,
    .disconnect = keyboard_disconnect,
};

void bthid_keyboard_register(void) {
    bthid_register_driver(&bthid_keyboard_driver);
}
