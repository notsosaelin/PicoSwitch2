# Android companion audio / microphone — CLOSED

Status: ⛔ **CLOSED — will not be implemented.** No viable transport exists within the project's
constraints.
Date: 2026-08-13 (closed same day it was investigated)
Related: [`android-controller-bridge.md`](android-controller-bridge.md)

## Decision

**The Android companion bridge does not and will not carry game audio or microphone.** Audio is the
one controller capability the bridge does not provide. This is a closed question, not deferred work.

## Constraints that close it

1. **WiFi is prohibited.** Maintainer decision, permanent: WiFi must never be enabled on this
   firmware. Any design that requires the CYW43's WiFi side is off the table regardless of its
   technical merit. This removes the only transport with enough bandwidth for PCM.
2. **Bluetooth HID cannot carry audio.** `BluetoothHidDevice` — the only no-root Android path — moves
   HID reports and nothing else. There is no audio channel in the profile, and smuggling PCM through
   report traffic is orders of magnitude short of the required bandwidth while destroying input
   latency.
3. **No non-root Android transport remains.** A2DP/HFP would require the phone to act in roles an
   ordinary foreground app cannot assume for live capture, and would additionally require the adapter
   to be both an A2DP sink and source. That fails the project's no-root, no-privileged-API rule for
   the app and adds a competing audio subsystem on the adapter.

With (1) and (2) both true, there is nothing left to evaluate. The earlier "measure CYW43 WiFi/BT
coexistence first" recommendation is withdrawn — it was predicated on WiFi being available, and it
is not.

## What the adapter's audio path actually is

Worth stating plainly to prevent this being reopened on a misunderstanding: the adapter's audio job
is **transmitting audio to the DualSense**. Console game audio arrives over USB (UAC1 speaker) and is
encoded to the DualSense over Bluetooth. There is no existing "receive audio from a wireless device"
path, and the Android bridge would have needed to create one.

## Reference note

[Dycool/NS-PC-Control](https://github.com/Dycool/NS-PC-Control) does support S2 audio/mic, using UAC1
on the console side and **UDP over WiFi** to its client. The console-facing half is directly
comparable to ours; the client-facing half depends entirely on WiFi and is therefore not applicable
here. Recorded so the difference is understood rather than re-derived.

## Retained, unrelated finding

Independent of the Android question, our UAC1 **microphone endpoint already exists and is
operational**, currently transmitting silence (`switch_pro2.c`, IF4 alt 1, EP `0x83`). That remains
relevant only to the separately tracked DualSense microphone-return work in PLAN.md; it is not a
stepping stone to Android audio, because the missing piece there was never the console side.
