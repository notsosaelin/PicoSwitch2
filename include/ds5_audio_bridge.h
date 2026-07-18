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

#endif  // DS5_AUDIO_BRIDGE_H
