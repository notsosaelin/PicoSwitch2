// Host-only proof for the Pro Controller 2 packet model used by firmware:
// one 240-byte Opus/CELT frame, split into two 120-byte GATT chunks.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "celt.h"
#include "opus.h"

#define RATE 48000
#define CHANNELS 2
#define SAMPLES 960
#define PACKET_BYTES 240
#define CHUNK_BYTES 120

int main(void) {
    CELTEncoder *encoder = malloc((size_t)celt_encoder_get_size(CHANNELS));
    int error = OPUS_OK;
    OpusDecoder *decoder = opus_decoder_create(RATE, CHANNELS, &error);
    float input[SAMPLES * CHANNELS];
    opus_int16 output[SAMPLES * CHANNELS];
    uint8_t packet[PACKET_BYTES], chunks[2][CHUNK_BYTES], joined[PACKET_BYTES];
    if (!encoder || !decoder || error != OPUS_OK ||
        celt_encoder_init(encoder, RATE, CHANNELS, 0) != OPUS_OK ||
        celt_encoder_ctl(encoder, CELT_SET_SIGNALLING(0)) != OPUS_OK ||
        celt_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)) != OPUS_OK ||
        celt_encoder_ctl(encoder, OPUS_SET_VBR(0)) != OPUS_OK ||
        celt_encoder_ctl(encoder, OPUS_SET_BITRATE(96000)) != OPUS_OK)
        return 2;

    for (int i = 0; i < SAMPLES; ++i) {
        const float sample = 0.125f * sinf(2.0f * 3.14159265358979323846f *
                                          1000.0f * i / RATE);
        input[2 * i] = sample;
        input[2 * i + 1] = sample;
    }
    packet[0] = 0xFC;
    const int payload = celt_encode_with_ec(encoder, input, SAMPLES,
                                             packet + 1,
                                             PACKET_BYTES - 1, NULL);
    if (payload != PACKET_BYTES - 1) return 3;
    memcpy(chunks[0], packet, CHUNK_BYTES);
    memcpy(chunks[1], packet + CHUNK_BYTES, CHUNK_BYTES);
    memcpy(joined, chunks[0], CHUNK_BYTES);
    memcpy(joined + CHUNK_BYTES, chunks[1], CHUNK_BYTES);
    if (memcmp(packet, joined, sizeof(packet)) != 0) return 4;

    const int decoded = opus_decode(decoder, joined, sizeof(joined), output,
                                    SAMPLES, 0);
    if (decoded != SAMPLES) return 5;
    double signal = 0.0, error_energy = 0.0;
    // CELT has algorithmic delay, so this is a structural decode check rather
    // than a sample-aligned conformance score. Nonzero bounded output proves
    // the split/reassembly remains one valid public Opus packet.
    int peak = 0;
    for (int i = 0; i < decoded * CHANNELS; ++i) {
        const int magnitude = output[i] < 0 ? -output[i] : output[i];
        if (magnitude > peak) peak = magnitude;
        signal += (double)output[i] * output[i];
        const double target = input[i] * 32768.0;
        const double delta = output[i] - target;
        error_energy += delta * delta;
    }
    printf("bytes=%d toc=%02X split=%u+%u decoded=%d peak=%d energy=%.0f raw_error=%.0f\n",
           payload + 1, packet[0], CHUNK_BYTES, CHUNK_BYTES, decoded, peak,
           signal, error_energy);
    opus_decoder_destroy(decoder);
    free(encoder);

    // A fresh production-shaped encoder must converge to the controller's
    // captured fixed idle packet. This validates the state-synchronization
    // path used when transport substitutes silence during an underrun.
    encoder = malloc((size_t)celt_encoder_get_size(CHANNELS));
    if (!encoder || celt_encoder_init(encoder, RATE, CHANNELS, 0) != OPUS_OK ||
        celt_encoder_ctl(encoder, CELT_SET_SIGNALLING(0)) != OPUS_OK ||
        celt_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)) != OPUS_OK ||
        celt_encoder_ctl(encoder, OPUS_SET_VBR(0)) != OPUS_OK ||
        celt_encoder_ctl(encoder, OPUS_SET_BITRATE(96000)) != OPUS_OK)
        return 6;
    memset(input, 0, sizeof(input));
    unsigned converged_at = 0;
    for (unsigned frame = 1; frame <= 16; ++frame) {
        packet[0] = 0xFC;
        if (celt_encode_with_ec(encoder, input, SAMPLES, packet + 1,
                                PACKET_BYTES - 1, NULL) != PACKET_BYTES - 1)
            return 7;
        const int fixed_idle = packet[0] == 0xFC && packet[1] == 0xFF &&
            packet[2] == 0xFE;
        unsigned nonzero_tail = 0;
        for (unsigned i = 3; i < PACKET_BYTES; ++i)
            nonzero_tail += packet[i] != 0;
        if (fixed_idle && nonzero_tail == 0 && converged_at == 0)
            converged_at = frame;
    }
    printf("idle_converged_at=%u\n", converged_at);
    free(encoder);
    return peak > 0 && signal > 0.0 && converged_at != 0 ? 0 : 8;
}
