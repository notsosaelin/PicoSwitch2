# Build + run the Android companion JVM unit tests in one command.
#
#   pwsh -File tools/run_android_tests.ps1
#
# These are pure JVM tests (no device, no emulator): the HID report encoder, the
# v2 motion/battery wire layout, the adapter-feedback decoder, sensor scaling,
# amiibo crypto/library, and the management protocol. They cover the Android half
# of the bridge contract; the firmware half is tools/run_mgmt_tests.ps1 and the
# cross-language descriptor check is tools/check_android_descriptor_parity.py.
#
# Toolchain notes (both were real blockers, so they are resolved here rather than
# left to environment luck):
#   * Gradle 8.13 cannot parse Java 25, which is what Android Studio's bundled JBR
#     now is -- using it fails with a bare "IllegalArgumentException: 25.0.2"
#     before any source is compiled. A JDK 17/21 is required.
#   * The Android SDK location must be discoverable; local.properties is not
#     checked in, so ANDROID_HOME is set from the standard install path.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root 'android/companion'

function Find-Jdk {
    if ($env:JAVA_HOME -and (Test-Path (Join-Path $env:JAVA_HOME 'bin/java.exe'))) {
        $version = & (Join-Path $env:JAVA_HOME 'bin/java.exe') -version 2>&1 | Select-Object -First 1
        if ($version -notmatch 'version "(2[2-9]|[3-9][0-9])') { return $env:JAVA_HOME }
    }
    $roots = @("$env:ProgramFiles\Eclipse Adoptium", "$env:ProgramFiles\Java",
               "$env:ProgramFiles\Microsoft\jdk")
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        $hit = Get-ChildItem $r -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'jdk-(17|21)' } |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($hit -and (Test-Path (Join-Path $hit.FullName 'bin/java.exe'))) { return $hit.FullName }
    }
    return $null
}

$jdk = Find-Jdk
if (-not $jdk) {
    Write-Host "No JDK 17/21 found. Install one, e.g.:" -ForegroundColor Red
    Write-Host "  winget install --id EclipseAdoptium.Temurin.21.JDK" -ForegroundColor Yellow
    Write-Host "(Android Studio's bundled JBR is Java 25, which Gradle 8.13 rejects.)" -ForegroundColor Yellow
    exit 1
}
$env:JAVA_HOME = $jdk
Write-Host "JAVA_HOME = $jdk" -ForegroundColor Cyan

if (-not $env:ANDROID_HOME) {
    $sdk = "$env:LOCALAPPDATA\Android\Sdk"
    if (Test-Path $sdk) { $env:ANDROID_HOME = $sdk }
    else { Write-Host "Android SDK not found; set ANDROID_HOME." -ForegroundColor Red; exit 1 }
}
Write-Host "ANDROID_HOME = $env:ANDROID_HOME" -ForegroundColor Cyan

Set-Location $project
& ./gradlew.bat :app:testDebugUnitTest --console=plain @args
$code = $LASTEXITCODE
if ($code -ne 0) {
    Write-Host "Android unit tests FAILED. Report:" -ForegroundColor Red
    Write-Host "  $project/app/build/reports/tests/testDebugUnitTest/index.html"
} else {
    Write-Host "Android companion unit tests passed" -ForegroundColor Green
}
exit $code
