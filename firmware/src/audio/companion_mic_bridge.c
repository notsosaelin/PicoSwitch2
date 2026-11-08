// companion_mic_bridge.c - Companion microphone: MIC-UP packets to USB mic PCM.
// See companion_mic_bridge.h and management/ns2_companion_mic.h.
#include "companion_mic_bridge.h"

#include <string.h>

#include "ns2_companion_mic.h"

static volatile uint16_t mic_gain_q15 = 32767u;

void companion_mic_bridge_set_control(bool muted, int16_t volume_db_256)
{
    mic_gain_q15 = ns2_companion_mic_gain_q15(muted, volume_db_256);
}

#if defined(NS2_DS5_AUDIO_LIVE_OPUS)

#include <malloc.h>

#include "companion_mic_decode.h"
#include "ds5_audio_bridge.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "pico/util/queue.h"

// Depth of the packet queue between the write handler and the worker: 80 ms.
// The worker normally empties it within one speaker block (10.7 ms); a full
// queue drops its OLDEST packet, the gap is then concealed, and the speaker
// never waits (ns2_companion_mic.h, PRIORITY).
#define MIC_PACKET_QUEUE_DEPTH 4u

// The mono decoder state, statically: 9,548 bytes for libopus 1.5's CELT
// decoder at 48 kHz mono on Cortex-M33 (struct 112 B + (2048 + 120 - 1)
// samples of history + band energies + LPC, celt_decoder.c). Static so the
// cost is visible at link time and the decoder never competes with the
// speaker encoder's heap allocation, which is freed and re-made when the
// controller changes. Checked against the codec's own size at init.
#define MIC_DECODER_BYTES 10240u

typedef struct {
    uint16_t sequence;
    uint8_t opus[NS2_COMPANION_MIC_OPUS_BYTES];
} mic_packet_t;

static queue_t mic_packet_queue;
static mic_packet_t mic_codec_packet;
static mic_packet_t mic_discard_packet;
static ns2_companion_mic_ring_t mic_ring;
static ns2_companion_mic_sequence_t mic_sequence;
static uint32_t mic_decoder_storage[MIC_DECODER_BYTES / sizeof(uint32_t)];
static void *mic_decoder;
static int16_t mic_pcm[NS2_COMPANION_MIC_BLOCK_FRAMES];
static bool mic_supported;

static volatile bool mic_active;
static volatile bool mic_usb_active;
// Bumped whenever the playout must start over: a new session, or the console
// reopening its microphone stream. Applied by the worker, which owns the
// decoder, the sequence and the producer side of the ring.
static volatile uint32_t mic_restart_generation;
static uint32_t mic_applied_generation;
static bool mic_applied_active;

static volatile uint32_t diag_received;
static volatile uint32_t diag_inactive;
static volatile uint32_t diag_queue_dropped;
static uint32_t diag_stale;
static uint32_t diag_lost;
static uint32_t diag_discontinuities;
static uint32_t diag_decoded;
static uint32_t diag_decode_errors;
static uint32_t diag_decode_max_us;

void companion_mic_bridge_init(void)
{
    queue_init(&mic_packet_queue, sizeof(mic_packet_t), MIC_PACKET_QUEUE_DEPTH);
    memset(&mic_ring, 0, sizeof(mic_ring));
    ns2_companion_mic_ring_reset(&mic_ring, NS2_COMPANION_MIC_BLE_TARGET_BLOCKS);
    ns2_companion_mic_sequence_reset(&mic_sequence);
    mic_active = false;
    mic_usb_active = false;
    mic_restart_generation = 0u;
    mic_applied_generation = 0u;
    mic_applied_active = false;
    mic_decoder = NULL;
    size_t const size = companion_mic_decoder_size();
    mic_supported = size > 0u && size <= sizeof(mic_decoder_storage);
}

bool companion_mic_bridge_supported(void) { return mic_supported; }

void companion_mic_bridge_set_active(bool active)
{
    if (active && !mic_supported) return;
    if (mic_active == active) return;
    if (active) __atomic_fetch_add(&mic_restart_generation, 1u, __ATOMIC_ACQ_REL);
    mic_active = active;
    __sev();  // the worker may be asleep in __wfe()
}

bool companion_mic_bridge_active(void) { return mic_active; }

void companion_mic_bridge_set_usb_active(bool active)
{
    if (mic_usb_active == active) return;
    if (active) __atomic_fetch_add(&mic_restart_generation, 1u, __ATOMIC_ACQ_REL);
    mic_usb_active = active;
    __sev();
}

bool companion_mic_bridge_usb_active(void) { return mic_usb_active; }

void companion_mic_bridge_submit(uint16_t sequence, const uint8_t *opus)
{
    if (!opus) return;
    if (!mic_active || !mic_usb_active) {
        // No session, or the console has not opened (or closed) its
        // microphone stream: nothing would read the audio.
        diag_inactive++;
        return;
    }
    mic_packet_t packet;
    packet.sequence = sequence;
    memcpy(packet.opus, opus, sizeof(packet.opus));
    if (!queue_try_add(&mic_packet_queue, &packet)) {
        // The worker is behind: the oldest packet gives way. Its gap is
        // concealed when the next packet is decoded.
        (void)queue_try_remove(&mic_packet_queue, &mic_discard_packet);
        diag_queue_dropped++;
        (void)queue_try_add(&mic_packet_queue, &packet);
    }
    diag_received++;
}

static bool mic_decoder_reset(void)
{
    mic_decoder = companion_mic_decoder_init(mic_decoder_storage,
                                             sizeof(mic_decoder_storage));
    return mic_decoder != NULL;
}

// One 20 ms block: the packet, or concealment when NULL.
static bool mic_decode_block(const uint8_t *opus, bool concealed, bool *extend)
{
    uint32_t const start_us = time_us_32();
    bool const decoded = companion_mic_decode(mic_decoder, opus, mic_pcm);
    uint32_t const elapsed_us = time_us_32() - start_us;
    if (elapsed_us > diag_decode_max_us) diag_decode_max_us = elapsed_us;
    if (!decoded) {
        diag_decode_errors++;
        return false;
    }
    if (!concealed) diag_decoded++;
    return ns2_companion_mic_ring_write(&mic_ring, mic_pcm,
                                        (uint16_t)NS2_COMPANION_MIC_BLOCK_FRAMES,
                                        concealed, extend);
}

static void mic_drain_queue(void)
{
    while (queue_try_remove(&mic_packet_queue, &mic_discard_packet)) {
    }
}

bool __not_in_flash_func(companion_mic_bridge_service)(void)
{
    if (!mic_supported) return false;
    bool const active = mic_active && mic_usb_active;
    uint32_t const generation =
        __atomic_load_n(&mic_restart_generation, __ATOMIC_ACQUIRE);
    if (generation != mic_applied_generation || active != mic_applied_active) {
        mic_applied_generation = generation;
        mic_applied_active = active;
        mic_drain_queue();
        ns2_companion_mic_sequence_reset(&mic_sequence);
        // The Android companion's Controller Link is Bluetooth Classic on this
        // radio: start deeper, as the downlink does (ns2_companion_mic.h).
        ns2_companion_mic_ring_reset(
            &mic_ring, ds5_audio_bridge_classic_link_present()
                           ? NS2_COMPANION_MIC_CLASSIC_TARGET_BLOCKS
                           : NS2_COMPANION_MIC_BLE_TARGET_BLOCKS);
        if (active) (void)mic_decoder_reset();
    }
    if (!active) {
        mic_drain_queue();
        return false;
    }
    if (!queue_try_remove(&mic_packet_queue, &mic_codec_packet)) return false;
    if (!mic_decoder && !mic_decoder_reset()) {
        diag_decode_errors++;
        return true;
    }

    int const gap = ns2_companion_mic_sequence_accept(&mic_sequence,
                                                      mic_codec_packet.sequence);
    if (gap < 0) {
        diag_stale++;
        return true;
    }
    if (gap > (int)NS2_COMPANION_MIC_MAX_GAP) {
        diag_discontinuities++;
    } else {
        diag_lost += (uint32_t)gap;
    }
    bool extend = false;
    for (uint8_t c = ns2_companion_mic_conceal_count(gap); c > 0u; c--)
        (void)mic_decode_block(NULL, true, &extend);
    (void)mic_decode_block(mic_codec_packet.opus, false, &extend);
    if (extend) (void)mic_decode_block(NULL, true, NULL);
    return true;
}

void __not_in_flash_func(companion_mic_bridge_read_usb)(
    uint8_t packet[COMPANION_MIC_BRIDGE_USB_PACKET_BYTES])
{
    // The endpoint buffer is word-aligned (CFG_TUSB_MEM_ALIGN) and the USB
    // stream is little-endian int16, as is the core.
    ns2_companion_mic_ring_read_usb(&mic_ring, (int16_t *)(void *)packet,
                                    mic_gain_q15);
}

void companion_mic_bridge_diag(companion_mic_bridge_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->supported = mic_supported;
    out->active = mic_active;
    out->usb_active = mic_usb_active;
    out->playing = mic_ring.playing;
    out->target_blocks = mic_ring.target_blocks;
    out->gain_q15 = mic_gain_q15;
    out->queued_frames = ns2_companion_mic_ring_queued(&mic_ring);
    out->received = diag_received;
    out->inactive = diag_inactive;
    out->queue_dropped = diag_queue_dropped;
    out->stale = diag_stale;
    out->lost = diag_lost;
    out->discontinuities = diag_discontinuities;
    out->decoded = diag_decoded;
    out->concealed = mic_ring.blocks_concealed;
    out->decode_errors = diag_decode_errors;
    out->decode_max_us = diag_decode_max_us;
    out->underruns = mic_ring.underruns;
    out->ring_dropped = mic_ring.blocks_dropped;
    out->extensions = mic_ring.extensions;
    out->frames_adjusted = mic_ring.frames_adjusted;
    // Heap still available: what sbrk has not handed out plus what malloc
    // holds free. The microphone's buffers are static; this shows the margin
    // left for the speaker encoder's allocation (ns2_companion_mic.h).
    extern char __end__;
    extern char __HeapLimit;
    struct mallinfo const info = mallinfo();
    uint32_t const heap_total = (uint32_t)(&__HeapLimit - &__end__);
    uint32_t const arena = (uint32_t)info.arena;
    out->heap_free = (heap_total > arena ? heap_total - arena : 0u) +
                     (uint32_t)info.fordblks;
}

#else  // !NS2_DS5_AUDIO_LIVE_OPUS

static volatile uint32_t diag_inactive;

void companion_mic_bridge_init(void) {}
bool companion_mic_bridge_supported(void) { return false; }
void companion_mic_bridge_set_active(bool active) { (void)active; }
bool companion_mic_bridge_active(void) { return false; }
void companion_mic_bridge_set_usb_active(bool active) { (void)active; }
bool companion_mic_bridge_usb_active(void) { return false; }
void companion_mic_bridge_submit(uint16_t sequence, const uint8_t *opus)
{
    (void)sequence;
    (void)opus;
    diag_inactive++;
}
bool companion_mic_bridge_service(void) { return false; }
void companion_mic_bridge_read_usb(uint8_t packet[COMPANION_MIC_BRIDGE_USB_PACKET_BYTES])
{
    memset(packet, 0, COMPANION_MIC_BRIDGE_USB_PACKET_BYTES);
}
void companion_mic_bridge_diag(companion_mic_bridge_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->gain_q15 = mic_gain_q15;
    out->inactive = diag_inactive;
}

#endif
