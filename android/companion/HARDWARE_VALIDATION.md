# Minimal first hardware validation

Validated baseline on 2026-08-13: AYN Thor / Android 13 discovers the adapter, displays adapter and
live built-in-controller state, switches personality, and controls a real game through the Android
HID bridge and PicoSwitch2. The pass exposed inverted Nintendo-style face labels; Auto/Nintendo/
Xbox normalization is now implemented but awaits replay. Ordinary Amiibo
Sync's former `CRC Failed Verification` was a client bug caused by firmware's zero unavailable-CRC
sentinel. The fixed APK completed the same 540-byte Sync on the Thor with no error; it retains
structural validation and strict figure-v3 CRC checks.

1. Install `app-debug.apk`, open it, and grant Nearby devices.
2. Open **More -> Appearance**. Select System, Light, Dark, and OLED black once; confirm the
   controls remain readable after each change, the OLED background is true black, and the choice
   remains after closing/reopening the app. Select each inspired palette and confirm this changes
   only app accents; it must not send a firmware color command.
3. Open **More -> Developer / diagnostics**. Confirm Bluetooth available/enabled, both permissions
   true, Companion manager true, and management initially offline.
4. On a source-default image, arm PicoSwitch2 management using physical Config/`mgmt on`. A
   diagnostic `-MgmtOn` image is already armed for that boot. On a clean app install, tap **Pair
   Adapter** and select PicoSwitch2 in Android's chooser. Expect GATT
   `Connected`, firmware `2.0`, and populated Adapter/Amiibo state.
5. Change to each output personality once. Expect a success message and **Identity refresh:
   Required**; confirm the console-facing USB controller returns after re-enumeration.
6. Import one owned 540/572-byte backup, load it, present/eject it, then make one console write and
   Sync. Expect dirty protection before Sync and a clean persisted state afterward. Reopen the app
   and confirm the local backup remains.
7. Close/reopen the app. Expect the saved adapter to reconnect without a chooser; if unavailable,
   expect **Reconnect** and **Pair another** to remain distinct actions.
8. On **Controller**, choose the built-in controller, leave layout on **Auto**, and tap **Use this
   handheld**. There must be no second adapter chooser. Move every stick and trigger, D-pad
   diagonals, and all buttons. Confirm physical A/B/X/Y labels now match console actions, then check
   explicit Nintendo and Xbox modes. Expect the live panel and report counter to change and the
   console to receive the same inputs.
   Android permits only one registered provider. On AYN Thor, wait for the registration callback:
   its immediate API result can be false even when registration succeeds milliseconds later. The
   app handles that OEM behavior without root or Shizuku.
9. Rotate once while connected and once while the bridge is ready. Expect the destination,
   selection, colors, and bridge state to remain, with no repeated mutation.
10. If anything differs, open diagnostics and **Share privacy-safe diagnostics**. Send the text file
   with the exact failed step; no Android Studio or logcat is required.

This checklist validates hardware behavior. JVM/emulator/build success does not replace it.
