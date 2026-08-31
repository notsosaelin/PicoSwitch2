#!/usr/bin/env pwsh
# Run the §14.5 HOGP probe WITH PACKAGE IDENTITY.
#
# THE ONE VARIABLE THIS CHANGES. The unpackaged run answered B1 and B2 and then
# hit an advertiser abort, leaving B3-B6 unmeasured. Two explanations survived
# and they lead to different product answers:
#
#   H1  the process has no package identity and no declared `bluetooth`
#       capability -- the shipping companion has both
#   H2  the radio or driver will not accept a connectable peripheral
#       advertisement, whatever `IsPeripheralRoleSupported` claims
#
# The executable here is byte-identical to the unpackaged one. Identity is the
# only thing that differs, which is what makes this a controlled comparison
# rather than a second attempt.
#
# Requires Developer Mode (loose-file registration is unsigned). Registration is
# per-user and is removed again by -Unregister.
#
#   pwsh -File tools/hogp_probe/run-packaged.ps1              # B1, B2, advertising
#   pwsh -File tools/hogp_probe/run-packaged.ps1 -Seconds 120 # full run, B3-B6
#   pwsh -File tools/hogp_probe/run-packaged.ps1 -Unregister
param(
    [int]$Seconds = 0,
    [string[]]$ProbeArgs = @(),
    [switch]$Unregister,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot | Split-Path -Parent
$probe = Join-Path $root 'tools/hogp_probe'
$layout = Join-Path $probe 'package'
$family = 'PicoSwitch2.Lab.HogpProbe_gsjdqbwyz5r4g'
$name = 'PicoSwitch2.Lab.HogpProbe'

function Get-Registered {
    Get-AppxPackage -Name $name -ErrorAction SilentlyContinue
}

if ($Unregister) {
    $existing = Get-Registered
    if ($existing) {
        Remove-AppxPackage -Package $existing.PackageFullName
        Write-Host "unregistered $($existing.PackageFullName)"
    } else {
        Write-Host 'nothing registered'
    }
    return
}

$devMode = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
if (-not (Test-Path $devMode) -or
    (Get-ItemProperty $devMode).AllowDevelopmentWithoutDevLicense -ne 1) {
    throw 'Developer Mode is required to register an unsigned loose-file package. ' +
          'Settings > System > For developers > Developer Mode.'
}

if (-not $SkipBuild) {
    Write-Host '== building probe =='
    dotnet build (Join-Path $probe 'HogpProbe.csproj') -c Release --nologo -v q
    if ($LASTEXITCODE -ne 0) { throw 'probe build failed' }
}

# The package layout is the manifest plus the build output, side by side. Copied
# rather than referenced: Add-AppxPackage -Register resolves Executable relative
# to the manifest, so the exe has to sit next to it.
$binaries = Join-Path $probe 'bin/Release/net9.0-windows10.0.22621.0/win-x64'
if (-not (Test-Path (Join-Path $binaries 'HogpProbe.exe'))) {
    throw "probe binaries not found at $binaries"
}

Write-Host '== staging package layout =='
Get-ChildItem $layout -File |
    Where-Object { $_.Name -notin @('AppxManifest.xml', 'logo.png') } |
    Remove-Item -Force
Copy-Item (Join-Path $binaries '*') $layout -Recurse -Force -Exclude '*.pdb'

# Re-registration is not incremental: a stale registration keeps pointing at the
# previous layout, and the run would silently measure old code.
$existing = Get-Registered
if ($existing) {
    Remove-AppxPackage -Package $existing.PackageFullName
}

Write-Host '== registering =='
Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml')
$package = Get-Registered
if (-not $package) { throw 'registration reported success but the package is not present' }
Write-Host "registered $($package.PackageFullName)"
$family = $package.PackageFamilyName

$findings = Join-Path $probe 'findings-packaged.json'
if (Test-Path $findings) { Remove-Item $findings -Force }

$arguments = @('--out', $findings) + $ProbeArgs
if ($Seconds -gt 0) { $arguments += @('--seconds', "$Seconds") } else { $arguments += '--probe-only' }

Write-Host "== running with identity $family =="
# Invoke-CommandInDesktopPackage launches the process INSIDE the package's
# identity context. It returns immediately and does not relay stdout, which is
# why the probe writes its findings to a file.
Invoke-CommandInDesktopPackage -PackageFamilyName $family -AppId 'HogpProbe' `
    -Command (Join-Path $layout 'HogpProbe.exe') -Args ($arguments -join ' ')

$budget = if ($Seconds -gt 0) { $Seconds + 60 } else { 90 }
$clock = [Diagnostics.Stopwatch]::StartNew()
while (-not (Test-Path $findings) -and $clock.Elapsed.TotalSeconds -lt $budget) {
    Start-Sleep -Milliseconds 500
}

if (-not (Test-Path $findings)) {
    throw "the packaged probe produced no findings within $budget s; it may have failed to start"
}

Write-Host '== findings =='
Get-Content $findings -Raw
