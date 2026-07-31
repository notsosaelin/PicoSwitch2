#include "ns2_amiibo_v3_runtime.h"

#include <string.h>

// Only for NS2_VIRTUAL_NFC_REPRESENT_COOLDOWN_MS. The three-second suppression
// after a logical removal is one behavior shared with the 540 path, so it keeps
// one definition rather than a second constant that could drift.
#include "ns2_virtual_nfc_runtime.h"

// Extracted verbatim from ns2_v3_serve() in src/switch_pro2/switch_pro2.c.
// Behavior is intentionally unchanged: the same conditions in the same order
// produce the same responses, status edges, and report-counter bumps. The only
// deliberate additions are the internal error enum -- which records *why* a
// failure produced the single console-facing 07/41 pair -- and the host
// interface that replaces three direct store calls.

#define NS2_AMIIBO_V3_SECTOR0_SIZE 1024u
// NTAG I2C 2K session register NS_REG lives in page 0xED (byte offset 0xED*4 =
// 0x3B4); its byte 2 carries SRAM_RF_READY (bit 0x08).
#define NS2_AMIIBO_V3_NS_REG_OFFSET 0x3B6u
#define NS2_AMIIBO_V3_SRAM_RF_READY 0x08u

// A v3 tag is a standard amiibo with 64 bytes of extra data inserted at 0x80.
// Removing that block restores the classic layout, so the standard
// tag_to_internal produces a byte-identical internal buffer and the HMACs
// verify unchanged. This is the same backwards-compatibility path the Switch's
// own NFC sysmodule takes.
#define NS2_AMIIBO_V3_COMPAT_SPLIT 0x80u
#define NS2_AMIIBO_V3_COMPAT_SHIFT 0x40u

const char *ns2_amiibo_v3_error_string(ns2_amiibo_v3_error_t error)
{
    switch (error) {
        case NS2_AMIIBO_V3_ERROR_NONE: return "none";
        case NS2_AMIIBO_V3_ERROR_READ_DESCRIPTOR: return "read_descriptor";
        case NS2_AMIIBO_V3_ERROR_SECTOR_READ: return "sector_read";
        case NS2_AMIIBO_V3_ERROR_STAGE_FRAMING: return "stage_framing";
        case NS2_AMIIBO_V3_ERROR_STAGE_NOT_ACTIVE: return "stage_not_active";
        case NS2_AMIIBO_V3_ERROR_STAGE_CHUNK: return "stage_chunk";
        case NS2_AMIIBO_V3_ERROR_COMMIT_STATE: return "commit_state";
        case NS2_AMIIBO_V3_ERROR_COMMIT_VALIDATION: return "commit_validation";
        case NS2_AMIIBO_V3_ERROR_COMMIT_APPLY: return "commit_apply";
        default: return "unknown";
    }
}

static void host_set_presented(const ns2_amiibo_v3_host_t *host, bool presented)
{
    if (host && host->set_presented) host->set_presented(host->ctx, presented);
}

static bool host_persist_pending(const ns2_amiibo_v3_host_t *host)
{
    return host && host->persist_pending && host->persist_pending(host->ctx);
}

static bool host_apply_console_write(const ns2_amiibo_v3_host_t *host,
                                     const uint8_t image[NS2_AMIIBO_V3_SIZE],
                                     uint32_t generation)
{
    if (!host || !host->apply_console_write) return false;
    return host->apply_console_write(host->ctx, image, generation);
}

static void bump_report_state(ns2_amiibo_v3_runtime_t *rt)
{
    rt->report_state = (uint8_t)((rt->report_state + 1u) & 0x07u);
}

void ns2_amiibo_v3_runtime_reset_transaction(ns2_amiibo_v3_runtime_t *rt)
{
    if (!rt) return;
    rt->operation_active = false;
    rt->nfc_status = 0x09u;
    rt->nfc_detail = 0x00u;
    rt->device_cmd_staged = false;
    rt->write_mode = false;
    rt->extended_mode = false;
    rt->extended_expected_size = 0;
    ns2_amiibo_v3_extended_sequence_reset(&rt->extended_sequence);
    rt->write_committed = false;
    rt->write_persisted = true;
    rt->eject_waiting_for_persist = false;
    rt->tag_ejected = false;
    rt->represent_after_ms = 0;
    rt->write_event_pending = false;
    rt->op_buffer_size = 0;
    ns2_virtual_nfc_write_init(&rt->write);
}

void ns2_amiibo_v3_runtime_init(ns2_amiibo_v3_runtime_t *rt)
{
    if (!rt) return;
    memset(rt, 0, sizeof(*rt));
    rt->serve_mode = 1u;
    rt->report_state = 0;
    rt->observed_generation = 0;
    rt->observed_generation_valid = false;
    ns2_amiibo_v3_runtime_reset_transaction(rt);
}

uint8_t ns2_amiibo_v3_runtime_report_state(const ns2_amiibo_v3_runtime_t *rt)
{
    return rt ? rt->report_state : 0u;
}

// The single console-facing failure state, plus the internal reason the wire
// cannot carry. `write_error` preserves the original counter semantics: only
// data-path failures increment write_error_count.
static void set_error(ns2_amiibo_v3_runtime_t *rt, uint32_t now_ms,
                      uint8_t subcommand, ns2_amiibo_v3_error_t error,
                      ns2_virtual_nfc_result_t result, uint16_t offset,
                      bool write_error)
{
    rt->operation_active = false;
    rt->nfc_status = 0x07u;
    rt->nfc_detail = 0x41u;
    rt->device_cmd_staged = false;
    rt->write_mode = false;
    rt->extended_mode = false;
    rt->extended_expected_size = 0;
    ns2_amiibo_v3_extended_sequence_reset(&rt->extended_sequence);
    rt->write_committed = false;
    rt->write_event_pending = false;
    ns2_virtual_nfc_write_cancel(&rt->write);
    bump_report_state(rt);
    if (write_error) rt->write_error_count++;

    rt->last_error = error;
    rt->last_error_sub = subcommand;
    rt->last_error_result = (uint8_t)result;
    rt->last_error_offset = offset;
    rt->last_error_ms = now_ms;
    rt->error_count++;
}

static void finish_committed_eject(ns2_amiibo_v3_runtime_t *rt,
                                   const ns2_amiibo_v3_host_t *host,
                                   uint32_t now_ms)
{
    rt->eject_waiting_for_persist = false;
    rt->write_committed = false;
    rt->write_event_pending = false;
    rt->tag_ejected = true;
    ns2_amiibo_v3_extended_sequence_reset(&rt->extended_sequence);
    rt->represent_after_ms = now_ms + NS2_VIRTUAL_NFC_REPRESENT_COOLDOWN_MS;
    rt->operation_active = false;
    rt->nfc_status = 0x07u;
    rt->nfc_detail = 0x41u;
    host_set_presented(host, false);
    bump_report_state(rt);
}

void ns2_amiibo_v3_runtime_tick(ns2_amiibo_v3_runtime_t *rt,
                                const ns2_amiibo_v3_host_t *host,
                                uint32_t now_ms)
{
    if (!rt) return;
    ns2_amiibo_v3_extended_sequence_expire(&rt->extended_sequence, now_ms);
    if (rt->write_event_pending &&
        (int32_t)(now_ms - rt->write_event_due_ms) >= 0) {
        rt->write_event_pending = false;
        bump_report_state(rt);
    }
    if (rt->write_committed && !rt->write_persisted &&
        !host_persist_pending(host)) {
        rt->write_persisted = true;
        if (rt->eject_waiting_for_persist)
            finish_committed_eject(rt, host, now_ms);
    }
}

static void build_compat540(const uint8_t image[NS2_AMIIBO_V3_SIZE],
                            uint8_t out[VIRTUAL_AMIIBO_RAW_SIZE])
{
    memcpy(out, image, NS2_AMIIBO_V3_COMPAT_SPLIT);
    memcpy(out + NS2_AMIIBO_V3_COMPAT_SPLIT,
           image + NS2_AMIIBO_V3_COMPAT_SPLIT + NS2_AMIIBO_V3_COMPAT_SHIFT,
           VIRTUAL_AMIIBO_RAW_SIZE - NS2_AMIIBO_V3_COMPAT_SPLIT);
}

// Result buffer for the 0x14/0x21 device command, byte-for-byte from the
// genuine capture of 2026-07-27: a 19-byte controller header followed by all 64
// SRAM bytes. The response is not "32 bytes plus a fixed zero/7A-C4 tail" --
// the CRC in bytes 62..63 is per tag, and substituting a captured constant made
// otherwise-valid images fail the console's device-response validation.
static void build_device_result(ns2_amiibo_v3_runtime_t *rt,
                                const uint8_t image[NS2_AMIIBO_V3_SIZE],
                                const uint8_t uid[7])
{
    uint8_t *out = rt->op_buffer;
    memset(out, 0, NS2_AMIIBO_V3_DEVICE_RESULT_SIZE);
    out[0] = 0x18;
    out[4] = 0x01;
    out[5] = 0x02;
    out[7] = 0x07;
    memcpy(out + 8, uid, 7u);
    out[18] = 0x06;
    ns2_amiibo_v3_sram_response(image, out + 19);
    rt->op_buffer_size = NS2_AMIIBO_V3_DEVICE_RESULT_SIZE;
}

// Assemble the buffer the console pulls with 0x15.
//
// The 0x06 read descriptor decodes as:
//   timeout:u16le | uid[7] | McuTagType | block_count | (start,end) x N
// so the correct reply is the requested page set, not a fixed-size image.
//
// Source selection matters: once the prefix advertises a v3 tag the console
// escalates to a 4-block descriptor reaching bytes 584 and 924, both past the
// 540-byte compatibility view. Serving those from compat made every extended
// block fail its bounds check and get skipped, and the console retried forever
// with no error and no recognition. Scan the descriptor first, and if anything
// reaches past the compat view, serve the raw 2 KB image.
static void build_buffer(ns2_amiibo_v3_runtime_t *rt,
                         const uint8_t image[NS2_AMIIBO_V3_SIZE],
                         const uint8_t *request, size_t request_size)
{
    size_t highest = 0;
    if (request_size >= 13u) {
        const uint8_t blocks = request[10];
        if (blocks && 11u + (size_t)blocks * 2u <= request_size) {
            for (uint8_t b = 0; b < blocks; ++b) {
                const uint8_t st = request[11u + (size_t)b * 2u];
                const uint8_t en = request[12u + (size_t)b * 2u];
                if (en < st) continue;
                const size_t end_byte = ((size_t)en + 1u) * 4u;
                if (end_byte > highest) highest = end_byte;
            }
        }
    }

    static uint8_t compat[VIRTUAL_AMIIBO_RAW_SIZE];
    const uint8_t *source = image;
    size_t source_size = NS2_AMIIBO_V3_SIZE;
    if (rt->serve_mode == 1u && highest <= VIRTUAL_AMIIBO_RAW_SIZE) {
        build_compat540(image, compat);
        source = compat;
        source_size = VIRTUAL_AMIIBO_RAW_SIZE;
    }

    uint8_t *out = rt->op_buffer;
    memset(out, 0, 60);
    // Byte-for-byte the genuine prefix confirmed by primary capture; the console
    // believes this is an NTAG215 and nothing here may claim otherwise.
    out[0] = 0x04;
    out[4] = 0x01;
    out[5] = 0x02;
    out[6] = 0x00;
    out[7] = 0x07;                          // UID length
    ns2_amiibo_v3_uid(image, out + 8);
    // The chip-identity byte. 0x06 is what a genuine controller reports for a v3
    // tag, and it alone drives the console's escalation from the NTAG215 page
    // set to the 4-block v3 descriptor. It belongs here and not in a UART
    // overlay, which does not survive a reflash.
    out[18] = 0x06;
    // out[19..50] is the tag's 32-byte originality signature (READ_SIG), which
    // the controller obtains from the chip -- NOT the SRAM window. A 2048-byte
    // dump does not carry it, so the served value is whatever was set over UART.
    if (rt->signature_set)
        memcpy(out + 19, rt->signature, NS2_AMIIBO_V3_SIGNATURE_SIZE);
    if (request_size >= 19u)
        memcpy(out + 51, request + 10, NS2_NFC_OPERATION_METADATA_SIZE);
    // RE probe: overlay prefix bytes so the chip identity can be swept live.
    for (size_t i = 0; i < NS2_AMIIBO_V3_PREFIX_SIZE; ++i)
        if (rt->hdr_probe_mask[i]) out[i] = rt->hdr_probe_value[i];

    // Copy exactly the page ranges the console asked for. Falls back to sector 0
    // if the descriptor cannot be parsed.
    size_t used = 60u;
    bool copied = false;
    if (request_size >= 13u) {
        const uint8_t blocks = request[10];
        const size_t first = 11u;
        if (blocks && first + (size_t)blocks * 2u <= request_size) {
            for (uint8_t b = 0; b < blocks; ++b) {
                const uint8_t start = request[first + (size_t)b * 2u];
                const uint8_t end = request[first + (size_t)b * 2u + 1u];
                if (end < start) continue;
                const size_t from = (size_t)start * 4u;
                const size_t len = ((size_t)(end - start) + 1u) * 4u;
                if (from + len > source_size ||
                    used + len > sizeof(rt->op_buffer))
                    continue;
                memcpy(out + used, source + from, len);
                used += len;
                copied = true;
            }
        }
    }
    if (!copied) {
        const size_t fallback = (source_size < NS2_AMIIBO_V3_SECTOR0_SIZE)
            ? source_size : NS2_AMIIBO_V3_SECTOR0_SIZE;
        memcpy(out + 60, source, fallback);
        used = 60u + fallback;
    }
    rt->op_buffer_size = used;
}

bool ns2_amiibo_v3_runtime_step(ns2_amiibo_v3_runtime_t *rt,
                                const ns2_amiibo_v3_host_t *host,
                                uint32_t now_ms, uint32_t generation,
                                uint8_t sub, const uint8_t *request,
                                size_t request_size,
                                uint8_t image[NS2_AMIIBO_V3_SIZE],
                                ns2_amiibo_v3_effects_t *effects)
{
    if (!rt || !image || !effects) return false;
    memset(effects, 0, sizeof(*effects));
    effects->response_direction = 0x04; // bare ACK unless a data reply is made

    // A portal upload/sync can replace the selected image while no NFC
    // transaction is active. Treat its generation edge as fresh media rather
    // than allowing an older console operation to commit over it.
    if (!rt->observed_generation_valid) {
        rt->observed_generation = generation;
        rt->observed_generation_valid = true;
    } else if (generation != rt->observed_generation) {
        ns2_amiibo_v3_runtime_reset_transaction(rt);
        rt->observed_generation = generation;
        host_set_presented(host, true);
    }

    ns2_amiibo_v3_runtime_tick(rt, host, now_ms);

    // Genuine dumps store NS_REG with SRAM_RF_READY CLEAR. The Switch 2 polls
    // that bit and only reads the SRAM window once it is SET, so raise it on
    // this served copy the way pixl.js raises it per read. The stored flash
    // image is never mutated. Serving it clear left the console waiting on SRAM
    // that never signalled ready, which is the 2011-0301 crash.
    const uint8_t stored_ns_reg = image[NS2_AMIIBO_V3_NS_REG_OFFSET];
    image[NS2_AMIIBO_V3_NS_REG_OFFSET] |= NS2_AMIIBO_V3_SRAM_RF_READY;

    uint8_t *payload = effects->payload;
    size_t payload_size = 0;
    uint8_t direction = 0x04;

    uint8_t uid[7];
    ns2_amiibo_v3_uid(image, uid);

    switch (sub) {
        case 0x03: // scan: (re)present the tag, ready state, signal the console
            if (rt->tag_ejected &&
                (int32_t)(now_ms - rt->represent_after_ms) >= 0) {
                rt->tag_ejected = false;
                host_set_presented(host, true);
            }
            rt->operation_active = false;
            if (rt->write_mode || rt->extended_mode) {
                ns2_virtual_nfc_write_cancel(&rt->write);
                rt->write_mode = false;
                rt->extended_mode = false;
                rt->extended_expected_size = 0;
            }
            if (!rt->write_committed) {
                rt->nfc_status = 0x09;
                rt->nfc_detail = 0x00;
            }
            bump_report_state(rt);
            break;
        case 0x04: { // stop: close the operation; eject a committed write
            const bool completed_write = rt->write_committed;
            const bool continue_extended_sequence =
                completed_write &&
                ns2_amiibo_v3_extended_sequence_continue_after_write(
                    &rt->extended_sequence, now_ms);
            rt->operation_active = false;
            rt->device_cmd_staged = false;
            if (rt->write_mode || rt->extended_mode)
                ns2_virtual_nfc_write_cancel(&rt->write);
            rt->write_mode = false;
            rt->extended_mode = false;
            rt->extended_expected_size = 0;
            if (continue_extended_sequence) {
                // Genuine hardware keeps the tag available here. In the positive
                // capture the console starts the second transaction about 130 ms
                // after this Stop. Ejecting after the clear-stage ordinary write
                // returns 07 41 instead and produces 2115-0096 before the
                // 167-byte update is ever sent.
                rt->write_committed = false;
                rt->eject_waiting_for_persist = false;
                rt->write_event_pending = false;
                rt->nfc_status = 0x09u;
                rt->nfc_detail = 0x00u;
                host_set_presented(host, true);
            } else if (completed_write) {
                if (rt->write_persisted)
                    finish_committed_eject(rt, host, now_ms);
                else
                    rt->eject_waiting_for_persist = true;
            } else if (!rt->tag_ejected) {
                rt->nfc_status = 0x09;
                rt->nfc_detail = 0x00;
            }
            break;
        }
        case 0x05: { // status: report the current NFC state + UID
            const bool presented = !rt->tag_ejected;
            ns2_virtual_nfc_build_status(
                presented, presented ? uid : NULL, payload);
            if (presented) {
                payload[0] = rt->nfc_status;
                payload[1] = rt->nfc_detail;
            }
            // A genuine controller reports device-command state 0x18 and
            // completed extended-operation state 0x16 with an otherwise empty
            // payload -- no UID or tag identity.
            if (rt->nfc_status == 0x15u || rt->nfc_status == 0x18u ||
                rt->nfc_status == 0x16u) {
                memset(payload, 0, NS2_NFC_STATUS_PAYLOAD_SIZE);
                payload[0] = rt->nfc_status;
            }
            // RE probe: overlay candidate tag-identity bytes so the field that
            // makes the console request a 2 KB page set can be swept live.
            for (size_t i = 0; i < NS2_NFC_STATUS_PAYLOAD_SIZE; ++i)
                if (rt->status_probe_mask[i])
                    payload[i] = rt->status_probe_value[i];
            payload_size = NS2_NFC_STATUS_PAYLOAD_SIZE;
            direction = 0x01;
            break;
        }
        case 0x06: { // begin read: require a structural read descriptor
            // Bytes 0..1 are the TIMEOUT, not a "D0 07 marker". The extended
            // 4-block descriptor uses 3000 ms (B8 0B), so gating on the literal
            // bytes silently rejected it: the operation never started, status
            // never reached 0x04, and the console stopped without reading.
            const uint8_t desc_blocks = request_size >= 11u ? request[10] : 0u;
            bool zero_uid = request_size >= 9u;
            if (zero_uid) {
                for (size_t i = 2u; i < 9u; ++i) {
                    if (request[i] != 0u) {
                        zero_uid = false;
                        break;
                    }
                }
            }
            const bool selected_uid =
                request_size >= 9u &&
                memcmp(request + 2u, uid, sizeof(uid)) == 0;
            const bool read_descriptor =
                request_size >= 13u && desc_blocks >= 1u &&
                (size_t)11u + (size_t)desc_blocks * 2u <= request_size &&
                (zero_uid || selected_uid) && !rt->tag_ejected;
            if (read_descriptor) {
                build_buffer(rt, image, request, request_size);
                rt->operation_active = true;
                rt->nfc_status = 0x04; // active -> console proceeds to 0x15
                rt->nfc_detail = 0x00;
                rt->write_committed = false;
                rt->eject_waiting_for_persist = false;
                rt->operation_generation = generation;
                if (rt->write_mode || rt->extended_mode)
                    ns2_virtual_nfc_write_cancel(&rt->write);
                rt->write_mode = false;
                rt->extended_mode = false;
                rt->extended_expected_size = 0;
                bump_report_state(rt);
            } else {
                set_error(rt, now_ms, sub,
                          NS2_AMIIBO_V3_ERROR_READ_DESCRIPTOR,
                          NS2_VIRTUAL_NFC_OK, 0u, false);
            }
            break;
        }
        case 0x15: { // fetch a chunk of the descriptor-built operation buffer
            if (rt->operation_active && rt->op_buffer_size &&
                request_size >= 2u) {
                const uint16_t offset =
                    (uint16_t)request[0] | ((uint16_t)request[1] << 8);
                size_t out_size = 0;
                if (ns2_virtual_nfc_build_buffer_chunk(
                        rt->op_buffer, rt->op_buffer_size, offset, payload,
                        &out_size) == NS2_VIRTUAL_NFC_OK) {
                    payload_size = out_size;
                    direction = 0x01;
                }
            }
            break;
        }
        case 0x1E: { // sector-aware read used to reopen written v3 amiibo
            // The immediate wire response remains a bare ACK. The controller
            // internally stages a type-0x15 result, bumps the report event
            // field, reports empty status 0x15, and serves the result through
            // the existing 0x15 chunk command. Omitting that transition left
            // status at 0x18 and made the console wait, Stop, and retry forever.
            if (rt->tag_ejected ||
                !ns2_amiibo_v3_build_sector_read_result(
                    image, rt->signature_set ? rt->signature : NULL,
                    request, request_size,
                    rt->op_buffer, sizeof(rt->op_buffer),
                    &rt->op_buffer_size)) {
                set_error(rt, now_ms, sub, NS2_AMIIBO_V3_ERROR_SECTOR_READ,
                          NS2_VIRTUAL_NFC_OK, 0u, false);
                break;
            }
            rt->operation_active = true;
            rt->nfc_status = 0x15u;
            rt->nfc_detail = 0x00u;
            rt->operation_generation = generation;
            if (rt->write_mode || rt->extended_mode)
                ns2_virtual_nfc_write_cancel(&rt->write);
            rt->write_mode = false;
            rt->extended_mode = false;
            rt->extended_expected_size = 0u;
            bump_report_state(rt);
            break;
        }
        case 0x14: {
            if (request_size < 4u || rt->tag_ejected) {
                set_error(rt, now_ms, sub, NS2_AMIIBO_V3_ERROR_STAGE_FRAMING,
                          NS2_VIRTUAL_NFC_OK, 0u, true);
                break;
            }
            const uint16_t offset =
                (uint16_t)request[0] | ((uint16_t)request[1] << 8);
            const uint16_t declared =
                (uint16_t)request[2] | ((uint16_t)request[3] << 8);
            const size_t available = request_size - 4u;
            const uint8_t *data = request + 4u;
            if (declared == 0u || declared > available) {
                // One USB read is not one protocol message. An 88-byte 0x14
                // delivered as 64 + 24 lands here; the stream reassembler
                // upstream is what keeps it from becoming a second command.
                set_error(rt, now_ms, sub, NS2_AMIIBO_V3_ERROR_STAGE_FRAMING,
                          NS2_VIRTUAL_NFC_OK, offset, true);
                break;
            }

            // The timeout in bytes 0..1 varies by operation. Identity begins
            // with the selected UID at byte 2. Three capture-derived families
            // are kept distinct: device command -> 0x21, mutable records ->
            // 0x08, and sector-aware 355/167-byte operations -> 0x20.
            if (offset == 0u &&
                ns2_amiibo_v3_is_device_command(data, declared, image)) {
                rt->device_cmd_staged = true;
                rt->device_cmd_staged_count++;
                break;
            }

            if (!rt->write_mode && !rt->extended_mode && offset == 0u &&
                rt->operation_active && rt->nfc_status == 0x04u &&
                ns2_amiibo_v3_is_write_start(data, declared, image)) {
                ns2_virtual_nfc_write_begin(&rt->write);
                rt->write_mode = true;
                rt->write_committed = false;
                rt->operation_generation = generation;
            } else if (!rt->write_mode && !rt->extended_mode && offset == 0u &&
                       rt->operation_active && rt->nfc_status == 0x04u &&
                       (rt->extended_expected_size =
                            ns2_amiibo_v3_extended_expected_size(
                                data, declared, image)) != 0u) {
                ns2_virtual_nfc_write_begin(&rt->write);
                rt->extended_mode = true;
                rt->operation_generation = generation;
            }
            if (!rt->operation_active ||
                (!rt->write_mode && !rt->extended_mode) ||
                rt->nfc_status != 0x04u) {
                set_error(rt, now_ms, sub, NS2_AMIIBO_V3_ERROR_STAGE_NOT_ACTIVE,
                          NS2_VIRTUAL_NFC_OK, offset, true);
                break;
            }
            const ns2_virtual_nfc_result_t chunk =
                ns2_virtual_nfc_write_chunk(&rt->write, offset, data, declared);
            if (chunk != NS2_VIRTUAL_NFC_OK) {
                set_error(rt, now_ms, sub, NS2_AMIIBO_V3_ERROR_STAGE_CHUNK,
                          chunk, offset, true);
                break;
            }
            if (rt->extended_mode)
                rt->extended_chunk_count++;
            else
                rt->write_chunk_count++;
            break;
        }
        case 0x21: { // execute the staged device command
            // Genuine controllers answer by publishing an 83-byte result whose
            // type byte is 0x18 and whose body is the tag's SRAM window. The
            // console reads it back with 0x15 and only then accepts the amiibo.
            if (rt->device_cmd_staged) {
                build_device_result(rt, image, uid);
                rt->operation_active = true;
                rt->nfc_status = 0x18;
                rt->device_cmd_staged = false;
                // The console does not poll 0x05 on a timer -- it waits for the
                // report's NFC state field to change. Omitting this bump left it
                // with no signal that the result was ready.
                bump_report_state(rt);
                rt->device_result_count++;
            }
            break;
        }
        case 0x08: { // commit one complete v3 mutable-data transaction
            if (request_size != 0u || rt->tag_ejected ||
                !rt->operation_active || !rt->write_mode ||
                rt->nfc_status != 0x04u) {
                set_error(rt, now_ms, sub, NS2_AMIIBO_V3_ERROR_COMMIT_STATE,
                          NS2_VIRTUAL_NFC_OK, 0u, true);
                break;
            }

            // SRAM_RF_READY is a reader-side presentation bit. Never write the
            // synthetic served value back into the browser-owned tag image.
            image[NS2_AMIIBO_V3_NS_REG_OFFSET] = stored_ns_reg;
            uint8_t record_count = 0;
            uint16_t data_bytes = 0;
            const ns2_virtual_nfc_result_t committed =
                ns2_amiibo_v3_write_commit(&rt->write, image, &record_count,
                                           &data_bytes);
            if (committed != NS2_VIRTUAL_NFC_OK) {
                set_error(rt, now_ms, sub,
                          NS2_AMIIBO_V3_ERROR_COMMIT_VALIDATION, committed, 0u,
                          true);
                break;
            }
            if (!host_apply_console_write(host, image,
                                          rt->operation_generation)) {
                set_error(rt, now_ms, sub, NS2_AMIIBO_V3_ERROR_COMMIT_APPLY,
                          NS2_VIRTUAL_NFC_OK, 0u, true);
                break;
            }
            (void)record_count;
            (void)data_bytes;

            rt->observed_generation = rt->operation_generation + 1u;
            rt->observed_generation_valid = true;
            rt->nfc_status = 0x05u;
            rt->nfc_detail = 0x00u;
            rt->operation_active = false;
            rt->write_mode = false;
            rt->extended_expected_size = 0;
            rt->write_committed = true;
            rt->write_persisted = false;
            rt->eject_waiting_for_persist = false;
            rt->write_event_due_ms = now_ms + NS2_AMIIBO_V3_WRITE_COMPLETE_MS;
            rt->write_event_pending = true;
            rt->write_commit_count++;
            break;
        }
        case 0x20: { // complete the separately framed extended operation
            if (request_size != 0u || rt->tag_ejected ||
                !rt->operation_active || !rt->extended_mode ||
                rt->nfc_status != 0x04u) {
                set_error(rt, now_ms, sub, NS2_AMIIBO_V3_ERROR_COMMIT_STATE,
                          NS2_VIRTUAL_NFC_OK, 0u, true);
                break;
            }

            // Genuine hardware applies sector-aware records here, reports 0x16
            // with an empty status body, then accepts a targeted page-3 read and
            // a normal 454-byte/0x08 write. Persist this stage without ejecting,
            // and let the later 0x08 own the committed-write/Stop lifecycle.
            image[NS2_AMIIBO_V3_NS_REG_OFFSET] = stored_ns_reg;
            uint8_t record_count = 0;
            uint16_t data_bytes = 0;
            const size_t committed_size = rt->extended_expected_size;
            const ns2_virtual_nfc_result_t committed =
                ns2_amiibo_v3_extended_commit(&rt->write, image, committed_size,
                                              &record_count, &data_bytes);
            if (committed != NS2_VIRTUAL_NFC_OK) {
                set_error(rt, now_ms, sub,
                          NS2_AMIIBO_V3_ERROR_COMMIT_VALIDATION, committed, 0u,
                          true);
                break;
            }
            if (!host_apply_console_write(host, image,
                                          rt->operation_generation)) {
                set_error(rt, now_ms, sub, NS2_AMIIBO_V3_ERROR_COMMIT_APPLY,
                          NS2_VIRTUAL_NFC_OK, 0u, true);
                break;
            }
            (void)record_count;
            (void)data_bytes;

            rt->observed_generation = rt->operation_generation + 1u;
            rt->observed_generation_valid = true;
            rt->nfc_status = 0x16u;
            rt->nfc_detail = 0x00u;
            rt->operation_active = false;
            rt->extended_mode = false;
            rt->extended_expected_size = 0;
            ns2_amiibo_v3_extended_sequence_note_commit(
                &rt->extended_sequence, committed_size, now_ms);
            rt->write_committed = false;
            rt->eject_waiting_for_persist = false;
            bump_report_state(rt);
            rt->extended_completion_count++;
            break;
        }
        default:
            break; // ACK-and-trace anything else so the log shows it
    }

    // RE probe: answer a chosen subcommand with candidate data instead of a bare
    // ACK, to test whether the console takes tag identity from that reply.
    if (rt->reply_sub != 0 && sub == rt->reply_sub && rt->reply_len != 0 &&
        rt->reply_len <= NS2_NFC_READ_CHUNK_PAYLOAD_SIZE) {
        memcpy(payload, rt->reply_data, rt->reply_len);
        payload_size = rt->reply_len;
        direction = 0x01;
    }

    effects->payload_size = payload_size;
    effects->response_direction = direction;
    return true;
}

bool ns2_amiibo_v3_runtime_set_signature(ns2_amiibo_v3_runtime_t *rt,
                                         const uint8_t *bytes, size_t len)
{
    if (!rt || !bytes || len != NS2_AMIIBO_V3_SIGNATURE_SIZE) return false;
    memcpy(rt->signature, bytes, NS2_AMIIBO_V3_SIGNATURE_SIZE);
    rt->signature_set = true;
    return true;
}

bool ns2_amiibo_v3_runtime_status_probe_set(ns2_amiibo_v3_runtime_t *rt,
                                            uint8_t index,
                                            const uint8_t *bytes, uint8_t len)
{
    if (!rt || !bytes || len == 0 ||
        (size_t)index + len > NS2_NFC_STATUS_PAYLOAD_SIZE)
        return false;
    for (uint8_t i = 0; i < len; ++i) {
        rt->status_probe_value[index + i] = bytes[i];
        rt->status_probe_mask[index + i] = 1u;
    }
    return true;
}

bool ns2_amiibo_v3_runtime_hdr_probe_set(ns2_amiibo_v3_runtime_t *rt,
                                         uint8_t index, const uint8_t *bytes,
                                         uint8_t len)
{
    if (!rt || !bytes || len == 0 ||
        (size_t)index + len > NS2_AMIIBO_V3_PREFIX_SIZE)
        return false;
    for (uint8_t i = 0; i < len; ++i) {
        rt->hdr_probe_value[index + i] = bytes[i];
        rt->hdr_probe_mask[index + i] = 1u;
    }
    return true;
}

bool ns2_amiibo_v3_runtime_set_reply(ns2_amiibo_v3_runtime_t *rt, uint8_t sub,
                                     const uint8_t *data, uint8_t len)
{
    if (!rt || sub == 0 || !data || len == 0 ||
        len > NS2_AMIIBO_V3_REPLY_MAX)
        return false;
    memcpy(rt->reply_data, data, len);
    rt->reply_len = len;
    rt->reply_sub = sub;
    return true;
}
