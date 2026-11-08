// ns2_companion_audio.h - Companion audio downlink plane (EXPERIMENTAL).
//
// WHAT THIS IS
//
// A third plane on the trusted management BLE connection, beside the JSON
// command channel and the Controller Link pair (ns2_companion_link.h). It
// carries the console's USB speaker audio to a companion -- today the Windows
// app -- so the player can listen on the PC's own output while playing.
//
//     console UAC1 speaker PCM (48 kHz stereo int16, switch_pro2.c)
//         -> shared PCM producer (ds5_audio_bridge.c)
//         -> the EXISTING Pro Controller 2 direct-CELT encoder
//            (CBR, 20 ms, TOC 0xFC; packet size set per session, below)
//         -> this frame
//         -> GATT notification on the management service
//         -> companion: standard Opus decode, local playback
//
// BITRATE: 96 kbit/s, THE CONSOLE'S OWN HEADSET RATE -- A FLOOR, NOT A KNOB
//
// The console streams 96 kbit/s CBR (240-byte packets, 20 ms) to a genuine
// Pro Controller 2 headset (docs/switch2/pro2-headset-audio.md). Companion
// audio is held to that quality. It is a maintainer requirement (2026-11-07):
// do NOT lower the bitrate to buy radio airtime.
// A 64 kbit/s default was tried for one build to ease contention with a
// Bluetooth Classic DualSense and rejected for exactly that reason. Transport
// problems are to be fixed in the transport, scheduling and buffering, never
// by spending audio quality.
//
// The frame size is nonetheless reported, not assumed: `caudio status` carries
// `kbps` and `frame_bytes`, and the companion sizes its receiver from them, so
// a future higher rate needs no companion change. The packet size of a session
// is set by capping the CELT output buffer: CBR emits exactly
// min(buffer, bitrate bytes) (celt_encoder.c, the `vbr==0` branch).
//
// The packet is a valid public Opus packet: TOC 0xFC is config 31 (CELT-only,
// full band, 20 ms), stereo, code 0 (one frame). docs/switch2/pro2-headset-audio.md
// records that a standard decoder decodes it with zero failures. The companion
// therefore needs no knowledge of the firmware's direct-CELT internals.
//
// SCOPE: COMPANION AUDIO IS PART OF CONTROLLER LINK
//
// Companion audio exists so a companion that IS the controller -- an Android
// handheld or a PC driving Controller Link -- is a complete Switch 2
// controller, headset included. It is therefore admitted only while a companion
// controller session exists (Controller Link active on this management link,
// or an Android HID bridge connection), and it ends when that session ends or
// another controller becomes the active source (`controller_link_ended`,
// `source_changed`). Streaming the console's audio to an app while an
// unrelated controller is in use is deliberately out of scope (maintainer
// decision, 2026-11-07): it puts a second controller's radio traffic beside
// the audio plane, which is where the second and third hardware passes lost audio.
//
// EXCLUSIVITY: A CONTROLLER'S OWN HEADSET WINS
//
// The Pico 2 W has one USB PCM producer and one live encoder state
// (ds5_audio_bridge.h), so companion audio is exclusive with controller audio.
// A headset physically plugged into the controller in use is the strongest
// statement of where the player wants sound, so it always wins:
//   - Start is refused (`controller_headset`) while the active source reports
//     an occupied jack, and (`encoder_busy`) while a genuine Pro Controller 2
//     headset stream owns the encoder.
//   - A jack occupied mid-session ends the session
//     (`preempted_controller_headset`, or `preempted_pro2`). The console keeps
//     its speaker stream open (the jack is still advertised) and the audio
//     moves to that controller's own headset path.
// While companion audio is active the DualSense speaker path is suppressed so
// the console's audio goes to the companion. Pico W has no live encoder and
// refuses at Start with a reason.
#ifndef NS2_COMPANION_AUDIO_H
#define NS2_COMPANION_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// WIRE CONTRACT
// ============================================================================

// Plane version. Independent of the Controller Link version: the planes are
// negotiated and versioned separately (research section 19, option A).
#define NS2_COMPANION_AUDIO_VERSION 1u

// Adapter -> companion: one complete Opus packet.
#define NS2_COMPANION_AUDIO_OP_OPUS 0x90u

// Frame:
//   [0]    version
//   [1]    opcode
//   [2..3] sequence, little-endian, one per 20 ms frame ENCODED, free to wrap
//   [4..]  one complete Opus packet, TOC byte first
//
// The sequence lets the companion count frames the adapter dropped or the
// radio lost (a gap) without any acknowledgement traffic. It is a diagnostic
// and concealment hint, not a reliability mechanism: audio is never resent.
//
// It numbers frames as ENCODED, not as sent (ns2_companion_audio_sequence).
// While connection events are missed the adapter holds at most five frames
// (three in the controller's ACL buffers, two in the encoder queue, which
// drops its oldest). Numbered per frame sent, a frame shed there left no gap:
// the receiver could not conceal it, the audio skipped ahead uncounted, and
// the playout lost 20 ms of depth that its drift control returns only at
// about 1 ms/s. Numbered per frame encoded, the receiver conceals it. In the
// pipeline simulation with the hold bounded, this cut Stress underruns from
// 41 to 13 an hour over three seeds and changed nothing on a clean link
// (CompanionAudioPipelineSimulationTests). Do not go back to counting sends.
//
// Every frame of one session has the same length; `caudio status` reports it
// as frame_bytes. The packet is CBR, so its length IS the bitrate.
#define NS2_COMPANION_AUDIO_HEADER_BYTES 4u

// The largest packet: the Pro Controller 2 operating point, 96 kbit/s.
#define NS2_COMPANION_AUDIO_MAX_OPUS_BYTES 240u
#define NS2_COMPANION_AUDIO_MAX_FRAME_BYTES \
    (NS2_COMPANION_AUDIO_HEADER_BYTES + NS2_COMPANION_AUDIO_MAX_OPUS_BYTES)

// Session rate (see BITRATE above): the genuine Pro Controller 2 headset rate.
#define NS2_COMPANION_AUDIO_DEFAULT_KBPS 96u

// Frame geometry the companion decoder needs: 48 kHz stereo, 960 samples per
// channel per packet. Reported over the control plane, not assumed.
#define NS2_COMPANION_AUDIO_SAMPLE_RATE 48000u
#define NS2_COMPANION_AUDIO_CHANNELS 2u
#define NS2_COMPANION_AUDIO_FRAME_MS 20u

// ============================================================================
// PURE LOGIC (host-testable, no BTstack, no hardware)
// ============================================================================

// Opus packet bytes per 20 ms frame at `kbps`, or 0 for a rate this plane does
// not offer. Only 96 kbit/s is offered (BITRATE above): nothing below the
// console's own headset quality, and nothing above the encoder's validated
// frame slot.
uint16_t ns2_companion_audio_opus_bytes(uint8_t kbps);

// Header plus packet: one notification's value. 0 for an unoffered rate.
uint16_t ns2_companion_audio_frame_bytes(uint8_t kbps);

// One audio frame must fit ONE notification (value <= ATT_MTU - 3). Refuse at
// Start below this rather than fragment: a GATT notification has no
// reassembly, so a truncated notification would be an undecodable packet.
uint16_t ns2_companion_audio_min_att_mtu(uint8_t kbps);

typedef enum {
    NS2_COMPANION_AUDIO_ADMIT_OK = 0,
    NS2_COMPANION_AUDIO_ADMIT_UNSUPPORTED,   // build has no live encoder (Pico W)
    NS2_COMPANION_AUDIO_ADMIT_NO_LINK,       // no management connection
    NS2_COMPANION_AUDIO_ADMIT_UNTRUSTED,     // link not bonded/encrypted/allowlisted
    NS2_COMPANION_AUDIO_ADMIT_MTU,           // negotiated MTU cannot carry a frame
    NS2_COMPANION_AUDIO_ADMIT_ENCODER_BUSY,  // Pro Controller 2 headset stream owns the encoder
    NS2_COMPANION_AUDIO_ADMIT_CONTROLLER_HEADSET,  // controller in use has a jack occupied
    NS2_COMPANION_AUDIO_ADMIT_RATE,          // selected bitrate is not one this plane offers
    NS2_COMPANION_AUDIO_ADMIT_NO_CONTROLLER_LINK,  // the companion is not the controller
    NS2_COMPANION_AUDIO_ADMIT_OTHER_CONTROLLER,    // another controller is the active source
    NS2_COMPANION_AUDIO_ADMIT_MIC_UNSUPPORTED,     // microphone asked for, build cannot decode it
} ns2_companion_audio_admit_t;

// What the plane needs to know about the controller side, gathered by the
// caller. Pure inputs so admission and session-end are host-testable.
typedef struct {
    bool controller_link;     // a companion controller session exists (SCOPE)
    bool other_controller;    // the active input source is NOT the companion
    bool pro2_stream;         // a genuine Pro Controller 2 headset stream owns the encoder
    uint8_t source_headset;   // CONTROLLER_HEADSET_* of the active source
} ns2_companion_audio_controller_t;

// Decide whether `caudio start` may arm the plane at `kbps`. Ordered so the
// reported reason is the most fundamental one: a Pico W says "unsupported" even
// with no link, because no link would ever change that answer.
ns2_companion_audio_admit_t ns2_companion_audio_admit(
    bool build_supported, bool have_link, bool link_trusted, uint8_t kbps,
    uint16_t att_mtu, const ns2_companion_audio_controller_t *controller);

// The microphone half of admission (ns2_companion_mic.h), checked after the
// speaker half passed: OK when no microphone was asked for or this build can
// decode one, MIC_UNSUPPORTED otherwise. Refused whole rather than started
// without the microphone, so the companion never believes it is heard when it
// is not.
ns2_companion_audio_admit_t ns2_companion_audio_admit_mic(bool mic_requested,
                                                          bool mic_supported);

// Why a running session must end now, as the static `last_stop` token, or
// NULL to keep playing. Checked on every service tick (2 ms).
const char *ns2_companion_audio_end_reason(
    const ns2_companion_audio_controller_t *controller);

// Stable lowercase token for an admission result, for the JSON status reply.
const char *ns2_companion_audio_admit_name(ns2_companion_audio_admit_t result);

// Build one frame. Returns the total length, or 0 when the packet is not
// exactly the session's CBR packet size or does not fit -- refusing rather than
// truncating, because a short Opus packet decodes as a different, wrong packet.
// The wire sequence of an encoded frame: its index counted from the
// session's first frame, so the session starts at 0 and every frame encoded
// and not sent -- shed while the link stalled, failed to encode, or refused
// by the stack -- is a gap. Wraps at 16 bits by design.
uint16_t ns2_companion_audio_sequence(uint32_t frame_index,
                                      uint32_t session_first_index);

uint16_t ns2_companion_audio_encode_frame(uint16_t sequence,
                                          const uint8_t *opus,
                                          uint16_t opus_len,
                                          uint16_t session_opus_bytes,
                                          uint8_t *out,
                                          uint16_t out_capacity);

// Whether a session must end (or may not start) because the controller in use
// has its own headset: an occupied jack on the active source. Only sources
// with a real audio path (DualSense, Pro Controller 2) report a jack.
bool ns2_companion_audio_controller_headset(uint8_t source_state);

// The jack state the console must see while the plane is active.
//
// The Switch only opens its USB speaker stream to a Pro Controller 2 that
// reports an occupied headphone jack (switch_pro2.c, report 0x09 byte 0x0D).
// While companion audio is active the adapter must therefore advertise one even
// though no physical controller has a jack occupied. A source that already
// reports a jack keeps its own state (a headset with microphone stays a
// headset). Otherwise the companion's own accessory is advertised: a headset
// WITH a microphone while its microphone session is on (ns2_companion_mic.h),
// headphones otherwise -- never a microphone that does not exist.
uint8_t ns2_companion_audio_headset_state(uint8_t source_state,
                                          bool companion_audio_active,
                                          bool companion_mic_active);

#endif  // NS2_COMPANION_AUDIO_H
