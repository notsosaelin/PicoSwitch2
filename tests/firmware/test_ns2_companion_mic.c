// Host tests for the companion microphone uplink (ns2_companion_mic.h).
//
// Pins the wire contract a companion encoder must meet, the sequence and
// concealment rules, the lock-free playout ring's behaviour, and -- by
// simulation, since the physical path cannot be exercised here -- that an
// hour of microphone audio over the measured link timing plays to the
// console's USB clock without running dry once settled, with the two clocks
// 600 ppm apart.

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ns2_companion_mic.h"

static ns2_companion_mic_ring_t ring;

static void make_frame(uint8_t *f, uint16_t seq)
{
    memset(f, 0, NS2_COMPANION_MIC_FRAME_BYTES);
    f[0] = NS2_COMPANION_MIC_VERSION;
    f[1] = NS2_COMPANION_MIC_OP_OPUS;
    f[2] = (uint8_t)(seq & 0xFFu);
    f[3] = (uint8_t)(seq >> 8);
    f[4] = NS2_COMPANION_MIC_TOC;
}

static void test_wire(void)
{
    // 64 kbit/s CBR, 20 ms: 160-byte packets, 164-byte frames; above the
    // DualSense's own 56.8 kbit/s microphone stream (ns2_companion_mic.h).
    assert(NS2_COMPANION_MIC_OPUS_BYTES == NS2_COMPANION_MIC_KBPS * 1000u / 8u / 50u);
    assert(NS2_COMPANION_MIC_FRAME_BYTES == 164u);
    assert(NS2_COMPANION_MIC_KBPS * 1000u >= 56800u);
    // TOC 0xF8: config 31 (CELT-only FB 20 ms), mono (s=0), code 0.
    assert((NS2_COMPANION_MIC_TOC >> 3) == 31u);
    assert((NS2_COMPANION_MIC_TOC & 0x04u) == 0u);
    assert((NS2_COMPANION_MIC_TOC & 0x03u) == 0u);
    // One write fits the management link's 247-byte minimum MTU.
    assert(NS2_COMPANION_MIC_FRAME_BYTES <= 247u - 3u);

    uint8_t f[NS2_COMPANION_MIC_FRAME_BYTES + 4];
    uint16_t seq = 0;
    const uint8_t *opus = NULL;
    make_frame(f, 0x1234);
    assert(ns2_companion_mic_parse(f, NS2_COMPANION_MIC_FRAME_BYTES, &seq, &opus) ==
           NS2_COMPANION_MIC_PARSE_OK);
    assert(seq == 0x1234u);
    assert(opus == &f[4] && opus[0] == 0xF8u);

    assert(ns2_companion_mic_parse(f, 4, &seq, &opus) == NS2_COMPANION_MIC_PARSE_SHORT);
    assert(ns2_companion_mic_parse(NULL, 164, &seq, &opus) == NS2_COMPANION_MIC_PARSE_SHORT);
    assert(ns2_companion_mic_parse(f, 163, &seq, &opus) == NS2_COMPANION_MIC_PARSE_SIZE);
    assert(ns2_companion_mic_parse(f, 165, &seq, &opus) == NS2_COMPANION_MIC_PARSE_SIZE);
    f[0] = 2;
    assert(ns2_companion_mic_parse(f, 164, &seq, &opus) == NS2_COMPANION_MIC_PARSE_VERSION);
    f[0] = 1;
    f[1] = 0x90;  // the DOWNLINK opcode is not a microphone frame
    assert(ns2_companion_mic_parse(f, 164, &seq, &opus) == NS2_COMPANION_MIC_PARSE_OPCODE);
    f[1] = 0x91;
    f[4] = 0xFC;  // stereo: not the microphone format
    assert(ns2_companion_mic_parse(f, 164, &seq, &opus) == NS2_COMPANION_MIC_PARSE_TOC);
    f[4] = 0x78;  // hybrid/SILK config: would need the SILK decoder
    assert(ns2_companion_mic_parse(f, 164, &seq, &opus) == NS2_COMPANION_MIC_PARSE_TOC);

    assert(strcmp(ns2_companion_mic_parse_name(NS2_COMPANION_MIC_PARSE_TOC), "toc") == 0);
    assert(strcmp(ns2_companion_mic_parse_name(NS2_COMPANION_MIC_PARSE_OK), "ok") == 0);
}

static void test_sequence(void)
{
    ns2_companion_mic_sequence_t s;
    ns2_companion_mic_sequence_reset(&s);
    assert(ns2_companion_mic_sequence_accept(&s, 100) == 0);  // first: anything
    assert(ns2_companion_mic_sequence_accept(&s, 101) == 0);
    assert(ns2_companion_mic_sequence_accept(&s, 104) == 2);  // 102, 103 lost
    assert(ns2_companion_mic_sequence_accept(&s, 104) == -1); // duplicate
    assert(ns2_companion_mic_sequence_accept(&s, 103) == -1); // late: out of order
    assert(ns2_companion_mic_sequence_accept(&s, 105) == 0);
    // Wrap.
    ns2_companion_mic_sequence_reset(&s);
    assert(ns2_companion_mic_sequence_accept(&s, 0xFFFF) == 0);
    assert(ns2_companion_mic_sequence_accept(&s, 0x0000) == 0);
    assert(ns2_companion_mic_sequence_accept(&s, 0x0002) == 1);
    // A restart of the companion's capture: a discontinuity, then in step.
    const int jump = ns2_companion_mic_sequence_accept(&s, 0x4000);
    assert(jump > (int)NS2_COMPANION_MIC_MAX_GAP);
    assert(ns2_companion_mic_sequence_accept(&s, 0x4001) == 0);

    assert(ns2_companion_mic_conceal_count(-1) == 0u);
    assert(ns2_companion_mic_conceal_count(0) == 0u);
    assert(ns2_companion_mic_conceal_count(1) == 1u);
    assert(ns2_companion_mic_conceal_count(2) == 2u);
    assert(ns2_companion_mic_conceal_count(7) == NS2_COMPANION_MIC_MAX_CONCEALED);
    assert(ns2_companion_mic_conceal_count(jump) == 0u);  // never across a restart
}

static void test_stretch(void)
{
    int16_t in[960], out[962];
    for (int i = 0; i < 960; i++) in[i] = (int16_t)(i * 30 - 14000);
    // Unchanged length: exact copy.
    assert(ns2_companion_mic_stretch(in, 960, out, 960) == 960u);
    assert(memcmp(in, out, sizeof(in)) == 0);
    // One frame longer and shorter: endpoints exact, a ramp stays a ramp.
    const uint16_t lengths[] = {961, 959};
    for (int k = 0; k < 2; k++) {
        const uint16_t n = lengths[k];
        assert(ns2_companion_mic_stretch(in, 960, out, n) == n);
        assert(out[0] == in[0]);
        assert(out[n - 1] == in[959]);
        for (int i = 1; i < n; i++) assert(out[i] >= out[i - 1]);
        // Linear: within a sample of the ideal ramp.
        for (int i = 0; i < n; i++) {
            const double ideal = -14000.0 + 30.0 * (959.0 * i / (n - 1));
            assert(fabs(out[i] - ideal) <= 1.0);
        }
    }
}

static void test_gain(void)
{
    assert(ns2_companion_mic_gain_q15(true, 0) == 0u);
    assert(ns2_companion_mic_gain_q15(false, 0) == 32767u);
    assert(ns2_companion_mic_gain_q15(false, 512) == 32767u);  // never above unity
    const uint16_t minus6 = ns2_companion_mic_gain_q15(false, -6 * 256);
    assert(minus6 >= 16400u && minus6 <= 16440u);
    const uint16_t minus60 = ns2_companion_mic_gain_q15(false, -60 * 256);
    assert(minus60 >= 32u && minus60 <= 34u);
}

static void read_packet(int16_t *out) { ns2_companion_mic_ring_read_usb(&ring, out, 32767u); }

static void test_ring_prebuffer_and_order(void)
{
    memset(&ring, 0, sizeof(ring));
    ns2_companion_mic_ring_reset(&ring, 3);
    int16_t block[960];
    int16_t usb[96];
    bool extend = false;

    // Silence until the target (3 blocks, 60 ms) is queued.
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < 960; i++) block[i] = (int16_t)(b * 960 + i);
        assert(ns2_companion_mic_ring_write(&ring, block, 960, false, &extend));
        assert(!extend);
    }
    read_packet(usb);
    for (int i = 0; i < 96; i++) assert(usb[i] == 0);
    assert(!ring.playing);
    for (int i = 0; i < 960; i++) block[i] = (int16_t)(2 * 960 + i);
    assert(ns2_companion_mic_ring_write(&ring, block, 960, false, &extend));
    assert(ns2_companion_mic_ring_queued(&ring) == 2880u);

    // Then the samples come out in order, mono on both channels.
    for (int p = 0; p < 60; p++) {
        read_packet(usb);
        for (int i = 0; i < 48; i++) {
            assert(usb[2 * i] == (int16_t)(p * 48 + i));
            assert(usb[2 * i + 1] == usb[2 * i]);
        }
    }
    assert(ring.playing);
    assert(ns2_companion_mic_ring_queued(&ring) == 0u);
    assert(ring.underruns == 0u);

    // Running dry pads with silence, counts an underrun, re-prebuffers...
    read_packet(usb);
    for (int i = 0; i < 96; i++) assert(usb[i] == 0);
    assert(!ring.playing);
    assert(ring.underruns == 1u);
    // ...and the next block deepens the target for the session.
    assert(ns2_companion_mic_ring_write(&ring, block, 960, false, &extend));
    assert(ring.target_blocks == 4u);
}

static void test_ring_gain_and_flush(void)
{
    memset(&ring, 0, sizeof(ring));
    ns2_companion_mic_ring_reset(&ring, 3);
    int16_t block[960];
    int16_t usb[96];
    for (int i = 0; i < 960; i++) block[i] = 20000;
    for (int b = 0; b < 3; b++) assert(ns2_companion_mic_ring_write(&ring, block, 960, false, NULL));
    ns2_companion_mic_ring_read_usb(&ring, usb, 0u);  // muted
    for (int i = 0; i < 96; i++) assert(usb[i] == 0);
    ns2_companion_mic_ring_read_usb(&ring, usb, 16384u);  // -6 dB
    assert(usb[0] == 10000);

    // A new session drops what the last one left queued, on the consumer's
    // next packet, and starts prebuffering again.
    ns2_companion_mic_ring_reset(&ring, NS2_COMPANION_MIC_CLASSIC_TARGET_BLOCKS);
    read_packet(usb);
    assert(ns2_companion_mic_ring_queued(&ring) == 0u);
    assert(!ring.playing);
    assert(ring.target_blocks == NS2_COMPANION_MIC_CLASSIC_TARGET_BLOCKS);
    // The ceiling holds whatever the caller asks for.
    ns2_companion_mic_ring_reset(&ring, 200);
    assert(ring.target_blocks == NS2_COMPANION_MIC_MAX_TARGET_BLOCKS);
}

static void test_ring_headroom(void)
{
    // A consumer that stopped reading, or a burst after a radio stall, is
    // bounded at the target plus the headroom: the excess is shed at once
    // rather than played a burst late.
    memset(&ring, 0, sizeof(ring));
    ns2_companion_mic_ring_reset(&ring, NS2_COMPANION_MIC_BLE_TARGET_BLOCKS);
    int16_t block[960] = {0};
    int written = 0;
    for (int b = 0; b < 20; b++) written += ns2_companion_mic_ring_write(&ring, block, 960, false, NULL);
    assert(written == (int)(NS2_COMPANION_MIC_BLE_TARGET_BLOCKS + NS2_COMPANION_MIC_HEADROOM_BLOCKS));
    assert(ring.blocks_dropped == (uint32_t)(20 - written));
    // Even at the ceiling the ring itself is never overrun.
    ns2_companion_mic_ring_reset(&ring, NS2_COMPANION_MIC_MAX_TARGET_BLOCKS);
    int16_t usb[96];
    read_packet(usb);  // applies the flush
    for (int b = 0; b < 20; b++) ns2_companion_mic_ring_write(&ring, block, 960, false, NULL);
    assert(ns2_companion_mic_ring_queued(&ring) <=
           (NS2_COMPANION_MIC_MAX_TARGET_BLOCKS + NS2_COMPANION_MIC_HEADROOM_BLOCKS) * 960u);
    assert(ns2_companion_mic_ring_queued(&ring) <= NS2_COMPANION_MIC_RING_FRAMES);
}

// ---------------------------------------------------------------------------
// Simulation: an hour of microphone audio over the link, against the USB clock.
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;
    double interval_us;     // BLE connection interval
    int pdus_per_event;     // ASSUMPTION, as for the downlink model
    double miss;            // events that carry nothing (retransmit / Classic slot)
    double host_jitter_us;  // companion capture -> write handed to the radio
    double decode_max_us;   // core-1 decode, including a speaker encode ahead of it
    double companion_ppm;   // capture clock
    double console_ppm;     // USB clock
    uint8_t initial_target; // the session's starting depth (link type)
    unsigned max_underruns_after_warmup;
    unsigned max_lost;      // frames the companion's bounded writer may shed
    unsigned max_dropped;   // bursts shed at the headroom (a 20 ms skip each)
    bool hold_drift;        // assert the drift error at the end
} sim_profile_t;

typedef struct {
    unsigned underruns, underruns_after_warmup, dropped, concealed, lost;
    double final_error_ms, p50_ms, max_ms;
} sim_result_t;

static double urand(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return ((*state >> 8) & 0xFFFFFFu) / (double)0x1000000;
}

#define SIM_SECONDS 3600
#define SIM_WARMUP_SECONDS 600
// The companion's bounded writer: frames waiting for the radio (160 ms).
#define WRITER_DEPTH 8

static sim_result_t simulate(const sim_profile_t *p, unsigned seed)
{
    memset(&ring, 0, sizeof(ring));
    ns2_companion_mic_ring_reset(&ring, p->initial_target);
    ns2_companion_mic_sequence_t seq;
    ns2_companion_mic_sequence_reset(&seq);
    static int16_t block[960];
    static unsigned histogram[400];
    memset(histogram, 0, sizeof(histogram));
    sim_result_t r = {0};

    const double frame_us = 20000.0 / (1.0 + p->companion_ppm * 1e-6);
    const double usb_us = 1000.0 / (1.0 + p->console_ppm * 1e-6);
    // Capture outlasts the run: with the companion's clock the faster one, an
    // exact hour of frames would end before the console's hour does and the
    // last packets would run dry -- an artefact, not an underrun.
    const unsigned frames_total = (SIM_SECONDS + 2u) * 50u;
    unsigned next_frame = 0;          // next to capture
    double writer_ready[WRITER_DEPTH];
    unsigned writer_seq[WRITER_DEPTH];
    unsigned writer_count = 0;
    double last_ready = 0;
    double next_event = 0;
    double next_usb = usb_us;
    double decoder_free = 0;
    // Packets on their way to the decoder (time decoded, sequence).
    double inflight_at[64];
    unsigned inflight_seq[64];
    unsigned inflight_head = 0, inflight_tail = 0;
    const double end_us = SIM_SECONDS * 1e6;
    const unsigned warmup_underruns_mark = 0;
    unsigned underruns_at_warmup = 0;
    bool warm = false;
    (void)warmup_underruns_mark;

    while (next_usb < end_us) {
        // Earliest pending event.
        const double capture_at = next_frame < frames_total ? (next_frame + 1) * frame_us : 1e30;
        const double decode_at = inflight_head != inflight_tail ? inflight_at[inflight_head % 64] : 1e30;
        double t = next_usb;
        int kind = 0;
        if (capture_at < t) { t = capture_at; kind = 1; }
        if (next_event < t) { t = next_event; kind = 2; }
        if (decode_at < t) { t = decode_at; kind = 3; }

        if (!warm && t >= SIM_WARMUP_SECONDS * 1e6) {
            warm = true;
            underruns_at_warmup = ring.underruns;
        }

        if (kind == 0) {
            int16_t usb[96];
            const uint32_t before = ring.underruns;
            read_packet(usb);
#ifdef MIC_SIM_TRACE
            if (ring.underruns != before)
                printf("    underrun at %.3f s: writer %u, inflight %u, next frame %u, last ready %.3f s, decoder free %.3f s, target %u\n",
                       t / 1e6, writer_count, inflight_tail - inflight_head, next_frame, last_ready / 1e6, decoder_free / 1e6, ring.target_blocks);
#else
            (void)before;
#endif
            if (ring.playing && warm) {
                const unsigned ms = ns2_companion_mic_ring_queued(&ring) / 48u;
                histogram[ms < 399 ? ms : 399]++;
            }
            next_usb += usb_us;
        } else if (kind == 1) {
            // Captured; the host stack hands it to the radio after jitter.
            double ready = t + urand(&seed) * p->host_jitter_us;
            if (ready < last_ready) ready = last_ready;
            last_ready = ready;
            if (writer_count == WRITER_DEPTH) {
                // Bounded writer: the oldest waiting frame gives way.
                memmove(&writer_ready[0], &writer_ready[1], (WRITER_DEPTH - 1) * sizeof(double));
                memmove(&writer_seq[0], &writer_seq[1], (WRITER_DEPTH - 1) * sizeof(unsigned));
                writer_count--;
                r.lost++;
            }
            writer_ready[writer_count] = ready;
            writer_seq[writer_count] = next_frame;
            writer_count++;
            next_frame++;
        } else if (kind == 2) {
            if (urand(&seed) >= p->miss) {
                int sent = 0;
                while (sent < p->pdus_per_event && writer_count > 0 && writer_ready[0] <= t) {
                    double at = t + 1500.0 + urand(&seed) * p->decode_max_us;
                    if (at < decoder_free) at = decoder_free;
                    decoder_free = at;
                    inflight_at[inflight_tail % 64] = at;
                    inflight_seq[inflight_tail % 64] = writer_seq[0];
                    inflight_tail++;
                    memmove(&writer_ready[0], &writer_ready[1], (WRITER_DEPTH - 1) * sizeof(double));
                    memmove(&writer_seq[0], &writer_seq[1], (WRITER_DEPTH - 1) * sizeof(unsigned));
                    writer_count--;
                    sent++;
                }
            }
            next_event += p->interval_us;
        } else {
            const unsigned s = inflight_seq[inflight_head % 64];
            inflight_head++;
            const int gap = ns2_companion_mic_sequence_accept(&seq, (uint16_t)s);
            if (gap < 0) continue;
            bool extend = false;
            for (uint8_t c = ns2_companion_mic_conceal_count(gap); c > 0; c--) {
                ns2_companion_mic_ring_write(&ring, block, 960, true, &extend);
                r.concealed++;
            }
            if (!ns2_companion_mic_ring_write(&ring, block, 960, false, &extend)) r.dropped++;
            if (extend) {
                ns2_companion_mic_ring_write(&ring, block, 960, true, NULL);
                r.concealed++;
            }
        }
    }

    r.underruns = ring.underruns;
    r.underruns_after_warmup = ring.underruns - underruns_at_warmup;
    r.dropped += ring.blocks_dropped;
    r.final_error_ms = ring.smoothed_q8 / 256.0 / 48.0 -
                       (ring.target_blocks - 1) * 20.0;
    unsigned long total = 0, acc = 0;
    for (int i = 0; i < 400; i++) total += histogram[i];
    for (int i = 0; i < 400; i++) {
        acc += histogram[i];
        if (r.p50_ms == 0 && acc * 2 >= total) r.p50_ms = i;
        if (histogram[i]) r.max_ms = i;
    }
    return r;
}

static void test_simulation(void)
{
    // Link figures from the downlink budget (docs/experiments/
    // companion-audio-downlink-prototype-2026-11-07.md): 15 ms interval,
    // 2 PDUs an event (ASSUMPTION), 2 % lost events on an all-BLE link, 20 %
    // with Bluetooth Classic on the radio (ASSUMPTION, the downlink's
    // pessimistic Android figure). Decode <= 3 ms, plus a 10.2 ms worst-case
    // speaker encode ahead of it. Clocks 600 ppm apart, both directions.
    const sim_profile_t profiles[] = {
        {"BLE Controller Link", 15000, 2, 0.02, 8000, 13200, +100, -500,
         NS2_COMPANION_MIC_BLE_TARGET_BLOCKS, 0, 0, 0, true},
        {"BLE Controller Link, clocks reversed", 15000, 2, 0.02, 8000, 13200, -500, +100,
         NS2_COMPANION_MIC_BLE_TARGET_BLOCKS, 0, 0, 0, true},
        {"Android (Classic on the radio)", 15000, 2, 0.20, 8000, 13200, -100, +500,
         NS2_COMPANION_MIC_CLASSIC_TARGET_BLOCKS, 2, 0, 15, true},
        // Beyond anything measured: bounded and recovering, never a backlog.
        {"Stress (33 % empty events, 15 ms jitter)", 15000, 2, 0.33, 15000, 13200, +500, -100,
         NS2_COMPANION_MIC_CLASSIC_TARGET_BLOCKS, 60, 20, 400, false},
    };
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        for (unsigned seed = 1; seed <= 3; seed++) {
            const sim_result_t r = simulate(&profiles[i], seed * 7919u);
            printf("  mic sim %-42s seed %u: underruns %u (after warm-up %u), "
                   "dropped %u, concealed %u, writer lost %u, queue p50 %.0f ms max %.0f ms, "
                   "final error %+.2f ms, target %u blocks\n",
                   profiles[i].name, seed, r.underruns, r.underruns_after_warmup,
                   r.dropped, r.concealed, r.lost, r.p50_ms, r.max_ms,
                   r.final_error_ms, ring.target_blocks);
            fflush(stdout);
            if (getenv("MIC_SIM_REPORT")) continue;
            assert(r.underruns_after_warmup <= profiles[i].max_underruns_after_warmup);
            assert(r.lost <= profiles[i].max_lost);
            assert(r.dropped <= profiles[i].max_dropped);
            if (profiles[i].hold_drift) {
                // Drift held: within the deadband of the target after an hour.
                assert(fabs(r.final_error_ms) <= 5.0);
            }
            // Latency bounded by the deepest target plus the headroom, always.
            assert(r.max_ms <= (NS2_COMPANION_MIC_MAX_TARGET_BLOCKS +
                                NS2_COMPANION_MIC_HEADROOM_BLOCKS) * 20.0);
        }
    }
}

int main(void)
{
    test_wire();
    test_sequence();
    test_stretch();
    test_gain();
    test_ring_prebuffer_and_order();
    test_ring_gain_and_flush();
    test_ring_headroom();
    test_simulation();
    printf("test_ns2_companion_mic: all tests passed\n");
    return 0;
}
