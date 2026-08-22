# Controller Link cycling failure — 2026-08-22

Status: **root cause Confirmed, and it is not in PicoSwitch2.** The tablet's Bluetooth becomes
unavailable by **two independent routes**: an abort inside Android's own Gd HCI layer (process
dies), and the controller becoming unavailable beneath a surviving host — mechanism **Unknown**,
and deliberately not called a sleep/wake stall. Both present identically to the user
because one radio serves both transports. Adapter-side changes in this pass add margin against the
second only; nothing here removes either fault.

## Question

The Android Controller Link normally connects, but repeatedly running the full lifecycle
(management connect → Touch Gamepad → exit → stop Controller Link → end management) eventually
produces a failure in which Controller Link will not connect, the management session drops at the
same moment, the app shows `0x85`, and everything recovers on its own without rebooting the
adapter. What fails?

## Environment

- Adapter: Pico 2 W, firmware profile controller `2.1.4` / bluetooth `12.0.0` / dsp `0.2.3`,
  personality `pro2`. The build under test is the **currently flashed release build**; none of this
  pass's uncommitted changes were on it.
- Tablet: onn 8 Core (`onn8Core`), Qualcomm Bluetooth, app `dev.picoswitch.companion.debug` 2.0.0-debug.
- Adapter Classic BD_ADDR ends `77:78` (`name:PicoSwitch2` in Fluoride logs).
- Access: UART on COM11, ADB over TCP. No physical interaction, no reflash.

## Method

Two independent bodies of evidence:

1. **Maintainer's manual run** (~10 min of deliberate cycling) plus the retained `logcat` buffer
   covering it.
2. **Automated reproduction**: 10 scripted lifecycle cycles driven entirely over ADB
   (`am force-stop` to end management → relaunch → `Reconnect` → `Touch Gamepad` → poll UART
   `btlife` until `classic_ready > 0` → `BACK` → `Stop playing`), with human-scale pacing and
   per-cycle UART `btlife` / `bthealth` snapshots. `logcat` was cleared first, so the capture
   covers exactly these cycles.

## Results

### Automated reproduction

| cycle | link established | `reject_window` | `disc.hci` | `disc.last_reason` | `control_tick_max_gap_ms` |
|-------|------------------|-----------------|-----------|--------------------|---------------------------|
| 1 | 6.1 s | 1 | 1 | `0x13` | 851 |
| 2 | 6.1 s | 1 | 2 | `0x13` | 851 |
| **3** | **failed (>25 s)** | 1 | 4 | **`0x08`** | 851 |
| **4** | **failed (>25 s)** | 1 | 4 | **`0x08`** | 851 |
| 5 | 6.3 s | 1 | 4 | `0x08` | 851 |
| 6–10 | 6.1–6.3 s | 1 | 5–9 | `0x13` | 851 |

The failure reproduced on cycle 3 of 10, matching the maintainer's "4–5 cycles".

### Second campaign — 25 cycles, and the dominant failure is neither A nor B

A longer run (25 cycles, same driver and pacing) failed **10 times (40 %)** and changed the
picture. Only **one** failure involved a Bluetooth process crash. Classified from the tablet's logs:

| count | mode | signature |
|-------|------|-----------|
| **8** | **Type C — encryption LMP collision** | `btm_sec_encryption_change_evt: Encryption failure 35` → `HCI_ERR_LMP_ERR_TRANS_COLLISION` → `disconnect_acl ... Encryption Failure` |
| 1 | Type A — host-stack abort | PID change; the crash at 15:21:28 |
| 1 | **Mode 1 — pre-ACL** | `OnConnectFail: ... reason:PAGE_TIMEOUT(0x04)` after 5.18 s |
| **0** | Mode 2 — admission | `reject_window` never moved, in this run or the first |

So across **35 total cycles**: Mode 2 never occurred, Mode 1 occurred **once**, and the failure the
maintainer actually experiences is overwhelmingly **Type C**.

#### Type C in detail

The ACL succeeds — this is *not* a paging or admission problem:

```
15:31:25.807 acl.cc: CreateClassicConnection ... remote:...77:78
15:31:26.868 acl.cc: OnConnectSuccess ... handle:5 initiator:local      <- ACL up in 1.06 s
15:31:26.871 btm_acl_role_changed: ... new_role:peripheral              <- adapter is master
15:31:26.871 transmit_command: AUTHENTICATION_REQUESTED(0x0411)
15:31:26.880 transmit_command: LINK_KEY_REQUEST_REPLY(0x040b)
15:31:26.915 btm_sec_auth_complete: status: 0                           <- AUTH SUCCEEDS
15:31:26.915 transmit_command: SET_CONNECTION_ENCRYPTION(0x0413)
15:31:26.922 btm_sec_encryption_change_evt: Encryption failure 35, disconnecting 5
15:31:26.922 btm_sec_encrypt_change: Encryption collision failed
             status:HCI_ERR_LMP_ERR_TRANS_COLLISION
15:31:26.922 disconnect_acl: ... comment: ... Encryption Failure
15:31:33.806 PicoSwitch: transport/HID connection: callback timeout after 8001ms
15:31:47.156 PicoSwitch: transport/host disconnected: elapsedMs=21324
```

Authentication **succeeds** — the stored Classic link key is fine, which independently confirms the
trust state is healthy. Encryption then fails with `HCI_ERR_LMP_ERR_TRANS_COLLISION` (0x23 = 35),
Android tears the ACL down, and the app sits out its ~21 s establishment budget.

A differential against a successful cycle shows identical structure and one telling difference:

| | success (15:30:50) | failure (15:31:26) |
|---|---|---|
| auth complete → encryption result | **47 ms**, `HCI_SUCCESS`, `key_size:16` | **7 ms**, collision |
| remote-features read | completes 7 ms after encryption starts | completes 3 ms **after the collision** |

The 7 ms rejection is the controller refusing immediately because another LMP transaction was in
flight. Android's own log flags the overlap in both cases
(`acl_peer_supports_sniff_subrating: Checking remote features but remote feature read is
incomplete`, and the `TIP: Maybe wait until read feature complete beforehand` seen in Type A).

#### Type C root cause: both hosts start the encryption procedure

Resolved by elimination on the Android side, then located in source on ours.

**Android is not the variable.** Across all 8 failures and 11 successes the Android-side HCI
sequence is *structurally identical* — same commands, same relative timings
(`role_change`, `WRITE_LINK_POLICY_SETTINGS`, `CHANGE_CONNECTION_PACKET_TYPE`,
`AUTHENTICATION_REQUESTED`, `LINK_KEY_REQUEST_REPLY`, `auth_complete`, `SET_CONNECTION_ENCRYPTION`).
Specifically eliminated:

- **Sniff / power-mode.** No `mode_change`, `bta_dm_pm_sniff` or `btm_pm_snd_md_req` appears in any
  collision window, success or failure. The sniff-policy hypothesis is **dead**; do not revive it.
- **Packet-type change.** `CHANGE_CONNECTION_PACKET_TYPE` is outstanding at encryption time in
  successes too, with the same ~45–50 ms gap. Not the discriminator.
- **Duplicate connects / app-driven security.** 26 app requests produced exactly 26
  `L2CA_ConnectReq` and 26 `CreateClassicConnection` — strictly 1:1 — and zero `createBond` /
  `setPairingConfirmation` / `removeBond` calls. The uncommitted Android establishment work is
  **exonerated**.

Since Android's behaviour is constant, the variable is ours. In pinned BTstack:

```c
// hci.c:4227  HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT
if (status == 0) {
    conn->authentication_flags |= AUTH_FLAG_CONNECTION_AUTHENTICATED;
    if ((conn->authentication_flags & AUTH_FLAG_CONNECTION_ENCRYPTED) == 0){
        conn->bonding_flags |= BONDING_SEND_ENCRYPTION_REQUEST;   // -> hci.c:7472
```

This is **unconditional**. It does not consider who initiated authentication, nor whether this host
requires encryption (our HID Host registers at `LEVEL_0` and requires none). Both controllers report
Authentication Complete for the same LMP authentication, so **both hosts then start the LMP
encryption procedure** — which is exactly what `LMP Error Transaction Collision` means, as opposed
to a generic "radio busy". Whichever request loses the race is rejected; the 7 ms-vs-47 ms split is
the race outcome, and our side losing/winning depends on whether `hci_run()` had a free command slot.

**Upstream master behaves identically** (`hci.c:5046`), so there is no upstream fix to backport, and
this is not a reason to revisit the SDK 2.3.0 decision.

#### Strategy choice

Three production strategies were compared in source before touching the implementation again.

**Strategy C — make the collision non-fatal — is unnecessary: BTstack already is.**
`HCI_OPCODE_HCI_SET_CONNECTION_ENCRYPTION` does not appear in
`handle_command_status_event()` at all, so a `0x23` command status falls through `default:` and is
silently dropped — no retry, no disconnect, no GAP propagation. The Encryption-Change failure path
(`hci.c:4205`) only clears dedicated bonding and drops to security level 0. **Android is the fatal
actor**: it calls `disconnect_acl(... "Encryption Failure")`. We cannot change Android, so the fix
must be to not create the collision.

**Strategy B — defer with a local fallback — was rejected as unfounded.** A fallback would need a
principled trigger, and none exists without physical evidence: BTstack exposes no "peer encryption
never happened" event, and inventing a timer is exactly the arbitrary-delay fix this investigation
has avoided throughout. The pending uncertainty is made *observable* instead (see instrumentation).
Note also that the pre-change behaviour already left links unencrypted after a collision — BTstack
never retried — so deferring introduces no new exposure.

**Strategy A — stand down for the trusted live companion — chosen**, because the peer-led path is
provably equivalent in BTstack:

```
HCI_EVENT_ENCRYPTION_CHANGE (hci.c:4135)   <- keyed on handle only, initiator-agnostic
  -> BIAS Secure-Connections downgrade check
  -> AUTH_FLAG_CONNECTION_AUTHENTICATED
  -> BONDING_SEND_READ_ENCRYPTION_KEY_SIZE
       -> hci_handle_read_encryption_key_size_complete()  (hci.c:2750)
            -> AUTH_FLAG_CONNECTION_ENCRYPTED, conn->encryption_key_size
            -> hci_handle_mutual_authentication_completed() -> GAP_EVENT_SECURITY_LEVEL
```

Encryption state, key size, BIAS protection and the security-level event are populated identically
whether we or the peer initiated. Nothing downstream can tell the difference.

#### The Classic encryption invariant

Standing down must not convert "occasionally fails while encrypting" into "connects but stays
unencrypted". It cannot, and this is **Confirmed from the capture** rather than argued:

```
15:30:50.324 l2c_csm: LCID 0x006d st: ORIG_W4_SEC_COMP  psm: BT_PSM_HIDC(0x0011)
15:30:50.382 btm_sec_encrypt_change: HCI_SUCCESS ... key_size:16
15:30:50.383 l2c_csm: ... evt: L2CEVT_SEC_COMP(0x7)
             Exit chnl_state=CST_W4_L2CAP_CONNECT_RSP
```

Android's HID control channel waits in `CST_ORIG_W4_SEC_COMP` — *originator waiting for security
complete* — and only advances on `L2CEVT_SEC_COMP`. Its HID Device profile requires encryption
(`security:0x30b6`), so **the peer's own L2CAP state machine blocks HID establishment until Classic
encryption is up**. Our HID Host being `LEVEL_0` does not weaken this, because the peer is the
originator here. Gating bridge activation on the security transition would therefore be redundant
with the peer's CSM, and was not added.

Because that is a peer guarantee rather than one we enforce, it is **verified rather than assumed**:
`enc.unencrypted_active` counts any companion HID-ready with `gap_encryption_key_size() == 0`. It
should always read 0; a non-zero value falsifies the paragraph above.

#### The fix

`hci.c:4708` forwards the event to the application ("notify upper stack") *after* setting the flag
and *before* `hci_run()` consumes it, so the application handler sits exactly in the gap.
`BONDING_SEND_ENCRYPTION_REQUEST`, `bonding_flags` and `hci_connection_for_handle()` are all public
in `hci.h`, so this needs **no BTstack patch** — which matters, because BTstack ships inside the
pinned SDK.

On Authentication Complete our handler now clears that flag, but only when
`ns2_bt_defer_classic_encryption()` allows: the peer must be the companion holding a live encrypted
management session, **and** this host must not have requested security on that link itself.
Controllers are untouched, and a link we asked to authenticate stays ours to encrypt — so no link
silently loses encryption it would otherwise have had.

The BTstack coupling is confined to one named helper,
`btstack_host_stand_down_from_encryption()`, guarded by two `_Static_assert`s: one on
`BONDING_SEND_ENCRYPTION_REQUEST == 0x2000` and one pinning BTstack 1.6.2. A future SDK bump
therefore **fails the build** rather than silently turning the race fix into a no-op — which
matters, because upstream master still behaves the same way, so a newer BTstack is not
automatically a fix.

Per-handle state (`classic_security_requested_handle`, `classic_encryption_deferred_handle`) is
cleared on HCI disconnect, since handles are reused. Both are fail-safe if missed: the effect is
simply that the next link does not defer, i.e. it reverts to stock BTstack behaviour.

#### What the next flash will tell us

Type C is otherwise **completely unobservable** on a flashed build — no HCI trace, `printf` does not
reach the UART diag channel, the `btlife` ring records no security events, and the tablet is a
production build with no root for btsnoop. `btstate` now reports:

| counter | meaning | expected |
|---|---|---|
| `enc.deferrals` | we stood down for the companion | **> 0** — proves the second request was real |
| `enc.peer_completed` | peer-led encryption came up on the deferral handle | **≈ deferrals** |
| `enc.collisions` | `0x23` seen on our side | **0** after the fix |
| `enc.unencrypted_active` | companion HID ready with no Classic encryption | **0** — invariant tripwire |

`deferrals > 0` with `collisions == 0` and `peer_completed ≈ deferrals` confirms the mechanism *and*
the fix in a single run. `deferrals == 0` would mean the stand-down never fired and the model is
wrong — which is exactly the falsification this is built to allow.

### What the reproduction rules out

- **Not admission (Mode 2).** `admission.reject_window` stayed at **1** across all ten cycles,
  including both failures — and across all 25 cycles of the second campaign too. In **35 cycles**
  the adapter never once rejected the tablet.
- **Not core-1 starvation.** `core1.control_tick_max_gap_ms` stayed at **851 ms** — its
  pre-existing high-water mark — and never moved. Nothing approached a supervision timeout.
- **Not the bounded HCI/CYW43 recovery.** `hci.recovery.attempts = 0`, `reboot.requests = 0`
  for the whole session. That path has still never fired.
- **HCI itself was healthy**: `probes {sent:443, ok:443, failed:0, timeouts:0}`.

Successful cycles ended `0x13` (`REMOTE_USER_TERMINATED_CONNECTION`, the normal app-driven
teardown). Failing cycles ended `0x08` (`CONNECTION_TIMEOUT`).

### Cause, from the tablet's own logs

**There are two distinct tablet-side failures, not one.** They must not be merged: one is a radio
power-mode stall with the Bluetooth process alive throughout, the other is a host-stack crash.

#### Failure type A — reproduced 14:18:00: Android's Bluetooth stack aborts

```
14:17:56.287 acl.cc: CreateClassicConnection: Connection initiated for classic to ...77:78
14:17:59.104 acl.cc: OnConnectSuccess: ... handle:11 initiator:local
14:17:59.105 btm_acl_created: ... role:peripheral            <- Android peripheral, adapter master
14:17:59.105 btm_acl: change_connection_packet_types: Unable to include remote supported
                      packet types as read feature incomplete
14:17:59.105 btm_acl: TIP: Maybe wait until read feature complete beforehand
14:17:59.106 btm_sec: btm_sec_l2cap_access_req: is_originator:true, psm=0x0011
14:17:59.106 btm_sec: btm_sec_check_upgrade: verify whether the link key should be upgraded
14:17:59.129 libc: Fatal signal 6 (SIGABRT) in tid 2843 (gd_stack_thread), pid 2815
14:18:00.110 DEBUG: Abort message: 'system/gd/hci/hci_layer.cc:255 handle_command_response:
                    Waiting for READ_REMOTE_SUPPORTED_FEATURES(0x041b),
                    got LINK_KEY_REQUEST_REPLY(0x040b)'
                    #04 libbluetooth_jni.so (HciLayer::impl::on_hci_event(...)+12172)
14:18:00.210 ActivityManager: Process com.android.bluetooth (pid 2815) has died: psvc PER
14:18:00.210 BluetoothSystemServer: ... requested to [Disable]. Reason is CRASH
```

This is a **native assertion failure inside Android's own Gd HCI layer**. It asserts that a command
completion matches the command it is waiting for, received `LINK_KEY_REQUEST_REPLY` while waiting
for `READ_REMOTE_SUPPORTED_FEATURES`, and called `log::fatal`. Android's own code warns about this
window 24 ms earlier ("*Maybe wait until read feature complete beforehand*").

Android was the **initiator** of both the ACL and the L2CAP connection
(`initiator:local`, `is_originator:true, psm=0x0011`), so the authentication that produced the
link-key request was started by Android's own outgoing security requirement. **No adapter behaviour
was identified that provokes it**, and the firmware deliberately does *not* request early
authentication for `hid_host_connect` peers (see the CYW43/DS4-clone note at the
`gap_request_security_level` call site). Treat this as an Android/Fluoride defect.

The Qualcomm `WakeRetransTimeout` at 14:17:52 and `soc_need_reload_patch=1` at 14:18:00.205 are HAL
**cleanup after the host process died**, not the cause. An earlier revision of this document had
that causality backwards.

The Bluetooth PID changed 2815 → 28621. The app then logged
`transport/HID profile: service disconnected` followed by five
`management/error: background: android.os.DeadObjectException` over the next 25 s. Cycle 4 failed
immediately afterwards (`HID connection rejected: elapsedMs=5222`) while the stack was still
re-initialising — a consequence of the crash, not an independent fault.

#### Failure type B — production 13:00:01: controller reports both links lost, no crash

**There is no abort and no process death anywhere in the log covering this failure.** The Bluetooth
process survived.

```
12:59:52.752 management/result: input: complete    <- last healthy GATT round trip (103 ms)
12:59:57.769 management/command.write: seq=2258    <- write sent, never answered
12:59:58.791 ibs_handler: DeviceSleep: TX Awake, Sending SLEEP_IND
12:59:58.791 ibs_handler: SerialClockVote: vote for UART CLK OFF
             ... 2.35 s of UART idle ...
13:00:01.141 ibs_handler: ProcessIbsCmd: Received IBS_WAKE_IND: 0xFD   <- chip wakes the host
13:00:01.145 bt_shim_hci: disconnection ... handle: 0x07, reason: 0x08
13:00:01.148 bt_shim_hci: disconnection ... handle: 0x06, reason: 0x08
```

Handle `0x06` was the LE management link and `0x07` the Classic HID link. Both were reported lost
with HCI reason `0x08`, and the chip raised the wake itself in order to report them.

**Do not read the 2.35 s as an outage.** It is a host↔chip **IBS UART sleep window**, and that is
routine: in the ten healthy minutes before this failure the same log contains **124 sleep cycles
alongside 113 successful GATT round trips**. Sleeping with live ACLs is normal and harmless — the
chip maintains the links autonomously while the UART is idle. All the window tells us is *when the
host learned*, not when the radio lost anything.

An earlier revision of this document treated 2.35 s as a measured stall and used "2.35 s < 6 s" to
justify the supervision-timeout change. That inference was wrong and has been withdrawn; see
*Confidence*.

What the logs **do** establish: the tablet's controller declared both ACLs timed out, the host was
told on the next wake, and its Bluetooth process never crashed. What they do **not** establish: how
long the radio-level outage actually was, or which side stopped responding first. A separate
wake-retransmit storm at 12:07:25 (dozens of `WakeRetransTimeout` at 10 ms intervals) shows this
device's wake path failing on other occasions, but that is a different moment and is not evidence
about this one.

### `0x85`

`0x85` is **our own label**, not a raw platform code. The app logs
`management/error: input: Bluetooth write failed (GATT status=0x85 ANDROID_GATT_ERROR)` for Android
GATT `status=133`. Established from our own log line paired with `management/command.write:
... status=133` on the same write — not from the `0x85 == 133` coincidence.

It is a **downstream consequence**: the GATT write was already in flight when the peer's stack
went away. Note that Fluoride *also* prints an unrelated `0x85` constantly
(`bta_dm_pm.cc: Current power mode:UNKNOWN[0x85]`, a BTM power-mode state during routine sniff
negotiation); that one is noise and must not be confused with ours.

## Interpretation

The tablet has two independent ways to take the Controller Link down, and both present identically
to the user. One radio serves both transports, so either takes management with it.

| | Type A (14:18) | Type B (13:00) |
|---|---|---|
| Bluetooth process | **dies** (SIGABRT, PID 2815→28621) | **survives** |
| Native abort | `hci_layer.cc:255` assertion | none in the entire log |
| Layer | Android Gd host stack | controller/radio, below the surviving host |
| Trigger seen | Android's own command-response ordering during its outgoing L2CAP security | none identified; controller simply reported both ACLs timed out |
| Adapter involvement | none identified | none identified |
| Recovery | stack restart (see below) | link re-establishment only |

What is common to both is only the *outcome*: the tablet's Bluetooth becomes unavailable, both ACLs
go, and the app surfaces `0x85`. That is as far as a shared explanation may be taken.

For Type B the honest ceiling is **"the tablet's Bluetooth controller became unavailable"**. Calling
it a sleep/wake fault reads more into the trace than it supports — the sleep window is routine, and
the Classic link's 20 s margin argues against a brief stall. The mechanism below the host is
**Unknown**.

Both are consistent with every symptom the maintainer reported: majority of cycles fine,
simultaneous management + Controller Link loss, `0x85`, self-recovery, and **no adapter reboot ever
required** — the adapter was never in a bad state.

### Where the ~30–40 s recovery actually goes

Measured from the Type A crash. The controller outage is the *smallest* term:

| T+ | event |
|----|-------|
| 0.00 s | `Process com.android.bluetooth (pid 2815) has died` |
| 0.52 s | `AdapterService` rebound in the new process |
| 1.12 s | vendor `patch_dl_manager` opens `cmbtfw13.tlv` |
| 1.43 s | **`Firmware download succeded.`** — controller re-patched |
| 1.91 s | `Bluetooth state changed: STATE_ON` |
| 1.99 s | app: `HID connection state: connecting` (immediate retry) |
| 10.0 s | app: `HID connection: callback timeout after 8006ms` |
| **21.9 s** | app: `transport/host disconnected: elapsedMs=20025` — the doomed attempt ends |
| 44.3 s | next attempt begins (script pacing; a human retrying by hand) |
| 46.5 s | `relationship/connect.verified` — management healthy again, 2.2 s after asking |

So the decomposition is:

1. **~2 s — genuine Bluetooth controller/stack outage.** Process restart *and* a full controller
   firmware patch re-download complete inside two seconds. This is not what the user waits for.
2. **~20 s — the app's own HID establishment attempt failing** against a stack that has only just
   returned. This is the dominant term and it is entirely app-side budget.
3. **remainder — retry latency**, i.e. how soon anyone asks again. Management itself reconnects in
   ~2.2 s once asked.

The user-visible "wait ~30 seconds and try again" is therefore **not** the tablet being unavailable.
It is one doomed 20 s attempt plus the delay before the next one.

Note this does **not** reduce to "retry sooner": cycle 4 retried ~58 s after the crash and was still
rejected in 5.2 s. Shortening or backing off the app's attempt window is a plausible improvement but
is **not** demonstrated by this data, so nothing was changed. Recorded as a candidate, not a fix.

### Classic supervision timeout is not the Classic drop's explanation

Traced from source: `hci.c:5010` initialises `link_supervision_timeout` to
`HCI_LINK_SUPERVISION_TIMEOUT_DEFAULT` (`0x7D00` = 32000 × 0.625 ms = **20 s**); this firmware never
calls `gap_set_link_supervision_timeout()`; and `hci.c:3859` queues
`GAP_CONNECTION_TASK_WRITE_SUPERVISION_TIMEOUT` only when the value *differs* from that default. So
no `Write_Link_Supervision_Timeout` is ever sent and the controller keeps its own default, also 20 s.

This is the most awkward fact in the Type B account, and it should stay visible rather than be
smoothed over: the Classic link had **20 s** of margin and still reported `0x08`. The last healthy
GATT round trip was only ~8.4 s before the disconnects, so a 20 s Classic supervision timeout should
not have expired in that window at all. Either the outage began earlier than any log line shows, or
the Classic loss was **not** a supervision timeout but a consequence of the controller tearing its
links down. The evidence does not choose between those, and neither should this document.

**Do not lengthen the Classic timeout.** At 20 s it is already far outside anything observed, and
nothing here suggests a longer value would have changed the outcome.

## Adapter-side response

We cannot fix the tablet's firmware. The adapter is the LE peripheral on the management link and
previously requested **nothing**, so Android's chosen supervision timeout alone decided how long a
peer stall had to be to kill the session.

Two things justify asking for margin, and note that the captured 2.35 s figure is **not** one of
them (see Type B — it is a UART sleep window, not an outage):

1. **An unexplained asymmetry.** The Classic link carries a 20 s supervision timeout while the LE
   management link runs on whatever the phone picked, typically ~2 s. Nothing in this project chose
   that asymmetry or defends it.
2. **Independent lineage evidence.** JoypadOS hit link loss under single-radio LE+Classic
   coexistence and fixed it by moving exactly this parameter from 2 s to 6 s (`efa0202`), reporting
   that the link then "rides through contention".
`ns2_bt_mgmt_link_params()` now requests a **6 s supervision timeout**, latency 0, and a
deliberately permissive 7.5–50 ms interval range. Only the timeout is the ask: the evidence calls
for margin, not a slower link, so the central keeps whatever interval it already chose. (JoypadOS
narrowed the interval too, but their dongle was also carrying DualSense audio; we have no evidence
requiring that here, and narrowing it would tax bulk management transfers for nothing.)
`gap_request_connection_parameter_update()` performs **no validation of its own** — it stores the
values and triggers L2CAP — so `ns2_bt_le_link_params_valid()` is the only thing standing between a
future edit and a combination the controller rejects.

### Exactly what that change can and cannot do

Scoped deliberately, because it is easy to over-credit:

- **Type B, LE management — may help, unquantified.** It raises the bar a peer stall must clear to
  kill the session from ~2 s to 6 s. Because the real outage length was never measured (the 2.35 s
  was UART idle, not outage), **we cannot say whether the captured failure would have survived it**.
  This is margin, not a proven cure.
- **Type B, Classic — no effect.** Classic supervision is a separate 20 s controller-side value we
  do not set and are not changing. Note this cuts against the simple story: Classic already had 20 s
  of margin and lost the link anyway, which means whatever happened was either longer than 20 s or
  not a supervision timeout at all. Either way this parameter could not have influenced it.
- **Type A — cannot help at all.** No connection parameter survives the peer's Bluetooth process
  aborting. Expect no improvement in the crash case.
- **No indirect benefit via cascade.** Checked in source: `CompanionViewModel.disconnect()` is
  explicitly *"management only"*, and the sole `releaseTouchInput(LinkEnded)` path keys on the
  bridge's **own** `BridgeLinkPhase`, not on management state. Nothing in the GATT-error path calls
  `session.stop()`. This matches BT-INV-008 and was observed live: after the 13:00 failure the app
  showed management "Connection failed" while Controller Link stayed `Playing` / "Input is
  streaming". **Keeping management alive therefore does not indirectly preserve the Controller
  Link** — the hoped-for secondary benefit does not exist, and the change must not be credited with
  it.

## Confidence

### Confirmed

- The lifecycle failure reproduces autonomously, without any physical interaction.
- Adapter admission counters do not move during it (`reject_window` unchanged across every cycle).
- Adapter core-starvation and recovery counters do not explain it (`control_tick_max_gap_ms` pinned
  at its prior high-water mark; `recovery.attempts` and `reboot.requests` both 0).
- Type A is an abort inside Android's own Gd HCI layer: SIGABRT in `gd_stack_thread`, assertion text
  and stack frame captured, PID 2815 → 28621.
- Type B has **no** abort and no process death anywhere in its log; the HAL sleep/wake trace
  brackets both disconnects.
- Both ACLs die together in the captured cases, LE and Classic.
- `0x85` is our label for Android GATT 133 and is downstream, established from our own log line.
- The Classic link carries a 20 s supervision timeout, traced to `hci.c:5010` / `hci.c:3859` with no
  override anywhere in this firmware.
- IBS UART sleep with live ACLs is routine on this device, not pathology: 124 sleep cycles
  alongside 113 successful GATT round trips in the ten healthy minutes preceding the Type B failure.
- The Mode-2 trust defect existed independently of any of this, and Mode 2 **never occurred** in 35
  cycles.
- Real controller/stack outage after a Type A crash is ~2 s, patch reload included.
- The dominant failure is Type C: 8 of 10 failures in the 25-cycle campaign ended in
  `HCI_ERR_LMP_ERR_TRANS_COLLISION` on `SET_CONNECTION_ENCRYPTION`, **after authentication had
  already succeeded**.
- Mode 1 (`PAGE_TIMEOUT`) is real but rare in this workload: **1 occurrence in 35 cycles**.
- Failures are not evenly spread: they cluster into adjacent pairs (cycles 13+14, 21+22, 24+25).
- Successful link-up latency does **not** degrade before a failure; it is bimodal at ~3.2 s / ~6.3 s,
  which is an artifact of the 2 s UART sampling interval, not a real distribution.

### Strongly supported, awaiting acceptance on new firmware

- A 6 s LE supervision timeout is the right *shape* of change: it removes an unexplained asymmetry
  (Classic 20 s vs LE ~2 s), and JoypadOS fixed link loss of the same class with the same parameter
  move. Note this rests on the asymmetry and the lineage, **not** on the captured 2.35 s, which was
  a UART sleep window rather than a measured outage.
- The idle inquiry gap should improve inbound Classic page opportunity. Mode 1 **did** finally
  reproduce (cycle 4, `PAGE_TIMEOUT` after 5.18 s), so the failure this targets is real rather than
  historical — but at 1 occurrence in 35 cycles it accounts for ~10 % of failures, so it cannot be
  the fix for the cycling problem the maintainer reports.
- Cross-transport trust closes the known Mode-2 admission hole, bound to a live encrypted session.

### Still unproven

- That the supervision change removes the user-visible cycling failure. It cannot touch Type A at
  all, and Type B's Classic leg is governed by a timeout we are not changing.
- **How long the Type B radio outage actually was.** The only bound available is that Classic's 20 s
  supervision expired or was bypassed, which is not consistent with a brief stall. This is the
  single biggest remaining hole in the Type B account.
- Which side stopped responding first in Type B.
- That the inquiry gap removes Mode-1 `PAGE_TIMEOUT`. Mode 1 occurred once in 35 cycles, so this
  needs a far longer campaign on the new firmware to show any effect at all.
- **That the Type C fix works.** The mechanism is source-established and the deferral is implemented,
  but our side's second `Set_Connection_Encryption` has never been *observed* — the flashed build
  emits no HCI trace, its `printf` output does not reach the UART diag channel, and the `btlife`
  ring records no security events. `enc_deferrals` in `btstate` is the counter that settles it on the
  next flash: non-zero means the collision source was real and is now suppressed.
- Why failures cluster into adjacent pairs.
- Adapter behaviour during a real Android Bluetooth reset — never exercised on the new build.
- Anything requiring the newly built firmware to actually run.
- Whether the app's 5 s `background-input-poll` cadence contributes by forcing repeated sleep/wake
  transitions: **Hypothesis**, untested.
- Whether shortening or backing off the app's ~20 s HID attempt window would help. Cycle 4 failed
  ~58 s after the crash, so "retry sooner" is not established.

## Remaining unknowns

- What provokes the SoC wake fault. Adapter workload correlates, but nothing here isolates a
  trigger, and the fault is in vendor firmware we cannot instrument.
- Whether other Android hosts show it. This is one tablet.
- Whether the requested parameters are accepted; Android may refuse the update. The adapter logs
  the request status, so this is answerable on the next flashed run.

## Related

- Radio-fairness change to idle Classic inquiry: see `ns2_bt_inquiry_restart_delay_ms()`.
  Motivated by the earlier Mode-1 `PAGE_TIMEOUT` reports, **not** validated by this experiment —
  Mode 1 did not occur in these ten cycles.
- Cross-transport Classic trust: see `ns2_bt_classic_trust_present()`. Closes a real admission gap
  introduced 2026-08-20, but `reject_window` proves it was **not** the failure captured here.
- Refuted paths recorded in [`refuted-hypotheses.md`](refuted-hypotheses.md).

## Reproducing the candidate

| | |
|---|---|
| Commit | the branch tip of `ns2-testing` at flash time; the Type C logic last changed in `0054a6d` |
| Flash | `build/pico2_w/PicoSwitchWGA-pico2_w.uf2` |
| SHA-256 | recompute after building - it embeds the build id, so it changes with every commit |
| Size | ~2.03 MB |
| Also built | `build/pico_w/PicoSwitchWGA-pico_w.uf2` - **not** the acceptance target |
| Reported build id | `<short8 of HEAD>+dirty` - see below |

```powershell
.\build.ps1                            # both boards
pwsh -File tools\run_host_tests.ps1     # firmware host suite (expect 70/70)
pwsh -File tools\run_android_tests.ps1  # companion unit tests
pwsh -File tools\uart_query.ps1 -Port COM11 -Command 'btstate'
```

**On `+dirty`, which will look alarming and is not.** `CMakeLists.txt` derives the build id from
`git status --porcelain --untracked-files=no`, so *any* modified tracked file marks the firmware
dirty. Firmware source is clean at this commit - verify with
`git status --porcelain --untracked-files=no -- src/ include/ tools/ CMakeLists.txt cmake/`, which
prints nothing. The suffix comes solely from the deliberately held Android/Touch Gamepad work (see
`TOUCH_GAMEPAD_ACCEPTANCE.md`). Do not stash that work to chase a clean id.

**Confirming the right firmware is running after the flash:** `uart_query.ps1 -Command 'bridge'`
reports a `build` field carrying `PICOSWITCH_BUILD_ID`. It must equal
`git rev-parse --short=8 <the commit you built>` followed by `+dirty`. Do not hardcode a hash here:
the id is derived from HEAD, so it changes on every commit, and so does the UF2's SHA-256. Anything
that does not match means the flash did not take, or a different tree was built. (`status` reports the firmware *profile*
versions `2.1.4 / 12.0.0 / 0.2.3`, which are configured values and do **not** change per commit -
they cannot identify the build.)

## Rollback

The candidate bundles three independent changes across several commits, so "revert the last commit"
does **not** undo Type C. Two clean recovery points:

| target | commit | what it gives you |
|---|---|---|
| Everything except Type C | `bed9035` | keeps the Mode 1 inquiry gap, Mode 2 cross-transport trust and the 6 s supervision margin; drops the encryption stand-down and its instrumentation |
| Pre-investigation baseline | `713a52b` | the build currently flashed (`bridge` reports `713a52ba+dirty`), known-good for everyday use and the state all the 2026-08-22 evidence was gathered against |

Type C spans `d92c85f` (stand-down introduced) -> `f6cbe41` (hardening + instrumentation) ->
`0054a6d` (naming/docs). Reverting only `0054a6d` leaves the workaround active.

No rollback binaries were retained; rebuild from source:

```powershell
git checkout <target>
.\build.ps1 pico2_w -Clean      # -Clean is required: an incremental build after
                                # changing commits can leave a stale uf2
```

Confirm the rollback took by reading `bridge` over UART and checking the `build` field matches the
target's short hash.

## Physical acceptance procedure — pending

One flash, one run. Designed so no second firmware iteration is needed unless the hypothesis is
actually false.

**Flash** `build/pico2_w/PicoSwitchWGA-pico2_w.uf2` from the commit recorded below.

1. **Baseline.** `pwsh -File tools/uart_query.ps1 -Port COM11 -Command 'btstate'` — record `enc`,
   `admission`, `disc`. All `enc` counters start at 0.
2. **Management.** Connect the companion. Confirm the app logs no
   `Config BLE link param request failed` (the 6 s supervision request was accepted).
3. **One cycle.** Open Touch Gamepad, confirm input reaches the console, exit, Stop playing, end
   management.
4. **Read `btstate`.** Expect `enc.deferrals >= 1`, `enc.peer_completed ≈ deferrals`,
   `enc.collisions == 0`, `enc.unencrypted_active == 0`.
   - `deferrals == 0` ⇒ the stand-down never fired; the Type C model is wrong. Stop and report.
   - `unencrypted_active > 0` ⇒ the peer-enforced encryption invariant is false. Stop and report.
5. **Campaign.** Run the same workload the baseline was measured on -- a different workload makes
   the before/after comparison meaningless:

   ```powershell
   python tools\controller_link_cycle.py --cycles 30 --port COM11
   ```

   It drives the full lifecycle over ADB, classifies every failed cycle into Type C / Mode 1 /
   Mode 2 / Type A / unknown, and prints a verdict against the criteria below. Requires the tablet
   unlocked with its screen on.
6. **Accept when:** Type C = 0 (no `Encryption failure 35` in `adb logcat`, `enc.collisions == 0`),
   Mode 2 = 0 (`admission.reject_window` unchanged), `enc.unencrypted_active == 0`, management stays
   usable, and Touch Gamepad input keeps working.
7. **Record separately, do not treat as Type C:** any `PAGE_TIMEOUT` (Mode 1 — baseline was 1 in 35,
   so a clean 30-cycle run neither confirms nor refutes the inquiry-gap change), and any
   `Process com.android.bluetooth ... has died` (Type A — an Android defect this pass does not
   address).

8. **Confirm the firmware first.** Before step 1, `uart_query.ps1 -Command 'bridge'` must report
   `build` = the short-8 hash of the commit you built, plus `+dirty`. See *Reproducing the candidate* for why `+dirty` is expected here.

### Classifying each failure

A cycle "fails" when the Controller Link does not become active within ~25 s. Classify from
`adb logcat` plus `btstate`; the four modes are distinguishable and must not be pooled:

| signature | mode | counts against the fix? |
|---|---|---|
| `btm_sec_encryption_change_evt: Encryption failure 35` / `LMP_ERR_TRANS_COLLISION`, or `enc.collisions > 0` | **Type C** | **yes** |
| `OnConnectFail: ... reason:PAGE_TIMEOUT(0x04)` | Mode 1 | no - record separately |
| `admission.reject_window` increased | Mode 2 | **yes** - would be a regression |
| `Process com.android.bluetooth ... has died` / `DeadObjectException` | Type A | no - Android defect |
| none of the above | unknown | investigate before judging |

### Verdicts

- **Accept Type C fix** - `enc.deferrals > 0`, `enc.peer_completed` tracks deferrals,
  `enc.collisions == 0`, `enc.unencrypted_active == 0`, and **zero Type C failures** across the
  campaign. Baseline was 8 Type C in 25 cycles, so ~30 clean cycles is a decisive result.
- **Mechanism falsified** - links establish normally but `enc.deferrals` stays 0. The stand-down
  never fired, so the source-established model is wrong. Capture `btstate` + logcat and stop; do not
  iterate blind.
- **Security failure** - `enc.unencrypted_active > 0`. Stop immediately: the peer-enforced
  encryption invariant is false and the stand-down must be reconsidered.
- **Residual collision** - `enc.collisions > 0` or the Android encryption-failure signature persists.
  The mechanism is right but the stand-down is not covering every path; capture the cycle.
- **Separate failure** - `PAGE_TIMEOUT` with no Type C evidence is Mode 1. At 1 occurrence in 35
  baseline cycles it neither confirms nor refutes the inquiry gap; log it and move on.

### Final user-flow check

Counters prove the mechanism; they do not prove the product. After the campaign, run one ordinary
session end to end and confirm:

- Touch Gamepad controls actually drive the console (sticks, D-pad, face buttons, triggers)
- Controller Link holds input authority while active
- exiting Touch Gamepad and Stop playing leave the console neutral - no stuck inputs
- management can be ended and reconnected afterwards
- no unexpected management loss during play
- if a physical controller is to hand, it still connects and works (no regression from the
  companion-only changes)

Baseline for comparison: **10 failures in 25 cycles (40 %)**, of which 8 were Type C.
