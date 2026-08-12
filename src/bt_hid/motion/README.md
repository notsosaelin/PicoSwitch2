# Switch 2 Motion Bridge

This directory contains the transport-independent motion modules used between Bluetooth input and
the console-facing Switch 2 report path.

| Module | Responsibility |
|---|---|
| `ns2_native_motion.c` | Own and snapshot genuine Pro Controller 2 native motion PDUs |
| `ns2_ds5_motion.c` | Translate calibrated DualSense IMU samples into the validated length-`0x1E` carrier |
| `ns2_ds5_motion40.c` | Experimental default-off coherent length-`0x28` translator and shared scheduler |
| `ns2_motion_hybrid.c` | Fail-closed semantic-group splicer |
| `ns2_motion_hybrid_projector.c` | Genuine-clock DS5 donor alignment and high-rate projection |
| `ns2_motion_hybrid_live.c` | Default-off modes, counters, and base/XOR retained capture |
| `ns2_motion_pdu.c` | Decode and encode independently understood native PDU fields |
| `ns2_motion_probe.c` | Generate explicitly controlled diagnostic motion values |
| `ds5_motion_pair_capture.c` | Capture paired source/native samples for UART analysis |

Production invariants:

- Genuine Pro Controller 2 PDUs remain opaque passthrough data.
- DualSense production output uses only the hardware-validated length-`0x1E` carrier.
- A software-generated length-`0x28` packed multi-sample IMU carrier is retained as a deferred
  research target. There is no magnetometer lane. It remains default-off; the static-template
  method and the complete coherent recipe both failed on hardware. Do not resume it without an
  explicit maintainer decision based on a concrete `0x1E` deficiency or a new observation point.
- Diagnostic capture and probe paths must remain opt-in and must not alter production output while
  disabled.

`tools/test_ns2_motion40_coherence.py` is the sequence-level gate: it drives the real C translators
from an analytic trajectory and independently checks time, orientation, gyro, acceleration, and
complete `0x1E -> 0x28 -> 0x1E` transitions. It complements rather than replaces genuine-corpus
byte-exact packer tests.

`tools/ns2_motion_hybrid.py` and `tools/test_ns2_motion_hybrid.py` provide the matching offline
fitment jig and live-capture auditor. They partition every bit, reject incompatible layouts, prove
that a group splice cannot alter unselected genuine bits, reconstruct base/XOR output, and reject
lossy or non-fail-closed captures. `tools/motion_lab.ps1 -HybridMode ... -Ready` is the only normal
way to run the live jig if the deferred campaign is explicitly reopened; it restores `off` after
each bounded experiment. The corrected sequence-wide prefix path remains hardware-unvalidated and
production DualSense motion remains length `0x1E`.

Protocol evidence and the refuted template experiment are documented in
[`../../../docs/switch2/report-0x09-motion.md`](../../../docs/switch2/report-0x09-motion.md) and
[`../../../docs/switch2/uart-magprobe.md`](../../../docs/switch2/uart-magprobe.md).
