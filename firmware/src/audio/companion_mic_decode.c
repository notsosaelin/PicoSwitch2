// companion_mic_decode.c - see companion_mic_decode.h.
//
// Compiled only where the live Opus codec is (Pico 2 W) and in the host tests;
// every other build gets nothing from this file.
#if defined(NS2_DS5_AUDIO_LIVE_OPUS) || defined(NS2_COMPANION_MIC_DECODE_HOST)

#include "companion_mic_decode.h"

#include <math.h>

#include "celt.h"
#include "ns2_companion_mic.h"

// CELT's float output for one block. Static: one decoder runs, on one core.
static float decode_pcm[NS2_COMPANION_MIC_BLOCK_FRAMES];

size_t companion_mic_decoder_size(void)
{
    int const size = celt_decoder_get_size((int)NS2_COMPANION_MIC_CHANNELS);
    return size > 0 ? (size_t)size : 0u;
}

void *companion_mic_decoder_init(void *storage, size_t storage_bytes)
{
    size_t const needed = companion_mic_decoder_size();
    if (!storage || needed == 0u || storage_bytes < needed) return NULL;
    CELTDecoder *const decoder = (CELTDecoder *)storage;
    if (celt_decoder_init(decoder, (opus_int32)NS2_COMPANION_MIC_SAMPLE_RATE,
                          (int)NS2_COMPANION_MIC_CHANNELS) != OPUS_OK)
        return NULL;
    // The Opus TOC byte is parsed here, not by CELT: the same arrangement as
    // libopus's own decoder (opus_decoder.c) and this firmware's encoders.
    if (celt_decoder_ctl(decoder, CELT_SET_SIGNALLING(0)) != OPUS_OK)
        return NULL;
    return decoder;
}

bool companion_mic_decode(void *decoder, const uint8_t *opus, int16_t *pcm)
{
    if (!decoder || !pcm) return false;
    // A packet's payload follows its one-byte TOC (code 0: one frame).
    int const frames = celt_decode_with_ec(
        (CELTDecoder *)decoder, opus ? opus + 1 : NULL,
        opus ? (int)NS2_COMPANION_MIC_OPUS_BYTES - 1 : 0, decode_pcm,
        (int)NS2_COMPANION_MIC_BLOCK_FRAMES, NULL, 0);
    if (frames != (int)NS2_COMPANION_MIC_BLOCK_FRAMES) return false;
    for (uint32_t i = 0; i < NS2_COMPANION_MIC_BLOCK_FRAMES; i++) {
        // CELT's float output is full scale at +/-1.0 (libopus FLOAT2INT16).
        float const scaled = decode_pcm[i] * 32768.0f;
        pcm[i] = scaled >= 32767.0f    ? (int16_t)32767
                 : scaled <= -32768.0f ? (int16_t)-32768
                                       : (int16_t)lrintf(scaled);
    }
    return true;
}

#endif
