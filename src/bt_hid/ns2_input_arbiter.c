#include "ns2_input_arbiter.h"

#include <string.h>

static uint32_t atomic_load_u32(const volatile uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void atomic_store_u32(volatile uint32_t *value, uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static uint32_t atomic_add_u32(volatile uint32_t *value, uint32_t delta)
{
    return __atomic_add_fetch(value, delta, __ATOMIC_ACQ_REL);
}

static void status_begin(ns2_input_arbiter_t *arbiter)
{
    // The only writer is the input core.  An odd sequence means a status
    // reader must retry; an even sequence is a stable snapshot.
    (void)atomic_add_u32(&arbiter->status_sequence, 1u);
}

static void status_end(ns2_input_arbiter_t *arbiter)
{
    (void)atomic_add_u32(&arbiter->status_sequence, 1u);
}

static bool keys_equal(const ns2_input_source_key_t *a,
                       const ns2_input_source_key_t *b)
{
    if (!a || !b || a->transport != b->transport ||
        a->instance != b->instance ||
        a->connection_generation != b->connection_generation ||
        a->stable_addr_valid != b->stable_addr_valid) {
        return false;
    }

    if (a->stable_addr_valid) {
        return memcmp(a->stable_addr, b->stable_addr, sizeof(a->stable_addr)) == 0;
    }
    return a->dev_addr == b->dev_addr;
}

static int find_source(const ns2_input_arbiter_t *arbiter,
                       const ns2_input_source_key_t *key)
{
    if (!arbiter || !key) return -1;
    for (unsigned i = 0; i < NS2_INPUT_ARBITER_MAX_SOURCES; ++i) {
        if (arbiter->sources[i].present && keys_equal(&arbiter->sources[i].key, key))
            return (int)i;
    }
    return -1;
}

static int find_id(const ns2_input_arbiter_t *arbiter, uint32_t id)
{
    if (!arbiter || id == NS2_INPUT_SOURCE_ID_NONE) return -1;
    for (unsigned i = 0; i < NS2_INPUT_ARBITER_MAX_SOURCES; ++i) {
        if (arbiter->sources[i].present && arbiter->sources[i].id == id)
            return (int)i;
    }
    return -1;
}

static uint32_t next_nonzero(volatile uint32_t *counter)
{
    uint32_t value = atomic_add_u32(counter, 1u);
    if (value != NS2_INPUT_SOURCE_ID_NONE) return value;
    return atomic_add_u32(counter, 1u);
}

static void update_metadata(ns2_input_source_info_t *source,
                            const char *name,
                            uint16_t vendor_id,
                            uint16_t product_id)
{
    if (name) {
        strncpy(source->name, name, sizeof(source->name) - 1u);
        source->name[sizeof(source->name) - 1u] = '\0';
    }
    if (vendor_id) source->vendor_id = vendor_id;
    if (product_id) source->product_id = product_id;
}

void ns2_input_arbiter_init(ns2_input_arbiter_t *arbiter)
{
    if (!arbiter) return;
    memset(arbiter, 0, sizeof(*arbiter));
    // The allocator increments before returning, so zero is the sentinel and
    // the first live source/generation is 1.
    arbiter->next_id = 0u;
    arbiter->next_generation = 0u;
}

bool ns2_input_arbiter_request_active(ns2_input_arbiter_t *arbiter,
                                      uint32_t id)
{
    if (!arbiter) return false;
    if (id != NS2_INPUT_SOURCE_ID_NONE && find_id(arbiter, id) < 0)
        return false;

    // A repeated request for the already active source is intentionally a
    // no-op.  This keeps source switching idempotent and avoids unnecessary
    // neutral boundaries when a UI retries an acknowledged command.
    //
    // It is only a no-op once the choice is already explicit. A source that owns
    // the console by automatic policy is NOT the same state as one the user
    // picked: selecting it is how the user pins it, and short-circuiting here
    // would leave the choice implicit and let a higher-class source take the
    // console away from them later.
    if (id == arbiter->active_id && arbiter->explicit_active &&
        !arbiter->awaiting_fresh &&
        atomic_load_u32(&arbiter->pending_request) == arbiter->applied_request) {
        return true;
    }

    return ns2_input_arbiter_queue_active(arbiter, id);
}

bool ns2_input_arbiter_queue_active(ns2_input_arbiter_t *arbiter,
                                    uint32_t id)
{
    if (!arbiter) return false;
    atomic_store_u32(&arbiter->pending_id, id);
    (void)atomic_add_u32(&arbiter->pending_request, 1u);
    return true;
}

// Best source to own the console when nobody has been chosen explicitly.
// Higher class wins; ties go to the earliest registered source so the result is
// stable and does not flap between two peers of the same class.
static int preferred_index(const ns2_input_arbiter_t *arbiter)
{
    int best = -1;
    for (unsigned i = 0; i < NS2_INPUT_ARBITER_MAX_SOURCES; ++i) {
        const ns2_input_source_info_t *source = &arbiter->sources[i];
        if (!source->present) continue;
        if (best < 0 || source->source_class > arbiter->sources[best].source_class)
            best = (int)i;
    }
    return best;
}

// Apply the automatic ownership policy. Only ever called when the user has made
// no explicit choice. Returns true when ownership actually moved, which obliges
// the caller to emit a neutral boundary.
static bool apply_auto_policy(ns2_input_arbiter_t *arbiter)
{
    if (arbiter->explicit_active) return false;
    int best = preferred_index(arbiter);
    uint32_t wanted = best < 0 ? NS2_INPUT_SOURCE_ID_NONE
                               : arbiter->sources[best].id;
    if (wanted == arbiter->active_id) return false;

    bool had_owner = arbiter->active_id != NS2_INPUT_SOURCE_ID_NONE;
    arbiter->active_id = wanted;
    // A first owner may publish immediately: there is no previous stream whose
    // final state has to be flushed. Taking the console away from a live source
    // does require the replacement to prove itself with a fresh report, so a
    // held button on the outgoing source cannot survive the handover.
    arbiter->awaiting_fresh = (had_owner && wanted != NS2_INPUT_SOURCE_ID_NONE) ? 1u : 0u;
    arbiter->transition_count++;
    return had_owner;
}

static int register_source(ns2_input_arbiter_t *arbiter,
                           const ns2_input_source_key_t *key,
                           const char *name,
                           uint16_t vendor_id,
                           uint16_t product_id,
                           uint8_t source_class,
                           bool *auto_switched)
{
    int existing = find_source(arbiter, key);
    if (existing >= 0) {
        update_metadata(&arbiter->sources[existing], name, vendor_id, product_id);
        // A source registered by a connection hook starts UNKNOWN and is
        // identified by its first report. That is a genuine change of standing,
        // so ownership is re-evaluated exactly as it is for a new arrival --
        // otherwise a controller that connected before it could be identified
        // would never reclaim the console from the companion bridge.
        if (arbiter->sources[existing].source_class != source_class) {
            arbiter->sources[existing].source_class = source_class;
            if (apply_auto_policy(arbiter) && auto_switched) *auto_switched = true;
        }
        return existing;
    }

    for (unsigned i = 0; i < NS2_INPUT_ARBITER_MAX_SOURCES; ++i) {
        ns2_input_source_info_t *source = &arbiter->sources[i];
        if (source->present) continue;
        memset(source, 0, sizeof(*source));
        source->present = 1u;
        source->key = *key;
        source->id = next_nonzero(&arbiter->next_id);
        source->generation = next_nonzero(&arbiter->next_generation);
        source->source_class = source_class;
        update_metadata(source, name, vendor_id, product_id);
        // A newly arrived source can outrank the current default owner -- this is
        // how a controller paired directly to the adapter reclaims the console
        // from the companion bridge without the user touching anything.
        if (apply_auto_policy(arbiter) && auto_switched) *auto_switched = true;
        return (int)i;
    }
    return -1;
}

static bool apply_pending(ns2_input_arbiter_t *arbiter, bool *transition)
{
    if (transition) *transition = false;
    uint32_t request = atomic_load_u32(&arbiter->pending_request);
    if (request == arbiter->applied_request)
        return true;

    uint32_t requested_id = atomic_load_u32(&arbiter->pending_id);
    if (requested_id != NS2_INPUT_SOURCE_ID_NONE &&
        find_id(arbiter, requested_id) < 0) {
        // A source can disconnect between validation and the input boundary.
        // Consume the request but leave the output in the already-neutral
        // state rather than silently falling back to another source.
        arbiter->active_id = NS2_INPUT_SOURCE_ID_NONE;
        arbiter->awaiting_fresh = 0u;
    } else {
        arbiter->active_id = requested_id;
        arbiter->awaiting_fresh = requested_id != NS2_INPUT_SOURCE_ID_NONE;
    }
    arbiter->explicit_active = 1u;
    arbiter->transition_count++;
    arbiter->applied_request = request;
    if (transition) *transition = true;
    return true;
}

bool ns2_input_arbiter_submit(ns2_input_arbiter_t *arbiter,
                              const ns2_input_source_key_t *key,
                              const char *name,
                              uint16_t vendor_id,
                              uint16_t product_id,
                              uint8_t source_class,
                              ns2_input_route_decision_t *decision)
{
    if (!arbiter || !key || !decision) return false;
    memset(decision, 0, sizeof(*decision));

    status_begin(arbiter);
    bool transition = false;
    bool auto_switched = false;
    (void)apply_pending(arbiter, &transition);
    int source_index = register_source(arbiter, key, name, vendor_id, product_id,
                                       source_class, &auto_switched);
    decision->auto_switched = auto_switched ? 1u : 0u;
    if (source_index < 0) {
        status_end(arbiter);
        return false;
    }

    ns2_input_source_info_t *source = &arbiter->sources[source_index];
    decision->transition_applied = transition ? 1u : 0u;
    if (source->id == arbiter->active_id) {
        decision->accepted = 1u;
        if (arbiter->awaiting_fresh) {
            arbiter->awaiting_fresh = 0u;
            decision->fresh_report = 1u;
        }
    }
    status_end(arbiter);
    return decision->accepted != 0u;
}

bool ns2_input_arbiter_disconnect(ns2_input_arbiter_t *arbiter,
                                  const ns2_input_source_key_t *key,
                                  bool *was_active)
{
    if (was_active) *was_active = false;
    if (!arbiter || !key) return false;

    status_begin(arbiter);
    int index = find_source(arbiter, key);
    if (index < 0) {
        status_end(arbiter);
        return false;
    }

    uint32_t id = arbiter->sources[index].id;
    bool pending_target = atomic_load_u32(&arbiter->pending_id) == id;
    bool active = id == arbiter->active_id;
    // A selected target can disappear after the core-0 request but before the
    // next report boundary.  Treat that as an ownership loss too: the caller
    // must re-affirm neutral and must not revive the previous source.
    if (pending_target) active = true;
    if (was_active) *was_active = active;
    memset(&arbiter->sources[index], 0, sizeof(arbiter->sources[index]));
    if (active) {
        arbiter->active_id = NS2_INPUT_SOURCE_ID_NONE;
        arbiter->awaiting_fresh = 0u;
        arbiter->transition_count++;
        // An explicit choice is final: losing it leaves the console deliberately
        // unowned rather than handing control to a source the user did not pick.
        //
        // Without an explicit choice, ownership is policy, so fall back to the
        // best remaining source. This is what returns the console to the
        // companion bridge when the directly-paired controller disconnects or
        // runs its battery flat. Previously this path latched explicit mode on
        // every disconnect, which permanently disabled automatic selection for
        // the rest of the session even though the user had never chosen anything.
        //
        // A source that vanished while it was the user's in-flight selection is
        // the exception: the choice was made, so it is honoured by leaving the
        // console unowned rather than reviving whatever else is connected.
        if (pending_target) arbiter->explicit_active = 1u;
        else (void)apply_auto_policy(arbiter);
    }
    if (pending_target) {
        atomic_store_u32(&arbiter->pending_id, NS2_INPUT_SOURCE_ID_NONE);
        arbiter->applied_request = atomic_load_u32(&arbiter->pending_request);
    }
    status_end(arbiter);
    return true;
}

uint32_t ns2_input_arbiter_source_id(const ns2_input_arbiter_t *arbiter,
                                     const ns2_input_source_key_t *key)
{
    if (!arbiter || !key) return NS2_INPUT_SOURCE_ID_NONE;
    ns2_input_arbiter_status_t status;
    ns2_input_arbiter_get_status(arbiter, &status);
    for (unsigned i = 0; i < status.source_count; ++i) {
        if (keys_equal(&status.sources[i].key, key))
            return status.sources[i].id;
    }
    return NS2_INPUT_SOURCE_ID_NONE;
}

bool ns2_input_arbiter_is_active_connection_generation(
    const ns2_input_arbiter_t *arbiter,
    uint8_t dev_addr,
    uint32_t connection_generation)
{
    if (!arbiter) return false;
    for (;;) {
        uint32_t before = atomic_load_u32(&arbiter->status_sequence);
        if (before & 1u) continue;
        uint32_t active_id = arbiter->active_id;
        bool active = false;
        for (unsigned i = 0; i < NS2_INPUT_ARBITER_MAX_SOURCES; ++i) {
            if (arbiter->sources[i].present &&
                arbiter->sources[i].key.dev_addr == dev_addr &&
                arbiter->sources[i].id == active_id &&
                (connection_generation == 0u ||
                 arbiter->sources[i].key.connection_generation ==
                     connection_generation)) {
                active = true;
                break;
            }
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t after = atomic_load_u32(&arbiter->status_sequence);
        if (before == after) return active;
    }
}

bool ns2_input_arbiter_is_active_connection(const ns2_input_arbiter_t *arbiter,
                                            uint8_t dev_addr)
{
    return ns2_input_arbiter_is_active_connection_generation(arbiter, dev_addr, 0u);
}

void ns2_input_arbiter_get_status(const ns2_input_arbiter_t *arbiter,
                                  ns2_input_arbiter_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!arbiter) return;

    for (;;) {
        uint32_t before = atomic_load_u32(&arbiter->status_sequence);
        if (before & 1u) continue;
        status->active_id = arbiter->active_id;
        // `pending_id` is a request slot: core 0 writes it, core 1 consumes it at a
        // report boundary. It is deliberately never cleared at the apply site --
        // clearing it from core 1 could wipe a newer core-0 request that landed
        // between `applied_request` advancing and the clear, silently turning that
        // selection into "no active source". The request counters are the
        // authoritative record of whether a switch is still outstanding, so derive
        // the reported value from them instead of exposing the stale target. Both
        // counters are read inside this seqlock, so they cannot disagree.
        uint32_t pending_request = atomic_load_u32(&arbiter->pending_request);
        status->pending_id = (pending_request == arbiter->applied_request)
                                 ? NS2_INPUT_SOURCE_ID_NONE
                                 : atomic_load_u32(&arbiter->pending_id);
        status->transition_count = arbiter->transition_count;
        status->explicit_active = arbiter->explicit_active;
        status->awaiting_fresh = arbiter->awaiting_fresh;
        status->source_count = 0;
        for (unsigned i = 0; i < NS2_INPUT_ARBITER_MAX_SOURCES; ++i) {
            if (!arbiter->sources[i].present) continue;
            if (status->source_count < NS2_INPUT_ARBITER_MAX_SOURCES)
                status->sources[status->source_count++] = arbiter->sources[i];
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t after = atomic_load_u32(&arbiter->status_sequence);
        if (before == after) return;
    }
}
