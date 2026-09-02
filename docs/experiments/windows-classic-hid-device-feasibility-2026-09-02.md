# Can Windows be a Bluetooth Classic HID **Device**, like Android?

**Date:** 2026-09-02
**Status:** Answered. **No** — not from user mode, and not from a kernel profile
driver either. The blocker is a specific, named reservation in `bthport.sys`.
**Follows:** [`windows-hogp-legacy-advertising-2026-09-02.md`](windows-hogp-legacy-advertising-2026-09-02.md),
which retired BLE HOGP as the Windows Controller Link carrier.

---

## Question

Android Controller Link works because it uses **two different transports** to
one dual-mode peer:

```
  BLE      Android companion  ──▶ PicoSwitch management GATT
  BR/EDR   Android BluetoothHidDevice ──▶ PicoSwitch Classic HID host
```

The Windows implementation substituted BLE HOGP for the second leg, which put
*two LE relationships* on one identity and was refused by the controller with
`0x0B ACL Connection Already Exists`.

So: **can Windows reproduce Android's actual architecture** — BLE management
plus a BR/EDR HID *Device* role — on one normal Windows-owned internal radio?

This is deliberately not answered from the HOGP result. HOGP says nothing about
Classic.

---

## What Android actually does (from source, not memory)

`android/companion/app/src/main/java/dev/picoswitch/companion/bridge/AndroidHidTransport.kt`:

| Step | Call |
|---|---|
| Acquire the profile | `BluetoothProfile.ServiceListener` → `BluetoothHidDevice` |
| Register the HID app | `hid.registerApp(sdp, null /*inQos*/, null /*outQos*/, executor, callback)` |
| SDP settings | `BluetoothHidDeviceAppSdpSettings(SDP_NAME, SDP_DESCRIPTION, SDP_PROVIDER, subclass, descriptorBytes)` |
| Name / description / provider | `"PicoSwitch Bridge Controller"` / `"Host controls passthrough"` / `"PicoSwitch2"` (`BridgeHidDescriptor`) |
| Subclass | `SUBCLASS1_COMBO or SUBCLASS2_GAMEPAD` |
| Descriptor | the shared 161-byte report descriptor, sha256 `f27315bf…5054`, bridge contract 4 |
| Open the link | `hid.connect(device)` — **the HID *Device* initiates**, to the Pico as HID *Host* |
| Input reports | `hid.sendReport(device, reportId, payload)` — interrupt channel, report ID 1, up to 125 Hz |
| Output reports | `onInterruptData(...)` **and** `onSetReport(...)` — both framings forwarded |
| Host poll | `onGetReport(...)` answered synchronously from the current state |
| Teardown | `unregisterApp()` — Android has **one HID Device slot per system** |

The firmware side is the ordinary Classic HID **host**: `hid_host_init()`,
`hid_host_connect()`, `PSM_HID_CONTROL 0x0011`, `PSM_HID_INTERRUPT 0x0013`
(`btstack_host.c`). Nothing exotic — it is the standard Bluetooth HID profile.

So Windows would need to serve **L2CAP PSM 0x0011 and 0x0013** plus a HID SDP
record, and accept an inbound connection from the adapter.

---

## Method

Disposable probes, built outside the repository, on the normal
Windows-owned radio with no driver changes. Each step reports its own
`WSAGetLastError`, and **RFCOMM is used as a control** — it is known-supported,
so if RFCOMM behaves the same way the result says nothing about L2CAP.

Environment: Windows 11 Pro 26200, Intel AX210 `USB\VID_8087&PID_0032` driver
24.40.10.8, internal, Windows-owned, radio on. Windows SDK 10.0.26100.

---

## Results

### C1 — an L2CAP Winsock provider *does* exist

`ws2bth.h` defines `BTHPROTO_L2CAP 0x0100`, and `WSAEnumProtocolsW` reports two
AF_BTH providers actually installed:

```
AF_BTH provider: proto=0x0100 socktype=1  "MSAFD L2CAP [Bluetooth]"
AF_BTH provider: proto=0x0003 socktype=1  "MSAFD RfComm [Bluetooth]"
```

`socket(AF_BTH, SOCK_STREAM, BTHPROTO_L2CAP)` **succeeds**. Only `SOCK_STREAM`
is accepted; `SOCK_SEQPACKET` returns `WSAESOCKTNOSUPPORT`, `SOCK_DGRAM` and
`SOCK_RAW` return `WSAEAFNOSUPPORT`.

**This corrects `WINDOWS_PASS.md` §8.4**, which stated user-mode Bluetooth
sockets are `BTHPROTO_RFCOMM`. The L2CAP catalog entry is real and a socket
handle is real. That is as far as it goes.

### C2 — no L2CAP bind succeeds, at any PSM

| Socket | Result |
|---|---|
| **RFCOMM `BT_PORT_ANY`** (control) | **bind OK (channel 4), listen OK — server up** |
| **RFCOMM channel 5** (control) | **bind OK, listen OK — server up** |
| L2CAP `BT_PORT_ANY` | bind FAILED `10050 WSAENETDOWN` |
| L2CAP PSM `0x1001` (free, odd) | bind FAILED `10050` |
| L2CAP PSM `0x5053` (BthPS3's artificial PSM) | bind FAILED `10050` |
| **L2CAP PSM `0x0011` HID CONTROL** | bind FAILED `10050` |
| **L2CAP PSM `0x0013` HID INTERRUPT** | bind FAILED `10050` |
| L2CAP PSM `0x0001` SDP / `0x0003` RFCOMM / `0x000F` BNEP | bind FAILED `10050` |

The controls bind and listen on the same radio in the same process, so the
radio is up and the process has the rights. **Every** L2CAP bind fails
identically — free PSMs, reserved PSMs, dynamic allocation alike.

The uniformity is the finding: this is **not** PSM-specific reservation at the
socket layer. The `MSAFD L2CAP` provider is registered in the Winsock catalog
but has no working server path. **There is no user-mode L2CAP server on
Windows.**

### C3 — the SDP half *is* available

`WSASetServiceW(RNRSERVICE_REGISTER)` from an ordinary user-mode process:

| Record | Result |
|---|---|
| custom 128-bit service class (control) | **REGISTER OK** |
| **HID service class `0x1124`** | **REGISTER OK** |

So Windows will happily let an application *advertise itself as a HID service*.
It will not let it serve the channels behind that advertisement — which is worse
than not advertising, because a host would discover the record, connect, and
fail.

### C4 — WinRT has no HID Device role at all

Scanning the generated projection for `Windows.Devices.Bluetooth*`: no type
matching `*Hid*` exists in any of those namespaces. The only server-role class
in the whole Bluetooth surface is `RfcommServiceProvider`
(`Windows.Devices.Bluetooth.Rfcomm`). There is no `BluetoothHidDeviceProvider`,
no HID equivalent of `GattServiceProvider`.

### C5 — kernel mode does not rescue it either

This is the part that makes the answer final rather than "needs a driver".

- **Microsoft's own position.** The `bthecho` L2CAP sample in
  `microsoft/Windows-driver-samples` is a **KMDF** profile driver pair that
  "registers PSM and L2CA server and published SDP record on startup", and
  states plainly: *"RFCOMM based profiles must be developed and accessed using
  user-mode socket APIs."* L2CAP servers are a kernel-mode profile-driver
  concept; BRBs (`BRB_L2CA_REGISTER_SERVER`) are not reachable from user mode.
- **The HID PSMs are reserved even from kernel mode.** The reservation is
  enforced by an internal check, `bthport.sys!BthIsSystemPSM`, which rejects
  registration of the HID PSMs `0x11`/`0x13`. The only documented way past it is
  **runtime binary patching of `bthport.sys`**.
- **Independent corroboration.** `nefarius/BthPS3` is a signed, widely deployed
  kernel-mode L2CAP profile-and-filter driver written specifically to carry
  Bluetooth HID traffic for PS3 peripherals. It could not take `0x11`/`0x13`
  either: it registers **artificial PSMs `0x5053` and `0x5055`** and filters
  traffic onto them. An expert-authored signed driver routing *around* exactly
  these PSMs is the strongest available evidence that they are not obtainable.

Patching `bthport.sys` at runtime is not a product option: it is incompatible
with HVCI and Secure Boot expectations, cannot be signed or serviced, and would
put an unsupported modification of the OS Bluetooth stack on a user's machine to
ship a gamepad feature.

---

## Requirement mapping

Everything a Classic HID Device needs, against what Windows offers:

| Requirement | Windows |
|---|---|
| SDP HID service record (`0x1124`), name/description/provider | **Available** — `WSASetService`, measured C3 |
| HID report descriptor in SDP | Available (record attributes) |
| Class of Device control | Partly — `BluetoothSetLocalServiceInfo`-era APIs; moot |
| **L2CAP server, PSM 0x0011 (control)** | **Unavailable.** No user-mode L2CAP server (C2); reserved from kernel mode (C5) |
| **L2CAP server, PSM 0x0013 (interrupt)** | **Unavailable.** Same |
| Accept inbound L2CAP connection | Unavailable — no server to accept on |
| HID handshake / control messages | Unreachable |
| Interrupt input reports at 125 Hz | Unreachable |
| Output reports / `SET_REPORT` | Unreachable |
| `GET_REPORT` synchronous answer | Unreachable |
| Virtual cable / reconnect | Unreachable |

The SDP half is available. The L2CAP half — which is the entire transport — is
not, at any trust level short of modifying the OS.

Full trust does not change it (C2 ran full trust). AppContainer does not change
it (it can only *lose* capability). A Windows service does not change it
(user-mode either way). A KMDF profile driver gets L2CAP but still not these
PSMs (C5).

---

## Conclusion

**Windows cannot reproduce Android's dual-transport Controller Link
architecture.** Not because BLE + BR/EDR concurrency is a problem — that
question is never reached — but because the BR/EDR HID **Device** role does not
exist on Windows at any supported layer.

The concurrency question in the original brief is therefore **moot**: there is
no second transport to run concurrently. (For the record, the *adapter* side is
demonstrably fine with mixed transports — `btstate` shows `inquiry_active: true`
alongside a live BLE management link. Windows was never the tested half and now
need not be.)

**Confidence: Confirmed** for the user-mode result (direct measurement with a
passing control on this exact radio). **Strong Evidence** for the kernel-mode
result (Microsoft's own sample documentation, the named `BthIsSystemPSM` check,
and BthPS3's independent workaround; not reproduced here because building a
signed profile driver to prove a negative was out of scope).

---

## Negative knowledge preserved

**Do not re-derive "Windows has no L2CAP" from the absence of
`BTHPROTO_L2CAP`.** The constant exists, the `MSAFD L2CAP [Bluetooth]` provider
is installed, and `socket()` returns a valid handle. The failure is at `bind`,
uniformly, with `WSAENETDOWN` — which is easy to misread as a radio problem.
Always run the RFCOMM control in the same process.

**Do not read "SDP registration succeeded" as progress toward Classic HID.**
Registering the `0x1124` HID service class works and means nothing on its own.
An SDP record with no L2CAP server behind it is a trap: hosts will find it and
fail to connect.

**Do not treat "it needs a kernel driver" as the answer.** It is worse than
that. A signed KMDF profile driver still cannot register PSM `0x11`/`0x13`;
BthPS3 had to invent `0x5053`/`0x5055` precisely because of this. Anyone
scoping "just write a driver" should read C5 first.

**Do not propose dropping BLE management to make a BLE HOGP link work.** That
was considered and rejected: it exists only to salvage a carrier that is being
retired, and it breaks the product invariant that management stays connected
while Controller Link runs.

---

## Remaining unknowns

- Whether `WSAENETDOWN` on L2CAP bind is a deliberate stub or an artefact of a
  provider that was never finished. Immaterial to the decision.
- Whether a future Windows release adds a HID Device role. Worth re-checking if
  Microsoft ships a peripheral-role Bluetooth API; nothing in SDK 10.0.26100
  suggests it.

---

## Recommendation

**CLASSIC HID NOT VIABLE UNDER THE NORMAL WINDOWS STACK — PATH C IS THE
RECOMMENDED WINDOWS TRANSPORT.**

Path C (`WINDOWS_PASS.md` §14.4, already sanctioned in `PLAN.md` as
"companion-provided normalized controller state") carries normalized
`ControllerState` over the **management link that already exists**. It needs no
second ACL, so it cannot hit the `0x0B` collision, and it needs no transport
role Windows does not have.

Carrier headroom, from the repository: `ns2_bt_mgmt_link_params()` requests an
interval floor of **7.5 ms** (6 units) with `latency = 0`, so 125 Hz is within
the negotiated envelope; a 25-byte report fits one ATT payload at any negotiated
MTU above the 23-byte default. The existing management channel is newline-JSON,
single-flight, request/response — so this needs a **separate binary
characteristic**, not a reuse of the command path, and firmware work to add the
input source.

Roughly everything above the transport boundary survives the pivot:
`ControllerInputSession`, `ControllerReportEncoder`, the 125 Hz scheduler,
latest-state-wins, neutralization, `BridgeOutputCodec`, `RumbleShaping`,
`ControllerLinkService` orchestration, management gating, the state model,
diagnostics, the physical-controller source and the Touch Gamepad source. Only
the AppContainer HOGP host and its package IPC would retire.

**Not authorised here.** This document establishes the feasibility decision
only; implementing Path C needs explicit approval and is a joint firmware +
Windows pass.
