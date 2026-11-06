# Security policy

PicoSwitch2 pairs with Bluetooth devices, accepts bonded management connections,
and stores pairing keys in flash, so security reports are welcome.

## Supported versions

Only the latest release and `main` receive fixes.

## Reporting a vulnerability

Please report privately through GitHub's
[private vulnerability reporting](https://github.com/notsosaelin/PicoSwitch2/security/advisories/new)
rather than in a public issue. Include the firmware or app version, the board,
and enough detail to reproduce it.

You should get an acknowledgement within a week. Once a fix is available, the
advisory is published with credit unless you ask otherwise.

## In scope

- The BLE management service: bypassing encryption, bonding or the pairing
  window, or reading or changing settings without a bond.
- Bluetooth admission: getting a peer accepted outside the pairing window, or
  disrupting an admitted controller from an unadmitted one.
- Handling of stored link keys and companion credentials.
- The companion apps' handling of adapter data and imported files (for example
  amiibo archives).

## Out of scope

- Attacks that need physical access to the Pico (the BOOTSEL button and USB
  flashing are trusted by design).
- Weaknesses in Bluetooth itself, or in a controller's or phone's own stack,
  unless PicoSwitch2 can reasonably mitigate them.
