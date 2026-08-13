# Android Companion

The native Android client lives in [`android/companion/`](../android/companion/README.md). Its README
is the build, architecture, protocol, responsive-layout, test, limitation, and hardware-validation
handoff.

The application consumes two intentionally separate PicoSwitch2 interfaces:

1. BLE GATT newline-JSON management, shared with the Web Portal.
2. Android Classic HID Device input, consumed by PicoSwitch2's generic Bluetooth gamepad parser.

The Android client does not redefine either protocol. Firmware sources and host fixtures remain the
authority for the wire contracts.

The UI now models these transports as one saved adapter relationship: first use says **Pair
Adapter**, returning launches direct management reconnect with discovery fallback, and controller
mode reuses the saved Classic bond without a second chooser. Android still owns the required
companion association, bond prompt, foreground HID registration, and HID connection underneath.
The AYN Thor completed the original app-led HID path through PicoSwitch2 into a real game on
2026-08-13; the simplified relationship flow and corrected Nintendo face-label mapping await a
focused replay.

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

The Android Amiibo page parity slice is recorded in
[`experiments/android-amiibo-page-parity-2026-08-13.md`](experiments/android-amiibo-page-parity-2026-08-13.md).
It now presents the raw-image identity fields that the portal uses and can read encrypted owner,
nickname, registration/write dates, write count, and game-data identifiers with the user’s own
portal-compatible 160-byte `key_retail.bin`. The key remains app-private, is excluded by the
no-backup manifest, and never enters firmware commands, diagnostics, or library export. Import,
selection, upload, Sync, present/eject, and clean/used copy operations remain key-free and offline-
safe. A compact seven-day AmiiboAPI cache now adds portal-matched friendly names, series/type/release,
compatible games/title-ID labels, and best-effort artwork; stale or offline catalog/image requests
never gate the local library or adapter flows. Initialization/re-signing, ZIP exchange, phone NFC,
and Mii rendering remain intentionally deferred.
