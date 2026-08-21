#include "ns2_input_arbiter.h"

#include <string.h>

// The product name for the Android companion's console-input bridge. It is the
// same relationship the app presents as "Controller Link", so the adapter and
// the app agree on what the console is being driven by.
#define NS2_INPUT_BRIDGE_DISPLAY_NAME "Controller Link"

const char *ns2_input_source_display_name(const char *name,
                                          uint8_t source_class)
{
    if (name) {
        // Whitespace-only is as unusable as empty for a UI row.
        for (const char *c = name; *c; ++c) {
            if (*c != ' ' && *c != '\t') return name;
        }
    }
    if (source_class == NS2_INPUT_SOURCE_CLASS_BRIDGE)
        return NS2_INPUT_BRIDGE_DISPLAY_NAME;
    return NULL;
}

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

// Group handle of whichever source currently owns the console, or 0 when the
// owner is a standalone source (or there is no owner).
static uint32_t active_group(const ns2_input_arbiter_t *arbiter)
{
    int index = find_id(arbiter, arbiter->active_id);
    return index < 0 ? 0u : arbiter->sources[index].group_id;
}

// A source may publish when it IS the owner, or when it is another peer of the
// owner's composite logical source.  Group membership is the only way two
// transport peers ever share console ownership.
static bool source_owns_console(const ns2_input_arbiter_t *arbiter,
                                const ns2_input_source_info_t *source)
{
    if (!source->present || arbiter->active_id == NS2_INPUT_SOURCE_ID_NONE)
        return false;
    if (source->id == arbiter->active_id) return true;
    if (source->group_id == 0u) return false;
    return source->group_id == active_group(arbiter);
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
    // A newly arrived peer of the composite that ALREADY owns the console is
    // not a change of logical source. Without this, the second half of a
    // Keyboard + Mouse source would move the owning token between two peers of
    // the same owner and emit a neutral boundary in the middle of live input.
    if (best >= 0 && arbiter->sources[best].group_id != 0u &&
        arbiter->sources[best].group_id == active_group(arbiter))
        return false;

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
                           uint32_t group_id,
                           bool *auto_switched)
{
    int existing = find_source(arbiter, key);
    if (existing >= 0) {
        update_metadata(&arbiter->sources[existing], name, vendor_id, product_id);
        arbiter->sources[existing].group_id = group_id;
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
        source->group_id = group_id;
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
    return ns2_input_arbiter_submit_group(arbiter, key, name, vendor_id,
                                          product_id, source_class, 0u,
                                          decision);
}

bool ns2_input_arbiter_submit_group(ns2_input_arbiter_t *arbiter,
                                    const ns2_input_source_key_t *key,
                                    const char *name,
                                    uint16_t vendor_id,
                                    uint16_t product_id,
                                    uint8_t source_class,
                                    uint32_t group_id,
                                    ns2_input_route_decision_t *decision)
{
    if (!arbiter || !key || !decision) return false;
    memset(decision, 0, sizeof(*decision));

    status_begin(arbiter);
    bool transition = false;
    bool auto_switched = false;
    (void)apply_pending(arbiter, &transition);
    int source_index = register_source(arbiter, key, name, vendor_id, product_id,
                                       source_class, group_id, &auto_switched);
    decision->auto_switched = auto_switched ? 1u : 0u;
    if (source_index < 0) {
        status_end(arbiter);
        return false;
    }

    ns2_input_source_info_t *source = &arbiter->sources[source_index];
    decision->transition_applied = transition ? 1u : 0u;
    if (source_owns_console(arbiter, source)) {
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
    uint32_t group = arbiter->sources[index].group_id;
    // Only a request that has NOT yet been applied is an in-flight selection.
    // `pending_id` is deliberately never cleared at the apply site (see
    // ns2_input_arbiter_get_status), so matching it alone would also fire for a
    // long-settled choice -- which is indistinguishable in outcome for a
    // standalone source, but would suppress the composite-source handover
    // below for a user-selected Keyboard + Mouse peer.
    bool pending_target =
        atomic_load_u32(&arbiter->pending_request) != arbiter->applied_request &&
        atomic_load_u32(&arbiter->pending_id) == id;
    bool active = id == arbiter->active_id;
    // A selected target can disappear after the core-0 request but before the
    // next report boundary.  Treat that as an ownership loss too: the caller
    // must re-affirm neutral and must not revive the previous source.
    if (pending_target) active = true;
    memset(&arbiter->sources[index], 0, sizeof(arbiter->sources[index]));

    // A composite logical source survives losing one of its peers.  Hand the
    // owning token to a surviving member instead of surrendering the console:
    // for Keyboard + Mouse this is what keeps the keyboard working when the
    // mouse's battery dies, without the remaining peer being mistaken for an
    // unrelated new source.  The caller still clears the departed role's own
    // state; only whole-source loss neutralizes the slot.
    if (active && !pending_target && group != 0u) {
        for (unsigned i = 0; i < NS2_INPUT_ARBITER_MAX_SOURCES; ++i) {
            if (!arbiter->sources[i].present ||
                arbiter->sources[i].group_id != group)
                continue;
            arbiter->active_id = arbiter->sources[i].id;
            active = false;
            break;
        }
    }
    if (was_active) *was_active = active;
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
        bool active = false;
        for (unsigned i = 0; i < NS2_INPUT_ARBITER_MAX_SOURCES; ++i) {
            if (arbiter->sources[i].present &&
                arbiter->sources[i].key.dev_addr == dev_addr &&
                source_owns_console(arbiter, &arbiter->sources[i]) &&
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
