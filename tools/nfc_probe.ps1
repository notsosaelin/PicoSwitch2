#!/usr/bin/env pwsh
<#
.SYNOPSIS
Capture console-visible NFC page ranges from a genuine Pro Controller 2 over
UART, with no console attached.

.DESCRIPTION
The dongle's NFC mirror normally forwards whatever the console asks. Armed as an
initiator it instead accepts commands from UART, so the genuine controller
becomes an interrogatable oracle: arbitrary page ranges, at our own pace,
repeatable, and observable without a Switch 2 in the loop.

That makes it a useful page-range snapshot tool and the fastest way to answer
protocol questions that previously needed a full console capture per attempt.
For NTAG215, the default ranges contain the complete 540-byte image. For v3
NTAG I2C Plus 2K, the controller's current 8-bit range descriptor cannot address
sector 1, so the output is intentionally a partial packed snapshot rather than
a complete 2048-byte image.

Requires: dongle powered with UART0 reachable, a genuine Pro Controller 2 paired
over BT. A console may be attached but is not used.

.EXAMPLE
  # Capture the useful default ranges (complete NTAG215; partial v3)
  .\tools\nfc_probe.ps1 -Port COM11 -Dump out.bin

.EXAMPLE
  # Read one explicit page range
  .\tools\nfc_probe.ps1 -Port COM11 -Ranges '00-3B,3C-77,78-91,E2-E6'

.EXAMPLE
  # Send a single raw command and print the reply
  .\tools\nfc_probe.ps1 -Port COM11 -Raw '0191000500000000'
#>
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$Dump,
    [string]$Ranges,
    [string]$Raw,
    [int]$Baud = 115200,
    [int]$PollSeconds = 15,
    [switch]$KeepArmed
)

$ErrorActionPreference = 'Stop'

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 3000
$sp.WriteTimeout = 3000
$sp.NewLine = "`n"
$sp.Open()

function Send-Line([string]$line) {
    $sp.WriteLine($line)
    try { return $sp.ReadLine().Trim() } catch { return '<timeout>' }
}

# One NFC command to the genuine controller. The firmware is deliberately
# nonblocking -- it hands the command to BTstack and returns -- so the reply is
# collected by polling rather than by holding core0 through a BLE round trip.
function Invoke-Nfc([string]$hex, [int]$timeoutMs = 3000) {
    $r = Send-Line "nfcmirror send $hex"
    if ($r -notmatch '"ok":true') { throw "send rejected: $r" }
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $r = Send-Line 'nfcmirror reply'
        if ($r -match '"ready":true') {
            if ($r -match '"payload":"([0-9A-Fa-f]+)"') {
                return [byte[]]( -split ($Matches[1] -replace '..', '$& ') |
                    ForEach-Object { [Convert]::ToByte($_, 16) } )
            }
        }
        Start-Sleep -Milliseconds 15
    }
    throw "no reply within ${timeoutMs}ms for $hex"
}

function Format-Hex([byte[]]$b) { ($b | ForEach-Object { '{0:X2}' -f $_ }) -join '' }

# Read the operation buffer the controller staged for the last 0x06/0x21.
# Chunks are 70 bytes; byte 8 of each reply is a flags field whose bit 0 marks
# the final chunk, and bytes 9..10 are that chunk's length.
function Read-Buffer {
    $out = New-Object System.Collections.Generic.List[byte]
    $offset = 0
    for ($i = 0; $i -lt 64; $i++) {
        $lo = '{0:X2}' -f ($offset -band 0xFF)
        $hi = '{0:X2}' -f (($offset -shr 8) -band 0xFF)
        $reply = Invoke-Nfc "0191001500020000$lo$hi"
        if ($reply.Length -lt 11) { throw "short read-buffer reply" }
        $flags = $reply[8]
        $len = [int]$reply[9] -bor ([int]$reply[10] -shl 8)
        if ($len -le 0 -or (11 + $len) -gt $reply.Length) { throw "bad chunk length $len" }
        # PowerShell turns an indexed array slice into Object[] even when the
        # source is Byte[]. Generic List[byte].AddRange() rejects that runtime
        # type, so materialize a byte array explicitly before appending.
        [byte[]]$chunk = $reply[11..(10 + $len)]
        $out.AddRange($chunk)
        $offset += $len
        if (($flags -band 0x01) -ne 0) { break }
    }
    return , $out.ToArray()
}

try {
    Start-Sleep -Milliseconds 150
    $sp.DiscardInBuffer()

    $status = Send-Line 'nfcmirror initiator on'
    # Enabling initiator mode requests the BLE notification subscription, but
    # that transition is asynchronous when the mirror was previously off.
    # Poll its bounded status instead of requiring state ACTIVE in the first
    # command reply.
    $armDeadline = (Get-Date).AddSeconds(3)
    while ($status -notmatch '"state":2' -and (Get-Date) -lt $armDeadline) {
        Start-Sleep -Milliseconds 50
        $status = Send-Line 'nfcmirror status'
    }
    if ($status -notmatch '"state":2') {
        throw "bridge not subscribed to a genuine Pro Controller 2: $status"
    }
    Write-Host "initiator armed: $status"

    if ($Raw) {
        $reply = Invoke-Nfc $Raw
        Write-Host "reply ($($reply.Length) bytes): $(Format-Hex $reply)"
        return
    }

    Write-Host "waiting for a tag (up to ${PollSeconds}s)..."
    $uid = $null
    $deadline = (Get-Date).AddSeconds($PollSeconds)
    while ((Get-Date) -lt $deadline -and -not $uid) {
        # Discovery is bounded: the timeout field of 0x03 is 0x03E8 = 1000 ms,
        # after which the reader goes idle and every status reply says "no tag"
        # forever. A console re-issues stop/start continuously for exactly this
        # reason, so do the same rather than polling a dead reader.
        Invoke-Nfc '0191000400000000' | Out-Null
        # Payload is [0]=00, [1..2] timeout u16 LE, [3..4]=0x012C. A console's
        # FIRST poll of a session uses timeout 0 and only later ones use 1000 ms
        # (genuine capture seq 2 vs seq 8), so 0 is the open-ended discovery that
        # actually waits for a tag. Polling with 1000 ms alone found nothing.
        Invoke-Nfc '01910003000500000000002C01' | Out-Null

        $cycle = (Get-Date).AddMilliseconds(900)
        while ((Get-Date) -lt $cycle) {
            $s = Invoke-Nfc '0191000500000000'
            # Byte 8 is the reader state. 0x09 is "tag present" -- 0x04 only
            # appears once a 0x06 descriptor has started an operation, so
            # waiting for 0x04 here could never succeed. The UID follows the
            # 0x07 length marker at byte 16.
            if ($s.Length -ge 24 -and $s[8] -eq 0x09 -and $s[16] -eq 0x07) {
                $uid = $s[17..23]
                break
            }
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $uid) { throw "no tag detected" }
    Write-Host "tag UID: $(Format-Hex $uid)"

    # NTAG215 exposes pages 00-86. A v3 (NTAG I2C Plus 2K) also answers through
    # A1 plus E2-E6; extending the third block to A1 includes Air Riders'
    # directly observed sector-0 update at pages 92-99 and some surrounding
    # context. Try that useful v3 snapshot first, then the narrower historical
    # v3 range and finally NTAG215. This keeps the normal command short:
    # -Dump file.bin needs no manual -Ranges.
    $rangeSets = if ($Ranges) {
        @($Ranges)
    } else {
        @(
            '00-3B,3C-77,78-A1,E2-E6',
            '00-3B,3C-77,78-91,E2-E6',
            '00-3B,3C-77,78-86'
        )
    }

    $buffer = $null
    foreach ($set in $rangeSets) {
        $pairs = $set -split ',' | ForEach-Object {
            $a, $b = $_.Trim() -split '-'
            '{0:X2}{1:X2}' -f [Convert]::ToInt32($a, 16), [Convert]::ToInt32($b, 16)
        }
        $blocks = $pairs.Count
        $ranges = ($pairs -join '').PadRight(16, '0')
        $desc = '0191000600130000' + 'D007' + (Format-Hex $uid) + '01' +
                ('{0:X2}' -f $blocks) + $ranges
        try {
            Invoke-Nfc $desc | Out-Null
            Invoke-Nfc '0191000500000000' | Out-Null
            $buffer = Read-Buffer
            Write-Host "read $set -> $($buffer.Length) bytes"
            break
        } catch {
            Write-Host "range set '$set' failed: $($_.Exception.Message)"
        }
    }
    if (-not $buffer) { throw "no range set produced a buffer" }

    # The first 60 bytes describe the operation; tag content starts after it.
    $prefix = $buffer[0..59]
    $tag = $buffer[60..($buffer.Length - 1)]
    Write-Host "prefix[18]    = $('{0:X2}' -f $prefix[18])"
    Write-Host "prefix[19:51] = $(Format-Hex $prefix[19..50])"
    Write-Host "range data    = $($tag.Length) bytes"
    if ($prefix[18] -eq 0x06 -and $tag.Length -ne 2048) {
        Write-Warning (
            "v3 range snapshot only ($($tag.Length) bytes), not a full 2048-byte image; " +
            "sector 1 is not addressable by the current 8-bit range descriptor"
        )
    }

    if ($Dump) {
        # System.IO resolves a relative path against the process working
        # directory, which may be C:\Windows even when PowerShell's current
        # location is the repository. Resolve through PowerShell's provider so
        # "-Dump Kirby.bin" reliably writes beside the launched command.
        $dumpPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Dump)
        [System.IO.File]::WriteAllBytes($dumpPath, $tag)
        Write-Host "wrote $dumpPath"
        [System.IO.File]::WriteAllBytes("$dumpPath.prefix", $prefix)
        Write-Host "wrote $dumpPath.prefix"
    }

    Invoke-Nfc '0191000400000000' | Out-Null
}
finally {
    if ($sp.IsOpen) {
        if (-not $KeepArmed) { Send-Line 'nfcmirror initiator off' | Out-Null }
        $sp.Close()
    }
    $sp.Dispose()
}
