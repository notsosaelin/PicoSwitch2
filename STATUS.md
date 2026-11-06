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

- **Two paired controllers no longer both drive the console** (`8a9e5bb`). The
  input arbiter was demoting its own owner when a raw-report hook registered the
  same peer as an unidentified source.
- **Report `0x05` IMU timestamp** now carries the motion sample's own time instead
  of the report's departure time (`dd73596`). This affects the PC/Steam path only;
  the console reads report `0x09`. A Steam gyro check with a DualSense closes it.
- **8BitDo Ultimate paddles via patched controller firmware removed** (`252fde0`).
  The controller works on stock firmware without its P1/P2 paddles.
- **Repository reorganised** into `firmware/`, `companions/`, `contracts/`,
  `tests/`, `tools/`, `web/` and `docs/`. No behavior change.

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
| Audio | Partial | DualSense speaker, headset jack and genuine Pro Controller 2 headphone output confirmed on Pico 2 W. DualSense microphone return not implemented. |
| Virtual Amiibo / NFC | Complete | 540/572-byte tags and the 2048-byte figure-v3 path confirmed. Native physical NFC writes still open. |
| Management (BLE + USB CDC) | Partial | Implementation complete; bonded, encrypted BLE management works in any personality. Part of the security and coexistence pass is unvalidated (below). |
| Android companion | Complete | Management, Controller Link, Touch Gamepad, Virtual Amiibo. |
| Windows companion | Partial | Management and Controller Link confirmed on hardware (no motion: Windows does not expose it). Touch Gamepad and installer shipped in v2.5.0. Virtual Amiibo transfer hardware validation pending. |

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
81/81; tool tests 28/28; Android JVM tests pass; Windows 1,231 tests pass with
descriptor parity; doc links OK. Software validation is not hardware validation.
