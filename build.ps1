#!/usr/bin/env pwsh
# Build PicoSwitchWGA for one or both boards using the Raspberry Pi Pico VS Code
# extension's bundled SDK / toolchain (no global installs required).
#
#   ./build.ps1                # build both pico_w (RP2040) and pico2_w (RP2350)
#   ./build.ps1 pico_w         # build only pico_w
#   ./build.ps1 pico2_w        # build only pico2_w
#   ./build.ps1 -Clean         # wipe build dirs first, then build
#
# Standard Pico 2 W builds include live DualSense audio at 300 MHz/1.20 V.
# Pico W retains its validated non-audio configuration. Lower-clock Pico 2
# diagnostics use dedicated dirs:
#   ./build.ps1 -Tone          # fixed diagnostic tone, no Opus encoder
#   ./build.ps1 -Audio          # 150 MHz live-audio comparison
#   ./build.ps1 -AudioOverclock # 200 MHz non-working comparison
#   ./build.ps1 -AudioOverclock300 # compatibility alias for pico2_w
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
    $extraArgs = @('-DNS2_DS5_AUDIO=OFF',
                   '-DNS2_DS5_AUDIO_TEST_TONE=ON',
                   '-DNS2_PICO2_SYSTEM_CLOCK_MHZ=150')
    $dirSuffix = '_tone'
}
if ($Audio) {
    $Boards = @('pico2_w')
    $extraArgs = @('-DNS2_DS5_AUDIO=ON',
                   '-DNS2_DS5_AUDIO_TEST_TONE=OFF',
                   '-DNS2_PICO2_SYSTEM_CLOCK_MHZ=150')
    $dirSuffix = '_audio'
}
if ($AudioOverclock) {
    $Boards = @('pico2_w')
    $extraArgs = @('-DNS2_DS5_AUDIO=ON',
                   '-DNS2_DS5_AUDIO_TEST_TONE=OFF',
                   '-DNS2_PICO2_SYSTEM_CLOCK_MHZ=200')
    $dirSuffix = '_audio_200mhz'
}
if ($AudioOverclock300) {
    $Boards = @('pico2_w')
    Write-Warning "-AudioOverclock300 is now the standard pico2_w build"
}

foreach ($b in $Boards) {
    $boardArgs = @($extraArgs)
    if (-not ($Tone -or $Audio -or $AudioOverclock)) {
        if ($b -eq 'pico2_w') {
            # Pass these explicitly so an existing CMake cache cannot preserve
            # the former non-audio Pico 2 W defaults.
            $boardArgs += @('-DNS2_DS5_AUDIO=ON',
                            '-DNS2_DS5_AUDIO_TEST_TONE=OFF',
                            '-DNS2_PICO2_SYSTEM_CLOCK_MHZ=300')
        } elseif ($b -eq 'pico_w') {
            # Pass OFF explicitly so a prior experimental Pico W audio cache
            # cannot leak into the standard artifact.
            $boardArgs += @('-DNS2_DS5_AUDIO=OFF',
                            '-DNS2_DS5_AUDIO_TEST_TONE=OFF')
        }
    }
    $bdir = "$root\build\$b$dirSuffix"
    if ($Clean -and (Test-Path $bdir)) { Remove-Item -Recurse -Force $bdir }
    Write-Host "=== Configuring $b$dirSuffix ===" -ForegroundColor Cyan
    & $cmake -S $root -B $bdir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DPICO_BOARD=$b" @boardArgs 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "configure failed for $b$dirSuffix" }
    Write-Host "=== Building $b$dirSuffix ===" -ForegroundColor Cyan
    & $cmake --build $bdir 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "build failed for $b$dirSuffix" }
    $uf2 = "$bdir\PicoSwitchWGA-$b.uf2"
    if (Test-Path $uf2) { Write-Host "OK -> $uf2" -ForegroundColor Green } else { throw "missing uf2 for $b$dirSuffix" }
}
