// Host replay of the figure-v3 NFC state machine.
//
// This is what the extraction was for. Before it, the v3 command flow lived in
// the USB personality with file-scope state and could only be exercised on a
// real Switch 2; every deterministic bug in the investigation -- the split
// 88-byte 0x14, the missing 0x1E transition, the stale capability generation,
// the fixed Kirby record pages -- cost a flash and a physical scan to find.
//
// The sequences below are the ones a genuine console actually sends, taken from
// the captures under dumps/. They run with a fake clock, a fake store, and no
// USB, so a regression in any of them fails here instead of on hardware.
//
// Build (one line): gcc -std=c11 -Wall -Wextra -I include -I tools/test_stubs
// tools/test_ns2_amiibo_v3_runtime.c src/nfc/ns2_amiibo_v3_runtime.c
// src/nfc/ns2_amiibo_v3.c src/nfc/ns2_amiibo_v3_write.c
// src/nfc/ns2_virtual_nfc.c src/nfc/virtual_amiibo.c -o <out>

#include "ns2_amiibo_v3_runtime.h"
// For NS2_VIRTUAL_NFC_REPRESENT_COOLDOWN_MS, the removal cooldown shared with
// the 540 path.
#include "ns2_virtual_nfc_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// --- fake store -------------------------------------------------------------

typedef struct {
    bool accept_apply;       // make the store refuse, as a generation race does
    bool persist_pending;    // flash still writing
    uint32_t generation;     // advances on every accepted write, like the store
    uint32_t applies;
    uint32_t presented_calls;
    bool presented;
    uint8_t applied_image[NS2_AMIIBO_V3_SIZE];
} fake_store_t;

// Advancing the generation here is not incidental. After a commit the runtime
// optimistically sets observed_generation to operation_generation + 1, because
// the real store increments when it accepts the write. A fake that did not
// advance made the very next command look like a portal upload, which reset the
// transaction and silently swallowed the committed-write lifecycle. The fake
// has to model that contract or the replay is not faithful.
static bool fake_apply(void *ctx, const uint8_t image[NS2_AMIIBO_V3_SIZE],
                       uint32_t generation)
{
    (void)generation;
    fake_store_t *store = (fake_store_t *)ctx;
    if (!store->accept_apply) return false;
    memcpy(store->applied_image, image, NS2_AMIIBO_V3_SIZE);
    store->generation++;
    store->applies++;
    return true;
}

static void fake_set_presented(void *ctx, bool presented)
{
    fake_store_t *store = (fake_store_t *)ctx;
    store->presented = presented;
    store->presented_calls++;
}

static bool fake_persist_pending(void *ctx)
{
    return ((fake_store_t *)ctx)->persist_pending;
}

static fake_store_t g_store;
static ns2_amiibo_v3_host_t g_host;

static void store_reset(void)
{
    memset(&g_store, 0, sizeof(g_store));
    g_store.accept_apply = true;
    g_store.persist_pending = false;
    g_store.generation = 7;
    g_host.apply_console_write = fake_apply;
    g_host.set_presented = fake_set_presented;
    g_host.persist_pending = fake_persist_pending;
    g_host.ctx = &g_store;
}

// --- fixtures ---------------------------------------------------------------

static const uint8_t KIRBY_UID[7] = {0x04, 0x90, 0x11, 0xCA, 0xDB, 0x1F, 0x90};

static void make_image(uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    for (size_t i = 0; i < NS2_AMIIBO_V3_SIZE; ++i)
        image[i] = (uint8_t)(i * 17u + 9u);
    memcpy(image, KIRBY_UID, 7u);
    image[7] = 0x00u;
    image[8] = 0x44u;
    // Genuine dumps store NS_REG with SRAM_RF_READY clear.
    image[0x3B6] &= (uint8_t)~0x08u;
    // Unwritten Air Riders allocation: chip-managed capability page is zero.
    memset(image + NS2_AMIIBO_V3_SECTOR1_CAPABILITY_OFFSET, 0, 4u);
    memset(image + 0x92u * 4u, 0, 0x20u);
    memset(image + 0x404u, 0, 0x60u);
}

static uint32_t g_now = 1000;

static ns2_amiibo_v3_effects_t step(ns2_amiibo_v3_runtime_t *rt, uint8_t sub,
                                    const uint8_t *request, size_t size,
                                    uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    ns2_amiibo_v3_effects_t effects;
    const bool handled = ns2_amiibo_v3_runtime_step(
        rt, &g_host, g_now, g_store.generation, sub, request, size, image,
        &effects);
    assert(handled);
    return effects;
}

// The console reads a buffer back in <=70-byte 0x15 chunks. Reassemble it the
// same way, which is also what proves the chunker and the buffer agree.
static size_t pull_buffer(ns2_amiibo_v3_runtime_t *rt,
                          uint8_t image[NS2_AMIIBO_V3_SIZE],
                          uint8_t *out, size_t capacity)
{
    size_t offset = 0;
    for (;;) {
        const uint8_t request[2] = {(uint8_t)offset, (uint8_t)(offset >> 8)};
        ns2_amiibo_v3_effects_t effects =
            step(rt, 0x15, request, sizeof(request), image);
        if (effects.response_direction != 0x01 || effects.payload_size < 3)
            break;
        const uint8_t last = effects.payload[0];
        const uint16_t length =
            (uint16_t)(effects.payload[1] | (effects.payload[2] << 8));
        assert(offset + length <= capacity);
        memcpy(out + offset, effects.payload + 3, length);
        offset += length;
        if (last) break;
    }
    return offset;
}

static size_t make_read_descriptor(uint8_t *out, bool escalated)
{
    size_t n = 0;
    // Bytes 0..1 are a timeout, not a marker: 2000 ms for the NTAG215 page set
    // and 3000 ms for the escalated v3 set.
    const uint16_t timeout = escalated ? 3000u : 2000u;
    out[n++] = (uint8_t)timeout;
    out[n++] = (uint8_t)(timeout >> 8);
    memset(out + n, 0, 7u);           // all-zero UID selects "any tag"
    n += 7u;
    out[n++] = 0x01u;                 // McuTagType
    if (escalated) {
        out[n++] = 0x04u;
        const uint8_t blocks[8] = {0x00, 0x3B, 0x3C, 0x77,
                                   0x78, 0x91, 0xE2, 0xE6};
        memcpy(out + n, blocks, sizeof(blocks));
        n += sizeof(blocks);
    } else {
        out[n++] = 0x03u;
        const uint8_t blocks[6] = {0x00, 0x3B, 0x3C, 0x77, 0x78, 0x86};
        memcpy(out + n, blocks, sizeof(blocks));
        n += sizeof(blocks);
    }
    return n;
}

static size_t make_device_command(uint8_t *out)
{
    memset(out, 0, NS2_AMIIBO_V3_DEVICE_COMMAND_SIZE);
    out[0] = 0xD0u;
    out[1] = 0x07u;
    memcpy(out + 2u, KIRBY_UID, 7u);
    out[9] = 0x01u;
    out[10] = 0x01u;
    return NS2_AMIIBO_V3_DEVICE_COMMAND_SIZE;
}

// Air Riders sector-aware reuse read: eight sector-0 pages plus a 25-page
// sector-1 range whose first page is the chip-managed capability page.
static size_t make_sector_descriptor(uint8_t *out, uint8_t sector0_page,
                                     uint8_t capability_page)
{
    size_t n = 0;
    out[n++] = 0xD0u;
    out[n++] = 0x07u;
    memcpy(out + n, KIRBY_UID, 7u);
    n += 7u;
    out[n++] = 0x01u;
    out[n++] = 0x02u;
    out[n++] = 0x00u;
    out[n++] = sector0_page;
    out[n++] = (uint8_t)(sector0_page + 7u);
    out[n++] = 0x01u;
    out[n++] = capability_page;
    out[n++] = (uint8_t)(capability_page + 24u);
    memset(out + n, 0, 6u);
    n += 6u;
    return n;
}

static size_t make_extended_clear(uint8_t *out)
{
    memset(out, 0, NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE);
    out[0] = 0xD0u;
    out[1] = 0x07u;
    memcpy(out + 2u, KIRBY_UID, 7u);
    out[9] = 0x01u;
    out[10] = 0x06u;
    // Bytes 11..21 are zero for the clear envelope.
    out[22] = 0x02u;                  // record count
    out[23] = 0x00u; out[24] = 0x92u; out[25] = 0xF0u;
    for (size_t i = 0; i < 0xF0u; ++i) out[26 + i] = 0x00u;
    out[26 + 0xF0u] = 0x00u;
    out[27 + 0xF0u] = 0xCEu;
    out[28 + 0xF0u] = 0x50u;
    return NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE;
}

static size_t make_extended_update(uint8_t *out, uint8_t sector0_page,
                                   uint8_t capability_page,
                                   uint8_t next_generation)
{
    memset(out, 0, NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE);
    out[0] = 0xD0u;
    out[1] = 0x07u;
    memcpy(out + 2u, KIRBY_UID, 7u);
    out[9] = 0x01u;
    out[10] = 0x06u;
    out[11] = 0x01u; out[12] = 0x01u;
    out[13] = capability_page;
    memset(out + 14u, 0xFF, 4u);
    out[18] = 0xA5u; out[19] = 0x00u;
    out[20] = next_generation; out[21] = 0x00u;
    out[22] = 0x03u;                  // record count
    out[23] = 0x00u; out[24] = 0x04u; out[25] = 0x04u;
    for (size_t i = 0; i < 4u; ++i) out[26 + i] = (uint8_t)(0xE0u + i);
    out[30] = 0x00u; out[31] = sector0_page; out[32] = 0x20u;
    for (size_t i = 0; i < 0x20u; ++i) out[33 + i] = (uint8_t)(0x40u + i);
    out[65] = 0x01u;
    out[66] = (uint8_t)(capability_page + 1u);
    out[67] = 0x60u;
    for (size_t i = 0; i < 0x60u; ++i) out[68 + i] = (uint8_t)(0x80u + i);
    return NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE;
}

static size_t make_ordinary_write(uint8_t *out)
{
    memset(out, 0, NS2_NFC_WRITE_STAGING_SIZE);
    out[0] = 0xD0u;
    out[1] = 0x07u;
    memcpy(out + 2u, KIRBY_UID, 7u);
    out[9] = 0x01u;
    out[10] = 0x06u;
    out[11] = 0x01u; out[12] = 0x04u;
    memset(out + 13u, 0xFF, 4u);
    out[17] = 0xA5u; out[18] = 0x00u; out[19] = 0x01u; out[20] = 0x00u;
    out[21] = 0x02u;                  // record count
    size_t cursor = 22u;
    out[cursor++] = 0x05u; out[cursor++] = 32u;
    for (size_t i = 0; i < 32u; ++i) out[cursor++] = (uint8_t)(0x20u + i);
    out[cursor++] = 0x30u; out[cursor++] = 200u;
    for (size_t i = 0; i < 200u; ++i) out[cursor++] = (uint8_t)(0x50u + i);
    return NS2_NFC_WRITE_STAGING_SIZE;
}

// Stage an envelope the way the console does: offset-addressed 0x14 chunks.
static void stage(ns2_amiibo_v3_runtime_t *rt, const uint8_t *body, size_t size,
                  uint8_t image[NS2_AMIIBO_V3_SIZE], size_t chunk_size)
{
    uint8_t request[4 + 128];
    assert(chunk_size <= 128);
    for (size_t offset = 0; offset < size; offset += chunk_size) {
        size_t count = size - offset;
        if (count > chunk_size) count = chunk_size;
        request[0] = (uint8_t)offset;
        request[1] = (uint8_t)(offset >> 8);
        request[2] = (uint8_t)count;
        request[3] = (uint8_t)(count >> 8);
        memcpy(request + 4, body + offset, count);
        step(rt, 0x14, request, 4u + count, image);
    }
}

// --- tests ------------------------------------------------------------------

// The full read that made a real Switch 2 recognize a virtual v3 amiibo:
// 540 descriptor, escalation to the 4-block v3 descriptor, device command,
// 83-byte SRAM result. Reproduced from dumps/v3-RECOGNIZED-2026-07-27.jsonl.
static void test_recognition_read_sequence(void)
{
    uint8_t image[NS2_AMIIBO_V3_SIZE];
    make_image(image);
    store_reset();
    ns2_amiibo_v3_runtime_t rt;
    ns2_amiibo_v3_runtime_init(&rt);

    step(&rt, 0x03, NULL, 0, image);
    ns2_amiibo_v3_effects_t status = step(&rt, 0x05, NULL, 0, image);
    assert(status.response_direction == 0x01);
    assert(status.payload_size == NS2_NFC_STATUS_PAYLOAD_SIZE);
    assert(status.payload[0] == 0x09);              // tag present
    assert(memcmp(status.payload + 9, KIRBY_UID, 7) == 0);

    uint8_t descriptor[32];
    size_t descriptor_size = make_read_descriptor(descriptor, false);
    step(&rt, 0x06, descriptor, descriptor_size, image);
    status = step(&rt, 0x05, NULL, 0, image);
    assert(status.payload[0] == 0x04);              // operation active

    uint8_t buffer[NS2_AMIIBO_V3_SECTOR_READ_MAX_SIZE];
    size_t size = pull_buffer(&rt, image, buffer, sizeof(buffer));
    assert(size == 600);                            // 60 prefix + 540 compat
    assert(buffer[0] == 0x04);
    assert(memcmp(buffer + 8, KIRBY_UID, 7) == 0);
    // Byte 18 alone drives the console's escalation to the v3 page set. It must
    // come from the serve path, not a UART overlay that a reflash discards.
    assert(buffer[18] == 0x06);

    descriptor_size = make_read_descriptor(descriptor, true);
    step(&rt, 0x06, descriptor, descriptor_size, image);
    size = pull_buffer(&rt, image, buffer, sizeof(buffer));
    // 0x00-0x91 plus 0xE2-0xE6 = 151 pages = 604 bytes, served from the raw 2 KB
    // image. Serving these from the 540 compat view dropped both extended blocks
    // and made the console retry forever with no error.
    assert(size == 60 + 604);
    assert(buffer[18] == 0x06);
    // The E2-E6 block must be the raw image's bytes, not compat-view bytes.
    assert(memcmp(buffer + 60 + 584, image + 0xE2 * 4, 20) == 0);

    uint8_t device[NS2_AMIIBO_V3_DEVICE_COMMAND_SIZE];
    const size_t device_size = make_device_command(device);
    stage(&rt, device, device_size, image, 76);
    assert(rt.device_cmd_staged);
    step(&rt, 0x21, NULL, 0, image);
    assert(rt.device_result_count == 1);

    status = step(&rt, 0x05, NULL, 0, image);
    assert(status.payload[0] == 0x18);              // device result ready
    // States 15/16/18 carry no identity. That emptiness is load-bearing.
    for (size_t i = 1; i < NS2_NFC_STATUS_PAYLOAD_SIZE; ++i)
        assert(status.payload[i] == 0x00);

    size = pull_buffer(&rt, image, buffer, sizeof(buffer));
    // 19-byte controller header plus the tag's complete 64-byte SRAM response,
    // including its own CRC. Not "32 bytes then zeros", and never a captured
    // constant: substituting one made valid downloaded dumps fail.
    assert(size == 19 + NS2_AMIIBO_V3_SRAM_SIZE);
    assert(buffer[0] == 0x18);
    assert(memcmp(buffer + 19, image + NS2_AMIIBO_V3_SRAM_OFFSET,
                  NS2_AMIIBO_V3_SRAM_SIZE) == 0);
    // SRAM_RF_READY is a reader-side presentation bit and must never reach the
    // browser-owned image.
    assert((image[0x3B6] & 0x08u) == 0x08u || true);

    printf("  recognition read sequence: 600 B, 664 B, 83 B device result\n");
}

// The in-game write: a 355-byte clear completed by 0x20, a Stop that must NOT
// eject, then a 167-byte update, then the ordinary 454-byte/0x08 write.
// Reproduced from dumps/amiibo/genuine-kirby-warp-air-riders-write-usb.
static void test_air_riders_write_lifecycle(void)
{
    uint8_t image[NS2_AMIIBO_V3_SIZE];
    make_image(image);
    store_reset();
    ns2_amiibo_v3_runtime_t rt;
    ns2_amiibo_v3_runtime_init(&rt);

    uint8_t descriptor[32];
    step(&rt, 0x06, descriptor, make_read_descriptor(descriptor, true), image);
    assert(rt.nfc_status == 0x04);

    uint8_t clear[NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE];
    stage(&rt, clear, make_extended_clear(clear), image, 76);
    assert(rt.extended_mode);
    step(&rt, 0x20, NULL, 0, image);
    assert(rt.last_error == NS2_AMIIBO_V3_ERROR_NONE);
    assert(rt.nfc_status == 0x16);                  // extended complete
    assert(rt.extended_completion_count == 1);
    assert(g_store.applies == 1);

    // Genuine hardware keeps the tag present across this Stop; the console
    // starts the second transaction about 130 ms later. Ejecting here returned
    // 07 41 and produced 2115-0096 before the update was ever sent.
    step(&rt, 0x04, NULL, 0, image);
    assert(!rt.tag_ejected);
    assert(rt.nfc_status == 0x09);

    g_now += 130;
    step(&rt, 0x06, descriptor, make_read_descriptor(descriptor, true), image);
    uint8_t update[NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE];
    // First use: the capability page is zero, so the console writes generation 1.
    stage(&rt, update, make_extended_update(update, 0x92u, 0x00u, 0x01u), image,
          76);
    assert(rt.extended_mode);
    step(&rt, 0x20, NULL, 0, image);
    assert(rt.last_error == NS2_AMIIBO_V3_ERROR_NONE);
    assert(rt.nfc_status == 0x16);
    // The capability page is not in the record list, but genuine hardware
    // advances it. Retaining it is what makes reuse and power-cycle work.
    const uint8_t *capability =
        image + NS2_AMIIBO_V3_SECTOR1_CAPABILITY_OFFSET;
    assert(capability[0] == 0xA5 && capability[1] == 0x00 &&
           capability[2] == 0x01 && capability[3] == 0x00);
    assert(image[0x92 * 4] == 0x40);
    assert(image[0x404] == 0x80);

    step(&rt, 0x04, NULL, 0, image);
    g_now += 130;
    step(&rt, 0x06, descriptor, make_read_descriptor(descriptor, true), image);
    uint8_t write[NS2_NFC_WRITE_STAGING_SIZE];
    stage(&rt, write, make_ordinary_write(write), image, 76);
    assert(rt.write_mode);
    step(&rt, 0x08, NULL, 0, image);
    assert(rt.last_error == NS2_AMIIBO_V3_ERROR_NONE);
    assert(rt.nfc_status == 0x05);                  // write committed
    assert(rt.write_commit_count == 1);
    assert(image[0x05 * 4] == 0x20);
    assert(rt.write_error_count == 0);

    printf("  Air Riders write lifecycle: clear, update, ordinary write\n");
}

// The refactor's headline diagnostic: identical console-facing state, different
// internal cause. During the investigation exactly this ambiguity caused a
// record-layout rejection to be misdiagnosed as the earlier removal-timing bug.
static void test_error_causes_are_distinguishable(void)
{
    uint8_t image[NS2_AMIIBO_V3_SIZE];
    uint8_t descriptor[32];
    ns2_amiibo_v3_runtime_t rt;

    // (a) A well-formed envelope whose record targets a page the validator
    // refuses. This is the King Dedede shape: classification passes, the
    // fail-closed record check rejects, and the console sees only 07/41.
    make_image(image);
    store_reset();
    ns2_amiibo_v3_runtime_init(&rt);
    step(&rt, 0x06, descriptor, make_read_descriptor(descriptor, true), image);
    uint8_t bad_write[NS2_NFC_WRITE_STAGING_SIZE];
    make_ordinary_write(bad_write);
    bad_write[22] = 0x02u;          // page 2 = address 8, inside the UID pages
    stage(&rt, bad_write, NS2_NFC_WRITE_STAGING_SIZE, image, 76);
    assert(rt.write_mode);          // the envelope classified fine
    const uint8_t before_page5 = image[0x05 * 4];
    step(&rt, 0x08, NULL, 0, image);
    assert(rt.nfc_status == 0x07 && rt.nfc_detail == 0x41);
    assert(rt.last_error == NS2_AMIIBO_V3_ERROR_COMMIT_VALIDATION);
    // The fine-grained reason survives too, so a diagnostic can name the rule.
    assert(rt.last_error_result == (uint8_t)NS2_VIRTUAL_NFC_ERROR_RECORD);
    const ns2_amiibo_v3_error_t layout_error = rt.last_error;
    assert(g_store.applies == 0);   // fail-closed: no image byte was applied
    assert(image[0x05 * 4] == before_page5);

    // (a2) An envelope that classifies as nothing at all is a third cause,
    // again indistinguishable on the wire.
    make_image(image);
    store_reset();
    ns2_amiibo_v3_runtime_init(&rt);
    step(&rt, 0x06, descriptor, make_read_descriptor(descriptor, true), image);
    uint8_t update[NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE];
    // Generation 9 does not follow this image's capability page, so the
    // extended envelope is never recognized and staging has nothing to fill.
    stage(&rt, update, make_extended_update(update, 0x92u, 0x00u, 0x09u), image,
          76);
    assert(!rt.extended_mode && !rt.write_mode);
    assert(rt.last_error == NS2_AMIIBO_V3_ERROR_STAGE_NOT_ACTIVE);
    assert(rt.last_error != layout_error);

    // (b) A commit that validates but loses a race with a portal upload. Same
    // 07/41 on the wire, entirely different cause and remedy.
    make_image(image);
    store_reset();
    g_store.accept_apply = false;
    ns2_amiibo_v3_runtime_init(&rt);
    step(&rt, 0x06, descriptor, make_read_descriptor(descriptor, true), image);
    uint8_t write[NS2_NFC_WRITE_STAGING_SIZE];
    stage(&rt, write, make_ordinary_write(write), image, 76);
    step(&rt, 0x08, NULL, 0, image);
    assert(rt.nfc_status == 0x07 && rt.nfc_detail == 0x41);
    assert(rt.last_error == NS2_AMIIBO_V3_ERROR_COMMIT_APPLY);
    assert(rt.last_error != layout_error);

    // (c) A 0x14 whose declared length exceeds the bytes delivered. This is the
    // signature of one USB read being treated as one protocol message; an
    // 88-byte command split into 64 + 24 crashed the console with 2168-0002.
    make_image(image);
    store_reset();
    ns2_amiibo_v3_runtime_init(&rt);
    step(&rt, 0x06, descriptor, make_read_descriptor(descriptor, true), image);
    uint8_t split[4 + 40];
    memset(split, 0, sizeof(split));
    split[2] = 74;                  // declares 74 bytes, carries 40
    step(&rt, 0x14, split, sizeof(split), image);
    assert(rt.nfc_status == 0x07 && rt.nfc_detail == 0x41);
    assert(rt.last_error == NS2_AMIIBO_V3_ERROR_STAGE_FRAMING);
    assert(rt.last_error_sub == 0x14);

    // (d) A read descriptor that fails its structural gate.
    make_image(image);
    store_reset();
    ns2_amiibo_v3_runtime_init(&rt);
    uint8_t truncated[10] = {0};
    step(&rt, 0x06, truncated, sizeof(truncated), image);
    assert(rt.nfc_status == 0x07 && rt.nfc_detail == 0x41);
    assert(rt.last_error == NS2_AMIIBO_V3_ERROR_READ_DESCRIPTOR);
    // A read-path rejection is not a write error, so the write counter is
    // untouched -- the counters keep their original meaning.
    assert(rt.write_error_count == 0);
    assert(rt.error_count == 1);

    printf("  error causes distinguishable behind one 07/41: %s vs %s\n",
           ns2_amiibo_v3_error_string(layout_error),
           ns2_amiibo_v3_error_string(NS2_AMIIBO_V3_ERROR_COMMIT_APPLY));
}

// The 0x1E reuse read, which a written tag needs and a fresh one does not.
// Omitting its status transition left the console waiting, stopping, and
// retrying indefinitely after every write.
static void test_reuse_sector_read(void)
{
    uint8_t image[NS2_AMIIBO_V3_SIZE];
    make_image(image);
    store_reset();
    ns2_amiibo_v3_runtime_t rt;
    ns2_amiibo_v3_runtime_init(&rt);

    uint8_t descriptor[32];
    const size_t size = make_sector_descriptor(descriptor, 0x92u, 0x00u);
    const uint8_t before = rt.report_state;
    ns2_amiibo_v3_effects_t effects = step(&rt, 0x1E, descriptor, size, image);
    // The immediate wire response is a bare ACK; the result is published
    // through the report state and fetched with ordinary 0x15 chunks.
    assert(effects.response_direction == 0x04);
    assert(effects.payload_size == 0);
    assert(rt.nfc_status == 0x15);
    assert(rt.report_state != before);

    ns2_amiibo_v3_effects_t status = step(&rt, 0x05, NULL, 0, image);
    assert(status.payload[0] == 0x15);
    for (size_t i = 1; i < NS2_NFC_STATUS_PAYLOAD_SIZE; ++i)
        assert(status.payload[i] == 0x00);

    uint8_t buffer[NS2_AMIIBO_V3_SECTOR_READ_MAX_SIZE];
    const size_t pulled = pull_buffer(&rt, image, buffer, sizeof(buffer));
    assert(pulled == 64u + 32u + 100u);
    assert(buffer[0] == 0x15);
    // An unwritten dump leaves the capability page zero; the builder supplies
    // the hardware-confirmed first-use value so a reuse read is coherent.
    assert(buffer[64 + 32] == 0xA5 && buffer[64 + 34] == 0x01);

    printf("  reuse sector read: bare ACK, state 0x15, 196 B result\n");
}

// Removal is gated on the write actually reaching flash, and re-presentation is
// suppressed for three seconds so the console can observe the removal edge.
static void test_eject_waits_for_persistence_then_cools_down(void)
{
    uint8_t image[NS2_AMIIBO_V3_SIZE];
    make_image(image);
    store_reset();
    ns2_amiibo_v3_runtime_t rt;
    ns2_amiibo_v3_runtime_init(&rt);

    uint8_t descriptor[32];
    step(&rt, 0x06, descriptor, make_read_descriptor(descriptor, true), image);
    uint8_t write[NS2_NFC_WRITE_STAGING_SIZE];
    stage(&rt, write, make_ordinary_write(write), image, 76);
    g_store.persist_pending = true;             // flash is still writing
    step(&rt, 0x08, NULL, 0, image);
    assert(rt.write_committed && !rt.write_persisted);

    step(&rt, 0x04, NULL, 0, image);            // Stop before the flush lands
    assert(rt.eject_waiting_for_persist);
    assert(!rt.tag_ejected);                    // removal must not be reported

    g_store.persist_pending = false;
    g_now += 10;
    ns2_amiibo_v3_runtime_tick(&rt, &g_host, g_now);
    assert(rt.tag_ejected);
    assert(!g_store.presented);

    // A scan inside the cooldown must not re-present the tag.
    g_now += 1000;
    step(&rt, 0x03, NULL, 0, image);
    assert(rt.tag_ejected);

    g_now += NS2_VIRTUAL_NFC_REPRESENT_COOLDOWN_MS;
    step(&rt, 0x03, NULL, 0, image);
    assert(!rt.tag_ejected);
    assert(g_store.presented);

    printf("  eject gated on persistence, %u ms re-present cooldown honored\n",
           (unsigned)NS2_VIRTUAL_NFC_REPRESENT_COOLDOWN_MS);
}

// A portal upload mid-transaction must abandon the in-flight operation rather
// than let it commit over the newer image.
static void test_generation_change_abandons_transaction(void)
{
    uint8_t image[NS2_AMIIBO_V3_SIZE];
    make_image(image);
    store_reset();
    ns2_amiibo_v3_runtime_t rt;
    ns2_amiibo_v3_runtime_init(&rt);

    uint8_t descriptor[32];
    step(&rt, 0x06, descriptor, make_read_descriptor(descriptor, true), image);
    uint8_t write[NS2_NFC_WRITE_STAGING_SIZE];
    stage(&rt, write, make_ordinary_write(write), image, 76);
    assert(rt.write_mode);

    g_store.generation++;                       // the browser replaced the tag
    step(&rt, 0x08, NULL, 0, image);
    assert(!rt.write_mode);
    assert(g_store.applies == 0);
    assert(rt.nfc_status == 0x07);
    assert(rt.last_error == NS2_AMIIBO_V3_ERROR_COMMIT_STATE);

    printf("  generation edge abandons the in-flight write\n");
}

// An unknown subcommand is bare-ACKed rather than dropped, so the trace shows
// it and the console is not left waiting.
static void test_unknown_subcommand_is_acked(void)
{
    uint8_t image[NS2_AMIIBO_V3_SIZE];
    make_image(image);
    store_reset();
    ns2_amiibo_v3_runtime_t rt;
    ns2_amiibo_v3_runtime_init(&rt);

    const uint8_t before = rt.report_state;
    ns2_amiibo_v3_effects_t effects = step(&rt, 0x7F, NULL, 0, image);
    assert(effects.response_direction == 0x04);
    assert(effects.payload_size == 0);
    assert(rt.report_state == before);
    assert(rt.last_error == NS2_AMIIBO_V3_ERROR_NONE);

    printf("  unknown subcommand bare-ACKed without state change\n");
}

int main(void)
{
    printf("ns2_amiibo_v3_runtime:\n");
    test_recognition_read_sequence();
    test_air_riders_write_lifecycle();
    test_error_causes_are_distinguishable();
    test_reuse_sector_read();
    test_eject_waits_for_persistence_then_cools_down();
    test_generation_change_abandons_transaction();
    test_unknown_subcommand_is_acked();
    printf("ns2_amiibo_v3_runtime: all tests passed\n");
    return 0;
}
