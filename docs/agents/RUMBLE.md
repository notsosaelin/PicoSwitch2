# Agent brief — rumble

Read [COMMON.md](COMMON.md) first.

## The chain

```
console output report 0x02
      ↓  ns2_hid_out_report()        switch_pro2.c — decode packed HD-rumble amplitudes
      ↓  report_set_rumble(0, L, R)  report.c — slot 0, generation counter bumped every call
      ↓  feedback_get_state()        ns2_seam.c — generation change marks the slot dirty
      ↓  driver task                 per-family output
             ├─ vendor drivers (DualSense, Xbox, …) → quirk send_rumble()
             └─ Android bridge       bthid_gamepad.c → android_bridge_encode_feedback()
      ↓  bthid_send_output_report()  DATA|OUTPUT header + report ID on the interrupt channel
      ↓  L2CAP
      ↓  Android BluetoothHidDevice.Callback.onInterruptData / onSetReport
      ↓  BridgeOutputCodec.decode() -> RumbleRequest(L,R)
      ↓  AndroidOutputBackend.apply(request)  shaping, hysteresis, ~25 Hz retrigger ceiling
      ↓  HapticRouting.choose()      WHICH vibrator — see the root cause below
      ↓  InputDevice vibrator (correct) │ system vibrator (wrong for a gamepad)
      ↓  Android input/vibrator service ← can silently discard here
      ↓  actuator
```

## Established facts — CONFIRMED

- **Report 0x02 decode.** `[id][16B left LRA][16B right LRA][9B reserved]`; each motor block is
  `[0x50|counter][5B packed freq/amp][zeros]`. The 5-byte field is 40-bit little-endian:
  `freq_0[0:10] | amp_0[10:20] | freq_1[20:30] | amp_1[30:40]`. Drive rumble from the **amplitude**
  fields only — the frequency fields are nonzero at rest and produce a constant idle buzz.
  Two independent sources agree bit-for-bit: ndeadly's `switch2_input_viewer.py` and the Linux
  `HID: nintendo` driver's `switch2_encode_rumble()`. `amp0`/`amp1` are two frequency bands of the
  same physical motor, so `max()` collapses them to one scalar per motor.
- **Per-motor separation is preserved** end to end so drivers with true stereo output keep it.
- **Change detection is generation-based, not value-based.** Comparing raw left/right values cannot
  distinguish "nothing happened" from "it changed and changed back before the last poll", which
  silently drops stop transitions on drivers with a long hardware sustain.
- **`find_player_index()` always resolves to slot 0.** This project has one output identity. It
  previously returned the caller's BTstack connection index, so any Classic device outside slot 0
  read a feedback slot that never received anything — rumble silently dead, across every driver
  family. Fixed 2026-07-12.

## AYN Thor — RESOLVED 2026-08-14, physically confirmed

**The Thor now physically rumbles.** Root cause was never in our code path.

### Primary cause: `Settings.System.VIBRATE_ON = 0`

Measured over ADB on the device: `settings get system vibrate_on` → **0**, and
`dumpsys vibrator_manager` showed `mVibrateOn=false`. AOSP's `VibrationSettings`:

```java
private static final int VIBRATE_ON_DISABLED_USAGE_ALLOWED = USAGE_ACCESSIBILITY;
if (!mVibrateOn && (VIBRATE_ON_DISABLED_USAGE_ALLOWED != usage))
    return Vibration.Status.IGNORED_FOR_SETTINGS;
```

With that setting at 0 **every vibration from every app is discarded**, for every usage except
ACCESSIBILITY. That is the whole of "zero rumble, ever", and it is exactly the
`status: ignored_for_settings, scale: 0.00` recorded against our package back on 2026-08-14 10:41.
Our usage flags were never the problem.

### Secondary cause (ours): the `isExternal` veto

The routing cascade added earlier the same day copied Moonlight's "system vibrator only if
`!isExternal()`" rule. The Thor's **built-in** controller reports
`Classes: KEYBOARD | GAMEPAD | JOYSTICK | EXTERNAL` with `ids=[] hasVibrator=false`, so that veto
resolved to `None` and cut off the only actuator on the device. Removed. The project's own ADB
audit had already recorded the rule —
`docs/experiments/android-controller-ayn-thor-adb-audit-2026-08-12.md`: *"do not reject
`isExternal == true` or a virtual origin"*. Android's EXTERNAL class means "not on the main
board", not "not part of this handheld".

### Measured proof

Driven autonomously through the app's own output path via the debug self-test broadcast, with
`dumpsys vibrator_manager` read before and after:

| `vibrate_on` | routing | dumpsys status | physical |
|---|---|---|---|
| 0 | `System` | `ignored_for_settings` | nothing |
| 1 | `System` | accepted, played, `cancelled_by_user` on stop | **maintainer felt it** |

Routing line after the fix:

```
haptics bound: System (device=yes ids=[] hasVibrator=false external=true system=true
               vibrateOnSetting=false) -- SYSTEM VIBRATION IS OFF ... amplitudeControl=true
```

### Corrections to this document's own earlier record

- The `haptics bound` line **does** appear — it is emitted at bind time and is visible in
  `adb logcat` (tag `PicoSwitch`). An earlier report that it never appeared was not reproducible.
- The recorded `USAGE_TOUCH` mechanism was wrong. A bare `vibrate(effect)` is `USAGE_UNKNOWN`, and
  `VibrationSettings` maps `USAGE_UNKNOWN` and `USAGE_MEDIA` to the same intensity, so the
  `USAGE_MEDIA` change was a no-op. It is retained as correct labelling only.
- The `IGNORED_BACKGROUND` hypothesis was **not** the cause here: every recorded status was
  `ignored_for_settings`, which sits after the background check. The foreground service is retained
  because it is independently correct for a Bluetooth HID bridge, not because it fixed this.

### The one thing that can silently undo this

`VIBRATE_ON` is a user-facing setting (Settings → Sound & vibration). The app now reads it at bind
time and says so in the routing line, so this can never again cost more than one glance. **It is
diagnostic only and deliberately does not affect routing** — the user can change it at any moment
and a stale read must never permanently disable output.

## Self-test: exercise the Android output path with no console and no adapter

Debug builds register an exported receiver dynamically (gated on `BuildConfig.DEBUG`):

```powershell
adb shell am broadcast -a dev.picoswitch.companion.SELF_TEST_RUMBLE --ei left 220 --ei right 220
adb shell am broadcast -a dev.picoswitch.companion.SELF_TEST_RUMBLE --ei left 0 --ei right 0
adb shell dumpsys vibrator_manager   # read status/scale/opPkg for the request
```

Use this before ever involving the console: "can this handheld vibrate at all" and "did the adapter
deliver rumble" are separate questions.

## Diagnostics — answer the whole firmware half in one read

`rumble` over UART (`rumble clear` to reset) reports both firmware-side ends:

```json
{"rumble":{"console":{"reports":N,"nonzero":N,"last":[L,R]},
           "bridge":{"sent":N,"failed":N,"nonzero":N,"last":[L,R],
                     "player":N,"motion_wanted":0|1}}}
```

Reading it:

| observation | conclusion |
|---|---|
| `console.nonzero == 0` | the console never asked for rumble, or report 0x02 never arrived. Nothing downstream matters. |
| `console.nonzero > 0`, `bridge.nonzero == 0` | decoded but never handed to the bridge. Look at input ownership (`find_player_index`) and the feedback generation. |
| `bridge.failed > 0` | the transport rejected the send. Bluetooth/L2CAP problem, not a haptics problem. |
| `bridge.sent` advancing, handheld silent | the firmware half is complete. The remaining question is entirely Android-side. |

Android side, in the app's own diagnostics log: `rumble received L=… R=…` (zero/non-zero edges
only) proves the value crossed the link; `haptics bound <stage> (device=… ids=[…] hasVibrator=… external=… system=…)` names the actuator
that was selected and what every rejected stage reported; `vibration issued stage=… L=… R=…`
proves the API call was made without throwing. **That call returning is not proof the actuator moved** — `Vibrator.vibrate()` is
fire-and-forget. `adb shell dumpsys vibrator_manager` is the only thing that says whether the
service played it.

## Two structural gates found 2026-08-14 — both fixed, neither hardware-confirmed

Ranked by how completely each explains "zero rumble, ever". **Neither is proven on the Thor.**

### Gate 1 (leading) — IGNORED_BACKGROUND: the app was not a foreground process

`VibrationSettings.shouldIgnoreVibration()` checks caller process state **first, before any
setting**:

```
if (!isUidForeground(uid) && !BACKGROUND_PROCESS_USAGE_ALLOWLIST.contains(usage))
    return IGNORED_BACKGROUND;
// allowlist = {RINGTONE, ALARM, NOTIFICATION, COMMUNICATION_REQUEST,
//              HARDWARE_FEEDBACK, PHYSICAL_EMULATION}
```

`USAGE_MEDIA` is not on that list; nor is `USAGE_UNKNOWN`. `isUidForeground` needs process state
<= `IMPORTANT_FOREGROUND` (6). An Activity with the screen off is `TOP_SLEEPING` (12); one the user
switched away from is `CACHED_ACTIVITY` (16). The companion's manifest declared **no service at
all** — so during real play, with the user looking at a television, every single effect was dropped
before any setting was consulted, and `Vibrator.vibrate()` returned normally regardless. Nothing in
the app could observe it.

Fixed by `BridgeForegroundService` (`foregroundServiceType="connectedDevice"`), started when the
HID link comes up and stopped on every teardown path. `FOREGROUND_SERVICE` is process state 4,
which passes. This is independently correct anyway: an Activity-only Bluetooth HID bridge is
eligible for cached-process kill at any time.

Caveat, stated rather than papered over: the one `dumpsys` capture we have recorded
`ignored_for_settings`, which sits *after* the background check — so at that instant the app was
foreground and a different gate fired. Consistent with a foreground bench test hitting gate B while
real play hits gate A, but it means gate A has **inferential** support only.

### Gate 2 — wrong vibrator object

**The app was driving the phone's system vibrator. A gamepad's motors are not there.**

AOSP implements the two as disjoint stacks that share no lookup:

| | resolves ids through |
|---|---|
| `SystemVibratorManager` (what `Context.getSystemService` gives you) | `IVibratorManagerService` |
| `InputDeviceVibratorManager` (`InputDevice.getVibratorManager()`) | `InputManagerGlobal.getVibratorIds(deviceId)` |

There is no union view, so an input-device actuator can **never** appear in
`VibratorManager.getDefaultVibrator()`. `InputDevice.getVibrator()`'s own javadoc says the device
vibrator "may be different from the system vibrator". A wrong-object bug is binary, which is why
the symptom was zero rumble rather than weak rumble.

It also explains why the `USAGE_MEDIA` fix did not help even though it was correct on its own
terms: `InputDeviceVibrator.vibrate()` calls `InputManagerGlobal.vibrate()` and **discards
`VibrationAttributes` entirely**, so the settings gate that was eating our effects does not exist
on the path we should have been using.

Corroboration from mature implementations — all scope to the InputDevice:

| project | how it obtains the vibrator |
|---|---|
| Dolphin | `device.vibratorManager` (31+) / `device.vibrator`; system manager is a *separate* mapping target |
| Moonlight | `device.getVibratorManager()` → `device.getVibrator()` → system vibrator **only if `!isExternal()`** |
| RetroArch (current) | `InputDevice.getDevice(id).getVibrator()`; system service only when `id == -1` |
| RetroArch (2019) | system vibrator only — and libretro#10338 is the matching open "no rumble" bug |
| SDL | `device.getVibratorManager()` on 31+; system vibrator below 31 (known limitation) |

`getVibratorIds()` is the existence test on the device path — `InputDeviceVibrator.hasVibrator()`
is hardcoded `true` and carries no capability signal. On the single-vibrator path
`InputDevice.getVibrator()` returns a `NullVibrator` when absent, so there `hasVibrator()` IS
meaningful.

Second, independent hazard on the system path only: `USAGE_MEDIA` is **not** in AOSP's
`BACKGROUND_PROCESS_USAGE_ALLOWLIST`, so `VibrationSettings` returns `IGNORED_BACKGROUND` whenever
the app is not foreground. The device-scoped paths have no such restriction.

Selection now lives in the pure, unit-tested `HapticRouting.choose()` and the app logs one line
naming the chosen stage and what every other stage reported. An **external** controller with no
actuator deliberately resolves to `None` rather than buzzing the phone.

The decisive property is not style: `InputDeviceVibrator.vibrate()` routes to
`InputManagerService`, **not** `VibratorManagerService`, so the device path has **no background
gate, no `VIBRATE_ON` gate, no intensity gate and no ringer-mode gate**. For a bridge app that is
background by design, that is the only path whose behavior we control.

**Still UNKNOWN:** whether the Thor's `Odin Controller` InputDevice exposes any vibrator ids at
all. If it exposes none and `external=false`, the app correctly falls back to the system vibrator
and Gate 1 becomes the whole story. The `haptics bound` diagnostic line answers this without a
staged hardware test.

### Cheapest decisive checks, in cost order

1. `adb shell settings get system vibrate_on` — closes out the recorded `ignored_for_settings`.
2. Read the app's `haptics bound` line — says whether an InputDevice vibrator exists at all.
3. Reproduce with the app genuinely foreground and screen on, then `dumpsys vibrator_manager`. If it
   plays foregrounded but real play records `ignored_background`, Gate 1 is confirmed.

## Rules

- Do not tune amplitudes before the signal's existence has been established at the stage in
  question. Find where it disappears first.
- Do not describe a never-working path as unreliable, weak, or intermittent.
