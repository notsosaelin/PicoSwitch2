#ifndef VIRTUAL_AMIIBO_STORE_H
#define VIRTUAL_AMIIBO_STORE_H

#include "virtual_amiibo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool loaded;
    bool dirty;
    bool persisted;
    bool has_originality_signature;
    bool has_used_copy;
    bool using_used_copy;
    bool upload_active;
    uint16_t size;
    uint16_t upload_size;
    uint16_t upload_received;
    uint32_t generation;
    uint8_t uid[7];
} virtual_amiibo_status_t;

// Initialize the shared runtime and recover the newest valid flash journal
// entry. Called once on core0 before core1 starts.
void virtual_amiibo_store_init(void);

void virtual_amiibo_store_status(virtual_amiibo_status_t *out);
virtual_amiibo_result_t virtual_amiibo_store_upload_begin(
    size_t size, uint32_t expected_crc);
virtual_amiibo_result_t virtual_amiibo_store_upload_chunk(
    size_t offset, const uint8_t *data, size_t size);
virtual_amiibo_result_t virtual_amiibo_store_upload_commit(void);
virtual_amiibo_result_t virtual_amiibo_store_upload_commit_used(void);
void virtual_amiibo_store_upload_cancel(void);

virtual_amiibo_result_t virtual_amiibo_store_read(
    size_t offset, uint8_t *out, size_t size);
virtual_amiibo_result_t virtual_amiibo_store_read_copy(
    bool used, size_t offset, uint8_t *out, size_t size);
void virtual_amiibo_store_acknowledge_download(void);
virtual_amiibo_result_t virtual_amiibo_store_select_used(bool used);

// Console NFC code snapshots the selected clean/used copy. Successful writes
// atomically update the used copy and queue a flash snapshot automatically.
virtual_amiibo_result_t virtual_amiibo_store_copy_raw(uint8_t out[540]);
virtual_amiibo_result_t virtual_amiibo_store_copy_image(
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE],
    bool *has_signature, uint32_t *generation);
virtual_amiibo_result_t virtual_amiibo_store_apply_console_write(
    const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    uint32_t expected_generation);
void virtual_amiibo_store_mark_dirty(void);

void virtual_amiibo_store_request_persist(void);
bool virtual_amiibo_store_persist_pending(void);

// Performs a pending alternating-bank flash snapshot. Must run on core1,
// which owns the multicore-lockout requester role.
void virtual_amiibo_store_service_save(void);

#endif
