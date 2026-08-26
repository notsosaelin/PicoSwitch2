# Established BLE management GATT failure boundary (2026-08-25)

## Result

**Pre-C status: confirmed and readily reproducible. Stage C status: not
reproduced after an extended initial endurance observation; the endurance run is
still in progress.**

An established Android management session failed while both BLE management and
Controller Link were active. Android's lower Bluetooth layer reported LE HCI
reason `0x08` (connection timeout) about 3 ms before the app received GATT write
status 133. The status-133 callback was therefore a downstream report on an
already-lost link, not the operation which dropped it.

The same episode removed the Classic Controller Link 15.000 s after the LE
disconnect. A later full-HCI capture mapped the two transports and reproduced
that ordering: LE expired on its five-second supervision timer and BR/EDR expired
15.002 s later on its independent 20-second supervision timer, with no host
Disconnect command. For that retained later incident, the second event is an
independent Classic timeout, not a management-policy teardown. The original
handle 20 lacks a retained Connection Complete event and is not silently promoted
to the same mapping. Neither incident proves that both controllers stopped
hearing RF at exactly the same instant or identifies the internal mechanism which
stopped useful exchange.

Source review and live instrumentation rule out a second Android management
queue and overlapping GATT operations. A bounded healthy comparison completed
75/75 management requests on one GATT generation, including 49 background polls
and two deliberate full Refresh workflows. No timeout, non-zero write callback,
disconnect, close, or reconnect occurred. A subsequent management-only failure
proved that Classic coexistence is not required. No production Bluetooth
behavior was changed during the pre-C investigation.

The later Stage C modernization changed the Pico host-side SDK, BTstack, and
cyw43-driver foundation while retaining the byte-identical CYW43439 Bluetooth
firmware image. The previously minute-scale reproducible failure has not been
observed on the Stage C candidate after a substantially longer, multi-hour
observation. This is a material behavioral separation, not proof of a zero
failure probability or isolation of one responsible upstream change.

## Scope and provenance

- Branch: `ns2-testing`
- Source HEAD: `2272cb5e6eae`
- Instrumented app: source HEAD plus the uncommitted diagnostic changes described
  below
- Installed package: `dev.picoswitch.companion.debug` 2.0.0-debug
  (`versionCode=20000`)
- Android device: AYN Odin 2 Mini, Android 13
- Adapter: Pico 2 W / CYW43439, Pro Controller 2 personality
- Android collection transport: `192.168.68.56:43331`
- Adapter UART: `COM11`
- Pairing and app data were preserved by replacement install. No UF2 was flashed,
  no bonds were reset, and no destructive live command was used.
- Controller Link auto-resumed from the saved handheld source during the first
  bounded comparison. It was later intentionally stopped to obtain a verified
  management-only baseline and the Condition F failure described below.

The initial failure was observed before the instrumented APK was installed. Its
timestamped lines were preserved in the session transcript, but that collector was
not file-backed and the original logcat window rotated before it could be pulled.
The exact ordering below is therefore durable as a contemporaneous transcript,
not as a raw retained log. The later instrumented comparison has raw Android and
UART artifacts.

## Pre-C dependency baseline

The failure captures in this document predate Stage C. The surviving pre-C CMake
cache resolves `PICO_SDK_PATH` to `C:/Users/notso/.pico-sdk/sdk/2.2.0`, and the
installed clean dependency trees identify the baseline exactly:

| Component | Pre-C version and commit | Path |
|---|---|---|
| Pico SDK | 2.2.0, `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779` | `C:/Users/notso/.pico-sdk/sdk/2.2.0` |
| BTstack | v1.6.2-1-g501e6d2b8, `501e6d2b86e6c92bfb9c390bcf55709938e25ac1` | `C:/Users/notso/.pico-sdk/sdk/2.2.0/lib/btstack` |
| cyw43-driver | v1.1.0, `dd7568229f3bf7a37737b9e1ef250c26efe75b23` | `C:/Users/notso/.pico-sdk/sdk/2.2.0/lib/cyw43-driver` |
| Toolchain | Arm GNU 14_2_Rel1 | `C:/Users/notso/.pico-sdk/toolchain/14_2_Rel1` |

The pre-C and Stage C
`lib/cyw43-driver/firmware/cyw43_btfw_43439.h` files are byte-identical, with
SHA-256 `2075e3be3b7e11734404351df68be47b2d71cce646845f484e8f6813159f8647`.
Stage C therefore changed the host-side integration and libraries, not the
PatchRAM image downloaded into the CYW43439.

## Established-session failure timeline

Android timestamps are `logcat -v epoch`, rendered here in local EDT for
readability.

| Local time | Layer | Observation |
|---|---|---|
| 11:39:19.263 | app/Android GATT | Last visible healthy background `input` reply completed on GATT client 11. |
| 11:39:24.278 approx. | app | The next background `input` write began; the callback later reported 4,970 ms elapsed. |
| 11:39:29.245 | Fluoride GATT | `bta_gattc_conn_cback` reported `reason=0x0008` for the registered GATT clients, including the companion. |
| 11:39:29.247 | Fluoride security/link | `btm_sec_disconnected` reported LE handle 19, reason 8. |
| 11:39:29.248 | app | `onCharacteristicWrite` returned status 133 for background `input`, 4,970 ms after the write began. |
| 11:39:29.252-.254 | app | The command and background poll surfaced the write failure. |
| 11:39:44.245 | Fluoride security/link | `btm_sec_disconnected` reported handle 20, reason 8, exactly 15.000 s after the LE report. |

The lower-layer LE disconnect precedes the app callback. The in-flight write is
useful as a probe of when the failure became visible, but it cannot be promoted to
the trigger. A later UART smoke check found `cble.client=false`, no controller,
and management advertising resumed. Inquiry churn had already evicted the relevant
adapter event-ring window, so the adapter trace cannot settle the initiating side.

## Source audit

### Ownership and serialization

- [`ManagementOwner.kt`](../../android/companion/app/src/main/java/dev/picoswitch/companion/data/ManagementOwner.kt)
  constructs one process-wide `AdapterRepository` and one
  `BleGattManagementTransport`. The Activity is also `singleTask` in
  [`AndroidManifest.xml`](../../android/companion/app/src/main/AndroidManifest.xml).
- [`SerializedManagementSession.kt`](../../android/companion/management-core/src/main/kotlin/dev/picoswitch/management/SerializedManagementSession.kt)
  puts exchanges and disconnect mutations behind the same `Mutex` and finishes an
  admitted operation in `NonCancellable` context.
- [`BleGattManagementTransport.kt`](../../android/companion/app/src/main/java/dev/picoswitch/companion/transport/BleGattManagementTransport.kt)
  waits for each write callback, then waits for the complete newline-framed reply,
  before releasing the serialized exchange.
- Setup operations are sequential: connect, MTU, service discovery, notification
  enable/CCC write, then identity validation. Callbacks from any retired GATT
  object are rejected by object ownership/generation checks.

There is no second background transport or queue. The five-second
`background-input-poll` in
[`CompanionViewModel.kt`](../../android/companion/app/src/main/java/dev/picoswitch/companion/ui/CompanionViewModel.kt)
uses the same repository, transport, and serialized session. It is skipped during
UI/KBM work, personality transitions, and Controller Link setup.

### Status 133 behavior

On a non-zero `onCharacteristicWrite`, the transport completes the one pending
write waiter exceptionally and surfaces the error. Status 133 does not itself call
`disconnect()`, `close()`, create another GATT generation, or start another write.
Timeout and oversized-reply paths do invalidate the session, but neither preceded
this status-133 callback.

### Connection parameters

The app asks Android for `CONNECTION_PRIORITY_HIGH` immediately after connection,
then `CONNECTION_PRIORITY_BALANCED` after identity validation. The adapter asks the
central for latency 0 and a six-second supervision timeout through
`ns2_bt_mgmt_link_params()`.

The newly observed Android `onConnectionUpdated` callbacks show the actual values
on this session:

| Phase | Interval | Latency | Supervision timeout |
|---|---:|---:|---:|
| high-priority update | 15 ms | 0 | 5,000 ms |
| balanced update | 30 ms | 0 | 5,000 ms |

Thus the live central/controller result was five seconds, not the requested six.
That is a direct observation of the established link, but it is not evidence that
the one-second difference caused the loss.

## Instrumentation retained

The diagnostic change adds observation only:

- monotonic `SystemClock.elapsedRealtime()` timestamps alongside wall-clock time;
- independent UTC timestamps for last command, result, and error;
- request identity and source before queue admission;
- queue wait, write start, immediate Android API result, write callback, first
  notification, complete reply, timeout, and lock release;
- GATT generation, object identity, connection phase, and app-requested
  disconnect/close boundaries;
- service-discovery, notification, CCC, connection-priority, and hidden framework
  connection-update callbacks; and
- UART monitor retention of unsolicited BTstack lines rather than discarding them.

The in-app diagnostic ring was enlarged from 400 to 2,400 entries so ordinary
five-second polling does not erase the failure boundary within a few minutes.
Debug logcat mirroring remains build-gated.

## Bounded idle-versus-interaction comparison

This was a short differential, not a failure-hunting soak.

1. Clear only the adapter lifecycle diagnostic counters/ring.
2. Start file-backed UART collection and a cleared, filtered Android logcat.
3. Replace-install and launch the diagnostic APK; preserve the existing bond/data.
4. Let automatic management and Controller Link setup settle.
5. Leave the app idle with its ordinary five-second background polling for about
   147 seconds after management readiness.
6. Tap **Refresh adapter** twice, 12 seconds apart. Each tap ran the normal ten-command
   foreground snapshot workflow.
7. Leave the same connection idle for about another 95 seconds.

### Results

- One GATT generation/object throughout: generation 1, object `0xf44bed6`.
- 75 requests queued and 75 complete replies received.
- Sources: 49 background, 23 foreground (20 from the two deliberate Refresh
  workflows plus three ordinary convergence/resume reads), and three reconnect
  identity reads.
- Complete-request latency: 44 ms minimum, 77 ms median, 109 ms p95, 117 ms
  maximum.
- Background queue wait: 1-4 ms. Longer waits were deliberate serialization and
  the existing post-reply turnaround: up to 190 ms during automatic setup, 112 ms
  during Refresh.
- Every immediate API result was success, every write callback was status 0, and
  every request received a complete reply.
- No request timeout, GATT error, stale callback, app disconnect/close, lower-layer
  disconnect, or generation replacement.
- UART observed management connect once, Controller Link become ready, and both
  remain ready through collection end. Final counters: management connects 1,
  management disconnects 0, controller disconnects 0, HCI disconnects 0, state
  losses 0, event drops 0.

This run demonstrates that both the idle poller and foreground refresh can operate
correctly on the same serialized session. It does not estimate a failure rate or
clear a rare radio failure.

## Rooted Android lower-layer follow-up

Read-only inspection identified the Odin's Bluetooth path as Qualcomm's HIDL
`android.hardware.bluetooth@1.1-service-qti`, using `libbluetooth_qti.so` and a
Qualcomm `hamilton` controller transport over `/dev/ttyHS0`. The controller
reported manufacturer ID `0x001D` (Qualcomm), HCI/LMP version `0x0D`, and
subversion `0x6E2B`. Full HCI snoop and Bluetooth Quality Report (BQR) support
were then enabled with the smallest reversible Android-side change; bonds were
not wiped. The controller accepted BQR mask `0x12`, comprising the requested
Approaching-LSTO event plus its mandatory controller-failure bit.

The full HCI trace established these Android/Pico handle pairs during one healthy
session with peer `88:A2:9E:D1:77:78`:

| Transport | Android controller handle | Pico/BTstack handle |
|---|---:|---:|
| BLE management | `0x0005` | `0x0040` |
| BR/EDR Controller Link | `0x0006` | `0x000B` |

Handles are local to each controller and are not expected to have equal numeric
values. The original transcript directly identifies handle 19 as LE. Its handle
20 lacks a retained Connection Complete event, so that one historical handle
cannot be remapped from raw evidence alone. A later retained dual-link incident
does establish the same ordering and timing with mapped handles:

- Android LE handle `0x0002` connected at 16:10:53.304 with final 30 ms interval,
  latency 0, and 5,000 ms supervision timeout.
- Android BR/EDR handle `0x0003` connected at 16:10:56.223 to the same peer.
- BQR reported Approaching LSTO for LE handle `0x0002` at 16:13:36.020353;
  Disconnection Complete reason `0x08` followed 1.165 ms later.
- BQR reported Approaching LSTO for BR/EDR handle `0x0003` at
  16:13:51.017242; Disconnection Complete reason `0x08` followed 6.537 ms
  later.
- The disconnect separation was 15.002261 s. No host Disconnect command,
  Android HCI Reset, Hardware Error event, Bluetooth-process restart, or HAL
  restart preceded either timeout.

The BQR `lsto` fields were 8,000 and 32,000 controller clock units, representing
half of the negotiated five-second LE and 20-second Classic supervision timers.
On this controller the warning reached the host only milliseconds before the
disconnect, not halfway through the outage. RSSI was `-38 dBm` for both reports;
that terminal sample does not establish the RF history during the preceding
silence. HCI snoop observes the host/controller boundary, not over-the-air
packets.

## Condition F: Classic is not required

This condition began as a verified management-only baseline:

| Local time | Observation |
|---|---|
| 16:21:41 | The app intentionally stopped Controller Link. Android issued the Classic disconnect; Pico later reported `controller_connected=false`, `clink=0xFFFF`, and local handle `0x000B` reason `0x13`. BLE management remained connected. |
| 16:22:22-16:23:07 | A 45-second bounded baseline remained management-only and healthy: `cble.client=true`, Classic raw/ready counts 0/0, no new BQR or disconnect. |
| 16:24:42.698390 | Last controller-to-host ATT management reply in the HCI trace. App request 119 completed at 16:24:42.724. |
| 16:24:47.739694 | Next host-to-controller ATT write; no ATT response followed. |
| 16:24:50.047401 | BQR Approaching LSTO for LE handle `0x0005`, RSSI `-38 dBm`. |
| 16:24:50.048375 | Android controller emitted LE Disconnection Complete, reason `0x08`. |
| 16:24:50.056 | The app received downstream write status 133. |
| 16:25:31 approx. | Only after management was already dead, the user attempted to start Controller Link; that separate Classic establishment attempt failed. |

This is a genuine established-management failure with no Classic ACL present.
**Classic coexistence is therefore not a necessary cause of established BLE
`0x08`.** It could still modulate load or probability, but it cannot be a required
precondition.

### Final Pico reboot/recovery finding

**Proven: the RP2350 rebooted at the Condition F boundary.** Although the UART
collector began after the failure, the new firmware lifetime was reconstructable
from 17 independently observed event-count transitions and their ring `t_ms`
values. Their intersected host-correlation interval (not an independently
synchronized hardware timestamp) is
16:24:49.932594-16:24:49.955170 EDT, only 93-116 ms before Android emitted the
LE `0x08`. The fresh ring begins with `scan_suppress/not_powered` at 1,872 ms,
then `scan_start` and `inquiry_start` at 1,999 ms and `adv_start` at 2,004 ms.
All lifecycle totals also restarted from their boot defaults: management
connects/disconnects 0/0, no ACLs, and a ten-entry startup ring at the first
post-failure snapshot. This is full firmware-uptime reset evidence, not merely a
CYW43439 link reset.

**Ruled out: the project's deliberate Bluetooth recovery reboot path.** The
post-boot `bthealth` response reported persistent recovery scratch state as
`consecutive_boots=0`, `last_boot_cause=0`, and `last_escalation.valid=false`.
That path writes its non-zero cause and escalation snapshot to watchdog scratch
before calling `watchdog_reboot()`. No such marker survived. Post-boot HCI
recovery attempts/completions were also 0/0.

**Consequential, not independently causal: the CYW43439 was initialized again as
part of the RP2350 boot.** The fresh not-powered, controller-ready, discovery,
and advertising sequence demonstrates reinitialization. Existing telemetry does
not show a CYW43439-only recovery before the RP2350 reboot.

**Not observable with the pre-C build: the generic RP2350 reset cause.** The
firmware did not expose `watchdog_caused_reboot()` or raw reset-reason state, and
the UART collector was not attached during the several seconds between the last
reply and the reconstructed boot origin. The artifacts therefore cannot
distinguish external power/brownout, hard reset, fault, or an uninstrumented
watchdog. They also cannot prove whether the reboot initiated the radio silence:
useful host-visible RX had already stopped several seconds earlier.

## Keep the Classic failure families separate

Reason `0x08` names a timeout result, not one mechanism. The evidence retains
three externally distinct families:

1. **`CLASSIC_PAGE_TIMEOUT`:** Android starts paging, but Pico sees neither HCI
   Connection Request nor `page_rx`; no Classic ACL exists. Host HCI cannot tell
   whether the page failed over RF or the controller failed to deliver it upward.
2. **`CLASSIC_ACL_TIMEOUT`:** Pico sees `page_rx` and `page_accept`, but ACL
   establishment never completes and no auth/security phase is reached. The
   CYW43439 later reports `0x08` after about 20.2-20.6 seconds.
3. **Established BLE management `0x08`:** a working LE ACL stops making useful
   progress and expires on its approximately five-second supervision timer.

`CLASSIC_ACL_TIMEOUT` is structurally closer to established BLE LSTO than
`CLASSIC_PAGE_TIMEOUT`. There is no evidence that `CLASSIC_PAGE_TIMEOUT` causes
or predicts the established BLE failure.

## Post-migration observation / Stage C

### Current configured dependency state

Read-only inspection on 2026-08-25 found branch `ns2-testing`, source HEAD
`2272cb5e6eae4dc79d3eb986ea923e8419f6ce3c`, and a dirty working tree containing
the Stage C migration plus the preserved diagnostic work. The configured
`build/pico2_w` candidate uses:

| Component | Current configured version and commit | Resolved path |
|---|---|---|
| Pico SDK | 2.3.0, `98a542c1a62fb549ffb5d66a3e5892b06276b670` | `C:/Users/notso/.pico-sdk/sdk/2.3.0` |
| BTstack | stock v1.8.2, `075a0780f0fad7ff67d58ac19f46e8953656a752` | `C:/Users/notso/.pico-sdk/sdk/2.3.0/lib/btstack` |
| cyw43-driver | v1.1.1, `055d64274b014dd7b1c2fc94d26e8a18face7124` | `C:/Users/notso/.pico-sdk/sdk/2.3.0/lib/cyw43-driver` |
| Compiler | Arm GNU 15.2.Rel1, GCC 15.2.1 20251203 | `C:/Users/notso/.pico-sdk/toolchain/15_2_Rel1` |
| Build tools | CMake 4.3.4, Ninja 1.13.2, picotool 2.3.0 | Pico SDK tool bundle |

The build graph resolves BTstack sources from the stock SDK path; no
post-v1.8.2 pin or `PICO_BTSTACK_PATH` override is present in this configured
candidate. Its embedded build identity is `2272cb5e+dirty`. The current
`build/pico2_w/PicoSwitchWGA-pico2_w.uf2` is 2,057,216 bytes, last written
2026-08-25 19:33:55 EDT, with SHA-256
`750cc6fcdd137a2539be66442471ac80c1814094bcbfac0bbd2bc6489ba15227`.
The active endurance board was deliberately left untouched, so existing host
artifacts cannot prove that this exact UF2 file is the image currently running.

### User-observed A/B evidence

The following pre-C wall times are maintainer observations from deliberate
retesting, not automatically collected trace timestamps:

- approximately 20:02 to 20:08: management established with Classic absent,
  then LE `0x08` after about six minutes;
- approximately 20:18 to 20:25: management established with Classic left
  connected, then LE `0x08` after about seven minutes;
- approximately 20:55 to 21:08: LE `0x08` after about 13 minutes;
- one additional failure after about 14 minutes;
- one additional failure after about two minutes whose last displayed management
  command was `Device`, not `input`;
- another failure after about two minutes; and
- additional closely spaced `0x08` observations during repeated pre-C testing.

Pre-C therefore reproduced the established-management failure repeatedly on the
order of minutes, with observed healthy lifetimes varying by roughly 2-14
minutes. This contradicts a fixed six- or ten-minute trigger. It also provides no
basis for assigning causality to one management command type.

The Stage C candidate was started at approximately 19:30 on a different Pico 2 W
and a different Android device. At the latest maintainer observation the ongoing
run had exceeded three hours with no observed HCI `0x08` or management
disconnect. Background `input` requests continued about every five seconds and
continued receiving complete responses. No obvious pairing regression was seen,
and approximately 20 Touch Gamepad reconnects completed without the familiar
pre-C Classic establishment error during this early smoke coverage.

This is not a perfectly controlled device-for-device A/B and does not establish
a new MTBF from one run. It does establish a substantial observed behavioral
separation: **the previously readily reproducible established-management `0x08`
has not reproduced on the Stage C candidate over a substantially longer
observation window.**

## Evidence artifacts

- [`dumps/picoswitch-management-20260825.log`](../../dumps/picoswitch-management-20260825.log)
  - SHA-256 `bbb1d84badfe860e06732523b1f7f2ed368e33a4b7e6a90a858a2a508931fb02`
- [`dumps/management-gatt-20260825-uart.jsonl`](../../dumps/management-gatt-20260825-uart.jsonl)
  - SHA-256 `9803a720b338302296cc86c9b56c04d98a0faca0e1ea13a1cdde387716cd0029`
- [`dumps/20260825-160751-odin-root-bluetooth-audit/`](../../dumps/20260825-160751-odin-root-bluetooth-audit/)
  - rooted Odin stack/HAL/controller audit, initial protected-log inventory, and
    exact reversible HCI/BQR settings
- [`dumps/20260825-161402-collector-smoke/`](../../dumps/20260825-161402-collector-smoke/)
  - retained dual-link HCI/BQR timeout and post-failure fresh Pico boot ring;
    `manifest.json` inventories and hashes every file
- [`dumps/20260825-161710-baseline-a-management-only/`](../../dumps/20260825-161710-baseline-a-management-only/)
  - despite its early directory label, this is the healthy 60-second dual-link
    Condition B and handle-mapping bundle
- [`dumps/20260825-162221-baseline-a-management-only-verified/`](../../dumps/20260825-162221-baseline-a-management-only-verified/)
  - verified healthy 45-second management-only Condition A
- [`dumps/20260825-162517-baseline-c-human-paced-transitions/`](../../dumps/20260825-162517-baseline-c-human-paced-transitions/)
  - Condition F freeze and post-failure Pico state; the failure occurred before
    the attempted Classic transition, so the directory's originally selected
    scenario does not describe the event's cause
  - HCI JSONL SHA-256
    `84827940c26fab2f7494002c201c96e1a3423d574ffb75d519d7333d680e7ce7`
  - Pico UART JSONL SHA-256
    `e6b25028461f70f992c6f2088b7b095a6ad316a528a06b621ad11619fbae8880`

## Interpretation and confidence

### Confirmed

- The observed status 133 followed a lower-layer LE `0x08` report and is a
  consequence, not the initiating event.
- The app has one management transport and one logical request lock. Background
  and foreground callers do not overlap GATT writes.
- The live link used 30 ms interval, latency 0, and 5 s supervision timeout after
  returning to balanced priority.
- The bounded comparison remained healthy under both ordinary background polling
  and two full foreground Refresh workflows.
- A mapped dual-link event independently expired LE and BR/EDR with five- and
  20-second supervision boundaries; no host Disconnect caused either event.
- Condition F reproduced established BLE `0x08` after Classic had been
  intentionally absent for more than three minutes. Classic is not required.
- The Condition F Pico had a new RP2350 boot origin at
  16:24:49.932594-16:24:49.955170, immediately before Android surfaced the LE
  timeout.
- Persistent recovery scratch rules out the project's deliberate Bluetooth
  recovery reboot path for that boot.
- Pre-C failures were user-observed repeatedly across roughly 2-14 minute
  lifetimes; neither a fixed elapsed-time trigger nor one command type is
  established.

### Strong evidence

- The pre-C failure is below the management command scheduler. The app was waiting on
  an already-admitted write when Android's lower layer declared the ACL timed out.
- In the mapped dual-link incident, both ACLs stopped surviving long enough to
  expire independently. A common lower-layer availability episode is plausible,
  but exact common T0 and shared internal cause are not proven by host HCI.
- Stage C materially changed observed reliability. Because the CYW43439 firmware
  image is unchanged while the host-side foundation changed, a Pico SDK,
  BTstack, cyw43 integration, or combined host-side change may have eliminated or
  dramatically reduced the failure.

### Hypotheses, not facts

- The exact pre-C initiating mechanism remains unknown. Condition F proves an
  RP2350 reboot but not its generic reset cause, and it does not prove whether
  the reboot initiated the preceding packet silence.
- The central accepting five seconds instead of the requested six reduces margin,
  but this trace does not show that another second would have preserved either ACL.
- Five-second background management traffic may reveal a dead link promptly; the
  successful differential provides no evidence that it creates the outage.
- Stage C changed several host-side dependencies at once. No isolation experiment
  identifies one upstream commit as responsible, and one ongoing endurance run
  cannot prove that failure probability is zero.

## Investigation closeout

The pre-C issue remains historically confirmed. After Stage C modernization it is
presently not reproducible under a substantially longer endurance observation.
Further pre-C root-cause work is paused because the implementation foundation and
the observed failure behavior both changed.

Current stale directions are explicitly rejected:

- more Android-only diagnostics and dozens more pre-C reproductions are low-value;
- Classic coexistence is disproven as a necessary condition;
- polling cadence, the `input` command, and the `Device` command are not
  established as causal;
- a fixed elapsed-time trigger is contradicted by the observed 2-14 minute range;
- changing LE supervision timeout, adding keepalives, or changing connection
  parameters before determining whether the final Stage C candidate reproduces
  would confound the comparison; and
- controller-firmware reverse engineering is not justified while the modernized
  host stack no longer readily reproduces the issue.

A separate future session may add comprehensive Pico-side UART observability
across application, async context, BTstack, CYW43/CYBT servicing, interrupts,
shared gSPI, controller ready/awake, HCI, per-ACL progress, and recovery/reset
state. That work is regression forensics: if Stage C or a later exactly pinned
BTstack candidate ever reproduces `0x08`, the next event should preserve the
initiating boundary. It is not a prerequisite for continuing the current
endurance run and was intentionally not implemented in this closeout.

Possible later closure gates, not results of this pass, are overnight
management-only endurance on the final candidate, multi-hour management plus
Classic endurance, persistent-bond reboot/reconnect validation, and a 500-cycle
Touch Gamepad reconnect soak. The currently running Stage C experiment must be
left undisturbed.
