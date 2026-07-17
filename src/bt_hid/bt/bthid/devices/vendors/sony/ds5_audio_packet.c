#include "ds5_audio_packet.h"

#include <stddef.h>
#include <string.h>

#define DS5_AUDIO_STREAM_REPORT_ID 0x39
#define DS5_AUDIO_MIC_REPORT_ID    0x32
#define DS5_AUDIO_INPUT_REPORT_ID  0x31
#define DS5_AUDIO_HEADSET_STATUS_OFFSET 55u

// Extended report 0x32 payload offsets. Its SetStateData block begins at byte
// 4; bit 7 of the first byte marks AudioControl as valid. This is the same
// activation transaction used by DS5Dongle before it starts report 0x39.
#define DS5_AUDIO_CONTROL_VALID_FLAGS_OFFSET 4u
#define DS5_AUDIO_CONTROL_ALLOW_HP_VOLUME    0x10u
#define DS5_AUDIO_CONTROL_ALLOW_SPK_VOLUME   0x20u
#define DS5_AUDIO_CONTROL_ALLOW_AUDIO        0x80u
#define DS5_AUDIO_CONTROL_VALID_FLAGS2_OFFSET 5u
#define DS5_AUDIO_CONTROL_ALLOW_MUTE          0x02u
#define DS5_AUDIO_CONTROL_HP_VOLUME_OFFSET    8u
#define DS5_AUDIO_CONTROL_SPK_VOLUME_OFFSET   9u
#define DS5_AUDIO_CONTROL_ROUTE_OFFSET       11u
#define DS5_AUDIO_CONTROL_MUTE_OFFSET        13u
#define DS5_AUDIO_ROUTE_HEADPHONES           0x02u
#define DS5_AUDIO_ROUTE_SPEAKER              0x30u
#define DS5_AUDIO_MUTE_SPEAKER_AND_HP        0x60u

bool ds5_audio_is_mic_input_report(const uint8_t *report, uint16_t len) {
    return report && len > 1u && report[0] == DS5_AUDIO_INPUT_REPORT_ID &&
           (report[1] & 0x02u) != 0;
}

bool ds5_audio_headset_connected(const uint8_t *report, uint16_t len) {
    // Reference captures count the outer 0xA1 transaction byte and place this
    // flag at byte 56. bthid removes that byte before driver dispatch, making
    // byte 55 the corresponding offset here.
    return report && len > DS5_AUDIO_HEADSET_STATUS_OFFSET &&
           report[0] == DS5_AUDIO_INPUT_REPORT_ID &&
           (report[DS5_AUDIO_HEADSET_STATUS_OFFSET] & 0x01u) != 0;
}

static uint32_t ds5_audio_crc32_raw(uint32_t seed,
                                    const uint8_t *data,
                                    size_t len) {
    uint32_t crc = seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                  (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return crc;
}

// DualSense Bluetooth output CRC is the standard reflected CRC32 over
// 0xA2 || report_without_crc. Seeding with 0xA2 first is equivalent without
// constructing another temporary buffer.
static uint32_t ds5_audio_output_crc(const uint8_t *report, size_t payload_len) {
    const uint8_t transaction = 0xA2;
    uint32_t crc = ds5_audio_crc32_raw(0xFFFFFFFFu, &transaction, 1);
    return ~ds5_audio_crc32_raw(crc, report, payload_len);
}

static void ds5_audio_store_crc(uint8_t *report, size_t report_len) {
    size_t const offset = report_len - 4u;
    uint32_t const crc = ds5_audio_output_crc(report, offset);
    report[offset + 0] = (uint8_t)(crc >> 0);
    report[offset + 1] = (uint8_t)(crc >> 8);
    report[offset + 2] = (uint8_t)(crc >> 16);
    report[offset + 3] = (uint8_t)(crc >> 24);
}

void ds5_audio_build_stream_report(
    uint8_t sequence,
    uint8_t packet_counter,
    bool mic_enabled,
    bool use_headphones,
    uint8_t buffer_length,
    const uint8_t frame_a[DS5_AUDIO_OPUS_FRAME_LEN],
    const uint8_t frame_b[DS5_AUDIO_OPUS_FRAME_LEN],
    uint8_t out[DS5_AUDIO_STREAM_REPORT_LEN]) {
    memset(out, 0, DS5_AUDIO_STREAM_REPORT_LEN);

    out[0] = DS5_AUDIO_STREAM_REPORT_ID;
    out[1] = (uint8_t)((sequence & 0x0Fu) << 4);
    out[2] = 0x91;
    out[3] = 6;
    out[4] = mic_enabled ? 0x7F : 0x7E;
    out[5] = buffer_length;
    out[6] = buffer_length;
    out[7] = buffer_length;
    out[8] = buffer_length;
    out[9] = packet_counter;

    // Two 64-byte haptics blocks occupy bytes 12..139. PicoSwitch2's retail
    // PC2 USB descriptor exposes two PCM channels rather than DS5Dongle's
    // four-channel speaker+haptics layout, so those blocks remain zero here.
    out[10] = 0xD2;
    out[11] = 64;

    // 0x13 selects the controller speaker; 0x16 selects its headset output.
    // Bits 6 and 7 mark the block valid in the DualSense audio transport.
    out[140] = (uint8_t)((use_headphones ? 0x16 : 0x13) | 0xC0);
    out[141] = DS5_AUDIO_OPUS_FRAME_LEN;
    memcpy(out + 142, frame_a, DS5_AUDIO_OPUS_FRAME_LEN);
    memcpy(out + 342, frame_b, DS5_AUDIO_OPUS_FRAME_LEN);

    ds5_audio_store_crc(out, DS5_AUDIO_STREAM_REPORT_LEN);
}

void ds5_audio_build_control_report(
    uint8_t sequence,
    bool use_headphones,
    bool speaker_muted,
    uint8_t speaker_volume,
    uint8_t out[DS5_AUDIO_CONTROL_REPORT_LEN]) {
    memset(out, 0, DS5_AUDIO_CONTROL_REPORT_LEN);

    out[0] = DS5_AUDIO_MIC_REPORT_ID;
    out[1] = (uint8_t)((sequence & 0x0Fu) << 4);
    out[2] = 0x90;
    out[3] = 0x3F;
    out[DS5_AUDIO_CONTROL_VALID_FLAGS_OFFSET] =
        DS5_AUDIO_CONTROL_ALLOW_HP_VOLUME |
        DS5_AUDIO_CONTROL_ALLOW_SPK_VOLUME |
        DS5_AUDIO_CONTROL_ALLOW_AUDIO;
    out[DS5_AUDIO_CONTROL_VALID_FLAGS2_OFFSET] =
        DS5_AUDIO_CONTROL_ALLOW_MUTE;
    out[DS5_AUDIO_CONTROL_HP_VOLUME_OFFSET] = speaker_volume;
    out[DS5_AUDIO_CONTROL_SPK_VOLUME_OFFSET] = speaker_volume;
    // Explicitly select the destination. Zero is "automatic" and left the
    // fixed speaker diagnostic dependent on whatever route the controller
    // retained from its previous host.
    out[DS5_AUDIO_CONTROL_ROUTE_OFFSET] =
        use_headphones ? DS5_AUDIO_ROUTE_HEADPHONES : DS5_AUDIO_ROUTE_SPEAKER;
    out[DS5_AUDIO_CONTROL_MUTE_OFFSET] =
        speaker_muted ? DS5_AUDIO_MUTE_SPEAKER_AND_HP : 0;

    ds5_audio_store_crc(out, DS5_AUDIO_CONTROL_REPORT_LEN);
}

void ds5_audio_build_mic_status_report(
    uint8_t sequence,
    bool mic_enabled,
    uint8_t out[DS5_AUDIO_MIC_STATUS_REPORT_LEN]) {
    memset(out, 0, DS5_AUDIO_MIC_STATUS_REPORT_LEN);

    out[0] = DS5_AUDIO_MIC_REPORT_ID;
    out[1] = (uint8_t)((sequence & 0x0Fu) << 4);
    out[2] = 0x91;
    out[3] = 1;
    out[4] = mic_enabled ? 0x03 : 0x02;

    ds5_audio_store_crc(out, DS5_AUDIO_MIC_STATUS_REPORT_LEN);
}
