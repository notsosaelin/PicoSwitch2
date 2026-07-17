#ifndef DS5_AUDIO_RESAMPLE_H
#define DS5_AUDIO_RESAMPLE_H

#include <stdint.h>

#define DS5_AUDIO_RESAMPLE_INPUT_FRAMES 512u
#define DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES 480u
#define DS5_AUDIO_RESAMPLE_CHANNELS 2u

// Convert the 51.2 kHz PCM cadence used by the proven DualSense transport into
// one 10 ms, 48 kHz stereo Opus frame.
void ds5_audio_resample_512_to_480_stereo(const int16_t *input,
                                          int16_t *output);

#endif  // DS5_AUDIO_RESAMPLE_H
