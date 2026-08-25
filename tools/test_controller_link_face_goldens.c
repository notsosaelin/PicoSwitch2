/*
 * Cross-layer golden for Controller Link face buttons.
 *
 * The companion resolves a built-in pad's face key into a logical Android HID
 * usage; this executable proves what that usage then does. It starts at the
 * usage, runs the production descriptor parser and the production bridge
 * resolver, and calls the production Pro Controller 2 button encoder.
 *
 * The shared fixture is also consumed by ControllerLinkFaceMappingTest, which
 * proves the Kotlin half turns each platform key into the usage claimed here.
 * Together they cover the whole path; separately, neither does -- which is how
 * the 2026-08-23 inversion reached hardware with every test green.
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
#include "ns2_kbm.h"
#include "ns2_remap.h"
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

static void fail_row(unsigned line, const char *id, const char *problem) {
    fprintf(stderr, "FAIL line %u row %s: %s\n", line, id, problem);
    failures++;
}

static void attach_bridge(void) {
    memset(&device, 0, sizeof(device));
    device.active = true;
    device.conn_index = 3;
    device.driver = &bthid_gamepad_driver;
    snprintf(device.name, sizeof(device.name), "%s", "Controller Link Golden Host");
    bthid_gamepad_driver.init(&device);
    bthid_gamepad_set_descriptor(&device, ANDROID_CONTROLLER_V2_HID_DESCRIPTOR,
                                 sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR));
}

static void send_usage(unsigned usage) {
    uint8_t report[ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN];
    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    const unsigned bit = usage - 1u;
    report[7u + bit / 8u] = (uint8_t)(1u << (bit % 8u));
    memset(&last_event, 0, sizeof(last_event));
    bthid_gamepad_driver.process_report(&device, report, sizeof(report));
}

static void run_row(unsigned line, const char *id, unsigned usage,
                    unsigned report_offset, unsigned report_mask) {
    static const uint32_t raw_face[4] = {
        JP_BUTTON_B1, JP_BUTTON_B2, JP_BUTTON_B3, JP_BUTTON_B4,
    };
    static const uint8_t bridge_face[4] = {
        NS2_DST_A, NS2_DST_B, NS2_DST_X, NS2_DST_Y,
    };
    if (usage < 1u || usage > 4u || report_offset >= 63u || report_mask > 0xFFu) {
        fail_row(line, id, "invalid fixture value");
        return;
    }

    send_usage(usage);
    const unsigned source = usage - 1u;
    if (!last_event.from_android_bridge)
        fail_row(line, id, "parser lost Android bridge provenance");
    if (last_event.buttons != raw_face[source])
        fail_row(line, id, "parser changed or contaminated the raw JP bitmap");
    const uint8_t destination = ns2_resolve_button_destination(
        (uint8_t)source, last_event.from_android_bridge);
    if (destination != bridge_face[source])
        fail_row(line, id, "bridge-aware seam destination is wrong");

    switch_pro_input_t in;
    memset(&in, 0, sizeof(in));
    ns2_kbm_apply_destination(destination, in.buttons, &in.extra);

    uint8_t out[63];
    memset(out, 0, sizeof(out));
    switch_pro2_encode_buttons(&in, &out[2]);
    if (out[report_offset] != (uint8_t)report_mask)
        fail_row(line, id, "final Pro Controller 2 output bit is not exact and isolated");
    if (out[3] != 0u || out[4] != 0u)
        fail_row(line, id, "an unrelated personality button field is non-neutral");
}

/*
 * The locked map for DIRECTLY PAIRED controllers must not drift while the bridge
 * correction exists beside it: that map is positional B/A/Y/X and is what every
 * non-bridge controller depends on.
 */
static void test_direct_controller_map_is_untouched(void) {
    static const uint8_t direct_face[4] = {
        NS2_DST_B, NS2_DST_A, NS2_DST_Y, NS2_DST_X,
    };
    for (uint8_t source = 0; source < 4u; source++) {
        if (ns2_resolve_button_destination(source, false) != direct_face[source]) {
            fprintf(stderr, "FAIL: locked direct-controller base map changed at %u\n", source);
            failures++;
        }
    }
}

int main(void) {
    attach_bridge();
    test_direct_controller_map_is_untouched();

    FILE *fixture = fopen("tools/fixtures/controller_link_face_mapping.csv", "rb");
    if (!fixture) {
        perror("tools/fixtures/controller_link_face_mapping.csv");
        return 1;
    }

    char line_text[512];
    unsigned line = 0;
    while (fgets(line_text, sizeof(line_text), fixture)) {
        line++;
        if (line_text[0] == '#' || line_text[0] == '\n' || line_text[0] == '\r') continue;
        char layout[16], platform_key[32], logical_button[8];
        unsigned usage, report_offset, report_mask;
        int parsed = sscanf(line_text, "%15[^,],%31[^,],%7[^,],%u,%u,%u",
                            layout, platform_key, logical_button,
                            &usage, &report_offset, &report_mask);
        if (parsed != 6) {
            fail_row(line, "<parse>", "fixture row has the wrong field count");
            continue;
        }
        char id[64];
        snprintf(id, sizeof(id), "%s/%s", layout, platform_key);
        rows++;
        run_row(line, id, usage, report_offset, report_mask);
    }
    fclose(fixture);

    if (rows != 8u) {
        fprintf(stderr, "FAIL: expected 8 Controller Link face rows, read %u\n", rows);
        failures++;
    }
    if (failures) {
        fprintf(stderr, "controller link face goldens: %d failure(s) across %u rows\n",
                failures, rows);
        return 1;
    }
    printf("controller link face goldens: %u/%u parser-seam-encoder cases passed\n", rows, rows);
    return 0;
}
