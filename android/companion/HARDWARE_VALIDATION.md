# Minimal first hardware validation

Validated baseline on 2026-08-13: AYN Thor / Android 13 discovers the adapter, displays adapter and
live `Odin Controller` state, switches personality, and reaches HID bridge Ready. Ordinary Amiibo
Sync's former `CRC Failed Verification` was a client bug caused by firmware's zero unavailable-CRC
sentinel. The fixed APK completed the same 540-byte Sync on the Thor with no error; it retains
structural validation and strict figure-v3 CRC checks.

1. Install `app-debug.apk`, open it, and grant Nearby devices.
2. Open **More -> Developer / diagnostics**. Confirm Bluetooth available/enabled, both permissions
   true, Companion manager true, and management initially offline.
3. On a source-default image, arm PicoSwitch2 management using physical Config/`mgmt on`. A
   diagnostic `-MgmtOn` image is already armed for that boot. Tap **Find adapter**. Expect GATT
   `Connected`, firmware `2.0`, and populated Adapter/Amiibo state.
4. Change to each output personality once. Expect a success message and **Identity refresh:
   Required**; confirm the console-facing USB controller returns after re-enumeration.
5. Import one owned 540/572-byte backup, load it, present/eject it, then make one console write and
   Sync. Expect dirty protection before Sync and a clean persisted state afterward. Reopen the app
   and confirm the local backup remains.
6. On **Controller**, choose `Odin Controller`, prepare the bridge, open the adapter's physical
   pairing window, and pair/select PicoSwitch2 in the in-app Android chooser. Move every stick and
   trigger, D-pad diagonals, and all buttons. Expect the live panel and report counter to change and
   the console to receive the same inputs.
   Android permits only one registered provider. On AYN Thor, wait for the registration callback:
   its immediate API result can be false even when registration succeeds milliseconds later. The
   app handles that OEM behavior without root or Shizuku.
7. Rotate once while connected and once while the bridge is ready. Expect the destination,
   selection, colors, and bridge state to remain, with no repeated mutation.
8. If anything differs, open diagnostics and **Share privacy-safe diagnostics**. Send the text file
   with the exact failed step; no Android Studio or logcat is required.

This checklist validates hardware behavior. JVM/emulator/build success does not replace it.
