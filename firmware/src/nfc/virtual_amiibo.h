#ifndef VIRTUAL_AMIIBO_H
#define VIRTUAL_AMIIBO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Transport-neutral, host-testable storage for one mutable NTAG215 image.
//
// A normal amiibo dump is 540 bytes. Some tools append the 32-byte NTAG
// READ_SIG originality signature, producing a 572-byte extended dump. The
// signature is immutable; console writes update only the raw 540-byte image.
#define VIRTUAL_AMIIBO_RAW_SIZE 540u
#define VIRTUAL_AMIIBO_SIGNATURE_SIZE 32u
#define VIRTUAL_AMIIBO_EXTENDED_SIZE \
    (VIRTUAL_AMIIBO_RAW_SIZE + VIRTUAL_AMIIBO_SIGNATURE_SIZE)
#define VIRTUAL_AMIIBO_UPLOAD_COVERAGE_SIZE \
    ((VIRTUAL_AMIIBO_EXTENDED_SIZE + 7u) / 8u)

typedef enum {
    VIRTUAL_AMIIBO_OK = 0,
    VIRTUAL_AMIIBO_ERROR_ARGUMENT,
    VIRTUAL_AMIIBO_ERROR_SIZE,
    VIRTUAL_AMIIBO_ERROR_BCC,
    VIRTUAL_AMIIBO_ERROR_UPLOAD_STATE,
    VIRTUAL_AMIIBO_ERROR_RANGE,
    VIRTUAL_AMIIBO_ERROR_CONFLICT,
    VIRTUAL_AMIIBO_ERROR_INCOMPLETE,
    VIRTUAL_AMIIBO_ERROR_CRC,
    VIRTUAL_AMIIBO_ERROR_DIRTY,
    VIRTUAL_AMIIBO_ERROR_NOT_LOADED,
} virtual_amiibo_result_t;

typedef struct {
    // Immutable baseline imported by the user. Console writes never modify
    // this copy, so selecting "Unused" is reversible.
    uint8_t clean_raw[VIRTUAL_AMIIBO_RAW_SIZE];

    // Mutable console-written copy. It is meaningful when
    // has_used_copy=true; using_used_copy selects which image the reader
    // presents without destroying either one.
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE];
    uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE];

    // Uploads are transactional: the selected tag is unchanged until every
    // byte has arrived, its CRC matches, and the NTAG UID/BCC validates.
    uint8_t upload[VIRTUAL_AMIIBO_EXTENDED_SIZE];
    uint8_t upload_coverage[VIRTUAL_AMIIBO_UPLOAD_COVERAGE_SIZE];
    uint32_t upload_expected_crc;
    uint16_t upload_size;
    uint16_t upload_received;

    uint32_t generation;
    uint16_t source_size;
    bool loaded;
    bool has_originality_signature;
    bool dirty;
    bool persisted;
    bool upload_active;
    bool has_used_copy;
    bool using_used_copy;
} virtual_amiibo_t;

void virtual_amiibo_init(virtual_amiibo_t *tag);

bool virtual_amiibo_supported_size(size_t size);
uint32_t virtual_amiibo_crc32(const uint8_t *data, size_t size);
virtual_amiibo_result_t virtual_amiibo_validate_raw(const uint8_t *raw,
                                                     size_t size);
const char *virtual_amiibo_result_string(virtual_amiibo_result_t result);

virtual_amiibo_result_t virtual_amiibo_load(virtual_amiibo_t *tag,
                                             const uint8_t *data,
                                             size_t size,
                                             bool persisted);
virtual_amiibo_result_t virtual_amiibo_upload_begin(virtual_amiibo_t *tag,
                                                     size_t size,
                                                     uint32_t expected_crc);
virtual_amiibo_result_t virtual_amiibo_upload_chunk(virtual_amiibo_t *tag,
                                                     size_t offset,
                                                     const uint8_t *data,
                                                     size_t size);
virtual_amiibo_result_t virtual_amiibo_upload_commit(virtual_amiibo_t *tag);
virtual_amiibo_result_t virtual_amiibo_upload_commit_used(
    virtual_amiibo_t *tag);
void virtual_amiibo_upload_cancel(virtual_amiibo_t *tag);

size_t virtual_amiibo_export_size(const virtual_amiibo_t *tag);
virtual_amiibo_result_t virtual_amiibo_read(const virtual_amiibo_t *tag,
                                             size_t offset,
                                             uint8_t *out,
                                             size_t size);
virtual_amiibo_result_t virtual_amiibo_read_copy(
    const virtual_amiibo_t *tag, bool used, size_t offset,
    uint8_t *out, size_t size);
virtual_amiibo_result_t virtual_amiibo_copy_active_raw(
    const virtual_amiibo_t *tag, uint8_t out[VIRTUAL_AMIIBO_RAW_SIZE]);
virtual_amiibo_result_t virtual_amiibo_select_used(
    virtual_amiibo_t *tag, bool used);
void virtual_amiibo_uid(const virtual_amiibo_t *tag, uint8_t uid[7]);
void virtual_amiibo_mark_dirty(virtual_amiibo_t *tag);
void virtual_amiibo_acknowledge_download(virtual_amiibo_t *tag);
virtual_amiibo_result_t virtual_amiibo_apply_console_write(
    virtual_amiibo_t *tag,
    const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    uint32_t expected_generation);

#endif
