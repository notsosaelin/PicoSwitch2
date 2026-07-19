#ifndef DS5_RECONNECT_TRANSPORT_H
#define DS5_RECONNECT_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

// bt_transport removes the HID transaction byte and report ID before calling
// btstack_classic_send_report(). These are therefore the payload lengths seen
// at the Classic transport boundary.
#define DS5_AUDIO_CONTROL_PAYLOAD_LEN 141u
#define DS5_AUDIO_STREAM_PAYLOAD_LEN  546u

// Only the oversized DualSense audio protocol reports need BTstack's extended
// HID Host API. Report 0x32 is shared by the activation and mic-status
// transactions; both have the same full length.
static inline bool ds5_reconnect_uses_long_hid_report(uint8_t report_id,
                                                       uint16_t payload_len) {
    return (report_id == 0x32u &&
            payload_len == DS5_AUDIO_CONTROL_PAYLOAD_LEN) ||
           (report_id == 0x39u &&
            payload_len == DS5_AUDIO_STREAM_PAYLOAD_LEN);
}

#endif
