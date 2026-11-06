#include <assert.h>
#include <stdio.h>

#include "ds5_reconnect_transport.h"

int main(void) {
    assert(ds5_reconnect_uses_long_hid_report(
        0x32u, DS5_AUDIO_CONTROL_PAYLOAD_LEN));
    assert(ds5_reconnect_uses_long_hid_report(
        0x39u, DS5_AUDIO_STREAM_PAYLOAD_LEN));

    // Report ID alone is insufficient: ordinary reports with the same ID must
    // remain on the unmodified HID Host path.
    assert(!ds5_reconnect_uses_long_hid_report(0x32u, 32u));
    assert(!ds5_reconnect_uses_long_hid_report(0x39u, 64u));
    assert(!ds5_reconnect_uses_long_hid_report(
        0x31u, DS5_AUDIO_STREAM_PAYLOAD_LEN));

    // Catch framing regressions at the transport boundary.
    assert(!ds5_reconnect_uses_long_hid_report(
        0x32u, DS5_AUDIO_CONTROL_PAYLOAD_LEN - 1u));
    assert(!ds5_reconnect_uses_long_hid_report(
        0x39u, DS5_AUDIO_STREAM_PAYLOAD_LEN + 1u));

    puts("ds5_reconnect_transport: all tests passed");
    return 0;
}
