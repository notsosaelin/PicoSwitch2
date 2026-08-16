# PicoSwitch2 Status

> Current-state snapshot. Historical implementation narratives are archived in
> [`docs/archive/status-through-2026-07-15.archived.md`](docs/archive/status-through-2026-07-15.archived.md).
> Planned work belongs in [`PLAN.md`](PLAN.md); evidence and protocol details belong under
> [`docs/`](docs/README.md).

Last verified: 2026-08-14 (Thor rumble PHYSICALLY CONFIRMED via ADB self-test; motion chart/state question open)
Branch: `ns2-testing`

Documentation/resource audit: 2026-07-25

## In-band BLE management transport — 🟡 recovery hardware-confirmed; active-use gate remains 2026-08-13

The configuration BLE service (RX/TX GATT + wireless command bridge) is now available in a **normal
controller personality**, gated by the RAM-only runtime flag `g_mgmt_enabled` (production default
on; `mgmt off` lasts until reboot). A phone or the web portal manages the adapter — Amiibo, colors, `personality`, `bonds`,
`wake` — **over Bluetooth while a controller drives the console**, with no CDC Config re-enumeration
(the console is never dropped). When disabled, the path is byte-identical to before (proven
zero-cost early return). Landed slices: `mgmt_access.{c,h}` (canonical access-control spec, exhaustive
256-state host test), `mgmt status/on/off` command + allowlist, `config_ble_authorized()` gate
decouple, unconditional `config_wireless_task()` pump, deferred wireless flash ops (`save`/`amiibo
clear`/`amiibo persist` no longer stall core0), asynchronously completed session-bound bond list/
remove replies, and a web-portal Management panel. Built clean on both
boards; all management and Android-controller contract host tests green
(`tools/run_mgmt_tests.ps1`, 11/11), including versioned bounded bond enumeration and
response-too-large fail-closed coverage. The session-level integration test now calls the
production authorization predicate directly and proves bonded plaintext and non-live sessions
cannot dispatch even allowlisted commands.

**Hardware state (2026-08-13):** the original workflow succeeded, and the controller-discovery
decoupling fix then held a Classic controller plus management client for 5.4 hours through ten USB
re-enumerations and three controller disconnect/reconnect cycles. Management stayed connected and
the controller recovered without a power cycle. The original `disc=0` wedge was not reproduced, so
the fix is inferred sufficient rather than a proof of that unseen management-half cause. Active
console use with audio, gyro, wake, and latency observation remains the final coexistence gate. See
[`docs/experiments/overnight-investigation-2026-08-13.md`](docs/experiments/overnight-investigation-2026-08-13.md).

**Authorization now implemented, hardware pending:** RX and notification-subscription writes require
ATT encryption with a 16-byte key; callbacks additionally require a durable LE bond; and a new
Just-Works bond is accepted only inside the existing double-tap pairing window. No-display Just
Works cannot provide MITM authentication, so this is accurately described as bonded and encrypted,
not `ATT_SECURITY_AUTHENTICATED`. Wake-burst advertiser handoff is implemented and awaits its
runtime pass alongside the audio/gyro/latency coexistence checks. See
[`docs/bluetooth/in-band-management-plan.md`](docs/bluetooth/in-band-management-plan.md).

## Motion encoder — ✅ unified on the one hardware-validated encoder 2026-08-14

There is now exactly **one** Switch 2 motion encoder for translated sources,
`src/bt_hid/motion/ns2_ds5_motion.c`. `ns2_build_report()` has two motion branches — opaque genuine
Pro Controller 2 passthrough, and the translator — with **no fallback and no per-source whitelist**.

**Deleted:** the per-axis "phase" encoder (`ns2_motion_tick()`, `ns2_encode_motion30()`,
`ns2_phase[]`, the anomaly-capture instrumentation, the `imuanom` CDC command, and the `phase=[…]`
field of `imu`). It integrated gyro rate into three independent int32 accumulators and wrote them
into motion bytes `0x04..0x0F`. **It never produced correct motion on hardware and could not have:**
those twelve bytes are one packed quaternion — three 26-bit fields split 24+2 across
non-aligned bytes, with the 2-bit chart state in the low bits of byte `0x04` — so an int32 angle
straddles field and state boundaries and decodes as an orientation that jumps whenever a carry
crosses a field boundary. That is a representation error, not a tuning error — which is why the
2026-07-10 bias/stillness work correctly fixed the mechanisms it examined without improving the
symptom, and why every newly-supported controller family reported "motion spams everywhere" until
it was individually added to the translator's source list.

The root cause of its survival was **documentation**, not code: the carrier had been decoded and the
correct packing had been shipping for weeks, but the top-of-document layout table in
`docs/switch2/report-0x09-motion.md` still read "Angular phase X/Y/Z". That table is now corrected
and the refutation is recorded in
[`docs/experiments/refuted-hypotheses.md`](docs/experiments/refuted-hypotheses.md).

Per-source frame differences remain in exactly one place, `ns2_motion_seam.c`, one determinant-+1
row per source. Adding a controller family means adding a seam row, not a branch in the encoder.
Sample consumption is now a single shared function (`ns2_motion_consume()`) used by both the report
builder and the config-mode debug tick, so those cannot drift.

**LOCALLY VERIFIED:** both boards build clean; management/Android host tests 11/11; motion PDU,
DS5 translator, and motion-seam host tests rebuilt and green; Python motion/trace tests green.
**REQUIRES HARDWARE TEST:** that a Wii Remote and any other previously-generic source now produce
usable motion (they reached the deleted encoder before this change).

Open follow-up: `ns2_ds5_motion.*` should be renamed to a neutral name — it is the common encoder,
not a DualSense one. Deferred as pure churn (373 references) rather than done as a rushed sed.

## Android companion motion — 🟡 direction fixed on hardware; timing fix pending 2026-08-14

**Hardware after the display-rotation fix:** left/right smooth and correct; up/down now the correct
physical direction but noticeably less smooth — choppy/stepped, with occasional brief clipping or
jumping that recovers within about half a second. Roughly two of three rotational dimensions
behaving.

**Axis mapping is now PROVEN correct in software, so this is not a mapping problem.**
`tools/test_ns2_motion_quality.c` drives the production encoder with synthetic pure yaw, pure pitch
and pure roll and decodes the wire output back to a rotation axis: leakage into each unintended
axis is **0.00000**, driven-axis magnitude **1.00000**, integrated angle within 0.005% of analytic,
on all three axes. Do not reshuffle seam rows to chase this.

**Root cause of the remaining choppiness (strongest evidence, hardware-pending): the Android source
was integrated against packet ARRIVAL time, not its own IMU clock.** The seam forwarded
`motion_timestamp` only for the DualSense, so Android fell through to the host clock. Bluetooth
delivers a steady 125 Hz sender in bursts, and pairing a rate sample with an interval it did not
occur over costs trajectory accuracy. Measured on a 2 Hz / 120 dps sweep, same mean cadence, only
the arrival pattern differing:

| clock | arrival | worst trajectory error |
|---|---|---|
| host | even | 0.002° |
| host | bursty | **0.505°** |
| source | bursty | 0.002° |

A constant rate cannot expose this (the intervals still sum to the true elapsed time, so the
endpoint is right either way — measured −0.17° over 28.8°). Only varying-rate motion does, which is
exactly when a player notices. The host-clock path additionally exposed Android to two hazards the
DualSense never touches: the 3800 µs minimum-period gate, which silently **drops** a sample arriving
inside it (and can starve bias warmup so that **no** motion is integrated at all), and the 16 ms
anti-lurch clamp, which discards rotation beyond it (measured: 40 ms gaps report 57.6° of a true
144°).

**Why pitch and not yaw — and why that does not localize the cause.** Anything that perturbs
integrated attitude surfaces in pitch and never in yaw, because the console cross-checks pitch
against gravity and gravity says nothing about yaw. The asymmetry is therefore expected for *any*
attitude-quality defect and is not evidence for a particular one. That is why this was measured
rather than inferred.

**Changed:** the source IMU clock is now forwarded for any source that authors one, with the unit
carried alongside it (`switch_pro_input_t.motion_timestamp_unit`), so the encoder stays
source-agnostic. The DualSense keeps 1/3 µs ticks with a 32-bit wrap; the Android bridge now sends
100 µs ticks with a 16-bit wrap instead of milliseconds — at 125 Hz a 1 ms quantum is 12.5% of the
interval, the same order as the jitter being removed. The delta is taken in the source's own
modulus before scaling, because converting absolute DualSense ticks to microseconds first is not
wrap-safe (2^32 is not divisible by 3). Wrap behavior is covered by a host test.

**Also settled, and it closes a standing open question:** `NS2_MOTION30_ACCEL_Q16_PER_COUNT = 68963`
is **correct**, not a DualSense correction leaking into the shared encoder. Decoding 147 resting
motion blocks from two genuine Pro Controller 2 captures gives a mean resting magnitude of
**4310.1 ordinary counts**; `4096 × 68963/65536` predicts 4310.2 (ratio 1.0000), while exact Q16.16
at 4096 counts/g would be 5.23% low. The genuine wire scale is ~4310 counts/g. The prior note in
`switch1-to-switch2-motion-spec.md` calling this "5.2% high, open" used the wrong reference and is
corrected. This also **rejects** the acceleration-scale hypothesis for the choppy pitch.

**LOCALLY VERIFIED:** both boards build; motion-quality harness (24 checks) green including the
100 µs wrap case; all motion/bridge/management host tests green; Android compiles and unit tests
pass. **REQUIRES HARDWARE TEST:** whether pitch smoothness is actually restored.

## Android companion rumble — ✅ PHYSICALLY CONFIRMED WORKING 2026-08-14

**The Thor rumbles.** Confirmed physically by the maintainer during an ADB-driven self-test.

**Primary root cause was a device setting, not our code:** `Settings.System.VIBRATE_ON = 0`.
Measured over ADB (`mVibrateOn=false` in `dumpsys vibrator_manager`). AOSP discards **every**
vibration from **every** app when this is 0, for every usage except ACCESSIBILITY. That is the
entire "zero rumble, ever" history and exactly the `ignored_for_settings, scale: 0.00` recorded
against our package. Our usage flags were never involved.

**Secondary cause, ours, introduced and fixed the same day:** the routing cascade copied Moonlight's
`!isExternal()` guard for the system-vibrator fallback. The Thor's built-in controller reports
`EXTERNAL` with `ids=[] hasVibrator=false`, so the guard resolved to `None` and cut off the only
actuator. Removed — the project's own 2026-08-12 ADB audit already said "do not reject
`isExternal == true`".

Measured, driven autonomously through the app's own output path:

| `vibrate_on` | routing | dumpsys status | physical |
|---|---|---|---|
| 0 | `System` | `ignored_for_settings` | nothing |
| 1 | `System` | accepted, played, `cancelled_by_user` on stop | **felt** |

**`VIBRATE_ON` is currently left at 1** (changed over ADB during the test). It is a user-facing
setting under Settings → Sound & vibration; turning it off will silently kill rumble again, which
the app now reports in its `haptics bound` line rather than failing invisibly.

Also corrected: the `haptics bound` diagnostic **does** appear (logcat tag `PicoSwitch`); the
earlier `USAGE_TOUCH` mechanism we recorded was wrong (a bare `vibrate()` is `USAGE_UNKNOWN`, which
shares an intensity with `USAGE_MEDIA`, so that fix was a no-op); and `IGNORED_BACKGROUND` was not
the cause — the foreground service is retained only because it is independently correct for a
Bluetooth HID bridge.

**Remaining, deferred:** the console → adapter → Android leg has not been re-measured since the fix.
The firmware `rumble` UART counters and the app's `rumble received` log will confirm it in one
session of actual play.

## On-screen C / GameChat button — ✅ implemented 2026-08-15

The companion's "Console buttons" row now offers **C / GameChat** alongside Home and Capture, wired
through the same `setVirtualButton` path rather than being UI-only.

**No firmware change was required.** The path already existed end to end: the generic sequential
profile maps wire button usage 15 to `JP_BUTTON_A3`, `NS2_BASE_BUTTON_MAP` index 18 routes that to
`NS2_DST_C`, and `ns2_seam.c` raises `SWITCH_EXTRA_C`. `BLE_MAX_BUTTONS` is 16, so there was already
room.

What changed is the wire contract and the app: 14 buttons + 2 pad bits became **15 + 1, inside the
same two bytes**, so every later field kept its byte offset and the descriptor stayed 161 bytes.
`tools/check_android_descriptor_parity.py` confirms the C and Kotlin descriptors remain
byte-identical.

Routing is proven, not assumed: `tools/test_bthid_android_bridge.c` drives wire bit 14 through the
production driver and asserts it arrives as `JP_BUTTON_A3` and nothing else, that usage 14 still
arrives as Capture (a one-off in the mask would have swapped them), and that the two are independent
and clear on release.

**Corrected 2026-08-15 (same day, maintainer direction):** `KEYCODE_BUTTON_C` and
`KEYCODE_BUTTON_Z` are now **unmapped and ignored**. They had been routed to Capture, which was
arbitrary — they are extra physical buttons on some handhelds and pads, not Capture keys — so it
caused unexpected behavior and consumed two inputs the future custom button-mapping system should
own. They are NOT reassigned to C / GameChat either. Capture consequently has no physical key by
default, which matches both ADB audits (neither handheld has a dedicated Capture key); it is
reached through its on-screen button. Home keeps `KEYCODE_BUTTON_MODE`.

The key table now lives in the pure `AndroidInputBackend.positionalButtonForKey()` and is pinned by
`PhysicalKeyMappingTest`, including a sweep asserting no physical key maps to Capture or C, and a
check that both remain reachable as virtual buttons.

**Durable rule recorded:** unknown or additional physical controller buttons should be preserved as
candidates for future custom mapping rather than silently assigned to unrelated controller
actions.

**LOCALLY VERIFIED:** both boards build; 7/7 host tests; management/bridge suite 11/11; descriptor
parity identical; Android unit tests pass (including four new C-button cases); APK builds.
**REQUIRES HARDWARE TEST:** that the console actually opens GameChat when the on-screen C is pressed.

## Next session — starting point

**Rumble: CLOSED, physically confirmed.** Do not refactor it. If it ever regresses, check
`adb shell settings get system vibrate_on` before touching any code.

**Motion chart/state transitions: investigated and REJECTED as a cause 2026-08-14.** The maintainer's
"S1/S2/S3-style" hunch was tested directly and does not hold. `tools/test_ns2_motion_quality.c` now
decodes the encoder's wire output back to an orientation and compares consecutive samples with a
sign-invariant angular metric. Across every trajectory — including a phased-axis run that reaches
**all four charts** with 30 transitions — the worst orientation step at a chart change equals the
worst step anywhere else, to four decimals (1.9237° vs 1.9237°, nominal 1.9200°). Zero build
failures; packed round-trip exact over 400 combinations with unrelated bits preserved. **A chart
change costs nothing.** Do not add hysteresis, and do not revisit this without new evidence.

**MOTION IS FROZEN BY MAINTAINER DECISION (2026-08-15).** Motion is working much better, and the
maintainer tested an Odin 2, Odin 3 and Odin 2 Mini and did **not** reproduce the remaining artifact
on any of them. That makes it look Thor-specific rather than a defect in the shared implementation,
so the shared motion path is not to be changed further without new evidence. The finding below is
retained as a measured fact, not as an open work item.

**Measured, not guessed: the bias estimator absorbs slow rotation.** A constant rotation below `NS2_DS5_GYRO_STILL_LIMIT` (40 counts = 2.44 dps) loses about
half its motion:

| rate | survives |
|---|---|
| 0.5 / 1.0 / 2.0 dps | ~52% |
| 3.0 dps | 97.2% |
| 5–30 dps | 100.1% |

The cliff is exactly on the threshold. Fine aiming below it feels sluggish and "recovers" when the
player speeds up. **Left unfixed on purpose:** the bias tracker exists because a DualSense at rest
drifts, and its current derivative-gated form took two hardware passes to get right. Moving the
threshold trades slow-aim fidelity for at-rest drift and needs a decision plus a hardware A/B.

The principled fix if reopened: a gyro alone cannot separate "still" from "rotating slowly and
smoothly" — both have near-zero derivative. The accelerometer can, because a real rotation moves the
gravity vector. Gate stillness on gravity-vector stability as well, rather than moving the gyro
threshold. Note this is a *different* symptom from the rare brief hitch the maintainer reported, so
it may be an additional defect rather than the same one.

**Not yet re-measured since the rumble fix:** the console → adapter → Android rumble leg. The
firmware `rumble` UART counters plus the app's `rumble received` log will confirm it in one session
of actual play.

**Tooling available directly — use it instead of asking:**
- ADB reaches the Thor wirelessly (`AYN_Thor`). Package `dev.picoswitch.companion.debug`;
  diagnostics mirror to logcat under tag `PicoSwitch`.
- Haptic self-test, no console or adapter needed:
  `adb shell am broadcast -a dev.picoswitch.companion.SELF_TEST_RUMBLE --ei left 220 --ei right 220`
- Adapter UART is the CP210x bridge on **COM11**. `tools/uart_query.ps1` exists for this but
  **got no reply on its first trial and is unvalidated** — verify the line protocol (baud, newline,
  echo, whether a prompt is required) before trusting it. This is the one piece of self-service
  tooling still missing.

## Agent knowledge briefs — ✅ added 2026-08-14

[`docs/agents/`](docs/agents/) holds four short durable briefs — `COMMON`, `MOTION`, `RUMBLE`,
`ANDROID` — so a focused investigation can be dispatched with a three-line prompt instead of pages
of restated history. They exist because large repeated prompts waste context and let agents
paraphrase established hardware observations into softer, wrong ones ("never worked" → "may have
issues"). `AGENTS.md` now points at them and states the prompt-discipline rule.

## Bridge contract version — ✅ implemented 2026-08-15

Runtime skew detection between the flashed firmware and the installed companion. Both ends now
report a bridge contract version (`ANDROID_BRIDGE_CONTRACT_VERSION` / `BridgeContract.VERSION`,
currently **3**), and the firmware reports a git build identity.

**The incident this closes:** C/GameChat changed the descriptor from 14 buttons to 15. The APK was
updated while older firmware stayed flashed. `android_bridge_identify()` requires an exact 161-byte
match, so it failed and the firmware fell back to the v1 generic profile — buttons and sticks kept
working while battery, motion and rumble/player-LED disappeared together, with no error anywhere.
Every source-level parity check passed, because they compare source tree to source tree and cannot
see what is flashed. Root cause confirmed from runtime evidence: after flashing current firmware,
`bridge_identify` reports `matched=2, profile=v2-bridge` and `bridge.sent` went 0 → 285.

- **Firmware:** `bridge` / `bridge clear` UART commands report contract, build id, identify
  call/match/reject counts, first mismatching byte on a content rejection, active profile, and a
  bounded `suspected_skew` hint (expected length, different content).
- **Companion:** the management `info` reply carries `bridge_contract` and `build`; a mismatch — or
  firmware reporting no contract at all — surfaces on the controller-link card and in the export.
  Never reported as compatible on silence.
- **Guard:** `check_android_descriptor_parity.py` now pins the version across C and Kotlin, and
  `BridgeContractTest` pins the version against the descriptor bytes that define it.

Controller behavior is unchanged; the descriptor and report layout were not touched.

## Bridge architecture split — ✅ implemented 2026-08-15, hardware re-validation pending

The companion's controller path is no longer an Android-specific bridge; it is a
**platform-neutral bridge with Android as its first backend**. `android/companion/` is now two
Gradle modules:

- **`:bridge-core`** — plain Kotlin/JVM, **no Android dependency**. Owns the normalized
  `ControllerState`, the canonical motion convention (`MotionConvention` / `MotionScale` /
  `ScreenOrientation`), `DeviceCapabilities`, the face-layout resolver, the source candidate rule,
  `ControllerInputState`, the HID descriptor and report codec, the normalized `RumbleRequest` /
  `BridgeOutput` model, and `BridgeSession` (cadence, motion gating, battery polling,
  neutralization, report accounting) behind `BridgeTransport` / `MotionBackend` /
  `BatteryBackend` / `OutputBackend` interfaces.
- **`:app`** — `dev.picoswitch.companion.bridge` holds the five Android pieces
  (`AndroidInputBackend`, `AndroidMotionBackend`, `AndroidBatteryBackend`, `AndroidOutputBackend`,
  `AndroidHidTransport`) assembled by `AndroidBridge`. The old
  `dev.picoswitch.companion.controller` package is gone.

The module boundary **is** the architecture guard: the Android SDK is not on `:bridge-core`'s
compile classpath, so a leak is a build failure. `ArchitectureGuardTest` additionally rejects
platform vocabulary in core identifiers and string literals.

The concrete payoff is test coverage that previously required a phone: `BridgeSessionTest` now
proves link handling, motion gating, per-motor rumble delivery, battery polling, cadence mode
switching, teardown ordering and neutralization on the JVM with fakes.

Behavior is intentionally preserved except for three items, all caused by the coupling being
corrected: the `AcquiringProfile` phase is renamed `Preparing`; the HID SDP service name is now
`PicoSwitch Bridge Controller` (the firmware matches descriptor bytes, never the name); and the
live-input panel shows D-pad directions instead of the wire hat code. **Not hardware re-validated
since the split** — the smallest useful regression pass is one AYN Thor session covering connect,
buttons/sticks/triggers, rumble, motion, and background/disconnect neutralization.

Contract: [`docs/bridge/PROTOCOL.md`](docs/bridge/PROTOCOL.md). Backend guide:
[`docs/bridge/PLATFORM_BACKEND.md`](docs/bridge/PLATFORM_BACKEND.md). No Windows or Linux backend
exists or is planned; those documents exist so building one is an implementation task.

## Android handheld controller bridge — 🟡 AYN Thor in-game hardware pass 2026-08-13

**Ownership update (2026-08-13):** the maintainer returned the completed Android/HID work to the
main repository workflow. There is no remaining Claude-only code boundary; changes must preserve
the hardware-confirmed v1 input path and its byte-compatible v2 extension.

The no-root Android path uses the public API-28+ HID Device profile and keeps PicoSwitch2 as the
console-facing protocol owner. Pico-side preparation now includes an exact 81-byte generic-gamepad
descriptor fixture, its neutral report, and a host test that compiles both through the production
Bluetooth gamepad driver and shared HID parser. The test pins the 10-byte wire report (ID 1, six
axes, 14 buttons, and hat), complete-state retention, malformed/wrong-ID rejection, and disconnect
cleanup.

An Android-initiated Classic HID connection reaches the generic fallback even when an OEM retains
a phone Class of Device; host coverage now pins that behavior. Pico-initiated inquiry deliberately
continues to reject phone/computer classes, so the app must initiate the connection. Descriptor-
backed generic reports shorter than the descriptor-derived minimum are now dropped atomically;
valid reports, longer vendor reports, and descriptorless Classic fallback are unchanged.

An ordinary no-root debug APK on an Android 13 AYN Thor now sees the built-in `Odin Controller`,
renders its live sticks/triggers/buttons, acquires Android's public HID Device profile, and reaches
registered/Ready. The first app-led pairing attempt exposed a missing
`android.software.companion_device_setup` manifest declaration and crashed synchronously; that is
fixed and guarded. A second Thor-specific failure came from treating `registerApp()`'s immediate
`false` as final even though the OEM stack then delivered a successful registration callback. That
made Retry collide with this app's own live record. The callback-authoritative fix is hardware-
confirmed through registration, the bonded host connection, Pico receipt, and working in-game
console input. The first real play pass exposed one semantic defect: Android face-key positions
were forwarded as letters, so the Nintendo-labeled Thor appeared A/B and X/Y flipped. The app now
normalizes a persisted `Auto` / `Nintendo` / `Xbox` layout before the unchanged HID encoder;
`Auto` recognizes the two audited Thor/Retroid identities and otherwise uses Android's documented
positional convention. Changing layout clears held state. That correction is statically tested but
not yet replayed in-game. Latency and lifecycle teardown remain unvalidated. See
[`docs/bluetooth/android-controller-bridge.md`](docs/bluetooth/android-controller-bridge.md).

The app now presents one adapter relationship: first use is **Pair Adapter** through Android's
required companion chooser and bond consent; the selected address/association is retained, known
management GATT reconnect is attempted directly with service-scan fallback, and controller mode
registers/connects HID to the same saved bond without a second chooser. Companion association,
Classic bond, BLE GATT, and foreground HID registration remain distinct Android operations under
that UX. Saved-address identity mismatch now also enters the bounded service-scan fallback, each
foreground session gets a fresh automatic attempt, and disconnect clears all stale adapter-derived
details and gates Amiibo mutations. This combined flow is source/JVM-tested; the earlier separate workflow reached in-game,
but first-run/returning behavior in the rebuilt APK still needs physical lifecycle validation.

## Explicit active-input source arbiter — 🟡 host/build validated; hardware pending 2026-08-13

The firmware now keeps a bounded registry of HID-ready sources using stable Bluetooth identity
when available plus a monotonic connection-generation token, rather than treating a reusable
BTstack connection index as identity. The registry exposes opaque source IDs through the bounded
`input sources` management query and applies an explicit selection request at an input report
boundary. Only the selected source may publish normalized state, raw buttons/identity, wake intent,
mouse deltas, or native motion to console slot 0. A source change immediately neutralizes the
complete slot (sticks, buttons, motion, mouse, identity, raw debug, rumble, and LEDs), then requires
one fresh complete report before output resumes. Active disconnects neutralize and do not fall back
to another source; stale disconnects cannot remove a source after connection-index reuse.

The seam now returns no player slot to feedback pollers for inactive sources, but a few legacy vendor
initialization paths still defensively fall back to slot 0. Source-aware rumble/LED delivery for
every driver is therefore explicitly deferred until those paths can be converted and host-tested;
the arbiter does not claim complete feedback isolation yet.

The first connected source still auto-selects for legacy single-controller operation, so no explicit
selection preserves the existing default behavior. Wired UART exposes `input sources` and
`input active <id|none>` for deterministic bring-up. Wireless enumeration is bounded to fit the
existing 512-byte response slot. Now that bonded/encrypted management is implemented, the same
selection command is allowlisted over BLE and the Android Input page exposes the bounded registry
as an **Active controller** selector. Focused policy coverage is in `tools/test_ns2_input_arbiter.c`; transport-integrated
rebind, stale report, and stale disconnect coverage is in
`tools/test_ns2_active_input_lifecycle.c`; both are included by `tools/run_mgmt_tests.ps1`. Standard
Pico W and standard 300 MHz Pico 2 W builds plus both install-marker checks pass in the integrated
tree. No flash, UART mutation, or physical multi-source/latency validation was performed.

The native Android Amiibo page now has portal-parity raw identity/details: character code/variant,
tag type, model/series, format, extended variant, and optional owner/nickname/date/write-count/game
metadata. The latter uses a direct port of the tested portal crypto path and the user's
validated 160-byte `key_retail.bin`; the key remains app-private, never enters firmware or
diagnostics, and Android backup is disabled. Local initialization/re-signing and bounded
portal-compatible ZIP library exchange are implemented transactionally. A strict foreground
phone-`NfcA` reader backs up ordinary NTAG215 figures as exact 540-byte images plus an optional real
32-byte signature; it rejects malformed tags, other NTAG sizes, and figure-v3 rather than truncating
or guessing. These paths are source/JVM validated; physical NFC/Amiibo validation remains open.
Structured owner-Mii extraction is feasible after a full-feature fixture pins the field map, but
graphical Mii rendering is deliberately deferred: the repository has neither a licensed renderer
nor a redistributable FFL resource, and a fake approximation would not be portal parity. A bounded
seven-day AmiiboAPI cache now supplies portal-matched friendly name/series/type/release, compatible
games/title-ID labels, and best-effort artwork without gating local or adapter operations. See
[`docs/experiments/android-amiibo-page-parity-2026-08-13.md`](docs/experiments/android-amiibo-page-parity-2026-08-13.md).

Earlier read-only ADB evidence from a Retroid Pocket Classic established that its API-34 OEM image
has HID Device enabled and its built-in controller exposes the required axes/buttons. The later AYN
Thor in-game pass supersedes the former API-feasibility uncertainty. The Retroid evidence still
proves source selection cannot filter on `isExternal` or virtual origin.

## Switch 1 Joy-Con / Pro Controller motion — 🟢 at parity with genuine hardware 2026-07-27

**A/B against a natively-connected Switch 1 Pro Controller: 98–100 % identical.** The small
residual lag is present on the native connection too, so it belongs to the controller (120 Hz
reports, no sensor timestamp), not to this firmware. Full write-up:
[docs/bluetooth/switch1-motion.md](docs/bluetooth/switch1-motion.md) §10.2–§10.3.

The axis map is `src {1,0,2}`, `sign {-1,1,1}` in `ns2_motion_seam.c`. It was resolved by
measurement, not iteration: reading a *resting* controller's accelerometer over UART put gravity on
slot 2 at +4245 against a genuine Pro Controller 2's 4279/4309 — a 1 % match — which pinned that
lane with nobody touching the controller.

The bug four sign guesses had missed was that the row had **determinant −1**: a reflection, which
cannot describe a physical sensor remount. Gravity cannot detect a reflected frame — a single
vector looks correct reflected — so the accelerometer matched genuine hardware while the gyro
produced no horizontal aim at all. `tools/test_ns2_motion_seam.c` now enforces determinant +1 on
every row and is verified against the shipped bug.

Gyro also switched from the three-frame mean to the newest frame, removing ~7.5 ms of group delay
(the frames span 15 ms while the Pro reports every 8.3 ms). Accel keeps the mean — it is the
console's gravity reference, where steadiness beats latency.

Implemented in `switch_pro_bt.c`: the `ENABLE_IMU` init step (subcommand `0x40`),
the three-frame IMU decode from report `0x30` bytes 13–48, and
`SWITCH_MOTION_SOURCE_SWITCH1` provenance so `switch_pro2.c` routes it to the
validated quaternion translator rather than the known-bad generic phase encoder
(that encoder was the cause of the "spamming all over the place" first seen on
hardware — the raw data was clean, confirmed by live `input status`).

Per-unit SPI-flash calibration is read too: subcommand `0x10` fetches the factory
block at `0x6020` and the user block at `0x8026`, the report-`0x21` reply is
parsed by `switch_parse_spi_reply()`, and the user block overrides the factory
one when its `B2 A1` magic is present. Calibration is strictly an improvement,
never a dependency: an absent, erased, or zero-span block is rejected and the
nominal §6 constants are used, so motion works from the first report regardless.

The §6 nominal-vs-datasheet gyro-scale disagreement was settled by hardware in
favour of the LSM6DS3 `0.070` dps/count value; the nominal assumption
under-reported rate noticeably against a DualSense.

Still open: 🟡 Joy-Con L (`0x2006`) / R (`0x2007`) share the Pro's seam row and
are unverified — §8 says the halves mount the IMU mirrored, so at least one axis
is likely wrong on at least one half. Note the halves differ from the Pro by a
*proper* rotation (Linux negates Y and Z on both sensors for the right half), so
any correction must keep determinant +1.

## Wii Remote motion — 🟢 hardware-confirmed working 2026-07-27

Accelerometer + Wii MotionPlus gyro now stream through the existing motion
carrier and are confirmed working on hardware by the project owner.

Implemented: split 10-bit accelerometer assembly, MotionPlus detection
(`0xA600FA`), 32-byte calibration read at `0xA60020` **before** activation with
CRC32 verification, the MotionPlus init pair, activation via `0xA600FE` with the
passthrough mode chosen from the downstream extension, verification at
`0xA400FA` with retries, and per-frame decode honouring `is_mp_data`, the
cross-byte slow bits and per-axis slow/fast calibration blocks. The documented
rumble-latch bug is also fixed (every output report rewrites the motor latch).

No new motion representation was invented: the Wii publishes the same SInput
convention the Sony parsers use (`±32767 = ±2000 dps` / `±4 g`) in the DualSense
slot frame, which `ns2_seam.c` already remounts into the Pro2 frame. It carries
its own `SWITCH_MOTION_SOURCE_WII` provenance so future IMU-bearing controllers
have a place for per-family policy.

Still open: EEPROM accelerometer calibration (fallback constants in use),
passthrough bit-reversal for an extension behind an active MotionPlus, and
re-expressing orientation detection on the calibrated vector. See
[docs/bluetooth/wii-motion.md](docs/bluetooth/wii-motion.md) §12.5.

**Unresolved evidential tension, raised 2026-08-14.** On 2026-07-27 the translator's source
whitelist was DualSense-only, so a Wii Remote's report-0x09 motion reached the deleted per-axis
phase encoder. That encoder cannot produce a decodable orientation (see the motion-encoder section
above), so either this confirmation was made against report 0x05 (Steam/PC, which consumes
`in.accel`/`in.gyro` directly and is unaffected), or "working" meant the console *responded* to
motion rather than that motion was correct — the same ambiguity the old protocol reference carried
("both Zeldas and Splatoon respond, but exact fidelity remains unresolved"). A console responds to a
garbage orientation with wild movement, so response is not correctness. The Wii Remote now routes
through the one validated encoder either way; **re-confirming it on the console is the cheapest way
to close this**, and it is bundled into the next hardware pass.

## Virtual amiibo — v3 (NTAG I2C Plus 2K / Kirby Air Riders) — 🟡 IN PROGRESS 2026-07-28

Full record: [`docs/Amiibo-v3.md`](docs/Amiibo-v3.md) §19.

The complete 2048-byte read/write path is hardware-confirmed with both the capture-rebuilt baseline
and an untouched downloaded `Kirby & Warp Star.bin`. It includes descriptor-driven page ranges, the
v3-only `0x14`/`0x21` device command, and an 83-byte `0x18` result formed from a 19-byte controller
header plus the dump's complete 64-byte SRAM response. The last two SRAM bytes are the
CRC-16/MCRF4XX over the preceding 62 bytes; they are per response (`7A C4` for the captured figure,
`E5 11` for the downloaded Kirby/Warp, `30 61` for Meta/Shadow), not a fixed controller trailer.

The untouched dump worked with no signature override and no key-based transformation, refuting the
signature/carrier theories. Its trace contains three correct full-SRAM results, six write chunks,
one `0x08` commit, `05 00`, and zero write errors. The persisted export remains HMAC-valid with the
console-written nickname/owner and unchanged SRAM. Capture:
[`docs/experiments/v3-full-sram-response-validation-2026-07-28.md`](docs/experiments/v3-full-sram-response-validation-2026-07-28.md).

Owner/format write, Stop/eject, next-scan updated readback, and power-cycle recovery are
hardware-confirmed. The one remaining lifecycle check is production-portal Sync of the intentionally
retained dirty generation, followed by firmware acknowledgement only after IndexedDB succeeds.

The genuine Pro Controller 2/physical Kirby & Warp Star positive control is now captured. It proves
that Air Riders uses two sector-aware `0x20` envelopes, not one fixed 355-byte no-op: a 355-byte
two-record clear and a 167-byte three-record update. Genuine completion reports empty state
`0x16`, then the console performs a selected-UID page-3 read and an ordinary 454-byte/`0x08`
write; only that later commit reports `05 00` and ejects on Stop. The post-write physical snapshot
confirms the second envelope's 32 bytes land at sector-0 pages `0x92..0x99`; its third record
directly addresses sector-1 pages `0x01..0x18`.

The firmware validates/applies both record layouts, generation-checks and journals each `0x20`
stage without ejecting, reports genuine `0x16`, and preserves the proven ordinary-write lifecycle.
The complete two-stage Air Riders write is hardware-confirmed: 18 ordinary chunks, three `0x08`
commits, eight extended chunks, two `0x20` completions, and zero write errors. The persisted
2048-byte export remains HMAC-valid, SRAM-valid, and contains both the sector-0 and sector-1 game
records.

Trying to reuse that written tag exposed one final read command. The console sends sector-aware
subcommand `0x1E`; the old virtual path bare-ACKed it but left state at `0x18`, causing repeated
three-second Stop/restart loops. A genuine Pro Controller 2 capture proves the ACK itself was
correct: genuine hardware stages a 196-byte result, changes status to empty `0x15`, signals one
report-state edge, and serves the result through three ordinary `0x15` chunks. The result is a
64-byte identity/signature/descriptor prefix plus sector-0 pages `0x92..0x99` and sector-1 pages
`0x00..0x18`; the chip-managed sector-1 capability page was `A5 00 01 00` after the first
Air Riders update even though ecosystem dumps leave that hardware slot zero.

That `0x1E` implementation is now hardware-confirmed: the console observed empty `0x15`, fetched
all 196 bytes in three chunks, and sent Stop. It then began another legitimate 167-byte update.
The first classifier correction let that subsequent 167-byte update and ordinary checkpoint
complete, persist, and export with zero write errors. Reusing the result then produced
“This amiibo is corrupted” during `0x1E`, before another write.

A genuine Pro Controller 2 positive control completed the same full read/write cycle, then read
the physical tag again without writing. A second full write and read-only control repeated the
transition. The captures prove that the envelope field is not page 4: it is the **next sector-1
page-0 value**. Genuine hardware advanced that implicit chip state
`A5 00 01 00 → A5 00 02 00 → A5 00 03 00`, while sector-0 page 4 independently advanced
`03 → 04 → 05` and every explicit sector-1 record began at page 1. The virtual path discarded
the field and continued serving `A5 00 01 00`, which is the only mismatched extended-read state.

The correction validates the next generation, stores the four-byte capability state in
the otherwise zero ecosystem-dump slot at `0x400`, persists/exports it with the 2048-byte image,
and serves the retained value through `0x1E`; zero-filled first-use/legacy images retain the
hardware-confirmed generation-1 fallback. Hardware then completed the virtual update from
`A5 00 01 00` to `A5 00 02 00`; the saved image exported with page 4 `A5 00 03 00`, sector-1
page 0 `A5 00 02 00`, valid amiibo HMACs, and the retained nickname/owner. Its immediate second
reuse was accepted by Air Riders and loaded the previously saved custom color. The successful
read trace returned the stored `A5 00 02 00` through `0x1E`. Both board builds, all 53 host tests,
all eight magnetometer tests, and both install-reset marker checks pass. A physical adapter
power cycle then restored the exact generation-4 image/CRC from flash, served the retained
`A5 00 02 00` through `0x1E`, and Air Riders accepted and loaded it without another write.
The dynamic sector-1 page-0 lifecycle is hardware-confirmed. A later save after completing an
Air Riders level validates non-cosmetic learned gameplay state: it used the same 167-byte extended
update plus ordinary six-chunk commit, advanced page 4 `05 → 06` and sector-1 page 0 `03 → 04`,
changed only the modeled writable ranges through `0x463`, and exported HMAC-valid with zero
write errors. See
[`docs/experiments/v3-air-riders-extended-operation-2026-07-28.md`](docs/experiments/v3-air-riders-extended-operation-2026-07-28.md).

King Dedede & Tank Star exposed allocation-relative Air Riders storage. Its update uses sector-0
page `0xB2` plus sector-1 capability/data pages `0x64/0x65`; Kirby uses `0x92` plus `0x00/0x01`.
The first correction accepted all three Dedede chunks, but the fixed Kirby record table rejected
both completions and returned error state `07 41`/`2115-0096`. The prepared codec now derives both
allocations from the envelope, validates them against the proven cleared/user-memory bounds, tracks
generation at the selected capability page, and makes `0x1E` fallback descriptor-relative. There
is no figure/UID whitelist. The portal Initialize path clears the complete second user-memory
sector for the same reason. The maintainer then flashed every one of the 16 available Air Riders
v3 dumps; all 16 completed both real-console reads and writes. This validates the generalized
allocation path across the complete available dump set rather than only the original Kirby
capture. All 53 host tests, both board builds, portal suites, motion checks, magnetometer checks,
and reset markers also pass. See
[`docs/experiments/v3-air-riders-dynamic-allocation-2026-07-28.md`](docs/experiments/v3-air-riders-dynamic-allocation-2026-07-28.md).

Retracted: the earlier claim that the read prefix carried a *dynamic* SRAM window
alternating between two values. It is constant across all 11 genuine reads; the
second value belonged to the `0x21` result buffer and was misattributed.

New instrument: `nfcmirror` gained an **initiator** mode, so UART can originate NFC
commands at a genuine controller with no console attached
(`tools/nfc_probe.ps1`). This dumps real tags — including v3, which nothing else
here can read — and turns per-question console captures into bench measurements.

Two real bugs were found and fixed along the way, both of which affected normal
use and not just v3:

- **Flash region collision (serious).** The amiibo journal bank 0 sat on BTstack's
  TLV region on RP2350 (pico-sdk 2.2.0 moves it one sector lower there), so writing
  a tag destroyed the Bluetooth bonds and BTstack destroyed the stored tag. Banks
  relocated; asserts now check `PICO_FLASH_BANK_STORAGE_OFFSET`.
- **v3 uploads were never durable.** `amiibo persist` gated on the 540 store's
  `loaded` flag, making it a silent no-op for v3, and the portal never called it.


## NFC investigation tooling — ✅ offline lab in place 2026-07-28

The v3 investigation cost hardware iterations on questions that were already answerable from data
on disk. The core offline laboratory now exists; the workflow, worked examples, and remaining gaps
are in
[`docs/re-methodology/nfc-investigation-workflow.md`](docs/re-methodology/nfc-investigation-workflow.md).

| Tool | What it settles before hardware |
|---|---|
| `tools/amiibo_corpus.py` | Structure, SRAM CRC, discovered allocation, and how many *distinct* images a corpus really holds |
| `tools/ns2_nfc_semantics.py` | One authoritative NFC layout vocabulary, imported by every other tool |
| `python tools/ns2_trace.py nfc` / `nfc-diff` | Reassembled transaction timelines, envelope classification, first semantic divergence |
| `tools/nfc_lab.ps1` | One hardware action captured as a hashed artifact bundle with its hypothesis and single variable |
| `.claude/skills/picoswitch2-nfc-lab` | Enforces the phase order for agent sessions |

## Shared protocol laboratory — 🟢 active infrastructure, genuine-controller discovery next

The NFC evidence workflow is now generalized without changing its proven runner.
`tools/PicoSwitch2Lab.psm1` provides one manifest/provenance contract;
`capture_to_fixture.py` generates deterministic JSON/C fixtures from zero-loss captures;
`ns2_command_atlas.py` now aggregates console-side `trace` and controller-side `blecap`
request/response shapes while retaining boundary, transport, GATT handle, completeness, and source
hash provenance. It rejects missing or non-zero loss metadata. The 2026-08-13 corpus audit admits
46 trace and 30 BLE captures but finds controller-side command traffic in only two files, covering
the same three initialization pairs; the ranked gaps are documented in
[`docs/switch2/controller-command-atlas.md`](docs/switch2/controller-command-atlas.md). Domain runners package
motion, audio, and firmware-update evidence. Repository-local Codex skills under `.agents/skills/`
enforce the same gates on a fresh clone.

The motion runner completed a zero-drop no-magnet/sham/polarity/distance/recovery campaign with a
genuine Pro Controller 2. Time-weighted A/B/A analysis found no polarity reversal or distance
scaling, and the matched no-magnet residual exceeded every 100 mm ceramic-magnet result. The
external field therefore did not produce a resolved response in G6/G7/G8 or the other retained
lanes; those lanes must not be described as a simple raw magnetic-field vector. See
[`docs/experiments/pro2-magnetic-stimulus-matrix-2026-07-29.md`](docs/experiments/pro2-magnetic-stimulus-matrix-2026-07-29.md).

Exact ICM-42670-P FIFO tables and the decrypted Pro2 `0x000A`/`0x000E` PCAPs then corrected the
field model. Handle `0x000A` carries an 18-byte raw timestamp/temperature/accel/gyro sample.
Length-`0x28` on handle `0x000E` is a Nintendo-packed multi-sample IMU payload; the reference
Pro2's 17–19-tick cadence uses its catch-up layout. The former G6/G7/G8 aliases cross the newest
packed gyro and accel fields and are not independent magnetic/reference lanes. The offline
`ns2_motion_reference.py` analyzer and tests reproduce the result. The default-off UART `imuref`
profile is hardware-validated: an explicit CCC handoff produces clean raw `0x000A` samples,
exclusive notification ownership, ATT success, and a reversible return to fresh native motion.
A same-pose production capture also establishes that the Joy-Con-derived normal-layout bit map
does not transfer directly to the Pro2's seven-tick high-rate form. Enabling both CCCs does not
yield simultaneous streams: native reporting takes priority, while disabling only the native CCC
resumes raw reporting. A zero-drop A/B/A capture brackets both paths.

A controlled 7.5–30 ms cadence matrix now resolves the full payload family. Tick deltas `0..10`
use signed22 fixed-point accel/gyro/accel; `11..14` use the mixed 13/14-bit normal form; and `15+`
use the corrected mixed 13/14/16-bit catch-up form. Every acceleration lane measures approximately
`1.052 g` in the live stationary corpus, and the scaled axes/gyro bias agree with raw handle
`0x000A`. The same catch-up form persists at the maximum accepted 30 ms interval. Bit 287 is the
single byte-alignment remainder after the 287 established data bits and remained zero in all 1,066
catch-up packets across 14 repository captures; it is treated as observed reserved-zero padding,
not an ordinary backlog field.
The next offline passes resolved the preamble and corrected the carrier boundary. Byte 2 plus byte
1's high nibble is a self-contained
12-bit elapsed count (`1274/1274` zero-drop predecessor comparisons), while byte 3 maps
`0x0D`/`0x0E`/`0x0F` exactly to high-rate/normal/catch-up (`1292/1292`). The first packet and
post-drop packets therefore no longer need a guessed layout. The high-rate/normal tail contains
two Q3 IMU-temperature samples sharing a signed ten-bit integer part. Two independent zero-drop
raw/native/raw captures bracket the native values with handle-`0x000A` temperature: the 7.5 ms run
measured raw `4.357`, native `4.28125`, raw `4.167`; the 15 ms run measured raw `4`, native
`3.951923`, raw `4`. The low fractions matched in `993/1023` records because the two samples are
usually, but not always, at the same sub-count temperature.

The prefix is not `flags2 + three equal-width values`. Its exact forms are
`mode2 + s24 + s23 + s25` in high-rate and `mode2 + s22 + s21 + s23` otherwise. Carrier 2 is
split on the wire: its low two bits precede its signed high bits. The former “separate state” was
that low fragment, not a state machine. Packing mode was `3` in all 2,592 analyzed records. Once
grouped only by the length-`0x1E` carrier state, the prefix fits the established retained
components at exact power-of-two scales. Paired pitch gives `0.999962` mean absolute correlation
under its former best constant offset; the packet-derived rule
`current tick - encoded elapsed + 4` raises it to `0.999996` and reduces fixed NRMSE from
`0.008728` to `0.002718`. The retained moving window remains `0.999996` / `0.002771`. The prefix is
therefore the truncated carrier four sensor ticks after the preceding carrier. A causal modular
history decoder reproduced interpolated length-`0x1E` truth with `0.000968°` and `0.004682°`
median angular error in the two dynamic sets, with zero chart mismatches.

Reciprocal zero-drop lazy-susan captures directly resolve the observed state-0/state-3 boundary.
State 0 wire `(G0,G1,G2)` and the state-0-boundary projection of state 3
`(G1,G2,G0)` are continuous across both
directions, with boundary delta norms `0.002563` and `0.001132`. Sixteen genuine rapid-motion
records exceed the strict retained-vector unit constraint (maximum `1.026738`), so strict
smallest-three is not an exact genuine-carrier model. The one prefix epoch inside each transition
selects chart 0 with residuals `0.000144` and `0.000790`. A later zero-drop Splatoon raid
captured `0 → 1`, selecting the local state-0 projection `(G2,G0,G1)` with residual `0.017025`;
the seam selects chart 1 over chart 0 (`0.010524` versus `0.091224`).
A second zero-drop raid then captured `3 → 1 → 0`. Its `1 → 0` edge has minimum unsigned
residual `1.185389`, refuting one globally composable unsigned permutation per state. The solver
now reports local edges and rejects the stateless global candidate. The cyclic omitted-component
topology plus a paired non-boundary sign flip fits that negative branch at `0.024716`; across all
five captured boundaries the structured model has RMS/max `0.025302/0.047878` and minimum branch
margin `0.324174`. State 1 has both sign branches captured. A later zero-drop state-2-only
trigger captured `3 → 2 → 3`; both reciprocal seams select topology `(G2,G0,G1)` with
opposite-branch signs `(+,−,−)`, at residuals `0.036162` and `0.011824`. The full
nine-boundary corpus covers all four chart states at RMS/max `0.023541/0.047878`.
Its interleaved `3 → 2` prefix seam selects chart 3 (`0.003833` versus `0.196168`);
the same local-frame audit recovers the former `3 → 1` seam as chart 1 (`0.008416`
versus `0.242898`).
Exact integer projection and rounding are now **resolved**, and every genuine `0x28` in the corpus
re-encodes byte-for-byte (858 high-rate, 149 normal, 981 catch-up, plus 2,070 `0x1E` carriers).

Two generator defects that byte-exactness could not catch have since been fixed
(`docs/experiments/pro2-carrier-unknown-fields-2026-07-31.md`):

- **Wire values are not a single unit.** Each layout and field packs at its own fixed-point scale;
  slot width does not determine it. High-rate acceleration uses eight fractional bits (`/256`),
  while high-rate gyro uses seven (`/128`). `ns2_motion_reference.WIRE_TO_COUNTS` is the single
  authority. Acceleration is verified by all eight slots agreeing on 1.051–1.052 g once normalized;
  gyro is verified against the sensor/common 16.4-count/dps scale and existing carrier integration.
- **Chart hysteresis is validated against hardware.** Replaying genuine orientation through
  `select_chart` agrees on 2,059/2,070 decisions (99.47%) with zero spurious swaps, though holds
  dominate and only 1 of 11 genuine swaps is reproduced. One-sample lookahead and an earlier fixed
  threshold are both refuted; swap timing is not a function of the carrier alone. This does not
  block generation, because chart choice is lossless — what must hold exactly is that no lane
  leaves the field, which swapping at the limit guarantees by construction.

`tools/ns2_motion_synth.py` compares a generated stream against an input-matched genuine one in
physical units; acceleration and gyro magnitudes are now the right size and unit on both sides.

**Packet roles and the interleaved clock are resolved.** Length-`0x1E` and length-`0x28` are both
native Pro Controller 2 forms, not “compatibility” and “next-generation” gyro respectively. In
clean genuine interleaving, both advance one 12-bit PDU clock and every encoded elapsed value is
the tick delta from the immediately preceding PDU of either length (`1274/1274`). Length-`0x28`
adds cadence-dependent sample history; it is not intrinsically more accurate than `0x1E`.

**Both firmware packers are hardware-byte-exact.** The catch-up packer rebuilds **981/981** genuine
packets and the high-rate packer rebuilds **853/853**, plus edge cases at field limits. They fail
closed on the wrong elapsed band, field overflow, and null input. Fixtures are generated by
`tools/gen_motion40_fixture.py`, not hand-written.

**The translation path is wired behind a default-off gate.** `ns2_ds5_motion40` buffers timestamped
DualSense samples, their contemporaneous `0x1E` carrier, and that carrier's proven tick in a
64-entry ring. The corrected mixed scheduler selects one native-rate `0x1E`/`0x28` PDU on the
shared timeline and holds it across intervening USB polls. Toggle with `ds5motion pdu40 on|off`;
`ds5motion pdu40 status` reports packet, hold, fallback, starvation, overlong, and saturation
counters. With the gate off, the validated production `0x1E` path is unchanged.

Four defects the offline analysis caught before any flash:

- **Raw gyro instead of de-biased.** Our stationary stream read 0.90 dps where genuine hardware
  reads 0.15 — the DualSense zero-rate bias, which on hardware is slow continuous rotation. Now
  fed `gyro_corrected`, the same sample the validated `0x1E` path integrates.
- **Empty motion block between packets.** USB polls near 1 kHz against a ~20 ms packet cadence;
  the first wiring left ~19 of every 20 polls with no motion data.
- **Slots did not span the emit window.** The first implementation filled the three acceleration
  slots from the first three samples to arrive and dropped the rest, so a packet covered only the
  head of its window and discarded the freshest ~40% of the data. Genuine packets put the oldest
  sample in slot 0 and the **newest** in the last slot: across 973 catch-up packets the
  seam `a2[N]`→`a0[N+1]` is the *smallest* gap in the stream (0.572 of a full window) and the
  within-packet `a0`→`a2` the largest short gap (0.866), strictly monotone in slot index; a paired
  sign test over 894 tick-contiguous pairs gives z = +10.2. Reproduce with
  `python tools/ns2_motion40_slot_timing.py`.
- **`elapsed` used a second generator clock.** The old `0x28` tick began at zero independently of
  the established `0x1E` tick, even though genuine interleaving is one PDU timeline. The corrected
  scheduler derives tick and elapsed from the latest carrier sample and the immediately preceding
  selected PDU; `last_sample_us` separately bounds the next sample-selection window.
- **One gyro sample represented a complete window.** The generated `0x1E` carrier integrates every
  accepted ~800 Hz source sample, but the first high-rate generator discarded all except one
  midpoint gyro reading. Passive UART measured stationary corrected-gyro standard deviations near
  80/50/35 counts, so that choice was not coherent with the packet's own carrier even though the
  mean motion was near zero. The replacement uses the tick-weighted mean of every sample the
  carrier integrated; the weights must sum exactly to encoded elapsed or the packet fails closed.
- **Acceleration was normalized twice.** `ns2_motion_seam_apply()` already converts the native
  DualSense 8192-count/g samples into the Pro2 frame at 4096 counts/g. The high-rate builder then
  divided them by two again, so live generated packets carried about 0.5 g. This escaped the
  offline synthesizer because its fixture supplied pre-seam samples directly. The module contract
  and host test now use the real post-seam 4096-count/g input.
- **Physical 1 g still disagreed with the interleaved carrier.** The validated translated `0x1E`
  path applies an established output gain of `68963/65536 = 1.052291870`, while the first post-
  seam `0x28` fix emitted bare Q8 physical counts. The same vector still jumped 5.23% on every
  representation change. LIVE now maps `source * 68963 / 256` into the high-rate Q8 lane; HALF
  retains the exact former `source * 128` diagnostic.

The design decisions above the packer are audited against the corpus rather than assumed —
saturation limits, slot placement, mode exclusivity, and field-specific binary points. Acceleration
converges on ±8192 ordinary counts = ±2.00 g. Normal/catch-up gyro reaches about ±499.5 dps;
high-rate gyro's signed22 `/128` field reaches about ±999 dps. Emission mode follows the BLE notification interval
with zero exceptions across 32 captures (6.0 ticks always interleaves, ≥ 8.0 ticks is always
`0x28`-only). Exact fractional slot positions remain **unresolved** — the corpus is stationary
(σ ≈ 2.0 counts/axis) and the structure function saturates before one window elapses, so the gaps
can be ordered but not measured. See
[docs/experiments/pro2-carrier-unknown-fields-2026-07-31.md](docs/experiments/pro2-carrier-unknown-fields-2026-07-31.md).

The historical mode-matched offline result (**1.0517 g genuine vs 1.0116 g synthetic**) used
pre-seam DualSense fixtures and did not represent the live firmware boundary. Live UART measured
about 4059 counts at rest after the seam; the rejected builder divided that again and emitted
about 0.5 g. The corrected builder preserves the post-seam vector and applies the exact same
output calibration already used by the hardware-validated `0x1E` carrier.

Status: 🔵 **Deferred by maintainer decision on 2026-08-01.** The production `0x1E` path remains
hardware-validated, and genuine Pro Controller 2 `0x1E`/`0x28` remains opaque passthrough. The
fully sequence-coherent generated high-rate recipe is hardware-rejected. The hybrid bisection
cleared acceleration and gyro and localized the first prefix failure to mixed carrier ownership,
but no further translated-`0x28` hardware work is planned without a concrete `0x1E` deficiency or
a materially better observation point.
The 2026-07-31 catch-up attempt failed violently. The 2026-08-01 high-rate interleaved attempts
remained unstable after the `/128` gyro correction, the shared-timeline scheduler, and the
tick-weighted gyro aggregation. Exact zero gyro still rotated, refuting rate selection as the
primary stationary cause. Disabling the gate restored a stable `0x1E` control immediately. Live
UART then exposed the 0.5 g double-normalization defect; that exact fix awaits hardware A/B.
Because the observed failure is abrupt multidirectional jumping rather than a simple scale error,
the mismatch is not yet claimed as the sole cause. A new closed-loop gate then caught the residual
5.23% `0x1E`/`0x28` acceleration-gain jump and fixed it. The resulting coherent LIVE hardware A/B
still produced continuous chaotic camera motion with no useful response to controller rotation.
At disable, UART reported 4,850 batches, 14,671 carriers, 56 starvation fallbacks, zero overlong
windows, two acceleration saturations, and zero gyro saturation; disabling the gate immediately
restored validated `0x1E`. This refutes decoded-lane physical coherence as sufficient and blocks
another flash of the same recipe.

The retained research observation point is implemented, build/host-validated, and partially
hardware-run.
Existing `blecap` and `motionpair` retain the complete genuine report/PDU and time-paired
raw/calibrated DualSense IMU. The new default-off live harness starts from a verified Nintendo
`057E:2069` packet, aligns a separately owned DS5 translator at an untouched genuine `0x1E`,
anchors its sample window to the genuine packet's Pico timestamp/elapsed boundary, and substitutes
only named semantic groups. Accel/gyro operate on eligible high-rate mode-3 `0x28` bases. Prefix
mode owns orientation across both interleaved `0x1E` and every mode-3 `0x28` cadence layout; every
other lane — including timing, status, packing, and temperature tail — remains controller-authored.
Physical-group stale/uncalibrated/unaligned/repeated/unsupported failures emit byte-identical
genuine data. After prefix ownership is anchored, short donor gaps hold the donor orientation
instead of alternating back to a second source history.
Base and output XOR are captured with reason/age/calibration provenance; the PC auditor rejects
drops, fallback edits, or out-of-mask changes before fixture generation. UART `off` immediately
restores opaque passthrough. Both boards build and all 57 compiled host tests pass.

Hardware results: the byte-identical control produced 95 unchanged records and stayed stable;
acceleration-only applied 14 packets with zero saturation/drops and stayed stable; gyro-only
applied cleanly and synchronized Display 3 video measured less than one pixel of displacement.
The first prefix run moved the camera violently, but it alternated 19 donor `0x28` prefixes with
68 genuine `0x1E` carriers and five stale-donor `0x28` fallbacks. Offline decode put donor and
genuine prefixes only `0.001..0.072°` apart. Repeating that small source discontinuity was the
harness defect. The corrected cross-length prefix ownership is host/build validated but was not
flashed before the campaign was deferred; it is preserved solely as research infrastructure and
is not a production promotion.

- **The prefix described the wrong instant.** A genuine prefix carries the orientation at
  `tick − elapsed + 4` ticks, not the packet's own tick, so sending the current carrier alongside
  the window's IMU samples double-counted the window's rotation. The firmware now buffers the
  carrier per sample and selects the entry nearest the window start. Scored 13.0× the achievable
  error floor before, 1.0× after.
- **Catch-up was the wrong layout.** Of the 773 genuine `0x28` packets with a `0x1E` alongside to
  validate against, **768 are high-rate and 2 are catch-up**. Catch-up was chosen because its tail
  is one always-zero bit rather than a temperature pair — ease of filling over strength of
  evidence. Emission is now interleaved high-rate, which also supplies the chart state the modular
  prefix needs, delivers each `0x28` exactly once, and lands where the two epoch models agree.
- **One-poll insertion was not genuine interleaving.** The failed 2026-08-01 build emitted a
  generated `0x28` for one ~1 ms USB poll, then returned to freshly advancing `0x1E`, while each
  form owned a different tick epoch. A genuine BLE PDU is held as the current USB snapshot until
  the next native notification. The replacement has one clock, one elapsed boundary, and one held
  PDU at a time; its host test pins `0x1E → 0x28` continuity.
- **The corrected scheduler also failed on hardware.** Its live counters matched the intended
  shared design (358 batches, 1088 carriers, 10140 held polls, one fallback/starvation, zero
  overlong or saturation), yet the stationary camera still swept through large rotations. This
  isolates the next defect above scheduling: the sole gyro vector was one noisy instantaneous
  sample rather than the tick-weighted rate area already integrated by the carrier.

Two latent defects surfaced while fixing those, both of the same class — a convenience that
quietly corrupts real data, invisible to any test that compares our code against our own code:

- both packers used `status ? status : default`, which cannot distinguish a genuine zero from an
  unset field. 5 of 858 genuine packets carry status `0x00`; the idiom rewrote them to `0x0D`.
- the decoder branched on elapsed alone, but **`packing_mode` is the layout discriminator**. Five
  packets carry mode 0 with status `0x00`, elapsed 0 and tick 127 identical across three captures —
  a different structure that was being mislabelled high-rate and inflating that corpus by five.

**Readiness gate:** `python tools/ns2_motion40_validate.py` — 0 FAIL, 0 UNKNOWN. It exists because
three "offline-validated" claims preceded the hardware failure; every test then in place compared
one of our implementations against another, and a consistency test cannot detect a wrong semantic
choice. Byte-exact validation of a *generated* `0x28` is impossible from BLE captures (the 800 Hz
source samples and the epoch-instant carrier are never transmitted), so the gate scores physical
accuracy against interpolated truth, worst-capture, with per-field tolerances.

The gate now also compiles the actual C translators and validates a deterministic physical stream
with an independent Python model. All 17 complete `0x1E -> 0x28 -> 0x1E` loops pass; maximum prefix,
gyro, and acceleration errors are 0.000011°, 0.429 count, and 0.0019 count. Six intentional recipe
corruptions are rejected individually: wrong prefix epoch, half acceleration, half gyro, swapped
gyro axes, detached elapsed, and stale following carrier. Its first pre-fix run is what exposed the
5.23% acceleration-gain discontinuity.

The last UNKNOWN was the gyro factor-of-two. It required no new capture: the ICM-42670-P datasheet
and retained Pro2 common-report evidence fix the sensor at `±2000 dps / 16.4 counts/dps`, so the
existing moving corpus identifies the high-rate gyro binary point as `/128`, not acceleration's
`/256`. Carrier-rotation recovery improves from median `0.554` to `1.108`; two independent captures
land at `1.000` and `0.994`. The generator now multiplies calibrated DualSense gyro counts by 128.

The unresolved boundary is controller-private FIFO/filter/state behavior: offline data cannot show
whether the console accepts a synthesized 3-carrier/1-batch history exactly like a genuine one, or
whether the modal temperature tail participates in that validation. The shared tick/elapsed
relation, gyro area, acceleration gain, and prefix/carrier trajectory are no longer independent
guesses. `X`/`Y` handedness continues to inherit the console-validated `0x1E` axis map. These are
recorded limits, not an active request for another A/B.

An orthogonal upright lazy-susan rotation remained state 3 throughout. A corpus audit now records
1,030 stable state-1 samples but initially no adjacent state-1 boundary. Passive gameplay
triggers subsequently supplied the clean state-0/state-1 seam, the held-out opposite-sign branch,
and the missing reciprocal state-2 crossing.
Production interval and fresh native
ownership were restored after the campaign. See
[`docs/experiments/pro2-mode3-carrier-prefix-2026-07-29.md`](docs/experiments/pro2-mode3-carrier-prefix-2026-07-29.md),
[`docs/experiments/pro2-carrier-chart-transition-2026-07-29.md`](docs/experiments/pro2-carrier-chart-transition-2026-07-29.md)
and
[`docs/experiments/pro2-raw-native-motion-pcap-2026-07-29.md`](docs/experiments/pro2-raw-native-motion-pcap-2026-07-29.md).

The audio runner is observational by default and the new `audio headset` UART command is read-only.
The firmware host model validates and reassembles complete `0x0D` transfers, but the dedicated
on-device flash sink remains unimplemented; generic 24-byte traces are rejected as insufficient.
The current-image audit finds a 1 MiB candidate region at
`0x2FA000..0x3FA000` on Pico 2 W and `0x0FA000..0x1FA000` on Pico W, but those
addresses are **not yet linker-reserved** and must not be written. All new Python/PowerShell unit
tests pass. The motion workflow is hardware-validated; audio and firmware-tap campaigns remain
pending. See
[`docs/re-methodology/controller-protocol-lab.md`](docs/re-methodology/controller-protocol-lab.md).

First result from the corpus analyzer, byte-exact over all 16 local Air Riders dumps: they are
4 riders × 4 machines, with 4 encrypted-body groups, 4 SRAM groups, 16 distinct (body, SRAM) pairs,
and 4 UIDs each shared by 4 files. **Rider identity is entirely in the encrypted body and machine
identity is entirely in the SRAM window; the axes are orthogonal.** Confidence: Confirmed. This is
the evidence that four captures of one physical figure cannot establish a field as constant — the
assumption that produced the false SRAM-CRC constant and the fixed Kirby record table.

### v3 state machine is host-replayable — ✅ 2026-07-29

`ns2_v3_serve()` and its twenty file-scope statics moved out of the USB personality into
[`src/nfc/ns2_amiibo_v3_runtime.c`](src/nfc/ns2_amiibo_v3_runtime.c), behind the same shape the 540
path has always had. `src/switch_pro2/switch_pro2.c` keeps only the transport (866 lines removed,
128 added; exactly one line outside the extracted block changed, the report-state accessor).
Durable side effects go through `ns2_amiibo_v3_host_t`, so a host test can inject an apply failure
or a pending flash write.

`tools/test_ns2_amiibo_v3_runtime.c` replays the real console sequences with a fake clock: the
recognition read including descriptor escalation and the 83-byte device result, the Air Riders
clear/update/write lifecycle including the Stop that must not eject, the `0x1E` reuse read, the
persistence-gated eject with its 3-second cooldown, and the mid-transaction generation edge.

Every v3 failure still reaches the console as status `0x07` / detail `0x41` → `2115-0096`, but
`ns2_amiibo_v3_error_t` now records which of eight internal rules fired, with the specific
`ns2_virtual_nfc_result_t` and `0x14` stage offset. `amiibo v3diag` reports it. This is the
ambiguity that caused a fail-closed record rejection to be misdiagnosed as the earlier
tag-removal timing bug.

**Verification: static, build, and hardware.** Before: 53/53 host tests, both boards clean. After:
54/54 host tests (the new replay suite), both boards clean, install-reset markers verified,
+608 B pico2_w / +1120 B pico_w.

Hardware-confirmed on a real Switch 2, 2026-07-29 (King Dedede, UID `0465B0228F2190`):
`dumps/experiments/20260729-101834-v3-post-extraction/`. Scan → in-game save → remove → rescan all
succeeded. The firmware reported `write_commits:1`, `extended_completions:1`, `dev_results:4`,
`write_errors:0`, and **`errors:0` / `last_error:"none"`** — the new internal counter confirming no
rule fired. Store generation advanced 24 → 25 with both journal banks valid.

A second run the same day (`dumps/experiments/20260729-102744-v3-reuse/`, King Dedede & Winged Star,
scan → save → remove) independently confirmed it: cumulative `write_commits:3`,
`extended_completions:2`, `dev_results:9`, `errors:0`.

It also produced a **third** Air Riders allocation — sector-0 page `0x9A`, sector-1 pages
`0x19/0x1A` — which resolved the allocation model. It is a **slot index**: slot *n* occupies
sector-0 page `0x92 + 8n` (32 B) and sector-1 page `25n` (100 B), and the tag holds exactly ten
slots. The 355-byte clear wipes 80 sector-0 pages = 10 × 8, and the runtime's two existing bounds
checks independently permit slots 0–9 and reject slot 10. Observed: Kirby slot 0, Dedede & Winged
slot 1, Dedede & Tank slot 4.

⬜ What selects the slot is unknown; it is **not** identity — those last two images share UID
`0465B0228F2190`, differing only in machine SRAM. Any UID- or rider-keyed table would have failed
this run. Detail:
[docs/Amiibo-v3.md](docs/Amiibo-v3.md) §8.

That first run also corrected the trace decoder. It flagged one `07 41` as a failure; the firmware said
zero errors. Status `0x07` / detail `0x41` is *also* the deliberate TagRemoved signal that
`finish_committed_eject()` emits after a committed write, because the console needs that edge to
leave its amiibo UI. `error_context()` now separates removal edges from failures, and `nfc_lab.ps1`
cross-checks the decoded trace against `v3diag`'s own counter and says so when they disagree. This
was the retrospective's own lesson — treating a wire value as a diagnosis — reproduced inside the
analysis tooling.

## Current release

[`v1.5.0`](https://github.com/notsosaelin/PicoSwitch2/releases/tag/v1.5.0) was published on
2026-07-22 with Pico W and Pico 2 W UF2 assets. All 35 host-test executables pass. This release adds
hardware-confirmed genuine Pro Controller 2 native-motion passthrough, UART protocol tracing,
current firmware identities, and bonded HOME reconnect through BTstack SM. Twenty consecutive
controller-off/HOME cycles restored input, P1 LED, and gyro without SYNC. Pico 2 W retains its
300 MHz live-audio/native-haptic build; Pico W retains its validated non-audio configuration.

The post-release `ns2-testing` branch also has hardware-confirmed genuine Pro Controller 2 headphone
output. Its 240-byte Opus/CELT framing now produces clean console audio while preserving input,
native gyro, rumble, headset insertion/removal, LED behavior, and BOOTSEL handling.

DualSense and DualSense Edge motion translation is also hardware-confirmed in Splatoon 3. The
production path emits the decoded length-`0x1E` Switch 2 quaternion carrier and preserves input,
audio, haptics, reconnect, LED, and BOOTSEL behavior. The unsafe static-template `0x28` generator
was removed. A later complete generator and genuine-base hybrid harness remain default-off
diagnostic infrastructure; their hardware campaign is documented and intentionally deferred.

The USB side of Config mode is now CDC-only in source and automated builds. The read-only MSC
drive, embedded FAT image/web page, callbacks, and generator were removed;
`tools/run_config_portal.ps1` serves the production portal locally. This removes 100,104 bytes
from the Pico 2 W binary and 100,160 bytes from the Pico W binary. Config enumeration, Virtual
Amiibo transfer, save/readback, and direct BOOTSEL exit are hardware-confirmed with the CDC-only
USB descriptor.

The same local portal and Android app now use one bonded/encrypted BLE management transport in
Config or a normal controller personality. The peripheral-role link remains separate from HID
controller slots, controller discovery may coexist with it, and an allowlisted production command
set executes through the existing core-0 parser. Standard builds boot with management on;
`mgmt off` disables it for the current boot. The recovery soak is hardware-confirmed, while the
first-bond/encryption and active audio/gyro/wake/latency matrices remain pending.

Virtual Amiibo is now always available rather than controlled by a stored toggle. A blank adapter
presents no virtual tag and can still fall through to a real reader source. Each browser profile
keeps its own user-supplied library as locally validated mutable records; AmiiboAPI supplies
optional ordering/details, and v3 rider/machine variants use content-derived keys. Neither
firmware nor the site ships tag images.
One browser-local loaded-slot pointer tracks the selected adapter image. A newly flashed UF2
performs a one-shot erase of all five PicoSwitch2 persistence sectors, clearing settings, both
virtual-tag banks, wake identity, and Bluetooth bonds. Ordinary power cycles retain state.
The board stores exactly one amiibo; the alternating flash banks are persistence generations, not
two active amiibo. The production manager is a single-slot layout: connected Load amiibo sends the
highlighted entry to the adapter, offline Select amiibo remembers it, and Sync amiibo pulls
console-written data back into the validated browser copy. Load/Select and Import/Sync occupy one
context-sensitive center action. On connection the portal selects the adapter's active cached entry
once, including a dirty same-UID v3 image, without forcing the carousel back on later status polls.
One merged button is uniformly labeled
"Eject amiibo"; its tooltip, confirmation, and `amiiboEjectActionState()` scope determine whether
it removes only the loaded pointer, wipes the adapter, or does both. Adapter-destructive modes
discard the stored image and both flash journal banks through `amiibo clear`; cancelling aborts
everything, and library dumps are never deleted. The console-driven Stop/write-back lifecycle is
unchanged.

The Virtual Amiibo library is **import-only**: users supply their own genuine dumps (single file or
recursive directory). A 2026-07-26 hardware test showed the Switch 2 validates amiibo cryptography,
so key-free generated images are rejected ("This isn't an amiibo") even though the portal identifies
them, and a random-UID "Random Mode" was removed because a runtime UID swap invalidates the
UID-bound tag HMAC. A key-based identity generator was removed, but the smaller browser-local
rewrite path remains for explicit Initialize on an imported dump: it requests the user's own
`key_retail.bin`, clears and re-signs ordinary or v3 save state, and self-verifies before replacing
the IndexedDB copy. It works without an adapter and does not create catalog identities. Research is
retained in
[`docs/switch2/amiibo-identity-and-generation.md`](docs/switch2/amiibo-identity-and-generation.md)
and [`docs/experiments/generated-amiibo-console-rejection-2026-07-26.md`](docs/experiments/generated-amiibo-console-rejection-2026-07-26.md).
The library exports/imports as a flat **.zip** (`library.json` manifest + one `.bin` per amiibo;
legacy `.json` backups still import). Directory imports fill the visible library progressively. The
carousel wraps deliberately at the ends while keeping the visible neighbor window non-wrapping,
and shows the centered amiibo's release date above it. The centered
artwork is fixed in the middle at 100% size; four non-overlapping neighbors on each side use exact
80/60/40/20% scaling. Movement is animated, names are omitted from the carousel, mouse-wheel,
touch-swipe, and keyboard-arrow navigation are supported, and the redundant visible arrows/count
are removed. Game-series, amiibo-series, and product-type chips cycle `All` followed by the
imported library's available values alphabetically without changing AmiiboAPI source order and sit
together below the compact primary action. Search stays in the compact header toolbar. Clicking
the centered artwork toggles a non-modal context drawer for
compatibility metadata and secondary/destructive actions; the center button alone owns
Load/Select/Import/Sync. Physical-tag scanning remains hidden behind a future firmware capability
rather than presenting a dead production control. The new presentation edge is
host/static-regression clean; real-console manual Eject/re-present validation is pending.

Controller-family **button** remap persistence and its portal UI have been removed. Firmware uses
one locked, regression-tested physical-to-Nintendo base map; user button remapping belongs to the
emulated controller in the Switch UI and therefore survives a change of source controller.
Controller appearance is intentionally retained: the portal exposes the shared Pro2 body/Sony
lightbar color and independent Joy-Con 2 Left/Right accents. Config schema v10 stores appearance
and wake identity only. The production page presents these three colors in one compact panel and
no longer shows the obsolete current-input/current-output identity cards. Bluetooth adapter identity
now uses the single name `PicoSwitch2` across Classic GAP/EIR, BLE GAP/ATT, and Config advertising.
Android retains a legacy `Joypad Adapter` discovery matcher for pre-name-change firmware; saved
addresses and link keys, not names, remain bond authority.

## Hardware-confirmed behavior

| Area | Status | Evidence |
|---|---|---|
| Switch 2 Pro Controller 2 USB identity and input | ✅ Confirmed | Real Switch 2 and PC/Steam |
| NSO GameCube USB identity and input | ✅ Confirmed | Real Switch 2 |
| NSO GameCube rumble | ✅ Confirmed | Real Switch 2; genuine-capture decoder |
| Real Pro Controller 2 input in NSO GameCube mode | ✅ Confirmed | L/R full-pull detents; ZL/ZR become GC ZL/Z |
| Joy-Con 2 Left and Right enumeration/input streaming | ✅ Confirmed | Real Switch 2 |
| Joy-Con 2 Left PC/Steam classification | ✅ Confirmed | Fresh Windows node and Steam UI; SDL Switch 2 driver enabled |
| Joy-Con 2 Left and Right sideways mappings | ✅ Confirmed | Real Switch 2; face/shoulder/trigger/stick profile |
| Joy-Con 2 rumble and STOP/reconnect behavior | ✅ Confirmed | Real Switch 2 |
| Joy-Con 2 Bluetooth mouse bridge | ✅ Confirmed | Mouse-only feature gating, pointer activation/motion, buttons, disconnect cleanup, and wheel-to-stick menu navigation |
| Switch 2 controller firmware identity/update status | ✅ Confirmed | Genuine `0x10/01` replies plus Switch 2 Update Controllers; Pro2, NSO GC, and both Joy-Con 2 personalities report up to date |
| Out-of-band UART protocol tracer | ✅ Confirmed | Real Switch 2 + genuine Pro Controller 2 source; complete 63-record Pro2 re-enumeration capture, zero overwrites, pull-transport framing validated |
| UART trace decoder and semantic differ | ✅ Host + live-capture confirmed | Known EP0/bulk/HID fields, sensitive-field redaction, strict comparison, timestamp wrap, corruption rejection, and two-capture Pro2 A/B workflow |
| Genuine Pro Controller 2 native motion passthrough | ✅ Confirmed | Splatoon 3 axes/aim, stationary behavior, power-off hold, and 20 consecutive controller-off/HOME reconnect cycles without SYNC; input, P1 LED, and native `0x1E`/`0x28` motion restore at 133 Hz |
| DualSense/Edge → Switch 2 motion translation | ✅ Confirmed | Splatoon 3 direction, scale, rapid movement, stationary behavior, reconnect recovery, and coexistence with input/audio/haptics using the length-`0x1E` carrier |
| DualSense and DualSense Edge input | ✅ Confirmed | Real Switch 2 and Steam |
| Edge paddles, Fn buttons, and mute mapping | ✅ Confirmed | Real hardware |
| DualSense/Edge LEDs and rumble | ✅ Confirmed | Real hardware after report-boundary scheduler fix |
| Pro2 body/Joy-Con accents, Sony lightbar matching, and DualSense player-slot dots | ✅ Confirmed | Real Switch 2 and DualSense; config v8 hardware pass |
| BOOTSEL report-boundary scheduling and former double/triple/hold policy | ✅ Confirmed | Real hardware after report-boundary gesture service |
| Revised single/double/triple/two-second BOOTSEL action matrix | ✅ Confirmed | Live-console single-tap cycle, paired double-tap bond-preserving pairing, triple-tap wipe/admission blocking, and two-second Config entry/exit are hardware-confirmed |
| BLE management transport (Config **and** in-band via `g_mgmt_enabled`) | 🟡 Host/build confirmed; hardware pending | Production-default-on service; shared parser, bounded bridge, allowlist, 16-byte ATT encryption, durable-bond callback checks, pairing-window-only first bond, and local Web Bluetooth portal. Android Just Works has no MITM and is not mislabeled authenticated. |
| Virtual Amiibo persistence and mutable single-slot library | 🟡 540 and v3 read/write/persistence hardware-confirmed; portal refactor pending | All 16 available v3 dumps completed real-console reads/writes; v3 Config Sync, reset-on-UF2, and Config BLE still require regression validation |
| Late BLE DIS VID/PID handoff and input continuity | ✅ Confirmed | Xbox Series BLE hardware regression after notification-first identity fix |
| Triple-tap post-wipe admission lock | ✅ Confirmed for the reported workflow | Wipe disconnects and requires an explicit new pairing window |
| Explicit re-pair after triple-tap wipe | ✅ Confirmed | Real hardware |
| Switch 2 wake from sleep | ✅ Confirmed | First real post-sleep controller input on real Switch 2 hardware |
| Reconnect/triple-tap false-wake protection | ✅ Confirmed | Most-controller pass plus genuine Switch 1 Pro initialization/reconnect regression |
| 8BitDo NGC Modkit rumble | ✅ Confirmed | Real hardware with BlueRetro-derived `0xA5 / DB LL RR` output |
| Retro Fighters BattlerGC Pro mapping | ✅ Confirmed | Pairing, labels, shoulders, analog/click triggers, L3/R3 suppression, and separate Home report |
| 8BitDo Ultimate Bluetooth P1/P2 | ✅ Confirmed | Custom firmware transport maps independent paddles to GL/GR and preserves wake |
| Bluetooth battery passthrough | ✅ Confirmed | Native HID/BLE sources and console-native USB power fields |
| Pro Controller 2 UAC1 USB audio function | ✅ Confirmed | Windows audio endpoints start without Code 10; no controller regressions |
| Genuine Pro Controller 2 headphone audio — Pico 2 W | ✅ Confirmed | Clean Switch 2 console audio through the physical jack; input, gyro, rumble, headset lifecycle, LED, and BOOTSEL regression checks pass |
| DualSense Bluetooth internal-speaker audio — Pico 2 W | ✅ Confirmed | Standard 300 MHz build; 13,225/13,225 PCM blocks encoded, zero drops/errors |
| DualSense Bluetooth internal-speaker audio — Pico W | ❌ Not supported | Fixed-point/XIP 300 MHz experiment barely played audio; standard build restored to validated non-audio profile |
| Standard 300 MHz Pico 2 W platform regression | ✅ Confirmed | LED/BOOTSEL, config persistence/readback, cold boot, and ten wake attempts per known controller |
| Standard 300 MHz Pico 2 W extended stability soak | ✅ Confirmed | Eight-hour Smash session with no observed thermal or stability issue; temperature was not instrumented |
| DualSense audio after bonded reconnect | ✅ Confirmed | Controller and dongle power cycles restore audio and native rumble through the saved bond; no fresh pair required |
| Switch 2 headset insertion and output | ✅ Confirmed | Physical DualSense jack is recognized; console audio plays through connected headphones with input/rumble/wake intact |
| Switch 2 headset removal/reinsert | ✅ Confirmed | Repeated cycles restore input, audio, and native haptics; unplugged full legacy rumble remains stable |
| DualSense rumble during console audio | ✅ Confirmed | Native-mode restoration, capture-derived peak preservation, and the waveform-preserving 3.25× curve are stable and judged close to HD Rumble |
| DualSense rumble without headset/audio | ✅ Confirmed | Pico 2 W reuses the native renderer with valid Opus silence only during active rumble plus a bounded two-packet STOP tail; Pico W retains compatibility rumble |
| Pico W and Pico 2 W builds | ✅ Compile-confirmed | Pico W uses the validated non-audio profile; Pico 2 W includes live audio at 300 MHz |

## Current USB personalities

Every boot starts in Pro Controller 2 mode. With a controller HID-ready, a single BOOTSEL tap
advances the volatile controller-only cycle:

1. Switch 2 Pro Controller 2 (`057E:2069`)
2. NSO GameCube Controller (`057E:2073`)
3. Joy-Con 2 Left (`057E:2067`, experimental)
4. Joy-Con 2 Right (`057E:2066`, experimental)
5. Back to Pro Controller 2

A two-second hold enters CDC/configuration mode (`CAFE:4012`) directly from any controller
personality; a two-second hold in Config returns directly to Pro2. Config is never part of the
single-tap cycle. The selection is not persisted across power cycles.

## Bluetooth and BOOTSEL architecture

- Core 1 runs BTstack plus the vendored joypad-os HID layer.
- A persistent global pairing lock is installed before triple-tap disconnect/erase begins. Only an
  explicit double-tap pairing window reopens admission.
- One dongle serves one controller. Background BLE scan and Classic inquiry run only while no
  controller is connected; once a controller is HID-ready the pairing window closes (LED goes
  solid) and discovery idles, freeing Bluetooth bandwidth. The host stays connectable/discoverable,
  so a bonded Classic controller reconnects by paging in and a bonded BLE controller reconnects once
  discovery resumes at zero connections. Hardware-confirmed (Classic + BLE reconnect, wake, wipe/
  re-pair). Retiring the always-on multi-controller discovery is the general fix for the scanning
  radio contention noted in
  [`docs/switch2/audio-passthrough-research.md`](docs/switch2/audio-passthrough-research.md).
- Config/management is a separate BLE Peripheral role, armed by `config_ble_authorized()` — the
  explicit Config USB personality **or** the in-band management flag `g_mgmt_enabled` (production
  default on; a runtime `mgmt off` lasts until reboot).
  Management advertising no longer stops controller discovery; the two roles are independently
  scheduled, and the 5.4-hour recovery soak confirms discovery resumes after controller drops.
  When neither trigger is set, the normal controller path performs only a mode-state comparison and
  generates no management radio traffic (byte-identical to before the feature).
- Switch 2 controllers use a custom ATT pairing handshake, so the wipe policy cannot depend only on
  BTstack's LE bond database.
- Successful custom pairing persists the normalized LTK in both the reconnect record and BTstack's
  LE database with RAND/EDIV zero. HOME reconnect must run through `sm_request_pairing()` so
  BTstack restores its bonded security state; issuing raw HCI encryption alone encrypts the ACL but
  leaves the controller in its running-LED/pre-active state. After SM success the dongle restores
  ACK/input CCCs, reasserts P1, and reruns the validated native-motion feature sequence.
- Core 0 samples BOOTSEL using a cooperative cross-core SRAM handshake at a 30 ms cadence.
- Incoming HID report boundaries service raw BOOTSEL sampling, gesture recognition, and
  `bthid_task()`. This prevents sustained DualSense Classic traffic from starving controller output
  or button gestures. The timers remain the quiet/disconnected fallback.
- BLE HID binds immediately from the best identity available and enables report notifications
  before querying Device Information Service. A later DIS VID/PID is always handed to BTHID for an
  idempotent re-evaluation; contradictory Xbox BLE, Stadia, and MouthPad name matches can no longer
  pin the wrong parser while input is already streaming. The updated path is hardware-confirmed
  with Xbox Series BLE.

See [`docs/architecture/overview.md`](docs/architecture/overview.md) and
[`docs/bluetooth/btstack-implementation.md`](docs/bluetooth/btstack-implementation.md).

## Known open issues

| Priority | Issue | State |
|---|---|---|
| **P1** | **In-band management ↔ controller disconnect/recovery failure** | 🟢 Recovery HW-confirmed. Decoupling fix (commit `68271a0`): a **5.4 h soak** held a Classic controller + management client stable through **10 re-enumerations**; across **3 controller disconnect/reconnect cycles** (all clean, controller-initiated `reason 0x13` — idle-sleep or low battery) the **controller reconnected on its own** (`ctrl False→True` at 05:48:48), management stayed connected throughout (`mgmt.disconnects=0`), and discovery resumed every time (`scan.starts=3`, `suppress.mgmt_armed=0`). **No power cycle ever needed** — the "controller fails to reconnect" symptom is gone. The original `disc=0` wedge was not reproduced (inferred-sufficient, not proven); the `pipe` diagnostic (built, unflashed) makes any recurrence decisive. Remaining: an active-use (console-awake, charged controller) coexistence pass. See [`docs/experiments/overnight-investigation-2026-08-13.md`](docs/experiments/overnight-investigation-2026-08-13.md) |
| **P2** | **Controlled re-enumeration after host-visible changes** | 🟡 Implemented and host/build validation pending hardware: bonded command `reenumerate` queues the existing same-personality core-0 USB detach/reset/reconnect path, preserving output personality and Bluetooth state. Android and the portal expose an explicit apply action after saving colors. Owner must confirm the console refreshes color and controller input recovers cleanly; mapping remains intentionally absent per project policy. |
| **P3** | **Web Portal state refresh after Amiibo Sync** | 🟡 Root cause found + fixed in `web/index.html` (2026-08-12): `amiiboInfoCache` (key→decrypted owner/nickname/dates/writeCount) was not invalidated on Sync, so the info box re-read stale metadata under the reused key (the sibling `initializeSelectedAmiibo` invalidates; `saveCurrentAmiibo` did not). Fix adds the same `amiiboInfoCache.delete(...)` before re-render. **Needs browser+adapter validation** (frontend, not host-testable) |
| **P3** | **In-band management production default** | 🟡 Resolved in code: standard builds boot with bonded/encrypted management on; `mgmt off` is a current-boot escape hatch and reboot restores on. No settings-schema migration or flash persistence was added. Physical first-pair, bonded reconnect, unbonded-write rejection, and reboot behavior remain to validate. |
| P2 | DualSense microphone return | 🟡 Headset presence is implemented; microphone Opus decode and USB return remain |
| P2 | Let reconnecting BLE controllers sleep with the console without touching bonds or admission | 🔵 Research concluded: no safe generic host-only path; controller-specific evidence required |
| P3 | Additional controller IMUs → console-native report `0x09` translation | 🔵 Native Pro2 passthrough and DualSense/Edge synthesis are confirmed; each remaining family needs verified calibration, axis, scale, and timestamp handling |
| **P2** | **AYN Thor gyro axes** | 🟡 Root cause identified 2026-08-14: the app never measured display rotation (`Context.getDisplay()` throws on an application context, API 30+), so Android's natural-orientation sensor frame was published unrotated. Fixed via `DisplayManager`; awaiting the in-game pitch/roll check and the rotation value the app now logs. |
| **P2** | **AYN Thor rumble** | 🔴 Never produced any rumble. Signal proven present as far as the Android vibrator service, which discarded it for `USAGE_TOUCH`; three app fixes are unvalidated. Firmware half now answerable in one UART `rumble` read. |
| P3 | `build\pico_w_switch1` (`NS2_PRO=OFF`) does not compile | 🔴 Pre-existing at `HEAD`, unrelated to this pass: `src/config.c` references `g_usb_reenumerate_request_pending` outside an `NS2_PRO` guard. Verified by building a clean stash of `HEAD`. |
| P3 | NFC/amiibo transactions | 🟡 Genuine Pro2 physical-tag reads and the complete Virtual Amiibo read/write/persist/eject/re-present/library workflow are hardware-confirmed. Native physical writes, production native-reader gating, and Switch 1 translation remain open |

## Validation

Current automated coverage includes:

- DualSense Bluetooth output report layout and CRC
- Switch 2 player-LED command-mask decoding
- Xbox rumble payload construction and STOP semantics
- Genuine-capture NSO GameCube rumble decoding
- GameCube and Joy-Con 2 input report encoders
- Joy-Con 2 per-side identity and configurable accent placement
- HID output normalization
- Retained UART protocol-trace disabled mode, payload truncation, chronological wraparound, and
  overwrite accounting
- Native Pro2 motion snapshot validation for length-30/length-40 packets, source-slot ownership,
  freshness and timer wrap, malformed input, disconnect hold, and clear semantics
- DualSense calibrated motion translation, smallest-three quaternion encoding, carrier boundaries,
  and timing/bias handling; exact historical length-`0x28` alias packing plus the corrected
  raw-report and packed multi-sample PCAP decoders
- UART trace JSONL validation, known-field decoding, default sensitive-data redaction, timestamp
  rollover, address-aware semantic alignment, and strict raw-prefix comparison
- Switch 2 pairing cryptography
- Switch 2 wake identity parsing and byte-exact advertisement construction
- Automatic wake policy across reconnect startup state, per-controller session cleanup, repeated
  held reports, BOOTSEL triple-tap maintenance suppression, and Switch 1 Pro initialization
  quarantine
- USB personality cycling
- BOOTSEL paired/unpaired/Config action policy, including the controller-only single-tap cycle,
  bond-preserving paired double-tap handoff, triple-tap wipe, and two-second Config toggle
- BOOTSEL gesture timing under timer-only, report-only starvation, and mixed scheduling
- Late BLE identity correction, including provisional and generic binding, transport filtering,
  idempotent confirmation, and input notifications immediately before and after a driver rebind
- Battery decoding for DualShock 3/4, DualSense, Switch Pro, Wii U Pro, and Wiimote; recurring BLE
  BAS updates with native-HID priority; and power-field encoding for Switch 1 Pro, Pro Controller 2,
  NSO GameCube, and both Joy-Con 2 personalities
- First-generation 8BitDo Ultimate Bluetooth identity gating and P1/P2 signature conversion to
  L4/R4 (GL/GR by default), including simultaneous and ordinary-input preservation
- 8BitDo NGC Modkit rumble report framing, per-profile VID authorization, and send-result
  propagation
- `gcusb` safety and protocol helpers
- Pro Controller 2 UAC1 descriptor topology, advertised Feature Units, both 192-byte isochronous
  streams, RP2040/RP2350 allocation path, and full mute/volume request surface
- DualSense audio report `0x39`/`0x32` byte layout and CRC, physical-jack parsing,
  Nintendo headset-state encoding, exact reconnect transport selection, native-haptic
  start/STOP lifecycle, interval peak preservation, plus the fixed 512-to-480 stereo
  resampler's constant-signal, channel-isolation, and ramp behavior
- Virtual amiibo 540/572-byte validation and transactional upload, exact export, dirty-state
  protection, a 61-byte status codec, the primary-capture-confirmed 600-byte reader buffer and
  70-byte offset chunks, a 64-byte write-preparation buffer, exact-UID write selection, atomic
  454-byte staged-write validation, generation-safe RAM commit, and modulo-eight NFC events
- Virtual Amiibo internal baseline/latest-write recovery, automatic console-write persistence
  request, deferred removal until persistence, version-1 migration, and
  alternating version-2 flash-bank CRC verification
- UART-gated genuine Pro Controller 2 NFC relay: extended `0x0016` command framing, asynchronous
  response translation, report-state passthrough, bounded timeout handling, and one
  hardware-confirmed physical amiibo read recognized by the Switch 2
- Loaded-tag-gated Virtual Amiibo runtime using the same 600-byte/chunk model, hardware-confirmed with
  an uploaded tag and a non-NFC source controller; the guarded transactional write completes on a
  real console without crashing, including complete 88-byte chunks, commit, and `05 00`. Logical
  post-write removal, next-scan re-presentation, same-session updated readback, and generation-safe
  UART export of a genuinely mutated 540-byte image are hardware-confirmed.
- Console vendor-OUT stream reassembly for the 88-byte `0x14` write command, including exact
  64+24-byte split reproduction, arbitrary fragmentation, coalesced commands, oversized-command
  discard/recovery, and USB-mount reset
- Live UART Virtual Amiibo export with generation-stable 64-byte pulls, PC-side exact-length and
  UID/BCC validation, and dirty acknowledgement only after the binary is safely written
- Config-portal recursive directory scanning and browser-local IndexedDB caching for all 1,035
  validated maintainer collection files; selected-tag identity/catalog display and cache-first
  replacement of console-written save data
- Offline production-library access, exact catalog/content deduplication, one loaded-slot pointer,
  and versioned full-library export/import backup preserving each mutable dump
- Standalone no-serial Virtual Amiibo diagnostic page with a separate simulated adapter slot,
  transactional chunk/CRC checks, controlled write injection, cache-first save-back, persistence,
  known-ID AmiiboAPI verification, and browser self-test
- Production and diagnostic amiibo libraries now use artwork carousels. The production carousel
  displays only imported owned files, fills during directory scanning, centers enlarged selected
  artwork with four progressively smaller neighbors on each side, animates navigation, preserves
  AmiiboAPI order, and filters by tap-to-cycle game/amiibo-series and product-type chips. The
  production manager now uses one context-aware center action, compact search, active-tag
  auto-selection on connection, save metadata below the artwork, and a non-modal details drawer
- Config mode links as CDC-only with a compile-time-checked descriptor and no MSC/web-disk symbols;
  both local portals pass JavaScript, DOM-reference, and localhost delivery checks
- Config/in-band BLE command transport with fragmented-write assembly, one-command backpressure,
  response chunking, session invalidation, stale-response rejection, and production-command
  allowlisting; the browser uses the same settings/Amiibo UI over Web Serial or Web Bluetooth

The firmware builds under the Pico SDK 2.2.0 toolchain. The standard `pico_w`
artifact retains its validated non-audio clock, memory layout, and Bluetooth
scheduling. The standard `pico2_w` artifact uses the hardware-confirmed
floating-point/SRAM audio path at 300 MHz/1.20 V. Both legacy `NS2_PRO=OFF`
Pico W build directories also pass their compile gates. The current workspace has 67
passing host-test executables, including battery decoder/source/encoder, DualSense
audio packet/control/tone/resampler, native-haptic lifecycle, peak preservation, and
bonded-reconnect transport suites, plus the virtual-tag store/codec, vendor transfer pump,
Config/in-band BLE cross-core bridge, locked base mapping, the NTAG I2C 2K data model, and its
capture-derived staged-write codec.

NTAG I2C 2K (Kirby Air Riders "figure v3") support is active. The portal imports, loads, and can
read/sync the complete 2048-byte image. The console read path is hardware-recognized; the isolated
write path, dirty/readback status, and power-safe journal integration pass host and board builds
but await the real-console write lifecycle described in
[`docs/Amiibo-v3.md`](docs/Amiibo-v3.md) §18.3.

Config v10 stores only the Pro Controller 2 body color, independent Joy-Con 2 Left/Right accents,
and learned wake identity. Every newly flashed UF2 starts from defaults, a blank virtual-tag
store, and no Bluetooth bonds; this is intentionally different from an ordinary reboot. Joy-Con
accents default to genuine retail values (`9B E1 E6` Left, `FF 8C 5F` Right). Each personality
advertises its configured appearance during enumeration, and the active Pro2/Joy-Con color drives
supported DualShock 4/DualSense lightbars independently of player-indicator LEDs. The locked base
button map replaces the retired per-family remap table.

## Documentation map

- [`docs/README.md`](docs/README.md) — documentation index and authority rules
- [`docs/status/compatibility-matrix.md`](docs/status/compatibility-matrix.md) — controller/personality validation
- [`docs/architecture/overview.md`](docs/architecture/overview.md) — runtime architecture and data flow
- [`docs/architecture/config-transports.md`](docs/architecture/config-transports.md) — USB Serial and bonded/encrypted in-band BLE management
- [`docs/re-methodology/evidence-standards.md`](docs/re-methodology/evidence-standards.md) — evidence tiers and experiment rules
- [`docs/re-methodology/nfc-investigation-workflow.md`](docs/re-methodology/nfc-investigation-workflow.md) — NFC/amiibo lab tooling and phase order
- [`docs/switch2/`](docs/switch2/) — Pro Controller 2 protocol
- [`docs/switch2-gc/`](docs/switch2-gc/) — NSO GameCube protocol and mapping
- [`docs/switch2-joycon2/`](docs/switch2-joycon2/) — Joy-Con 2 protocol and mapping
- [`docs/bluetooth/`](docs/bluetooth/) — Bluetooth host, identity, pairing, and controller profiles
- [`docs/experiments/`](docs/experiments/) — immutable experiment records and refuted hypotheses

## Next recommended work

**Hardware pass, in this order (2026-08-14 changes):**

1. Flash `build\pico2_w\PicoSwitchWGA-pico2_w.uf2`, install the rebuilt companion APK, and play
   any motion game with the Thor. Read the app's diagnostic log line
   `motion frame — screen rotation Ndeg`. If it reads 90/270 and pitch/roll are now correct, the
   rotation defect was the cause and Thor motion is closed. If it reads 0, the Thor is
   natural-landscape, the fix is a no-op, and the cause is elsewhere — say so and stop; do not
   start tuning.
2. In the same session, trigger console rumble and read UART `rumble`. The interpretation table in
   [`docs/agents/RUMBLE.md`](docs/agents/RUMBLE.md) converts that one line into which stage is at
   fault. If `bridge.sent` is advancing, the firmware half is done and the remaining question is
   `adb shell dumpsys vibrator_manager`.
3. Re-confirm Wii Remote motion on the console. It previously reached the deleted encoder, so its
   🟢 status is not safe to carry forward unexamined (see the Wii Remote section).


1. Run one bounded genuine-controller reconnect/power A/B from the completed controller-side atlas.
   The current corpus admits 46 zero-loss traces and 30 zero-loss BLE captures, but only two BLE
   files contain framed command traffic and both cover the same initialization path.
2. Capture a genuine Pro2 physical-tag write/readback before enabling native writes.
3. Finish the dedicated, research-build-only firmware-update sink so the next genuine controller
   update opportunity can be captured completely without using truncated generic traces.
4. Add DualSense microphone Opus decode and USB return only after headset classification is
   hardware-confirmed and the existing speaker/haptic baseline is protected.
5. Add a reproducible release checklist with board, firmware revision, controller firmware,
   console firmware, and result data.
6. Revisit controller sleep only after capturing a verified per-family sleep command or a stable
   distinction between automatic-reconnect and user-wake advertisements.
