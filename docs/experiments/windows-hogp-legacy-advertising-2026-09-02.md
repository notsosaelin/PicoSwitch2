# Windows HOGP: why the adapter never links to the PC

**Date:** 2026-09-02
**Status:** **Root cause confirmed at the HCI level.** Discovery, admission and
the dial are all correct. The adapter's controller refuses the connection with
`0x0B ACL Connection Already Exists`, because the Windows HOGP peripheral
resolves to the **same Bluetooth identity as the active management link**. That
is an architectural constraint, not a defect, and it needs a product decision.

A second, independent defect found on the way — a latched
`pairing_close_deferred` that disabled *all* pairing until power cycle — is
**fixed and hardware-validated** (`94a1ad6`).

**Supersedes:** the working-note theory that Windows advertises the hosted GATT
service through extended-only PDUs, and this document's own earlier theories
that the pairing window never opened and that the dial used the wrong peer
address type. Both were measured and falsified; see Negative knowledge.

**Followed by:**
[`windows-classic-hid-device-feasibility-2026-09-02.md`](windows-classic-hid-device-feasibility-2026-09-02.md),
which asks whether Windows can instead reproduce Android's actual architecture
(BLE management + BR/EDR Classic HID). It cannot: the Classic HID **Device**
role does not exist on Windows at any supported layer. BLE HOGP as the Windows
Controller Link carrier is therefore **retired**, and Path C is recommended.

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
`ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION`. It does **not** define
`ENABLE_LE_EXTENDED_ADVERTISING`.

- No extended advertising ⇒ the adapter's `GAP_EVENT_ADVERTISING_REPORT` handler
  (`btstack_host.c:4623`) can only ever receive **legacy** advertising reports.
- `ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION` is what gates BTstack's **controller
  resolving-list** management in `hci.c` (`hci_load_le_device_db_entry_into_-
  resolving_list()`, the `LE_RESOLVING_LIST_*` state machine, guarded at
  `hci.c:4930`, `6243`, `9948` in pico-sdk 2.1.1's BTstack). `hci.c` does **not**
  rewrite advertising-report addresses in software. So a report that arrives
  bearing an identity address arrived that way **from the controller**, meaning
  address resolution is enabled and the peer's IRK is loaded.

Peer address **type** is therefore the thing to watch: Core spec reserves
`0x02`/`0x03` (Public/Random *Identity* Address) to tell a controller "resolve
RPAs against this identity". A dial that passes plain `0x00` asks the controller
to look for that literal address on air, which a private-addressed peer never
uses.

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

### R7 — the dial uses the identity address type, and BTstack accepts it

With the `dial` diagnostic in place (firmware `94a1ad62`):

```
dial n=3 addr=14:18:C3:47:C4:89 type=2 status=0x00 rpa_trust=false
```

`type=2` is `BD_ADDR_TYPE_LE_PUBLIC_IDENTITY`, which BTstack passes straight
through as the HCI `Peer_Address_Type` (`hci.c`, `hci_le_create_connection`).
**The theory that the dial normalises a resolved identity back to plain `0x00`
is falsified.** `status` is `gap_connect()`'s return, which reports *queueing*,
not controller acceptance — so on its own it proves nothing.

### R8 — the controller refuses the command: `0x0B`

With the Command Status diagnostic in place (firmware `1c7fcfcd`), every dial:

```
dial n=3 addr=14:18:C3:47:C4:89 type=2 gap=0x00 cmd=0x200D CMD_STATUS=0x0B
```

`cmd 0x200D` is `LE Create Connection`. **`0x0B` is
`ACL Connection Already Exists`.** The adapter's controller rejects the command
outright and never puts a `CONNECT_IND` on air — which is why the Windows radio
sees nothing (R5) and why three attempts each sit out the full 10 s watchdog.

The only ACL to that identity is the **management link**: Windows as central,
adapter as peripheral, `handle 0x0008` in BTHPORT HCIRAW, established ~40 s
before the first dial and encrypted throughout.

### R9 — the `pairing_close_deferred` fix holds

Same run, with `94a1ad6` in place:

```
 30.8s  win=true  def=false  hid=2  led=pairing
 33.0s  win=false def=TRUE   hid=2  led=pairing   ← window expires mid-connect
 37.5s  win=false def=false  hid=1  led=idle      ← resolves, LED returns to idle
```

Before the fix this latched for the rest of the boot. It now clears within one
watchdog period, the owner LED returns to idle, and pairing remains usable with
no power cycle. Reproduced across two runs. **Hardware-validated.**

---

## Interpretation — the confirmed chain

1. Windows advertises the hosted GATT service from a **resolvable private
   address** (R1). No WinRT API controls this:
   `GattServiceProviderAdvertisingParameters` exposes only `IsConnectable`,
   `IsDiscoverable`, `ServiceData` and the two secondary-PHY properties.
2. The adapter has the PC **bonded**, from the management pairing (R3) —
   which Controller Link *requires*.
3. The adapter's controller resolves the advertiser's RPA against the stored IRK
   and delivers the **identity** address `14:18:C3:47:C4:89` to the handler
   (R3 — proved by the raw `memcmp` matching, which an on-air RPA cannot do).
4. Because the delivered address is an identity address,
   `btstack_host_addr_is_rpa()` is false, so `ble_rpa_trust_candidate` is false
   and the code's RPA branch is bypassed.
5. `btstack_host_connect_ble_candidate()` therefore calls `gap_connect()` on the
   **identity** address, with `Peer_Address_Type = 0x02` (R7).
6. The adapter's controller answers `LE Create Connection` with
   **`0x0B ACL Connection Already Exists`** (R8) and emits nothing on air.
7. Windows sees no connection request at all (R5); every attempt sits out the
   10 s watchdog (R4).

### Why `0x0B`, and why it is not a defect

The adapter already holds an LE ACL to identity `14:18:C3:47:C4:89` — the
**management link**, Windows as central, adapter as peripheral. Controller Link
then asks the same controller to open a *second* LE connection, as central, to
that same identity.

A Bluetooth LE controller identifies peers by device identity, not by role or by
the address currently on air. Two simultaneous LE connections between the same
two devices are not a thing the link layer offers, and `0x0B` is precisely the
error reserved for asking. Windows advertising the HOGP service from an RPA does
not create a second peer: the RPA resolves to the same identity, which is the
entire purpose of resolvable private addressing.

**So Controller Link, as architected, cannot connect while management is
connected on the same radio — not because either side is misbehaving, but
because both relationships terminate at the same Bluetooth identity.**

The irony is load-bearing and now proven at the HCI level: **Controller Link
requires trusted management, and the management link is exactly what makes the
HOGP connection impossible.** A genuine controller is a different device with a
different identity, so it never hits this. The PC is the one peer that is
already talking to the adapter when Controller Link asks to talk to it again.

**Confidence: Confirmed.** The chain is a controller-reported HCI status, a
Windows-side HCI capture, a differential counter measurement with a control, and
repeated direct state reads.

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

**Fixed in `94a1ad6` and hardware-validated (R9).** The deferral now resolves on
"the attempt is no longer in flight" — an exit that cannot fail to happen —
with a 12 s bound as a backstop, decided by the pure, unit-tested
`ns2_bt_pairing_deferral_resolved()`.

---

## Conclusion

| Question | Answer |
|---|---|
| Does Windows advertise in a format the unchanged adapter can receive? | **Yes** — legacy `ADV_IND`, HID `0x1812`, scan-response name. Confirmed. |
| Does the adapter discover and admit it? | **Yes** — window opens, candidate admitted, `gap_connect` called three times. Confirmed. |
| Does the adapter dial correctly? | **Yes** — identity address, `Peer_Address_Type = 0x02`. Confirmed. |
| Why does no link form? | The adapter's controller refuses `LE Create Connection` with **`0x0B ACL Connection Already Exists`**: the Windows HOGP peripheral resolves to the same identity as the live management link, and an LE controller will not hold two connections to one peer device. Confirmed. |
| Is there a Windows-side fix? | **No.** The advertising address type is not exposed by any WinRT API, an RPA resolves to the same identity by design, and one radio has one identity. |
| Is there an adapter-side fix? | **Not a local one.** The refusal is a link-layer property, not firmware policy. Resolving it means changing *when* the two relationships coexist — a product decision (see below). |
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
anything until power-cycled. Fixed in `94a1ad6`.

**Do not trust `gap_connect()`'s return value as evidence the adapter dialled.**
It reports that BTstack *queued* a connection, not that the controller accepted
the command. `status=0x00` alongside `CMD_STATUS=0x0B` is exactly what a refused
dial looks like, and for one hardware session the `0x00` was read as "the dial
went out and the peer ignored it". Always read the Command Status.

**Do not assume the peer address type was the problem.** It was the obvious
suspect — the bond is stored type 0, identity types are 0x02/0x03, and a
mismatch would have explained everything. Measured: the dial already uses 0x02.
Two separate theories (the pairing window never opening, and the address type
being wrong) were each plausible, each fitted the evidence available at the
time, and each was wrong. The Command Status was the only field that could
distinguish them.

---

## Remaining unknowns

- Whether the CYW43439 would accept a second connection to the same identity if
  asked with the on-air RPA and `Peer_Address_Type = 0x01` instead. The spec
  says peers are identified by resolved identity, so it should refuse
  identically, but this has not been measured. It is the only cheap experiment
  that could still overturn the conclusion.
- Whether Windows, as an LE peripheral, would even accept an incoming connection
  from a device it is already central to. Untestable until the adapter side can
  ask.
- Whether the Android Controller Link path avoids this only because it uses
  Bluetooth **Classic** HID for the controller link and BLE for management —
  two different transports to the same identity, which the controller does allow.
  If so, that is the reason the Android design never met this wall, and it is
  worth stating explicitly in the Windows architecture notes.

## Where this leaves the product — a decision, not a bug fix

`0x0B` is a link-layer property. No amount of firmware policy or Windows API use
changes the fact that one radio has one identity and an LE controller will not
hold two connections to one peer. The options are therefore about **when the two
relationships coexist**, and each has a real cost:

**P1 — hand the radio over.** Drop the BLE management link while Controller Link
runs, and restore it on Stop. The dial would then succeed. Costs: management is
unavailable during play, which contradicts §27's same-radio
management-during-stream qualification; `ControllerLinkService`'s "requires
active trusted management" invariant weakens to "requires it to start"; §28's
management-loss handling needs rework so a deliberate handover is not read as
carrier loss. This is the only option that keeps Controller Link on one radio.

**P2 — ship Windows without Controller Link.** §14.6's documented branch.
Management, Touch Gamepad and the layout editor all ship; the Gamepad page
explains the constraint. Nothing is faked and nothing is half-built.

**P3 — Path C (§14.4).** Companion-provided normalized controller state over the
*existing* management link, as a new firmware input source. No second connection
is ever needed, because the link that already exists carries the input. This is
already a sanctioned fallback in `PLAN.md` and
`docs/bluetooth/android-controller-bridge.md`, and this experiment is the
strongest evidence yet that it is the right shape for Windows — but it is a
joint firmware + Windows pass, not a closeout.

**Not viable:** asking Windows for a second identity (one radio, one identity),
or dialling around the resolution (an RPA resolves to the same identity by
design).

## Firmware follow-ups from this pass

**F2 — done.** `94a1ad6`, hardware-validated (R9), with
`ns2_bt_pairing_deferral_resolved()` pinned by host tests including the
clock-wrap edges.

**F0 — done.** `94a1ad6` and `1c7fcfc` added the `dial` block to `btreconnect`:
attempt count, address, address type, `gap_connect()` return, RPA-trust branch,
and the controller's own Command Status. Keep it. It converted an
indistinguishable failure into a one-line answer, and no other diagnostic in the
firmware could tell "dialled and refused" from "never dialled".

**F1 — withdrawn.** It assumed the dial was targeting the wrong address. R7 and
R8 falsified that: the address and type are correct and the controller refuses
for an unrelated reason. Implementing it would have changed nothing.
