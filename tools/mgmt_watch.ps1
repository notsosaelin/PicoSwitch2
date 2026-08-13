#!/usr/bin/env pwsh
# Continuous in-band-management / BLE coexistence monitor over the GP0/GP1 UART.
#
#   pwsh -File tools/mgmt_watch.ps1                 # auto-select the USB-UART port
#   pwsh -File tools/mgmt_watch.ps1 -Port COM11 -IntervalMs 2000
#
# Polls `btstate` on a cadence and logs timestamped JSONL to -OutputPath (default
# dumps/mgmt-watch-<stamp>.jsonl). On any state TRANSITION -- controller_connected,
# cble.client/advertising, scan_active, mgmt_enabled, or any increment in
# disc.ctrl / disc.hci / mgmt.disconnects -- it flags the change AND dumps the full
# btlife lifecycle ring, so the last-good/first-failing state is captured without a
# human reading values by hand. Also dumps the ring every -RingEverySec and once on
# exit. The adapter's USB-C stays on the Switch throughout; UART is the only link.
#
# This is the automated diagnostic harness for
# docs/experiments/in-band-mgmt-coexistence-failure-2026-08-12.md -- run it, let the
# session play out, and read the log. Ctrl-C (or -DurationSec) stops it cleanly.
param(
    [string]$Port,
    [string]$OutputPath,
    [ValidateRange(300, 10000)][int]$IntervalMs = 2000,
    [ValidateRange(5, 600)][int]$RingEverySec = 30,
    [int]$DurationSec = 0,
    [switch]$List
)
$ErrorActionPreference = 'Stop'

function Get-SerialDevices {
    try {
        @(Get-CimInstance Win32_PnPEntity |
          Where-Object { $_.Name -match '\(COM\d+\)' } |
          ForEach-Object {
              [pscustomobject]@{
                  Port  = [regex]::Match($_.Name, 'COM\d+').Value
                  Name  = $_.Name
                  PnpId = $_.PNPDeviceID
              }
          })
    } catch {
        @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object | ForEach-Object {
            [pscustomobject]@{ Port = $_; Name = $_; PnpId = '' }
        })
    }
}

$devices = @(Get-SerialDevices)
if ($List) { $devices | Sort-Object Port | Format-Table Port, Name, PnpId -AutoSize; exit 0 }

if (-not $Port) {
    $usbUart = @($devices | Where-Object {
        $_.PnpId -match 'VID_(0403|10C4|1A86|067B)' -or
        $_.Name  -match 'FTDI|FT232|CP210|Silicon Labs|CH340|CH341|PL2303|Prolific|USB Serial'
    })
    $candidates = @($devices | Where-Object { $_.PnpId -notmatch 'ACPI\\PNP050[01]' })
    if     ($usbUart.Count   -eq 1) { $Port = $usbUart[0].Port }
    elseif ($candidates.Count -eq 1) { $Port = $candidates[0].Port }
    else { throw "Could not select one UART adapter automatically. Use -Port COMx or -List." }
}
$Port = $Port.ToUpperInvariant()

if (-not $OutputPath) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputPath = Join-Path $PSScriptRoot "..\dumps\mgmt-watch-$stamp.jsonl"
}
$OutputPath = [System.IO.Path]::GetFullPath(
    ([System.IO.Path]::IsPathRooted($OutputPath) ? $OutputPath : (Join-Path (Get-Location) $OutputPath)))
$parent = [System.IO.Path]::GetDirectoryName($OutputPath)
if (-not [System.IO.Directory]::Exists($parent)) { [System.IO.Directory]::CreateDirectory($parent) | Out-Null }
$enc = [System.Text.UTF8Encoding]::new($false)

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.Handshake = 'None'; $serial.DtrEnable = $false; $serial.RtsEnable = $false
$serial.ReadTimeout = 2500; $serial.WriteTimeout = 2500; $serial.NewLine = "`n"; $serial.ReadBufferSize = 65536

function Log([string]$line) { [System.IO.File]::AppendAllText($OutputPath, "$line`n", $enc) }
function Cmd([string]$c) {
    try { $serial.DiscardInBuffer(); $serial.Write("$c`n"); return $serial.ReadLine().Trim() }
    catch { return "{`"error`":`"timeout`",`"cmd`":`"$c`"}" }
}
function Now { (Get-Date).ToString('o') }
function DumpRing([string]$why) {
    $st = Cmd 'btstate'
    Log "{`"host_ts`":`"$(Now)`",`"why`":`"$why`",`"resp`":$st}"
    $count = 0; try { $count = [int](($st | ConvertFrom-Json).events.count) } catch {}
    for ($i = 0; $i -lt $count; $i++) {
        $e = Cmd "btlife read $i"
        Log "{`"host_ts`":`"$(Now)`",`"why`":`"$why.ring`",`"i`":$i,`"resp`":$e}"
    }
    return $st
}

$serial.Open(); Start-Sleep -Milliseconds 150; $serial.DiscardInBuffer()
Write-Host "mgmt_watch on $Port -> $OutputPath (interval ${IntervalMs}ms, ring every ${RingEverySec}s). Ctrl-C to stop." -ForegroundColor Cyan
$prev = $null
$lastRing = [System.Diagnostics.Stopwatch]::StartNew()
$deadline = if ($DurationSec -gt 0) { (Get-Date).AddSeconds($DurationSec) } else { [datetime]::MaxValue }
try {
    DumpRing 'start' | Out-Null
    while ((Get-Date) -lt $deadline) {
        $line = Cmd 'btstate'
        Log "{`"host_ts`":`"$(Now)`",`"why`":`"poll`",`"resp`":$line}"
        $s = $null; try { $s = $line | ConvertFrom-Json } catch {}
        if ($s) {
            $flags = @()
            if ($prev) {
                if ($s.controller_connected -ne $prev.controller_connected) { $flags += "controller_connected:$($prev.controller_connected)->$($s.controller_connected)" }
                if ($s.cble.client        -ne $prev.cble.client)            { $flags += "cble.client:$($prev.cble.client)->$($s.cble.client)" }
                if ($s.cble.advertising   -ne $prev.cble.advertising)       { $flags += "cble.adv:$($prev.cble.advertising)->$($s.cble.advertising)" }
                if ($s.scan_active        -ne $prev.scan_active)            { $flags += "scan_active:$($prev.scan_active)->$($s.scan_active)" }
                if ($s.mgmt_enabled       -ne $prev.mgmt_enabled)           { $flags += "mgmt_enabled:$($prev.mgmt_enabled)->$($s.mgmt_enabled)" }
                if ([int]$s.disc.ctrl     -gt [int]$prev.disc.ctrl)         { $flags += "disc.ctrl+=$([int]$s.disc.ctrl-[int]$prev.disc.ctrl)(reason=$($s.disc.last_reason))" }
                if ([int]$s.disc.hci      -gt [int]$prev.disc.hci)          { $flags += "disc.hci+=$([int]$s.disc.hci-[int]$prev.disc.hci)(reason=$($s.disc.last_reason))" }
                if ([int]$s.mgmt.disconnects -gt [int]$prev.mgmt.disconnects) { $flags += "mgmt.disc+=$([int]$s.mgmt.disconnects-[int]$prev.mgmt.disconnects)" }
            }
            $tag = "ctrl=$($s.controller_connected) ble=$($s.ble_conns) scan=$($s.scan_active) adv=$($s.cble.advertising) client=$($s.cble.client) supp.mgmt=$($s.suppress.mgmt_armed) disc(c/h)=$($s.disc.ctrl)/$($s.disc.hci)"
            if ($flags.Count) {
                Write-Host "[$(Get-Date -Format HH:mm:ss)] *** $($flags -join ', ') | $tag" -ForegroundColor Yellow
                DumpRing ("transition:" + ($flags -join ';')) | Out-Null
                $lastRing.Restart()
            } else {
                Write-Host "[$(Get-Date -Format HH:mm:ss)] $tag"
            }
            $prev = $s
        }
        if ($lastRing.Elapsed.TotalSeconds -ge $RingEverySec) { DumpRing 'periodic' | Out-Null; $lastRing.Restart() }
        Start-Sleep -Milliseconds $IntervalMs
    }
} finally {
    DumpRing 'final' | Out-Null
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
    Write-Host "mgmt_watch stopped. Log: $OutputPath" -ForegroundColor Green
}
