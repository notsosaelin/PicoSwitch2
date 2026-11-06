#include "virtual_amiibo.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void make_valid_dump(uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE])
{
    for (size_t i = 0; i < VIRTUAL_AMIIBO_RAW_SIZE; ++i)
        raw[i] = (uint8_t)(i * 37u + 11u);
    raw[3] = (uint8_t)(0x88u ^ raw[0] ^ raw[1] ^ raw[2]);
    raw[8] = (uint8_t)(raw[4] ^ raw[5] ^ raw[6] ^ raw[7]);
}

int main(void)
{
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE];
    make_valid_dump(raw);
    assert(virtual_amiibo_validate_raw(raw, sizeof(raw)) ==
           VIRTUAL_AMIIBO_OK);
    raw[3] ^= 1;
    assert(virtual_amiibo_validate_raw(raw, sizeof(raw)) ==
           VIRTUAL_AMIIBO_ERROR_BCC);
    raw[3] ^= 1;

    virtual_amiibo_t tag;
    virtual_amiibo_init(&tag);
    const uint32_t crc = virtual_amiibo_crc32(raw, sizeof(raw));
    assert(virtual_amiibo_upload_begin(&tag, sizeof(raw), crc) ==
           VIRTUAL_AMIIBO_OK);

    // Out-of-order chunks and byte-identical retries are supported.
    assert(virtual_amiibo_upload_chunk(&tag, 500, raw + 500, 40) ==
           VIRTUAL_AMIIBO_OK);
    assert(virtual_amiibo_upload_chunk(&tag, 500, raw + 500, 40) ==
           VIRTUAL_AMIIBO_OK);
    for (size_t offset = 0; offset < 500; offset += 25) {
        assert(virtual_amiibo_upload_chunk(&tag, offset, raw + offset, 25) ==
               VIRTUAL_AMIIBO_OK);
    }
    assert(tag.upload_received == sizeof(raw));
    assert(virtual_amiibo_upload_commit(&tag) == VIRTUAL_AMIIBO_OK);
    assert(tag.loaded && !tag.dirty && !tag.persisted);
    assert(!tag.has_used_copy && !tag.using_used_copy);
    assert(memcmp(tag.clean_raw, raw, sizeof(raw)) == 0);
    assert(virtual_amiibo_export_size(&tag) == VIRTUAL_AMIIBO_RAW_SIZE);

    uint8_t exported[VIRTUAL_AMIIBO_EXTENDED_SIZE];
    memset(exported, 0, sizeof(exported));
    assert(virtual_amiibo_read(&tag, 0, exported, sizeof(raw)) ==
           VIRTUAL_AMIIBO_OK);
    assert(memcmp(exported, raw, sizeof(raw)) == 0);

    uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE];
    for (size_t i = 0; i < sizeof(signature); ++i)
        signature[i] = (uint8_t)(0xA0u + i);
    uint8_t extended[VIRTUAL_AMIIBO_EXTENDED_SIZE];
    memcpy(extended, raw, sizeof(raw));
    memcpy(extended + sizeof(raw), signature, sizeof(signature));
    assert(virtual_amiibo_load(&tag, extended, sizeof(extended), true) ==
           VIRTUAL_AMIIBO_OK);
    assert(tag.has_originality_signature && tag.persisted);
    assert(virtual_amiibo_read(&tag, VIRTUAL_AMIIBO_RAW_SIZE,
                               exported, sizeof(signature)) ==
           VIRTUAL_AMIIBO_OK);
    assert(memcmp(exported, signature, sizeof(signature)) == 0);

    const uint32_t write_generation = tag.generation;
    uint8_t updated[VIRTUAL_AMIIBO_RAW_SIZE];
    memcpy(updated, tag.raw, sizeof(updated));
    updated[100] ^= 0x5A;
    assert(virtual_amiibo_apply_console_write(
               &tag, updated, write_generation + 1) ==
           VIRTUAL_AMIIBO_ERROR_CONFLICT);
    assert(virtual_amiibo_apply_console_write(
               &tag, updated, write_generation) == VIRTUAL_AMIIBO_OK);
    assert(tag.dirty && !tag.persisted &&
           tag.generation == write_generation + 1);
    assert(tag.has_used_copy && tag.using_used_copy);
    assert(memcmp(tag.clean_raw, extended, sizeof(raw)) == 0);
    assert(memcmp(tag.raw, updated, sizeof(raw)) == 0);

    // Clean/used selection is reversible and export keeps the used save even
    // while the console is presenting the clean baseline.
    assert(virtual_amiibo_select_used(&tag, false) == VIRTUAL_AMIIBO_OK);
    assert(!tag.using_used_copy);
    uint8_t active[VIRTUAL_AMIIBO_RAW_SIZE];
    assert(virtual_amiibo_copy_active_raw(&tag, active) ==
           VIRTUAL_AMIIBO_OK);
    assert(memcmp(active, extended, sizeof(active)) == 0);
    assert(virtual_amiibo_read(&tag, 0, exported, sizeof(active)) ==
           VIRTUAL_AMIIBO_OK);
    assert(memcmp(exported, updated, sizeof(active)) == 0);
    assert(virtual_amiibo_select_used(&tag, true) == VIRTUAL_AMIIBO_OK);
    assert(tag.using_used_copy);
    updated[0] ^= 1;
    updated[3] = (uint8_t)(0x88u ^ updated[0] ^ updated[1] ^ updated[2]);
    assert(virtual_amiibo_apply_console_write(
               &tag, updated, tag.generation) ==
           VIRTUAL_AMIIBO_ERROR_CONFLICT);

    virtual_amiibo_mark_dirty(&tag);
    assert(virtual_amiibo_upload_begin(&tag, sizeof(raw), crc) ==
           VIRTUAL_AMIIBO_ERROR_DIRTY);
    virtual_amiibo_acknowledge_download(&tag);
    assert(!tag.dirty && !tag.persisted);
    assert(virtual_amiibo_upload_begin(&tag, sizeof(raw), crc) ==
           VIRTUAL_AMIIBO_OK);
    assert(virtual_amiibo_upload_chunk(&tag, 0, raw, 10) ==
           VIRTUAL_AMIIBO_OK);
    uint8_t conflicting[10];
    memcpy(conflicting, raw, sizeof(conflicting));
    conflicting[5] ^= 1;
    assert(virtual_amiibo_upload_chunk(&tag, 0, conflicting,
                                       sizeof(conflicting)) ==
           VIRTUAL_AMIIBO_ERROR_CONFLICT);
    assert(virtual_amiibo_upload_commit(&tag) ==
           VIRTUAL_AMIIBO_ERROR_INCOMPLETE);

    virtual_amiibo_upload_cancel(&tag);
    assert(!tag.upload_active);
    assert(virtual_amiibo_upload_begin(&tag, sizeof(raw), crc ^ 1u) ==
           VIRTUAL_AMIIBO_OK);
    assert(virtual_amiibo_upload_chunk(&tag, 0, raw, sizeof(raw)) ==
           VIRTUAL_AMIIBO_OK);
    assert(virtual_amiibo_upload_commit(&tag) ==
           VIRTUAL_AMIIBO_ERROR_CRC);

    virtual_amiibo_upload_cancel(&tag);
    assert(virtual_amiibo_load(&tag, extended, sizeof(extended), true) ==
           VIRTUAL_AMIIBO_OK);
    uint8_t imported_used[VIRTUAL_AMIIBO_EXTENDED_SIZE];
    memcpy(imported_used, extended, sizeof(imported_used));
    imported_used[120] ^= 0x33;
    const uint32_t used_crc =
        virtual_amiibo_crc32(imported_used, sizeof(imported_used));
    assert(virtual_amiibo_upload_begin(
               &tag, sizeof(imported_used), used_crc) == VIRTUAL_AMIIBO_OK);
    assert(virtual_amiibo_upload_chunk(
               &tag, 0, imported_used, sizeof(imported_used)) ==
           VIRTUAL_AMIIBO_OK);
    assert(virtual_amiibo_upload_commit_used(&tag) == VIRTUAL_AMIIBO_OK);
    assert(tag.has_used_copy && tag.using_used_copy && !tag.dirty);
    assert(memcmp(tag.clean_raw, extended, VIRTUAL_AMIIBO_RAW_SIZE) == 0);
    assert(memcmp(tag.raw, imported_used, VIRTUAL_AMIIBO_RAW_SIZE) == 0);

    puts("virtual_amiibo: all tests passed");
    return 0;
}
