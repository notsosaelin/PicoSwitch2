#include "ns2_virtual_nfc_runtime.h"

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

static void fill_uid(uint8_t out[7],
                     const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE])
{
    out[0] = raw[0];
    out[1] = raw[1];
    out[2] = raw[2];
    out[3] = raw[4];
    out[4] = raw[5];
    out[5] = raw[6];
    out[6] = raw[7];
}

static void make_begin(uint8_t out[19],
                       const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
                       bool write_mode)
{
    const uint8_t metadata[9] =
        {0x03, 0x00, 0x3B, 0x3C, 0x77, 0x78, 0x86, 0, 0};
    memset(out, 0, 19);
    out[0] = 0xD0;
    out[1] = 0x07;
    if (write_mode)
        fill_uid(out + 2, raw);
    out[9] = 0x01;
    memcpy(out + 10, metadata, sizeof(metadata));
}

static void make_write(uint8_t staging[NS2_NFC_WRITE_STAGING_SIZE],
                       const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE])
{
    memset(staging, 0, NS2_NFC_WRITE_STAGING_SIZE);
    staging[0] = 0xD0;
    staging[1] = 0x07;
    fill_uid(staging + 2, raw);
    staging[17] = 0x11;
    staging[18] = 0x22;
    staging[19] = 0x33;
    staging[20] = 0x44;
    staging[21] = 2;
    staging[22] = 5;
    staging[23] = 4;
    staging[24] = 0xA1;
    staging[25] = 0xA2;
    staging[26] = 0xA3;
    staging[27] = 0xA4;
    staging[28] = 129;
    staging[29] = 4;
    staging[30] = 0xB1;
    staging[31] = 0xB2;
    staging[32] = 0xB3;
    staging[33] = 0xB4;
}

static void send_write_chunks(ns2_virtual_nfc_runtime_t *runtime,
                              uint32_t now_ms,
                              uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
                              const uint8_t signature[32],
                              const uint8_t staging[NS2_NFC_WRITE_STAGING_SIZE],
                              ns2_virtual_nfc_response_t *response)
{
    uint8_t request[4 + 76];
    for (size_t offset = 0; offset < NS2_NFC_WRITE_STAGING_SIZE;
         offset += 76) {
        size_t count = NS2_NFC_WRITE_STAGING_SIZE - offset;
        if (count > 76) count = 76;
        request[0] = (uint8_t)offset;
        request[1] = (uint8_t)(offset >> 8);
        request[2] = (uint8_t)count;
        request[3] = (uint8_t)(count >> 8);
        memcpy(request + 4, staging + offset, count);
        assert(ns2_virtual_nfc_runtime_dispatch(
            runtime, now_ms++, 0x14, request, count + 4, true, raw,
            signature, response));
        assert(response->response_direction == 0x04);
        assert(response->payload_size == 0);
    }
}

static void test_read_and_write(void)
{
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE];
    uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE] = {0};
    make_valid_dump(raw);

    ns2_virtual_nfc_runtime_t runtime;
    ns2_virtual_nfc_runtime_init(&runtime);
    ns2_virtual_nfc_response_t response;

    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 100, 0x03, NULL, 0, true, raw, signature, &response));
    assert(response.response_direction == 0x04 &&
           response.payload_size == 0);
    // First observation accounts for TagPresented; ScanReady is delayed.
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 1);
    ns2_virtual_nfc_runtime_tick(&runtime, 139);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 1);
    ns2_virtual_nfc_runtime_tick(&runtime, 140);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 2);

    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 141, 0x05, NULL, 0, true, raw, signature, &response));
    assert(response.response_direction == 0x01);
    assert(response.payload_size == NS2_NFC_STATUS_PAYLOAD_SIZE);
    assert(response.payload[0] == 0x09 && response.payload[1] == 0);

    uint8_t begin[19];
    make_begin(begin, raw, false);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 150, 0x06, begin, sizeof(begin), true, raw, signature,
        &response));
    ns2_virtual_nfc_runtime_tick(&runtime, 190);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 3);

    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 191, 0x05, NULL, 0, true, raw, signature, &response));
    assert(response.payload[0] == 0x04 && response.payload[1] == 0);

    const uint8_t offset0[2] = {0, 0};
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 192, 0x15, offset0, sizeof(offset0), true, raw,
        signature, &response));
    assert(response.response_direction == 0x01);
    assert(response.payload_size == 73);
    assert(response.payload[0] == 0 && response.payload[1] == 70 &&
           response.payload[2] == 0 && response.payload[3] == 4);

    const uint8_t offset560[2] = {0x30, 0x02};
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 193, 0x15, offset560, sizeof(offset560), true, raw,
        signature, &response));
    assert(response.payload_size == 43);
    assert(response.payload[0] == 1 && response.payload[1] == 40);
    assert(memcmp(response.payload + 3, raw + 500, 40) == 0);

    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 194, 0x04, NULL, 0, true, raw, signature, &response));
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 3);

    // The tag remains presented for the immediate write phase.
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 200, 0x03, NULL, 0, true, raw, signature, &response));
    ns2_virtual_nfc_runtime_tick(&runtime, 240);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 4);

    make_begin(begin, raw, true);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 241, 0x06, begin, sizeof(begin), true, raw, signature,
        &response));
    ns2_virtual_nfc_runtime_tick(&runtime, 281);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 5);

    // A UID-bearing 0x06 selects the 64-byte write-preparation buffer.
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 282, 0x15, offset0, sizeof(offset0), true, raw,
        signature, &response));
    assert(response.response_direction == 0x01);
    assert(response.payload_size == 67);
    assert(response.payload[0] == 1 && response.payload[1] == 64 &&
           response.payload[2] == 0);
    assert(memcmp(response.payload + 63, raw + 12, 4) == 0);

    uint8_t staging[NS2_NFC_WRITE_STAGING_SIZE];
    make_write(staging, raw);
    send_write_chunks(&runtime, 290, raw, signature, staging, &response);

    // Identical retransmission is safe and does not disturb coverage.
    uint8_t retry[4 + 8] = {22, 0, 8, 0};
    memcpy(retry + 4, staging + 22, 8);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 300, 0x14, retry, sizeof(retry), true, raw, signature,
        &response));

    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 301, 0x08, NULL, 0, true, raw, signature, &response));
    assert(response.write_committed);
    assert(response.write_record_count == 2);
    assert(response.write_data_bytes == 8);
    assert(memcmp(raw + 16, (uint8_t[]){0x11, 0x22, 0x33, 0x44}, 4) ==
           0);
    assert(memcmp(raw + 20, (uint8_t[]){0xA1, 0xA2, 0xA3, 0xA4}, 4) ==
           0);
    assert(memcmp(raw + 516, (uint8_t[]){0xB1, 0xB2, 0xB3, 0xB4}, 4) ==
           0);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 5);
    ns2_virtual_nfc_runtime_tick(&runtime, 1000);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 5);
    ns2_virtual_nfc_runtime_tick(&runtime, 1001);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 6);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 1002, 0x05, NULL, 0, true, raw, signature, &response));
    assert(response.payload[0] == 0x05 && response.payload[1] == 0);

    // Stop is acknowledged, but TagRemoved is deferred until the integration
    // layer confirms the console-written snapshot reached flash.
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 1003, 0x04, NULL, 0, true, raw, signature, &response));
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 6);
    assert(runtime.eject_waiting_for_persist);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 1004, 0x05, NULL, 0, true, raw, signature, &response));
    assert(response.payload[0] == 0x05 && response.payload[1] == 0);

    ns2_virtual_nfc_runtime_set_write_persisted(&runtime, true, 1005);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 7);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 1006, 0x05, NULL, 0, true, raw, signature, &response));
    assert(response.payload[0] == 0x07 && response.payload[1] == 0x41);

    // Scans during the three-second removal window must remain absent. This
    // gives the console time to leave its "remove the amiibo" state instead of
    // immediately detecting the same selected image again.
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 1007, 0x03, NULL, 0, true, raw, signature, &response));
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 7);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 1008, 0x05, NULL, 0, true, raw, signature, &response));
    assert(response.payload[0] == 0x07 && response.payload[1] == 0x41);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 4004, 0x03, NULL, 0, true, raw, signature, &response));
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 7);

    // The first scan at the deadline re-presents the same selected, mutated
    // image. Presentation and ScanReady each advance the modulo-eight counter.
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 4005, 0x03, NULL, 0, true, raw, signature, &response));
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 0);
    ns2_virtual_nfc_runtime_tick(&runtime, 4045);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 1);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 4046, 0x05, NULL, 0, true, raw, signature, &response));
    assert(response.payload[0] == 0x09 && response.payload[1] == 0);

    make_begin(begin, raw, false);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 4047, 0x06, begin, sizeof(begin), true, raw, signature,
        &response));
    const uint8_t offset60[2] = {60, 0};
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 4048, 0x15, offset60, sizeof(offset60), true, raw,
        signature, &response));
    assert(response.payload_size == 73);
    assert(memcmp(response.payload + 3, raw, 70) == 0);
}

static void test_fail_closed(void)
{
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE];
    uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE] = {0};
    make_valid_dump(raw);
    uint8_t before[VIRTUAL_AMIIBO_RAW_SIZE];
    memcpy(before, raw, sizeof(before));

    ns2_virtual_nfc_runtime_t runtime;
    ns2_virtual_nfc_runtime_init(&runtime);
    ns2_virtual_nfc_response_t response;
    uint8_t begin[19];
    make_begin(begin, raw, true);
    begin[8] ^= 1u;

    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 10, 0x06, begin, sizeof(begin), true, raw, signature,
        &response));
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 11, 0x05, NULL, 0, true, raw, signature, &response));
    assert(response.payload[0] == 0x07 && response.payload[1] == 0x41);
    assert(memcmp(raw, before, sizeof(raw)) == 0);

    // Stopping an incomplete write aborts it without auto-ejecting the tag.
    ns2_virtual_nfc_runtime_init(&runtime);
    make_begin(begin, raw, true);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 15, 0x06, begin, sizeof(begin), true, raw, signature,
        &response));
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 16, 0x04, NULL, 0, true, raw, signature, &response));
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 17, 0x05, NULL, 0, true, raw, signature, &response));
    assert(response.payload[0] == 0x09 && response.payload[1] == 0);
    assert(memcmp(raw, before, sizeof(raw)) == 0);

    ns2_virtual_nfc_runtime_init(&runtime);
    make_begin(begin, raw, true);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 20, 0x06, begin, sizeof(begin), true, raw, signature,
        &response));
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 21, 0x08, NULL, 0, true, raw, signature, &response));
    assert(!response.write_committed);
    assert(memcmp(raw, before, sizeof(raw)) == 0);

    ns2_virtual_nfc_runtime_write_apply_failed(&runtime, 30);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 31, 0x05, NULL, 0, true, raw, signature, &response));
    assert(response.payload[0] == 0x07 && response.payload[1] == 0x41);
}

static void test_format_promotion(void)
{
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE];
    uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE] = {0};
    make_valid_dump(raw);

    ns2_virtual_nfc_runtime_t runtime;
    ns2_virtual_nfc_runtime_init(&runtime);
    ns2_virtual_nfc_response_t response;
    uint8_t begin[19];
    make_begin(begin, raw, false);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 100, 0x06, begin, sizeof(begin), true, raw, signature,
        &response));

    uint8_t staging[NS2_NFC_WRITE_STAGING_SIZE];
    make_write(staging, raw);
    send_write_chunks(&runtime, 101, raw, signature, staging, &response);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 200, 0x08, NULL, 0, true, raw, signature, &response));
    assert(response.write_committed);
}

static void test_absent_tag(void)
{
    ns2_virtual_nfc_runtime_t runtime;
    ns2_virtual_nfc_runtime_init(&runtime);
    ns2_virtual_nfc_response_t response;
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 200, 0x03, NULL, 0, false, NULL, NULL, &response));
    ns2_virtual_nfc_runtime_tick(&runtime, 300);
    assert(ns2_virtual_nfc_runtime_report_state(&runtime) == 0);
    assert(ns2_virtual_nfc_runtime_dispatch(
        &runtime, 301, 0x05, NULL, 0, false, NULL, NULL, &response));
    assert(response.payload[0] == 0x07 && response.payload[1] == 0x41);
}

int main(void)
{
    test_read_and_write();
    test_fail_closed();
    test_format_promotion();
    test_absent_tag();
    puts("ns2_virtual_nfc_runtime: all tests passed");
    return 0;
}
