param(
    [ValidateRange(1024, 65535)]
    [int]$Port = 8765
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$webRoot = Join-Path $repoRoot "web"
$diagnosticUrl = "http://127.0.0.1:$Port/diagnostic.html"

$python = Get-Command python -ErrorAction Stop
$server = Start-Process -FilePath $python.Source `
    -ArgumentList @("-m", "http.server", $Port, "--bind", "127.0.0.1") `
    -WorkingDirectory $webRoot `
    -WindowStyle Hidden `
    -PassThru

try {
    Start-Sleep -Milliseconds 500
    if ($server.HasExited) {
        throw "The local diagnostic server could not start. Port $Port may already be in use."
    }
    Start-Process $diagnosticUrl
    Write-Host "Virtual Amiibo diagnostic: $diagnosticUrl"
    Write-Host "No serial device is required. Press Enter here to stop the local server."
    Read-Host | Out-Null
}
finally {
    if (-not $server.HasExited) {
        Stop-Process -Id $server.Id
    }
}
