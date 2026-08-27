/*
 * Host contract test for the Android Controller Bridge v2 feature-parity
 * extension: motion, battery, rumble, and player LED.
 *
 * Compiles the PRODUCTION generic gamepad driver, the production HID descriptor
 * parser, and the production bridge module against the canonical v2 descriptor
 * the Android app registers. Nothing here re-implements the wire format; the
 * contract comes from tools/fixtures/android_controller_hid.h, which the Kotlin
 * encoder mirrors.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
 *   -ffunction-sections -fdata-sections \
 *   -Itools/host_stubs -Isrc -Isrc/bt_hid -Iinclude -Itools \
 *   tools/test_bthid_android_bridge.c \
 *   src/bt_hid/bt/bthid/devices/generic/bthid_gamepad.c \
 *   src/bt_hid/bt/bthid/devices/generic/bthid_gamepad_quirks.c \
 *   src/bt_hid/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ngc_modkit.c \
 *   src/bt_hid/bt/bthid/devices/generic/bthid_android_bridge.c \
 *   src/bt_hid/usb/usbh/hid/devices/generic/hid_parser.c \
 *   src/ns2_remap.c \
 *   -Wl,--gc-sections -o build/host-tests/test_bthid_android_bridge.exe
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
#include "ns2_remap.h"

static int failures;
static bthid_device_t device;
static input_event_t last_event;
static unsigned submitted;

// Captured output reports (rumble / player LED).
static unsigned sent_reports;
static uint8_t sent_report_id;
static uint8_t sent_payload[8];
static uint8_t sent_len;
static bool send_should_fail;

static feedback_state_t fb;
static int player_index_result;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) printf("OK:   %s\n", message);                          \
        else { printf("FAIL: %s\n", message); failures++; }                     \
    } while (0)

const gamepad_quirk_t QUIRK_BITDO_ULTIMATE_MG = {0};
const gamepad_quirk_t QUIRK_BITDO_M30 = {0};
const gamepad_quirk_t QUIRK_BITDO_PADDLE = {0};
const gamepad_quirk_t QUIRK_XBOX_ELITE2 = {0};
const gamepad_quirk_t QUIRK_XBOX = {0};

void router_submit_input(const input_event_t *event) { last_event = *event; submitted++; }
void router_device_disconnected(uint8_t a, int8_t i) { (void)a; (void)i; }
void router_device_disconnected_with_generation(uint8_t a, int8_t i, uint32_t g)
{ (void)a; (void)i; (void)g; }
void remove_players_by_address(int a, int i) { (void)a; (void)i; }
int find_player_index(int dev_addr, int instance) { (void)dev_addr; (void)instance; return player_index_result; }
feedback_state_t *feedback_get_state(uint8_t player_index) { (void)player_index; return &fb; }
void feedback_clear_dirty(uint8_t player_index) { (void)player_index; fb.rumble_dirty = false; }
void bthid_register_driver(const bthid_driver_t *driver) { (void)driver; }
bthid_device_t *bthid_get_device(uint8_t conn_index)
{ return device.active && device.conn_index == conn_index ? &device : NULL; }

bool bthid_send_output_report(uint8_t conn_index, uint8_t report_id,
                              const uint8_t *data, uint16_t len)
{
    (void)conn_index;
    if (send_should_fail) return false;
    sent_reports++;
    sent_report_id = report_id;
    sent_len = (uint8_t)(len < sizeof(sent_payload) ? len : sizeof(sent_payload));
    memcpy(sent_payload, data, sent_len);
    return true;
}

static void put_le16(uint8_t *p, int16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }

static void attach_with_identity(const uint8_t *descriptor, uint16_t descriptor_len,
                                 uint16_t vendor_id, uint16_t product_id,
                                 const char *name)
{
    // Release the previous attachment first. gamepad_init() claims one of only
    // BTHID_MAX_DEVICES slots and never reuses a live one, so a suite that
    // attaches more times than that silently keeps running against the LAST
    // successfully allocated slot -- every later test then reads a stale map and
    // whether it passes depends on which descriptor happened to be parsed into
    // that slot. Disconnecting makes each test independent, which is what the
    // per-test attach was always meant to mean.
    if (device.driver_data) bthid_gamepad_driver.disconnect(&device);

    memset(&device, 0, sizeof(device));
    device.active = true;
    device.conn_index = 3;
    device.is_ble = false;
    device.driver = &bthid_gamepad_driver;
    device.vendor_id = vendor_id;
    device.product_id = product_id;
    snprintf(device.name, sizeof(device.name), "%s", name);
    submitted = 0;
    bthid_gamepad_driver.init(&device);
    bthid_gamepad_set_descriptor(&device, descriptor, descriptor_len);
}

static void attach(const uint8_t *descriptor, uint16_t descriptor_len)
{
    attach_with_identity(descriptor, descriptor_len, 0, 0, "AYN Thor");
}

static void send_v2(const uint8_t *report)
{
    bthid_gamepad_driver.process_report(&device, report,
                                        ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN);
}

// ---------------------------------------------------------------------------
// The v1 AXIS offsets must survive verbatim inside the v2 descriptor. Those are
// the regression that matters most: v1 is already hardware-validated in a game.
//
// The button COUNT is allowed to grow, and has twice. Usage 15 (C / GameChat)
// was appended inside the existing two button bytes -- 14 + 2 pad became 15 + 1
// -- and every later field kept its offset. Contract 4 could not do that: GL/GR
// are two buttons and one pad bit remained, so the field became three bytes and
// THE HAT MOVED FROM BYTE 9 TO BYTE 10, taking the whole vendor extension with
// it. That is deliberate and is why contract 4 exists.
//
// This is safe only because the parser computes every item's bit offset from the
// descriptor rather than assuming a layout -- which is exactly what these
// assertions prove, now that the layout has actually moved for the first time.
// ---------------------------------------------------------------------------
static void test_v2_descriptor_preserves_v1_layout(void)
{
    attach(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR,
           sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR));

    char map[1024];
    CHECK(bthid_gamepad_dump_map(device.conn_index, map, sizeof(map)),
          "v2 descriptor parses through the production HID parser");
    CHECK(strstr(map, "\"report_id\":1") && strstr(map, "\"button_cnt\":17") &&
          strstr(map, "\"x\":{\"byte\":1") && strstr(map, "\"hat\":{\"byte\":10"),
          "v2 keeps the v1 axis offsets, carries 17 buttons, and moved the hat to 10");

    send_v2(ANDROID_CONTROLLER_V2_NEUTRAL_REPORT);
    CHECK(submitted == 1 && last_event.buttons == 0 &&
          last_event.analog[ANALOG_LX] == 128 && last_event.analog[ANALOG_LY] == 128 &&
          last_event.analog[ANALOG_RX] == 128 && last_event.analog[ANALOG_RY] == 128,
          "v2 neutral report still yields a neutral gamepad snapshot");
    CHECK(!last_event.has_motion,
          "neutral report with the motion-valid flag clear publishes no motion");

    uint8_t report[ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN];
    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    report[1] = 0; report[2] = 255; report[5] = 64;
    report[7] = 0x01;   // usage 1
    report[10] = 2;     // hat east -- byte 10 since contract 4
    send_v2(report);
    CHECK(last_event.analog[ANALOG_LX] == 0 && last_event.analog[ANALOG_LY] == 255 &&
          last_event.analog[ANALOG_L2] == 64 &&
          last_event.buttons == (JP_BUTTON_B1 | JP_BUTTON_DR),
          "v1 axes/buttons/hat still decode correctly alongside the extension");
}

// ---------------------------------------------------------------------------
// GL/GR are wire button usages 16/17: the Pro Controller 2 grip buttons. Like C
// they are console controls almost no handheld has a physical key for, so they
// reach the bridge from the on-screen controller.
//
// The destinations are NOT new. NS2_BASE_BUTTON_MAP already routed JP_BUTTON_A4
// to NS2_DST_GL and JP_BUTTON_A5 to NS2_DST_GR; what contract 4 added is a way
// for the bridge to reach them at all. This checks the naming, and that it is
// the BRIDGE's own profile doing it -- the shared sequential table must not
// acquire the same interpretation, or any pad declaring 17 buttons would start
// sending grip presses.
// ---------------------------------------------------------------------------
static void test_grip_buttons(void)
{
    attach(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR,
           sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR));

    uint8_t report[ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN];

    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    report[8] = 0x80;   // usage 16 -- the last bit of the second button byte
    send_v2(report);
    CHECK(last_event.buttons == JP_BUTTON_A4, "usage 16 is GL (JP_BUTTON_A4)");
    CHECK(ns2_resolve_button_destination(19, true) == NS2_DST_GL,
          "and A4's source slot still resolves to the existing GL destination");

    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    report[9] = 0x01;   // usage 17 -- the first bit of the new third byte
    send_v2(report);
    CHECK(last_event.buttons == JP_BUTTON_A5, "usage 17 is GR (JP_BUTTON_A5)");
    CHECK(ns2_resolve_button_destination(22, true) == NS2_DST_GR,
          "and A5's source slot still resolves to the existing GR destination");

    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    report[8] = 0x80;
    report[9] = 0x01;
    report[7] = 0x01;   // usage 1, to prove they coexist with ordinary buttons
    send_v2(report);
    CHECK(last_event.buttons == (JP_BUTTON_A4 | JP_BUTTON_A5 | JP_BUTTON_B1),
          "both grips and a face button report together");

    send_v2(ANDROID_CONTROLLER_V2_NEUTRAL_REPORT);
    CHECK(last_event.buttons == 0, "and they release");

    // The shared fallback table is untouched: it still stops at usage 15, so an
    // unrecognized pad declaring more buttons gains nothing from this change.
    const gamepad_quirk_t *generic = gamepad_quirks_generic();
    CHECK(generic->button_map_size == 16,
          "the generic sequential table still ends at usage 15");
    CHECK(gamepad_quirks_android_bridge()->button_map_size == 18,
          "and only the bridge's own profile names 16/17");
}

// ---------------------------------------------------------------------------
// C / GameChat is wire button usage 15. It is the one console button an Android
// handheld can offer that no handheld has a physical key for, so it exists only
// as an on-screen control -- which makes "is it actually routed" a real question
// rather than a formality.
//
// This proves the firmware half: bit 14 of the button field must arrive at the
// router as JP_BUTTON_A3. The rest of the path is the static base map, where
// index 18 (JP_BUTTON_A3's source slot) is NS2_DST_C, which ns2_seam.c raises as
// SWITCH_EXTRA_C.
// ---------------------------------------------------------------------------
static void test_gamechat_button_routes(void)
{
    attach(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR,
           sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR));
    send_v2(ANDROID_CONTROLLER_V2_NEUTRAL_REPORT);

    uint8_t report[ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN];
    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    // Buttons occupy wire bytes 7..8; usage 15 is bit 14, i.e. bit 6 of byte 8.
    report[8] = 0x40;
    send_v2(report);
    CHECK(last_event.buttons == JP_BUTTON_A3,
          "wire button usage 15 routes as JP_BUTTON_A3 (C / GameChat) and nothing else");

    // Usage 14 (Capture) is the neighbouring bit; a one-off in the mask would
    // silently swap the two, which is exactly the kind of error a count change
    // introduces.
    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    report[8] = 0x20;
    send_v2(report);
    CHECK(last_event.buttons == JP_BUTTON_A2,
          "wire button usage 14 still routes as JP_BUTTON_A2 (Capture)");

    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    report[8] = 0x60;
    send_v2(report);
    CHECK(last_event.buttons == (JP_BUTTON_A2 | JP_BUTTON_A3),
          "Capture and C are independent bits and can be held together");

    send_v2(ANDROID_CONTROLLER_V2_NEUTRAL_REPORT);
    CHECK(last_event.buttons == 0, "releasing C clears it from the routed event");
}

// ---------------------------------------------------------------------------
// Touch and physical Android input are normalized to logical A/B/X/Y before
// this report is encoded. Exercise the production descriptor parser and its
// provenance flag together with the exact resolver called by ns2_seam.c. The
// deliberately controller-looking host identity proves that an incidental
// phone/PC name or VID/PID cannot replace the descriptor-declared bridge
// contract with a controller-family quirk.
// ---------------------------------------------------------------------------
static void test_face_buttons_reach_logical_seam_destinations(void)
{
    static const uint32_t expected_source[4] = {
        JP_BUTTON_B1, JP_BUTTON_B2, JP_BUTTON_B3, JP_BUTTON_B4,
    };
    static const uint8_t expected_destination[4] = {
        NS2_DST_A, NS2_DST_B, NS2_DST_X, NS2_DST_Y,
    };

    attach_with_identity(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR,
                         sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR),
                         0x2DC8, 0x286A, "8BitDo NGC Bridge Host");

    for (uint8_t phase = 0; phase < 2; phase++) {
        // First prove descriptor-time selection. Then simulate late SDP identity
        // resolution and prove it cannot displace the exact descriptor match.
        if (phase == 1u)
            bthid_gamepad_update_vid(&device);

        for (uint8_t usage = 1; usage <= 4; usage++) {
            uint8_t report[ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN];
            memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
            report[7] = (uint8_t)(1u << (usage - 1u));
            send_v2(report);

            CHECK(last_event.from_android_bridge,
                  "exact bridge descriptor marks the routed event as bridge input");
            CHECK(last_event.buttons == expected_source[usage - 1u],
                  "bridge face usage survives parser identity collision as its sequential JP slot");
            CHECK(!last_event.gc_has_native_layout && !last_event.gc_native_zl &&
                      !last_event.gc_native_z && !last_event.gc_l_detent &&
                      !last_event.gc_r_detent,
                  "bridge report never runs the colliding controller's native extra extractor");
            CHECK(ns2_resolve_button_destination((uint8_t)(usage - 1u),
                                                 last_event.from_android_bridge) ==
                      expected_destination[usage - 1u],
                  "bridge-aware seam resolver preserves logical A/B/X/Y destination");
        }
    }
}

static void test_motion_ingest(void)
{
    uint8_t report[ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN];
    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    put_le16(&report[ANDROID_BRIDGE_OFF_GYRO + 0], 1000);
    put_le16(&report[ANDROID_BRIDGE_OFF_GYRO + 2], -2000);
    put_le16(&report[ANDROID_BRIDGE_OFF_GYRO + 4], 32767);
    put_le16(&report[ANDROID_BRIDGE_OFF_ACCEL + 0], -8192);
    put_le16(&report[ANDROID_BRIDGE_OFF_ACCEL + 2], 8192);
    put_le16(&report[ANDROID_BRIDGE_OFF_ACCEL + 4], -32768);
    report[ANDROID_BRIDGE_OFF_FLAGS] = ANDROID_BRIDGE_FLAG_MOTION_VALID;
    put_le16(&report[ANDROID_BRIDGE_OFF_TIMESTAMP], 1234);
    send_v2(report);

    CHECK(last_event.has_motion, "motion-valid flag publishes motion");
    CHECK(last_event.gyro[0] == 1000 && last_event.gyro[1] == -2000 &&
          last_event.gyro[2] == 32767,
          "signed 16-bit gyro axes survive the wire round trip");
    CHECK(last_event.accel[0] == -8192 && last_event.accel[1] == 8192 &&
          last_event.accel[2] == -32768,
          "signed 16-bit accel axes survive the wire round trip, including INT16_MIN");
    CHECK(last_event.gyro_range == ANDROID_BRIDGE_GYRO_RANGE_DPS &&
          last_event.accel_range == ANDROID_BRIDGE_ACCEL_RANGE_MILLI_G,
          "published ranges match the documented sensor scale");
    CHECK(last_event.motion_from_android_bridge,
          "bridge provenance is flagged so the seam can select the Android axis row");
    CHECK(last_event.motion_timestamp == 1234 && last_event.motion_timestamp_valid,
          "motion timestamp reaches the normalized event");

    // A resend of the SAME sample (app republishing after a button edge) must not
    // look like a new IMU sample to downstream motion consumers.
    uint32_t seq = last_event.motion_sequence;
    report[7] = 0x02;
    send_v2(report);
    CHECK(last_event.motion_sequence == seq,
          "repeating an unchanged motion timestamp does not inflate the sequence");

    put_le16(&report[ANDROID_BRIDGE_OFF_TIMESTAMP], 1250);
    send_v2(report);
    CHECK(last_event.motion_sequence == seq + 1,
          "a new motion timestamp advances the sequence exactly once");

    // Sensors idled: motion must go away, not latch the last sample forever.
    report[ANDROID_BRIDGE_OFF_FLAGS] = 0;
    send_v2(report);
    CHECK(!last_event.has_motion,
          "clearing the motion-valid flag stops publishing stale motion");
}

static void test_battery_ingest(void)
{
    uint8_t report[ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN];
    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    report[ANDROID_BRIDGE_OFF_BATTERY] = 77;
    report[ANDROID_BRIDGE_OFF_FLAGS] = ANDROID_BRIDGE_FLAG_BATTERY_VALID;
    send_v2(report);
    CHECK(last_event.battery_level == 77 && !last_event.battery_charging &&
          last_event.battery_source == INPUT_BATTERY_NATIVE_HID,
          "handheld battery level lands as authoritative native telemetry");

    report[ANDROID_BRIDGE_OFF_FLAGS] =
        ANDROID_BRIDGE_FLAG_BATTERY_VALID | ANDROID_BRIDGE_FLAG_CHARGING;
    send_v2(report);
    CHECK(last_event.battery_charging, "charging state reaches the normalized event");

    report[ANDROID_BRIDGE_OFF_BATTERY] = 200;  // out of range
    send_v2(report);
    CHECK(last_event.battery_level == 100, "battery level is clamped to 100 percent");
}

static void test_truncated_v2_report_is_atomic(void)
{
    uint8_t report[ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN];
    memcpy(report, ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, sizeof(report));
    unsigned before = submitted;
    bthid_gamepad_driver.process_report(&device, report, sizeof(report) - 1);
    CHECK(submitted == before,
          "a truncated v2 report is rejected atomically, never half-applied");
}

// ---------------------------------------------------------------------------
// Feedback: one output report carries rumble + player LED + the motion request.
// ---------------------------------------------------------------------------
static void test_feedback_output(void)
{
    player_index_result = 0;
    memset(&fb, 0, sizeof(fb));
    sent_reports = 0;

    fb.rumble.left = 200;
    fb.rumble.right = 100;
    fb.led.pattern = 0x04;      // ns2_seam encodes player 3 as 1 << 2
    fb.rumble_dirty = true;
    bthid_gamepad_driver.task(&device);

    CHECK(sent_reports == 1 && sent_report_id == ANDROID_CONTROLLER_OUTPUT_REPORT_ID,
          "feedback goes out on the declared output report ID");
    CHECK(sent_len == ANDROID_CONTROLLER_OUTPUT_PAYLOAD_LEN &&
          sent_payload[0] == 200 && sent_payload[1] == 100 && sent_payload[2] == 3,
          "rumble amplitudes and the decoded player number are encoded");
    CHECK((sent_payload[3] & ANDROID_BRIDGE_OUT_FLAG_MOTION_WANTED) != 0,
          "motion-wanted flag defaults on through the weak host hook");

    // Unchanged state must not retransmit every tick (the task runs continuously).
    bthid_gamepad_driver.task(&device);
    CHECK(sent_reports == 1, "unchanged feedback is not retransmitted");

    // A player-LED change alone must still be delivered, which the rumble-only
    // dirty gate used by ordinary quirks would have missed.
    fb.led.pattern = 0x01;
    bthid_gamepad_driver.task(&device);
    CHECK(sent_reports == 2 && sent_payload[2] == 1,
          "a player-LED change alone is delivered to the handheld");

    // A failed send must not poison the cache: the next tick retries.
    fb.rumble.left = 5;
    send_should_fail = true;
    bthid_gamepad_driver.task(&device);
    CHECK(sent_reports == 2, "a failed send does not count as delivered");
    send_should_fail = false;
    bthid_gamepad_driver.task(&device);
    CHECK(sent_reports == 3 && sent_payload[0] == 5,
          "the next tick retries the feedback that failed to send");
}

// ---------------------------------------------------------------------------
// A v1 app (no extension) must be completely unaffected: no motion, no battery,
// and crucially no output report sent to a device that never declared one.
// ---------------------------------------------------------------------------
static void test_v1_device_is_unaffected(void)
{
    attach(ANDROID_CONTROLLER_HID_DESCRIPTOR,
           sizeof(ANDROID_CONTROLLER_HID_DESCRIPTOR));
    sent_reports = 0;
    player_index_result = 0;
    memset(&fb, 0, sizeof(fb));

    bthid_gamepad_driver.process_report(&device, ANDROID_CONTROLLER_NEUTRAL_REPORT,
                                        ANDROID_CONTROLLER_WIRE_REPORT_LEN);
    CHECK(submitted == 1 && !last_event.has_motion &&
          last_event.battery_source == INPUT_BATTERY_NONE &&
          !last_event.motion_from_android_bridge,
          "a v1 app publishes input only: no motion, no battery, no provenance flag");

    fb.rumble.left = 255;
    fb.rumble_dirty = true;
    bthid_gamepad_driver.task(&device);
    CHECK(sent_reports == 0,
          "no output report is sent to a device that did not declare one");
}

/*
 * The identification trace.
 *
 * Battery, motion, rumble and the player LED are all gated on one exact
 * descriptor match, so a v2 feature loss with working buttons means identify
 * returned false -- and the trace has to say WHY, or the next investigation
 * infers it from downstream silence again and gets it wrong.
 */
static void test_identify_trace(void)
{
    android_bridge_ext_t ext;
    const android_bridge_identify_trace_t *t;

    android_bridge_identify_trace_reset();
    CHECK(android_bridge_identify(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR,
                                  (uint16_t)sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR),
                                  &ext),
          "canonical descriptor still identifies as the bridge");
    t = android_bridge_identify_trace();
    CHECK(t->calls == 1 && t->matched == 1, "a match is counted");
    CHECK(t->active_profile == 2u, "a match selects the v2 bridge profile");
    CHECK(t->first_mismatch == -1, "a match reports no mismatch offset");

    /* Wrong length: the v1 descriptor is a real, previously shipped case. */
    android_bridge_identify_trace_reset();
    CHECK(!android_bridge_identify(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR, 81u, &ext),
          "a wrong-length descriptor is rejected");
    t = android_bridge_identify_trace();
    CHECK(t->rejected_length == 1 && t->rejected_content == 0,
          "a length rejection is distinguished from a content rejection");
    CHECK(t->last_len == 81u &&
          t->expected_len == (uint16_t)sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR),
          "both the received and required lengths are reported");
    CHECK(t->active_profile == 1u, "a rejection leaves the v1 generic profile active");

    /* Right length, one wrong byte: report exactly where and what. */
    uint8_t mutated[sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR)];
    memcpy(mutated, ANDROID_CONTROLLER_V2_HID_DESCRIPTOR, sizeof(mutated));
    const uint16_t victim = 36u;  /* Usage Maximum: 14 buttons vs 15 */
    const uint8_t original = mutated[victim];
    mutated[victim] = (uint8_t)(original ^ 0x01u);
    android_bridge_identify_trace_reset();
    CHECK(!android_bridge_identify(mutated, (uint16_t)sizeof(mutated), &ext),
          "a single wrong byte is rejected");
    t = android_bridge_identify_trace();
    CHECK(t->rejected_content == 1 && t->rejected_length == 0,
          "a content rejection is distinguished from a length rejection");
    CHECK(t->first_mismatch == (int32_t)victim,
          "the first differing byte offset is reported");
    CHECK(t->expected_byte == original && t->actual_byte == (uint8_t)(original ^ 0x01u),
          "the expected and received bytes are both reported");

    /* A null descriptor is its own case, not silently a length failure. */
    android_bridge_identify_trace_reset();
    CHECK(!android_bridge_identify(NULL, 161u, &ext), "a null descriptor is rejected");
    t = android_bridge_identify_trace();
    CHECK(t->rejected_null == 1, "a null descriptor is counted separately");

    android_bridge_identify_trace_reset();
}

int main(void)
{
    test_identify_trace();
    test_v2_descriptor_preserves_v1_layout();
    test_gamechat_button_routes();
    test_grip_buttons();
    test_face_buttons_reach_logical_seam_destinations();
    test_motion_ingest();
    test_battery_ingest();
    test_truncated_v2_report_is_atomic();
    test_feedback_output();
    test_v1_device_is_unaffected();

    if (failures) {
        printf("bthid_android_bridge: %d failure(s)\n", failures);
        return 1;
    }
    puts("bthid_android_bridge: all tests passed");
    return 0;
}
