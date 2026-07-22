# PicoSwitch2 Architecture

Status: ✅ current as of 2026-07-21

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
| Live DualSense Opus worker (Pico 2 W) | 1 foreground | Blocks on complete PCM windows; CYW43/BTstack background IRQ may preempt |
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

For a genuine Pro Controller 2 source in the Pro2 personality, native motion follows a parallel
opaque side channel: BTstack enables GATT report `0x000E`, records the source connection slot, and
publishes its length-30/length-40 PDU through a cross-core seqlock snapshot. The USB encoder accepts
that snapshot only when output slot 0 still identifies as `057E:2069`, then copies it into report
`0x09` without quaternion decoding or regeneration. Buttons and sticks from `0x000E` are normalized
back through the ordinary input path because the controller stops sending `0x000A` after native
motion is enabled. Other controllers remain on the generic normalized IMU path.

Controller-specific drivers own wire parsing. The seam owns policy: which normalized button becomes
which Switch 2 destination for the active output personality. In Pico 2 W audio
builds, the same event also carries the DualSense physical-jack state. Pro Controller
2 reports advertise a headset only while that jack is occupied.

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
Fresh Sony pairing sends through raw direct L2CAP. Bonded Sony reconnects receive
through BTstack HID Host, but the oversized DualSense `0x32`/`0x39` audio reports
bypass its eight-bit output-length API through the already-negotiated interrupt CID.
All ordinary reconnect output remains on HID Host.
While DualSense USB audio is active, rumble is not sent as a competing legacy
output report. The current left/right magnitudes are rendered into report
`0x39`'s two native 64-byte stereo signed-8 haptic PCM blocks at 3 kHz. Closing
USB audio restores the established legacy report path, including an explicit STOP.
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

### Standard live-audio scheduling

The Pico SDK `threadsafe_background` CYW43 architecture services Bluetooth from a
low-priority IRQ on core1; `btstack_run_loop_execute()` otherwise leaves the foreground
waiting. In the standard Pico 2 W build, that foreground runs a blocking Opus worker:
core0 accumulates a complete 512-frame stereo PCM window, the queue wake schedules core1
immediately, and the worker resamples 512→480 and encodes one frame. The background
Bluetooth context may preempt encoding and transports completed frames in two-frame
DualSense report `0x39` packets. USB remains on core0 and Bluetooth ownership remains on
core1, avoiding the enumeration and BOOTSEL regressions observed when the cores were split
differently.

Pico 2 W keeps the hardware-confirmed floating-point Opus archive and hot memory
primitives in SRAM with a 48 KiB stack. The attempted Pico W fixed-point/XIP
profile passed build and memory gates but barely played audio on hardware; it
was removed, leaving Pico W on its prior non-audio architecture.

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
- Native side channels must carry source ownership and be rejected when the active output slot no
  longer belongs to that source; a retained disconnect sample is not permission to cross sessions.
- A controller driver must not encode output-personality policy that belongs in the seam.

## Build configurations

- Default: Pico W, Pro2-capable multi-personality firmware without the
  DualSense Bluetooth audio bridge
- `PICO_BOARD=pico2_w`: Pico 2 W with live DualSense Opus audio at the
  hardware-confirmed 300 MHz/1.20 V system clock
- `NS2_PICO2_SYSTEM_CLOCK_MHZ=150|200`: lower-clock Pico 2 W diagnostic comparisons
- `NS2_PRO=OFF`: legacy Switch 1 Pro Controller target

Both board targets use the same source tree and are release-gated together.
