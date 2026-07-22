#!/usr/bin/env pwsh
# Read PicoSwitch2's out-of-band UART0 diagnostic channel through a USB-to-TTL
# adapter while the Pico's USB-C port remains attached to the Switch.
# Use -OutputPath with trace dump to save validated JSONL for ns2_trace.py.
param(
    [string]$Port,
    [string]$Command = 'fwreads',
    [string]$OutputPath,
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

function Write-DiagnosticLines {
    param([string[]]$Lines)

    if ($OutputPath) {
        $resolved = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
            [System.IO.Path]::GetFullPath($OutputPath)
        } else {
            [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputPath))
        }
        $parent = [System.IO.Path]::GetDirectoryName($resolved)
        if (-not [System.IO.Directory]::Exists($parent)) {
            throw "Output directory does not exist: $parent"
        }
        [System.IO.File]::WriteAllLines(
            $resolved, $Lines, [System.Text.UTF8Encoding]::new($false))
        Write-Host "Saved $($Lines.Count) JSON line(s) to $resolved" -ForegroundColor Green
    }
    Write-Output $Lines
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
$serial.ReadBufferSize = 65536
$serial.ReadTimeout = $TimeoutMs
$serial.WriteTimeout = $TimeoutMs
$serial.NewLine = "`n"

try {
    $serial.Open()
    Start-Sleep -Milliseconds 100
    $serial.DiscardInBuffer()
    Write-Host "UART $Port @ 115200: $Command" -ForegroundColor Cyan
    if ($Command -eq 'trace dump') {
        # Pull one retained record per request. This keeps the Pico, USB-UART
        # bridge, SerialPort receive buffer, JSON parser, and console output in
        # lockstep instead of relying on sustained unsolicited transmission.
        $serial.Write("trace dump`n")
        $manifestLine = $serial.ReadLine().Trim()
        $manifest = $manifestLine | ConvertFrom-Json
        if ($manifest.trace -ne 'dump' -or
            $manifest.PSObject.Properties.Name -notcontains 'count' -or
            $manifest.PSObject.Properties.Name -notcontains 'overwritten') {
            throw "Invalid trace manifest: $manifestLine"
        }

        $lines = [System.Collections.Generic.List[string]]::new()
        $previousSequence = $null
        for ($index = 0; $index -lt [int]$manifest.count; $index++) {
            $serial.Write("trace read $index`n")
            $line = $serial.ReadLine().Trim()
            $parsed = $line | ConvertFrom-Json
            if ($parsed.trace -ne 'record') {
                throw "Expected trace record $index, received: $line"
            }
            $required = @('seq', 't_us', 'personality', 'kind', 'dir',
                          'id', 'sub', 'length', 'captured', 'payload')
            foreach ($name in $required) {
                if ($parsed.PSObject.Properties.Name -notcontains $name) {
                    throw "Trace record is incomplete (missing '$name'): $line"
                }
            }
            if ($null -ne $previousSequence -and
                [uint64]$parsed.seq -ne ([uint64]$previousSequence + 1)) {
                throw "Trace sequence discontinuity after ${previousSequence}: $line"
            }
            if ($parsed.personality -notin @('pro2', 'gc', 'joycon_l', 'joycon_r', 'config') -or
                $parsed.kind -notin @('ep0_setup', 'ep0_response', 'bulk_command',
                                      'bulk_response', 'hid_output') -or
                $parsed.dir -notin @('console_to_device', 'device_to_console')) {
                throw "Trace enum framing mismatch at sequence $($parsed.seq): $line"
            }
            if ($parsed.payload -notmatch '^[0-9A-F]*$' -or
                $parsed.payload.Length -ne ([int]$parsed.captured * 2)) {
                throw "Trace payload framing mismatch at sequence $($parsed.seq): $line"
            }
            $previousSequence = [uint64]$parsed.seq
            $lines.Add($line)
        }
        $end = [ordered]@{
            trace = 'end'
            records = $lines.Count
            overwritten = [uint64]$manifest.overwritten
        } | ConvertTo-Json -Compress
        $lines.Add($end)
        Write-DiagnosticLines -Lines $lines.ToArray()
    } elseif ($Command -eq 'blecap dump') {
        # Stop capture, snapshot its retained count, then pull exactly that many
        # records. BLE and console USB remain live throughout; UART is the only
        # transport used for this diagnostic data.
        $serial.Write("blecap dump`n")
        $manifestLine = $serial.ReadLine().Trim()
        $manifest = $manifestLine | ConvertFrom-Json
        if ($manifest.blecap -ne 'dump' -or
            $manifest.PSObject.Properties.Name -notcontains 'count' -or
            $manifest.PSObject.Properties.Name -notcontains 'dropped') {
            throw "Invalid BLE capture manifest: $manifestLine"
        }

        $lines = [System.Collections.Generic.List[string]]::new()
        for ($index = 0; $index -lt [int]$manifest.count; $index++) {
            $serial.Write("blecap read`n")
            $line = $serial.ReadLine().Trim()
            $parsed = $line | ConvertFrom-Json
            if ($parsed.blecap -ne 'record') {
                throw "Expected BLE capture record $index, received: $line"
            }
            foreach ($name in @('t_us', 'kind', 'handle', 'length', 'captured', 'payload')) {
                if ($parsed.PSObject.Properties.Name -notcontains $name) {
                    throw "BLE capture record is incomplete (missing '$name'): $line"
                }
            }
            if ($parsed.payload -notmatch '^[0-9A-F]*$' -or
                $parsed.payload.Length -ne ([int]$parsed.captured * 2)) {
                throw "BLE capture payload framing mismatch at record ${index}: $line"
            }
            $lines.Add($line)
        }
        $end = [ordered]@{
            blecap = 'end'
            records = $lines.Count
            dropped = [uint64]$manifest.dropped
            variant = [int]$manifest.variant
        } | ConvertTo-Json -Compress
        $lines.Add($end)
        Write-DiagnosticLines -Lines $lines.ToArray()
    } else {
        $serial.Write("$Command`n")
        $response = $serial.ReadLine().Trim()
        # Validate the framing before returning the original one-line JSON,
        # which is easiest to paste into an issue/session without reformatting.
        $null = $response | ConvertFrom-Json
        Write-DiagnosticLines -Lines @($response)
    }
} catch [System.TimeoutException] {
    throw "Timed out waiting for PicoSwitch2 on $Port. Check crossed TX/RX, shared GND, 3.3 V signal level, and leave VCC disconnected."
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
