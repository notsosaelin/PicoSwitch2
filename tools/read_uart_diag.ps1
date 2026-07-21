#!/usr/bin/env pwsh
# Read PicoSwitch2's out-of-band UART0 diagnostic channel through a USB-to-TTL
# adapter while the Pico's USB-C port remains attached to the Switch.
param(
    [string]$Port,
    [string]$Command = 'fwreads',
    [ValidateRange(250, 30000)]
    [int]$TimeoutMs = 5000,
    [switch]$List
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($Command) -or $Command.Contains("`r") -or $Command.Contains("`n")) {
    throw 'Command must be one non-empty line.'
}

function Get-SerialDevices {
    try {
        @(Get-CimInstance Win32_PnPEntity |
          Where-Object { $_.Name -match '\(COM\d+\)' } |
          ForEach-Object {
              [pscustomobject]@{
                  Port = [regex]::Match($_.Name, 'COM\d+').Value
                  Name = $_.Name
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
if ($List) {
    if ($devices.Count -eq 0) {
        Write-Host 'No serial ports found.'
    } else {
        $devices | Sort-Object Port | Format-Table Port, Name, PnpId -AutoSize
    }
    exit 0
}

if (-not $Port) {
    $usbUart = @($devices | Where-Object {
        $_.PnpId -match 'VID_(0403|10C4|1A86|067B)' -or
        $_.Name -match 'FTDI|FT232|CP210|Silicon Labs|CH340|CH341|PL2303|Prolific|USB Serial'
    })
    if ($usbUart.Count -eq 1) {
        $Port = $usbUart[0].Port
    } elseif ($devices.Count -eq 1) {
        $Port = $devices[0].Port
    } else {
        $summary = if ($devices.Count) {
            ($devices | ForEach-Object { "$($_.Port) ($($_.Name))" }) -join ', '
        } else {
            'none'
        }
        throw "Could not select one UART adapter automatically. Available ports: $summary. Use -Port COMx or -List."
    }
}

$Port = $Port.ToUpperInvariant()
$serial = [System.IO.Ports.SerialPort]::new(
    $Port, 115200, [System.IO.Ports.Parity]::None, 8,
    [System.IO.Ports.StopBits]::One)
$serial.Handshake = [System.IO.Ports.Handshake]::None
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.ReadTimeout = $TimeoutMs
$serial.WriteTimeout = $TimeoutMs
$serial.NewLine = "`n"

try {
    $serial.Open()
    Start-Sleep -Milliseconds 100
    $serial.DiscardInBuffer()
    $serial.Write("$Command`n")
    $response = $serial.ReadLine().Trim()

    # Validate the framing before returning the original one-line JSON, which
    # is easiest to paste into an issue/session without PowerShell reformatting.
    $null = $response | ConvertFrom-Json
    Write-Host "UART $Port @ 115200: $Command" -ForegroundColor Cyan
    Write-Output $response
} catch [System.TimeoutException] {
    throw "Timed out waiting for PicoSwitch2 on $Port. Check crossed TX/RX, shared GND, 3.3 V signal level, and leave VCC disconnected."
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
