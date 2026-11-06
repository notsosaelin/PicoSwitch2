#ifndef NS2_AMIIBO_V3_WRITE_H
#define NS2_AMIIBO_V3_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns2_amiibo_v3.h"
#include "ns2_virtual_nfc.h"

// The v3 console-write envelope is the same 454-byte staged format used by
// NTAG215 writes. The captured records cover pages 0x05 through 0x91 and end
// exactly at byte 0x248; bytes beyond that point (including the machine SRAM
// window at 0x3C0) are not part of this transaction.
#define NS2_AMIIBO_V3_WRITE_END 0x248u
#define NS2_AMIIBO_V3_DEVICE_COMMAND_SIZE 74u
#define NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE 355u
#define NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE 167u
#define NS2_AMIIBO_V3_EXTENDED_MAX_SIZE \
    NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE
#define NS2_AMIIBO_V3_EXTENDED_SEQUENCE_TIMEOUT_MS 5000u

typedef enum {
    NS2_AMIIBO_V3_EXTENDED_SEQUENCE_IDLE = 0,
    NS2_AMIIBO_V3_EXTENDED_SEQUENCE_AWAIT_UPDATE,
    NS2_AMIIBO_V3_EXTENDED_SEQUENCE_UPDATE_COMMITTED,
} ns2_amiibo_v3_extended_sequence_phase_t;

typedef struct {
    ns2_amiibo_v3_extended_sequence_phase_t phase;
    uint32_t deadline_ms;
} ns2_amiibo_v3_extended_sequence_t;

// Classify the observed offset-zero command-0x14 bodies. Bytes 0..1 are a
// little-endian timeout, not a fixed marker; identity begins with the UID at
// byte 2. The device command carries operation 01 and is executed by 0x21.
// Normal mutable data has a nonzero record count and is committed by 0x08.
// The separately framed sector-aware operations carry their record count at
// byte 22 and are completed by 0x20. The genuine Air Riders trace contains:
//   355 B: clear sector-0 pages 0x92-0xE1
//   167 B: update page 0x04 plus one self-described 32-byte sector-0
//          allocation and one self-described 96-byte sector-1 allocation.
//          Kirby uses pages 0x92 and 0x01; King Dedede uses 0xB2 and 0x65.
bool ns2_amiibo_v3_is_device_command(
    const uint8_t *data, size_t size,
    const uint8_t image[NS2_AMIIBO_V3_SIZE]);
bool ns2_amiibo_v3_is_write_start(
    const uint8_t *data, size_t size,
    const uint8_t image[NS2_AMIIBO_V3_SIZE]);
bool ns2_amiibo_v3_is_extended_start(
    const uint8_t *data, size_t size,
    const uint8_t image[NS2_AMIIBO_V3_SIZE]);
size_t ns2_amiibo_v3_extended_expected_size(
    const uint8_t *data, size_t size,
    const uint8_t image[NS2_AMIIBO_V3_SIZE]);

// Validate and atomically apply one complete staged console write. No image
// byte changes until the full envelope, UID, record list, ranges, and trailing
// padding have all passed validation.
ns2_virtual_nfc_result_t ns2_amiibo_v3_write_commit(
    ns2_virtual_nfc_write_t *write,
    uint8_t image[NS2_AMIIBO_V3_SIZE],
    uint8_t *record_count_out, uint16_t *data_bytes_out);

// Validate and atomically apply one complete sector-aware 0x20 operation. The
// caller supplies the size classified from the offset-zero chunk so incomplete
// and overlong staging cannot be mistaken for either observed shape.
ns2_virtual_nfc_result_t ns2_amiibo_v3_extended_commit(
    ns2_virtual_nfc_write_t *write,
    uint8_t image[NS2_AMIIBO_V3_SIZE], size_t expected_size,
    uint8_t *record_count_out, uint16_t *data_bytes_out);

// Air Riders writes a 355-byte clear + ordinary 0x08 checkpoint, then starts a
// second transaction and writes the 167-byte update + another ordinary 0x08
// checkpoint. Genuine hardware keeps the tag present across the first Stop.
// This bounded helper prevents the first checkpoint from triggering the normal
// virtual-tag auto-eject while ensuring an abandoned sequence cannot affect a
// later unrelated write.
void ns2_amiibo_v3_extended_sequence_reset(
    ns2_amiibo_v3_extended_sequence_t *sequence);
void ns2_amiibo_v3_extended_sequence_note_commit(
    ns2_amiibo_v3_extended_sequence_t *sequence,
    size_t committed_size, uint32_t now_ms);
void ns2_amiibo_v3_extended_sequence_expire(
    ns2_amiibo_v3_extended_sequence_t *sequence, uint32_t now_ms);
bool ns2_amiibo_v3_extended_sequence_continue_after_write(
    ns2_amiibo_v3_extended_sequence_t *sequence, uint32_t now_ms);

#endif /* NS2_AMIIBO_V3_WRITE_H */
