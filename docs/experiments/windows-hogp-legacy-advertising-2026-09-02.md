# Windows HOGP advertising: does the PicoSwitch2 adapter ever see it?

**Date:** 2026-09-02
**Status:** Question answered. Discovery is not the blocker. A new, narrower
adapter-side blocker is isolated and remains open.
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

Two candidate mechanisms, both untested:

- **C1 — single-slot `pending_ble_gamepad` thrashing.** The deferral state is one
  slot for the whole radio. `adv` runs at ~30/s in this RF environment; the PC's
  `ADV_IND` and its `SCAN_RSP` are separate events, and any other HID-looking
  peer's `ADV` landing between them overwrites the slot, so the merge
  (`memcmp(addr, pending.addr, 6) == 0`) fails and `has_hid_uuid` is lost.
- **C2 — an interaction between address resolution and the deferral/trust
  bookkeeping** when the same peer identity already holds a peripheral-role
  management link to this adapter.

C1 and C2 are **hypotheses**, not conclusions.

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

**Confidence:** High for every "Confirmed" line — each is a direct HCI capture
or a differential counter measurement with a control. C1/C2 are unranked
hypotheses.

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

- Which term of `is_controller && … && ble_candidate_admitted` is false on the
  sightings that increment `bonded_adv`.
- Whether `pending_ble_gamepad`'s single slot survives a busy RF environment.
- Whether an already-bonded peer that currently holds a peripheral-role
  management link to this adapter is reachable by a central-role `gap_connect`.

## Suggested follow-up

One experiment settles all three, and it is the smallest possible:

> Build the existing firmware with `pico_enable_stdio_uart(PicoSwitchWGA 1)` —
> or add nothing at all and simply route the *existing*
> `[BTSTACK_HOST] Generic BLE HID detected` / `gap_connect returned status`
> prints to UART0 — flash it to the bench adapter, and re-run the capture above
> while streaming UART.
>
> No discovery, admission or filter logic changes. The prints already exist and
> already name the answer. Expect either "Generic BLE HID detected" absent
> (⇒ C1, the merge is being lost) or present with no `gap_connect` line
> (⇒ admission), or `gap_connect returned status=<n>` with a non-zero status
> (⇒ C2, the link-layer refuses the peer).

That requires flashing a diagnostic firmware build, which the current pass is
not authorised to do.
