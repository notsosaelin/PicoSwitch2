# Android relationship reliability gate (pending)

The 2026-08-20 relationship/GATT/color pass is **source-tested**, not physically closed. The older
AYN Thor / Android 13 baseline proved service discovery and the controller bridge, but it does not
prove the new association generations, callback-aware teardown, bounded 133 recovery, or automatic
color apply. Run the matrices below in order and record the exact starting Android bond, companion
association, app relationship, adapter LE bond list, firmware build, app build, and console state.

Do not delete an Android bond, disassociate a companion relationship, clear an adapter bond, install
an APK, or flash firmware unless the maintainer has explicitly chosen that destructive step.

## Diagnostics for every failure

Keep `adb logcat -s PicoSwitch` running when ADB is available, then export **Settings -> About ->
Diagnostics**. Android events include the logical attempt, reason, association presence, bond state,
GATT generation/status, retry count, close request, callback/timeout retirement, and stale callback.

For the controller-drop/solid-LED symptom, discover UART first with:

```powershell
tools\read_uart_diag.ps1 -List
```

Capture these immediately before management Disconnect and immediately after it:

```powershell
pwsh -File tools\uart_query.ps1 -Port <COM> -Command btstate
pwsh -File tools\uart_query.ps1 -Port <COM> -Command "input sources"
pwsh -File tools\uart_query.ps1 -Port <COM> -Command btdev
```

`btstate.cble` distinguishes the management peripheral link; `connections.*_ready` and `input
sources` distinguish physical-controller readiness/ownership; `owner_led.reason` explains the LED.
A solid LED alone is not evidence that trust was deleted or that management disconnected a
controller.

## Matrix A — clean first pairing

Only after an explicitly authorized clean-state setup:

1. Open the Pico's physical pairing window and tap **Pair Adapter** once.
2. Approve Android's system association/bond UI once.
3. Confirm the app stays in Pairing while Android reports `BOND_BONDING`, then reports Connected
   only after management identity/state populate.
4. Confirm one saved adapter and one app-owned companion association. Record the Android bond and
   adapter LE bond separately.

Pass: one logical app attempt, no duplicate GATT connection, no redundant chooser/prompt, and a
verified saved relationship.

## Matrix B — ordinary returning reconnect

Close and reopen the app without opening the Pico pairing window. Expect one direct automatic
attempt, no system pairing UI, a verified management session, and no parallel scan. If the adapter
is unavailable, expect a terminal Reconnect state rather than an infinite loop.

## Matrix C — explicit management disconnect and controller isolation

With an external physical controller already working, connect management, capture the diagnostics
above, tap **Disconnect**, and capture them again. The app must become offline with **Reconnect**;
the saved relationship, Android bond, companion association, adapter LE bond, physical controller,
active input, and Controller Bridge must not be intentionally changed. Record the exact LED reason.

## Matrix D — repeated reconnect

Repeat connect -> Disconnect -> Reconnect at least five times. Every old GATT must reach
`gatt.closed` before the next `connect.generation`. A transient 133 may receive one retry; it must
not create a storm, erase the relationship, or routinely require Android Settings.

## Matrix E — controlled transient failure

Only when explicitly authorized, interrupt the radio during one connect. Confirm one clean bounded
retry, then either recovery or one actionable terminal state. Preserve the attempt/status/close
sequence; status 133 is a symptom, not a root-cause finding.

## Matrix F — color

Commit one active-personality identity color once. Expect mutation, readback, completed persistence
when supported, automatic same-personality USB re-enumeration, and the new console color after one
brief controller pause. No second Apply press should appear. If USB refresh alone fails, expect
**Color saved; USB identity refresh still needs to be applied** and one **Retry** action.

## Matrix G — repair pairing

Only after explicitly authorized destructive Android bond manipulation, create a known missing/stale
bond state. Confirm **Repair pairing** explains the distinction, removes only app/CDM state it owns,
uses Android Bluetooth Settings only when the public API cannot remove the platform bond, and
returns to ordinary Pair Adapter afterward. It must not clear all adapter LE bonds.

## Matrix H — Controller Bridge coexistence

While the handheld is also the active Controller Bridge, connect and disconnect management. The HID
bridge must remain independent, input must continue, and diagnostics must attribute any solid LED
to the actual ready owner. Repeat once with a directly paired physical controller as owner.

These matrices validate hardware behavior. JVM/build success does not replace them.

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
