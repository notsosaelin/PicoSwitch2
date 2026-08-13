#include "battlergc_pro.h"

#include <stdio.h>
#include <string.h>

#include "battlergc_pro_report.h"
#include "bt/bthid/devices/vendors/microsoft/xbox_rumble.h"
#include "core/buttons.h"
#include "core/input_event.h"
#include "core/router/router.h"
#include "core/services/players/feedback.h"
#include "core/services/players/manager.h"

typedef struct {
    input_event_t event;
    bool initialized;
    bool home_pressed;
    uint8_t rumble_left;
    uint8_t rumble_right;
} battlergc_pro_data_t;

static battlergc_pro_data_t battler_data[BTHID_MAX_DEVICES];

static bool battlergc_match(const char *device_name, const uint8_t *class_of_device,
                            uint16_t vendor_id, uint16_t product_id, bool is_ble)
{
    (void)class_of_device;
    if (is_ble || vendor_id != 0 || product_id != 0 || !device_name) return false;
    return strcmp(device_name, "Xbox Wireless Controller") == 0;
}

static bool battlergc_init(bthid_device_t *device)
{
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (battler_data[i].initialized) continue;

        battlergc_pro_data_t *data = &battler_data[i];
        memset(data, 0, sizeof(*data));
        init_input_event(&data->event);
        data->initialized = true;
        data->event.type = INPUT_TYPE_GAMEPAD;
        data->event.transport = INPUT_TRANSPORT_BT_CLASSIC;
        data->event.dev_addr = device->conn_index;
        data->event.instance = 0;
        data->event.button_count = 11;
        data->event.layout = LAYOUT_GAMECUBE;
        data->event.gc_has_native_layout = true;
        device->driver_data = data;
        printf("[BATTLERGC] Native Retro Fighters profile active\n");
        return true;
    }
    return false;
}

static void battlergc_process_report(bthid_device_t *device,
                                     const uint8_t *report, uint16_t len)
{
    battlergc_pro_data_t *data = (battlergc_pro_data_t *)device->driver_data;
    if (!data) return;

    bool home_pressed;
    if (battlergc_pro_decode_home_report(report, len, &home_pressed)) {
        data->home_pressed = home_pressed;
        if (home_pressed)
            data->event.buttons |= JP_BUTTON_A1;
        else
            data->event.buttons &= ~JP_BUTTON_A1;
        router_submit_input(&data->event);
        return;
    }

    battlergc_pro_decoded_report_t decoded;
    if (!battlergc_pro_decode_report(report, len, &decoded)) return;

    data->event.buttons = decoded.buttons;
    if (data->home_pressed) data->event.buttons |= JP_BUTTON_A1;
    data->event.analog[ANALOG_LX] = decoded.lx;
    data->event.analog[ANALOG_LY] = decoded.ly;
    data->event.analog[ANALOG_RX] = decoded.rx;
    data->event.analog[ANALOG_RY] = decoded.ry;
    data->event.analog[ANALOG_L2] = decoded.lt;
    data->event.analog[ANALOG_R2] = decoded.rt;
    data->event.gc_native_zl = decoded.gc_native_zl;
    data->event.gc_native_z = decoded.gc_native_z;
    data->event.gc_l_detent = decoded.gc_l_detent;
    data->event.gc_r_detent = decoded.gc_r_detent;
    router_submit_input(&data->event);
}

static void battlergc_task(bthid_device_t *device)
{
    battlergc_pro_data_t *data = (battlergc_pro_data_t *)device->driver_data;
    if (!data) return;

    int player_idx = find_player_index(data->event.dev_addr, data->event.instance);
    if (player_idx < 0) return;
    feedback_state_t *fb = feedback_get_state(player_idx);
    if (!fb || !fb->rumble_dirty) return;

    uint8_t left = fb->rumble.left;
    uint8_t right = fb->rumble.right;
    if (left != data->rumble_left || right != data->rumble_right) {
        uint8_t payload[XBOX_RUMBLE_DATA_LEN];
        xbox_rumble_build_payload(left, right, payload);
        if (!bthid_send_output_report(device->conn_index, XBOX_RUMBLE_REPORT_ID,
                                      payload, sizeof(payload))) {
            return;
        }
        data->rumble_left = left;
        data->rumble_right = right;
    }
    feedback_clear_dirty(player_idx);
}

static void battlergc_disconnect(bthid_device_t *device)
{
    battlergc_pro_data_t *data = (battlergc_pro_data_t *)device->driver_data;
    if (!data) return;

    router_device_disconnected_with_generation(data->event.dev_addr,
                                               data->event.instance,
                                               data->event.connection_generation);
    remove_players_by_address(data->event.dev_addr, data->event.instance);
    memset(data, 0, sizeof(*data));
}

const bthid_driver_t battlergc_pro_driver = {
    .name = "Retro Fighters BattlerGC Pro",
    .transports = BTHID_TRANSPORT_CLASSIC,
    .match = battlergc_match,
    .init = battlergc_init,
    .process_report = battlergc_process_report,
    .task = battlergc_task,
    .disconnect = battlergc_disconnect,
};

void battlergc_pro_register(void)
{
    bthid_register_driver(&battlergc_pro_driver);
}
