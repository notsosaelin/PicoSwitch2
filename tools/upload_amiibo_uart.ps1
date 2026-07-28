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
    [int]$ChunkSize = 64,
    [int]$Baud = 115200
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Path)) { throw "No such file: $Path" }
$bytes = [System.IO.File]::ReadAllBytes($Path)
if ($bytes.Length -notin @(540, 572, 2048)) {
    throw "Unsupported image size $($bytes.Length); expected 540, 572 or 2048"
}

# CRC-32 (same polynomial the firmware and portal use).
function Get-Crc32([byte[]]$data) {
    $table = New-Object uint32[] 256
    for ($i = 0; $i -lt 256; $i++) {
        $c = [uint32]$i
        for ($k = 0; $k -lt 8; $k++) {
            if ($c -band 1) { $c = [uint32](0xEDB88320 -bxor ($c -shr 1)) }
            else { $c = [uint32]($c -shr 1) }
        }
        $table[$i] = $c
    }
    $crc = [uint32]0xFFFFFFFF
    foreach ($b in $data) {
        $crc = [uint32]($table[($crc -bxor $b) -band 0xFF] -bxor ($crc -shr 8))
    }
    return [uint32]($crc -bxor 0xFFFFFFFF)
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
