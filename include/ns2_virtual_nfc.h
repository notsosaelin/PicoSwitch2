#ifndef NS2_VIRTUAL_NFC_H
#define NS2_VIRTUAL_NFC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "virtual_amiibo.h"

// Transport-neutral Switch 2 virtual-tag codec. This module deliberately does
// not own USB, timing, flash, or the selected-tag store, so every byte-level
// rule can be exercised by host tests before command 0x01 dispatch is enabled.
#define NS2_NFC_STATUS_PAYLOAD_SIZE 61u
#define NS2_NFC_READ_BUFFER_SIZE 600u
#define NS2_NFC_WRITE_PREP_BUFFER_SIZE 64u
#define NS2_NFC_READ_CHUNK_DATA_SIZE 70u
#define NS2_NFC_READ_CHUNK_HEADER_SIZE 3u
#define NS2_NFC_READ_CHUNK_PAYLOAD_SIZE \
    (NS2_NFC_READ_CHUNK_HEADER_SIZE + NS2_NFC_READ_CHUNK_DATA_SIZE)
#define NS2_NFC_OPERATION_METADATA_SIZE 9u
#define NS2_NFC_WRITE_STAGING_SIZE 454u
#define NS2_NFC_WRITE_COVERAGE_SIZE \
    ((NS2_NFC_WRITE_STAGING_SIZE + 7u) / 8u)

typedef enum {
    NS2_VIRTUAL_NFC_OK = 0,
    NS2_VIRTUAL_NFC_ERROR_ARGUMENT,
    NS2_VIRTUAL_NFC_ERROR_RAW_TAG,
    NS2_VIRTUAL_NFC_ERROR_NOT_ACTIVE,
    NS2_VIRTUAL_NFC_ERROR_RANGE,
    NS2_VIRTUAL_NFC_ERROR_CONFLICT,
    NS2_VIRTUAL_NFC_ERROR_INCOMPLETE,
    NS2_VIRTUAL_NFC_ERROR_HEADER,
    NS2_VIRTUAL_NFC_ERROR_UID,
    NS2_VIRTUAL_NFC_ERROR_RECORD_COUNT,
    NS2_VIRTUAL_NFC_ERROR_RECORD,
    NS2_VIRTUAL_NFC_ERROR_TRAILING_DATA,
} ns2_virtual_nfc_result_t;

typedef struct {
    uint8_t bytes[NS2_NFC_WRITE_STAGING_SIZE];
    uint8_t coverage[NS2_NFC_WRITE_COVERAGE_SIZE];
    uint16_t received;
    bool active;
} ns2_virtual_nfc_write_t;

const char *ns2_virtual_nfc_result_string(ns2_virtual_nfc_result_t result);

void ns2_virtual_nfc_build_status(bool tag_present, const uint8_t uid[7],
                                  uint8_t out[NS2_NFC_STATUS_PAYLOAD_SIZE]);

// Builds the exact 600-byte reader buffer observed from a genuine Pro
// Controller 2. The console retrieves this buffer with command 0x01/0x15,
// passing a little-endian offset and receiving at most 70 bytes per command.
ns2_virtual_nfc_result_t ns2_virtual_nfc_build_read_buffer(
    const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    const uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE],
    const uint8_t operation_metadata[NS2_NFC_OPERATION_METADATA_SIZE],
    uint8_t out[NS2_NFC_READ_BUFFER_SIZE]);

// The write-preparation form uses the same 60-byte identity/signature/
// operation prefix followed only by NTAG page 3's four-byte capability
// container. This is the transport-neutral 64-byte buffer implied by the
// existing write captures; hardware validation remains gated.
ns2_virtual_nfc_result_t ns2_virtual_nfc_build_write_prep_buffer(
    const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    const uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE],
    const uint8_t operation_metadata[NS2_NFC_OPERATION_METADATA_SIZE],
    uint8_t out[NS2_NFC_WRITE_PREP_BUFFER_SIZE]);

// Produces the data portion of a command 0x01/0x15 response:
//   [last:u8][length:u16le][data:0..70]
// out_size is therefore 3..73 bytes. An offset at or beyond the end of the
// reader buffer is rejected rather than returning an ambiguous empty chunk.
ns2_virtual_nfc_result_t ns2_virtual_nfc_build_read_chunk(
    const uint8_t buffer[NS2_NFC_READ_BUFFER_SIZE], uint16_t offset,
    uint8_t out[NS2_NFC_READ_CHUNK_PAYLOAD_SIZE], size_t *out_size);

ns2_virtual_nfc_result_t ns2_virtual_nfc_build_buffer_chunk(
    const uint8_t *buffer, size_t buffer_size, uint16_t offset,
    uint8_t out[NS2_NFC_READ_CHUNK_PAYLOAD_SIZE], size_t *out_size);

void ns2_virtual_nfc_write_init(ns2_virtual_nfc_write_t *write);
void ns2_virtual_nfc_write_begin(ns2_virtual_nfc_write_t *write);
void ns2_virtual_nfc_write_cancel(ns2_virtual_nfc_write_t *write);
ns2_virtual_nfc_result_t ns2_virtual_nfc_write_chunk(
    ns2_virtual_nfc_write_t *write, size_t offset,
    const uint8_t *data, size_t size);

// Applies a complete staged console write atomically. On any error, raw is
// byte-identical to its input.
ns2_virtual_nfc_result_t ns2_virtual_nfc_write_commit(
    ns2_virtual_nfc_write_t *write,
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    uint8_t *record_count_out, uint16_t *data_bytes_out);

#endif
