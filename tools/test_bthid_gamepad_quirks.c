/*
 * Host-side contract tests for the generic Bluetooth gamepad quirk registry.
 * These pin the behavior of the pre-refactor monolithic bthid_gamepad.c before
 * the surviving per-controller profiles are reconnected to its shared engine.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt/bthid/bthid.h"
#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "bt/bthid/devices/vendors/microsoft/xbox_rumble.h"
#include "core/buttons.h"

static int failures;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            printf("FAIL: %s\n", message);                                   \
            failures++;                                                       \
        }                                                                      \
    } while (0)

static struct {
    unsigned calls;
    bool result;
    uint8_t conn_index;
    uint8_t report_id;
    uint8_t data[16];
    uint16_t len;
} sent;

bool bthid_send_output_report(uint8_t conn_index, uint8_t report_id,
                              const uint8_t *data, uint16_t len) {
    sent.calls++;
    sent.conn_index = conn_index;
    sent.report_id = report_id;
    sent.len = len;
    memset(sent.data, 0, sizeof(sent.data));
    if (len <= sizeof(sent.data)) memcpy(sent.data, data, len);
    return sent.result;
}

static const gamepad_quirk_t *identify(uint16_t vid, uint16_t pid,
                                        const char *name, uint8_t buttons,
                                        const char *expected) {
    const gamepad_quirk_t *quirk = gamepad_quirks_identify(vid, pid, name, buttons);
    CHECK(quirk != NULL, "identify never returns NULL");
    CHECK(quirk && strcmp(quirk->name, expected) == 0, expected);
    return quirk;
}

static void select_map(const gamepad_quirk_t *quirk, uint8_t buttons,
                       bool sim_triggers, const uint32_t **map, uint8_t *size) {
    if (quirk->select_button_map) {
        quirk->select_button_map(buttons, sim_triggers, map, size);
    } else {
        *map = quirk->button_map;
        *size = quirk->button_map_size;
    }
    CHECK(*map != NULL, "selected button map is non-NULL");
    CHECK(*size > 0, "selected button map is non-empty");
}

static input_event_t fresh_event(input_transport_t transport) {
    input_event_t event;
    init_input_event(&event);
    event.transport = transport;
    return event;
}

static void test_identification(void) {
    identify(0, 0, "Plain HID Gamepad", 12, "generic");
    identify(0x045E, 0x02FD, "", 16, "xbox");
    identify(0, 0, "Xbox Wireless Controller", 16, "xbox");
    identify(0x045E, 0x0B05, "Xbox Elite", 16, "xbox_elite2");
    identify(0x045E, 0x0B22, "Xbox Elite", 16, "xbox_elite2");
    identify(0x2DC8, 0x286A, "8BitDo NGC", 16, "bitdo_ngc_modkit");
    identify(0x2DC8, 0x200B, "8BitDo Ultimate 2", 16, "bitdo_ultimate_mg");
    identify(0, 0, "8BitDo M30 gamepad", 12, "bitdo_m30");
    identify(0x2DC8, 0x0651, "", 12, "bitdo_m30");
    identify(0x2DC8, 0x5006, "", 12, "bitdo_m30");
    identify(0x2DC8, 0x1234, "8BitDo Controller", 16, "bitdo_paddle");
    identify(0x2DC8, 0x1234, "8BitDo Controller", 12, "generic");
}

static void test_button_maps(void) {
    const uint32_t *map;
    uint8_t size;

    const gamepad_quirk_t *generic = identify(0, 0, "Generic", 12, "generic");
    select_map(generic, 12, false, &map, &size);
    CHECK(size == 16, "generic map size");
    CHECK(map[1] == JP_BUTTON_B1 && map[8] == JP_BUTTON_R2 &&
          map[15] == JP_BUTTON_A3, "generic sequential map");

    const gamepad_quirk_t *xbox = identify(0x045E, 0x02FD, "Xbox", 16, "xbox");
    select_map(xbox, 16, true, &map, &size);
    CHECK(size == 17, "Xbox BLE map size");
    CHECK(map[3] == 0 && map[4] == JP_BUTTON_B3 && map[11] == JP_BUTTON_S1 &&
          map[16] == JP_BUTTON_A2, "Xbox BLE gap map");
    select_map(xbox, 15, false, &map, &size);
    CHECK(size == 16, "Xbox Classic map size");
    CHECK(map[3] == JP_BUTTON_B3 && map[7] == JP_BUTTON_S1 &&
          map[15] == JP_BUTTON_A2, "Xbox Classic sequential map");

    const gamepad_quirk_t *paddle = identify(0x2DC8, 0x1234, "8BitDo", 16,
                                              "bitdo_paddle");
    select_map(paddle, 16, false, &map, &size);
    CHECK(size == 17 && map[3] == JP_BUTTON_R4 && map[6] == JP_BUTTON_L4,
          "8BitDo usage 3/6 paddle map");
    select_map(paddle, 14, false, &map, &size);
    CHECK(size == 16 && map[3] == JP_BUTTON_B3 && map[6] == JP_BUTTON_R1,
          "8BitDo non-paddle fallback map");

    const gamepad_quirk_t *ngc = identify(0x2DC8, 0x286A, "8BitDo NGC", 16,
                                           "bitdo_ngc_modkit");
    select_map(ngc, 16, false, &map, &size);
    CHECK(size == 17, "NGC map size");
    CHECK(map[1] == JP_BUTTON_B2 && map[2] == JP_BUTTON_B1 &&
          map[9] == 0 && map[10] == 0 && map[11] == JP_BUTTON_R1 &&
          map[14] == JP_BUTTON_A2 && map[15] == JP_BUTTON_A1,
          "NGC evidence-backed button map");

    const gamepad_quirk_t *m30 = identify(0, 0, "8BitDo M30 gamepad", 12,
                                           "bitdo_m30");
    CHECK(m30->digital_shoulder_triggers, "M30 drops synthesized analog triggers");
    select_map(m30, 12, false, &map, &size);
    CHECK(size == 16 && map[7] == JP_BUTTON_L2 && map[8] == JP_BUTTON_R2,
          "M30 retains remappable digital shoulders");
}

static void test_extra_inputs(void) {
    uint8_t data[20] = {0};
    input_event_t event;

    const gamepad_quirk_t *ngc = identify(0x2DC8, 0x286A, "8BitDo NGC", 16,
                                           "bitdo_ngc_modkit");
    ble_report_map_t ngc_map = {0};
    ngc_map.buttonLoc[8] = (ble_usage_loc_t){.byteIndex = 9, .bitMask = 0x01};
    ngc_map.buttonLoc[9] = (ble_usage_loc_t){.byteIndex = 9, .bitMask = 0x02};
    ngc_map.buttonLoc[10] = (ble_usage_loc_t){.byteIndex = 9, .bitMask = 0x04};
    data[9] = 0x07;
    event = fresh_event(INPUT_TRANSPORT_BT_CLASSIC);
    ngc->extract_extra(&ngc_map, data, sizeof(data), &event);
    CHECK(event.gc_has_native_layout && event.gc_l_detent && event.gc_r_detent &&
          event.gc_native_z, "NGC native Z and trigger detents");

    memset(data, 0, sizeof(data));
    const gamepad_quirk_t *mg = identify(0x2DC8, 0x200B, "8BitDo Ultimate 2", 16,
                                          "bitdo_ultimate_mg");
    data[8] = 0x24;
    event = fresh_event(INPUT_TRANSPORT_BT_BLE);
    mg->extract_extra(NULL, data, 9, &event);
    CHECK((event.buttons & (JP_BUTTON_L4 | JP_BUTTON_R4)) ==
          (JP_BUTTON_L4 | JP_BUTTON_R4), "Ultimate MG raw-byte paddles");

    const gamepad_quirk_t *xbox = identify(0x045E, 0x02FD, "Xbox", 16, "xbox");
    memset(data, 0, sizeof(data));
    data[15] = 0x01;
    event = fresh_event(INPUT_TRANSPORT_BT_BLE);
    xbox->extract_extra(NULL, data, 16, &event);
    CHECK((event.buttons & JP_BUTTON_A2) != 0, "Xbox BLE extra byte -> Share");
    event = fresh_event(INPUT_TRANSPORT_BT_CLASSIC);
    xbox->extract_extra(NULL, data, 16, &event);
    CHECK((event.buttons & JP_BUTTON_S1) != 0, "Xbox Classic extra byte -> Back");

    memset(data, 0, sizeof(data));
    data[19] = 0x0F;
    event = fresh_event(INPUT_TRANSPORT_BT_BLE);
    xbox->extract_extra(NULL, data, 20, &event);
    CHECK((event.buttons & (JP_BUTTON_L4 | JP_BUTTON_L5 | JP_BUTTON_R4 | JP_BUTTON_R5)) ==
          (JP_BUTTON_L4 | JP_BUTTON_L5 | JP_BUTTON_R4 | JP_BUTTON_R5),
          "Xbox 20-byte fallback extracts all Elite paddles");

    const gamepad_quirk_t *elite = identify(0x045E, 0x0B22, "Xbox Elite", 16,
                                             "xbox_elite2");
    memset(data, 0, sizeof(data));
    data[15] = 0x01;
    event = fresh_event(INPUT_TRANSPORT_BT_BLE);
    elite->extract_extra(NULL, data, 16, &event);
    CHECK((event.buttons & JP_BUTTON_A2) != 0,
          "Elite retains base Xbox short-report Share fallback");
}

static void test_rumble(void) {
    const gamepad_quirk_t *generic = identify(0, 0, "Generic", 12, "generic");
    CHECK(generic->send_rumble == NULL, "generic controller does not invent rumble");
    CHECK(!gamepad_quirk_can_send_rumble(generic, 0),
          "generic controller cannot authorize output");

    const gamepad_quirk_t *xbox = identify(0x045E, 0x02FD, "Xbox", 16, "xbox");
    CHECK(xbox->send_rumble != NULL, "Xbox quirk provides rumble");
    CHECK(!gamepad_quirk_can_send_rumble(xbox, 0),
          "name-only Xbox cannot send rumble before VID resolves");
    CHECK(gamepad_quirk_can_send_rumble(xbox, 0x045E),
          "resolved Microsoft VID authorizes Xbox rumble");
    memset(&sent, 0, sizeof(sent));
    sent.result = true;
    CHECK(xbox->send_rumble(3, 255, 128), "Xbox rumble propagates send success");
    CHECK(sent.calls == 1 && sent.conn_index == 3 &&
          sent.report_id == XBOX_RUMBLE_REPORT_ID && sent.len == XBOX_RUMBLE_DATA_LEN,
          "Xbox rumble output framing");
    CHECK(sent.data[0] == 0x03 && sent.data[3] == 100 && sent.data[4] == 50 &&
          sent.data[5] == 0x05 && sent.data[7] == 0xEB,
          "Xbox rumble payload");

    const gamepad_quirk_t *elite = identify(0x045E, 0x0B22, "Xbox Elite", 16,
                                             "xbox_elite2");
    CHECK(elite->send_rumble == xbox->send_rumble,
          "Elite reuses the validated Xbox rumble implementation");
    CHECK(elite->rumble_vendor_id == 0x045E,
          "Elite preserves resolved-Microsoft-VID output gate");

    const gamepad_quirk_t *ngc = identify(0x2DC8, 0x286A, "8BitDo NGC Modkit", 16,
                                           "bitdo_ngc_modkit");
    CHECK(ngc->send_rumble != NULL, "NGC Modkit quirk provides rumble");
    CHECK(!gamepad_quirk_can_send_rumble(ngc, 0),
          "NGC Modkit waits for resolved identity before output");
    CHECK(!gamepad_quirk_can_send_rumble(ngc, 0x045E),
          "wrong vendor cannot authorize NGC Modkit output");
    CHECK(gamepad_quirk_can_send_rumble(ngc, 0x2DC8),
          "resolved 8BitDo VID authorizes NGC Modkit rumble");
    memset(&sent, 0, sizeof(sent));
    sent.result = true;
    CHECK(ngc->send_rumble(2, 0xA0, 0x35),
          "NGC Modkit rumble propagates send success");
    CHECK(sent.calls == 1 && sent.conn_index == 2 &&
          sent.report_id == 0xA5 && sent.len == 3,
          "NGC Modkit rumble output framing");
    CHECK(sent.data[0] == 0xDB && sent.data[1] == 0xA0 &&
          sent.data[2] == 0x35,
          "NGC Modkit rumble payload preserves low/high-frequency power");
}

int main(void) {
    test_identification();
    test_button_maps();
    test_extra_inputs();
    test_rumble();

    if (failures) {
        printf("bthid_gamepad_quirks: %d failure(s)\n", failures);
        return 1;
    }
    puts("bthid_gamepad_quirks: all tests passed");
    return 0;
}
