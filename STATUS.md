# Status

Where PicoSwitch2 stands right now, and the handoff for whoever works on it
next. Where it is going is in [`PLAN.md`](PLAN.md); what each release changed is
in [`CHANGELOG.md`](CHANGELOG.md); what has physically been validated, per
controller and personality, is in the
[compatibility matrix](docs/status/compatibility-matrix.md).

This page is kept short on purpose. Replace what a change supersedes; do not
append a log. Earlier versions are archived under [`docs/archive/`](docs/archive/),
most recently
[`status-through-2026-11-10.archived.md`](docs/archive/status-through-2026-11-10.archived.md),
which holds the per-item validation record for v2.6.0.

_Last updated 2026-11-22._

## Resume here

**v2.6.1 is tagged and its CI draft release is being built** (2026-11-22). It
carries Android Controller Link on one Bluetooth link (Path C), discovery
stepping aside while a companion streams, the audit pass of 2026-11-10, the
web portal rework and the phone layouts. The Android APK and the Windows
installer are not in the draft: the signing key and Inno Setup are not on the
build machine.

1. **Finish the 2.6.1 draft.** Build the signed APK
   (`.\gradlew :app:assembleRelease` with the release keystore) and the Windows
   installers, add them, extend `SHA256SUMS`, then publish the draft
   (maintainer).
2. **Gate 19 against a Switch 2 console**, on the production adapter: so far
   only a PC running an emulator has been the host.
3. **Keyboard & Mouse Link is designed and waiting for a go-ahead.** Read
   [`docs/architecture/kbm-link-hld.md`](docs/architecture/kbm-link-hld.md).
   The release shortcut is settled (Ctrl+Alt+Shift+Q); one preference is open:
   whether system shortcuts are captured by default. Do not start it unasked.
4. Run validation gates 14 to 18 below on both boards.
5. Decide the open items at the top of [`PLAN.md`](PLAN.md): branch protection,
   the watchdog, `NS2_PRO=OFF`, UART diagnostics in release images.
6. Three Dependabot pull requests are open (actions, Android, .NET); CI fails
   on the Android and .NET ones and nobody has looked at why.

If work stops in the middle of a task, write the next step here.

## At a glance

| | |
| --- | --- |
| Released baseline | **v2.6.1** (2026-11-22, draft): firmware for both boards (update and clean-install UF2), Windows portable ZIP and Linux executables (x64, arm64) from CI; signed APK and Windows installer still to add. Previous: v2.6.0 (2026-11-08) |
| Development branch | `main` |
| Release blockers | None known |
| Product version | `VERSION` (2.6.1); the firmware reports it in `info` |
| Bridge contract | 4 (`ANDROID_BRIDGE_CONTRACT_VERSION` / `BridgeContract.VERSION`); firmware and companions must come from the same release |
| Settings schema | 11 |
| Toolchain | Pico SDK 2.3.0, BTstack 1.8.2, Arm GCC 15.2.Rel1 |
| Boards | Pico 2 W: production, 300 MHz, live DualSense audio. Pico W: validated non-audio profile. Keep them different. |

## Released in v2.6.1, hardware validation incomplete

An audit and hardening pass, 2026-11-10. All of it is built and software-tested
only.

- **Management pairing requires LE Secure Connections.** A new management bond
  offered with legacy pairing is declined (`mgmt_accept_new_bond_pairing`).
  Controllers keep the existing request-not-require policy.
  [`docs/bluetooth/SECURITY.md`](docs/bluetooth/SECURITY.md).
- **Bench-only commands are off the BLE allowlist**: `amiibo reader ...` and
  `caudio lock ...` remain on USB CDC and UART.
- **Short vendor commands read zeros, not stale bytes**, on all three
  personalities. An over-long USB CDC line is refused instead of run truncated.
- **Web portal**: device text is escaped before it reaches `innerHTML`, the page
  declares a Content-Security-Policy, catalog images load over https only, and
  an imported ZIP is size-bounded. Companions fetch artwork over https only.
- **Firmware reports its real version.** `info` returned `2.5` in the 2.6.0
  release; it now comes from the root `VERSION` file.
- **`printf` is compiled out of project firmware sources** (the firmware has no
  stdio back end, so the calls formatted text and discarded it): about 45 KB
  less flash on each board.
- **`NS2_PRO=OFF` builds again** and CI compiles it.
- **CI**: actions pinned to commit SHAs, downloads checksum-verified, the
  management parity check added, a tag-triggered draft-release workflow
  (never yet run), Dependabot.
- **Agent documentation restructured**: `AGENTS.md` is the short entry point,
  `CLAUDE.md` imports it, the engineering principles and per-subsystem
  validation rules moved under `docs/`, four project skills added.

And on 2026-11-11, also software-tested only:

- **Web portal rebuilt on the companion shell**: Play (Controller Link from a
  browser gamepad, console buttons, Wake, input sources, the amiibo), Keyboard
  & Mouse (drawn keyboard, bindings, mouse tuning, profiles), Customize,
  Adapter (emulated controller, pairing, paired devices), Settings. What a
  browser cannot do is listed in [`web/README.md`](web/README.md).
  `tools/portal_preview.py` renders every page against the repository's
  fixtures.
- **Pico W has about 107 KB of heap, up from 22 KB.** Its diagnostic rings are
  built at the `standard` size; the full `lab` sizes are one build flag away
  ([profile](docs/bluetooth/DIAGNOSTICS.md#diagnostic-buffer-profile)). Pico 2 W
  is unchanged.
- **Genuine amiibo dumps left the repository.** `tests/fixtures/amiibo/` and the
  archive built from it are git-ignored; the tests that read them skip and say
  so ([`tests/README.md`](tests/README.md)). They remain in earlier history.
- **Keyboard & Mouse Link designed**, not implemented
  ([design](docs/architecture/kbm-link-hld.md)).

And on 2026-11-22, the phone layout pass, looked at on a Galaxy S25 Ultra
(384 × 832 dp) in both orientations with no adapter:

- **Web portal**: in landscape the side navigation was cut off and Adapter and
  Settings could not be reached; now it fits. The header is one row on a
  phone. `tools/portal_preview.py --shots` renders nine phone sizes. Checked in
  Firefox (Chrome is disabled on that phone), so only layout, not Bluetooth.
- **Android companion**: in portrait the bottom navigation ran off the screen
  and Adapter could not be opened from it; it now stacks names under icons in
  equal cells below 560 dp. In landscape the closed rail no longer shows in
  the camera cutout strip and the console tiles' labels are no longer clipped.
  Play's actions and controller row adapt to a narrow window
  ([layout spec](docs/architecture/companion-shell-layout.md)).
- **Android Controller Link on the management link (Path C).** The separate
  Classic HID connection failed on a Galaxy S25 Ultra in two ways: its page
  was missed while the adapter ran controller inquiry (the slow first
  connect), and while it streamed the LE management link starved and timed
  out, taking Controller Link with it. Reproduced on pre-2.6.0 firmware, so
  not a firmware regression. Android now uses the same data plane as Windows
  and Linux, and the Classic HID transport is gone from the app. No
  firmware change. The Touch Gamepad menu gained a Headset switch
  ([record](docs/experiments/android-two-link-coexistence-2026-11-22.md)).

## Released in v2.6.0, hardware validation incomplete

The changelog describes each item. These parts have not been seen on hardware:

- Update UF2 keep-everything first boot (markers verify; never flashed and observed).
- Windows and Linux controller navigation with a real pad.
- Linux Game Mode full-screen sizing confirmed by measurement, not by eye.
- Console cues on Play (`console` object in the input sources reply) against a console.
- Companion audio after its fourth pass: Controller Link scoping, drift-controlled
  playout, the microphone uplink, headset re-plug on Controller Link return, and
  the Classic DualSense Edge degradation fixes
  ([record](docs/experiments/companion-audio-downlink-prototype-2026-11-07.md)).
- The Linux companion against an adapter beyond pairing, connecting and Play on
  the Ally ([open questions](companions/linux/README.md#6-open-questions-for-the-first-hardware-pass)).

## Subsystems

Status markers follow the [engineering principles](docs/engineering-principles.md#status-markers). "Confirmed" means
on real hardware; anything else says what it is.

| Subsystem | Status | Notes |
| --- | --- | --- |
| Pro Controller 2 personality | Complete | Buttons, sticks, C/GL/GR, rumble, wake from sleep. 1 ms USB interval by design (genuine is 4 ms). |
| NSO GameCube personality | Complete | Analog triggers, detents, rumble. GameCube `Z` is the host-facing ZR control. |
| Joy-Con 2 L / R personalities | Partial | Experimental; separate sideways controllers, not a combined pair. Native mouse mode works. |
| Bluetooth input sources | Complete | Sony, Xbox, Nintendo (Switch 1 and 2), Wii, 8BitDo, Retro Fighters families confirmed. One console owner at a time. |
| Keyboard / keyboard + mouse | Complete | Confirmed with a keyboard and mouse pair: per-role reconnect, translated mouse-to-stick in game ([record](docs/bluetooth/keyboard-mouse-input.md#hardware-validation)). |
| Motion | Complete | Genuine Pro Controller 2 passthrough and DualSense translation on the length-`0x1E` carrier are confirmed. Synthesized length-`0x28` is rejected on hardware and deferred. |
| Rumble and haptics | Complete | Confirmed across driver families, including native DualSense haptics on Pico 2 W. |
| Audio | Partial | DualSense speaker, headset jack and genuine Pro Controller 2 headphone output confirmed on Pico 2 W. DualSense microphone return not implemented. Companion audio downlink plays on hardware: Windows, and Android (Odin 2 Mini, with a Windows PC as the USB host, eighth pass of the [record](docs/experiments/companion-audio-downlink-prototype-2026-11-07.md)). Android against a Switch 2, the companion microphone uplink on either platform, and the descriptor-time Controller Link admission fix are implemented, hardware validation pending. |
| Virtual Amiibo / NFC | Complete | 540/572-byte tags and the 2048-byte figure-v3 path confirmed. Native physical NFC writes still open. |
| Management (BLE + USB CDC) | Partial | Implementation complete; bonded, encrypted BLE management works in any personality. Part of the security and coexistence pass is unvalidated (below). |
| Android companion | Complete | Management, Controller Link, Touch Gamepad, Virtual Amiibo. Controller Link on the management link (Path C) since 2.6.1; played against a PC emulator host on an S25 Ultra and a Thor, not yet against a Switch 2 (gate 19). |
| Windows companion | Partial | Management and Controller Link confirmed on hardware (no motion: Windows does not expose it). Touch Gamepad and installer shipped in v2.5.0. Virtual Amiibo transfer hardware validation pending. |
| Web portal | Partial | Rebuilt on the companion shell 2026-11-11; implementation complete for the pages listed in [`web/README.md`](web/README.md), software-validated and rendered against fixtures only (also on a phone, 2026-11-22, layout only). The amiibo manager and colours are the previously shipped code, unchanged. |
| Linux companion | Partial | Implementation complete for v1 (pairing, management, Controller Link with motion); hardware validation pending. Keyboard & Mouse and Amiibo pages, the Touch Gamepad and companion audio (headset and microphone) ported (software only; hardware validation pending). Open questions: [`companions/linux/README.md`](companions/linux/README.md#6-open-questions-for-the-first-hardware-pass). |

## Open validation gates

Each needs hardware in front of the maintainer; none is release-blocking.

1. **Controller Link reliability candidate.** The Android encryption stand-down
   (Type C) is source-established, not confirmed. Procedure:
   [controller-link-cycling-failure-2026-08-22](docs/experiments/controller-link-cycling-failure-2026-08-22.md).
2. **Windows Virtual Amiibo transfer** (implemented 2026-08-31; Android's is confirmed).
3. **Management security.** Bonded reconnect after teardown, plaintext write
   rejection, management restored after reboot, wake-burst advertiser handoff.
4. **Management coexistence.** Gyro under load, wake from sleep during a session,
   latency measurement.
5. **`reenumerate`.** The console picking up refreshed controller colors.
6. **Companion lifecycle.** Returning-session reconnect, teardown, and the same pass
   on a release APK.
7. **Touch Gamepad breadth.** Every control against each real console personality;
   held-contact profile changes with the Classic link retained.
8. **Web portal Amiibo Sync** cache refresh and retained dirty v3 generations.
9. **Wii Remote motion** re-check through the current encoder.
10. **Native physical NFC writes**, including Joy-Con 2 Right's reader.
11. **Bluetooth wipe/flash matrix** remainder: [VALIDATION.md](docs/bluetooth/VALIDATION.md).
12. **Released in v2.6.0, never observed on hardware**: two-controller arbitration,
    report `0x05` timestamp, and the keep-everything first boot of an update UF2.
13. **Companion audio and microphone** (Windows, Android): Controller Link
    scope, speaker continuity, microphone intelligibility, mid-session headset ↔
    headphones re-plug, and no Controller Link regression
    ([procedure A and B](docs/experiments/companion-audio-downlink-prototype-2026-11-07.md#hardware-validation-procedure-next)).
14. **Management pairing requires Secure Connections** (changed 2026-11-10, in
    2.6.1): pair each companion (Android, Windows, Linux) fresh and confirm the
    bond forms; `btstate` reports `cble.legacy_declined` if one was refused for
    offering legacy pairing. Existing bonds are unaffected.
15. **Firmware built with `printf` compiled out** (2026-11-10, in 2.6.1): a
    normal session on each board, to confirm that removing the discarded
    logging changed no timing-sensitive behavior.
16. **Web portal against an adapter** (2026-11-11, in 2.6.1), in Chrome or
    Edge over Bluetooth: each page loads; a binding change reaches the console;
    Pair and Forget work; Controller Link from a browser gamepad plays a game
    with the face buttons where expected, rumble arrives, and hiding the tab
    releases every input. Read `clink status` during play for the applied rate
    and `max_gap_ms`: the connection interval a browser gets is unknown.
17. **Pico W with short diagnostic rings** (2026-11-11, in 2.6.1): an
    ordinary session on a Pico W, then `trace status` and `btlife status` to
    confirm the reported capacities (64 and 256) and that nothing else moved.
18. **Phone layouts against an adapter** (2026-11-22, in 2.6.1): the
    Android debug build and the web portal in Chrome on a phone, connected, in
    both orientations: every page reachable, Play while streaming, the
    Keyboard page with an adapter's real mapping.
19. **Android on Path C** (2026-11-22, in 2.6.1). Done with a PC running a
    Switch 2 emulator as the USB host, Pico 2 W dev adapter: S25 Ultra about
    14 minutes (92,719 frames, all applied, 0 neutralizations, no management
    drop, headset live); AYN Thor on the 2.6.1 build (all frames applied,
    `scan_active`/`inquiry_active` false while streaming, headset 0 skipped at
    the adapter and 0 dropped at the phone, link on LE 2M). Still to do:
    against a Switch 2 console on the production adapter, motion and rumble,
    30 minutes or more, reconnect after leaving range.

## Known technical debt

- **`NS2_PRO=OFF` builds again** (2026-11-10) and CI compiles it. It has not run on
  hardware since the Switch 2 work began; whether to keep it is open in PLAN.md.
- **Pico W diagnostic history is short by default.** Its three diagnostic rings
  are built at the `standard` size (about 107 KB of heap, up from 22 KB). A soak
  or a full capture on Pico W needs `build.ps1 pico_w -DiagProfile lab`
  ([profile](docs/bluetooth/DIAGNOSTICS.md#diagnostic-buffer-profile)).
- **No watchdog is armed.** A hang on either core needs a power cycle. Tracked in
  PLAN.md.
- **`btstack_host.c` is 14,700 lines** and holds project code (management,
  Controller Link, companion audio, pairing) inside the vendored host.
- **`main` is not a protected branch**, and `web/` deploys from it to GitHub Pages.
- **`ns2_ds5_motion.*` is misnamed**; it is the shared translated-motion encoder.
  The rename is a separate mechanical task.
- **Feedback is not fully source-aware.** A few legacy vendor init paths fall back
  to slot 0, so rumble/LED isolation between sources is not complete.
- **Classic link keys are not listed** by `btbonds` or the management bond list,
  which walk the LE database only.
- **The RSSI liveness probe cannot detect a dead remote** (the local controller
  answers it). It has never misfired; do not remove it without evidence.
- **BTstack 1.8.2 CTKD hazard.** BR/EDR→LE key derivation can write the existing
  Classic key back byte-reversed; upstream fixes `f25861592` and `a0f82a97c` are
  not in the SDK's pin. The Android companion is the exposed peer:
  [migration record](docs/experiments/pico-sdk-2.3-btstack-1.8.2-migration-2026-08-25.md).
- **Unexplained:** background `input sources` polls took ~8 s on 2026-08-29 while
  foreground commands on the same link took ~150 ms. The origin appears to be on
  the Android side. Next step: timestamp the management exchange end to end.

## Settled: do not rediscover

The full list, with evidence, is in
[refuted-hypotheses.md](docs/experiments/refuted-hypotheses.md) and the archived
status. The ones most often re-proposed:

- The Controller Link "cycling failure" is the tablet's Bluetooth stack, not the adapter.
- The per-axis "angular phase" motion encoder was refuted and deleted; those bytes are one packed quaternion.
- Synthesized translated `0x28` motion was rejected on hardware even with a coherent recipe.
- `NS2_MOTION30_ACCEL_Q16_PER_COUNT = 68963` is correct; the wire scale is ~4310 counts/g, not 4096.
- A determinant of +1 is not enough for a motion seam row; rows are pinned by a gravity-anchor test.
- The Switch 2 validates amiibo cryptography; key-free generated images are rejected.
- The quirk table, not a descriptor heuristic, decides whether a peer is a supported controller.
- The v2 bridge regression was firmware/APK descriptor skew, not the bridge-core split.

## Automated validation

All suites and their commands are listed in [`tests/README.md`](tests/README.md).
Last full run, 2026-11-11, on the uncommitted tree: Pico W (standard and lab
profiles), Pico 2 W and the `NS2_PRO=OFF` configuration build; host suite 84/84
declared targets rebuilt and passed, 5 excluded by declaration; tool tests
34/34 (one of them, the v3 compatibility view, reports a skip as a pass: it has
no dumps to read); management command parity OK; doc links OK; Windows 1,371
passed and 14 skipped of 1,385, with descriptor parity; Linux backend 130
passed; Android JVM 1,299 run, 0 failed, 10 skipped. The skips are the amiibo
checks that need genuine dumps ([`tests/README.md`](tests/README.md)), so the
cross-implementation amiibo crypto check did not run. The web portal was
rendered against fixtures and looked at; it has not met an adapter. The shared
portable suites were not re-run on Linux, the WinUI shell was not compiled, and
the Android APK was not assembled.
On 2026-11-22, after the phone layout pass: tool tests 34/34; Android JVM
1,299 run, 0 failed, 10 skipped; the Android debug APK assembled and was looked
at on a phone. Firmware, host, Windows and Linux suites were not re-run (none
of their files changed).
Software validation is not hardware validation.
