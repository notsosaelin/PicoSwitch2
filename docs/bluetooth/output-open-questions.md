# Bluetooth Output Open Questions

Last updated: 2026-07-15

Current hardware-confirmed output includes DualSense/Edge rumble and LEDs, Xbox rumble in Pro
Controller 2 and GameCube workflows, and previously confirmed Switch-family rumble. This file owns
remaining fidelity questions that do not belong in source comments.

## Switch 1 Pro Controller and Joy-Con HD rumble

The current encoder uses a bounded linear approximation for amplitude. Nintendo's native encoding
uses a logarithmic amplitude curve and packed frequency values. A future fidelity pass should port
the documented lookup/curve with explicit actuator-safe limits and compare perceived output at
several levels. The current implementation should not be described as byte-faithful HD rumble.

## Xbox transports

The shared Xbox packet builder is host-tested and physical Xbox rumble has worked in Pro Controller
2 and GameCube workflows. Still run explicit ON, STOP, reconnect, BLE, and Classic-BT cases before
claiming every transport/model combination.

## DualSense output

DualSense and Edge output now uses the dedicated Bluetooth report shape and is hardware-confirmed
for rumble and LEDs. Regression tests should continue to cover CRC/framing, Edge identification,
zero/stop packets, reconnect, and BOOTSEL responsiveness during a high-rate report stream.
