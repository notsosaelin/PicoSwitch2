#include "ns2_virtual_nfc_runtime.h"

#include <string.h>

#define NS2_NFC_STATE_TRANSITION_MS 40u
#define NS2_NFC_WRITE_COMPLETE_MS 700u
#define NS2_NFC_ERROR_TRANSITION_MS 1u

static bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void schedule_event(ns2_virtual_nfc_runtime_t *runtime,
                           uint32_t now_ms, uint32_t delay_ms,
                           ns2_virtual_nfc_event_t event)
{
    runtime->pending_report_state =
        (uint8_t)((runtime->report_state + 1u) & 0x07u);
    runtime->transition_due_ms = now_ms + delay_ms;
    runtime->pending_event = event;
    runtime->transition_pending = true;
}

static void cancel_event(ns2_virtual_nfc_runtime_t *runtime)
{
    runtime->transition_pending = false;
    runtime->pending_event = NS2_VIRTUAL_NFC_EVENT_NONE;
}

static void emit_event_now(ns2_virtual_nfc_runtime_t *runtime)
{
    cancel_event(runtime);
    runtime->report_state =
        (uint8_t)((runtime->report_state + 1u) & 0x07u);
    runtime->pending_report_state = runtime->report_state;
}

static bool uid_is_zero(const uint8_t *request, size_t request_size)
{
    if (!request || request_size < 9u) return false;
    for (size_t i = 2; i < 9u; ++i) {
        if (request[i] != 0) return false;
    }
    return true;
}

static void uid_from_raw(const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
                         uint8_t uid[7])
{
    uid[0] = raw[0];
    uid[1] = raw[1];
    uid[2] = raw[2];
    uid[3] = raw[4];
    uid[4] = raw[5];
    uid[5] = raw[6];
    uid[6] = raw[7];
}

static bool uid_matches_raw(const uint8_t *request, size_t request_size,
                            const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE])
{
    if (!request || request_size < 9u || !raw) return false;
    uint8_t uid[7];
    uid_from_raw(raw, uid);
    return memcmp(request + 2, uid, sizeof(uid)) == 0;
}

static void set_ready_status(ns2_virtual_nfc_runtime_t *runtime)
{
    runtime->nfc_status = 0x09;
    runtime->nfc_detail = 0x00;
}

static void set_active_status(ns2_virtual_nfc_runtime_t *runtime)
{
    runtime->nfc_status = 0x04;
    runtime->nfc_detail = 0x00;
}

static void set_error_status(ns2_virtual_nfc_runtime_t *runtime,
                             uint32_t now_ms)
{
    runtime->nfc_status = 0x07;
    runtime->nfc_detail = 0x41;
    runtime->operation_active = false;
    runtime->write_mode = false;
    ns2_virtual_nfc_write_cancel(&runtime->write);
    schedule_event(runtime, now_ms, NS2_NFC_ERROR_TRANSITION_MS,
                   NS2_VIRTUAL_NFC_EVENT_ERROR);
}

static void observe_tag_presence(ns2_virtual_nfc_runtime_t *runtime,
                                 bool tag_present)
{
    // The first command after a loaded image is presented accounts for the
    // genuine controller's TagPresented event. This preserves the confirmed
    // 0->1->2->3 read sequence while keeping later states modulo eight.
    if (tag_present && !runtime->tag_was_present) {
        runtime->report_state =
            (uint8_t)((runtime->report_state + 1u) & 0x07u);
        runtime->pending_report_state = runtime->report_state;
    }
    runtime->tag_was_present = tag_present;
}

void ns2_virtual_nfc_runtime_init(ns2_virtual_nfc_runtime_t *runtime)
{
    if (!runtime) return;
    memset(runtime, 0, sizeof(*runtime));
    runtime->write_persisted = true;
    set_ready_status(runtime);
    ns2_virtual_nfc_write_init(&runtime->write);
}

static void finish_committed_eject(ns2_virtual_nfc_runtime_t *runtime,
                                   uint32_t now_ms)
{
    runtime->eject_waiting_for_persist = false;
    runtime->write_committed = false;
    runtime->tag_ejected = true;
    runtime->represent_after_ms =
        now_ms + NS2_VIRTUAL_NFC_REPRESENT_COOLDOWN_MS;
    runtime->tag_was_present = false;
    runtime->nfc_status = 0x07;
    runtime->nfc_detail = 0x41;
    emit_event_now(runtime);
}

void ns2_virtual_nfc_runtime_set_write_persisted(
    ns2_virtual_nfc_runtime_t *runtime, bool persisted, uint32_t now_ms)
{
    if (!runtime) return;
    runtime->write_persisted = persisted;
    if (persisted && runtime->eject_waiting_for_persist)
        finish_committed_eject(runtime, now_ms);
}

void ns2_virtual_nfc_runtime_tick(ns2_virtual_nfc_runtime_t *runtime,
                                  uint32_t now_ms)
{
    if (!runtime || !runtime->transition_pending ||
        !time_reached(now_ms, runtime->transition_due_ms))
        return;
    runtime->report_state = runtime->pending_report_state;
    cancel_event(runtime);
}

uint8_t ns2_virtual_nfc_runtime_report_state(
    const ns2_virtual_nfc_runtime_t *runtime)
{
    return runtime ? runtime->report_state : 0;
}

void ns2_virtual_nfc_runtime_write_apply_failed(
    ns2_virtual_nfc_runtime_t *runtime, uint32_t now_ms)
{
    if (!runtime) return;
    runtime->write_committed = false;
    set_error_status(runtime, now_ms);
}

bool ns2_virtual_nfc_runtime_dispatch(
    ns2_virtual_nfc_runtime_t *runtime, uint32_t now_ms, uint8_t subcommand,
    const uint8_t *request, size_t request_size, bool tag_present,
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    const uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE],
    ns2_virtual_nfc_response_t *response)
{
    if (!runtime || !response || (!request && request_size != 0))
        return false;

    memset(response, 0, sizeof(*response));
    response->response_direction = 0x04;
    ns2_virtual_nfc_runtime_tick(runtime, now_ms);
    bool presented =
        tag_present && raw && !runtime->tag_ejected;
    observe_tag_presence(runtime, presented);

    switch (subcommand) {
        case 0x03:
            // A selected image is persistent reader media, not permanently
            // glued to the antenna. A completed write ejects its current
            // presentation; a scan after the removal cooldown presents the
            // same (now mutated) image as a fresh tag encounter.
            if (!presented && runtime->tag_ejected &&
                tag_present && raw &&
                time_reached(now_ms, runtime->represent_after_ms)) {
                runtime->tag_ejected = false;
                presented = true;
                observe_tag_presence(runtime, true);
            }
            runtime->scan_active = true;
            if (presented && !runtime->write_mode &&
                !runtime->write_committed) {
                set_ready_status(runtime);
                schedule_event(runtime, now_ms, NS2_NFC_STATE_TRANSITION_MS,
                               NS2_VIRTUAL_NFC_EVENT_SCAN_READY);
            }
            return true;

        case 0x04:
            runtime->scan_active = false;
            if (runtime->transition_pending &&
                runtime->pending_event == NS2_VIRTUAL_NFC_EVENT_SCAN_READY)
                cancel_event(runtime);
            // Stop closes the current operation. An incomplete write is only
            // aborted. After a successful commit, however, genuine readers
            // report TagRemoved: retain the mutated browser-loaded image for
            // download, but stop presenting it until the next console scan.
            const bool completed_write = runtime->write_committed;
            runtime->operation_active = false;
            if (runtime->write_mode)
                ns2_virtual_nfc_write_cancel(&runtime->write);
            runtime->write_mode = false;
            if (completed_write) {
                if (runtime->write_persisted)
                    finish_committed_eject(runtime, now_ms);
                else
                    runtime->eject_waiting_for_persist = true;
            } else if (presented) {
                runtime->write_committed = false;
                set_ready_status(runtime);
            }
            return true;

        case 0x05: {
            uint8_t uid[7];
            if (presented) uid_from_raw(raw, uid);
            ns2_virtual_nfc_build_status(
                presented, presented ? uid : NULL, response->payload);
            if (presented) {
                response->payload[0] = runtime->nfc_status;
                response->payload[1] = runtime->nfc_detail;
            }
            response->payload_size = NS2_NFC_STATUS_PAYLOAD_SIZE;
            response->response_direction = 0x01;
            return true;
        }

        case 0x06: {
            const bool descriptor_valid =
                presented && signature && request_size >= 19u &&
                request[0] == 0xD0 && request[1] == 0x07;
            const bool read_mode =
                descriptor_valid && uid_is_zero(request, request_size);
            const bool write_mode =
                descriptor_valid &&
                uid_matches_raw(request, request_size, raw);
            if (!read_mode && !write_mode) {
                set_error_status(runtime, now_ms);
                return true;
            }

            ns2_virtual_nfc_result_t result;
            if (write_mode) {
                result = ns2_virtual_nfc_build_write_prep_buffer(
                    raw, signature, request + 10, runtime->operation_buffer);
                runtime->operation_buffer_size =
                    NS2_NFC_WRITE_PREP_BUFFER_SIZE;
            } else {
                result = ns2_virtual_nfc_build_read_buffer(
                    raw, signature, request + 10, runtime->operation_buffer);
                runtime->operation_buffer_size = NS2_NFC_READ_BUFFER_SIZE;
            }
            if (result != NS2_VIRTUAL_NFC_OK) {
                set_error_status(runtime, now_ms);
                return true;
            }
            runtime->operation_active = true;
            runtime->write_mode = write_mode;
            runtime->write_committed = false;
            runtime->eject_waiting_for_persist = false;
            if (write_mode)
                ns2_virtual_nfc_write_begin(&runtime->write);
            else
                ns2_virtual_nfc_write_cancel(&runtime->write);
            set_active_status(runtime);
            schedule_event(runtime, now_ms, NS2_NFC_STATE_TRANSITION_MS,
                           NS2_VIRTUAL_NFC_EVENT_OPERATION_READY);
            return true;
        }

        case 0x15: {
            if (!runtime->operation_active || request_size < 2u)
                return true;
            const uint16_t offset =
                (uint16_t)request[0] | ((uint16_t)request[1] << 8);
            if (ns2_virtual_nfc_build_buffer_chunk(
                    runtime->operation_buffer,
                    runtime->operation_buffer_size, offset,
                    response->payload, &response->payload_size) ==
                NS2_VIRTUAL_NFC_OK) {
                response->response_direction = 0x01;
            } else {
                set_error_status(runtime, now_ms);
            }
            return true;
        }

        case 0x14: {
            if (!presented || request_size < 4u) {
                set_error_status(runtime, now_ms);
                return true;
            }
            const uint16_t offset =
                (uint16_t)request[0] | ((uint16_t)request[1] << 8);
            const uint16_t declared =
                (uint16_t)request[2] | ((uint16_t)request[3] << 8);
            const size_t available = request_size - 4u;
            if (declared == 0 || declared > available) {
                set_error_status(runtime, now_ms);
                return true;
            }

            // Some captured format flows begin with a zero-UID/read descriptor
            // and promote only when the offset-zero staging block proves both
            // the D0 07 header and the selected tag UID.
            if (!runtime->write_mode && runtime->operation_active &&
                runtime->nfc_status == 0x04 && offset == 0 &&
                declared >= 9u && request[4] == 0xD0 &&
                request[5] == 0x07 &&
                uid_matches_raw(request + 4, declared, raw)) {
                runtime->write_mode = true;
                runtime->write_committed = false;
                ns2_virtual_nfc_write_begin(&runtime->write);
            }
            if (!runtime->operation_active || !runtime->write_mode ||
                runtime->nfc_status != 0x04 ||
                ns2_virtual_nfc_write_chunk(
                    &runtime->write, offset, request + 4, declared) !=
                    NS2_VIRTUAL_NFC_OK)
                set_error_status(runtime, now_ms);
            return true;
        }

        case 0x08: {
            if (request_size != 0 || !presented ||
                !runtime->operation_active || !runtime->write_mode ||
                runtime->nfc_status != 0x04) {
                set_error_status(runtime, now_ms);
                return true;
            }
            const ns2_virtual_nfc_result_t result =
                ns2_virtual_nfc_write_commit(
                    &runtime->write, raw, &response->write_record_count,
                    &response->write_data_bytes);
            if (result != NS2_VIRTUAL_NFC_OK) {
                set_error_status(runtime, now_ms);
                return true;
            }
            runtime->nfc_status = 0x05;
            runtime->nfc_detail = 0x00;
            runtime->operation_active = false;
            runtime->write_mode = false;
            runtime->write_committed = true;
            runtime->write_persisted = false;
            response->write_committed = true;
            schedule_event(runtime, now_ms, NS2_NFC_WRITE_COMPLETE_MS,
                           NS2_VIRTUAL_NFC_EVENT_WRITE_COMPLETE);
            return true;
        }

        default:
            return false;
    }
}
