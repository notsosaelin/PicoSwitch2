#ifndef DS5_AUDIO_RESAMPLE_H
#define DS5_AUDIO_RESAMPLE_H

#include <stdint.h>

#define DS5_AUDIO_RESAMPLE_INPUT_FRAMES 512u
#define DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES 480u
#define DS5_AUDIO_RESAMPLE_CHANNELS 2u

// Convert 512 real 48 kHz PCM frames (10.6667 ms) into the 480 samples encoded
// as one nominal 10 ms Opus frame. The DualSense consumes that frame at an
// effective 45 kHz clock; this conversion preserves source pitch and duration.
void ds5_audio_resample_512_to_480_stereo(const int16_t *input,
                                          int16_t *output);

#endif  // DS5_AUDIO_RESAMPLE_H
