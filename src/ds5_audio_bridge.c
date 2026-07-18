#include "ds5_audio_bridge.h"
#include "ds5_audio_resample.h"

#include <string.h>

static volatile bool speaker_control_muted;
static volatile uint8_t speaker_control_volume = 100;

void ds5_audio_bridge_set_speaker_control(bool muted, int16_t volume_db_256) {
    int32_t volume = 100 + volume_db_256 / 256;
    if (volume < 40) volume = 40;
    if (volume > 100) volume = 100;
    speaker_control_volume = (uint8_t)volume;
    speaker_control_muted = muted;
}

void ds5_audio_bridge_get_speaker_control(bool *muted, uint8_t *volume) {
    if (muted) *muted = speaker_control_muted;
    if (volume) *volume = speaker_control_volume;
}

// Always compiled so config-mode diagnostics remain linkable in ordinary
// builds. Only RP2350 audio builds feed these counters. Aligned uint32_t loads
// and stores are atomic on RP2040/RP2350; an occasionally mixed snapshot while
// core 1 updates is acceptable for monotonic diagnostic counters.
static volatile uint32_t diag_core1_max_gap_us;
static volatile uint32_t diag_core1_gaps_over_10ms;
static volatile uint32_t diag_send_max_gap_us;
static volatile uint32_t diag_send_gaps_over_40ms;
static volatile uint32_t diag_sends_total;
static volatile uint32_t diag_hci_complete_max_gap_us;
static volatile uint32_t diag_hci_complete_gaps_over_40ms;
static volatile uint32_t diag_hci_complete_events;
static volatile uint32_t diag_hci_completed_packets;
static volatile uint32_t diag_hci_complete_max_batch;
static volatile uint32_t diag_pcm_packets_total;
static volatile uint32_t diag_pcm_nonzero_packets;
static volatile uint32_t diag_pcm_short_packets;
static volatile uint32_t diag_pcm_dropped_packets;
static volatile uint32_t diag_pcm_max_gap_us;
static volatile uint32_t diag_pcm_gaps_over_2ms;
static volatile uint32_t diag_pcm_queue_max_depth;
static volatile uint32_t diag_opus_frames_total;
static volatile uint32_t diag_opus_encode_errors;
static volatile uint32_t diag_opus_max_gap_us;
static volatile uint32_t diag_opus_gaps_over_20ms;
static volatile uint32_t diag_opus_encode_max_us;
static volatile uint32_t diag_pipeline_resets;
static volatile uint32_t diag_codec_calls_total;
static volatile uint32_t diag_codec_no_encoder;
static volatile uint32_t diag_codec_disconnected;
static volatile uint32_t diag_codec_usb_inactive;
static volatile uint32_t diag_codec_no_pcm;
static volatile uint32_t diag_codec_blocks_dequeued;
static volatile uint32_t diag_codec_call_max_gap_us;
static volatile uint32_t diag_codec_call_gaps_over_10ms;
static volatile uint32_t diag_codec_gap_le_3ms;
static volatile uint32_t diag_codec_gap_le_7ms;
static volatile uint32_t diag_codec_gap_le_12ms;
static volatile uint32_t diag_codec_gap_le_25ms;
static volatile uint32_t diag_codec_gap_over_25ms;
static volatile uint32_t diag_usb_speaker_on_edges;
static volatile uint32_t diag_usb_speaker_off_edges;
static volatile uint32_t diag_usb_speaker_active_us;
static volatile bool diag_usb_speaker_active;
static volatile bool diag_audio_started;
static uint32_t diag_core1_last_us;
static uint32_t diag_send_last_us;
static uint32_t diag_hci_complete_last_us;
static uint32_t diag_pcm_last_us;
static uint32_t diag_opus_last_us;
static uint32_t diag_codec_call_last_us;
static uint32_t diag_usb_speaker_last_edge_us;

void ds5_audio_diag_note_core1_activity(uint32_t now_us) {
    // Pairing, controller activation, and USB personality changes can contain
    // long but audio-irrelevant pauses. Keep the heartbeat current before the
    // first stream packet, but only score gaps once audio has actually begun.
    if (diag_audio_started && diag_core1_last_us) {
        uint32_t const gap = now_us - diag_core1_last_us;
        if (gap > diag_core1_max_gap_us) diag_core1_max_gap_us = gap;
        if (gap > 10000u) diag_core1_gaps_over_10ms++;
    }
    diag_core1_last_us = now_us;
}

void ds5_audio_diag_note_l2cap_send(uint32_t now_us) {
    diag_audio_started = true;
    if (diag_send_last_us) {
        uint32_t const gap = now_us - diag_send_last_us;
        if (gap > diag_send_max_gap_us) diag_send_max_gap_us = gap;
        if (gap > 40000u) diag_send_gaps_over_40ms++;
    }
    diag_send_last_us = now_us;
    diag_sends_total++;
}

void ds5_audio_diag_note_hci_completion(uint32_t now_us,
                                        uint16_t completed_packets) {
    if (!diag_audio_started || completed_packets == 0) return;
    if (diag_hci_complete_last_us) {
        uint32_t const gap = now_us - diag_hci_complete_last_us;
        if (gap > diag_hci_complete_max_gap_us)
            diag_hci_complete_max_gap_us = gap;
        if (gap > 40000u) diag_hci_complete_gaps_over_40ms++;
    }
    diag_hci_complete_last_us = now_us;
    diag_hci_complete_events++;
    diag_hci_completed_packets += completed_packets;
    if (completed_packets > diag_hci_complete_max_batch)
        diag_hci_complete_max_batch = completed_packets;
}

void ds5_audio_diag_note_usb_pcm(uint32_t now_us, bool nonzero,
                                 bool short_packet, bool dropped,
                                 uint32_t queue_depth) {
    if (diag_pcm_last_us) {
        uint32_t const gap = now_us - diag_pcm_last_us;
        if (gap > diag_pcm_max_gap_us) diag_pcm_max_gap_us = gap;
        if (gap > 2000u) diag_pcm_gaps_over_2ms++;
    }
    diag_pcm_last_us = now_us;
    diag_pcm_packets_total++;
    if (nonzero) diag_pcm_nonzero_packets++;
    if (short_packet) diag_pcm_short_packets++;
    if (dropped) diag_pcm_dropped_packets++;
    if (queue_depth > diag_pcm_queue_max_depth)
        diag_pcm_queue_max_depth = queue_depth;
}

void ds5_audio_diag_note_opus_frame(uint32_t now_us, uint32_t encode_us,
                                    bool success) {
    if (diag_opus_last_us) {
        uint32_t const gap = now_us - diag_opus_last_us;
        if (gap > diag_opus_max_gap_us) diag_opus_max_gap_us = gap;
        if (gap > 20000u) diag_opus_gaps_over_20ms++;
    }
    diag_opus_last_us = now_us;
    if (success) diag_opus_frames_total++;
    else diag_opus_encode_errors++;
    if (encode_us > diag_opus_encode_max_us)
        diag_opus_encode_max_us = encode_us;
}

void ds5_audio_diag_note_pipeline_reset(void) {
    diag_pipeline_resets++;
}

static void ds5_audio_diag_note_codec_call(uint32_t now_us) {
    diag_codec_calls_total++;
    if (diag_codec_call_last_us) {
        uint32_t const gap = now_us - diag_codec_call_last_us;
        if (gap > diag_codec_call_max_gap_us)
            diag_codec_call_max_gap_us = gap;
        if (gap > 10000u) diag_codec_call_gaps_over_10ms++;
        if (gap <= 3000u) diag_codec_gap_le_3ms++;
        else if (gap <= 7000u) diag_codec_gap_le_7ms++;
        else if (gap <= 12000u) diag_codec_gap_le_12ms++;
        else if (gap <= 25000u) diag_codec_gap_le_25ms++;
        else diag_codec_gap_over_25ms++;
    }
    diag_codec_call_last_us = now_us;
}

static void ds5_audio_diag_note_usb_speaker(bool active, uint32_t now_us) {
    if (active == diag_usb_speaker_active) return;
    if (diag_usb_speaker_active && diag_usb_speaker_last_edge_us)
        diag_usb_speaker_active_us += now_us - diag_usb_speaker_last_edge_us;
    diag_usb_speaker_active = active;
    diag_usb_speaker_last_edge_us = now_us;
    if (active) diag_usb_speaker_on_edges++;
    else diag_usb_speaker_off_edges++;
}

void ds5_audio_diag_get(ds5_audio_diag_t *out) {
    if (!out) return;
    out->core1_max_gap_us = diag_core1_max_gap_us;
    out->core1_gaps_over_10ms = diag_core1_gaps_over_10ms;
    out->send_max_gap_us = diag_send_max_gap_us;
    out->send_gaps_over_40ms = diag_send_gaps_over_40ms;
    out->sends_total = diag_sends_total;
    out->hci_complete_max_gap_us = diag_hci_complete_max_gap_us;
    out->hci_complete_gaps_over_40ms = diag_hci_complete_gaps_over_40ms;
    out->hci_complete_events = diag_hci_complete_events;
    out->hci_completed_packets = diag_hci_completed_packets;
    out->hci_complete_max_batch = diag_hci_complete_max_batch;
    out->pcm_packets_total = diag_pcm_packets_total;
    out->pcm_nonzero_packets = diag_pcm_nonzero_packets;
    out->pcm_short_packets = diag_pcm_short_packets;
    out->pcm_dropped_packets = diag_pcm_dropped_packets;
    out->pcm_max_gap_us = diag_pcm_max_gap_us;
    out->pcm_gaps_over_2ms = diag_pcm_gaps_over_2ms;
    out->pcm_queue_max_depth = diag_pcm_queue_max_depth;
    out->opus_frames_total = diag_opus_frames_total;
    out->opus_encode_errors = diag_opus_encode_errors;
    out->opus_max_gap_us = diag_opus_max_gap_us;
    out->opus_gaps_over_20ms = diag_opus_gaps_over_20ms;
    out->opus_encode_max_us = diag_opus_encode_max_us;
    out->pipeline_resets = diag_pipeline_resets;
    out->codec_calls_total = diag_codec_calls_total;
    out->codec_no_encoder = diag_codec_no_encoder;
    out->codec_disconnected = diag_codec_disconnected;
    out->codec_usb_inactive = diag_codec_usb_inactive;
    out->codec_no_pcm = diag_codec_no_pcm;
    out->codec_blocks_dequeued = diag_codec_blocks_dequeued;
    out->codec_call_max_gap_us = diag_codec_call_max_gap_us;
    out->codec_call_gaps_over_10ms = diag_codec_call_gaps_over_10ms;
    out->codec_gap_le_3ms = diag_codec_gap_le_3ms;
    out->codec_gap_le_7ms = diag_codec_gap_le_7ms;
    out->codec_gap_le_12ms = diag_codec_gap_le_12ms;
    out->codec_gap_le_25ms = diag_codec_gap_le_25ms;
    out->codec_gap_over_25ms = diag_codec_gap_over_25ms;
    out->usb_speaker_on_edges = diag_usb_speaker_on_edges;
    out->usb_speaker_off_edges = diag_usb_speaker_off_edges;
    out->usb_speaker_active_us = diag_usb_speaker_active_us;
    out->usb_speaker_active = diag_usb_speaker_active;
}

void ds5_audio_diag_reset(void) {
    diag_core1_max_gap_us = 0;
    diag_core1_gaps_over_10ms = 0;
    diag_send_max_gap_us = 0;
    diag_send_gaps_over_40ms = 0;
    diag_sends_total = 0;
    diag_hci_complete_max_gap_us = 0;
    diag_hci_complete_gaps_over_40ms = 0;
    diag_hci_complete_events = 0;
    diag_hci_completed_packets = 0;
    diag_hci_complete_max_batch = 0;
    diag_pcm_packets_total = 0;
    diag_pcm_nonzero_packets = 0;
    diag_pcm_short_packets = 0;
    diag_pcm_dropped_packets = 0;
    diag_pcm_max_gap_us = 0;
    diag_pcm_gaps_over_2ms = 0;
    diag_pcm_queue_max_depth = 0;
    diag_opus_frames_total = 0;
    diag_opus_encode_errors = 0;
    diag_opus_max_gap_us = 0;
    diag_opus_gaps_over_20ms = 0;
    diag_opus_encode_max_us = 0;
    diag_pipeline_resets = 0;
    diag_codec_calls_total = 0;
    diag_codec_no_encoder = 0;
    diag_codec_disconnected = 0;
    diag_codec_usb_inactive = 0;
    diag_codec_no_pcm = 0;
    diag_codec_blocks_dequeued = 0;
    diag_codec_call_max_gap_us = 0;
    diag_codec_call_gaps_over_10ms = 0;
    diag_codec_gap_le_3ms = 0;
    diag_codec_gap_le_7ms = 0;
    diag_codec_gap_le_12ms = 0;
    diag_codec_gap_le_25ms = 0;
    diag_codec_gap_over_25ms = 0;
    diag_usb_speaker_on_edges = 0;
    diag_usb_speaker_off_edges = 0;
    diag_usb_speaker_active_us = 0;
    diag_usb_speaker_active = false;
    diag_audio_started = false;
    diag_core1_last_us = 0;
    diag_send_last_us = 0;
    diag_hci_complete_last_us = 0;
    diag_pcm_last_us = 0;
    diag_opus_last_us = 0;
    diag_codec_call_last_us = 0;
    diag_usb_speaker_last_edge_us = 0;
}

#if defined(NS2_DS5_AUDIO_TEST_TONE)

#include "ds5_audio_test_tone.h"
#include "pico/time.h"

// DS5Dongle accumulates 512 real 48 kHz samples, resamples them to one nominal
// 480-sample Opus frame, and sends two frames per report. daidr's independent
// Bluetooth implementation expresses the same controller clock directly as
// 480 / 45000 seconds per frame. One report pair is therefore exactly
// 2 * 480 / 45000 = 64/3 ms, not 20 ms. Alternate 21333/21333/21334 us so the
// integer-microsecond scheduler has zero long-term cadence error.
#define TEST_TONE_PAIR_INTERVAL_BASE_US 21333u
#define TEST_TONE_PAIR_INTERVAL_MAX_US  21334u
#define TEST_TONE_PREFILL_PAIRS 1u

static volatile bool usb_mic_active;
static volatile bool bridge_connected;
static volatile uint8_t bridge_conn_index;
static uint8_t test_tone_pairs_ready;
static uint32_t test_tone_pair_deadline_us;
static uint8_t test_tone_interval_phase;

static uint32_t test_tone_next_interval_us(void) {
    test_tone_interval_phase++;
    if (test_tone_interval_phase == 3u) {
        test_tone_interval_phase = 0;
        return TEST_TONE_PAIR_INTERVAL_MAX_US;
    }
    return TEST_TONE_PAIR_INTERVAL_BASE_US;
}

void ds5_audio_bridge_init(void) {
    ds5_audio_bridge_set_speaker_control(false, 0);
    usb_mic_active = false;
    bridge_connected = false;
    bridge_conn_index = 0xFF;
    test_tone_pairs_ready = 0;
    test_tone_pair_deadline_us = 0;
    test_tone_interval_phase = 0;
}

void ds5_audio_bridge_set_usb_streams(bool speaker_active, bool mic_active) {
    // This diagnostic deliberately runs independently of Windows's streaming
    // alternate setting. Shared-mode Windows can idle alt 1 when no client is
    // playing, which otherwise turns the injected tone into misleading bursts.
    (void)speaker_active;
    usb_mic_active = mic_active;
}

void ds5_audio_bridge_submit_speaker_pcm(const uint8_t *data, uint16_t len) {
    (void)data;
    (void)len;
}

void ds5_audio_bridge_connect(uint8_t conn_index) {
    bridge_conn_index = conn_index;
    bridge_connected = true;
    test_tone_pairs_ready = TEST_TONE_PREFILL_PAIRS;
    test_tone_pair_deadline_us = 0;
    test_tone_interval_phase = 0;
}

void ds5_audio_bridge_disconnect(uint8_t conn_index) {
    if (bridge_connected && bridge_conn_index == conn_index) {
        bridge_connected = false;
        bridge_conn_index = 0xFF;
        test_tone_pairs_ready = 0;
        test_tone_pair_deadline_us = 0;
        test_tone_interval_phase = 0;
    }
}

bool ds5_audio_bridge_owns_connection(uint8_t conn_index) {
    return bridge_connected && bridge_conn_index == conn_index;
}

void ds5_audio_bridge_codec_task(void) {
    if (!bridge_connected) return;

    uint32_t const now_us = time_us_32();
    if (test_tone_pair_deadline_us == 0) {
        test_tone_pair_deadline_us = now_us + test_tone_next_interval_us();
    } else if ((int32_t)(now_us - test_tone_pair_deadline_us) >= 0) {
        uint32_t const late_us = now_us - test_tone_pair_deadline_us;
        uint32_t const next_interval_us = test_tone_next_interval_us();
        // Advance from the prior deadline so task jitter cannot accumulate.
        // If Bluetooth was stalled for more than one whole frame pair, restart
        // from now instead of trying to catch up with a burst of stale audio.
        test_tone_pair_deadline_us =
            late_us >= TEST_TONE_PAIR_INTERVAL_MAX_US
                ? now_us + next_interval_us
                : test_tone_pair_deadline_us + next_interval_us;
        if (test_tone_pairs_ready < TEST_TONE_PREFILL_PAIRS)
            test_tone_pairs_ready++;
    }
}

bool ds5_audio_bridge_peek_speaker_pair(
    uint8_t frame_a[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN],
    uint8_t frame_b[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN]) {
    if (test_tone_pairs_ready == 0) return false;
    memcpy(frame_a, ds5_audio_test_tone_frames[0],
           DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    memcpy(frame_b, ds5_audio_test_tone_frames[1],
           DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    return true;
}

void ds5_audio_bridge_commit_speaker_pair(void) {
    if (test_tone_pairs_ready != 0) test_tone_pairs_ready--;
}

bool ds5_audio_bridge_mic_active(void) {
    // USB microphone return is still a silence stub. Do not ask the controller
    // to spend Bluetooth bandwidth sending Opus microphone frames that this
    // milestone deliberately drops.
    (void)usb_mic_active;
    return false;
}

#elif defined(NS2_DS5_AUDIO_LIVE_OPUS)

#include "opus.h"
#include "pico/time.h"
#include "pico/util/queue.h"

#define PCM_USB_PACKET_BYTES 192u
#define PCM_BLOCK_QUEUE_DEPTH 2u
#define ENCODED_FRAME_QUEUE_DEPTH 2u
#define PCM_INPUT_FRAMES DS5_AUDIO_RESAMPLE_INPUT_FRAMES
#define PCM_OUTPUT_FRAMES DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES
#define PCM_CHANNELS DS5_AUDIO_RESAMPLE_CHANNELS
#define PCM_BLOCK_SAMPLES (PCM_INPUT_FRAMES * PCM_CHANNELS)

typedef struct {
    int16_t samples[PCM_BLOCK_SAMPLES];
} pcm_block_t;

typedef struct {
    uint32_t generation;
    uint8_t data[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN];
} encoded_frame_t;

static queue_t speaker_pcm_block_queue;
static queue_t speaker_encoded_frame_queue;
static volatile bool usb_speaker_active;
static volatile bool usb_mic_active;
static volatile bool bridge_connected;
static volatile uint8_t bridge_conn_index;

// TinyUSB/core0 owns this accumulator. This is deliberately the same
// producer boundary as DS5Dongle: cross to the codec core only after a full
// 512-frame source window exists, rather than queueing individual 1 ms USB
// packets and asking core1 to assemble them under Bluetooth load.
static pcm_block_t producer_pcm_block;
static pcm_block_t producer_discard_block;
static uint16_t producer_samples;

static OpusEncoder *speaker_encoder;
static bool speaker_encoder_init_attempted;
static pcm_block_t codec_pcm_block;
static int16_t resampled_pcm[PCM_OUTPUT_FRAMES * PCM_CHANNELS];
static encoded_frame_t codec_encoded_frame;
static encoded_frame_t codec_discard_frame;
static encoded_frame_t transport_pair[2];
static uint8_t transport_pair_count;
static volatile uint32_t pipeline_reset_generation;
static uint32_t codec_reset_generation;

static void __not_in_flash_func(bridge_encode_input_frame)(
    const pcm_block_t *block, uint32_t generation) {
    ds5_audio_resample_512_to_480_stereo(block->samples, resampled_pcm);
    uint32_t const encode_start_us = time_us_32();
    int const encoded = opus_encode(
        speaker_encoder, resampled_pcm, PCM_OUTPUT_FRAMES,
        codec_encoded_frame.data, DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    uint32_t const encode_end_us = time_us_32();
    bool const success = encoded == DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN;
    ds5_audio_diag_note_opus_frame(encode_end_us,
                                   encode_end_us - encode_start_us, success);
    if (!success || generation != pipeline_reset_generation) return;

    // Core1 produces complete Opus frames; core0/BTstack consumes them in
    // pairs. Match DS5Dongle's depth-two encoded FIFO and favor fresh audio if
    // transport is temporarily backpressured.
    codec_encoded_frame.generation = generation;
    if (queue_is_full(&speaker_encoded_frame_queue)) {
        queue_try_remove(&speaker_encoded_frame_queue, &codec_discard_frame);
    }
    queue_try_add(&speaker_encoded_frame_queue, &codec_encoded_frame);
}

static void bridge_clear_pipeline(void) {
    // Invalidate an encode already in progress before draining. Encoded frames
    // carry this generation too, so core0 can reject any stale frame that
    // crosses the queue during the reset race.
    pipeline_reset_generation++;
    producer_samples = 0;
    while (queue_try_remove(&speaker_pcm_block_queue,
                            &producer_discard_block)) {}
    while (queue_try_remove(&speaker_encoded_frame_queue,
                            &transport_pair[0])) {}
    transport_pair_count = 0;
    ds5_audio_diag_note_pipeline_reset();
}

static bool __not_in_flash_func(bridge_prepare_encoder)(void) {
    if (!speaker_encoder && !speaker_encoder_init_attempted) {
        speaker_encoder_init_attempted = true;
        int error = OPUS_OK;
        // daidr's independently working DualSense path explicitly requires
        // low-delay Opus (pure CELT). It produces the same 200-byte/10-ms
        // payload accepted by the controller while avoiding general audio
        // mode work on the Bluetooth-shared codec core.
        speaker_encoder =
            opus_encoder_create(48000, PCM_CHANNELS,
                                OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
        if (!speaker_encoder || error != OPUS_OK) {
            speaker_encoder = NULL;
            return false;
        }
        opus_encoder_ctl(speaker_encoder,
                         OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
        opus_encoder_ctl(
            speaker_encoder,
            OPUS_SET_BITRATE(DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN * 8 * 100));
        opus_encoder_ctl(speaker_encoder, OPUS_SET_VBR(0));
        opus_encoder_ctl(speaker_encoder, OPUS_SET_COMPLEXITY(0));
    }
    return speaker_encoder != NULL;
}

static uint32_t __not_in_flash_func(bridge_sync_encoder_generation)(void) {
    uint32_t const requested_generation = pipeline_reset_generation;
    if (codec_reset_generation != requested_generation) {
        if (speaker_encoder)
            opus_encoder_ctl(speaker_encoder, OPUS_RESET_STATE);
        codec_reset_generation = requested_generation;
    }
    return requested_generation;
}

void ds5_audio_bridge_init(void) {
    ds5_audio_bridge_set_speaker_control(false, 0);
    queue_init(&speaker_pcm_block_queue, sizeof(pcm_block_t),
               PCM_BLOCK_QUEUE_DEPTH);
    queue_init(&speaker_encoded_frame_queue, sizeof(encoded_frame_t),
               ENCODED_FRAME_QUEUE_DEPTH);
    usb_speaker_active = false;
    usb_mic_active = false;
    bridge_connected = false;
    bridge_conn_index = 0xFF;
    producer_samples = 0;
    transport_pair_count = 0;
    pipeline_reset_generation = 0;
    codec_reset_generation = 0;
    speaker_encoder = NULL;
    speaker_encoder_init_attempted = false;
}

void ds5_audio_bridge_set_usb_streams(bool speaker_active, bool mic_active) {
    ds5_audio_diag_note_usb_speaker(speaker_active, time_us_32());
    usb_speaker_active = speaker_active;
    usb_mic_active = mic_active;
    if (!speaker_active) {
        // TinyUSB and this accumulator share core0, so an inactive alternate
        // setting is a safe point to discard a partial source window.
        bridge_clear_pipeline();
    }
}

void ds5_audio_bridge_submit_speaker_pcm(const uint8_t *data, uint16_t len) {
    if (!usb_speaker_active || !data || len == 0) return;
    if (len > PCM_USB_PACKET_BYTES) len = PCM_USB_PACKET_BYTES;
    len &= (uint16_t)~3u;  // complete interleaved stereo int16 frames only
    if (len == 0) return;

    bool nonzero = false;
    for (uint16_t i = 0; i < len; ++i) {
        if (data[i] != 0) {
            nonzero = true;
            break;
        }
    }
    bool const short_packet = len != PCM_USB_PACKET_BYTES;
    bool dropped = false;

    uint16_t source_samples = len / sizeof(int16_t);
    uint8_t const *source = data;
    while (source_samples != 0) {
        uint16_t const room = (uint16_t)(PCM_BLOCK_SAMPLES - producer_samples);
        uint16_t const copy_samples =
            source_samples < room ? source_samples : room;
        memcpy(producer_pcm_block.samples + producer_samples, source,
               copy_samples * sizeof(int16_t));
        producer_samples += copy_samples;
        source += copy_samples * sizeof(int16_t);
        source_samples -= copy_samples;

        if (producer_samples == PCM_BLOCK_SAMPLES) {
            if (queue_is_full(&speaker_pcm_block_queue)) {
                queue_try_remove(&speaker_pcm_block_queue,
                                 &producer_discard_block);
                dropped = true;
            }
            queue_try_add(&speaker_pcm_block_queue, &producer_pcm_block);
            producer_samples = 0;
        }
    }

    ds5_audio_diag_note_usb_pcm(time_us_32(), nonzero, short_packet, dropped,
                                queue_get_level(&speaker_pcm_block_queue));
}

void ds5_audio_bridge_connect(uint8_t conn_index) {
    bridge_conn_index = conn_index;
    bridge_connected = true;
    bridge_clear_pipeline();
}

void ds5_audio_bridge_disconnect(uint8_t conn_index) {
    if (bridge_connected && bridge_conn_index == conn_index) {
        bridge_connected = false;
        bridge_conn_index = 0xFF;
        bridge_clear_pipeline();
    }
}

bool ds5_audio_bridge_owns_connection(uint8_t conn_index) {
    return bridge_connected && bridge_conn_index == conn_index;
}

void __not_in_flash_func(ds5_audio_bridge_codec_task)(void) {
    ds5_audio_diag_note_codec_call(time_us_32());
    // Codec construction and all encode calls stay on core1's explicit 48 KiB
    // stack. Core0's normal SDK stack is intentionally much smaller.
    if (!bridge_prepare_encoder()) {
        diag_codec_no_encoder++;
        return;
    }
    uint32_t const requested_generation = bridge_sync_encoder_generation();
    if (!bridge_connected) {
        diag_codec_disconnected++;
        return;
    }
    if (!usb_speaker_active) {
        diag_codec_usb_inactive++;
        return;
    }

    // Cooperative fallback retained for non-worker callers: consume at most one
    // complete source block per call.
    if (queue_try_remove(&speaker_pcm_block_queue, &codec_pcm_block)) {
        diag_codec_blocks_dequeued++;
        bridge_encode_input_frame(&codec_pcm_block, requested_generation);
    } else {
        diag_codec_no_pcm++;
    }
}

void __not_in_flash_func(ds5_audio_bridge_codec_worker)(void) {
    // The SDK's threadsafe-background CYW43 architecture runs BTstack from a
    // low-priority IRQ on core1. btstack_run_loop_execute() therefore left the
    // core1 foreground sleeping. Block that foreground directly on the PCM
    // queue and wake it from core0's producer notification. Bluetooth can
    // preempt an encode, but codec work no longer occupies or waits for a
    // BTstack timer callback.
    (void)bridge_prepare_encoder();
    for (;;) {
        queue_remove_blocking(&speaker_pcm_block_queue, &codec_pcm_block);
        ds5_audio_diag_note_codec_call(time_us_32());

        if (!bridge_prepare_encoder()) {
            diag_codec_no_encoder++;
            continue;
        }
        uint32_t const requested_generation =
            bridge_sync_encoder_generation();
        if (!bridge_connected) {
            diag_codec_disconnected++;
            continue;
        }
        if (!usb_speaker_active) {
            diag_codec_usb_inactive++;
            continue;
        }

        diag_codec_blocks_dequeued++;
        bridge_encode_input_frame(&codec_pcm_block, requested_generation);
    }
}

bool ds5_audio_bridge_peek_speaker_pair(
    uint8_t frame_a[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN],
    uint8_t frame_b[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN]) {
    while (transport_pair_count < 2) {
        encoded_frame_t *slot = &transport_pair[transport_pair_count];
        if (!queue_try_remove(&speaker_encoded_frame_queue, slot)) break;
        if (slot->generation == pipeline_reset_generation)
            transport_pair_count++;
    }
    if (transport_pair_count < 2) return false;
    memcpy(frame_a, transport_pair[0].data, DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    memcpy(frame_b, transport_pair[1].data, DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    return true;
}

void ds5_audio_bridge_commit_speaker_pair(void) {
    if (transport_pair_count >= 2) transport_pair_count = 0;
}

bool ds5_audio_bridge_mic_active(void) {
    // Microphone decode/USB return is not implemented yet. Keep the controller
    // mic disabled instead of enabling traffic that is discarded by ds5_bt.c.
    (void)usb_mic_active;
    return false;
}

#else

void ds5_audio_bridge_init(void) {
    ds5_audio_bridge_set_speaker_control(false, 0);
}
void ds5_audio_bridge_set_usb_streams(bool speaker_active, bool mic_active) {
    (void)speaker_active;
    (void)mic_active;
}
void ds5_audio_bridge_submit_speaker_pcm(const uint8_t *data, uint16_t len) {
    (void)data;
    (void)len;
}
void ds5_audio_bridge_connect(uint8_t conn_index) { (void)conn_index; }
void ds5_audio_bridge_disconnect(uint8_t conn_index) { (void)conn_index; }
bool ds5_audio_bridge_owns_connection(uint8_t conn_index) {
    (void)conn_index;
    return false;
}
void ds5_audio_bridge_codec_task(void) {}
void ds5_audio_bridge_codec_worker(void) {}
bool ds5_audio_bridge_peek_speaker_pair(
    uint8_t frame_a[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN],
    uint8_t frame_b[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN]) {
    (void)frame_a;
    (void)frame_b;
    return false;
}
void ds5_audio_bridge_commit_speaker_pair(void) {}
bool ds5_audio_bridge_mic_active(void) { return false; }

#endif
