# Android Companion

The native Android client lives in [`android/companion/`](../android/companion/README.md). Its README
is the build, architecture, protocol, responsive-layout, test, limitation, and hardware-validation
handoff.

The application consumes two intentionally separate PicoSwitch2 interfaces:

1. BLE GATT newline-JSON management, shared with the Web Portal.
2. Android Classic HID Device input, consumed by PicoSwitch2's generic Bluetooth gamepad parser.

The Android client does not redefine either protocol. Firmware sources and host fixtures remain the
authority for the wire contracts.

Management's Android-free reference implementation, normative language-neutral contract, BLE
session rules, and conformance material are indexed at
[`management/README.md`](management/README.md). Android keeps discovery, pairing, GATT lifecycle,
local files, NFC, and presentation; it consumes portable command/reply and workflow behavior from
`:management-core`.

The UI now models these transports as one saved adapter relationship: first use says **Pair
Adapter**, returning launches direct management reconnect with discovery fallback, and controller
mode reuses the saved Classic bond without a second chooser. Android still owns the required
companion association, bond prompt, foreground HID registration, and HID connection underneath.
The AYN Thor completed the original app-led HID path through PicoSwitch2 into a real game on
2026-08-13; the simplified relationship flow and corrected Nintendo face-label mapping await a
focused replay.

The application is organized around five destinations — **Adapter**, **Keyboard**, **Amiibo**,
**Gamepad**, **Settings** — with **Diagnostics** and **Amiibo settings** pushed over them rather
than holding permanent navigation space. Layout branches on measured content width, never on
orientation or device names. The full information architecture, shared Compose primitives,
responsive rules, and the debug-only layout lab used to inspect them are documented in
[`android/companion/README.md`](../android/companion/README.md).

**Keyboard & Mouse** is a first-class management area covering the firmware's complete `kbm`
surface: device and role status, input mode, both mapping profiles with a per-input editor, mouse
button mapping, and mouse translation tuning. Those settings apply to adapter RAM immediately and
are written to flash only by an explicit Save, and the UI represents exactly that rather than
implying persistence. Every accepted range is taken from the adapter's own `kbm mouse` reply, so
the client never carries a duplicate copy of a firmware limit.

Two requested management features are **blocked by missing firmware capability**, not by client
work. Controller remapping would need a `remap` command family with persisted overrides —
`NS2_BASE_BUTTON_MAP` is a compile-time table today. Adapter renaming would need a persisted name
plus dynamic advertising, EIR and ATT Device Name construction — the name is currently a
compile-time constant whose length is locked by a `_Static_assert`.

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
never gate the local library or adapter flows. Android now also provides confirmation-gated local
initialization/re-signing with the user's own key, a bounded traversal-safe portal-compatible v3 ZIP
library exchange, and a host-tested foreground one-shot ordinary-NTAG215 NFC backup path. None
changes the adapter. The physical NFC gate is pending, figure-v3 phone reads remain deliberately
rejected, and Mii rendering remains deferred.
