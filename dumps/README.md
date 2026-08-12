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
| `experiments/20260801-*-ds5-hybrid-*/` | Zero-loss genuine-base hybrid controls and one-group substitution bundles |
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

`v3-downloaded-kirby-warp-read-write-2026-07-28.jsonl` is the decisive 2048-byte figure-v3 trace:
an untouched downloaded dump completed three full-SRAM device results, a targeted read, six write
chunks, `0x08`, `05 00`, and Stop with no errors.
`v3-downloaded-kirby-warp-written-2026-07-28.bin` is the corresponding dirty-but-persisted,
HMAC-valid console-written export. The earlier `v3-downloaded-*-rejected-2026-07-28.jsonl` captures
preserve the fixed-`7A C4` failure and the zero-signature control.

`v3-kirby-air-riders-game-write-lock-2026-07-28.jsonl` isolates the separate game-data operation:
five `0x14` chunks assemble a 355-byte buffer, empty `0x20` completes it, and the old normal-write
classifier returns `07 41`. `v3-meta-shadow-after-game-write-lock-2026-07-28.bin` confirms that
the rejected operation did not alter the stored figure image.
`v3-air-riders-extended-noop-2115-0088-2026-07-28.jsonl` proves the new gate reaches `05 00`
without errors or mutation, then captures three complete retries caused by reporting the same tag
present immediately after Stop.
`v3-air-riders-tagremoved-2115-0096-2026-07-28.jsonl` confirms the corrected post-Stop scan
receives absent `07 41`. The console later re-presents and retries the unchanged image three times,
ending at `2115-0096`; this separates the fixed removal timing from the still-missing
extended-application persistence/readback.

`amiibo/genuine-kirby-warp-air-riders-write-usb-2026-07-28.jsonl` is the complete successful
positive control through a genuine Pro Controller 2 and physical Kirby & Warp Star amiibo. It
captures both sector-aware `0x20` envelopes, empty state `0x16`, each page-3 preparation read, and
both following ordinary `0x08` commits with zero trace overwrite.
`amiibo/genuine-kirby-warp-air-riders-write-ble-2026-07-28.jsonl` is the corroborating controller
BLE capture; its smaller ring dropped 121 records, so use the USB trace for sequence truth.
`amiibo/Kirby.bin` and
`amiibo/genuine-kirby-warp-after-air-riders-2026-07-28.bin` are packed 668-byte pre/post snapshots.
Their diff confirms page 4's writable tail and sector-0 pages `0x92..0x99`; the format cannot
directly observe sector 1.

`amiibo/v3-extended-build-valid-read-write-control-2026-07-28.jsonl` is the ordinary System
Settings positive control for the prepared extended-write firmware: 212 complete records, one
six-chunk/`0x08` write, zero write errors, and no extended command. Its exported result,
`amiibo/v3-extended-build-valid-read-write-output-2026-07-28.bin`, remains HMAC-valid and has a
valid `7AC4` SRAM CRC. The preceding
`amiibo/v3-extended-build-initial-load-regression-2026-07-28.jsonl` is a negative control against
the historical `v3-write-output-2026-07-28.bin`; that image's SRAM body calculates `7AC4` but
stores the incompatible `E511` trailer, so the console rejects it before the extended path.

`amiibo/v3-extended-persist-2115-0096-2026-07-28.jsonl` proves the prepared build commits the
355-byte clear and following ordinary write with zero errors, but then returns `07 41` on the
immediate next scan instead of retaining the tag for the 167-byte stage.
`amiibo/v3-extended-persist-2115-0096-output-2026-07-28.bin` is the corresponding HMAC-valid,
SRAM-valid post-failure image. This capture isolates the remaining fault to the inter-stage
auto-eject lifecycle rather than record parsing or persistence.

`amiibo/v3-extended-two-stage-success-reuse-freeze-2026-07-28.jsonl` and its
`amiibo/v3-extended-two-stage-success-output-2026-07-28.bin` prove the bounded lifecycle completes
both Air Riders stages and persists both sector records with zero write errors. Reusing that
written image exposed a separate unimplemented sector-read command.

`amiibo/genuine-kirby-warp-reuse-sub1e-usb-2026-07-28.jsonl` is the zero-overwrite genuine Pro
Controller 2 positive control for that reuse. It proves subcommand `0x1E` bare-ACKs, transitions
to empty status `0x15`, and stages a 196-byte buffer containing a 64-byte prefix plus sector-0
pages `0x92..0x99` and sector-1 pages `0x00..0x18`. This capture is the primary byte-level source
for the prepared virtual reuse implementation.

`amiibo/v3-reuse-sub1e-write-freeze-2026-07-28.jsonl` hardware-validates that virtual `0x1E`
implementation: the console receives state `0x15`, all 196 bytes, and Stop. It then sends another
167-byte update carrying `A5 00 02 00`; the old classifier's hard-coded `A5 00 01 00` rejection
is the sole observed failure.

`amiibo/v3-reuse-dynamic-header-result-2026-07-28.jsonl` and
`amiibo/v3-reuse-dynamic-header-success-output-2026-07-28.bin` prove that transaction completed
and persisted. `amiibo/v3-second-reuse-corrupted-2026-07-28.jsonl` shows the resulting image was
rejected during the next `0x1E` read before any further write.

`amiibo/genuine-air-riders-existing-data-read-write-2026-07-28.jsonl` is the genuine full-cycle
positive control. `amiibo/genuine-air-riders-postwrite-read-only-2026-07-28.jsonl` reads the same
physical tag afterward without another save. Together they prove sector-1 page 0 advanced from
`A5 00 01 00` to `A5 00 02 00`; the 167-byte envelope header carries that next value even though
its explicit sector-1 record starts at page 1. The genuine new sector data is byte-identical to
the virtual failed image, isolating the missing implicit page-0 transition.

`amiibo/genuine-air-riders-second-existing-data-write-2026-07-28.jsonl` and
`amiibo/genuine-air-riders-second-postwrite-read-only-2026-07-28.jsonl` repeat the experiment.
They prove sector-1 page 0 advances again from `A5 00 02 00` to `A5 00 03 00`, while page 4
independently advances from `A5 00 04 00` to `A5 00 05 00`. Both captures retained every record
with zero overwrite.

`amiibo/v3-dynamic-sector1-page0-write-2026-07-28.jsonl` is the corrected virtual update.
`amiibo/v3-dynamic-sector1-page0-write-output-2026-07-28.bin` is its HMAC-valid 2048-byte export,
with page 4 `A5 00 03 00` and retained sector-1 page 0 `A5 00 02 00`.
`amiibo/v3-dynamic-sector1-page0-second-read-success-2026-07-28.jsonl` is the decisive immediate
second-reuse control: Air Riders accepted the tag, loaded the previously saved custom color, and
the wire response served the retained `A5 00 02 00`.

`amiibo/v3-dynamic-sector1-page0-powercycle-read-success-2026-07-28.jsonl` is the final
flash-recovery control. After a physical adapter power cycle, it retained all 86 NFC records with
zero overwrite, served `A5 00 02 00` through `0x1E`, and contains no write. UART reported the same
2048-byte generation-4 image, payload CRC `91A6178B`, clean and persisted before and after the
accepted Air Riders read.

`amiibo/v3-air-riders-trained-before-save-2026-07-28.bin`,
`amiibo/v3-air-riders-learned-state-save-2026-07-28.jsonl`, and
`amiibo/v3-air-riders-learned-state-after-save-2026-07-28.bin` are the non-cosmetic learned
gameplay-state control. The 152-record trace has zero overwrite/errors and uses the known
three-chunk extended update plus six-chunk ordinary commit. The generation-7 → generation-9 diff
touches only modeled ranges through `0x463`; the after image has CRC `3DEE59FF` and valid amiibo
HMACs. It leaves dirty state intact for the production-portal Sync test.

`amiibo/v3-air-riders-king-dedede-tank-2115-0096-2026-07-28.jsonl` isolates the untouched
King Dedede & Tank Star write failure. The retained final attempt carries `0x64` at extended
update-envelope byte 13 where prior Kirby captures used `0x00`; the old fixed classifier rejected
all three chunks plus completion and returned `07 41`.
`amiibo/v3-air-riders-king-dedede-tank-after-2115-0096-2026-07-28.bin` is the corresponding
2048-byte, HMAC-valid post-failure image (UID `0465B0228F2190`). It preserves the failed state
without acknowledging dirty data.

`amiibo/v3-dedede-fix1-2115-0096-2026-07-28.jsonl` is the decisive first-fix retest. The header
change let the five clear chunks and both three-chunk Dedede update attempts stage, but only the
clear completion succeeded: final diagnostics were 11 extended chunks, one completion, and two
write errors. The body reveals sector-0 page `0xB2` and sector-1 capability/data pages
`0x64/0x65`, disproving the fixed Kirby `0x92`/`0x00`/`0x01` layout.
`amiibo/v3-dedede-fix1-after-2115-0096-2026-07-28.bin` is the retained 2048-byte dirty image after
that diagnostic failure.

`BLE CAPTURE/pro2-imuref-raw-native-raw-2026-07-29.jsonl` and
`pro2-imuref-15ms-raw-native-raw-2026-07-29.jsonl` are zero-drop same-pose A/B/A controls between
raw handle `0x000A` and native handle `0x000E`. The
`pro2-native-interval-{8,10,11,14,15,16,18}-2026-07-29.jsonl` files, together with
`pro2-native-same-pose-2026-07-29.jsonl` (production six units) and
`pro2-native-15ms-2026-07-29.jsonl` (12 units), form the controlled 7.5–22.5 ms cadence matrix.
They prove the high-rate/normal/catch-up boundaries and signed accel/gyro field maps documented in
`docs/experiments/pro2-raw-native-motion-pcap-2026-07-29.md`.

`BLE CAPTURE/pro2-chart-transition-lazy-susan-2026-07-29.jsonl` and
`pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl` are reciprocal, zero-drop,
127-record genuine-Pro2 chart-transition controls. Their adjacent `.fixture.json` and `.fixture.h`
files are deterministic derivatives. They establish the state-0/state-3 cyclic carrier mapping,
refute strict smallest-three as an exact genuine model during rapid motion, and capture one
length-`0x28` prefix seam in each direction. See
`docs/experiments/pro2-carrier-chart-transition-2026-07-29.md`.

`BLE CAPTURE/pro2-chart-transition-splatoon-0-to-1-2026-07-29.jsonl` is a zero-drop
state-0/state-1 seam captured during ordinary play. The independent
`pro2-chart-transition-splatoon-3-to-1-2026-07-29.jsonl` stress capture contains a zero-drop
`3 → 1 → 0` sequence and refutes composing the state-0 seam projections into one global unsigned
permutation per state. Both are primary UART captures; see the same experiment report for the
evidence boundary.

`BLE CAPTURE/pro2-chart-transition-3-to-2-2026-07-29.jsonl` is the zero-drop
state-2 closure capture. Its 127 notifications contain 93 length-`0x1E` carriers,
34 length-`0x28` packets, and `0 → 1 → 3 → 2 → 3`. The reciprocal state-2
seams both select cyclic topology `(G2,G0,G1)` with opposite-branch signs
`(+,−,−)`. Its interleaved prefix epoch selects chart 3 with residual
`0.003833`, versus `0.196168` under chart 2. SHA-256:
`FDDD5C028E59D149A424A3226382C7FDDDF98337699677AC870DBEB0F84B2270`.

The dated `20260801-*-ds5-hybrid-*` directories contain the final translated-`0x28` fitment
campaign: byte-identical genuine control, acceleration-only, gyro-only, and the diagnostic
prefix-only failure. Each complete directory carries raw records, an audit, generated fixtures,
diagnostics, provenance, and hashes. `20260801-live-prefix-montage.png` is the compact visual
derivative showing the prefix-window camera sweep. Large raw desktop recordings were moved out of
the evidence tree into ignored `build/transient-motion-media/`, and the interrupted runner stub was
removed during closure; the lossless protocol bundles and montage retain the evidence used by the
report.

Large derived media is intentionally retained because the 2026-07-24 motion result depended on
comparing genuine Pro2 and translated DualSense behavior without requiring repeated physical
controller movement. New experiments should use dated subdirectories and document which files are
primary captures versus derived analysis.
