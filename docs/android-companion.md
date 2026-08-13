# Android Companion

The native Android client lives in [`android/companion/`](../android/companion/README.md). Its README
is the build, architecture, protocol, responsive-layout, test, limitation, and hardware-validation
handoff.

The application consumes two intentionally separate PicoSwitch2 interfaces:

1. BLE GATT newline-JSON management, shared with the Web Portal.
2. Android Classic HID Device input, consumed by PicoSwitch2's generic Bluetooth gamepad parser.

The Android client does not redefine either protocol. Firmware sources and host fixtures remain the
authority for the wire contracts.

The companion's appearance is intentionally client-local. It supports System, Light, Dark, and
true OLED-black themes, with a small set of labeled Joy-Con-inspired accent palettes. The verified
Joy-Con 2 references are the left `#9BE1E6` and right `#FF8C5F` accents documented in
[`switch2-joycon2/protocol.md`](switch2-joycon2/protocol.md); selecting an inspired palette does
not issue `body`, `jcl`, or `jcr` firmware commands and is not a hardware identity claim.

Current implementation status is tracked in
[`android/companion/FEATURE_PARITY.md`](../android/companion/FEATURE_PARITY.md). The intentionally
short eventual hardware session is
[`android/companion/HARDWARE_VALIDATION.md`](../android/companion/HARDWARE_VALIDATION.md).
The first AYN Thor live results and remaining end-to-end gates are preserved in
[`experiments/android-companion-ayn-thor-live-2026-08-13.md`](experiments/android-companion-ayn-thor-live-2026-08-13.md).
