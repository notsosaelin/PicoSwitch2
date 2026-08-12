# Current Continuation Context

Last reconciled: 2026-08-01

**Start here if you are resuming v3 amiibo work:** `docs/Amiibo-v3.md` §18 is authoritative; the
NFC boundary section below carries the short form and the open experiment.

Working branch: `ns2-testing`

Latest published release: `v1.5.0` (2026-07-22)

Always run `git status`, `git log -1`, and read `STATUS.md` at the start of a new session. This file
deliberately does not pin the latest commit because it should remain useful after later checkpoints.
On a new workstation, clone `ns2-testing` with `--recurse-submodules`; `third_party/opus` is a
required Git submodule for Pico 2 W audio builds.

## Current validated baseline

- Pico 2 W: standard 300 MHz build with DualSense audio/native haptics.
- Pico W: validated non-audio profile.
- Pro Controller 2, NSO GameCube, Joy-Con 2 Left, and Joy-Con 2 Right personalities enumerate and
  report current firmware identities without an update prompt.
- Input, mappings, rumble, battery passthrough, wake, reconnect, LEDs, BOOTSEL gestures, and config
  persistence have broad hardware coverage; see the compatibility matrix for per-controller scope.
- Genuine Pro Controller 2 native `0x1E`/`0x28` motion passthrough, bonded HOME reconnect, P1 LED,
  gyro, and headset audio are hardware-confirmed.
- DualSense and DualSense Edge translation to the Switch 2 length-`0x1E` motion carrier is
  hardware-confirmed in Splatoon 3, including rapid movement and reconnect. The length-`0x28`
  generator and hybrid harness are **default-off, research-only, and deliberately deferred**; see
  the Motion boundary below.
- DualSense console/Windows audio, physical headset state, reconnect audio, and waveform-preserving
  native haptics are hardware-confirmed on Pico 2 W.

## Protocol laboratory boundary

- Shared workflow: `docs/re-methodology/controller-protocol-lab.md`.
- `motion_lab.ps1` captures one short stationary condition and packages `ns2_magprobe` analysis,
  baseline comparison, fixtures and hashes. Use separate no-magnet, sham, polarity, distance and
  recovery runs. The 2026-07-29 genuine-Pro2 campaign is complete. For a stimulus bracketed by
  baseline and recovery, use `ns2_magprobe.py aba ... --fraction F` to remove time-weighted drift.
  ⚠️ **The retained ring is short and it has already cost one experiment**: `pitch90` is named for a
  90° rotation but captured only the stationary period on either side of it. Start the capture
  before the motion begins and confirm the motion is inside the retained window.
- `audio_lab.ps1` is observational by default and protects the validated speaker path. `audio
  headset` reports normalized jack state without changing routing.
- `ns2_firmware_update.py` is a host-only capture model. It rejects the generic tracer's truncated
  long `0x0D/04` records; the dedicated on-device flash sink is still pending.
- Repository-local Codex skills live under `.agents/skills/`. Existing NFC/Claude workflow remains
  under `.claude/skills/`.

## Motion boundary

### Production state (unchanged and hardware-validated)

- Genuine Pro2 input transports its controller-generated `0x1E`/`0x28` PDUs opaquely.
- DualSense/Edge output synthesizes the decoded length-`0x1E` quaternion carrier only. This is the
  shipping path and is console-validated in Splatoon 3.
- The length-`0x28` generator is behind `ds5motion pdu40 on|off`, **default off**, and is NOT
  hardware-validated. With the gate off the emission path is byte-identical to the shipping build.
- The maintainer closed this campaign on 2026-08-01 because `0x28` is cadence/history packaging,
  not a higher-fidelity replacement for `0x1E`. Do not request the remaining prefix retest or tune
  the generator unless a concrete production-`0x1E` defect or a new observation point justifies
  reopening it.

### Length-`0x28` layout — decoded and byte-exact

- 40 bytes = 4-byte preamble (12-bit tick, 12-bit elapsed split across byte 1 high nibble and
  byte 2, status byte) + 288-bit payload. Every payload bit is accounted for in both layouts; there
  is no spare space and therefore no room for an unidentified lane.
- **`packing_mode` (payload bits 0-1) is the layout discriminator, NOT elapsed.** Elapsed only
  selects among the mode-3 cadence layouts. Found 2026-07-31: five corpus packets carry mode 0 with
  status `0x00`, elapsed 0 and tick 127 identical across three captures, and 13-15 non-zero bytes of
  40. Decoding them with the high-rate map produced nonsense (gyro y exactly 16384 every time) and
  had silently inflated the "high-rate" corpus by five. Their real structure is UNKNOWN.
- Mode-3 cadence layouts, selected by elapsed:
  - high-rate `0..10` ticks, status `0x0D`: accel22 / gyro22 / accel22. Acceleration has eight
    fractional bits (`/256`); gyro has seven (`/128`). Carrier lanes are s24/s23/s25 with no
    precision shift; the tail is a 16-bit temperature pair.
  - normal `11..14`, status `0x0E`: mixed 13/14-bit.
  - catch-up `15+`, status `0x0F`: accel14 / gyro16 / accel13 / gyro16 / accel14, carrier lanes
    s22/s21/s23 shifted by 2, 1-bit always-zero tail.
- Carrier lane 2 is split on the wire: its low two bits precede its signed high bits. The former
  "separate state field" was that low fragment.
- Both firmware packers are byte-exact against genuine hardware: **981/981 catch-up** and
  **853/853 high-rate**, plus edge cases the corpus never reaches
  (`build-host-test-ns2-motion-pdu40`). Fixtures are generated by `tools/gen_motion40_fixture.py`,
  never hand-written.
- **Status is written verbatim in both packers.** A `status ? status : default` idiom cannot
  distinguish a genuine zero from an unset field, and zero is real wire data — it silently rewrote
  five genuine `0x00` packets to `0x0D`.

### The 2026-07-31 hardware A/B — FAILED, and why

The console **accepted** the packets and acted on them; in-game motion was violent and erratic.
Counters were healthy throughout (50.6 packets/s, 19.75 ms cadence, `emitted_len` 40, zero
saturation), so the failure was entirely in semantics no test covered. Two root causes:

1. **The prefix described the wrong instant.** The `0x28` prefix is a modular slice of the same
   orientation carrier `0x1E` sends outright, so a generator must choose *which moment* it
   describes — a question the byte layout never asks, which is why byte-exactness, range checks and
   round-trips all pass either way. The firmware passed the CURRENT carrier. Measured across 24
   interleaved captures and 773 paired packets, a genuine prefix carries the orientation at
   `tick − elapsed + 4` ticks. At the right epoch the reconstruction error is 0.00023 deg; at
   `tick` the moving captures degrade 20–80× (0.039 → 0.901 deg). Stationary captures barely move,
   which is exactly why every stationary comparison had passed. FIXED: the carrier is snapshotted
   per sample into the ring and the builder selects the entry nearest the window start plus the
   lag; `ns2_ds5_motion40_build()` no longer takes a carrier at all.
2. **Catch-up was the wrong layout to target.** Of the 773 genuine `0x28` packets that have a
   `0x1E` alongside them to validate against, **768 are high-rate and 2 are catch-up** — catch-up
   appears almost only in `0x28`-only captures, which carry no carrier by definition. Catch-up had
   been chosen because its tail is one always-zero bit rather than a Q3 temperature pair a
   DualSense cannot measure: ease of filling over strength of evidence. FIXED: emission is now
   interleaved high-rate.

Retargeting closed three open questions structurally rather than by experiment — a `0x1E` rides
alongside and supplies the chart state the modular prefix needs; each `0x28` reaches the console
once between carriers instead of ~20 times at a 1 kHz poll; and at elapsed 7–8 the two candidate
epoch models agree within one tick instead of the 9 they differ by at a catch-up cadence.

### The 2026-08-01 interleaved hardware A/Bs — both FAILED

Correcting high-rate gyro to `/128` made the old interleaved build less violent, but it remained
severely degraded: stationary aim jumped and worsened over time, movement stayed jittery, and yaw
lagged. During the live screen/UART interval the generator added 1809 packets, 102 starvation
events and 32 overlong windows with zero saturation. An immediate eight-second `0x1E` control was
stable.

The retained genuine moving stream then exposed a structural defect: 255 consecutive native PDUs
(185 `0x1E`, 70 `0x28`) share one 12-bit clock, and all 254 comparable elapsed fields equal the
tick delta from the immediately preceding PDU. The generator instead gave `0x28` a second clock
starting at zero, inserted it for one 1 ms USB poll, and returned to freshly advancing `0x1E` on
the next poll. This alternated incompatible epochs and ownership cadences.

The first offline-green corrective scheduler stores the proven `0x1E` tick with every sample,
selects both lengths on one native-rate PDU timeline, rewrites selected carrier elapsed from the
same boundary, and holds the selected PDU across intervening USB polls. A host test pins
`0x1E → 0x28` tick/elapsed continuity, cadence, and hold behavior.

Hardware then rejected that corrected scheduler too. Its live signature was internally healthy:
358 batches, 1088 carriers, 10140 held polls, one fallback/starvation, zero overlong windows, and
zero saturation over roughly ten seconds. Nevertheless, a stationary screen timeline swept
through large camera rotations; disabling the gate immediately restored a stable `0x1E` control.
This proves the shared timeline was necessary but not sufficient.

The next candidate was carrier/sample coherence: the rejected builder selected one noisy midpoint
gyro reading while `0x1E` integrated the complete window. Its tick-weighted replacement was flashed
and also failed. More decisively, forcing all gyro axes to exact zero still produced stationary
rotations in the interleaved mode. The same zero-gyro source was stable with isolated `0x28`
delivery, and a fixed-rate probe proved those isolated packets were consumed rather than ignored.

The live seam then exposed another real factor. `input status` reported stationary DualSense Edge
acceleration near `[-21,-669,4059]`: `ns2_motion_seam_apply()` had already converted native
DualSense 8192-count/g acceleration to the Pro2 frame at 4096 counts/g. The high-rate builder
halved it again and therefore emitted about 0.5 g. Correcting that to physical 1 g was still not
enough: a new closed-loop fixture found that the hardware-validated `0x1E` path applies an output
calibration of `68963 / 65536 = 1.0522918701171875`, while the first corrected `0x28` path used
gain 1.0. The same acceleration vector therefore jumped by **5.23%** at every representation seam.

`tools/test_ns2_motion40_coherence.py` now compiles the real C translators against an analytic
800 Hz three-axis trajectory, independently decodes their wire output, and closes every
`0x1E -> 0x28 -> 0x1E` loop. It checks shared time, carrier and prefix epochs, gyro area, two
acceleration samples, gravity direction/magnitude, and representation continuity. Six negative
controls prove it rejects stale prefix, half acceleration, half gyro, swapped gyro axes, detached
elapsed, and a stale following carrier. The default `live` path now matches the established `0x1E`
acceleration gain; `half` intentionally reproduces the original 0.5 g defect and `zero` remains a
diagnostic. This is offline proof of internal physical coherence, not proof of Nintendo's private
filter or console acceptance. The weighted gyro remains because it is coherent, but is documented
as refuted as the primary stationary cause. See
[`ds5-pdu40-interleaved-hardware-2026-08-01.md`](../experiments/ds5-pdu40-interleaved-hardware-2026-08-01.md).

### The 2026-08-01 genuine-base hybrid bisection — accel/gyro pass, carrier ownership localized

The default-off `motionhybrid` harness now starts from each exact Nintendo-authored PDU and records
the base plus emitted XOR. The byte-identical control produced 95 unchanged records and no visible
change. Acceleration-only applied 14 high-rate packets with zero drops/saturation and stayed stable.
Gyro-only also stayed stable; a synchronized 30 fps Display 3 recording measured less than one
pixel of coherent displacement through the 650 ms substitution window.

The first prefix-only run produced the familiar violent camera sweep, but the capture changed the
diagnosis. It sent 19 DualSense-derived `0x28` prefixes between 68 untouched genuine `0x1E`
orientation carriers, with five additional genuine `0x28` fallbacks during short donor gaps.
History decode put each donor prefix only `0.001..0.072°` from the genuine prefix. The failure was
therefore repeated switching between two absolute-orientation histories, not a gross prefix value.

Prefix mode now owns the orientation group across the entire interleaved sequence: it replaces the
exact prefix mask in both `0x1E` and mode-3 `0x28`, including normal/catch-up layouts, while leaving
timing, status, packing, flags, IMU fields, and tail genuine. Short donor gaps hold the latest donor
orientation rather than reverting packet-by-packet. Host tests prove mask isolation and cross-length
ownership; both boards and all 57 compiled host tests pass. This correction was not flashed before
the campaign was deferred and is retained only as a future forensic tool. Full report:
[`ds5-motion-hybrid-harness-2026-08-01.md`](../experiments/ds5-motion-hybrid-harness-2026-08-01.md).

### Other defects found and fixed offline

- **Slots did not span the emit window.** Filling from the first samples to arrive covered only the
  head of the window and discarded the freshest ~40%. Genuine packets run oldest slot to newest:
  across 973 catch-up packets the seam `a2[N] → a0[N+1]` is the *smallest* gap in the stream
  (0.572 of a full window) against a within-packet `a0 → a2` of 0.866, strictly monotone in slot
  index, paired sign test z = +10.2 over 894 pairs.
- **`elapsed` used a generator-local clock.** Interleaved `0x1E`/`0x28` is one PDU timeline.
  Every source sample now retains the proven carrier tick; the last selected PDU tick determines
  encoded elapsed, while `last_sample_us` separately bounds the sample-selection window so no
  physical sample appears in two packets.
- **The sample ring was sized on a wrong source rate.** A DualSense supplying a valid sensor
  timestamp takes the ungated path in `ns2_ds5_motion_update()` — the 3800 µs period gate only
  guards the host-time fallback — so samples arrive at the controller's IMU rate. Measured on
  hardware: 9,721 samples in 12.7 s = **~763 Hz**, not the ~250 Hz a report-rate assumption
  suggests. The 16-entry ring held only one window and evicted the oldest samples; it is now 64.
- **An overlong window is dropped, not clamped.** Elapsed selects the layout, so a window past the
  10-tick high-rate ceiling would be decoded with the wrong field map. Counted as `overlong`.
  Genuine hardware switches layout there instead — that is what the layouts are *for* (the
  maintainer's loss-compensation reading, confirmed) — and implementing that switch is open work.
- **High-rate gyro was decoded with acceleration's binary point.** The sensor/common gyro remains
  the documented and directly confirmed `16.4 counts/dps`, but the high-rate gyro lane uses seven
  fractional bits (`/128`) while its acceleration lanes use eight (`/256`). The old conversion
  recovered only a `0.554` median of the carrier rotation; the corrected existing-corpus result is
  `1.108`, with two captures independently at `1.000` and `0.994`.
- **Acceleration crossed the representation seam at two different output gains.** The shared input
  is already in Pro2 4096-count/g units; the high-rate builder first halved it again, then its first
  correction used bare physical `input * 256`. The validated `0x1E` carrier uses
  `input * 68963` in Q16.16, so bare-Q8 `0x28` was still 5.23% lower. The default high-rate path now
  rounds `(input * 68963) / 256`, giving both representations the same calibrated vector.

### Refuted — do not revisit without new evidence

- **Static-template `0x28` generator** (genuine static body + dynamic timing/G6/G7/G8). Caused
  immediate random motion because it corrupted fragments of real packed gyro/accel samples.
- **`0x28` as next-generation replacement for compatibility `0x1E`.** Both lengths are native
  Switch 2 Pro Controller PDUs on one carrier trajectory. Switch 1 compatibility is the separate
  report-`0x30` protocol.
- **Magnetometer lane / "9-axis".** Bit accounting leaves zero unallocated bits in a `0x28`, and a
  controlled magnet matrix with polarity, distance and sham controls found no response. The former
  G6/G7/G8 aliases cross real packed gyro and accel ranges. Separately, the genuine `0x1E`
  quaternion has substantial native time drift with pose unchanged — a true 9-axis fusion would not
  drift in yaw — so if a magnetometer exists in the part it drives nothing observable.
- **Byte-exact validation of a GENERATED `0x28`.** Impossible in principle from BLE captures, not
  merely hard: the controller transmits derived products, never its inputs. The internal 800 Hz IMU
  stream is not sent (handle `0x000A` runs at the notification rate, ~133 Hz), and the carrier at
  the prefix's epoch instant is not sent either — across 449 genuine packets the epoch coordinate
  landed on a transmitted `0x1E` exactly **zero** times.
- **`|q| = 1.0000` as evidence the carrier decode is right.** Circular:
  `decode_motion30_orientation` reconstructs the omitted component as `sqrt(1 − retained_energy)`
  and then normalises, so the norm is 1 by construction regardless of scale. The non-circular check
  is `retained_energy`, capped at 0.75 for true smallest-three; measured max over 1226 carriers is
  0.524, so the `sqrt(2)` scale is not refuted but only 70% of the ceiling is exercised.
- **The repeat-tolerance hypothesis** (that resending each `0x28` ~20× caused the erratic motion)
  was never tested and is not the established cause. The fill is runtime-selectable
  (`ds5motion pdu40 fill empty|repeat|carrier`) so it remains testable, but the two proven root
  causes are the epoch and the layout.

### IMU constants — audited 2026-08-01 against physical references

Full record: `docs/experiments/pro2-imu-constants-audit-2026-08-01.md`.

- ✅ **Acceleration 4096 counts/g stands.** At rest the corpus reads 4307.5 counts = 1.052 g under
  that constant, and that figure was already in the repository — the scale-normalization work
  recorded all eight slots "agreeing on 1.051–1.052 g" and treated the agreement as success. Slots
  agreeing with slots validates *relative* wire scaling and can never reveal a common absolute
  error. A sphere fit over all orientations separates the two: radius 4070.2 (within 0.6% of 4096)
  plus a **254-count accelerometer bias, mostly +Z**. Consequence: the earlier "1.0517 g genuine vs
  1.0116 g synthetic" comparison scored a biased genuine reading against an unbiased synthetic one,
  and the synthetic was the closer of the two to a true 1 g.
- 🔵 **Tick 1257 µs (795.5 Hz)** measured against the assumed 1250 µs, three captures agreeing
  within 0.15%. As likely the host clock as the crystal; 800 Hz is nominal-and-confirmed-to-0.6%.
- 🔵 **DualSense 8288.7 counts/g** at rest against 8192 assumed. The halving into Pro2 counts is off
  by 1.8%; negligible.
- ✅ **Gyro factor-of-two is resolved without another capture.** The ICM-42670-P datasheet plus the
  retained genuine-controller `0x05` result establish `±2000 dps / 16.4 counts/dps`; the guided
  sibling normal-layout analysis independently measures 16.16–16.54. Therefore the bad product was the
  high-rate wire factor: gyro is `/128` (seven fractional bits), not acceleration's `/256`. The
  same four existing moving captures now recover carrier rotation ratios `1.000`, `0.994`, `1.215`,
  and `1.325` (median `1.108`) instead of `0.500`, `0.497`, `0.608`, and `0.663` (median `0.554`).
- 🔵 **The temperature tail's layout is sound but its semantics are unsupported.** The integer part
  spans 3..10 over 1002 packets; a die at 3–10 °C is not plausible. ICM parts report an offset from
  25 °C, which would give 28–35 °C. The decoder no longer presents it as Celsius. The firmware
  replays the modal genuine value `0x01C0` rather than computing one.

### Historical readiness gate — required only if the campaign is explicitly reopened

`python tools/ns2_motion40_validate.py` exits non-zero unless every capture-answerable question
passes, and prints what it CANNOT answer as loudly as what it can. Its offline checks pass, but the
fully coherent recipe still failed on hardware. Passing this gate is therefore necessary and not
sufficient; it does not justify another flash by itself.

It exists because three "offline-validated" claims preceded the hardware failure. Two scoring
decisions inside it are load-bearing, both found by the gate initially getting them wrong:

- it scores the epoch on the **worst capture, not the pooled median** — a wrong epoch only
  misplaces orientation while rotating, and most of the corpus is stationary, so by median it
  reports 0.0006 deg for the epoch that failed on hardware and would PASS the broken firmware;
- it scores against the **achievable floor as a ratio**, not an absolute angle, because the floor is
  set by interpolating a ~133 Hz carrier and is itself 0.056 deg during fast rotation.

Supporting measurements, each re-runnable: `tools/ns2_motion40_prefix_epoch.py` (when the prefix
describes), `tools/ns2_motion40_slot_timing.py` (where slots sit in the window, plus the layout
band table), `tools/ns2_motion40_gyro_axes.py` (gyro axis, sign and scale).

### Known limits of the deferred path

- 🔵 The console-private coupling between orientation history, packed IMU history, filter/FIFO
  state, and cadence remains unresolved. A tick-weighted replacement, shared clock, corrected
  acceleration gain, and internally coherent transitions all passed offline but the complete
  recipe still failed on hardware.
- 🔵 The corrected sequence-wide prefix hybrid is host/build validated and intentionally not
  hardware-tested. It is not queued work.
- 🔴 Mode-0 packet structure, and the meaning of `status = 0x00`.
- 🔵 Prefix epoch model: fixed lag (~3 ticks) or window-relative (`elapsed − 4`)? Indistinguishable
  because elapsed is 7 in almost every paired packet. Irrelevant at the emitted cadence, where they
  agree within one tick.
- 🔵 Gyro X/Y axes are not individually disambiguated by a pure reference rotation. This is not a
  new-capture request or a release blocker: the translator inherits the console-validated `0x1E`
  axis map, while the lazy-susan/return pair confirms the shared Z frame and sign.
- 🔵 Accelerometer bias vs scale would be settled decisively by at-rest captures in two or more
  orientations. Every at-rest packet in the corpus is in one pose.
- ⬜ Layout switching on gaps, which is what the `overlong` counter exposes.

### Routes tried that did NOT work, with reasons

- **DualSense as a transfer standard for the gyro constant.** Correct in principle — 16.384
  counts/dps is externally documented and implicitly validated by the working `0x1E` path — but the
  `ds5-pro2-paired-*` captures are not co-moved: `ds5_age_us` walks from 1,100 to 61,365 µs, so one
  DualSense sample repeats for up to 61 ms while the Pro2 updates continuously, and the counts/dps
  ratio at fresh pairs scatters from 7.9 to 33. `-yaw` contains zero records.
- **Raw IMU as an angle reference.** Handle `0x000A` reports ordinary gyro counts, but every capture
  holding it is stationary (max 4 counts). Zero-rate bias can bracket field placement, not determine
  an absolute angular scale or distinguish seven from eight fractional bits.
- **Existing known-angle captures.** The chart-transition captures sweep 41–92° but start and end
  mid-motion, so neither endpoint gives a clean gravity reference.

## NFC boundary

- Config mode accepts strictly validated 540/572-byte NTAG215 and 2048-byte
  NTAG I2C Plus 2K images through transactional 32-byte chunks.
  Virtual Amiibo is always available: a blank store presents no virtual tag, while a loaded image
  automatically owns the virtual source. The adapter's internal baseline/latest-written recovery
  pair remains implementation-only; the portal does not expose reset-to-original behavior.
- The production portal library remains available without an adapter connection. It accepts one
  file or recursively scans a selected directory and caches one record per AmiiboAPI catalog ID
  with one mutable dump in browser-local IndexedDB. v3 rider/machine variants use content-derived
  keys so distinct combinations sharing one catalog identity remain available without duplicate
  copies of the same dump. The shared catalog is downloaded and stored once, then matched
  locally, so original tag bytes/IDs/UIDs/save data never leave the browser.
- The production carousel starts empty and displays only user-imported records. Directory scans
  add those records to the visible library progressively. AmiiboAPI order remains authoritative;
  each compact filter chip cycles `All`, then only the imported library's available values
  alphabetically.
  Centered artwork stays fixed in the middle at 100%; four non-overlapping neighbors on each side
  are exactly 80/60/40/20%. Selection changes animate without rebuilding the track, and carousel
  names are omitted. Mouse wheel/trackpad while hovered, horizontal touch/pen swipe, and keyboard
  arrows navigate; visible arrow buttons and the position counter are intentionally absent. The
  three filter chips sit together below the compact primary action.
- **Sync amiibo** retrieves the latest active image, validates its raw tag identity, overwrites the
  loaded or UID-matching cached entry, and acknowledges firmware dirty state only after IndexedDB
  confirms the write. The AmiiboAPI catalog enriches the record but never gates it. Browser
  security does not permit silently overwriting the original OS file.
- `web/diagnostic.html` and `tools/run_amiibo_portal_test.ps1` provide the same library/metadata
  workflow plus a browser-only simulated adapter, transactional upload, controlled write,
  persistence, cache-first save-back, and self-test without Web Serial or hardware.
- Both portals use an artwork carousel. The production manager shows centered save facts below the
  artwork; clicking the centered amiibo toggles a non-modal context drawer with friendly catalog
  metadata, compatible games, and secondary/destructive actions.
  `tools/run_config_portal.ps1` serves the real portal from the same localhost origin.
- The board stores exactly one amiibo (the alternating flash banks are persistence generations,
  not two active amiibo). The manager is single-slot: connected **Load amiibo** uploads the
  highlighted entry; offline **Select amiibo** remembers it. A conditional **Import amiibo** or
  **Sync amiibo** action pulls an unknown or console-written adapter image into IndexedDB. The
  merged button is always labeled **Eject amiibo**; `amiiboEjectActionState()` and its tooltip
  choose between loaded-pointer-only removal, confirmed adapter wipe, or both. Cancel aborts
  everything; library dumps are never deleted; the console-driven Stop/write-back lifecycle and
  `amiibo present` re-activation are unchanged.
- Virtual Amiibo library is **import-only** (single file or recursive directory of the user's own
  genuine dumps). Amiibo crypto is enforced (2026-07-26 hardware test,
  [[amiibo-identity-and-generation]] / `docs/experiments/generated-amiibo-console-rejection-2026-07-26.md`):
  the Switch 2 rejects key-free generated images ("This isn't an amiibo"). Random Mode was removed
  (UID-bound HMAC). A key-based identity generator was removed for import-only simplicity. The
  narrower browser-local Initialize operation remains: it requests the user's own
  `key_retail.bin`, clears/re-signs an imported ordinary or v3 dump, self-verifies, and works
  without an adapter. Alternate-UID copies are feasible only through a complete keyed
  re-sign/re-encrypt path and are not implemented.
- Library export/import is a flat **.zip** (`library.json` manifest + one `.bin` per amiibo) via a
  self-contained store-only ZIP writer/reader (`amiiboZipStore`/`amiiboZipEntries`, deflate fallback
  via DecompressionStream); import reconstructs from the `.bin` files and legacy `.json` still
  imports. Import is structural-only: `coerceAmiiboImport()` accepts 540/572 and larger
  emulator-container dumps. Exact 2048-byte figure-v3 images remain whole with their contiguous
  UID layout; 540/572-byte NTAG215 inputs retain their native layout and BCC checks. The AmiiboAPI
  catalog is enhancement-only and never gates import, so brand-new amiibo not yet in AmiiboAPI
  import fine.
- Carousel navigation wraps at both ends (`moveAmiiboCarousel`) while the visible neighbor window
  uses real non-wrapping indices for clean slides; the centered amiibo's release date shows above
  it. Compact search remains because the owned library exceeds 900 entries. Game series, Amiibo
  series, and product type are tap-to-cycle chips. One center slot conditionally shows
  Load/Select/Import/Sync; connection status follows the adapter's active cached entry once without
  hijacking later browsing. Download/Initialize/Eject/Delete and compatible-game details live in
  the centered item's inline drawer. The **Scan physical amiibo** control stays hidden until
  firmware exposes a capability-reported Config-transport API; UART `nfc_probe` alone is not a
  production contract.
- Kirby Air Riders "Figure Player" amiibo (**v3** = NTAG I2C Plus 2K, 2048 bytes) are ✅
  **READ/WRITE CONFIRMED on hardware as of 2026-07-28**. Authoritative detail is
  `docs/Amiibo-v3.md` §19 and
  `docs/experiments/v3-full-sram-response-validation-2026-07-28.md`. An untouched downloaded
  Kirby/Warp dump completed the full sequence with no signature override: three device results,
  targeted read, six encrypted `0x14` write chunks, `0x08` commit, `05 00`, and zero write errors.
- Layout, re-measured 2026-07-27 by diffing all 16 dumps: **rider identity is in the encrypted body;
  machine identity is entirely in the SRAM block `0x3C0..0x3FF`, outside amiibo crypto.** A rider's
  four machine variants are byte-identical except for 21–22 bytes there, and all four share one UID
  (they are one physical figure reconfigured four times). Machine fields = an ASCII code
  (`"PB4W717"` Warp/Winged, `"PB5T432"` Shadow, `"PC6V628"` Tank) plus an `01 01 0X` byte; the
  12-byte blob at `0x3C2` is per-unit.
- The v3 write path is hardware-confirmed. Capture-derived classification keeps the 74-byte
  `01 01` device command on the `0x21` path while `01 06` starts the six-chunk, 454-byte data
  transaction completed by `0x08`. The full stored SRAM response is preserved across writes.
  Owner/format write, Stop/eject, next-scan readback, HMAC-valid export, flash persistence, and
  power-cycle recovery have passed; only production-portal Sync of the retained dirty generation
  remains.
- A genuine Pro Controller 2 plus physical Kirby & Warp Star positive control captured the missing
  Air Riders protocol. `0x20` accepts two sector-aware record envelopes: 355 bytes clears
  sector-0 pages `0x92..0xE1`; 167 bytes updates page 4, sector-0 pages `0x92..0x99`, and
  sector-1 pages `0x01..0x18`. Record count is at byte 22 and records are
  `(sector,page,length,data)`. Genuine completion is bare ACK followed by empty status `0x16`,
  selected-UID page-3 read, then the ordinary 454-byte/`0x08` write. The post-write physical
  snapshot exactly matches the 32-byte sector-0 record. Firmware now implements both shapes,
  generation-safe journaling without intermediate ejection, and `0x16`; 53 host tests, both board
  builds, magnetometer tests, and install markers pass. The prepared build's ordinary System
  Settings read/write control passed with zero write errors and a valid exported image. Later
  bullets record the completed Air Riders lifecycle and its follow-up corrections.
- The apparent initial-read regression on that prepared build was an invalid test image, not code:
  historical `v3-write-output-2026-07-28.bin` calculates SRAM CRC `7AC4` but stores `E511`.
  Replacing it over UART with corrected capture-rebuilt CRC32 `8D337603` restored read/write
  immediately without a reflash.
- The first prepared-build Air Riders run reached the 355-byte clear, `0x16`, page-3 read, six
  ordinary chunks, `0x08`, and `05 00` with zero errors, then failed `2115-0096`. Exact trace
  comparison showed why: genuine hardware reports the same tag present about 130 ms after the
  first Stop so the console can send the 167-byte stage; PicoSwitch2 returned cooldown `07 41`.
  The next build suppresses auto-eject only for this clear-to-update checkpoint, expires abandoned
  sequences after five seconds, and restores normal eject after the update checkpoint. Host tests
  cover timing and wrap.
- That lifecycle build completed the full Air Riders operation on hardware. Diagnostics ended at
  18 ordinary chunks, three `0x08` commits, eight extended chunks, two `0x20` completions, and zero
  write errors. The exported 2048-byte image is HMAC-valid, keeps SRAM CRC `7AC4/7AC4`, and
  contains the captured sector-0 and sector-1 game data. Primary files:
  `dumps/amiibo/v3-extended-two-stage-success-reuse-freeze-2026-07-28.jsonl` and
  `dumps/amiibo/v3-extended-two-stage-success-output-2026-07-28.bin`.
- Reusing that written image then froze because virtual NFC did not stage subcommand `0x1E`.
  Genuine capture
  `dumps/amiibo/genuine-kirby-warp-reuse-sub1e-usb-2026-07-28.jsonl` proves `0x1E` itself returns
  a bare ACK, then changes to empty state `0x15` and exposes a 196-byte result through `0x15`
  chunks. The result is a 64-byte prefix plus sector-0 pages `0x92..0x99` and sector-1 pages
  `0x00..0x18`. Sector-1 page 0 read as chip metadata `A5 00 01 00` after the first Air Riders
  update even though portable dumps leave that slot zero. The prepared build reproduced all 196
  bytes in a host fixture and passed
  53 host tests, both board builds, eight magnetometer tests, and both reset-marker checks.
  Hardware then validated the entire `0x1E` path: state `0x15`, three chunks, and Stop.
- After the validated reuse read, Air Riders immediately sent another 167-byte update. Trace
  `dumps/amiibo/v3-reuse-sub1e-write-freeze-2026-07-28.jsonl` first exposed
  `A5 00 02 00`; a temporary page-4 comparison let the update complete, but its persisted result
  was called corrupted on the next `0x1E` read.
- Decisive positive controls:
  `dumps/amiibo/genuine-air-riders-existing-data-read-write-2026-07-28.jsonl` is a complete
  genuine read/write cycle, and
  `dumps/amiibo/genuine-air-riders-postwrite-read-only-2026-07-28.jsonl` reads that same physical
  tag afterward without writing. A second pair,
  `genuine-air-riders-second-existing-data-write-2026-07-28.jsonl` and
  `genuine-air-riders-second-postwrite-read-only-2026-07-28.jsonl`, repeats the result.
  Together they prove the header is the next chip-managed sector-1 page-0 value, not a page-4
  echo: genuine `0x1E` advanced `A5 00 01 00 → A5 00 02 00 → A5 00 03 00`, while page 4
  independently advanced `03 → 04 → 05` and the explicit sector-1 record still began at page 1.
  The source
  validates/retains that implicit state at image offset `0x400` and serves it dynamically;
  zero-filled ecosystem images retain the generation-1 fallback. Both board builds, all 53 host
  tests, all eight magnetometer tests, and both install-reset markers pass. Hardware then completed
  a virtual update to sector-1 page 0 `A5 00 02 00`; the exported 2048-byte image was HMAC-valid
  and retained the customized figure state. Its immediate second reuse was accepted by Air Riders,
  which loaded the saved custom color. Captures:
  `dumps/amiibo/v3-dynamic-sector1-page0-write-2026-07-28.jsonl` and
  `dumps/amiibo/v3-dynamic-sector1-page0-second-read-success-2026-07-28.jsonl`. A physical
  adapter power cycle restored the exact generation-4 image with CRC `91A6178B`; the clean
  read-only trace
  `dumps/amiibo/v3-dynamic-sector1-page0-powercycle-read-success-2026-07-28.jsonl` again served
  `A5 00 02 00`, and Air Riders accepted it. The dynamic-state persistence lifecycle is fully
  hardware-confirmed.
- A non-cosmetic save after completing an Air Riders level is captured in
  `dumps/amiibo/v3-air-riders-learned-state-save-2026-07-28.jsonl`, with exact before/after images.
  It uses the same 167-byte extended update plus ordinary six-chunk commit, advances generation
  7 → 9, page 4 `A5 00 05 00 → A5 00 06 00`, and sector-1 page 0
  `A5 00 03 00 → A5 00 04 00`. The 552 changed bytes are confined to the modeled ranges:
  423 in `0x000..0x247`, 32 in `0x248..0x267`, one generation byte at `0x402`, and all
  96 bytes at `0x404..0x463`; nothing after `0x463` changed. The output is HMAC-valid, persisted,
  dirty, and recorded zero write errors. No new command or storage shape is needed.
- King Dedede & Tank Star proves Air Riders storage is allocation-relative. It uses sector-0 page
  `0xB2` and sector-1 capability/data pages `0x64/0x65`; Kirby uses `0x92` and `0x00/0x01`.
  The header-only fix accepted all three chunks, but its two `0x20` completions still failed
  because commit validation retained Kirby's pages. The prepared codec derives pages from the
  record envelope, bounds sector 0 to the proven clear window and sector 1 to the tag, tracks
  capability generation at the selected page, and makes `0x1E` fallback descriptor-relative.
  No UID/product table exists. All 16 available Air Riders v3 dumps completed both reads and
  writes on a real Switch 2. Both board builds, all 53 host tests, portal suites,
  motion/magprobe checks, and both install markers pass. Evidence:
  `docs/experiments/v3-air-riders-dynamic-allocation-2026-07-28.md`.
- ⚠️ The "provenance" conclusion (that a v3 image needs its own machine's SRAM block and signature)
  is **RETRACTED** — see `docs/Amiibo-v3.md` §18.1a. That experiment moved three variables at once;
  the downloaded dump was never retried after `prefix[18]=0x06` moved into `ns2_v3_build_buffer()`,
  which alone explains every earlier rejection. **Retail keys are ruled out as the discriminator:**
  all 16 downloaded dumps *and* the accepted rebuilt image verify HMAC-VALID
  (`node tools/verify_amiibo_crypto.mjs <path>`), and the firmware serves each image's own UID via
  `ns2_amiibo_v3_uid()`, so the UID/key binding is never broken.
- The signature/carrier hypothesis is refuted. The successful downloaded image used
  `signature_set=false`; `key_retail.bin` was needed only for offline evidence verification. The
  actual blocker was truncating the 64-byte SRAM response to 32 bytes and forcing the captured
  figure's CRC `7A C4`. The firmware now carries all of `image[0x3C0..0x3FF]`; downloaded
  Kirby/Warp ends `E5 11`, Meta/Shadow ends `30 61`. `amiibo v3sig` remains diagnostic only.
- Corrected capture-rebuilt baseline: `dumps/kirby-warpstar-rebuilt-from-genuine.bin`
  (CRC32 `8D337603`, UID `049011CADB1F90`, SRAM CRC `7A C4`).
- `amiibo status` now reports the active v3 image's 2048-byte size, contiguous UID, generation,
  dirty/persisted state, and payload CRC. Config and UART reads route to the v3 store, and the
  portal uses UID plus CRC to identify the exact rider/machine variant. Sync replaces the old
  content-keyed IndexedDB record only after the complete updated image validates.
- Sync clears dirty-write protection only after IndexedDB persistence; it does not unload the tag.
  Console formatting/reset remains the authority.
- The USB side of Config mode is now CDC-only. The MSC descriptor/callbacks, generated
  `src/web_disk.h`, embedded FAT image, `CONFIG.HTM`, and generator were removed. The config
  VID/PID and CDC command protocol remain unchanged; `tools/run_config_portal.ps1` is the
  production entry point. Config enumeration, Virtual Amiibo transfer, save/readback, and direct
  BOOTSEL exit are hardware-confirmed with the CDC-only USB descriptor.
- That same portal now supports a Config-personality-only BLE GATT transport. Controller discovery
  stops before low-duty management advertising; the incoming Peripheral-role link is classified
  before HID/GATT-client setup; and a bounded cross-core bridge executes only production
  settings/Amiibo commands through the existing core-0 parser. Leaving Config disconnects the
  browser before discovery resumes. Normal controller personalities perform no management radio
  work. All 53 host tests, three firmware build axes, and portal static checks pass; Config BLE
  hardware validation is pending.
- Config schema v10 removes the Virtual Amiibo flag and controller-family mapping tables. A locked
  base button map feeds the emulated Nintendo identity, leaving persistent button remapping to the
  Switch. The shared Pro2 body/Sony lightbar color and independent Joy-Con accent controls remain
  together in one compact production-portal panel and in config storage. The obsolete visible
  current-input/current-output cards are removed. Each
  flashed UF2 contains a one-shot marker that erases the five persistence sectors (settings, both
  amiibo banks, wake identity, and BTstack bonds) before normal startup; an ordinary reboot sees
  the consumed marker and retains state. Both BLE-visible names are `PicoSwitch2`.
- BOOTSEL now has a host-tested action matrix: single-tap cycles only controller personalities
  when a controller is HID-ready; double-tap opens pairing; triple-tap wipes/disconnects; and a
  two-second hold enters Config directly. Config ignores single/double, permits triple-tap wipe,
  and uses the same hold to return directly to Pro2. Paired double-tap first disconnects without
  deleting the bond. Both board builds pass; this revised physical gesture matrix awaits hardware
  validation.
- Sectors `-3` and `-5` hold alternating version-2 snapshots with generation, header/payload CRC,
  internal baseline/latest-written images, selection, dirty state, and optional signature. The previous bank stays
  valid until the new bank is programmed and verified. Flash work runs only through the existing
  core1 config-save service after the install-reset first boot.
- A genuine Pro2 physical-tag read through the UART-gated bridge was recognized by the Switch.
  Primary capture proves USB uses a 600-byte reader buffer fetched as eight 70-byte chunks plus one
  40-byte final chunk; it does not request one 622-byte payload.
- The always-available virtual runtime handles the confirmed read flow after a tag is loaded. A real Switch 2
  recognized an uploaded Virtual Amiibo using a non-NFC source controller.
- With a stable native write capture unavailable, a conservative transactional virtual write path
  was reconstructed from the local command examples, existing primary read/state captures, and the
  pinned capture-derived Dycool codec: exact-UID `0x06`, 64-byte write preparation, bounded
  454-byte `0x14` staging, atomic `0x08` commit, generation-safe store update, dirty state, and a
  modulo-eight report event counter. A real-console write completes through `0x08` and accepted
  `05 00` without a crash. Each commit updates/selects the internal latest-written image and queues
  persistence.
- The first write test crashed the Switch with `2168-0002`. Root cause was exact and local:
  `0x14` totals 88 bytes, while `ns2_task` dispatched each 64-byte `tud_vendor_read` result
  immediately. `ns2_vendor_rx` now reassembles the envelope-declared command across the observed
  64+24 split, handles arbitrary/coalesced/oversized framing, and resets on USB mount. The rebuilt
  UF2 is hardware-confirmed not to crash.
- The successful retest trace shows the console received `05 00`, sent Stop, then rescanned once
  per second because the retained RAM image still appeared physically present. The runtime now
  separates retained image state from presentation. A committed Stop waits for the pending flash
  snapshot, emits logical removal only after verification, and keeps the latest-written image selected for
  saving. The next `0x03` scan re-presents that same updated image as a fresh tag. The removal and
  re-presentation lifecycle, persistence gate, and power-cycle recovery are hardware-confirmed.
- The production portal retains its cached library without an adapter connection. Prior
  hardware/browser validation confirms cache-first writeback and export/clear/import restoration
  of the complete versioned library backup. The current one-dump/single-loaded-pointer manager is
  host/static-regression clean; its manual Eject/Present path still awaits real-console validation.
- USB CDC remains unavailable while the Pico is attached to the console. A two-second BOOTSEL
  hold can now enter Config there and expose the local portal over BLE for settings and Virtual
  Amiibo management. UART remains the independent live-console research/export path during a
  normal controller personality; `amiibo status/read/acknowledge` and `amiibo dump -OutputPath`
  retain generation, exact-size, and UID/BCC validation before dirty acknowledgement.
- Hardware validation captured committed write → `05 00` → Stop → absent `07 41`, followed by a
  later scan, the same UID, and a complete updated read. UART status changed from clean generation
  4 to dirty generation 7; export produced a valid 540-byte image with 426 changed bytes confined
  to writable ranges and cleared dirty only after saving.
- Normal 540-byte files do not contain the NTAG `READ_SIG` result. The successful native buffer's
  signature field appeared zeroed, and the real console accepted the virtual reader path; the
  original hardware test did not record whether its selected upload was 540 or 572 bytes, so
  per-format signature compatibility is not yet distinguished.
- Native Pro2 relay writes `0x0016`, subscribes to `0x001E`, and receives matching ordinary NFC
  replies on `0x001A`. It remains UART-gated and read-only pending production/reconnect/removal and
  write validation.
- A 2026-07-25 Smash native-write attempt produced no `0x14`, `0x08`, or UID-bearing `0x06`
  because the multi-amiibo device changed UID between presentations. It cannot validate native
  writes. Its `4,5,6,7,0` progression independently supports the implemented modulo-eight event
  counter.
- Switch 1 Pro/Joy-Con Right NFC uses report `0x31` plus its MCU protocol and requires translation,
  not raw forwarding.

## Highest-value open work

1. Extend `ns2_command_atlas.py` to the controller-side `blecap` schema while retaining transport
   provenance. The current scan found 42 zero-loss console-side traces/30 command pairs, but only
   two of 29 zero-loss genuine BLE captures contain command traffic, both the same initialization
   path. Use the completed atlas to choose one missing behavior for a passive or reversible A/B;
   do not begin by collecting another broad capture.
2. Run production-portal **Sync amiibo** against the currently retained dirty v3 generation and
   confirm acknowledgement occurs only after IndexedDB persistence.
3. Hardware-validate the implemented manual Eject/Present path, including replacement and reconnect.
4. Capture a genuine Pro2 physical-tag write/readback before enabling native writes.
5. Complete the research-build-only firmware-update sink so a future genuine controller update can
   be captured without truncated `0x0D/04` payloads.
6. Add DualSense microphone return only after preserving the confirmed speaker/haptic path.
7. Extend motion translation to another controller family only after verifying its calibration,
   axes, units, timestamps, and stationary-bias behavior.

## Known traps

- A fresh Codex session has no conversation memory. Repository evidence is the handoff.
- Do not use symptom-driven “spin and tell me” tuning when captures, UART, or screen analysis can
  answer the question.
- **A test that compares our code against our own code cannot detect a wrong assumption**, because
  both sides share it. Encoder against decoder, C against Python, slots against slots, layouts
  against layouts — all are *consistency* checks. Every `0x28` defect that reached hardware was a
  semantic constant or convention, never a bit layout, and each had been "validated" by a check
  that could not have failed. Only an external physical reference catches them: gravity is exactly
  1 g, a known angle is a known angle, a host clock is a clock.
- **Say which relationship was validated, not just "validated".** "Byte-exact on 981 packets"
  means the layout is a correct bijection. It says nothing about whether the values we generate are
  right. Conflating the two is what sent three builds to the console.
- If translated `0x28` is ever explicitly reopened, state the predicted console behaviour first,
  then run `python tools/ns2_motion40_validate.py`. Any UNKNOWN is a reason not to flash, while a
  clean run is still only necessary—not sufficient—after the hardware rejection.
- Run `python tools/test_ns2_motion40_coherence.py` after any scheduler, scale, axis, or carrier
  change. Its clean analytic source and deliberately corrupted packets catch composition defects
  that byte-exact packer and capture-corpus tests cannot see.
- Do not assume a controller's VID/PID can be recovered late from every saved bond; fresh pairing
  and already-bonded paths differ.
- Do not merge Pico 2 W audio scheduling into Pico W.
- Do not alter genuine Pro2 audio framing: one 240-byte/20 ms CELT frame is split into ordered
  120-byte `0x04` and `0x02` GATT writes.
- Do not add large captured media without updating `dumps/README.md` and binary attributes.
