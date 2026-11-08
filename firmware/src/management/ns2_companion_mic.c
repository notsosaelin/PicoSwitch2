// ns2_companion_mic.c - Companion microphone uplink, pure logic.
// See ns2_companion_mic.h. No BTstack, no SDK, no codec: host-testable.
#include "ns2_companion_mic.h"

#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Wire
// ---------------------------------------------------------------------------

ns2_companion_mic_parse_t ns2_companion_mic_parse(const uint8_t *frame,
                                                  uint16_t len,
                                                  uint16_t *sequence,
                                                  const uint8_t **opus)
{
    if (!frame || len < NS2_COMPANION_MIC_HEADER_BYTES + 1u)
        return NS2_COMPANION_MIC_PARSE_SHORT;
    if (frame[0] != NS2_COMPANION_MIC_VERSION)
        return NS2_COMPANION_MIC_PARSE_VERSION;
    if (frame[1] != NS2_COMPANION_MIC_OP_OPUS)
        return NS2_COMPANION_MIC_PARSE_OPCODE;
    if (len != NS2_COMPANION_MIC_FRAME_BYTES)
        return NS2_COMPANION_MIC_PARSE_SIZE;
    if (frame[NS2_COMPANION_MIC_HEADER_BYTES] != NS2_COMPANION_MIC_TOC)
        return NS2_COMPANION_MIC_PARSE_TOC;
    if (sequence)
        *sequence = (uint16_t)(frame[2] | ((uint16_t)frame[3] << 8));
    if (opus) *opus = &frame[NS2_COMPANION_MIC_HEADER_BYTES];
    return NS2_COMPANION_MIC_PARSE_OK;
}

const char *ns2_companion_mic_parse_name(ns2_companion_mic_parse_t result)
{
    switch (result) {
        case NS2_COMPANION_MIC_PARSE_OK: return "ok";
        case NS2_COMPANION_MIC_PARSE_SHORT: return "short";
        case NS2_COMPANION_MIC_PARSE_VERSION: return "version";
        case NS2_COMPANION_MIC_PARSE_OPCODE: return "opcode";
        case NS2_COMPANION_MIC_PARSE_SIZE: return "size";
        case NS2_COMPANION_MIC_PARSE_TOC: return "toc";
    }
    return "unknown";
}

void ns2_companion_mic_sequence_reset(ns2_companion_mic_sequence_t *s)
{
    if (!s) return;
    s->have = false;
    s->next = 0u;
}

int ns2_companion_mic_sequence_accept(ns2_companion_mic_sequence_t *s,
                                      uint16_t sequence)
{
    if (!s) return 0;
    if (!s->have) {
        s->have = true;
        s->next = (uint16_t)(sequence + 1u);
        return 0;
    }
    const uint16_t ahead = (uint16_t)(sequence - s->next);
    if (ahead >= 0x8000u) return -1;  // duplicate or older: out of order
    s->next = (uint16_t)(sequence + 1u);
    if (ahead > NS2_COMPANION_MIC_MAX_GAP) return (int)NS2_COMPANION_MIC_MAX_GAP + 1;
    return (int)ahead;
}

uint8_t ns2_companion_mic_conceal_count(int gap)
{
    if (gap <= 0 || gap > (int)NS2_COMPANION_MIC_MAX_GAP) return 0u;
    return gap > (int)NS2_COMPANION_MIC_MAX_CONCEALED
               ? (uint8_t)NS2_COMPANION_MIC_MAX_CONCEALED
               : (uint8_t)gap;
}

// ---------------------------------------------------------------------------
// Ring
// ---------------------------------------------------------------------------

#define RING_MASK (NS2_COMPANION_MIC_RING_FRAMES - 1u)
#define LOW_WATER_FRAMES (10u * 48u)  // CompanionAudioPlayout.LowWaterMillis

static uint32_t load_acquire(const volatile uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_release(volatile uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

uint32_t ns2_companion_mic_ring_queued(const ns2_companion_mic_ring_t *r)
{
    if (!r) return 0u;
    return load_acquire(&r->tail) - load_acquire(&r->head);
}

void ns2_companion_mic_ring_reset(ns2_companion_mic_ring_t *r,
                                  uint8_t initial_target_blocks)
{
    if (!r) return;
    if (initial_target_blocks < 1u) initial_target_blocks = 1u;
    if (initial_target_blocks > NS2_COMPANION_MIC_MAX_TARGET_BLOCKS)
        initial_target_blocks = (uint8_t)NS2_COMPANION_MIC_MAX_TARGET_BLOCKS;
    r->target_blocks = initial_target_blocks;
    r->underruns_seen = __atomic_load_n(&r->underruns, __ATOMIC_ACQUIRE);
    r->drift_blocks = 0u;
    r->smoothed_q8 = 0;
    r->correcting = 0;
    r->frames_adjusted = 0;
    r->blocks_written = 0u;
    r->blocks_dropped = 0u;
    r->blocks_concealed = 0u;
    r->extensions = 0u;
    // The producer cannot move `head`: it publishes where the consumer is to
    // skip to, and the consumer applies it on its next packet.
    store_release(&r->flush_to, r->tail);
    __atomic_store_n(&r->flush_requested, true, __ATOMIC_RELEASE);
}

// The drift law of CompanionAudioDriftControl.FramesFor, in frames.
static uint16_t drift_frames_for(ns2_companion_mic_ring_t *r, uint32_t queued)
{
    const int32_t queued_q8 = (int32_t)(queued << 8);
    if (r->drift_blocks == 0u) {
        r->smoothed_q8 = queued_q8;
    } else {
        r->smoothed_q8 += (queued_q8 - r->smoothed_q8) /
                          NS2_COMPANION_MIC_SMOOTHING_BLOCKS;
    }
    r->drift_blocks++;
    if (r->drift_blocks < NS2_COMPANION_MIC_SETTLE_BLOCKS)
        return (uint16_t)NS2_COMPANION_MIC_BLOCK_FRAMES;

    const int32_t target_q8 =
        (int32_t)(((uint32_t)r->target_blocks - 1u) *
                  NS2_COMPANION_MIC_BLOCK_FRAMES) << 8;
    const int32_t error = r->smoothed_q8 - target_q8;
    const int8_t toward = error > 0 ? -1 : 1;
    const int32_t magnitude = error < 0 ? -error : error;
    if (magnitude > (int32_t)(NS2_COMPANION_MIC_DEADBAND_FRAMES << 8)) {
        // Always toward the target, including when the target moved past
        // the queue while a correction was under way.
        r->correcting = toward;
    } else if (r->correcting != toward ||
               magnitude < (int32_t)(NS2_COMPANION_MIC_RELEASE_FRAMES << 8)) {
        r->correcting = 0;
    }
    r->frames_adjusted += r->correcting;
    return (uint16_t)((int32_t)NS2_COMPANION_MIC_BLOCK_FRAMES + r->correcting);
}

bool ns2_companion_mic_ring_write(ns2_companion_mic_ring_t *r,
                                  const int16_t *pcm, uint16_t frames,
                                  bool concealed, bool *extend)
{
    if (extend) *extend = false;
    if (!r || !pcm || frames == 0u ||
        frames > NS2_COMPANION_MIC_BLOCK_FRAMES)
        return false;

    // The consumer ran dry since the last block: deepen the target for the
    // rest of the session and let the drift control start over.
    const uint32_t underruns = __atomic_load_n(&r->underruns, __ATOMIC_ACQUIRE);
    if (underruns != r->underruns_seen) {
        r->underruns_seen = underruns;
        if (r->target_blocks < NS2_COMPANION_MIC_MAX_TARGET_BLOCKS)
            r->target_blocks++;
        r->drift_blocks = 0u;
        r->correcting = 0;
    }

    const uint32_t tail = r->tail;
    const uint32_t queued = tail - load_acquire(&r->head);
    uint16_t out_frames = frames;
    if (__atomic_load_n(&r->playing, __ATOMIC_ACQUIRE)) {
        // Nearly dry: grow now, while there is still audio to conceal from.
        if (queued < LOW_WATER_FRAMES &&
            r->target_blocks < NS2_COMPANION_MIC_MAX_TARGET_BLOCKS) {
            r->target_blocks++;
            r->extensions++;
            if (extend) *extend = true;
        }
        const uint16_t want = drift_frames_for(r, queued);
        out_frames = (uint16_t)(frames + want - NS2_COMPANION_MIC_BLOCK_FRAMES);
    } else {
        r->drift_blocks = 0u;
        r->correcting = 0;
    }

    const uint32_t bound =
        ((uint32_t)r->target_blocks + NS2_COMPANION_MIC_HEADROOM_BLOCKS) *
        NS2_COMPANION_MIC_BLOCK_FRAMES;
    if (queued + out_frames > bound ||
        queued + out_frames > NS2_COMPANION_MIC_RING_FRAMES) {
        r->blocks_dropped++;
        return false;
    }

    int16_t *const scratch = r->scratch;
    const uint16_t n = ns2_companion_mic_stretch(pcm, frames, scratch, out_frames);
    const uint32_t start = tail & RING_MASK;
    const uint32_t first = NS2_COMPANION_MIC_RING_FRAMES - start;
    if (n <= first) {
        memcpy(&r->samples[start], scratch, n * sizeof(int16_t));
    } else {
        memcpy(&r->samples[start], scratch, first * sizeof(int16_t));
        memcpy(&r->samples[0], &scratch[first], (n - first) * sizeof(int16_t));
    }
    store_release(&r->tail, tail + n);
    r->blocks_written++;
    if (concealed) r->blocks_concealed++;
    return true;
}

void ns2_companion_mic_ring_read_usb(ns2_companion_mic_ring_t *r,
                                     int16_t *stereo_out, uint16_t gain_q15)
{
    if (!stereo_out) return;
    if (!r) {
        memset(stereo_out, 0, NS2_COMPANION_MIC_USB_FRAMES * 2u * sizeof(int16_t));
        return;
    }
    if (__atomic_load_n(&r->flush_requested, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&r->flush_requested, false, __ATOMIC_RELEASE);
        store_release(&r->head, load_acquire(&r->flush_to));
        __atomic_store_n(&r->playing, false, __ATOMIC_RELEASE);
    }

    const uint32_t head = r->head;
    const uint32_t queued = load_acquire(&r->tail) - head;
    r->packets_read++;
    if (!r->playing) {
        const uint32_t start_at =
            (uint32_t)r->target_blocks * NS2_COMPANION_MIC_BLOCK_FRAMES;
        if (queued < start_at) {
            memset(stereo_out, 0,
                   NS2_COMPANION_MIC_USB_FRAMES * 2u * sizeof(int16_t));
            r->packets_silent++;
            return;
        }
        __atomic_store_n(&r->playing, true, __ATOMIC_RELEASE);
    }

    const uint32_t n = queued < NS2_COMPANION_MIC_USB_FRAMES
                           ? queued
                           : NS2_COMPANION_MIC_USB_FRAMES;
    for (uint32_t i = 0; i < n; i++) {
        const int32_t s = r->samples[(head + i) & RING_MASK];
        const int16_t v = (int16_t)((s * (int32_t)gain_q15 + 16384) >> 15);
        stereo_out[2u * i] = v;
        stereo_out[2u * i + 1u] = v;
    }
    for (uint32_t i = n; i < NS2_COMPANION_MIC_USB_FRAMES; i++) {
        stereo_out[2u * i] = 0;
        stereo_out[2u * i + 1u] = 0;
    }
    store_release(&r->head, head + n);
    if (n < NS2_COMPANION_MIC_USB_FRAMES) {
        // Ran dry: pad with silence and refill the prebuffer.
        __atomic_store_n(&r->playing, false, __ATOMIC_RELEASE);
        __atomic_fetch_add(&r->underruns, 1u, __ATOMIC_ACQ_REL);
    }
}

uint16_t ns2_companion_mic_stretch(const int16_t *in, uint16_t in_frames,
                                   int16_t *out, uint16_t out_frames)
{
    if (!in || !out || in_frames == 0u || out_frames == 0u) return 0u;
    if (out_frames == in_frames) {
        memcpy(out, in, in_frames * sizeof(int16_t));
        return in_frames;
    }
    if (out_frames == 1u) {
        out[0] = in[0];
        return 1u;
    }
    const uint32_t step_q16 =
        ((uint32_t)(in_frames - 1u) << 16) / (uint32_t)(out_frames - 1u);
    for (uint32_t i = 0; i + 1u < out_frames; i++) {
        const uint32_t position = i * step_q16;
        uint32_t left = position >> 16;
        if (left > (uint32_t)in_frames - 1u) left = (uint32_t)in_frames - 1u;
        const uint32_t right =
            left + 1u < in_frames ? left + 1u : (uint32_t)in_frames - 1u;
        const int64_t fraction = (int64_t)(position & 0xFFFFu);
        const int64_t a = in[left];
        const int64_t b = in[right];
        out[i] = (int16_t)(a + (((b - a) * fraction + 32768) >> 16));
    }
    out[out_frames - 1u] = in[in_frames - 1u];
    return out_frames;
}

uint16_t ns2_companion_mic_gain_q15(bool muted, int16_t volume_db_256)
{
    if (muted) return 0u;
    if (volume_db_256 >= 0) return 32767u;
    const double db = (double)volume_db_256 / 256.0;
    const double gain = pow(10.0, db / 20.0) * 32767.0;
    return (uint16_t)(gain + 0.5);
}
