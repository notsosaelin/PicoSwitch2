# Changelog

Release notes describe user-visible behavior. Detailed implementation history remains in
`docs/archive/` and the experiment records.

## Unreleased

## 1.5.0 — 2026-07-22

### Added

- Native motion passthrough from a genuine Switch 2 Pro Controller to the Pro Controller 2 output
  personality, including automatic reconnect recovery and stationary source-off hold.
- Out-of-band UART protocol tracing and diagnostics for console USB traffic, BLE capture,
  firmware reads, bonded reconnect state, and native-motion ownership.
- Genuine current firmware identities for Pro Controller 2, NSO GameCube, and both Joy-Con 2
  personalities; the Switch 2 now reports each emulated personality as up to date.

### Changed

- The controller-side native-report setup now uses console-captured commands, verified GATT
  handles, and a 7.5 ms BLE interval in a named production profile. UART variants remain isolated
  diagnostics.

### Fixed

- Genuine Pro Controller 2 bonded HOME reconnect now restores the controller through BTstack's
  Security Manager instead of raw HCI encryption, preserving input and native gyro without SYNC.
- Player 1 LED state is reasserted after controller power cycles instead of remaining in the
  running/search pattern after a successful reconnect.
- Switch 2 custom pairing now serializes GATT transactions, preserves the controller's
  authoritative LTK and raw-HCI address order, and retains the durable bond across recoverable
  reconnect failures.

### Validation

- Splatoon 3 confirms correct native motion aim and stable reconnect behavior.
- Twenty consecutive controller-off/HOME cycles restored input, P1 LED, and gyro without SYNC.
- All 35 host-test executables pass; Pico W and Pico 2 W release builds compile successfully.
- An eight-hour 300 MHz Pico 2 W gameplay soak completed without an observed stability issue.

### Known limitations

- Translating motion from non-Nintendo controllers into genuine Pro Controller 2 native PDUs
  remains unresolved; the current native path is specific to a genuine Pro Controller 2 source.
- Joy-Con 2 bonded HOME reconnect has not received the same 20-cycle regression pass.
- NFC/amiibo passthrough remains research-only.

## 1.4.0 — 2026-07-18

### Added

- Full Pro Controller 2 UAC1 speaker/microphone USB function, replacing the descriptor-only
  audio stub.
- Live Switch 2/Windows audio output through a paired DualSense on Pico 2 W, using the
  hardware-confirmed 300 MHz floating-point/SRAM Opus path.
- Conditional Switch 2 headset presence from the DualSense physical jack, including stable
  removal and reinsertion without freezing controller input.
- DualSense native PCM haptics during console audio and headset-free rumble, with
  peak-preserving 3.25× rendering and a bounded two-packet STOP tail.
- Bluetooth battery passthrough across native HID telemetry, BLE Battery Service, and every
  console-facing USB personality.

### Changed

- Pico 2 W now uses the validated 300 MHz live-audio configuration by default. Pico W retains
  its previously validated non-audio clock and Bluetooth scheduling.
- DualSense and DualSense Edge rumble now use the more accurate native PCM renderer on Pico 2 W,
  whether or not a headset is connected.
- DualSense Edge Fn L/Fn R default to GL/GR.
- Bluetooth discovery idles after one controller connects, making one dongle to one controller
  the explicit supported scope and preserving radio bandwidth for audio.

### Fixed

- Audio and native rumble after a saved-bond DualSense reconnect or dongle power cycle; no fresh
  pair is required.
- DualSense headset unplug/replug input freezes and failure to restore audio/haptics.
- Chopped DualSense playback caused by incorrect stream timing, Opus scheduling, XIP stalls, and
  underspeed RP2350 execution.
- Native haptic intensity loss caused by sampling only the latest Nintendo rumble value instead
  of preserving each audio interval's left/right peaks.

### Known limitations

- Live DualSense audio is Pico 2 W-only; the Pico W experiment could not sustain playback.
- DualSense microphone return is not implemented.
- Extended 300 MHz thermal soak testing and the Pro Controller 2 update-prompt investigation
  remain open.
- Native console motion report `0x09` and NFC/amiibo remain blocked on stronger capture evidence.

## 1.3.0 — 2026-07-17

### Added

- Dedicated Retro Fighters BattlerGC Pro Bluetooth XInput profile with native NSO GameCube
  shoulders, click-gated analog triggers, separate Home-event handling, and Xbox-compatible rumble.
- First-generation 8BitDo Ultimate Bluetooth integration for independent P1/P2 paddles as GL/GR,
  plus guarded firmware patch, validation, recovery, and flash tooling.
- 8BitDo NGC Modkit rumble using its BlueRetro-derived `0xA5 / DB LL RR` output format.
- Host regressions for BOOTSEL gesture scheduling, late Bluetooth identity resolution, reconnect
  wake policy, Classic Xbox/Battler reports, and 8BitDo controller extensions.

### Fixed

- Joy-Con 2 Left Windows/Steam classification by restoring the missing Microsoft OS interface
  property.
- Late BLE Device Information VID/PID handoff without delaying initial input notifications.
- False console wake from controller reconnect/startup reports, including the genuine Switch 1
  Pro Controller initialization sequence.
- Classic-Xbox unsigned stick decoding and BattlerGC Pro pairing, shoulder, trigger, Home, and
  GameCube-mode L3/R3 behavior.
- BOOTSEL gesture progression under sustained report-driven scheduling.

### Known limitations

- The first-generation 8BitDo Ultimate custom paddle transport requires its guarded controller
  firmware patch; stock Bluetooth firmware does not expose P1/P2 independently.
- BattlerGC Pro screenshot and P1/P2 controls are not distinguishable in Bluetooth XInput reports.
- Native console motion report `0x09` and NFC/amiibo remain blocked on stronger capture evidence.

## 1.2.0 — 2026-07-16

### Added

- Hardware-confirmed individual Joy-Con 2 Left and Right personalities and sideways mappings.
- Configurable Pro Controller 2 body color and independent Joy-Con 2 Left/Right accent colors.
- Sony lightbar color matching and DualSense player-indicator forwarding.

### Fixed

- Preserved the confirmed Pro Controller 2, NSO GameCube, wake, rumble, pairing, and BOOTSEL
  behavior while adding the new personalities and appearance controls.

## 1.1.0 — 2026-07-15

### Added

- Native NSO GameCube Controller output personality with real-console input and rumble.
- Experimental Joy-Con 2 Left and Right output personalities.
- DualSense Edge paddles, Fn buttons, mute button, lightbar, and rumble support.
- Xbox rumble framing shared across the Xbox input paths.
- Explicit Bluetooth admission control for pairing windows and post-wipe forgotten devices.
- Host tests for GameCube rumble decoding, DualSense output, Xbox rumble, and existing reports.

### Fixed

- GameCube rumble ON/OFF/STOP decoding and unbounded full-strength output.
- DualSense and DualSense Edge input, LEDs, button mapping, and rumble regressions.
- BOOTSEL double-tap, triple-tap, and five-second hold starvation while a high-rate
  DualSense report stream is active.
- Switch 2 Pro Controller shoulder-button mapping.
- Pairing wipe now disconnects active controllers and prevents immediate automatic readmission.

### Known limitations

- Joy-Con 2 mappings need a complete real-console validation pass.
- Joy-Con 2 Left may require manual setup in Steam on Windows while Right is recognized.
- Native console motion output and wake-from-sleep remain unfinished.

## 1.0.0

- Initial Switch 2 Pro Controller release.
