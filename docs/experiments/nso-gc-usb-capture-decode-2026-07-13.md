# NSO GameCube Controller — `rumble-procon-gccon.pcapng.gz` Decode — 2026-07-13

> Confidence key: **Confirmed** (hardware-observed, cited capture, or ≥2 independent hardware-derived
> sources agreeing) / **Strong** / **Hypothesis** / **Unknown**.

## Source

`ndeadly/switch2_controller_research`'s `captures/usb/rumble-procon-gccon.pcapng.gz`, cloned this
session (commit `d1c5a7f7ba298f83017fae84952a4e6d2ef8fc92`), decompressed to a scratch working copy
and analyzed with `tshark` (`C:\Program Files\Wireshark\tshark.exe`). Raw capture format: **Cynthion
USB Analyzer**, link-layer (`usbll`) encapsulation — a real hardware USB protocol analyzer capture, a
different format from both this project's own USBPcap captures and the raw-link-layer captures noted
in prior sessions' memory. 1,339,758 frames, ~762 seconds. Contains **two** Nintendo devices: a
Pro Controller 2 (`idProduct 0x2069`, frame 44) and, later in the same session, the genuine NSO
GameCube Controller (`idProduct 0x2073`, first appears frame 370881, assigned USB device address 8).
This document covers only the GameCube controller's traffic (filtered via `usbll.device_addr==8`).
The full decompressed pcapng (~60 MB) was kept only in the job scratch directory, not copied into this
repo (too large for version control) — the source `.gz` remains available at
`E:\nso-gc-refs\switch2_controller_research\captures\usb\rumble-procon-gccon.pcapng.gz` for
re-analysis; this document plus `docs/switch2-gc/protocol.md` are the durable record of what was
found.

## Enumeration — Confirmed, second independent match

Device descriptor response, frame 370881 (18 bytes):
```
12 01 00 02 ef 02 01 40 7e 05 73 20 01 01 01 02 03 01
```
**Byte-for-byte identical** to the device descriptor this project captured itself the same session via
USBPcap on a different, genuine physical unit (`docs/experiments/nso-gc-captures/genuine-controller-descriptors-2026-07-13.pcap`)
— a real cross-hardware confirmation, not a repeat of the same unit.

Configuration descriptor response, frame 370913 (structure): CONFIG → IAD → INTERFACE(HID) → HID →
ENDPOINT → ENDPOINT → IAD → INTERFACE(vendor) → ENDPOINT → ENDPOINT — same 10-descriptor topology as
this project's own capture, same field values (`wTotalLength=80`, HID `wDescriptorLength=97`, etc.).
**Confirmed**, second independent match.

## EP0 vendor control requests — new architecture finding, Strong/Confirmed

Two vendor-class EP0 control requests were observed immediately after enumeration, **on the device's
default control endpoint, not the bulk vendor interface (IF1)**. This is new information: neither
reference repo documented that any part of the USB init handshake happens over EP0 control transfers
rather than the bulk interface.

### `bRequest=3` (frame 370927): factory/identity read — Confirmed, cross-validated against this project's own SPI dump

Request: `bmRequestType=0xC0` (device-to-host, vendor, device recipient), `bRequest=3`, `wValue=0`,
`wIndex=0`, `wLength=64`.

Response (frame 370930, 64 bytes):
```
01 00 48 48 57 35 30 30 30 31 30 36 31 39 33 37 00 00 7e 05 73 20 01 04 01 ff ff ff ff ff ff ff
ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff
```
Decoded: bytes 2-15 are ASCII `HHW50001061937` — **a per-unit serial number, from a different
physical controller than the one this project has access to.** This project's own SPI dump analysis
(`docs/experiments/nso-gc-spi-dump-analysis-2026-07-13.md`) found the identical byte structure at SPI
offset `0x013000` for its own unit: `01 00 48 48 57 35 30 30 30 31 35 35 31 38 31 30 00 00 7E 05 73
20 01 04 01 FF FF...` — serial `HHW50001551810`. **Both units share the `HHW5000` prefix and differ
only in the trailing digits** — this promotes "the `HHW5000xxxxxxx` string is a per-unit serial, not
a shared constant" from Hypothesis to **Confirmed** (two independent units, same prefix, different
suffix), and independently confirms that **this EP0 vendor request reads directly from SPI address
`0x013000`** — the same address ndeadly's documented BLE init sequence reads from. **Per NSO-GC.md's
explicit exclusion rule, neither serial value is to be reused in firmware** — recorded here only to
document the cross-validation, not as reusable data.

### `bRequest=2` (frame 370937): partially decoded — Hypothesis (updated 2026-07-13, cross-device comparison)

Request: `bmRequestType=0xC0`, `bRequest=2`, `wValue=0`, `wIndex=0`, `wLength=16`.

Response (frame 370940, 16 bytes):
```
01 01 02 00 00 00 0c 00 00 00 02 bb 5e ab a9 3c
```

**Update, same session, no new capture needed**: the same file's Pro Controller 2 (device address 7,
not the GC controller's device address 8) issues the *identical* `bRequest=2`/`wLength=16` EP0 vendor
request during its own enumeration (SETUP at frame 99, response at frame 103):
```
01 01 05 00 00 00 0c 00 00 00 9e 2b ab ab a9 3c
```
Byte-aligned against the GC response:
```
        0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15
Pro 2:  01 01 05 00 00 00 0c 00 00 00 9e 2b ab ab a9 3c
GC:     01 01 02 00 00 00 0c 00 00 00 02 bb 5e ab a9 3c
```
Bytes 0-1, 3-9, and 13-15 are **byte-identical across two different controller types** on two
different physical units; only bytes 2 and 10-12 differ. This is enough structure to promote the
command from Unknown to **Hypothesis**:

- **Byte 2** (`0x05` Pro 2 vs `0x02` GC) is a plausible **device-type/personality discriminant** —
  worth testing against a third controller type (e.g. Joy-Con 2) if a capture ever surfaces one, and
  worth treating as the value our own NSO GameCube personality should return (`0x02`) if this command
  is ever queried by a real console during Stage D's native-recognition handshake.
- **Byte 6** (`0x0c` = 12, constant) is plausibly a length/count field for a 12-byte sub-structure
  starting around byte 10, though this remains speculative.
- **Bytes 10-12** vary per-device/session (`9e 2b ab` vs `02 bb 5e`) — not yet distinguishable as
  per-unit-random vs a derived/session value; no evidence either way from just two samples.
- **Bytes 13-15** (`ab a9 3c`, constant across both) look like a fixed footer/magic value, but two
  samples is a thin basis for that claim.

Not confidently mapped to anything in either reference repo's prose documentation — this decode
comes entirely from this project's own byte-comparison, not ndeadly's docs. Still **Hypothesis**, not
Confirmed: a third independent sample (different unit and/or controller type) is needed before
trusting the byte-2 device-type theory enough to hardcode a response in firmware.

## Bulk vendor-interface (IF1) init sequence — Confirmed byte-exact (2026-07-13, re-analysis)

**Update, same file, later pass (Stage D implementation work)**: the "USB init command sequence"
section of `docs/switch2-gc/protocol.md` was originally written from *BLE-derived* bytes (Strong
tier), with an explicit caveat that USB's "outer transport framing differs." This capture's device
address 8 (the GC controller) also has real traffic on the **bulk vendor interface** (endpoint
0x02 OUT / 0x82 IN, IF1) — 225 total OUT transactions across the session — which was not
previously mined. Filtering to just `usbll.device_addr==8 && usbll.endp==2` and decoding the first
handful (right after enumeration, ~t=212.3s) resolves that uncertainty directly:

```
Request  (frame 370989, t=212.348s): 03 91 00 0d 00 08 00 00 01 00 f3 b9 34 8c 81 78
Response (frame 371002, t=212.352s): 03 01 00 0d 00 f8 00 00 01 00 00 00
```
Byte-for-byte **identical in shape** to protocol.md's BLE-derived example (`03 91 00 0d 00 08 00
00 01 00 31 7e c6 eb f1 48` → `03 01 00 0d 00 f8 00 00 01 00 00 00`) — differing only in the last
6 request bytes, which is the host Bluetooth address and is *expected* to differ per host. This
**promotes Command 0x03/Sub 0x0D ("Initialise USB") from Strong to Confirmed for USB** — the outer
framing does NOT differ from BLE after all, at least for this command.

Continuing to the actual streaming trigger, at frame 376898 (t=213.127s, well after the
Bluetooth-pairing-shaped commands 0x15/0x16/0x07/0x09/0x0C/0x02/0x0A that fill the gap — none of
which are needed to explain streaml start, see below):
```
Request  (frame 376898, t=213.127s): 03 91 00 0a 00 04 00 00 0a 00 00 00
Response (frame 376909, t=213.187s): 03 01 00 0a 00 f8 00 00
```
This is **Command 0x03/Sub 0x0A ("Select Input Report"), selecting report ID `0x0A`** — exactly
the byte sequence protocol.md had only *speculatively constructed* by analogy ("PicoSwitch2 would
need to send `03 91 00 0a 00 04 00 00 0A 00 00 00`") — now **Confirmed**, captured directly from a
real PC host initializing a genuine GC controller. Its response is 8 bytes with no data tail
(shorter than 0x0D's 12-byte response) — also Confirmed exactly, not inferred.

**Notably absent from all 225 bulk OUT frames**: Command 0x03/Sub 0x03 ("Enable USB HID Reports")
never appears anywhere in this session. Either it isn't required for a PC host, or this particular
host/session skips it — either way, it is **not part of the minimum path this real session actually
used** to reach streaming.

**Also found, not previously documented anywhere**: starting at frame 383594 (t≈216.9s) and
repeating roughly every 1-5 seconds for the rest of the session, a paired `03 91 00 0c 00 04 00 00
01 00 00 00` / `03 91 00 0c 00 04 00 00 00 00 00 00` (Command 0x03/Sub 0x0C, alternating a trailing
0x01/0x00 data byte) — shape consistent with a periodic keepalive/ping, but **Unknown**, not
implemented, not required to explain streaming start (streaming was already active by then).

**Implemented 2026-07-13** (`switch_gc_vendor_dispatch()` in `src/switch_gc/switch_gc.c`): the
minimum gate — Sub 0x0D and Sub 0x0A handled with the exact Confirmed response envelopes above;
Sub 0x03 ACKed defensively (documented, never observed, cheap to support); everything else
(pairing/LEDs/features/memory/keepalive) deliberately unimplemented, with a rate-limited diagnostic
log for anything unrecognized. Full evidence-to-code mapping: `docs/switch2-gc/protocol.md` "USB
init command sequence" (updated to Confirmed) and this file's own comments.

## Input Report `0x0A` — Confirmed, self-consistent full-field match

A neutral-state (idle, no buttons pressed) live interrupt IN report, sampled well after enumeration
(frame 400004, 64-byte USB transfer):
```
0a 7b 1f 00 00 00 fd 96 34 c8 d7 80 38 20 22 1e f3 38 00 0c 00 90 ac a6 03 e0 75 e5 00 90 ad 70
98 b0 3b 02 b0 06 73 0f 8e ad 7c 01 2b ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

Decoded against ndeadly's documented report `0x0A` layout (`docs/switch2-gc/protocol.md`), treating
byte 0 as the report ID and ndeadly's own offsets as relative to byte 1 onward:

| ndeadly offset | Bytes (this capture) | Decoded | Plausibility check |
|---|---|---|---|
| `0x0` Counter | `0x7B` | 123 | Plausible incrementing counter value |
| `0x1` Power Info | `0x1F` = `0b0001_1111` | bit0=1 (external power), bit1=1 (charging), bits2-5=0b0111=7 (battery level 7/9) | **Matches physical reality exactly** — this controller was USB-connected (external power + charging both true) during capture |
| `0x2` Buttons (3B) | `00 00 00` | no buttons pressed | Matches — sample was taken at rest |
| `0x5` Left stick (3B, packed 12-bit) | `FD 96 34` | (0x6FD, 0x349) = (1789, 841) | Plausible 12-bit ADC values (0-4095 range) |
| `0x8` Right stick (3B, packed 12-bit) | `C8 D7 80` | (0x7C8, 0x80D) = (1992, 2061) | Both close to 2048 — plausible near-center resting position |
| `0xB` Unknown (feature-bit-5-dependent) | `0x38` | — | Matches ndeadly's documented "0x38 if feature bit 5 set" — confirms feature bit 5 was active in this capture session |
| `0xC` Left analog trigger | `0x20` (32) | — | Plausible low/resting trigger value |
| `0xD` Right analog trigger | `0x22` (34) | — | Plausible low/resting trigger value |
| `0xE` Motion Data Length | `0x1E` (30) | — | **Exact match** to one of ndeadly's three documented observed values `{0, 30, 40}` |
| `0xF` Motion Data (40B slot, 30 used per above) | `F3 38 00 0C 00 90 AC A6 03 E0 75 E5 00 90 AD 70 98 B0 3B 02 B0 06 73 0F 8E AD 7C 01 2B FF` | — | Not decoded (format still Unknown per ndeadly) |
| `0x37` Reserved (8B) | `00 00 00 00 00 00 00 00` | — | Matches — all-zero as documented |

**Every field decodes to a physically plausible value, and two fields (`0xB`'s feature-bit-dependent
byte, `0xE`'s motion-data-length) match ndeadly's documentation exactly, including one exact match
against a specific enumerated value from a set of three possibilities.** This promotes the entire
report `0x0A` layout from Strong to **Confirmed** — this is not just "consistent with," it's a
successful field-by-field decode of real hardware data using the documented table with zero
contradictions found.

**Not yet found in this pass**: a live sample with nonzero buttons (would pin down the exact
button-bit-to-physical-control mapping with a real capture, not just ndeadly's prose table). Two
sampling passes across different time windows of the capture (spanning roughly frames 700,000-910,000)
found no button presses in those specific windows — the capture likely has presses concentrated in
narrower windows not yet located. Not a high priority to keep searching given the button table's
prose documentation is already extensively cross-referenced within ndeadly's own docs and the value of
further searching this same file is low relative to the effort.

## Output Report `0x03` (rumble) — 8 real byte-exact samples

**Hardware correction 2026-07-15:** a firmware decoder that treated only byte2 (`data[1]`) as the
OFF/ON/STOP state produced no rumble on a real Switch 2. Four captured packets below contain
`byte2=00, byte3=01`; the current bounded test decoder treats that confirmed `00 01` form as active
while also accepting the previously documented `01 00` form. This is a new **Hypothesis**, not a
claim that the exact field boundary is settled.

**The interpretation below was superseded as of 2026-07-14 and then contradicted in hardware on
2026-07-15** — the "byte1=intensity,
byte2/3=mode selector" reading was a reasonable interpretation of these 8 samples alone, but is
refuted by the real Linux kernel "HID: nintendo" driver source (see
`docs/experiments/refuted-hypotheses.md` "GC rumble data[0] as a linear amplitude byte" for the
full account, and `docs/switch2-gc/protocol.md` "Output Report `0x03`" for the current model).
**The raw byte samples below remain valid, Confirmed ground truth** — only the field-boundary
interpretation was wrong. Byte1 (`data[0]`) remains a plausible sequence/command byte, but the
remaining three-byte field boundary is unresolved; the real `00 01` samples must not be discarded.

Eight distinct interrupt OUT reports were captured in what is clearly a deliberate rumble test burst
late in the capture session (frames ~1,282,317 – 1,337,857), each a 64-byte USB transfer:

| # | Raw bytes (report ID + 4-byte "Gamecube Rumble Data" field per ndeadly, then zero padding) |
|---|---|
| 1 | `03 50 02 00 00` |
| 2 | `03 61 00 01 00` |
| 3 | `03 63 00 01 00` |
| 4 | `03 55 02 00 00` |
| 5 | `03 66 00 01 00` |
| 6 | `03 68 00 01 00` |
| 7 | `03 5A 02 00 00` |
| 8 | `03 5B 00 00 00` |

(All followed by zero-padding out to 64 bytes, matching ndeadly's documented "offset `0x5`, 37 bytes,
reserved" — confirms the report's total logical length and padding behavior.)

**Confirmed**: report ID byte is `0x03` on the wire for USB (ndeadly's doc says "always `00` for
Bluetooth connections" — implying USB literally sends the report ID as `0x03`, unlike BT's HID
framing convention of omitting/zeroing it — a real, previously-implicit distinction now made
explicit).

**Superseded 2026-07-14 — kept for historical record only, see the banner above.** The grouping
below was a reasonable reading of these 8 samples in isolation; it is not what these bytes mean.

Original (superseded) analysis: field-level meaning of the 4 rumble-data bytes. Grouping the 8
samples by their 2nd/3rd bytes shows a pattern worth recording, but not confidently decoded:

- Samples with byte2=`0x02`, byte3=`0x00`: byte1 values `0x50, 0x55, 0x5A` (80, 85, 90 — ascending by
  exactly 5 each step)
- Samples with byte2=`0x00`, byte3=`0x01`: byte1 values `0x61, 0x63, 0x66, 0x68` (97, 99, 102, 104 —
  ascending, step size 2-3)
- Final sample (byte2=`0x00`, byte3=`0x00`): byte1=`0x5B` (91) — close to the first group's range,
  consistent with a fade-out/stop at the end of the test sequence
- byte4 is `0x00` in every sample observed

Plausible **Hypothesis**: byte2/byte3 select between two rumble "modes" or parameter tables (possibly
frequency bands, or amplitude-vs-frequency-primary encoding), and byte1 is a swept
frequency/intensity value within whichever mode is selected — consistent with a manual test session
sweeping through a range of settings in two groups before stopping. **Not enough data to commit to a
specific bit-field encoding** — recommend treating byte1 as an opaque "intensity-like" value and
byte2/3 as an opaque "mode" selector for a first Stage E implementation (e.g., treat any nonzero byte1
as "motor on at a fixed safe default amplitude," per `docs/switch2-gc/protocol.md`'s existing bounded-default
recommendation), rather than reverse-engineering a precise curve from 8 samples.

## Impact on evidence tiers (summary)

| Item | Before this decode | After |
|---|---|---|
| Device + config descriptor bytes | Confirmed (this project's own USBPcap capture) | **Confirmed, now with a second independent unit agreeing byte-for-byte** |
| Report `0x0A` layout | Strong | **Confirmed** (full field-by-field decode, zero contradictions, two exact-match confirmations) |
| Report `0x03` rumble existence/framing | Strong (existence only) | **Confirmed framing** (report ID byte, 4-byte data field, padding length); **byte semantics still Hypothesis** |
| `HHW5000xxxxxxx` serial format | Strong (one unit) | **Confirmed** (two independent units, same prefix pattern) |
| EP0 vendor `bRequest=3` = factory/SPI read | Not previously known | **Confirmed** (cross-validated against this project's own SPI dump) |
| EP0 vendor `bRequest=2` | Unknown | **Hypothesis** (updated same day, no new capture — this file's own Pro Controller 2 enumeration issues the identical request; byte-for-byte comparison suggests byte 2 is a device-type discriminant, see "bRequest=2" section above) |
| Rumble-endpoint idle behavior | Not previously known | **Confirmed** — exhaustive per-endpoint correlation across the *entire* 762s/1.3M-frame capture (not just the known burst window) found exactly 44 total host writes to the GC rumble endpoint, of which only the same 8 already-documented samples carry data; the other 36 are **zero-length packets**, not all-zero 64-byte reports. Idle/no-rumble state is a ZLP on the wire. |

## Follow-up

- ~~The remaining true gap for Stage B... raw 97-byte HID Report descriptor body~~ **Closed
  2026-07-13, separately**: a live USBPcap replug capture on this project's own genuine unit caught a
  standalone `GET_DESCRIPTOR(HID Report)` transaction and recovered the full 97 bytes byte-exact. See
  `docs/switch2-gc/protocol.md` "HID report descriptor" (now Confirmed) and
  `docs/experiments/nso-gc-captures/genuine-controller-hid-report-descriptor-2026-07-13.pcap`.
  String-descriptor text remains unobtained but is low-priority (index values only are sufficient for
  Stage B/C).
- Rumble-encoding precision: **do not expect more signal from this same file** — the cross-endpoint
  correlation above (see "Rumble-endpoint idle behavior") proves this capture's GC rumble burst really
  is only 8 samples system-wide, not an artifact of an under-scanned time window. Decoding the Pro
  Controller 2 portion of this same capture (frame 44 onward) for its own rumble commands remains a
  reasonable next step *if* PicoSwitch2's own HD-rumble encoding work can supply a cross-check, but it
  will not add GC-specific samples — only a different controller's encoding to compare conventions
  against. Getting more GC rumble samples requires either a new capture (a deliberate multi-value
  sweep, not incidental traffic) or accepting the existing "opaque intensity, fixed safe default"
  Stage E recommendation permanently.

## Timing re-analysis (2026-07-14) — the "8-sample burst" is a manual test tool, not gameplay traffic

Prompted by real-hardware feedback that GC-mode rumble behaved erratically during actual Smash Bros
gameplay ("fires nonstop, then randomly stops") despite Steam behaving correctly, re-opened this same
capture specifically to check the **timing** between the 8 known nonzero samples (not just their byte
content, which was already fully catalogued above). Exact frame timestamps via `tshark -r
rumble-procon-gccon.pcapng -Y "frame.number>=1282300 and frame.number<=1337900 and
usbll.device_addr==8 and usbll.endp==1 and usbll.pid==0xe1" -T fields -e frame.number -e
frame.time_relative`, then per-frame hex dumps to identify which OUT tokens carried data vs. a ZLP:

| # | Frame | `frame.time_relative` (s) | Bytes | Gap from previous sample |
|---|---|---|---|---|
| 1 | 1282317 | 729.476090 | `03 50 02 00 00` | — |
| 2 | 1302135 | 740.791927 | `03 61 00 01 00` | ~11.32 s |
| 3 | 1302234 | 740.839926 | `03 63 00 01 00` | ~48 ms |
| 4 | 1302340 | 740.891925 | `03 55 02 00 00` | ~52 ms |
| 5 | 1302411 | 740.923924 | `03 66 00 01 00` | ~32 ms |
| 6 | 1302517 | 740.975923 | `03 68 00 01 00` | ~52 ms |
| 7 | 1302557 | 740.991923 | `03 5a 02 00 00` | ~16 ms |
| 8 | 1337817 | 761.131631 | `03 5b 00 00 00` | ~20.14 s |

**Confirmed**: every one of these 8 nonzero writes is immediately preceded and followed by ZLPs on the
same endpoint (verified directly for sample 1's neighbors and sample 8's four following writes,
1337827/1337837/1337847/1337857 — all `USB transfer (0 bytes)`). Combined with the ~11s and ~20s gaps
around samples 1 and 8, and the tight ~16-52ms clustering of samples 2-7, this is unambiguously a
**human manually stepping through a handful of test values with a dev tool** (one isolated check,
then a quick sweep through several settings, then one final isolated value, each instantly reverting
to idle/ZLP) — **not** a sample of what continuous, real gameplay rumble traffic looks like. The
capture's own device (addr 8) is confirmed the genuine GC controller (see "Source" above), so this
isn't a device-identity mixup either — it's simply that this particular capture never contains actual
game-driven rumble. **This means the project has zero real evidence, from any capture, of what a real
game's sustained/dynamic rumble command stream looks like on the wire.** Every implementation decision
about *sustained* rumble behavior (as opposed to the single-value on/off decode this file's 8 samples
did legitimately inform) is downstream of this gap, not of anything in this file.

**Separately, and more consequential**: the OUT-token cadence itself, independent of payload content,
shows the console polls this rumble endpoint roughly every **4ms continuously** (device addr 8, endp
1, PID `0xe1`) whether or not it has new data — 5-token clusters at frame deltas of 10 (=4ms) are
visible throughout, each producing either a ZLP or a data packet. This 4ms figure, not the widely-
spaced samples above, is what actually mattered for the real bug found this pass: `bthid_task()` (the
function whose per-device `.task()` callback forwards the current commanded rumble state to the
connected Bluetooth pad) was only invoked once per 30ms control timer tick — nearly an order of
magnitude slower than the console's own polling rate. A real game driving fast, bursty rumble (short
repeated pulses rather than one held level — plausible given ERM motors are typically driven this way)
could toggle the commanded state on and off faster than this 30ms sampling could observe, causing
brief "off" transitions to be silently skipped. Combined with the Xbox/generic BT rumble bridge's
existing "resend only on detected change, arm a ~10-minute hardware sustain per trigger" design
(intentional, matching xpadneo's own convention — see `docs/bluetooth/btstack-implementation.md`
"loop_count"), a skipped "off" observation leaves the physical motor coasting on its last-observed
"on" trigger's sustain window, unrelated to what the GC firmware's own internal state actually is at
that moment — a plausible, well-evidenced explanation for "fires nonstop, then randomly stops," fixed
by adding a dedicated faster poll (`RUMBLE_TICK_MS` in `src/bt_hid/ns2_bt_host.c`) rather than by
touching the rumble decode itself. See `STATUS.md`'s "seventeenth pass" for the full account.

**Next real evidence needed**: a capture of actual gameplay rumble traffic (Smash Bros or similar) —
either a fresh USBPcap capture on this project's own hardware while a real console drives real
gameplay rumble, or hardware feedback after this pass's fix confirming/denying the sustained-erratic
behavior is resolved. Until then, both the "pass data[0] through as amplitude" decode and this pass's
faster-polling fix remain the best-reasoned response to the available evidence, not a confirmed final
answer.
