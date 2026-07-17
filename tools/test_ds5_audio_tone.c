#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ds5_audio_test_tone.h"
#include "opus.h"

int main(void) {
    int error = OPUS_OK;
    OpusDecoder *decoder = opus_decoder_create(48000, 2, &error);
    assert(decoder != NULL && error == OPUS_OK);

    int16_t pcm[480 * 2];
    int64_t energy = 0;
    unsigned zero_crossings = 0;

    for (unsigned frame = 0; frame < 2; ++frame) {
        const uint8_t *packet = ds5_audio_test_tone_frames[frame];
        assert(opus_packet_get_nb_channels(packet) == 2);
        assert(opus_packet_get_nb_samples(
                   packet, DS5_AUDIO_TEST_TONE_FRAME_LEN, 48000) == 480);

        int decoded = opus_decode(decoder, packet,
                                  DS5_AUDIO_TEST_TONE_FRAME_LEN,
                                  pcm, 480, 0);
        assert(decoded == 480);

        for (int sample = 0; sample < decoded; ++sample) {
            int32_t left = pcm[sample * 2];
            int32_t right = pcm[sample * 2 + 1];
            // The generated test signal is stereo with equal channels.
            assert(left == right);
            energy += (int64_t)left * left;
            if (sample > 0) {
                int32_t previous = pcm[(sample - 1) * 2];
                if ((previous < 0 && left >= 0) ||
                    (previous >= 0 && left < 0))
                    zero_crossings++;
            }
        }
    }

    // A 1 kHz signal has about 20 zero crossings per 10 ms frame. Allow for
    // codec startup/transient behavior while rejecting silence or bad cadence.
    assert(energy > 1000000000LL);
    assert(zero_crossings >= 30 && zero_crossings <= 50);

    // The firmware loops these two packets indefinitely. Verify that replaying
    // them through a stateful decoder does not decay into silence or create
    // low-energy frames at the pair boundary.
    assert(opus_decoder_ctl(decoder, OPUS_RESET_STATE) == OPUS_OK);
    int64_t minimum_frame_energy = INT64_MAX;
    for (unsigned frame = 0; frame < 400; ++frame) {
        int decoded = opus_decode(
            decoder, ds5_audio_test_tone_frames[frame & 1u],
            DS5_AUDIO_TEST_TONE_FRAME_LEN, pcm, 480, 0);
        assert(decoded == 480);
        int64_t frame_energy = 0;
        for (int sample = 0; sample < decoded * 2; ++sample)
            frame_energy += (int64_t)pcm[sample] * pcm[sample];
        if (frame_energy < minimum_frame_energy)
            minimum_frame_energy = frame_energy;
    }
    assert(minimum_frame_energy > 1000000000LL);

    opus_decoder_destroy(decoder);
    puts("ds5_audio_tone: valid stereo 10 ms Opus packets");
    return 0;
}
