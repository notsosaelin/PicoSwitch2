// Host-only probe: treat each genuine 0x04/0x02 transport pair as one
// 240-byte Opus packet. Decode the whole capture to preserve codec state, but
// write only the 29-packet fixture interval used by the hardware replay.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opus.h"
#include "switch2_pro2_audio_replay_fixture.h"

static void put_u16(FILE *f, uint16_t v) {
    fputc((int)(v & 0xff), f); fputc((int)(v >> 8), f);
}

static void put_u32(FILE *f, uint32_t v) {
    put_u16(f, (uint16_t)v); put_u16(f, (uint16_t)(v >> 16));
}

static void wav_header(FILE *f, uint32_t samples) {
    const uint32_t bytes = samples * 4u;
    fwrite("RIFF", 1, 4, f); put_u32(f, 36u + bytes);
    fwrite("WAVEfmt ", 1, 8, f); put_u32(f, 16u);
    put_u16(f, 1u); put_u16(f, 2u); put_u32(f, 48000u);
    put_u32(f, 192000u); put_u16(f, 4u); put_u16(f, 16u);
    fwrite("data", 1, 4, f); put_u32(f, bytes);
}

static int nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, uint8_t *out, int capacity) {
    int count = 0;
    while (s[0] && s[1] && count < capacity) {
        const int hi = nibble((unsigned char)s[0]);
        const int lo = nibble((unsigned char)s[1]);
        if (hi < 0 || lo < 0) break;
        out[count++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return count;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s output.wav [reverse] < btatt-values.txt\n",
                argv[0]);
        return 2;
    }
    const int reverse = argc == 3 && strcmp(argv[2], "reverse") == 0;
    int error = OPUS_OK;
    OpusDecoder *decoder = opus_decoder_create(48000, 2, &error);
    FILE *wav = fopen(argv[1], "wb+");
    if (!decoder || error != OPUS_OK || !wav) return 3;
    wav_header(wav, 0);

    char line[1024];
    uint8_t value[256];
    uint8_t stream4[120] = {0};
    uint8_t packet[240];
    opus_int16 pcm[5760 * 2];
    int have_stream4 = 0;
    unsigned fixture_index = 0;
    unsigned pairs = 0, failures = 0, wrong_duration = 0, written_packets = 0;
    uint32_t written_samples = 0;

    while (fgets(line, sizeof(line), stdin)) {
        const int bytes = parse_hex(line, value, (int)sizeof(value));
        if (bytes != 123 || value[0] != 0x00 || value[2] != 0x78) continue;
        if (value[1] == 0x04) {
            memcpy(stream4, value + 3, sizeof(stream4));
            have_stream4 = 1;
            continue;
        }
        if (value[1] != 0x02 || !have_stream4) continue;

        memcpy(packet + (reverse ? 120 : 0), stream4, 120);
        memcpy(packet + (reverse ? 0 : 120), value + 3, 120);
        const int samples = opus_decode(decoder, packet, sizeof(packet), pcm,
                                        5760, 0);
        ++pairs;
        if (samples <= 0) {
            ++failures;
            continue;
        }
        if (samples != 960) ++wrong_duration;
        if (reverse) continue;

        if (fixture_index < SW2_PRO2_REPLAY_FRAME_COUNT &&
            memcmp(value + 3, switch2_pro2_replay_frames[fixture_index],
                   SW2_PRO2_REPLAY_FRAME_BYTES) == 0) {
            fwrite(pcm, sizeof(*pcm), (size_t)samples * 2u, wav);
            written_samples += (uint32_t)samples;
            ++written_packets;
            ++fixture_index;
        } else if (fixture_index != 0 &&
                   fixture_index < SW2_PRO2_REPLAY_FRAME_COUNT) {
            fprintf(stderr, "fixture sequence broke at index %u\n",
                    fixture_index);
            return 4;
        }
    }

    fseek(wav, 0, SEEK_SET);
    wav_header(wav, written_samples);
    fclose(wav);
    opus_decoder_destroy(decoder);
    fprintf(stderr,
            "order=%s pairs=%u failures=%u wrong_duration=%u fixture=%u "
            "written=%u samples=%u seconds=%.3f\n",
            reverse ? "02+04" : "04+02", pairs, failures, wrong_duration,
            fixture_index, written_packets, written_samples,
            written_samples / 48000.0);
    return reverse || fixture_index == SW2_PRO2_REPLAY_FRAME_COUNT ? 0 : 5;
}
