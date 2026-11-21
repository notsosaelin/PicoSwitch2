---
name: picoswitch2-release
description: Prepare, check and stage a PicoSwitch2 release. Covers the version bump, the full validation set, tagging, the CI-built draft release, the artifacts CI cannot sign, and the documentation that must change. Use when asked to cut, prepare, tag or publish a release, bump the version, or check whether the tree is releasable. Never tags or publishes without explicit maintainer approval.
---

# PicoSwitch2 release

Firmware and companions share wire contracts, so they ship together under one
version. Nothing here tags, pushes or publishes on its own initiative: each of
those needs the maintainer to say so in the current conversation.

## 1. Is the tree releasable

- `git status --short` is clean and the branch is the one the maintainer named.
- `STATUS.md` lists no release blocker. Open validation gates are stated in the
  release notes, not hidden.
- Run everything in the `picoswitch2-validate` skill's command list. All of it,
  not the subset for the last change.

## 2. Version

`VERSION` at the repository root is the product version. Change it there, then
make the copies match:

| File | Field |
| --- | --- |
| `VERSION` | the version, one line |
| `companions/android/app/build.gradle.kts` | `versionName`, and `versionCode` = major*10000 + minor*100 + patch |
| `companions/windows/src/PicoSwitch.Companion.App/Package.appxmanifest` | `Identity Version="X.Y.Z.0"` |

`python tests\tools\test_version_consistency.py` fails until they agree. The
firmware reads `VERSION` at configure time; Linux takes its version from the tag.

The bridge contract (`ANDROID_BRIDGE_CONTRACT_VERSION` / `BridgeContract.VERSION`)
and the settings schema are NOT the product version. They move only when the
wire or the stored layout changes.

A persistence layout the previous release cannot read must ship the
clean-install behavior. See `docs/bluetooth/PERSISTENCE.md`.

## 3. Documents

- `CHANGELOG.md`: the release section, each entry with its validation level.
- `STATUS.md`: baseline, bridge contract, settings schema, last full run.
  Remove "unreleased" wording for what is now released.
- `PLAN.md`: remove what shipped.
- `docs/status/compatibility-matrix.md` if hardware validation changed.

## 4. Tag, then build

Tag before building: the firmware build identity is the git hash at configure
time, and a hash that names the release commit is the point.

Pushing a tag `vX.Y.Z` runs `.github/workflows/release.yml`. It checks the tag
against `VERSION`, runs the CI jobs, writes `SHA256SUMS` over every artifact,
attests provenance and creates a DRAFT release with:

- update and `-wipe` UF2 for Pico W and Pico 2 W
- Windows portable ZIP, x64 and arm64
- Linux single-file executables, x64 and arm64

Check on first use: that workflow was written on 2026-11-10 and had not run when
this skill was written. If it fails, the release can still be built by hand, as
v2.6.0 was.

## 5. What CI cannot produce

These need keys that are deliberately outside the repository and its secrets:

- **Signed Android APK.** `.\gradlew :app:assembleRelease` with the release
  keystore resolved from `app/keystore.properties` or the `PICOSWITCH_*`
  environment variables. Without the key the build is unsigned; do not publish
  that, and never substitute a debug APK. If the key is not on this machine, say
  so and ship without the APK rather than delay silently.
- **Windows installer.**
  `companions\windows\build.ps1 -ReleaseArtifacts -Configuration Release -Platform x64` (and `arm64`); needs Inno Setup.

Add them to the draft and extend `SHA256SUMS` to cover them.

## 6. Before publishing

- Each UF2 passes `tools\verify_install_reset_marker.py` with its flash size.
- The firmware `info` reply reports the new version and the tag's hash, not
  `+dirty`.
- A flashed adapter and each companion from the same release connect and report
  a matching bridge contract. Record which board, console firmware and
  controller were used.
- The release notes state what was validated on hardware and what was not.

Publishing the draft is the maintainer's action.
