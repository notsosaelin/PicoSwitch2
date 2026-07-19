#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ds5_audio_packet.h"

static uint32_t report_crc(const uint8_t *report, uint16_t len) {
    return (uint32_t)report[len - 4] |
           ((uint32_t)report[len - 3] << 8) |
           ((uint32_t)report[len - 2] << 16) |
           ((uint32_t)report[len - 1] << 24);
}

int main(void) {
    const uint8_t ordinary_input[] = {0x31, 0x00, 0x80, 0x80};
    const uint8_t mic_input[] = {0x31, 0x02, 0x00, 0x7F};
    const uint8_t other_report[] = {0x01, 0x02};
    assert(!ds5_audio_is_mic_input_report(NULL, 0));
    assert(!ds5_audio_is_mic_input_report(ordinary_input,
                                           sizeof(ordinary_input)));
    assert(ds5_audio_is_mic_input_report(mic_input, sizeof(mic_input)));
    assert(!ds5_audio_is_mic_input_report(other_report,
                                           sizeof(other_report)));

    uint8_t bt_input[78] = {0};
    bt_input[0] = 0x31;
    assert(!ds5_audio_headset_connected(NULL, 0));
    assert(!ds5_audio_headset_connected(bt_input, 55));
    assert(!ds5_audio_headset_connected(bt_input, sizeof(bt_input)));
    assert(ds5_audio_headset_state(bt_input, sizeof(bt_input)) ==
           CONTROLLER_HEADSET_NONE);
    bt_input[55] = 0x01;
    assert(ds5_audio_headset_connected(bt_input, sizeof(bt_input)));
    assert(ds5_audio_headset_state(bt_input, sizeof(bt_input)) ==
           CONTROLLER_HEADSET_HEADPHONES);
    bt_input[55] = 0x03;
    assert(ds5_audio_headset_state(bt_input, sizeof(bt_input)) ==
           CONTROLLER_HEADSET_HEADSET);
    bt_input[55] = 0;
    bt_input[56] = 0x01;
    assert(!ds5_audio_headset_connected(bt_input, sizeof(bt_input)));
    bt_input[0] = 0x01;
    bt_input[55] = 0x01;
    assert(!ds5_audio_headset_connected(bt_input, sizeof(bt_input)));

    assert(controller_headset_switch2_state(CONTROLLER_HEADSET_NONE, 0) == 0);
    assert(controller_headset_switch2_state(CONTROLLER_HEADSET_HEADPHONES, 0) ==
           0x05);
    assert(controller_headset_switch2_state(CONTROLLER_HEADSET_HEADPHONES, 1) ==
           0x0D);
    assert(controller_headset_switch2_state(CONTROLLER_HEADSET_HEADSET, 0) ==
           0x07);
    assert(controller_headset_switch2_state(CONTROLLER_HEADSET_HEADSET, 1) ==
           0x0F);

    uint8_t frame_a[DS5_AUDIO_OPUS_FRAME_LEN];
    uint8_t frame_b[DS5_AUDIO_OPUS_FRAME_LEN];
    for (unsigned i = 0; i < DS5_AUDIO_OPUS_FRAME_LEN; ++i) {
        frame_a[i] = (uint8_t)i;
        frame_b[i] = (uint8_t)(255u - i);
    }

    uint8_t stream[DS5_AUDIO_STREAM_REPORT_LEN];
    ds5_audio_build_stream_report(3, 0x22, false, false, 0, 0, 64,
                                  frame_a, frame_b, stream);
    assert(stream[0] == 0x39 && stream[1] == 0x30);
    assert(stream[2] == 0x91 && stream[3] == 6 && stream[4] == 0x7E);
    assert(stream[5] == 64 && stream[8] == 64 && stream[9] == 0x22);
    assert(stream[10] == 0xD2 && stream[11] == 64);
    for (unsigned i = 12; i < 140; ++i) assert(stream[i] == 0);
    assert(stream[140] == 0xD3 && stream[141] == 200);
    for (unsigned i = 0; i < DS5_AUDIO_OPUS_FRAME_LEN; ++i) {
        assert(stream[142 + i] == frame_a[i]);
        assert(stream[342 + i] == frame_b[i]);
    }
    // Independently generated with zlib.crc32(A2 || report[0:543]).
    assert(report_crc(stream, sizeof(stream)) == 0x903B700Eu);

    ds5_audio_build_stream_report(15, 0x24, true, true, 0, 0, 48,
                                  frame_a, frame_b, stream);
    assert(stream[1] == 0xF0 && stream[4] == 0x7F);
    assert(stream[5] == 48 && stream[140] == 0xD6);

    ds5_audio_build_stream_report(4, 0x26, false, true, 255, 128, 64,
                                  frame_a, frame_b, stream);
    // 3 kHz stereo signed-8 haptic PCM: fixed 187.5 Hz sine, with
    // independent left/right magnitude scaling in both 64-byte blocks.
    assert((int8_t)stream[12] == 0 && (int8_t)stream[13] == 0);
    assert((int8_t)stream[14] == 127 && (int8_t)stream[15] == 79);
    assert((int8_t)stream[20] == 127 && (int8_t)stream[21] == 127);
    assert((int8_t)stream[44] == 0 && (int8_t)stream[45] == 0);
    assert((int8_t)stream[76] == 0 && (int8_t)stream[77] == 0);
    assert((int8_t)stream[108] == 0 && (int8_t)stream[109] == 0);
    // Haptic PCM occupies only bytes 12..139. It must never modify either
    // independently encoded speaker block.
    for (unsigned i = 0; i < DS5_AUDIO_OPUS_FRAME_LEN; ++i) {
        assert(stream[142 + i] == frame_a[i]);
        assert(stream[342 + i] == frame_b[i]);
    }

    // The capture-derived peak scalar remains below saturation after the
    // deliberately small 3x -> 3.25x increase.
    ds5_audio_build_stream_report(5, 0x28, false, true, 68, 68, 64,
                                  frame_a, frame_b, stream);
    assert((int8_t)stream[20] == 110 && (int8_t)stream[21] == 110);

    uint8_t control[DS5_AUDIO_CONTROL_REPORT_LEN];
    ds5_audio_build_control_report(7, false, false, 100, control);
    assert(control[0] == 0x32 && control[1] == 0x70);
    assert(control[2] == 0x90 && control[3] == 0x3F);
    assert(control[4] == 0xB1 && control[5] == 0x02);
    assert(control[8] == 100 && control[9] == 100);
    assert(control[11] == 0x30 && control[13] == 0);
    for (unsigned i = 6; i < sizeof(control) - 4; ++i) {
        if (i == 8 || i == 9 || i == 11 || i == 13) continue;
        assert(control[i] == 0);
    }
    // Independently generated with zlib.crc32(A2 || report[0:138]).
    assert(report_crc(control, sizeof(control)) == 0x79D14E9Bu);

    ds5_audio_build_control_report(9, true, true, 40, control);
    assert(control[1] == 0x90 && control[8] == 40 && control[9] == 40);
    assert(control[11] == 0x02 && control[13] == 0x60);
    assert(report_crc(control, sizeof(control)) == 0x53D8E54Cu);

    uint8_t mic[DS5_AUDIO_MIC_STATUS_REPORT_LEN];
    ds5_audio_build_mic_status_report(5, false, mic);
    assert(mic[0] == 0x32 && mic[1] == 0x50);
    assert(mic[2] == 0x91 && mic[3] == 1 && mic[4] == 2);
    assert(report_crc(mic, sizeof(mic)) == 0x7EED7F8Fu);

    ds5_audio_build_mic_status_report(5, true, mic);
    assert(mic[4] == 3);
    assert(report_crc(mic, sizeof(mic)) == 0x84FF8899u);

    puts("ds5_audio_packet: all tests passed");
    return 0;
}
