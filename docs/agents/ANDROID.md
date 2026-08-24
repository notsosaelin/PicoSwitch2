# Agent brief — Android companion bridge

Read [COMMON.md](COMMON.md) first. Full reference:
[`../bluetooth/android-controller-bridge.md`](../bluetooth/android-controller-bridge.md).

## Where the code is (changed 2026-08-15)

The companion is now **two Gradle modules**, and Android is one backend of a platform-neutral
bridge rather than the bridge itself:

| | Package | Android? |
|---|---|---|
| `:bridge-core` | `dev.picoswitch.bridge.{core,protocol,session,touch}` | **No.** No Android SDK on its classpath. |
| `:app` | `dev.picoswitch.companion.bridge` | Yes — the Android backend |

The old `dev.picoswitch.companion.controller` package is gone. Rough map:

| Was | Now |
|---|---|
| `AndroidInputRouter` | `AndroidInputBackend` (Android) + `ControllerInputState` (core) |
| `HidDeviceBridge` | `AndroidHidTransport` (Android) + `BridgeSession` (core) |
| `MotionSource` | `AndroidMotionBackend` |
| `BatterySource` | `AndroidBatteryBackend` |
| `HandheldHaptics` | `AndroidOutputBackend` |
| `AndroidControllerDescriptor` | `BridgeHidDescriptor` (core) |
| `ControllerFeedback` | `BridgeOutput` + `RumbleRequest` (core) / `BridgeOutputCodec` (core) |
| `MotionOrientation`, `MotionScale` | `ScreenOrientation`, `MotionScale`, `MotionConvention` (core) |
| `BridgePhase` | `BridgeLinkPhase` (`AcquiringProfile` renamed `Preparing`) |
| `ControllerAutoResumePolicy` | `SessionResumePolicy` (core) |

`AndroidBridge` assembles the four Android backends into the shared session. **Put nothing
Android-specific in `:bridge-core`** — it will not compile, and `ArchitectureGuardTest` also
rejects platform vocabulary in identifiers and strings. Contract:
[`../bridge/PROTOCOL.md`](../bridge/PROTOCOL.md), backend guide:
[`../bridge/PLATFORM_BACKEND.md`](../bridge/PLATFORM_BACKEND.md).

Touch profiles, immutable templates, sparse overrides, named profile libraries, composition, editor
operations, alignment/snapping, audit rules, schema metadata and the storage interfaces belong to
`:bridge-core`. Android owns only Canvas rendering, Compose pointer/editor UI, lifecycle integration
and the `SharedPreferences` backend. The module's path under `android/companion/` is historical; do
not create a duplicate shared layout module. `TouchLayoutOverrideJsonCodec` and
`TouchProfileLibraryJsonCodec` name the Kotlin JSON implementations explicitly—the document schemas
are portable, kotlinx.serialization is not a cross-platform API.

The layout editor's default profile is never stored: `TouchProfileLibrary` synthesizes it from the
shipped template on every read. Do not "fix" that by persisting it — its protection depends on it
not existing in storage.

## What it is

An ordinary no-root Android app (`android/companion/`) that registers as a Bluetooth HID **device**
using the public API-28+ `BluetoothHidDevice` profile and streams the handheld's built-in controls,
IMU and battery to PicoSwitch2, which remains the console-facing protocol owner. Reference hardware
is an **AYN Thor** (Android 13 / API 33, Qualcomm stack, built-in controls exposed as
`Odin Controller`).

## Versioning — no backward compatibility required

The maintainer is the only user. Firmware and app evolve together; each development release may
deprecate the previous one. **Do not** add compatibility shims, version negotiation, or preserve a
poor protocol decision because an older development build used it. Correctness and clean
architecture outrank development-build compatibility. Stable guarantees come later, at distribution.

## 🔴 Contract skew — check this FIRST

**Symptom: buttons and sticks work, but battery, motion AND rumble are all missing together.**
That is not three bugs. It is one: the flashed firmware and the installed APK implement different
bridge contracts, `android_bridge_identify()`'s exact 161-byte match failed, and the firmware fell
back to the v1 generic profile — which carries the v1 fields and nothing else.

Happened 2026-08-15: C/GameChat took the descriptor from 14 buttons + 2 pad bits to 15 + 1, the APK
shipped, the adapter kept older firmware. Source parity tests all passed — they compare source to
source and cannot see what is flashed.

Diagnose in one read: `pwsh -File tools/uart_query.ps1 -Port <COM> -Command bridge`

```text
"profile":"v2-bridge"   identification succeeded, all features enabled
"profile":"v1-generic"  it failed -- read first_mismatch / rejected / suspected_skew
"contract":3            what the FLASHED firmware implements
"build":"<hash>"        which build is actually on the adapter
```

The app compares the adapter's reported `bridge_contract` against `BridgeContract.VERSION` and
shows a mismatch on the controller-link card and in the diagnostics export. Firmware that reports
no contract is `UNVERIFIED`, never "compatible".

**Editing the descriptor? Bump `ANDROID_BRIDGE_CONTRACT_VERSION` and `BridgeContract.VERSION`.**
Rule and history: [`../bridge/PROTOCOL.md`](../bridge/PROTOCOL.md) §1.

## Wire contract

Single source of truth: `tools/fixtures/android_controller_hid.h`. The Kotlin encoder mirrors it and
is pinned by golden tests; the firmware identifies the bridge by an **exact** match on the
descriptor bytes, never by VID/PID (a handheld reports its phone identity).

INPUT report 1 (Android → Pico), 26 bytes:

| bytes | field |
|---|---|
| `0` | report ID (1) |
| `1..6` | X, Y, Z, Rz, Rx, Ry — sticks then triggers, 0..255 |
| `7..8` | buttons 1..**15** + 1 pad bit (15 = C / GameChat) |
| `9` | hat (low nibble, 8 = neutral) |
| `10..21` | gyro X,Y,Z then accel X,Y,Z, int16 LE |
| `22` | battery 0..100 |
| `23` | flags: `0x01` charging, `0x02` motion valid, `0x04` battery valid |
| `24..25` | motion timestamp, **100 µs ticks**, uint16 LE, free-running (wraps every 6.5536 s) |

OUTPUT report 2 (Pico → Android), 5 bytes: `[id][rumble L][rumble R][player LED 0..8][flags]`,
flags bit 0 = motion wanted.

Units are chosen to match what the seam already expects: **8192 counts/g** (the seam halves it to
the carrier's 4096) and **16.384 counts/dps** (1:1 end to end).

## Motion

The app publishes Android's standard sensor frame, **screen-orientation normalized**, and the
firmware's `SWITCH_MOTION_SOURCE_ANDROID` seam row is the identity — Android's frame
(X right, Y toward the top of the screen, Z out of the screen) is already the carrier frame. Two
independent derivations agree: the frame convention itself, and the composite transform used by
Dycool's NS-PC-Control. See
[`../bluetooth/android-motion-axis-derivation-2026-08-13.md`](../bluetooth/android-motion-axis-derivation-2026-08-13.md).

**The orientation normalization is the fragile part.** Android reports sensor axes in the device's
*natural* orientation, which on a handheld is often not how it is held. `ScreenOrientation` converts
to the current display frame; the transform table is the standard AOSP one and is unit-tested.

> **Trap, found 2026-08-14.** The rotation was read with `Context.getDisplay()` on the
> **application** context. On API 30+ that **throws `UnsupportedOperationException`** for any
> non-visual context — `runCatching` swallowed it and the cached value stayed 0 forever, so the
> correction silently never ran on any modern device. Now read via
> `DisplayManager.getDisplay(Display.DEFAULT_DISPLAY)`, which is valid from any context.
> On a natural-portrait handheld the old behavior is a 90° error about the screen normal:
> yaw survives, pitch and roll are exchanged and inverted. See [MOTION.md](MOTION.md).

The app registers sensors **on demand**, driven by the adapter's `motion wanted` flag, which the
firmware derives from the console's real negotiated IMU state (`ns2_motion_negotiated()`). Streaming
an IMU nothing reads is pure battery cost on a phone.

Motion timestamps must come from `SensorEvent.timestamp`, not from send time: the report cadence
(125 Hz) is faster than the IMU delivers, so the same physical sample is sent more than once and the
firmware de-duplicates on this field. Stamping at send time made every repeat look like a fresh IMU
frame.

Since 2026-08-14 the firmware also **integrates against this clock** instead of packet arrival
time, and the field is 100 µs ticks rather than milliseconds: at a 125 Hz cadence a 1 ms quantum is
12.5% of the interval, the same order as the Bluetooth arrival jitter the timestamp exists to
eliminate. See [MOTION.md](MOTION.md) for the measured cost of getting this wrong.

## On-screen console buttons

A handheld has no Home, Capture or C key, so those three are offered as touch controls that are
live only while the handheld is the active controller. They are ordinary buttons on the wire —
usages 13, 14 and 15 — and go through the same `ControllerInputState.setVirtualButton` path as any
physical key, not a side channel.

**C / GameChat (added 2026-08-15) needed no firmware change.** The generic sequential profile
already mapped usage 15 to `JP_BUTTON_A3`, which `NS2_BASE_BUTTON_MAP` index 18 routes to
`NS2_DST_C`, which `ns2_seam.c` raises as `SWITCH_EXTRA_C`. Only the wire contract and the app
changed: 14 buttons + 2 pad bits became 15 + 1, **inside the same two bytes**, so every later field
kept its byte offset and the descriptor length is unchanged at 161 bytes.

Two things to preserve if this area is touched again:
- `ControllerButton`'s **ordinal is the wire bit** (`1 shl ordinal`, read by the firmware as usage
  `ordinal + 1`). Append only; never reorder.
- The button field mask in `encodeCore` is `0x7F` on the high byte, not `0x3F`. A stale mask would
  silently drop C while everything else kept working.
- The companion has already normalized face usages 1–4 to logical A/B/X/Y. In `ns2_seam.c`,
  descriptor-proven `from_android_bridge` must keep selecting the face-only direct destination
  rule; applying `NS2_BASE_BUTTON_MAP` there reverses A/B and X/Y a second time. Directly paired
  controllers, every non-face bridge usage, and the raw JP bitmap remain unchanged.
- The exact bridge descriptor outranks the host phone/PC's incidental Bluetooth name and VID/PID.
  `bthid_gamepad.c` must force the sequential/no-extra generic parse profile for that descriptor,
  including after a late identity update; otherwise a controller quirk can corrupt the report
  before `from_android_bridge` reaches the seam.

### Unassigned physical buttons — durable rule

> **Unknown or additional physical controller buttons should be preserved as candidates for future
> custom mapping rather than silently assigned to unrelated controller actions.**

`KEYCODE_BUTTON_C` and `KEYCODE_BUTTON_Z` were routed to Capture until 2026-08-15. That was
arbitrary: they are extra physical buttons present on some handhelds and pads, not Capture keys. It
produced surprising behavior, and it consumed two inputs that the eventual custom button-mapping
system should own. **They are now unmapped and ignored.** Do not reassign them — including to C /
GameChat.

Capture therefore has no physical key by default, which is correct: neither audited handheld has a
dedicated Capture key, and Capture is reached through its on-screen button. Home keeps
`KEYCODE_BUTTON_MODE`.

`AndroidInputBackend.positionalButtonForKey()` is the whole table and is deliberately incomplete —
anything absent returns null and is ignored. `PhysicalKeyMappingTest` pins it, including a sweep
asserting that **no** physical key maps to Capture or C.

## Haptics binding

> **Check `Settings.System.VIBRATE_ON` first, always.** It was 0 on the Thor and accounted for the
> entire "no rumble ever" history: AOSP discards every vibration from every app when it is 0. The
> app now reports it in the `haptics bound` line. `adb shell settings get system vibrate_on`.

> **Never veto on `isExternal`.** The Thor's built-in controller is classified EXTERNAL by Android
> and exposes no vibrator of its own, so an isExternal guard removes the only actuator present.
> Already recorded in the 2026-08-12 ADB audit.


`AndroidOutputBackend.bindToSource(...)` must be called on **every** controller-selection change
(`AndroidBridge.selectSource` does both halves in one call so they cannot disagree): the
correct actuator belongs to that InputDevice, not to the application. Driving the phone's system
vibrator instead is the confirmed cause of the AYN Thor's total rumble silence — see
[RUMBLE.md](RUMBLE.md). Left amplitude goes to vibrator id 0 and right to id 1 when the device
exposes two, so the per-motor separation preserved all the way from the console finally reaches
hardware.

## Rumble / output

See [RUMBLE.md](RUMBLE.md). The firmware sends the feedback report only when a value changes, so the
app must not assume a refresh, and Android's vibrator service can discard effects for reasons that
are invisible to the API.

## Lifecycle facts worth keeping

- Android exposes **one HID Device slot per system**. Holding a registration across a dropped link
  makes the next attempt collide with this app's own orphaned record and report that another app
  owns the profile. Release on disconnect; re-acquiring is the normal resume path.
- The AYN Thor's OEM stack returns `false` from `registerApp()` and then delivers
  `onAppStatusChanged(registered=true)`. **The callback is authoritative**; treating the synchronous
  boolean as final leaves an accepted registration alive.
- An Android-initiated Classic HID connection reaches the firmware's generic fallback even when the
  OEM keeps a phone Class of Device. Pico-initiated inquiry deliberately rejects phone/computer
  classes, so **the app must initiate**.
- Adapter feedback arrives on the interrupt channel on most stacks and as a control-channel
  `SET_REPORT` on others; both are handled and the decoder tolerates either framing.
- The Thor exposes only `GAS`/`BRAKE` for triggers — no `LTRIGGER`/`RTRIGGER` aliases — so the
  fallback axis pair is the only path there.
- `temp_abxy_layout_mode` can swap ABXY. Trust Android's delivered key codes over printed legends;
  the app normalizes a persisted `Auto`/`Nintendo`/`Xbox` layout ahead of the HID encoder.

## Diagnostics

The app's diagnostic log records: HID registration and connection transitions, motion start/stop,
`motion frame — screen rotation Ndeg` (or `unreadable`), `rumble received`, `vibrator` capability,
and `vibration issued`. Debug builds mirror every entry to logcat. `adb` is available on the Thor
and the device is rooted; `dumpsys vibrator_manager` and `dumpsys input` have both already produced
decisive evidence and are cheap to ask for.
