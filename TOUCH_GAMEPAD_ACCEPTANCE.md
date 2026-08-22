# Touch Gamepad — physical acceptance checklist

The controller surface itself is **confirmed rendering and responding** on the onn 8 Core tablet
(Android 16). Everything below the surface — what the console actually receives — is still
**unvalidated**, because the Controller Bridge cannot currently link from that tablet. See
[Known blocker](#known-blocker-controller-bridge-link-on-the-onn-tablet).

## Build under test

| | |
|---|---|
| Debug APK | `android/companion/app/build/outputs/apk/debug/app-debug.apk` |
| SHA-256 | `63d00e69e3e90c1339b1f2f7cc350ed574a5199018a7d6245a53a487512b8d0b` |
| Source | branch `ns2-testing`, working tree on top of `f6cbe41` (uncommitted — see below) |
| Bridge contract | 3, **unchanged** — the Touch Gamepad work itself needs no firmware flash |

### What is held, and why

The uncommitted tree is **two separable workstreams**, not a dirty tree. Verified 2026-08-22: the
Android establishment files contain zero touch references, and the Touch Gamepad files contain zero
Bluetooth-fix references, so neither depends on the other.

**A. Android establishment ownership** — held only because it ships in the same APK as (B).
Source-proven defect: `onServiceConnected()` re-registered and connected guarded solely by
`stopped`, which only `stop()` writes, so a profile rebind after a failed attempt began an
establishment nobody requested.

```
app/src/main/java/dev/picoswitch/companion/bridge/AndroidHidTransport.kt      (modified)
app/src/main/java/dev/picoswitch/companion/bridge/HidConnectionState.kt       (new)
app/src/main/java/dev/picoswitch/companion/bridge/HidEstablishmentPolicy.kt   (new)
app/src/test/java/dev/picoswitch/companion/bridge/HidConnectionStateTest.kt   (new)
app/src/test/java/dev/picoswitch/companion/bridge/HidEstablishmentPolicyTest.kt (new)
```

*Acceptance condition:* the app survives an Android Bluetooth profile-service restart without
starting an unrequested establishment generation. Note the 2026-08-22 Type A capture produces exactly
that trigger (`transport/HID profile: service disconnected`), so a Type A event during any campaign
is an opportunity to observe it.

**B. Touch Gamepad** — everything else in the uncommitted tree: `bridge-core` `touch/` and
`InputAuthority`, the app's `ui/touch/`, `TouchGamepadSettings`, `AndroidTouchFeedback`, the
`CompanionViewModel`/`ControllerScreen`/`CompanionApp`/`MainActivity`/`AndroidBridge`/
`AndroidInputBackend` wiring, `ControllerInputState`/`ControllerLayout`, the instrumentation tests,
`build.gradle.kts`, and the docs (`README.md`, `FEATURE_PARITY.md`,
`docs/bridge/PLATFORM_BACKEND.md`, this file).

*Acceptance condition:* this checklist, run on hardware.

Nothing learned about Type C invalidates either workstream: Type C is a firmware/BTstack encryption
race, and the Android app was specifically **exonerated** (26 connect requests produced exactly 26
`L2CA_ConnectReq`, strictly 1:1, with no app-initiated bonding calls).

Installed on the onn tablet at 2026-08-21 20:30:59.

## Known blocker: Controller Bridge link on the onn tablet

Measured over ADB on 2026-08-21. The Touch Gamepad opens and renders correctly; the **link** does
not complete:

```text
transport/HID registration: registered                       <- Android accepts the HID Device app
transport/HID connection: requested accepted=true bond=12 type=3
transport/HID connection state: connecting
bta_dm_acl_up   ... transport:BT_TRANSPORT_BR_EDR            <- Classic ACL comes up
transport/HID connection state: disconnecting                <- and never reaches connected
bta_dm_acl_down ... transport:BT_TRANSPORT_BR_EDR
transport/HID connection: callback timeout after 8003ms
```

`bond=12` is BONDED and `type=3` is DUAL, so the tablet believes it is properly paired. Registration
succeeds and `connect()` is accepted. The HID Device profile simply never reaches CONNECTED.

**This is not a Touch Gamepad defect** — the touch path ends in the same `ControllerState` the
physical path uses, and it never gets as far as mattering. It is the Controller Bridge's Classic HID
link.

### Superseded 2026-08-22 — read this before acting on the above

Two claims in this section were **refuted** by the 2026-08-22 investigation and are kept only so the
original reasoning stays legible:

- **"the link does not complete" is wrong as an absolute.** Automated cycling showed it completes
  most of the time: 25 cycles produced 15 successes and 10 failures. The symptom is intermittent,
  not a hard block.
- **"the adapter has no Classic link key for this tablet" is refuted.** In every captured failure
  `btm_sec_auth_complete` reports `status: 0` — authentication *succeeds* against a stored Classic
  link key. The adapter's `admission.reject_window` also never moved across 35 cycles, so the
  connection was never refused for missing trust. **Do not open a pairing window to fix this**; it
  would not have helped and it is not what is wrong.

The actual dominant cause is **Type C**: authentication succeeds, then both hosts start the LMP
encryption procedure and Android aborts the ACL on
`HCI_ERR_LMP_ERR_TRANS_COLLISION`. A candidate firmware fix exists and is awaiting flash — see
[`docs/experiments/controller-link-cycling-failure-2026-08-22.md`](docs/experiments/controller-link-cycling-failure-2026-08-22.md)
and open validation gate 12 in [`STATUS.md`](STATUS.md).

**Sequencing:** flash the Controller Link candidate (gate 12) *first*, then run this checklist. Doing
it in that order means a Touch Gamepad failure here is a Touch Gamepad failure, not Type C noise.

**Alternative if you want touch results sooner:** run this checklist on the AYN Thor instead, where
the Controller Bridge is already hardware-confirmed. The Thor also has physical controls, which makes
section 4 meaningful.

```powershell
adb install -r android\companion\app\build\outputs\apk\debug\app-debug.apk
```

## Already confirmed on hardware (onn 8 Core, Android 16)

- [x] Touch Gamepad opens full-screen, landscape, edge-to-edge, with the navigation chrome hidden
- [x] the layout resolves and fits (913 x 599 dp) with every control placed and legible
- [x] a custom background image loads and persists across launches
- [x] a link that cannot complete now fails in 8 s with a stated reason and a working **Retry**,
      instead of showing "Connecting" forever

If the APK has been cleaned away, rebuild it with:

```powershell
cd android\companion
.\gradlew :app:assembleDebug
```

## Getting there

1. Pair or reconnect the adapter as usual.
2. **Gamepad → Touch Gamepad.** No physical input source needs to be selected; that gate was
   removed for this path on purpose.
3. The controller opens immediately. If the link is not up yet it says so in a strip in the middle
   of the screen and starts it; input stays neutral until it is `Playing`.
4. If the adapter's active console source is not already this handheld, set it on the Gamepad page
   (Console input) as you would for the physical bridge.

The **MENU** control at the top opens settings and the way out. System back also exits.

---

## 1. Every control reaches the console

Tick each once, watching the console rather than the app:

- [ ] Face diamond: bottom, right, left, top — the letters drawn are what the console receives
      (default presentation is Nintendo: bottom is **B**, right is **A**, left is **Y**, top is **X**)
- [ ] `L`, `R`
- [ ] `ZL`, `ZR`
- [ ] D-pad Up, Down, Left, Right
- [ ] D-pad UpLeft, UpRight, DownLeft, DownRight
- [ ] Left stick: full circle, all the way to the rim
- [ ] Right stick: full circle, all the way to the rim
- [ ] `L3`, `R3`
- [ ] `-`, `+`
- [ ] `HOME`
- [ ] `CAP` (Capture)
- [ ] `C` (GameChat)

Then flip **Face buttons** to Xbox in the menu and confirm the drawn letters change **and** the
console receives the letter now drawn. (Held input is cleared when you change this — that is
deliberate.)

## 2. Multi-touch

- [ ] left stick + a face button
- [ ] left stick + `ZR`
- [ ] left stick + `ZR` + a face button
- [ ] both sticks at once, in different directions
- [ ] D-pad + a face button
- [ ] as many at once as your thumbs allow — nothing should drop out

Specifically watch for the stick **jumping or letting go** when another finger lands or lifts. That
is the failure this design is built against, and it is the one worth reporting in detail.

## 3. Stuck-input torture — release blocking

For each row: **hold a control down** (a stick pushed to the rim, or `ZR`), do the thing, then look
at the console. It must be neutral — no drift, no held button.

- [ ] press Android Home / switch apps
- [ ] lock the screen
- [ ] rotate the device
- [ ] swipe the system bars into view
- [ ] open the Touch Gamepad menu with a control still held
- [ ] open the background picker with a control still held
- [ ] disconnect the adapter (or walk out of range)
- [ ] reconnect — it must start from neutral, not resume what was held
- [ ] exit Touch Gamepad with a finger still down
- [ ] press back with a finger still down

If any row leaves the console holding an input, stop and record: which control, which row, and the
`Touch gamepad` + `Host input authority` + `Touch contribution` lines from
**Settings → About → Diagnostics → export**. That export names the boundary that failed.

## 4. Physical controller is unaffected

- [ ] with Touch Gamepad closed, the Thor's own sticks/buttons behave exactly as before
- [ ] open Touch Gamepad: the Thor's physical controls do nothing (expected — touch is authoritative)
- [ ] exit Touch Gamepad: the physical controls work again immediately, and rumble still reaches the
      handheld

## 5. Feel — play something for a few minutes

Pick something that needs movement and action at the same time. Watch for:

- [ ] face misfires (pressing one, getting its neighbour)
- [ ] D-pad chatter at a sector boundary
- [ ] deadzone too large (a dead patch at the centre) or too small (drift when resting)
- [ ] diagonals feeling faster than cardinals — they should not
- [ ] the edge back-gesture firing while reaching for `ZL`/`ZR`
- [ ] accidental `-`/`+`/`HOME` taps during play
- [ ] visible lag between thumb and knob
- [ ] local haptics being annoying (turn them off in the menu if so, and say)

The stick deadzone starts at 5% and is adjustable in the menu. **If it needs changing, tell me the
number that felt right** — the default is an engineering baseline, not a measured one, and this is
the one tuning value that genuinely needs a thumb.

## 6. Sustained use

A few minutes of continuous play, ideally with a background image set and haptics on:

- [ ] no crash or ANR
- [ ] the bridge does not drop
- [ ] the report counter keeps climbing (Diagnostics → export, `Reports`)
- [ ] the phone does not get unusually hot or drain unusually fast

---

## What to send back if something fails

The diagnostics export (**Settings → About → Diagnostics → the share action**) now carries three
lines written for exactly this:

```
Host input authority   Physical | Touch
Touch gamepad          active claimed=N unclaimed=N contested=N cancelled=N held=N releaseAll=N/REASON layoutFits=true
Touch contribution     TouchContribution(leftX=…, …)
```

Read left to right: `claimed=0` with contacts happening means the surface never saw them;
`unclaimed` high means they landed on nothing; `held` non-zero with no finger down is a stuck
control; `releaseAll=N/REASON` names the last boundary that cleared everything.
