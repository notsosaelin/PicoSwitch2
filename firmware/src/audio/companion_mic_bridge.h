// companion_mic_bridge.h - Companion microphone: MIC-UP packets to USB mic PCM.
//
// The firmware half of ns2_companion_mic.h. Owns the packet queue between the
// Bluetooth write handler and core 1's codec worker, the direct-CELT mono
// decoder, and the playout ring the USB microphone endpoint reads.
//
// Threads:
//   core 1, BTstack background IRQ  submit()
//   core 1, codec worker foreground service()   -- only when no speaker block waits
//   core 0, TinyUSB                 read_usb(), set_usb_active(), set_control()
//   any (management command path)   set_active()
//
// Builds without the live Opus codec (Pico W) compile stubs: unsupported,
// silence on the endpoint, every submit counted as inactive.
#ifndef COMPANION_MIC_BRIDGE_H
#define COMPANION_MIC_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#define COMPANION_MIC_BRIDGE_USB_PACKET_BYTES 192u  // 1 ms, 48 kHz stereo int16

void companion_mic_bridge_init(void);

// True on builds that can decode the microphone (Pico 2 W live-Opus builds).
bool companion_mic_bridge_supported(void);

// A microphone session starts or ends (ns2_companion_mic.h, SCOPE AND GATING).
// Starting begins a new session: decoder, sequence and ring start over.
void companion_mic_bridge_set_active(bool active);
bool companion_mic_bridge_active(void);

// The console opened (alt 1) or closed the USB microphone stream. Packets are
// decoded only while it is open; reopening starts the playout over.
void companion_mic_bridge_set_usb_active(bool active);
bool companion_mic_bridge_usb_active(void);

// UAC1 Feature Unit 0x05 (microphone) mute and volume, as the console set them.
void companion_mic_bridge_set_control(bool muted, int16_t volume_db_256);

// One validated MIC-UP packet (NS2_COMPANION_MIC_OPUS_BYTES, TOC first).
void companion_mic_bridge_submit(uint16_t sequence, const uint8_t *opus);

// Decode at most one queued packet (plus any concealment it implies). Returns
// true when it did work, so the worker checks for speaker PCM again before
// sleeping.
bool companion_mic_bridge_service(void);

// Fill one 1 ms USB microphone packet. Always the whole packet: silence while
// no session plays.
void companion_mic_bridge_read_usb(uint8_t packet[COMPANION_MIC_BRIDGE_USB_PACKET_BYTES]);

typedef struct {
    bool supported;
    bool active;
    bool usb_active;
    bool playing;
    uint8_t target_blocks;
    uint16_t gain_q15;
    uint32_t queued_frames;
    uint32_t received;          // packets accepted from the write handler
    uint32_t inactive;          // packets with no session, or the console not listening
    uint32_t queue_dropped;     // displaced in the packet queue: the worker fell behind
    uint32_t stale;             // duplicate or out-of-order sequence
    uint32_t lost;              // frames missing in the sequence
    uint32_t discontinuities;   // sequence jumps treated as a capture restart
    uint32_t decoded;
    uint32_t concealed;         // packet-loss-concealment blocks written
    uint32_t decode_errors;
    uint32_t decode_max_us;     // worst wall-clock decode, preemption included
    uint32_t underruns;         // the USB stream ran dry
    uint32_t ring_dropped;      // blocks shed at the headroom
    uint32_t extensions;        // near-underrun growths
    int32_t frames_adjusted;    // drift control, net frames added (+) / removed (-)
    uint32_t heap_free;         // bytes the allocator can still hand out
} companion_mic_bridge_diag_t;

void companion_mic_bridge_diag(companion_mic_bridge_diag_t *out);

#endif  // COMPANION_MIC_BRIDGE_H
