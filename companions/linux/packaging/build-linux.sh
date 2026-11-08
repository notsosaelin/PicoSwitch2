#!/usr/bin/env bash
# Build PicoSwitch2 Companion for Linux as ONE self-contained executable per
# architecture.
#
#   companions/linux/packaging/build-linux.sh [--arch x64|arm64|all] [--skip-tests]
#
# Output, in companions/linux/artifacts/:
#   PicoSwitch2Companion-<version>-linux-x64      (+ .sha256)
#   PicoSwitch2Companion-<version>-linux-arm64    (+ .sha256)
#
# Requirements: a .NET 9 SDK on PATH and sha256sum. Runs on any x86_64 or
# arm64 Linux (CI, WSL, a handheld in desktop mode); arm64 is cross-published,
# so one x86_64 machine builds both. Nothing is installed system-wide.
#
# WHY A PLAIN EXECUTABLE, NOT AN APPIMAGE. Steam's Game Mode client runs with
# NoNewPrivs=1, which every game it launches inherits, and that stops the
# setuid fusermount the AppImage runtime mounts itself with ("Cannot mount
# AppImage, please check your FUSE setup"; observed on Bazzite 2026-11-06,
# reproduced with `setpriv --no-new-privs`). The only AppImage-side cure was a
# per-shortcut launch option, which the project does not ask users to set. A
# single-file .NET executable mounts nothing: Steam runs it like any native
# game. On first launch it unpacks its native libraries (~16 MB) once into
# ~/.net/PicoSwitch2Companion/<hash>/ and reuses them on every later launch.
#
# Reproducibility:
#   - self-contained, so it needs no system .NET and no ICU
#     (InvariantGlobalization is set in the project);
#   - the version comes from `git describe`, with -dirty on a modified tree, so
#     every artifact is attributable to source state;
#   - every native dependency (SDL3, Skia, HarfBuzz) ships its own linux-arm64
#     build in its NuGet package, so arm64 is the same publish with another RID.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
linux="$(cd "$here/.." && pwd)"
repo="$(cd "$linux/../.." && pwd)"
artifacts="$linux/artifacts"

arches="x64 arm64"
skip_tests=0
while [ $# -gt 0 ]; do
  case "$1" in
    --skip-tests) skip_tests=1 ;;
    --arch)
      shift
      case "${1:-}" in
        x64|arm64) arches="$1" ;;
        all) arches="x64 arm64" ;;
        *) echo "error: --arch takes x64, arm64 or all" >&2; exit 2 ;;
      esac
      ;;
    *) echo "error: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done

version="$(git -C "$repo" describe --tags --always --dirty 2>/dev/null || echo 0.0.0-unknown)"
# MSBuild wants a numeric Version; the full describe goes in InformationalVersion.
numeric="$(echo "$version" | sed -nE 's/^v?([0-9]+\.[0-9]+\.[0-9]+).*/\1/p')"
numeric="${numeric:-0.0.0}"
echo "PicoSwitch2 Companion $version"

if [ "$skip_tests" = 0 ]; then
  echo "== tests"
  dotnet test "$linux/tests/PicoSwitch.Companion.Linux.Tests" -c Release -v q --nologo
fi

mkdir -p "$artifacts"
for arch in $arches; do
  echo "== publish linux-$arch"
  staging="$artifacts/publish-linux-$arch"
  rm -rf "$staging"
  dotnet publish "$linux/src/PicoSwitch.Companion.Linux.App" \
    -c Release -r "linux-$arch" --self-contained true \
    -p:PublishSingleFile=true \
    -p:IncludeNativeLibrariesForSelfExtract=true \
    -p:EnableCompressionInSingleFile=true \
    -p:Version="$numeric" -p:InformationalVersion="$version" \
    -p:DebugType=none -p:DebugSymbols=false \
    -o "$staging" -v q --nologo

  out="$artifacts/PicoSwitch2Companion-$version-linux-$arch"
  install -m 0755 "$staging/PicoSwitch2Companion" "$out"
  rm -rf "$staging"
  ( cd "$artifacts" && sha256sum "$(basename "$out")" > "$(basename "$out").sha256" )
  ls -la "$out"
  cat "$out.sha256"
done
echo "== done"
