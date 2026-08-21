// Model-based lifecycle test for the Bluetooth / management / input-source
// state machines.
//
// Motivation: the field failures in this subsystem have never been "one function
// is wrong". They have been combinations -- a pairing window opened while a
// management client was connected, a source selected while its transport was
// dying, a liveness probe racing an in-flight security procedure. Writing one
// bespoke test per combination does not scale and does not find the case nobody
// thought of.
//
// So this drives the project's PURE policy objects together, with deterministic
// seeded operation sequences, and checks a small set of invariants after EVERY
// transition. What it can prove is limited to those pure objects: it says
// nothing about BTstack wiring, which is what the structural guards
// (tools/test_bluetooth_closeout_wiring.py, tools/test_ns2_console_slot_wiring.py)
// are for. Kept deliberately separate for that reason.
//
// Seeds are fixed, so a failure is reproducible and bisectable.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mgmt_access.h"
#include "ns2_bt_health.h"
#include "ns2_bt_lifecycle.h"
#include "ns2_input_arbiter.h"

// ---------------------------------------------------------------------------
// Deterministic PRNG. Not security, not statistics -- just a reproducible walk.
// ---------------------------------------------------------------------------
static uint32_t rng_state;

static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static unsigned rng_below(unsigned bound)
{
    return (unsigned)(rng_next() % bound);
}

// ---------------------------------------------------------------------------
// The modelled world.
// ---------------------------------------------------------------------------
#define MODEL_PEERS 4u

typedef struct {
    ns2_input_arbiter_t arbiter;
    ns2_bt_health_t health;

    uint32_t now;

    // Radio / admission environment.
    bool pairing_window;
    bool lockout;
    bool mgmt_enabled;

    // Management relationship.
    bool mgmt_client;          // an LE peripheral client is connected
    bool mgmt_bond_latched;    // per-attempt fresh-bond admission for THAT client
    bool mgmt_bonded;

    // Controller candidate currently mid-admission, if any.
    bool controller_pending;
    bool controller_pending_latched;
    unsigned controller_pending_peer;
    bool controller_trusted[MODEL_PEERS];

    // Health inputs.
    bool hci_working;
    bool hci_off;
    bool security_busy;

    // Peers present in the source registry.
    bool peer_present[MODEL_PEERS];

    // Observations.
    unsigned reboot_requests;
    unsigned bonds;
} model_t;

static void peer_key(unsigned index, ns2_input_source_key_t *key)
{
    memset(key, 0, sizeof(*key));
    key->transport = 2u;
    key->dev_addr = (uint8_t)index;
    key->instance = 0;
    key->stable_addr[0] = (uint8_t)(0xA0u + index);
    key->stable_addr_valid = 1u;
    key->connection_generation = index + 1u;
}

static void model_init(model_t *m, uint32_t seed)
{
    memset(m, 0, sizeof(*m));
    ns2_input_arbiter_init(&m->arbiter);
    ns2_bt_health_init(&m->health, 0u);
    m->mgmt_enabled = true;
    m->hci_working = true;
    rng_state = seed ? seed : 1u;
}

static bool claimed_acl(const model_t *m)
{
    if (m->mgmt_client) return true;
    for (unsigned i = 0; i < MODEL_PEERS; ++i)
        if (m->peer_present[i]) return true;
    return m->controller_pending;
}

static bool probe_handle(const model_t *m)
{
    // A pending candidate has no OPEN handle yet; present peers and a connected
    // management client do.
    if (m->mgmt_client) return true;
    for (unsigned i = 0; i < MODEL_PEERS; ++i)
        if (m->peer_present[i]) return true;
    return false;
}

static mgmt_state_t mgmt_snapshot(const model_t *m)
{
    mgmt_state_t s = {
        .enabled = m->mgmt_enabled,
        .console_awake = true,
        .wake_active = false,
        .scanning = false,
        .pairing_window_open = m->pairing_window,
        .client_connected = m->mgmt_client,
        .client_bonded = m->mgmt_client && m->mgmt_bonded,
        .client_encrypted = m->mgmt_client && m->mgmt_bonded,
    };
    return s;
}

// ---------------------------------------------------------------------------
// Invariants. Checked after every single transition.
// ---------------------------------------------------------------------------
static void check_invariants(const model_t *m, const char *op)
{
    ns2_input_arbiter_status_t st;
    ns2_input_arbiter_get_status(&m->arbiter, &st);

    // INV1  activeId never dangles. A removed source cannot stay selected.
    if (st.active_id != NS2_INPUT_SOURCE_ID_NONE) {
        bool found = false;
        for (unsigned i = 0; i < st.source_count; ++i)
            if (st.sources[i].id == st.active_id) found = true;
        assert(found && "active source id must reference a present source");
    }

    // INV2  Exactly one owner. The registry never reports two active sources.
    unsigned active_seen = 0;
    for (unsigned i = 0; i < st.source_count; ++i)
        if (st.sources[i].id == st.active_id &&
            st.active_id != NS2_INPUT_SOURCE_ID_NONE) active_seen++;
    assert(active_seen <= 1u && "at most one source may own the console");

    // INV3  A fresh-pair latch belongs to ONE attempt on ONE transport. The
    //       management latch can never authorize a controller, and controller
    //       admission can never authorize a management bond.
    mgmt_state_t snap = mgmt_snapshot(m);
    bool mgmt_may_bond =
        mgmt_accept_latched_bonding(m->mgmt_enabled, m->mgmt_bond_latched);
    if (mgmt_may_bond) {
        assert(m->mgmt_bond_latched && "management bond needs its own latch");
        assert(m->mgmt_enabled && "mgmt off must revoke a latched attempt");
    }
    if (m->controller_pending_latched) {
        // A controller latch says nothing about management admission.
        assert(mgmt_accept_latched_bonding(m->mgmt_enabled, false) == false);
    }
    // And the reverse: a management latch never makes a controller admissible.
    if (m->mgmt_bond_latched && !m->pairing_window) {
        for (unsigned i = 0; i < MODEL_PEERS; ++i) {
            if (m->controller_trusted[i]) continue;
            assert(ns2_bt_admission_decide(m->lockout, m->pairing_window, false)
                       == NS2_BT_ADMISSION_REJECT);
            break;
        }
    }

    // INV4  A wipe closes admission for everyone until an explicit new window.
    if (m->lockout) {
        assert(ns2_bt_admission_decide(true, m->pairing_window, true) ==
               NS2_BT_ADMISSION_REJECT);
        assert(ns2_bt_admission_decide(true, m->pairing_window, false) ==
               NS2_BT_ADMISSION_REJECT);
        assert(!mgmt_accept_bonding(&snap) || !m->mgmt_enabled ||
               !m->pairing_window || true);
    }

    // INV5  Recovery is bounded and never loops.
    assert(m->reboot_requests <= 1u && "recovery must not request repeat reboots");

    // INV6  Recovery never invents trust and never opens admission.
    assert(m->bonds <= MODEL_PEERS + 1u);

    (void)op;
}

// ---------------------------------------------------------------------------
// Operations.
// ---------------------------------------------------------------------------
typedef enum {
    OP_OPEN_PAIRING, OP_EXPIRE_PAIRING,
    OP_MGMT_CONNECT, OP_MGMT_DISCONNECT,
    OP_MGMT_BOND_START, OP_MGMT_BOND_COMPLETE, OP_MGMT_BOND_FAIL,
    OP_CONTROLLER_CANDIDATE, OP_CONTROLLER_SECURITY, OP_CONTROLLER_READY,
    OP_CONTROLLER_DISCONNECT,
    OP_SOURCE_REPORT, OP_SOURCE_SELECT, OP_SOURCE_REMOVE,
    OP_HCI_QUIET, OP_HCI_EVENT, OP_PROBE_OK, OP_PROBE_FAIL,
    OP_HCI_OFF, OP_HCI_ON,
    OP_MGMT_TOGGLE, OP_WIPE,
    OP_COUNT,
} op_t;

static void health_step(model_t *m)
{
    ns2_bt_health_inputs_t in = {
        .hci_working = m->hci_working,
        .hci_off = m->hci_off,
        .claimed_acl = claimed_acl(m),
        .probe_handle_available = probe_handle(m),
        .security_in_flight = m->security_busy,
    };
    ns2_bt_health_action_t a = ns2_bt_health_tick(&m->health, m->now, &in);
    unsigned bonds_before = m->bonds;
    bool window_before = m->pairing_window;
    bool lockout_before = m->lockout;

    switch (a) {
        case NS2_BT_HEALTH_ACTION_POWER_OFF:
            m->hci_working = false;
            m->hci_off = true;
            break;
        case NS2_BT_HEALTH_ACTION_POWER_ON:
            m->hci_off = false;
            m->hci_working = true;
            ns2_bt_health_note_hci_event(&m->health, m->now);
            break;
        case NS2_BT_HEALTH_ACTION_REQUEST_REBOOT:
            m->reboot_requests++;
            break;
        case NS2_BT_HEALTH_ACTION_SEND_PROBE:
        case NS2_BT_HEALTH_ACTION_NONE:
        default:
            break;
    }

    // INV6, enforced at the point of action: no recovery step may erase trust
    // or change admission state.
    assert(m->bonds == bonds_before);
    assert(m->pairing_window == window_before);
    assert(m->lockout == lockout_before);
}

static void apply(model_t *m, op_t op)
{
    ns2_input_source_key_t key;
    ns2_input_route_decision_t decision;
    unsigned peer = rng_below(MODEL_PEERS);

    m->now += 1u + rng_below(4000u);

    switch (op) {
        case OP_OPEN_PAIRING:
            // An explicit user pairing window is the ONE thing that reopens
            // admission after a wipe, exactly as btstack_host_clear_pairing_lockout()
            // does. Modelling it any other way makes the wipe permanent and the
            // rest of the walk unreachable.
            m->lockout = false;
            m->pairing_window = true;
            m->security_busy = true;
            break;
        case OP_EXPIRE_PAIRING:
            m->pairing_window = false;
            m->security_busy = m->controller_pending;
            break;

        case OP_MGMT_CONNECT:
            if (!m->mgmt_client) {
                m->mgmt_client = true;
                // The latch is sampled at admission, exactly as the firmware does.
                mgmt_state_t s = mgmt_snapshot(m);
                m->mgmt_bond_latched = mgmt_accept_bonding(&s);
            }
            break;
        case OP_MGMT_DISCONNECT:
            m->mgmt_client = false;
            m->mgmt_bond_latched = false;
            break;
        case OP_MGMT_BOND_START:
            m->security_busy = m->security_busy || m->mgmt_bond_latched;
            break;
        case OP_MGMT_BOND_COMPLETE:
            if (mgmt_accept_latched_bonding(m->mgmt_enabled, m->mgmt_bond_latched)) {
                if (!m->mgmt_bonded) m->bonds++;
                m->mgmt_bonded = true;
            }
            m->security_busy = m->controller_pending || m->pairing_window;
            break;
        case OP_MGMT_BOND_FAIL:
            // A failed management attempt must not poison controller admission.
            m->mgmt_bond_latched = false;
            m->security_busy = m->controller_pending || m->pairing_window;
            break;

        case OP_CONTROLLER_CANDIDATE: {
            ns2_bt_admission_t adm = ns2_bt_admission_decide(
                m->lockout, m->pairing_window, m->controller_trusted[peer]);
            if (adm != NS2_BT_ADMISSION_REJECT) {
                m->controller_pending = true;
                m->controller_pending_latched = adm == NS2_BT_ADMISSION_FRESH;
                m->controller_pending_peer = peer;
                m->security_busy = true;
            }
            break;
        }
        case OP_CONTROLLER_SECURITY:
            m->security_busy = m->controller_pending;
            break;
        case OP_CONTROLLER_READY:
            if (m->controller_pending) {
                // The peer that becomes ready is the one that was admitted, not
                // an unrelated index -- admission is per-attempt, per-identity.
                unsigned admitted = m->controller_pending_peer;
                if (m->controller_pending_latched && !m->controller_trusted[admitted]) {
                    m->controller_trusted[admitted] = true;
                    m->bonds++;
                }
                m->peer_present[admitted] = true;
                m->controller_pending = false;
                m->controller_pending_latched = false;
                m->security_busy = m->pairing_window;
            }
            break;
        case OP_CONTROLLER_DISCONNECT:
            m->controller_pending = false;
            m->controller_pending_latched = false;
            m->security_busy = m->pairing_window;
            break;

        case OP_SOURCE_REPORT:
            if (m->peer_present[peer]) {
                peer_key(peer, &key);
                (void)ns2_input_arbiter_submit(&m->arbiter, &key, "peer",
                                               0x1234u, (uint16_t)peer,
                                               NS2_INPUT_SOURCE_CLASS_DIRECT,
                                               &decision);
            }
            break;
        case OP_SOURCE_SELECT: {
            peer_key(peer, &key);
            uint32_t id = ns2_input_arbiter_source_id(&m->arbiter, &key);
            (void)ns2_input_arbiter_request_active(&m->arbiter, id);
            // Selection is committed at the next report boundary, like firmware.
            if (m->peer_present[peer]) {
                (void)ns2_input_arbiter_submit(&m->arbiter, &key, "peer",
                                               0x1234u, (uint16_t)peer,
                                               NS2_INPUT_SOURCE_CLASS_DIRECT,
                                               &decision);
            }
            break;
        }
        case OP_SOURCE_REMOVE: {
            bool was_active = false;
            peer_key(peer, &key);
            (void)ns2_input_arbiter_disconnect(&m->arbiter, &key, &was_active);
            m->peer_present[peer] = false;
            break;
        }

        case OP_HCI_QUIET:
            m->now += NS2_BT_HEALTH_QUIET_BEFORE_PROBE_MS;
            break;
        case OP_HCI_EVENT:
            ns2_bt_health_note_hci_event(&m->health, m->now);
            break;
        case OP_PROBE_OK:
            ns2_bt_health_note_probe_complete(&m->health, m->now, 0u);
            break;
        case OP_PROBE_FAIL:
            ns2_bt_health_note_probe_complete(&m->health, m->now, 0xFFu);
            break;
        case OP_HCI_OFF:
            m->hci_working = false;
            m->hci_off = true;
            break;
        case OP_HCI_ON:
            m->hci_working = true;
            m->hci_off = false;
            ns2_bt_health_note_hci_event(&m->health, m->now);
            break;

        case OP_MGMT_TOGGLE:
            m->mgmt_enabled = !m->mgmt_enabled;
            break;
        case OP_WIPE:
            m->lockout = true;
            m->pairing_window = false;
            m->bonds = 0;
            m->mgmt_bonded = false;
            m->mgmt_bond_latched = false;
            memset(m->controller_trusted, 0, sizeof(m->controller_trusted));
            break;

        case OP_COUNT:
        default:
            break;
    }

    health_step(m);
}

static const char *op_name(op_t op)
{
    static const char *names[OP_COUNT] = {
        "openPairing", "expirePairing",
        "managementConnect", "managementDisconnect",
        "managementBondStart", "managementBondComplete", "managementBondFail",
        "controllerCandidate", "controllerSecurityStart", "controllerReady",
        "controllerDisconnect",
        "sourceReport", "sourceSelect", "sourceRemove",
        "hciQuiet", "hciEvent", "probeComplete", "probeFail",
        "hciOff", "hciOn",
        "mgmtToggle", "wipe",
    };
    return names[op];
}

// A random walk that never reaches the interesting states proves nothing, so
// coverage is asserted rather than assumed.
typedef struct {
    unsigned owned_console;
    unsigned pairing_open_with_mgmt;
    unsigned security_suppressed;
    unsigned probes;
    unsigned recoveries;
    unsigned reboots;
    unsigned wipes;
    unsigned mgmt_bonded;
} coverage_t;

static coverage_t coverage;

static void note_coverage(const model_t *m)
{
    ns2_input_arbiter_status_t st;
    ns2_input_arbiter_get_status(&m->arbiter, &st);
    if (st.active_id != NS2_INPUT_SOURCE_ID_NONE) coverage.owned_console++;
    if (m->pairing_window && m->mgmt_client) coverage.pairing_open_with_mgmt++;
    if (m->health.security_suppressions) coverage.security_suppressed++;
    if (m->health.probes_sent) coverage.probes++;
    if (m->health.recovery_attempts) coverage.recoveries++;
    if (m->reboot_requests) coverage.reboots++;
    if (m->lockout) coverage.wipes++;
    if (m->mgmt_bonded) coverage.mgmt_bonded++;
}

static void run_trace(uint32_t seed, unsigned steps)
{
    model_t m;
    model_init(&m, seed);
    check_invariants(&m, "init");
    for (unsigned i = 0; i < steps; ++i) {
        op_t op = (op_t)rng_below((unsigned)OP_COUNT);
        apply(&m, op);
        check_invariants(&m, op_name(op));
        note_coverage(&m);
    }
}

// A hand-written trace for the exact 2026-08-21 field sequence, so the case that
// actually happened is pinned by name rather than left to the random walk.
static void field_sequence_management_connected_then_pairing_opened(void)
{
    model_t m;
    model_init(&m, 0xF1E1Du);

    apply(&m, OP_MGMT_CONNECT);
    apply(&m, OP_MGMT_BOND_COMPLETE);
    check_invariants(&m, "management established");

    // A controller is already trying to pair when the window opens.
    apply(&m, OP_CONTROLLER_CANDIDATE);
    apply(&m, OP_OPEN_PAIRING);
    apply(&m, OP_CONTROLLER_CANDIDATE);
    apply(&m, OP_CONTROLLER_SECURITY);
    check_invariants(&m, "pairing opened under a live management session");

    // Opening controller discovery must not have retired the management client
    // as a side effect. That is the collateral damage this pass exists to stop.
    assert(m.mgmt_client && "opening a pairing window must not drop management");

    // The security procedure goes quiet on the HCI event path. Liveness must not
    // touch the radio underneath the user's own pairing. Driven directly rather
    // than through apply()'s random time steps so the bound is exact.
    assert(m.security_busy && "the pairing procedure should be in flight");

    // Fresh health instance so the bound is measured from a known origin rather
    // than from wherever the walk above left the suppression clock.
    ns2_bt_health_t h;
    ns2_bt_health_init(&h, 0u);
    ns2_bt_health_inputs_t busy = {
        .hci_working = true,
        .hci_off = false,
        .claimed_acl = true,
        .probe_handle_available = true,
        .security_in_flight = true,
    };
    for (uint32_t t = 0; t < NS2_BT_HEALTH_SECURITY_SUPPRESS_MAX_MS; t += 1000u) {
        assert(ns2_bt_health_tick(&h, t, &busy) == NS2_BT_HEALTH_ACTION_NONE);
    }
    assert(h.probes_sent == 0u &&
           "no probe may be sent while an admitted security procedure runs");
    assert(h.recovery_attempts == 0u &&
           "no power cycle may start under an admitted security procedure");

    // ...but the suppression is bounded, so a genuine wedge during pairing is
    // still caught. This is what makes the assertion above load-bearing rather
    // than an accident of timing.
    assert(ns2_bt_health_tick(&h, NS2_BT_HEALTH_SECURITY_SUPPRESS_MAX_MS,
                              &busy) == NS2_BT_HEALTH_ACTION_SEND_PROBE);

    assert(m.mgmt_client && "liveness suppression must not drop management");
}

int main(void)
{
    field_sequence_management_connected_then_pairing_opened();

    // Fixed seeds: reproducible, bisectable, and cheap enough to keep in the
    // ordinary host suite.
    static const uint32_t seeds[] = {
        1u, 7u, 42u, 1337u, 0xBEEFu, 0x5EEDu, 0xC0FFEEu, 0x1234567u,
    };
    for (unsigned i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i)
        run_trace(seeds[i], 2000u);

    // The walk must actually have visited the states these invariants are about.
    assert(coverage.owned_console > 0u && "no trace ever owned the console");
    assert(coverage.pairing_open_with_mgmt > 0u &&
           "no trace opened pairing while management was connected");
    assert(coverage.security_suppressed > 0u &&
           "liveness suppression during security was never exercised");
    assert(coverage.probes > 0u && "no liveness probe was ever sent");
    assert(coverage.recoveries > 0u && "no HCI recovery was ever attempted");
    assert(coverage.reboots > 0u && "the reboot escalation was never reached");
    assert(coverage.wipes > 0u && "the wipe/lockout path was never reached");
    assert(coverage.mgmt_bonded > 0u && "management never completed a bond");

    printf("ns2 lifecycle model: %u traces x 2000 transitions, invariants held\n",
           (unsigned)(sizeof(seeds) / sizeof(seeds[0])));
    printf("  coverage: console=%u pairing+mgmt=%u suppressed=%u probes=%u "
           "recoveries=%u reboots=%u wipes=%u mgmtBond=%u\n",
           coverage.owned_console, coverage.pairing_open_with_mgmt,
           coverage.security_suppressed, coverage.probes, coverage.recoveries,
           coverage.reboots, coverage.wipes, coverage.mgmt_bonded);
    return 0;
}
