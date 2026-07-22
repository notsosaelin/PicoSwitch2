# Native Pro Controller 2 Motion Passthrough — 2026-07-21

Status: ✅ hardware-confirmed on a real Switch 2 in Splatoon 3

## Result

A genuine Switch 2 Pro Controller can now supply motion to the dongle's Pro Controller 2 USB
personality without decoding or regenerating Nintendo's motion representation. The dongle repeats
the console-observed BLE initialization sequence, requests a 7.5 ms connection interval, receives
the controller's native `0x000E` notifications, and copies the variable-length motion block into
USB report `0x09` byte-for-byte.

This establishes a production-quality native passthrough for PID `057E:2069`. It does **not** solve
translation from DualSense or other generic IMUs; those sources still require an independently
validated encoder.

## Evidence chain

1. The out-of-band UART tracer recorded the real console's controller initialization. The
   motion-relevant sequence was configure `0x27`; reads at `0x13080/0x40`, `0x130C0/0x40`,
   `0x1FC040/0x40`, `0x13040/0x10`, `0x13100/0x18`, and `0x13060/0x20`; enable `0x27`; write
   `{0x85,0x00}` to the report-rate descriptor; then enable the native notification.
2. Full live GATT discovery identified report value handle `0x000E`, CCC `0x000F`, and the
   report-rate descriptor at `0x0010`. This supersedes the earlier handle-arithmetic hypothesis.
3. At the controller's prior 30 ms BLE interval, the native stream was exactly 33.33 Hz and the
   observed packets carried only a 40-byte (`0x28`) motion block.
4. A central-side connection update to the standard BLE minimum of 7.5 ms produced 133.35 Hz.
   The captured run contained 160 length-30 (`0x1E`) and 60 length-40 (`0x28`) blocks with no
   source-counter gaps.
5. The automatic production path reproduced that result after a 250 ms post-initialization guard:
   170 native notifications, 7.499 ms median interval, 125 `0x1E` and 45 `0x28` blocks, with no
   UART command required.

Canonical captures:

- [`sw2_pro2_gatt_discovery_2026-07-21.jsonl`](../../dumps/BLE%20CAPTURE/sw2_pro2_gatt_discovery_2026-07-21.jsonl)
- [`sw2_uart_variant8_verified_rate_2026-07-21.jsonl`](../../dumps/BLE%20CAPTURE/sw2_uart_variant8_verified_rate_2026-07-21.jsonl)
- [`sw2_uart_variant9_fast_link_2026-07-21.jsonl`](../../dumps/BLE%20CAPTURE/sw2_uart_variant9_fast_link_2026-07-21.jsonl)
- [`sw2_auto_motion_failure_2026-07-21.jsonl`](../../dumps/BLE%20CAPTURE/sw2_auto_motion_failure_2026-07-21.jsonl)
- [`sw2_auto_motion_success_2026-07-21.jsonl`](../../dumps/BLE%20CAPTURE/sw2_auto_motion_success_2026-07-21.jsonl)
- [`sw2_native_passthrough_live_2026-07-21.jsonl`](../../dumps/BLE%20CAPTURE/sw2_native_passthrough_live_2026-07-21.jsonl)

## Production behavior

- Automatic activation is restricted to a genuine source PID `0x2069` and Pro Controller 2 USB
  personality. UART experiments and GATT discovery suppress the automatic sequence so captures
  remain attributable.
- The controller stops its ordinary `0x000A` input after native `0x000E` is enabled. Buttons and
  sticks are therefore normalized from `0x000E` into the existing Switch 2 driver path.
- Native motion crosses cores through a coherent single-producer/single-consumer snapshot. The
  snapshot records its Bluetooth source slot, and core 0 accepts it only when output slot 0 still
  identifies as `057E:2069`.
- On source power-off, the last genuine `0x1E` phase/acceleration block is retained while only its
  800 Hz timing word advances. This prevents the console from extrapolating the last angular
  velocity. A later reconnect replaces the hold with live native packets.
- A 250 ms post-init guard is required. Starting immediately after the ordinary Switch 2 BLE init
  produced no `0x000E` stream; the guarded automatic run is repeatable.

## Hardware validation

- Pitch, yaw, and roll aim correctly in Splatoon 3.
- No stationary drift was observed.
- Buttons, sticks, and rumble retained their prior behavior.
- Motion returns after controller power-cycle and bonded reconnect.
- Powering the controller off while it is rotating no longer leaves the game drifting.

The user's separate eight-hour Smash session on the standard 300 MHz Pico 2 W build also completed
without an observed thermal or stability problem. That is a platform soak result, not a measured
temperature characterization.

## Remaining boundary

The validated path transports a real controller's already-correct Nintendo data. Translating a
DualSense or another IMU remains separate work because it requires generating the same semantics,
including Nintendo's length-30/length-40 cadence and filtering. Do not infer that generic motion is
solved from this passthrough result.
