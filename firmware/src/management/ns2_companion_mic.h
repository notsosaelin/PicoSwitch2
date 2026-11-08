// ns2_companion_mic.h - Companion microphone uplink plane (EXPERIMENTAL).
//
// WHAT THIS IS
//
// The return half of companion audio (ns2_companion_audio.h). With the
// microphone on, the companion is a headset WITH a microphone plugged into the
// controller it is: the adapter advertises the jack as HEADSET (report 0x09
// byte 0x0D = 0x07/0x0F, controller_headset.h) instead of HEADPHONES, and the
// companion's microphone is what the console reads from the USB microphone
// stream.
//
//     companion microphone (48 kHz mono, its own clock)
//         -> CELT-only Opus, CBR, 20 ms (companion)
//         -> this frame, one GATT write-without-response on the management
//            service (MIC-UP characteristic, protocol-v1.json)
//         -> core 1: sequence check, direct CELT decode, drift control
//            (ds5_audio_bridge.c, codec worker; speaker work always first)
//         -> lock-free ring (this file)
//         -> core 0: 1 ms USB isochronous IN packets, EP 0x83, 48 kHz
//            stereo int16 (the mono capture on both channels)
//
// FORMAT: 48 kHz MONO, 64 kbit/s CBR -- NOT A REDUCTION
//
// A headset microphone is one capsule, so mono carries everything it can
// capture. 64 kbit/s is above the DualSense's own microphone stream (48 kHz
// mono Opus, 71 bytes per 10 ms = 56.8 kbit/s,
// docs/switch2/audio-passthrough-research.md section 2.2) and, per channel,
// above the 96 kbit/s stereo speaker downlink (48 kbit/s a channel). CELT-only
// full band keeps the whole 20 kHz audio band; CBR makes every frame one size,
// so the frame length is checked exactly, as on the downlink.
//
// The packet is a valid public Opus packet: TOC 0xF8 is config 31 (CELT-only,
// full band, 20 ms), mono, code 0 (one frame). Any standard encoder in
// restricted-low-delay mode at 64 kbit/s CBR emits it; the firmware decodes it
// with the CELT decoder underneath libopus's public one, the same lower layer
// the speaker encoder uses, so no SILK code is linked.
//
// SCOPE AND GATING
//
// The microphone is part of a companion audio session: it is requested at
// Start (`caudio start mic`) and lives and ends with that session, because a
// headset's microphone is not separable from the headset. Changing it means a
// new session -- the companion stops and starts again, which the console sees
// as the headset being unplugged and a headset with a microphone plugged in.
// Writes are accepted only from the session's trusted management link while a
// microphone session is active, and decoded only while the console has the
// USB microphone stream open.
//
// PRIORITY: THE SPEAKER NEVER WAITS FOR THE MICROPHONE
//
// Core 1's codec worker encodes every pending speaker block before it decodes
// a microphone packet, and a full microphone packet queue drops its oldest
// packet. Under pressure the microphone loses a frame (concealed) before the
// speaker loses a moment.
#ifndef NS2_COMPANION_MIC_H
#define NS2_COMPANION_MIC_H

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// WIRE CONTRACT
// ============================================================================

#define NS2_COMPANION_MIC_VERSION 1u

// Companion -> adapter: one complete Opus packet. The downlink's opcode is
// 0x90; the planes are separate characteristics, the opcode keeps a frame
// self-describing in captures.
#define NS2_COMPANION_MIC_OP_OPUS 0x91u

// Frame:
//   [0]    version
//   [1]    opcode
//   [2..3] sequence, little-endian, increments once per frame CAPTURED, free to wrap
//   [4..]  one complete Opus packet, TOC byte first
//
// A gap in the sequence is a lost frame; the adapter conceals up to
// NS2_COMPANION_MIC_MAX_CONCEALED of them so the console's stream keeps time.
#define NS2_COMPANION_MIC_HEADER_BYTES 4u
#define NS2_COMPANION_MIC_KBPS 64u
#define NS2_COMPANION_MIC_OPUS_BYTES 160u  // 64 kbit/s x 20 ms / 8
#define NS2_COMPANION_MIC_FRAME_BYTES \
    (NS2_COMPANION_MIC_HEADER_BYTES + NS2_COMPANION_MIC_OPUS_BYTES)
#define NS2_COMPANION_MIC_TOC 0xF8u        // config 31, mono, code 0

#define NS2_COMPANION_MIC_SAMPLE_RATE 48000u
#define NS2_COMPANION_MIC_CHANNELS 1u
#define NS2_COMPANION_MIC_FRAME_MS 20u
#define NS2_COMPANION_MIC_BLOCK_FRAMES 960u

// ============================================================================
// PURE LOGIC (host-testable, no BTstack, no hardware)
// ============================================================================

typedef enum {
    NS2_COMPANION_MIC_PARSE_OK = 0,
    NS2_COMPANION_MIC_PARSE_SHORT,    // not even a header
    NS2_COMPANION_MIC_PARSE_VERSION,  // a plane version this firmware does not speak
    NS2_COMPANION_MIC_PARSE_OPCODE,
    NS2_COMPANION_MIC_PARSE_SIZE,     // not exactly one 64 kbit/s CBR packet
    NS2_COMPANION_MIC_PARSE_TOC,      // not CELT-only full-band mono 20 ms
} ns2_companion_mic_parse_t;

// Validate one MIC-UP write. On OK, *sequence and *opus (pointing into
// `frame`, NS2_COMPANION_MIC_OPUS_BYTES long, TOC first) are set. Refuses
// rather than guesses: a packet of another shape would decode as noise.
ns2_companion_mic_parse_t ns2_companion_mic_parse(const uint8_t *frame,
                                                  uint16_t len,
                                                  uint16_t *sequence,
                                                  const uint8_t **opus);

const char *ns2_companion_mic_parse_name(ns2_companion_mic_parse_t result);

// Sequence tracking. Frames lost before this one (0 when in order), or -1 for
// a duplicate or one older than the last accepted (dropped: playing it would
// put audio out of order). A jump of more than NS2_COMPANION_MIC_MAX_GAP is a
// discontinuity (the companion restarted its capture): no concealment, and
// the count restarts there.
#define NS2_COMPANION_MIC_MAX_CONCEALED 2u
#define NS2_COMPANION_MIC_MAX_GAP 50u

typedef struct {
    bool have;
    uint16_t next;
} ns2_companion_mic_sequence_t;

void ns2_companion_mic_sequence_reset(ns2_companion_mic_sequence_t *s);
int ns2_companion_mic_sequence_accept(ns2_companion_mic_sequence_t *s,
                                      uint16_t sequence);

// How many concealment blocks to decode before a packet that arrived `gap`
// frames late: the lost frames, at most NS2_COMPANION_MIC_MAX_CONCEALED, and
// none across a discontinuity.
uint8_t ns2_companion_mic_conceal_count(int gap);

// ----------------------------------------------------------------------------
// Playout ring: single producer (core 1, decoded 20 ms blocks), single
// consumer (core 0, 1 ms USB packets). Mono samples. Lock-free: the producer
// owns `tail`, the target and the drift state; the consumer owns `head`,
// `playing` and the underrun count. Each side only reads the other's fields.
//
// Policy, the companions' playout (CompanionAudioPlayout) on the adapter side:
//  - The consumer starts playing once `target_blocks` blocks are queued and
//    plays silence until then. The session starts at
//    NS2_COMPANION_MIC_BLE_TARGET_BLOCKS, or the deeper
//    NS2_COMPANION_MIC_CLASSIC_TARGET_BLOCKS when Bluetooth Classic shares
//    the radio (the Android companion's Controller Link), as on the downlink.
//  - Running dry is an underrun: the consumer pads that packet with silence
//    and re-prebuffers; the producer then deepens the target by one block (up
//    to NS2_COMPANION_MIC_MAX_TARGET_BLOCKS) for the rest of the session.
//  - A block that would leave more than NS2_COMPANION_MIC_HEADROOM_BLOCKS
//    above the target queued is dropped. After a burst (frames held up by the
//    radio arriving together) this sheds the excess at once; drift control
//    alone moves the queue 1 ms a second and would leave the microphone a
//    burst late for a minute.
//  - Drift control: the companion's capture clock and the console's USB clock
//    differ by up to ~600 ppm. Each block is written as 960 +/- 1 frames
//    (linear interpolation, inaudible) to hold the queue, measured just before
//    the write, at (target - 1) blocks -- the same law, constants and
//    always-toward-the-target rule as CompanionAudioDriftControl.
// ----------------------------------------------------------------------------

#define NS2_COMPANION_MIC_RING_FRAMES 8192u  // 170 ms; power of two
#define NS2_COMPANION_MIC_USB_FRAMES 48u     // one 1 ms USB packet
#define NS2_COMPANION_MIC_BLE_TARGET_BLOCKS 4u      // 80 ms
#define NS2_COMPANION_MIC_CLASSIC_TARGET_BLOCKS 6u  // 120 ms
#define NS2_COMPANION_MIC_MAX_TARGET_BLOCKS 6u
#define NS2_COMPANION_MIC_HEADROOM_BLOCKS 2u
#define NS2_COMPANION_MIC_MAX_BLOCK_FRAMES (NS2_COMPANION_MIC_BLOCK_FRAMES + 1u)

#define NS2_COMPANION_MIC_SETTLE_BLOCKS 50u
#define NS2_COMPANION_MIC_SMOOTHING_BLOCKS 50  // EMA weight 1/50
#define NS2_COMPANION_MIC_DEADBAND_FRAMES (4u * 48u)  // 4 ms
#define NS2_COMPANION_MIC_RELEASE_FRAMES (1u * 48u)   // 1 ms

_Static_assert((NS2_COMPANION_MIC_RING_FRAMES &
                (NS2_COMPANION_MIC_RING_FRAMES - 1u)) == 0u,
               "ring size must be a power of two");
// The deepest target, plus the block being written, plus a block of drift
// and jitter overshoot, must fit: a write that does not fit is dropped.
_Static_assert((NS2_COMPANION_MIC_MAX_TARGET_BLOCKS +
                NS2_COMPANION_MIC_HEADROOM_BLOCKS) *
                   NS2_COMPANION_MIC_MAX_BLOCK_FRAMES <=
               NS2_COMPANION_MIC_RING_FRAMES,
               "ring too small for the deepest target");

typedef struct {
    int16_t samples[NS2_COMPANION_MIC_RING_FRAMES];
    // Shared indices, free-running (masked on access).
    volatile uint32_t head;          // consumer
    volatile uint32_t tail;          // producer
    volatile uint8_t target_blocks;  // producer
    volatile bool playing;           // consumer
    volatile bool flush_requested;   // producer asks, consumer honours
    volatile uint32_t flush_to;      // producer: where the consumer skips to
    volatile uint32_t underruns;     // consumer
    // Producer-only.
    uint32_t underruns_seen;
    uint32_t drift_blocks;
    int32_t smoothed_q8;             // queue depth in frames, Q8
    int8_t correcting;
    int32_t frames_adjusted;
    uint32_t blocks_written;
    uint32_t blocks_dropped;         // beyond the headroom, or did not fit
    uint32_t blocks_concealed;       // PLC blocks written
    uint32_t extensions;             // near-underrun growths
    int16_t scratch[NS2_COMPANION_MIC_MAX_BLOCK_FRAMES];
    // Consumer-only.
    uint32_t packets_read;
    uint32_t packets_silent;
} ns2_companion_mic_ring_t;

// Producer: start a session at `initial_target_blocks` (clamped to the
// ceiling), dropping anything still queued.
void ns2_companion_mic_ring_reset(ns2_companion_mic_ring_t *r,
                                  uint8_t initial_target_blocks);

// Producer: one decoded 20 ms block (`frames` mono samples, normally 960).
// `concealed` marks a packet-loss-concealment block, for the counters.
// Returns true when written. Sets *extend when the queue had nearly run dry
// and the target grew: the caller then writes one concealment block more,
// which is the deeper queue the drift control holds from now on.
bool ns2_companion_mic_ring_write(ns2_companion_mic_ring_t *r,
                                  const int16_t *pcm, uint16_t frames,
                                  bool concealed, bool *extend);

// Consumer: fill one USB packet, NS2_COMPANION_MIC_USB_FRAMES stereo frames
// (both channels the mono sample, scaled by gain_q15; 0 = mute). Always fills
// the whole packet: silence while prebuffering or after running dry.
void ns2_companion_mic_ring_read_usb(ns2_companion_mic_ring_t *r,
                                     int16_t *stereo_out, uint16_t gain_q15);

// Frames queued (written, not yet read). Either side.
uint32_t ns2_companion_mic_ring_queued(const ns2_companion_mic_ring_t *r);

// Linear-interpolation stretch of a mono block to exactly `out_frames`, the
// first and last samples kept: CompanionAudioDriftControl.Stretch for one
// channel, in fixed point. Returns frames written.
uint16_t ns2_companion_mic_stretch(const int16_t *in, uint16_t in_frames,
                                   int16_t *out, uint16_t out_frames);

// UAC1 Feature Unit volume (signed 1/256 dB, -60..0 dB here) and mute as a
// Q15 linear gain (32767 = 0 dB, 0 = muted).
uint16_t ns2_companion_mic_gain_q15(bool muted, int16_t volume_db_256);

#endif  // NS2_COMPANION_MIC_H
