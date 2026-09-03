# Windows Phase 6 — Controller Link closeout

Date: 2026-09-03
Branch: `ns2-testing`

What Phase 6 asked for, what it got, and what it did not.

## Architecture as shipped — Path C

Controller Link carries the normalized `ControllerState` over the **existing
trusted management BLE connection**, on a dedicated binary characteristic pair
beside the newline-JSON command channel.

```
Windows companion                       PicoSwitch adapter
  ControllerInputSession                  CL-IN  (write without response)
  ControllerReportEncoder   ── 30 B ──▶   ns2_companion_link_parse
  ControllerLinkWriter                    ns2_active_input_submit
        │                                 router_submit_input
        ◀── CL-OUT (notify) ──────────     clink_service_feedback
```

- one Windows-owned Bluetooth connection, **no second ACL**;
- **no HOGP**, no `GattServiceProvider` HID host, no remote HID pairing;
- **no Windows Classic HID Device dependency** — that role does not exist for a
  user-mode app, proven separately;
- 4-byte header (version, opcode, LE sequence) + 26-byte canonical v2 payload;
- at most **one write in flight** with a latest-state mailbox; older states are
  superseded, never replayed.

The state model has only phases that exist: `Unavailable`, `Ready`, `Starting`,
`Streaming`, `Stopping`, `Stopped`, `Error`. `Advertising`, `WaitingForConnection`
and `Connecting` were **removed rather than renamed** — nothing advertises,
nothing connects to us, and the adapter never dials this PC.

## HOGP retirement

The AppContainer helper, HOGP advertising, the IPC protocol, the feasibility
probe, the `windows.appService` manifest extension and the C++/WinRT project are
all gone from the product (~2600 lines). The research that produced them stays:
an LE controller will not hold two connections to one peer identity (`0x0B`, ACL
Connection Already Exists), and Windows exposes no user-mode Classic HID Device
role.

`LayeringGuardTests.NothingReintroducesTheRetiredAppContainerHost` was **inverted
rather than deleted** — it used to assert the app service existed and was shaped
correctly, and now asserts nothing brings one back. That guard belongs on the
manifest specifically because an app service is invisible from the app's own
code: nothing would fail to compile if one returned, and it would quietly
re-acquire an out-of-process identity, a second trust level and a packaging
surface the product no longer needs.

Side effect worth having: the unpackaged app no longer needs Visual C++ Build
Tools. Only MSIX packaging still requires .NET Framework MSBuild.

## Input-source discovery

Measured, not assumed (`windows-controller-enumeration-2026-09-02.md`):

| Surface | Gives | Does not give |
| --- | --- | --- |
| `RawGameController` | every controller, VID/PID, `NonRoamableId`, counts | a real name, named controls |
| `Gamepad` | named buttons/axes, rumble | anything outside the XInput class |
| HID `DeviceInformation` | real product name, `ContainerId` | any gameplay input |

Fixes preserved and regression-tested:

- **`Gamepad` is a subset** — polling it alone was silently blind to DualSense,
  DualShock 4, Switch Pro and most non-Xbox hardware.
- **Cold start** — `RawGameController.RawGameControllers` reads empty on the first
  synchronous access (measured 0 → 1 after ~87 ms); the catalog now waits, bounded
  by evidence rather than a blind sleep.
- **XInput identity reconciliation** — an XInput pad reports a different product id
  per surface (`045E:0B00` vs `VID_045E&PID_02FF`), so the exact join missed and
  silently lost name, `ContainerId` and connection type.
- **`ContainerId`** classifies a handheld's built-in controls generically, with no
  VID/PID database to maintain.
- **Adapter self-exclusion** — the adapter enumerates as an ordinary
  `057E:2069`; it is never auto-selected, keyed on the management **wire names**
  (`pro2`, `gc`, `jcl`, `jcr`), with a test walking every `Personality` so a
  missing one cannot make the guard silently inert. An explicit choice still wins.

Unsupported `RawGameController` devices are offered and honestly labelled
"cannot be read"; no positional mapping is guessed.

## Latency and the analog backlog

Root-caused in `windows-controller-link-analog-backlog-2026-09-03.md`: the
reported "backlog" was **not in Controller Link**. With the controller on
Bluetooth the input takes two radio hops on one adapter, the controller's own link
loses airtime, and Windows returns stale readings the app forwards faithfully.
Every Controller Link counter stayed perfect throughout.

What the app owns and fixed:

- the source is sampled **at encode time**, not by a second unsynchronised timer;
- send-on-change with separate digital/analog ceilings, so a button can never
  queue behind a moving stick;
- an axis must move **2/255** before it is worth a frame, which stopped stick
  noise from setting the send rate and made the app a lighter neighbour on the
  shared radio;
- a 100 ms keepalive, because the adapter neutralizes a source that goes quiet —
  Classic needs no equivalent since its link is the liveness signal;
- `ThroughputOptimized` connection parameters, granted by Windows.

The player reports clean gameplay over both Bluetooth and wired sources,
including extended play.

## Firmware defect found and fixed

A companion that dies without sending `clink stop` used to leave the adapter
holding its sequence high-water mark, so the replacement's frames were all
rejected as superseded — `frames_received` climbing, `frames_applied` frozen, and
both ends reporting a healthy stream while nothing reached the console.
`clink start` now re-arms, through `ns2_companion_link_arm_session()` in the pure
layer so the contract is host-tested rather than living as two assignments in a
file no test can reach.

## Face-button ownership

The on-screen controller transmitted the opposite letter to the one drawn
whenever the face-layout preference was not Nintendo. The preference describes
the printed legend on a **physical** controller; the on-screen pad has none, and
with a freeform layout editor a control's label *is* its binding. The touch path
now maps with the same fixed presentation the renderer draws with, so drawn ==
sent by construction. `MapPhysicalFaceKey` is untouched.

The Touch Gamepad also gained the **personality dropdown** from Android, which
replaced that toggle there for the same reason: once the surface can present four
genuine controllers, the letters follow the controller.

## Feature parity with Android Classic

| Feature | Android Classic | Windows Path C | |
| --- | --- | --- | --- |
| Canonical buttons, D-pad, sticks, shoulders, triggers, stick clicks, ±| yes | yes | PASS |
| Home / Capture / C | yes | yes, via Controller Link | PASS |
| Source selection and switching | yes | yes, plus adapter self-exclusion | PASS |
| Start / Stop / restart | yes | yes, same management session | PASS |
| Neutralization on every transition | yes | yes | PASS |
| Management coexistence | yes | yes — 117 ms streaming vs 241 ms idle | PASS |
| Watchdog / process-loss safety | link is the signal | explicit stale-input watchdog + keepalive | PASS |
| Rumble / output | yes | yes | PASS |
| Battery | yes | yes | PASS |
| Personality selection in the touch surface | yes | yes | PASS |
| Diagnostics | counters | counters + per-second live line | PASS |
| **Motion** | where exposed | **not available** | DOCUMENTED |

Motion is the one intentional platform difference: `Windows.Gaming.Input` exposes
none, and Phase 6 asks for motion "only where a device genuinely exposes it"
while the exit criteria require an absent capability to be reported rather than
faked.

## Not done

- **Android Classic performance comparison.** `adb devices -l` was empty for the
  whole pass, so the Classic baseline could not be measured. The Classic sender's
  architecture was read from source and is recorded, but **no measured Android
  latency figure exists** and none is claimed. This is an external-device
  blocker, not a software gap.
- **Final firmware hardware smoke.** The adapter is still running `9cc2511f`; the
  session re-arm fix and the pure-layer extraction are newer. The final image has
  not been flashed, so nothing built after `9cc2511f` is hardware-qualified.

## Exit criteria

| Criterion | Status |
| --- | --- |
| `android_bridge_identify_trace` reports `matched > 0` | met — `matched=1 profile=v2-bridge` |
| input reaches the console correctly under both face layouts | met — fixed and regression-tested |
| no lifecycle transition can leave a held input | met — Stop, watchdog, app-kill and management loss all verified neutral |
| capabilities the PC lacks reported, not faked | met — motion absent, unreadable sources labelled |
