# Overnight autonomous investigation (2026-08-13)

Owner asleep; hardware left connected; `tools/mgmt_soak.ps1` owns the UART (COM11) — **no UART
commands issued during this session** so the soak is undisturbed. All work here is repository/host-side:
code tracing, host-test development, design, and documentation. Evidence standard is explicit:
**CONFIRMED** (repo/code-proven or UART-proven earlier) · **HYPOTHESIS** · **WEAKENED/REFUTED** ·
**NEEDS VALIDATION**. No speculative firmware changes; diagnostics designed/prepared for review.

Companion: [`in-band-mgmt-coexistence-failure-2026-08-12.md`](in-band-mgmt-coexistence-failure-2026-08-12.md).

---

## P3 — Amiibo stale-metadata: full cache-lifecycle audit (task 9)

`amiiboInfoCache` maps `entry.key → decrypted {owner, nickname, setupDate, lastWriteDate,
writeCounter, appData}` (or `{error}`). `amiiboInfoFor` returns the cached value on a key hit **without
checking whether the bytes changed** — so any path that rewrites an entry's bytes under a reused key
leaves the displayed metadata stale.

**Audited every mutation path (web/index.html):**
| Path | Byte-write chokepoint | Was it invalidated before? |
|---|---|---|
| **Sync** (`saveCurrentAmiibo`) | `cacheSingleAmiiboEntry` | ❌ (the reported bug) |
| **Initialize** (`initializeSelectedAmiibo`) | `cacheSingleAmiiboEntry` | ✅ (ad-hoc delete) |
| **Single-file import** | `cacheSingleAmiiboEntry` | ❌ (latent; new key so usually benign, but a 540-byte identity-keyed re-import could collide) |
| **Directory / library import** (bulk) | `replaceCachedAmiiboLibrary` | ❌ (latent) |
| Select / change active tag | (no byte change) | n/a — keyed per entry, correct |
| Key import / removal | — | ✅ clears whole cache |

**Class fix (not an instance patch):** invalidate at the two byte-persistence chokepoints —
`cacheSingleAmiiboEntry` deletes `entry.key` + `replaceKey`; `replaceCachedAmiiboLibrary` clears the
cache. The ad-hoc deletes in Sync and Initialize were then **removed** so there is one invariant:
*persisting amiibo bytes invalidates that entry's decrypted-info cache.* This covers Sync, Initialize,
and both import paths uniformly. **NEEDS browser+adapter validation** (frontend; the decrypt path uses
`SubtleCrypto`, so it is not host-testable without a portal JS harness — a harness is future work, and
the fix being centralized makes it a single, testable surface if one is added).

## Personality-switch path (task 7, read-only)

`usb_apply_personality(next, reason)` (usb.c:109): `tud_disconnect()` → `sleep_ms(USB_DETACH_MS)` →
`usb_reset_personality_state(next)` → `g_usb_personality = next` → `tud_connect()`. Idempotent
(next==old ignored). Used by the BOOTSEL controller cycle, the Config toggle, and the app
`personality` request (via the edge-triggered `g_usb_personality_request_pending` flag consumed in
`usb_core_task`).

**Torn down vs preserved across a switch (CONFIRMED from code):**
| State | On a personality switch |
|---|---|
| Console-facing USB | Fully re-enumerated (disconnect→reconnect); console sees the old controller drop + new appear |
| **Incoming** personality's private module state | Reset (`ns2_init` / `switch_gc_reset` / `switch_joycon2_reset`) |
| Outgoing personality's private state | Not explicitly reset (never read while inactive — usb.c:71–79) |
| **BT core (core1)** | **Untouched** — the controller stays connected |
| Input seam / global gamepad input (`report.c s_inputs`) | **Untouched** — input keeps flowing |
| `virtual_amiibo_store` data, config/settings, bonds, wake identity | **Untouched** (`ns2_init` only resets the *presented* view) |
| `g_mgmt_enabled` | **Untouched** (core0 RAM global) |
| `config_ble` arming | Follows `g_usb_config_mode` (derived from the personality): arms on →CDC_CONFIG, disarms on leaving it unless `g_mgmt_enabled` |

**Cold-boot vs transition init:** a transition runs **only** `usb_reset_personality_state(next)` — it
does **not** re-init the BT core, seam, or report layer. `ns2_init` resets Pro2 private state
(vendor tx/rx, virtual-NFC runtime, DS5 motion translation, factory, report flags). Nothing in the
transition path assumes a reboot; the BT/seam/report layers persist by design. `sleep_ms(USB_DETACH_MS)`
briefly blocks core0 (so `config_wireless_task`, UART diag, report generation pause for that window) —
a bounded, deliberate hitch, not a failure source.

**Comparison to the failure (evidence-based):**
- A **controller-personality** switch (Pro2→GC/JCL/JCR) keeps `g_usb_config_mode=false`, so it does
  **not** arm `config_ble` and cannot trigger the scan-suppression bug. It is BT-safe on the adapter
  side (matches the same-identity re-enum test: no BT disturbance).
- A switch **to/from CDC_CONFIG** flips `g_usb_config_mode`, which in the **old** firmware armed
  `config_ble.mode_active` and thereby **suppressed controller discovery** (start_scan gate) and stole
  the scan to advertise — the *same* root cause as the management failure.
- **HYPOTHESIS (code-supported, not yet confirmed):** the owner's "CDC/USB-personality transition
  failure" is the **same** `config_ble.mode_active` scan-suppression bug, now fixed by the full
  decoupling (commit `68271a0`). Confirming requires triggering a Config transition — see the diag
  command below (blocked over UART tonight; needs the new command flashed, or a BOOTSEL 2 s hold).

**Remote-transition diagnostic (removes the physical dependency; task 12):** add a dev-only
(`NS2_UART_DIAG`) UART command `persona <pro2|gc|jcl|jcr|config>` that sets
`g_usb_requested_personality` + `g_usb_personality_request_pending` — except it may also target
`CDC_CONFIG` (the app path forbids that; the dev diag path allows it, gated to dev builds). Mirrors the
proven flag pattern; lets a future session trigger *and* observe (`btstate` + the `pipe` diagnostic)
the exact CDC transition over UART without a BOOTSEL press. Prepared for review; see the implementation
batch note.

## P1 — trace-vs-code divergence analysis

### What the two traces actually say
- **Healthy (fixed firmware, this session):** `scan.starts=1, scan.stops=1, suppress.mgmt_armed=0,
  controller_connected=true, ble_conns=0, client=true, adv=false, disc=0/0`. A **Classic** controller
  (`ble_conns=0`) + a management client, stable.
- **Failure (old firmware, `dumps/mgmt-fail-20260812-230735.jsonl`):** `scan.starts=0,
  suppress.mgmt_armed=684, controller_connected=true, ble_conns=0, disc.ctrl=0, disc.hci=0`. Ring: 45×
  `scan_suppress/mgmt_armed` all at **t=22.6–24.0 s** (boot), then `mgmt_disconnect (reason 0x13)` at
  **t=254 s** → `adv_start` → `mgmt_connect` at t=257 s.

### Findings (evidence-based)
1. **CONFIRMED — no ACL disconnect occurred in the failure.** `btlife_record(BTLIFE_HCI_DISCONNECT…)`
   fires in the `HCI_EVENT_DISCONNECTION_COMPLETE` handler *before* any config gate, for **both**
   Classic and BLE ACL drops. The failure trace has `disc.hci=0` and `disc.ctrl=0` → **the controller
   never dropped at the BT level.** So "controller disconnected" (as seen on the Switch) was **not** a
   Bluetooth disconnect: either the input/report pipeline to the console stopped while the BT link
   stayed up, or the connection went **stale** (see finding 4).
2. **CONFIRMED — the 684 scan suppressions were at boot, not during the failure.** All ring
   `scan_suppress` timestamps are t=22.6–24.0 s; the controller (Classic) then paged in (~t=24 s) and
   `controller_connected` went true *without* a scan (`scan.starts=0`). After that, discovery idled
   (a connected controller stops scanning), so no further suppressions until the failure at t=254 s
   which is a **management-client** cycle, not the controller.
3. **CONFIRMED — Classic reconnect is independent of config/management.** The host is made
   connectable/discoverable at init (`gap_connectable_control(1)`/`gap_discoverable_control(1)`,
   btstack_host.c:2071–2072) and this is toggled off **only** by `pairing_lockout` and
   `btstack_host_disconnect_all_devices()` (9996–9997) — **never** by config mode, in-band management,
   or advertising. Incoming Classic `HCI_EVENT_CONNECTION_REQUEST` (3040) is auto-accepted, gated
   **only** by `pairing_lockout`. So a bonded **Classic** controller re-pages regardless of the
   config/management state — **the scan-suppression bug I fixed does not block Classic reconnect.**
   The decoupling fix is still correct and necessary (it fixed real scan/connect/wake starvation that
   blocks the **BLE**-controller and inquiry paths, and the "can't recover" scan path), but for a
   **Classic** source it is **necessary-but-possibly-not-sufficient**.
4. **HYPOTHESIS — stale Classic slot.** A Classic slot (`classic_state.connections[i]`) is freed only
   in `HID_SUBEVENT_CONNECTION_CLOSED` (9345→9372) or the ACL-disconnect path. With **no** ACL
   disconnect event (finding 1), a physically-gone controller would leave `active=true, hid_ready=true`
   → `any_controller_hid_ready()`/`controller_connected` stays **true** (stale), the single 1:1 slot
   stays occupied, and discovery stays idle. Whether a re-paging controller can then re-acquire a slot
   needs the slot-allocation path audited + a reproduction; **needs validation**.
5. **WEAKENED — "management advertising causes the Classic controller to drop" (RF interference).**
   That mechanism predicts a supervision-timeout **HCI disconnect** (`disc.hci>0`). The trace shows
   `disc.hci=0`. So either advertising is not the cause, or its effect is an *input stall without an
   ACL drop* (a different, more specific claim). Do **not** treat advertising interference as the root
   cause. Also note: in steady state with a client connected, `config_ble_start_advertising()` returns
   early (handle valid) so **advertising is off while a client is connected** (btstate confirms
   `adv=false, client=true`); advertising-while-controller-connected only happens in windows with **no**
   management client.

### The real diagnostic gap this exposes
Current diagnostics (`btstate`/`btlife`) cover the **BT** side well but give **no visibility into the
core0 → console input/report pipeline**. Finding 1 says the failure is likely there (or a stale slot),
and I currently **cannot distinguish** "BT link fine, input stopped" from "BT wedged" from one
`btstate`. See the diagnostics-improvement section below — this is the single change that would make
the next reproduction decisive.

### Concrete pipeline mechanism (CONFIRMED from code)
Input flow: BT/core1 → `set_global_gamepad_input` (report.c:54, under a critical section) → shared
`s_inputs[]` → `get_global_gamepad_input` (report.c:62) → `ns2_build_report` (switch_pro2.c:1304, per
console poll) → `tud_vendor_write` → console. **The seam publishes a neutral state on a drop only when
it is *notified*** (`ns2_seam.c:318–327`). So if a controller drops **without** a disconnect event
(the `disc=0` case), the seam is never notified, `set_global_gamepad_input` is never called again, and
core0 keeps serializing the **last, frozen** input to the console — the console shows a connected but
unresponsive controller. `report.c` stores **no last-update timestamp**, so nothing today can observe
this stall.

### Diagnostic design to make the next reproduction decisive (P1 + task 12)
Minimal, additive, read-only (safe for the ~1 kHz hot path — a counter + a `time_us_32()` store):
- **report.c:** add `s_input_update_us[idx]` set inside `set_global_gamepad_input` (already under the
  lock); getter `report_input_age_ms(idx)`.
- **switch_pro2.c `ns2_build_report`:** `s_report_count++; s_last_report_us = time_us_32();`; getters
  `ns2_pro2_report_count()` / `ns2_pro2_last_report_age_ms()`.
- **ns2_uart_diag.c:** a `pipe` command emitting `{reportCount, reportAgeMs, inputAgeMs}`.

Discrimination at the next failure:
| reportAgeMs | inputAgeMs | Meaning |
|---|---|---|
| small (reports flowing) | **large** | BT stopped feeding input; core0 alive → frozen/neutral to console. **Leading expectation** given `disc=0`. |
| **large** | any | core0 report pipeline stalled (console stopped polling, or core0 stuck/starved). |
| small | small | pipeline healthy; the "disconnect" is elsewhere (e.g. console-side). |

This is the single change that turns the next reproduction from ambiguous into decisive. Prepared for
review (built, not flashed); see the implementation batch note at the end.

### P1 status
The decoupling fix (committed, flashed, under soak) is **CONFIRMED correct** and removes a real
starvation bug (BLE/inquiry discovery + the can't-recover scan path). Whether it **fully** resolves the
owner's specific (**Classic**-source, `disc=0`) failure is **NOT established** — the evidence points at
an input-pipeline stall or a stale slot, neither visible to current instrumentation. The soak + the
`pipe` diagnostic close this.
