// Pico SDK 2.2.0's BTstack HID Host keeps a 16-bit report length internally,
// but its public send API truncates the length to eight bits. Build the
// upstream implementation in this translation unit, then add a narrow
// 16-bit-length entry point which uses the same private connection state and
// scheduling machinery. CMake replaces only the original hid_host.c source
// with this file in live DualSense-audio builds.
#include <classic/hid_host.c>

#include "bt_hid/bt/btstack/hid_host_long_report.h"

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
