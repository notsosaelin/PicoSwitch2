#include "ds5_audio_resample.h"

#if defined(PICO_ON_DEVICE) && defined(NS2_DS5_AUDIO_LIVE_OPUS)
#include "pico.h"
#define DS5_AUDIO_RESAMPLE_FUNC(name) __not_in_flash_func(name)
#else
#define DS5_AUDIO_RESAMPLE_FUNC(name) name
#endif

void DS5_AUDIO_RESAMPLE_FUNC(ds5_audio_resample_512_to_480_stereo)(
    const int16_t *input, int16_t *output) {
    for (uint32_t out_frame = 0;
         out_frame < DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES; ++out_frame) {
        uint32_t const position =
            out_frame * DS5_AUDIO_RESAMPLE_INPUT_FRAMES;
        uint32_t const index =
            position / DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES;
        uint32_t const fraction =
            position % DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES;
        uint32_t const next =
            index < (DS5_AUDIO_RESAMPLE_INPUT_FRAMES - 1u) ? index + 1u
                                                           : index;
        for (uint32_t channel = 0;
             channel < DS5_AUDIO_RESAMPLE_CHANNELS; ++channel) {
            int32_t const a =
                input[index * DS5_AUDIO_RESAMPLE_CHANNELS + channel];
            int32_t const b =
                input[next * DS5_AUDIO_RESAMPLE_CHANNELS + channel];
            int32_t const value =
                (a * (int32_t)(DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES - fraction) +
                 b * (int32_t)fraction) /
                (int32_t)DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES;
            output[out_frame * DS5_AUDIO_RESAMPLE_CHANNELS + channel] =
                (int16_t)value;
        }
    }
}
