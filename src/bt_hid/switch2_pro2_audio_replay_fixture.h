#ifndef SWITCH2_PRO2_AUDIO_REPLAY_FIXTURE_H
#define SWITCH2_PRO2_AUDIO_REPLAY_FIXTURE_H

#include <stdint.h>

#define SW2_PRO2_REPLAY_FRAME_BYTES 120u
#define SW2_PRO2_REPLAY_FRAME_COUNT 29u

// Captured second halves of genuine 240-byte speaker Opus packets. Diagnostic
// replay only; these blocks are not independently decodable codec frames.
extern const uint8_t switch2_pro2_replay_frames
    [SW2_PRO2_REPLAY_FRAME_COUNT][SW2_PRO2_REPLAY_FRAME_BYTES];

#endif
