#!/usr/bin/env pwsh
# Overnight autonomous soak + re-enumeration stress for the in-band-management /
# controller coexistence failure, over the GP0/GP1 UART. Non-destructive: no flash
# writes, no bond changes -- only `btstate`/`btlife` reads and same-identity USB
# re-enumerations (`profile default`, the same operation as a BOOTSEL cycle, which
# does NOT reset the BT core).
#
#   pwsh -File tools/mgmt_soak.ps1 -Port COM11 -DurationSec 2700
#
# Two test vectors for docs/experiments/in-band-mgmt-coexistence-failure-2026-08-12.md:
#   1. Passive soak  -> the time/RF "after a period" failure.
#   2. Re-enum stress -> the "CDC/USB-personality transition" hypothesis the owner
#      flagged as possibly the same bug.
#
# It polls btstate on a cadence, flags any transition (controller/mgmt connect or
# disconnect, scan/advertise, disc counters, suppression), and dumps the full
# btlife ring on each. Every -ReenumEverySec it fires a re-enumeration and watches
# whether the controller + management client survive and recover. On the FIRST
# non-recovered regression it records "FAILURE REPRODUCED", dumps the ring, and
# STOPS firing re-enums (keeps polling so the wedged state is captured). All JSONL
# goes to -OutputPath; a plain-text summary goes to the same path with .summary.txt.
param(
    [string]$Port = 'COM11',
    [string]$OutputPath,
    [int]$DurationSec = 2700,        # total run (default 45 min)
    [int]$PollSec = 3,               # btstate cadence
    [int]$BaselineSec = 300,         # passive baseline before any stress
    [int]$ReenumEverySec = 120,      # re-enum cadence after baseline
    [int]$SettleSec = 10,            # observe window after each re-enum
    [int]$RecoverGraceSec = 30,      # controller/mgmt must recover within this
    [int]$MaxReenums = 20            # safety cap on total re-enumerations
)
$ErrorActionPreference = 'Stop'
if (-not $OutputPath) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputPath = Join-Path $PSScriptRoot "..\dumps\mgmt-soak-$stamp.jsonl"
}
$OutputPath = [System.IO.Path]::GetFullPath(
    ([System.IO.Path]::IsPathRooted($OutputPath) ? $OutputPath : (Join-Path (Get-Location) $OutputPath)))
$dir = [System.IO.Path]::GetDirectoryName($OutputPath)
if (-not [System.IO.Directory]::Exists($dir)) { [System.IO.Directory]::CreateDirectory($dir) | Out-Null }
$SummaryPath = [System.IO.Path]::ChangeExtension($OutputPath, '.summary.txt')
$enc = [System.Text.UTF8Encoding]::new($false)

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.Handshake='None'; $serial.DtrEnable=$false; $serial.RtsEnable=$false
$serial.ReadTimeout=2800; $serial.WriteTimeout=2800; $serial.NewLine="`n"; $serial.ReadBufferSize=65536

function Now { (Get-Date).ToString('o') }
function LogJson([string]$s) { [System.IO.File]::AppendAllText($OutputPath, "$s`n", $enc) }
function Summary([string]$s) {
    $line = "[$(Get-Date -Format 'HH:mm:ss')] $s"
    Write-Host $line
    [System.IO.File]::AppendAllText($SummaryPath, "$line`n", $enc)
}
function Cmd([string]$c) {
    try { $serial.DiscardInBuffer(); $serial.Write("$c`n"); return $serial.ReadLine().Trim() }
    catch { return "{`"error`":`"timeout`",`"cmd`":`"$c`"}" }
}
function State { $l = Cmd 'btstate'; LogJson "{`"host_ts`":`"$(Now)`",`"why`":`"poll`",`"resp`":$l}"; try { return $l | ConvertFrom-Json } catch { return $null } }
function DumpRing([string]$why) {
    $st = Cmd 'btstate'; LogJson "{`"host_ts`":`"$(Now)`",`"why`":`"$why`",`"resp`":$st}"
    $n = 0; try { $n = [int](($st | ConvertFrom-Json).events.count) } catch {}
    for ($i=0; $i -lt $n; $i++) { LogJson "{`"host_ts`":`"$(Now)`",`"why`":`"$why.ring`",`"i`":$i,`"resp`":$(Cmd "btlife read $i")}" }
}
function Tag($s) { if (-not $s) { return '<no-state>' }
    "ctrl=$($s.controller_connected) ble=$($s.ble_conns) scan=$($s.scan_active) adv=$($s.cble.advertising) client=$($s.cble.client) supp.mgmt=$($s.suppress.mgmt_armed) scan.starts=$($s.scan.starts) disc(c/h)=$($s.disc.ctrl)/$($s.disc.hci)" }
function Undiscoverable($s) { return ($s.mgmt_enabled -and -not $s.cble.client -and -not $s.cble.advertising) }

$serial.Open(); Start-Sleep -Milliseconds 150; $serial.DiscardInBuffer()
Summary "=== mgmt_soak start on $Port -> $OutputPath ==="
Summary "identity: $(Cmd 'fwreads')"
DumpRing 'start'
$startState = State
Summary "baseline: $(Tag $startState)"

$deadline = (Get-Date).AddSeconds($DurationSec)
$nextReenum = (Get-Date).AddSeconds($BaselineSec)
$reenums = 0
$stressStopped = $false
$prev = $startState
$failReported = $false

try {
    while ((Get-Date) -lt $deadline) {
        $s = State
        if ($s) {
            $flags = @()
            if ($prev) {
                foreach ($f in @(
                    @{n='ctrl';   a=$prev.controller_connected; b=$s.controller_connected},
                    @{n='client'; a=$prev.cble.client;          b=$s.cble.client},
                    @{n='adv';    a=$prev.cble.advertising;     b=$s.cble.advertising},
                    @{n='scan';   a=$prev.scan_active;          b=$s.scan_active})) {
                    if ($f.a -ne $f.b) { $flags += "$($f.n):$($f.a)->$($f.b)" }
                }
                if ([int]$s.disc.ctrl -gt [int]$prev.disc.ctrl) { $flags += "disc.ctrl+=$([int]$s.disc.ctrl-[int]$prev.disc.ctrl)(reason=$($s.disc.last_reason))" }
                if ([int]$s.disc.hci  -gt [int]$prev.disc.hci)  { $flags += "disc.hci+=$([int]$s.disc.hci-[int]$prev.disc.hci)(reason=$($s.disc.last_reason))" }
                if ([int]$s.suppress.mgmt_armed -gt [int]$prev.suppress.mgmt_armed) { $flags += "supp.mgmt+=$([int]$s.suppress.mgmt_armed-[int]$prev.suppress.mgmt_armed)" }
            }
            if ($flags.Count) { Summary "*** $($flags -join ', ') | $(Tag $s)"; DumpRing "transition:$($flags -join ';')" }
            $prev = $s
        }

        # Re-enumeration stress cycle.
        if (-not $stressStopped -and (Get-Date) -ge $nextReenum -and $reenums -lt $MaxReenums) {
            $reenums++
            $pre = State
            Summary "--- reenum #$reenums : pre = $(Tag $pre)"
            DumpRing "reenum$reenums.pre"
            Summary "reenum #$reenums resp: $(Cmd 'profile default')"
            # Observe the settle window.
            $recovered = $false
            $settleEnd = (Get-Date).AddSeconds([Math]::Max($SettleSec,$RecoverGraceSec))
            while ((Get-Date) -lt $settleEnd) {
                Start-Sleep -Seconds 2
                $post = State
                if ($post) {
                    Summary "    reenum #$reenums settle: $(Tag $post)"
                    $ctrlOk = ([bool]$post.controller_connected) -or ([int]$post.ble_conns -gt 0)
                    $discoverable = (-not (Undiscoverable $post))
                    if ($ctrlOk -and $discoverable) { $recovered = $true; break }
                }
            }
            $post = State
            DumpRing "reenum$reenums.post"
            if (-not $recovered) {
                $failReported = $true
                $stressStopped = $true
                Summary "!!! FAILURE REPRODUCED by re-enum #$reenums : ctrl/mgmt did not recover within ${RecoverGraceSec}s. post = $(Tag $post)"
                Summary "!!! Re-enum stress STOPPED; continuing passive capture of the wedged state."
                DumpRing "FAILURE_REPRODUCED"
            } else {
                Summary "reenum #$reenums OK (recovered): $(Tag $post)"
            }
            $nextReenum = (Get-Date).AddSeconds($ReenumEverySec)
        }

        # Spontaneous failure watch (no re-enum): controller gone or undiscoverable, not recovering.
        if (-not $failReported -and $s -and (((-not $s.controller_connected) -and ([int]$s.ble_conns -eq 0)) -or (Undiscoverable $s))) {
            $confirmEnd = (Get-Date).AddSeconds($RecoverGraceSec); $stillBad = $true
            while ((Get-Date) -lt $confirmEnd) { Start-Sleep -Seconds 3; $c = State
                if ($c -and (($c.controller_connected -or [int]$c.ble_conns -gt 0) -and -not (Undiscoverable $c))) { $stillBad=$false; break } }
            if ($stillBad) { $failReported = $true; $stressStopped = $true
                Summary "!!! SPONTANEOUS FAILURE: controller-gone/undiscoverable persisted > ${RecoverGraceSec}s. $(Tag (State))"
                DumpRing "SPONTANEOUS_FAILURE" }
        }

        Start-Sleep -Seconds $PollSec
    }
} finally {
    DumpRing 'final'
    Summary "=== mgmt_soak end. reenums=$reenums failure=$failReported. final=$(Tag (State)) ==="
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
