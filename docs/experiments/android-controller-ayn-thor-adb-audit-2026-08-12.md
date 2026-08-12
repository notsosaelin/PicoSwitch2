# AYN Thor Android controller bridge ADB audit — 2026-08-12

Status: read-only source-device audit; no APK installed, Bluetooth setting changed, or pairing
attempted. Second device evidence for [`android-controller-bridge.md`](../bluetooth/android-controller-bridge.md),
complementing the [Retroid Pocket Classic audit](android-controller-retroid-pocket-classic-adb-audit-2026-08-11.md).
Purpose: attack the design's #1 stated risk — OEM Bluetooth-stack variance — with a second,
different vendor and API level, and confirm the fixed HID contract covers another real handheld.

## Device and Android stack

| Item | Observed value |
|---|---|
| Product | AYN Thor (`kalama` = Snapdragon 8 Gen 2) |
| Manufacturer / brand | AYN / qti |
| Android | 13, API 33 |
| Build | `Thor_V1.0.0.377_20260206_...`, user/release-keys, AYN version `01.001V01` |
| Bluetooth implementation | Qualcomm (`bluetooth_qti` family) |
| Root | Device is rooted (`su` available); audit kept read-only regardless |

**Cross-device contrast that matters:** Retroid = AYN 14 / **API 34**, Moorechip; Thor =
**API 33**, AYN. Both Qualcomm stacks. Two different OEMs and two API levels now show the same
positive feasibility signals, which is exactly the variance the design doc said must be proven.

## HID Device profile feasibility gate — PASS

- `bluetooth.profile.hid.device.enabled = true`;
- `dumpsys bluetooth_manager` lists `Profile: HidDeviceService` (alongside GATT/HeadsetService/
  A2dp/HidHost/Pan/Map/AvrcpController/Sap/Opp);
- `com.android.bluetooth/.hid.HidDeviceService` is **declared** for the
  `android.bluetooth.IBluetoothHidDevice` intent and observed **running and bound** to the system
  Bluetooth manager (active `ServiceRecord` + `ConnectionRecord`).

Same caveat as the Retroid: this proves the OEM image retains the HID Device implementation, not
that an ordinary APK's `getProfileProxy(..., HID_DEVICE)` + `registerApp()` succeeds. That remains
a Phase-0 APK gate — and per the maintainer, no APK work proceeds until the PicoSwitch2 side is
fully ready.

`CompanionDeviceManager` is present with no existing associations — fits the first-run app chooser.

## Built-in controller

InputManager exposes the built-in controls as **`Odin Controller`**:

| Property | Observed value |
|---|---|
| Name | `Odin Controller` (Device 13; a sibling `ODIN Station Virtual Mouse` is a separate MOUSE node) |
| Descriptor | `8e1073ea5832500672194344d81498833991c43c` |
| Bus / vendor / product | USB-style virtual bus `0x0003`, `0x2020`, `0x0111` |
| Sources | `KEYBOARD | GAMEPAD | JOYSTICK` |
| Android classification | `IsExternal: true`, controller number 1, kernel node `event9` |
| Key layout | **`/system/usr/keylayout/Vendor_2020_Product_0111.kl`** (vendor-specific) |

Like the Retroid, the controller is physically built in but Android marks it `IsExternal: true` and
backs it with a virtual input node. The selection rules already in the design doc hold verbatim:
select by GAMEPAD/JOYSTICK sources, present name + vendor/product, remember `InputDevice.descriptor`,
and **do not** reject `isExternal == true` or a virtual origin.

**New vs the Retroid:** the Thor ships a **vendor-specific key layout** (Retroid used `Generic.kl`).
That does not change the canonical contract — the mapped Android key codes below are standard — but
it is a reminder that key delivery is per-OEM and Android's delivered codes, not printed legends,
are the authority.

### Axes

| Android axis (rawAxis) | Raw range | flat | Use |
|---|---:|---:|---|
| `AXIS_X` (0), `AXIS_Y` (1) | `-32767..32767` | 15 (~0.00046) | left stick |
| `AXIS_Z` (2), `AXIS_RZ` (5) | `-32767..32767` | 15 (~0.00046) | right stick |
| `AXIS_GAS` (9), `AXIS_BRAKE` (10) | `0..32767` | 0 | right / left trigger |
| `AXIS_HAT_X` (16), `AXIS_HAT_Y` (17) | `-1..1` | 0 | D-pad |

Sticks are `X/Y` + `Z/RZ` — **matches the fixed HID contract's stick usages exactly.**

**Trigger difference (validates the documented fallback):** the Thor exposes **only `GAS`/`BRAKE`**,
with **no `LTRIGGER`/`RTRIGGER` aliases at all**. The Retroid exposed both alias pairs; the Thor
exposes only the fallback pair. This confirms the bridge's trigger-axis policy must be
"prefer `LTRIGGER`/`RTRIGGER`, then fall back to `BRAKE`/`GAS`" — on the Thor the fallback is the
*only* path. (`BRAKE` = left, `GAS` = right, per the Android convention.)

Same exceptionally small stick `flat=15` (~0.00046) as the Retroid — far below a conventional dead
zone. The Phase-0 debug APK must still measure real stationary jitter before trusting it.

### Keys (`Vendor_2020_Product_0111.kl`)

```
0x130 BUTTON_A   0x131 BUTTON_B   0x132 BUTTON_C   0x133 BUTTON_X
0x134 BUTTON_Y   0x135 BUTTON_Z   0x136 BUTTON_L1  0x137 BUTTON_R1
0x138 BUTTON_L2  0x139 BUTTON_R2  0x13a BUTTON_SELECT  0x13b BUTTON_START
0x13c BUTTON_MODE 0x13d BUTTON_THUMBL 0x13e BUTTON_THUMBR
0x66 HOME  0x9e BACK  0x244 APP_SWITCH  DPAD_UP/DOWN/LEFT/RIGHT (0x220–0x223)
```

This is a clean **superset of the 14-button contract** and maps with no new requirements:

| Contract usage | Thor key |
|---:|---|
| 1..4 | `BUTTON_A/B/X/Y` |
| 5..6 | `BUTTON_L1/R1` |
| 7..8 | `BUTTON_L2/R2` (digital; analog also via GAS/BRAKE) |
| 9..10 | `BUTTON_SELECT/START` |
| 11..12 | `BUTTON_THUMBL/THUMBR` (L3/R3) |
| 13 | `BUTTON_MODE` → Home |
| 14 (Capture) | **open** — candidates `BUTTON_C`, `BUTTON_Z`, or an OEM key |

`temp_abxy_layout_mode = 1` is set (same as the Retroid), i.e. the OS can swap ABXY — one more
reason the app must trust Android's delivered key codes over physical legends. Extra controls
beyond the 14 (`C`, `Z`, `BACK`, `APP_SWITCH`) exist but are not part of version one's fixed map;
`APP_SWITCH` in particular is likely intercepted by System UI.

## Impact on the PicoSwitch2 side (no firmware change needed)

The Odin Controller fits the existing checked-in contract — the same 81-byte descriptor / 10-byte
wire report and the sequential 14-button generic map that `tools/test_bthid_android_controller.c`
already validates against the production parser (all assertions pass, 2026-08-12). Nothing in this
audit requires a parser or descriptor change. It confirms:

- the fixed HID contract covers a second real handheld unchanged;
- trigger-axis selection genuinely needs the `BRAKE`/`GAS` fallback (Thor has no LTRIGGER/RTRIGGER);
- stick usages `X/Y` + `Z/RZ` are the right choice across both audited devices;
- the Capture (usage 14) source is still the one unresolved per-device question, deferred to a
  labeled-input pass in the eventual Phase-0 APK.

## Read-only reproduction commands

```powershell
adb shell getprop ro.product.model
adb shell getprop ro.build.version.sdk
adb shell getprop bluetooth.profile.hid.device.enabled
adb shell dumpsys bluetooth_manager
adb shell dumpsys activity services com.android.bluetooth
adb shell dumpsys input                       # find "Device N: Odin Controller" + MotionRanges
adb shell cat /proc/bus/input/devices          # Odin Controller = event9, vendor 2020 product 0111
adb shell su -c "cat /system/usr/keylayout/Vendor_2020_Product_0111.kl"
adb shell dumpsys companiondevice
adb shell settings get system temp_abxy_layout_mode
```

The `event9` node number is ephemeral; rediscover it from `/proc/bus/input/devices` or
`dumpsys input` before any future live event capture.

## Remaining physical gates (unchanged, APK-phase — not started per maintainer directive)

Same as the Retroid list: a minimal ordinary debug APK proving the HID Device proxy + `registerApp()`
callback; one labeled press/release of every physical control (resolve C/Z/Mode/Home/Capture
delivery); neutral-jitter and full-scale axis measurement; app-led association + Android-initiated
HID connect to PicoSwitch2; Pico UART identity/descriptor capture through the production parser; and
background/force-stop/screen-lock neutralization. Build/host success does not validate OEM HID
Device support — the claim stays per physical handheld and firmware build.
