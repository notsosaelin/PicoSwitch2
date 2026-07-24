// Pico SDK 2.2.0's BTstack HID Host keeps a 16-bit report length internally,
// but its public send API truncates the length to eight bits. Build the
// upstream implementation in this translation unit, then add a narrow
// 16-bit-length entry point which uses the same private connection state and
// scheduling machinery. CMake replaces only the original hid_host.c source
// with this file in live DualSense-audio builds.
#include <l2cap.h>
#include <pico/time.h>

#include "ds5_audio_bridge.h"

static uint8_t ns2_hid_host_l2cap_send_prepared(uint16_t local_cid,
                                                 uint16_t len);

// Observe the actual HID Host L2CAP submission rather than merely accepting a
// report into its private W2_SEND_REPORT state. The direct-pair path already
// records this boundary in btstack_host.c; bonded reconnects use this wrapper.
#define l2cap_send_prepared ns2_hid_host_l2cap_send_prepared
#include <classic/hid_host.c>
#undef l2cap_send_prepared

#include "bt_hid/bt/btstack/hid_host_long_report.h"

static uint8_t ns2_hid_host_l2cap_send_prepared(uint16_t local_cid,
                                                 uint16_t len) {
    uint8_t const *packet = l2cap_get_outgoing_buffer();
    bool const dualsense_audio =
        len >= 2u && packet[0] == 0xA2u && packet[1] == 0x39u;
    uint8_t const status = l2cap_send_prepared(local_cid, len);
    if (status == ERROR_CODE_SUCCESS && dualsense_audio)
        ds5_audio_diag_note_l2cap_send(time_us_32());
    return status;
}

uint8_t ns2_hid_host_send_long_report(uint16_t hid_cid, uint16_t report_id,
                                      const uint8_t *report,
                                      uint16_t report_len) {
    hid_host_connection_t *connection =
        hid_host_get_connection_for_hid_cid(hid_cid);
    if (!connection || !connection->control_cid ||
        !connection->interrupt_cid) {
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }

    // The DS5 state machine retries rejected sends. Accept only from the idle
    // established state so a 2 ms service pass cannot overwrite an output
    // report which HID Host has not yet submitted to L2CAP.
    if (connection->state != HID_HOST_CONNECTION_ESTABLISHED) {
        return ERROR_CODE_COMMAND_DISALLOWED;
    }

    if ((l2cap_max_mtu() - 2u) < report_len) {
        return ERROR_CODE_COMMAND_DISALLOWED;
    }

    connection->state = HID_HOST_W2_SEND_REPORT;
    connection->report_type = HID_REPORT_TYPE_OUTPUT;
    connection->report_id = report_id;
    connection->report = report;
    connection->report_len = report_len;

    l2cap_request_can_send_now_event(connection->interrupt_cid);
    return ERROR_CODE_SUCCESS;
}
