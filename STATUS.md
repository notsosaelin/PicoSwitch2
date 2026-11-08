# Status

Where PicoSwitch2 stands right now. Where it is going is in [`PLAN.md`](PLAN.md);
what each release changed is in [`CHANGELOG.md`](CHANGELOG.md); what has physically
been validated, per controller and personality, is in the
[compatibility matrix](docs/status/compatibility-matrix.md).

This page is kept short on purpose. The detailed per-subsystem record and the dated
entries it replaced are archived in
[`docs/archive/status-through-2026-11-06.archived.md`](docs/archive/status-through-2026-11-06.archived.md).

_Last updated 2026-11-06._

## At a glance

| | |
| --- | --- |
| Released baseline | **v2.5.0** (2026-09-04): firmware for both boards, Android APK, Windows installer and portable ZIP |
| Development branch | `main` |
| Release blockers | None known |
| Bridge contract | 4 (`ANDROID_BRIDGE_CONTRACT_VERSION` / `BridgeContract.VERSION`); firmware and companions must come from the same release |
| Settings schema | 11 |
| Toolchain | Pico SDK 2.3.0, BTstack 1.8.2, Arm GCC 15.2.Rel1 |
| Boards | Pico 2 W: production, 300 MHz, live DualSense audio. Pico W: validated non-audio profile. Keep them different. |

## Unreleased on `main`

Software-verified, not yet hardware-validated, not yet in a release:

- **Firmware updates keep pairings, settings and amiibo** (2026-11-08). The
  standard UF2 no longer erases persistence on its first boot; a separate
  clean-install `-wipe.uf2` (`build.ps1 -Wipe`, `NS2_INSTALL_RESET`) does. Both
  build for Pico 2 W and their markers verify; the keep-everything first boot
  has not yet been flashed and observed. See
  [`docs/bluetooth/PERSISTENCE.md`](docs/bluetooth/PERSISTENCE.md#install-marker-update-uf2-and-clean-install-uf2).
- **Android navigation and Play home screen** (2026-11-08): navigation placeable
  at any edge (Settings → Navigation), docked under touch and gone while
  playing or under a controller, called up by the D-pad or Back; no hide or
  pin controls; Back never leaves the app from a page. Reviewed on an AYN Odin
  2 Mini (Left and Bottom). Later the same day:
  the navigation is always a slide-out (never docked); a Back tap shows / hides
  it and holding Back for 0.8 s releases Controller Link; the Amiibo page's
  controller focus is the highlight. Seen on the Odin with injected key events.
  While streaming, the handheld's controller now drives the app whenever the
  navigation, another page or an overlay is in front (the console gets it only
  on the bare Play screen, and is neutralized at the hand-off); previously the
  amiibo library could not be driven during Controller Link. Hand-off seen in
  the log with real-node `sendevent` input; owner then drove the carousel.
- **Linux and Windows navigation and Play home screen** (2026-11-08): the same
  slide-out navigation, Play home layout, pad ownership, Back tap / hold
  (Escape on the desktops) and Amiibo focus as Android, with the controller
  read from SDL / Windows.Gaming.Input frames. Linux: keyboard and controller
  paths driven in the screenshot harness. Windows: Play, the navigation and the
  Amiibo Enter / Escape path looked at on screen with the keyboard; the
  controller path is not yet run with a real pad on either desktop.
- **Linux fills the screen in Game Mode** (2026-11-08): gamescope ignored the
  full-screen request and letterboxed the default-size window; the window now
  sizes itself to the screen under gamescope. Measured as a non-Steam game on a
  ROG Xbox Ally X (1920×1080 window on the 1920×1080 screen); not yet confirmed
  by eye.
- **Version 2.6.0** in the Android and Windows manifests (no tag; Linux takes
  its version from the tag at packaging).
- **The adapter's amiibo shows on connect** (2026-11-08): the app read
  `amiibo status` only on the Amiibo page or a manual refresh, which the old
  wipe-on-every-flash hid; after an update UF2 Play said "No Amiibo" while the
  adapter (UART) held the figure. Now read once per verified connection;
  confirmed on the Odin.
- **Console cues on Play** (2026-11-08): the input sources reply carries an
  optional `console` object (`player` from the player LEDs, `amiibo_wanted`
  while the console's NFC scan runs); Android Play shows a Player N pill and a
  "game wants an amiibo" tile. Contract fixtures and both conformance suites
  cover it; not yet seen against a console.

- **Two paired controllers no longer both drive the console** (`8a9e5bb`). The
  input arbiter was demoting its own owner when a raw-report hook registered the
  same peer as an unidentified source.
- **Report `0x05` IMU timestamp** now carries the motion sample's own time instead
  of the report's departure time (`dd73596`). This affects the PC/Steam path only;
  the console reads report `0x09`. A Steam gyro check with a DualSense closes it.
- **8BitDo Ultimate paddles via patched controller firmware removed** (`252fde0`).
  The controller works on stock firmware without its P1/P2 paddles.
- **Companion audio prototype** (experimental), Windows, Linux and Android. Console
  audio is Opus-encoded by the Pro Controller 2 encoder and sent as a third GATT
  plane on the management link, then decoded and played by the companion
  (Play → **Headset on/off**). The adapter advertises a headphone jack so the
  console routes its audio there. Pico 2 W only; Pico W refuses with
  `unsupported`.
  - **Confirmed on hardware 2026-11-07:**
    - plays cleanly at 96 kbit/s with Controller Link (50 frames/s, 0 busy/failed);
    - DualSense rumble stays normal;
    - closing the app returns audio to the TV at once.
  - **Degraded with a Bluetooth Classic DualSense Edge as the controller:**
    chop, level dips, skips and late inputs. The adapter dropped source PCM, and
    the transport did not back up.
  - **Implemented in response; hardware validation pending:**
    - Headset off now clears the advertised jack immediately. It is applied
      per report at the output boundary; before, it waited for the next source
      input event, so audio stayed off the TV;
    - exclusive (CYW43-locked) companion encode while a DualSense is connected,
      the DualSense speaker path's production policy (`caudio lock on|off`);
    - deeper companion PCM queue and per-session `caudio diag`;
    - an adaptive jitter buffer;
    - a headset on the controller now always takes the audio.
  - **Bitrate stays at 96 kbit/s**, the console's own Pro Controller 2 headset
    rate. This is a requirement: lower rates are refused.
  - **Fourth pass, implemented and software-tested only (no hardware run):**
    - the headset belongs to Controller Link: it is refused without it and ends
      with it, by maintainer decision;
    - drift-controlled playout: one hour simulated with 0 underruns on BLE;
    - Android playback;
    - the **companion microphone uplink** (Mic toggle, `caudio start mic`):
      the adapter shows a headset, decodes 64 kbit/s mono Opus (above the
      DualSense microphone's 56.8 kbit/s) and serves it as the console's USB
      microphone. The cross-codec host test reproduces the signal at −0.00 dB.
    - The speaker encoder state is static (31.9 KB) since the adapter hung on
      the first companion block, a probable out-of-memory panic on core 1
      (experiment record, seventh pass). 16.4 KB of heap remains for
      newlib and the USB-host parser. Retested on hardware: no hang
      (owner, 2026-11-07).
  - **2026-11-07, Android on an Odin 2 Mini (experiment record, eighth and
    ninth passes):**
    - Android playback confirmed audible (the AudioTrack start threshold was the
      silence);
    - the headset is one accessory chosen at plug time; the microphone is
      **muted, never re-plugged**: confirmed no `caudio` command and no
      playback gap across six toggles;
    - Odin Bluetooth off/on recovers Controller Link by itself: confirmed 3/3;
    - firmware notes the Android companion when its bridge descriptor binds,
      so the headset no longer waits for a button press (built; hardware
      validation pending the flash);
    - the headset re-plugs by itself when Controller Link returns (software
      only).

  See the [record and procedure](docs/experiments/companion-audio-downlink-prototype-2026-11-07.md).
- **Repository reorganised** into `firmware/`, `companions/`, `contracts/`,
  `tests/`, `tools/`, `web/` and `docs/`. No behavior change.
- **Linux companion v1** (`companions/linux/`): Avalonia app shipped as one
  self-contained executable for x86_64 and arm64 (no AppImage: Steam Game Mode cannot
  mount one), handhelds in Game Mode first. Pairing and management over
  BlueZ, Controller Link from any SDL3 gamepad **with motion**. Runs on Linux
  (WSLg); never yet run against an adapter or on a Deck. UI redesigned around one
  measured layout grammar, with Keyboard & Mouse and Amiibo pages and a headless
  screenshot harness for reviewing every page across screen sizes.
- **One shell on all three companions**
  ([`companion-shell-layout.md`](docs/architecture/companion-shell-layout.md)):
  a slide-out navigation (originally a top bar and floating dock), Play as home, always full screen with no toggle
  (F11 on the desktops), one size scale, Customization and Adapter/Keyboard &
  Mouse tabs so pages fit without scrolling, and the same short-window layout
  (under 560 of height) on all three. Built on Windows, Linux and Android.
  Android's layout was checked in the debug layout lab on an AYN Odin 2 Mini
  (832×468 dp), where every page but Keyboard & Mouse → Profiles fits without
  scrolling; Linux's in the screenshot harness at 832×468 with the same
  result; Windows' was looked at on screen on 2026-11-08 (Play, navigation,
  Amiibo). No platform has run it against an adapter yet. Builds:
  Windows x64/arm64 portable, Linux x64/arm64 single-file, Android debug APK.
- **Shared C# layers extracted for two platforms.** `PicoSwitch.Companion.Platform`
  holds the platform contracts (`ICompanionPlatform`); Services now targets plain
  `net9.0`; `ControllerLinkService` carries motion when a backend supplies it.
  Windows behaviour is unchanged apart from host-neutral wording ("This computer").

## Subsystems

Status markers follow [CLAUDE.md](CLAUDE.md#status-markers). "Confirmed" means
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
| Audio | Partial | DualSense speaker, headset jack and genuine Pro Controller 2 headphone output confirmed on Pico 2 W. DualSense microphone return not implemented. Companion audio downlink plays on hardware (Windows); Controller Link scoping, drift-controlled playout, Android playback and the companion microphone uplink are implemented, hardware validation pending. |
| Virtual Amiibo / NFC | Complete | 540/572-byte tags and the 2048-byte figure-v3 path confirmed. Native physical NFC writes still open. |
| Management (BLE + USB CDC) | Partial | Implementation complete; bonded, encrypted BLE management works in any personality. Part of the security and coexistence pass is unvalidated (below). |
| Android companion | Complete | Management, Controller Link, Touch Gamepad, Virtual Amiibo. |
| Windows companion | Partial | Management and Controller Link confirmed on hardware (no motion: Windows does not expose it). Touch Gamepad and installer shipped in v2.5.0. Virtual Amiibo transfer hardware validation pending. |
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
12. **Unreleased fixes above**: two-controller arbitration, report `0x05` timestamp.
13. **Companion audio and microphone** (Windows, Android): Controller Link
    scope, speaker continuity, microphone intelligibility, mid-session headset ↔
    headphones re-plug, and no Controller Link regression
    ([procedure A and B](docs/experiments/companion-audio-downlink-prototype-2026-11-07.md#hardware-validation-procedure-next)).

## Known technical debt

- **`NS2_PRO=OFF` does not build.** `firmware/src/settings/config.c` references
  personality state outside its `NS2_PRO` guard. Tracked in PLAN.md.
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
Last full run, 2026-11-06, on the monorepo layout: both boards build; host suite
81/81; tool tests 28/28; Android JVM tests pass; Windows 1,246 tests pass with
descriptor parity; doc links OK. Linux: backend 79 tests, and the three shared
portable suites (1,119 tests) also pass on Linux (WSL Ubuntu 24.04).
Software validation is not hardware validation.
