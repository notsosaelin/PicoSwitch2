# DualSense / Pro Controller 2 Motion Analysis — 2026-07-24

This directory contains derived camera captures, reference frames, optical-flow transforms, UART
JSONL, and paired-controller measurements used to calibrate the DualSense-to-Switch 2 motion
translator.

Authoritative conclusions are in:

- [`../../../docs/bluetooth/dualsense-motion.md`](../../../docs/bluetooth/dualsense-motion.md)
- [`../../../docs/switch2/report-0x09-motion.md`](../../../docs/switch2/report-0x09-motion.md)
- [`../../../docs/switch2/uart-magprobe.md`](../../../docs/switch2/uart-magprobe.md)
- [`../../../docs/experiments/refuted-hypotheses.md`](../../../docs/experiments/refuted-hypotheses.md)

The `motionprobe-*`, `qmatrix-*`, `state1*`, and similar files are derived diagnostic artifacts.
The paired JSONL and genuine Pro2 captures are stronger evidence. The hardware-refuted synthetic
length-`0x28` packet must not be reconstructed from these files without first decoding its changing
leading and middle lanes.
