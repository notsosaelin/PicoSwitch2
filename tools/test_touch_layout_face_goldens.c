/*
 * Cross-layer golden for every face control registered by the touch catalog.
 *
 * The shared CSV is also consumed by TouchProfileCatalogTest, which proves its
 * key set and labels exactly match the Kotlin catalog. This executable starts
 * at the Android HID usage, runs the production descriptor parser and bridge
 * resolver, then calls the selected production personality encoder.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt/bthid/bthid.h"
#include "bt/bthid/devices/generic/bthid_gamepad.h"
#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"
#include "core/router/router.h"
#include "core/services/players/feedback.h"
#include "fixtures/android_controller_hid.h"
#include "ns2_kbm.h"
#include "ns2_remap.h"
#include "switch_gc_encode.h"
#include "switch_joycon2_encode.h"
#include "switch_pro2_encode.h"

static int failures;
static unsigned rows;
static bthid_device_t device;
static input_event_t last_event;

const gamepad_quirk_t QUIRK_BITDO_ULTIMATE_MG = {0};
const gamepad_quirk_t QUIRK_BITDO_M30 = {0};
const gamepad_quirk_t QUIRK_BITDO_PADDLE = {0};
const gamepad_quirk_t QUIRK_XBOX_ELITE2 = {0};
const gamepad_quirk_t QUIRK_XBOX = {0};

void router_submit_input(const input_event_t *event) { last_event = *event; }
void router_device_disconnected(uint8_t a, int8_t i) { (void)a; (void)i; }
void router_device_disconnected_with_generation(uint8_t a, int8_t i, uint32_t g)
{ (void)a; (void)i; (void)g; }
void remove_players_by_address(int a, int i) { (void)a; (void)i; }
int find_player_index(int a, int i) { (void)a; (void)i; return 0; }
static feedback_state_t feedback;
feedback_state_t *feedback_get_state(uint8_t i) { (void)i; return &feedback; }
void feedback_clear_dirty(uint8_t i) { (void)i; }
void bthid_register_driver(const bthid_driver_t *driver) { (void)driver; }
bthid_device_t *bthid_get_device(uint8_t conn_index)
{ return device.active && device.conn_index == conn_index ? &device : NULL; }
bool bthid_send_output_report(uint8_t i, uint8_t id, const uint8_t *p, uint16_t n)
{ (void)i; (void)id; (void)p; (void)n; return true; }

static void fail_row(unsigned line, const char *control, const char *problem) {
    fprintf(stderr, "FAIL line %u control %s: %s\n", line, control, problem);
    failures++;
}

static void attach_bridge(void) {
    memset(&device, 0, sizeof(device));
    device.active = true;
    device.conn_index = 3;
    device.driver = &bthid_gamepad_driver;
    snprintf(device.name, sizeof(device.name), "%s", "Touch Golden Host");
    bthid_gamepad_driver.init(&device);
    bthid_gamepad_set_descriptor(&device, ANDROID_CONTROLLER_V2_HID_DESCRIPTOR,
                                 sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR));
}

static void send_usage(unsigned usage) {
    uint8_t report[ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN];
    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    unsigned bit = usage - 1u;
    report[7u + bit / 8u] = (uint8_t)(1u << (bit % 8u));
    memset(&last_event, 0, sizeof(last_event));
    bthid_gamepad_driver.process_report(&device, report, sizeof(report));
}

static void encode_personality(const char *personality, const switch_pro_input_t *in,
                               uint32_t raw_buttons, uint8_t out[63]) {
    memset(out, 0, 63);
    if (strcmp(personality, "pro2") == 0) {
        switch_pro2_encode_buttons(in, &out[2]);
    } else if (strcmp(personality, "gc") == 0) {
        switch_gc_encode_report(in, 0, out);
    } else if (strcmp(personality, "jcl") == 0) {
        switch_joycon2_encode_report(in, raw_buttons, JOYCON2_SIDE_LEFT, 0, out);
    } else if (strcmp(personality, "jcr") == 0) {
        switch_joycon2_encode_report(in, raw_buttons, JOYCON2_SIDE_RIGHT, 0, out);
    }
}

static void run_row(unsigned line, const char *personality, const char *control,
                    unsigned usage, unsigned report_offset, unsigned report_mask) {
    static const uint32_t raw_face[4] = {
        JP_BUTTON_B1, JP_BUTTON_B2, JP_BUTTON_B3, JP_BUTTON_B4,
    };
    static const uint8_t direct_face[4] = {
        NS2_DST_B, NS2_DST_A, NS2_DST_Y, NS2_DST_X,
    };
    static const uint8_t bridge_face[4] = {
        NS2_DST_A, NS2_DST_B, NS2_DST_X, NS2_DST_Y,
    };
    if (usage < 1u || usage > 4u || report_offset >= 63u || report_mask > 0xFFu) {
        fail_row(line, control, "invalid fixture value");
        return;
    }

    send_usage(usage);
    const unsigned source = usage - 1u;
    if (!last_event.from_android_bridge)
        fail_row(line, control, "parser lost Android bridge provenance");
    if (last_event.buttons != raw_face[source])
        fail_row(line, control, "parser changed or contaminated the raw JP bitmap");
    if (ns2_resolve_button_destination((uint8_t)source, false) != direct_face[source])
        fail_row(line, control, "locked direct-controller base map changed");
    const uint8_t destination = ns2_resolve_button_destination(
        (uint8_t)source, last_event.from_android_bridge);
    if (destination != bridge_face[source])
        fail_row(line, control, "bridge-aware seam destination is wrong");

    switch_pro_input_t in;
    memset(&in, 0, sizeof(in));
    ns2_kbm_apply_destination(destination, in.buttons, &in.extra);
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in.left_stick);
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in.right_stick);

    uint8_t out[63];
    encode_personality(personality, &in, last_event.buttons, out);
    if (out[report_offset] != (uint8_t)report_mask)
        fail_row(line, control, "final personality output bit is not exact and isolated");
    if (out[3] != 0u)
        fail_row(line, control, "an unrelated personality button field is non-neutral");
    if (strcmp(personality, "jcl") != 0 && strcmp(personality, "jcr") != 0 && out[4] != 0u)
        fail_row(line, control, "an unrelated personality extra field is non-neutral");
}

static void test_pro2_extracted_button_encoder(void) {
    switch_pro_input_t in;
    memset(&in, 0, sizeof(in));
    uint8_t out[3] = {0xFF, 0xFF, 0xFF};
    switch_pro2_encode_buttons(&in, out);
    if (out[0] != 0u || out[1] != 0u || out[2] != 0u) {
        fprintf(stderr, "FAIL: Pro2 pure encoder is not neutral for neutral input\n");
        failures++;
    }

    in.buttons[0] = SWITCH_MASK_A | SWITCH_MASK_B | SWITCH_MASK_X | SWITCH_MASK_Y |
                    SWITCH_MASK_R | SWITCH_MASK_ZR;
    in.buttons[1] = SWITCH_MASK_MINUS | SWITCH_MASK_PLUS | SWITCH_MASK_L3 |
                    SWITCH_MASK_R3 | SWITCH_MASK_HOME | SWITCH_MASK_CAPTURE;
    in.buttons[2] = SWITCH_MASK_DPAD_UP | SWITCH_MASK_DPAD_DOWN |
                    SWITCH_MASK_DPAD_LEFT | SWITCH_MASK_DPAD_RIGHT |
                    SWITCH_MASK_L | SWITCH_MASK_ZL;
    in.extra = SWITCH_EXTRA_C | SWITCH_EXTRA_GL | SWITCH_EXTRA_GR;
    switch_pro2_encode_buttons(&in, out);
    if (out[0] != 0xFFu || out[1] != 0xFFu || out[2] != 0x1Fu) {
        fprintf(stderr, "FAIL: Pro2 pure encoder does not preserve the complete 0x09 button map\n");
        failures++;
    }
}

int main(void) {
    attach_bridge();
    test_pro2_extracted_button_encoder();
    FILE *fixture = fopen("tools/fixtures/touch_face_mapping.csv", "rb");
    if (!fixture) {
        perror("tools/fixtures/touch_face_mapping.csv");
        return 1;
    }

    char line_text[512];
    unsigned line = 0;
    while (fgets(line_text, sizeof(line_text), fixture)) {
        line++;
        if (line_text[0] == '#' || line_text[0] == '\n' || line_text[0] == '\r') continue;
        char personality[16], template_id[80], presentation[16];
        char control[80], label[32];
        unsigned usage, report_offset, report_mask;
        int parsed = sscanf(
            line_text, "%15[^,],%79[^,],%15[^,],%79[^,],%31[^,],%u,%u,%u",
            personality, template_id, presentation, control, label,
            &usage, &report_offset, &report_mask);
        if (parsed != 8) {
            fail_row(line, "<parse>", "fixture row has the wrong field count");
            continue;
        }
        if (template_id[0] == '\0' || presentation[0] == '\0' || label[0] == '\0') {
            fail_row(line, control, "fixture identity or label is empty");
            continue;
        }
        rows++;
        run_row(line, personality, control, usage, report_offset, report_mask);
    }
    fclose(fixture);

    if (rows != 20u) {
        fprintf(stderr, "FAIL: expected 20 catalog-linked face rows, read %u\n", rows);
        failures++;
    }
    if (failures) {
        fprintf(stderr, "touch face goldens: %d failure(s) across %u rows\n", failures, rows);
        return 1;
    }
    printf("touch face goldens: %u/%u parser-seam-encoder cases passed\n", rows, rows);
    return 0;
}
