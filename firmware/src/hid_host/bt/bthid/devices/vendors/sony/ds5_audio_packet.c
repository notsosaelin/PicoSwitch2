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
#define DS5_AUDIO_CONTROL_ENABLE_RUMBLE_EMULATION 0x01u
#define DS5_AUDIO_CONTROL_ALLOW_HP_VOLUME    0x10u
#define DS5_AUDIO_CONTROL_ALLOW_SPK_VOLUME   0x20u
#define DS5_AUDIO_CONTROL_ALLOW_AUDIO        0x80u
#define DS5_AUDIO_CONTROL_VALID_FLAGS2_OFFSET 5u
#define DS5_AUDIO_CONTROL_ALLOW_MUTE          0x02u
#define DS5_AUDIO_CONTROL_HP_VOLUME_OFFSET    8u
#define DS5_AUDIO_CONTROL_SPK_VOLUME_OFFSET   9u
#define DS5_AUDIO_CONTROL_PATH_OFFSET        11u
#define DS5_AUDIO_CONTROL_MUTE_OFFSET        13u
#define DS5_AUDIO_CONTROL_HEADSET_PATH        0x02u
#define DS5_AUDIO_CONTROL_SPEAKER_PATH        0x30u
#define DS5_AUDIO_MUTE_SPEAKER_AND_HP        0x60u
#define DS5_AUDIO_HAPTIC_GAIN_NUMERATOR       13
#define DS5_AUDIO_HAPTIC_GAIN_DENOMINATOR      4

bool ds5_audio_is_mic_input_report(const uint8_t *report, uint16_t len) {
    return report && len > 1u && report[0] == DS5_AUDIO_INPUT_REPORT_ID &&
           (report[1] & 0x02u) != 0;
}

bool ds5_audio_headset_connected(const uint8_t *report, uint16_t len) {
#ifdef NS2_DS5_AUDIO
    return ds5_audio_headset_state(report, len) != CONTROLLER_HEADSET_NONE;
#else
    return report && len > DS5_AUDIO_HEADSET_STATUS_OFFSET &&
           report[0] == DS5_AUDIO_INPUT_REPORT_ID &&
           (report[DS5_AUDIO_HEADSET_STATUS_OFFSET] & 0x01u) != 0;
#endif
}

#ifdef NS2_DS5_AUDIO
uint8_t ds5_audio_headset_state(const uint8_t *report, uint16_t len) {
    // Reference captures count the outer 0xA1 transaction byte and place this
    // flag at byte 56. bthid removes that byte before driver dispatch, making
    // byte 55 the corresponding offset here. Bit 0 means headphones are
    // physically inserted; bit 1 distinguishes a microphone-equipped headset.
    if (!report || len <= DS5_AUDIO_HEADSET_STATUS_OFFSET ||
        report[0] != DS5_AUDIO_INPUT_REPORT_ID) {
        return CONTROLLER_HEADSET_NONE;
    }
    uint8_t const status = report[DS5_AUDIO_HEADSET_STATUS_OFFSET];
    if ((status & 0x01u) == 0) return CONTROLLER_HEADSET_NONE;
    return (status & 0x02u) != 0
        ? CONTROLLER_HEADSET_HEADSET
        : CONTROLLER_HEADSET_HEADPHONES;
}
#endif

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

#ifdef NS2_DS5_AUDIO
// Report 0x39 carries two consecutive 64-byte haptic blocks. Each block is
// 32 stereo signed-8 PCM frames at 3 kHz, matching DS5Dongle's hardware-proven
// path. The ordinary Nintendo rumble seam currently supplies one magnitude per
// physical side rather than the original HD-rumble frequencies, so render a
// phase-continuous fixed 187.5 Hz sine (two LUT steps per 3 kHz sample).
//
// A complete 0x39 packet contains exactly four cycles, so restarting the LUT
// at the next packet boundary is continuous. Zero magnitude remains byte-zero
// silence, preserving the previous packet output when rumble is idle.
//
// Hardware comparison preferred this native waveform's feel over compatible
// rumble but found it slightly light. The 13/4 (3.25x) curve is an 8.3% bump
// over the validated 3x path. In the genuine Switch 2 capture used for tuning,
// the strongest collapsed scalar was 68: its PCM peak rises from 102 to 110
// without clipping. Saturation remains the guard for larger console commands.
static void ds5_audio_write_haptics(uint8_t *out,
                                    uint8_t left,
                                    uint8_t right) {
    static const int8_t sine32[32] = {
          0,  25,  49,  71,  90, 106, 117, 125,
        127, 125, 117, 106,  90,  71,  49,  25,
          0, -25, -49, -71, -90,-106,-117,-125,
       -127,-125,-117,-106, -90, -71, -49, -25,
    };

    for (unsigned frame = 0; frame < 64u; ++frame) {
        int16_t const wave = sine32[(frame * 2u) & 31u];
        int32_t sample_left =
            ((int32_t)wave * left * DS5_AUDIO_HAPTIC_GAIN_NUMERATOR) /
            (255 * DS5_AUDIO_HAPTIC_GAIN_DENOMINATOR);
        int32_t sample_right =
            ((int32_t)wave * right * DS5_AUDIO_HAPTIC_GAIN_NUMERATOR) /
            (255 * DS5_AUDIO_HAPTIC_GAIN_DENOMINATOR);
        if (sample_left > 127) sample_left = 127;
        if (sample_left < -127) sample_left = -127;
        if (sample_right > 127) sample_right = 127;
        if (sample_right < -127) sample_right = -127;
        out[12u + frame * 2u] = (uint8_t)(int8_t)sample_left;
        out[13u + frame * 2u] = (uint8_t)(int8_t)sample_right;
    }
}
#endif

void ds5_audio_build_stream_report(
    uint8_t sequence,
    uint8_t packet_counter,
    bool mic_enabled,
    bool use_headphones,
#ifdef NS2_DS5_AUDIO
    uint8_t haptic_left,
    uint8_t haptic_right,
#endif
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

    // Two 64-byte haptic PCM blocks occupy bytes 12..139.
    out[10] = 0xD2;
    out[11] = 64;
#ifdef NS2_DS5_AUDIO
    ds5_audio_write_haptics(out, haptic_left, haptic_right);
#endif

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
        DS5_AUDIO_CONTROL_ENABLE_RUMBLE_EMULATION |
        DS5_AUDIO_CONTROL_ALLOW_HP_VOLUME |
        DS5_AUDIO_CONTROL_ALLOW_SPK_VOLUME |
        DS5_AUDIO_CONTROL_ALLOW_AUDIO;
    out[DS5_AUDIO_CONTROL_VALID_FLAGS2_OFFSET] =
        DS5_AUDIO_CONTROL_ALLOW_MUTE;
    out[DS5_AUDIO_CONTROL_HP_VOLUME_OFFSET] = speaker_volume;
    out[DS5_AUDIO_CONTROL_SPK_VOLUME_OFFSET] = speaker_volume;
    // Hardware establishes that the audible Bluetooth setup needs these
    // AudioControl values in addition to report 0x39's per-block destination.
    // They are not simple route enums: 0x02 selects the external MicSelect
    // state while retaining the default output channel path, whereas 0x30 is
    // the compatibility value used by the established speaker setup.
    out[DS5_AUDIO_CONTROL_PATH_OFFSET] =
        use_headphones ? DS5_AUDIO_CONTROL_HEADSET_PATH
                       : DS5_AUDIO_CONTROL_SPEAKER_PATH;
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
