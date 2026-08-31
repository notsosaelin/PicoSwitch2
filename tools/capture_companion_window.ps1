<#
.SYNOPSIS
    Screenshot the Windows companion's own window, for UI acceptance.

.DESCRIPTION
    Visual acceptance is part of finishing a UI change in this project, and
    reasoning from XAML dimensions has twice produced a layout that compiled,
    measured correctly and looked wrong. This grabs what is actually on screen.

    WHY PrintWindow AND NOT A SCREEN GRAB. Copying from the screen needs the
    target window in front, and SetForegroundWindow is restricted for a process
    that does not own the foreground: the call silently fails and the capture
    quietly contains whatever window WAS in front. That has happened here, and a
    screenshot of the wrong application is worse than no screenshot, because it
    looks like evidence. PrintWindow asks the window to render itself and works
    while it is partly covered or behind another app.

    PW_RENDERFULLCONTENT (2) is required: without it a WinUI 3 window renders
    blank, because its content is composed rather than drawn into the DC.

    The window renders at PHYSICAL pixels, so on a 4K display a nominally
    1600-point window arrives ~3400 wide. -MaxWidth downscales for review.

.PARAMETER ProcessName
    Defaults to the companion.

.PARAMETER Out
    Destination PNG.

.PARAMETER MaxWidth
    Longest edge after downscaling. 0 keeps the native size.

.EXAMPLE
    ./tools/capture_companion_window.ps1 -Out shots/amiibo.png
#>
[CmdletBinding()]
param(
    [string]$ProcessName = 'PicoSwitch.Companion.App',
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$MaxWidth = 1500
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class WindowCapture
{
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

$process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } |
    Select-Object -First 1

if (-not $process) {
    throw "$ProcessName is not running with a visible window."
}

$handle = $process.MainWindowHandle
$rect = New-Object WindowCapture+RECT
if (-not [WindowCapture]::GetWindowRect($handle, [ref]$rect)) {
    throw 'GetWindowRect failed.'
}

$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) {
    throw "Window has no size ($width x $height); it may be minimised."
}

$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$dc = $graphics.GetHdc()
try {
    # 2 = PW_RENDERFULLCONTENT. Without it a WinUI 3 window comes back blank.
    $ok = [WindowCapture]::PrintWindow($handle, $dc, 2)
} finally {
    $graphics.ReleaseHdc($dc)
    $graphics.Dispose()
}

if (-not $ok) {
    $bitmap.Dispose()
    throw 'PrintWindow failed.'
}

$final = $bitmap
if ($MaxWidth -gt 0 -and $width -gt $MaxWidth) {
    $scale = $MaxWidth / $width
    $final = New-Object System.Drawing.Bitmap $MaxWidth, ([int]($height * $scale))
    $scaler = [System.Drawing.Graphics]::FromImage($final)
    $scaler.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $scaler.DrawImage($bitmap, 0, 0, $final.Width, $final.Height)
    $scaler.Dispose()
    $bitmap.Dispose()
}

$directory = Split-Path -Parent $Out
if ($directory -and -not (Test-Path $directory)) {
    New-Item -ItemType Directory -Force $directory | Out-Null
}

$final.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$final.Dispose()

Write-Output "Captured $($process.ProcessName) ($width x $height physical) to $Out"
