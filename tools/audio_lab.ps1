#!/usr/bin/env pwsh
<#
.SYNOPSIS
Capture a non-mutating PicoSwitch2 audio regression experiment over UART.

.DESCRIPTION
Samples cumulative audio diagnostics while the maintainer performs one playback
action. The default path never resets counters and never changes codec, route,
speaker, microphone, or Pro Controller 2 audio state. -ResetCounters is an
explicit mutation intended only for a controlled baseline.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Scenario,
    [Parameter(Mandatory = $true)][string]$Action,
    [string]$Hypothesis = '',
    [string]$Variable = '',
    [string]$Expect = '',
    [ValidateRange(2, 3600)][int]$DurationSeconds = 30,
    [ValidateRange(1, 60)][int]$SampleSeconds = 5,
    [string]$Baseline,
    [string]$Port,
    [string]$OutputRoot = 'dumps/experiments',
    [switch]$ResetCounters,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$reader = Join-Path $PSScriptRoot 'read_uart_diag.ps1'
$module = Join-Path $PSScriptRoot 'PicoSwitch2Lab.psm1'
Import-Module $module -Force

$kind = if ($Variable) { 'hypothesis-test' } else { 'observation' }
Write-Host "Audio experiment: $Scenario" -ForegroundColor Cyan
Write-Host "Kind            : $kind"
Write-Host "Duration        : $DurationSeconds s"
Write-Host "Sample interval : $SampleSeconds s"
Write-Host "Counter reset   : $($ResetCounters.IsPresent)"
Write-Host "Action          : $Action" -ForegroundColor Yellow
Write-Host 'Mutation policy : no codec/route/stream commands' -ForegroundColor DarkGray
if ($DryRun) {
    Write-Host 'Dry run: no UART traffic and no artifact directory created.'
    exit 0
}

$python = Get-Ps2LabPython
if (-not $python) { throw 'Python is required for audio analysis.' }
$portInfo = Get-Ps2LabPort -Reader $reader -Port $Port
$Port = $portInfo.port
$outputDir = New-Ps2LabDirectory -RepoRoot $repoRoot `
    -OutputRoot $OutputRoot -Scenario $Scenario

$diagnosticCommands = @(
    'audio status',
    'audio headset',
    'ds5codec status',
    'pro2audio status',
    'input status',
    'motionauto'
)
$before = [ordered]@{}
foreach ($command in $diagnosticCommands) {
    $before[$command] = Get-Ps2LabJsonReply -Reader $reader -Port $Port `
        -Command $command -AllowFailure
}
Set-Ps2LabJson -Path (Join-Path $outputDir 'diagnostics.before.json') `
    -Value $before

if ($ResetCounters) {
    $cleared = Get-Ps2LabJsonReply -Reader $reader -Port $Port `
        -Command 'audio clear'
    if ($cleared.audio -ne 'cleared') { throw 'Audio counters did not clear.' }
}

Write-Host ''
Write-Host '>>> START THIS ONE PLAYBACK ACTION:' -ForegroundColor Yellow
Write-Host ">>> $Action" -ForegroundColor Yellow
Read-Host '>>> press Enter when playback is active'

$samples = [System.Collections.Generic.List[object]]::new()
$clock = [System.Diagnostics.Stopwatch]::StartNew()
while ($true) {
    $audio = Get-Ps2LabJsonReply -Reader $reader -Port $Port `
        -Command 'audio status'
    $samples.Add([pscustomobject][ordered]@{
        elapsed_ms = [int64]$clock.ElapsedMilliseconds
        captured_at = (Get-Date).ToString('o')
        audio = $audio
    })
    if ($clock.Elapsed.TotalSeconds -ge $DurationSeconds) { break }
    $remaining = $DurationSeconds - $clock.Elapsed.TotalSeconds
    Start-Sleep -Milliseconds ([int](
        [math]::Min($SampleSeconds, $remaining) * 1000))
}
$clock.Stop()

$after = [ordered]@{}
foreach ($command in $diagnosticCommands) {
    $after[$command] = Get-Ps2LabJsonReply -Reader $reader -Port $Port `
        -Command $command -AllowFailure
}
Set-Ps2LabJson -Path (Join-Path $outputDir 'diagnostics.after.json') `
    -Value $after

$samplesPath = Join-Path $outputDir 'audio.samples.json'
Set-Ps2LabJson -Path $samplesPath -Value ([ordered]@{
    schema = 'picoswitch2-audio-samples/v1'
    reset_counters = $ResetCounters.IsPresent
    samples = $samples.ToArray()
})

$analysisText = Join-Path $outputDir 'audio.analysis.txt'
$analysisJson = Join-Path $outputDir 'audio.analysis.json'
$arguments = @(
    (Join-Path $PSScriptRoot 'audio_lab_analyze.py'),
    $samplesPath
)
if ($Baseline) {
    $baselinePath = if ([System.IO.Path]::IsPathRooted($Baseline)) {
        [System.IO.Path]::GetFullPath($Baseline)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Baseline))
    }
    if (-not (Test-Path -LiteralPath $baselinePath)) {
        throw "Baseline does not exist: $baselinePath"
    }
    $arguments += @('--baseline', $baselinePath)
}
$analysisTextOutput = @(& $python @arguments)
if ($LASTEXITCODE -ne 0) { throw 'Audio text analysis failed.' }
Set-Ps2LabText -Path $analysisText -Lines $analysisTextOutput
$analysisJsonOutput = @(& $python @arguments --json)
if ($LASTEXITCODE -ne 0) { throw 'Audio JSON analysis failed.' }
Set-Ps2LabText -Path $analysisJson -Lines $analysisJsonOutput
$analysis = Get-Content -Raw -LiteralPath $analysisJson | ConvertFrom-Json
$verdict = if ($Baseline) {
    [string]$analysis.current.verdict
} else {
    [string]$analysis.verdict
}

$artifactPaths = @(
    (Join-Path $outputDir 'diagnostics.before.json'),
    (Join-Path $outputDir 'diagnostics.after.json'),
    $samplesPath,
    $analysisText,
    $analysisJson
)
$manifest = New-Ps2LabManifest -Tool 'audio_lab' -Scenario $Scenario `
    -Action $Action -RepoRoot $repoRoot -Kind $kind `
    -Hypothesis $Hypothesis -Variable $Variable -Expectation $Expect `
    -Port $Port -Baseline $Baseline `
    -Conditions ([ordered]@{
        duration_seconds = $DurationSeconds
        sample_seconds = $SampleSeconds
        reset_counters = $ResetCounters.IsPresent
        mutation_policy = 'observational except explicit counter reset'
    }) -Diagnostics ([ordered]@{ before = $before; after = $after }) `
    -Verdict $verdict -ArtifactPaths $artifactPaths
Set-Ps2LabJson -Path (Join-Path $outputDir 'experiment.json') -Value $manifest

Write-Host ''
Write-Host "Result          : $verdict" -ForegroundColor Green
Write-Host "Artifacts       : $outputDir"
Get-Content -LiteralPath $analysisText
