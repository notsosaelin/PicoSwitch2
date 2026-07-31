#!/usr/bin/env pwsh
<#
.SYNOPSIS
Package and validate an offline PicoSwitch2 command-0x0D firmware capture.

.DESCRIPTION
This tool never talks to a controller and never writes firmware. It validates a
full retained trace with ns2_firmware_update.py, emits the reassembled encrypted
image plus metadata, and generates a command-0x0D fixture. A generic trace that
retained only 24 bytes per command is rejected instead of producing a partial
blob.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Trace,
    [string]$Scenario = 'firmware-update-capture',
    [string]$OutputRoot = 'dumps/experiments',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$module = Join-Path $PSScriptRoot 'PicoSwitch2Lab.psm1'
Import-Module $module -Force
$python = Get-Ps2LabPython
if (-not $python) { throw 'Python is required for firmware capture analysis.' }
$tracePath = if ([System.IO.Path]::IsPathRooted($Trace)) {
    [System.IO.Path]::GetFullPath($Trace)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Trace))
}
if (-not (Test-Path -LiteralPath $tracePath)) {
    throw "Trace does not exist: $tracePath"
}

Write-Host "Firmware capture: $tracePath" -ForegroundColor Cyan
Write-Host 'Safety          : offline parsing only; no UART and no device writes'
if ($DryRun) {
    Write-Host 'Dry run: no files written.'
    exit 0
}

$outputDir = New-Ps2LabDirectory -RepoRoot $repoRoot `
    -OutputRoot $OutputRoot -Scenario $Scenario
$imagePath = Join-Path $outputDir 'firmware-update.bin'
$metadataPath = Join-Path $outputDir 'firmware-update.json'
$analysisPath = Join-Path $outputDir 'firmware-update.txt'
$fixturePath = Join-Path $outputDir 'firmware-update.fixture.json'
$fixtureCPath = Join-Path $outputDir 'firmware-update.fixture.h'

$analysisOutput = @(
    & $python (Join-Path $PSScriptRoot 'ns2_firmware_update.py') `
        $tracePath --format trace --image $imagePath --metadata $metadataPath)
if ($LASTEXITCODE -ne 0) { throw 'Firmware update validation failed.' }
Set-Ps2LabText -Path $analysisPath -Lines $analysisOutput
& $python (Join-Path $PSScriptRoot 'capture_to_fixture.py') $tracePath `
    --name firmware_update_0d --command 0x0D `
    --output-json $fixturePath --output-c $fixtureCPath
if ($LASTEXITCODE -ne 0) { throw 'Firmware fixture generation failed.' }

$metadata = Get-Content -Raw -LiteralPath $metadataPath | ConvertFrom-Json
$artifactPaths = @(
    $imagePath,
    $metadataPath,
    $analysisPath,
    $fixturePath,
    $fixtureCPath
)
$manifest = New-Ps2LabManifest -Tool 'firmware_lab' -Scenario $Scenario `
    -Action 'offline validation of a captured command-0x0D transfer' `
    -RepoRoot $repoRoot -Kind 'observation' `
    -Conditions ([ordered]@{
        offline_only = $true
        source_trace = $tracePath
        target_addresses_acted_on = $false
    }) -Verdict 'complete capture validated' -ArtifactPaths $artifactPaths
Set-Ps2LabJson -Path (Join-Path $outputDir 'experiment.json') -Value $manifest

Write-Host ''
Write-Host 'Result          : complete capture validated' -ForegroundColor Green
Write-Host "Bytes           : $($metadata.observed_size)"
Write-Host "CRC32           : $('{0:X8}' -f [uint32]$metadata.computed_crc32)"
Write-Host "SHA256          : $($metadata.sha256)"
Write-Host "Artifacts       : $outputDir"
