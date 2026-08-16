#include "bthid_android_bridge.h"

#include <string.h>

// The canonical HID contract is deliberately ONE file shared by the firmware,
// the host tests, and (mirrored) the Kotlin encoder, so the descriptor and the
// parser cannot drift apart. `tools` is on the include path for exactly this.
#include "fixtures/android_controller_hid.h"

_Static_assert(ANDROID_BRIDGE_FEEDBACK_MAX_LEN ==
                   ANDROID_CONTROLLER_OUTPUT_PAYLOAD_LEN,
               "feedback buffer size must track the canonical HID contract");

// int16 little-endian read; the wire format is fixed by the canonical contract.
static int16_t read_le16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// See the header: every v2 capability is gated on this match, so the outcome is
// recorded rather than inferred from downstream silence.
static android_bridge_identify_trace_t s_identify_trace = {
    .expected_len = (uint16_t)sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR),
    .first_mismatch = -1,
};

const android_bridge_identify_trace_t *android_bridge_identify_trace(void)
{
    return &s_identify_trace;
}

void android_bridge_identify_trace_reset(void)
{
    uint16_t expected = (uint16_t)sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR);
    memset(&s_identify_trace, 0, sizeof(s_identify_trace));
    s_identify_trace.expected_len = expected;
    s_identify_trace.first_mismatch = -1;
}

bool android_bridge_identify(const uint8_t *descriptor, uint16_t descriptor_len,
                             android_bridge_ext_t *out)
{
    s_identify_trace.calls++;
    s_identify_trace.expected_len =
        (uint16_t)sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR);
    s_identify_trace.last_len = descriptor_len;

    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!descriptor) {
        s_identify_trace.rejected_null++;
        s_identify_trace.active_profile = 1u;
        return false;
    }
    if (descriptor_len != sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR)) {
        s_identify_trace.rejected_length++;
        s_identify_trace.active_profile = 1u;
        return false;
    }
    if (memcmp(descriptor, ANDROID_CONTROLLER_V2_HID_DESCRIPTOR,
               sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR)) != 0) {
        s_identify_trace.rejected_content++;
        s_identify_trace.first_mismatch = -1;
        for (uint16_t i = 0; i < descriptor_len; i++) {
            if (descriptor[i] != ANDROID_CONTROLLER_V2_HID_DESCRIPTOR[i]) {
                s_identify_trace.first_mismatch = (int32_t)i;
                s_identify_trace.expected_byte =
                    ANDROID_CONTROLLER_V2_HID_DESCRIPTOR[i];
                s_identify_trace.actual_byte = descriptor[i];
                break;
            }
        }
        s_identify_trace.active_profile = 1u;
        return false;
    }

    s_identify_trace.matched++;
    s_identify_trace.first_mismatch = -1;
    s_identify_trace.active_profile = 2u;

    out->has_motion = true;
    out->has_battery = true;
    out->has_timestamp = true;
    out->has_output = true;
    out->gyro_offset = ANDROID_BRIDGE_OFF_GYRO;
    out->accel_offset = ANDROID_BRIDGE_OFF_ACCEL;
    out->battery_offset = ANDROID_BRIDGE_OFF_BATTERY;
    out->flags_offset = ANDROID_BRIDGE_OFF_FLAGS;
    out->timestamp_offset = ANDROID_BRIDGE_OFF_TIMESTAMP;
    out->output_report_id = ANDROID_CONTROLLER_OUTPUT_REPORT_ID;
    out->output_len = ANDROID_CONTROLLER_OUTPUT_PAYLOAD_LEN;
    out->min_report_len = ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN;
    return true;
}

void android_bridge_extract(const android_bridge_ext_t *ext,
                            android_bridge_state_t *state,
                            const uint8_t *data, uint16_t len,
                            input_event_t *event)
{
    if (!ext || !data || !event) return;

    uint8_t flags = 0;
    if (ext->has_battery && ext->flags_offset + 1u <= len) {
        flags = data[ext->flags_offset];
    }

    if (ext->has_motion && (flags & ANDROID_BRIDGE_FLAG_MOTION_VALID) &&
        ext->gyro_offset + 6u <= len && ext->accel_offset + 6u <= len) {
        for (unsigned i = 0; i < 3; i++) {
            event->gyro[i] = read_le16(&data[ext->gyro_offset + i * 2u]);
            event->accel[i] = read_le16(&data[ext->accel_offset + i * 2u]);
        }
        // Published in the same units the motion seam already expects from a
        // DualSense, so the Android source reuses the hardware-validated carrier
        // translation rather than introducing a second scaling convention.
        event->gyro_range = ANDROID_BRIDGE_GYRO_RANGE_DPS;
        event->accel_range = ANDROID_BRIDGE_ACCEL_RANGE_MILLI_G;
        event->has_motion = true;
        event->motion_from_android_bridge = true;

        if (ext->has_timestamp && ext->timestamp_offset + 2u <= len) {
            uint16_t stamp = (uint16_t)((uint16_t)data[ext->timestamp_offset] |
                                        ((uint16_t)data[ext->timestamp_offset + 1u] << 8));
            event->motion_timestamp = stamp;
            event->motion_timestamp_valid = true;
            if (state) {
                // Advance once per genuinely new IMU sample. Repeating the same
                // timestamp (the app resending an unchanged state after a button
                // edge) must not inflate the sequence, which downstream motion
                // consumers use to detect fresh samples.
                if (!state->has_last || stamp != state->last_timestamp) {
                    state->sequence++;
                    state->last_timestamp = stamp;
                    state->has_last = true;
                }
                event->motion_sequence = state->sequence;
            }
        } else if (state) {
            event->motion_sequence = ++state->sequence;
        }
    }

    if (ext->has_battery && (flags & ANDROID_BRIDGE_FLAG_BATTERY_VALID) &&
        ext->battery_offset + 1u <= len) {
        // The handheld's own battery is authoritative telemetry for this source,
        // the same standing a controller-native battery report has.
        input_event_set_native_battery(event, data[ext->battery_offset],
                                       (flags & ANDROID_BRIDGE_FLAG_CHARGING) != 0);
    }
}

uint8_t android_bridge_encode_feedback(const android_bridge_ext_t *ext,
                                       uint8_t rumble_left,
                                       uint8_t rumble_right,
                                       uint8_t player_led,
                                       bool motion_wanted,
                                       uint8_t *out, uint8_t out_capacity)
{
    if (!ext || !ext->has_output || !out) return 0;
    if (out_capacity < ANDROID_CONTROLLER_OUTPUT_PAYLOAD_LEN) return 0;

    out[0] = rumble_left;
    out[1] = rumble_right;
    out[2] = player_led;
    out[3] = motion_wanted ? ANDROID_BRIDGE_OUT_FLAG_MOTION_WANTED : 0u;
    return ANDROID_CONTROLLER_OUTPUT_PAYLOAD_LEN;
}
