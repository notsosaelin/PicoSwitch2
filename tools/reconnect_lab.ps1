#!/usr/bin/env pwsh
# Controlled bonded-reconnect capture for a two-peer (keyboard + mouse) source.
#
#   pwsh -File tools/reconnect_lab.ps1
#   pwsh -File tools/reconnect_lab.ps1 -Port COM11 -PollSeconds 120
#
# WHY THIS EXISTS
#
# The open question is why a bonded BLE keyboard or mouse fails to auto-reconnect
# after being power-cycled while its partner stays connected. That question CANNOT
# be answered from a snapshot taken after the peers have been manually re-paired:
# re-pairing rewrites the bond table and the single `last_connected` reconnect
# target, destroying the state that existed during the failure. This harness
# captures labelled checkpoints around the power cycle so the before/after
# comparison is valid.
#
# PROTOCOL -- the script prompts at each step; do not pair anything during the run.
#
#   1 baseline    both peers connected and working
#   2 absent      exactly ONE peer powered OFF
#   3 returning   that same peer powered back ON, WITHOUT pairing
#   4 settled     ~60 s later, whether or not it rejoined
#
# At every checkpoint it records btbonds, btreconnect, btstate, kbm status and
# btdev. The decisive comparisons afterwards are:
#
#   btbonds across 1 -> 2 -> 3   did the absent peer's bond survive, or was it
#                                evicted/overwritten while it was away?
#   btreconnect.addr             which single peer owns the reconnect target
#                                slot, and is it the absent one or the survivor?
#   bonded_adv / nontarget_adv   is the returning peer seen advertising at all,
#                                and does its address match a stored bond?
#   rpa_adv                      is it advertising under a rotating private
#                                address that no raw bond compare can match?
#   state / scanning             was discovery even running while it was absent?
#   target_connects/success/fail was a connect ever attempted, and did it fail?
#
# Writes JSONL to -OutputPath (default dumps/reconnect-lab-<stamp>.jsonl).
param(
    [string]$Port,
    [string]$OutputPath,
    [ValidateRange(10, 600)][int]$PollSeconds = 60,
    [int]$TimeoutMs = 2500
)

$ErrorActionPreference = 'Stop'

$commands = @('btbonds', 'btreconnect', 'btstate', 'kbm status', 'btdev')

if (-not $OutputPath) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputPath = Join-Path 'dumps' "reconnect-lab-$stamp.jsonl"
}
$dir = Split-Path -Parent $OutputPath
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }

function Invoke-Checkpoint {
    param([string]$Label, [string]$Note)

    Write-Host ""
    Write-Host "=== checkpoint: $Label ===" -ForegroundColor Cyan
    if ($Note) { Write-Host $Note -ForegroundColor DarkGray }

    $args = @{ Command = $commands; TimeoutMs = $TimeoutMs }
    if ($Port) { $args['Port'] = $Port }
    $replies = & (Join-Path $PSScriptRoot 'uart_query.ps1') @args

    $ts = (Get-Date).ToString('o')
    foreach ($line in $replies) {
        if ($line -notmatch '^\[(?<cmd>[^\]]+)\]\s*(?<json>.*)$') { continue }
        $cmd = $Matches['cmd']; $json = $Matches['json']
        Write-Host "  $cmd -> $json"
        $record = [ordered]@{
            t          = $ts
            checkpoint = $Label
            command    = $cmd
            reply      = $json
        }
        ($record | ConvertTo-Json -Compress -Depth 4) | Add-Content -Path $OutputPath -Encoding utf8
    }
}

function Wait-ForOperator {
    param([string]$Prompt)
    Write-Host ""
    Write-Host $Prompt -ForegroundColor Yellow
    Read-Host "press Enter when done" | Out-Null
}

Write-Host "Bonded-reconnect capture -> $OutputPath" -ForegroundColor Green
Write-Host "Do NOT pair anything during this run. Re-pairing destroys the evidence." -ForegroundColor Yellow

Wait-ForOperator "Have BOTH the keyboard and the mouse connected and confirmed working."
Invoke-Checkpoint -Label 'baseline' -Note 'both peers present'

Wait-ForOperator "Power OFF exactly ONE peer. Leave the other ON. Do not pair anything."
Invoke-Checkpoint -Label 'absent' -Note 'one peer powered off'

Wait-ForOperator "Power that SAME peer back ON. Do NOT pair it -- it must reconnect on its own."
Invoke-Checkpoint -Label 'returning' -Note 'peer powered back on, no pairing'

Write-Host ""
Write-Host "Holding $PollSeconds s to let any reconnect attempt play out..." -ForegroundColor DarkGray
Start-Sleep -Seconds $PollSeconds
Invoke-Checkpoint -Label 'settled' -Note "after $PollSeconds s"

Write-Host ""
Write-Host "Capture complete -> $OutputPath" -ForegroundColor Green
Write-Host "Compare btbonds across baseline/absent/returning, and btreconnect.addr against the peer that went away."
