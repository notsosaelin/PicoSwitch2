# Controller Link explicit-versus-Touch differential (2026-08-25)

## Result

**Status: no meaningful Bluetooth-establishment path difference found.**

The explicit **Use this handheld** action and standalone **Touch Gamepad** action
reach the same `BridgeSession.start()` and `AndroidHidTransport.start()` path.
Standalone Touch binds the Touch source about 5-6 ms before asking for that same
Controller Link, but it does not start report transmission before HID readiness,
does not issue a second management command, and did not issue a duplicate Classic
connect in the controlled traces.

The apparent reliability differential came from a historical-labeling error. The
retained 500-cycle "Controller Link" workload did tear down and recreate the
Classic ACL on judgeable cycles, but the UI action it exercised was standalone
Touch Gamepad. It was not an independent explicit-activation control. The
repository therefore contains no evidence for "zero failures in approximately
1,000 explicit fresh Classic establishments."

This result explains the apparent path differential. It does **not** establish a
new root cause for the separately observed `PAGE_TIMEOUT` and post-`page_accept`
HCI `0x08` device/radio failures.

## Provenance and constraints

- Branch: `ns2-testing`
- Source HEAD: `6eb79d2a4c5d`
- Installed app: `dev.picoswitch.companion.debug` 2.0.0 (`versionCode=20000`)
- Android device: AYN Odin 2 Mini, Android 13
- Adapter: Pico 2 W / CYW43439, Pro Controller 2 personality
- Adapter-reported firmware identity: `d767674d+dirty`, bridge contract 3
- Android collection transport: `192.168.68.56:43331`
- Adapter UART: `COM11`
- Bluetooth pairing was preserved. No APK replacement and no UF2 flash occurred.
- The adapter USB/UART cable was intermittently loose and reset the adapter
  counters outside controlled collection. Those intervals and an
  automatic-resume attempt after management reconnect were excluded before the
  controlled A/B comparison.
- The worktree contained pre-existing modified, deleted, and untracked work.
  It was preserved.

Android timestamps below are `logcat -v epoch` wall time. Adapter timestamps are
milliseconds since its boot. Each cycle is internally ordered on the adapter
clock; Android `STATE_CONNECTED` was used as the cross-clock alignment point for
the latency comparison.

## Verified historical soak semantics

The retained automation is `tools/controller_link_cycle.py --workload A`.
Despite its Controller Link name, workload A performs this UI sequence:

1. Open the app's Controller page.
2. Invoke `app_enter_touch()` and tap **Touch Gamepad**.
3. Wait for the adapter's Classic-ready state.
4. Exit Touch and tap **Stop playing**.
5. Require the adapter's Classic link to become inactive before the next cycle.

The pre-2026-08-24 Touch UI used Back to call `exitTouchGamepad()` directly, so
the historical runner's Back action did leave Touch. The subsequent **Stop
playing** action and adapter `classic_ready=0` gate made each judgeable next
attempt a fresh ACL establishment rather than a logical-source toggle over a
retained link.

Relevant Git and document evidence:

- `f1561b8`: retained 500-cycle workload-A run. Cycle 491 was an ADB transport
  loss, not a Bluetooth establishment result.
- `fdebfb2`: 100-cycle workload-A campaign with nine `PAGE_TIMEOUT` results.
- `28cb397`: 40-cycle workload-A campaign with a post-page HCI `0x08` result.
- `32c5512`: a 200-cycle workload-A run stopped at cycle 82 by host decoding.
- `docs/experiments/controller-link-cycling-failure-2026-08-22.md` and
  `CYW43439_Bluetooth_Investigation.md` describe the same Touch-driven workload,
  including the already-closed inquiry-suppression A/B experiment.

Thus the fresh-ACL part is verified for judgeable retained cycles, but the
"explicit Controller Link" interpretation is false. No second retained
approximately-500-cycle artifact establishes an explicit-path denominator.

## Source-path comparison

### Path A: explicit Controller Link

1. `ControllerScreen` handles **Use this handheld**.
2. `MainActivity.requestControllerBridge()` checks/requests Android permission.
3. `CompanionViewModel.acquireControllerBridge()` calls
   `session.start(knownControllerHost())`.
4. `BridgeSession.start()` publishes the pre-link phase and calls the one
   `BridgeTransport.start()` instance.
5. `AndroidHidTransport.start()` records `HidEstablishmentIntent.Wanted`, stores
   the requested host, and publishes `Preparing`.
6. It obtains the `BluetoothProfile.HID_DEVICE` proxy, registers the HID Device
   app, receives the registered callback, and calls `beginConnect()`.
7. `beginConnect()` publishes `Connecting` and invokes exactly one
   `BluetoothHidDevice.connect(target)` in each observed attempt.
8. Android callbacks progress through `STATE_CONNECTING` and `STATE_CONNECTED`.
9. Transport link-up reaches `BridgeSession.onLinkUp()`. Only then does the
   report sender start, the first report get sent, and the UI reach `Playing`.

### Path B: standalone Touch Gamepad with no Controller Link

1. `MainActivity.openTouchGamepad()` calls
   `CompanionViewModel.enterTouchGamepad()`.
2. `AndroidBridge.enterTouchMode()` remembers the prior source, neutralizes any
   old source (with no transport send while unlinked), activates the Touch
   backend, binds the session source and capabilities, and applies the layout.
3. `openTouchGamepad()` reads the current bridge phase. Because it is not active,
   it calls the **same** `MainActivity.requestControllerBridge()` used by Path A.
4. Permission handling, `acquireControllerBridge()`, `BridgeSession.start()`, HID
   proxy acquisition/registration, `beginConnect()`, callbacks, link-up, sender
   start, first report, and `Playing` then follow Path A exactly.

The Touch source mutation is the only pre-request source-path difference found.
It is synchronous and local. Existing management polling is suppressed during
`Preparing`, `Registering`, and `Connecting`; no Touch-specific GATT command or
parallel Bluetooth transport exists.

### Path C: Touch after Controller Link already exists

`openTouchGamepad()` still activates and binds the Touch source. The bridge
phase is already active, so it does not call `requestControllerBridge()`.
The existing HID link, session, and sender are reused immediately. There is no
new profile registration, `connect()`, page, ACL, or HID-ready transition.

### Separate lifecycle entry

`MainActivity.onResume()` can call `requestAutomaticControllerResume()`. After
reading the adapter input state, this may call the same `session.start()` path.
It established a link before a UI action during one reconnect/setup attempt, so
that attempt was excluded and the link was torn down before A/B collection.
No overlapping auto-resume/UI request occurred in the six controlled cycles.

## Live comparison method

Every counted Path A and Path B cycle began with:

- BLE management connected and responsive;
- `classic_raw=0`, `classic_ready=0`, and no pending teardown;
- adapter personality `pro2`, stable power, and a closed pairing window;
- prior teardown confirmed as HCI reason `0x13`;
- adapter Bluetooth-lifecycle event ring cleared before the user action; and
- the same installed APK, firmware, bond, and physical devices.

Three fresh cycles were collected per path, as requested. No long or
failure-hunting soak was run.

## Timestamped trace results

`connect->page_rx` aligns the adapter page receive to Android's connect request.
`page_rx->ACL` and `page_rx->HID` use the adapter clock directly.

| Path | Cycle | Android sequence (epoch seconds) | Adapter sequence (boot ms) | connect->page_rx | page_rx->ACL | page_rx->HID |
|---|---:|---|---|---:|---:|---:|
| Explicit | 1 | Preparing 8493.797; connect 8493.930; connected 8497.148; link 8497.161; report 8497.175 | page 164389; ACL 164558; enc 164840; HID 165762 | 1.845 s | 0.169 s | 1.373 s |
| Explicit | 2 | Preparing 8581.263; connect 8581.395; connected 8585.834; link 8585.843; report 8585.852 | page 252992; ACL 253451; enc 253724; HID 254384 | 3.047 s | 0.459 s | 1.392 s |
| Explicit | 3 | Preparing 8603.121; connect 8603.236; connected 8607.523; link 8607.538; report 8607.542 | page 274598; ACL 274850; enc 275115; HID 276073 | 2.812 s | 0.252 s | 1.475 s |
| Touch | 1 | source 8663.153; Preparing 8663.159; connect 8663.284; connected 8670.197; link 8670.207; report 8670.214 | page 336683; ACL 336980; enc 337800; HID 338743 | 4.853 s | 0.297 s | 2.060 s |
| Touch | 2 | source 8785.875; Preparing 8785.882; connect 8786.004; connected 8789.735; link 8789.754; report 8789.763 | page 456923; ACL 457159; enc 457429; HID 458284 | 2.370 s | 0.236 s | 1.361 s |
| Touch | 3 | source 8816.604; Preparing 8816.610; connect 8816.731; connected 8820.984; link 8820.996; report 8821.002 | page 488236; ACL 488469; enc 488804; HID 489526 | 2.963 s | 0.233 s | 1.290 s |

The shortened epoch values omit the common leading `178766` for readability.
Median connect-to-page time was 2.812 s explicit and 2.963 s Touch. Median
page-to-HID time was 1.392 s explicit and 1.361 s Touch. Touch cycle 1 was a
slower successful page/HID establishment, but it did not contain a different
operation or ordering.

| Observation | Explicit (3/3) | Standalone Touch (3/3) |
|---|---:|---:|
| HID proxy/app registration before connect | yes | yes |
| Source bound before connect | no source change | yes, 5-6 ms before `Preparing` |
| `BluetoothHidDevice.connect()` calls per cycle | 1 | 1 |
| Page receives / ACL ups / HID-ready events per cycle | 1 / 1 / 1 | 1 / 1 / 1 |
| First outgoing report before HID ready | no | no |
| Touch-specific management command near connect | no | no |
| Auth/encryption collision or HCI recovery | no | no |
| Normal teardown verified | 3/3 | 3/3 |

For Path C, Touch source binding occurred at epoch `1787668906.583` and the
Touch screen opened at `.584`. The adapter event ring remained empty, the same
Classic handle `0x000B` stayed ready, and Android emitted no bridge phase,
registration, connect, or link-up transition. Later periodic management reads
were ordinary post-activation polling, not establishment overlap.

## Hypothesis disposition

| Hypothesis | Result |
|---|---|
| H1: different Controller Link state-machine entry | Rejected: both UI paths call the same `requestControllerBridge()` and session/transport owner. |
| H2: Touch initialization overlaps the request | Source setup precedes it by milliseconds, but is synchronous/local; no causal Bluetooth overlap observed. |
| H3: bridge/report activity starts before HID ready | Rejected by source and all six traces. |
| H4: Touch GATT traffic overlaps Classic establishment | Rejected in controlled traces; polling is suppressed in establishment phases. |
| H5: Touch bypasses an explicit readiness gate | Rejected: permission, registration, transport, and readiness gates are shared. |
| H6: duplicate/overlapping Touch connect | Rejected for these paths: exactly one call/page/ACL per cycle. |
| H7: explicit leaves HID pre-prepared | Rejected: both paths registered before every counted connect. |
| H8: historical cycles retained the ACL | Rejected for judgeable cycles; they were fresh. The broader comparison is still invalid because those cycles were Touch, not explicit. |
| H9: teardown overlap | Excluded by the common initial condition; not present in any counted attempt. |
| H10: no meaningful path difference; sampling/artifact | Supported. The historical control was mislabeled and current Bluetooth-boundary traces match. |

## Changes and validation

No production Android or firmware behavior was changed.

`tools/controller_link_cycle.py` received diagnostic-harness maintenance:

- `--adb-serial` pins every ADB operation to one transport when the same device
  is visible twice;
- bounded vertical scrolling reaches the current Touch and exit controls;
- Touch exit supports both historical Back behavior and the current explicit
  **Exit Touch Gamepad** menu; and
- activity ownership now matches the companion's full component name instead
  of counting every installed activity named `MainActivity`.

`tools/test_controller_link_classification.py` adds a regression proving that an
unrelated launcher's `.MainActivity` is not mistaken for a second companion
owner.

Validation:

- `python -m py_compile tools/controller_link_cycle.py`
- `python tools/test_controller_link_classification.py`
- live exact-component owner check: one companion owner while an unrelated
  launcher `MainActivity` also existed
- after the counted cycles and controlled cleanup: BLE management connected,
  Classic raw/ready both zero, last teardown reason `0x13`

A later loose-cable reset cleared the runtime counters again. The final passive
handoff read showed the adapter idle and advertising for management, with no BLE
client and Classic raw/ready both zero; it was not reconnected merely to improve
the final snapshot.

One post-change harness smoke attempt was excluded because automatic resume had
already established Classic before the requested workload. It is not included
as a Bluetooth result and no additional cycles were run merely to improve a
count.

## Recommendation

Keep the harness corrections and close this path-differential investigation as
**no meaningful path difference found**. Do not add delays, split/consolidate
an already-shared state machine, flash firmware, or launch another failure soak
on the basis of this premise. Future rare-device-failure work should retain the
existing `PAGE_TIMEOUT` versus post-page HCI `0x08` classification, but should
not describe workload A as an explicit Controller Link control.
