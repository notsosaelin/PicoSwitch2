// Host tests for the companion audio downlink plane contract.
//
// Pins what a companion decoder depends on: the frame layout, the per-rate
// packet sizes, that only a whole session-sized CBR packet is ever framed, the
// admission order that decides which refusal reason the companion shows, the
// Controller Link gate and the reasons a running session ends, the
// controller-headset priority, and the headset advertisement that makes the
// console open its speaker stream at all.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ns2_companion_audio.h"
#include "controller_headset.h"

static void test_rates(void)
{
    // Quality floor: the console's own Pro Controller 2 headset rate, 96 kbit/s
    // CBR, 240 bytes per 20 ms. A maintainer requirement -- companion audio is
    // never cheaper than the genuine headset (ns2_companion_audio.h, BITRATE).
    assert(NS2_COMPANION_AUDIO_DEFAULT_KBPS == 96u);
    assert(ns2_companion_audio_opus_bytes(96) == 240u);
    assert(ns2_companion_audio_opus_bytes(96) == NS2_COMPANION_AUDIO_MAX_OPUS_BYTES);
    // Lower rates are not offered at all, so no setting can reintroduce them.
    assert(ns2_companion_audio_opus_bytes(64) == 0u);
    assert(ns2_companion_audio_opus_bytes(48) == 0u);
    assert(ns2_companion_audio_opus_bytes(0) == 0u);
    assert(ns2_companion_audio_opus_bytes(128) == 0u);
    assert(ns2_companion_audio_frame_bytes(64) == 0u);
    assert(ns2_companion_audio_min_att_mtu(64) == 0u);

    assert(ns2_companion_audio_frame_bytes(96) == 244u);
    assert(NS2_COMPANION_AUDIO_MAX_FRAME_BYTES == 244u);
    // One notification carries one frame: value <= MTU - 3.
    assert(ns2_companion_audio_min_att_mtu(96) == 247u);
}

static void test_encode_frame(void)
{
    // The framing itself is size-agnostic; the session size is what is checked.
    uint8_t opus[160];
    for (unsigned i = 0; i < sizeof(opus); i++) opus[i] = (uint8_t)(i * 7u + 1u);
    opus[0] = 0xFC;

    uint8_t out[NS2_COMPANION_AUDIO_MAX_FRAME_BYTES + 8u];
    memset(out, 0xEE, sizeof(out));
    uint16_t n = ns2_companion_audio_encode_frame(0xBEEF, opus, sizeof(opus), 160u,
                                                  out, sizeof(out));
    assert(n == 164u);
    assert(out[0] == NS2_COMPANION_AUDIO_VERSION);
    assert(out[1] == NS2_COMPANION_AUDIO_OP_OPUS);
    assert(out[2] == 0xEF && out[3] == 0xBE);  // little-endian sequence
    assert(out[4] == 0xFC);                     // TOC first: a public Opus packet
    assert(memcmp(&out[4], opus, sizeof(opus)) == 0);
    assert(out[164] == 0xEE);                   // nothing written past the frame

    // The validated 96 kbit/s packet still frames to 244 bytes.
    uint8_t big[NS2_COMPANION_AUDIO_MAX_OPUS_BYTES] = {0xFC};
    assert(ns2_companion_audio_encode_frame(1, big, sizeof(big), sizeof(big), out,
                                            sizeof(out)) == 244u);
}

static void test_encode_refuses_partial(void)
{
    uint8_t opus[NS2_COMPANION_AUDIO_MAX_OPUS_BYTES] = {0xFC};
    uint8_t out[NS2_COMPANION_AUDIO_MAX_FRAME_BYTES];
    // A short packet is a different packet to the decoder, never a short frame.
    assert(ns2_companion_audio_encode_frame(1, opus, 159u, 160u, out, sizeof(out)) == 0u);
    // A packet from another session's rate (stale queue entry) is refused too.
    assert(ns2_companion_audio_encode_frame(1, opus, 240u, 160u, out, sizeof(out)) == 0u);
    assert(ns2_companion_audio_encode_frame(1, opus, 160u, 160u, out, 163u) == 0u);
    assert(ns2_companion_audio_encode_frame(1, NULL, 160u, 160u, out, sizeof(out)) == 0u);
    assert(ns2_companion_audio_encode_frame(1, opus, 160u, 160u, NULL, sizeof(out)) == 0u);
    assert(ns2_companion_audio_encode_frame(1, opus, 0u, 0u, out, sizeof(out)) == 0u);
}

static ns2_companion_audio_controller_t link_only(void)
{
    ns2_companion_audio_controller_t c = {true, false, false, CONTROLLER_HEADSET_NONE};
    return c;
}

static ns2_companion_audio_admit_t admit_with(ns2_companion_audio_controller_t c)
{
    return ns2_companion_audio_admit(true, true, true, 96, 527, &c);
}

static void test_admission_order(void)
{
    const uint16_t mtu = ns2_companion_audio_min_att_mtu(96);
    const ns2_companion_audio_controller_t ok = link_only();
    assert(ns2_companion_audio_admit(true, true, true, 96, mtu, &ok) ==
           NS2_COMPANION_AUDIO_ADMIT_OK);
    // Pico W: no live encoder, and nothing else would change that answer.
    const ns2_companion_audio_controller_t worst = {false, true, true, CONTROLLER_HEADSET_HEADSET};
    assert(ns2_companion_audio_admit(false, false, false, 0, 0, &worst) ==
           NS2_COMPANION_AUDIO_ADMIT_UNSUPPORTED);
    assert(ns2_companion_audio_admit(true, false, false, 96, 0, &ok) ==
           NS2_COMPANION_AUDIO_ADMIT_NO_LINK);
    assert(ns2_companion_audio_admit(true, true, false, 96, mtu, &ok) ==
           NS2_COMPANION_AUDIO_ADMIT_UNTRUSTED);
    // A reduced rate is refused outright, never streamed.
    assert(ns2_companion_audio_admit(true, true, true, 64, 527, &ok) ==
           NS2_COMPANION_AUDIO_ADMIT_RATE);
    assert(ns2_companion_audio_admit(true, true, true, 96, mtu - 1u, &ok) ==
           NS2_COMPANION_AUDIO_ADMIT_MTU);
    // The Controller Link minimum is NOT enough for audio.
    assert(ns2_companion_audio_admit(true, true, true, 96, 33, &ok) ==
           NS2_COMPANION_AUDIO_ADMIT_MTU);
    assert(ns2_companion_audio_admit(true, true, true, 96, 527, NULL) ==
           NS2_COMPANION_AUDIO_ADMIT_NO_CONTROLLER_LINK);

    // SCOPE: the companion must BE the controller.
    ns2_companion_audio_controller_t c = link_only();
    c.controller_link = false;
    assert(admit_with(c) == NS2_COMPANION_AUDIO_ADMIT_NO_CONTROLLER_LINK);
    // Controller Link up but a DualSense is the source in use.
    c = link_only();
    c.other_controller = true;
    assert(admit_with(c) == NS2_COMPANION_AUDIO_ADMIT_OTHER_CONTROLLER);
    // No Controller Link outranks "another controller": the fix is to start it.
    c.controller_link = false;
    assert(admit_with(c) == NS2_COMPANION_AUDIO_ADMIT_NO_CONTROLLER_LINK);
    c = link_only();
    c.pro2_stream = true;
    assert(admit_with(c) == NS2_COMPANION_AUDIO_ADMIT_ENCODER_BUSY);
    c = link_only();
    c.source_headset = CONTROLLER_HEADSET_HEADPHONES;
    assert(admit_with(c) == NS2_COMPANION_AUDIO_ADMIT_CONTROLLER_HEADSET);

    assert(strcmp(ns2_companion_audio_admit_name(NS2_COMPANION_AUDIO_ADMIT_OK), "ok") == 0);
    assert(strcmp(ns2_companion_audio_admit_name(NS2_COMPANION_AUDIO_ADMIT_ENCODER_BUSY),
                  "encoder_busy") == 0);
    assert(strcmp(ns2_companion_audio_admit_name(NS2_COMPANION_AUDIO_ADMIT_CONTROLLER_HEADSET),
                  "controller_headset") == 0);
    assert(strcmp(ns2_companion_audio_admit_name(NS2_COMPANION_AUDIO_ADMIT_RATE), "rate") == 0);
    assert(strcmp(ns2_companion_audio_admit_name(NS2_COMPANION_AUDIO_ADMIT_NO_CONTROLLER_LINK),
                  "no_controller_link") == 0);
    assert(strcmp(ns2_companion_audio_admit_name(NS2_COMPANION_AUDIO_ADMIT_OTHER_CONTROLLER),
                  "other_controller") == 0);
}

static void test_end_reason(void)
{
    // Steady state: keep playing.
    ns2_companion_audio_controller_t c = link_only();
    assert(ns2_companion_audio_end_reason(&c) == NULL);
    // Source kind NONE (nothing has sent input yet / it left) is not "other".
    assert(ns2_companion_audio_end_reason(&c) == NULL);

    c.controller_link = false;
    assert(strcmp(ns2_companion_audio_end_reason(&c), "controller_link_ended") == 0);
    c = link_only();
    c.other_controller = true;
    assert(strcmp(ns2_companion_audio_end_reason(&c), "source_changed") == 0);
    c = link_only();
    c.pro2_stream = true;
    assert(strcmp(ns2_companion_audio_end_reason(&c), "preempted_pro2") == 0);
    c = link_only();
    c.source_headset = CONTROLLER_HEADSET_HEADSET;
    assert(strcmp(ns2_companion_audio_end_reason(&c), "preempted_controller_headset") == 0);
    // A headset on the new controller is the more specific reason: the
    // companion app tells the player the audio moved to that headset.
    c.other_controller = true;
    c.controller_link = false;
    assert(strcmp(ns2_companion_audio_end_reason(&c), "preempted_controller_headset") == 0);
    assert(strcmp(ns2_companion_audio_end_reason(NULL), "controller_link_ended") == 0);
}

static void test_controller_headset_priority(void)
{
    // A jack on the controller in use, with or without a microphone, wins.
    assert(!ns2_companion_audio_controller_headset(CONTROLLER_HEADSET_NONE));
    assert(ns2_companion_audio_controller_headset(CONTROLLER_HEADSET_HEADPHONES));
    assert(ns2_companion_audio_controller_headset(CONTROLLER_HEADSET_HEADSET));
}

static void test_headset_advertisement(void)
{
    // Inactive: the source's own jack state passes through untouched.
    assert(ns2_companion_audio_headset_state(CONTROLLER_HEADSET_NONE, false, false) == CONTROLLER_HEADSET_NONE);
    assert(ns2_companion_audio_headset_state(CONTROLLER_HEADSET_HEADSET, false, false) == CONTROLLER_HEADSET_HEADSET);
    // A microphone flag without a session advertises nothing.
    assert(ns2_companion_audio_headset_state(CONTROLLER_HEADSET_NONE, false, true) == CONTROLLER_HEADSET_NONE);
    // Active with no physical jack: headphones, never an invented microphone.
    assert(ns2_companion_audio_headset_state(CONTROLLER_HEADSET_NONE, true, false) == CONTROLLER_HEADSET_HEADPHONES);
    // ...and a headset with a microphone exactly while the companion's
    // microphone session is on.
    assert(ns2_companion_audio_headset_state(CONTROLLER_HEADSET_NONE, true, true) == CONTROLLER_HEADSET_HEADSET);
    // Active with a real jack: keep what the hardware says. (The session ends
    // on the next service tick, but the console must never see the jack drop
    // in between, or it would close the stream the controller is about to use.)
    assert(ns2_companion_audio_headset_state(CONTROLLER_HEADSET_HEADSET, true, false) == CONTROLLER_HEADSET_HEADSET);
    assert(ns2_companion_audio_headset_state(CONTROLLER_HEADSET_HEADPHONES, true, false) == CONTROLLER_HEADSET_HEADPHONES);
    assert(ns2_companion_audio_headset_state(CONTROLLER_HEADSET_HEADPHONES, true, true) == CONTROLLER_HEADSET_HEADPHONES);
}

static void test_mic_admission(void)
{
    assert(ns2_companion_audio_admit_mic(false, false) == NS2_COMPANION_AUDIO_ADMIT_OK);
    assert(ns2_companion_audio_admit_mic(false, true) == NS2_COMPANION_AUDIO_ADMIT_OK);
    assert(ns2_companion_audio_admit_mic(true, true) == NS2_COMPANION_AUDIO_ADMIT_OK);
    assert(ns2_companion_audio_admit_mic(true, false) == NS2_COMPANION_AUDIO_ADMIT_MIC_UNSUPPORTED);
    assert(strcmp(ns2_companion_audio_admit_name(NS2_COMPANION_AUDIO_ADMIT_MIC_UNSUPPORTED),
                  "mic_unsupported") == 0);
}

/*
 * The sequence numbers frames ENCODED: a frame shed while the link stalled is
 * a gap the receiver conceals, not an uncounted skip (ns2_companion_audio.h).
 */
static void test_sequence_counts_encoded_frames(void)
{
    // A session starts at 0 wherever the encoder's index is.
    assert(ns2_companion_audio_sequence(1000u, 1000u) == 0u);
    assert(ns2_companion_audio_sequence(1001u, 1000u) == 1u);

    // Frames 1002 and 1003 shed: the next frame sent shows a gap of two.
    const uint16_t before = ns2_companion_audio_sequence(1001u, 1000u);
    const uint16_t after = ns2_companion_audio_sequence(1004u, 1000u);
    assert((uint16_t)(after - before) - 1u == 2u);

    // 16-bit wrap is continuous, and so is the 32-bit index wrapping.
    assert(ns2_companion_audio_sequence(1000u + 65536u, 1000u) == 0u);
    assert(ns2_companion_audio_sequence(5u, 0xFFFFFFFEu) == 7u);
}

int main(void)
{
    test_sequence_counts_encoded_frames();
    test_rates();
    test_encode_frame();
    test_encode_refuses_partial();
    test_admission_order();
    test_end_reason();
    test_controller_headset_priority();
    test_headset_advertisement();
    test_mic_admission();
    printf("test_ns2_companion_audio: all tests passed\n");
    return 0;
}
