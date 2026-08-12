# Retroid Pocket Classic Android controller bridge ADB audit — 2026-08-11

Status: read-only source-device audit; no APK installed, Bluetooth setting changed, or pairing
attempted by this investigation.

## Device and Android stack

| Item | Observed value |
|---|---|
| Product | Retroid Pocket Classic (`parrot`) |
| Manufacturer / platform brand | Moorechip / qti |
| Android | 14, API 34 |
| Build | `UKQ1.240624.001`, user/release-keys, 2024-07-05 security patch |
| Bluetooth implementation | Qualcomm `libbluetooth_qti.so` |
| Local Bluetooth name | `RPClassic` |

The primary public-API feasibility signals are all present:

- `bluetooth.profile.hid.device.enabled=true`;
- `com.android.bluetooth/.hid.HidDeviceService` is declared for
  `android.bluetooth.IBluetoothHidDevice`;
- `dumpsys bluetooth_manager` lists `Profile: HidDeviceService`; and
- ActivityManager shows that service running and bound to the system Bluetooth manager.

This is strong evidence that the OEM image retains Android's HID Device implementation. It is not
yet the final public-app proof: a small ordinary APK still must call
`BluetoothAdapter.getProfileProxy(..., BluetoothProfile.HID_DEVICE)`, register the canonical SDP
record, and receive `onAppStatusChanged()`.

Companion Device Manager is present and reported no existing associations during the audit. That
fits the proposed first-run app chooser; the audit did not create an association.

## Built-in controller

InputManager exposes one controller:

| Property | Observed value |
|---|---|
| Name | `Retroid Pocket Controller` |
| Descriptor | `dc75afea56e3c3a269b97967aa26b8c93c0bd3fb` |
| Bus / vendor / product | USB-style virtual bus `0x0003`, `0x2022`, `0x3001` |
| Sources | `KEYBOARD | GAMEPAD | JOYSTICK` |
| Android classification | `IsExternal: true`, controller number 1 |
| Kernel origin | `/devices/virtual/input/input17`, event node observed as `/dev/input/event7` |
| Key layout | `/system/usr/keylayout/Generic.kl` |

The controller is physically built in but Android marks it external and implements it as a virtual
input node. The app must not reject `isExternal == true`, require a non-virtual kernel origin, or
remember the transient Android device/event number. Select by gamepad/joystick sources and present
the name plus vendor/product to the user; remember `InputDevice.descriptor` with a name/vendor/
product fallback.

### Axes

| Android axes | Range | Use |
|---|---:|---|
| `AXIS_X`, `AXIS_Y` | `-1..1` | left stick |
| `AXIS_Z`, `AXIS_RZ` | `-1..1` | right stick |
| `AXIS_LTRIGGER`, `AXIS_BRAKE` | `0..1` | duplicate views of left trigger |
| `AXIS_RTRIGGER`, `AXIS_GAS` | `0..1` | duplicate views of right trigger |
| `AXIS_HAT_X`, `AXIS_HAT_Y` | `-1, 0, 1` | D-pad |

The raw stick range is `-32767..32767` and advertises `flat=15`, exposed by InputManager as only
about `0.00046`. That value is much smaller than a conventional fixed dead zone. The debug APK must
record stationary jitter and full travel before deciding whether the reported flat is sufficient;
do not silently import VCC's fixed `0.08` value. Triggers rest at zero and use raw `0..32767`.

InputManager exposes both trigger aliases. The state owner should select one member of each alias
pair, not process both as independent controls.

### Keys

The Generic Android key layout exposes:

- `BUTTON_A/B/X/Y`, `BUTTON_C/Z`;
- `BUTTON_L1/R1/L2/R2`;
- `BUTTON_SELECT/START`, `BUTTON_MODE`, and `BUTTON_THUMBL/THUMBR`;
- D-pad key events in addition to the hat axes; and
- OEM/system-facing `MOVE_HOME`, `BACK`, volume, and `APP_SWITCH` keys.

The bridge can directly fill canonical HID usages 1 through 13 from A/B/X/Y, shoulders/triggers,
Select/Start, stick clicks, and Mode. Capture/share (usage 14) needs a physical event test: `C`, `Z`,
or an OEM key may be suitable, while `APP_SWITCH` may be intercepted by System UI. System settings
show `temp_abxy_layout_mode=1` and a `controllerStyle` quick-settings tile, so Android's delivered
key codes—not printed shell labels or assumed physical legends—must be the mapping authority.

Merge D-pad key and hat sources into one retained four-direction state. Deduplicate them so one
physical press cannot create two transitions.

## Read-only reproduction commands

```powershell
adb devices -l
adb shell getprop ro.build.version.sdk
adb shell getprop bluetooth.profile.hid.device.enabled
adb shell dumpsys bluetooth_manager
adb shell dumpsys activity services com.android.bluetooth/.hid.HidDeviceService
adb shell dumpsys input
adb shell getevent -lp /dev/input/event7
adb shell cat /proc/bus/input/devices
adb shell cat /system/usr/keylayout/Generic.kl
adb shell dumpsys companiondevice
```

The event number is ephemeral; rediscover it from `dumpsys input` before any future live event
capture.

## Remaining physical gates

1. Install a minimal ordinary debug APK and prove the HID Device profile proxy and `registerApp()`
   callback, without root or privileged permissions.
2. Capture one labeled press/release of every physical control through Activity `KeyEvent` and
   `MotionEvent`; specifically resolve C, Z, Mode, Home, and App Switch delivery.
3. Measure neutral jitter, axis direction, trigger alias behavior, and full-scale endpoints.
4. Complete app-led association/bonding and Android-initiated HID connect to PicoSwitch2.
5. Capture Pico UART identity/descriptor data and run neutral, every button, hats, and axes through
   the already checked-in production parser contract.
6. Prove background, force-stop/process death, Bluetooth toggle, and screen lock all produce a
   normal HID-close cleanup or otherwise neutralize held state before claiming lifecycle safety.
