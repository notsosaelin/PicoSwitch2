# Android Handheld Controller Bridge

Status: implemented under `android/companion/`; AYN Thor app-side input and public HID Device
registration hardware-validated 2026-08-13. App-led bond and Pico/console receipt remain open.

## Goal

Let an Android handheld use its built-in game controls as the one Bluetooth input controller for
PicoSwitch2. The app reads ordinary Android gamepad events, normalizes them, and presents the phone
to PicoSwitch2 as a standard Bluetooth HID gamepad.

The intended first-run experience stays inside the app except for Android-owned permission,
device-selection, and bond-confirmation overlays:

1. Open the app and tap **Connect adapter**.
2. Double-tap PicoSwitch2 BOOTSEL to open its explicit 30-second pairing window.
3. Approve Android's Nearby devices permission and select `Joypad Adapter` / `PicoSwitch2` in the
   system chooser launched by the app.
4. Approve the system bond prompt if Android shows one.
5. Return directly to the app, select the built-in controls, and tap **Play**.

No root, Shizuku, accessibility service, input injection, private Android API, or manual visit to
Bluetooth Settings is part of the design.

## Feasibility conclusion

This is feasible on Android 9 / API 28 and newer with the public
[`BluetoothHidDevice`](https://developer.android.com/reference/android/bluetooth/BluetoothHidDevice.html)
API. It lets a foreground app register a HID report descriptor, connect to a paired HID host, send
interrupt reports, and receive HID host callbacks. PicoSwitch2 already implements the other half:
a Classic Bluetooth HID host plus a descriptor-driven generic gamepad parser.

The two material compatibility risks were bounded on the intended handheld before the remaining
end-to-end pairing pass:

- The OEM Bluetooth stack must expose the Android HID Device profile. API level alone does not
  prove that `BluetoothAdapter.getProfileProxy(..., BluetoothProfile.HID_DEVICE)` will succeed on
  every vendor image.
- Registering the app with gamepad subclass `SUBCLASS2_GAMEPAD` may or may not change the OEM's
  Classic Class of Device. Source and host-test review confirms this does not block the intended
  Android-initiated HID connection: after HID opens, PicoSwitch2's no-match path binds the generic
  gamepad driver. It does block Pico-initiated inquiry, whose deliberately strict filter ignores
  phone/computer classes. Keep Android as the connection initiator and record the observed Class of
  Device during hardware validation.

These are bounded feasibility checks, not reasons to begin with a custom protocol.

## Constraints from the public Android APIs

| Constraint | Product consequence |
|---|---|
| `BluetoothHidDevice` was added in API 28 | Set `minSdk = 28`; older Android releases are unsupported. |
| The HID host must already be paired before `BluetoothHidDevice.connect()` | Initial setup includes an Android system bond/consent step. It can be launched by the app but cannot be silent. |
| Only one HID Device app can be registered at a time | Detect registration failure and explain that another HID-emulation app may be active. |
| Android automatically unregisters the HID app when it is no longer foreground | Keep the bridge Activity visible while playing. Treat backgrounding, screen lock, and process death as disconnects. |
| Registering HID Device disables Android's HID Host service until unregister | The first version targets built-in controls. Bluetooth gamepads connected to the handheld may disconnect while bridge mode is active. |
| Android 12+ Bluetooth operations require runtime Nearby devices permission | Request `BLUETOOTH_CONNECT`; request scanning only if the chosen association path needs it. No root bypass exists or is needed. |
| Application input arrives as `KeyEvent` and `MotionEvent` to the focused app | The app can read built-in game controls while its Activity is foreground. It cannot globally capture reserved system keys or another app's input. |

The Android sources supporting these decisions are the
[`BluetoothHidDevice` API contract](https://developer.android.com/reference/android/bluetooth/BluetoothHidDevice.html),
the [Bluetooth permission guide](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions),
the [Companion Device pairing guide](https://developer.android.com/develop/connectivity/bluetooth/companion-device-pairing),
and the [controller-input guide](https://developer.android.com/games/sdk/game-controller/controller-input).
Android's public HID Device API registers an SDP record and AOSP routes it through the
[HID Device profile interface](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/heads/main/android/app/jni/com_android_bluetooth_hid_device.cpp),
so Classic BR/EDR HID is the primary path. Do not assume the same API creates a BLE HOGP peripheral.

## End-to-end architecture

```text
built-in handheld controls
  -> Android KeyEvent / MotionEvent (foreground Activity)
  -> source selection + per-axis range/dead-zone normalization
  -> immutable canonical ControllerState
  -> fixed HID report encoder
  -> BluetoothHidDevice.sendReport()
  -> Classic Bluetooth HID interrupt channel
  -> PicoSwitch2 BTstack HID Host
  -> generic HID descriptor parser (bthid_gamepad)
  -> input_event_t / router / ns2_seam
  -> selected Nintendo USB personality
  -> console
```

The app is a thin transport bridge. It does not emulate a Switch controller, decode Switch 2
protocols, or change PicoSwitch2's console-facing behavior. PicoSwitch2 remains responsible for
the active Pro Controller 2, Joy-Con 2, NSO GameCube, or Switch 1 USB identity.

### Android project shape

The independent Gradle project lives under `android/companion/` so the descriptor and Pico parser
contract stay versioned together:

```text
android/companion/
  app/                         Compose UI and lifecycle
  bridge/input/                InputDevice discovery and event normalization
  bridge/hid/                  descriptor, report encoder, BluetoothHidDevice wrapper
  bridge/pairing/              CompanionDeviceManager, bond, saved-host state machine
  bridge/model/                immutable ControllerState
  test/                        encoder and mapping fixtures
```

Do not add a native library for the first version. Kotlin/Java public framework APIs cover the
whole data path and keep the compatibility boundary obvious.

## Local VCC prototype reference audit

The local Virtual Console Companion prototype was inspected read-only on 2026-08-11 as design
input. It is not a PicoSwitch2 dependency, was not built or executed for this review, and contains
many unrelated emulator, firmware, root, Shizuku, display, and transport experiments. Only its
small controller-state seam is relevant.

Reusable concepts, to reimplement narrowly rather than copy wholesale:

| VCC prototype concept | Use here |
|---|---|
| Immutable `NormalizedControllerState` | Keep Android/OEM input details out of the Bluetooth encoder. |
| Stable 14-button semantic enum | Match PicoSwitch2's existing sequential generic button order. |
| Selected-device router | Ignore unrelated keyboards/controllers and publish neutral when the selected source changes. |
| Four retained D-pad directions converted to one hat | Opposite directions cancel, and releasing one side restores the still-held side without inventing an edge. |
| Complete-state reports | A new axis event cannot accidentally release a held button. |
| Capacity-one latest-report mailbox | While a send is in flight, replace the pending state instead of queueing stale controller history. |
| Neutral on pause, disconnect, and source change | Make teardown behavior explicit and testable. |
| Kotlin golden vectors and state tests | Pin button positions, hat diagonals/opposites, axis endpoints, report size, and neutral output before hardware. |

Do not import these VCC choices into the primary path:

- VCC Link currently uses a private BLE GATT service and a 20-byte version/generation/sequence
  packet. That is an application protocol, not Android `BluetoothHidDevice`, despite one stale
  source comment calling its report a Bluetooth HID report.
- Its 32 ms heartbeat and 100 ms Pico watchdog solve stale state in that custom GATT protocol. The
  standard Classic HID path instead uses HID connection teardown plus PicoSwitch2's controller
  disconnect cleanup; add a heartbeat only if physical evidence requires one.
- Its `signedAxis()` assumes normalized `-1..1` input and a fixed `0.08` dead zone. The production
  bridge must use the selected device's `InputDevice.MotionRange` minimum, maximum, flat, and
  available-axis set.
- Its first controller event implicitly chooses the source device. The dedicated bridge should
  show an explicit one-time source selector and remember the stable device descriptor.
- Its BLE scan/GATT client, USB personality selector, emulator integration, Shizuku/root helpers,
  broad manifest permissions, and companion-display UI are outside this app's scope.

If the primary Classic HID feasibility gate fails on a real OEM build, the VCC prototype's
versioned complete-state packet, generation/sequence rejection, latest-only mailbox, neutral
watchdog, and Kotlin/C golden-vector pairing are useful evidence for the fallback GATT design. They
still require a fresh protocol review against PicoSwitch2 ownership and regression constraints.

## Retroid Pocket Classic source-device audit

A read-only ADB audit of the connected Retroid Pocket Classic (Android 14 / API 34) found the OEM
HID Device profile enabled, declared, running, and bound to Android's Bluetooth manager. Its built-in
`Retroid Pocket Controller` exposes the required sticks, triggers, D-pad, and standard gamepad
buttons. The full evidence and remaining ordinary-APK gates are in
[`docs/experiments/android-controller-retroid-pocket-classic-adb-audit-2026-08-11.md`](../experiments/android-controller-retroid-pocket-classic-adb-audit-2026-08-11.md).

Device-specific implementation requirements from that audit:

- Match the selected source using GAMEPAD/JOYSTICK capabilities plus name and vendor/product
  `0x2022:0x3001`; remember its stable Android descriptor. Do not reject it because
  `InputDevice.isExternal()` is true or because the OEM implements it as a virtual input node.
- Use `X/Y` and `Z/RZ` for the sticks. Choose one of each duplicate trigger pair
  (`LTRIGGER`/`BRAKE`, `RTRIGGER`/`GAS`) rather than processing both.
- Merge and deduplicate D-pad hat axes and key events.
- Treat Android-delivered key codes as authoritative because the OEM exposes a controller-style
  toggle. C/Z/Mode/App Switch need one live labeled-input pass before choosing Capture/Home policy.
- Measure neutral jitter before accepting the exceptionally small reported stick flat value
  (`15/32767`, about `0.00046`) as a sufficient dead zone.

## AYN Thor source-device audit (second device)

A read-only ADB audit of the **AYN Thor** (Android 13 / API 33, Qualcomm/AYN) on 2026-08-12
independently confirms the design against a different OEM and API level. Full evidence:
[`android-controller-ayn-thor-adb-audit-2026-08-12.md`](../experiments/android-controller-ayn-thor-adb-audit-2026-08-12.md).

- HID Device profile feasibility gate **passes** (`hid.device.enabled=true`, `HidDeviceService`
  declared/running/bound) — the same positive signal as the Retroid, on a different vendor/API,
  which is the cross-OEM variance the feasibility section required.
- Built-in `Odin Controller` (vendor/product `0x2020:0x0111`, `IsExternal:true`, virtual node,
  bus `0x0003`) — same selection considerations as the Retroid; it ships a **vendor-specific key
  layout** rather than `Generic.kl`, but its delivered key codes are standard.
- Sticks are `X/Y` + `Z/RZ` (matches the fixed contract). It exposes **only `GAS`/`BRAKE` triggers
  with no `LTRIGGER`/`RTRIGGER` aliases**, so it validates that the documented trigger fallback is
  mandatory, not optional. Same tiny `flat=15` stick dead zone.
- Its key map is a clean **superset of the 14-button contract**; only the Capture (usage 14) source
  remains a per-device question (candidates `BUTTON_C`/`BUTTON_Z`), the same open item as the Retroid.
- **No firmware/parser change required** — the Thor fits the already-checked-in descriptor + wire
  contract that `tools/test_bthid_android_controller.c` validates.

Two different handhelds now map cleanly onto the single fixed HID contract, which strengthens the
case for keeping one narrow descriptor rather than per-device profiles.

### AYN Thor live APK pass (2026-08-13)

The ordinary debug APK was then installed on that Thor. It discovered `Odin Controller`, rendered
live built-in input, acquired the public HID Device proxy, registered the canonical descriptor, and
reached Ready without root or Shizuku. The initial Pair action crashed because Android 13 requires
the manifest to declare `android.software.companion_device_setup` before
`CompanionDeviceManager.associate()`. The declaration is now present and synchronous OEM/framework
failures are caught and reported instead of terminating the Activity.

The device also has VCC's root `:input` daemon still running. It can register as Android's sole HID
Device application and displace/reject this app. That is an environmental conflict, not a need for
privilege in PicoSwitch Companion: stop the competing HID-emulation service before the final
chooser, bond, and console-input test.

## Pairing and connection state machine

Use
[`CompanionDeviceManager`](https://developer.android.com/reference/android/companion/CompanionDeviceManager.html)
to launch a filtered system chooser from the app. Match the current Classic name `Joypad Adapter`
and the migration-safe name `PicoSwitch2`. Companion association performs discovery on the app's
behalf, but it does not create the HID connection; after selection the app owns the bond check and
profile connection.

```text
UNSUPPORTED
  HID_DEVICE profile proxy unavailable

IDLE
  -> request Nearby devices permission
  -> acquire HID_DEVICE profile proxy
  -> registerApp(gamepad SDP + fixed descriptor)

SELECTING_ADAPTER
  -> launch CompanionDeviceManager chooser
  -> receive selected BluetoothDevice
  -> if BOND_NONE: createBond() and await BOND_BONDED

CONNECTING
  -> BluetoothHidDevice.connect(selectedPico)
  -> await onConnectionStateChanged(CONNECTED)

PLAYING
  -> capture selected InputDevice events
  -> encode/send complete state snapshots
  -> on pause, lock, profile loss, or disconnect: best-effort neutral report

STOPPING
  -> disconnect host
  -> unregisterApp()
  -> close profile proxy
```

Important ordering and UX rules:

- Register the HID app before initiating the final HID connection so Android has installed the SDP
  record and gamepad subclass.
- Require the user to explicitly open PicoSwitch2's pairing window. Do not weaken the adapter's
  pairing lock or silently admit a new phone.
- Check `BluetoothDevice.bondState` after companion selection. Association and Bluetooth bonding
  are related system flows but should not be treated as the same state in app logic.
- Track success through `onAppStatusChanged()` and `onConnectionStateChanged()`, not the immediate
  boolean return from `registerApp()` or `connect()`.
- Store the association/device address through Android's supported APIs. Never store or manage link
  keys in the app.
- On later launches, register the HID app and call `connect()` for the saved bonded Pico. No chooser
  or Settings visit should be needed while both sides retain the bond.

The primary flow has Android initiate the HID profile connection to the already selected Pico. It
does not request Android discoverability and does not depend on Pico inquiry finding the phone.
If a target OEM only completes first pairing in the opposite direction, record that as a device-
specific fallback experiment instead of silently adding `ACTION_REQUEST_DISCOVERABLE` to every run.

## Fixed HID contract

Start with one input report and no output or feature reports. A narrow descriptor makes both the
Android encoder and PicoSwitch2 parser deterministic.

| Field | HID usage | Encoding | PicoSwitch2 interpretation |
|---|---|---|---|
| Left X/Y | Generic Desktop `X`, `Y` | unsigned 8-bit, `0..255`, center `128` | left stick |
| Right X/Y | Generic Desktop `Z`, `Rz` | unsigned 8-bit, `0..255`, center `128` | right stick |
| L2/R2 | Generic Desktop `Rx`, `Ry` | unsigned 8-bit, `0..255`, rest `0` | analog triggers |
| Buttons 1..14 | Button page usages `1..14` | 14 bits plus 2 padding bits | face, shoulders, digital triggers, select/start, L3/R3, Home, Capture |
| D-pad | Generic Desktop Hat Switch | 4-bit `0..7`, neutral `8` | up/right/down/left combinations |

Use report ID `1`. The data passed to `BluetoothHidDevice.sendReport(device, 1, payload)` excludes
the ID; the Classic HID packet received by PicoSwitch2 includes it. The proposed payload is nine
bytes in descriptor order: six axes, two button bytes, then hat/padding. PicoSwitch2 should
therefore observe report ID at wire byte 0, axes at bytes 1..6, buttons at bytes 7..8, and the hat
at byte 9.

The canonical 81-byte descriptor and neutral wire report are checked into
[`tools/fixtures/android_controller_hid.h`](../../tools/fixtures/android_controller_hid.h).
`tools/test_bthid_android_controller.c` compiles that fixture against the production HID parser and
generic Bluetooth gamepad driver; the Android encoder must reproduce the same nine payload bytes.

Keep all six axes unsigned. PicoSwitch2's current generic parser scales a raw field against its
logical maximum and does not apply a signed logical minimum, so a conventional signed `-32768..32767`
descriptor would parse incorrectly even though it is valid HID.

### Canonical button order

Use the existing generic fallback's sequential map, which is already the repository's W3C order:

| HID usage | Android key | Canonical result |
|---:|---|---|
| 1..4 | `BUTTON_A`, `BUTTON_B`, `BUTTON_X`, `BUTTON_Y` | B1..B4 |
| 5..6 | `BUTTON_L1`, `BUTTON_R1` | L1/R1 |
| 7..8 | `BUTTON_L2`, `BUTTON_R2` or trigger threshold | digital L2/R2 |
| 9..10 | `BUTTON_SELECT`, `BUTTON_START` | select/start |
| 11..12 | `BUTTON_THUMBL`, `BUTTON_THUMBR` | L3/R3 |
| 13 | `BUTTON_MODE` when delivered to the app | Home |
| 14 | selected OEM capture/share key when delivered | Capture |

Do not add a general remapping UI in version one. The app normalizes OEM key/axis names into this
fixed contract; user remapping remains in the console's persistent controller settings, consistent
with the firmware policy. A small per-device compatibility table is acceptable only when a target
handheld proves that its built-in controller uses nonstandard Android key codes.

## Android input normalization

At startup and on `InputManager.InputDeviceListener` changes, enumerate devices supporting
`SOURCE_GAMEPAD`, `SOURCE_JOYSTICK`, or `SOURCE_DPAD`. Show a one-time source selector rather than
assuming device ID `0`, rejecting virtual-looking devices, or filtering on
`InputDevice.isExternal()`. The audited Retroid's physically built-in controller reports itself as
external and is backed by a virtual kernel input node.

Capture controller events at the Activity boundary before Compose focus navigation consumes them:

- Buttons: override `dispatchKeyEvent()` and maintain held state from down/up events. Ignore key
  repeat as a new transition.
- Axes: override `dispatchGenericMotionEvent()` and read every historical sample plus the current
  sample when useful for smoothness.
- Left stick: `AXIS_X` / `AXIS_Y`.
- Right stick: prefer `AXIS_Z` / `AXIS_RZ`, then fall back to `AXIS_RX` / `AXIS_RY` after inspecting
  the selected device's motion ranges.
- Triggers: prefer `AXIS_LTRIGGER` / `AXIS_RTRIGGER`, then `AXIS_BRAKE` / `AXIS_GAS`, with digital
  button fallback.
- D-pad: merge `AXIS_HAT_X` / `AXIS_HAT_Y` and `KEYCODE_DPAD_*` into one hat state.

Normalize using each `InputDevice.MotionRange` rather than assuming `-1..1`. Apply the reported
flat/dead-zone, clamp, then map sticks to `0..255` with `128` neutral and triggers to `0..255` with
`0` neutral. Provide a diagnostic screen showing raw and normalized values so reversed axes,
off-center sticks, missing releases, and reserved buttons are obvious before connecting.

Use a single serialized state owner on a dedicated `HandlerThread` or coroutine dispatcher. Input
callbacks update state; the sender coalesces axis motion to at most one complete report per 8 ms
while sending button edges promptly. Send full snapshots, not deltas. A low-rate unchanged-state
keepalive may be added only if hardware shows it is needed; the Pico router already retains the
latest controller state.

Graceful stop/background paths send a neutral report before teardown. Process death cannot run app
cleanup; that case relies on Android automatically unregistering the HID app, the resulting link
disconnect, and PicoSwitch2's disconnect cleanup. The hardware gate must prove that this path also
clears every held input.

## Scope boundaries

Version one includes:

- one Android handheld and one PicoSwitch2 adapter;
- built-in buttons, two sticks, D-pad, and analog triggers;
- app-led initial selection/bonding and saved-bond reconnect;
- a visible connection/input diagnostic screen;
- neutralization on stop/disconnect; and
- the adapter's existing console personalities without firmware remapping.

Version one deliberately excludes:

- root, Shizuku, accessibility capture, hidden APIs, or `/dev/input` access;
- background/global input capture while another Android app is focused;
- touch-screen virtual controls;
- motion sensors, touchpads, battery forwarding, audio, microphone, LEDs, and rumble;
- impersonating DualSense, Xbox, Switch, or Switch 2 hardware;
- BLE HOGP emulation; and
- multiple simultaneous source controllers.

Some handheld buttons may be reserved by the OS or vendor launcher (Power, Volume, Android Home,
and sometimes a vendor guide key). The app cannot promise to capture those without privileged
access. The diagnostic screen must report what Android actually delivers and the default map must
work without a guide/capture button if the OS withholds it.

## Implementation and validation path

### Phase 0 — one-device feasibility spike (partially hardware-complete)

Build the smallest possible debug Activity before designing the final UI:

1. Enumerate the built-in `InputDevice` and log every key, axis, range, flat value, source, and
   external/virtual classification.
2. Acquire the HID Device profile proxy and fail clearly if unavailable.
3. Register the fixed gamepad SDP/descriptor and log `onAppStatusChanged()`.
4. Pair/select Pico through the app-owned companion flow, call `connect()`, and log the callbacks.
5. Send neutral, one face-button press/release, and four stick extremes.
6. On Pico UART, capture the observed address, Class of Device, name, descriptor parse summary,
   selected `bthid_gamepad` driver, and normalized event values.

Final gate: do not claim completion until a real target handheld reaches the console with the correct face button,
centered sticks, four directions, and no stuck state, without opening Android Bluetooth Settings.

Pico-side preparation completed 2026-08-11: the canonical descriptor parses to a 10-byte wire
report with report ID 1, six axes at bytes 1..6, 14 buttons at bytes 7..8, and hat at byte 9. Host
coverage pins neutral/full-state parsing, diagonal hat, held-button preservation, wrong-ID and
truncated-report rejection, Classic phone-Class-of-Device fallback, and disconnect cleanup. This is
source/host validation only; it does not prove an OEM exposes Android's HID Device profile.

### Phase 1 — deterministic core (implemented)

- Add immutable `ControllerState`, the nine-byte encoder, and JVM golden tests.
- Pin neutral, every individual button, diagonal hats, stick centers/extremes, trigger endpoints,
  clamping, axis inversion, opposite-D-pad cancellation/recovery, and held-button preservation
  across unrelated axis changes.
- Add a capacity-one sender test proving that state 3 replaces pending state 2 while state 1 is in
  flight; no transport completion may release stale historic input after a newer snapshot exists.
- Add the lifecycle/pairing state machine with fake profile and bond adapters.
- Make every stop/error path generate a neutral state before teardown when the link still exists.

### Phase 2 — usable app (implemented; hardware pass in progress)

- Add source selection, raw/normalized live diagnostics, permission rationale, adapter chooser,
  connection state, and one large Play/Stop control.
- Keep the display awake while playing and make foreground-only operation explicit.
- Save only source identity/calibration and the Android companion association; do not create a
  second controller-remapping system.
- Add concise recovery actions for Bluetooth off, permission denied, bond removed, HID profile
  unavailable, app registration rejected, and adapter timeout.

### Phase 3 — hardware regression matrix

For each supported handheld/Android build, record:

- OS/API level, vendor build, Bluetooth controller, and whether HID Device proxy acquisition works;
- built-in input device identity and actual key/axis map;
- first pair entirely from the app flow and reconnect after both device power cycles;
- all buttons, diagonals, stick centers/extremes, trigger endpoints, simultaneous inputs, and
  press-then-background neutralization;
- 30-minute play, screen-lock/unlock, Bluetooth toggle, app kill/relaunch, and bond removal;
- measured input latency/jitter and whether an 8 ms send cap loses samples; and
- PicoSwitch2 input, rumble, motion, audio, wake, LED, BOOTSEL, Config, and bond behavior remaining
  unchanged when returning to a previously validated physical controller.

Build success and emulator tests do not validate Android HID Device support. The compatibility
claim is per physical handheld and firmware build.

## Fallback if public Classic HID Device fails

Only after Phase 0 records a real OEM failure, use a custom app-specific BLE GATT service:

```text
Android BLE advertiser + GATT server
  -> one versioned input-state characteristic
  -> new narrow PicoSwitch2 BLE source driver
  -> existing input_event_t / router path
```

This remains possible without root, but it is second choice because it adds a new protocol,
Android peripheral/advertising compatibility work, firmware code, pairing/reconnect policy, and a
new regression surface. It must not reuse Config-mode BLE or advertise during Config; controller
input and management lifecycles have different ownership. Define a version byte, complete-state
packets, sequence number, neutral-on-timeout rule, and host fixtures before any firmware flash.

Do not fall back to pretending the Android app is a DualSense/Xbox/Switch controller. A private
vendor protocol buys no value for this passthrough use case and makes pairing, output, and
compatibility substantially harder.

## First acceptance checklist

- [x] Target handheld exposes `BluetoothProfile.HID_DEVICE` to an ordinary app (AYN Thor API 33).
- [x] Built-in controls arrive through public `KeyEvent`/`MotionEvent` APIs while the app is focused
      (`Odin Controller` live panel hardware-confirmed).
- [ ] Initial adapter selection, bond consent, and HID connection complete without opening Settings.
- [x] Source/host fixture: PicoSwitch2 accepts an Android-initiated Classic HID connection with a
      phone Class of Device and selects `bthid_gamepad` (physical Android/Pico confirmation pending).
      Verified green in `tools/test_bthid_late_identity.c`; the phone binds generic via the
      no-match fallback even though `gamepad_match()` (correctly) rejects a phone major class.
- [x] Phone-CoD generic binding survives late Classic SDP VID/PID re-evaluation: no disconnect/
      rebind churn, only a VID-flag refresh, and input keeps flowing (`test_bthid_late_identity.c`).
- [x] The production parser fixture reports X/Y/Z/Rz/Rx/Ry, hat, report ID 1, and 14 buttons at the
      expected offsets (`tools/test_bthid_android_controller.c`, verified green).
- [x] Canonical reports round-trip through the production generic parser to the expected `input_event_t`.
- [x] Two independent handhelds (Retroid Pocket Classic API 34, AYN Thor API 33) pass the HID Device
      profile feasibility gate and map onto the fixed 14-button contract with no firmware change.
- [ ] A/B/X/Y, shoulders, D-pad, both sticks, both triggers, Start/Select, and stick clicks pass on console.
- [ ] App pause, screen lock, Bluetooth loss, and process death cannot leave a held input on console.
- [ ] Saved-bond reconnect works after restarting the app, PicoSwitch2, and handheld.
- [ ] Returning to a validated physical controller does not regress existing adapter behavior.
