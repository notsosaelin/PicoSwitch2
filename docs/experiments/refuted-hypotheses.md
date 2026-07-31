# Refuted Hypotheses — Archive

> This document exists so future contributors (and future LLM sessions) don't re-investigate a
> path this project has already ruled out with direct evidence. Per `CLAUDE.md`'s instruction to
> never let assumptions silently calcify into facts, and never silently discard evidence either —
> a hypothesis that turned out wrong is still a real finding, just not a live one. Live docs
> (`docs/switch2-gc/protocol.md`, `STATUS.md`, `PLAN.md`) should only carry the *current*
> best-evidence understanding; this file is where the superseded understanding goes, with enough
> detail to explain why it was reasonable at the time and exactly what evidence overturned it.
>
> Confidence key: **Confirmed** / **Strong** / **Hypothesis** / **Unknown** — same as every other
> doc in this project. Entries below record what confidence level the *refuted* claim held before
> being overturned, and what overturned it.

---

## GC rumble output-report `0x03`, `data[0]` as a linear 0-255 amplitude byte

**Held**: 2026-07-13 (initial implementation) through 2026-07-14 (two revisions), at Hypothesis
tightening toward Strong with each revision, before being fully refuted 2026-07-14.

**The claim**: byte offset `0x1` of the 4-byte rumble-data field (`data[0]` in
`switch_gc_hid_out_report()`'s own indexing) is a continuous amplitude value, 0-255, directly
controlling motor intensity — first as "any nonzero value = on", later refined to "pass the value
straight through as amplitude."

**Why it seemed reasonable at the time**: the only real evidence available was 8 manual-test
samples from `rumble-procon-gccon.pcapng.gz` (see
`docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`), all with `data[0]` in the range
`0x50`-`0x68`, grouped by a separate 2-byte field that looked like a "mode selector." Reading the
swept-looking `data[0]` values as intensity was a plausible, evidence-consistent interpretation of
that limited sample — genuinely the best reading available at the time, not a careless guess.

**What refuted it**: 2026-07-14, reading the actual Linux kernel "HID: nintendo" driver source
(Vicki Pfau, linux-input mailing list v11 patch series,
`https://marc.info/?l=linux-input&w=2&r=1&s=hid+switch2&q=b`). The kernel driver shows the genuine
GameCube controller has no continuous-amplitude rumble hardware at all — `data[0]` (kernel's
`rumble_buffer[1]`) is an incrementing sequence/command byte, and the real intensity-relevant field
is `data[1]` (kernel's `rumble_buffer[2]`), a 3-value state enum (`GC_RUMBLE_OFF=0`/`ON=1`/
`STOP=2`). Real hosts simulate a continuous perceived amplitude via delta-sigma/error-accumulation
duty-cycle modulation of that ON/OFF state, not via any single byte's magnitude.

**Why this matters going forward**: this single wrong assumption plausibly explains the *entire*
observed bug arc across four firmware revisions — the sequence byte is essentially never exactly
zero, so every real rumble packet looked like "some nonzero amplitude" to the old decode
regardless of the game's actual OFF/ON/STOP intent. Don't revert to reading `data[0]` (or any
single byte in this field) as a linear amplitude. Current model:
`docs/switch2-gc/protocol.md` "Output Report `0x03`".

**Not yet Confirmed** (only Strong) — the corrected model has not itself been hardware-validated
yet; that remains the next real test.

---

## GC rumble: "any of the 4 rumble-data bytes nonzero" as the on/off signal

**Held**: 2026-07-13 (initial implementation) through early 2026-07-14.

**The claim**: OR all 4 rumble-data bytes together; nonzero means "vibrate," all-zero means "off."

**Why it seemed reasonable**: simplest possible reading consistent with the 8 known samples, all
of which had at least one nonzero byte.

**What refuted it**: real hardware feedback (PC/Steam) that the motor stuck on permanently. The
"mode selector" byte (kernel-confirmed to actually be part of the sequence/state fields, not a
mode at all — see the entry above) stayed nonzero across a stop transition, so OR-ing all 4 bytes
together misread a real "stop" as "still vibrating." Narrowed to checking a single byte
(`data[0]`) instead — itself later found wrong per the entry above, but the OR-all-bytes approach
was refuted first and independently.

---

## Personality-transition rumble bugs: three candidate root causes

**Held**: as open hypotheses through 2026-07-14's static code audit
(`docs/experiments/gcusb-rumble-lab-2026-07-14.md`), refuted the same day by direct code reading
(no hardware needed for any of these three).

1. **Stale shared rumble state survives the Pro2→GameCube transition.** Refuted:
   `switch_gc_init()`'s `report_set_rumble(0, 0, 0)` runs synchronously before the personality
   flag flips (`src/usb.c`'s `usb_apply_mode_cycle()`), verified by direct trace of the exact
   transition sequence.
2. **A race between `g_usb_personality` flipping and `switch_gc_init()` running lets the BT task
   observe an inconsistent intermediate state.** Refuted for rumble specifically: the rumble reset
   writes to a personality-agnostic shared global (`report.c`), so it takes effect immediately
   regardless of `g_usb_personality`'s exact timing.
3. **Vendor-bulk initialization commands get misrouted into the HID rumble decoder.** Refuted:
   every `report_set_rumble()` call site in `switch_gc.c` was verified exhaustively — only one is
   gated behind an actual `report_id==0x03` HID OUT report; vendor-bulk commands go through a
   structurally separate code path (`switch_gc_vendor_dispatch()`) that never calls
   `report_set_rumble()` at all.

**Why this matters going forward**: rules out an entire category of "shared state/plumbing" bugs
for this specific symptom. The real cause was in byte-level decode semantics (see the two entries
above) and downstream Bluetooth-forwarding envelope timing (below), not in the
personality-transition machinery itself.

---

## Xbox rumble bridge: `pulse_sustain_10ms = 0xFF` is fine for real gameplay

**Held**: since this project's 2026-07-12 Xbox rumble fix (adopting xpadneo's own
Windows-driver-compatible `loop_count=0xEB`/`pulse_sustain_10ms=0xFF` convention) through
2026-07-14's nineteenth pass.

**The claim**: a ~2.55-second-per-trigger hold, matching what a real Windows Xbox driver uses, is
a safe, correct value regardless of how the trigger is used.

**What refuted it**: real console gameplay (Smash Bros) feedback that normal small/frequent
rumble ticks smeared into one continuous "powerful" buzz. This value is fine for what it was
originally fixed for (a single sustained rumble effect from a PC driver that expects to hold a
level until told otherwise) but wrong for a rapid stream of separate short ticks — real gameplay
rumble (and, per the delta-sigma model above, GC's *entire* rumble protocol) sends updates much
faster than 2.55 seconds apart, so each new trigger re-armed a multi-second hold before the
previous one could decay. Fixed by shortening to `0x05` (~50ms) for a genuine trigger — see
`STATUS.md`'s "twentieth pass" for the full account.

**Not yet re-tested** against real gameplay with the shortened value.

---

## Switch 2 motion: a genuine static `0x28` template plus dynamic G6/G7/G8 is sufficient

**Held**: 2026-07-24 as a deliberately gated Hypothesis, then refuted by its first live
Switch 2/Splatoon 3 test the same day.

**The claim**: the unresolved leading and middle lanes of a normal status-`0x0D`,
length-`0x28` motion PDU could remain fixed at values from one genuine Pro Controller 2 packet,
while firmware updated timing and the independently decoded G6/G7/G8 lanes from a second
DualSense quaternion. Interleaving that packet one in four samples with the already validated
length-`0x1E` carrier might be enough for the console to consume the corrected/reference
orientation.

**Why it seemed reasonable**: the packet began as a byte-for-byte genuine normal PDU; the
G6/G7/G8 signed 22/22/20-bit codec had genuine golden-vector and signed-boundary tests; the
experiment preserved all shared unknown bits; and its one-in-four schedule matched the observed
native interleave. It was default-off and reachable only through UART.

**What refuted it**: enabling `ds5motion ref28 on` immediately made live gyro motion random and
unusable. UART diagnostics simultaneously showed valid generated packets, a selected
length of 40, changing G6/G7/G8 values, and zero representation rejects. Disabling the gate
immediately restored emitted length 30 and the validated DualSense motion path without a reflash.
The failure therefore reached the console; it was not a packer rejection or stale diagnostic
display.

**Why this matters going forward**: the changing leading/middle `0x28` lanes are semantically
consumed, cross-validated, or both. They must be decoded and generated coherently before another
synthetic `0x28` packet is sent. The unsafe UART generator and runtime gate were removed after this
result; only passive analysis and the exact historical bit-range codec remain.

**Later correction, 2026-07-29**: exact reference-PCAP analysis transferred the 288-bit
multi-sample IMU map to the genuine Pro2 catch-up stream. G6/G7/G8 cross payload bits belonging to
the newest packed gyro and acceleration samples; they are not independent lanes or a second
quaternion. The failed generator therefore corrupted parts of two real samples while leaving the
rest of the packet static. The live rejection was an early symptom of this exact coherence error.
Current evidence and safe next work:
[`../switch2/uart-magprobe.md`](../switch2/uart-magprobe.md).

---

## Switch 2 motion: G6/G7/G8 is a simple externally responsive magnetic-field vector

**Held**: as one candidate interpretation after the signed 22/22/20-bit lanes were decoded on
2026-07-24; refuted as a simple external-field model by the controlled 2026-07-29 hardware matrix.

**The claim**: if G6/G7/G8 directly or monotonically represented the external magnetic field, a
stationary genuine Pro Controller 2 should show a repeatable direction or magnitude response when
an external magnet is introduced. Reversing the presented magnet face should reverse or
substantially redirect that response, and reducing distance should increase it.

**What refuted it**: zero-drop captures bracketed each field condition with stationary baseline
and recovery captures. Time-weighted A/B/A subtraction removed ordinary drift. Two ceramic-magnet
faces at 100 mm produced nearly parallel residual directions rather than a polarity reversal; the
50 mm condition did not scale upward; and the matched no-magnet G6/G7/G8 residual (`0.0652°`) was
larger than every 100 mm field result (`0.0357°..0.0620°`). A neodymium disc down to the closest
stable sub-10 mm placement likewise produced no repeatable response beyond the control envelope.
Other changing bytes, including the initially interesting `p17`, also lacked polarity reversal and
distance scaling.

**Evidence boundary at the time**: this refuted only the simple externally responsive vector
interpretation under the tested geometry, distances and field strengths. It did not establish that
no magnetometer exists or that an internal sensor is unused.

**Later correction, 2026-07-29**: the tested aliases are not a vector at all. Their bit ranges
cross packed gyro and gravity-bearing acceleration samples in the length-`0x28` catch-up layout.
That is the direct reason no coherent magnetic polarity/distance response existed. The hardware
campaign remains valid negative evidence and does not establish whether a separate physical
magnetometer exists.

Full matrix, commands and limitations:
[`pro2-magnetic-stimulus-matrix-2026-07-29.md`](pro2-magnetic-stimulus-matrix-2026-07-29.md).

---

## Length-0x28 catch-up begins only above 16 ticks and uses gyro15/accel14 middle fields

**Held**: from the initial Joy-Con-derived packed-layout notes and the first genuine-Pro2
reference PCAP, whose 17–19-tick records did not expose the boundary.

**The claim**: tick deltas through 16 use the normal map; only deltas above 16 use a catch-up map
with signed15 gyro 1 and signed14 middle acceleration at payload bits `110..154` and `155..196`.

**What refuted it**: a zero-drop genuine-Pro2 cadence matrix produced mixed 14/15-tick packets in
one 17.5 ms capture and mixed 14/15/16-tick packets in one 18.75 ms capture. Normal decoding was
coherent for every delta through 14 and impossible for every delta 15 or 16. The exact alternative
tiling—gyro 1 signed16 at `110..157`, middle accel signed13 at `158..196`, gyro 2 signed16 at
`197..244`—made all three acceleration lanes approximately `1.052 g` and both gyro lanes agree
with raw stationary bias after their scale corrections.

**Correct model**: high-rate `0..10`, normal `11..14`, catch-up `15+`. See
[`pro2-raw-native-motion-pcap-2026-07-29.md`](pro2-raw-native-motion-pcap-2026-07-29.md).

---

## Switch 2 motion: the 0x28 prefix is flags2 plus three equal-width smallest-three components

**Held**: as an offline working interpretation on 2026-07-29 after the cadence matrix established
a continuous leading state across the high-rate, normal, and catch-up layouts.

**The claim**: payload bits after the first two flags formed three adjacent, equal-width signed
components: three signed24 lanes in high-rate and three signed22 lanes otherwise. A single
capture-fitted scale could then decode them as a standalone smallest-three quaternion.

**Why it seemed reasonable**: the three extracted values were continuous at layout transitions
after dividing the high-rate form by four. A clean pitch capture produced `0.991934` delta
correlation against the established length-`0x1E` quaternion and only `0.048449 degrees` p90
error after fitting a scale.

**What refuted it**: direct bit alignment against the interleaved length-`0x1E` retained carrier
showed asymmetric lanes. The first correction still stopped at a false boundary and called two
bits before lane 2 a separate state. Testing that proposed transition law showed that the bits are
lane 2's low fragment. High-rate is `mode2 + s24 + s23 + s25`; normal/catch-up is
`mode2 + s22 + s21 + s23`. Grouping only by the length-`0x1E` carrier state and testing the sample
epoch initially produced `0.999962` and `0.999996` mean absolute correlation in the two dynamic
captures, with fixed power-of-two scales and no capture-specific scale multiplier. Applying the
packet's encoded elapsed field then resolves the remaining constant-offset discrepancy:
`current tick - elapsed + 4` gives `0.999996` in both captures.

**Correct boundary**: this establishes a mode-3, cadence-dependent truncated
orientation-carrier relationship, not a standalone quaternion decoder. The tail is now identified
as two Q3 temperature samples, and a causal modular history decoder is validated against
interpolated length-`0x1E` truth. Bit 287 is observed reserved-zero padding across 1,066 catch-up
packets.

Reciprocal chart-transition captures then refuted the stronger assumption that the genuine
length-`0x1E` state itself is always a strict smallest-three omitted-component selector. State 0
and state 3 are cyclic lane assignments of a continuous carrier, and 16 genuine rapid-motion
records have retained-vector energy above one (maximum `1.026738`). A positive omitted component
does not exist for those records. The production DualSense length-`0x1E` encoder remains a
hardware-validated approximation; this correction limits the genuine decoder claim and does not
invalidate that production behavior. Exact integer rounding and states 1/2 remain unresolved.

**Follow-up, 2026-07-29:** the later zero-drop `3 → 1 → 0` capture resolves the observed state-1
branch law. The cyclic omitted-component topology plus a paired flip of the two non-boundary lanes
fits the negative `1 → 0` boundary at `0.024716`; all five captured boundaries have
RMS/max `0.025302/0.047878`. Exact rounding and unseen state 2 remain unresolved.

**State-2 closure, 2026-07-29:** the state-2-only passive trigger later captured
`3 → 2 → 3` with zero drops. Both reciprocal boundaries select topology
`(G2,G0,G1)` and the opposite omitted-sign branch `(+,−,−)`, with residuals
`0.036162` and `0.011824`. The full nine-boundary corpus covers all four chart
states at RMS/max `0.023541/0.047878`. The interleaved `3 → 2` prefix selects
chart 3 (`0.003833` versus `0.196168`), while the same local-frame audit
recovers the direct `3 → 1` prefix as chart 1 (`0.008416` versus `0.242898`).
Exact integer rounding remains unresolved.

Full method and results:
[`pro2-mode3-carrier-prefix-2026-07-29.md`](pro2-mode3-carrier-prefix-2026-07-29.md) and
[`pro2-carrier-chart-transition-2026-07-29.md`](pro2-carrier-chart-transition-2026-07-29.md).

---

## A rotating multi-amiibo device can capture a Smash write after a successful read

**Held**: briefly as the intended 2026-07-25 native-write capture setup.

**The claim**: after Smash successfully read one emulated amiibo, continuing through the game's
interaction with the same multi-amiibo device would produce the native write command sequence.

**What refuted it**: the device changed from UID `04 A4 47 F2 6F 40 80` to
`04 D5 E7 48 CC F9 71`. The retained USB and BLE traces contain a complete read of the first UID,
then only scan/status/stop traffic for the second. There are zero `0x14` write-buffer chunks, zero
`0x08` commit commands, and no UID-bearing `0x06` write descriptor. The bridge itself was healthy:
41/41 commands received matching responses, with zero timeouts or rejections.

**Why this matters going forward**: a write capture needs one stable, writable tag identity across
the prerequisite read, write prompt, and readback. Repeating this setup cannot reveal write framing
unless the device can be locked to one tag. See
[`smash-native-nfc-write-attempt-2026-07-25.md`](smash-native-nfc-write-attempt-2026-07-25.md).

---

## A 64-byte `tud_vendor_read` buffer is sufficient for Switch 2 command dispatch

**Held**: throughout the previously validated initialization, input, audio, motion, and NFC-read
work because every exercised console request fit in one 64-byte read.

**The claim**: TinyUSB vendor OUT traffic could be read into `cmd[64]` and immediately passed to
`ns2_dispatch`; larger protocol responses mattered, but console commands did not need stream
reassembly.

**What refuted it**: the first stable Virtual Amiibo game-owned write crashed the Switch 2 with
error `2168-0002`. Code-level framing analysis found that a normal `0x14` request is 88 bytes:
eight envelope bytes plus an 80-byte payload. The old loop dispatched the first 64 bytes, so the
NFC runtime received only 52 of the declared 76 staging-data bytes after the four-byte chunk
prefix. The remaining 24 USB bytes were then treated as a second command. This failure was
impossible on the validated read path because its requests are short.

**Correction**: `ns2_vendor_rx` now buffers the command stream until the big-endian length in
header bytes 4–5 is complete. Host tests reproduce the exact 64+24 boundary, arbitrary splits,
coalesced commands, and oversized-command recovery. See
[`virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md`](virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md).

---

## Figure-v3 `0x21` result has a fixed zero tail ending in `7A C4`

**Held**: 2026-07-27 through the first downloaded-dump retests on 2026-07-28.

**The claim**: only result bytes `[19..50]` come from the image's SRAM block; `[51..79]` are fixed
zeros and `[80..82]` end in a controller- or protocol-level constant `00 7A C4`.

**Why it seemed reasonable**: all six genuine results across two sessions were byte-identical, even
after owner data had been written. Both sessions, however, used the same physical Warp Star. The
test also searched several CRC-16 variants over the wrong candidate ranges and therefore failed to
identify the trailer.

**What refuted it**: the 83-byte framing is exactly a 19-byte controller header plus the full
64-byte SRAM response. CRC-16/MCRF4XX over the genuine response's first 62 bytes is `7A C4`.
Untouched downloaded dumps carry their own correct CRCs (`E5 11` for Kirby/Warp and `30 61` for
Meta/Shadow). Firmware had copied only 32 bytes and substituted `7A C4`, making every other
response invalid. Publishing all 64 stored bytes made an untouched downloaded dump complete a
full real-console read and write with no signature override and zero write errors. See
[`v3-full-sram-response-validation-2026-07-28.md`](v3-full-sram-response-validation-2026-07-28.md).

**Why this matters going forward**: never synthesize or transplant the SRAM trailer. Preserve the
complete `image[0x3C0..0x3FF]` response supplied by the dump, including its CRC.

---

## Figure-v3 downloaded dumps require a signature override or captured carrier

**Held**: as competing hypotheses through 2026-07-28.

**The claim**: a downloaded figure-v3 dump might need its own NXP originality signature, a
signature/UID-matched captured figure, or a key-based re-sign onto the known-good figure's UID and
SRAM.

**What refuted it**: an untouched downloaded `Kirby & Warp Star.bin` was accepted and written by a
real Switch 2 with `signature_set=false`; prefix bytes `[19..50]` were zero. The image's original
UID, encrypted body, and SRAM response were served without transformation. Retail keys were used
only for offline HMAC verification.

**Why this matters going forward**: keep the portal import-only and do not require
`key_retail.bin`, a physical enrollment capture, or a carrier conversion for valid 2048-byte dumps.
The UART signature command is diagnostic infrastructure, not a production dependency.

---

## The first King Dedede failure was only a premature TagRemoved lifecycle

**Held**: briefly after the first King Dedede/Tank Star hardware trace on 2026-07-28.

**The claim**: the adapter had accepted the figure-v3 update, but an early virtual TagRemoved
transition prevented the console from following through with the expected `0x16` completion.
The update envelope's byte 13 was treated as opaque, so matching the already proven Kirby record
pages appeared sufficient.

**Why it seemed reasonable**: the visible console symptom was again `2115-0096`, and an earlier
Kirby failure really had been caused by premature removal between the two Air Riders stages.

**What refuted it**: the first header-only fix classified and staged all 11 King Dedede chunks and
reached one completion, but diagnostics then recorded two write errors. The exact records locate
the 32-byte sector-0 update at page `0xB2`, place the sector-1 capability at page `0x64`, and place
the following 96 bytes at page `0x65`; Kirby instead uses pages `0x92`, `0x00`, and `0x01`.
Envelope byte 13 therefore selects the allocation-relative sector-1 capability page. The firmware
was returning fail-closed `07 41` because its commit validator still required Kirby's fixed pages,
not because the tag had been removed.

**Correction**: the write validator, generation check, commit, `0x1E` read path, and browser-local
Initialize operation now use each image's self-described safe allocation. There is no UID,
character, product, or dump whitelist. See
[`v3-air-riders-dynamic-allocation-2026-07-28.md`](v3-air-riders-dynamic-allocation-2026-07-28.md).

---

## A cold-capture raw-IMU profile can be layered over an active native-report session

**Held**: for the first UART-gated `imuref` hardware discriminator on 2026-07-29.

**The claim**: replaying the genuine handle-`0x000A` PCAP's report selector, feature `0x2F`,
calibration reads, report-rate write, and common CCC would switch a controller that was already
running the production handle-`0x000E` native profile.

**Why it seemed reasonable**: every controller command and CCC operation in the source PCAP was
accepted, and the new sequence reproduced all motion-relevant writes. Feature/report selection had
previously caused the controller to stop one report format when another became active.

**What refuted it**: the UART state machine completed, but after 750 ms `imuref status` reported
`common=0` and `native=170`. The existing native stream never stopped. `imuref off` then restored
the production profile cleanly, with fresh owned length-`0x28` motion immediately visible through
`motionusb`.

**Correction**: live profile switching must explicitly disable the currently selected CCC before
replaying the target profile. The revised gate disables `0x000F` before raw selection and disables
`0x000B` before native restoration. A failed native unsubscribe aborts the raw transition
fail-closed. See
[`pro2-raw-native-motion-pcap-2026-07-29.md`](pro2-raw-native-motion-pcap-2026-07-29.md).

---

## Switch 2 motion: common and native CCCs can deliver raw and packed IMU simultaneously

**Held**: as a default-off hardware discriminator on 2026-07-29.

**The claim**: after the verified raw profile owns handle `0x000A`, enabling the native
handle-`0x000E` CCC without changing any report-selection command might leave both streams active.
That would provide sample-simultaneous raw/native correlation.

**Why it seemed reasonable**: the two characteristics have independent CCCs and notification
listeners. The raw command profile remained selected, and the native CCC write returned ATT
success.

**What refuted it**: after the native CCC became active, the common counter stopped at `387` while
the native counter advanced past `2,900`. A zero-drop capture contained 46 native records and zero
common records. Disabling only the native CCC immediately resumed common notifications without
replaying the raw command sequence.

**Correction**: report generation is selector-exclusive and the enabled native CCC has priority.
CCC-only switching is reversible, so tightly bracketed raw/native A/B captures remain possible,
but they are not simultaneous and must not be paired sample-for-sample. See
[`pro2-raw-native-motion-pcap-2026-07-29.md`](pro2-raw-native-motion-pcap-2026-07-29.md).

---

## Switch 2 motion chart states compose as one unsigned lane permutation per state

**Held**: as a provisional offline model after the first three zero-drop chart boundaries on
2026-07-29.

**The claim**: state 0 wire `(G0,G1,G2)`, state 1 `(G2,G0,G1)`, and state 3 `(G1,G2,G0)` could be
treated as fixed projections into one global three-lane frame. The reciprocal `0 ↔ 3` boundaries
had residuals `0.002563` and `0.001132`, while the first `0 → 1` boundary selected its permutation
with residual `0.017025`, about 29 times below the runner-up.

**Why it seemed reasonable**: all three captured state-0 boundaries were locally continuous, the
lane permutations composed algebraically, and the state-stable prefix decoder retained excellent
accuracy. State 1 had not yet been observed in a second adjacent branch.

**What refuted it**: a separate zero-drop Splatoon capture contained `3 → 1 → 0`, with state 1
present for one carrier record. The `1 → 0` edge had minimum unsigned-permutation residual
`1.185389`; adding the capture to the global solver forced RMS/max residual
`0.818124/1.252822` and a `1.204945` excess over independently best edge fits. The direct
`3 → 1` edge also cannot be evaluated by composing the two state-0 projections. An independently
fitted signed lane transform reduces the `1 → 0` residual to `0.024716`, but one sample cannot
establish a reusable signed or nonlinear chart law.

**Correction**: describe the earlier permutations as the same-sign branch of a stateful cyclic
omission model, not universal unsigned state maps. The held-out `1 → 0` edge exercises the
opposite-sign branch: the topology permutation `(G2,G0,G1)` plus paired non-boundary signs
`(+,−,−)` reduces its residual to `0.024716`. Across all five boundaries, this narrow two-branch
model has RMS/max `0.025302/0.047878`, with every branch choice at least `0.324174` better than
its alternative. Analyze each edge independently, reject stateless global composition, and keep
state 2 prediction-only until captured. Production DualSense/Edge motion remains on the validated
length-`0x1E` carrier. See
[`pro2-carrier-chart-transition-2026-07-29.md`](pro2-carrier-chart-transition-2026-07-29.md).

**Follow-up:** state 2 was subsequently captured in both directions against
state 3. The same cyclic opposite-sign branch fits both seams, so state 2 is no
longer prediction-only; exact integer projection/rounding remains open.

---

## Format notes for future entries

Each entry should have: the claim, the confidence level it held, why it was reasonable given the
evidence available *at the time*, and the specific evidence that overturned it. Link forward to
whichever live doc now carries the corrected understanding. Don't delete an entry once a "correct"
replacement is itself later refuted — append a note here instead, so the history of *why* stays
intact.

## Switch 2 motion: length-`0x1E` byte 12 bit 7 is the omitted-component sign

**The claim**: the unexplained flag at byte 12 bit 7 records the whole-quaternion
sign canonicalization — whether the omitted component was negated before packing,
as Nintendo's Switch 1 DScale packer does without transmitting the result.

**Why it was plausible**: the flag is 0% across stationary captures and 23–48%
across moving ones, which is exactly what a sign that flips on zero-crossings
would look like. If true it would make the omitted component recoverable.

**The discriminator**: a whole-quaternion negation flips all three transmitted
lanes together, so negating the later record must restore continuity precisely
when the flag toggles. This needed no new capture — the existing corpus already
contains a 94-record all-state-3 rotation (`pro2-chart-face-forward-no-transition`)
plus five boundary captures.

**Result**: across 160 adjacent same-chart toggles, negation restored continuity
in **0**. Median `d_raw` was `0.002511` against `d_neg` `1.318595`; the trajectory
is already smooth without negation in every case. Also refuted alongside it: any
relation to the `0x1E`/`0x28` interleaving or connection interval, where the flag
rate is a flat 12–14% across every slice.

**What survives**: the flag tracks motion (100× separation in inter-record carrier
change) and is per-sample rather than modal. It remains unexplained and is carried
through synthesizers verbatim, never inferred.

Evidence: [`pro2-carrier-unknown-fields-2026-07-31.md`](pro2-carrier-unknown-fields-2026-07-31.md).
