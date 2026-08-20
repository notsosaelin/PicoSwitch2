# Build + run the in-band management framework host-test suite in one command.
#
#   pwsh -File tools/run_mgmt_tests.ps1
#
# These are pure host tests (no hardware, no firmware): they exercise the
# wireless command bridge, the access-control spec, the session composition, the
# bonds grammar/serializer, and the Android controller HID contract. Keep this green while
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
    @{ n='test_ns2_input_arbiter';                   src='tools/test_ns2_input_arbiter.c src/bt_hid/ns2_input_arbiter.c';            flags='' }
    # Bluetooth Keyboard / Keyboard + Mouse: the portable mapping/merge/
    # translation/role model and the keyboard report-descriptor scanner.
    @{ n='test_ns2_kbm';                            src='tools/test_ns2_kbm.c src/ns2_kbm.c';                                           flags='' }
    @{ n='test_bthid_keyboard_report';              src='tools/test_bthid_keyboard_report.c src/bt_hid/bt/bthid/devices/generic/bthid_keyboard_report.c'; flags='-Isrc/bt_hid' }
    @{ n='test_ns2_kbm_config_persistence';         src='tools/test_ns2_kbm_config_persistence.c src/config_persist.c src/ns2_kbm.c';   flags='' }
    # The KB/M status snapshot is rendered by ONE formatter shared by the
    # management and UART surfaces. Both previously had their own printf, and a
    # format/argument mismatch silently corrupted every field of the diagnostic.
    @{ n='test_ns2_kbm_status';                     src='tools/test_ns2_kbm_status.c src/ns2_kbm_status.c src/ns2_kbm.c';               flags='-Isrc/bt_hid -Wno-unused-parameter' }
    @{ n='test_ns2_ble_reconnect';                  src='tools/test_ns2_ble_reconnect.c src/ns2_ble_reconnect.c';                       flags='' }
    @{ n='test_ns2_bt_lifecycle';                   src='tools/test_ns2_bt_lifecycle.c src/ns2_bt_lifecycle.c';                         flags='' }
    # Xbox / Xbox Elite quirk pipeline. The Elite is identified by the quirk
    # table (name-matched, because BLE PnP often fails to resolve VID/PID), not
    # by any descriptor heuristic; these pin that chain so a future
    # classification change cannot make it unreachable again.
    #
    # xbox_rumble.c is required to link: QUIRK_XBOX's send_rumble pulls in
    # xbox_rumble_build_payload(). Its absence is why test_bthid_gamepad_quirks
    # previously failed to link with no diagnostic beyond "ld returned 1".
    @{ n='test_xbox_elite_quirk_pipeline';          src='tools/test_xbox_elite_quirk_pipeline.c src/bt_hid/bt/bthid/devices/generic/bthid_gamepad_quirks.c src/bt_hid/bt/bthid/devices/generic/quirks/xbox/bthid_gamepad_quirk_xbox.c src/bt_hid/bt/bthid/devices/generic/quirks/xbox/bthid_gamepad_quirk_xbox_elite2.c src/bt_hid/bt/bthid/devices/vendors/microsoft/xbox_rumble.c src/bt_hid/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_m30.c src/bt_hid/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ngc_modkit.c src/bt_hid/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_paddle.c src/bt_hid/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ultimate_mg.c'; flags='-Itools/host_stubs -Isrc/bt_hid -Wno-unused-parameter' }
    @{ n='test_bthid_gamepad_quirks';               src='tools/test_bthid_gamepad_quirks.c src/bt_hid/bt/bthid/devices/generic/bthid_gamepad_quirks.c src/bt_hid/bt/bthid/devices/generic/quirks/xbox/bthid_gamepad_quirk_xbox.c src/bt_hid/bt/bthid/devices/generic/quirks/xbox/bthid_gamepad_quirk_xbox_elite2.c src/bt_hid/bt/bthid/devices/vendors/microsoft/xbox_rumble.c src/bt_hid/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_m30.c src/bt_hid/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ngc_modkit.c src/bt_hid/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_paddle.c src/bt_hid/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ultimate_mg.c'; flags='-Itools/host_stubs -Isrc/bt_hid -Wno-unused-parameter' }
    @{ n='test_ns2_active_input_lifecycle';          src='tools/test_ns2_active_input_lifecycle.c src/bt_hid/bt/bthid/bthid.c src/bt_hid/bt/bthid/bthid_identity.c src/bt_hid/bt/bthid/devices/generic/bthid_keyboard_report.c src/bt_hid/ns2_input_arbiter.c'; flags='-Isrc/bt_hid -Wno-unused-parameter -Wno-sign-compare -ffunction-sections -fdata-sections "-Wl,--gc-sections"' }
    @{ n='test_config_wireless_bridge';             src='tools/test_config_wireless_bridge.c src/config_wireless_bridge.c';             flags='' }
    @{ n='test_config_wireless_bridge_edge';        src='tools/test_config_wireless_bridge_edge.c src/config_wireless_bridge.c';        flags='' }
    @{ n='test_config_wireless_bridge_concurrency'; src='tools/test_config_wireless_bridge_concurrency.c src/config_wireless_bridge.c'; flags='-pthread' }
    @{ n='test_mgmt_access';                        src='tools/test_mgmt_access.c src/mgmt_access.c src/config_wireless_bridge.c';     flags='' }
    @{ n='test_mgmt_session';                       src='tools/test_mgmt_session.c src/mgmt_access.c src/config_wireless_bridge.c';     flags='' }
    @{ n='test_bonds_command';                      src='tools/test_bonds_command.c src/mgmt_bonds.c';                                  flags='' }
    @{ n='test_mgmt_bonds';                         src='tools/test_mgmt_bonds.c src/mgmt_bonds.c';                                     flags='' }
    # Android companion bridge feature parity (motion/battery in, rumble/LED out)
    # compiled against the production generic driver + HID parser.
    @{ n='test_bthid_android_bridge';               src='tools/test_bthid_android_bridge.c src/bt_hid/bt/bthid/devices/generic/bthid_gamepad.c src/bt_hid/bt/bthid/devices/generic/bthid_gamepad_quirks.c src/bt_hid/bt/bthid/devices/generic/bthid_android_bridge.c src/bt_hid/usb/usbh/hid/devices/generic/hid_parser.c'; flags='-Itools/host_stubs -Isrc/bt_hid -Wno-unused-parameter -ffunction-sections -fdata-sections "-Wl,--gc-sections"' }
    @{ n='test_bthid_android_controller';           src='tools/test_bthid_android_controller.c src/bt_hid/bt/bthid/devices/generic/bthid_gamepad.c src/bt_hid/bt/bthid/devices/generic/bthid_gamepad_quirks.c src/bt_hid/bt/bthid/devices/generic/bthid_android_bridge.c src/bt_hid/usb/usbh/hid/devices/generic/hid_parser.c'; flags='-Itools/host_stubs -Isrc/bt_hid -Wno-unused-parameter -ffunction-sections -fdata-sections "-Wl,--gc-sections"' }
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

Write-Host "== test_bluetooth_secret_diagnostics ==" -ForegroundColor Cyan
python tools/test_bluetooth_secret_diagnostics.py
if ($LASTEXITCODE -ne 0) {
    $fail++
    $failed += 'test_bluetooth_secret_diagnostics (run)'
} else {
    $pass++
}

Write-Host ""
Write-Host "in-band management suite: $pass passed, $fail failed" -ForegroundColor ($fail ? 'Red' : 'Green')
if ($fail) { $failed | ForEach-Object { Write-Host "  FAILED: $_" -ForegroundColor Red }; exit 1 }
exit 0
