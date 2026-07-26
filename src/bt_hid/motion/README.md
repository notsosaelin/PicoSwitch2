# Switch 2 Motion Bridge

This directory contains the transport-independent motion modules used between Bluetooth input and
the console-facing Switch 2 report path.

| Module | Responsibility |
|---|---|
| `ns2_native_motion.c` | Own and snapshot genuine Pro Controller 2 native motion PDUs |
| `ns2_ds5_motion.c` | Translate calibrated DualSense IMU samples into the validated length-`0x1E` carrier |
| `ns2_motion_pdu.c` | Decode and encode independently understood native PDU fields |
| `ns2_motion_probe.c` | Generate explicitly controlled diagnostic motion values |
| `ds5_motion_pair_capture.c` | Capture paired source/native samples for UART analysis |

Production invariants:

- Genuine Pro Controller 2 PDUs remain opaque passthrough data.
- DualSense production output uses only the hardware-validated length-`0x1E` carrier.
- A software-generated length-`0x28` reference/magnetometer carrier is an accepted target for
  controllers without that hardware. It must not be emitted until every changing lane required by
  the console is decoded and can be generated coherently; the static-template method is refuted.
- Diagnostic capture and probe paths must remain opt-in and must not alter production output while
  disabled.

Protocol evidence and the refuted template experiment are documented in
[`../../../docs/switch2/report-0x09-motion.md`](../../../docs/switch2/report-0x09-motion.md) and
[`../../../docs/switch2/uart-magprobe.md`](../../../docs/switch2/uart-magprobe.md).
