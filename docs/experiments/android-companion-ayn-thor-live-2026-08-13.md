# Android companion live pass — AYN Thor — 2026-08-13

## Scope

Validate the ordinary no-root Android companion against an AYN Thor (Android 13 / API 33) and a
PicoSwitch2 concurrently connected to the Thor and Switch 2. No firmware was flashed during this
pass. ADB targeted the explicitly connected Thor only.

## Observed working behavior

- In-app BLE management connected and populated adapter state.
- Output personality changes completed successfully.
- The connected-controller card populated.
- The built-in `Odin Controller` (`VID 2020`, `PID 0111`) appeared as the selected source and its
  live inputs updated correctly.
- The adapter's loaded Amiibo was detected.
- After resetting a stale Android Bluetooth HID registration, the companion acquired the public
  `BluetoothProfile.HID_DEVICE`, registered its descriptor, and reached `Ready` / `Pair a
  PicoSwitch2 host` without root or Shizuku.

## Pair action crash

The first Pair action produced this Android 13 exception on the main thread:

```text
java.lang.IllegalStateException: Must declare uses-feature
android.software.companion_device_setup in manifest to use this API
  at android.companion.CompanionDeviceManager.associate(...)
  at dev.picoswitch.companion.MainActivity.pairControllerHost(...)
```

Root cause: the app called the public `CompanionDeviceManager` correctly but had not declared the
software feature Android checks synchronously. The manifest now declares the optional feature, and
the association call is guarded so an OEM framework rejection becomes a user-visible error rather
than a process crash. HID proxy acquisition and descriptor registration are guarded similarly.

## Competing HID Device provider

The Thor still runs the old VCC root input daemon as `app.vcc.companion:input`. Android's Bluetooth
service reported `registerApp(): failed because another app is registered`, matching the public
platform limit of one HID Device application at a time. Restarting Bluetooth briefly allowed this
companion to reach Ready, after which the legacy daemon could reclaim the slot.

This is a test-device conflict, not a PicoSwitch Companion privilege requirement. The final
chooser/bond/Pico-input pass must run with VCC's HID provider stopped. Do not add root, Shizuku, a
hidden API, or a custom protocol to compete for the slot.

## Amiibo Sync CRC failure

The app detected the adapter image but ordinary Sync failed with `CRC Failed Verification`.
Firmware zero-initializes `payloadCrc` for ordinary 540/572-byte images and only fills a whole-image
CRC for figure v3. Android incorrectly treated ordinary `00000000` as an asserted checksum.

The corrected client behavior is:

- validate supported size, UID/BCC structure, bounded chunks, and generation stability for every
  image;
- treat only ordinary `00000000` as CRC unavailable;
- require and compare figure-v3 whole-image CRC, including rejecting v3 zero; and
- acknowledge dirty data only after local bytes/index are durable and the generation (plus CRC
  when available) still matches.

`key_retail.bin` is not required to copy or Sync an already encrypted raw Amiibo image. It is a
phone/browser-local input for optional decrypt, owner/nickname metadata, initialization, and
re-signing. Keys and decrypted private data must never be sent to firmware diagnostics.

The rebuilt APK was installed over ADB and the same 540-byte adapter image was synced again. The
operation reached all 540 bytes and completed; Developer diagnostics reported the Amiibo command
complete with no last error. This closes the ordinary-image CRC regression on the tested Thor and
running adapter image. Figure-v3 remains covered by host tests rather than this physical pass.

## Stale connected-controller identity

After the DualSense powered off, the management `device` response continued publishing its old
name/VID/PID. The disconnect router neutralized input but did not clear the global identity. The
firmware fix clears identity in the same disconnect transaction, and Android now polls controller
state every five seconds and treats a blank zero/zero response as `No controller`.

This correction requires a newly built/flashed firmware image before the powered-off transition
can be hardware-validated. It does not change controller input, output personality, bonds, motion,
audio, or the console-facing USB path.

## Remaining physical gates

1. Stop VCC's competing HID Device provider without clearing unrelated application data.
2. Open PicoSwitch2's physical controller-pairing window.
3. Complete the app-launched Android chooser and bond prompt without visiting Bluetooth Settings.
4. Confirm Pico selects the generic gamepad parser and the Switch receives all Thor inputs.
5. Validate pause, process death, Bluetooth loss, saved-bond reconnect, and return to a physical
   controller with no stuck input or regression.
6. Flash a build containing the identity-clear fix and confirm the controller card becomes
   `No controller` within one five-second poll after power-off.
