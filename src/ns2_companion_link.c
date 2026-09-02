// ns2_companion_link.c - pure frame logic for the Windows Controller Link
// data plane. See ns2_companion_link.h for why this carrier exists.
//
// Deliberately free of BTstack and hardware so tools/test_ns2_companion_link.c
// can pin the wire contract, the staleness rule and the watchdog without a
// radio. The runtime half lives in btstack_host.c, where the GATT handles and
// the companion input source are.

#include "ns2_companion_link.h"

#include <string.h>

#include "core/buttons.h"
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

// Canonical payload offsets, report ID EXCLUDED. Stated once, adjacent to the
// static asserts that bind the payload size, and mirroring the contract-4 wire
// layout documented in tools/fixtures/android_controller_hid.h:
//
//   [0..5]   X, Y, Z, Rz, Rx, Ry  -> LX, LY, RX, RY, L2, R2
//   [6..8]   buttons 1..17, LSB first
//   [9]      hat, low nibble, 8 = released
//   [10..25] vendor extension, owned by android_bridge_extract()
#define CL_OFF_LX      0u
#define CL_OFF_LY      1u
#define CL_OFF_RX      2u
#define CL_OFF_RY      3u
#define CL_OFF_L2      4u
#define CL_OFF_R2      5u
#define CL_OFF_BUTTONS 6u
#define CL_BUTTON_BYTES 3u
#define CL_OFF_HAT     9u
#define CL_BUTTON_COUNT 17u

// Same table and orientation as bthid_gamepad.c: 8 = released, 0 = North, then
// clockwise. Duplicating the ENGINE would be wrong; duplicating this nine-entry
// constant is not, because the alternative is exporting an internal from the
// generic driver purely to satisfy a decoder that has no descriptor.
static const uint8_t CL_HAT_TO_DPAD[9] = {
    0b0001, 0b0011, 0b0010, 0b0110, 0b0100, 0b1100, 0b1000, 0b1001, 0b0000
};

void ns2_companion_link_decode_base(const uint8_t *payload,
                                    const uint32_t *button_map,
                                    uint8_t button_map_size,
                                    input_event_t *event)
{
    if (payload == NULL || event == NULL)
        return;

    event->analog[ANALOG_LX] = payload[CL_OFF_LX];
    event->analog[ANALOG_LY] = payload[CL_OFF_LY];
    event->analog[ANALOG_RX] = payload[CL_OFF_RX];
    event->analog[ANALOG_RY] = payload[CL_OFF_RY];
    event->analog[ANALOG_L2] = payload[CL_OFF_L2];
    event->analog[ANALOG_R2] = payload[CL_OFF_R2];

    uint32_t buttons = 0u;

    // Hat -> dpad. The canonical descriptor declares a 0-based hat, so no
    // hat_min adjustment applies; anything outside 0..7 is released.
    uint8_t hat = (uint8_t)(payload[CL_OFF_HAT] & 0x0Fu);
    uint8_t direction = (hat <= 8u) ? hat : 8u;
    uint8_t dpad = CL_HAT_TO_DPAD[direction];
    if (dpad & 0x01u) buttons |= JP_BUTTON_DU;
    if (dpad & 0x02u) buttons |= JP_BUTTON_DR;
    if (dpad & 0x04u) buttons |= JP_BUTTON_DD;
    if (dpad & 0x08u) buttons |= JP_BUTTON_DL;

    // Usage number = bit index + 1, matching the generic engine's loop.
    if (button_map != NULL) {
        for (uint8_t usage = 1u; usage <= CL_BUTTON_COUNT; usage++) {
            uint8_t bit = (uint8_t)(usage - 1u);
            uint8_t byte = (uint8_t)(CL_OFF_BUTTONS + (bit / 8u));
            if ((payload[byte] & (uint8_t)(1u << (bit % 8u))) == 0u)
                continue;
            if (usage < button_map_size)
                buttons |= button_map[usage];
        }
    }

    event->buttons = buttons;
    event->button_count = 4u;  // four face buttons, as the canonical layout has
}

bool ns2_companion_link_input_stale(bool streaming,
                                    bool neutralized,
                                    uint32_t last_frame_ms,
                                    uint32_t now_ms,
                                    uint32_t stale_ms)
{
    // Nothing to neutralize if the link has never carried input, and
    // neutralizing twice would republish neutral over whatever took the console
    // next.
    if (!streaming || neutralized)
        return false;

    return (uint32_t)(now_ms - last_frame_ms) >= stale_ms;
}
