#ifndef NS2_AMIIBO_V3_H
#define NS2_AMIIBO_V3_H

// NTAG I2C Plus 2K "figure v3" amiibo model (e.g. Kirby Air Riders).
//
// These tags are 2048 bytes and are a different chip from the 540-byte NTAG215
// all prior amiibo use. Format confirmed from xSke/pixl.js figure-v3 support;
// see docs/Amiibo-v3.md.
//
// This module is intentionally isolated from the validated 540/572 NTAG215
// virtual_amiibo store: it is a pure, host-testable data model for the v3 image
// (validation, identity, UID, GET_VERSION, bounded page/byte reads). Flash,
// transaction state, and the capture-derived write codec remain separate.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS2_AMIIBO_V3_SIZE 2048u
#define NS2_AMIIBO_V3_SRAM_OFFSET 0x3C0u
#define NS2_AMIIBO_V3_SRAM_SIZE 64u
#define NS2_AMIIBO_V3_SRAM_DATA_SIZE 62u
#define NS2_AMIIBO_V3_SIGNATURE_SIZE 32u
// Kirby uses sector-1 page 0 for its Air Riders capability state. Other
// figures can allocate the same state elsewhere (King Dedede uses page 0x64),
// so runtime code must derive the address from the sector-aware envelope or
// read descriptor. This constant names only the legacy/Kirby default.
#define NS2_AMIIBO_V3_SECTOR1_CAPABILITY_OFFSET 0x400u
#define NS2_AMIIBO_V3_SECTOR1_CAPABILITY_SIZE 4u
#define NS2_AMIIBO_V3_SECTOR_READ_PREFIX_SIZE 64u
#define NS2_AMIIBO_V3_SECTOR_READ_MAX_SIZE \
    (NS2_AMIIBO_V3_SECTOR_READ_PREFIX_SIZE + NS2_AMIIBO_V3_SIZE)

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

// Copy the complete 64-byte SRAM response stored at pages 0xF0..0xFF. The
// Switch 2 Pro Controller 2 publishes all 64 bytes after the 19-byte device
// result header; bytes 62..63 are the big-endian CRC-16/MCRF4XX over bytes
// 0..61. Do not replace the per-image CRC with a captured constant.
void ns2_amiibo_v3_sram_response(
    const uint8_t image[NS2_AMIIBO_V3_SIZE],
    uint8_t out[NS2_AMIIBO_V3_SRAM_SIZE]);

// Validate the SRAM response's transport CRC without making it a prerequisite
// for accepting the 2048-byte image. This remains separately queryable so old
// research dumps can still be inspected instead of being rejected at upload.
bool ns2_amiibo_v3_sram_response_valid(
    const uint8_t image[NS2_AMIIBO_V3_SIZE]);

// Bounded read of the flat image. Pages are 4 bytes; NTAG I2C addresses memory
// as sector*256 + page, so a linear offset = (sector*256 + page)*4. Reads that
// fall wholly within the 2048-byte image copy into out and return true;
// out-of-range reads copy nothing and return false.
bool ns2_amiibo_v3_read(const uint8_t image[NS2_AMIIBO_V3_SIZE],
                        size_t offset, uint8_t *out, size_t len);

// Build the result staged by controller NFC subcommand 0x1E. Its request is a
// sector-aware descriptor:
//   timeout:u16le | uid[7] | tag_type:u8 | count:u8 |
//   (sector:u8,start_page:u8,end_page:u8)[count] | reserved[6]
//
// A genuine Pro Controller 2 publishes a 64-byte identity/signature/descriptor
// prefix followed by the requested pages, reports state 0x15, and lets the
// console retrieve this buffer through ordinary 0x15 chunks. The first page of
// the Air Riders sector-1 range is chip-managed capability state. Its page is
// allocation-relative: Kirby uses page 0, while King Dedede uses page 0x64.
// Genuine captures proved that it advances from A5 00 01 00 to A5 00 02 00
// even though the explicit record starts at the following page. Virtual writes
// retain it at the descriptor-selected location so Sync/export/re-import keeps
// a coherent complete image. Ecosystem source dumps leave the slot zero; the
// builder supplies the hardware-confirmed A5 00 01 00 first-use fallback.
bool ns2_amiibo_v3_build_sector_read_result(
    const uint8_t image[NS2_AMIIBO_V3_SIZE],
    const uint8_t signature[NS2_AMIIBO_V3_SIGNATURE_SIZE],
    const uint8_t *request, size_t request_size,
    uint8_t *out, size_t out_capacity, size_t *out_size);

#endif /* NS2_AMIIBO_V3_H */
