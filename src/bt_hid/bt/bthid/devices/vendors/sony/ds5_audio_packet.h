#ifndef DS5_AUDIO_PACKET_H
#define DS5_AUDIO_PACKET_H

#include <stdbool.h>
#include <stdint.h>

#define DS5_AUDIO_OPUS_FRAME_LEN 200u
#define DS5_AUDIO_STREAM_REPORT_LEN 547u
#define DS5_AUDIO_CONTROL_REPORT_LEN 142u
#define DS5_AUDIO_MIC_STATUS_REPORT_LEN 142u

// Input presented to the Sony driver begins at report ID (the outer 0xA1 HID
// transaction byte has already been removed). Audio-bearing 0x31 reports use
// bit 1 of the following header byte and must not enter the gamepad parser.
bool ds5_audio_is_mic_input_report(const uint8_t *report, uint16_t len);

// Read the headset-present flag from a normal Bluetooth 0x31 input report.
// The report begins at its ID; the outer 0xA1 HID transaction byte has already
// been removed by bthid.
bool ds5_audio_headset_connected(const uint8_t *report, uint16_t len);

// Build complete Bluetooth HID output reports, beginning with report ID and
// ending with the CRC32. The HID transaction byte (0xA2) is not included.
void ds5_audio_build_stream_report(
    uint8_t sequence,
    uint8_t packet_counter,
    bool mic_enabled,
    bool use_headphones,
    uint8_t buffer_length,
    const uint8_t frame_a[DS5_AUDIO_OPUS_FRAME_LEN],
    const uint8_t frame_b[DS5_AUDIO_OPUS_FRAME_LEN],
    uint8_t out[DS5_AUDIO_STREAM_REPORT_LEN]);

// Enable the controller's audio-control section before streaming. DualSense
// expects this extended report 0x32 transaction before it will consume the
// Opus blocks carried by report 0x39.
void ds5_audio_build_control_report(
    uint8_t sequence,
    bool use_headphones,
    bool speaker_muted,
    uint8_t speaker_volume,
    uint8_t out[DS5_AUDIO_CONTROL_REPORT_LEN]);

void ds5_audio_build_mic_status_report(
    uint8_t sequence,
    bool mic_enabled,
    uint8_t out[DS5_AUDIO_MIC_STATUS_REPORT_LEN]);

#endif  // DS5_AUDIO_PACKET_H
