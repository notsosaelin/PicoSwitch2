/*
 * Xbox / Xbox Elite quirk pipeline regression.
 *
 * The Elite is NOT identified by a descriptor heuristic. It reaches its
 * behavior through the project's established machinery:
 *
 *     generic gamepad driver
 *       -> gamepad_quirks_identify() (name / VID / PID, most-specific-first)
 *       -> QUIRK_XBOX or QUIRK_XBOX_ELITE2
 *       -> quirk->extract_extra() at report time
 *       -> the "Xbox + 20-byte report" paddle fallback
 *
 * That chain is deliberately name-driven because the BLE PnP query often fails
 * to resolve VID/PID. This test pins it, so a future classification change
 * cannot quietly make the Elite unreachable again -- which is exactly what the
 * Keyboard/Mouse pass did by reclassifying the pad off the generic driver
 * before its descriptor was ever parsed.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"

static int failures;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) printf("OK:   %s\n", message);                         \
        else { printf("FAIL: %s\n", message); failures++; }                   \
    } while (0)

// The quirk table's Xbox rumble sender is authorized by VID and is not under
// test here; satisfy the link without pretending to send anything.
bool bthid_send_output_report(uint8_t conn_index, uint8_t report_id,
                              const uint8_t *data, uint16_t len) {
    (void)conn_index; (void)report_id; (void)data; (void)len;
    return true;
}

static input_event_t make_event(input_transport_t transport) {
    input_event_t event;
    init_input_event(&event);
    event.transport = transport;
    return event;
}

// ---------------------------------------------------------------------------

// The regression case: a BLE Elite whose PnP query never resolved. Identity is
// name-only, VID/PID are both zero, and the quirk table must still route it to
// the Xbox profile whose extract_extra carries the 20-byte paddle fallback.
static void test_unresolved_ble_identity_still_reaches_xbox(void) {
    const gamepad_quirk_t *quirk =
        gamepad_quirks_identify(0x0000u, 0x0000u, "Xbox Wireless Controller", 17u);
    CHECK(quirk && strcmp(quirk->name, "xbox") == 0,
          "unresolved BLE VID/PID still resolves to the Xbox quirk by name");
    CHECK(quirk && !gamepad_quirks_is_generic(quirk),
          "a name-matched Xbox pad is NOT an unresolved generic HID peer");
    CHECK(quirk && quirk->extract_extra != NULL,
          "the Xbox quirk supplies the extract_extra the paddle fallback lives in");
}

// The 20-byte Elite fallback itself, driven through the quirk's own function
// pointer rather than by calling the paddle helper directly.
static void test_twenty_byte_report_extracts_elite_paddles(void) {
    const gamepad_quirk_t *quirk =
        gamepad_quirks_identify(0x0000u, 0x0000u, "Xbox Wireless Controller", 17u);
    if (!quirk || !quirk->extract_extra) {
        CHECK(false, "Xbox quirk exposes extract_extra");
        return;
    }

    // Byte 19 carries the paddles: R4=0x01, R5=0x02, L4=0x04, L5=0x08.
    uint8_t report[20];
    memset(report, 0, sizeof(report));
    report[19] = 0x04u;  // upper-left paddle
    input_event_t event = make_event(INPUT_TRANSPORT_BT_BLE);
    quirk->extract_extra(NULL, report, sizeof(report), &event);
    CHECK((event.buttons & JP_BUTTON_L4) != 0,
          "20-byte report routes the upper-left Elite paddle through the quirk");

    report[19] = 0x0Fu;  // all four
    event = make_event(INPUT_TRANSPORT_BT_BLE);
    quirk->extract_extra(NULL, report, sizeof(report), &event);
    CHECK((event.buttons & JP_BUTTON_L4) && (event.buttons & JP_BUTTON_L5) &&
          (event.buttons & JP_BUTTON_R4) && (event.buttons & JP_BUTTON_R5),
          "20-byte report routes all four Elite paddles through the quirk");

    report[19] = 0x00u;
    event = make_event(INPUT_TRANSPORT_BT_BLE);
    quirk->extract_extra(NULL, report, sizeof(report), &event);
    CHECK((event.buttons & (JP_BUTTON_L4 | JP_BUTTON_L5 | JP_BUTTON_R4 |
                            JP_BUTTON_R5)) == 0,
          "released paddles report nothing");
}

// A normal Xbox pad sends 16-byte reports and must keep its own share/back
// behavior -- the paddle fallback must not reach it.
static void test_normal_xbox_report_unchanged(void) {
    const gamepad_quirk_t *quirk =
        gamepad_quirks_identify(0x045Eu, 0x0B13u, "Xbox Wireless Controller", 16u);
    if (!quirk || !quirk->extract_extra) {
        CHECK(false, "Xbox quirk exposes extract_extra");
        return;
    }

    uint8_t report[16];
    memset(report, 0, sizeof(report));
    report[15] = 0x01u;
    input_event_t ble = make_event(INPUT_TRANSPORT_BT_BLE);
    quirk->extract_extra(NULL, report, sizeof(report), &ble);
    CHECK((ble.buttons & JP_BUTTON_A2) != 0,
          "16-byte BLE Xbox report still maps Share to A2");
    CHECK((ble.buttons & (JP_BUTTON_L4 | JP_BUTTON_L5 | JP_BUTTON_R4 |
                          JP_BUTTON_R5)) == 0,
          "a normal 16-byte Xbox report never synthesizes Elite paddles");

    input_event_t classic = make_event(INPUT_TRANSPORT_BT_CLASSIC);
    quirk->extract_extra(NULL, report, sizeof(report), &classic);
    CHECK((classic.buttons & JP_BUTTON_S1) != 0,
          "16-byte Classic Xbox report still maps Back to S1");
}

// The exact-PID path stays ahead of the name fallback.
static void test_exact_elite_pid_selects_elite_quirk(void) {
    const gamepad_quirk_t *bt =
        gamepad_quirks_identify(0x045Eu, 0x0B22u, "Xbox Wireless Controller", 17u);
    CHECK(bt && strcmp(bt->name, "xbox_elite2") == 0,
          "Elite 2 Bluetooth PID 0x0B22 selects the Elite quirk");
    const gamepad_quirk_t *wired =
        gamepad_quirks_identify(0x045Eu, 0x0B05u, "Xbox Wireless Controller", 17u);
    CHECK(wired && strcmp(wired->name, "xbox_elite2") == 0,
          "Elite 2 wired PID 0x0B05 selects the Elite quirk");
    CHECK(bt && !gamepad_quirks_is_generic(bt),
          "an exact-PID Elite is not an unresolved generic HID peer");
}

// The other half of the contract: peers the table does NOT claim stay generic,
// so keyboard/mouse descriptor classification remains available to them.
static void test_unknown_peers_remain_generic(void) {
    const gamepad_quirk_t *keyboard =
        gamepad_quirks_identify(0x0B05u, 0x1B2Cu, "ROG FALCHION RX LO", 0u);
    CHECK(keyboard && gamepad_quirks_is_generic(keyboard),
          "an ordinary keyboard stays an unresolved generic peer");

    const gamepad_quirk_t *mouse =
        gamepad_quirks_identify(0x046Du, 0xB034u, "MX Master 3S", 0u);
    CHECK(mouse && gamepad_quirks_is_generic(mouse),
          "an ordinary mouse stays an unresolved generic peer");

    const gamepad_quirk_t *nameless =
        gamepad_quirks_identify(0x0000u, 0x0000u, NULL, 0u);
    CHECK(nameless && gamepad_quirks_is_generic(nameless),
          "a peer with no identity at all stays generic");

    CHECK(gamepad_quirks_is_generic(NULL),
          "identification that has not run yet counts as unresolved");
}

int main(void) {
    puts("xbox elite quirk pipeline:");
    test_unresolved_ble_identity_still_reaches_xbox();
    test_twenty_byte_report_extracts_elite_paddles();
    test_normal_xbox_report_unchanged();
    test_exact_elite_pid_selects_elite_quirk();
    test_unknown_peers_remain_generic();
    if (failures) {
        printf("xbox elite quirk pipeline: %d failure(s)\n", failures);
        return 1;
    }
    puts("xbox elite quirk pipeline: all tests passed");
    return 0;
}
