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

// --- Diagnostics (always compiled; fed only from core 1 in audio builds) -----
static volatile uint32_t diag_core1_max_gap_us;
static volatile uint32_t diag_core1_gaps_over_10ms;
static volatile uint32_t diag_send_max_gap_us;
static volatile uint32_t diag_send_gaps_over_40ms;
static volatile uint32_t diag_sends_total;
static uint32_t diag_core1_last_us;
static uint32_t diag_send_last_us;

void ds5_audio_diag_note_core1_tick(uint32_t now_us) {
    if (diag_core1_last_us) {
        uint32_t gap = now_us - diag_core1_last_us;
        if (gap > diag_core1_max_gap_us) diag_core1_max_gap_us = gap;
        if (gap > 10000u) diag_core1_gaps_over_10ms++;
    }
    diag_core1_last_us = now_us;
}

void ds5_audio_diag_note_send(uint32_t now_us) {
    if (diag_send_last_us) {
        uint32_t gap = now_us - diag_send_last_us;
        if (gap > diag_send_max_gap_us) diag_send_max_gap_us = gap;
        if (gap > 40000u) diag_send_gaps_over_40ms++;
    }
    diag_send_last_us = now_us;
    diag_sends_total++;
}

void ds5_audio_diag_get(ds5_audio_diag_t *out) {
    if (!out) return;
    out->core1_max_gap_us = diag_core1_max_gap_us;
    out->core1_gaps_over_10ms = diag_core1_gaps_over_10ms;
    out->send_max_gap_us = diag_send_max_gap_us;
    out->send_gaps_over_40ms = diag_send_gaps_over_40ms;
    out->sends_total = diag_sends_total;
}

void ds5_audio_diag_reset(void) {
    diag_core1_max_gap_us = 0;
    diag_core1_gaps_over_10ms = 0;
    diag_send_max_gap_us = 0;
    diag_send_gaps_over_40ms = 0;
    diag_sends_total = 0;
    diag_core1_last_us = 0;
    diag_send_last_us = 0;
}

#if defined(NS2_DS5_AUDIO_TEST_TONE)

#include "ds5_audio_test_tone.h"
#include "pico/time.h"

// Each report carries two 480-sample, 48 kHz Opus frames: exactly 20 ms.
// Pacing this from the USB endpoint's former 512-frame accumulation interval
// (21.333 ms) guaranteed an underrun between every report pair.
#define TEST_TONE_PAIR_INTERVAL_US 20000u

static volatile bool usb_mic_active;
static volatile bool bridge_connected;
static volatile uint8_t bridge_conn_index;
static bool test_tone_pair_ready;
static uint32_t test_tone_pair_deadline_us;

void ds5_audio_bridge_init(void) {
    ds5_audio_bridge_set_speaker_control(false, 0);
    usb_mic_active = false;
    bridge_connected = false;
    bridge_conn_index = 0xFF;
    test_tone_pair_ready = false;
    test_tone_pair_deadline_us = 0;
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
    test_tone_pair_ready = false;
    test_tone_pair_deadline_us = 0;
}

void ds5_audio_bridge_disconnect(uint8_t conn_index) {
    if (bridge_connected && bridge_conn_index == conn_index) {
        bridge_connected = false;
        bridge_conn_index = 0xFF;
        test_tone_pair_ready = false;
        test_tone_pair_deadline_us = 0;
    }
}

bool ds5_audio_bridge_owns_connection(uint8_t conn_index) {
    return bridge_connected && bridge_conn_index == conn_index;
}

void ds5_audio_bridge_codec_task(uint8_t conn_index) {
    if (!bridge_connected || bridge_conn_index != conn_index ||
        test_tone_pair_ready)
        return;

    uint32_t const now_us = time_us_32();
    if (test_tone_pair_deadline_us == 0) {
        test_tone_pair_deadline_us = now_us + TEST_TONE_PAIR_INTERVAL_US;
        test_tone_pair_ready = true;
    } else if ((int32_t)(now_us - test_tone_pair_deadline_us) >= 0) {
        uint32_t const late_us = now_us - test_tone_pair_deadline_us;
        // Advance from the prior deadline so task jitter cannot accumulate.
        // If Bluetooth was stalled for more than one whole frame pair, restart
        // from now instead of trying to catch up with a burst of stale audio.
        test_tone_pair_deadline_us =
            late_us >= TEST_TONE_PAIR_INTERVAL_US
                ? now_us + TEST_TONE_PAIR_INTERVAL_US
                : test_tone_pair_deadline_us + TEST_TONE_PAIR_INTERVAL_US;
        test_tone_pair_ready = true;
    }
}

bool ds5_audio_bridge_peek_speaker_pair(
    uint8_t frame_a[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN],
    uint8_t frame_b[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN]) {
    if (!test_tone_pair_ready) return false;
    memcpy(frame_a, ds5_audio_test_tone_frames[0],
           DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    memcpy(frame_b, ds5_audio_test_tone_frames[1],
           DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    return true;
}

void ds5_audio_bridge_commit_speaker_pair(void) {
    test_tone_pair_ready = false;
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
#include "pico/util/queue.h"

#define PCM_PACKET_MAX_BYTES 192u
#define PCM_PACKET_QUEUE_DEPTH 12u
#define PCM_INPUT_FRAMES 480u
#define PCM_OUTPUT_FRAMES 480u
#define PCM_CHANNELS DS5_AUDIO_RESAMPLE_CHANNELS

typedef struct {
    uint16_t len;
    uint8_t data[PCM_PACKET_MAX_BYTES];
} pcm_packet_t;

static queue_t speaker_pcm_queue;
static volatile bool usb_speaker_active;
static volatile bool usb_mic_active;
static volatile bool bridge_connected;
static volatile uint8_t bridge_conn_index;

static OpusEncoder *speaker_encoder;
static bool speaker_encoder_init_attempted;
static int16_t input_pcm[PCM_INPUT_FRAMES * PCM_CHANNELS];
static uint16_t input_samples;
static uint8_t encoded_pair[2][DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN];
static uint8_t encoded_count;

static void bridge_clear_pipeline(void) {
    pcm_packet_t discarded;
    while (queue_try_remove(&speaker_pcm_queue, &discarded)) {}
    input_samples = 0;
    encoded_count = 0;
    if (speaker_encoder) opus_encoder_ctl(speaker_encoder, OPUS_RESET_STATE);
}

void ds5_audio_bridge_init(void) {
    ds5_audio_bridge_set_speaker_control(false, 0);
    queue_init(&speaker_pcm_queue, sizeof(pcm_packet_t), PCM_PACKET_QUEUE_DEPTH);
    usb_speaker_active = false;
    usb_mic_active = false;
    bridge_connected = false;
    bridge_conn_index = 0xFF;
    input_samples = 0;
    encoded_count = 0;
    speaker_encoder = NULL;
    speaker_encoder_init_attempted = false;
}

void ds5_audio_bridge_set_usb_streams(bool speaker_active, bool mic_active) {
    usb_speaker_active = speaker_active;
    usb_mic_active = mic_active;
    if (!speaker_active) {
        // Core0 is the sole producer. Do not mutate core1's codec buffers here;
        // queued PCM is harmless and core1 clears the full pipeline when it
        // observes the inactive state.
        pcm_packet_t discarded;
        while (queue_try_remove(&speaker_pcm_queue, &discarded)) {}
    }
}

void ds5_audio_bridge_submit_speaker_pcm(const uint8_t *data, uint16_t len) {
    if (!usb_speaker_active || !data || len == 0) return;
    if (len > PCM_PACKET_MAX_BYTES) len = PCM_PACKET_MAX_BYTES;
    len &= (uint16_t)~3u;  // complete interleaved stereo int16 frames only
    if (len == 0) return;

    pcm_packet_t packet;
    packet.len = len;
    memcpy(packet.data, data, len);
    if (queue_is_full(&speaker_pcm_queue)) {
        pcm_packet_t discarded;
        queue_try_remove(&speaker_pcm_queue, &discarded);
    }
    queue_try_add(&speaker_pcm_queue, &packet);
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

void ds5_audio_bridge_codec_task(uint8_t conn_index) {
    if (!bridge_connected || bridge_conn_index != conn_index) return;

    // Codec construction and all encode calls stay on core1's explicit 48 KiB
    // stack. Core0's normal SDK stack is intentionally much smaller.
    if (!speaker_encoder && !speaker_encoder_init_attempted) {
        speaker_encoder_init_attempted = true;
        int error = OPUS_OK;
        speaker_encoder = opus_encoder_create(48000, PCM_CHANNELS,
                                              OPUS_APPLICATION_AUDIO, &error);
        if (!speaker_encoder || error != OPUS_OK) {
            speaker_encoder = NULL;
            return;
        }
        opus_encoder_ctl(speaker_encoder,
                         OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
        opus_encoder_ctl(
            speaker_encoder,
            OPUS_SET_BITRATE(DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN * 8 * 100));
        opus_encoder_ctl(speaker_encoder, OPUS_SET_VBR(0));
        opus_encoder_ctl(speaker_encoder, OPUS_SET_COMPLEXITY(0));
    }

    if (!speaker_encoder || !usb_speaker_active) {
        if (input_samples || encoded_count) bridge_clear_pipeline();
        return;
    }
    if (encoded_count >= 2) return;

    pcm_packet_t packet;
    while (input_samples < (PCM_INPUT_FRAMES * PCM_CHANNELS) &&
           queue_try_remove(&speaker_pcm_queue, &packet)) {
        uint16_t const packet_samples = packet.len / sizeof(int16_t);
        uint16_t const room =
            (uint16_t)(PCM_INPUT_FRAMES * PCM_CHANNELS - input_samples);
        uint16_t const copy_samples = packet_samples < room ? packet_samples : room;
        memcpy(input_pcm + input_samples, packet.data,
               copy_samples * sizeof(int16_t));
        input_samples += copy_samples;

        // Preserve a packet's tail if a future USB endpoint packet size does
        // not divide the 480-frame Opus window exactly. The current 192-byte
        // stereo packet contains 48 frames, so ten packets are exactly 10 ms.
        if (copy_samples < packet_samples) {
            uint16_t const tail_samples = packet_samples - copy_samples;
            pcm_packet_t tail = {.len = (uint16_t)(tail_samples * sizeof(int16_t))};
            memcpy(tail.data, packet.data + copy_samples * sizeof(int16_t),
                   tail.len);
            // This task is the only consumer, but queue_add would place the
            // tail after newer packets. Save it directly after encoding below.
            if (input_samples == PCM_INPUT_FRAMES * PCM_CHANNELS) {
                int encoded = opus_encode(
                    speaker_encoder, input_pcm, PCM_OUTPUT_FRAMES,
                    encoded_pair[encoded_count], DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
                if (encoded == DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN) {
                    encoded_count++;
                }
                memcpy(input_pcm, tail.data, tail.len);
                input_samples = tail_samples;
                return;
            }
        }
    }

    if (input_samples == PCM_INPUT_FRAMES * PCM_CHANNELS) {
        int encoded = opus_encode(
            speaker_encoder, input_pcm, PCM_OUTPUT_FRAMES,
            encoded_pair[encoded_count], DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
        input_samples = 0;
        if (encoded == DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN) {
            encoded_count++;
        }
    }
}

bool ds5_audio_bridge_peek_speaker_pair(
    uint8_t frame_a[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN],
    uint8_t frame_b[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN]) {
    if (encoded_count < 2) return false;
    memcpy(frame_a, encoded_pair[0], DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    memcpy(frame_b, encoded_pair[1], DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    return true;
}

void ds5_audio_bridge_commit_speaker_pair(void) {
    if (encoded_count >= 2) encoded_count = 0;
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
void ds5_audio_bridge_codec_task(uint8_t conn_index) { (void)conn_index; }
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
