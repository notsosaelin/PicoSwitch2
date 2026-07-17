#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ds5_audio_bridge.h"

static void expect_control(bool expected_muted, uint8_t expected_volume) {
    bool muted = !expected_muted;
    uint8_t volume = 0;
    ds5_audio_bridge_get_speaker_control(&muted, &volume);
    assert(muted == expected_muted);
    assert(volume == expected_volume);
}

int main(void) {
    ds5_audio_bridge_init();
    expect_control(false, 100);

    ds5_audio_bridge_set_speaker_control(true, -20 * 256);
    expect_control(true, 80);

    ds5_audio_bridge_set_speaker_control(false, -60 * 256);
    expect_control(false, 40);

    // Clamp requests outside the UAC descriptor's advertised -60..0 dB range.
    ds5_audio_bridge_set_speaker_control(false, -80 * 256);
    expect_control(false, 40);
    ds5_audio_bridge_set_speaker_control(false, 10 * 256);
    expect_control(false, 100);

    // Either output pointer is independently optional for cross-core callers.
    bool muted = true;
    uint8_t volume = 0;
    ds5_audio_bridge_get_speaker_control(&muted, NULL);
    assert(!muted);
    ds5_audio_bridge_get_speaker_control(NULL, &volume);
    assert(volume == 100);

    puts("ds5_audio_control: all tests passed");
    return 0;
}
