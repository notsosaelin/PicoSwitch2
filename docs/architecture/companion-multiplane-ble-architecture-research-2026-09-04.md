# One trusted BLE companion relationship, many purpose-built planes

**Type:** high-level design / feasibility research. **No implementation.**
**Date:** 2026-09-04
**Branch:** `ns2-testing`
**Scope:** transport architecture only. Nothing in this document authorises code.

Supersedes nothing. It **extends**
[`../experiments/android-gameplay-carrier-choice-2026-09-03.md`](../experiments/android-gameplay-carrier-choice-2026-09-03.md)
with source-level Android platform facts that document did not have, and it
**reopens one closed question** — see §12.4 on
[`../bluetooth/android-audio-feasibility-2026-08-13.md`](../bluetooth/android-audio-feasibility-2026-08-13.md).

---

## Evidence classes used throughout

| Class | Meaning |
|---|---|
| **REPO** | Established from this repository's source, tests, fixtures or its own hardware records. |
| **AOSP** | Read directly from Android Open Source Project source this session. Authoritative for stock Android; OEM overlays can change resource values. |
| **EXT** | External documentation or credible community measurement. Named at the point of use. |
| **LOCAL** | Measured in this working tree this session. |
| **HYP** | Plausible, not yet validated. The falsifying test is named. |
| **INFER** | Design inference from the above. Not a measurement. |

Nothing below is promoted past its class. Where a number would change the
decision, the experiment that would settle it is named.

---

## 1. Executive summary

**The central question.** Windows Path C proved that a dedicated realtime binary
GATT data plane can ride the *existing trusted management BLE ACL* and feel
native in real gameplay. Should PicoSwitch2 therefore treat BLE as a general
**companion transport** carrying several purpose-built planes — management,
controller input, controller feedback, and eventually audio downlink and
microphone uplink — and should Android gameplay move off Bluetooth Classic HID
onto it?

**The answer, in one line: the architecture is right; the Android migration is
now *worth prototyping* rather than *worth doing*, and the reason has changed.**

The 2026-09-03 recommendation to keep Classic rested on two legs. This pass
knocks one out and replaces it with a better one.

- **Leg 1 — "the stated benefit is already delivered."** Still true, still
  decisive on its own axis (§2). Management gating of the Classic carrier is
  enforced in firmware and is a property of the *relationship*, not the carrier.
  Nothing here changes that.
- **Leg 2 — "the Android BLE link is tuned for management, not gameplay."**
  **This is now known to be wrong as stated.** Stock Android
  `CONNECTION_PRIORITY_HIGH` requests **11.25–15 ms** (AOSP `config.xml`
  `gatt_high_priority_min_interval` = 9, `max` = 12 units, latency 0), which is
  the *same* band Windows Path C requested and qualified at. The 65–194 ms
  management round trips are **not** the connection interval; they are the JSON
  control plane plus a firmware bridge that holds exactly one command and one
  response at a time. §4 and §5.
- **A new leg appears, and it is sharper than the one it replaces.**
  `BluetoothGatt` enforces **one outstanding GATT operation per remote device**
  via `mDeviceBusy` — shared by reads, writes and descriptor writes, across
  *every characteristic on that `BluetoothGatt` object*, and **not** bypassed by
  `WRITE_TYPE_NO_RESPONSE`. Worse, when it is busy the framework
  `writeCharacteristic` **blocks the calling thread** in `Thread.sleep(10)` up to
  5 times. On Android, a management command with response and a gameplay frame
  therefore contend at framework level in a way they demonstrably do **not** on
  Windows. §6.2. This is the single most important engineering fact this pass
  produces, it is solvable, and it must be *designed for*, not discovered.

**On audio.** Raw PCM over this BLE link is not marginal, it is impossible by
roughly 5x (§11). Compressed transport is mandatory. But the project has
**already built the hard half twice** and validated it on hardware: a UAC1
console-facing speaker endpoint that receives real Switch 2 PCM, and an Opus/CELT
encoder that sustains 96 kbit/s to a genuine Pro Controller 2 over GATT writes
*while* input notifications keep flowing (§10, §12). That is an existence proof
at the exact bitrate a companion downlink would need, on this exact radio. It is
also **RP2350-only**, by a CMake gate and by a measured 20 KB of free SRAM on the
RP2040 (§13).

**Recommendation:** `PATH C LOOKS PROMISING; BUILD AN EXPERIMENTAL ANDROID
CARRIER` — behind a developer flag, with Classic kept as production and as
rollback, and with the connection-interval measurement taken first because it is
one small firmware diagnostic away (§5.4).

---

## 2. Current Android architecture, from source

```text
  ANDROID COMPANION                                  PicoSwitch2 adapter
  -----------------                                  -------------------

  BleGattManagementTransport ---- BLE ACL ---------> config_ble (ATT server,
    connectGatt(...)                                   peripheral role)
    requestConnectionPriority(HIGH)                    RX  : write / write-no-rsp
    requestMtu(517)                                    TX  : notify
    RX/TX newline-JSON commands                        (CL-IN / CL-OUT present
    WRITE_TYPE_DEFAULT (with response)                  but UNREFERENCED here)
                |
                |  trust established
                v
  AndroidHidTransport ---------- BR/EDR ACL -------> BTstack HID Host
    BluetoothHidDevice.registerApp(sdp)                hid_host_connect()
    hid.connect(adapter)                               PSM 0x0011 / 0x0013
    hid.sendReport(id=1, 26 B payload)  -->            android_bridge_identify()
    onInterruptData / onSetReport       <--            exact 161-byte match
```

**REPO.** Traced this session:

| Stage | Source of truth |
|---|---|
| Registration, SDP, one-HID-slot-per-system rule | `android/companion/app/src/main/java/dev/picoswitch/companion/bridge/AndroidHidTransport.kt` |
| Management trust gating of Classic admission | `ns2_bt_companion_classic_admission_allowed()`; teardown in `classic_companion_release_on_mgmt_loss()` (`btstack_host.c`) |
| Report composition, cadence, motion, battery, rumble, neutralisation | `bridge-core/.../session/BridgeSession.kt` — **platform-neutral**, shared by every backend |
| Input cadence | `BridgeSession.REPORT_INTERVAL_MS = 8` gives a **125 Hz ceiling**, conflated mailbox, newest state retained |
| Battery poll | `BATTERY_POLL_MS = 30_000` |
| Output watchdog | `OUTPUT_WATCHDOG_TICK_MS = 250` |
| Wire contract | `tools/fixtures/android_controller_hid.h`, 161-byte descriptor, sha256-pinned per contract version |
| Identity | **exact 161-byte descriptor match**, never VID/PID (`docs/bridge/PROTOCOL.md` §2) |

Three Android-stack facts already recorded in `AndroidHidTransport.kt` and worth
carrying into any migration plan: **one HID Device slot per system**; **the
callbacks are authoritative, not the return values** (an OEM stack returns
`false` from `registerApp()` and then delivers
`onAppStatusChanged(registered = true)`); and **output arrives with two framings**
(interrupt channel *and* control-channel `SET_REPORT`).

**The stated motivation for migrating is already satisfied**, and this pass does
not disturb that finding: a cross-transport companion is refused Classic
admission unless its management session is trusted, and the Classic link is torn
down when management drops. That is a property of the companion *relationship*,
not of the carrier.

**What is carrier-specific vs. semantic.** The split is already clean, and this
is the load-bearing fact for the whole migration question:

| Layer | Owner | Carrier-specific? |
|---|---|---|
| Normalized `ControllerInputState`, touch engine, layouts | `:bridge-core` | No |
| Report composition, cadence, motion gating, rumble, neutralisation | `BridgeSession` | No |
| Encode to the canonical 26-byte payload | `ControllerReportEncoder` | No |
| Delivering bytes to the adapter | `BridgeTransport` | **Yes — this is the only thing a Path C carrier replaces** |

`BridgeTransport` is a small interface. An Android Path C carrier is a second
implementation of it, not a second gameplay path.

---

## 3. Current Windows Path C architecture, from source

```text
  WINDOWS COMPANION                                  PicoSwitch2 adapter
  -----------------                                  -------------------
                       ONE BLE ACL, one bond, one Windows-owned radio

  BleGattManagementTransport ---- RX/TX -----------> config_ble JSON command plane
                                                     (one command + one response
  ControllerLinkService                               in flight, by construction)
    sample every 2 ms
    digital floor    2 ms  --.
    analog ceiling   8 ms  --+-- ControllerLinkSendPolicy
    keepalive      100 ms  --'
    ControllerLinkWriter  --- CL-IN (write-no-rsp) -> ns2_companion_link_parse()
      latest-state mailbox                              version/opcode/sequence
      at most 1 write in flight                         canonical v2 decode
                          <-- CL-OUT (notify) --------  COMPANION input source
                                                        arbiter, bridge pipeline
```

**REPO.** All four characteristics live in **one 128-bit service** so they
inherit one bond, one encryption context and one allowlist
(`setup_att_server()`, `btstack_host.c`). CL-IN is declared
`ATT_PROPERTY_WRITE_WITHOUT_RESPONSE` **only** — deliberately, so an acknowledged
write is refused and gameplay cannot serialise behind a round trip. CL-OUT is
`ATT_PROPERTY_NOTIFY`.

| Property | Value | Source |
|---|---|---|
| Frame | `[version][opcode][seq_lo][seq_hi]` + 26-byte canonical payload = **30 B** | `include/ns2_companion_link.h` |
| Minimum ATT MTU | 33 (30 + 3 ATT Write Command overhead) — **measured, refuses to start below it** | same |
| Output frame | `[version][opcode][report id]` + 4 B feedback = **7 B** | same |
| Sequence semantics | signed 16-bit delta, wrap-safe, advances only on accept | `ns2_companion_link_parse()` |
| Session re-arm | `ns2_companion_link_arm_session()` on **every** `clink start` | fixed 2026-09-03 after a silent dead-client high-water-mark bug |
| Stale watchdog | 300 ms, keyed on **data frames**, armed only once streaming | `ns2_companion_link_input_stale()` |
| Source identity | explicit **COMPANION** class, never a faked bthid slot | `ns2_companion_link.h` header contract |

**Qualified on hardware 2026-09-03** (firmware `9cc2511f`, Switch 2 attached):
ATT MTU 527; `received == applied` in every run; stale/short/version/opcode and
arbiter-rejected all zero; `MaximumInFlight` 1; 250 Hz applied over 45 s at fixed
cadence; about 10 Hz at idle once the publisher moved to send-on-change.
Management during streaming: **median 117 ms against 241 ms idle** — *faster*,
because the link is already on a short interval rather than relaxed.

**The backpressure lesson, and why it matters here.** `WriteValueWithResultAsync`
completes when the **Windows driver accepts the buffer**, not when the radio
transmits. Anything beyond the link's drain rate therefore accumulates *inside
the driver* and is replayed **in order** — which is why over-sending presented as
a stick that kept moving after the player stopped, rather than as latency
(`windows-controller-link-analog-backlog-2026-09-03.md`; the actual root cause
there was radio contention from the controller's own Bluetooth link, and every
Path C counter stayed perfect throughout). The cure was structural: a
**latest-state mailbox with at most one write in flight**, plus a 2/255 analog
epsilon so an idle stick cannot set the send rate.

---

## 4. Why management RTT is not a BLE gameplay latency measurement

**REPO + AOSP.** The 65–194 ms figure decomposes into at least: app scheduling,
JSON serialisation, `writeCharacteristic` with `WRITE_TYPE_DEFAULT` (a **full ATT
round trip**, at least two connection events), firmware command dispatch on a
bridge that **holds one command and one response at a time and answers a third
with `INSUFFICIENT_RESOURCES`** (`config_wireless_bridge_receive()`, documented in
`ManagementRetryPolicy.kt`), reply generation, notification, and Android callback
dispatch.

Windows already demonstrated the separation empirically: the *same* JSON control
plane on the *same* ACL measured 241 ms idle while the binary data plane beside
it ran at 250 Hz with zero stale frames. A slow control plane coexisting with a
fast data plane is observed behaviour, not a hope.

**Conclusion (INFER, strongly supported):** management RTT is an **upper bound on
the control plane** and says nothing about a dedicated binary data plane. It must
not be used as a gameplay latency argument in either direction.

---

## 5. What the Android BLE link actually negotiates

### 5.1 What stock Android *requests* — AOSP, read this session

`BluetoothGatt.requestConnectionPriority()` calls
`GattService.connectionParameterUpdate()`, which reads
`CompanionManager.getGattConnParameters()`, which reads
`packages/modules/Bluetooth/android/app/res/values/config.xml`:

| Priority | min | max | latency | Interval |
|---|---|---|---|---|
| `CONNECTION_PRIORITY_HIGH` | **9** | **12** | 0 | **11.25 – 15 ms** |
| `CONNECTION_PRIORITY_BALANCED` | 24 | 40 | 0 | 30 – 50 ms |
| `CONNECTION_PRIORITY_LOW_POWER` | 80 | 100 | 2 | 100 – 125 ms |
| `CONNECTION_PRIORITY_DCK` | 24 | 24 | 0 | 30 ms |

Two further facts from the same code path:

- **`GattService` hard-codes the supervision timeout to `500` (5 s)** on every
  `requestConnectionPriority` call. The firmware's own
  `gap_request_connection_parameter_update(..., 600 /* 6 s */)` from
  `ns2_bt_mgmt_link_params()` is a *peripheral request the central may ignore*;
  against stock Android the effective value will be **5 s**, chosen by the phone.
  That is close to the 6 s the JoypadOS evidence called for and well clear of the
  roughly 2 s the header worried about, so this is a **recorded fact, not a
  defect** — but "typically ~2 s" is not what stock Android does once the app
  calls `HIGH`, which our app already does on connect.
- The `_primary` / `_secondary` variants (7.5 ms with latency 45, and so on)
  apply only to a **CDM companion device** (`CompanionManager.isCompanionDevice`,
  the single paired watch-shaped device carrying primary/secondary metadata). A
  normally bonded adapter is `COMPANION_TYPE_NONE` and gets the default column.

**These are AOSP defaults and are overridable by OEM overlay.** That is the whole
reason §5.4 exists.

### 5.2 What the app requests today

**REPO.** `BleGattManagementTransport.kt` already does the right things on
connect: `requestConnectionPriority(CONNECTION_PRIORITY_HIGH)` immediately on
`STATE_CONNECTED`, then `requestMtu(517)` before service discovery. It drops back
to `CONNECTION_PRIORITY_BALANCED` when idle.

### 5.3 What was *negotiated* — still unknown, and that is the gap

**REPO.** `onConnectionUpdated` is declared defensively in the callback (it is a
hidden framework method with no public SDK stub) and logs `gatt.params`. **This
OEM does not dispatch it**, so no line was ever produced. Requested is not
negotiated, and this document does not treat the table in §5.1 as an observation.

### 5.4 The measurement that would settle it — designed, NOT implemented

The adapter can already see the answer and simply does not report it.

**What exists today (REPO):**

- `HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE` is handled in `btstack_host.c` and
  **already prints** `interval / latency / timeout` to UART for *every* LE link,
  including the management peripheral-role ACL. It fires only on an *update*,
  not on the initial connection parameters.
- `gap_le_connection_interval(con_handle)` is already used elsewhere in the same
  file (`switch2_v2_request_fast_link`), so the live value is readable at any
  moment without new BTstack surface.
- `btstate` already reports `clink.handle` and the management connection state.

**The minimal diagnostic (unauthorised; specified so it is not re-derived):**

> Add to the existing `btstate` reply, for `config_ble.handle`: `interval_units`
> from `gap_le_connection_interval()`, plus the last values latched by the
> existing `HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE` handler (`latency`,
> `supervision_timeout`, `status`) and a monotonic **update count**. Latch rather
> than sample on demand, so a transient renegotiation is not missed. No new event
> registration, no GATT change, no behaviour change — read-only reporting of
> state BTstack already holds.

That is one struct and one format-string change, and it answers §5.3 for **both**
companions on **any** phone, permanently. It is the highest information per line
of change available here, and it is the recommended next experiment.

---

## 6. Android GATT realtime behaviour

### 6.1 Write-without-response *is* flow controlled on Android — AOSP

From `packages/modules/Bluetooth/system/stack/gatt/gatt_cl.cc`, read this
session:

```c
/* gatt_act_write, GATT_WRITE_NO_RSP */
op_code = GATT_CMD_WRITE;
rt = gatt_send_write_msg(tcb, p_clcb, op_code, ...);   /* queued into cl_cmd_q */
if (rt != GATT_CMD_STARTED) gatt_end_operation(p_clcb, rt, NULL);
```

```c
/* gatt_cl_send_next_cmd_inq */
att_ret = attp_send_msg_to_l2cap(tcb, cmd.cid, cmd.p_cmd);
if (att_ret != GATT_SUCCESS && att_ret != GATT_CONGESTED) { pop_front(); continue; }
if (cmd.op_code == GATT_CMD_WRITE || cmd.op_code == GATT_SIGN_CMD_WRITE) {
    p_clcb = gatt_cmd_dequeue(tcb, cmd.cid, &rsp_code);
    gatt_end_operation(p_clcb, att_ret, NULL);   /* fires onCharacteristicWrite */
    if (att_ret == GATT_SUCCESS) continue;       /* "if no ack needed, keep sending" */
    return true;                                 /* congested: stop, stay queued */
}
```

**So (AOSP):** `onCharacteristicWrite` **does** fire for a write command, and its
`status` is the **L2CAP accept result** — `GATT_SUCCESS` when the transmit path
took it, `GATT_CONGESTED` when it did not. Independently corroborated by
Punchthrough's Android BLE guide ("for `WRITE_TYPE_NO_RESPONSE`, where we'd
expect the callback not to be delivered, it does still get delivered") and by the
long-standing community note that one must wait for the callback because the
platform withholds it while buffers are full. Punchthrough's *other* article
("Write Requests vs. Write Commands") states the opposite; the source above is
the tiebreaker, and their own guide agrees with it.

**This is better than Windows.** Windows Path C had to *infer* a backpressure
boundary from local operation completion and defend it with a rate ceiling;
Android hands you a genuine transmit-path credit.

### 6.2 But Android forces single-flight *per device*, across all characteristics

From `packages/modules/Bluetooth/framework/java/android/bluetooth/BluetoothGatt.java`:

```java
synchronized (mDeviceBusyLock) {
    if (mDeviceBusy) return BluetoothStatusCodes.ERROR_GATT_WRITE_REQUEST_BUSY;
    mDeviceBusy = true;
}
...
for (int i = 0; i < WRITE_CHARACTERISTIC_MAX_RETRIES /* 5 */; i++) {
    requestStatus = mService.writeCharacteristic(...);
    if (requestStatus != ERROR_GATT_WRITE_REQUEST_BUSY) break;
    Thread.sleep(WRITE_CHARACTERISTIC_TIME_TO_WAIT /* 10 ms */);
}
```

`mDeviceBusy` is a **single boolean on the `BluetoothGatt` object**, taken by
`readCharacteristic`, `writeCharacteristic`, `readDescriptor`, `writeDescriptor`,
`readUsingCharacteristicUuid` and `executeReliableWrite` alike, and cleared in the
matching completion callback. It is **not** per-characteristic and **not**
bypassed by `WRITE_TYPE_NO_RESPONSE`.

Three consequences a naive Android Path C client would meet as bugs:

1. **A management command blocks gameplay.** Our management writes use
   `WRITE_TYPE_DEFAULT` (with response, deliberately — that is what surfaces the
   firmware `INSUFFICIENT_RESOURCES` drop, per `ManagementRetryPolicy.kt`). It
   holds `mDeviceBusy` for a **full ATT round trip**, at least two connection
   events, roughly 25–30 ms at a 15 ms interval. Every gameplay frame in that
   window is refused.
2. **Refusal costs wall-clock time on the caller's thread.** Up to
   5 × `Thread.sleep(10)` = **50 ms of blocking** inside `writeCharacteristic`. A
   gameplay sender must therefore never run on the main thread, and never on a
   thread shared with anything latency-sensitive.
3. **Reads count too.** The background input poll and any status read hold the
   same token.

This is the substantive difference from Windows, where management ran at 117 ms
median *while* gameplay streamed at 250 Hz on the same ACL with no observed
interaction. The Windows stack allows the concurrency; the Android framework does
not.

### 6.3 Candidate answers (all HYP — none validated)

| Option | Idea | Risk |
|---|---|---|
| **A. One outbound scheduler** | A single owner of the `BluetoothGatt` token. Gameplay is the default holder; management commands take an explicit priority window and are deferred, not dropped, while a frame is outstanding. | Management latency during gameplay grows. Cheapest and most predictable. |
| **B. Two `BluetoothGatt` objects to the same device** | `connectGatt` twice from one app registers two client interfaces over **one ACL and one bond**; each `BluetoothGatt` has its **own** `mDeviceBusy`, so the framework head-of-line block disappears. | **Unverified.** Both still share one ATT bearer and one `cl_cmd_q` in the native stack, so this removes framework serialisation, not link serialisation. OEM behaviour unknown. |
| **C. LE L2CAP CoC for the data plane** | `BluetoothDevice.createL2capChannel(psm)` (API 29+, LE only) gives a real credit-based channel, independent of ATT and of `mDeviceBusy`, on the same bond. BTstack already has `ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE` compiled in (`src/btstack_config.h`). | **No Windows equivalent** (§8). Would make the two companions structurally different at the transport layer — which §23 argues may be acceptable. |

**Option A needs no new platform assumption and should be the first prototype.**
B and C are measurable improvements to try afterwards, in that order.

---

## 7. Multiple characteristics are not multiple radios

**INFER, and it needs stating because the plane diagram invites the opposite
reading.**

`MGMT-RX/TX`, `CL-IN`, `CL-OUT` and any future `AUDIO-DOWN` / `MIC-UP` are
**application planes**. They share, at minimum:

- one LE connection and one Link Layer schedule (one interval, one event budget);
- one ATT bearer and one L2CAP channel, unless EATT is negotiated (§8);
- the controller's ACL buffer pool (`MAX_NR_CONTROLLER_ACL_BUFFERS 3` in
  `src/btstack_config.h`);
- on the adapter, the **CYW43 radio that is also carrying BR/EDR HID to the
  source controller**;
- on Android additionally, `mDeviceBusy` for everything outbound (§6.2).

Therefore **the QoS model must be explicit and must live in the application**,
because no layer below it will prioritise one plane over another. §15.

---

## 8. Transport primitive comparison

| | **A. GATT characteristics** | **B. LE L2CAP CoC** | **C. EATT** | **D. LE ISO / LE Audio** |
|---|---|---|---|---|
| Windows public API | **Yes** — WinRT GATT, in production today (REPO) | **No.** `MSAFD L2CAP` is in the Winsock catalog and `socket()` succeeds, but **every** bind fails `10050 WSAENETDOWN` at every PSM; "there is no user-mode L2CAP server on Windows" (REPO, `windows-classic-hid-device-feasibility-2026-09-02.md` §C1–C2). `AF_BTH` is BR/EDR addressing regardless. | No app-level bearer selection | No |
| Android public API | **Yes** | **Yes** — `createL2capChannel` / `listenUsingL2capChannel`, API 29+, LE only (EXT: Android reference) | Implemented in the stack (`EattExtension` appears throughout `gatt_cl.cc`) but **no app API to select a bearer** (AOSP) | LE Audio is a system profile; **no app API to create a CIS or BIS** |
| BTstack / Pico | Yes, in use | Yes — `ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE` already defined (REPO) | Not enabled (`ENABLE_GATT_OVER_EATT` absent) | **Unknown** — CYW43439 is BT 5.2, but ISO controller support is unverified |
| Bidirectional | Yes (write plus notify) | Yes | Yes | Yes |
| Flow control | ATT/L2CAP congestion surfaces as `GATT_CONGESTED` on Android; nothing comparable surfaced on Windows | **Explicit L2CAP credits** — the cleanest model available | As GATT | Isochronous, deadline scheduled |
| Reuses the existing bond | Yes | Yes | Yes | Yes |
| Complexity and risk | **Lowest — already shipping** | Medium; second listener plus PSM exchange | High, and not controllable anyway | Very high; unverified controller support |

**Verdict (INFER).** GATT characteristics remain the **cross-platform baseline**
and must carry management, controller input and controller feedback. LE CoC is
the only primitive offering real credits and it is **Android-only** in this
project's platform set, so it belongs in the "measure later, Android-only
optimisation" column rather than in the baseline. EATT is not selectable from an
app on either platform, so it can only ever be a transparent win. LE ISO is not
reachable: no app API on Android, none on Windows, and unverified controller
support on the CYW43439. **Do not pursue an exotic primitive because it is
elegant.**

---

## 9. Plane-by-plane requirements

### 9.1 Management and control — leave it exactly as it is

Low rate, reliable, request/response, JSON, one command and one response in
flight in firmware by construction. It must remain usable during realtime traffic
and must never block gameplay. On Windows it already achieves both. On Android
that requires the scheduler of §6.3-A.

**Recommendation:** no change to the JSON protocol and no realtime traffic on it,
ever. The `bridge_contract` and `build` fields in `info`, plus the `version`
field in the `clink` reply, are the only negotiation surface needed for now
(§18).

### 9.2 Controller input (companion to adapter)

| Requirement | How it is met today |
|---|---|
| Complete canonical `ControllerState` | 26-byte canonical v2 payload, shared fixtures |
| Latest-state-wins | 16-bit sequence, signed-delta compare, wrap-safe |
| No replay of historical analog state | latest-state **mailbox**, never a queue |
| Bounded backlog | at most one write in flight (Windows: enforced by the app; Android: enforced by the framework) |
| Digital correctness | separate 2 ms digital floor, never rate-limited by the analog ceiling |
| Stale watchdog | 300 ms on the adapter keyed on data frames; 100 ms keepalive on the companion |

**Ideal Android cadence (INFER).** At an 11.25–15 ms interval, more than roughly
66–89 frames per second cannot each occupy their own connection event, and the
Android single-flight gate makes over-sending self-defeating rather than merely
wasteful. `BridgeSession` already ceilings at 125 Hz with a conflated mailbox,
comfortably above the link and structurally safe. **Do not raise it, and do not
invent a 250 Hz Android mode.** The knob worth porting from Windows is the 2/255
analog epsilon and the send-on-change plus keepalive shape, which cut the Windows
idle rate from 250 Hz to about 10 Hz.

**Write type:** `WRITE_TYPE_NO_RESPONSE` (CL-IN declares nothing else), paced by
`onCharacteristicWrite`, never by a timer alone.

### 9.3 Controller output and feedback (adapter to companion)

CL-OUT is a 7-byte notification carrying report id plus 4 canonical feedback
bytes (rumble L, rumble R, player LED, flags). Change-suppressed, low rate.
Notifications are **inbound** on both platforms and touch neither `mDeviceBusy`
nor any send queue.

**Assessment (INFER): CL-OUT is already sufficient cross-platform** and needs no
redesign for Android. The reverse path is proven working on Windows
(`outputs sent=36 failed=0`, `output=4/4 malformed=0`); what remains unmeasured
there is *amplitude-change latency*, because only the console can command it.

### 9.4 Audio downlink (console, adapter, companion playback)

**The console-facing half already exists and is validated (REPO).**
`src/switch_pro2/switch_pro2.c` implements the byte-verified retail PC2 **UAC1**
function: EP `0x03` isochronous OUT and EP `0x83` isochronous IN,
`NS2_AUDIO_PACKET_SIZE = 192` at 1 ms, that is **48 kHz, 16-bit, stereo =
192,000 B/s** in each direction. The Pico SDK generic `tud_audio_*` driver could
not be used (its `open()` accepts UAC2 interface protocol only), so the project
implements the UAC1 endpoint and control lifecycle in its own narrow class driver
while keeping the retail descriptor bytes, including `usbd_edpt_iso_alloc` for
the RP2040/RP2350 isochronous allocation.

So the answer to "does the firmware actually receive usable audio samples, or
merely advertise a descriptor?" is **it receives them**, and speaker PCM is
already consumed by `ds5_audio_bridge`.

**The companion-facing half does not exist at all.** There is no "send audio to a
companion" path, and per `android-audio-feasibility-2026-08-13.md` there is no
"receive audio from a wireless device" path either.

### 9.5 Microphone uplink (companion mic, adapter, console)

**Not symmetric with the downlink, in three ways (REPO):**

1. The USB **microphone endpoint already exists and is operational, transmitting
   silence** (`switch_pro2.c`, IF4 alt 1, EP `0x83`). Filling it is a smaller
   change than creating it.
2. The **DualSense microphone decode path is explicitly still open** — its own
   PLAN.md item ("DualSense microphone return"), and
   `audio-passthrough-research.md` records both "Microphone decoding/USB return
   remains open" and "the disabled microphone path remains intentional".
3. The genuine Pro Controller 2 microphone data is **not a separate stream**: it
   rides the same `0x002E` extended input notification, at offsets `0x0E` (length,
   `0` or `0x32`) and `0x0F..0x40` (a 50-byte payload). **Its codec and content
   are unknown** — the repository deliberately records that a headphone-only TRS
   plug still produced the `0x07/0x0F` "headset plus microphone" pair and a full
   50-byte high-entropy field, so those values are preserved byte-for-byte and are
   **not** treated as proof of a microphone. Do not guess the codec from the size.

**Ordering conclusion (INFER):** microphone uplink must be the **last** plane
attempted, and should not be designed until the downlink exists.

---

## 10. Genuine Pro Controller 2 wireless audio — what is actually known

**REPO, hardware validated 2026-07-22** (`docs/switch2/pro2-headset-audio.md`).
This is unusually strong evidence: live GATT discovery against PID `0x2069` plus
decrypted HCI captures agree, and the framing was proven against **all 1,846
packet pairs** in the genuine capture.

| Handle | Direction | Purpose |
|---|---|---|
| `0x002C` | host to controller | Speaker Opus packet chunks |
| `0x002E` | controller to host | Extended input, headset/mic audio and motion notifications |
| `0x002F` | host to controller | CCC for `0x002E` |
| `0x0032` | host to controller | Audio/control setup commands |

**Confirmed:**

- **Proprietary GATT, not standard Bluetooth audio.** No A2DP, no LE Audio, no
  ISO. Custom characteristics on the controller's own service.
- **Codec: Opus/CELT**, 48 kHz **stereo**, **96 kbit/s**, CBR, 20 ms /
  960-sample frames, giving **240 bytes per 20 ms**.
- **Framing:** each 240-byte codec packet is split across **two ordered GATT
  writes** — `00 04 78` plus bytes 0..119 (byte 0 is the Opus TOC, `FC`), then
  `00 02 78` plus bytes 120..239 (continuation, no TOC). Reversing the order
  caused 98 decode failures and 1,710 duration mismatches; the correct order
  decoded 960 samples per channel with **zero** failures.
- **Fixed idle packet:** `FC FF FE` followed by 237 zero bytes, split at the same
  boundary.
- The setup command's trailing `F0 00` is the **240-byte codec frame size**, not
  an independent stream size.
- **Disproven, and preserved as negative knowledge:** `0x04` and `0x02` are *not*
  separate speaker and haptic codecs. Treating them that way produced a malformed
  240-byte range-coded packet and the repeatable, severely distorted playback
  heard during development.
- **Pacing:** eight exact idle packets prime the controller; the first half is
  sent about 5 ms before the second; complete frames stay paced at 20 ms; a frame
  leaves the queue only after **both** writes succeed; and a late PCM window sends
  another fixed idle packet while advancing the stateful encoder through matching
  silence.

**Unknown:** the meaning of the 50-byte `0x002E` audio/mic field's contents, and
whether it is a microphone at all.

**Why this matters more than as trivia.** Nintendo solved realtime input,
feedback and headset audio over one proprietary BLE controller relationship, and
this project has **already reimplemented the audio half of it and validated it on
hardware**. The parameters above are therefore not aspirational targets; they are
a working operating point, on this radio, for this codec, at this bitrate. That
does **not** mean copying Nintendo's framing for a companion link — it is designed
for their controller, not for a phone — but it does mean 96 kbit/s Opus over GATT
is demonstrably achievable here.

---

## 11. Quantitative bandwidth budget

### 11.1 The link-layer model, and why it is pessimistic

**EXT with local corroboration.** `raspberrypi/pico-sdk` issue #1465 reports that
with `ENABLE_LE_DATA_LENGTH_EXTENSION` the CYW43439 accepts 251 Tx octets but
reports a **Tx time of 328 microseconds**, which permits only the minimal 27-byte
packets — that is, **DLE is effectively non-functional on this controller**. The
issue was filed 2023-07-29, milestoned 2.2.0, and remains open.

328 microseconds is exactly the air time of one 27-octet encrypted LE Data PDU at
1M PHY (1 preamble + 4 access address + 2 header + 27 payload + 4 MIC + 3 CRC =
41 octets at 8 microseconds each), which corroborates the report rather than
merely quoting it.

So, per one-way application PDU with an empty acknowledgement:

```
328 us (data) + 150 us (T_IFS) + 80 us (empty PDU) + 150 us (T_IFS) = ~708 us
```

**Usable payload is 27 octets per PDU**, minus the 4-byte L2CAP header on the
first fragment of each frame. This is the honest planning number for this
adapter. It is *not* the PHY headline rate and must not be replaced by one.

### 11.2 Per-plane cost

| Plane | Frame on the wire | LL PDUs per frame | Rate | App bytes/s | **Air time** |
|---|---|---|---|---|---|
| CL-IN (controller input) | 30 B + 3 ATT + 4 L2CAP = 37 B | 2 | 125 Hz ceiling | 3,750 | **17.7 %** |
| CL-IN, Windows-shaped (send-on-change, 2/255 epsilon, 100 ms keepalive) | as above | 2 | ~10 Hz idle, ~66 Hz moving | ~2,000 | **~10 %** |
| CL-OUT (feedback) | 7 B + 3 + 4 = 14 B | 1 | on change, 10 Hz or less | 70 | **under 1 %** |
| Management reply, 500 B | 507 B | 19 | bursty, 1/s or less | ~500 | **~1.3 %** |
| **Audio down, raw PCM 48/16/2** | — | — | — | **192,000** | **about 504 %** |
| Audio down, Opus 96 kbit/s (Pro2 parity) | 240 B + 7 = 247 B | 10 | 50 fps | 12,000 | **35.4 %** |
| Audio down, Opus 64 kbit/s | 160 B + 7 = 167 B | 7 | 50 fps | 8,000 | **24.8 %** |
| Mic up, Opus 32 kbit/s mono | 80 B + 7 = 87 B | 4 | 50 fps | 4,000 | **14.2 %** |

**Raw PCM is not marginal, it is impossible.** Even *if* DLE worked (251-octet
PDUs, roughly 2,500 microseconds per exchange) raw PCM would still need about
191 % of wall-clock air time. Only 2M PHY **and** working DLE would bring it near
100 %, which is not a design. **Compression is mandatory, not an optimisation.**

### 11.3 Scenarios

| Scenario | Air time on the companion ACL |
|---|---|
| Controller only | **10 – 18 %** |
| Controller plus management | **11 – 19 %** |
| Controller plus Opus 96k downlink | **45 – 53 %** |
| Controller plus Opus 64k downlink plus Opus 32k mic uplink | **49 – 57 %** |
| Controller plus 96k down plus 32k up plus management | **63 – 71 %** |

**And this is one radio's share.** The same CYW43 is simultaneously running
BR/EDR HID to the source controller, and on Android today a second ACL to the
phone as well. The first two scenarios have comfortable margin. The last does
not, and **must not be committed to on the strength of this table** — it is a
screening estimate whose job is to say which experiments are worth running.

---

## 12. Codecs

### 12.1 What the repository already has

**REPO.** `third_party/opus` is pinned as a submodule (BSD-3-Clause), built with
`OPUS_FIXED_POINT OFF` and `OPUS_ENABLE_FLOAT_API ON`, and its hot code and
tables are **relocated into SRAM** by an `objcopy` step in `CMakeLists.txt`
(`--rename-section .text=.time_critical.opus_text` and friends). Two encoders are
in production use: a direct-CELT path producing the Pro Controller 2's 240-byte
96 kbit/s frames, and a DualSense path at 10 ms / 200 bytes / 160 kbit/s with
`OPUS_SET_COMPLEXITY(0)` and VBR off.

**A codec is therefore not the risk. The board is (§13).**

### 12.2 Comparison, for a companion downlink

| Codec | Bitrate for acceptable stereo game audio | Frame | Algorithmic latency | RP2040 | RP2350 at 300 MHz | Host decode | Licence |
|---|---|---|---|---|---|---|---|
| **Opus/CELT** | 64–96 kbit/s | 10 or 20 ms | ~2.5–6.5 ms plus frame | **No** — the fixed-point/XIP experiment failed hardware playback (REPO) | **Yes, in production** | Android: `MediaCodec audio/opus` or libopus via the NDK; Windows: libopus P/Invoke or Concentus | BSD-3-Clause |
| **LC3** | 48–96 kbit/s, better than Opus at the low end | 7.5 or 10 ms | ~10 ms | Untested; lighter than Opus | Untested | **No public Android `MediaCodec` API found**; LE Audio uses it internally. `google/liblc3` (Apache-2.0) is portable to firmware and host | Apache-2.0 (liblc3) |
| **IMA ADPCM** | 4:1 fixed, so **384 kbit/s** at 48 kHz stereo | per sample | ~0 | Trivial | Trivial | Trivial | Public domain |
| **Raw PCM** | 1,536 kbit/s | — | 0 | — | — | — | — |

**ADPCM does not help.** 384 kbit/s is 48,000 B/s, about 126 % of air time on its
own; it fails for the same reason raw PCM does, just less spectacularly.
**Anything that works here must be a real perceptual codec.**

**Recommendation (INFER): Opus.** It is already vendored, already tuned for this
CPU, already validated at exactly 96 kbit/s on this radio, and decodable on both
hosts. LC3 becomes interesting only if RP2040 support ever becomes a goal, and
§13 says it will not.

### 12.3 Not implemented

No codec work is authorised by this document.

### 12.4 One closed question is reopened, narrowly

[`android-audio-feasibility-2026-08-13.md`](../bluetooth/android-audio-feasibility-2026-08-13.md)
declares Android companion audio **CLOSED — will not be implemented**, on three
constraints. Two are permanent and unchallenged: **WiFi is prohibited**, and
**Bluetooth HID cannot carry audio**. The third is:

> *"No non-root Android transport remains. A2DP/HFP would require the phone to
> act in roles an ordinary foreground app cannot assume..."*

**That enumeration did not consider a binary GATT data plane on the already
bonded management ACL, because Path C did not exist on 2026-08-13.** A GATT
characteristic is a non-root, ordinary-foreground-app transport, and §11 shows a
compressed stream fits with margin.

This does **not** reopen the decision. It corrects the *reason*: Android companion
audio is now blocked by **bandwidth, board and effort**, not by "no transport
exists". That document's concluding observation — "the missing piece there was
never the console side" — remains exactly right, and §9.4 and §9.5 above show
what the missing companion-side piece is. **A one-line correction to that
document is warranted when audio work is next picked up. It is not made here,
because this pass has not measured the transport.**

---

## 13. RP2040 against RP2350 — the resource verdict

**LOCAL**, `arm-none-eabi-size -A` on the checked-in build ELFs this session:

| | **Pico W (RP2040)** | **Pico 2 W (RP2350)** |
|---|---|---|
| `.ram_vector_table` | 192 | 272 |
| `.data` | 8,612 | **129,140** (relocated libopus) |
| `.bss` | 230,624 | 299,824 |
| `.heap` | 2,048 | 2,048 |
| stacks | 2,048 + 2,048 (scratch banks) | 2,048 |
| **SRAM used, main region** | **241,476 / 262,144** | **433,332 / 524,288** |
| **SRAM free** | **about 20.2 KB** | **about 88.8 KB** |
| `.text` plus `.rodata` | about 905 KB of 2 MB flash | about 915 KB of 4 MB flash |

**REPO, and it agrees.** `CMakeLists.txt` gates the whole live-audio path on
`PICO_PLATFORM MATCHES "^rp2350"`; the Pico 2 W runs at **300 MHz** (from a
150 MHz default) with `CYW43_PIO_CLOCK_DIV_INT` recomputed to keep the CYW43 PIO
bus at or below 75 MHz; and `audio-passthrough-research.md` states plainly that
**"Pico W is intentionally non-audio after its fixed-point/XIP experiment failed
hardware playback"**, retaining "its validated non-audio clock, memory layout,
and Bluetooth scheduling."

**Verdicts:**

- **Controller Path C: both boards.** Its entire cost is a 30-byte frame, a
  16-bit sequence, a millisecond timestamp and two ATT handles. Nothing in the
  table above is threatened by it.
- **Audio, either direction: Pico 2 W only.** 20 KB of free SRAM on the RP2040
  cannot hold a codec whose tables alone consume about 120 KB on the RP2350, and
  the RP2040 has no FPU for the float build in use. Settled by measurement and by
  an existing CMake gate, not by clock-speed reasoning.
- **Consequence:** if audio is ever built it must be a **negotiated capability**
  (§18), not a compile-time assumption. A Pico W must answer "no" and a companion
  must accept that gracefully. Do not compromise the controller planes to make
  one board's limitation universal.

**Not measured this session:** CPU headroom on either board, core-1 load, and the
audio jitter-buffer working set. `audio-passthrough-research.md` names the core-1
budget as *"the load-bearing open question — everything else is detail work until
this is answered"*, and it is still open for a **third** concurrent audio stream.

---

## 14. Host-side audio architecture (sketch only)

**Windows.** WASAPI shared-mode render for playback; `AudioGraph` is simpler but
adds buffering. Decode with libopus through P/Invoke, or Concentus; the app is
already .NET 9.

**Android.** `AudioTrack` with `PERFORMANCE_MODE_LOW_LATENCY`, or Oboe/AAudio
with exclusive sharing. Android's own low-latency guidance (EXT) puts a
well-tuned path at about 20 ms and recommends a buffer of twice the burst size.
Capture through `AudioRecord` (`VOICE_COMMUNICATION`) for the microphone plane.
Decode with libopus via the NDK rather than `MediaCodec`, to avoid codec-pipeline
latency on 20 ms frames.

**Shared design points (INFER):**

- Decode on the **companion**, always. The adapter is the constrained end.
- **Jitter buffer of 2 to 3 frames (40–60 ms)** as a starting point, sized from
  the measured connection-interval jitter rather than guessed.
- **Underrun policy: conceal and continue.** Never stall, and never grow the
  buffer to hide a systemic deficit.
- Volume, device selection and reconnect are ordinary app concerns and are out of
  scope for this document.

---

## 15. QoS and scheduling

### 15.1 Priority, derived rather than assumed

The candidate ordering puts controller freshness first, then audio, then
microphone, then feedback, then management. Deriving it from the plane properties
instead:

| Plane | Size | Nature | Cost of one lost or late unit |
|---|---|---|---|
| Controller input | tiny (2 PDUs) | latest-state | **Nothing, if a newer one follows.** Everything, if none does. |
| Audio downlink | large (7–10 PDUs) | continuous, deadline | An audible gap; not recoverable by a later frame |
| Mic uplink | medium | continuous, deadline | As above, remotely |
| Feedback | tiny | edge-ish, change-suppressed | Brief wrong rumble state |
| Management | large, bursty | request/response | Latency only |

That yields a **different and better rule than a strict priority list**:

> **Controller input pre-empts everything but is rate-capped, so it can never
> starve a deadline plane. Audio is *reserved* rather than prioritised. Management
> is best-effort and pre-emptible, and is the only plane that may be made to
> wait.**

Strict priority is wrong for audio. Controller frames are so cheap that giving
them absolute precedence costs audio nothing *at 125 Hz*, but a strict-priority
scheduler with an uncapped controller plane is exactly how a stuck-open sender
would starve audio. The cap is what makes the priority safe — and the existing
send policy already provides that cap, for an unrelated reason.

### 15.2 Mechanism

- **Per-plane queues with per-plane disciplines**, not one shared queue:
  controller is a 1-deep **mailbox** (replace, never append); audio is a bounded
  **ring** with an explicit drop-oldest policy; management keeps the existing
  serialised request/response; feedback is a 1-deep mailbox.
- **One outbound scheduler per companion**, owning the platform's send token.
  §6.2 makes this mandatory on Android and merely tidy on Windows.
- **Reserved opportunity:** after N consecutive controller frames the scheduler
  must offer the slot to the audio ring even when a newer controller frame
  exists. N derived from the measured per-event packet count, not chosen.
- **Management windowing:** a management command is admitted only between
  gameplay frames, and its reply is allowed to take longer during gameplay.
  Windows measured 117 ms during streaming, which is fine.
- **Never fragment a gameplay frame.** The existing `MinimumAttMtu` check
  (measure, then refuse to start) is the right pattern and should be repeated for
  any audio frame size.

### 15.3 What must not happen

Audio starving controller input; management blocking controller input; controller
spam causing audio underruns; BLE queues accumulating stale controller state; a
large JSON reply monopolising the bearer. Each maps to one mechanism above.

---

## 16. Backpressure must be structural

The Windows analog-backlog episode is the governing precedent: **local API
completion is not delivery**, and a queue built out of concurrency is still a
queue.

| Plane | Where backpressure comes from |
|---|---|
| Controller input | **Latest-state mailbox plus at most one in flight.** On Android, additionally the real `onCharacteristicWrite` credit (§6.1) — pace on the callback, never on a timer alone. A failed write is **superseded, never replayed**. |
| Controller feedback | Change suppression at the source; a 1-deep mailbox on the adapter |
| Audio downlink | **Bounded ring with an explicit overrun policy** (drop oldest, and count it). No unbounded OS queue anywhere. If the ring is full the encoder must skip a frame rather than stall the codec task. |
| Mic uplink | Bounded ring, explicit conceal-or-drop, counted |
| Management | The existing serialised request/response is already correct and needs no credit scheme |

**Application-level ACK or credit is required only for audio, and only if
measurement shows the transport credit is insufficient.** Do not build one
speculatively. But do not build a system whose correctness rests on *"we probably
do not send faster than the host drains"* — that sentence is what the analog
backlog was.

---

## 17. Session and security model

The strongest argument for this architecture is not latency. It is that **one
bond, one trusted relationship, one lifecycle** is simpler and safer than two.

```text
  BLE bond (LE Secure Connections, bonded, encrypted)
    |
    +-- trusted management session (config_ble, allowlisted, encrypted)
          |
          +-- negotiated companion capabilities
                |
                +-- plane activation (clink start, audio start, ...)
```

**Invariants (INFER, but each is grounded in a real incident):**

1. **No plane may outlive the session that owns it.** The 2026-09-03 phantom
   source is the lesson: `btstack_host_companion_link_stop()` had exactly one
   caller, the explicit `clink stop`, so a companion that simply died left
   `clink.active` true and left a *selectable arbiter source with nothing behind
   it*. Fixed; the rule must be written down so it is not re-broken.
2. **Every plane re-arms on activation, not on first data.**
   `ns2_companion_link_arm_session()` exists because a replacement client
   inherited a dead client's sequence high-water mark and every frame it sent was
   silently rejected as stale while both ends reported a healthy stream.
3. **Every plane has a stale watchdog keyed on its own traffic**, never on the
   carrier's liveness. Controller: 300 ms, then publish neutral exactly once.
   Audio would need its own (mute, rather than hold the last frame).
4. **No second bond, no second pairing flow, no HID impersonation.** The
   COMPANION source class stays explicit; §3 cites the header contract listing
   exactly what must never be faked and why — source ownership, stale-input
   neutralisation, diagnostics attribution and feedback routing all key on it.
5. **Firmware reboot, capability mismatch and version skew must fail at Start**,
   with a reason, never as garbage data. The MTU gate is the model.

Per-plane lifecycle obligations (app kill, BLE disconnect, reconnect, plane stop,
firmware reboot, capability mismatch, stale session, sequence reset) should be
enumerated in a table **when a second plane is actually built**. Writing it now
for planes that do not exist would be speculative.

---

## 18. Capability negotiation

**Today (REPO):** `info` returns `bridge_contract` and `build`; the `clink` reply
returns the data-plane `version`, `frame_bytes`, `att_mtu`, `min_att_mtu` and
`mtu_ok`. `docs/management/PROTOCOL.md` states explicitly that there is **no
negotiated management protocol version**, and that compatibility is exact command
behaviour plus optional-family probing.

**That is enough for one plane and will not be enough for four.**

Minimum viable shape (INFER — compact and versioned, not a framework):

```json
{"companion": {
   "planes": ["controller.v1", "feedback.v1"],
   "board": "rp2350",
   "audio": null
}}
```

where `audio`, when present, would be something like
`{"codec":"opus","bitrate_max":96000,"frame_bytes":240,"dir":["down"]}`.

Rules:

- **Absent means unsupported.** Older firmware that omits the field is
  `UNVERIFIED`, never "compatible" — the pattern `docs/bridge/PROTOCOL.md`
  already uses for `bridge_contract`.
- **A plane is refused at Start with a reason**, never discovered as garbage.
- **Board capability is explicit** — an `rp2040` cannot offer audio (§13).
- **Android Classic must remain usable while `controller.v1` is absent, or the
  app is older than the firmware.** Non-negotiable during any transition.

Do not add a capability entry for a plane that does not exist.

---

## 19. Separate characteristics against one multiplexed channel

| | **A. One characteristic pair per plane** | **B. One binary uplink and downlink with plane IDs** | **C. Hybrid** |
|---|---|---|---|
| Discoverability | Best — each plane is visible in a capture and in a GATT browser | Worst — opaque bytes | Good |
| Independent subscription | Yes; a companion can take feedback without audio | No; all-or-nothing CCC | Yes for the realtime planes |
| Versioning | Per plane, naturally | One version for everything, or a nested one | Per plane |
| CCC count and ATT DB size | Grows. `MAX_ATT_DB_SIZE 512` in `src/btstack_config.h` is **a real ceiling to check** | Minimal | Moderate |
| ATT scheduling | **No difference** — same bearer either way (§7) | Same | Same |
| Code complexity | Lower: no demux, no plane-ID validation | Higher: a demux is a new place to be wrong | Middle |
| Diagnostics | Best: per-characteristic counters, exactly how `clink` reads today | Needs per-plane counters invented on top | Good |

**Recommendation: A, until an ATT database or handle-count limit forces
otherwise.** The decisive argument is not aesthetics. Per-plane characteristics
give per-plane **counters, subscription and refusal reasons** for free, and the
`clink` diagnostic is the proof of how much that is worth. Check the current
database size against `MAX_ATT_DB_SIZE 512` before adding any plane —
`att_db_util_get_size()` is already printed at startup.

---

## 20. Android migration strategy, if it proves worthwhile

**No flag day. Classic stays production throughout.**

| Phase | Content | Exit condition |
|---|---|---|
| **A** | Adapter-side LE connection-parameter diagnostic (§5.4). No app change, no GATT change. | The negotiated interval, latency and timeout of the Android management link are known, on at least two phones. |
| **B** | Experimental Android Path C carrier: a second `BridgeTransport` implementation, developer flag only, same `BridgeSession`, same canonical payload, same feedback semantics. Port the Windows send policy (2/255 epsilon, send-on-change, 100 ms keepalive). Add the §6.3-A outbound scheduler. | It runs, counters are clean, Classic is untouched and selectable. |
| **C** | **A/B benchmark on the same device** (§21). | Data exists for both carriers. |
| **D** | Long-run soak; management coexistence under gameplay; rumble and reconnect qualification; power. | Parity table complete (§26). |
| **E** | Product decision: keep Classic, make Path C optional, make Path C default, or deprecate Classic later. | — |

**A rollback path must exist at every phase.** The carrier stays a runtime choice
until phase E says otherwise.

**Phase A alone may change the recommendation**, in either direction, and costs
almost nothing.

---

## 21. A/B benchmark methodology

**Hold constant:** the same phone, the same adapter, the same firmware build, the
same console, the same radio environment, the same room, back-to-back runs,
alternating order.

**The input source must be synthetic.** Comparing carriers by moving a physical
Bluetooth controller measures *that controller's* wireless latency and radio
contention, which is precisely the mistake that cost four wrong fixes in the
Windows analog-backlog investigation. Use a deterministic canonical-state
generator inside `BridgeSession` (a square-wave button at a known period, a ramped
stick), so both carriers encode byte-identical sequences.

**Measure, on both carriers:**

| Metric | Where |
|---|---|
| Send cadence: median, p95, p99 gap | companion counters |
| Adapter ingress and applied cadence | `clink` (`frames.received`, `applied`, `max_gap_ms`) for Path C; existing bridge counters for Classic |
| Coalesced or dropped frames | the `ControllerLinkWriter.StatesCoalesced` equivalent |
| Stalls over 50 ms | derived from max gap |
| Controller-state freshness at the adapter | adapter-side timestamp of the last applied frame |
| Management RTT during gameplay against idle | the Windows 117 / 241 ms comparison, repeated |
| Feedback and rumble latency | adapter `outputs.sent` to app receipt |
| Session start and reconnect latency | wall clock |
| CPU and power | Battery Historian or `dumpsys batterystats`, best effort |

**End-to-end latency, honestly.** Do **not** subtract unsynchronised clocks. Two
defensible methods:

1. **Sequence-anchored round trip.** The adapter echoes the sequence number of
   the last applied frame back on CL-OUT (or, for Classic, in the feedback
   report); the companion measures its own send-to-echo interval on one clock.
   That measures companion to adapter to companion, and half of it is the honest
   one-way estimate under a symmetry assumption that must be stated.
2. **External observation.** A camera or photodiode on the console output against
   a logic-analyser probe on the companion's send event. Highest fidelity, most
   setup.

Method 1 requires an adapter change and is therefore **not authorised here**; it
is recorded so the benchmark design does not have to be reinvented.

---

## 22. Audio experiment plan (future, separate)

**Do not combine the Android carrier migration and audio into one patch.**

| Step | Question | Success criterion |
|---|---|---|
| A | Does the adapter capture usable console speaker PCM *for a purpose other than the DualSense bridge*? | 48 kHz stereo frames arrive continuously with a bounded, counted underrun rate |
| B | Can a synthetic compressed payload be generated at the target rate with **no BLE involved**? | Encoder sustains 50 fps with headroom; core-1 budget measured |
| C | Compressed audio over a companion BLE plane to **Windows** | No controller regression: `clink` stale and coalesced counters unchanged within noise |
| D | Decode and play on Windows | Bounded, measured end-to-end audio latency; no unbounded queue |
| E | Controller latency **while** audio streams | §21 metrics unchanged within noise |
| F | Android playback | As C to E |
| G | Microphone uplink | **Only after F.** §9.5 says why it is last. |

**Global success criteria:** no controller regression, bounded audio latency, no
unbounded queue anywhere, tolerable quality, an acceptable and *counted* underrun
rate, and management still usable.

**Windows first, not Android.** Windows already has the qualified Path C
reference and no `mDeviceBusy` complication, so a failure there is a transport or
codec failure rather than a platform-scheduler failure.

---

## 23. The cross-platform architecture question

> Should the long-term model be **companion equals custom companion protocol**
> and **physical controller equals native controller protocol**, rather than every
> companion impersonating a controller wherever the OS makes that possible?

**Yes (INFER, with high confidence).** The evidence is that impersonation is
available on exactly one platform, and by accident:

| Platform | Can it be a Bluetooth HID **Device**? | Evidence |
|---|---|---|
| Android | **Yes** — `BluetoothHidDevice`, one slot per system | REPO, in production |
| Windows | **No.** No user-mode L2CAP server at any PSM; HID PSMs `0x11` and `0x13` reserved even from kernel mode by `bthport.sys!BthIsSystemPSM`; BLE HOGP refused with `0x0B ACL Connection Already Exists` | REPO, two experiment records |
| macOS / iOS | No supported app-level HID Device role | EXT, not investigated here |
| Linux | Yes in principle (BlueZ input profile, or a custom L2CAP server), with privileges | Not investigated |

So "impersonate a controller" is **not a portable architecture**; it is one
platform's convenience. A companion protocol is portable by construction, because
a GATT client is available everywhere a companion could run.

The boundary this creates is also the *correct* one conceptually. A companion is
not a controller: it holds a management relationship, it can be told things, it
renders UI, and it may one day carry audio. Forcing it through a HID descriptor
discards all of that and then re-adds it over a second link.

**But the conclusion is architectural, not operational.** Android Classic works,
is hardware-validated, and carries buttons, sticks, motion, battery, rumble and
player LED today. "The architecture is right" is not a reason to remove a working
carrier before its replacement is measured.

---

## 24. Risk register

| # | Risk | Likelihood | Impact | Detection | Mitigation | Experiment |
|---|---|---|---|---|---|---|
| 1 | **`mDeviceBusy` head-of-line blocking** — a management command with response stalls gameplay for a round trip | **High** (structural, AOSP §6.2) | High | Gameplay gap spikes correlated with management traffic | §6.3-A outbound scheduler; defer management during streaming | Phase B/C |
| 2 | **`Thread.sleep` inside `writeCharacteristic`** — up to 50 ms blocking on the caller's thread | High if unaddressed | High | Sender-thread stalls | Dedicated sender thread, never the main thread; treat `ERROR_GATT_WRITE_REQUEST_BUSY` as a signal, not a retry loop | Phase B |
| 3 | **OEM connection-parameter variance** — an overlay overrides the AOSP 9/12 defaults | Medium | High | §5.4 diagnostic | Refuse or warn below a threshold interval; keep Classic available | **Phase A** |
| 4 | **Radio contention on the adapter** — companion ACL plus BR/EDR HID to the source controller on one CYW43 | Medium | High | `clink max_gap_ms`; source-controller report gaps | Rate cap; a wired source controller is an independent cure, already proven on Windows | Phase C/D |
| 5 | **No working DLE on CYW43439** — roughly 5x less payload per PDU than the spec allows | **Confirmed present** (EXT, issue open) | Medium for controller, **High for audio** | Air-time budget §11 | Design to 27-octet PDUs; treat any DLE fix as upside | — |
| 6 | **2M PHY availability unknown** on CYW43439 | Unknown | High **upside**, no downside | `gap_set_connection_phys` plus the §5.4 diagnostic extended to PHY | If available, every audio number in §11 halves | Cheap add-on to Phase A |
| 7 | **Audio starves the controller plane** | Medium if unscheduled | High | §21 metrics with audio on | §15 reserved-opportunity scheduler | Step E of §22 |
| 8 | **RP2040 cannot host audio** | **Confirmed** (§13) | Medium | — | Capability negotiation (§18); never a compile-time universal assumption | — |
| 9 | **Core-1 budget** — a third concurrent audio stream on the core already running BTstack plus codec | Medium | High | Core-1 timing counters | Named as the load-bearing open question since 2026-07 | Step B of §22 |
| 10 | **Switch 2 audio initialisation unknowns** for a non-DualSense consumer | Medium | Medium | UAC1 alt-setting and Feature Unit traces | The UAC1 path is validated; the *consumer* is what is new | Step A of §22 |
| 11 | **Stale plane ownership** (the phantom-source class of bug) | Medium | High | Arbiter source list against plane `active` flags | §17 invariants 1 to 3; a regression test per plane | Any new plane |
| 12 | **Protocol-version drift** across firmware and app | Medium | Medium | `info` plus per-plane version, §18 | Refuse at Start with a reason; follow the descriptor-digest precedent | Any new plane |
| 13 | **Power and battery** on the phone — an 11.25 ms interval held during gameplay | Medium | Medium | Battery stats | Drop to `BALANCED` when not streaming — **already done** | Phase D |
| 14 | **Two `BluetoothGatt` objects to one device** do not behave as hoped | Medium | Low (it is an optimisation) | Counter comparison | Fall back to §6.3-A | Phase C, optional |
| 15 | **The genuine `0x002E` 50-byte field is unexplained** and may not be a microphone | High | Low (affects only a copy-Nintendo design) | — | Do not design the mic plane from the genuine controller's framing | Step G of §22 |

---

## 25. What would keep Android on Classic — permanently

Stated before the data arrives, so the conclusion is not fitted to it. **Any
one** of these is sufficient:

1. **The negotiated interval cannot be held low.** If the measured Android
   management interval under `CONNECTION_PRIORITY_HIGH` is **30 ms or more** on
   ordinary devices, or drifts back up during gameplay, Path C cannot match the
   Classic interrupt channel and the case is closed.
2. **p95/p99 gap is materially worse** than Classic on the same device — not the
   median, the tail. A carrier that is usually fine and occasionally 60 ms late
   is worse than one that is uniformly 8 ms.
3. **The `mDeviceBusy` contention proves structurally uncontrollable** — even
   with a dedicated scheduler, management traffic reliably produces gameplay gaps
   a player can feel.
4. **Management is degraded during gameplay on Android**, unlike Windows, badly
   enough to make the companion UI unusable while playing.
5. **Power is materially worse** — a sustained 11.25 ms interval costing enough
   battery to shorten a session noticeably.
6. **Feedback is less reliable** over CL-OUT than over the Classic interrupt and
   `SET_REPORT` pair.
7. **Audio never happens**, or lands on a design better served by a dedicated
   link — in which case the single-ACL argument loses most of its forward-looking
   value and Classic wins on proven behaviour alone.

**If the data says Classic is better, this document's recommendation is wrong and
the answer is to say so.** Reason 1 of the 2026-09-03 record — that the stated
motivation is already delivered — stands regardless, which means there is no cost
to concluding that.

---

## 26. What would justify deprecating Classic

**All** of the following, measured, on at least two Android devices:

1. Path C controller latency **at least as good as** Classic — median *and* p95
   *and* p99.
2. No increased jitter or stalls under a 30-minute soak.
3. No management regression: the Windows 117/241 ms comparison reproduced in
   spirit on Android.
4. Feedback parity: rumble and player LED, including start, stop and change.
5. Motion and battery parity through the same canonical payload.
6. Reconnect parity, including app kill, BLE drop, and an adapter reflash.
7. Power no worse in a measured session.
8. A clean one-session lifecycle: no plane outlives its management session, in
   every teardown path.
9. A demonstrated architectural benefit beyond tidiness — in practice, **a
   working second plane**.

**And even then, deprecation is a separate decision from defaulting.** Ship Path
C as the default with Classic retained and selectable for at least one release
before removing anything.

---

## 27. Recommended staged roadmap

```text
NOW --> (A) Adapter LE-parameter diagnostic        one firmware file, read-only
            answers 5.3 permanently, both companions, any phone
         |
         +--> interval >= 30 ms, or OEM-variable --> STOP. Record. Keep Classic. (25.1)
         |
         +--> interval 11.25-15 ms confirmed
              |
              v
        (B) Experimental Android Path C carrier    developer flag, Classic untouched
            + outbound scheduler (6.3-A)
            + Windows send policy ported
              |
              v
        (C) A/B benchmark, synthetic source (21)
              |
              +--> worse --> record the negative, keep Classic, done
              |
              +--> parity or better
                   |
                   v
        (D) Soak, coexistence, feedback, reconnect, power (26)
                   |
                   v
        (E) Default Path C, retain Classic for at least one release

        - - - separately, and only on Pico 2 W - - -

        (F) Audio downlink prototype, Windows first (22 A-E)
                   |
                   v
        (G) Android audio playback  -->  (H) microphone uplink
```

**Nothing past (A) is accepted work.** (A) is a recommendation, not an
authorisation.

---

## 28. Exact implementation boundaries that remain UNAUTHORIZED

This pass produced **documentation only**. The following are explicitly **not**
authorised by it and must be requested separately:

- Any Android Path C client, prototype, flag or `BridgeTransport` implementation.
- Any removal, deprecation or de-prioritisation of the Android Classic carrier.
- Any GATT schema change: no new characteristic, no property change, no UUID.
- Any firmware behaviour change, **including the §5.4 diagnostic**, which is
  specified so it need not be re-derived, not approved.
- Any audio work of any kind: no capture path, no codec, no transport, no host
  playback, no microphone.
- Any change to the Windows Controller Link implementation.
- Any change to `tools/fixtures/management/protocol-v1.json`, the bridge
  contract, or the descriptor.
- Any capability-negotiation field (§18) — the shape is a proposal.
- The one-line correction to
  `docs/bluetooth/android-audio-feasibility-2026-08-13.md` identified in §12.4.

---

## 29. Open questions, ranked by how much they would move the decision

1. **What interval does Android actually negotiate, on real devices?** (§5.3)
   Settled by the §5.4 diagnostic. **Highest value per unit of effort in this
   entire document.**
2. **Does the §6.3-A scheduler make `mDeviceBusy` a non-issue in practice?** Only
   a prototype answers this.
3. **Does the CYW43439 support 2M PHY?** Every audio number in §11 halves if so.
4. **Core-1 budget for a third audio stream.** Open since 2026-07 and still the
   load-bearing question for audio.
5. **Do two `BluetoothGatt` objects to one device give independent send tokens?**
   An optimisation, not a gate.
6. **What is in the genuine controller's 50-byte `0x002E` audio field?**
   Interesting; affects nothing on the companion path.

---

## Sources

Repository sources are cited inline by path. External sources consulted this
session:

- AOSP `packages/modules/Bluetooth`: `system/stack/gatt/gatt_cl.cc`,
  `framework/java/android/bluetooth/BluetoothGatt.java`,
  `android/app/src/com/android/bluetooth/gatt/GattService.java`,
  `android/app/src/com/android/bluetooth/btservice/CompanionManager.java`,
  `android/app/res/values/config.xml`
- [Android BLE: The Ultimate Guide To Bluetooth Low Energy](https://punchthrough.com/android-ble-guide/)
- [Write Requests vs. Write Commands](https://punchthrough.com/ble-write-requests-vs-write-commands/) (contradicted on the callback question by the AOSP source above)
- [Maximizing BLE Throughput on iOS and Android](https://punchthrough.com/maximizing-ble-throughput-on-ios-and-android/)
- [pico-sdk issue #1465 — LE Data Length Extensions not working on cyw43439](https://github.com/raspberrypi/pico-sdk/issues/1465)
- [Android `BluetoothDevice` reference (`createL2capChannel`)](https://developer.android.com/reference/android/bluetooth/BluetoothDevice)
- [Android low-latency audio guidance](https://developer.android.com/games/sdk/oboe/low-latency-audio)
- [google/liblc3](https://github.com/google/liblc3)
