#ifndef VIRTUAL_AMIIBO_STORE_H
#define VIRTUAL_AMIIBO_STORE_H

#include "virtual_amiibo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool loaded;
    bool presented;
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
// Lock-free read for the 1 kHz USB report path. True means a stored image is
// currently presented to the console. Ejecting only changes presentation; it
// never deletes Save 1 or Save 2 from RAM/flash. Discarding the stored image
// entirely is the separate explicit clear request below.
bool virtual_amiibo_store_loaded(void);
virtual_amiibo_result_t virtual_amiibo_store_set_presented(bool presented);
// Discard the stored virtual amiibo entirely: RAM images, presentation, and
// both flash journal banks, returning the store to its blank post-install
// state. The erase runs on core1 through the same config-save service as
// ordinary snapshots; poll virtual_amiibo_store_clear_pending() for
// completion.
void virtual_amiibo_store_request_clear(void);
bool virtual_amiibo_store_clear_pending(void);

// --- NTAG I2C 2K ("figure v3", e.g. Kirby Air Riders) present-and-trace slot ---
// A RAM-only 2048-byte slot kept entirely separate from the 540/572 NTAG215
// store and its flash journal, so this experimental path cannot affect the
// validated NFC/persistence behavior. It exists to serve a v3 image to the
// console so the UART tracer can capture how the console reads a 2 KB tag.
// Not persisted; cleared on power cycle. See
// docs/switch2/kirby-air-riders-extended-amiibo.md.
virtual_amiibo_result_t virtual_amiibo_store_v3_upload_begin(
    size_t size, uint32_t expected_crc);
virtual_amiibo_result_t virtual_amiibo_store_v3_upload_chunk(
    size_t offset, const uint8_t *data, size_t size);
virtual_amiibo_result_t virtual_amiibo_store_v3_upload_commit(void);
bool virtual_amiibo_store_v3_upload_active(void);
// True once a valid v3 image has been uploaded and is presented.
bool virtual_amiibo_store_v3_loaded(void);
void virtual_amiibo_store_v3_clear(void);
// Copies the 2048-byte v3 image; returns false if none is loaded.
bool virtual_amiibo_store_v3_copy(uint8_t out[2048]);
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


// Flash-journal inspector (diagnostics). Reports what is actually stored in each
// bank, independent of the in-RAM slots, so "the write never happened" can be
// told apart from "the write happened but boot did not load it".
typedef struct {
    uint8_t  header[24];      // raw record header as found in flash
    bool     v3_valid;
    bool     v2_valid;
    uint32_t v3_generation;
    uint32_t v2_generation;
} virtual_amiibo_bank_debug_t;

typedef struct {
    int active_bank;                       // -1 when none
    bool persist_pending;
    bool v3_slot_loaded;
    virtual_amiibo_bank_debug_t bank[2];
} virtual_amiibo_journal_debug_t;

void virtual_amiibo_store_journal_debug(virtual_amiibo_journal_debug_t *out);

#endif
