#!/usr/bin/env pwsh
# Watch the adapter's UART for the keyboard-input trace.
#
#   pwsh -File tools/watch_kbm_trace.ps1
#   pwsh -File tools/watch_kbm_trace.ps1 -Port COM11
#   pwsh -File tools/watch_kbm_trace.ps1 -All        # every line, not just the trace
#
# Press the mapped key once while this runs. [KBM_TRACE] lines are highlighted
# and every line is written to dumps/kbm-trace-<stamp>.log regardless of filter,
# so nothing is lost if the interesting line is not the one expected.
#
# Reading the output:
#
#   (no "report" line)              the HID report never reaches the keyboard driver
#   report ... decode=fail          it arrives but does not decode as a keyboard report
#   report ... decode=ok usage=N    it decoded; look at the admit line next
#   admit: accepted                 it went all the way through
#   admit: not-active-source        the KB/M composite does not own the console
#   admit: no-keyboard-role         the peer does not hold the keyboard slot
#   admit: reject-mode              the role policy refused it
#   admit: classification-pending   still waiting on the descriptor
#   admit: no-peer-key              the peer could not be identified
#
# Ctrl-C to stop.
param(
    [string]$Port,
    [switch]$All,
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
    if     ($usbUart.Count    -eq 1) { $Port = $usbUart[0].Port }
    elseif ($candidates.Count -eq 1) { $Port = $candidates[0].Port }
    else { throw "Could not select one UART adapter automatically. Use -Port COMx or -List." }
}
$Port = $Port.ToUpperInvariant()

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\dumps\kbm-trace-$stamp.log"))
$parent = [System.IO.Path]::GetDirectoryName($logPath)
if (-not [System.IO.Directory]::Exists($parent)) { [System.IO.Directory]::CreateDirectory($parent) | Out-Null }

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.Handshake = 'None'; $serial.DtrEnable = $false; $serial.RtsEnable = $false
$serial.ReadTimeout = 400; $serial.NewLine = "`n"; $serial.ReadBufferSize = 262144

$serial.Open(); Start-Sleep -Milliseconds 200; $serial.DiscardInBuffer()

Write-Host ""
Write-Host "Watching $Port for [KBM_TRACE]. Log: $logPath" -ForegroundColor Cyan
Write-Host "Press the mapped key once now. Ctrl-C to stop." -ForegroundColor Yellow
Write-Host ""

$enc = [System.Text.UTF8Encoding]::new($false)
try {
    while ($true) {
        try { $line = $serial.ReadLine().TrimEnd() } catch { continue }
        if (-not $line) { continue }

        $stampedLine = "{0:HH:mm:ss.fff}  {1}" -f (Get-Date), $line
        [System.IO.File]::AppendAllText($logPath, "$stampedLine`n", $enc)

        if ($line -match '\[KBM_TRACE\]') {
            Write-Host $stampedLine -ForegroundColor Green
        } elseif ($All) {
            Write-Host $stampedLine -ForegroundColor DarkGray
        }
    }
}
finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
    Write-Host ""
    Write-Host "Saved: $logPath" -ForegroundColor Cyan
}
