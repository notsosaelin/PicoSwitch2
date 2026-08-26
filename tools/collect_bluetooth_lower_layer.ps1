#!/usr/bin/env pwsh
# Bounded, file-backed Android HCI/HAL/kernel plus Pico UART collector.

[CmdletBinding()]
param(
    [string]$Serial,
    [string]$Port,
    [ValidateRange(10, 1800)][int]$DurationSec = 120,
    [string]$Scenario = 'bluetooth-lower-layer',
    [string]$OutputRoot = 'dumps',
    [switch]$Bugreport
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $PSScriptRoot 'PicoSwitch2Lab.psm1') -Force

function Get-AdbSerial {
    param([string]$Requested)
    if ($Requested) { return $Requested }
    $devices = @(
        & adb devices |
            Select-Object -Skip 1 |
            ForEach-Object { ($_ -split '\s+')[0] } |
            Where-Object { $_ }
    )
    if ($devices.Count -ne 1) {
        throw "Expected exactly one adb transport; found $($devices.Count). Use -Serial."
    }
    return $devices[0]
}

function Invoke-AdbText {
    param([Parameter(Mandatory = $true)][string[]]$CommandArgs)
    return @(& adb -s $Serial @CommandArgs 2>&1 | ForEach-Object { $_.ToString() })
}

function Start-FileBackedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath
    )
    return Start-Process -FilePath $FilePath -ArgumentList $ArgumentList `
        -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath `
        -PassThru -WindowStyle Hidden
}

function Stop-OwnedProcess {
    param($Process)
    if (-not $Process) { return }
    try {
        $Process.Refresh()
        if (-not $Process.HasExited) {
            Stop-Process -Id $Process.Id -ErrorAction SilentlyContinue
            $Process.WaitForExit(5000) | Out-Null
        }
    } catch {}
}

function Copy-RootFile {
    param(
        [Parameter(Mandatory = $true)][string]$RemotePath,
        [Parameter(Mandatory = $true)][string]$LocalPath
    )
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = (Get-Command adb).Source
    foreach ($argument in @('-s', $Serial, 'exec-out', 'su', '-c', "cat $RemotePath")) {
        $startInfo.ArgumentList.Add($argument)
    }
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($startInfo)
    $output = [System.IO.File]::Create($LocalPath)
    try {
        $process.StandardOutput.BaseStream.CopyTo($output)
    } finally {
        $output.Dispose()
    }
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "Could not copy $RemotePath through root: $stderr"
    }
}

$Serial = Get-AdbSerial -Requested $Serial
$rootIdentity = (Invoke-AdbText -CommandArgs @('shell', 'su', '-c', 'id')) -join "`n"
if ($rootIdentity -notmatch 'uid=0\(root\)') {
    throw "Root is unavailable through adb shell: $rootIdentity"
}

$reader = Join-Path $PSScriptRoot 'read_uart_diag.ps1'
$portInfo = Get-Ps2LabPort -Reader $reader -Port $Port
$Port = $portInfo.port
$outputDirectory = New-Ps2LabDirectory -RepoRoot $repoRoot `
    -OutputRoot $OutputRoot -Scenario $Scenario

$startHost = (Get-Date).ToString('o')
$startDevice = Invoke-AdbText -CommandArgs @('shell', 'date', '+%s.%N %Y-%m-%dT%H:%M:%S.%3N%z')
$properties = @(
    'persist.bluetooth.btsnooplogmode',
    'persist.bluetooth.bqr.event_mask',
    'persist.bluetooth.bqr.min_interval_ms',
    'persist.vendor.qcom.bluetooth.soc',
    'ro.bluetooth.library_name'
)
$propertyLines = foreach ($property in $properties) {
    $value = (Invoke-AdbText -CommandArgs @('shell', 'getprop', $property)) -join ''
    "${property}=[$value]"
}
Set-Ps2LabText -Path (Join-Path $outputDirectory 'start.txt') -Lines @(
    "host=$startHost"
    "device=$($startDevice -join '')"
    "adb_serial=$Serial"
    "uart=$Port"
    "duration_seconds=$DurationSec"
    "scenario=$Scenario"
    $propertyLines
)

$logcatPath = Join-Path $outputDirectory 'android-logcat-all.txt'
$logcatErrorPath = Join-Path $outputDirectory 'android-logcat-all.stderr.txt'
$kernelPath = Join-Path $outputDirectory 'android-kernel-live.txt'
$kernelErrorPath = Join-Path $outputDirectory 'android-kernel-live.stderr.txt'
$uartPath = Join-Path $outputDirectory 'pico-uart.jsonl'
$uartStdoutPath = Join-Path $outputDirectory 'pico-uart-collector.txt'
$uartErrorPath = Join-Path $outputDirectory 'pico-uart-collector.stderr.txt'

$logcatProcess = $null
$kernelProcess = $null
$uartProcess = $null
$collectionError = $null
try {
    $logcatProcess = Start-FileBackedProcess -FilePath (Get-Command adb).Source `
        -ArgumentList @('-s', $Serial, 'logcat', '-b', 'all', '-v', 'epoch') `
        -StdoutPath $logcatPath -StderrPath $logcatErrorPath
    $kernelProcess = Start-FileBackedProcess -FilePath (Get-Command adb).Source `
        -ArgumentList @('-s', $Serial, 'shell', 'su', '-c', 'dmesg -w') `
        -StdoutPath $kernelPath -StderrPath $kernelErrorPath
    $uartProcess = Start-FileBackedProcess -FilePath (Get-Command pwsh).Source `
        -ArgumentList @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'mgmt_watch.ps1'),
                        '-Port', $Port, '-OutputPath', $uartPath,
                        '-IntervalMs', '500', '-RingEverySec', '10',
                        '-DurationSec', $DurationSec.ToString()) `
        -StdoutPath $uartStdoutPath -StderrPath $uartErrorPath

    Write-Host "Collecting $Scenario for ${DurationSec}s -> $outputDirectory" -ForegroundColor Cyan
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $lastReport = -10
    while ($stopwatch.Elapsed.TotalSeconds -lt $DurationSec) {
        Start-Sleep -Milliseconds 500
        foreach ($item in @(
            @{ Name = 'logcat'; Process = $logcatProcess },
            @{ Name = 'kernel'; Process = $kernelProcess },
            @{ Name = 'uart'; Process = $uartProcess }
        )) {
            $item.Process.Refresh()
            if ($item.Process.HasExited -and $item.Name -ne 'uart') {
                throw "$($item.Name) collector exited early with code $($item.Process.ExitCode)"
            }
        }
        $seconds = [int]$stopwatch.Elapsed.TotalSeconds
        if ($seconds -ge $lastReport + 10) {
            Write-Host "  ${seconds}s / ${DurationSec}s"
            $lastReport = $seconds
        }
    }

    # Give mgmt_watch enough time to execute its final btstate/ring capture.
    if (-not $uartProcess.WaitForExit(8000)) {
        throw 'UART collector did not finish its final snapshot in time.'
    }
    if ($uartProcess.ExitCode -ne 0) {
        throw "UART collector exited with code $($uartProcess.ExitCode)"
    }
} catch {
    $collectionError = $_.Exception.Message
    Write-Warning $collectionError
} finally {
    Stop-OwnedProcess -Process $logcatProcess
    Stop-OwnedProcess -Process $kernelProcess
    Stop-OwnedProcess -Process $uartProcess
}

$freezeHost = (Get-Date).ToString('o')
Set-Ps2LabText -Path (Join-Path $outputDirectory 'freeze-time.txt') -Lines @(
    "host=$freezeHost"
    "device=$((Invoke-AdbText -CommandArgs @('shell', 'date', '+%s.%N %Y-%m-%dT%H:%M:%S.%3N%z')) -join '')"
    "collection_error=$collectionError"
)
Set-Ps2LabText -Path (Join-Path $outputDirectory 'bluetooth-manager.txt') `
    -Lines (Invoke-AdbText -CommandArgs @('shell', 'dumpsys', 'bluetooth_manager'))
Set-Ps2LabText -Path (Join-Path $outputDirectory 'bluetooth-hal-processes.txt') `
    -Lines (Invoke-AdbText -CommandArgs @('shell', 'ps', '-A'))
Set-Ps2LabText -Path (Join-Path $outputDirectory 'kernel-freeze.txt') `
    -Lines (Invoke-AdbText -CommandArgs @('shell', 'su', '-c', 'dmesg'))
Set-Ps2LabText -Path (Join-Path $outputDirectory 'protected-log-list.txt') `
    -Lines (Invoke-AdbText -CommandArgs @('shell', 'su', '-c',
        'find /data/misc/bluetooth/logs /data/vendor/bluetooth /data/vendor/ramdump/bluetooth /data/vendor/ssrdump -maxdepth 2 -type f -ls'))

$remoteHciFiles = @(
    Invoke-AdbText -CommandArgs @('shell', 'su', '-c',
        'find /data/misc/bluetooth/logs -maxdepth 1 -type f -print') |
        Where-Object { $_ -match '^/data/' }
)
foreach ($remotePath in $remoteHciFiles) {
    $localPath = Join-Path $outputDirectory ([System.IO.Path]::GetFileName($remotePath))
    Copy-RootFile -RemotePath $remotePath -LocalPath $localPath
    $header = [System.IO.File]::ReadAllBytes($localPath)
    if ($header.Length -gt 16 -and [System.Text.Encoding]::ASCII.GetString($header, 0, 8) -eq "btsnoop`0") {
        $jsonlPath = "$localPath.jsonl"
        $summaryLines = @(
            & (Get-Ps2LabPython) (Join-Path $PSScriptRoot 'bluetooth_hci_timeline.py') `
                $localPath --jsonl $jsonlPath
        )
        if ($LASTEXITCODE -ne 0) { throw "HCI timeline parse failed for $localPath" }
        Set-Ps2LabText -Path "$localPath.summary.txt" -Lines $summaryLines
    }
}

if ($Bugreport) {
    $bugreportBase = Join-Path $outputDirectory 'android-bugreport'
    & adb -s $Serial bugreport $bugreportBase
    if ($LASTEXITCODE -ne 0) { throw 'adb bugreport failed' }
}

$artifactPaths = Get-ChildItem -LiteralPath $outputDirectory -File |
    Where-Object Name -ne 'manifest.json' |
    Select-Object -ExpandProperty FullName
$manifest = New-Ps2LabManifest -Tool 'collect_bluetooth_lower_layer.ps1' `
    -Scenario $Scenario -Action "Observe one bounded ${DurationSec}s condition without changing Bluetooth behavior." `
    -RepoRoot $repoRoot -Kind 'observation' `
    -Hypothesis 'A synchronized Android HCI/HAL/kernel and Pico UART window can locate the first observable lower-layer failure.' `
    -Variable 'The human-selected condition named by -Scenario; collector settings remain fixed.' `
    -Expectation 'Preserve HCI handle lifetimes, BQR events, HAL/kernel health, and Pico lifecycle state in one timestamped bundle.' `
    -Port $Port -Baseline 'Full HCI snoop enabled; BQR Approaching LSTO requested; no bond wipe and no Pico firmware change.' `
    -Conditions ([ordered]@{android_serial=$Serial; duration_seconds=$DurationSec; rooted=$true; bugreport=[bool]$Bugreport}) `
    -Diagnostics ([ordered]@{collection_error=$collectionError; hci_files=@($remoteHciFiles); output=$outputDirectory}) `
    -Verdict ($collectionError ? "Collector completed with error: $collectionError" : 'Bounded collection completed and freeze artifacts were written.') `
    -ArtifactPaths $artifactPaths
Set-Ps2LabJson -Path (Join-Path $outputDirectory 'manifest.json') -Value $manifest

if ($collectionError) { throw $collectionError }
Write-Host "Collection frozen: $outputDirectory" -ForegroundColor Green
Write-Output $outputDirectory
