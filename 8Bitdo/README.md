# 8BitDo Ultimate Bluetooth (first model)

This folder contains the dedicated PicoSwitch2 integration, tests,
documentation, controller-firmware tooling, and local research workspace for
the first-generation 8BitDo Ultimate Bluetooth Controller.

- `integration/`: live Switch Pro compatibility and independent P1/P2 transport
- `tests/`: host-side coverage for the integration
- `firmware/`: guarded 1.11 patch, validation, recovery, and flash tools
- `docs/`: investigation and implementation record
- `research/`: ignored local binaries, decoded firmware, updater copies, and
  captures, including intact mixed-software trees containing MG/MGX and modkit
  application references

The known-good controller image preserves P1/P2 as GL/GR and console wake. The
reconnect-timeout experiment is hardware-rejected and blocked by the flash
harness because it did not improve reconnection speed and broke wake.

The live Ultimate MG/MGX and controller-modkit implementations remain in their
original project locations. Mixed decompiled application references are stored
intact under `research/mixed-software/`; they were not edited as part of this
reorganization.
