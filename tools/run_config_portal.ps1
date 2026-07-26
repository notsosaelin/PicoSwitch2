param(
    [ValidateRange(1024, 65535)]
    [int]$Port = 8765
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$webRoot = Join-Path $repoRoot "web"
$portalUrl = "http://127.0.0.1:$Port/index.html"

$python = Get-Command python -ErrorAction Stop
$server = Start-Process -FilePath $python.Source `
    -ArgumentList @("-m", "http.server", $Port, "--bind", "127.0.0.1") `
    -WorkingDirectory $webRoot `
    -WindowStyle Hidden `
    -PassThru

try {
    Start-Sleep -Milliseconds 500
    if ($server.HasExited) {
        throw "The local portal server could not start. Port $Port may already be in use."
    }
    Start-Process $portalUrl
    Write-Host "PicoSwitch2 configuration portal: $portalUrl"
    Write-Host "Enter Config mode, then connect over USB Serial or Bluetooth in Chrome/Edge."
    Write-Host "Press Enter here to stop the local server."
    Read-Host | Out-Null
}
finally {
    if (-not $server.HasExited) {
        Stop-Process -Id $server.Id
    }
}
