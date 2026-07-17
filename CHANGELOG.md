# Changelog

Release notes describe user-visible behavior. Detailed implementation history remains in
`docs/archive/` and the experiment records.

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
