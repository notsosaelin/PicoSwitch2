#!/usr/bin/env pwsh
# Read PicoSwitch2's out-of-band UART0 diagnostic channel through a USB-to-TTL
# adapter while the Pico's USB-C port remains attached to the Switch.
# Use -OutputPath with trace dump to save validated JSONL for ns2_trace.py,
# or with amiibo dump to save the live mutable image without moving USB.
param(
    [string]$Port,
    [string]$Command = 'fwreads',
    [string]$OutputPath,
    [ValidateRange(100, 850)]
    [int]$CaptureMs = 600,
    [ValidateRange(250, 30000)]
    [int]$TimeoutMs = 5000,
    [switch]$NoAcknowledge,
    [switch]$List,
    # Print the port this script would select, then exit. Lets nfc_lab.ps1 and
    # any other orchestrator record the port without a second copy of the
    # adapter-selection heuristic.
    [switch]$ReportPort
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

function Get-Crc32([byte[]]$Data) {
    # Keep the arithmetic in int64 because PowerShell treats 0xFFFFFFFF as -1
    # and checked uint32 conversions reject intermediate high-bit values.
    $table = New-Object 'System.Int64[]' 256
    for ($i = 0; $i -lt 256; $i++) {
        [int64]$entry = $i
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($entry -band 1L) -ne 0L) {
                $entry = (0xEDB88320L -bxor ($entry -shr 1)) -band 0xFFFFFFFFL
            } else {
                $entry = ($entry -shr 1) -band 0xFFFFFFFFL
            }
        }
        $table[$i] = $entry
    }
    [int64]$crc = 0xFFFFFFFFL
    foreach ($value in $Data) {
        $index = [int](($crc -bxor [int64]$value) -band 0xFFL)
        $crc = ((($crc -shr 8) -band 0xFFFFFFL) -bxor $table[$index]) `
            -band 0xFFFFFFFFL
    }
    return ($crc -bxor 0xFFFFFFFFL) -band 0xFFFFFFFFL
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
    # A motherboard legacy port (ACPI\PNP0501) is never the USB-TTL bridge.
    # Excluding it stops a single-legacy-port machine from "auto-discovering"
    # a port that cannot answer, which reads as a dead board.
    $candidates = @($devices | Where-Object { $_.PnpId -notmatch 'ACPI\\PNP050[01]' })
    if ($usbUart.Count -eq 1) {
        $Port = $usbUart[0].Port
    } elseif ($candidates.Count -eq 1) {
        $Port = $candidates[0].Port
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
if ($ReportPort) {
    $selected = $devices | Where-Object { $_.Port -eq $Port } | Select-Object -First 1
    Write-Output ([pscustomobject]@{
        port = $Port
        name = if ($selected) { $selected.Name } else { '' }
        pnpId = if ($selected) { $selected.PnpId } else { '' }
    } | ConvertTo-Json -Compress)
    exit 0
}
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
    if ($Command -eq 'amiibo dump') {
        if (-not $OutputPath) {
            throw 'amiibo dump requires -OutputPath ending in .bin'
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

        $serial.Write("amiibo status`n")
        $statusLine = $serial.ReadLine().Trim()
        $status = $statusLine | ConvertFrom-Json
        if ($status.amiibo -ne 'status' -or
            (-not $status.loaded -and -not $status.v3loaded) -or
            [int]$status.size -notin @(540, 572, 2048)) {
            throw "No supported Virtual Amiibo is loaded: $statusLine"
        }

        $total = [int]$status.size
        $generation = [uint64]$status.generation
        $image = [System.Collections.Generic.List[byte]]::new($total)
        $offset = 0
        while ($offset -lt $total) {
            $serial.Write("amiibo read $offset`n")
            $line = $serial.ReadLine().Trim()
            $record = $line | ConvertFrom-Json
            if ($record.amiibo -ne 'data' -or
                [int]$record.offset -ne $offset -or
                [int]$record.total -ne $total -or
                [uint64]$record.generation -ne $generation -or
                [int]$record.length -le 0 -or
                $record.payload -notmatch '^[0-9A-F]+$' -or
                $record.payload.Length -ne ([int]$record.length * 2)) {
                throw "Invalid Virtual Amiibo record at offset ${offset}: $line"
            }
            for ($i = 0; $i -lt $record.payload.Length; $i += 2) {
                $image.Add([Convert]::ToByte($record.payload.Substring($i, 2), 16))
            }
            $offset += [int]$record.length
        }
        if ($image.Count -ne $total) {
            throw "Virtual Amiibo length mismatch: received $($image.Count), expected $total"
        }

        $bytes = $image.ToArray()
        if ($total -eq 2048) {
            if ($bytes[0] -ne 0x04 -or $bytes[7] -ne 0x00 -or
                $bytes[8] -ne 0x44) {
                throw 'Downloaded Virtual Amiibo failed NTAG I2C 2K identity validation.'
            }
            $uid = -join (0..6 | ForEach-Object { '{0:X2}' -f $bytes[$_] })
            if ($status.uid -and $uid -ne ([string]$status.uid).ToUpperInvariant()) {
                throw "Downloaded Virtual Amiibo UID mismatch: received $uid, expected $($status.uid)"
            }
        } else {
            $bcc0 = [byte](0x88 -bxor $bytes[0] -bxor $bytes[1] -bxor $bytes[2])
            $bcc1 = [byte]($bytes[4] -bxor $bytes[5] -bxor $bytes[6] -bxor $bytes[7])
            if ($bytes[3] -ne $bcc0 -or $bytes[8] -ne $bcc1) {
                throw 'Downloaded Virtual Amiibo failed NTAG215 UID/BCC validation.'
            }
        }
        $actualCrc = '{0:X8}' -f (Get-Crc32 $bytes)
        $expectedCrc = ([string]$status.payload_crc).ToUpperInvariant()
        if ($expectedCrc -match '^[0-9A-F]{8}$' -and
            $actualCrc -ne $expectedCrc) {
            throw "Downloaded Virtual Amiibo CRC mismatch: received $actualCrc, expected $expectedCrc"
        }
        [System.IO.File]::WriteAllBytes($resolved, $bytes)

        $ackLine = $null
        if (-not $NoAcknowledge) {
            # Match the production portal's Sync semantics: only clear dirty
            # protection after every byte has been validated and saved.
            $serial.Write("amiibo acknowledge`n")
            $ackLine = $serial.ReadLine().Trim()
            $ack = $ackLine | ConvertFrom-Json
            if ($ack.amiibo -ne 'acknowledged' -or $ack.dirty) {
                throw "Virtual Amiibo was saved but not acknowledged: $ackLine"
            }
        }
        $suffix = if ($NoAcknowledge) { ' (dirty state retained)' } else { '' }
        Write-Host "Saved $total Virtual Amiibo bytes to $resolved$suffix" -ForegroundColor Green
        Write-Output $statusLine
        if ($ackLine) { Write-Output $ackLine }
    } elseif ($Command -eq 'pro2audio pcm dump') {
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
        # If an output path is supplied, persist each validated record
        # immediately. A console reboot can power-cycle the Pico while pulling
        # a crash trace; incremental output preserves everything read before
        # that reset instead of losing the entire in-memory list.
        $traceOutput = $null
        $traceEncoding = [System.Text.UTF8Encoding]::new($false)
        if ($OutputPath) {
            $traceOutput = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
                [System.IO.Path]::GetFullPath($OutputPath)
            } else {
                [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputPath))
            }
            $parent = [System.IO.Path]::GetDirectoryName($traceOutput)
            if (-not [System.IO.Directory]::Exists($parent)) {
                throw "Output directory does not exist: $parent"
            }
            [System.IO.File]::WriteAllText($traceOutput, '', $traceEncoding)
        }
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
            if ($traceOutput) {
                [System.IO.File]::AppendAllText(
                    $traceOutput, "$line`n", $traceEncoding)
            }
        }
        $end = [ordered]@{
            trace = 'end'
            records = $lines.Count
            overwritten = [uint64]$manifest.overwritten
        } | ConvertTo-Json -Compress
        $lines.Add($end)
        if ($traceOutput) {
            [System.IO.File]::AppendAllText(
                $traceOutput, "$end`n", $traceEncoding)
        }
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
    } elseif ($Command -match '^motionhybrid capture(?: (genuine|accel|gyro|prefix|imu|all))?$') {
        # Run a bounded genuine-base/DualSense-donor fitment experiment over
        # this already-open UART. The firmware captures the exact genuine base
        # plus output XOR on-device; UART timing is not part of the sample
        # alignment. Always return to mode off before closing the port.
        $requestedMode = $Matches[1]
        $modeArmed = $false
        try {
            if ($requestedMode) {
                $serial.Write("motionhybrid mode $requestedMode`n")
                $modeLine = $serial.ReadLine().Trim()
                $modeReply = $modeLine | ConvertFrom-Json
                if ($modeReply.motionhybrid -ne 'mode_requested' -or
                    $modeReply.requested -ne $requestedMode) {
                    throw "Could not select hybrid mode ${requestedMode}: $modeLine"
                }
                $modeArmed = $true
            } else {
                $serial.Write("motionhybrid status`n")
                $modeLine = $serial.ReadLine().Trim()
                $modeReply = $modeLine | ConvertFrom-Json
                $requestedMode = [string]$modeReply.requested
                if ($modeReply.motionhybrid -ne 'status' -or
                    $requestedMode -notin @('genuine', 'accel', 'gyro',
                                            'prefix', 'imu', 'all')) {
                    throw ('Select a non-off mode first, or use ' +
                           'motionhybrid capture <mode>: ' + $modeLine)
                }
                $modeArmed = $true
            }

            $serial.Write("motionhybrid capture start`n")
            $startLine = $serial.ReadLine().Trim()
            $start = $startLine | ConvertFrom-Json
            if ($start.motionhybrid -ne 'capture_started' -or
                -not $start.capture) {
                throw "Could not start hybrid capture: $startLine"
            }
            Start-Sleep -Milliseconds $CaptureMs

            $serial.Write("motionhybrid capture dump`n")
            $manifestLine = $serial.ReadLine().Trim()
            $manifest = $manifestLine | ConvertFrom-Json
            if ($manifest.motionhybrid -ne 'dump' -or $manifest.capture -or
                $manifest.PSObject.Properties.Name -notcontains 'count' -or
                $manifest.PSObject.Properties.Name -notcontains 'dropped') {
                throw "Invalid hybrid capture manifest: $manifestLine"
            }

            $lines = [System.Collections.Generic.List[string]]::new()
            for ($index = 0; $index -lt [int]$manifest.count; $index++) {
                $serial.Write("motionhybrid capture read`n")
                $line = $serial.ReadLine().Trim()
                $parsed = $line | ConvertFrom-Json
                if ($parsed.motionhybrid -ne 'record') {
                    throw "Expected hybrid record $index, received: $line"
                }
                foreach ($name in @(
                        't_us', 'native_len', 'mode', 'reason',
                        'requested_groups', 'changed_bits', 'ds5_age_us',
                        'ds5_seq', 'cal_state', 'pose_aligned', 'base',
                        'output_xor')) {
                    if ($parsed.PSObject.Properties.Name -notcontains $name) {
                        throw "Hybrid record is incomplete (missing '$name'): $line"
                    }
                }
                if ([int]$parsed.native_len -notin @(30, 40) -or
                    $parsed.mode -notin @('genuine', 'accel', 'gyro',
                                          'prefix', 'imu', 'all') -or
                    $parsed.base -notmatch '^[0-9A-F]+$' -or
                    $parsed.output_xor -notmatch '^[0-9A-F]+$' -or
                    $parsed.base.Length -ne ([int]$parsed.native_len * 2) -or
                    $parsed.output_xor.Length -ne ([int]$parsed.native_len * 2)) {
                    throw "Hybrid payload framing mismatch at record ${index}: $line"
                }
                $lines.Add($line)
            }
            $end = [ordered]@{
                motionhybrid = 'end'
                records = $lines.Count
                dropped = [uint64]$manifest.dropped
                mode = $requestedMode
                status = $manifest
            } | ConvertTo-Json -Compress -Depth 5
            $lines.Add($end)
            Write-DiagnosticLines -Lines $lines.ToArray()
        } finally {
            if ($modeArmed -and $serial.IsOpen) {
                try {
                    $serial.Write("motionhybrid mode off`n")
                    $null = $serial.ReadLine()
                } catch {
                    Write-Warning 'Could not confirm motionhybrid mode off; power-cycle before ordinary use.'
                }
            }
        }
    } elseif ($Command -in @(
            'motionpair dump',
            'motionpair capture',
            'motionpair trigger capture')) {
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
        } elseif ($Command -eq 'motionpair trigger capture') {
            $serial.Write("motionpair trigger`n")
            $startLine = $serial.ReadLine().Trim()
            $start = $startLine | ConvertFrom-Json
            if ($start.motionpair -ne 'trigger_armed' -or
                -not $start.enabled -or -not $start.chart.armed) {
                throw "Could not arm chart-transition capture: $startLine"
            }

            $deadline = [System.Diagnostics.Stopwatch]::StartNew()
            do {
                Start-Sleep -Milliseconds 50
                $serial.Write("motionpair status`n")
                $statusLine = $serial.ReadLine().Trim()
                $status = $statusLine | ConvertFrom-Json
                if ($status.motionpair -ne 'status' -or
                    $status.PSObject.Properties.Name -notcontains 'chart') {
                    throw "Invalid chart-transition status: $statusLine"
                }
                if ($status.chart.complete) { break }
            } while ($deadline.ElapsedMilliseconds -lt $TimeoutMs)

            if (-not $status.chart.complete) {
                $serial.Write("motionpair stop`n")
                $null = $serial.ReadLine()
                throw (
                    "Chart transition did not complete within ${TimeoutMs} ms; " +
                    "last status: $statusLine")
            }
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
            chart = $manifest.chart
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
