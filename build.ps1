#!/usr/bin/env pwsh
# Build PicoSwitchWGA for one or both boards using the Raspberry Pi Pico VS Code
# extension's bundled SDK / toolchain (no global installs required).
#
#   ./build.ps1                # build both pico_w (RP2040) and pico2_w (RP2350)
#   ./build.ps1 pico_w         # build only pico_w
#   ./build.ps1 pico2_w        # build only pico2_w
#   ./build.ps1 -Clean         # wipe build dirs first, then build
#   ./build.ps1 pico2_w -NoUartDiag # omit the GP0/GP1 live diagnostic channel
#   ./build.ps1 pico2_w -MgmtOn     # compatibility alias; management now boots on
#   ./build.ps1 pico2_w -NS2FirmwareVersion 2.1.4 `
#       -NS2BluetoothVersion 12.0.0 -NS2DspVersion 0.2.3
#                              # select one coherent firmware identity tuple
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
    [string]$NS2FirmwareVersion = '2.1.4',
    [string]$NS2BluetoothVersion = '12.0.0',
    [string]$NS2DspVersion = '0.2.3',
    [switch]$NoUartDiag,
    [switch]$MgmtOn,
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
$sdkVersion = '2.3.0'
$toolchainVersion = '15_2_Rel1'
$cmakeVersion = 'v4.3.4'
$ninjaVersion = 'v1.13.2'
$picotoolVersion = '2.3.0'

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

# Always configure the complete identity tuple explicitly. This prevents a
# previous experimental CMake cache value from leaking into an ordinary build.
$extraArgs += @("-DNS2_PRO_FIRMWARE_VERSION=$NS2FirmwareVersion",
                "-DNS2_PRO_BLUETOOTH_VERSION=$NS2BluetoothVersion",
                "-DNS2_PRO_DSP_VERSION=$NS2DspVersion",
                "-DNS2_UART_DIAG=$(if ($NoUartDiag) { 'OFF' } else { 'ON' })",
                # Keep ordinary builds deterministic even if an older build
                # directory cached the former diagnostic default.
                "-DNS2_MGMT_DEFAULT_ON=ON")

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

    # The SDK version is declared twice: here, and as $sdkVersion in
    # CMakeLists.txt's Pico VS Code header. When pico-vscode.cmake exists it
    # sets PICO_SDK_PATH as a CMake variable BEFORE pico_sdk_import.cmake reads
    # the environment, so CMakeLists.txt silently wins and this script's
    # $env:PICO_SDK_PATH is ignored. That is fine while they agree and invisible
    # when they do not -- which is how a build can quietly compile against the
    # wrong SDK. Fail loudly instead of trusting the two to stay in sync.
    $expectedSdk = "$pico\sdk\$sdkVersion".Replace('\', '/')
    $resolvedSdk = (Select-String -Path "$bdir\CMakeCache.txt" `
                                  -Pattern '^PICO_SDK_PATH:[^=]*=(.*)$').Matches.Groups[1].Value
    if ($resolvedSdk.Replace('\', '/').TrimEnd('/') -ne $expectedSdk.TrimEnd('/')) {
        throw "SDK selection skew for ${b}${dirSuffix}: build.ps1 wants '$expectedSdk' but CMake resolved '$resolvedSdk'. Check sdkVersion in CMakeLists.txt."
    }

    Write-Host "=== Building $b$dirSuffix ===" -ForegroundColor Cyan
    & $cmake --build $bdir 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "build failed for $b$dirSuffix" }
    $uf2 = "$bdir\PicoSwitchWGA-$b.uf2"
    if (Test-Path $uf2) { Write-Host "OK -> $uf2" -ForegroundColor Green } else { throw "missing uf2 for $b$dirSuffix" }
}
