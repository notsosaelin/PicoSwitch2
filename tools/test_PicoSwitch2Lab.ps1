#!/usr/bin/env pwsh
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$module = Join-Path $PSScriptRoot 'PicoSwitch2Lab.psm1'
Import-Module $module -Force

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "assertion failed: $Message" }
}

$temporary = Join-Path ([System.IO.Path]::GetTempPath()) (
    'picoswitch2-lab-test-' + [guid]::NewGuid().ToString('N'))
try {
    [System.IO.Directory]::CreateDirectory($temporary) | Out-Null

    $textPath = Join-Path $temporary 'nested\plain.txt'
    Set-Ps2LabText -Path $textPath -Lines @('alpha', 'βeta')
    $bytes = [System.IO.File]::ReadAllBytes($textPath)
    Assert-True ($bytes.Length -gt 3) 'UTF-8 file should not be empty'
    Assert-True (-not ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and
                      $bytes[2] -eq 0xBF)) 'UTF-8 output must not contain a BOM'

    $blankPath = Join-Path $temporary 'blank-lines.txt'
    Set-Ps2LabText -Path $blankPath -Lines @('alpha', '', 'beta')
    $blankLines = [System.IO.File]::ReadAllLines($blankPath)
    Assert-True ($blankLines.Count -eq 3) 'blank lines should be retained'
    Assert-True ($blankLines[1] -eq '') 'middle blank line should round-trip'

    $jsonPath = Join-Path $temporary 'value.json'
    Set-Ps2LabJson -Path $jsonPath -Value ([ordered]@{
        schema = 'test/v1'
        nested = [ordered]@{ value = 7 }
    })
    $parsed = Get-Content -Raw -LiteralPath $jsonPath | ConvertFrom-Json
    Assert-True ($parsed.schema -eq 'test/v1') 'JSON schema should round-trip'
    Assert-True ($parsed.nested.value -eq 7) 'nested JSON should round-trip'

    $digest = Get-Ps2LabDigest -Path $textPath
    Assert-True ($digest.bytes -eq $bytes.Length) 'digest length should match'
    Assert-True ($digest.sha256 -match '^[0-9A-F]{64}$') 'digest must be SHA-256'

    $run = New-Ps2LabDirectory -RepoRoot $temporary -OutputRoot 'runs' `
        -Scenario 'magnet north @ 50 mm' `
        -Timestamp ([datetime]'2026-07-29T12:34:56')
    Assert-True ((Split-Path -Leaf $run) -eq
        '20260729-123456-magnet-north---50-mm') 'run slug should be deterministic'

    $manifest = New-Ps2LabManifest -Tool 'test_lab' -Scenario 'self-test' `
        -Action 'none' -RepoRoot $PSScriptRoot -ArtifactPaths @($textPath)
    Assert-True ($manifest.schema -eq 'picoswitch2-lab/v1') `
        'manifest schema should be stable'
    Assert-True ($manifest.artifacts.Count -eq 1) `
        'manifest should hash existing artifacts'

    Write-Host 'PicoSwitch2Lab self-test passed.'
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}
