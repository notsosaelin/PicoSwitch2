#ifndef DS5_AUDIO_BRIDGE_H
#define DS5_AUDIO_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#define DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN 200u

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
void ds5_audio_bridge_codec_task(uint8_t conn_index);
bool ds5_audio_bridge_peek_speaker_pair(
    uint8_t frame_a[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN],
    uint8_t frame_b[DS5_AUDIO_BRIDGE_OPUS_FRAME_LEN]);
void ds5_audio_bridge_commit_speaker_pair(void);
bool ds5_audio_bridge_mic_active(void);
void ds5_audio_bridge_get_speaker_control(bool *muted, uint8_t *volume);

// --- Diagnostics ------------------------------------------------------------
// Instrumentation to localize the periodic DualSense-audio dropout. It answers
// one question: during an audio gap, does the BTstack core-1 run loop freeze
// (core1_* spikes toward the gap length) or does only report delivery stall
// (send_* spikes while core-1 keeps ticking at ~2 ms)? A core-1 freeze points
// at a blocking op (e.g. a flash write under multicore_lockout); a send-only
// stall points at the radio/L2CAP path. Fed from core 1; read in config mode
// via the "audiostat" CDC command. Always linkable; only fed in audio builds.
typedef struct {
    uint32_t core1_max_gap_us;      // worst interval between core-1 audio ticks
    uint32_t core1_gaps_over_10ms;  // count of core-1 ticks delayed >10 ms
    uint32_t send_max_gap_us;       // worst interval between 0x39 report sends
    uint32_t send_gaps_over_40ms;   // count of send intervals >40 ms (>2 reports)
    uint32_t sends_total;           // total 0x39 stream reports sent
} ds5_audio_diag_t;

void ds5_audio_diag_get(ds5_audio_diag_t *out);
void ds5_audio_diag_reset(void);
void ds5_audio_diag_note_core1_tick(uint32_t now_us);
void ds5_audio_diag_note_send(uint32_t now_us);

#endif  // DS5_AUDIO_BRIDGE_H
