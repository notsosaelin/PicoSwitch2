// Companion microphone uplink, end to end on the host with the firmware's own
// codec: the packets the Windows companion's encoder produced
// (contracts/companion_mic_vectors.txt, written by CompanionMicTests) are
// framed as MIC-UP writes, parsed by ns2_companion_mic_parse, decoded by
// companion_mic_decode (libopus's CELT decoder, the same sources and float
// configuration the Pico 2 W links), queued in the playout ring and read out
// as the USB microphone endpoint's 1 ms stereo packets.
//
// What this proves: the two codecs agree on the packet (Concentus encoder,
// libopus decoder), the decoder wrapper's TOC handling and float scaling are
// right, and the console-facing stream carries the captured signal at the
// captured level on both channels. What it does not prove: timing on the
// RP2350 (measured on hardware as `caudio mic decode` max_us).
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "companion_mic_decode.h"
#include "ns2_companion_mic.h"

#define MAX_PACKETS 64
#define WARMUP_BLOCKS 5

static uint8_t packets[MAX_PACKETS][NS2_COMPANION_MIC_OPUS_BYTES];
static int packet_count;
static double signal_hz, signal_amplitude;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void load_fixture(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    char line[1024];
    int frames = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "signal sine %lf Hz amplitude %lf frames %d",
                   &signal_hz, &signal_amplitude, &frames) == 3)
            continue;
        if (strncmp(line, "packet ", 7) != 0) continue;
        assert(packet_count < MAX_PACKETS);
        const char *hex = line + 7;
        for (unsigned i = 0; i < NS2_COMPANION_MIC_OPUS_BYTES; i++) {
            int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
            assert(hi >= 0 && lo >= 0);
            packets[packet_count][i] = (uint8_t)(hi << 4 | lo);
        }
        // Exactly one packet's worth of hex, nothing more.
        assert(hex_nibble(hex[2 * NS2_COMPANION_MIC_OPUS_BYTES]) < 0);
        packet_count++;
    }
    fclose(f);
    assert(signal_hz > 0 && signal_amplitude > 0);
    assert(packet_count == frames);
}

// Energy at hz, on the same scale as the sum of squares.
static double goertzel(const int16_t *x, size_t n, size_t stride, double hz)
{
    double const coeff = 2.0 * cos(2.0 * M_PI * hz / 48000.0);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        double const s0 = x[i * stride] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2.0 * (s1 * s1 + s2 * s2 - coeff * s1 * s2) / (double)n;
}

static void *new_decoder(void)
{
    size_t const size = companion_mic_decoder_size();
    assert(size > 0);
    void *const storage = malloc(size);
    void *const dec = companion_mic_decoder_init(storage, size);
    assert(dec == storage);
    // Undersized storage is refused, never overrun.
    uint8_t small[64];
    assert(companion_mic_decoder_init(small, sizeof small) == NULL);
    return dec;
}

static void test_decoder_reproduces_the_companions_signal(void)
{
    void *const dec = new_decoder();
    int16_t pcm[NS2_COMPANION_MIC_BLOCK_FRAMES];
    double tone = 0, total = 0;
    for (int b = 0; b < packet_count; b++) {
        assert(companion_mic_decode(dec, packets[b], pcm));
        if (b < WARMUP_BLOCKS) continue;
        tone += goertzel(pcm, NS2_COMPANION_MIC_BLOCK_FRAMES, 1, signal_hz);
        for (unsigned i = 0; i < NS2_COMPANION_MIC_BLOCK_FRAMES; i++)
            total += (double)pcm[i] * pcm[i];
    }
    double const blocks = packet_count - WARMUP_BLOCKS;
    double const rms = sqrt(total / (blocks * NS2_COMPANION_MIC_BLOCK_FRAMES));
    double const expected = signal_amplitude / sqrt(2.0);
    double const level_db = 20.0 * log10(rms / expected);
    printf("  decode: rms %.0f (expected %.0f, %+.2f dB), tone share %.4f\n",
           rms, expected, level_db, tone / total);
    assert(fabs(level_db) < 1.0);
    assert(tone / total > 0.95);

    // Concealment (NULL) keeps producing whole blocks and decays rather than
    // repeating the tone at full level.
    double plc_energy = 0;
    for (int b = 0; b < 3; b++) {
        assert(companion_mic_decode(dec, NULL, pcm));
        for (unsigned i = 0; i < NS2_COMPANION_MIC_BLOCK_FRAMES; i++)
            plc_energy += (double)pcm[i] * pcm[i];
    }
    double const plc_rms = sqrt(plc_energy / (3.0 * NS2_COMPANION_MIC_BLOCK_FRAMES));
    printf("  conceal: rms over 3 blocks %.0f\n", plc_rms);
    assert(plc_rms < rms);
    free(dec);
}

static ns2_companion_mic_ring_t ring;

static void test_usb_stream_carries_the_signal_on_both_channels(void)
{
    void *const dec = new_decoder();
    ns2_companion_mic_sequence_t seq;
    ns2_companion_mic_sequence_reset(&seq);
    ns2_companion_mic_ring_reset(&ring, NS2_COMPANION_MIC_BLE_TARGET_BLOCKS);

    // The fixture as MIC-UP writes, one per 20 ms, with the USB side reading
    // twenty 1 ms packets after each: the ring prebuffers its target first
    // (silence), then plays. Writing everything at once would exceed the
    // headroom and be shed, which is the ring doing its job.
    size_t const total_frames = (size_t)packet_count * NS2_COMPANION_MIC_BLOCK_FRAMES;
    int16_t *const usb = calloc(total_frames * 2, sizeof(int16_t));
    size_t usb_ms = 0;
    for (int b = 0; b < packet_count; b++) {
        uint8_t frame[NS2_COMPANION_MIC_FRAME_BYTES];
        frame[0] = NS2_COMPANION_MIC_VERSION;
        frame[1] = NS2_COMPANION_MIC_OP_OPUS;
        frame[2] = (uint8_t)b;
        frame[3] = 0;
        memcpy(frame + NS2_COMPANION_MIC_HEADER_BYTES, packets[b], NS2_COMPANION_MIC_OPUS_BYTES);
        uint16_t sequence = 0;
        const uint8_t *opus = NULL;
        assert(ns2_companion_mic_parse(frame, sizeof frame, &sequence, &opus) ==
               NS2_COMPANION_MIC_PARSE_OK);
        assert(ns2_companion_mic_sequence_accept(&seq, sequence) == 0);
        int16_t pcm[NS2_COMPANION_MIC_BLOCK_FRAMES];
        assert(companion_mic_decode(dec, opus, pcm));
        bool extend = false;
        assert(ns2_companion_mic_ring_write(&ring, pcm, NS2_COMPANION_MIC_BLOCK_FRAMES, false, &extend));
        assert(!extend);
        for (unsigned ms = 0; ms < NS2_COMPANION_MIC_FRAME_MS; ms++, usb_ms++)
            ns2_companion_mic_ring_read_usb(&ring, usb + usb_ms * NS2_COMPANION_MIC_USB_FRAMES * 2, 32767u);
    }
    assert(ring.underruns == 0 && ring.blocks_dropped == 0);

    // Output starts once the target is queued, (target - 1) blocks of silence
    // in; skip those and the decoder warm-up, analyse what follows.
    size_t const start = (NS2_COMPANION_MIC_BLE_TARGET_BLOCKS - 1 + WARMUP_BLOCKS) * NS2_COMPANION_MIC_BLOCK_FRAMES;
    size_t const n = total_frames - start;
    for (size_t i = 0; i < (NS2_COMPANION_MIC_BLE_TARGET_BLOCKS - 1) * NS2_COMPANION_MIC_BLOCK_FRAMES * 2; i++)
        assert(usb[i] == 0);
    double total = 0;
    for (size_t i = 0; i < n; i++) {
        int16_t const l = usb[(start + i) * 2], r = usb[(start + i) * 2 + 1];
        assert(l == r);  // mono capsule, both channels
        total += (double)l * l;
    }
    double const tone = goertzel(usb + start * 2, n, 2, signal_hz);
    double const rms = sqrt(total / (double)n);
    double const level_db = 20.0 * log10(rms / (signal_amplitude / sqrt(2.0)));
    printf("  usb: rms %.0f (%+.2f dB), tone share %.4f\n", rms, level_db, tone / total);
    assert(fabs(level_db) < 1.0);
    assert(tone / total > 0.95);

    // Mute is silence, not a quieter tone. The reset's flush is applied by
    // the consumer (the producer cannot move `head`), so one packet is read
    // before refilling, as the USB side does every millisecond on hardware.
    ns2_companion_mic_ring_reset(&ring, NS2_COMPANION_MIC_BLE_TARGET_BLOCKS);
    int16_t packet[NS2_COMPANION_MIC_USB_FRAMES * 2];
    ns2_companion_mic_ring_read_usb(&ring, packet, 32767u);
    assert(ns2_companion_mic_ring_queued(&ring) == 0);
    int16_t pcm[NS2_COMPANION_MIC_BLOCK_FRAMES];
    for (int b = 0; b < (int)NS2_COMPANION_MIC_BLE_TARGET_BLOCKS + 1; b++) {
        bool extend = false;
        assert(companion_mic_decode(dec, packets[b % packet_count], pcm));
        assert(ns2_companion_mic_ring_write(&ring, pcm, NS2_COMPANION_MIC_BLOCK_FRAMES, false, &extend));
    }
    ns2_companion_mic_ring_read_usb(&ring, packet, ns2_companion_mic_gain_q15(true, 0));
    for (unsigned i = 0; i < NS2_COMPANION_MIC_USB_FRAMES * 2; i++) assert(packet[i] == 0);

    free(usb);
    free(dec);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    load_fixture(argc > 1 ? argv[1] : "contracts/companion_mic_vectors.txt");
    printf("test_companion_mic_decode: %d packets, %.0f Hz at %.0f, decoder state %zu B\n",
           packet_count, signal_hz, signal_amplitude, companion_mic_decoder_size());
    test_decoder_reproduces_the_companions_signal();
    test_usb_stream_carries_the_signal_on_both_channels();
    printf("test_companion_mic_decode: OK\n");
    return 0;
}
