# PicoSwitch2 Architecture

Status: ✅ current as of 2026-07-16

## Purpose

PicoSwitch2 accepts Bluetooth controller input and presents one of several Nintendo Switch 2 USB
controller personalities. It uses two RP2040/RP2350 cores so USB timing and Bluetooth host work do
not block each other.

## Runtime ownership

| Component | Core | Responsibility |
|---|---:|---|
| TinyUSB device loop | 0 | USB enumeration, descriptors, input streaming, output reception |
| USB personalities | 0 | Pro2, NSO GameCube, Joy-Con 2 L/R, CDC/config |
| BOOTSEL raw sampler | 0 | Safe flash-CS sample after cooperative core-1 park |
| BTstack/CYW43 | 1 | Discovery, pairing, reconnection, HID/GATT transport |
| joypad-os bthid | 1 | Controller identity, input parsing, output tasks |
| Seam/router | 1 | Unified input → Switch button/capability model |
| BOOTSEL gestures and LED | 1 | Pairing, wipe, personality requests, status indication |

## Input path

```text
Bluetooth report
  → BTstack transport
  → bthid device driver
  → input_event_t
  → router_submit_input()
  → ns2_seam mapping
  → shared switch_pro_input_t
  → active USB personality encoder
  → TinyUSB interrupt IN
```

Controller-specific drivers own wire parsing. The seam owns policy: which normalized button becomes
which Switch 2 destination for the active output personality.

## Output path

```text
Switch 2 / PC interrupt OUT
  → active USB personality decoder
  → shared feedback state
  → bthid_task()
  → controller-specific output builder
  → BTstack HID interrupt / GATT write
```

GameCube rumble is decoded as an ON/OFF/STOP state and converted to a bounded downstream pulse.
DualSense and Xbox use their own packet builders and preserve explicit zero-magnitude STOP writes.
The active output personality's configured appearance (Pro2 body or per-side Joy-Con 2 accent) is
routed to supported Sony RGB lightbars. Separately, Switch 2 command `0x09` player assignments cross
cores through generation-counted shared state and are translated into controller-specific player
indicators (including DualSense dots).

## Late BLE identity invariant

BLE HID readiness is not blocked on Device Information Service. The host binds from advertisement
identity or a provisional name, enables report notifications, and only then runs the serialized
DIS → BAS sequence. Every valid DIS PnP result is subsequently delivered to BTHID, even when the
connection cache already held the same IDs. Vendor name fallbacks remain valid while VID/PID is
unknown, but known contradictory identity invalidates the Xbox BLE, Stadia, and MouthPad provisional
matches and triggers the existing disconnect/init rebind at the consuming layer.

`tools/test_bthid_late_identity.c` links the production BTHID state machine to mock drivers and
transport state. It pins reports before DIS, corrective provisional-to-specific and
generic-to-specific rebinds, fallback to generic, transport gating, repeated-DIS idempotence, and
delivery of the next input notification after a rebind. GATT serialization itself remains covered
by firmware build inspection and hardware behavior rather than simulated by that host test. The
combined notification-first and late-identity path is hardware-confirmed with Xbox Series BLE.

## Scheduling invariant

Core 1 runs BTstack's non-returning event loop. Timer callbacks alone are not a sufficient
maintenance mechanism under sustained Classic HID traffic. Each inbound HID report boundary must:

1. Service a pending cooperative BOOTSEL sample request.
2. Advance and dispatch BOOTSEL gestures.
3. Run `bthid_task()` for LED/rumble/output progress.

The 3 ms rumble timer and 30 ms control timer remain fallbacks when reports are quiet or absent.
This invariant was hardware-confirmed with DualSense and DualSense Edge after timer-only scheduling
caused output and gestures to disappear.

Gesture recognition itself is isolated in the pure `bootsel_gesture_update()` state machine; the
Pico wrapper only supplies the latest sampled state. `tools/test_bootsel_gesture.c` drives that same
production recognizer with timer-only calls, a sustained report-only stream with zero timer calls,
and mixed calls. This pins starvation resistance and prevents either servicing path from emitting a
completed gesture twice. Physical QSPI sampling and the cross-core SRAM park remain hardware-only
concerns and are deliberately outside that host test.

## BOOTSEL sampling

The Pico BOOTSEL button shares the external flash chip-select signal. Sampling therefore requires
the other core to execute only from SRAM while flash CS is tri-stated.

- Core 0 requests a sample asynchronously and continues servicing USB.
- Core 1 observes the request at a safe point, disables interrupts, and parks in SRAM.
- Core 0 samples, publishes the result, and releases core 1.
- Core 1's gesture state machine recognizes double-tap, triple-tap, and five-second hold.

The sample cadence remains 30 ms. Shortening it to 5 ms previously disrupted real-console
GameCube rumble timing.

## Pairing and wipe policy

Triple-tap means "forget all controllers," not merely "delete BTstack keys." The wipe installs a
persistent global admission lock before disconnecting devices, disables discovery/connectability,
clears stored reconnect identity, deletes Classic and LE key material, and rejects late connection
events. A subsequent explicit double-tap pairing window clears the lock.

The global policy is necessary because Switch 2 controllers use a custom ATT pairing handshake and
do not rely on BTstack LE Security Manager bonds.

## USB personality lifecycle

Every boot starts in Pro Controller 2 mode. A five-second hold asks core 0 to disconnect TinyUSB,
reset personality-owned state, select the next personality, reconnect, and publish an LED
acknowledgment. The selection is volatile.

See [`../../STATUS.md`](../../STATUS.md) for the current cycle and
[`../switch2-gc/usb-personality.md`](../switch2-gc/usb-personality.md) for detailed callback
centralization and transition mechanics.

## Shared-state rules

- Cross-core input and feedback state uses the repository's critical-section/generation mechanisms.
- USB descriptor callbacks are centralized because TinyUSB permits one link-time definition.
- Protocol personality state must be reset when switching personalities.
- A controller driver must not encode output-personality policy that belongs in the seam.

## Build configurations

- Default: Pico W, Pro2-capable multi-personality firmware
- `PICO_BOARD=pico2_w`: Pico 2 W
- `NS2_PRO=OFF`: legacy Switch 1 Pro Controller target

Both board targets use the same source tree and are release-gated together.
