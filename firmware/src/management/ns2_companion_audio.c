#include "ns2_companion_audio.h"

#include <string.h>

#include "controller_headset.h"
#include "ds5_audio_bridge.h"

// The largest packet is the encoder's own buffer: the companion path writes
// into the Pro Controller 2 frame slot, capped below it.
_Static_assert(NS2_COMPANION_AUDIO_MAX_OPUS_BYTES == SWITCH2_PRO2_AUDIO_OPUS_FRAME_LEN,
               "companion audio packets must fit the Pro2 encoder frame slot");

uint16_t ns2_companion_audio_opus_bytes(uint8_t kbps)
{
    switch (kbps) {
        case 96u:
            // kbit/s * 20 ms / 8 bits: 240 bytes, the Pro Controller 2 packet.
            return (uint16_t)((uint16_t)kbps * NS2_COMPANION_AUDIO_FRAME_MS / 8u);
        default:
            return 0u;
    }
}

uint16_t ns2_companion_audio_frame_bytes(uint8_t kbps)
{
    const uint16_t opus = ns2_companion_audio_opus_bytes(kbps);
    return opus ? (uint16_t)(NS2_COMPANION_AUDIO_HEADER_BYTES + opus) : 0u;
}

uint16_t ns2_companion_audio_min_att_mtu(uint8_t kbps)
{
    const uint16_t frame = ns2_companion_audio_frame_bytes(kbps);
    return frame ? (uint16_t)(frame + 3u) : 0u;
}

ns2_companion_audio_admit_t ns2_companion_audio_admit(
    bool build_supported, bool have_link, bool link_trusted, uint8_t kbps,
    uint16_t att_mtu, const ns2_companion_audio_controller_t *controller)
{
    if (!build_supported) return NS2_COMPANION_AUDIO_ADMIT_UNSUPPORTED;
    if (!have_link) return NS2_COMPANION_AUDIO_ADMIT_NO_LINK;
    if (!link_trusted) return NS2_COMPANION_AUDIO_ADMIT_UNTRUSTED;
    const uint16_t min_mtu = ns2_companion_audio_min_att_mtu(kbps);
    if (min_mtu == 0u) return NS2_COMPANION_AUDIO_ADMIT_RATE;
    if (att_mtu < min_mtu) return NS2_COMPANION_AUDIO_ADMIT_MTU;
    if (!controller || !controller->controller_link)
        return NS2_COMPANION_AUDIO_ADMIT_NO_CONTROLLER_LINK;
    if (controller->other_controller) return NS2_COMPANION_AUDIO_ADMIT_OTHER_CONTROLLER;
    if (controller->pro2_stream) return NS2_COMPANION_AUDIO_ADMIT_ENCODER_BUSY;
    if (ns2_companion_audio_controller_headset(controller->source_headset))
        return NS2_COMPANION_AUDIO_ADMIT_CONTROLLER_HEADSET;
    return NS2_COMPANION_AUDIO_ADMIT_OK;
}

const char *ns2_companion_audio_end_reason(
    const ns2_companion_audio_controller_t *controller)
{
    if (!controller) return "controller_link_ended";
    // Most specific first: each names the thing the player did.
    if (controller->pro2_stream) return "preempted_pro2";
    if (ns2_companion_audio_controller_headset(controller->source_headset))
        return "preempted_controller_headset";
    if (!controller->controller_link) return "controller_link_ended";
    if (controller->other_controller) return "source_changed";
    return NULL;
}

const char *ns2_companion_audio_admit_name(ns2_companion_audio_admit_t result)
{
    switch (result) {
        case NS2_COMPANION_AUDIO_ADMIT_OK:           return "ok";
        case NS2_COMPANION_AUDIO_ADMIT_UNSUPPORTED:  return "unsupported";
        case NS2_COMPANION_AUDIO_ADMIT_NO_LINK:      return "no_link";
        case NS2_COMPANION_AUDIO_ADMIT_UNTRUSTED:    return "untrusted";
        case NS2_COMPANION_AUDIO_ADMIT_MTU:          return "mtu";
        case NS2_COMPANION_AUDIO_ADMIT_ENCODER_BUSY: return "encoder_busy";
        case NS2_COMPANION_AUDIO_ADMIT_CONTROLLER_HEADSET: return "controller_headset";
        case NS2_COMPANION_AUDIO_ADMIT_RATE:         return "rate";
        case NS2_COMPANION_AUDIO_ADMIT_NO_CONTROLLER_LINK: return "no_controller_link";
        case NS2_COMPANION_AUDIO_ADMIT_OTHER_CONTROLLER: return "other_controller";
        case NS2_COMPANION_AUDIO_ADMIT_MIC_UNSUPPORTED: return "mic_unsupported";
    }
    return "unknown";
}

uint16_t ns2_companion_audio_sequence(uint32_t frame_index,
                                      uint32_t session_first_index)
{
    return (uint16_t)(frame_index - session_first_index);
}

uint16_t ns2_companion_audio_encode_frame(uint16_t sequence,
                                          const uint8_t *opus,
                                          uint16_t opus_len,
                                          uint16_t session_opus_bytes,
                                          uint8_t *out,
                                          uint16_t out_capacity)
{
    if (!opus || !out) return 0u;
    if (opus_len == 0u || opus_len != session_opus_bytes) return 0u;
    if (opus_len > NS2_COMPANION_AUDIO_MAX_OPUS_BYTES) return 0u;
    const uint16_t total = (uint16_t)(NS2_COMPANION_AUDIO_HEADER_BYTES + opus_len);
    if (out_capacity < total) return 0u;
    out[0] = (uint8_t)NS2_COMPANION_AUDIO_VERSION;
    out[1] = (uint8_t)NS2_COMPANION_AUDIO_OP_OPUS;
    out[2] = (uint8_t)(sequence & 0xFFu);
    out[3] = (uint8_t)(sequence >> 8);
    memcpy(&out[NS2_COMPANION_AUDIO_HEADER_BYTES], opus, opus_len);
    return total;
}

bool ns2_companion_audio_controller_headset(uint8_t source_state)
{
    return source_state != CONTROLLER_HEADSET_NONE;
}

uint8_t ns2_companion_audio_headset_state(uint8_t source_state,
                                          bool companion_audio_active,
                                          bool companion_mic_active)
{
    if (!companion_audio_active) return source_state;
    if (source_state != CONTROLLER_HEADSET_NONE) return source_state;
    return companion_mic_active ? CONTROLLER_HEADSET_HEADSET
                                : CONTROLLER_HEADSET_HEADPHONES;
}

ns2_companion_audio_admit_t ns2_companion_audio_admit_mic(bool mic_requested,
                                                          bool mic_supported)
{
    if (mic_requested && !mic_supported)
        return NS2_COMPANION_AUDIO_ADMIT_MIC_UNSUPPORTED;
    return NS2_COMPANION_AUDIO_ADMIT_OK;
}
