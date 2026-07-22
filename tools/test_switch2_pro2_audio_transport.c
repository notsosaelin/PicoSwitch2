#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "switch2_pro2_audio_transport.h"

static void test_compacts_headset_report(void)
{
    uint8_t src[SW2_PRO2_AUDIO_INPUT_REPORT_LEN] = {0};
    uint8_t dst[SW2_PRO2_AUDIO_COMPACT_LEN];
    src[0] = 0x42;
    src[0x0D] = 0x0F;
    src[0x0E] = SW2_PRO2_AUDIO_DATA_LEN;
    memset(&src[0x0F], 0xA5, SW2_PRO2_AUDIO_DATA_LEN);
    src[0x41] = 0x28;
    for (unsigned i = 0; i < 0x28; i++) src[0x42 + i] = (uint8_t)(i + 1);

    assert(switch2_pro2_audio_compact_input(src, sizeof(src), dst));
    assert(dst[0] == 0x42);
    assert(dst[0x0D] == 0x0F);
    assert(dst[0x0E] == 0x28);
    assert(memcmp(&dst[0x0F], &src[0x42], 0x28) == 0);
}

static void test_compacts_report_without_audio_stream(void)
{
    uint8_t src[SW2_PRO2_AUDIO_INPUT_REPORT_LEN] = {0};
    uint8_t dst[SW2_PRO2_AUDIO_COMPACT_LEN];
    src[0] = 0x84;
    src[0x0E] = 0;
    src[0x41] = 0x1E;
    for (unsigned i = 0; i < 0x1E; i++) src[0x42 + i] = (uint8_t)(0x80u + i);

    assert(switch2_pro2_audio_compact_input(src, sizeof(src), dst));
    assert(dst[0] == 0x84);
    assert(dst[0x0E] == 0x1E);
    assert(memcmp(&dst[0x0F], &src[0x42], 0x1E) == 0);
}

static void test_rejects_malformed_report(void)
{
    uint8_t src[SW2_PRO2_AUDIO_INPUT_REPORT_LEN] = {0};
    uint8_t dst[SW2_PRO2_AUDIO_COMPACT_LEN];
    src[0x0E] = SW2_PRO2_AUDIO_DATA_LEN;
    src[0x41] = 0x28;
    assert(!switch2_pro2_audio_compact_input(src, sizeof(src) - 1, dst));
    src[0x0E] = 0x31;
    assert(!switch2_pro2_audio_compact_input(src, sizeof(src), dst));
    src[0x0E] = SW2_PRO2_AUDIO_DATA_LEN;
    src[0x41] = 0x20;
    assert(!switch2_pro2_audio_compact_input(src, sizeof(src), dst));
}

static void test_headset_state(void)
{
    assert(switch2_pro2_audio_headset_state(0x00) == CONTROLLER_HEADSET_NONE);
    assert(switch2_pro2_audio_headset_state(0x04) == CONTROLLER_HEADSET_NONE);
    assert(switch2_pro2_audio_headset_state(0x05) == CONTROLLER_HEADSET_HEADPHONES);
    assert(switch2_pro2_audio_headset_state(0x0D) == CONTROLLER_HEADSET_HEADPHONES);
    assert(switch2_pro2_audio_headset_state(0x07) == CONTROLLER_HEADSET_HEADSET);
    assert(switch2_pro2_audio_headset_state(0x0F) == CONTROLLER_HEADSET_HEADSET);
}

static void test_input_fallback_gate(void)
{
    assert(switch2_pro2_audio_needs_input_fallback(100000, 0));
    assert(!switch2_pro2_audio_needs_input_fallback(100000, 50001));
    assert(switch2_pro2_audio_needs_input_fallback(100000, 50000));
    assert(!switch2_pro2_audio_needs_input_fallback(0x20u, 0xFFFFFFF0u));
    assert(switch2_pro2_audio_needs_input_fallback(0x10000u, 0xFFFF0000u));
}

int main(void)
{
    test_compacts_headset_report();
    test_compacts_report_without_audio_stream();
    test_rejects_malformed_report();
    test_headset_state();
    test_input_fallback_gate();
    puts("switch2_pro2_audio_transport: all tests passed");
    return 0;
}
