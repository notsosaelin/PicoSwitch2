# Windows HOGP: why the adapter never links to the PC

**Date:** 2026-09-02
**Status:** Root cause identified and confirmed. Two distinct adapter-side
defects isolated. No firmware change was authorised in this pass, so both remain
open.
**Supersedes:** the working-note theory that Windows advertises the hosted GATT
service through extended-only PDUs.

---

## Question

The Windows companion's AppContainer HOGP host reaches `Started` and stays
advertising, but the adapter never links and
`android_bridge_identify_trace` reports `calls = 0`.

1. Does Windows emit an advertisement the adapter's **unchanged** discovery path
   can receive — i.e. one using **legacy** PDUs?
2. If so, does the adapter discover and admit it?
3. If so, why does no LE connection form?

---

## Background

`src/btstack_config.h` defines `ENABLE_LE_CENTRAL`, `ENABLE_LE_PERIPHERAL` and
`ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION`. It defines neither
`ENABLE_LE_EXTENDED_ADVERTISING` nor **`ENABLE_LE_RESOLVING_LIST`**. Both
omissions matter, and they matter at different layers:

- no extended advertising ⇒ the adapter's `GAP_EVENT_ADVERTISING_REPORT` handler
  (`btstack_host.c:4623`) can only ever receive **legacy** advertising reports;
- privacy resolution **without** a resolving list ⇒ BTstack resolves incoming
  RPAs **in software, on the host**, and hands the application the peer's
  *identity* address — but the **controller has no resolving list**, so an
  initiator dialling that identity address cannot match an RPA on the air.

The second point is the root cause. It was invisible until discovery was ruled
out.

---

## Method

One variable at a time, on the shipping package. **No firmware change, and no
change to the adapter's discovery, admission, scanning or filter logic.**

1. Build and register the real development MSIX; remove stale registrations
   first so attribution is unambiguous.
2. ETW over the Bluetooth providers actually registered on the machine
   (`logman query providers`). The decisive one is
   `Microsoft-Windows-BTH-BTHPORT` `{8A1F9517-3A8C-4A9E-A018-4F17A200F277}`,
   whose event **402** carries raw HCI on channel
   `Microsoft-Windows-BTH-HCI/HCIRAW` (`BIP_Type`, `BIP_Data`).
3. Drive the installed companion through UI Automation: Connect → Gamepad →
   Start Controller Link.
4. Decode both directions: advertising configuration (0x2035–0x2039) and LE
   connection events (`LE Meta` 0x3E subevents 0x01/0x0A/0x12).
5. Read the adapter's own counters over UART: `btstate`, `btreconnect`,
   `btbonds`, `btreject`, `btrefuse`, `btlife dump 0`, `bridge status`.
6. Repeat with the compatibility publisher removed, changing nothing else.
7. Power-cycle the adapter and repeat, sampling `btstate` every 1.2 s so the
   first 40 s are resolved.

### Environment

| | |
|---|---|
| PC | Windows 11 Pro 26200 |
| Radio | Intel AX210, `USB\VID_8087&PID_0032`, driver 24.40.10.8, internal, Windows-owned |
| Second radio | none used |
| Package | `PicoSwitch2.Companion_0.1.0.0_x64__ccebdjf58vkew`, one MSIX, hidden `windowsApp`/`appContainer` app service |
| Adapter | firmware build `ba11a0b1`, bridge contract 4, personality `pro2` |
| Adapter UART | COM11 |
| PC identity address | `14:18:C3:47:C4:89` (public) |
| Adapter address | `88:A2:9E:D1:77:78` |

---

## Results

### R1 — Windows already emits a legacy connectable advertisement

`StartAdvertising(IsConnectable=true, IsDiscoverable=true)` programs **two**
advertising sets:

| Handle | `Advertising_Event_Properties` | Meaning | ADV payload | Scan response | Address |
|---|---|---|---|---|---|
| 1 | `0x0013` | connectable + scannable + **LEGACY PDUs** (`ADV_IND`) | `Flags=1A`, Complete 16-bit UUIDs `0x180A, 0x1812` | Complete Local Name | **random (RPA)** |
| 2 | `0x0001` | connectable, extended PDUs | same nine bytes | — | random (RPA) |

Set 1 raw parameters:
`01 1300 A00000 A00000 07 01 00 000000000000 00 7F 01 00 01 01 00`
— 100 ms interval, all three primary channels, primary PHY LE 1M,
`Own_Address_Type = 0x01` (**random**), followed by
`LE Set Advertising Set Random Address handle=1`.

Windows publishes a legacy set for legacy centrals **and** an extended set for
modern ones. Set 1 is exactly the `ADV_IND` carrying HID `0x1812` the adapter
looks for. **The extended-PDU theory is falsified.**

### R2 — the compatibility publisher is inert, and could never have worked

With the experiment in place a **third** set appeared: props `0x0010` — legacy
but **not connectable and not scannable** (`ADV_NONCONN_IND`) — carrying only
manufacturer data `FFFF 'PSCL'` and no HID UUID.

This is a code-level certainty. `BluetoothLEAdvertisementPublisher` has **no**
`IsConnectable` member: in the Windows SDK (10.0.26100) `IsConnectable` exists
on `IBluetoothLEAdvertisementReceivedEventArgs2` — a *received* property — and
on `IGattServiceProviderAdvertisingParameters`, but not on the publisher. It can
only ever emit a non-connectable advertisement. Removing it changed nothing
measurable. **Removed.**

### R3 — the adapter receives and resolves the advertisement

`bonded_adv` counts sightings of a HID-looking peer whose address raw-matches a
stored bond; `rpa_adv` counts HID-looking peers at unresolvable private
addresses.

| Condition | duration | `adv` delta | `bonded_adv` delta | `rpa_adv` |
|---|---|---|---|---|
| Controller Link **stopped** | 20 s | +327 | **+0** (736 → 736) | 0 |
| running, publisher present | 10 s | +222 | **+18** | 0 |
| running, publisher removed | 30 s | +617 | **+52** | 0 |

`btbonds` holds exactly one entry: `14:18:C3:47:C4:89` **type 0 (public)** — the
PC, stored by the management pairing.

`btstack_host_addr_is_bonded()` (`btstack_host.c:2324`) is a raw `memcmp`
against the LE device DB. A raw compare of an on-air RPA against a stored
identity address **cannot** match. It matched anyway, and `rpa_adv` stayed 0.

**This proves the address delivered to the handler is the resolved *identity*
address, not the RPA that was on the air.** Software privacy resolution is doing
its job.

### R4 — after a power cycle, the adapter discovers, admits and dials

Sampling `btstate` every 1.2 s across the first Controller Link start after a
power cycle:

```
 T-0    win=false defer=false  hid=1  scan=1/0   led=idle
  2.5s  win=true  defer=false  hid=1  scan=1/0   led=pairing   ← window opens
  3.8s  win=true  defer=false  hid=2  scan=1/1   led=pairing   ← CONNECTING, scan stopped
 …
 14.3s  win=true  defer=false  hid=2  scan=2/2   led=pairing   ← attempt 2
 24.7s  win=true  defer=false  hid=1  scan=3/2   led=pairing   ← timed out
 27.3s  win=true  defer=false  hid=2  scan=3/3   led=pairing   ← attempt 3
 32.5s  win=false defer=TRUE   hid=2  scan=3/3   led=pairing   ← window expires mid-connect
 37.8s  win=false defer=true   hid=1  scan=4/3   led=pairing   ← timed out; flag stays set
```

`hid_state` 2 is `BLE_STATE_CONNECTING`, and `scan.stops` moves in lockstep
because `btstack_host_connect_ble_candidate()` stops the scan as its first
action. **The adapter admitted the Windows peripheral and called `gap_connect`
three times.** Discovery and admission — the questions this pass opened with —
both **pass**.

`ble_raw`/`ble_ready` stay `0/0` and `disc` stays `0/0`: no attempt ever
produced a link.

### R5 — the connection request never reaches Windows

BTHPORT HCIRAW for the whole run contains exactly one LE connection event:

```
16:04:22.756  LE Enhanced Connection Complete status=0x00 handle=0x0008
              role=central peer=88:A2:9E:D1:77:78
```

That is the **management** link — Windows as central, dialling the adapter.
There is **no** peripheral-role `LE Connection Complete`, **no**
`LE Enhanced Connection Complete` with `role=peripheral`, and **no**
`LE Advertising Set Terminated` (which is what fires when a connectable
advertising set is connected).

**The adapter's connection requests never arrive at the Windows radio.**

### R6 — the Windows side behaves correctly throughout

```
16:04:24.854 INFO [connect]         connected 88:A2:9E:D1:77:78 as PicoSwitch2:
                                    fw=2.0 build=ba11a0b1 contract=4 personality=pro2
16:04:50.724 INFO [controller-link] activating same-package Bluetooth host
16:04:51.131 INFO [controller-link] host state=Ready → Starting → Advertising → WaitingForConnection
16:04:51.132 INFO [controller-link] advertising settled at Started; report scheduler active
16:04:53.234 INFO [pairing]         start op=1 state=discovering reason=none
```

Helper: activated in `windowsApp/AppContainer`, caller authenticated as same
package, pipe handshake complete, descriptor **161 bytes /
`f27315bfdf48b7ab5f76336f065fa27d9e04a45fdd17f96e4e752473a6725054`** /
contract 4, `AdvertisementStatus 3 (Aborted) → 2 (Started)` in ~145 ms.

Report path over a 3.7-minute run:
`generated=27949 queued=27949 sent=27949 coalesced=0 intervalAvgMs=8.00
intervalMaxMs=29.38`.

---

## Interpretation — the confirmed chain

1. Windows advertises the hosted GATT service from a **resolvable private
   address** (R1). No WinRT API controls this:
   `GattServiceProviderAdvertisingParameters` exposes only `IsConnectable`,
   `IsDiscoverable`, `ServiceData` and the two secondary-PHY properties.
2. The adapter has the PC **bonded**, from the management pairing (R3) —
   which Controller Link *requires*.
3. `ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION` resolves the advertiser's RPA in
   software and delivers the **identity** address `14:18:C3:47:C4:89`,
   type public (R3).
4. Because the delivered address is an identity address,
   `btstack_host_addr_is_rpa()` is false, so `ble_rpa_trust_candidate` is false
   and the code's RPA branch is bypassed.
5. `btstack_host_connect_ble_candidate()` therefore calls `gap_connect()` on the
   **identity** address. **`ENABLE_LE_RESOLVING_LIST` is not defined**, so the
   controller holds no resolving list and the initiator cannot match the RPA
   that is actually on the air.
6. The connection request is emitted for an address nobody is advertising from.
   Windows sees nothing (R5); every attempt times out (R4).

**The irony is load-bearing: Controller Link requires trusted management, and
the management bond is precisely what makes the HOGP advertisement undialable.**
An *unbonded* peripheral would not be resolved, `is_rpa` would be true, and the
adapter would dial the on-air RPA — the path a genuine controller takes.

**Confidence: Confirmed.** Every step is a direct HCI capture, a differential
counter measurement with a control, or a repeated direct state read.

### The second defect: `pairing_close_deferred` latches

`btstack_host_close_pairing_window()` sets `pairing_close_deferred = true` when
`hid_state.state == BLE_STATE_CONNECTING`, and only
`resolve_deferred_pairing_close()` (on `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`),
`btstack_host_clear_transient_radio_state()` (HCI leaving the working state) or
the destructive `btstack_host_delete_all_bonds()` clear it.

R4 shows the 30 s pairing window expiring at T+32.5 s while a connect is in
flight, setting the flag — and no connection-complete ever arrives to clear it.
`open_pairing_window()` (`ns2_bt_host.c:141`) early-returns while the flag is
set, so from that moment:

- the owner LED stays in the pairing blink for the rest of the boot;
- every later pairing attempt — the **BOOTSEL gesture included** — is a silent
  no-op reported to clients as `BLOCKED / BUSY`;
- only a power cycle recovers.

This is user-visible and was independently reported from the bench before it was
explained. It is **not** Windows-specific: any BLE connect that never resolves
while a pairing window expires will latch it.

---

## Conclusion

| Question | Answer |
|---|---|
| Does Windows advertise in a format the unchanged adapter can receive? | **Yes** — legacy `ADV_IND`, HID `0x1812`, scan-response name. Confirmed. |
| Does the adapter discover and admit it? | **Yes** — window opens, candidate admitted, `gap_connect` called three times. Confirmed. |
| Why does no link form? | The adapter dials the **resolved identity address**; Windows is on air as an **RPA**; the adapter has no controller resolving list. Confirmed. |
| Is there a Windows-side fix? | **No.** The advertising address type is not exposed by any WinRT API, and the publisher that could set data sections cannot advertise connectably. |
| Was any firmware filter changed? | **No.** Nothing in discovery, admission, scanning or the predicate was touched. |

---

## Negative knowledge preserved

**Do not re-add a co-resident `BluetoothLEAdvertisementPublisher` to force
legacy advertising.** The theory is attractive because the firmware genuinely
cannot see extended advertising, so "make Windows speak legacy" looks like the
whole problem. Windows already does, on set handle 1, and the publisher can only
add a non-connectable set with no HID UUID.

**Do not conclude "extended advertising" from seeing HCI opcode 0x2036.**
Windows programs *all* LE advertising through `LE Set Extended Advertising
Parameters` on a capable radio. Legacy is `Advertising_Event_Properties`
**bit 4**. `0x0013` is a legacy `ADV_IND`; `0x0001` is a genuine extended set.

**Do not read `bonded_adv` climbing as "the adapter is happy".** It increments
before the connect gate, and it matched here only because host-side privacy
resolution had already rewritten the address — which is the very thing that
broke the dial.

**Do not treat the stuck pairing LED as a UI bug.** It is the honest indicator
of a latched `pairing_close_deferred`, and the adapter really is unable to pair
anything until power-cycled.

---

## Remaining unknowns

- Whether the CYW43439 in this adapter supports an LE resolving list of useful
  depth, and whether the Pico SDK's BTstack build can enable
  `ENABLE_LE_RESOLVING_LIST` without disturbing the validated management
  peripheral path.
- Whether any genuine BLE controller in the supported set advertises from an RPA
  after bonding. If one does, this same dial failure already affects it, and the
  fix is not Windows-specific at all.

## Suggested follow-up — firmware, and not yet authorised

Two independent changes, neither in discovery, admission or the filter:

**F1 — dial an address the peer is actually using.** Either enable
`ENABLE_LE_RESOLVING_LIST` and populate the controller resolving list from the
LE device DB so the initiator can match RPAs against a resolved identity, or
carry the observed on-air address alongside the resolved identity in
`pending_ble_gamepad` and pass *that* to `gap_connect()`. Both are
transport-general and would equally fix reconnecting to any bonded BLE
controller that rotates its address; neither special-cases Windows.

**F2 — make `pairing_close_deferred` un-latchable.** Give the deferred close a
bounded timeout, or clear it on the connect watchdog's own expiry rather than
only on `LE_CONNECTION_COMPLETE`. Needed regardless of F1: today, one connect
that never resolves disables all pairing — gesture included — until the adapter
is power-cycled.

Both need a firmware build for Pico W and Pico 2 W, a flash, and regression
coverage for the existing Android and physical-controller paths. F2 should get a
host test in `tools/test_ns2_bt_lifecycle.c` pinning that an unresolved connect
cannot leave the window permanently shut.
