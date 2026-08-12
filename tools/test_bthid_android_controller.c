/*
 * Host contract test for the no-root Android Controller Bridge HID path.
 *
 * This compiles the production generic Bluetooth gamepad driver and HID
 * descriptor parser against the descriptor Android will register.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
 *   -ffunction-sections -fdata-sections \
 *   -Itools/host_stubs -Isrc -Isrc/bt_hid -Iinclude -Itools \
 *   tools/test_bthid_android_controller.c \
 *   src/bt_hid/bt/bthid/devices/generic/bthid_gamepad.c \
 *   src/bt_hid/bt/bthid/devices/generic/bthid_gamepad_quirks.c \
 *   src/bt_hid/usb/usbh/hid/devices/generic/hid_parser.c \
 *   -Wl,--gc-sections -o build/host-tests/test_bthid_android_controller.exe
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt/bthid/bthid.h"
#include "bt/bthid/devices/generic/bthid_gamepad.h"
#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"
#include "core/router/router.h"
#include "core/services/players/feedback.h"
#include "fixtures/android_controller_hid.h"

static int failures;
static bthid_device_t device;
static input_event_t last_event;
static unsigned submitted;
static unsigned disconnected;
static unsigned players_removed;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) printf("OK:   %s\n", message);                         \
        else { printf("FAIL: %s\n", message); failures++; }                  \
    } while (0)

// The generic registry references all named profiles even though this fixture
// intentionally resolves to its plain sequential fallback.
const gamepad_quirk_t QUIRK_BITDO_NGC_MODKIT = {0};
const gamepad_quirk_t QUIRK_BITDO_ULTIMATE_MG = {0};
const gamepad_quirk_t QUIRK_BITDO_M30 = {0};
const gamepad_quirk_t QUIRK_BITDO_PADDLE = {0};
const gamepad_quirk_t QUIRK_XBOX_ELITE2 = {0};
const gamepad_quirk_t QUIRK_XBOX = {0};

void router_submit_input(const input_event_t *event)
{
    last_event = *event;
    submitted++;
}

void router_device_disconnected(uint8_t dev_addr, int8_t instance)
{
    CHECK(dev_addr == device.conn_index && instance == 0,
          "disconnect clears the Android controller's router identity");
    disconnected++;
}

void remove_players_by_address(int dev_addr, int instance)
{
    CHECK(dev_addr == device.conn_index && instance == 0,
          "disconnect removes the Android controller's player assignment");
    players_removed++;
}

int find_player_index(int dev_addr, int instance)
{
    (void)dev_addr;
    (void)instance;
    return -1;
}

feedback_state_t *feedback_get_state(uint8_t player_index)
{
    (void)player_index;
    return NULL;
}

void feedback_clear_dirty(uint8_t player_index) { (void)player_index; }
void bthid_register_driver(const bthid_driver_t *driver) { (void)driver; }

bthid_device_t *bthid_get_device(uint8_t conn_index)
{
    return device.active && device.conn_index == conn_index ? &device : NULL;
}

static void send_report(const uint8_t report[ANDROID_CONTROLLER_WIRE_REPORT_LEN])
{
    bthid_gamepad_driver.process_report(&device, report,
                                         ANDROID_CONTROLLER_WIRE_REPORT_LEN);
}

static void test_descriptor_and_neutral(void)
{
    memset(&device, 0, sizeof(device));
    device.active = true;
    device.conn_index = 3;
    device.is_ble = false;
    device.driver = &bthid_gamepad_driver;
    strcpy(device.name, "PicoSwitch2 Android Bridge");

    CHECK(bthid_gamepad_driver.init(&device),
          "production generic gamepad driver initializes for Classic HID");
    bthid_gamepad_set_descriptor(&device, ANDROID_CONTROLLER_HID_DESCRIPTOR,
                                  sizeof(ANDROID_CONTROLLER_HID_DESCRIPTOR));

    char map[1024];
    CHECK(bthid_gamepad_dump_map(device.conn_index, map, sizeof(map)),
          "canonical Android descriptor parses through the production HID parser");
    CHECK(strstr(map, "\"report_id\":1") &&
          strstr(map, "\"button_cnt\":14") &&
          strstr(map, "\"x\":{\"byte\":1") &&
          strstr(map, "\"hat\":{\"byte\":9"),
          "parsed offsets match the 10-byte wire contract");

    send_report(ANDROID_CONTROLLER_NEUTRAL_REPORT);
    CHECK(submitted == 1, "complete neutral snapshot reaches the router once");
    CHECK(last_event.type == INPUT_TYPE_GAMEPAD &&
          last_event.transport == INPUT_TRANSPORT_BT_CLASSIC,
          "Android input retains gamepad type and Classic transport");
    CHECK(last_event.buttons == 0 && last_event.analog[ANALOG_LX] == 128 &&
          last_event.analog[ANALOG_LY] == 128 &&
          last_event.analog[ANALOG_RX] == 128 &&
          last_event.analog[ANALOG_RY] == 128 &&
          last_event.analog[ANALOG_L2] == 0 &&
          last_event.analog[ANALOG_R2] == 0,
          "neutral axes, triggers, buttons, and hat remain neutral");
}

static void test_full_state_and_atomic_rejection(void)
{
    uint8_t report[ANDROID_CONTROLLER_WIRE_REPORT_LEN] = {
        ANDROID_CONTROLLER_REPORT_ID,
        0, 255, 255, 0, 64, 192,
        0x21, 0x22, // usages 1, 6, 10, and 14
        1           // north-east
    };
    send_report(report);
    CHECK(submitted == 2, "non-neutral complete snapshot reaches the router");
    CHECK(last_event.analog[ANALOG_LX] == 0 &&
          last_event.analog[ANALOG_LY] == 255 &&
          last_event.analog[ANALOG_RX] == 255 &&
          last_event.analog[ANALOG_RY] == 0 &&
          last_event.analog[ANALOG_L2] == 64 &&
          last_event.analog[ANALOG_R2] == 192,
          "all six unsigned axes preserve endpoints and trigger values");
    const uint32_t expected = JP_BUTTON_B1 | JP_BUTTON_R1 | JP_BUTTON_S2 |
                              JP_BUTTON_A2 | JP_BUTTON_DU | JP_BUTTON_DR;
    CHECK(last_event.buttons == expected,
          "sequential buttons and diagonal hat map to canonical input bits");

    report[1] = 17;
    send_report(report);
    CHECK(submitted == 3 && last_event.analog[ANALOG_LX] == 17 &&
          last_event.buttons == expected,
          "an axis update preserves every held button in the full snapshot");

    unsigned before = submitted;
    report[0] = 2;
    bthid_gamepad_driver.process_report(&device, report, sizeof(report));
    CHECK(submitted == before, "wrong report ID is ignored atomically");

    report[0] = ANDROID_CONTROLLER_REPORT_ID;
    bthid_gamepad_driver.process_report(&device, report, sizeof(report) - 1);
    CHECK(submitted == before, "truncated descriptor-backed report is ignored atomically");
}

static void test_every_button_and_hat(void)
{
    static const uint32_t button_map[14] = {
        JP_BUTTON_B1, JP_BUTTON_B2, JP_BUTTON_B3, JP_BUTTON_B4,
        JP_BUTTON_L1, JP_BUTTON_R1, JP_BUTTON_L2, JP_BUTTON_R2,
        JP_BUTTON_S1, JP_BUTTON_S2, JP_BUTTON_L3, JP_BUTTON_R3,
        JP_BUTTON_A1, JP_BUTTON_A2,
    };
    static const uint32_t hat_map[9] = {
        JP_BUTTON_DU,
        JP_BUTTON_DU | JP_BUTTON_DR,
        JP_BUTTON_DR,
        JP_BUTTON_DR | JP_BUTTON_DD,
        JP_BUTTON_DD,
        JP_BUTTON_DD | JP_BUTTON_DL,
        JP_BUTTON_DL,
        JP_BUTTON_DL | JP_BUTTON_DU,
        0,
    };

    uint8_t report[ANDROID_CONTROLLER_WIRE_REPORT_LEN];
    for (unsigned usage = 1; usage <= 14; usage++) {
        memcpy(report, ANDROID_CONTROLLER_NEUTRAL_REPORT, sizeof(report));
        unsigned bit = usage - 1;
        report[7 + bit / 8] = (uint8_t)(1u << (bit % 8));
        unsigned before = submitted;
        send_report(report);
        CHECK(submitted == before + 1 && last_event.buttons == button_map[bit],
              "individual Android button usage maps through the sequential profile");
    }

    for (uint8_t hat = 0; hat <= 8; hat++) {
        memcpy(report, ANDROID_CONTROLLER_NEUTRAL_REPORT, sizeof(report));
        report[9] = hat;
        unsigned before = submitted;
        send_report(report);
        CHECK(submitted == before + 1 && last_event.buttons == hat_map[hat],
              "Android hat direction maps through the canonical D-pad table");
    }
}

static void test_disconnect_cleanup(void)
{
    bthid_gamepad_driver.disconnect(&device);
    CHECK(disconnected == 1 && players_removed == 1,
          "link loss clears router state and player ownership exactly once");
}

int main(void)
{
    test_descriptor_and_neutral();
    test_full_state_and_atomic_rejection();
    test_every_button_and_hat();
    test_disconnect_cleanup();

    if (failures) {
        printf("bthid_android_controller: %d failure(s)\n", failures);
        return 1;
    }
    puts("bthid_android_controller: all tests passed");
    return 0;
}
