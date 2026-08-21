# Android fresh-pair bond transport, and a mixed-workload coexistence soak

**Date:** 2026-08-21
**Branch:** `ns2-testing`
**Areas:** Android companion pairing, BLE management admission, Classic/BLE coexistence

Two questions were resolved in one session: why fresh management pairing failed with a bogus PIN
error, and whether the resulting build survives a long mixed Bluetooth/audio/management workload.

---

## Part 1 — Why fresh pairing reported "incorrect PIN or passkey"

### Question

Android discovered the adapter, reached the system pairing prompt, then failed with
*"Couldn't pair because of incorrect PIN or passkey."* No PIN is ever typed for this relationship,
so the string could not be taken literally. What actually refused the bond?

### Background

PicoSwitch2 is a genuine dual-mode device on a single public BD_ADDR (`88:A2:9E:D1:77:78`):

- **Classic (BR/EDR)** — HID *host* for physical controllers. Always connectable; discoverable only
  inside the BOOTSEL pairing window. Unbonded incoming ACLs outside that window are refused by
  `btstack_host_classic_connection_filter()`.
- **BLE** — management *peripheral* (`config_ble`). Its RX/TX characteristics and the TX CCC are
  `ATT_SECURITY_ENCRYPTED`, and `mgmt_session_authorized()` requires a bonded, 16-byte-encrypted LE
  link.

The companion app connects management with `BluetoothDevice.TRANSPORT_LE`, but started the bond with
`device.createBond()`.

### Hypothesis

`createBond()` is `createBond(TRANSPORT_AUTO)`. TRANSPORT_AUTO prefers BR/EDR whenever Android's
cached device record is `DEVICE_TYPE_DUAL`. If the phone had ever observed the adapter's Classic
identity, the app would be running BR/EDR SSP against the *controller* admission gate instead of LE
SMP against the management service.

### Method

Live capture on the maintainer's phone and adapter. `adb logcat` for the Android Bluetooth stack,
`tools/uart_query.ps1` for adapter counters before/after each attempt, with the adapter's pairing
window deliberately **closed** so the control case was unambiguous.

### Environment

| | |
|---|---|
| Adapter | Pico 2 W, `pro2` personality, `NS2_MGMT_DEFAULT_ON=ON`, `NS2_UART_DIAG=ON`, 300 MHz |
| Adapter BD_ADDR | `88:A2:9E:D1:77:78` |
| Phone | Android 13, Qualcomm/`bt_btif` stack, BD_ADDR `68:24:99:26:82:26` |
| App | `dev.picoswitch.companion.debug` 2.0.0-debug |

### Results

**Before the change** — one "Pair Adapter" tap:

```
btif_get_device_type: Device [88:a2:9e:d1:77:78] type 3      <- DEVICE_TYPE_DUAL
bt_btm_sec: transport=classic, btm_status=8                  <- BTM_DEVICE_TIMEOUT
btif_dm_auth_cmpl_evt() - Pairing timeout; retrying (2) ...
bta_dm_authentication_complete_cback deleting 88:a2:9e:d1:77:78 - result: 0x0e
bondStateChangeCallback: Status: 9 ... newState: 0 hciReason: 14
```

Adapter counters across the same attempt: `admission.reject_window` +1, and
`mgmt.connects` / `adv.starts` **unchanged** — the phone never reached the LE management peripheral
at all. Elapsed to failure: 8.2 s.

**After forcing `createBond(TRANSPORT_LE)`** — same tap, window still closed:

```
relationship/bond.mechanism: attempt=1 mechanism=le-create-bond type=dual
bt_btm_sec: transport=le, btm_status=10
bondStateChangeCallback: Status: 9 ... hciReason: 85          <- 0x55, SMP pairing failed
```

Adapter counters: `mgmt.connects` 6→7, `mgmt.disconnects` 6→7, `adv.starts` 7→8,
`admission.reject_window` **unchanged**. Elapsed to failure: 0.7 s.

With the pairing window **open**, the same build completed a fresh bond and the app reached
Connected. Android's bonded-device list afterwards:

```
88:A2:9E:D1:77:78 [ DUAL ] PicoSwitch2
```

### Interpretation

The dual-mode device record — not the bond — is what steers TRANSPORT_AUTO, and Android keeps that
record across *Forget*, so once a phone has seen the Classic identity every subsequent
`createBond()` goes BR/EDR. The Classic gate then refuses an unbonded ACL outside the pairing
window, which is correct behaviour, and Android renders that refusal with its generic
authentication-failure string.

A BR/EDR bond was never merely the slower option here: it is the wrong relationship. A Classic link
key cannot satisfy `mgmt_session_authorized()`, so even a *successful* SSP would have produced a
management link the firmware must reject.

The counter delta is the cleanest single discriminator: a Classic attempt moves
`admission.reject_window`; an LE attempt moves `mgmt.connects` / `adv.starts`.

### Conclusion

**Confirmed.** Fresh management pairing must bond on the LE transport explicitly. TRANSPORT_AUTO is
unsafe for this product on any phone that has ever observed the adapter's Classic identity.

### API compatibility

`BluetoothDevice.createBond(int transport)` is absent from `android.jar` through **API 36** and
public from **API 37** (verified with `javap` against the installed platforms). On every currently
shipping Android it therefore needs one reflective compatibility seam
(`CompanionViewModel.androidBondPlatform`), where `getMethod` doubles as runtime feature detection.
Where that seam is unavailable, `AdapterBondStarter` falls back to `LeGattInitiated` — opening the
`TRANSPORT_LE` management GATT link and letting the encryption-required characteristics provoke SMP
through public API only. TRANSPORT_AUTO is never used, not even as a fallback.

| Path | Evidence |
|---|---|
| `le-create-bond` via the reflective seam | **Confirmed** on hardware (above) |
| `createBond(int)` public at API 37 | **Confirmed** from the platform `android.jar` |
| `le-gatt-initiated` fallback | **Strong Evidence** — AOSP answers `GATT_INSUFFICIENT_AUTHENTICATION` by encrypting the LE link, which starts SMP when no LTK exists; the firmware's own side is source-verified (`host_att_write_callback` returns `ATT_ERROR_INSUFFICIENT_AUTHENTICATION` for an untrusted link). Not exercised on hardware: doing so needs a device where the hidden method is blocked, or a deliberate unpair plus a physical re-pair. |

### Related: the pairing-admission latch

`config_ble_accept_new_bond()` used to re-read the live `hid_pairing_window_open` at SM confirmation
time. That moment sits *after* Android's own pairing dialog, which is human-paced, so authorization
the user had already given could expire mid-procedure. Controller BLE candidates never had this
problem because they latch at connection complete (`conn->fresh_pairing_admitted`).

Management now matches that rule: `config_ble.fresh_bond_admitted` is latched when the connection is
accepted and cleared on disconnect and on the HCI-loss transient reset. Admission is unchanged in
who it admits — `mgmt off` still revokes a latched attempt, and an attempt that was not admitted at
connect time can never become admitted. Pinned by `mgmt_accept_latched_bonding()` in
`tools/test_mgmt_access.c` and by `tools/test_bluetooth_closeout_wiring.py`.

Live confirmation on the accepted build: `btlife` reports `cble.fresh_bond: true` for the
management connection that was admitted inside the window.

---

## Part 2 — Mixed-workload coexistence soak

### Question

The failure mode this reliability pass targeted eventually left *both* Classic controller
connectivity and BLE management unreconnectable. Does the accepted build survive a long run with
every subsystem live at once?

### Method

Unplanned but complete: the maintainer left the full setup running and walked away. Verified
afterwards from the adapter's lifecycle ring and counters, and from the app's own diagnostic log.

### Environment

Adapter on console in `pro2` personality; DualSense Edge connected over Classic; controller audio
playing continuously (music playlist); Android management connected over BLE GATT. No manual
interaction, no recovery.

### Results

Adapter, from `btlife` and its 48-entry lifecycle ring:

| Observation | Value |
|---|---|
| `mgmt.connects` / `mgmt.disconnects` | `1` / `0` |
| `disc.ctrl` (controller disconnects) | `0` |
| `disc.hci` | `8`, all during the first 18 s of boot |
| Classic connections held | `classic_raw 2`, `classic_ready 2` |
| `pairing.lockout` | `false` |
| **Last Bluetooth lifecycle event of any kind** | `t = 218 s` (controller ready) |
| `bthealth` probes / recoveries / reboot requests | `0` / `0` / `0` |
| `control_tick_max_gap_ms` | `852` |

App, from its mirrored diagnostic log:

| Observation | Value |
|---|---|
| GATT generation | `1` throughout — never reconnected |
| Session age at last sample | `sinceReadyMs = 4 732 829` (**78.9 min**) |
| Negotiated ATT MTU | `517` |
| Command round-trip | 49–93 ms, one notification per reply, no retries |
| Errors / timeouts / disconnects / stale callbacks | **0** |

Adapter uptime was ≈79 min, so roughly **75 minutes elapsed with zero Bluetooth lifecycle events**
while a Classic controller, continuous controller audio, and a BLE management session were all live.

### Interpretation

The previously observed wedge did not recur, and the HCI liveness module never had to act — its
probe and recovery counters are still zero, so this run is evidence about the *primary* paths, not
about the recovery path. Management held one GATT generation for the whole soak, which rules out
silent reconnect churn masquerading as stability.

### Conclusion

**Confirmed** for sustained coexistence of physical Classic controller + continuous controller audio
+ BLE management, ≥75 minutes, no drops and no manual recovery.

### Remaining unknowns

- The bounded HCI/CYW43 OFF/ON recovery has still never fired on hardware. Its logic is host-tested
  (`tools/test_ns2_bt_health.c`); the recovery *path* remains unvalidated in the field.
- The `le-gatt-initiated` bond fallback is unexercised on hardware (see the table above).
- Longer-than-80-minute behaviour, and behaviour across console sleep/wake during a soak, are
  untested.

---

## Part 3 — Two counter questions raised by the same captures

### The adapter kept an LE bond the phone had forgotten

`btbonds` showed one LE bond for the phone while Android had none — asymmetric trust residue from
the earlier failed-pairing work, not a lifecycle defect.

BTstack resolves this on re-pair. Before adding a bond, `sm.c` searches the LE device DB by public
identity address:

```c
if ((le_db_index < 0) && (setup->sm_peer_addr_type == BD_ADDR_TYPE_LE_PUBLIC)) {
    ...  log_info("sm: device found for public address, updating");
         le_db_index = i;                 /* reuse the existing slot */
}
```

Only when no match is found does it `le_device_db_add()` into the first empty slot. **Confirmed
empirically:** after Forget-on-Android plus a fresh re-pair, the adapter still lists exactly one LE
bond for the phone (DB slot 15, 1 of 16 used) — updated in place, not duplicated.

The `bonds list` / `bonds remove <index>` contract uses the real LE device DB slot on both sides
(`entry->index = slot`; removal calls `le_device_db_info(idx, …)` on the same number), so the
Settings remove button targets the entry it displays. The UART `btbonds` `i` field is a compacted
snapshot position and is a different, read-only view — it is not what `bonds remove` consumes.

**Classification: harmless residue; normal re-pairing handles it safely.** No automatic cleanup is
warranted. Manual cleanup already exists (Settings → Remove pairing, and the BOOTSEL wipe gesture).
A stale entry could only accumulate for a peer whose *identity* changed, and 16 slots with an
explicit remove is sufficient.

### `admission.reject_window` incremented while idle

Measured on hardware rather than assumed:

| Adapter state | `reject_window` rate |
|---|---|
| No controller connected; BLE scan **and** Classic inquiry running | ≈1 per minute |
| Controller connected; `scan_active: false`, `inquiry_active: false` | **0 in 3 minutes** |

The increments track *discovery being active*, not a persistent intruder. All seven
`btstack_host_record_fresh_admission(false)` sites are refusals of an unbonded peer trying to form
trust outside the explicit pairing window — incoming Classic ACL filter, Classic PIN request,
Classic SSP confirmation, Classic SSP passkey, unadmitted Classic link-key notification, BLE
controller SM Just Works, and the Switch 2 custom final admission. Every one refuses.

**Classification: security working as designed.** The counter is a refusal odometer that only ticks
while the adapter is actively hunting for a controller in an environment containing other Bluetooth
devices. Do not weaken admission to silence it.

The real limitation is attribution: the counter is aggregate and records no transport, peer, or
which of the seven sites fired. That is a diagnostics gap, not a defect, and is not a release
blocker. If attribution is ever needed, add a bounded "last rejected admission {site, transport,
address prefix}" snapshot alongside the counter.

---

## Part 4 — Controller-Link input with DualSense audio

### Observation

With **Controller Link** selected as the active console input source, audio kept flowing to the
physically connected DualSense Edge.

### Determination: emergent, and architecturally safe

Two ownership domains exist and neither reads the other:

| Domain | Owner | Keyed on |
|---|---|---|
| Console input | `ns2_input_arbiter` / `ns2_active_input` | the arbiter's active source id |
| Audio sink | `ds5_audio_bridge` | `bridge_conn_index`, claimed in `ds5_connect()` and released in `ds5_disconnect()` |

`ds5_audio_bridge.c` contains no reference to the arbiter, and the gate the DS5 audio task uses is
`ds5_audio_bridge_owns_connection(conn_index)`, which is `bridge_connected && bridge_conn_index ==
conn_index` — connection identity alone.

So the behaviour is **emergent**, not explicitly designed for this case and not accidental: it falls
out of the project's existing separation between source-controller behaviour and console-facing
output behaviour. There is no shared mutable state between the domains and no conflicting-owner
state, because the DS5's outbound audio travels on its own Classic link regardless of who owns
console input.

It is also the behaviour the product wants: the Android Controller Link cannot transport controller
audio, so tying the audio sink to input ownership would silence a working headset for no reason.

### Made explicit

`tools/test_bluetooth_closeout_wiring.py` now pins the decoupling: `ds5_audio_bridge.c` must not
reference `ns2_active_input` / `ns2_input_arbiter`, both audio builds must gate on connection
identity alone, and the DS5 audio task must not gate on input ownership. Mutation-checked — injecting
an arbiter reference into either file fails the guard.

**Status: intentional as of this record.** Active console input may be Controller Link while a
connected audio-capable physical controller remains the audio sink.
