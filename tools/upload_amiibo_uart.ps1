#!/usr/bin/env pwsh
# Upload an amiibo image to the dongle over UART0, bypassing the web portal and
# the BLE config bridge entirely.
#
# Exists because the portal's Bluetooth upload is the only way to load a tag
# while the dongle stays attached to the console, and when that path is broken
# there is otherwise no way to exercise the serve path at all. Uses the same
# begin/chunk/commit/persist store calls the portal does.
#
#   .\tools\upload_amiibo_uart.ps1 -Port COM11 -Path "some amiibo.bin"
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [Parameter(Mandatory = $true)][string]$Path,
    # The UART parser accepts 127 payload characters per line. A 64-byte
    # hexadecimal chunk exceeds that once the command/offset are included;
    # 32 bytes matches the production portal and remains safely bounded.
    [int]$ChunkSize = 32,
    [int]$Baud = 115200
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Path)) { throw "No such file: $Path" }
$bytes = [System.IO.File]::ReadAllBytes($Path)
if ($bytes.Length -notin @(540, 572, 2048)) {
    throw "Unsupported image size $($bytes.Length); expected 540, 572 or 2048"
}

# CRC-32 (same polynomial the firmware and portal use).
# PowerShell's -bxor promotes operands in ways that overflow a uint32, so this
# works in int64 throughout and masks explicitly at every step.
function Get-Crc32([byte[]]$data) {
    $table = New-Object 'System.Int64[]' 256
    for ($i = 0; $i -lt 256; $i++) {
        [int64]$c = $i
        for ($k = 0; $k -lt 8; $k++) {
            if (($c -band 1L) -ne 0L) {
                $c = (0xEDB88320L -bxor ($c -shr 1)) -band 0xFFFFFFFFL
            } else {
                $c = ($c -shr 1) -band 0xFFFFFFFFL
            }
        }
        $table[$i] = $c
    }
    [int64]$crc = 0xFFFFFFFFL
    foreach ($b in $data) {
        $idx = [int](($crc -bxor [int64]$b) -band 0xFFL)
        $crc = ((($crc -shr 8) -band 0xFFFFFFL) -bxor $table[$idx]) -band 0xFFFFFFFFL
    }
    return ($crc -bxor 0xFFFFFFFFL) -band 0xFFFFFFFFL
}

$crc = '{0:x8}' -f (Get-Crc32 $bytes)
Write-Host "$([System.IO.Path]::GetFileName($Path)): $($bytes.Length) bytes, crc32=$crc"

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 4000
$sp.WriteTimeout = 4000
$sp.NewLine = "`n"
$sp.Open()
try {
    Start-Sleep -Milliseconds 150
    $sp.DiscardInBuffer()

    function Send-Line([string]$line) {
        $sp.WriteLine($line)
        try { return $sp.ReadLine().Trim() } catch { return '<timeout>' }
    }

    $r = Send-Line "amiibo begin $($bytes.Length) $crc"
    if ($r -notmatch '"ok"') { throw "begin failed: $r" }

    $sent = 0
    for ($off = 0; $off -lt $bytes.Length; $off += $ChunkSize) {
        $n = [Math]::Min($ChunkSize, $bytes.Length - $off)
        $hex = -join (0..($n - 1) | ForEach-Object { '{0:x2}' -f $bytes[$off + $_] })
        $r = Send-Line "amiibo chunk $off $hex"
        if ($r -notmatch '"ok"') {
            Send-Line "amiibo cancel" | Out-Null
            throw "chunk at $off failed: $r"
        }
        $sent += $n
        if (($off / $ChunkSize) % 8 -eq 0) {
            Write-Host -NoNewline "`r  $sent / $($bytes.Length) bytes"
        }
    }
    Write-Host "`r  $sent / $($bytes.Length) bytes"

    $r = Send-Line "amiibo commit"
    if ($r -notmatch '"ok"') { throw "commit failed: $r" }
    # Without this the image is RAM-only and does not survive a power blip.
    Send-Line "amiibo persist" | Out-Null
    Start-Sleep -Milliseconds 600
    Write-Host "status: $(Send-Line 'amiibo status')"
    Write-Host "upload complete."
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}
