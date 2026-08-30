#!/usr/bin/env pwsh
# Capture how the adapter CLASSIFIES a keyboard or mouse as it pairs.
#
#   pwsh -File tools/capture_kbm_pairing.ps1                    # auto-select the UART
#   pwsh -File tools/capture_kbm_pairing.ps1 -Port COM11 -Label "8bitdo-retro"
#   pwsh -File tools/capture_kbm_pairing.ps1 -List              # show serial ports
#
# The question this answers
# -------------------------
# A BLE peer that declares BOTH a keyboard collection and a pointer collection is
# classified as a MOUSE, because COMBO requires a Class-of-Device statement and
# BLE has none, so capability precedence decides and pointer wins. An 8BitDo Retro
# keyboard therefore arrives as a mouse and its keys never reach the keyboard
# role. See docs/experiments/ble-keyboard-classified-as-mouse-2026-08-29.md.
#
# The rule exists to stop a gaming mouse with macro keys from taking the keyboard
# role, so it cannot simply be inverted. What is needed is a discriminator, and
# that needs the actual REPORT DESCRIPTORS of both kinds of device.
#
# What this captures
# ------------------
#   * every UART line during pairing, timestamped -- including
#     "[BTHID] Descriptor reclassify: ...", "[BTHID_KEYBOARD] Parsed report ...",
#     and "[BTHID_MOUSE] Parsed report ...";
#   * `btid desc`  -- the RAW report descriptor bytes, which is the evidence that
#     actually settles the design question;
#   * `btid dump`  -- the identity / driver-binding event log;
#   * `kbm status` -- what the adapter ended up calling the device.
#
# It then runs tools/analyze_hid_descriptor.py over the captured bytes, so the
# answer is in the report rather than in a hex string someone has to decode.
#
# Run it once per device. Capture a real keyboard AND, if one is to hand, a mouse
# with macro keys -- the discriminator has to separate those two, so one sample
# cannot establish it.
param(
    [string]$Port,
    [string]$Label,
    [string]$OutputPath,
    [ValidateRange(10, 600)][int]$PairWindowSec = 90,
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
if (-not $Label) { $Label = 'device' }
$safeLabel = ($Label -replace '[^A-Za-z0-9._-]', '-')
if (-not $OutputPath) {
    $OutputPath = Join-Path $PSScriptRoot "..\dumps\kbm-pairing-$safeLabel-$stamp.jsonl"
}
$OutputPath = [System.IO.Path]::GetFullPath(
    ([System.IO.Path]::IsPathRooted($OutputPath) ? $OutputPath : (Join-Path (Get-Location) $OutputPath)))
$parent = [System.IO.Path]::GetDirectoryName($OutputPath)
if (-not [System.IO.Directory]::Exists($parent)) { [System.IO.Directory]::CreateDirectory($parent) | Out-Null }
$reportPath = [System.IO.Path]::ChangeExtension($OutputPath, '.md')
$enc = [System.Text.UTF8Encoding]::new($false)

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.Handshake = 'None'; $serial.DtrEnable = $false; $serial.RtsEnable = $false
$serial.ReadTimeout = 400; $serial.WriteTimeout = 2500; $serial.NewLine = "`n"; $serial.ReadBufferSize = 262144

function Now { (Get-Date).ToString('o') }
function Log([string]$line) { [System.IO.File]::AppendAllText($OutputPath, "$line`n", $enc) }
function LogEvent([string]$why, [string]$line) {
    Log (([ordered]@{ host_ts = (Now); why = $why; line = $line } | ConvertTo-Json -Compress))
}

# Interesting lines are echoed to the console as they arrive, so the operator can
# see the classification happen instead of finding out afterwards.
$Notable = '\[BTHID\]|\[BTHID_KEYBOARD\]|\[BTHID_MOUSE\]|reclassify|Parsed report|bonded|paired|KBM|kbm'

function Drain([string]$why) {
    while ($true) {
        try { $line = $serial.ReadLine().Trim() } catch { break }
        if (-not $line) { continue }
        LogEvent $why $line
        if ($line -match $Notable) { Write-Host "  $line" -ForegroundColor DarkGray }
    }
}

# A command whose one-line JSON reply we want, without discarding the printf
# traffic that arrives alongside it -- that traffic IS the evidence here.
function Cmd([string]$c, [int]$timeoutMs = 3000) {
    $serial.Write("$c`n")
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $line = $serial.ReadLine().Trim() } catch { continue }
        if (-not $line) { continue }
        if ($line -match '^\s*\{') { return $line }
        LogEvent 'uart.unsolicited' $line
        if ($line -match $Notable) { Write-Host "  $line" -ForegroundColor DarkGray }
    }
    return "{`"error`":`"timeout`",`"cmd`":`"$c`"}"
}

$serial.Open(); Start-Sleep -Milliseconds 200; $serial.DiscardInBuffer()

Write-Host ""
Write-Host "KB/M pairing capture on $Port" -ForegroundColor Cyan
Write-Host "  device label : $Label"
Write-Host "  raw log      : $OutputPath"
Write-Host "  report       : $reportPath"
Write-Host ""

try {
    Log (([ordered]@{
        host_ts = (Now); why = 'session.start'; label = $Label; port = $Port
    } | ConvertTo-Json -Compress))

    # A clean identity ring, so the log holds THIS pairing and not the last one.
    LogEvent 'cmd.btid.clear' (Cmd 'btid clear')
    LogEvent 'baseline.kbm' (Cmd 'kbm status')
    LogEvent 'baseline.bt' (Cmd 'btstate')

    Write-Host "1. Put the adapter in controller-pairing mode (double-tap its pairing button)." -ForegroundColor Yellow
    Write-Host "2. Put the keyboard (or mouse) into pairing mode." -ForegroundColor Yellow
    Write-Host "3. Wait for it to connect. Press a few keys once it has." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Capturing for up to $PairWindowSec s. Press Enter as soon as it has connected." -ForegroundColor Cyan
    Write-Host ""

    $deadline = (Get-Date).AddSeconds($PairWindowSec)
    while ((Get-Date) -lt $deadline) {
        Drain 'uart.pairing'
        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq 'Enter') { break }
        }
        Start-Sleep -Milliseconds 50
    }

    Drain 'uart.pairing'
    Write-Host ""
    Write-Host "Reading the descriptor and classification..." -ForegroundColor Cyan

    # The raw report descriptor. This is the evidence the design question needs:
    # everything else describes what the adapter DECIDED, and only this says what
    # the device actually declared.
    $desc = Cmd 'btid desc' 6000
    LogEvent 'cmd.btid.desc' $desc

    $identity = Cmd 'btid dump' 6000
    LogEvent 'cmd.btid.dump' $identity

    $kbm = Cmd 'kbm status' 4000
    LogEvent 'cmd.kbm.status' $kbm

    $bt = Cmd 'btstate' 4000
    LogEvent 'cmd.btstate' $bt

    Drain 'uart.tail'
    Log (([ordered]@{ host_ts = (Now); why = 'session.end' } | ConvertTo-Json -Compress))
}
finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}

# ---------------------------------------------------------------- the report

$python = @('python', 'py', 'python3') |
    ForEach-Object { Get-Command $_ -ErrorAction SilentlyContinue } |
    Select-Object -First 1

$analyzer = Join-Path $PSScriptRoot 'analyze_hid_descriptor.py'
if ($python -and (Test-Path $analyzer)) {
    Write-Host "Analysing the descriptor..." -ForegroundColor Cyan
    & $python.Source $analyzer $OutputPath --label $Label --out $reportPath
    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "Report written: $reportPath" -ForegroundColor Green
        Write-Host "Send that file -- it holds the descriptor, the parsed collections and the verdict." -ForegroundColor Green
    } else {
        Write-Host "Analysis failed; the raw log is still complete at $OutputPath" -ForegroundColor Yellow
    }
} else {
    Write-Host "Python not found; skipping analysis. Send the raw log: $OutputPath" -ForegroundColor Yellow
}
