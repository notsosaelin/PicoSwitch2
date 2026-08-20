# Configuration transports

> The canonical cross-platform management architecture and logical protocol now live under
> [`../management/`](../management/README.md). This document remains the focused firmware transport,
> resource, and hardware-validation record.

Status: 🟡 implemented and host/build validated; bonded/encrypted Bluetooth hardware validation pending  
Last updated: 2026-08-13

> **Update 2026-08-12 — in-band management.** The BLE GATT management service is no longer
> exclusive to the Config USB personality. It is now armed by `config_ble_authorized()` =
> `g_usb_config_mode || g_mgmt_enabled`, so with the runtime flag `g_mgmt_enabled` on (production
> default; `mgmt off` lasts until reboot)
> the same service, parser, and allowlist run **in a normal controller personality** — managing the
> adapter over Bluetooth without dropping the console. The access-control state machine
> (`src/mgmt_access.c`), first-pair policy, and remaining hardware gates are detailed in
> [`../bluetooth/in-band-management-plan.md`](../bluetooth/in-band-management-plan.md).

## Purpose

PicoSwitch2 exposes one bounded newline-JSON command parser through two transports:

- USB CDC/Web Serial in the explicit Config USB personality; and
- bonded/encrypted BLE GATT management in Config or a normal controller personality.

A two-second BOOTSEL hold still enters Config for wired setup and diagnostics. Standard builds also
boot with in-band management enabled, so the app or portal can change production settings and
Virtual Amiibo state without replacing the console-facing controller personality. `mgmt off`
disables that wireless path for the current boot; reboot restores it.

## Safety and radio invariants

- The USB Config identity remains CDC-only at `CAFE:4012`; MSC and embedded web storage remain
  removed.
- In normal mode the service advertises only while management is enabled, the console is awake,
  wake does not own the advertiser, and no management client is connected. Controller discovery
  may coexist; suppressing management advertising during discovery caused reconnect starvation.
- RX and TX-notification subscription require an active 16-byte encrypted ATT link and a durable LE
  bond. A new Just-Works bond is admitted only during the physical double-tap pairing window.
  Android's no-display Just Works flow does not provide MITM authentication, so this is accurately
  described as bonded/encrypted, not authenticated pairing.
- One incoming peripheral-role management link is accepted and kept separate from controller HID
  slots and controller-central SM/GATT discovery.
- Entering Config still supplies the CDC identity and removes USB audio/controller output. It does
  not create a second parser or a less-protected Bluetooth write path.
- Advertising uses a low-duty 100–150 ms interval. Browser writes are split into minimum-MTU-safe
  20-byte pieces and replies are notified in negotiated-MTU-sized pieces.
- Wake advertising and management advertising never own the advertiser simultaneously.

The custom service remains in BTstack's static ATT database because rebuilding that database around
active controller links is unsafe. Discovery of the UUID is not authorization: writes and TX
subscriptions still require the trusted management link described above.

## GATT surface

| Item | UUID | Direction/properties |
|---|---|---|
| Service | `7c5ad4ed-2731-417c-b316-058505c7c083` | Primary service |
| RX | `5252186a-817f-489f-ad75-94c3bd444769` | Browser → Pico, Write/Write Without Response |
| TX | `81462706-8e64-407a-bc3d-d303529fbe1c` | Pico → browser, Notify |

The advertisement contains the service UUID. The scan response and GAP Device Name both use the
single friendly identity `PicoSwitch2`; the service UUID distinguishes Config from unrelated
devices.

No generic Nordic UART UUID is used, preventing the portal from matching unrelated UART-like
peripherals.

## Bluetooth product identity and migration

The canonical current adapter name is **`PicoSwitch2`**, encoded as 11 ASCII bytes. Firmware uses
that same value for the Classic GAP local name (and BTstack's local-name/EIR path), the BLE GAP
Device Name, and the Config scan-response complete-local-name field. The latter's AD length byte
is `0x0C` — one type byte plus the 11-byte name — and its complete AD structure is 13 bytes.

The adapter's USB identities are separate Nintendo personality descriptors (`Pro Controller 2`,
`Nintendo GameCube Controller`, `Joy-Con 2 (L/R)`, or `PicoSwitch Config`); `Joypad Adapter` is
not a USB descriptor, SDP product string, capture, or vendored joypad-os product identity. The
repository audit found the old spelling only in the Android discovery compatibility path and the
current comments/docs that explain that migration path; it is absent from historical captures and
archived observations. It is retained there as the **legacy name used by pre-`PicoSwitch2` firmware**
so an existing adapter can still be found during migration. Current-name matches are preferred
when both a current and legacy bond are present.

A name change does not migrate Bluetooth bonds: the remote address and stored link keys remain the
relationship authority. Android keeps the saved-address reconnect path and accepts the legacy name
only for discovery/fallback matching; it does not delete, recreate, or infer a bond from a name.

The source/byte-length and USB-negative checks are reproducible with
`python tools/test_bluetooth_identity.py`; Android's current/legacy chooser and host-match rules
are covered by `AdapterBluetoothIdentityTest`.

## Framing and core ownership

Both transports retain the existing newline-delimited command/JSON-response protocol:

```text
browser command + "\n"
  → USB CDC parser or BLE fragment assembler
  → one bounded cross-core command slot
  → existing config parser on core 0
  → one JSON response + "\n"
  → USB CDC or BLE TX notifications
```

BLE callbacks do not parse settings or wait for flash. Core 1 only assembles a command and drains
the reply. Core 0 continues to execute the parser and pump TinyUSB during a persistence wait; core
1's established config-save service remains the only flash writer. Session generations discard a
late response if the browser disconnects while a command is executing.

The bridge deliberately holds one 127-byte command and one 512-byte reply. The browser already
waits for each response before issuing the next command, so an unbounded queue is unnecessary.

Bond enumeration is bounded at the command boundary. `bonds list` retains the historical `bonds`
array field but now returns a version-2 envelope (`v:2`, `total`, and `next:null`) when the complete
list fits. If it cannot fit, the firmware returns the compact
`{"error":"response_too_large","code":413}` response and never publishes a partial array.
Clients that receive that error request `bonds list v2` and follow the integer `next` device-DB slot
cursor until it is `null`; every page is independently bounded below 511 payload bytes. Older
clients ignore the additional envelope fields on bounded lists, while newer clients can prove that
the aggregate is complete. A legacy/unversioned response is not treated as authoritative by the
Android companion.

## Wireless command policy

Bluetooth exposes the production configuration surface:

- `info`, `ping`, `get`, `save`;
- `device` for the connected-controller summary;
- bounded `input sources` and `input active <id|none>` selection;
- `personality`, `reenumerate`, `wake`, `mgmt`, and versioned `bonds` management;
- `body`, `jcl`, `jcr`, and the legacy slot-0 `lb` alias;
- every `amiibo` upload, status, read, select, present/eject, clear, persist, and save-back
  command.

Research commands remain USB/UART-only: audio diagnostics, motion/anomaly diagnostics, firmware
read tracing, raw BLE capture, GATT experiments, and Bluetooth identity-log inspection. The portal
does not poll its developer panel over Bluetooth.

## Browser behavior

`web/index.html` offers separate **Connect USB** and **Connect Bluetooth** actions while retaining
one settings/Amiibo UI and one serialized command queue. Web Bluetooth service discovery is
user-gesture initiated. The production launcher serves the page from stable localhost because Web
Bluetooth requires a secure context. Both transports poll only the low-rate controller summary and
Amiibo status. Raw input, mapping, motion, audio, and capture diagnostics remain off the wireless
path, avoiding unnecessary management-link airtime.

The offline IndexedDB Amiibo library is independent of either transport.

## Install reset boundary

Every UF2 contains a page-aligned pending marker in application flash. On the first boot of that
flashed image—before USB, CYW43, or core 1 start—firmware erases the five reserved persistence
sectors and programs the marker page to its consumed all-zero state. This clears settings, both
Virtual Amiibo banks, learned wake identity, and BTstack bonds. Erase happens before marker
consumption, so a power interruption repeats the safe reset. An ordinary reboot reads the consumed
marker and does not erase anything. The browser-local Amiibo library belongs to the user's
computer/phone and is unaffected.

## Resource state

Current linked builds after the 2026-08-13 active-input management slice:

| Measurement | Pico 2 W | Pico W |
|---|---:|---:|
| Firmware `.bin` | 975,120 bytes | 850,956 bytes |

The service adds no clock change, worker, idle NFC work, or Pico W audio dependency. Pico 2 W
remains on the validated 300 MHz profile; Pico W remains on its non-audio profile.

## Validation

Completed:

- pure host coverage for fragmented commands, busy rejection, oversized-line recovery, response
  chunking, disconnect session invalidation, and stale-response rejection;
- all 67 compiled host-test executables pass at the repository-wide baseline;
- the 11-test management/source suite passes after the active-input allowlist change;
- Pico 2 W and Pico W Release builds link and retain valid install-reset markers;
- portal JavaScript parses and every referenced DOM ID exists.

Hardware still required:

1. Validate first management bond only inside the double-tap window, encrypted saved reconnect
   outside it, plaintext/new-client rejection, `mgmt off`, and reboot restoring management on.
2. Exercise normal-mode management during active console play with audio, gyro, wake, and latency
   observation. The existing 5.4-hour controller-plus-management soak established reconnect
   stability but was not this complete gameplay matrix.
3. Exercise physical-to-Android-to-physical active-input switching repeatedly and confirm neutral
   transitions, fresh-state gating, feedback routing limits, latency, and no radio starvation.
4. Complete the focused phone/portal mutation and Virtual Amiibo hardware matrix.

Build success is not evidence for those physical behaviors.
