# Capture and Analysis Artifacts

This tree stores primary captures and derived experiment media. Captures are evidence; generated
screenshots, optical-flow files, and recordings are supporting artifacts and should be interpreted
through the corresponding document under `docs/`.

| Directory | Contents |
|---|---|
| `BLE CAPTURE/` | UART/BLE native reports and diagnostic JSONL/NDJSON captures |
| `SPI/` | Genuine controller SPI dumps |
| `audio/dualsense/` | DualSense audio recordings used during transport/timing analysis |
| `audio/pro2/` | Genuine Pro Controller 2 audio recordings, spectra, and UART traces |
| `motion/2026-07-24/` | DualSense/Pro2 motion calibration videos, images, optical flow, and paired captures |
| `diagnostics/` | Desktop-capture/tool invocation checks rather than protocol evidence |
| `research/` | Third-party discussion exports used as leads, not primary evidence |

Large derived media is intentionally retained because the 2026-07-24 motion result depended on
comparing genuine Pro2 and translated DualSense behavior without requiring repeated physical
controller movement. New experiments should use dated subdirectories and document which files are
primary captures versus derived analysis.
