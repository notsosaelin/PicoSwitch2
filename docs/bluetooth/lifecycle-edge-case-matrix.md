# Bluetooth / management / input lifecycle: edge-case matrix

Status: living document. Last reviewed 2026-08-21.

The failures in this subsystem have never been "one function is wrong". They have been
*combinations* — a pairing window opened while a management client was connected, a source
selected while its transport was dying, a liveness probe racing an in-flight security procedure.

This document classifies the operation orderings a user can actually produce, and records which
ones are made impossible by an invariant rather than by a bespoke test. **The goal is the small set
of invariants, not 160 tests.**

## Derived invariants

These are the load-bearing rules. Each one makes a whole group of rows unreachable.

| # | Invariant | Enforced by |
|---|---|---|
| I1 | A fresh-pair authorization latch belongs to one concrete attempt, identity, and transport. A management latch never authorizes a controller and vice versa. | `mgmt_accept_latched_bonding()`, `conn->fresh_pairing_admitted`, `config_ble.fresh_bond_admitted` — `test_mgmt_access.c` (256-state proof), `test_ns2_lifecycle_model.c` INV3 |
| I2 | Opening or closing controller discovery never invalidates an established management ACL. | `test_bluetooth_closeout_wiring.py::check_controller_discovery_never_touches_management` (mutation-verified) |
| I3 | An active logical input always maps to canonical console slot 0. | `NS2_CONSOLE_SLOT`, `test_ns2_console_slot_wiring.py` (mutation-verified) |
| I4 | Source removal cannot leave `activeId` referencing a nonexistent source. | `ns2_input_arbiter` source keys; `test_ns2_lifecycle_model.c` INV1 |
| I5 | Global radio recovery has exactly one owner, and every transition completes or escalates within a bound. | `ns2_bt_health` phase machine; `test_ns2_bt_health.c::escalation_is_always_bounded` |
| I6 | Recovery never erases trust and never opens admission. | `test_ns2_lifecycle_model.c::health_step` asserts bonds/window/lockout are unchanged by every recovery action |
| I7 | Recovery boot counters cannot create reboot loops. | `RECOVERY_MAX_CONSECUTIVE_REBOOTS`, `RECOVERY_STABLE_CLEAR_MS`; model INV5 (`reboot_requests <= 1`) |
| I8 | An admitted security procedure is not mistaken for a wedged radio, and the exemption is itself bounded. | `NS2_BT_HEALTH_SECURITY_SUPPRESS_MAX_MS`; `test_ns2_bt_health.c`, model field-sequence test |
| I9 | A claimed ACL with no OPEN handle is not by itself evidence of a wedge. | `NS2_BT_HEALTH_NO_HANDLE_GRACE_MS`; `test_ns2_bt_health.c::missing_handle_needs_a_confirmation_interval` |
| I10 | The management bond is only ever attempted on the LE transport. | `AdapterBondStarter`; `AdapterBondStartPolicyTest` |
| I11 | No user operation can enqueue unbounded management work. | `OperationAdmissionGate` (synchronous admission, not the lagging UI busy flag); `OperationAdmissionGateTest` |
| I12 | USB personality transition and Bluetooth lifecycle are independently owned. | core0/core1 split; `STATUS.md` "Bluetooth, pairing, and wake" |
| I13 | Audio sink ownership is independent of console input ownership. | `test_bluetooth_closeout_wiring.py::check_audio_sink_is_independent_of_input_ownership` |

## Classification key

- **INV** — impossible by an invariant above; no per-case test needed.
- **UNIT** — covered by an existing pure/unit/host test.
- **MODEL** — reached by the seeded lifecycle model (`tools/test_ns2_lifecycle_model.c`), invariants checked after every transition.
- **STRUCT** — covered by a source-level structural guard.
- **HW** — genuinely needs hardware; listed in the acceptance matrix or as a remaining gap.
- **UNSUP** — intentionally unsupported; must fail or recover cleanly, never wedge.

## A. Management session + pairing window

| # | Case | Class | Notes |
|---|---|---|---|
| 1 | management connected → open pairing | INV/STRUCT | I2. The 2026-08-21 field case. |
| 2 | management command active → open pairing | MODEL | Commands are single-flight (I11); pairing is a core-1 gesture. |
| 3 | management Refresh active → open pairing | MODEL | Same as 2. |
| 4 | mutation/save polling active → open pairing | MODEL | Flash save is deferred to the control tick; `control_tick_max_gap_ms` bounds it. |
| 5 | personality transition active → open pairing | INV | I12. |
| 6 | management connected but idle → window expires | INV | I2; expiry only stops discovery. |
| 7 | pairing opened → no controller appears | MODEL | Window expires, discovery retires, management untouched. |
| 8 | pairing opened → controller pairs | MODEL/HW | Model covers admission; HW row 2 of the acceptance matrix. |
| 9 | pairing opened → controller pairing fails | MODEL | I1: a failed attempt clears only its own latch. |
| 10 | pairing opened repeatedly | MODEL | `ns2_input_arbiter_request_active` is idempotent; `open_pairing_window` guards a deferred close. |
| 11 | pairing opened while management reconnect in progress | MODEL | I2. |
| 12 | pairing opened while LE management scan in progress | HW | Android-side scan; not modelled. |
| 13 | pairing opened while management SMP itself is in progress | INV/UNIT | I1 + I8. The latch was sampled at connection admission, so a window change mid-SMP cannot revoke or grant it. |

## B. Controller already pairing

| # | Case | Class | Notes |
|---|---|---|---|
| 14 | controller in pairing mode first → then Pico pairing | MODEL/HW | The field sequence; pinned by name in the model. |
| 15 | Pico pairing first → then controller pairing | MODEL | Ordinary path. |
| 16 | connection starts exactly as window expires | UNIT | `pairing_close_deferred`; documented in `btstack_host_close_pairing_window()`. |
| 17 | raw ACL but not security → window expires | UNIT | Same deferral. |
| 18 | security procedure running → window expires | INV | I1: admission was latched at attempt start. |
| 19 | HID setup running → window expires | INV | I1. |
| 20 | controller powers off during security | MODEL | Candidate cleared; no trust written. |
| 21 | powers off after trust written, before HID-ready | UNIT | `ns2_bt_classic_key_commit_allowed` — key committed only after authentication. |
| 22 | retries while previous cleanup pending | MODEL | Connection generation in the source key. |
| 23 | second controller advertises mid-pairing | MODEL | I1: one attempt owns the latch. |
| 24 | bonded reconnect while fresh pairing active | MODEL | `ns2_bt_admission_decide` returns RECONNECT vs FRESH independently. |
| 25 | Classic and BLE candidates race | MODEL | Separate latches per transport (I1). |

## C. Management pairing vs controller pairing

| # | Case | Class | Notes |
|---|---|---|---|
| 26–29 | management SMP/reconnect overlapping controller SSP/SMP | MODEL | I1; model exercises both latches concurrently. |
| 30 | management SMP dialog outlives the window | INV/UNIT | I1 — the whole reason `config_ble.fresh_bond_admitted` exists. |
| 31 | controller handshake outlives the window | INV | I1 + deferred close. |
| 32 | one attempt admitted, then another peer appears | MODEL | I1. |
| 33 | a latch from one transport must never authorize the other | INV/UNIT/MODEL | I1; `test_mgmt_access.c` exhaustive, model INV3. |
| 34 | failed management pairing must not poison controller admission | MODEL | Explicitly modelled (`OP_MGMT_BOND_FAIL`). |
| 35 | failed controller pairing must not poison management admission | MODEL | Explicitly modelled. |

## D. Discovery / advertising ownership

| # | Case | Class | Notes |
|---|---|---|---|
| 36 | inquiry + scan + management advertising all requested | HW | Radio-capability question; only hardware can answer. **Open.** |
| 37 | management client connected while scan/inquiry rearmed | STRUCT/HW | I2 structurally; the radio behaviour is HW row 1–3. |
| 38 | management disconnects during inquiry | MODEL | |
| 39 | advertising restart while candidate connecting | UNIT | `mgmt_should_advertise` requires no client connected. |
| 40 | controller ready while advertising transition pending | MODEL | |
| 41 | wake advertiser active while pairing requested | UNIT | `BTLIFE_CAUSE_WAKE` suppression; `scan_requested` replay. |
| 42 | wake advertiser expires while management reconnect pending | UNIT | Same path. |
| 43 | personality transition during pairing discovery | INV | I12. |
| 44 | USB re-enumeration while window active | INV | I12. |
| 45 | `mgmt off/on` while controller pairing active | INV/UNIT | I1: `mgmt off` revokes only the management latch. |

## E. Bond / trust asymmetry

| # | Case | Class | Notes |
|---|---|---|---|
| 46 | Android forgets Pico, Pico still trusts Android | **Confirmed HW** | 2026-08-21; BTstack updates the existing public-identity slot in place. |
| 47 | Pico removes Android bond, Android still trusts Pico | HW | Settings → Remove pairing. **Open.** |
| 48 | controller forgets Pico, Pico retains trust | HW | **Open.** |
| 49 | Pico wipes trust while peers still think bonded | HW | Wipe matrix, `docs/bluetooth/VALIDATION.md`. **Open.** |
| 50 | stale LE bond slot replaced by fresh bond, same identity | **Confirmed HW** | 2026-08-21, slot reused not duplicated. |
| 51 | trust DB full / last slot consumed | UNIT | `le_device_db_add` returns -1; bonds capacity surfaced by `btreconnect`. **Untested at capacity.** |
| 52 | failed pairing must not overwrite a valid old key | UNIT | `classic_restore_existing_key()`; `ns2_bt_classic_key_update_admitted`. |
| 53 | authenticated key change preserves previous key until success | UNIT | Same. |
| 54 | reboot between key generation and persistence | HW | **Open.** |
| 55 | reset immediately after successful pairing | HW | **Open.** |
| 56 | save pending while pairing succeeds | HW | Deferred flash write on the control tick. **Open.** |

## F. Active input / Controller Link interactions

| # | Case | Class | Notes |
|---|---|---|---|
| 57 | physical active → Controller Link starts | **Confirmed HW** | 2026-08-21. |
| 58 | Controller Link active → physical reconnects | MODEL | I1 class ranking; explicit choice never overridden. |
| 59 | active source disconnects, alternate exists | UNIT/MODEL | `ns2_input_arbiter` disconnect policy; explicit vs automatic. |
| 60 | inactive source disconnects while Link active | MODEL | I4. |
| 61 | Controller Link HID transport dies while selected | **Confirmed HW** | This is what the slot misroute looked like; now I3. |
| 62 | Android app process dies while Link owns input | HW | **Open.** |
| 63 | management GATT dies while Link HID alive | HW | Independent transports. **Open.** |
| 64 | Link remains Playing but reports stop | **Confirmed HW** | Root-caused to I3 on 2026-08-21. |
| 65 | rapid source switching | MODEL | Model does this constantly. |
| 66 | switch exactly as first fresh report arrives | UNIT | `awaiting_fresh` latch; `test_ns2_input_arbiter.c`. |
| 67 | switch while a report is mid-decode | INV | Selection is committed at report boundaries only. |
| 68 | two Classic connections in the opposite order | **Confirmed HW** | The 2026-08-21 misroute; now I3. |
| 69 | one source lacks a device name | UNIT | `ns2_input_source_display_name`; `test_ns2_input_arbiter.c`. |
| 70 | device reconnects under a different connection index | INV | I3 + connection generation in the source key. |
| 71 | stale registry entry after disconnect | UNIT | Generation-keyed disconnect. |
| 72 | activeId points at a source removed this turn | INV/MODEL | I4, model INV1. |

## G. Audio coexistence

| # | Case | Class | Notes |
|---|---|---|---|
| 73–78 | audio active during Refresh / pairing / fresh pair / reconnect / input handover | **Confirmed HW (73, 76, 77, 78)** | 75-minute soak, 2026-08-21. 74/75 **open**. |
| 79 | audio sink disconnects while Link active | HW | **Open.** |
| 80 | audio queue saturated while a management command arrives | UNIT | Core-0 fairness: `config_wireless_task()` at the audio transfer boundary. |
| 81 | USB re-enumeration while audio active | **Confirmed HW** | Interruption is the deliberate USB disconnect. |
| 82 | HCI recovery while audio link already dead | MODEL | I6. |
| 83 | HCI recovery while audio link healthy | HW | **Open.** |
| 84 | management spam while audio active | UNIT | I11; stutter allowed, wedge forbidden. |

## H. HCI / CYW43 recovery races

| # | Case | Class | Notes |
|---|---|---|---|
| 85 | quiet threshold reached during legitimate pairing | UNIT/MODEL | **I8 — the fix in this pass.** |
| 86 | probe queued behind security commands | UNIT | Any later HCI event satisfies the probe. |
| 87 | unrelated HCI event arrives while probe pending | UNIT | `unrelated_hci_progress_satisfies_liveness_probe`. |
| 88 | probe target disconnects before the command executes | UNIT | **I9 — the fix in this pass.** |
| 89 | probe returns failure immediately | UNIT | `timed_out_probe_cycles_hci` / `probe_failed`. |
| 90 | probe times out | UNIT | Same. |
| 91 | power-off requested while security is active | INV | I8. |
| 92 | OFF state never arrives | **Confirmed HW** | 2026-08-21: escalated to watchdog reboot. |
| 93 | OFF completes but ON fails | UNIT | `POWERING_ON` timeout → reboot. |
| 94 | ON succeeds but advertising not restored | HW | Desired state must be re-derived after WORKING. **Open.** |
| 95 | ON succeeds but scan/inquiry not restored | HW | **Open.** |
| 96 | ON succeeds but trust not reloaded | **Confirmed HW** | Trust survived the 2026-08-21 reboot. |
| 97 | recovery while management UI thinks Connected | HW | UI must not claim Connected past transport death. **Open.** |
| 98 | recovery while Controller Link thinks Playing | HW | **Open.** |
| 99 | second failure immediately after recovery | UNIT | Rate limiter. |
| 100 | rate limiter suppresses reboot | UNIT | `RECOVERY_MAX_CONSECUTIVE_REBOOTS`. |
| 101 | watchdog reboot occurs | **Confirmed HW** | 2026-08-21, `last_boot_cause=1`. |
| 102 | second recovery boot before stable-clear | UNIT | Counter increments; I7. |
| 103 | stable interval clears the counter | **Confirmed HW** | `consecutive_boots` was 0 after the event. |
| 104 | intentional reboot must not be misclassified | INV | Only `ns2_bt_recovery_request_reboot()` writes the cause. |
| 105 | flash save/lockout must not cause false liveness detection | UNIT | Flash parks core 0; health runs on core 1 and keys on HCI events. |

## I. USB / personality transitions

| # | Case | Class | Notes |
|---|---|---|---|
| 106–111 | personality change under every ownership combination | **Confirmed HW (106, 107, 110, 111)** | 2026-08-21. 108/109 **open**. |
| 112 | two rapid personality taps | UNIT | I11 + volatile cycle. |
| 113 | re-enumeration delayed | HW | **Open.** |
| 114 | USB host never comes back | HW | Observed post-reboot: enumerated, but no host application reopened the HID device. Benign. |
| 115 | Bluetooth alive while USB identity changes | **Confirmed HW** | I12. |
| 116 | Bluetooth dies during USB transition, must reconnect cleanly | HW | **Open.** |

## J. Android lifecycle

| # | Case | Class | Notes |
|---|---|---|---|
| 117 | app backgrounded during pairing dialog | UNIT | `awaitAdapterBond` polls the retained device; broadcast is advisory. |
| 118 | Activity stopped/resumed during BOND_BONDING | UNIT | `resumePendingAdapterBond`. |
| 119 | bond broadcast missed | UNIT | `AdapterBondWaitPolicyTest`. |
| 120 | process killed after bond, before persistence | HW | **Open.** |
| 121 | valid bond, no saved relationship | UNIT | `AdapterRelationshipLifecycleTest`. |
| 122 | saved relationship, bond removed | UNIT | RepairRequired path. |
| 123 | correct address but cache says DUAL | **Confirmed HW** | I10 — the root cause fixed on 2026-08-21. |
| 124 | hidden LE createBond seam unavailable | UNIT | `AdapterBondStartPolicyTest` → `LeGattInitiated`. |
| 125 | hidden method exists but refused | UNIT | Same test → `Unavailable`, reported not papered over. |
| 126 | GATT-provoked LE SMP fallback succeeds | HW | **Open** — Strong Evidence only. |
| 127 | fallback fails without poisoning the next attempt | UNIT | Coordinator generation. |
| 128–129 | Android Bluetooth toggled off/on | HW | **Open.** |
| 130 | phone dozes during a long session | **Confirmed HW** | 79-minute session survived. |
| 131 | foreground after long idle with stale GATT | HW | **Open.** |
| 132 | callback from a retired GATT generation | UNIT | `GattCallbackAuthority`; `staleCallback`. |
| 133 | disconnect callback after a new generation exists | UNIT | Same. |

## K. User abuse / rapid operations

| # | Case | Class | Notes |
|---|---|---|---|
| 134–142 | Refresh ×20, Pair spam, gesture spam, near-simultaneous Pair+X | UNIT | I11 — `OperationAdmissionGate` admits synchronously, before a coroutine can queue. |
| 143 | app Reconnect while adapter recovery underway | HW | **Open.** |
| 144 | BOOTSEL gestures during management mutation | MODEL | I2. |
| 145 | wipe during an active management session | UNIT | Wipe includes the management bond and disconnects the client deliberately. |
| 146 | wipe while controller pairing active | MODEL | Lockout closes admission first. |
| 147 | wipe during management fresh pairing | MODEL | Same. |
| 148 | controller power-cycled repeatedly during discovery | MODEL | Generation-keyed sources. |

## L. Long-run / wrap / resource

| # | Case | Class | Notes |
|---|---|---|---|
| 149 | many connect/disconnect generations | MODEL | 16 000 modelled transitions per run. |
| 150 | many pairing windows without a pair | MODEL | |
| 151 | many failed fresh-admission attempts | **Confirmed HW** | `admission.reject_window` odometer. |
| 152 | management sequence/counter wrap | UNIT | Sequence is a Kotlin `Long`. |
| 153 | timer `uint32` wrap-safe comparisons | UNIT | All health/lifecycle comparisons use `(uint32_t)(now - since) >= interval`. **A 0 timestamp is no longer a sentinel** — that ambiguity was found by the model on 2026-08-21 and replaced with explicit armed flags. |
| 154 | source IDs reused after disconnect | INV | IDs are never reused within a boot. |
| 155 | BTstack connection handles reused | INV | I3 + connection generation. |
| 156 | GATT generations increasing | UNIT | Monotonic. |
| 157 | bond DB churn across all slots | HW | **Open.** |
| 158 | repeated personality re-enumerations | HW | **Open.** |
| 159 | repeated health probes over hours | **Confirmed HW** | Zero probes needed in a 79-minute soak. |
| 160 | long mixed soak with occasional user actions | **Confirmed HW** | 2026-08-21. |

## Summary

| Class | Count (approx.) |
|---|---|
| Confirmed on hardware | 22 |
| Impossible by invariant (INV) | 21 |
| Existing pure/unit/host test (UNIT) | 44 |
| Reached by the lifecycle model (MODEL) | 33 |
| Structural guard (STRUCT) | 4 |
| Still genuinely requires hardware (**open**) | 36 |

The 36 open rows are deliberately not turned into bespoke tests. Most are one of four hardware
questions: *(a)* whether inquiry + scan + management advertising is a supported radio combination
on CYW43, *(b)* whether desired advertising/scan state is correctly re-derived after an HCI
recovery, *(c)* asymmetric-trust cleanup, and *(d)* Android process/Bluetooth lifecycle. Answering
those four closes most of the list.
