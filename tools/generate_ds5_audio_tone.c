#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "opus.h"

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define FRAME_SAMPLES 480
#define PACKET_BYTES 200
#define PRE_ROLL_FRAMES 10
#define TONE_HZ 1000.0
#define TONE_AMPLITUDE 20000.0

int main(void) {
    int error = OPUS_OK;
    OpusEncoder *encoder =
        opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_AUDIO,
                            &error);
    if (!encoder || error != OPUS_OK) return 1;

    if (opus_encoder_ctl(
            encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS)) !=
            OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(PACKET_BYTES * 8 * 100)) !=
            OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_VBR(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)) != OPUS_OK)
        return 1;

    int16_t pcm[FRAME_SAMPLES * CHANNELS];
    uint8_t packets[2][PACKET_BYTES];
    for (unsigned frame = 0; frame < PRE_ROLL_FRAMES + 2; ++frame) {
        for (unsigned sample = 0; sample < FRAME_SAMPLES; ++sample) {
            double phase =
                2.0 * 3.14159265358979323846 * TONE_HZ *
                (double)(frame * FRAME_SAMPLES + sample) / SAMPLE_RATE;
            int16_t value = (int16_t)lrint(sin(phase) * TONE_AMPLITUDE);
            pcm[sample * 2] = value;
            pcm[sample * 2 + 1] = value;
        }

        uint8_t packet[PACKET_BYTES];
        int encoded =
            opus_encode(encoder, pcm, FRAME_SAMPLES, packet, sizeof(packet));
        if (encoded != PACKET_BYTES) {
            fprintf(stderr, "expected %d bytes, got %d\n", PACKET_BYTES,
                    encoded);
            return 1;
        }
        if (frame >= PRE_ROLL_FRAMES)
            for (unsigned i = 0; i < PACKET_BYTES; ++i)
                packets[frame - PRE_ROLL_FRAMES][i] = packet[i];
    }

    puts("#include \"ds5_audio_test_tone.h\"");
    puts("");
    puts("const uint8_t");
    puts("    ds5_audio_test_tone_frames[2][DS5_AUDIO_TEST_TONE_FRAME_LEN] = {");
    for (unsigned frame = 0; frame < 2; ++frame) {
        puts("{");
        for (unsigned i = 0; i < PACKET_BYTES; ++i) {
            if ((i % 16) == 0) fputs("    ", stdout);
            printf("0x%02X", packets[frame][i]);
            if (i + 1 != PACKET_BYTES) fputs(", ", stdout);
            if ((i % 16) == 15 || i + 1 == PACKET_BYTES) puts("");
        }
        puts(frame == 0 ? "}," : "}");
    }
    puts("};");

    opus_encoder_destroy(encoder);
    return 0;
}
