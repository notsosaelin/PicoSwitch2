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

The UI now models these transports as one saved adapter relationship, while keeping four underlying
truths explicit: app relationship, app-owned CompanionDeviceManager association, Android Bluetooth
bond, and adapter-side LE bond database. One generation-owned coordinator arbitrates association,
bond, foreground/manual reconnect, and verified management progression. Android 13+ documents that
association success is delivered through both `onAssociationCreated` and the Activity result; that
duplicate is idempotent, and `BOND_BONDING` never starts GATT. First use says **Pair Adapter**,
returning use reconnects without a chooser, a missing platform bond becomes **Repair pairing**, and
controller mode reuses the saved Classic bond without a second chooser.

### The management bond is always started on the LE transport

The adapter is genuinely dual-mode on one public BD_ADDR, so once a phone has observed its Classic
identity Android caches `DEVICE_TYPE_DUAL` and keeps that across *Forget*. `createBond()` is
`createBond(TRANSPORT_AUTO)`, which then prefers **BR/EDR** and runs SSP against the adapter's
*controller* admission gate; that gate correctly refuses an unbonded Classic ACL and Android reports
the refusal as "Couldn't pair because of incorrect PIN or passkey" (hardware-confirmed 2026-08-21).
A Classic link key could not satisfy `mgmt_session_authorized()` in any case, so BR/EDR was never a
slower-but-valid route here.

`AdapterBondStarter` therefore only ever picks LE mechanisms, and **TRANSPORT_AUTO is never used,
not even as a fallback**:

| Mechanism | When | API |
|---|---|---|
| `le-create-bond` | normal | `createBond(TRANSPORT_LE)` — public from API 37, otherwise reached through the single reflective seam `CompanionViewModel.androidBondPlatform` |
| `le-gatt-initiated` | the seam is blocked or absent | open the `TRANSPORT_LE` management GATT link and let the encryption-required characteristics provoke SMP — public API only |
| `none` | a working entry point refused | reported, not papered over |

`getMethod` is the runtime feature detection: a platform that hides or blocks the overload throws,
and the policy routes to the public GATT path. That one method is the *only* non-SDK Bluetooth entry
point in the app. The chosen mechanism and Android's cached device type are always logged as
`relationship/bond.mechanism`, so a future pairing failure is attributable from one line.

The GATT-initiated path carries Android's own human-paced pairing dialog inside its connect
deadline, so `ManagementConnectionContext.expectsBonding` raises that one connect's timeout; every
other connect keeps the ordinary bonded-link deadline.

The Android backend also owns one `BluetoothGatt` generation at a time. Teardown waits for its
matching disconnected callback with a bounded timeout before closing exactly once; stale callbacks
cannot mutate a newer session. A recoverable 133, connection timeout, or congestion result receives
one clean retry, followed by one service scan pinned to the saved address. Management reports
Connected only after the PicoSwitch2 identity probe succeeds. Explicit Disconnect closes management
only and does not call the independent Controller Bridge or physical-controller paths.

Color commit is now one happy-path action: mutate, authoritative readback, save, identified
persistence completion when supported, and automatic same-personality USB re-enumeration. Only a
partial re-enumeration failure leaves a Retry affordance. These lifecycle/color changes are
source/JVM-tested as of 2026-08-20 and await the focused AYN Thor matrix in
[`HARDWARE_VALIDATION.md`](../android/companion/HARDWARE_VALIDATION.md). The original app-led HID
path through PicoSwitch2 into a real game remains hardware-confirmed from 2026-08-13.

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
