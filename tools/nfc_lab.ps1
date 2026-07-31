#!/usr/bin/env pwsh
<#
.SYNOPSIS
One instrumented NFC hardware run, captured as a complete artifact bundle.

.DESCRIPTION
The v3 investigation asked the maintainer to repeat physical tests far more
often than the evidence required, and several runs produced results that could
not be attributed afterwards because the firmware revision, the loaded image,
or the diagnostic baseline was not recorded alongside the trace.

This script makes the hardware unit of work an *experiment*, not a build:

  * every UART command and output filename is owned here, not typed by hand;
  * baseline and post-run diagnostics are captured automatically;
  * the git revision, dirty state, and firmware identity are recorded;
  * the maintainer performs exactly one described physical action;
  * the trace is decoded and, when a baseline is supplied, compared;
  * every artifact is hashed into experiment.json.

It orchestrates read_uart_diag.ps1 rather than reimplementing the serial
transport, so there is one place where UART framing can be wrong.

The tag image is fetched with -NoAcknowledge by default: acknowledging clears
the firmware's dirty-state protection, which is a mutation, and an
observational run must not change the state it is observing.

.EXAMPLE
./tools/nfc_lab.ps1 -Scenario v3-air-riders-write `
  -Hypothesis 'The sector-1 capability page is retained across a power cycle' `
  -Variable 'firmware: persist capability page in the extended commit' `
  -Expect 'reuse read returns generation 2, no 2115-0096' `
  -Action 'Scan King Dedede in Air Riders, save, remove the figure'

.EXAMPLE
./tools/nfc_lab.ps1 -Scenario v3-reuse -Action 'Rescan the written figure' `
  -Baseline dumps/amiibo/genuine-air-riders-postwrite-read-only-2026-07-28.jsonl
#>
[CmdletBinding()]
param(
    # Short slug naming what is being exercised. Becomes the artifact directory.
    [Parameter(Mandatory = $true)][string]$Scenario,

    # The single physical action the maintainer should perform. Printed once,
    # verbatim. If you need two actions, you need two experiments.
    [Parameter(Mandatory = $true)][string]$Action,

    [string]$Hypothesis = '',
    # The one intended variable. Prompted for when omitted interactively, so a
    # later reader can tell whether a result was attributable or whether several
    # things moved at once. Answer with an empty line to record an observation
    # run instead; both are legitimate, and experiment.json says which.
    [string]$Variable = '',
    # The pass/fail discriminator. Prompted for on a hypothesis test.
    [string]$Expect = '',

    [string]$Port,
    [string]$OutputRoot = 'dumps/experiments',
    # A genuine-controller or known-good capture to compare against.
    [string]$Baseline,
    # Compare shape rather than identity when the baseline is a different figure.
    [switch]$IgnoreIdentity,
    # Skip the tag image download (faster, and required when no image is loaded).
    [switch]$NoImage,
    # Clear the firmware's dirty flag after downloading the image. Off by
    # default: this mutates state and is never needed to observe a run.
    [switch]$Acknowledge,
    # Run unattended, dwelling this long instead of waiting for Enter.
    [int]$DwellSeconds = 0,
    # Print the plan and exit without touching the UART.
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$reader = Join-Path $PSScriptRoot 'read_uart_diag.ps1'
if (-not (Test-Path $reader)) { throw "Missing $reader" }

function Get-PythonCommand {
    foreach ($candidate in @('python', 'py', 'python3')) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    return $null
}

function Invoke-Diag {
    param([string]$Command, [string]$OutputPath, [switch]$AllowFailure)
    $arguments = @{ Command = $Command }
    if ($Port) { $arguments['Port'] = $Port }
    if ($OutputPath) { $arguments['OutputPath'] = $OutputPath }
    if ($Command -eq 'amiibo dump' -and -not $Acknowledge) {
        $arguments['NoAcknowledge'] = $true
    }
    try {
        return @(& $reader @arguments 2>&1 | Where-Object { $_ -is [string] })
    } catch {
        if ($AllowFailure) {
            Write-Warning "$Command failed: $($_.Exception.Message)"
            return @("{`"error`":`"$($_.Exception.Message -replace '"', "'")`"}")
        }
        throw
    }
}

# Windows PowerShell 5.1's `Set-Content -Encoding utf8` writes a UTF-8 BOM,
# pwsh 7's does not. An artifact bundle that parses on one machine and not the
# other is worse than useless, so write bytes explicitly the way
# read_uart_diag.ps1 already does. Caught by a real run: experiment.json from a
# 5.1 session began EF BB BF and no JSON reader would touch it.
function Save-Text {
    param([string]$Path, [string[]]$Lines)
    [System.IO.File]::WriteAllLines(
        $Path, $Lines, [System.Text.UTF8Encoding]::new($false))
}

function Get-FileDigest {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    $item = Get-Item $Path
    return [pscustomobject]@{
        path   = [System.IO.Path]::GetFileName($Path)
        bytes  = $item.Length
        sha256 = (Get-FileHash -Path $Path -Algorithm SHA256).Hash
    }
}

# --- provenance ------------------------------------------------------------
$git = [ordered]@{}
try {
    $git['branch'] = (git -C $repoRoot rev-parse --abbrev-ref HEAD).Trim()
    $git['commit'] = (git -C $repoRoot rev-parse HEAD).Trim()
    $status = @(git -C $repoRoot status --porcelain)
    $git['dirty'] = $status.Count -gt 0
    # A dirty tree means the flashed firmware may not match any commit. That is
    # allowed during active work, but it must be visible in the record.
    $git['dirty_files'] = @($status | Select-Object -First 20)
} catch {
    $git['error'] = $_.Exception.Message
}

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$slug = ($Scenario -replace '[^A-Za-z0-9\-_.]', '-')
$outputDir = Join-Path $repoRoot (Join-Path $OutputRoot "$timestamp-$slug")

Write-Host "Experiment : $Scenario" -ForegroundColor Cyan
Write-Host "Output     : $outputDir"

# Ask for the intended variable rather than warning about it afterwards. A
# result that cannot be attributed is the failure mode that produced the
# retracted v3 provenance conclusion, and a warning printed after the fact does
# nothing to prevent it. Not every run is a hypothesis test, though: capturing a
# genuine golden path is legitimately observational, and forcing a variable on
# those just trains people to type filler. So offer both, and record which.
$kind = 'hypothesis-test'
$interactive = $DwellSeconds -le 0
if (-not $Variable) {
    if ($interactive) {
        Write-Host ''
        Write-Host 'What is the ONE thing that changed since the last run?' -ForegroundColor Yellow
        Write-Host '(firmware change, different dump, different figure, a setting...)' -ForegroundColor DarkGray
        Write-Host 'Press Enter with nothing if this is an observation run.' -ForegroundColor DarkGray
        $Variable = (Read-Host 'Variable').Trim()
    }
    if (-not $Variable) {
        $kind = 'observation'
        $Variable = '(none - observation run)'
    }
}
if ($kind -eq 'hypothesis-test' -and -not $Expect -and $interactive) {
    Write-Host ''
    Write-Host 'What result would tell you the hypothesis is right or wrong?' -ForegroundColor Yellow
    Write-Host 'Press Enter to skip.' -ForegroundColor DarkGray
    $Expect = (Read-Host 'Pass/fail discriminator').Trim()
}

Write-Host ''
if ($Hypothesis) { Write-Host "Hypothesis : $Hypothesis" }
Write-Host "Variable   : $Variable"
if ($Expect)     { Write-Host "Expect     : $Expect" }
Write-Host "Kind       : $kind"
Write-Host "Action     : $Action" -ForegroundColor Yellow
if ($DryRun) {
    Write-Host 'Dry run: no UART traffic, no files written.' -ForegroundColor DarkGray
    exit 0
}

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

# --- port ------------------------------------------------------------------
if (-not $Port) {
    $reported = & $reader -ReportPort | ConvertFrom-Json
    $Port = $reported.port
    Write-Host "UART       : $Port ($($reported.name))" -ForegroundColor DarkGray
}

# --- baseline diagnostics --------------------------------------------------
# Captured before anything is armed so a later reader can tell what the board
# already held. `amiibo status` alone cannot identify a loaded v3 image; the
# journal header's payload CRC can, so both are recorded.
$diagnosticCommands = @('amiibo status', 'amiibo v3diag', 'amiibo journal', 'trace status')
$before = [ordered]@{}
foreach ($command in $diagnosticCommands) {
    $before[$command] = (Invoke-Diag -Command $command -AllowFailure) -join "`n"
}
Save-Text (Join-Path $outputDir 'diagnostics.before.json') `
    ($before | ConvertTo-Json -Depth 4)

$imageBefore = $null
if (-not $NoImage) {
    $imageBefore = Join-Path $outputDir 'image.before.bin'
    try { Invoke-Diag -Command 'amiibo dump' -OutputPath $imageBefore | Out-Null }
    catch { Write-Warning "image.before.bin unavailable: $($_.Exception.Message)"; $imageBefore = $null }
}

# --- arm -------------------------------------------------------------------
Invoke-Diag -Command 'trace clear' | Out-Null
Invoke-Diag -Command 'trace start nfc' | Out-Null

Write-Host ''
Write-Host '>>> PERFORM THIS ONE ACTION NOW:' -ForegroundColor Yellow
Write-Host ">>> $Action" -ForegroundColor Yellow
if ($DwellSeconds -gt 0) {
    Write-Host ">>> capturing for $DwellSeconds s" -ForegroundColor DarkGray
    Start-Sleep -Seconds $DwellSeconds
} else {
    Read-Host '>>> press Enter when the action is complete'
}

# --- collect ---------------------------------------------------------------
$tracePath = Join-Path $outputDir 'trace.raw.jsonl'
Invoke-Diag -Command 'trace dump' -OutputPath $tracePath | Out-Null

$after = [ordered]@{}
foreach ($command in $diagnosticCommands) {
    $after[$command] = (Invoke-Diag -Command $command -AllowFailure) -join "`n"
}
Save-Text (Join-Path $outputDir 'diagnostics.after.json') `
    ($after | ConvertTo-Json -Depth 4)

$imageAfter = $null
if (-not $NoImage) {
    $imageAfter = Join-Path $outputDir 'image.after.bin'
    try { Invoke-Diag -Command 'amiibo dump' -OutputPath $imageAfter | Out-Null }
    catch { Write-Warning "image.after.bin unavailable: $($_.Exception.Message)"; $imageAfter = $null }
}

# --- decode ----------------------------------------------------------------
$python = Get-PythonCommand
$decodedPath = Join-Path $outputDir 'trace.decoded.txt'
$comparisonPath = Join-Path $outputDir 'comparison.md'
$verdict = 'not decoded (python not found)'
if ($python -and (Test-Path $tracePath)) {
    Save-Text $decodedPath (& $python (Join-Path $PSScriptRoot 'ns2_trace.py') nfc $tracePath)
    $decoded = Get-Content $decodedPath
    # 07/41 is also the deliberate TagRemoved signal after a committed write,
    # so the decoder separates removal edges from genuine failures. Counting
    # both as errors made a healthy write/remove/rescan cycle look broken.
    $errors = @($decoded | Select-String -Pattern 'error state \(detail')
    $removals = @($decoded | Select-String -Pattern 'tag removed after a committed write')
    $verdict = if ($errors.Count) {
        "$($errors.Count) error state(s); first: $($errors[0].Line.Trim())"
    } else {
        $summary = ($decoded | Select-Object -First 1)
        if ($removals.Count) {
            "$summary; no errors, $($removals.Count) expected removal edge(s)"
        } else {
            $summary
        }
    }

    if ($Baseline) {
        $baselinePath = if ([System.IO.Path]::IsPathRooted($Baseline)) { $Baseline }
                        else { Join-Path $repoRoot $Baseline }
        $diffArguments = @((Join-Path $PSScriptRoot 'ns2_trace.py'), 'nfc-diff',
                           $baselinePath, $tracePath)
        if ($IgnoreIdentity) { $diffArguments += '--ignore-identity' }
        $comparison = & $python @diffArguments
        Save-Text $comparisonPath `
            @("# Comparison", '', "Baseline: ``$Baseline``", '', '```', $comparison, '```')
    }
}

# --- manifest --------------------------------------------------------------
$artifacts = @(
    (Get-FileDigest (Join-Path $outputDir 'diagnostics.before.json'))
    (Get-FileDigest (Join-Path $outputDir 'diagnostics.after.json'))
    (Get-FileDigest $tracePath)
    (Get-FileDigest $decodedPath)
    (Get-FileDigest $comparisonPath)
    if ($imageBefore) { Get-FileDigest $imageBefore }
    if ($imageAfter) { Get-FileDigest $imageAfter }
) | Where-Object { $_ }

# The firmware counts its own internal failures, and the wire cannot express
# them. If the decoded trace and v3diag disagree, trust the firmware and treat
# the decoder as wrong -- that is exactly how the removal-edge false positive
# was found.
$firmwareErrors = $null
try {
    $v3diag = $after['amiibo v3diag'] | ConvertFrom-Json
    if ($null -ne $v3diag.errors) {
        $firmwareErrors = [int]$v3diag.errors
        if ($firmwareErrors -gt 0) {
            $verdict = "$verdict; firmware last_error=$($v3diag.last_error)" +
                       " result=$($v3diag.last_error_result) sub=$($v3diag.last_error_sub)"
        } elseif ($errors.Count -gt 0) {
            $verdict = "DECODER DISAGREES WITH FIRMWARE: trace shows " +
                       "$($errors.Count) error state(s) but v3diag reports errors=0. " +
                       'Treat the trace classification as suspect.'
        }
    }
} catch { }

$experiment = [ordered]@{
    tool        = 'nfc_lab'
    version     = 1
    scenario    = $Scenario
    timestamp   = (Get-Date).ToString('o')
    kind        = $kind
    hypothesis  = $Hypothesis
    variable    = $Variable
    expectation = $Expect
    action      = $Action
    port        = $Port
    baseline    = $Baseline
    git         = $git
    verdict     = $verdict
    firmware_errors = $firmwareErrors
    artifacts   = $artifacts
}
Save-Text (Join-Path $outputDir 'experiment.json') `
    ($experiment | ConvertTo-Json -Depth 6)

Write-Host ''
Write-Host "Verdict    : $verdict" -ForegroundColor Green
Write-Host "Artifacts  : $outputDir"
foreach ($artifact in $artifacts) {
    Write-Host ("  {0,-26} {1,8} B  {2}" -f $artifact.path, $artifact.bytes,
                $artifact.sha256.Substring(0, 16))
}
if ($imageBefore -and $imageAfter) {
    $changed = (Get-FileHash $imageBefore -Algorithm SHA256).Hash -ne
               (Get-FileHash $imageAfter -Algorithm SHA256).Hash
    Write-Host ("Tag image  : {0}" -f ($(if ($changed) { 'CHANGED' } else { 'unchanged' })))
}
Write-Host ''
Write-Host 'Next: classify the result as a golden fixture, a fail-closed fixture,' -ForegroundColor DarkGray
Write-Host 'or an entry in docs/experiments/refuted-hypotheses.md.' -ForegroundColor DarkGray
