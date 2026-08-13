# Overnight autonomous investigation (2026-08-13)

Owner asleep; hardware left connected; `tools/mgmt_soak.ps1` owns the UART (COM11) — **no UART
commands issued during this session** so the soak is undisturbed. All work here is repository/host-side:
code tracing, host-test development, design, and documentation. Evidence standard is explicit:
**CONFIRMED** (repo/code-proven or UART-proven earlier) · **HYPOTHESIS** · **WEAKENED/REFUTED** ·
**NEEDS VALIDATION**. No speculative firmware changes; diagnostics designed/prepared for review.

Companion: [`in-band-mgmt-coexistence-failure-2026-08-12.md`](in-band-mgmt-coexistence-failure-2026-08-12.md).

---

## Implementation batch — built, NOT flashed (ready for the morning)

Additive diagnostics only (no behavior change to the shipping paths); built clean on both boards with
`build.ps1 -MgmtOn`, so the morning uf2 = the decoupling fix **plus** these:
- **`pipe`** (UART) → `{reportCount, reportAgeMs, inputAgeMs}`. `reportAgeMs` large = core0 report loop
  stalled; `reportAgeMs` small + `inputAgeMs` large = BT stopped feeding input (frozen to console).
  Makes the `disc=0` case decisive. (report.c input-freshness stamp + switch_pro2.c report counters.)
- **`persona <pro2|gc|jcl|jcr|config>`** (UART, dev-gated) → triggers a personality transition (and the
  CDC Config toggle) over UART via the existing edge-triggered flags, so the CDC/personality-transition
  hypothesis can be tested remotely without a BOOTSEL press.
- **`mgmt on|off|status`** (UART, dev-gated) → toggle management over UART without entering Config mode
  (also unblocks dev soaks that need to flip the flag).
- `mgmt_soak.ps1` detector fixed to treat intentional disconnects (reason 0x13/0x15/0x16) as normal.

## Next-hardware-interaction plan (task 13) — what to flash, test, and expect

**Priority A — confirm P1 recovery end-to-end (no reflash needed; current firmware):**
1. Read the current state (`btstate`): expect `controller_connected=false, scan_active=true,
   client=true` (healthy, waiting — captured overnight).
2. **Press a button on the DualSense to wake it.** Expected: it re-pages and reconnects
   (`controller_connected→true`, a `btlife mgmt`-side unchanged, `disc` unchanged). *Discriminates:*
   reconnect works → the P1 recovery path is fully validated on hardware; reconnect fails while
   `scan_active=true` and connectable → a **new** reconnect-admission issue to chase (would be the first
   evidence of a residual bug beyond scan-starvation).

**Priority B — flash the diagnostic build (`build/pico2_w/PicoSwitchWGA-pico2_w.uf2`, `-MgmtOn`) and:**
3. `persona config` then `persona pro2` while a controller + management client are connected; watch
   `btstate`/`btlife`. *Discriminates:* the CDC-transition hypothesis — if the controller/management
   survive with discovery resuming, the CDC-transition failure was the same (now-fixed) scan-starvation
   bug; if it wedges, capture the ring (first direct evidence of a distinct CDC-transition mechanism).
4. If the `disc=0` unresponsive-controller symptom ever recurs, run `pipe`. *Discriminates:*
   `reportAgeMs` large → core0 stalled; `inputAgeMs` large + `reportAgeMs` small → BT input stall
   (seam not notified) → then the fix is in the seam/notify path, not the BT arbitration.
5. Long soak again with `tools/mgmt_soak.ps1` (detector now reason-aware) for a multi-hour confirmation
   including a real controller-sleep/wake cycle.

**Still genuinely requires the owner (physical only):** the button press to wake the controller (A2);
flashing the diagnostic build (Pico USB is on the Switch). Everything else (state capture, transition
triggers via `persona`, `pipe` reads, soak) is now automatable over UART.

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

## P2 — controlled re-enumeration mechanism (task 8, design)

**Which changes actually require re-enumeration (from the code/PLAN):**
| Change | Host-visible? | Re-enum needed? |
|---|---|---|
| Body/Joy-Con colors (`body`/`jcl`/`jcr`/`lb`) | Yes — console reads color from factory memory (Pro2 offset `0x13019`) **at enumeration** | **Yes** (owner-observed) |
| Output personality (`personality <t>`) | Yes — different descriptors/PID | **Yes** (already wired) |
| Firmware-identity profile (`profile`) | Yes — descriptor identity tuple | **Yes** (already wired via `usb_apply_diag_reenumeration`) |
| Button mapping | **No** — retired to **Switch-side** remapping on a stable emulated identity (PLAN.md "Reliability") | **No** (nothing adapter-side to apply) |
| Amiibo / bonds / wake identity | No host-visible descriptor impact | No |

**Proven primitive:** `usb_apply_diag_reenumeration()` (same-identity `tud_disconnect`→`tud_connect`,
BT core untouched) — **UART-confirmed tonight** to leave the controller + management links byte-identical.
This is the safe building block.

**Recommended mechanism (clean, not scattered):**
1. A single **edge-triggered** `g_usb_reenumerate_requested` flag (usb.h/usb.c), consumed at the same
   safe loop point as `g_usb_personality_request_pending` in `usb_core_task` → calls
   `usb_apply_diag_reenumeration()`. Feature code **never** calls the blocking re-enum inline.
2. **Coalesce on an explicit apply, not per-change.** Color commands write RAM immediately; the console
   only re-reads at enumeration. So the trigger belongs on the **`save`** step (or a dedicated `apply`),
   which naturally collapses a burst of R/G/B edits into **one** re-enum. Recommend: `save` persists
   **and** raises `g_usb_reenumerate_requested` when the pending changes include host-visible fields.
3. **Ack / timing:** the re-enum briefly drops the console (controller blinks) but **not** the BT or
   management links (proven). Over BLE the management reply survives the USB bounce, so the client gets
   its ack; the client should warn "applying briefly reconnects the controller to the console."
4. **Active controller state during the op:** unchanged — the BT controller stays connected; only the
   console-facing USB re-enumerates. The `sleep_ms(USB_DETACH_MS)` core0 stall is bounded and identical
   to the BOOTSEL cycle.
5. **Open owner decision (UX):** auto-re-enum on Save (recommended — single action) vs. a separate
   "Apply to console" control. Both use the same flag; only the trigger UI differs. Flagged for the
   owner; **not implemented** pending that call.

**Tests to prepare (host-side, no hardware):** the flag is edge-consumed like the existing
`g_usb_personality_request_pending` (already covered by `tools/test_usb_mode_cycle.c`); a small unit
around "save with host-visible change raises the flag; save without does not" can be added once the
which-fields-are-dirty predicate exists.

## Shared management architecture — Android + portal (task 11, review)

The management command **surface** is correctly shared and generic — the firmware has **no**
portal-specific coupling. `config.c handle_line` dispatches newline-JSON commands; the wireless
allowlist (`config_wireless_command_allowed`) gates the same set for any BLE client; the cross-core
bridge chunks responses MTU-safe. "browser/portal" appears only in descriptive comments. Both the Web
Portal and the Android companion app consume this identical surface (as
`docs/bluetooth/app-interface-audit.md` §1 already frames it).

**Two contracts the Android app MUST honor (client-side, shared — document so they're not rediscovered):**
1. **One-command-at-a-time serialization.** The bridge is a single-slot request/response channel: a
   client must wait for each JSON-line reply before sending the next (config.c:1242, and
   `config_wireless_bridge`'s `CONFIG_WIRELESS_RX_BUSY`). The portal already does this; the Android app
   must **not pipeline** commands or it will get `BUSY`/dropped replies. This is a shared-protocol
   contract, not portal-specific behavior.
2. **Amiibo metadata decryption is client-side by design.** Owner/nickname/dates/write-count are
   decrypted in the client (portal JS + `SubtleCrypto` + the user's retail keys), because keys are
   deliberately **never** on the adapter (passthrough architecture; see
   `docs/switch2/amiibo-crypto-research-2026-08.md`). The firmware correctly exposes **raw bytes**
   (`amiibo read`) + **plaintext** `figureId`. The Android app must reimplement the same crypto —
   recommend a **shared client-crypto spec** (JS + Kotlin from one definition), **not** moving crypto
   into firmware. The Sync *orchestration* (read-loop + validate + dirty/`acknowledge`) likewise uses
   shared firmware **primitives** with client-specific sequencing — correct boundary.

**One shared gap (not portal-specific):** real physical-amiibo backup via a connected controller
(G4, `ns2_nfc_mirror` initiator) is wired **only** to the UART diag, so **neither** client can do it.
It should be lifted to the config/BLE surface (allowlisted, bonded) so both clients gain it — already
tracked in the interface audit; re-affirmed here as shared work, not a portal feature.

**Conclusion:** architecture is sound (shared surface, correct client/firmware boundary). Action items
are documentation (the two contracts above) + the G4 lift — no portal-specific logic needs to move.

## In-band management persistence (task 10, investigation + recommendation)

**Where it lives / why it resets:** `g_mgmt_enabled` is a `volatile bool` in `usb.c` (RAM), default
false (or true under the `NS2_MGMT_DEFAULT_ON` diagnostic build flag). It is not in the persistent
config, so an ordinary reboot restores the RAM default.

**Existing persistence mechanism:** `pico_config_t` (config.c:53, `CONFIG_VERSION=10`) in flash
(`CONFIG_FLASH_OFFSET`) holds `body_color`, Joy-Con accents, and the wake identity; saved by
`config_service_save` (core1, the only flash writer), loaded by `config_load`. Adding a `bool` field
(bump to v11 + migration) is the straightforward way to persist a setting.

**Interaction with BOOTSEL/personality:** `g_mgmt_enabled` is personality-independent (survives
switches — good). BOOTSEL does not currently toggle it. Every **UF2 flash** erases all five
persistence sectors via the install marker (config.c:63–72), so a reflash always resets a persisted
setting to default.

**Recommendation — do NOT persist `g_mgmt_enabled` yet. Keep it RAM-only, default-off.** Two decisive
reasons grounded in the current architecture:
1. **It is currently the guaranteed clean-boot escape hatch.** Because it is RAM-only, a **power cycle
   always returns to a safe, management-off state** — which is exactly how the owner recovered from the
   P1 wedge. Persisting *enabled* would remove that escape hatch: any management-path bug that wedges on
   boot would **re-wedge every boot** → effectively bricked until a reflash. That is a real
   permanent-inaccessibility risk the RAM default avoids.
2. **Management is currently unauthenticated (plan C4 not implemented).** Persisting *enabled* means the
   adapter boots advertising an **open** management service on every power-up with no user present to
   deliberately enable it — a standing security exposure.

**Path forward (recorded, not implemented):**
- **Development:** the `NS2_MGMT_DEFAULT_ON` build flag already solves "reboot disrupts debugging"
  without touching flash or the escape hatch (a normal `build.ps1` reverts to off). Recommend **also**
  adding a dev-gated UART `mgmt on|off|status` diag command (see task 12) so a session can toggle
  management over UART without entering Config mode — this also removes a real testing dependency.
- **Production (only AFTER C4 authenticated bonding lands):** persist via a new `pico_config_t` field
  (v11 + migration), loaded into `g_mgmt_enabled` at `config_load`, **plus** an escape hatch before it
  ships: the triple-tap wipe must also clear persisted management-enabled, and/or a boot-health
  watchdog that reverts to off if the prior boot did not reach a healthy steady state (prevents the
  re-wedge brick). The install-marker reflash-erase remains the last-resort recovery.

## ★ Overnight soak result (headline, 2026-08-12 23:49 → 08-13 05:22)

`tools/mgmt_soak.ps1` ran ~5.4 h against the decoupling-fix firmware with a Classic controller
(DualSense, `ble_conns=0`) + a management client connected. Full btlife ring (6 events, **0 dropped** —
the rate-limit fix held):
```
scan_start → adv_start → scan_stop(ctrl paged in) → mgmt_connect
  → [5.4h stable, 10 re-enumerations, every one ctrl+client survived, disc=0/0, suppress.mgmt_armed=0]
  → 05:11:49  scan_start (scan.starts 1→2)  → hci_disconnect reason 0x13 handle 0x000B
```

**CONFIRMED (hardware):**
- **5.4 h of stable coexistence + 10 clean re-enumerations** — the controller and management client both
  survived every one (`disc=0/0`). The old firmware failed "after a short period"; this is a dramatic
  improvement.
- On the controller disconnect, **discovery resumed** (`scan.starts 1→2`) with **`suppress.mgmt_armed=0`**
  — *this is the exact behavior the bug broke*. The old firmware would have suppressed this scan. The
  decoupling fix demonstrably restores post-drop discovery.
- **Management stayed connected** across the controller drop (`client=true`, `mgmt.disconnects=0`) — it did
  **not** go undiscoverable.
- **Final state is healthy, not wedged:** `scan_active=true, inquiry_active=true, controller_connected=false,
  client=true` — the adapter is correctly scanning for the controller's return. This is the **opposite** of
  the original failure (undiscoverable, power-cycle required).

**INTERPRETATION (well-supported):** `reason=0x13` is `REMOTE_USER_TERMINATED_CONNECTION` — the controller
*chose* to disconnect. After ~5.4 h idle (owner asleep, no input) the DualSense idle-slept (it is documented
to power down; PLAN.md "Wake from sleep"). The firmware correctly does **not** chase a `0x13`/power-off
reconnect (btstack_host.c:3775 `reason_warrants_reconnect`) and instead resumes discovery — precisely what
we observe. This is **normal controller sleep, handled correctly**, not the failure.

**NOT proven (needs the morning):**
- **Reconnect-after-wake:** whether the DualSense re-pages and reconnects when woken (a physical button
  press). The adapter is scanning + connectable, so it should — but this is unproven and is the single
  most valuable morning check.
- The **original `disc=0` wedge was NOT reproduced**, so the fix's sufficiency for *that specific*
  mechanism remains inferred from the healthy post-drop scanning, not directly demonstrated. The `pipe`
  diagnostic (below) is what would make a recurrence decisive.

**Diagnostic bug found + to fix:** `mgmt_soak.ps1`'s failure detector flagged this intentional sleep as a
"SPONTANEOUS FAILURE" because the controller was gone >30 s — it must treat `hci_disconnect` reason `0x13`
(remote-user-terminated) and `0x15`/`0x16` (power-off / host-terminated) as **expected**, not a failure.
Fixed below.

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
