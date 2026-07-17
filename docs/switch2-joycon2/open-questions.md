# Joy-Con 2 Open Questions

Last updated: 2026-07-16

Both side personalities enumerate and stream on real Switch 2 hardware. Their descriptor and
wire-layout facts remain in [protocol.md](protocol.md); this file owns unresolved interpretations.

## Mapping

- The complete sideways map for both sides is hardware-confirmed, including face-button rotation,
  L1/R1 -> SL/SR, trigger reassignment, D-pad-to-stick synthesis, and stick-axis rotation.
- Determine why Left appears as a generic `Nintendo Joy-Con 2 (L)` with a Steam setup prompt while
  Right is recognized. Real-console enumeration works, so `bcdDevice` or Windows driver cache may
  be relevant, but neither is established as the cause.

## Output and command behavior

- Rumble ON/STOP/reconnect behavior is hardware-confirmed on both output personalities. Exact
  opaque report `0x01` byte semantics still require a side-specific capture before fidelity claims.
- Several EP0 and vendor-command responses reuse confirmed family framing with Joy-Con-specific
  identity bytes. Capture the exact Joy-Con exchange before assigning meaning to opaque fields.
- Motion, mouse, magnetometer, NFC, and side-specific accessory fields remain outside the first
  implementation and must not be inferred from Pro Controller 2 or GameCube behavior.

## Physical USB constraint

One Pico USB peripheral has one address, so it cannot expose a genuine two-child wired Joy-Con
pair. Left and Right are intentionally separate personalities, matching the real Charging Grip's
two independently addressed children rather than inventing a merged device.
