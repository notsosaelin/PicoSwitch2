# In-band management ↔ controller coexistence failure

Status: 🟡 FIX APPLIED (HW verification pending) — controller-half root cause confirmed live over
UART and fixed; a secondary stale-Classic-link wedge is noted for post-fix watch.
Date: 2026-08-12
Related: [`../bluetooth/in-band-management-plan.md`](../bluetooth/in-band-management-plan.md),
[`../architecture/config-transports.md`](../architecture/config-transports.md)

## Autonomous overnight session (2026-08-12 ~23:30→, owner asleep, hardware left connected)

Constraint: **cannot reflash** overnight (Pico USB-C is on the Switch; flashing needs BOOTSEL on the
PC). So the work is: stress the *currently flashed* decoupling-fix firmware over UART to prove whether
P1 is resolved, capture a clean trace if it breaks, and do hardware-independent fixes.

**Finding 1 — the decoupling fix is live and correct at the radio level (UART-proven).** Immediately
after boot, `btstate` shows `scan_active=true` AND `cble.advertising=true` together with
`suppress.mgmt_armed=0` and `scan.starts` climbing — the CYW43 runs concurrent LE scan + advertise,
and controller discovery is no longer starved. A Classic controller + a management client then both
connect and hold (`ctrl=true, client=true, disc=0/0`).

**Finding 2 — same-identity USB re-enumeration does NOT disturb the BT link (UART-proven, hypothesis
disproven).** Driving `profile default` (`usb_apply_diag_reenumeration`: `tud_disconnect` →
`tud_connect`, BT core untouched) left the BT state byte-identical before/after over 12 s —
`controller_connected=true`, `client=true`, `disc=0/0`, `events.count` unchanged. So the "USB
re-enumeration breaks the BT/management link" hypothesis is **false on the fixed firmware**; core
independence holds. **Caveat/blocked:** this only exercises a *same-identity* re-enum. The more likely
"CDC/personality-transition" culprit is a *different-identity* switch (Pro2→GC/→CDC), which **cannot
be triggered over UART** on the current firmware — it needs a physical BOOTSEL tap or a new diag
command (a flash). Recorded as a physical/flash dependency; every non-blocked avenue continued.

**Finding 3 — P3 (portal stale metadata after Sync) root-caused and fixed** (hardware-independent):
`amiiboInfoCache` was not invalidated on Sync. Fixed in `web/index.html`; needs browser validation.

**In progress — overnight soak.** `tools/mgmt_soak.ps1` runs the passive soak (spontaneous time/RF
failure) plus periodic proven-safe re-enum checks, auto-dumping the `btlife` ring on any regression
and stopping the stress on first reproduction. Log: `dumps/mgmt-soak-*.jsonl` (+ `.summary.txt`).
Results to be read in the morning; if it stays stable, that is strong evidence the decoupling fix
resolved P1 (short-window stability already observed for >11 min continuous, plus a clean re-enum).

## Question

With the in-band BLE management transport enabled (`g_mgmt_enabled`, default off), the full
management workflow works — but after a short period of operation **both** the management link and
the controller link drop and cannot recover without a power cycle. Why, and which subsystem fails
first?

## Environment

- Firmware: `ns2-testing`, in-band management transport (commits `97eb68b`, `df73ff9`, `8c88406`).
- Adapter driving a real Switch 2 as a Pro Controller 2; a controller bridged over Bluetooth.
- Web Portal (`web/index.html`) as the management client over BLE.
- `g_mgmt_enabled` turned on via the Config path, then returned to Pro2 for gameplay.

## What worked (owner hardware report, 2026-08-12)

The end-to-end management + Amiibo workflow is functional while the connection is healthy:

- Connect to the adapter through the Web Portal over BLE in normal Pro2 mode.
- Write a new controller color. **Observation:** the color did not take effect until the device
  re-enumerated — colors are read into the USB identity at enumeration, so a live change needs a
  re-enumeration to surface (see Remaining questions).
- Upload an Amiibo through the portal; use it on the Switch 2.
- Modify the Amiibo on the Switch 2 (nickname), save on-console.
- Trigger **Sync Amiibo** from the portal; the console's written data synced back and the portal
  showed the updated owner / nickname / registration date / last-write date / write count. **UI
  observation:** the portal needed a manual page refresh to display the synced metadata.
- No noticeable input latency, dropped inputs, or missed presses while navigating the Switch 2 UI
  (informal — not a dedicated latency test). Gyro and audio were **not** reached (the link did not
  stay up long enough to enter a game); they remain **untested**, not confirmed working or broken.

## The failure

After a short period, in order observed from the outside:

1. The Web Portal lost its connection to the adapter.
2. The adapter became undiscoverable to the portal (no management advertising).
3. The controller disconnected.
4. The controller did **not** automatically reconnect.
5. The adapter stayed unusable until a **power cycle**, which fully restored it.

Because `g_mgmt_enabled` is RAM-only (reverts to off on reboot), the post-power-cycle adapter was
back in the safe default, so the exact time-to-failure could not be re-measured in that pass. The
owner noted the failure mode resembles a previously seen CDC↔USB personality-switch state where the
adapter also stopped advertising and the controller refused to reconnect.

## Live confirmation (UART `btstate`, `-MgmtOn` build, 2026-08-12)

Read directly off the adapter over COM11, with management enabled from boot and **no controller
ever connected**:

```json
{"mgmt_enabled":true,"config_mode":false,"personality":"pro2","powered_on":true,"hid_state":0,
 "scan_active":false,"controller_connected":false,"ble_conns":0,
 "cble":{"available":true,"armed":true,"advertising":true,"client":false},
 "scan":{"starts":0,"stops":0},"adv":{"starts":1,"stops":0},
 "suppress":{"config_mode":0,"mgmt_armed":38,"wake":0,"other":0}}
```

`scan.starts = 0` with `suppress.mgmt_armed = 38` is the smoking gun: controller discovery has
**never started** because every call was refused for `mgmt_armed`. The management service armed at
boot (`adv.starts=1`), latching `config_ble.mode_active`, before any controller could connect. This
proves the code analysis below on hardware — no gameplay required.

## Root cause — controller half: CONFIRMED from code

`btstack_host_start_scan()` early-returns whenever the config/management BLE service is armed:

```c
// src/bt_hid/bt/btstack/btstack_host.c  (btstack_host_start_scan)
if (g_usb_config_mode || config_ble.mode_active) {
    return;  // Config/management owns LE advertising; suppresses discovery
}
```

`config_ble.mode_active` latches **true** for the entire time `g_mgmt_enabled` is on. In the
original config-mode design this suppression was correct (Config drops the console, so no controller
discovery is wanted). **In-band management keeps the service armed during gameplay, so every attempt
to (re)start controller scanning is refused for the whole session.** The moment the controller link
drops for any reason (RF blip, supervision timeout), the reconnect path
(`HCI_EVENT_DISCONNECTION_COMPLETE` → `btstack_host_start_scan()` at the tail of the handler) is
silently blocked. Only a power cycle clears it — because the flag resets to off on boot, which is
exactly the observed recovery. This is the same class as the CDC/personality-switch resemblance the
owner flagged: both latch `mode_active`/config and starve discovery.

This directly explains symptoms 3–5.

## Root cause — management half: NOT YET ISOLATED

Why management advertising also stops and stays stopped (symptoms 1–2) is not yet proven from code.
The config_ble service was designed assuming **exclusive** radio ownership (config = no controller
traffic). Running it concurrently with an active controller central link (dual role: peripheral +
central on one CYW43) is new, and the exact wedge — a failed `gap_advertisements_enable`, a stuck
`config_ble.closing`, ACL/role exhaustion, or a shared teardown with the controller ACL — needs a
hardware trace to distinguish. Hence the instrumentation below.

## Diagnostics added (this pass)

UART-only, always-on, purely additive (recording never alters BT behavior). Available on the GP0/GP1
diag link in every personality, so the last-good/first-fail state is visible **without** the Web
Portal.

- **`btstate`** (alias `btlife`): live snapshot — `mgmt_enabled`, `config_mode`, `personality`,
  `powered_on`, `hid_state`, `scan_active`, `inquiry_active`, `wake_adv`, `controller_connected`,
  `ble_conns`, the `cble` service block (`armed`/`advertising`/`client`/`closing`/`notify`), and
  cumulative counters: `scan.starts/stops`, `adv.starts/stops`, `suppress.{config_mode,mgmt_armed,
  wake,other}`, `mgmt.connects/disconnects`, `disc.{ctrl,hci,last_handle,last_reason}`.
- **`btlife read <N>`**: one lifecycle event (0 = oldest) from a 48-entry ring, each with a ms
  timestamp and a decoded code (`scan_start/stop/suppress`, `adv_start/stop`,
  `mgmt_connect/disconnect`, `ctrl_disconnect`, `hci_disconnect`). Ordering answers *which link
  failed first*; `hci_disconnect`/`ctrl_disconnect` carry the HCI reason.
- **`btlife clear`**: reset the ring + counters for a clean run.

**The fingerprint to look for:** after the failure, `suppress.mgmt_armed` climbing while
`scan_active=false` and `controller_connected=false` confirms the controller-starvation cause above.
The `adv_*` counts and the `cble` block around the first `mgmt_disconnect` should reveal whether
advertising failed to resume (management half).

### Reproduction aid

`g_mgmt_enabled` is RAM-only, so it reverts to off on reboot (a deliberate safe default). To
reproduce immediately after a power cycle, build a **diagnostic** firmware that boots with it on:

```
./build.ps1 -MgmtOn            # -DNS2_MGMT_DEFAULT_ON=ON
```

Never ship `-MgmtOn`: management is unauthenticated until plan C4.

## Suggested test procedure (next pass)

1. Flash the `-MgmtOn` diagnostic build; attach the GP0/GP1 UART.
2. `btlife clear`, then run the workflow until the failure.
3. On failure, capture `btstate` and walk `btlife read 0..N`.
4. Read off: which `*_disconnect` came first; the HCI reason; whether `adv_start` fired again after
   the `mgmt_disconnect`; and whether `suppress.mgmt_armed` is climbing.

## Live failure trace (2026-08-12)

Captured over UART **during the actual failure** (pre-fix `-MgmtOn` build), saved to
`dumps/mgmt-fail-20260812-230735.jsonl`. Key state at failure:

```
scan.starts = 0, suppress.mgmt_armed = 684, events.dropped = 641
controller_connected = true, ble_conns = 0, disc.ctrl = 0, disc.hci = 0
cble: armed=true, advertising=false, client=true
ring: 45× scan_suppress/mgmt_armed (t=22.6-24.0s) then
      mgmt_disconnect reason 0x13 (t=254s) -> adv_start (+23ms) -> mgmt_connect (t=257s)
```

Reading: ~700 blocked scan attempts in the first 24 s (controller never discoverable via scan);
the controller is **Classic** (`ble_conns=0` yet `controller_connected=true`) and connected by paging
us, then physically dropped **without an HCI disconnection event reaching us** (`disc.*=0`), leaving
the stack wedged believing it is still connected — so it neither rediscovers nor recovers. The
management advertiser itself behaved (it resumed after a client drop and reaccepted a client): the
management-"undiscoverable" symptom is downstream of the wedged controller state, not a dead
advertiser. Two instrumentation lessons: (1) the suppress flood evicted the disconnect ordering
(641 dropped) — now rate-limited in the ring; (2) the counters survived the flood and carried the
proof.

## Correct architecture (2026-08-12, owner-directed)

The controller BT link lives on **core1**. Config mode, in-band management, and personality
re-enumeration are **core0 / USB-face** concerns. The Switch re-enumeration the owner designed is
core0-only and **never requires killing the BT link** — the two cores are deliberately independent.
Config mode originally dropped/suppressed the controller only because it was a *standalone* mode with
no controller in play; that coupling is obsolete and must not be inherited by management. **The
controller link is therefore fully decoupled: nothing in config/management may gate, drop, or block
controller discovery, connection, or reconnection.** Verified: no code actually disconnects a
connected controller on config entry (only double-tap "replace source" and triple-tap wipe do); the
sole defect was discovery/reconnection being *gated* on config/management state.

Controller discovery is a **central-role** LE scan / Classic inquiry. The config/management service
owns only the **peripheral-role** LE advertiser — a different radio function — so they coexist. Wake
replay (also the advertiser) is the only thing the service yields to.

## Fix applied (2026-08-12) — full decoupling of the controller link

All in `btstack_host.c`. The config gate is removed entirely (not merely swapped from management to
config), so config mode and in-band management behave identically toward the controller:

- **`btstack_host_start_scan()`** no longer checks `g_usb_config_mode` **or** `config_ble.mode_active`.
  Controller discovery is driven purely by controller state (and the legitimate wake / lockout /
  scan-suppress / powered gates). Config/management never blocks it.
- **`btstack_host_connect_ble()`** no longer defers controller connects for config/management at all —
  a found/bonded controller connects/reconnects regardless of the USB face.
- **`config_ble_service_task()`** owns only the LE advertiser and **never stops controller discovery**.
  It advertises whenever authorized and idle, coexisting with an active scan; it yields the advertiser
  only to a wake burst (`wake_adv.active`). Config and management use the same path.
- **`btstack_host_start_wake_advertisement()`** yields under management (and the service task drops the
  advert while `wake_adv.active`), so enabling management does not break wake-from-sleep.

Empirical unknown to validate on HW: the CYW43 running LE scan + LE advertise (+ Classic inquiry,
dual-role connections) concurrently. **HW verification pending:** confirm `scan.starts` increments
under management, a dropped controller reconnects, config_ble still advertises alongside a scan, and
the stale-Classic-link wedge does not recur (watch `disc.*` and `controller_connected` vs `ble_conns`).

## Remaining questions

- Management-half wedge mechanism (symptoms 1–2) — the trace should localize it.
- Exact time-to-failure and trigger (idle vs. active vs. a specific event).
- Live color change: is a targeted re-enumeration after a color write the right UX (the owner
  suggested manually triggering it), or should the Pro2 identity re-read colors without a full
  re-enumeration? (Separate from this failure.)
- Portal auto-refresh after Amiibo sync (UI-only).

## Future work

- Confirm the root cause with the UART trace, then implement the coexistence fix above.
- Fold the confirmed behavior into the plan's C5 (wake/advertiser hand-off) and the HW coexistence
  gates before revisiting gyro/audio validation.
