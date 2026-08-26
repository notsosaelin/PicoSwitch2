// A local build of BTstack's HID Host with two PicoSwitch additions that need
// its private connection state. CMake removes the upstream translation unit
// from pico_btstack_classic and compiles this one instead, for live
// DualSense-audio builds only (see the NS2_DS5_AUDIO block in CMakeLists.txt).
//
// WHAT IS STILL PATCHED, against BTstack 1.8.2:
//
//   1. A stricter send guard. hid_host_send_report() accepts any state in
//      [HID_HOST_CONNECTION_ESTABLISHED, HID_HOST_W4_INTERRUPT_CONNECTION_DISCONNECTED),
//      and HID_HOST_W2_SEND_REPORT sits inside that range. So a second send
//      arriving while the first is still queued overwrites connection->report,
//      connection->report_len and the report pointer before L2CAP has read
//      them. The DS5 audio state machine retries rejected sends every 2 ms
//      service pass, which makes that race ordinary rather than theoretical.
//      ns2_hid_host_send_long_report() accepts only from the idle established
//      state, so a pending report is never clobbered -- the caller retries.
//
//   2. An l2cap_send_prepared() interposition, so the audio diagnostics observe
//      the actual L2CAP submission rather than merely HID Host accepting the
//      report into W2_SEND_REPORT. The direct-pair path records this boundary
//      in btstack_host.c; bonded reconnects are owned by HID Host and reach it
//      only through here.
//
// WHAT IS NO LONGER PATCHED:
//
//   The 16-bit report length. BTstack 1.6.2 declared
//   hid_host_send_report(..., uint8_t report_len) while storing report_len as a
//   uint16_t internally, so the 547-byte DualSense audio report could not be
//   passed through the public API and this file carried its own copy of the
//   accept sequence. Upstream widened the parameter to uint16_t in 1.8.2
//   (commit 6f867fb49), so the entry point below now just guards and delegates.
#include <l2cap.h>
#include <pico/time.h>

#include "ds5_audio_bridge.h"

static uint8_t ns2_hid_host_l2cap_send_prepared(uint16_t local_cid,
                                                 uint16_t len);

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
    // hid_host_get_connection_for_hid_cid() is static upstream; reading the
    // state is the whole reason this file textually includes hid_host.c.
    hid_host_connection_t *connection =
        hid_host_get_connection_for_hid_cid(hid_cid);
    if (connection == NULL) {
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }

    // The one deviation from upstream: exact state, not upstream's range.
    // HID_HOST_W2_SEND_REPORT is inside that range, and accepting there would
    // overwrite a report L2CAP has not submitted yet.
    if (connection->state != HID_HOST_CONNECTION_ESTABLISHED) {
        return ERROR_CODE_COMMAND_DISALLOWED;
    }

    // Everything else -- the control/interrupt channel checks, the MTU bound,
    // and the 16-bit accept sequence -- is upstream's.
    return hid_host_send_report(hid_cid, report_id, report, report_len);
}
