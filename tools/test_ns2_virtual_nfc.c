#include "ns2_virtual_nfc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void make_valid_dump(uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE])
{
    for (size_t i = 0; i < VIRTUAL_AMIIBO_RAW_SIZE; ++i)
        raw[i] = (uint8_t)(i * 29u + 7u);
    raw[3] = (uint8_t)(0x88u ^ raw[0] ^ raw[1] ^ raw[2]);
    raw[8] = (uint8_t)(raw[4] ^ raw[5] ^ raw[6] ^ raw[7]);
}

static void make_write(uint8_t staging[NS2_NFC_WRITE_STAGING_SIZE],
                       const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE])
{
    memset(staging, 0, NS2_NFC_WRITE_STAGING_SIZE);
    staging[0] = 0xD0;
    staging[1] = 0x07;
    staging[2] = raw[0];
    staging[3] = raw[1];
    staging[4] = raw[2];
    staging[5] = raw[4];
    staging[6] = raw[5];
    staging[7] = raw[6];
    staging[8] = raw[7];
    staging[17] = 0x11;
    staging[18] = 0x22;
    staging[19] = 0x33;
    staging[20] = 0x44;
    staging[21] = 2;
    staging[22] = 5;  // page 5, 4 bytes
    staging[23] = 4;
    staging[24] = 0xA1;
    staging[25] = 0xA2;
    staging[26] = 0xA3;
    staging[27] = 0xA4;
    staging[28] = 129;  // last writable page, 4 bytes
    staging[29] = 4;
    staging[30] = 0xB1;
    staging[31] = 0xB2;
    staging[32] = 0xB3;
    staging[33] = 0xB4;
}

int main(void)
{
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE];
    make_valid_dump(raw);
    const uint8_t uid[7] =
        {raw[0], raw[1], raw[2], raw[4], raw[5], raw[6], raw[7]};

    uint8_t status[NS2_NFC_STATUS_PAYLOAD_SIZE];
    ns2_virtual_nfc_build_status(false, NULL, status);
    assert(status[0] == 0x07 && status[1] == 0x41);
    for (size_t i = 2; i < sizeof(status); ++i) assert(status[i] == 0);
    ns2_virtual_nfc_build_status(true, uid, status);
    assert(status[0] == 0x09 && status[1] == 0x00);
    assert(status[4] == 1 && status[5] == 1 && status[6] == 2);
    assert(status[8] == 7 && memcmp(status + 9, uid, 7) == 0);

    uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE];
    uint8_t metadata[NS2_NFC_OPERATION_METADATA_SIZE];
    for (size_t i = 0; i < sizeof(signature); ++i)
        signature[i] = (uint8_t)(0x80u + i);
    for (size_t i = 0; i < sizeof(metadata); ++i)
        metadata[i] = (uint8_t)(0x30u + i);
    uint8_t buffer[NS2_NFC_READ_BUFFER_SIZE];
    assert(ns2_virtual_nfc_build_read_buffer(
               raw, signature, metadata, buffer) ==
           NS2_VIRTUAL_NFC_OK);
    assert(buffer[0] == 4);
    assert(buffer[4] == 1 && buffer[5] == 2 && buffer[6] == 0);
    assert(buffer[7] == 7 && memcmp(buffer + 8, uid, 7) == 0);
    assert(memcmp(buffer + 19, signature, sizeof(signature)) == 0);
    assert(memcmp(buffer + 51, metadata, sizeof(metadata)) == 0);
    assert(memcmp(buffer + 60, raw, sizeof(raw)) == 0);

    uint8_t chunk[NS2_NFC_READ_CHUNK_PAYLOAD_SIZE];
    size_t chunk_size = 0;
    assert(ns2_virtual_nfc_build_read_chunk(
               buffer, 0, chunk, &chunk_size) ==
           NS2_VIRTUAL_NFC_OK);
    assert(chunk_size == 73);
    assert(chunk[0] == 0 && chunk[1] == 70 && chunk[2] == 0);
    assert(memcmp(chunk + 3, buffer, 70) == 0);

    assert(ns2_virtual_nfc_build_read_chunk(
               buffer, 560, chunk, &chunk_size) ==
           NS2_VIRTUAL_NFC_OK);
    assert(chunk_size == 43);
    assert(chunk[0] == 1 && chunk[1] == 40 && chunk[2] == 0);
    assert(memcmp(chunk + 3, buffer + 560, 40) == 0);

    assert(ns2_virtual_nfc_build_read_chunk(
               buffer, 599, chunk, &chunk_size) ==
           NS2_VIRTUAL_NFC_OK);
    assert(chunk_size == 4);
    assert(chunk[0] == 1 && chunk[1] == 1 && chunk[2] == 0);
    assert(chunk[3] == buffer[599]);
    assert(ns2_virtual_nfc_build_read_chunk(
               buffer, 600, chunk, &chunk_size) ==
           NS2_VIRTUAL_NFC_ERROR_RANGE);

    uint8_t write_prep[NS2_NFC_WRITE_PREP_BUFFER_SIZE];
    assert(ns2_virtual_nfc_build_write_prep_buffer(
               raw, signature, metadata, write_prep) ==
           NS2_VIRTUAL_NFC_OK);
    assert(memcmp(write_prep, buffer, 60) == 0);
    assert(memcmp(write_prep + 60, raw + 12, 4) == 0);
    assert(ns2_virtual_nfc_build_buffer_chunk(
               write_prep, sizeof(write_prep), 0, chunk, &chunk_size) ==
           NS2_VIRTUAL_NFC_OK);
    assert(chunk_size == 67);
    assert(chunk[0] == 1 && chunk[1] == 64 && chunk[2] == 0);
    assert(memcmp(chunk + 3, write_prep, sizeof(write_prep)) == 0);

    uint8_t staging[NS2_NFC_WRITE_STAGING_SIZE];
    make_write(staging, raw);
    ns2_virtual_nfc_write_t write;
    ns2_virtual_nfc_write_init(&write);
    assert(ns2_virtual_nfc_write_chunk(&write, 0, staging, 1) ==
           NS2_VIRTUAL_NFC_ERROR_NOT_ACTIVE);
    ns2_virtual_nfc_write_begin(&write);
    for (size_t offset = 0; offset < sizeof(staging); offset += 37) {
        size_t count = sizeof(staging) - offset;
        if (count > 37) count = 37;
        assert(ns2_virtual_nfc_write_chunk(
                   &write, offset, staging + offset, count) ==
               NS2_VIRTUAL_NFC_OK);
    }
    // Identical retries are harmless; conflicting retries are rejected.
    assert(ns2_virtual_nfc_write_chunk(&write, 22, staging + 22, 8) ==
           NS2_VIRTUAL_NFC_OK);
    uint8_t conflict = (uint8_t)(staging[25] ^ 1u);
    assert(ns2_virtual_nfc_write_chunk(&write, 25, &conflict, 1) ==
           NS2_VIRTUAL_NFC_ERROR_CONFLICT);

    uint8_t count;
    uint16_t bytes;
    assert(ns2_virtual_nfc_write_commit(&write, raw, &count, &bytes) ==
           NS2_VIRTUAL_NFC_OK);
    assert(count == 2 && bytes == 8);
    assert(memcmp(raw + 16, (uint8_t[]){0x11, 0x22, 0x33, 0x44}, 4) == 0);
    assert(memcmp(raw + 20, (uint8_t[]){0xA1, 0xA2, 0xA3, 0xA4}, 4) == 0);
    assert(memcmp(raw + 516, (uint8_t[]){0xB1, 0xB2, 0xB3, 0xB4}, 4) == 0);

    // Malformed transactions never partially alter the selected image.
    uint8_t before[VIRTUAL_AMIIBO_RAW_SIZE];
    memcpy(before, raw, sizeof(before));
    make_write(staging, raw);
    staging[22] = 4;  // protected manufacturer area
    ns2_virtual_nfc_write_begin(&write);
    assert(ns2_virtual_nfc_write_chunk(
               &write, 0, staging, sizeof(staging)) ==
           NS2_VIRTUAL_NFC_OK);
    assert(ns2_virtual_nfc_write_commit(&write, raw, NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_RECORD);
    assert(memcmp(raw, before, sizeof(raw)) == 0);

    puts("ns2_virtual_nfc: all tests passed");
    return 0;
}
