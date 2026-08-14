#include "ns2_active_input.h"

#include <string.h>

#include "bt/bthid/bthid.h"
#include "ns2_native_motion.h"
#include "report.h"

static ns2_input_arbiter_t s_arbiter;

static void source_key_for_connection(uint8_t conn_index,
                                      int8_t instance,
                                      uint8_t transport_hint,
                                      uint32_t generation_hint,
                                      ns2_input_source_key_t *key,
                                      const bthid_device_t **device_out)
{
    memset(key, 0, sizeof(*key));
    key->transport = transport_hint;
    key->dev_addr = conn_index;
    key->instance = instance;

    const bthid_device_t *device = bthid_get_device(conn_index);
    if (device) {
        memcpy(key->stable_addr, device->bd_addr, sizeof(key->stable_addr));
        for (unsigned i = 0; i < sizeof(key->stable_addr); ++i) {
            if (key->stable_addr[i] != 0u) {
                key->stable_addr_valid = 1u;
                break;
            }
        }
        key->connection_generation = generation_hint != 0u
            ? generation_hint : device->connection_generation;
        // The event's transport is authoritative when present.  Raw reports
        // and native motion do not have an event, so derive their transport
        // from the HID connection here.
        if (key->transport == INPUT_TRANSPORT_NONE)
            key->transport = device->is_ble ? INPUT_TRANSPORT_BT_BLE
                                            : INPUT_TRANSPORT_BT_CLASSIC;
    }
    if (device_out) *device_out = device;
}

void ns2_active_input_init(void)
{
    ns2_input_arbiter_init(&s_arbiter);
}

bool ns2_active_input_submit(const input_event_t *event,
                             ns2_input_route_decision_t *decision)
{
    if (!event || !decision) return false;

    ns2_input_source_key_t key;
    const bthid_device_t *device = NULL;
    source_key_for_connection(event->dev_addr, event->instance,
                              (uint8_t)event->transport,
                              event->connection_generation, &key, &device);
    const char *name = device ? device->name : NULL;
    uint16_t vendor_id = device ? device->vendor_id : 0u;
    uint16_t product_id = device ? device->product_id : 0u;
    // The companion bridge is identified by its own declared descriptor, never by
    // name or VID/PID, so an ordinary controller can never be demoted by
    // resembling it.
    uint8_t source_class = event->from_android_bridge
                               ? NS2_INPUT_SOURCE_CLASS_BRIDGE
                               : NS2_INPUT_SOURCE_CLASS_DIRECT;
    bool accepted = ns2_input_arbiter_submit(&s_arbiter, &key, name, vendor_id,
                                             product_id, source_class, decision);
    // The arbiter is a pure object and cannot reach the report seam, so the
    // neutral boundary for a policy handover is owed here -- the same one an
    // explicit selection emits.
    if (decision->auto_switched) {
        report_neutralize_slot(0);
        ns2_native_motion_clear();
    }
    return accepted;
}

bool ns2_active_input_disconnected(uint8_t dev_addr, int8_t instance)
{
    return ns2_active_input_disconnected_generation(dev_addr, instance, 0u);
}

bool ns2_active_input_disconnected_generation(uint8_t dev_addr,
                                              int8_t instance,
                                              uint32_t connection_generation)
{
    ns2_input_source_key_t key;
    source_key_for_connection(dev_addr, instance, INPUT_TRANSPORT_NONE,
                              connection_generation, &key, NULL);
    bool was_active = false;
    if (!ns2_input_arbiter_disconnect(&s_arbiter, &key, &was_active) || !was_active)
        return false;

    // Disconnect is a hard ownership boundary.  Do not retain the old native
    // motion sample here: the hold-last-sample policy is only for ordinary
    // physical power loss, not an explicit source handoff.
    report_neutralize_slot(0);
    ns2_native_motion_clear();
    return true;
}

bool ns2_active_input_connection_is_active(uint8_t conn_index)
{
    return ns2_active_input_connection_is_active_generation(conn_index, 0u);
}

bool ns2_active_input_connection_is_active_generation(
    uint8_t conn_index, uint32_t connection_generation)
{
    return ns2_input_arbiter_is_active_connection_generation(
        &s_arbiter, conn_index, connection_generation);
}

uint32_t ns2_active_input_connection_generation(uint8_t conn_index)
{
    const bthid_device_t *device = bthid_get_device(conn_index);
    return device ? device->connection_generation : 0u;
}

void ns2_active_input_note_connection(uint8_t conn_index)
{
    ns2_input_source_key_t key;
    const bthid_device_t *device = NULL;
    source_key_for_connection(conn_index, 0, INPUT_TRANSPORT_NONE, 0u,
                              &key, &device);
    // A raw/native callback can race HID setup.  Do not create a provisional
    // source with an empty address/generation; the HID-ready lifecycle hook or
    // the first normalized event will register the fully identified source.
    if (!device) return;
    ns2_input_route_decision_t decision;
    // No report has arrived yet, so this source cannot be classified. UNKNOWN
    // ranks lowest: it may take an idle console, never a claimed one, and its
    // first report settles what it actually is.
    (void)ns2_input_arbiter_submit(&s_arbiter, &key,
                                   device ? device->name : NULL,
                                   device ? device->vendor_id : 0u,
                                   device ? device->product_id : 0u,
                                   NS2_INPUT_SOURCE_CLASS_UNKNOWN,
                                   &decision);
}

void ns2_active_input_rebind(uint8_t conn_index)
{
    ns2_active_input_note_connection(conn_index);
}

// Strong bthid lifecycle hook used to enumerate an idle connected source.
void bthid_on_hid_ready(uint8_t conn_index)
{
    ns2_active_input_note_connection(conn_index);
}

bool ns2_active_input_request(uint32_t source_id)
{
    ns2_input_arbiter_status_t status;
    ns2_input_arbiter_get_status(&s_arbiter, &status);
    bool found = source_id == NS2_INPUT_SOURCE_ID_NONE;
    for (unsigned i = 0; i < status.source_count; ++i) {
        if (status.sources[i].id == source_id) {
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    // A repeated explicit request is idempotent.  An automatically selected
    // legacy source still needs a queued request so it becomes explicit.
    if (status.explicit_active && status.active_id == source_id &&
        status.pending_id == NS2_INPUT_SOURCE_ID_NONE && !status.awaiting_fresh)
        return true;
    if (!ns2_input_arbiter_queue_active(&s_arbiter, source_id))
        return false;

    // Neutralize immediately on core 0 so a quiet old source cannot remain on
    // the console while core 1 waits for its next report boundary.  The arbiter
    // still commits the selected token only at a report boundary.
    report_neutralize_slot(0);
    ns2_native_motion_clear();
    return true;
}

void ns2_active_input_status(ns2_input_arbiter_status_t *status)
{
    ns2_input_arbiter_get_status(&s_arbiter, status);
}
