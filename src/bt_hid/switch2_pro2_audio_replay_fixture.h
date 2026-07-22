#ifndef SWITCH2_PRO2_AUDIO_REPLAY_FIXTURE_H
#define SWITCH2_PRO2_AUDIO_REPLAY_FIXTURE_H

#include <stdint.h>

#define SW2_PRO2_REPLAY_FRAME_BYTES 120u
#define SW2_PRO2_REPLAY_FRAME_COUNT 29u

// A captured HD-haptic burst copied byte-for-byte from a decrypted genuine
// Pro Controller 2 0x002C capture. Diagnostic replay only; not a codec.
extern const uint8_t switch2_pro2_replay_frames
    [SW2_PRO2_REPLAY_FRAME_COUNT][SW2_PRO2_REPLAY_FRAME_BYTES];

#endif
