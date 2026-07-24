#include "ds5_audio_bridge.h"
#include "ds5_audio_resample.h"

#include <stdlib.h>
#include <string.h>

#ifdef NS2_DS5_AUDIO
#include "pico/time.h"
#endif

static volatile bool speaker_control_muted;
static volatile uint8_t speaker_control_volume = 100;
static volatile bool ds5_encoder_exclusive_encode = true;
static volatile uint8_t switch2_pro2_encoder_complexity;
static volatile bool switch2_pro2_encoder_analysis;

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

void ds5_audio_bridge_set_switch2_pro2_complexity(uint8_t complexity) {
    if (complexity > 10u) complexity = 10u;
    __atomic_store_n(&switch2_pro2_encoder_complexity, complexity,
                     __ATOMIC_RELEASE);
}

uint8_t ds5_audio_bridge_switch2_pro2_complexity(void) {
    return __atomic_load_n(&switch2_pro2_encoder_complexity,
                           __ATOMIC_ACQUIRE);
}

void ds5_audio_bridge_set_switch2_pro2_analysis(bool enabled) {
    __atomic_store_n(&switch2_pro2_encoder_analysis, enabled,
                     __ATOMIC_RELEASE);
}

bool ds5_audio_bridge_switch2_pro2_analysis(void) {
    return __atomic_load_n(&switch2_pro2_encoder_analysis,
                           __ATOMIC_ACQUIRE);
}

void ds5_audio_bridge_set_ds5_exclusive_encode(bool enabled) {
    __atomic_store_n(&ds5_encoder_exclusive_encode, enabled,
                     __ATOMIC_RELEASE);
}

bool ds5_audio_bridge_ds5_exclusive_encode(void) {
    return __atomic_load_n(&ds5_encoder_exclusive_encode,
                           __ATOMIC_ACQUIRE);
}

// Always compiled so config-mode diagnostics remain linkable in ordinary
// builds. Live-audio builds feed these counters. Aligned uint32_t loads
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

#ifdef NS2_DS5_AUDIO

#if defined(NS2_DS5_AUDIO_LIVE_OPUS)
// UART PCM capture and Pro2 live encoding are deliberately mutually
// exclusive diagnostics. Overlay their ~4 KiB workspaces so the signal-path
// stack can grow without increasing total RP2350 SRAM use.
static union {
    uint8_t capture[DS5_AUDIO_PCM_CAPTURE_BYTES];
    int16_t pro2[960u * 2u];
} pcm_workspace;
#define pcm_capture_data pcm_workspace.capture
#define pro2_pcm pcm_workspace.pro2
#else
static uint8_t pcm_capture_data[DS5_AUDIO_PCM_CAPTURE_BYTES];
#endif
static volatile bool pcm_capture_armed;
static volatile bool pcm_capture_complete;
static volatile uint16_t pcm_capture_length;
static volatile uint16_t pcm_capture_packets;
static volatile uint32_t pcm_capture_start_us;
static volatile uint32_t pcm_capture_end_us;

static void ds5_audio_pcm_capture_submit(const uint8_t *data, uint16_t len,
                                         bool nonzero) {
    if (!pcm_capture_armed || !data || len == 0) return;
    // Arming while the console is streaming silence is useful only if the
    // capture waits for an audible packet. Once triggered, retain every byte,
    // including zero crossings and any later silence within the window.
    if (pcm_capture_length == 0 && !nonzero) return;

    if (pcm_capture_length == 0) pcm_capture_start_us = time_us_32();
    uint16_t const remaining =
        (uint16_t)(DS5_AUDIO_PCM_CAPTURE_BYTES - pcm_capture_length);
    uint16_t const copy_len = len < remaining ? len : remaining;
    memcpy(&pcm_capture_data[pcm_capture_length], data, copy_len);
    pcm_capture_length = (uint16_t)(pcm_capture_length + copy_len);
    pcm_capture_packets++;

    if (pcm_capture_length == DS5_AUDIO_PCM_CAPTURE_BYTES) {
        pcm_capture_end_us = time_us_32();
        pcm_capture_armed = false;
        pcm_capture_complete = true;
    }
}

void ds5_audio_pcm_capture_arm(void) {
    if (ds5_audio_bridge_switch2_pro2_active()) return;
    pcm_capture_armed = false;
    pcm_capture_complete = false;
    pcm_capture_length = 0;
    pcm_capture_packets = 0;
    pcm_capture_start_us = 0;
    pcm_capture_end_us = 0;
    pcm_capture_armed = true;
}

void ds5_audio_pcm_capture_stop(void) {
    pcm_capture_armed = false;
    if (pcm_capture_length != 0) {
        pcm_capture_end_us = time_us_32();
        pcm_capture_complete = true;
    }
}

static uint32_t pcm_capture_crc32(const uint8_t *data, uint16_t len) {
    uint32_t crc = UINT32_MAX;
    for (uint16_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc ^ UINT32_MAX;
}

void ds5_audio_pcm_capture_get_status(ds5_audio_pcm_capture_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->armed = pcm_capture_armed;
    out->complete = pcm_capture_complete;
    out->captured_bytes = pcm_capture_length;
    out->capacity_bytes = DS5_AUDIO_PCM_CAPTURE_BYTES;
    out->packets = pcm_capture_packets;
    out->start_us = pcm_capture_start_us;
    out->end_us = pcm_capture_end_us;
    out->crc32 = pcm_capture_crc32(pcm_capture_data, pcm_capture_length);

    uint16_t const frames = (uint16_t)(pcm_capture_length / 4u);
    for (uint16_t frame = 0; frame < frames; ++frame) {
        uint16_t const offset = (uint16_t)(frame * 4u);
        int16_t const left = (int16_t)((uint16_t)pcm_capture_data[offset] |
                                      ((uint16_t)pcm_capture_data[offset + 1u] << 8));
        int16_t const right = (int16_t)((uint16_t)pcm_capture_data[offset + 2u] |
                                       ((uint16_t)pcm_capture_data[offset + 3u] << 8));
        int32_t const left_abs = left < 0 ? -(int32_t)left : left;
        int32_t const right_abs = right < 0 ? -(int32_t)right : right;
        if ((uint32_t)left_abs > out->peak_left)
            out->peak_left = (uint16_t)left_abs;
        if ((uint32_t)right_abs > out->peak_right)
            out->peak_right = (uint16_t)right_abs;
        out->sum_left += left;
        out->sum_right += right;
        out->sum_squares_left += (uint64_t)((int32_t)left * left);
        out->sum_squares_right += (uint64_t)((int32_t)right * right);
    }
}

uint16_t ds5_audio_pcm_capture_read(uint16_t offset, uint8_t *out,
                                    uint16_t max_len) {
    if (!out || max_len == 0 || pcm_capture_armed ||
        offset >= pcm_capture_length) return 0;
    uint16_t available = (uint16_t)(pcm_capture_length - offset);
    if (max_len > DS5_AUDIO_PCM_CAPTURE_READ_MAX)
        max_len = DS5_AUDIO_PCM_CAPTURE_READ_MAX;
    uint16_t const copy_len = available < max_len ? available : max_len;
    memcpy(out, &pcm_capture_data[offset], copy_len);
    return copy_len;
}

#else

void ds5_audio_pcm_capture_arm(void) {}
void ds5_audio_pcm_capture_stop(void) {}
void ds5_audio_pcm_capture_get_status(ds5_audio_pcm_capture_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->capacity_bytes = DS5_AUDIO_PCM_CAPTURE_BYTES;
}
uint16_t ds5_audio_pcm_capture_read(uint16_t offset, uint8_t *out,
                                    uint16_t max_len) {
    (void)offset;
    (void)out;
    (void)max_len;
    return 0;
}

#endif

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

bool ds5_audio_bridge_speaker_requested(void) {
    return bridge_connected;
}

void ds5_audio_bridge_set_switch2_pro2_active(bool active) { (void)active; }
bool ds5_audio_bridge_switch2_pro2_active(void) { return false; }
bool ds5_audio_bridge_usb_speaker_active(void) { return false; }
bool ds5_audio_bridge_peek_switch2_pro2_frame(
    uint8_t frame[SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN]) {
    (void)frame;
    return false;
}
void ds5_audio_bridge_commit_switch2_pro2_frame(void) {}
void ds5_audio_bridge_note_switch2_pro2_idle_frame(void) {}

#elif defined(NS2_DS5_AUDIO_LIVE_OPUS)

#include "opus.h"
#include "celt.h"
#include "cpu_support.h"
#include "analysis.h"
#include "pico/time.h"
#include "pico/util/queue.h"
#include "pico/async_context.h"
#include "pico/cyw43_arch.h"

// Opus's tonality analyzer references this otherwise tiny helper from
// opus_encoder.c. Providing the float-build equivalent locally prevents that
// single reference from retaining the public Opus wrapper and the unused SILK
// encoder graph. All inputs here are finite CELT analysis samples.
int is_digital_silence(const opus_res *pcm, int frame_size, int channels,
                       int lsb_depth) {
    opus_val32 sample_max = 0;
    int const sample_count = frame_size * channels;
    for (int i = 0; i < sample_count; ++i) {
        opus_val32 magnitude = pcm[i];
        if (magnitude < 0) magnitude = -magnitude;
        if (magnitude > sample_max) sample_max = magnitude;
    }
    return sample_max <= (opus_val16)1 / (1 << lsb_depth);
}

#define PCM_USB_PACKET_BYTES 192u
#define PCM_BLOCK_QUEUE_DEPTH 2u
#define ENCODED_FRAME_QUEUE_DEPTH 2u
#define PCM_INPUT_FRAMES DS5_AUDIO_RESAMPLE_INPUT_FRAMES
#define PCM_OUTPUT_FRAMES DS5_AUDIO_RESAMPLE_OUTPUT_FRAMES
#define PCM_CHANNELS DS5_AUDIO_RESAMPLE_CHANNELS
#define PCM_BLOCK_SAMPLES (PCM_INPUT_FRAMES * PCM_CHANNELS)
#define PRO2_PCM_FRAMES 960u
#define PRO2_PCM_SAMPLES (PRO2_PCM_FRAMES * PCM_CHANNELS)
#define DS5_OPUS_TOC 0xF4u

typedef struct {
    int16_t samples[PCM_BLOCK_SAMPLES];
    bool nonzero;
} pcm_block_t;

typedef struct {
    uint32_t generation;
    uint8_t data[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN];
} encoded_frame_t;

typedef struct {
    uint32_t generation;
    uint8_t data[SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN];
} pro2_encoded_frame_t;

typedef enum {
    BRIDGE_ENCODER_NONE = 0,
    BRIDGE_ENCODER_DS5,
    BRIDGE_ENCODER_PRO2,
} bridge_encoder_mode_t;

static queue_t speaker_pcm_block_queue;
static queue_t speaker_encoded_frame_queue;
static queue_t pro2_encoded_frame_queue;
static volatile bool usb_speaker_active;
static volatile bool usb_mic_active;
static volatile bool bridge_connected;
static volatile uint8_t bridge_conn_index;
static volatile bool pro2_bridge_active;
static volatile uint32_t pro2_idle_frames_pending;

// TinyUSB/core0 owns this accumulator. This is deliberately the same
// producer boundary as DS5Dongle: cross to the codec core only after a full
// 512-frame source window exists, rather than queueing individual 1 ms USB
// packets and asking core1 to assemble them under Bluetooth load.
static pcm_block_t producer_pcm_block;
static pcm_block_t producer_discard_block;
static uint16_t producer_samples;

static void *ds5_encoder_allocation;
static CELTEncoder *ds5_encoder;
static float *ds5_encoder_pcm;
static void *pro2_encoder_allocation;
static CELTEncoder *pro2_encoder;
static float *pro2_encoder_pcm;
static TonalityAnalysisState *pro2_analysis;
static const CELTMode *pro2_celt_mode;
static uint8_t pro2_encoder_applied_complexity;
static bool pro2_encoder_applied_analysis;
static bridge_encoder_mode_t speaker_encoder_mode;
static pcm_block_t codec_pcm_block;
static int16_t resampled_pcm[PCM_OUTPUT_FRAMES * PCM_CHANNELS];
static uint16_t pro2_pcm_samples;
static bool pro2_pcm_nonzero;
static encoded_frame_t codec_encoded_frame;
static encoded_frame_t codec_discard_frame;
static encoded_frame_t transport_pair[2];
static uint8_t transport_pair_count;
static pro2_encoded_frame_t pro2_codec_frame;
static pro2_encoded_frame_t pro2_discard_frame;
static pro2_encoded_frame_t pro2_transport_frame;
static bool pro2_transport_valid;
static volatile uint32_t pipeline_reset_generation;
static uint32_t codec_reset_generation;
static uint8_t silent_frames[2][DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN];
static volatile bool silent_frames_ready;

// Keep Pro Controller 2's validated tonality-analysis input identical to
// libopus's downmix_float(), but define it locally. Referencing the public
// helper pulls the entire general Opus encoder object into SRAM even though
// both controller transports use the lower-level CELT encoder directly.
static void pro2_analysis_downmix_float(const void *input,
                                        opus_val32 *output,
                                        int subframe, int offset,
                                        int channel1, int channel2,
                                        int channels) {
    const float *samples = input;
    for (int frame = 0; frame < subframe; ++frame) {
        output[frame] =
            FLOAT2SIG(samples[(frame + offset) * channels + channel1]);
    }
    if (channel2 >= 0) {
        for (int frame = 0; frame < subframe; ++frame) {
            output[frame] +=
                FLOAT2SIG(samples[(frame + offset) * channels + channel2]);
        }
    } else if (channel2 == -2) {
        for (int channel = 1; channel < channels; ++channel) {
            for (int frame = 0; frame < subframe; ++frame) {
                output[frame] += FLOAT2SIG(
                    samples[(frame + offset) * channels + channel]);
            }
        }
    }
    for (int frame = 0; frame < subframe; ++frame) {
        if (output[frame] < -65536.0f) output[frame] = -65536.0f;
        if (output[frame] > 65536.0f) output[frame] = 65536.0f;
        if (celt_isnan(output[frame])) output[frame] = 0;
    }
}

static bool bridge_encoder_available(bridge_encoder_mode_t mode) {
    if (speaker_encoder_mode != mode) return false;
    if (mode == BRIDGE_ENCODER_DS5)
        return ds5_encoder != NULL && ds5_encoder_pcm != NULL;
    if (mode == BRIDGE_ENCODER_PRO2)
        return pro2_encoder != NULL && pro2_encoder_pcm != NULL;
    return false;
}

static void bridge_destroy_encoder(void) {
    free(ds5_encoder_allocation);
    ds5_encoder_allocation = NULL;
    ds5_encoder = NULL;
    ds5_encoder_pcm = NULL;
    free(pro2_encoder_allocation);
    pro2_encoder_allocation = NULL;
    pro2_encoder = NULL;
    pro2_encoder_pcm = NULL;
    pro2_analysis = NULL;
    pro2_celt_mode = NULL;
    pro2_encoder_applied_complexity = UINT8_MAX;
    pro2_encoder_applied_analysis = false;
    speaker_encoder_mode = BRIDGE_ENCODER_NONE;
}

static bool __not_in_flash_func(bridge_configure_encoder)(
    bridge_encoder_mode_t mode) {
    if (bridge_encoder_available(mode)) return true;

    // Both controller transports carry fixed-size, CELT-only Opus packets.
    // Use dedicated direct-CELT states for each format so DualSense does not
    // pay for the general public Opus encoder's larger state and wrapper path.
    // Keep the already validated Pro Controller 2 configuration independent.
    bridge_destroy_encoder();
    if (mode == BRIDGE_ENCODER_PRO2) {
        int const encoder_size = celt_encoder_get_size(PCM_CHANNELS);
        if (encoder_size <= 0) return false;
        size_t const encoder_alignment = _Alignof(float);
        size_t const encoder_bytes =
            ((size_t)encoder_size + encoder_alignment - 1u) &
            ~(encoder_alignment - 1u);
        size_t const pcm_bytes =
            PRO2_PCM_SAMPLES * sizeof(*pro2_encoder_pcm);
        size_t const analysis_alignment = _Alignof(TonalityAnalysisState);
        size_t const analysis_offset =
            (encoder_bytes + pcm_bytes + analysis_alignment - 1u) &
            ~(analysis_alignment - 1u);
        size_t const allocation_bytes =
            analysis_offset + sizeof(*pro2_analysis);
        pro2_encoder_allocation = malloc(allocation_bytes);
        if (!pro2_encoder_allocation) return false;
        uint8_t *const allocation = pro2_encoder_allocation;
        pro2_encoder = (CELTEncoder *)allocation;
        pro2_encoder_pcm = (float *)(allocation + encoder_bytes);
        pro2_analysis =
            (TonalityAnalysisState *)(allocation + analysis_offset);
        if (celt_encoder_init(pro2_encoder, 48000, PCM_CHANNELS,
                              opus_select_arch()) != OPUS_OK) {
            bridge_destroy_encoder();
            return false;
        }
    } else if (mode == BRIDGE_ENCODER_DS5) {
        int const encoder_size = celt_encoder_get_size(PCM_CHANNELS);
        if (encoder_size <= 0) return false;
        size_t const encoder_alignment = _Alignof(float);
        size_t const encoder_bytes =
            ((size_t)encoder_size + encoder_alignment - 1u) &
            ~(encoder_alignment - 1u);
        size_t const pcm_bytes =
            PCM_OUTPUT_FRAMES * PCM_CHANNELS * sizeof(*ds5_encoder_pcm);
        ds5_encoder_allocation = malloc(encoder_bytes + pcm_bytes);
        if (!ds5_encoder_allocation) return false;
        uint8_t *const allocation = ds5_encoder_allocation;
        ds5_encoder = (CELTEncoder *)allocation;
        ds5_encoder_pcm = (float *)(allocation + encoder_bytes);
        if (celt_encoder_init(ds5_encoder, 48000, PCM_CHANNELS,
                              opus_select_arch()) != OPUS_OK) {
            bridge_destroy_encoder();
            return false;
        }
    } else {
        return false;
    }

    int status = OPUS_OK;
    if (mode == BRIDGE_ENCODER_PRO2) {
        status |= celt_encoder_ctl(pro2_encoder, CELT_SET_SIGNALLING(0));
        uint8_t const complexity =
            ds5_audio_bridge_switch2_pro2_complexity();
        status |= celt_encoder_ctl(pro2_encoder,
                                   OPUS_SET_COMPLEXITY(complexity));
        status |= celt_encoder_ctl(pro2_encoder, OPUS_SET_VBR(0));
        status |= celt_encoder_ctl(pro2_encoder, OPUS_SET_BITRATE(96000));
        status |= celt_encoder_ctl(pro2_encoder,
                                   CELT_GET_MODE(&pro2_celt_mode));
        tonality_analysis_init(pro2_analysis, 48000);
        pro2_analysis->application = OPUS_APPLICATION_RESTRICTED_CELT;
        pro2_encoder_applied_complexity = complexity;
        pro2_encoder_applied_analysis =
            ds5_audio_bridge_switch2_pro2_analysis();
    } else if (mode == BRIDGE_ENCODER_DS5) {
        status |= celt_encoder_ctl(ds5_encoder, CELT_SET_SIGNALLING(0));
        status |= celt_encoder_ctl(ds5_encoder, OPUS_SET_COMPLEXITY(0));
        status |= celt_encoder_ctl(ds5_encoder, OPUS_SET_VBR(0));
        status |= celt_encoder_ctl(
            ds5_encoder,
            OPUS_SET_BITRATE(DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN * 8 * 100));
        status |= celt_encoder_ctl(ds5_encoder, OPUS_RESET_STATE);
    }
    if (status != OPUS_OK ||
        (mode == BRIDGE_ENCODER_PRO2 && !pro2_celt_mode)) {
        bridge_destroy_encoder();
        return false;
    }
    speaker_encoder_mode = mode;
    return true;
}

static bool __not_in_flash_func(bridge_encode_ds5_packet)(
    const float *pcm, uint8_t packet[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN]) {
    packet[0] = DS5_OPUS_TOC;
    int const payload_bytes = celt_encode_with_ec(
        ds5_encoder, pcm, PCM_OUTPUT_FRAMES, packet + 1,
        DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN - 1u, NULL);
    return payload_bytes ==
           (int)DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN - 1;
}

static void __not_in_flash_func(bridge_encode_input_frame)(
    const pcm_block_t *block, uint32_t generation) {
    if (!bridge_configure_encoder(BRIDGE_ENCODER_DS5)) return;
    ds5_audio_resample_512_to_480_stereo(block->samples, resampled_pcm);
    for (unsigned i = 0; i < PCM_OUTPUT_FRAMES * PCM_CHANNELS; ++i)
        ds5_encoder_pcm[i] =
            (float)resampled_pcm[i] * (1.0f / 32768.0f);
    uint32_t const encode_start_us = time_us_32();
    bool const success = bridge_encode_ds5_packet(
        ds5_encoder_pcm, codec_encoded_frame.data);
    uint32_t const encode_end_us = time_us_32();
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

static void __not_in_flash_func(bridge_encode_pro2_pcm)(
    uint32_t generation) {
    if (!bridge_configure_encoder(BRIDGE_ENCODER_PRO2)) return;
    uint8_t const requested_complexity =
        ds5_audio_bridge_switch2_pro2_complexity();
    bool const analysis_enabled =
        ds5_audio_bridge_switch2_pro2_analysis();
    if (requested_complexity != pro2_encoder_applied_complexity ||
        analysis_enabled != pro2_encoder_applied_analysis) {
        if (celt_encoder_ctl(pro2_encoder,
                            OPUS_SET_COMPLEXITY(requested_complexity)) !=
            OPUS_OK) return;
        pro2_encoder_applied_complexity = requested_complexity;
        pro2_encoder_applied_analysis = analysis_enabled;
    }
    uint8_t discard[SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN];
    uint32_t pending_idle = __atomic_exchange_n(
        &pro2_idle_frames_pending, 0, __ATOMIC_ACQ_REL);
    // After several identical silence frames CELT's predictor and tonality
    // history have converged. Cap a long USB-inactive interval so resuming
    // audio never causes a burst of hundreds of catch-up encodes.
    if (pending_idle > 8u) pending_idle = 8u;
    if (pending_idle != 0) {
        memset(pro2_encoder_pcm, 0,
               PRO2_PCM_SAMPLES * sizeof(*pro2_encoder_pcm));
        for (uint32_t frame = 0; frame < pending_idle; ++frame) {
            AnalysisInfo idle_analysis;
            AnalysisInfo *idle_analysis_ptr = NULL;
            if (analysis_enabled) {
                memset(&idle_analysis, 0, sizeof(idle_analysis));
                run_analysis(pro2_analysis, pro2_celt_mode,
                             pro2_encoder_pcm, PRO2_PCM_FRAMES,
                             PRO2_PCM_FRAMES, 0, -2, PCM_CHANNELS, 48000,
                             16, pro2_analysis_downmix_float,
                             &idle_analysis);
                idle_analysis_ptr = &idle_analysis;
            }
            if (celt_encoder_ctl(pro2_encoder,
                                 CELT_SET_ANALYSIS(idle_analysis_ptr)) !=
                OPUS_OK) return;
            discard[0] = 0xFC;
            int const idle_bytes = celt_encode_with_ec(
                pro2_encoder, pro2_encoder_pcm, PRO2_PCM_FRAMES,
                discard + 1, SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN - 1u,
                NULL);
            if (idle_bytes !=
                (int)SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN - 1) return;
        }
    }

    for (unsigned i = 0; i < PRO2_PCM_SAMPLES; ++i)
        pro2_encoder_pcm[i] = (float)pro2_pcm[i] * (1.0f / 32768.0f);

    AnalysisInfo analysis_info;
    AnalysisInfo *analysis_ptr = NULL;
    if (analysis_enabled) {
        memset(&analysis_info, 0, sizeof(analysis_info));
        run_analysis(pro2_analysis, pro2_celt_mode, pro2_encoder_pcm,
                     PRO2_PCM_FRAMES, PRO2_PCM_FRAMES, 0, -2,
                     PCM_CHANNELS, 48000, 16,
                     pro2_analysis_downmix_float,
                     &analysis_info);
        analysis_ptr = &analysis_info;
    }
    if (celt_encoder_ctl(pro2_encoder, CELT_SET_ANALYSIS(analysis_ptr)) !=
        OPUS_OK) return;

    uint32_t const encode_start_us = time_us_32();
    // A genuine transport interval is one 240-byte full-band stereo Opus
    // packet. GATT splits it into two 120-byte writes, but that split is below
    // the codec layer: byte zero is the 0xFC TOC and all remaining 239 bytes
    // belong to one range-coded CELT frame.
    pro2_codec_frame.data[0] = 0xFC;
    int const payload_bytes = celt_encode_with_ec(
        pro2_encoder, pro2_encoder_pcm, PRO2_PCM_FRAMES,
        pro2_codec_frame.data + 1,
        SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN - 1u, NULL);
    uint32_t const encode_end_us = time_us_32();
    int const encoded_bytes =
        payload_bytes < 0 ? payload_bytes : payload_bytes + 1;
    bool const success =
        encoded_bytes == SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN;
    ds5_audio_diag_note_opus_frame(encode_end_us,
                                   encode_end_us - encode_start_us, success);
    if (!success || generation != pipeline_reset_generation) return;

    pro2_codec_frame.generation = generation;
    if (queue_is_full(&pro2_encoded_frame_queue)) {
        queue_try_remove(&pro2_encoded_frame_queue, &pro2_discard_frame);
    }
    queue_try_add(&pro2_encoded_frame_queue, &pro2_codec_frame);
}

static void __not_in_flash_func(bridge_consume_pro2_block)(
    const pcm_block_t *block, uint32_t generation) {
    uint16_t source_samples = 0;
    while (source_samples < PCM_BLOCK_SAMPLES) {
        uint16_t const remaining =
            (uint16_t)(PCM_BLOCK_SAMPLES - source_samples);
        uint16_t const room = (uint16_t)(PRO2_PCM_SAMPLES - pro2_pcm_samples);
        uint16_t const copy_samples = remaining < room ? remaining : room;
        memcpy(&pro2_pcm[pro2_pcm_samples], &block->samples[source_samples],
               copy_samples * sizeof(int16_t));
        pro2_pcm_nonzero |= block->nonzero;
        pro2_pcm_samples += copy_samples;
        source_samples += copy_samples;
        if (pro2_pcm_samples == PRO2_PCM_SAMPLES) {
            // Keep the stateful CELT encoder synchronized with the controller's
            // decoder across digital silence. Substituting the fixed idle
            // packet here freezes only our encoder state, so prediction can
            // diverge when nonzero audio resumes. The transport still uses the
            // console's exact FC FF FE idle packet during startup or a genuine
            // encoded-frame underrun.
            bridge_encode_pro2_pcm(generation);
            pro2_pcm_samples = 0;
            pro2_pcm_nonzero = false;
        }
    }
}

static void bridge_clear_pipeline(void) {
    // Invalidate an encode already in progress before draining. Encoded frames
    // carry this generation too, so core0 can reject any stale frame that
    // crosses the queue during the reset race.
    pipeline_reset_generation++;
    producer_samples = 0;
    producer_pcm_block.nonzero = false;
    while (queue_try_remove(&speaker_pcm_block_queue,
                            &producer_discard_block)) {}
    while (queue_try_remove(&speaker_encoded_frame_queue,
                            &transport_pair[0])) {}
    while (queue_try_remove(&pro2_encoded_frame_queue,
                            &pro2_discard_frame)) {}
    transport_pair_count = 0;
    pro2_transport_valid = false;
    ds5_audio_diag_note_pipeline_reset();
}

static bool __not_in_flash_func(bridge_prepare_encoder)(void) {
    bridge_encoder_mode_t const requested_mode = pro2_bridge_active
        ? BRIDGE_ENCODER_PRO2 : BRIDGE_ENCODER_DS5;
    if (!bridge_configure_encoder(requested_mode)) return false;
    if (requested_mode == BRIDGE_ENCODER_DS5 && !silent_frames_ready) {
        // daidr's independently working DualSense path explicitly requires
        // low-delay Opus (pure CELT). It produces the same 200-byte/10-ms
        // payload accepted by the controller while avoiding general audio
        // mode work on the Bluetooth-shared codec core.
        // Report 0x39 always contains two Opus blocks, even when it is being
        // used only for native haptic PCM. Generate a steady-state silence
        // pair once on the large codec-core stack, then reset so this pre-roll
        // cannot influence real console audio. Repeating these valid frames is
        // preferable to zero-filled bytes, which are not an Opus packet.
        memset(resampled_pcm, 0, sizeof(resampled_pcm));
        memset(ds5_encoder_pcm, 0,
               PCM_OUTPUT_FRAMES * PCM_CHANNELS *
                   sizeof(*ds5_encoder_pcm));
        bool silence_ok = true;
        for (unsigned frame = 0; frame < 12u; ++frame) {
            uint8_t discard[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN];
            uint8_t *destination =
                frame >= 10u ? silent_frames[frame - 10u] : discard;
            if (!bridge_encode_ds5_packet(ds5_encoder_pcm, destination)) {
                silence_ok = false;
                break;
            }
        }
        celt_encoder_ctl(ds5_encoder, OPUS_RESET_STATE);
        silent_frames_ready = silence_ok;
    }
    return bridge_encoder_available(requested_mode);
}

static uint32_t __not_in_flash_func(bridge_sync_encoder_generation)(void) {
    uint32_t const requested_generation = pipeline_reset_generation;
    if (codec_reset_generation != requested_generation) {
        if (speaker_encoder_mode == BRIDGE_ENCODER_DS5 && ds5_encoder)
            celt_encoder_ctl(ds5_encoder, OPUS_RESET_STATE);
        else if (speaker_encoder_mode == BRIDGE_ENCODER_PRO2 && pro2_encoder) {
            celt_encoder_ctl(pro2_encoder, OPUS_RESET_STATE);
            if (pro2_analysis) tonality_analysis_reset(pro2_analysis);
        }
        pro2_pcm_samples = 0;
        pro2_pcm_nonzero = false;
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
    queue_init(&pro2_encoded_frame_queue, sizeof(pro2_encoded_frame_t),
               ENCODED_FRAME_QUEUE_DEPTH);
    usb_speaker_active = false;
    usb_mic_active = false;
    bridge_connected = false;
    bridge_conn_index = 0xFF;
    pro2_bridge_active = false;
    pro2_idle_frames_pending = 0;
    producer_samples = 0;
    producer_pcm_block.nonzero = false;
    pro2_pcm_samples = 0;
    pro2_pcm_nonzero = false;
    transport_pair_count = 0;
    pro2_transport_valid = false;
    pipeline_reset_generation = 0;
    codec_reset_generation = 0;
    ds5_encoder_allocation = NULL;
    ds5_encoder = NULL;
    ds5_encoder_pcm = NULL;
    pro2_encoder_allocation = NULL;
    pro2_encoder = NULL;
    pro2_encoder_pcm = NULL;
    pro2_analysis = NULL;
    pro2_celt_mode = NULL;
    pro2_encoder_applied_complexity = UINT8_MAX;
    pro2_encoder_applied_analysis = false;
    speaker_encoder_mode = BRIDGE_ENCODER_NONE;
    silent_frames_ready = false;
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

    ds5_audio_pcm_capture_submit(data, len, nonzero);

    uint16_t source_samples = len / sizeof(int16_t);
    uint8_t const *source = data;
    while (source_samples != 0) {
        uint16_t const room = (uint16_t)(PCM_BLOCK_SAMPLES - producer_samples);
        uint16_t const copy_samples =
            source_samples < room ? source_samples : room;
        memcpy(producer_pcm_block.samples + producer_samples, source,
               copy_samples * sizeof(int16_t));
        producer_pcm_block.nonzero |= nonzero;
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
            producer_pcm_block.nonzero = false;
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
    // Codec construction and all encode calls stay on core1's explicit
    // platform-sized audio stack. Core0's normal SDK stack is much smaller.
    if (!bridge_prepare_encoder()) {
        diag_codec_no_encoder++;
        return;
    }
    uint32_t const requested_generation = bridge_sync_encoder_generation();
    bool const pro2_active = pro2_bridge_active;
    if (!bridge_connected && !pro2_active) {
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
        if (pro2_active)
            bridge_consume_pro2_block(&codec_pcm_block,
                                      requested_generation);
        else
            bridge_encode_input_frame(&codec_pcm_block,
                                      requested_generation);
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
        bool const pro2_active = pro2_bridge_active;
        if (!bridge_connected && !pro2_active) {
            diag_codec_disconnected++;
            continue;
        }
        if (!usb_speaker_active) {
            diag_codec_usb_inactive++;
            continue;
        }

        diag_codec_blocks_dequeued++;
        if (pro2_active)
            bridge_consume_pro2_block(&codec_pcm_block,
                                      requested_generation);
        else if (ds5_audio_bridge_ds5_exclusive_encode()) {
            // The CYW43 threadsafe-background worker normally preempts this
            // foreground CELT encode repeatedly. Its SDK lock makes the IRQ
            // defer work, then processes everything pending on release. Keep
            // this experiment strictly DualSense-only: the independently
            // validated Pro Controller 2 codec/transport path is untouched.
            async_context_t *const radio_context =
                cyw43_arch_async_context();
            async_context_acquire_lock_blocking(radio_context);
            bridge_encode_input_frame(&codec_pcm_block,
                                      requested_generation);
            async_context_release_lock(radio_context);
        } else {
            bridge_encode_input_frame(&codec_pcm_block,
                                      requested_generation);
        }
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

void ds5_audio_bridge_set_switch2_pro2_active(bool active) {
    if (pro2_bridge_active == active) return;
    if (active) ds5_audio_pcm_capture_stop();
    if (!active)
        __atomic_store_n(&pro2_idle_frames_pending, 0, __ATOMIC_RELEASE);
    pro2_bridge_active = active;
    // This setter is called from BTstack's background IRQ on core1. Draining
    // queues here can re-enter a queue lock held by the preempted codec worker.
    // Generation invalidation is lock-free; stale queued frames are discarded
    // lazily by their consumers and the codec core resets its own accumulator.
    pipeline_reset_generation++;
    pro2_transport_valid = false;
    ds5_audio_diag_note_pipeline_reset();
}

bool ds5_audio_bridge_switch2_pro2_active(void) {
    return pro2_bridge_active;
}

bool ds5_audio_bridge_usb_speaker_active(void) {
    return usb_speaker_active;
}

bool ds5_audio_bridge_peek_switch2_pro2_frame(
    uint8_t frame[SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN]) {
    if (!frame) return false;
    while (!pro2_transport_valid) {
        if (!queue_try_remove(&pro2_encoded_frame_queue,
                              &pro2_transport_frame)) return false;
        if (pro2_transport_frame.generation == pipeline_reset_generation)
            pro2_transport_valid = true;
    }
    memcpy(frame, pro2_transport_frame.data,
           SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN);
    return true;
}

void ds5_audio_bridge_commit_switch2_pro2_frame(void) {
    pro2_transport_valid = false;
}

void ds5_audio_bridge_note_switch2_pro2_idle_frame(void) {
    __atomic_fetch_add(&pro2_idle_frames_pending, 1, __ATOMIC_ACQ_REL);
}

bool ds5_audio_bridge_get_silent_pair(
    uint8_t frame_a[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN],
    uint8_t frame_b[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN]) {
    if (!silent_frames_ready || !frame_a || !frame_b) return false;
    memcpy(frame_a, silent_frames[0], DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    memcpy(frame_b, silent_frames[1], DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN);
    return true;
}

bool ds5_audio_bridge_speaker_requested(void) {
    return usb_speaker_active && bridge_connected;
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
void ds5_audio_bridge_set_switch2_pro2_active(bool active) { (void)active; }
bool ds5_audio_bridge_switch2_pro2_active(void) { return false; }
bool ds5_audio_bridge_usb_speaker_active(void) { return false; }
bool ds5_audio_bridge_peek_switch2_pro2_frame(
    uint8_t frame[SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN]) {
    (void)frame;
    return false;
}
void ds5_audio_bridge_commit_switch2_pro2_frame(void) {}
void ds5_audio_bridge_note_switch2_pro2_idle_frame(void) {}
bool ds5_audio_bridge_mic_active(void) { return false; }

#endif
