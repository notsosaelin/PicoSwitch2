#ifndef NS2_AMIIBO_V3_H
#define NS2_AMIIBO_V3_H

// NTAG I2C Plus 2K "figure v3" amiibo model (e.g. Kirby Air Riders).
//
// These tags are 2048 bytes and are a different chip from the 540-byte NTAG215
// all prior amiibo use. Format confirmed from xSke/pixl.js figure-v3 support;
// see docs/switch2/kirby-air-riders-extended-amiibo.md.
//
// This module is intentionally isolated from the validated 540/572 NTAG215
// virtual_amiibo store: it is a pure, host-testable data model for the v3 image
// (validation, identity, UID, GET_VERSION, bounded page/byte reads). It does not
// touch flash or the existing NFC serve path. Wiring it into the console serve
// path is a later, hardware-gated phase.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS2_AMIIBO_V3_SIZE 2048u

// True if bytes look like a v3 NTAG I2C 2K amiibo: exact size, NXP UID prefix,
// and the contiguous-UID internal bytes (data[7]=0x00, data[8]=0x44) this chip
// uses in place of the NTAG215 BCC0/BCC1 interleave.
bool ns2_amiibo_v3_valid(const uint8_t *bytes, size_t len);

// 7-byte contiguous UID (bytes 0..6 — no BCC interleave).
void ns2_amiibo_v3_uid(const uint8_t image[NS2_AMIIBO_V3_SIZE], uint8_t uid[7]);

// 8-byte amiibo identity block at offset 0x54 (rider/character id).
void ns2_amiibo_v3_identity(const uint8_t image[NS2_AMIIBO_V3_SIZE],
                            uint8_t id[8]);

// GET_VERSION (0x60) reply the console reads to detect the NTAG I2C 2K chip:
// {0x00,0x04,0x04,0x05,0x02,0x02,0x15,0x03}. Copies 8 bytes into out.
void ns2_amiibo_v3_version(uint8_t out[8]);

// Bounded read of the flat image. Pages are 4 bytes; NTAG I2C addresses memory
// as sector*256 + page, so a linear offset = (sector*256 + page)*4. Reads that
// fall wholly within the 2048-byte image copy into out and return true;
// out-of-range reads copy nothing and return false.
bool ns2_amiibo_v3_read(const uint8_t image[NS2_AMIIBO_V3_SIZE],
                        size_t offset, uint8_t *out, size_t len);

#endif /* NS2_AMIIBO_V3_H */
