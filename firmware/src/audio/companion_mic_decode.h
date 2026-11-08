// companion_mic_decode.h - The companion microphone's Opus decode, alone.
//
// One MIC-UP packet (ns2_companion_mic.h: CELT-only, full band, mono, 20 ms,
// TOC 0xF8) to 960 int16 samples, with the CELT decoder underneath libopus's
// public one. Separate from companion_mic_bridge.c, and free of SDK calls, so
// the host tests decode real companion-encoded packets through exactly this
// code (tests/firmware/test_companion_mic_decode.c).
#ifndef COMPANION_MIC_DECODE_H
#define COMPANION_MIC_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bytes of decoder state needed, or 0 if the codec cannot be configured.
size_t companion_mic_decoder_size(void);

// Initialize (or reset) a decoder in `storage` (companion_mic_decoder_size()
// bytes, word-aligned). Returns the decoder handle, or NULL.
void *companion_mic_decoder_init(void *storage, size_t storage_bytes);

// Decode one packet (`opus`, NS2_COMPANION_MIC_OPUS_BYTES, TOC first) into
// `pcm` (NS2_COMPANION_MIC_BLOCK_FRAMES samples), or conceal one lost packet
// when `opus` is NULL. Returns false on a decode error (pcm untouched).
bool companion_mic_decode(void *decoder, const uint8_t *opus, int16_t *pcm);

#endif  // COMPANION_MIC_DECODE_H
