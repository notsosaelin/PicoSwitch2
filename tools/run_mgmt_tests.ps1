# Build + run the in-band management slice of the host suite, plus the Python
# structural/boundary suites that belong with it.
#
#   pwsh -File tools/run_mgmt_tests.ps1
#
# These are pure host tests (no hardware, no firmware): they exercise the
# wireless command bridge, the access-control spec, the session composition, the
# bonds grammar/serializer, the Bluetooth lifecycle/liveness policy objects, and
# the Android controller HID contract. Keep this green while working on in-band
# management (docs/bluetooth/in-band-management-plan.md).
#
# The compiled tests are NOT declared here. tools/run_host_tests.ps1 owns the one
# build manifest for every host test in the repository, and this script asks it
# for the `management` group. Keeping a second table here is how the project
# ended up with 56 test sources that no runner could build.
#
# For the whole suite:  pwsh -File tools/run_host_tests.ps1
#
# Requires gcc on PATH (MSYS2/mingw). Exit code is non-zero if any test fails.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$pass = 0; $fail = 0; $failed = @()

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'run_host_tests.ps1') -Group management
if ($LASTEXITCODE -ne 0) { $fail++; $failed += 'run_host_tests -Group management' } else { $pass++ }

# Python boundary/structural suites. These read source rather than link it, so
# they are not part of the compiled manifest.
$python = @(
    'test_bluetooth_secret_diagnostics',
    'test_bluetooth_wipe_transport',
    'test_bluetooth_closeout_wiring',
    'test_ns2_console_slot_wiring'
)
foreach ($t in $python) {
    Write-Host "== $t ==" -ForegroundColor Cyan
    python "tools/$t.py"
    if ($LASTEXITCODE -ne 0) { $fail++; $failed += "$t (run)" } else { $pass++ }
}

Write-Host ""
Write-Host "in-band management suite: $pass passed, $fail failed" -ForegroundColor ($fail ? 'Red' : 'Green')
if ($fail) {
    $failed | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
exit 0
