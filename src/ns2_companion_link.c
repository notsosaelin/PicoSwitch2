// ns2_companion_link.c - pure frame logic for the Windows Controller Link
// data plane. See ns2_companion_link.h for why this carrier exists.
//
// Deliberately free of BTstack and hardware so tools/test_ns2_companion_link.c
// can pin the wire contract, the staleness rule and the watchdog without a
// radio. The runtime half lives in btstack_host.c, where the GATT handles and
// the companion input source are.

#include "ns2_companion_link.h"

#include <string.h>

#include "fixtures/android_controller_hid.h"

// The frame sizes in the header are bound to the canonical contract HERE, so a
// contract change breaks the build instead of silently shifting every field.
// Contract 4 grew the button field from two bytes to three and moved motion,
// battery and the timestamp by one byte; a decoder carrying its own idea of the
// size would have read all three from the wrong place and still "worked".
_Static_assert(NS2_COMPANION_LINK_PAYLOAD_BYTES == ANDROID_CONTROLLER_V2_PAYLOAD_LEN,
               "Path C payload must be the canonical v2 report payload");
_Static_assert(NS2_COMPANION_LINK_REPORT_BYTES == ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN,
               "Path C report must be the canonical v2 wire report");
_Static_assert(NS2_COMPANION_LINK_REPORT_ID == ANDROID_CONTROLLER_REPORT_ID,
               "Path C input report ID must be the canonical input report ID");
// The feedback bound is the canonical output BODY, which is the same number
// ANDROID_BRIDGE_FEEDBACK_MAX_LEN and C# BridgeOutputCodec.BodySize carry.
// Asserting against the contract rather than against the encoder's own constant
// keeps one source of truth: bthid_android_bridge.c already asserts its constant
// against this same contract, so this covers both transitively.
_Static_assert(NS2_COMPANION_LINK_OUT_MAX_PAYLOAD == ANDROID_CONTROLLER_OUTPUT_PAYLOAD_LEN,
               "Path C feedback payload bound must be the canonical output body");
_Static_assert(NS2_COMPANION_LINK_OUT_HEADER_BYTES + NS2_COMPANION_LINK_OUT_MAX_PAYLOAD
                   == NS2_COMPANION_LINK_OUT_FRAME_BYTES,
               "Path C feedback frame size must match its parts");

ns2_companion_frame_result_t ns2_companion_link_parse(const uint8_t *data,
                                                      uint16_t len,
                                                      uint16_t *last_sequence,
                                                      bool *have_last,
                                                      const uint8_t **payload_out)
{
    if (data == NULL || len < NS2_COMPANION_LINK_FRAME_BYTES)
        return NS2_COMPANION_FRAME_SHORT;

    if (data[0] != NS2_COMPANION_LINK_VERSION)
        return NS2_COMPANION_FRAME_VERSION;

    if (data[1] != NS2_COMPANION_LINK_OP_STATE)
        return NS2_COMPANION_FRAME_OPCODE;

    uint16_t sequence = (uint16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));

    // Signed delta so a wrap from 0xFFFF to 0x0000 reads as +1, not -65535.
    // Equal sequences are stale too: a duplicate carries nothing new, and
    // applying it would cost a publish for no state change.
    if (have_last != NULL && *have_last && last_sequence != NULL) {
        int16_t delta = (int16_t)(sequence - *last_sequence);
        if (delta <= 0)
            return NS2_COMPANION_FRAME_STALE;
    }

    if (last_sequence != NULL)
        *last_sequence = sequence;
    if (have_last != NULL)
        *have_last = true;
    if (payload_out != NULL)
        *payload_out = data + NS2_COMPANION_LINK_HEADER_BYTES;

    return NS2_COMPANION_FRAME_OK;
}

uint16_t ns2_companion_link_encode_output(uint8_t report_id,
                                          const uint8_t *payload,
                                          uint8_t payload_len,
                                          uint8_t *out,
                                          uint16_t out_capacity)
{
    if (out == NULL || payload_len > NS2_COMPANION_LINK_OUT_MAX_PAYLOAD)
        return 0u;
    if (payload_len > 0u && payload == NULL)
        return 0u;

    uint16_t total = (uint16_t)(NS2_COMPANION_LINK_OUT_HEADER_BYTES + payload_len);
    if (out_capacity < total)
        return 0u;

    out[0] = NS2_COMPANION_LINK_VERSION;
    out[1] = NS2_COMPANION_LINK_OP_OUTPUT;
    out[2] = report_id;
    if (payload_len > 0u)
        memcpy(&out[NS2_COMPANION_LINK_OUT_HEADER_BYTES], payload, payload_len);

    return total;
}

bool ns2_companion_link_input_stale(bool active,
                                    bool neutralized,
                                    uint32_t last_frame_ms,
                                    uint32_t now_ms,
                                    uint32_t stale_ms)
{
    // Nothing to neutralize if the link is not carrying input, and neutralizing
    // twice would republish neutral state over whatever took the console next.
    if (!active || neutralized)
        return false;

    return (uint32_t)(now_ms - last_frame_ms) >= stale_ms;
}
