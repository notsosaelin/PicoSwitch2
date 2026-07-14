#include "hid_out_normalize.h"

hid_out_normalized_t hid_out_normalize(uint8_t report_id, const uint8_t *buffer, uint16_t bufsize) {
    hid_out_normalized_t r;
    if (report_id != 0) {
        // Control SET_REPORT path: ID already separated out by TinyUSB.
        r.report_id = report_id;
        r.data = buffer;
        r.data_len = bufsize;
    } else if (bufsize > 0) {
        // Interrupt OUT path with real data: ID is buffer[0].
        r.report_id = buffer[0];
        r.data = buffer + 1;
        r.data_len = (uint16_t)(bufsize - 1);
    } else {
        // Interrupt OUT ZLP: no report ID at all.
        r.report_id = 0;
        r.data = buffer;
        r.data_len = 0;
    }
    return r;
}
