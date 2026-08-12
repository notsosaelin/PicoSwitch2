# NSO GameCube Open Questions

Last updated: 2026-08-12

The current GameCube personality enumerates, streams input, and rumbles on a real Switch 2.
This file owns the remaining hypotheses; source comments should link here rather than preserve
experiment narratives.

## Rumble semantics

Confirmed capture forms include active packets shaped as `sequence 00 01 00` and a previously
documented host form shaped as `sequence 01 00 00`. The decoder accepts either state-byte
position, gives STOP precedence, treats an all-zero pair as OFF, and ignores zero-length idle
interrupt slots. A bounded pulse expiry prevents a missing STOP from leaving a motor active.

Still unknown:

- the semantic reason for the two state-byte positions;
- whether either form is host-, firmware-, or transport-specific;
- whether the protocol carries more than binary motor state.

Do not replace the state decoder with a Pro Controller 2 HD-rumble amplitude decoder. The native
GameCube controller uses a different motor and captured protocol.

## Identity and command fields

- The device-type byte in one compared response appears to distinguish Pro Controller 2 from
  GameCube, but a third controller sample is needed before treating that interpretation as fact.
- Some command response bytes are replayed from real captures without known semantics.
- Report `0x0A` bytes 10–12 are stable enough for interoperability but not semantically decoded.
- The periodic command `0x03/0x0C` observed after streaming may be a keepalive; it is not required
  by the currently confirmed minimum startup path.

## Input mapping validation

The output report supports native Z, analog L/R triggers, and independent trigger detents.
Per-input-controller policy is documented in [mapping.md](mapping.md). Remaining work is a full
physical matrix, especially for controllers whose controls do not map one-to-one to GameCube.

## Motion (feasible to add — format already known)

The GameCube controller **does** report motion. The confirmed report `0x0A` (protocol.md) carries the
**same motion block as Pro Controller 2's report 0x09**: a motion-data-length byte at offset `0xE`
(observed values {0, 30, 40}) followed by the motion data at `0xF`, "activated via feature bit 2."
That is byte-for-byte the same structure — and therefore, on the same console with the same IMU
family, almost certainly the same **int32 angular-phase + Q16.16 accelerometer** packing decoded for
Pro2 (see `docs/switch2/report-0x09-motion.md`).

**Current state:** `switch_gc.c`'s report builder emits motion-length `0` (no motion), exactly as the
Pro2 builder did before its motion work.

**To add it:** reuse the Pro2 report-0x09 int32 motion emission in the GC report builder, gated on the
same feature-bit-2 IMU-enable, filling `0xE`/`0xF`. This is a small, mechanical extension **once the
Pro2 report-0x09 int32 motion lands** (that work is the prerequisite; do not fork a second decoder).

**Still to verify on hardware:** that the console actually enables + reads GC motion (feature-bit-2
handshake in GC mode), the axis signs/scale match Pro2, and emitting motion does not regress GC
recognition. Treat exactly like the Pro2 console-gyro validation, in GC mode.
