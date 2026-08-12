#!/usr/bin/env pwsh
<#
.SYNOPSIS
Capture one attributable Switch 2 native-motion experiment over UART.

.DESCRIPTION
Ordinary runs record one stationary condition in the retained paired-motion
ring, then run ns2_magprobe and optionally compare it with a baseline. With
-HybridMode, the runner instead performs one bounded, default-off genuine-base
/ DualSense-donor substitution and audits the exact base/output XOR before
generating a fixture.

The ordinary path never changes motion output. The hybrid path is explicitly
mutating, requires -Ready, records the selected mode in provenance, and always
returns the firmware to hybrid mode off after its bounded capture.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Scenario,
    [Parameter(Mandatory = $true)][string]$Action,
    [string]$Hypothesis = '',
    [string]$Variable = '',
    [string]$Expect = '',
    [string]$Baseline,
    [ValidateSet(
        'none',
        'sham',
        'neodymium-face-a',
        'neodymium-face-b',
        'ceramic-face-a',
        'ceramic-face-b',
        'unknown'
    )]
    [string]$Stimulus = 'none',
    [ValidateRange(0, 1000)][int]$DistanceMm = 0,
    [ValidateRange(100, 850)][int]$CaptureMs = 650,
    [ValidateSet('off', 'genuine', 'accel', 'gyro', 'prefix', 'imu', 'all')]
    [string]$HybridMode = 'off',
    [string]$Controller = 'Switch 2 Pro Controller',
    [string]$ExistingCapture,
    [string]$Port,
    [string]$OutputRoot = 'dumps/experiments',
    [switch]$Ready,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$reader = Join-Path $PSScriptRoot 'read_uart_diag.ps1'
$module = Join-Path $PSScriptRoot 'PicoSwitch2Lab.psm1'
Import-Module $module -Force

if (-not (Test-Path -LiteralPath $reader)) { throw "Missing $reader" }
$kind = if ($Variable) { 'hypothesis-test' } else { 'observation' }
$isHybrid = $HybridMode -ne 'off'
if ($isHybrid -and -not $Ready -and -not $DryRun) {
    throw 'Live motion hybrid experiments require -Ready explicit maintainer confirmation.'
}
if ($isHybrid -and $Baseline) {
    throw 'Hybrid captures are self-auditing base/output pairs; -Baseline is not applicable.'
}
$conditions = [ordered]@{
    controller = $Controller
    stimulus = $Stimulus
    distance_mm = $DistanceMm
    capture_ms = $CaptureMs
    controller_stationary = $true
    hybrid_mode = $HybridMode
}

Write-Host "Motion experiment: $Scenario" -ForegroundColor Cyan
Write-Host "Kind             : $kind"
Write-Host "Condition        : $Stimulus at $DistanceMm mm"
Write-Host "Capture          : $CaptureMs ms, controller stationary"
Write-Host "Hybrid mode      : $HybridMode"
if ($Hypothesis) { Write-Host "Hypothesis       : $Hypothesis" }
if ($Expect) { Write-Host "Expect           : $Expect" }
Write-Host "Action           : $Action" -ForegroundColor Yellow
if ($DryRun) {
    Write-Host 'Dry run: no UART traffic and no artifact directory created.'
    exit 0
}

$python = Get-Ps2LabPython
if (-not $python) { throw 'Python is required for motion analysis.' }

$diagnosticCommands = @(
    'motionpair status',
    'motionhybrid status',
    'magraw status',
    'motionauto',
    'input status',
    'ds5motion status',
    'audio status',
    'btversion'
)
$before = [ordered]@{}
$after = [ordered]@{}

if ($ExistingCapture) {
    $tracePath = if ([System.IO.Path]::IsPathRooted($ExistingCapture)) {
        [System.IO.Path]::GetFullPath($ExistingCapture)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $repoRoot $ExistingCapture))
    }
    if (-not (Test-Path -LiteralPath $tracePath -PathType Leaf)) {
        throw "Existing capture does not exist: $tracePath"
    }
    $outputDir = Split-Path -Parent $tracePath
    $Port = 'offline-resume'
    foreach ($phase in @('before', 'after')) {
        $diagnosticPath = Join-Path $outputDir "diagnostics.$phase.json"
        if (Test-Path -LiteralPath $diagnosticPath -PathType Leaf) {
            $value = Get-Content -Raw -LiteralPath $diagnosticPath |
                ConvertFrom-Json
            if ($phase -eq 'before') { $before = $value } else { $after = $value }
        }
    }
    Write-Host 'UART             : skipped (offline resume)'
    Write-Host "Existing capture : $tracePath"
    Write-Host "Artifacts        : $outputDir"
} else {
    $portInfo = Get-Ps2LabPort -Reader $reader -Port $Port
    $Port = $portInfo.port
    $outputDir = New-Ps2LabDirectory -RepoRoot $repoRoot `
        -OutputRoot $OutputRoot -Scenario $Scenario
    Write-Host "UART             : $Port ($($portInfo.name))"
    Write-Host "Artifacts        : $outputDir"

    foreach ($command in $diagnosticCommands) {
        $before[$command] = Get-Ps2LabJsonReply -Reader $reader -Port $Port `
            -Command $command -AllowFailure
    }
    Set-Ps2LabJson -Path (Join-Path $outputDir 'diagnostics.before.json') `
        -Value $before

    $nativeMotion = $before['motionauto']
    if (-not $nativeMotion -or -not $nativeMotion.fired -or
        $nativeMotion.pid -eq '0x0000') {
        throw ('No active native-motion source before capture. ' +
               'Wake or reconnect the genuine controller, confirm input, ' +
               'then rerun without changing the physical condition.')
    }
    if ($isHybrid -and $HybridMode -ne 'genuine') {
        $donor = $before['motionhybrid status']
        if (-not $donor -or -not $donor.donor_seen -or
            [uint64]$donor.donor_age_us -gt 20000 -or
            [int]$donor.donor_cal_state -ne 2) {
            throw ('No fresh calibrated DualSense donor before live capture. ' +
                   'Connect it alongside the genuine controller, leave both ' +
                   'stationary through calibration, and rerun. Status: ' +
                   ($donor | ConvertTo-Json -Compress))
        }
    }

    Write-Host ''
    Write-Host '>>> PREPARE THIS ONE CONTROLLED CONDITION:' -ForegroundColor Yellow
    Write-Host ">>> $Action" -ForegroundColor Yellow
    if (-not $Ready) {
        Read-Host '>>> press Enter when the controller and stimulus are stationary'
    } else {
        Write-Host '>>> maintainer explicitly confirmed ready; capturing now'
    }

    $tracePath = Join-Path $outputDir $(if ($isHybrid) {
        'motion.hybrid.raw.jsonl'
    } else {
        'motion.raw.jsonl'
    })
    $captureCommand = if ($isHybrid) {
        "motionhybrid capture $HybridMode"
    } else {
        'motionpair capture'
    }
    Invoke-Ps2LabDiag -Reader $reader -Port $Port -Command $captureCommand `
        -CaptureMs $CaptureMs -OutputPath $tracePath | Out-Null

    foreach ($command in $diagnosticCommands) {
        $after[$command] = Get-Ps2LabJsonReply -Reader $reader -Port $Port `
            -Command $command -AllowFailure
    }
    Set-Ps2LabJson -Path (Join-Path $outputDir 'diagnostics.after.json') `
        -Value $after
}

$analysisText = $null
$analysisCsv = $null
if ($isHybrid) {
    $analysisJson = Join-Path $outputDir 'motion.hybrid.audit.json'
    $auditOutput = @(
        & $python (Join-Path $PSScriptRoot 'ns2_motion_hybrid.py') `
            audit-capture $tracePath --output $analysisJson)
    if ($LASTEXITCODE -ne 0) { throw 'live motion-hybrid audit failed.' }
} else {
    $analysisText = Join-Path $outputDir 'motion.analysis.txt'
    $analysisJson = Join-Path $outputDir 'motion.analysis.json'
    $analysisCsv = Join-Path $outputDir 'motion.0x28.csv'
    $textOutput = @(
        & $python (Join-Path $PSScriptRoot 'ns2_magprobe.py') analyze `
            $tracePath --csv $analysisCsv --correlations 24)
    if ($LASTEXITCODE -ne 0) { throw 'ns2_magprobe text analysis failed.' }
    Set-Ps2LabText -Path $analysisText -Lines $textOutput
    $jsonOutput = @(
        & $python (Join-Path $PSScriptRoot 'ns2_magprobe.py') analyze `
            $tracePath --json)
    if ($LASTEXITCODE -ne 0) { throw 'ns2_magprobe JSON analysis failed.' }
    Set-Ps2LabText -Path $analysisJson -Lines $jsonOutput
}

$analysis = Get-Content -Raw -LiteralPath $analysisJson | ConvertFrom-Json
if ([int]$analysis.dropped -ne 0) {
    throw "motion capture lost $($analysis.dropped) record(s)"
}
if (-not $isHybrid -and [int]$analysis.length_40 -eq 0) {
    throw 'motion capture contains no length-0x28 records'
}

$comparisonText = $null
$comparisonJson = $null
if ($Baseline) {
    $baselinePath = if ([System.IO.Path]::IsPathRooted($Baseline)) {
        [System.IO.Path]::GetFullPath($Baseline)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Baseline))
    }
    if (-not (Test-Path -LiteralPath $baselinePath)) {
        throw "Baseline does not exist: $baselinePath"
    }
    $comparisonText = Join-Path $outputDir 'motion.comparison.txt'
    $comparisonJson = Join-Path $outputDir 'motion.comparison.json'
    $comparisonTextOutput = @(
        & $python (Join-Path $PSScriptRoot 'ns2_magprobe.py') compare `
            $baselinePath $tracePath)
    $comparisonTextExit = $LASTEXITCODE
    if ($comparisonTextExit -ge 2) {
        throw 'ns2_magprobe text comparison failed.'
    }
    Set-Ps2LabText -Path $comparisonText -Lines $comparisonTextOutput
    $comparisonJsonOutput = @(
        & $python (Join-Path $PSScriptRoot 'ns2_magprobe.py') compare `
            $baselinePath $tracePath --json)
    $comparisonJsonExit = $LASTEXITCODE
    if ($comparisonJsonExit -ge 2) {
        throw 'ns2_magprobe JSON comparison failed.'
    }
    Set-Ps2LabText -Path $comparisonJson -Lines $comparisonJsonOutput
}

$fixtureJson = Join-Path $outputDir 'motion.fixture.json'
$fixtureC = Join-Path $outputDir 'motion.fixture.h'
$fitmentJson = $null
$fixtureName = 'motion_' + ($Scenario -replace '[^A-Za-z0-9_]', '_')
& $python (Join-Path $PSScriptRoot 'capture_to_fixture.py') $tracePath `
    --name $fixtureName --output-json $fixtureJson --output-c $fixtureC
if ($LASTEXITCODE -ne 0) { throw 'capture-to-fixture generation failed.' }
if (-not $isHybrid) {
    $fitmentJson = Join-Path $outputDir 'motion.fitment.json'
    & $python (Join-Path $PSScriptRoot 'ns2_motion_hybrid.py') capture $tracePath `
        --output $fitmentJson | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'motion fitment manifest generation failed.' }
}

$verdict = if ($isHybrid) {
    "hybrid capture valid and fail-closed ($HybridMode)"
} else {
    'capture valid'
}
if ($Baseline) {
    $comparison = Get-Content -Raw -LiteralPath $comparisonJson |
        ConvertFrom-Json
    $verdict += '; baseline comparison generated'
    if ($comparison.movement_warning) {
        $verdict += '; conservative movement warning'
    }
}
$artifactPaths = @(
    (Join-Path $outputDir 'diagnostics.before.json'),
    (Join-Path $outputDir 'diagnostics.after.json'),
    $tracePath,
    $analysisJson,
    $fixtureJson,
    $fixtureC
)
if ($analysisText) { $artifactPaths += $analysisText }
if ($analysisCsv) { $artifactPaths += $analysisCsv }
if ($fitmentJson) { $artifactPaths += $fitmentJson }
if ($comparisonText) { $artifactPaths += $comparisonText }
if ($comparisonJson) { $artifactPaths += $comparisonJson }

$manifest = New-Ps2LabManifest -Tool 'motion_lab' -Scenario $Scenario `
    -Action $Action -RepoRoot $repoRoot -Kind $kind `
    -Hypothesis $Hypothesis -Variable $Variable -Expectation $Expect `
    -Port $Port -Baseline $Baseline -Conditions $conditions `
    -Diagnostics ([ordered]@{ before = $before; after = $after }) `
    -Verdict $verdict -ArtifactPaths $artifactPaths
Set-Ps2LabJson -Path (Join-Path $outputDir 'experiment.json') -Value $manifest

Write-Host ''
Write-Host "Result           : $verdict" -ForegroundColor Green
if ($isHybrid) {
    Write-Host "Records          : $($analysis.records)"
    Write-Host "Applied/fallback : $($analysis.applied) / $($analysis.fallbacks)"
    Write-Host "Changed records  : $($analysis.changed_records)"
} else {
    Write-Host "0x1E / 0x28      : $($analysis.length_30) / $($analysis.length_40)"
    Write-Host "Duration         : $([math]::Round($analysis.duration_ms, 1)) ms"
}
Write-Host "Dropped          : $($analysis.dropped)"
Write-Host "Artifacts        : $outputDir"
