#!/usr/bin/env pwsh
# Builds gcusb.exe with MinGW-w64 (gcc, MSYS2 ucrt64) against the Windows SetupAPI/WinUSB/HID
# APIs directly -- no libusb, no driver replacement. See PROMPT.md and this directory's own
# gcusb_win.c header comment for the full rationale.
#
#   ./build.ps1          # builds tools/gcusb/gcusb.exe
#
# Requires MSYS2 ucrt64 gcc on PATH (this machine has it at C:\msys64\ucrt64\bin).
param(
    [string]$GccPath = "C:\msys64\ucrt64\bin\gcc.exe"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

if (-not (Test-Path $GccPath)) {
    throw "gcc not found at $GccPath -- pass -GccPath or install MSYS2 ucrt64."
}

$out = Join-Path $root 'gcusb.exe'
& $GccPath -Wall -O2 `
    -I $root `
    -o $out `
    (Join-Path $root 'gcusb_win.c') `
    (Join-Path $root 'gcusb_core.c') `
    -lsetupapi -lwinusb -lhid -lole32
if ($LASTEXITCODE -ne 0) { throw "gcc build failed" }
Write-Host "OK -> $out" -ForegroundColor Green
