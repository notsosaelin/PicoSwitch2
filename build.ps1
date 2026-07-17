#!/usr/bin/env pwsh
# Build PicoSwitchWGA for one or both boards using the Raspberry Pi Pico VS Code
# extension's bundled SDK / toolchain (no global installs required).
#
#   ./build.ps1                # build both pico_w (RP2040) and pico2_w (RP2350)
#   ./build.ps1 pico_w         # build only pico_w
#   ./build.ps1 pico2_w        # build only pico2_w
#   ./build.ps1 -Clean         # wipe build dirs first, then build
#
# Experimental DualSense-audio variants (RP2350 / pico2_w only; see
# docs/switch2/dualsense-audio-bridge.md). Each uses its own build directory so
# it never clobbers the ordinary build:
#   ./build.ps1 -Tone          # build/pico2_w_tone : fixed diagnostic tone, no encoder
#   ./build.ps1 -Audio         # build/pico2_w_audio: live USB->DualSense Opus bridge
#
# Output: build/<dir>/PicoSwitchWGA-<board>.uf2
param(
    [string[]]$Boards = @('pico_w', 'pico2_w'),
    [switch]$Clean,
    [switch]$Tone,
    [switch]$Audio
)
# CMake writes its normal message() output to stderr; don't let that be treated
# as a terminating error. Gate on the explicit $LASTEXITCODE checks below.
$ErrorActionPreference = 'Continue'
$PSNativeCommandUseErrorActionPreference = $false

# Versions match those installed by the Pico VS Code extension (see CMakeLists.txt header).
$pico = "$env:USERPROFILE\.pico-sdk"
$sdkVersion = '2.2.0'
$toolchainVersion = '14_2_Rel1'
$cmakeVersion = 'v3.31.5'
$ninjaVersion = 'v1.12.1'
$picotoolVersion = '2.2.0-a4'

$cmake = "$pico\cmake\$cmakeVersion\bin\cmake.exe"
$ninja = "$pico\ninja\$ninjaVersion\ninja.exe"
$env:PICO_SDK_PATH = "$pico\sdk\$sdkVersion"
$env:PICO_TOOLCHAIN_PATH = "$pico\toolchain\$toolchainVersion"
$env:Path = "$pico\toolchain\$toolchainVersion\bin;$pico\cmake\$cmakeVersion\bin;$pico\ninja\$ninjaVersion;$pico\picotool\$picotoolVersion\picotool;" + $env:Path

$root = $PSScriptRoot

# The DualSense-audio variants are RP2350-only and use their own build dir and a
# feature-specific cmake flag. They are mutually exclusive.
if ($Tone -and $Audio) { throw "-Tone and -Audio are mutually exclusive" }
$extraArgs = @()
$dirSuffix = ''
if ($Tone)  { $Boards = @('pico2_w'); $extraArgs = @('-DNS2_DS5_AUDIO_TEST_TONE=ON'); $dirSuffix = '_tone' }
if ($Audio) { $Boards = @('pico2_w'); $extraArgs = @('-DNS2_DS5_AUDIO=ON');           $dirSuffix = '_audio' }

foreach ($b in $Boards) {
    $bdir = "$root\build\$b$dirSuffix"
    if ($Clean -and (Test-Path $bdir)) { Remove-Item -Recurse -Force $bdir }
    Write-Host "=== Configuring $b$dirSuffix ===" -ForegroundColor Cyan
    & $cmake -S $root -B $bdir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DPICO_BOARD=$b" @extraArgs 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "configure failed for $b$dirSuffix" }
    Write-Host "=== Building $b$dirSuffix ===" -ForegroundColor Cyan
    & $cmake --build $bdir 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "build failed for $b$dirSuffix" }
    $uf2 = "$bdir\PicoSwitchWGA-$b.uf2"
    if (Test-Path $uf2) { Write-Host "OK -> $uf2" -ForegroundColor Green } else { throw "missing uf2 for $b$dirSuffix" }
}
