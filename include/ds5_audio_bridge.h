#ifndef DS5_AUDIO_BRIDGE_H
#define DS5_AUDIO_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#define DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN 200u
#define SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN 240u
#define SWITCH2_PRO2_AUDIO_CHUNK_LEN 120u
#if SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN != 2u * SWITCH2_PRO2_AUDIO_CHUNK_LEN
#error "Pro Controller 2 Opus frames must split into two equal GATT chunks"
#endif
#define DS5_AUDIO_PCM_CAPTURE_BYTES 4096u
#define DS5_AUDIO_PCM_CAPTURE_READ_MAX 256u

// Initialize before core1 launches. On builds without NS2_DS5_AUDIO these are
// cheap no-op stubs so Pico W retains the same USB behavior.
void ds5_audio_bridge_init(void);

// Core0 / TinyUSB producer API.
void ds5_audio_bridge_set_usb_streams(bool speaker_active, bool mic_active);
void ds5_audio_bridge_submit_speaker_pcm(const uint8_t *data, uint16_t len);
// UAC1 volume is signed 1/256 dB. The bridge converts the advertised
// -60..0 dB range into the DualSense's documented speaker-volume range.
void ds5_audio_bridge_set_speaker_control(bool muted, int16_t volume_db_256);

// Core1 / Bluetooth consumer API.
void ds5_audio_bridge_connect(uint8_t conn_index);
void ds5_audio_bridge_disconnect(uint8_t conn_index);
bool ds5_audio_bridge_owns_connection(uint8_t conn_index);
void ds5_audio_bridge_codec_task(void);
// Live-Opus builds run this non-returning worker from core1's foreground.
// BTstack/CYW43 remains on the same core in its SDK background IRQ context.
void ds5_audio_bridge_codec_worker(void);
bool ds5_audio_bridge_peek_speaker_pair(
    uint8_t frame_a[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN],
    uint8_t frame_b[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN]);
void ds5_audio_bridge_commit_speaker_pair(void);
#ifdef NS2_DS5_AUDIO_LIVE_OPUS
// Return two valid steady-state Opus-silence frames generated once on the
// codec core. Native haptic-only reports still require well-formed audio
// blocks, but must not consume or disturb queued console PCM.
bool ds5_audio_bridge_get_silent_pair(
    uint8_t frame_a[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN],
    uint8_t frame_b[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN]);
#endif
#ifdef NS2_DS5_AUDIO
// True once the USB host requests the speaker alternate setting. The Sony
// driver must reserve the combined report-0x39 audio/haptic path before the
// first packet: allowing legacy rumble to churn until a successful 0x39 can
// starve the very activation/stream reports needed to break that condition.
bool ds5_audio_bridge_speaker_requested(void);
#endif
bool ds5_audio_bridge_mic_active(void);
void ds5_audio_bridge_get_speaker_control(bool *muted, uint8_t *volume);

// The Pico 2 W has one shared USB PCM producer and one codec worker. Only one
// Bluetooth controller is admitted at a time, so the worker switches between
// independent direct-CELT configurations without retaining both states.
// DualSense encoding can be made interruptible again over UART for diagnostic
// A/B tests. Production keeps the CELT call exclusive so the CYW43 background
// worker cannot repeatedly preempt it; pending radio work runs on lock release.
void ds5_audio_bridge_set_ds5_exclusive_encode(bool enabled);
bool ds5_audio_bridge_ds5_exclusive_encode(void);
void ds5_audio_bridge_set_switch2_pro2_active(bool active);
bool ds5_audio_bridge_switch2_pro2_active(void);
void ds5_audio_bridge_set_switch2_pro2_complexity(uint8_t complexity);
uint8_t ds5_audio_bridge_switch2_pro2_complexity(void);
void ds5_audio_bridge_set_switch2_pro2_analysis(bool enabled);
bool ds5_audio_bridge_switch2_pro2_analysis(void);
bool ds5_audio_bridge_usb_speaker_active(void);
bool ds5_audio_bridge_peek_switch2_pro2_frame(
    uint8_t frame[SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN]);
void ds5_audio_bridge_commit_switch2_pro2_frame(void);
// Keep the stateful CELT encoder synchronized whenever transport substitutes
// the genuine fixed idle packet instead of consuming an encoded frame.
void ds5_audio_bridge_note_switch2_pro2_idle_frame(void);

// Live bridge instrumentation retained for regression and performance checks.
// "core1" measures the longest interval between known BTstack activity points
// (the audio timer and inbound HID report boundaries). "send" measures successful
// report-0x39 l2cap_send() calls, not merely queue admission.
typedef struct {
    uint32_t core1_max_gap_us;
    uint32_t core1_gaps_over_10ms;
    uint32_t send_max_gap_us;
    uint32_t send_gaps_over_40ms;
    uint32_t sends_total;
    uint32_t hci_complete_max_gap_us;
    uint32_t hci_complete_gaps_over_40ms;
    uint32_t hci_complete_events;
    uint32_t hci_completed_packets;
    uint32_t hci_complete_max_batch;
    uint32_t pcm_packets_total;
    uint32_t pcm_nonzero_packets;
    uint32_t pcm_short_packets;
    uint32_t pcm_dropped_packets;
    uint32_t pcm_max_gap_us;
    uint32_t pcm_gaps_over_2ms;
    uint32_t pcm_queue_max_depth;
    uint32_t opus_frames_total;
    uint32_t opus_encode_errors;
    uint32_t opus_max_gap_us;
    uint32_t opus_gaps_over_20ms;
    uint32_t opus_encode_max_us;
    uint32_t pipeline_resets;
    uint32_t codec_calls_total;
    uint32_t codec_no_encoder;
    uint32_t codec_disconnected;
    uint32_t codec_usb_inactive;
    uint32_t codec_no_pcm;
    uint32_t codec_blocks_dequeued;
    uint32_t codec_call_max_gap_us;
    uint32_t codec_call_gaps_over_10ms;
    uint32_t codec_gap_le_3ms;
    uint32_t codec_gap_le_7ms;
    uint32_t codec_gap_le_12ms;
    uint32_t codec_gap_le_25ms;
    uint32_t codec_gap_over_25ms;
    uint32_t usb_speaker_on_edges;
    uint32_t usb_speaker_off_edges;
    uint32_t usb_speaker_active_us;
    bool usb_speaker_active;
} ds5_audio_diag_t;

void ds5_audio_diag_get(ds5_audio_diag_t *out);
void ds5_audio_diag_reset(void);
void ds5_audio_diag_note_core1_activity(uint32_t now_us);
void ds5_audio_diag_note_l2cap_send(uint32_t now_us);
void ds5_audio_diag_note_hci_completion(uint32_t now_us,
                                        uint16_t completed_packets);
void ds5_audio_diag_note_usb_pcm(uint32_t now_us, bool nonzero,
                                 bool short_packet, bool dropped,
                                 uint32_t queue_depth);
void ds5_audio_diag_note_opus_frame(uint32_t now_us, uint32_t encode_us,
                                    bool success);
void ds5_audio_diag_note_pipeline_reset(void);

// UART-gated probe of the raw UAC1 speaker PCM received from the console.
// Capture waits for the first nonzero packet, then retains about 21.3 ms of
// 48-kHz stereo int16 PCM. Reading happens only after capture, so 115200-baud
// UART diagnostics never interfere with the live USB audio callback.
typedef struct {
    bool armed;
    bool complete;
    uint16_t captured_bytes;
    uint16_t capacity_bytes;
    uint16_t packets;
    uint32_t start_us;
    uint32_t end_us;
    uint32_t crc32;
    uint16_t peak_left;
    uint16_t peak_right;
    int64_t sum_left;
    int64_t sum_right;
    uint64_t sum_squares_left;
    uint64_t sum_squares_right;
} ds5_audio_pcm_capture_status_t;

void ds5_audio_pcm_capture_arm(void);
void ds5_audio_pcm_capture_stop(void);
void ds5_audio_pcm_capture_get_status(ds5_audio_pcm_capture_status_t *out);
uint16_t ds5_audio_pcm_capture_read(uint16_t offset, uint8_t *out,
                                    uint16_t max_len);

#endif  // DS5_AUDIO_BRIDGE_H
