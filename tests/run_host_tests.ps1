# Authoritative host-test suite: build EVERY active host test from current
# source, then run only what was built in this invocation.
#
#   pwsh -File tests/run_host_tests.ps1              # everything
#   pwsh -File tests/run_host_tests.ps1 -Group management
#   pwsh -File tests/run_host_tests.ps1 -Filter arbiter
#
# WHY THIS EXISTS
#
# Until 2026-08-21 there was no such command. `tests/run_mgmt_tests.ps1` built
# 23 tests; the other 56 sources had no build recipe at all, and their
# executables simply accumulated in build/host-tests from ad-hoc gcc
# invocations. AGENTS.md then told readers to run whatever `.exe` files happened
# to be in that directory, so "all host tests pass" counted binaries of unknown
# build vintage. Cleaning build/ deleted them and exposed the gap.
#
# Negative knowledge, recorded so it is not rediscovered: a totals line derived
# from a directory listing is not evidence about current source. Totals must
# come from tests compiled during the run that reports them.
#
# Three rules follow from that, and this script enforces all three:
#   1. the output directory is recreated empty every run, so a stale binary can
#      never be executed or counted;
#   2. every tools/test_*.c must appear either in $tests or in $notHostTests
#      with a reason -- an unlisted source FAILS the run;
#   3. the printed total counts only tests built by this invocation.
#
# Recipes are the smallest intentional source set, established by compiling each
# test and resolving exactly what it needed. They are NOT auto-derived: a greedy
# "add sources until it links" pass produced wrong answers (it pulled
# switch_pro.c, and with it the Pico SDK, into two motion tests that in fact
# need only ns2_motion_seam.c).
param(
    [string]$Group = '',
    [string]$Filter = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$out = Join-Path $root 'build/host-tests'

# A stale executable must never count as a passing test.
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null

# ---------------------------------------------------------------------------
# Active host tests. group is for callers that want a subset; it does not
# affect correctness. Unusual flags are commented where they are not obvious.
# ---------------------------------------------------------------------------
$tests = @(
    # -- management / in-band BLE, config transport, bonds --------------------
    @{ g='management'; n='test_config_wireless_bridge';             src='tests/firmware/test_config_wireless_bridge.c firmware/src/management/config_wireless_bridge.c';             flags='' }
    @{ g='management'; n='test_config_wireless_bridge_edge';        src='tests/firmware/test_config_wireless_bridge_edge.c firmware/src/management/config_wireless_bridge.c';        flags='' }
    @{ g='management'; n='test_config_wireless_bridge_concurrency'; src='tests/firmware/test_config_wireless_bridge_concurrency.c firmware/src/management/config_wireless_bridge.c'; flags='-pthread' }
    @{ g='management'; n='test_mgmt_access';                        src='tests/firmware/test_mgmt_access.c firmware/src/management/mgmt_access.c firmware/src/management/config_wireless_bridge.c';       flags='' }
    @{ g='management'; n='test_mgmt_session';                       src='tests/firmware/test_mgmt_session.c firmware/src/management/mgmt_access.c firmware/src/management/config_wireless_bridge.c';      flags='' }
    @{ g='management'; n='test_bonds_command';                      src='tests/firmware/test_bonds_command.c firmware/src/management/mgmt_bonds.c';                                  flags='' }
    @{ g='management'; n='test_mgmt_bonds';                         src='tests/firmware/test_mgmt_bonds.c firmware/src/management/mgmt_bonds.c';                                     flags='' }
    @{ g='management'; n='test_mgmt_peers';                         src='tests/firmware/test_mgmt_peers.c firmware/src/management/mgmt_peers.c';                                     flags='' }
    @{ g='management'; n='test_mgmt_pairing';                       src='tests/firmware/test_mgmt_pairing.c firmware/src/management/mgmt_pairing.c';                                 flags='' }
    @{ g='management'; n='test_config_save_tracker';                src='tests/firmware/test_config_save_tracker.c firmware/src/settings/config_save_tracker.c';                   flags='' }
    @{ g='management'; n='test_ns2_kbm_config_persistence';         src='tests/firmware/test_ns2_kbm_config_persistence.c firmware/src/settings/config_persist.c firmware/src/input/ns2_kbm.c';   flags='' }

    # -- Bluetooth lifecycle, admission, liveness, input ownership ------------
    @{ g='management'; n='test_ns2_input_arbiter';                  src='tests/firmware/test_ns2_input_arbiter.c firmware/src/input/ns2_input_arbiter.c';                flags='' }
    @{ g='management'; n='test_ns2_ble_reconnect';                  src='tests/firmware/test_ns2_ble_reconnect.c firmware/src/bluetooth/ns2_ble_reconnect.c';                       flags='' }
    @{ g='management'; n='test_ns2_bt_lifecycle';                   src='tests/firmware/test_ns2_bt_lifecycle.c firmware/src/bluetooth/ns2_bt_lifecycle.c';                         flags='' }
    @{ g='management'; n='test_ns2_companion_link';                 src='tests/firmware/test_ns2_companion_link.c firmware/src/management/ns2_companion_link.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad_quirks.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ngc_modkit.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_android_bridge.c firmware/src/hid_host/usb/usbh/hid/devices/generic/hid_parser.c firmware/src/input/ns2_remap.c'; flags='-Itests/firmware/host_stubs -Ifirmware/src/hid_host -Wno-unused-parameter -ffunction-sections -fdata-sections "-Wl,--gc-sections"' }
    @{ g='management'; n='test_ns2_bt_health';                      src='tests/firmware/test_ns2_bt_health.c firmware/src/bluetooth/ns2_bt_health.c';                               flags='' }
    # Model-based lifecycle test: deterministic seeded operation sequences over
    # the pure policy objects, invariants checked after every transition. Pure
    # policy only -- BTstack wiring is guarded by the structural Python tests.
    @{ g='management'; n='test_ns2_lifecycle_model';                src='tests/firmware/test_ns2_lifecycle_model.c firmware/src/bluetooth/ns2_bt_health.c firmware/src/bluetooth/ns2_bt_lifecycle.c firmware/src/input/ns2_input_arbiter.c firmware/src/management/mgmt_access.c firmware/src/management/config_wireless_bridge.c'; flags='-Ifirmware/src/hid_host' }
    @{ g='management'; n='test_ns2_owner_led';                      src='tests/firmware/test_ns2_owner_led.c firmware/src/app/ns2_owner_led.c';                               flags='' }
    @{ g='management'; n='test_ns2_active_input_lifecycle';         src='tests/firmware/test_ns2_active_input_lifecycle.c firmware/src/hid_host/bt/bthid/bthid.c firmware/src/hid_host/bt/bthid/bthid_identity.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_keyboard_report.c firmware/src/input/ns2_input_arbiter.c'; flags='-Ifirmware/src/hid_host -Wno-unused-parameter -Wno-sign-compare -ffunction-sections -fdata-sections "-Wl,--gc-sections"' }
    @{ g='bluetooth'; n='test_ns2_pairing_crypto';                  src='tests/firmware/test_ns2_pairing_crypto.c firmware/src/bluetooth/ns2_pairing_crypto.c';                     flags='' }
    @{ g='bluetooth'; n='test_ns2_wake_protocol';                   src='tests/firmware/test_ns2_wake_protocol.c firmware/src/bluetooth/ns2_wake_protocol.c';                       flags='' }
    @{ g='bluetooth'; n='test_bootsel_gesture';                     src='tests/firmware/test_bootsel_gesture.c firmware/src/app/bootsel_gesture.c';                           flags='' }
    # Compiles the real firmware/src/app/bootsel.c against the pico-sdk stubs in tests/firmware/host_stubs and drives
    # its two cores as threads. A regression here is a HANG (the remote-pairing freeze), not a
    # wrong value, so the test carries its own watchdog and this recipe must stay in the suite.
    @{ g='bluetooth'; n='test_bootsel_park';                        src='tests/firmware/test_bootsel_park.c firmware/src/app/bootsel.c firmware/src/app/bootsel_gesture.c tests/firmware/host_stubs/pico_host_hooks.c'; flags='-Itests/firmware/host_stubs -pthread' }
    @{ g='bluetooth'; n='test_bootsel_action';                      src='tests/firmware/test_bootsel_action.c firmware/src/app/bootsel_action.c';                             flags='' }
    # core/input_event.h carries inline helpers whose parameters are unused in
    # some build configurations; every test that includes it needs this.
    @{ g='bluetooth'; n='test_controller_battery';                  src='tests/firmware/test_controller_battery.c firmware/src/input/controller_battery.c';                     flags='-Ifirmware/src/hid_host -Wno-unused-parameter' }

    # -- Keyboard / mouse ------------------------------------------------------
    @{ g='management'; n='test_ns2_kbm';                            src='tests/firmware/test_ns2_kbm.c firmware/src/input/ns2_kbm.c';                                           flags='' }
    @{ g='management'; n='test_bthid_keyboard_report';              src='tests/firmware/test_bthid_keyboard_report.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_keyboard_report.c'; flags='-Ifirmware/src/hid_host' }
    @{ g='management'; n='test_kbm_runtime_lifecycle';              src='tests/firmware/test_kbm_runtime_lifecycle.c firmware/src/input/ns2_kbm_runtime.c firmware/src/input/ns2_kbm.c'; flags='-Ifirmware/src/hid_host -Itests/firmware/host_stubs -Wno-unused-parameter' }
    @{ g='management'; n='test_kbm_keyboard_pipeline';              src='tests/firmware/test_kbm_keyboard_pipeline.c firmware/src/input/ns2_kbm.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_keyboard_report.c'; flags='-Ifirmware/src/hid_host' }
    # The KB/M status snapshot is rendered by ONE formatter shared by the
    # management and UART surfaces. Both previously had their own printf, and a
    # format/argument mismatch silently corrupted every field of the diagnostic.
    @{ g='management'; n='test_ns2_kbm_status';                     src='tests/firmware/test_ns2_kbm_status.c firmware/src/input/ns2_kbm_status.c firmware/src/input/ns2_kbm.c';               flags='-Ifirmware/src/hid_host -Wno-unused-parameter' }
    # The KB/M READ surface (mapping pages, profile library, active mappings).
    # These formatters used to live in firmware/src/settings/config.c, which does not compile on
    # the host, so their pagination was covered only by hand-written client
    # fixtures -- and a page-index bug shipped and reached hardware. This drives
    # the real formatter and also GENERATES contracts/management/
    # kbm-wire-corpus.json, which the Windows and Android tests replay.
    @{ g='management'; n='test_ns2_kbm_commands';                   src='tests/firmware/test_ns2_kbm_commands.c firmware/src/input/ns2_kbm_commands.c firmware/src/input/ns2_kbm.c';           flags='-Ifirmware/src/hid_host -Wno-unused-parameter' }
    # Profile-switch keys: selecting a RESIDENT SLOT with no companion attached,
    # which is the reason the adapter stores profiles at all. Covers the
    # properties whose absence is dangerous rather than merely wrong -- a stuck
    # button across a switch, a key that both switches and fires, a hotkey that
    # reaches across layouts, and runtime activation leaking into the persisted
    # boot choice (which would put flash wear on the gameplay path).
    @{ g='management'; n='test_ns2_kbm_switch_keys';                src='tests/firmware/test_ns2_kbm_switch_keys.c firmware/src/input/ns2_kbm.c';                               flags='-Ifirmware/src/hid_host -Wno-unused-parameter' }
    @{ g='input';     n='test_bthid_mouse_report';                  src='tests/firmware/test_bthid_mouse_report.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_mouse_report.c firmware/src/hid_host/usb/usbh/hid/devices/generic/hid_parser.c'; flags='-Ifirmware/src/hid_host' }

    # -- Controller drivers / quirks ------------------------------------------
    # xbox_rumble.c is required to link: QUIRK_XBOX's send_rumble pulls in
    # xbox_rumble_build_payload(). Its absence is why test_bthid_gamepad_quirks
    # previously failed to link with no diagnostic beyond "ld returned 1".
    @{ g='drivers'; n='test_xbox_elite_quirk_pipeline';             src='tests/firmware/test_xbox_elite_quirk_pipeline.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad_quirks.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/xbox/bthid_gamepad_quirk_xbox.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/xbox/bthid_gamepad_quirk_xbox_elite2.c firmware/src/hid_host/bt/bthid/devices/vendors/microsoft/xbox_rumble.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_m30.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ngc_modkit.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_paddle.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ultimate_mg.c'; flags='-Itests/firmware/host_stubs -Ifirmware/src/hid_host -Wno-unused-parameter' }
    @{ g='drivers'; n='test_bthid_gamepad_quirks';                  src='tests/firmware/test_bthid_gamepad_quirks.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad_quirks.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/xbox/bthid_gamepad_quirk_xbox.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/xbox/bthid_gamepad_quirk_xbox_elite2.c firmware/src/hid_host/bt/bthid/devices/vendors/microsoft/xbox_rumble.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_m30.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ngc_modkit.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_paddle.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ultimate_mg.c'; flags='-Itests/firmware/host_stubs -Ifirmware/src/hid_host -Wno-unused-parameter' }
    # Android companion bridge feature parity (motion/battery in, rumble/LED out)
    # compiled against the production generic driver + HID parser.
    @{ g='management'; n='test_bthid_android_bridge';               src='tests/firmware/test_bthid_android_bridge.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad_quirks.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ngc_modkit.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_android_bridge.c firmware/src/hid_host/usb/usbh/hid/devices/generic/hid_parser.c firmware/src/input/ns2_remap.c'; flags='-Itests/firmware/host_stubs -Ifirmware/src/hid_host -Wno-unused-parameter -ffunction-sections -fdata-sections "-Wl,--gc-sections"' }
    @{ g='management'; n='test_bthid_android_controller';           src='tests/firmware/test_bthid_android_controller.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad_quirks.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_android_bridge.c firmware/src/hid_host/usb/usbh/hid/devices/generic/hid_parser.c'; flags='-Itests/firmware/host_stubs -Ifirmware/src/hid_host -Wno-unused-parameter -ffunction-sections -fdata-sections "-Wl,--gc-sections"' }
    @{ g='drivers'; n='test_xbox_bt_report';                        src='tests/firmware/test_xbox_bt_report.c firmware/src/hid_host/bt/bthid/devices/vendors/microsoft/xbox_bt_report.c';   flags='-Ifirmware/src/hid_host' }
    @{ g='drivers'; n='test_xbox_rumble';                           src='tests/firmware/test_xbox_rumble.c firmware/src/hid_host/bt/bthid/devices/vendors/microsoft/xbox_rumble.c';         flags='-Ifirmware/src/hid_host -Ifirmware/src/hid_host/bt/bthid/devices/vendors/microsoft' }
    @{ g='drivers'; n='test_battlergc_pro_report';                  src='tests/firmware/test_battlergc_pro_report.c firmware/src/hid_host/bt/bthid/devices/vendors/retrofighters/battlergc_pro_report.c'; flags='-Ifirmware/src/hid_host' }
    @{ g='drivers'; n='test_ds5_output';                            src='tests/firmware/test_ds5_output.c firmware/src/hid_host/bt/bthid/devices/vendors/sony/ds5_output.c';               flags='-Ifirmware/src/hid_host/bt/bthid/devices/vendors/sony' }
    @{ g='drivers'; n='test_ds5_native_haptics';                    src='tests/firmware/test_ds5_native_haptics.c';                                              flags='-Ifirmware/src/hid_host' }
    @{ g='drivers'; n='test_ds5_reconnect_transport';               src='tests/firmware/test_ds5_reconnect_transport.c';                                         flags='-Ifirmware/src/hid_host' }
    @{ g='drivers'; n='test_wii_motionplus';                        src='tests/firmware/test_wii_motionplus.c firmware/src/motion/wii_motionplus.c';               flags='-Ifirmware/src/hid_host' }

    # -- Console-facing personalities / USB -----------------------------------
    # usb.h publishes usb_personality_t only for the Switch 2 firmware.
    @{ g='usb'; n='test_usb_mode_cycle';                            src='tests/firmware/test_usb_mode_cycle.c firmware/src/app/usb_mode_cycle.c';                             flags='-DNS2_PRO' }
    @{ g='usb'; n='test_ns2_gc_identity';                           src='tests/firmware/test_ns2_gc_identity.c firmware/src/console/gamecube/ns2_gc_identity.c';                           flags='' }
    @{ g='usb'; n='test_ns2_joycon2_identity';                      src='tests/firmware/test_ns2_joycon2_identity.c firmware/src/console/joycon2/ns2_joycon2_identity.c';                 flags='' }
    @{ g='usb'; n='test_ns2_firmware_profile';                      src='tests/firmware/test_ns2_firmware_profile.c firmware/src/console/common/ns2_firmware_profile.c';                 flags='' }
    @{ g='usb'; n='test_ns2_vendor_rx';                             src='tests/firmware/test_ns2_vendor_rx.c firmware/src/console/pro2/ns2_vendor_rx.c';                   flags='' }
    @{ g='usb'; n='test_ns2_vendor_tx';                             src='tests/firmware/test_ns2_vendor_tx.c firmware/src/console/pro2/ns2_vendor_tx.c';                   flags='' }
    @{ g='usb'; n='test_switch_gc_report';                          src='tests/firmware/test_switch_gc_report.c firmware/src/console/gamecube/switch_gc_encode.c firmware/src/input/controller_battery.c';      flags='-Ifirmware/src/console/gamecube' }
    @{ g='usb'; n='test_switch_gc_report_select';                   src='tests/firmware/test_switch_gc_report_select.c firmware/src/console/gamecube/switch_gc_report_select.c'; flags='-Ifirmware/src/console/gamecube' }
    @{ g='usb'; n='test_switch_gc_rumble_decode';                   src='tests/firmware/test_switch_gc_rumble_decode.c firmware/src/console/gamecube/switch_gc_rumble_decode.c'; flags='-Ifirmware/src/console/gamecube' }
    @{ g='usb'; n='test_switch_joycon2_report';                     src='tests/firmware/test_switch_joycon2_report.c firmware/src/console/joycon2/switch_joycon2_encode.c firmware/src/input/controller_battery.c'; flags='-Ifirmware/src/console/joycon2 -Ifirmware/src/hid_host' }
    @{ g='usb'; n='test_touch_layout_face_goldens';                 src='tests/firmware/test_touch_layout_face_goldens.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad_quirks.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ngc_modkit.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_android_bridge.c firmware/src/hid_host/usb/usbh/hid/devices/generic/hid_parser.c firmware/src/input/ns2_remap.c firmware/src/input/ns2_kbm.c firmware/src/console/pro2/switch_pro2_encode.c firmware/src/console/gamecube/switch_gc_encode.c firmware/src/console/joycon2/switch_joycon2_encode.c firmware/src/input/controller_battery.c'; flags='-Itests/firmware/host_stubs -Ifirmware/src/hid_host -Ifirmware/src/console/gamecube -Ifirmware/src/console/joycon2 -Wno-unused-parameter -ffunction-sections -fdata-sections "-Wl,--gc-sections"' }
    @{ g='usb'; n='test_controller_link_face_goldens';               src='tests/firmware/test_controller_link_face_goldens.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_gamepad_quirks.c firmware/src/hid_host/bt/bthid/devices/generic/quirks/bitdo/bthid_gamepad_quirk_bitdo_ngc_modkit.c firmware/src/hid_host/bt/bthid/devices/generic/bthid_android_bridge.c firmware/src/hid_host/usb/usbh/hid/devices/generic/hid_parser.c firmware/src/input/ns2_remap.c firmware/src/input/ns2_kbm.c firmware/src/console/pro2/switch_pro2_encode.c'; flags='-Itests/firmware/host_stubs -Ifirmware/src/hid_host -Wno-unused-parameter -ffunction-sections -fdata-sections "-Wl,--gc-sections"' }
    @{ g='usb'; n='test_ns2_player_led';                            src='tests/firmware/test_ns2_player_led.c firmware/src/console/common/ns2_player_led.c';                             flags='' }
    @{ g='usb'; n='test_ns2_locked_mapping';                        src='tests/firmware/test_ns2_locked_mapping.c firmware/src/input/ns2_remap.c';                              flags='' }
    @{ g='usb'; n='test_hid_out_normalize';                         src='tests/firmware/test_hid_out_normalize.c firmware/src/app/hid_out_normalize.c';                       flags='' }
    @{ g='usb'; n='test_rumble_peak';                               src='tests/firmware/test_rumble_peak.c';                                                     flags='' }
    # The pure, Windows-API-free core of the tools/gcusb PC-side USB protocol lab.
    @{ g='usb'; n='test_gcusb_core';                              src='tests/firmware/test_gcusb_core.c tools/gcusb/gcusb_core.c';                  flags='-Itools/gcusb' }

    # -- Diagnostics ----------------------------------------------------------
    # The protocol trace ring is compiled out unless the UART diagnostic surface
    # is built; without -DNS2_UART_DIAG the module is empty and every record
    # silently vanishes, which is exactly how this test failed when restored.
    @{ g='diag'; n='test_ns2_protocol_trace';                       src='tests/firmware/test_ns2_protocol_trace.c firmware/src/diagnostics/ns2_protocol_trace.c';                     flags='-DNS2_UART_DIAG' }
    @{ g='diag'; n='test_ns2_diag_input';                           src='tests/firmware/test_ns2_diag_input.c firmware/src/input/ns2_diag_input.c';                             flags='' }

    # -- Motion ---------------------------------------------------------------
    @{ g='motion'; n='test_ns2_motion_seam';                        src='tests/firmware/test_ns2_motion_seam.c firmware/src/motion/ns2_motion_seam.c';             flags='-Ifirmware/src/hid_host' }
    @{ g='motion'; n='test_ns2_motion_quality';                     src='tests/firmware/test_ns2_motion_quality.c firmware/src/motion/ns2_motion_seam.c firmware/src/motion/ns2_motion_pdu.c firmware/src/motion/ns2_ds5_motion.c'; flags='-Ifirmware/src/hid_host' }
    @{ g='motion'; n='test_ns2_motion_pdu';                         src='tests/firmware/test_ns2_motion_pdu.c firmware/src/motion/ns2_motion_pdu.c';               flags='-Ifirmware/src/hid_host' }
    @{ g='motion'; n='test_ns2_ds5_motion';                         src='tests/firmware/test_ns2_ds5_motion.c firmware/src/motion/ns2_ds5_motion.c firmware/src/motion/ns2_motion_pdu.c'; flags='-Ifirmware/src/hid_host' }
    @{ g='motion'; n='test_ns2_motion_hybrid';                      src='tests/firmware/test_ns2_motion_hybrid.c firmware/src/motion/ns2_motion_hybrid.c firmware/src/motion/ns2_motion_pdu.c'; flags='-Ifirmware/src/hid_host' }
    @{ g='motion'; n='test_ns2_motion_hybrid_projector';            src='tests/firmware/test_ns2_motion_hybrid_projector.c firmware/src/motion/ns2_motion_hybrid_projector.c firmware/src/motion/ns2_motion_hybrid.c firmware/src/motion/ns2_ds5_motion.c firmware/src/motion/ns2_ds5_motion40.c firmware/src/motion/ns2_motion_pdu.c'; flags='-Ifirmware/src/hid_host' }
    @{ g='motion'; n='test_ds5_motion_calibration';                 src='tests/firmware/test_ds5_motion_calibration.c firmware/src/hid_host/bt/bthid/devices/vendors/sony/ds5_motion_calibration.c'; flags='-Ifirmware/src/hid_host/bt/bthid/devices/vendors/sony' }
    @{ g='motion'; n='test_ds5_motion_chart_trigger';               src='tests/firmware/test_ds5_motion_chart_trigger.c firmware/src/motion/ds5_motion_chart_trigger.c'; flags='-Ifirmware/src/hid_host' }

    # -- Audio -----------------------------------------------------------------
    # Built in the NON-audio configuration: this test covers the speaker/mute/
    # volume control state, not the codec. The bridge's diagnostic helpers are
    # only referenced by the live-audio paths, so they are unused here.
    @{ g='audio'; n='test_ds5_audio_control';                       src='tests/firmware/test_ds5_audio_control.c firmware/src/audio/ds5_audio_bridge.c';                        flags='-Ifirmware/src/hid_host -Wno-unused-function' }
    @{ g='audio'; n='test_ds5_audio_resample';                      src='tests/firmware/test_ds5_audio_resample.c firmware/src/audio/ds5_audio_resample.c';                     flags='' }
    @{ g='audio'; n='test_switch2_pro2_audio_transport';            src='tests/firmware/test_switch2_pro2_audio_transport.c firmware/src/audio/switch2_pro2_audio_transport.c'; flags='-Ifirmware/src/hid_host' }

    # -- Amiibo / NFC ----------------------------------------------------------
    @{ g='nfc'; n='test_ns2_amiibo_v3';                             src='tests/firmware/test_ns2_amiibo_v3.c firmware/src/nfc/ns2_amiibo_v3.c';                           flags='' }
    @{ g='nfc'; n='test_ns2_amiibo_v3_write';                       src='tests/firmware/test_ns2_amiibo_v3_write.c firmware/src/nfc/ns2_amiibo_v3_write.c firmware/src/nfc/ns2_amiibo_v3.c firmware/src/nfc/ns2_virtual_nfc.c firmware/src/nfc/virtual_amiibo.c'; flags='' }
    @{ g='nfc'; n='test_ns2_amiibo_v3_runtime';                     src='tests/firmware/test_ns2_amiibo_v3_runtime.c firmware/src/nfc/ns2_amiibo_v3_runtime.c firmware/src/nfc/ns2_amiibo_v3.c firmware/src/nfc/ns2_amiibo_v3_write.c firmware/src/nfc/ns2_virtual_nfc.c firmware/src/nfc/virtual_amiibo.c'; flags='' }
    @{ g='nfc'; n='test_virtual_amiibo';                            src='tests/firmware/test_virtual_amiibo.c firmware/src/nfc/virtual_amiibo.c';                         flags='' }
    @{ g='nfc'; n='test_ns2_virtual_nfc';                           src='tests/firmware/test_ns2_virtual_nfc.c firmware/src/nfc/ns2_virtual_nfc.c firmware/src/nfc/virtual_amiibo.c'; flags='' }
    @{ g='nfc'; n='test_ns2_virtual_nfc_runtime';                   src='tests/firmware/test_ns2_virtual_nfc_runtime.c firmware/src/nfc/ns2_virtual_nfc_runtime.c firmware/src/nfc/ns2_virtual_nfc.c firmware/src/nfc/virtual_amiibo.c'; flags='' }
    @{ g='nfc'; n='test_ns2_nfc_mirror';                            src='tests/firmware/test_ns2_nfc_mirror.c firmware/src/nfc/ns2_nfc_mirror.c';                         flags='' }
)

# ---------------------------------------------------------------------------
# Sources under tests/firmware/ that look like host tests but are not part of this
# suite. Every one needs a reason; the completeness gate below reads this table,
# so "it does not build" can never be silently absorbed by deleting an entry.
# ---------------------------------------------------------------------------
$notHostTests = @{
    # C -- requires a real platform. Host-linking these would mean mocking the
    #      Pico SDK or BTstack, which buys nothing this suite does not already
    #      cover through the pure seams those modules sit behind.
    'test_bthid_late_identity' = 'platform: needs bthid.c, which needs the Pico SDK'
    'test_ns2_motion_probe'    = 'platform: ns2_motion_probe.c includes pico/time.h'
    'test_ns2_native_motion'   = 'platform: ds5_motion_pair_capture.c includes pico/critical_section.h'
    'test_ns2_wake_policy'     = 'platform: btstack_host.h includes BTstack bluetooth.h'
    'test_ds5_audio_tone'      = 'platform: needs the vendored Opus library; built by the audio lab tooling'
}

# ---------------------------------------------------------------------------
# Completeness gate: no host-test source may be silently unaccounted for.
# ---------------------------------------------------------------------------
$declared = @{}
foreach ($t in $tests) { $declared[$t.n] = $true }
$onDisk = Get-ChildItem (Join-Path $root 'tests/firmware') -Filter 'test_*.c' | ForEach-Object { $_.BaseName }
$undeclared = $onDisk | Where-Object { -not $declared.ContainsKey($_) -and -not $notHostTests.ContainsKey($_) }
if ($undeclared) {
    Write-Host "Host-test sources with no declared build recipe:" -ForegroundColor Red
    $undeclared | ForEach-Object { Write-Host "  tests/firmware/$_.c" -ForegroundColor Red }
    Write-Host "Add a recipe to `$tests, or an explicit reason to `$notHostTests." -ForegroundColor Red
    exit 1
}

$selected = $tests
if ($Group)  { $selected = $selected | Where-Object { $_.g -eq $Group } }
if ($Filter) { $selected = $selected | Where-Object { $_.n -like "*$Filter*" } }
if (-not $selected) { Write-Host "No tests matched." -ForegroundColor Red; exit 1 }

# Every firmware module directory (headers are included by basename), the
# shared config headers, the repository root (for "contracts/<name>.h"), and
# the test directory itself. Mirrors PICOSWITCH_MODULE_DIRS in firmware/CMakeLists.txt.
$moduleDirs = 'app audio bluetooth diagnostics input management motion nfc settings ' +
              'console/common console/gamecube console/joycon2 console/pro console/pro2'
$moduleIncludes = ($moduleDirs -split ' ' | ForEach-Object { "-Ifirmware/src/$_" }) -join ' '
$common = '-std=c11 -Wall -Wextra -Werror ' + $moduleIncludes + ' -Ifirmware/config -I. -Itests/firmware'
$built = 0; $pass = 0; $failed = @()

foreach ($t in $selected) {
    $exe = Join-Path $out "$($t.n).exe"
    Write-Host "== $($t.n) ==" -ForegroundColor Cyan
    Invoke-Expression "gcc $common $($t.flags) $($t.src) -o `"$exe`""
    if ($LASTEXITCODE -ne 0) { $failed += "$($t.n) (build)"; continue }
    if (-not (Test-Path $exe)) { $failed += "$($t.n) (no executable)"; continue }
    $built++
    & $exe
    if ($LASTEXITCODE -ne 0) { $failed += "$($t.n) (run)" } else { $pass++ }
}

Write-Host ""
Write-Host "host suite: $pass/$built passed (built from source in this run)" -ForegroundColor ($failed.Count ? 'Red' : 'Green')
Write-Host "excluded by declaration: $($notHostTests.Count) (see `$notHostTests)" -ForegroundColor DarkGray
if ($failed.Count) {
    Write-Host "failures:" -ForegroundColor Red
    $failed | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
exit 0
