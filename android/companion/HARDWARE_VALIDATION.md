# Minimal first hardware validation

1. Install `app-debug.apk`, open it, and grant Nearby devices.
2. Open **More -> Developer / diagnostics**. Confirm Bluetooth available/enabled, both permissions
   true, Companion manager true, and management initially offline.
3. Arm PicoSwitch2 management using the documented physical Config/`mgmt on` path. Tap **Find
   adapter**. Expect GATT `Connected`, firmware `2.0`, and populated Adapter/Amiibo state.
4. Change to each output personality once. Expect a success message and **Identity refresh:
   Required**; confirm the console-facing USB controller returns after re-enumeration.
5. Import one owned 540/572-byte backup, load it, present/eject it, then make one console write and
   Sync. Expect dirty protection before Sync and a clean persisted state afterward. Reopen the app
   and confirm the local backup remains.
6. On **Controller**, choose `Odin Controller`, prepare the bridge, open the adapter's physical
   pairing window, and pair/select PicoSwitch2 in the in-app Android chooser. Move every stick and
   trigger, D-pad diagonals, and all buttons. Expect the live panel and report counter to change and
   the console to receive the same inputs.
7. Rotate once while connected and once while the bridge is ready. Expect the destination,
   selection, colors, and bridge state to remain, with no repeated mutation.
8. If anything differs, open diagnostics and **Share privacy-safe diagnostics**. Send the text file
   with the exact failed step; no Android Studio or logcat is required.

This checklist validates hardware behavior. JVM/emulator/build success does not replace it.
