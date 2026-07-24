#!/usr/bin/env pwsh
# Read PicoSwitch2's out-of-band UART0 diagnostic channel through a USB-to-TTL
# adapter while the Pico's USB-C port remains attached to the Switch.
# Use -OutputPath with trace dump to save validated JSONL for ns2_trace.py.
param(
    [string]$Port,
    [string]$Command = 'fwreads',
    [string]$OutputPath,
    [ValidateRange(100, 850)]
    [int]$CaptureMs = 600,
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
    if ($Command -eq 'pro2audio pcm dump') {
        if (-not $OutputPath) {
            throw 'pro2audio pcm dump requires -OutputPath ending in .wav'
        }
        $resolved = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
            [System.IO.Path]::GetFullPath($OutputPath)
        } else {
            [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputPath))
        }
        $parent = [System.IO.Path]::GetDirectoryName($resolved)
        if (-not [System.IO.Directory]::Exists($parent)) {
            throw "Output directory does not exist: $parent"
        }

        $serial.Write("pro2audio pcm status`n")
        $statusLine = $serial.ReadLine().Trim()
        $status = $statusLine | ConvertFrom-Json
        if ($status.pro2audio_pcm -ne 'status' -or -not $status.complete -or
            [int]$status.captured -le 0) {
            throw "PCM capture is not complete: $statusLine"
        }

        $pcm = [System.Collections.Generic.List[byte]]::new([int]$status.captured)
        $offset = 0
        while ($offset -lt [int]$status.captured) {
            $serial.Write("pro2audio pcm read $offset`n")
            $line = $serial.ReadLine().Trim()
            $record = $line | ConvertFrom-Json
            if ($record.pro2audio_pcm -ne 'data' -or
                [int]$record.offset -ne $offset -or
                $record.payload -notmatch '^[0-9A-F]*$' -or
                $record.payload.Length -ne ([int]$record.length * 2) -or
                [int]$record.total -ne [int]$status.captured) {
                throw "Invalid PCM record at offset ${offset}: $line"
            }
            for ($i = 0; $i -lt $record.payload.Length; $i += 2) {
                $pcm.Add([Convert]::ToByte($record.payload.Substring($i, 2), 16))
            }
            $offset += [int]$record.length
        }
        if ($pcm.Count -ne [int]$status.captured -or ($pcm.Count % 4) -ne 0) {
            throw "PCM length mismatch: received $($pcm.Count), expected $($status.captured)"
        }

        $stream = [System.IO.File]::Open(
            $resolved, [System.IO.FileMode]::Create,
            [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        $writer = [System.IO.BinaryWriter]::new($stream)
        try {
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF'))
            $writer.Write([uint32](36 + $pcm.Count))
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes('WAVE'))
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes('fmt '))
            $writer.Write([uint32]16)
            $writer.Write([uint16]1)       # PCM
            $writer.Write([uint16]2)       # stereo
            $writer.Write([uint32]48000)
            $writer.Write([uint32]192000)  # 48k * 2ch * 16-bit
            $writer.Write([uint16]4)
            $writer.Write([uint16]16)
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes('data'))
            $writer.Write([uint32]$pcm.Count)
            $writer.Write($pcm.ToArray())
        } finally {
            $writer.Dispose()
            $stream.Dispose()
        }
        Write-Host "Saved $($pcm.Count) PCM bytes to $resolved" -ForegroundColor Green
        Write-Output $statusLine
    } elseif ($Command -eq 'trace dump') {
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
    } elseif ($Command -in @('motionpair dump', 'motionpair capture')) {
        # Stop and drain one time-aligned genuine-Pro2/DualSense motion corpus.
        # Each native PDU is paired on-device with the latest DS5 sample using
        # the Pico's clock, avoiding UART timing as part of the measurement.
        # `motionpair capture` starts and stops through this already-open port,
        # avoiding process/open latency that can fill the 127-record ring before
        # a second script invocation gets a chance to stop it.
        if ($Command -eq 'motionpair capture') {
            $serial.Write("motionpair start`n")
            $startLine = $serial.ReadLine().Trim()
            $start = $startLine | ConvertFrom-Json
            if ($start.motionpair -ne 'started' -or -not $start.enabled) {
                throw "Could not start paired-motion capture: $startLine"
            }
            Start-Sleep -Milliseconds $CaptureMs
        }
        $serial.Write("motionpair dump`n")
        $manifestLine = $serial.ReadLine().Trim()
        $manifest = $manifestLine | ConvertFrom-Json
        if ($manifest.motionpair -ne 'dump' -or
            $manifest.PSObject.Properties.Name -notcontains 'count' -or
            $manifest.PSObject.Properties.Name -notcontains 'dropped') {
            throw "Invalid paired-motion manifest: $manifestLine"
        }

        $lines = [System.Collections.Generic.List[string]]::new()
        for ($index = 0; $index -lt [int]$manifest.count; $index++) {
            $serial.Write("motionpair read`n")
            $line = $serial.ReadLine().Trim()
            $parsed = $line | ConvertFrom-Json
            if ($parsed.motionpair -ne 'record') {
                throw "Expected paired-motion record $index, received: $line"
            }
            foreach ($name in @('t_us', 'native_len', 'native', 'ds5_valid',
                                'ds5_seq', 'ds5_t_us', 'ds5_age_us', 'ds5_sensor',
                                'cal_state', 'raw_g', 'raw_a', 'cal_g', 'cal_a')) {
                if ($parsed.PSObject.Properties.Name -notcontains $name) {
                    throw "Paired-motion record is incomplete (missing '$name'): $line"
                }
            }
            if ([int]$parsed.native_len -notin @(30, 40) -or
                $parsed.native -notmatch '^[0-9A-F]+$' -or
                $parsed.native.Length -ne ([int]$parsed.native_len * 2)) {
                throw "Paired-motion payload framing mismatch at record ${index}: $line"
            }
            foreach ($field in @('raw_g', 'raw_a', 'cal_g', 'cal_a')) {
                if (@($parsed.$field).Count -ne 3) {
                    throw "Paired-motion $field axis count mismatch at record ${index}: $line"
                }
            }
            $lines.Add($line)
        }
        $end = [ordered]@{
            motionpair = 'end'
            records = $lines.Count
            dropped = [uint64]$manifest.dropped
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
