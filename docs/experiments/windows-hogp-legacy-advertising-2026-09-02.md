# Windows HOGP advertising: does the PicoSwitch2 adapter ever see it?

**Date:** 2026-09-02
**Status:** Question answered. Discovery is not the blocker — Windows advertises
correctly and the adapter receives it. The blocker is the adapter's controller
pairing window never opening, isolated to a latched
`pairing_close_deferred` flag. One free, non-destructive experiment confirms or
refutes it.
**Supersedes the root-cause theory in:** the Controller Link work-in-progress
notes that attributed the failure to Windows using extended advertising PDUs.

---

## Question

The Windows companion's AppContainer HOGP host reaches `Started` and stays
advertising, but the PicoSwitch2 adapter never connects and
`android_bridge_identify_trace` reports `calls = 0`.

Two questions, in order:

1. Does Windows emit an advertisement the adapter's **unchanged** discovery
   path can physically receive — i.e. one using **legacy** advertising PDUs?
2. If it does, where in the adapter's unchanged candidate→connect path is the
   Windows peripheral dropped?

---

## Background

`src/btstack_config.h` defines `ENABLE_LE_CENTRAL`, `ENABLE_LE_PERIPHERAL` and
`ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION`. It does **not** define
`ENABLE_LE_EXTENDED_ADVERTISING`. The controller-discovery handler in
`src/bt_hid/bt/btstack/btstack_host.c:4623` is `GAP_EVENT_ADVERTISING_REPORT`,
BTstack's **legacy** advertising-report event. There is no
`GAP_EVENT_EXTENDED_ADVERTISING_REPORT` handler.

So the adapter can only ever receive legacy `ADV_IND` / `ADV_SCAN_IND` /
`ADV_NONCONN_IND` / `SCAN_RSP`. An extended-PDU (`ADV_EXT_IND`) advertisement is
invisible to it, and a legacy initiator cannot connect to one either.

This made a compact and attractive hypothesis available.

### Hypothesis H1 (the attractive one)

> Windows programs `GattServiceProvider` through an **extended, non-legacy**
> advertising set. The adapter therefore never receives the advertisement, and
> the fix is to keep a legacy-format advertisement alive beside it.

An implementation of that fix — a co-resident
`BluetoothLEAdvertisementPublisher` with `UseExtendedAdvertisement(false)` —
existed in the working tree, compiled, and had never been measured.

---

## Method

One variable at a time, on the shipping package, with **no firmware change and
no change to the adapter's discovery or admission predicate.**

1. Build and register the real development MSIX; remove stale registrations
   first so attribution is unambiguous.
2. Start an ETW session over the Bluetooth providers actually registered on the
   machine (`logman query providers`), the decisive one being
   `Microsoft-Windows-BTH-BTHPORT` `{8A1F9517-3A8C-4A9E-A018-4F17A200F277}`,
   whose event **402** carries raw HCI on channel
   `Microsoft-Windows-BTH-HCI/HCIRAW` (`BIP_Type`, `BIP_Data`).
3. Drive the installed companion through UI Automation: Connect → Gamepad →
   Start Controller Link.
4. Decode every `LE Set Extended Advertising Parameters` (0x2036),
   `... Data` (0x2037), `... Scan Response Data` (0x2038),
   `... Enable` (0x2039) and `LE Set Advertising Set Random Address` (0x2035),
   and read `Advertising_Event_Properties` **bit 4 = legacy PDUs**.
5. Read the adapter's own counters over UART with `btreconnect`, `btstate`,
   `btbonds`, `btreject`, `btrefuse`, `btlife dump 0` and `bridge status`.
6. Repeat the whole run with the compatibility publisher **removed**, changing
   nothing else.

### Environment

| | |
|---|---|
| PC | Windows 11 Pro 26200 |
| Radio | Intel AX210, `USB\VID_8087&PID_0032`, driver 24.40.10.8, internal, Windows-owned |
| Second radio | none used (a CSR dongle is present but not started) |
| Package | `PicoSwitch2.Companion_0.1.0.0_x64__ccebdjf58vkew`, one MSIX, hidden `windowsApp`/`appContainer` app service |
| Adapter | PicoSwitch2 firmware build `ba11a0b1`, bridge contract 4, personality `pro2` |
| Adapter UART | COM11 |
| PC identity address | `14:18:C3:47:C4:89` (public) |
| Adapter local address | `88:A2:9E:D1:77:78` |

---

## Results

### R1 — Windows already emits a legacy connectable advertisement. **H1 is FALSIFIED.**

`StartAdvertising(IsConnectable=true, IsDiscoverable=true)` programs **two**
advertising sets, not one:

| Handle | `Advertising_Event_Properties` | Meaning | ADV payload | Scan response | Address |
|---|---|---|---|---|---|
| 1 | `0x0013` | connectable + scannable + **LEGACY PDUs** (`ADV_IND`) | `Flags=1A`, Complete 16-bit UUIDs `0x180A, 0x1812` | Complete Local Name `YGGSDRASIL` | random, RPA |
| 2 | `0x0001` | connectable, extended PDUs (`ADV_EXT_IND`) | same nine bytes | — | random, RPA |

Set 1 raw parameters: `01 1300 A00000 A00000 07 01 00 000000000000 00 7F 01 00 01 01 00`
— 100 ms interval, all three primary channels, primary PHY LE 1M,
`Own_Address_Type = 0x01` (random).

Windows publishes a legacy set for legacy centrals **and** an extended set for
modern ones. The legacy set is precisely the `ADV_IND` carrying HID `0x1812`
that the adapter's unchanged predicate looks for.

### R2 — the compatibility publisher is inert, and could never have worked

With the experiment in place, a **third** set appears:

| Handle | Properties | Meaning | Payload |
|---|---|---|---|
| 3 | `0x0010` | **LEGACY PDUs, not connectable, not scannable** (`ADV_NONCONN_IND`) | Manufacturer Specific `FFFF 5053434C` (`PSCL`) |

This is a code-level certainty, not an accident of configuration.
`BluetoothLEAdvertisementPublisher` has **no** `IsConnectable` member: in the
Windows SDK (10.0.26100) `IsConnectable` exists on
`IBluetoothLEAdvertisementReceivedEventArgs2` — a *received* property — and on
`IGattServiceProviderAdvertisingParameters`, but not on the publisher. A
publisher can therefore only ever emit a non-connectable advertisement, and it
carries no HID UUID, so the adapter's predicate rejects it and nothing can dial
it. It only consumes radio airtime the connectable set needs.

### R3 — the adapter **is** receiving and resolving the Windows advertisement

`btreconnect` exposes the advertising-report counters. `bonded_adv` counts
sightings of a HID-looking peer whose address raw-matches a stored bond;
`rpa_adv` counts HID-looking peers at unresolvable private addresses.

| Condition | duration | `adv` delta | `bonded_adv` delta | `rpa_adv` |
|---|---|---|---|---|
| Controller Link **stopped** | 20 s | +327 | **+0** (736 → 736) | 0 |
| Controller Link **running**, with publisher | 10 s | +222 | **+18** (585 → 603) | 0 |
| Controller Link **running**, publisher removed | 30 s | +617 | **+52** (1547 → 1599) | 0 |

`bonded_adv` moves **only** while the Windows host advertises, at a steady
~1.7–1.8 sightings/s, and freezes the moment Controller Link stops. `btbonds`
shows exactly one bond, `14:18:C3:47:C4:89` type 0 (public) — the PC, stored by
the management pairing.

`btstack_host_addr_is_bonded()` (`btstack_host.c:2324`) is a raw `memcmp`
against the LE device DB and cannot itself resolve an RPA. The counter
nevertheless matches, and `rpa_adv` stays 0, because
`ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION` makes BTstack resolve the advertiser's
RPA against the stored IRK and deliver the **identity** address to the handler.

**Confirmed:** the Windows HOGP advertisement reaches the adapter, is address-
resolved to the bonded PC identity, and is classified as HID-bearing.
Discovery — the assumed blocker — is not the blocker.

### R4 — the adapter never attempts a connection

Across every run:

- `btstate`: `hid_state: 1` (`BLE_STATE_SCANNING`) throughout; never 2
  (`BLE_STATE_CONNECTING`).
- `btstate`: `scan.stops` never increments.
  `btstack_host_connect_ble_candidate()` calls `btstack_host_stop_scan()` as its
  first action, so a single connect attempt would move it.
- `btlife dump 0` across a full 40 s run: only Classic `inquiry_start` /
  `inquiry_stop` cycling. No BLE scan stop, no connect, no admission event.
- `btreject` → `none`; `btrefuse` → `none`.
- `btstate.admission`: `fresh_accepted 0`, `reject_window 0`, `reject_lockout 0`.
- `bridge status`: `calls 0`, `matched 0`, all rejection counters 0.

### R5 — removing the publisher changes nothing

Identical `bonded_adv` rate, identical `rpa_adv = 0`, identical
`hid_state = 1`, identical `calls = 0`. The experiment neither helped nor hurt
discovery. It is removed.

### R6 — the adapter's controller pairing window never opens

Sampling `btstate` every 2.5 s across a full 42 s Controller Link start:

```
  2.6s  window_open=false close_deferred=true  hid=1  scan=0/0
  ...   (every sample identical)
 41.7s  window_open=false close_deferred=true  hid=1  scan=0/0
```

`hid_pairing_window_open` is **false for the entire run**, while the companion's
diagnostic ring shows the management verb being accepted:

```
15:47:09.913 INFO [controller-link] activating same-package Bluetooth host
15:47:10.246 INFO [controller-link] advertising settled at Started; report scheduler active
15:47:12.372 INFO [pairing]         start op=5 state=discovering reason=none
15:47:12.372 INFO [controller-link] remote pairing: op=5 state=discovering reason=none
```

`pairing_close_deferred` is **true in every sample taken today**, across app
restarts and two management sessions, while `hid_state` is 1
(`BLE_STATE_SCANNING`) and never 2 (`BLE_STATE_CONNECTING`).

### R7 — the main→helper report path already meets the cadence target

From the same run's counters, over ~3.7 minutes of continuous streaming with no
peer connected:

```
generated=27949 queued=27949 sent=27949 coalesced=0
intervalAvgMs=8.00 intervalMaxMs=29.38
output=0/0 malformed=0 outputFailures=0
```

125 Hz sustained, nothing dropped, nothing coalesced. This measures
`ControllerInputSession → ControllerReportEncoder → binary pipe → AppContainer
host`; it does **not** measure GATT notification cadence, because no central
ever subscribed.

---

## Interpretation

Static reading of the unchanged predicate says the Windows peripheral **should**
be dialled, which is why the remaining gap is worth recording precisely rather
than guessing at.

Walking `btstack_host.c:4739`–`4818` with the measured advertisement:

| Gate | Value | Passes? |
|---|---|---|
| `has_hid_uuid` (`0x1812` in the complete 16-bit UUID list) | true | yes |
| `bt_device_lookup("YGGSDRASIL", 0)` | no substring in `name_table` matches | `BT_PROFILE_DEFAULT` |
| `is_known_controller` | false | generic path taken |
| `BT_PROFILE_DEFAULT.classic_only` | false (`bt_device_db.c:12`) | not excluded |
| `BT_PROFILE_DEFAULT.ble` | `BT_BLE_GATT_HIDS` | ≠ `BT_BLE_NONE` |
| name deferral (`!name[0] && adv_event_type != 0x04`) | ADV carries no name; `SCAN_RSP` carries `YGGSDRASIL` and active scanning is on (`gap_set_scan_params(1, …)`) | should merge and clear `pending_ble_gamepad` |
| `hid_state.state == BLE_STATE_SCANNING` | 1 | yes |
| `ns2_bt_admission_decide(false, window, trust_present)` | `trust_present` ⇒ `RECONNECT`, never `REJECT` (`ns2_bt_lifecycle.c:45`) | admitted |

Every gate reads as passing, and `bonded_adv` incrementing is itself evidence
that execution reaches the counter block at `btstack_host.c:4797` — which sits
**after** the deferral `break` and only a few lines before the connect. Yet no
connect is ever issued.

That contradiction cannot be resolved from the host side. It needs one
observation the adapter cannot currently give: which of `is_controller` or
`ble_candidate_admitted` is false on the sightings that increment the counter.
The firmware's `printf` diagnostics would say directly — they print
`Generic BLE HID detected: …` and `gap_connect returned status=…` — but
`CMakeLists.txt:398` builds with `pico_enable_stdio_uart(PicoSwitchWGA 0)`, so
they go nowhere on a normal build.

Two terms decide `ble_candidate_admitted`, and R6 shows both are false:

```c
ble_trust_present       = is_controller && btstack_host_find_le_device(addr, addr_type) >= 0;
ble_rpa_trust_candidate = is_controller && !ble_trust_present &&
                          btstack_host_addr_is_rpa(addr, addr_type) && le_device_db_count() > 0;
fresh_pairing_authorized = hid_pairing_window_open || (…SWITCH2 only…);
ble_admission            = ns2_bt_admission_decide(pairing_lockout,
                                                   fresh_pairing_authorized,
                                                   ble_trust_present);
ble_candidate_admitted   = ble_admission != REJECT ||
                           (!pairing_lockout && ble_rpa_trust_candidate);
```

- `btstack_host_addr_is_rpa(addr, addr_type)` is **false**, because after
  `ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION` does its work `addr` is the *identity*
  address `14:18:C3:47:C4:89`, not the RPA that was on the air. So
  `ble_rpa_trust_candidate` cannot rescue the decision. This is also why
  `rpa_adv` stays 0 while `bonded_adv` climbs: `btstack_host_addr_is_bonded()`
  compares the address **ignoring the type** (`btstack_host.c:2324`) and matches,
  while a type-aware `btstack_host_find_le_device()` need not.
- `fresh_pairing_authorized` is **false**, measured directly (R6).

With both false and no trust match, `ns2_bt_admission_decide` returns `REJECT`
(`ns2_bt_lifecycle.c:45`) and the candidate is dropped **one line before**
`btstack_host_connect_ble_candidate()` — after the counter block, which is
exactly why `bonded_adv` moves while nothing else does.

### Why the window never opens

`open_pairing_window()` (`ns2_bt_host.c:141`) begins:

```c
if (btstack_host_pairing_close_deferred()) {
    return;
}
```

`pairing_close_deferred` is set in exactly one place —
`btstack_host_close_pairing_window()` when `hid_state.state ==
BLE_STATE_CONNECTING` — and cleared in exactly three:
`resolve_deferred_pairing_close()` on `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`,
`btstack_host_clear_transient_radio_state()` when HCI leaves the working state,
and the destructive `btstack_host_delete_all_bonds()`.

If a pairing window ever closed while a BLE connect was in flight and that
connect never produced a resolving `LE_CONNECTION_COMPLETE`, the flag latches
**true for the lifetime of the boot** — and `open_pairing_window()` becomes a
permanent no-op for both the BOOTSEL gesture and the remote-pairing verb. That
matches every observation: the flag is true in every sample, the window never
opens, and no fresh candidate of any kind can be admitted.

**Classification: Strong Evidence, not Confirmed.** One observation does not fit
cleanly: with `pairing_until_ms` left at 0, the control tick's `else` branch
should report `MGMT_PAIRING_BLOCKED / REASON_BUSY`, and the companion would then
say "The adapter is busy with another pairing request" within ~1 s. Instead the
companion showed `discovering` for the full ~30 s. Either the tick's ordering
differs from the static reading, or a second mechanism is involved. Settling
that needs adapter-side observation.

This is **not** a discovery-filter problem, and no adapter discovery, admission
or scanning logic was changed, broadened or bypassed in this pass.

---

## Conclusion

- Windows emits a legacy, connectable, scannable `ADV_IND` carrying HID `0x1812`
  with a scan-response local name, from one internal Windows-owned Intel AX210,
  concurrently with an active management central connection. **Confirmed.**
- The PicoSwitch2 adapter receives it, resolves its private address to the
  bonded PC identity, and counts it as a HID-bearing sighting. **Confirmed.**
- The adapter never issues `gap_connect` for it. **Confirmed.**
- The extended-PDU root-cause theory is **disproven**.
- A co-resident `BluetoothLEAdvertisementPublisher` cannot help, because the
  WinRT publisher cannot advertise connectably. **Confirmed by API surface and
  by measurement.** Removed from the tree.
- The adapter's controller pairing window never opens during a Controller Link
  start, so no fresh candidate can be admitted. **Confirmed.**
- That is attributable to `pairing_close_deferred` being latched true, which
  makes `open_pairing_window()` a no-op for both the remote verb and the
  BOOTSEL gesture. **Strong Evidence.**
- The Windows main→helper report path sustains 125 Hz with nothing dropped.
  **Confirmed.** GATT notification cadence to a real peer remains **unmeasured.**

**Confidence:** High for every "Confirmed" line — each is a direct HCI capture,
a differential counter measurement with a control, or a repeated direct state
read. The `pairing_close_deferred` attribution is Strong Evidence with one
unexplained observation, recorded above rather than rounded up.

---

## Negative knowledge preserved

**Do not re-add a co-resident `BluetoothLEAdvertisementPublisher` to force
legacy advertising for the hosted GATT server.** The theory is attractive
because the firmware genuinely cannot see extended advertising, so "make Windows
speak legacy" looks like the whole problem. It is not: Windows already does, on
advertising set handle 1, and the publisher can only add a non-connectable set
with no HID UUID.

**Do not conclude "extended advertising" from seeing HCI opcode 0x2036.**
Windows programs *all* LE advertising through `LE Set Extended Advertising
Parameters` on a radio that supports it. Whether the set is legacy is
`Advertising_Event_Properties` **bit 4**, not the choice of opcode. `0x0013` is
a legacy `ADV_IND`; `0x0001` is a genuine extended set.

---

## Remaining unknowns

- Whether `pairing_close_deferred` latching true is the whole story, or only
  part of it (see the classification note above).
- How this adapter's flag came to be set — which past connect attempt closed a
  window in flight and never resolved.
- Whether `btstack_host_find_le_device()` matches a resolved *identity* address
  whose type BTstack reports as `…_IDENTITY` rather than the stored type 0. If
  it does not, a bonded peer at a resolved address has no trust path at all once
  the pairing window is shut, which would make the window the only route in.

## Suggested follow-up

**Experiment 1 — free, non-destructive, decides the root cause.**

> Power-cycle the adapter (unplug and replug; the flag is a file-scope `bool`
> and initialises to false at boot). Confirm `btstate` reports
> `close_deferred: false`, then repeat this capture unchanged.
>
> - Window opens, adapter connects, `bridge status` `calls > 0` ⇒ root cause
>   **confirmed**, and the durable fix is a bounded timeout or an
>   HCI-failure path that cannot leave `pairing_close_deferred` latched.
> - Window opens and the adapter still does not connect ⇒ the admission theory
>   is wrong; the remaining suspect is `btstack_host_find_le_device()` versus
>   the resolved identity address type.
> - Window still does not open ⇒ the flag is not the mechanism.

**Experiment 2 — only if Experiment 1 is inconclusive.**

> Build the existing firmware with `pico_enable_stdio_uart(PicoSwitchWGA 1)`
> (`CMakeLists.txt:398`) so the *existing* prints reach UART0 — they already
> say the answer: `Generic BLE HID detected: …` and
> `gap_connect returned status=…`. Flash the bench adapter and re-run while
> streaming UART. No discovery, admission or filter logic changes.

Experiment 2 requires flashing a diagnostic firmware build, which the current
pass is not authorised to do.

---

## What this pass did NOT change

- No firmware change of any kind. No adapter discovery predicate, admission
  policy, scanning mode, HID check, appearance check or filter was broadened,
  bypassed, weakened or special-cased. The frozen contract in
  `btstack_host.c:4739`–`4818` is byte-for-byte as it was.
- No second Bluetooth radio, no WinUSB/Zadig, no driver change, no rebinding of
  the Intel radio. Windows retained normal ownership of Bluetooth throughout,
  including an active management central connection on the same AX210 while the
  AppContainer host held the peripheral role.
- No bonds were deleted.
