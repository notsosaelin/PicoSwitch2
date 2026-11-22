# Android's two links to the adapter, and the move to one (2026-11-22)

Status: **Resolved by design change.** Android Controller Link moved from its
own Classic HID connection to the Path C data plane on the management link.
Software and first device run done; gameplay and soak validation pending
(STATUS.md, gate 19).

Supersedes the recommendation in
[`android-gameplay-carrier-choice-2026-09-03.md`](android-gameplay-carrier-choice-2026-09-03.md).

## Symptoms

On a Galaxy S25 Ultra (Android, Samsung stack), against a production Pico 2 W
(build `fbcfc449+dirty`) and a UART-equipped dev adapter (build `e054bfd1+dirty`,
which predates v2.6.0):

1. Controller Link often did not connect on the first try.
2. Management and Controller Link dropped together mid-session, three times in
   one morning; the Touch Gamepad was then left over a dead link.

The same app and firmware did not show this on an AYN Thor, an Odin 2 Mini or a
Pocket Nova (maintainer report).

## Evidence

Adapter: `btlife dump`, `btstate`, `bthealth`, `fault` over UART. Phone:
`logcat` (the app's own diagnostic events) and `dumpsys bluetooth_manager`.
Adapter uptime and phone time were aligned on the management drop
(adapter 230 676 ms = phone 11:24:54.076).

**Not a firmware regression (Confirmed).** The dev adapter runs pre-2.6.0
firmware and showed the same drop. No firmware change since v2.6.0 touches the
radio, connection or Controller Link paths; each was read.

**Not a crash (Confirmed).** `fault` empty, `bthealth` no probe failures, no
recovery, no reboot request.

**The drop (Confirmed mechanism, Strong cause).** Both ends saw an LE
supervision timeout, reason `0x08`, on the management link. 0.1 s later the
adapter closed the Classic Controller Link itself (`0x16`,
`clink.mgmt_teardowns` 1): that teardown is the trust rule that binds a
Controller Link to a live management session, working as designed. The Classic
link was healthy; the LE link starved. The phone's Bluetooth log shows the
Classic ACL renegotiating power mode about 70 times a second throughout
(`active -> sniff`, `pending -> active`): Pico 2 W builds refuse sniff
(`NS2_DS5_AUDIO`, link policy without `LM_LINK_POLICY_ENABLE_SNIFF_MODE`) and the
S25 asks again every ~15 ms. One radio on each side serves both links. The
handhelds that work do not storm this way (inferred from their behaviour; not
captured).

**The slow first connect (Confirmed).** The phone paged the adapter's Classic
side for 6.4 s (11:22:50.094 to 11:22:56.528) and the adapter recorded no
`page_accept` in that window: it was inside a 6.4 s controller inquiry round
(inquiry runs about 76 % of the time while no controller is attached:
`INQUIRY_DURATION` 5, `NS2_BT_INQUIRY_IDLE_GAP_MS` 2000). The next attempt was
accepted at the very end of a round. This confirms the mechanism the 2026-08-21
fix only rated Strong.

**Companion audio was on in every drop.** The app re-plugs the headset when
Controller Link starts if it was on before, so "no audio" sessions had speaker
and microphone streaming. Audio is not the root cause (it rides the LE link in
both designs), but it raised the LE link's load while the Classic link
competed for the same radio. The audio record had already measured the cost:
20 % empty connection events with a Classic HID link on the radio, against 2 %
without, and 144-184 ms audio latency on Android against 103-106 ms on Path C.

## Why the earlier recommendation no longer holds

The 2026-09-03 analysis declined the move for two reasons. Both are now false:

1. *The stated benefit is already delivered.* It is not: the second link is
   what fails, in two independent ways.
2. *The Android LE interval is unobserved.* Observed on the S25: 7.5 ms during
   setup, 15 ms at `CONNECTION_PRIORITY_HIGH`, 30 ms balanced.

## Change

- Android Controller Link rides Path C: `clink start|stop|status`, the canonical
  v2 report framed by `ControllerLinkDataPlane` (Kotlin port, pinned to the same
  geometry as C# and firmware), written without response to the Controller Link
  characteristic, feedback (rumble, player, motion wanted) from its
  notifications. Motion, battery and flags travel in the same report; the
  firmware decodes Path C with the same extractor as the Classic bridge.
- Single-flight writer, newest frame wins, keepalive every 100 ms (the adapter
  neutralizes a source quiet for 300 ms; the session only sends on change).
- Connection priority is HIGH while Controller Link or audio is open, BALANCED
  otherwise, decided in one place (closing audio used to drop a live link to
  BALANCED).
- The Classic HID transport was then removed from the app: every firmware
  since 2.5.0 has the data plane and the bridge contract already pins firmware
  and app to one release. No firmware change: Path C has no platform gating.

## First device run

S25 Ultra, debug build, dev adapter (`e054bfd1+dirty`), Touch Gamepad, no
controller attached: launch to `Playing` 2.9 s on the first attempt, no Classic
connection (`classic_raw` 0); `clink status` received 163 = applied 163,
stale/short/version/opcode/rejected 0, neutralizations 0, `max_gap_ms` 135.

## Second finding: discovery never stepped aside for Path C

During the S25 session `btstate` showed `scan_active` and `inquiry_active`
true throughout. The discovery policy counted a streaming companion as a
controller, but the retire path (`btstack_host_idle_scan_if_connected`) asked
only about HID peers and the idle safety net restarted discovery whenever no
HID connection existed, so every Path C session (Windows and Linux included)
streamed beside an active LE scan and Classic inquiry. Fixed in 2.6.1 through
`ns2_bt_input_source_holds_discovery()`, host-tested.

## 2.6.1 build on hardware

Pico 2 W dev adapter flashed with the 2.6.1 changes, AYN Thor (Android 13),
PC emulator host:

- `scan_active` false and `inquiry_active` false while the Thor streamed
  (both true throughout the S25 session on the earlier build).
- Controller Link: all frames applied, 0 neutralizations, `max_gap_ms` 149.
- Headset and microphone: adapter `busy` 0 of 819 frames (2.6 % before),
  `send_max_gap_us` 30 764 (71 056 before), 0 failed; phone sink 0 dropped of
  1 001 blocks, the one underrun being the start-up prebuffer.
- LE 2M PHY on both directions (the Thor had already negotiated it).

## Still to show (gate 19)

Gameplay against a console on both adapters: inputs, motion, rumble, the
headset, a session of at least 30 minutes with no management drop, reconnect
after the phone leaves range, and the same pass on a Thor or Odin to confirm the
handhelds did not regress.
