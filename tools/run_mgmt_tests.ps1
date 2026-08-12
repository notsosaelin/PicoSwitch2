# Build + run the in-band management framework host-test suite in one command.
#
#   pwsh -File tools/run_mgmt_tests.ps1
#
# These are pure host tests (no hardware, no firmware): they exercise the
# wireless command bridge, the access-control spec, the session composition, the
# bonds grammar, and the Android controller HID contract. Keep this green while
# building the in-band management feature (docs/bluetooth/in-band-management-plan.md).
#
# Requires gcc on PATH (MSYS2/mingw). Exit code is non-zero if any test fails.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$out = Join-Path $root 'build/host-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# name -> (source files, extra gcc flags). Include dirs are common to all.
$tests = @(
    @{ n='test_config_wireless_bridge';             src='tools/test_config_wireless_bridge.c src/config_wireless_bridge.c';             flags='' }
    @{ n='test_config_wireless_bridge_edge';        src='tools/test_config_wireless_bridge_edge.c src/config_wireless_bridge.c';        flags='' }
    @{ n='test_config_wireless_bridge_concurrency'; src='tools/test_config_wireless_bridge_concurrency.c src/config_wireless_bridge.c'; flags='-pthread' }
    @{ n='test_mgmt_access';                        src='tools/test_mgmt_access.c src/config_wireless_bridge.c';                        flags='' }
    @{ n='test_mgmt_session';                       src='tools/test_mgmt_session.c src/config_wireless_bridge.c';                       flags='' }
    @{ n='test_bonds_command';                      src='tools/test_bonds_command.c';                                                   flags='' }
)

$common = '-std=c11 -Wall -Wextra -Werror -Isrc -Iinclude -Itools'
$pass = 0; $fail = 0; $failed = @()

foreach ($t in $tests) {
    $exe = Join-Path $out "$($t.n).exe"
    $cmd = "gcc $common $($t.flags) $($t.src) -o `"$exe`""
    Write-Host "== $($t.n) ==" -ForegroundColor Cyan
    Invoke-Expression $cmd
    if ($LASTEXITCODE -ne 0) { $fail++; $failed += "$($t.n) (build)"; continue }
    & $exe
    if ($LASTEXITCODE -ne 0) { $fail++; $failed += "$($t.n) (run)" } else { $pass++ }
}

Write-Host ""
Write-Host "in-band management suite: $pass passed, $fail failed" -ForegroundColor ($fail ? 'Red' : 'Green')
if ($fail) { $failed | ForEach-Object { Write-Host "  FAILED: $_" -ForegroundColor Red }; exit 1 }
exit 0
