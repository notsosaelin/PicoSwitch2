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

Root-level dated JSONL captures are short primary UART traces retained next to their experiment
documents. `virtual-amiibo-write-retest-2026-07-25.jsonl` confirms complete 88-byte NFC `0x14`
commands, acknowledged `0x08` commit, accepted `05 00` completion, and the subsequent repeated
scan/status/Stop loop that identified missing logical tag removal.

`virtual-amiibo-lifecycle-validation-2026-07-25.jsonl` confirms the corrected committed-write,
logical-removal, later re-presentation, updated read, and second write flow.
`virtual-amiibo-after-write-2026-07-25.bin` is the generation-stable, UID/BCC-validated 540-byte
image exported over UART after that hardware test.

Large derived media is intentionally retained because the 2026-07-24 motion result depended on
comparing genuine Pro2 and translated DualSense behavior without requiring repeated physical
controller movement. New experiments should use dated subdirectories and document which files are
primary captures versus derived analysis.
