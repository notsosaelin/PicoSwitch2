# In-band management ↔ controller coexistence failure

Status: 🟡 IN PROGRESS — root cause identified for the controller half; diagnostics added to
confirm on hardware and to isolate the management-advertising half.
Date: 2026-08-12
Related: [`../bluetooth/in-band-management-plan.md`](../bluetooth/in-band-management-plan.md),
[`../architecture/config-transports.md`](../architecture/config-transports.md)

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

## Proposed fix (design, not yet implemented — pending trace)

The coexistence requirement the plan under-specified: **management must not suppress controller
discovery.** Candidate direction, to be validated against the trace and on hardware:

- Do **not** gate `btstack_host_start_scan()` on `config_ble.mode_active` when the arming reason is
  in-band management (only when it is the exclusive CDC Config personality). Controller scanning and
  low-duty management advertising then coexist (the CYW43 supports simultaneous scan + advertise;
  connect attempts already `stop_scan` briefly and resume).
- Ensure management advertising reliably resumes after a client disconnect even while a controller
  central link is up (verify `config_ble.closing` clears and `config_ble_start_advertising()` is
  reachable each tick).
- Confirm dual-role stability (peripheral advert + central controller connection) on the CYW43; if
  it is the wedge, fall back to the plan's audio-window-style suppression (advertise only when the
  radio is otherwise idle) rather than permanent suppression.

This is deliberately **not** applied yet: the owner asked to instrument and isolate first, and the
management-half wedge must be understood before changing radio arbitration ("nothing breaks").

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
