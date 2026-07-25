// Host-only proof for a DualSense-specific direct-CELT optimization candidate:
// one public 200-byte Opus packet containing a 10 ms, 48 kHz stereo CELT frame.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "celt.h"
#include "opus.h"

#define RATE 48000
#define CHANNELS 2
#define SAMPLES 480
#define PACKET_BYTES 200
#define DS5_TOC 0xF4

static int configure_celt(CELTEncoder *encoder) {
    int status = celt_encoder_init(encoder, RATE, CHANNELS, 0);
    status |= celt_encoder_ctl(encoder, CELT_SET_SIGNALLING(0));
    status |= celt_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
    status |= celt_encoder_ctl(encoder, OPUS_SET_VBR(0));
    status |= celt_encoder_ctl(encoder, OPUS_SET_BITRATE(160000));
    return status;
}

static int benchmark_mode(CELTEncoder *encoder, OpusDecoder *decoder,
                          const float *input, const char *name,
                          int stream_channels, int end_band, uint8_t toc) {
    enum { BENCHMARK_FRAMES = 5000 };
    uint8_t packet[PACKET_BYTES];
    opus_int16 decoded_pcm[SAMPLES * CHANNELS];
    int status = OPUS_OK;
    status |= celt_encoder_ctl(encoder, OPUS_RESET_STATE);
    status |= celt_encoder_ctl(encoder, CELT_SET_CHANNELS(stream_channels));
    status |= celt_encoder_ctl(encoder, CELT_SET_END_BAND(end_band));
    status |= opus_decoder_ctl(decoder, OPUS_RESET_STATE);
    if (status != OPUS_OK) return status;

    unsigned checksum = 0;
    clock_t const start = clock();
    for (unsigned frame = 0; frame < BENCHMARK_FRAMES; ++frame) {
        packet[0] = toc;
        int const payload = celt_encode_with_ec(
            encoder, input, SAMPLES, packet + 1, PACKET_BYTES - 1, NULL);
        if (payload != PACKET_BYTES - 1) return OPUS_INTERNAL_ERROR;
        checksum += packet[1 + frame % (PACKET_BYTES - 1)];
    }
    clock_t const stop = clock();
    if (opus_decode(decoder, packet, sizeof(packet), decoded_pcm,
                    SAMPLES, 0) != SAMPLES)
        return OPUS_INVALID_PACKET;
    double const us_per_frame =
        1000000.0 * (double)(stop - start) /
        ((double)CLOCKS_PER_SEC * BENCHMARK_FRAMES);
    printf("benchmark=%-14s toc=%02X channels=%d end_band=%d "
           "encode_us=%.2f checksum=%u\n",
           name, toc, stream_channels, end_band, us_per_frame, checksum);
    return OPUS_OK;
}

static int benchmark_vbr(CELTEncoder *encoder, OpusDecoder *decoder,
                         const float *input, int bitrate) {
    enum { BENCHMARK_FRAMES = 5000 };
    uint8_t packet[PACKET_BYTES];
    opus_int16 decoded_pcm[SAMPLES * CHANNELS];
    int status = OPUS_OK;
    status |= celt_encoder_ctl(encoder, OPUS_RESET_STATE);
    status |= celt_encoder_ctl(encoder, CELT_SET_CHANNELS(2));
    status |= celt_encoder_ctl(encoder, CELT_SET_END_BAND(21));
    status |= celt_encoder_ctl(encoder, OPUS_SET_VBR(1));
    status |= celt_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
    status |= opus_decoder_ctl(decoder, OPUS_RESET_STATE);
    if (status != OPUS_OK) return status;

    unsigned checksum = 0;
    unsigned payload_total = 0;
    clock_t const start = clock();
    for (unsigned frame = 0; frame < BENCHMARK_FRAMES; ++frame) {
        packet[0] = DS5_TOC;
        int const payload = celt_encode_with_ec(
            encoder, input, SAMPLES, packet + 1, PACKET_BYTES - 1, NULL);
        if (payload <= 0 || payload >= PACKET_BYTES) return OPUS_INTERNAL_ERROR;
        payload_total += (unsigned)payload;
        int const pad_amount = PACKET_BYTES - (payload + 2);
        if (pad_amount < 0) return OPUS_INTERNAL_ERROR;
        if (pad_amount == 0) {
            memmove(packet + 2, packet + 1, (size_t)payload);
            packet[0] = (uint8_t)((packet[0] & 0xFCu) | 0x03u);
            packet[1] = 0x01u;
        } else {
            memmove(packet + 3, packet + 1, (size_t)payload);
            packet[0] = (uint8_t)((packet[0] & 0xFCu) | 0x03u);
            packet[1] = 0x41u;
            packet[2] = (uint8_t)(pad_amount - 1);
            memset(packet + 3 + payload, 0, (size_t)(pad_amount - 1));
        }
        checksum += packet[1 + frame % (PACKET_BYTES - 1)];
    }
    clock_t const stop = clock();
    if (opus_decode(decoder, packet, sizeof(packet), decoded_pcm,
                    SAMPLES, 0) != SAMPLES)
        return OPUS_INVALID_PACKET;
    double const us_per_frame =
        1000000.0 * (double)(stop - start) /
        ((double)CLOCKS_PER_SEC * BENCHMARK_FRAMES);
    printf("benchmark=vbr-%-6d toc=%02X channels=2 end_band=21 "
           "encode_pad_us=%.2f avg_payload=%.1f checksum=%u\n",
           bitrate, DS5_TOC, us_per_frame,
           (double)payload_total / BENCHMARK_FRAMES, checksum);
    status = OPUS_OK;
    status |= celt_encoder_ctl(encoder, OPUS_SET_VBR(0));
    status |= celt_encoder_ctl(encoder, OPUS_SET_BITRATE(160000));
    return status;
}

int main(void) {
    CELTEncoder *encoder = malloc((size_t)celt_encoder_get_size(CHANNELS));
    int error = OPUS_OK;
    OpusEncoder *public_encoder = opus_encoder_create(
        RATE, CHANNELS, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    OpusDecoder *decoder = opus_decoder_create(RATE, CHANNELS, &error);
    float input[SAMPLES * CHANNELS];
    opus_int16 output[SAMPLES * CHANNELS];
    uint8_t packet[PACKET_BYTES];
    uint8_t public_packet[PACKET_BYTES];
    if (!encoder || !public_encoder || !decoder || error != OPUS_OK ||
        configure_celt(encoder) != OPUS_OK)
        return 2;
    int public_status = OPUS_OK;
    public_status |= opus_encoder_ctl(
        public_encoder,
        OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    public_status |= opus_encoder_ctl(public_encoder,
                                      OPUS_SET_BITRATE(160000));
    public_status |= opus_encoder_ctl(public_encoder,
                                      OPUS_SET_FORCE_CHANNELS(OPUS_AUTO));
    public_status |= opus_encoder_ctl(public_encoder,
                                      OPUS_SET_BANDWIDTH(OPUS_AUTO));
    public_status |= opus_encoder_ctl(public_encoder,
                                      OPUS_SET_SIGNAL(OPUS_AUTO));
    public_status |= opus_encoder_ctl(public_encoder,
                                      OPUS_SET_COMPLEXITY(0));
    public_status |= opus_encoder_ctl(public_encoder, OPUS_SET_VBR(0));
    if (public_status != OPUS_OK) return 8;

    double decoded_energy = 0.0;
    int peak = 0;
    unsigned mismatched_packets = 0;
    for (unsigned frame = 0; frame < 100u; ++frame) {
        for (int i = 0; i < SAMPLES; ++i) {
            float const sample = 0.125f * sinf(
                2.0f * 3.14159265358979323846f * 1000.0f *
                (float)(frame * SAMPLES + (unsigned)i) / RATE);
            input[2 * i] = sample;
            input[2 * i + 1] = sample;
        }
        packet[0] = DS5_TOC;
        int const payload = celt_encode_with_ec(
            encoder, input, SAMPLES, packet + 1, PACKET_BYTES - 1, NULL);
        if (payload != PACKET_BYTES - 1 || packet[0] != DS5_TOC) return 3;
        int const public_bytes = opus_encode_float(
            public_encoder, input, SAMPLES, public_packet, PACKET_BYTES);
        if (public_bytes != PACKET_BYTES) return 9;
        if (memcmp(packet, public_packet, PACKET_BYTES) != 0)
            ++mismatched_packets;
        int const decoded = opus_decode(decoder, packet, sizeof(packet),
                                        output, SAMPLES, 0);
        if (decoded != SAMPLES) return 4;
        for (int i = 0; i < decoded * CHANNELS; ++i) {
            int const magnitude = output[i] < 0 ? -output[i] : output[i];
            if (magnitude > peak) peak = magnitude;
            decoded_energy += (double)output[i] * output[i];
        }
    }

    celt_encoder_ctl(encoder, OPUS_RESET_STATE);
    opus_decoder_ctl(decoder, OPUS_RESET_STATE);
    memset(input, 0, sizeof(input));
    unsigned silent_packets = 0;
    for (unsigned frame = 0; frame < 16u; ++frame) {
        packet[0] = DS5_TOC;
        if (celt_encode_with_ec(encoder, input, SAMPLES, packet + 1,
                                PACKET_BYTES - 1, NULL) != PACKET_BYTES - 1)
            return 5;
        if (opus_decode(decoder, packet, sizeof(packet), output,
                        SAMPLES, 0) != SAMPLES)
            return 6;
        ++silent_packets;
    }

    printf("bytes=%u toc=%02X frames=100 decoded_per_frame=%u peak=%d "
           "energy=%.0f silent_packets=%u state_bytes=%d->%d "
           "public_mismatches=%u\n",
           PACKET_BYTES, DS5_TOC, SAMPLES, peak, decoded_energy,
           silent_packets, opus_encoder_get_size(CHANNELS),
           celt_encoder_get_size(CHANNELS), mismatched_packets);
    for (int i = 0; i < SAMPLES; ++i) {
        input[2 * i] =
            0.10f * sinf(2.0f * 3.14159265358979323846f *
                         997.0f * (float)i / RATE);
        input[2 * i + 1] =
            0.08f * sinf(2.0f * 3.14159265358979323846f *
                         1601.0f * (float)i / RATE);
    }
    int benchmark_status = OPUS_OK;
    benchmark_status |= benchmark_mode(
        encoder, decoder, input, "full-stereo", 2, 21, 0xF4);
    benchmark_status |= benchmark_mode(
        encoder, decoder, input, "swb-stereo", 2, 19, 0xD4);
    benchmark_status |= benchmark_mode(
        encoder, decoder, input, "wb-stereo", 2, 17, 0xB4);
    benchmark_status |= benchmark_mode(
        encoder, decoder, input, "full-mono", 1, 21, 0xF0);
    benchmark_status |= benchmark_mode(
        encoder, decoder, input, "swb-mono", 1, 19, 0xD0);
    benchmark_status |= benchmark_mode(
        encoder, decoder, input, "wb-mono", 1, 17, 0xB0);
    benchmark_status |= benchmark_vbr(encoder, decoder, input, 144000);
    benchmark_status |= benchmark_vbr(encoder, decoder, input, 128000);
    benchmark_status |= benchmark_vbr(encoder, decoder, input, 112000);
    benchmark_status |= benchmark_vbr(encoder, decoder, input, 96000);
    opus_encoder_destroy(public_encoder);
    opus_decoder_destroy(decoder);
    free(encoder);
    // Direct CELT need not make the same rate-allocation decisions as the
    // public Opus wrapper. Decoder acceptance and decoded signal/silence are
    // the compatibility requirements; byte identity is only diagnostic.
    return peak > 0 && decoded_energy > 0.0 && silent_packets == 16u &&
                   benchmark_status == OPUS_OK
               ? 0
               : 7;
}
