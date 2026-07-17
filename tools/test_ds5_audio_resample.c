#include "ds5_audio_resample.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int16_t input[DS5_AUDIO_RESAMPLE_INPUT_FRAMES *
                     DS5_AUDIO_RESAMPLE_CHANNELS];
static int16_t output[DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES *
                      DS5_AUDIO_RESAMPLE_CHANNELS];

static int16_t expected_ramp(uint32_t out_frame, int32_t scale,
                             int32_t offset) {
    uint32_t const position =
        out_frame * DS5_AUDIO_RESAMPLE_INPUT_FRAMES;
    uint32_t const index = position / DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES;
    uint32_t const fraction = position % DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES;
    int32_t const a = (int32_t)index * scale + offset;
    int32_t const b = (int32_t)(index + 1u) * scale + offset;
    return (int16_t)(
        (a * (int32_t)(DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES - fraction) +
         b * (int32_t)fraction) /
        (int32_t)DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES);
}

int main(void) {
    for (uint32_t i = 0;
         i < DS5_AUDIO_RESAMPLE_INPUT_FRAMES *
                 DS5_AUDIO_RESAMPLE_CHANNELS;
         ++i)
        input[i] = (i & 1u) ? -12345 : 12345;

    ds5_audio_resample_512_to_480_stereo(input, output);
    for (uint32_t frame = 0;
         frame < DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES; ++frame) {
        assert(output[frame * 2u] == 12345);
        assert(output[frame * 2u + 1u] == -12345);
    }

    for (uint32_t frame = 0;
         frame < DS5_AUDIO_RESAMPLE_INPUT_FRAMES; ++frame) {
        input[frame * 2u] = (int16_t)((int32_t)frame * 31 - 8000);
        input[frame * 2u + 1u] =
            (int16_t)((int32_t)frame * -29 + 7000);
    }
    ds5_audio_resample_512_to_480_stereo(input, output);

    uint32_t const probes[] = {0, 1, 15, 239, 478, 479};
    for (uint32_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        uint32_t const frame = probes[i];
        assert(output[frame * 2u] == expected_ramp(frame, 31, -8000));
        assert(output[frame * 2u + 1u] ==
               expected_ramp(frame, -29, 7000));
    }

    puts("ds5 audio resample tests passed");
    return 0;
}
