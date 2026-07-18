#!/usr/bin/env pwsh
# Build PicoSwitchWGA for one or both boards using the Raspberry Pi Pico VS Code
# extension's bundled SDK / toolchain (no global installs required).
#
#   ./build.ps1                # build both pico_w (RP2040) and pico2_w (RP2350)
#   ./build.ps1 pico_w         # build only pico_w
#   ./build.ps1 pico2_w        # build only pico2_w
#   ./build.ps1 -Clean         # wipe build dirs first, then build
#
# Experimental RP2350-only DualSense-audio variants use dedicated build dirs:
#   ./build.ps1 -Tone          # fixed diagnostic tone, no Opus encoder
#   ./build.ps1 -Audio          # 150 MHz live-audio diagnostic control
#   ./build.ps1 -AudioOverclock # 200 MHz non-working comparison
#   ./build.ps1 -AudioOverclock300 # validated 300 MHz live-audio configuration
#
# Output: build/<dir>/PicoSwitchWGA-<board>.uf2
param(
    [string[]]$Boards = @('pico_w', 'pico2_w'),
    [switch]$Clean,
    [switch]$Tone,
    [switch]$Audio,
    [switch]$AudioOverclock,
    [switch]$AudioOverclock300
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

if ($Tone -and ($Audio -or $AudioOverclock -or $AudioOverclock300)) {
    throw "-Tone cannot be combined with an audio build"
}
if (($Audio -and ($AudioOverclock -or $AudioOverclock300)) -or
    ($AudioOverclock -and $AudioOverclock300)) {
    throw "-Audio, -AudioOverclock, and -AudioOverclock300 are mutually exclusive"
}
$extraArgs = @()
$dirSuffix = ''
if ($Tone) {
    $Boards = @('pico2_w')
    $extraArgs = @('-DNS2_DS5_AUDIO_TEST_TONE=ON')
    $dirSuffix = '_tone'
}
if ($Audio) {
    $Boards = @('pico2_w')
    $extraArgs = @('-DNS2_DS5_AUDIO=ON')
    $dirSuffix = '_audio'
}
if ($AudioOverclock) {
    $Boards = @('pico2_w')
    $extraArgs = @('-DNS2_DS5_AUDIO=ON', '-DNS2_DS5_AUDIO_OVERCLOCK=ON')
    $dirSuffix = '_audio_200mhz'
}
if ($AudioOverclock300) {
    $Boards = @('pico2_w')
    $extraArgs = @('-DNS2_DS5_AUDIO=ON',
                   '-DNS2_DS5_AUDIO_OVERCLOCK=ON',
                   '-DNS2_DS5_AUDIO_OVERCLOCK_MHZ=300')
    $dirSuffix = '_audio_300mhz'
}

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
