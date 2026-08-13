# Minimal first hardware validation

Validated baseline on 2026-08-13: AYN Thor / Android 13 discovers the adapter, displays adapter and
live built-in-controller state, switches personality, and controls a real game through the Android
HID bridge and PicoSwitch2. The pass exposed inverted Nintendo-style face labels; Auto/Nintendo/
Xbox normalization is now implemented but awaits replay. Ordinary Amiibo
Sync's former `CRC Failed Verification` was a client bug caused by firmware's zero unavailable-CRC
sentinel. The fixed APK completed the same 540-byte Sync on the Thor with no error; it retains
structural validation and strict figure-v3 CRC checks.

1. Install `app-debug.apk`, open it, and grant Nearby devices.
2. Open **Settings -> Appearance**. Select System, Light, Dark, and OLED black once; confirm the
   controls remain readable after each change, the OLED background is true black, and the choice
   remains after closing/reopening the app. Select each inspired palette and confirm this changes
   only app accents; it must not send a firmware color command.
3. Open **Settings -> Developer**. Confirm Bluetooth available/enabled, both permissions
   true, Companion manager true, and management initially offline.
4. On a source-default image, confirm PicoSwitch2 advertises management without entering Config.
   First try **Pair Adapter** outside the pairing window and confirm a new bond cannot complete.
   Double-tap BOOTSEL, tap **Pair Adapter**, and select PicoSwitch2 in Android's chooser. Expect GATT
   `Connected`, firmware `2.0`, and populated Adapter/Amiibo state.
   Disconnect/reconnect outside the window and confirm the stored bond works. From Developer,
   issue `mgmt off` and confirm the current session closes and advertising stops; reboot the adapter
   and confirm production-default advertising returns. An unbonded/plaintext client must never
   execute a command.
5. Change to each output personality once. Expect a success message and **Identity refresh:
   Required**; confirm the console-facing USB controller returns after re-enumeration.
6. Change the active personality's controller color and save. Confirm it is marked pending, choose
   **Apply identity changes**, and verify the Switch 2 renders the new color after one brief USB
   reconnect. Confirm input, motion, rumble, audio (Pico 2 W), wake, and the management connection
   recover without a power cycle.
7. Import one owned 540/572-byte backup, load it, present/eject it, then make one console write and
   Sync. Expect dirty protection before Sync and a clean persisted state afterward. Reopen the app
   and confirm the local backup remains.
8. Close/reopen the app. Expect the saved adapter to reconnect without a chooser; if unavailable,
   expect **Reconnect** and **Pair another** to remain distinct actions.
9. On **Input**, choose the built-in controller, leave layout on **Auto**, and tap **Use this
   handheld**. There must be no second adapter chooser. Move every stick and trigger, D-pad
   diagonals, and all buttons. Confirm physical A/B/X/Y labels now match console actions, then check
   explicit Nintendo and Xbox modes. Expect the live panel and report counter to change and the
   console to receive the same inputs.
   Android permits only one registered provider. On AYN Thor, wait for the registration callback:
   its immediate API result can be false even when registration succeeds milliseconds later. The
   app handles that OEM behavior without root or Shizuku.
10. Rotate once while connected and once while the bridge is ready. Expect the destination,
   selection, colors, and bridge state to remain, with no repeated mutation.
11. If anything differs, open Settings -> Developer and **Share diagnostics**. Send the text file
   with the exact failed step; no Android Studio or logcat is required.

This checklist validates hardware behavior. JVM/emulator/build success does not replace it.

## Phone NFC physical gate (pending)

The phone-reader slice is host-tested but has not yet been run against a physical tag. On an
NFC-capable Android phone with NFC enabled and no adapter connection required:

1. Open **Amiibo**, tap **Scan**, and hold an ordinary NTAG215 Amiibo to the phone. Reader mode must
   arm only after that action; status must say it is waiting for an ordinary NTAG215.
2. A successful scan must issue the strict NTAG215 sequence and save exactly 540 bytes, or 572
   bytes when the tag returns an exact 32-byte `READ_SIG`. A missing/unsupported signature must be
   reported as an explicit 540-byte backup, never as zero padding.
3. Confirm the new item appears in the private library and that scanning the same bytes again reports
   a duplicate without creating another item. No adapter command or controller mode is involved.
4. Leave the app while it is armed, then return. Reader mode must be disabled on pause, no partial
   file may appear, and a new scan must require another explicit **Scan** tap.
5. If an ordinary tag with a bad UID manufacturer/BCC is available, confirm the scan is rejected and
   no library item is created. A figure-v3/NTAG I2C 2K tag must be explicitly rejected as unsupported;
   the app must not attempt sector-select, authentication, NDEF, or write commands.
6. Repeat with NFC disabled (or on a phone without NFC). The action must remain unavailable or report
   a clear reader-mode error; it must not claim a backup was saved.

This gate validates the phone's RF/tag boundary. JVM tests prove command order, strict parsing,
assembly, and atomic-library ordering; they do not prove antenna coupling, Android OEM reader-mode
behavior, or a physical tag's optional signature support.
