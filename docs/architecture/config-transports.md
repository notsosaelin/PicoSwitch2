# Configuration transports

Status: 🟡 implemented and host/build validated; Bluetooth hardware validation pending  
Last updated: 2026-07-26

## Purpose

PicoSwitch2 configuration remains an explicit operating mode. A two-second BOOTSEL hold changes
the USB personality to Config before any settings or Virtual Amiibo data may be changed.
The browser can then reach the same command parser through either:

- USB CDC/Web Serial when the Pico is connected to the browser host; or
- a temporary BLE GATT management service while the Pico remains physically attached to the
  console.

This is not live configuration during gameplay. Leaving Config disconnects the BLE management
client before normal controller discovery resumes and re-enumerates USB directly as Pro Controller
2.

## Safety and radio invariants

- The USB Config identity remains CDC-only at `CAFE:4012`; MSC and embedded web storage remain
  removed.
- The management service never advertises in Pro2, NSO GameCube, or either Joy-Con 2 personality.
- Its RX characteristic rejects writes unless both the USB personality and Bluetooth service state
  say Config.
- Normal controller mode performs only a 30 ms state comparison. It creates no configuration
  advertisements, notifications, connection attempts, or polling traffic.
- Entering Config stops BLE scanning and Classic inquiry before starting management advertising.
  An already HID-ready controller may remain linked so its identity and battery remain visible, but
  USB audio is absent in the Config descriptor and therefore cannot be streamed.
- Direct BLE-controller reconnect attempts are deferred while Config owns the advertiser.
- Only one incoming management link is accepted. It is classified by the Pico's LE Peripheral role
  before the connection can consume a controller/HID slot or enter controller SM/GATT discovery.
- The management characteristics intentionally require no controller-style BLE bond: the physical
  two-second Config gesture is the access gate, the service accepts only one client, and every
  characteristic write is rejected immediately after Config exit. Do not extend this service into
  normal mode without adding a separate authenticated authorization design.
- Advertising uses a low-duty 100–150 ms interval. Browser writes are split into minimum-MTU-safe
  20-byte pieces and replies are notified in negotiated-MTU-sized pieces.
- Wake advertising and Config advertising never own the advertiser simultaneously.

The custom service must remain in BTstack's static ATT database because rebuilding that database
around active controller links is unsafe. A BLE controller that independently acts as a GATT client
toward the host could therefore discover the service once during setup, but it receives no
advertisement, accepted configuration write, subscription, or steady-state traffic outside Config.

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

## Wireless command policy

Bluetooth exposes the production configuration surface:

- `info`, `ping`, `get`, `save`;
- `device` for the connected-controller summary;
- `body`, `jcl`, `jcr`, and the legacy slot-0 `lb` alias;
- every `amiibo` upload, status, read, select, present/eject, clear, `mode save`/`mode random`,
  persist, and save-back command.

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

Current linked builds after this implementation:

| Measurement | Pico 2 W | Pico W |
|---|---:|---:|
| Firmware `.bin` | 907,592 bytes | 777,668 bytes |
| `.data` | 128,876 bytes | 7,924 bytes |
| `.bss` | 180,276 bytes | 111,076 bytes |

The service adds no clock change, worker, idle NFC work, or Pico W audio dependency. Pico 2 W
remains on the validated 300 MHz profile; Pico W remains on its non-audio profile.

## Validation

Completed:

- pure host coverage for fragmented commands, busy rejection, oversized-line recovery, response
  chunking, disconnect session invalidation, and stale-response rejection;
- all 49 compiled host-test executables pass;
- Pico 2 W, Pico W, and legacy Switch 1 Pico W builds link;
- portal JavaScript parses and every referenced DOM ID exists.

Hardware still required:

1. Confirm the service appears only after entering Config and disappears on exit.
2. Connect over desktop Chromium and, if available, Chrome for Android.
3. Read/save colors over BLE; verify persistence after returning to Pro2.
4. Upload both 540- and 572-byte Amiibo files, select Save 1/Save 2, persist, and read them back.
5. Repeat with a Classic controller and a BLE controller already HID-ready in Config.
6. Confirm Config exit restores enumeration, input, motion, rumble, reconnect, wake, LED, and
   BOOTSEL behavior.
7. Play DualSense and genuine Pro2 audio after Config exit and confirm no stutter. Also verify from
   the phone that the configuration service is not discoverable during that normal audio session.

Build success is not evidence for those physical behaviors.
