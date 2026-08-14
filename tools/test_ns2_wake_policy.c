/*
 * Host-compilable regression for the automatic Switch 2 wake policy.
 *
 *   gcc -std=c11 -DNS2_PRO -I tools/test_stubs -I include -I src/bt_hid \
 *       -o test_ns2_wake_policy tools/test_ns2_wake_policy.c src/ns2_wake.c
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "bt/btstack/btstack_host.h"
#include "ns2_wake.h"
#include "ns2_wake_protocol.h"
#include "usb.h"

static int failures;
static int advertisement_starts;
static bool advertisement_active;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL: %s\n", message);                            \
            failures++;                                                        \
        }                                                                      \
    } while (0)

volatile usb_personality_t g_usb_personality = USB_PERSONALITY_SWITCH2_PRO2;
volatile bool usb_lockout_ready;
volatile bool g_usb_mode_cycle_requested;

bool config_get_wake_identity(config_wake_identity_t *out) {
    memset(out, 0, sizeof(*out));
    out->product_id = 0x2069;
    out->host_count = 1;
    out->controller_addr_wire[0] = 1;
    out->host_addr_wire[0][0] = 1;
    return true;
}

void config_store_wake_identity(const config_wake_identity_t *identity) {
    (void)identity;
}

void ns2_wake_build_advertisement(uint16_t product_id,
                                  const uint8_t host_addr_wire[6],
                                  uint8_t out[NS2_WAKE_ADV_LEN]) {
    (void)product_id;
    (void)host_addr_wire;
    memset(out, 0, NS2_WAKE_ADV_LEN);
}

bool ns2_wake_parse_pairing_data(const uint8_t *data, size_t len,
                                 uint16_t product_id,
                                 const uint8_t controller_addr_wire[6],
                                 config_wake_identity_t *out) {
    (void)data;
    (void)len;
    (void)product_id;
    (void)controller_addr_wire;
    (void)out;
    return false;
}

bool btstack_host_start_wake_advertisement(
    const uint8_t advertiser_addr[6],
    const uint8_t advertisements[][BTSTACK_HOST_WAKE_ADV_LEN],
    uint8_t advertisement_count) {
    (void)advertiser_addr;
    (void)advertisements;
    (void)advertisement_count;
    advertisement_starts++;
    advertisement_active = true;
    return true;
}

bool btstack_host_wake_advertisement_active(void) {
    return advertisement_active;
}

int main(void) {
    // Establish an awake controller session and neutral baseline, then leave
    // USB inactive long enough to arm.
    ns2_wake_controller_session_started(0);
    ns2_wake_publish_usb_state(true, false, 100);
    ns2_wake_service(100);
    ns2_wake_note_controller_input(0, false, 150);
    ns2_wake_publish_usb_state(false, false, 200);
    ns2_wake_service(2200);

    ns2_wake_note_controller_input(0, false, 2250);
    ns2_wake_service(2250);
    CHECK(advertisement_starts == 0, "neutral reconnect traffic does not wake");

    ns2_wake_note_controller_input(0, true, 2300);
    ns2_wake_service(2301);
    CHECK(advertisement_starts == 1, "first pressed edge starts one wake attempt");

    ns2_wake_note_controller_input(0, true, 2350);
    ns2_wake_service(2351);
    CHECK(advertisement_starts == 1, "held reports cannot repeat wake");

    ns2_wake_note_controller_input(0, false, 2400);
    ns2_wake_note_controller_input(0, true, 2450);
    ns2_wake_service(2451);
    CHECK(advertisement_starts == 1,
          "new press waits while previous advertisement is active");

    advertisement_active = false;
    ns2_wake_service(2500);
    CHECK(advertisement_starts == 2,
          "new pressed edge retries after previous advertisement completes");

    ns2_wake_note_controller_input(0, true, 2550);
    advertisement_active = false;
    ns2_wake_service(2600);
    CHECK(advertisement_starts == 2, "second held state still cannot repeat");

    // Reconnect while the console remains asleep. A restored/startup pressed
    // state is not a physical edge in this new session; neutral must be seen
    // before a later press becomes wake intent.
    ns2_wake_controller_session_ended(0);
    ns2_wake_controller_session_started(0);
    ns2_wake_note_controller_input(0, true, 2700);
    ns2_wake_service(2701);
    CHECK(advertisement_starts == 2,
          "first non-neutral report after reconnect cannot resend wake");
    ns2_wake_note_controller_input(0, false, 2750);
    ns2_wake_service(2751);
    CHECK(advertisement_starts == 2,
          "neutral reconnect baseline alone does not wake");
    ns2_wake_note_controller_input(0, true, 2800);
    ns2_wake_service(2801);
    CHECK(advertisement_starts == 3,
          "new press after reconnect neutral baseline can wake");

    // Genuine Switch 1 Pro initialization is allowed to keep routing gameplay
    // reports, but the seam rebaselines wake rather than forwarding those
    // reports as intent until the driver's report-mode setup is complete.
    advertisement_active = false;
    ns2_wake_controller_session_ended(0);
    ns2_wake_controller_session_started(0);
    ns2_wake_controller_rebaseline(0);  // temporary 0x3F neutral
    ns2_wake_controller_rebaseline(0);  // temporary/restored pressed state
    ns2_wake_service(2825);
    CHECK(advertisement_starts == 3,
          "quarantined Switch Pro initialization cannot wake");
    ns2_wake_note_controller_input(0, false, 2840);
    ns2_wake_note_controller_input(0, true, 2850);
    ns2_wake_service(2851);
    CHECK(advertisement_starts == 4,
          "Switch Pro wakes after initialization and a fresh press");

    // Model BOOTSEL triple-tap dispatch: the edge is already latched while the
    // console is asleep, then maintenance synchronously cancels it before the
    // control tick services wake or asynchronous disconnect cleanup runs.
    advertisement_active = false;
    ns2_wake_note_controller_input(0, false, 2900);
    ns2_wake_note_controller_input(0, true, 2950);
    ns2_wake_set_input_suppressed(true);
    ns2_wake_note_controller_input(0, false, 2951);
    ns2_wake_note_controller_input(0, true, 2952);
    ns2_wake_service(2953);
    CHECK(advertisement_starts == 4,
          "BOOTSEL maintenance window cannot emit a wake advertisement");
    ns2_wake_set_input_suppressed(false);

    // Session cleanup is source-scoped: one controller disconnecting must not
    // consume another controller's already-established physical press.
    ns2_wake_controller_session_started(1);
    ns2_wake_note_controller_input(1, false, 3000);
    ns2_wake_note_controller_input(0, false, 3010);
    ns2_wake_note_controller_input(0, true, 3020);
    ns2_wake_controller_session_ended(1);
    ns2_wake_service(3021);
    CHECK(advertisement_starts == 5,
          "unrelated controller disconnect cannot cancel another wake edge");

    // ---------------------------------------------------------------------
    // App/management-initiated wake must report what ACTUALLY happened.
    //
    // The `wake` command can only confirm delivery: it latches on core0 and is
    // performed later on core1. Reporting that as success is what made the app
    // claim the console was woken when it was not. Each branch below is a
    // distinct outcome the app can now surface honestly.
    // ---------------------------------------------------------------------
    ns2_wake_status_t st;
    ns2_wake_get_status(&st);
    CHECK(st.result == NS2_WAKE_RESULT_NONE,
          "no app wake reported before one is requested");

    ns2_wake_manual_request();
    ns2_wake_get_status(&st);
    CHECK(st.result == NS2_WAKE_RESULT_PENDING,
          "a latched app wake reads as pending until core1 services it");

    // Console awake: nothing to do, and it must not be reported as success.
    ns2_wake_publish_usb_state(true, false, 4000);
    ns2_wake_service(4000);
    ns2_wake_get_status(&st);
    CHECK(st.result == NS2_WAKE_RESULT_CONSOLE_AWAKE,
          "app wake while the console is awake reports console_awake, not success");
    CHECK(st.attempts == 1, "a serviced app wake counts exactly one attempt");

    // Console asleep and the radio free: the advertisement really starts.
    advertisement_active = false;
    ns2_wake_publish_usb_state(false, false, 4100);
    ns2_wake_service(4900);
    int before_manual = advertisement_starts;
    ns2_wake_manual_request();
    ns2_wake_service(4901);
    ns2_wake_get_status(&st);
    CHECK(advertisement_starts == before_manual + 1,
          "app wake with the console asleep starts a wake advertisement");
    CHECK(st.result == NS2_WAKE_RESULT_ADVERTISED,
          "a started advertisement reports advertised");
    CHECK(st.console_asleep == 1 && st.identity_valid == 1,
          "status carries the console/identity preconditions it observed");

    // Radio already busy with a wake burst: deferred, never a false success.
    advertisement_active = true;
    ns2_wake_manual_request();
    ns2_wake_service(4950);
    ns2_wake_get_status(&st);
    CHECK(st.result == NS2_WAKE_RESULT_RADIO_BUSY,
          "app wake while a wake advert is running reports radio_busy");
    CHECK(st.attempts == 3, "every serviced app wake is counted");

    if (failures) return 1;
    printf("ns2_wake_policy: all tests passed\n");
    return 0;
}
