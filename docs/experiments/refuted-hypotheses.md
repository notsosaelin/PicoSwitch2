# Refuted Hypotheses — Archive

> This document exists so future contributors (and future LLM sessions) don't re-investigate a
> path this project has already ruled out with direct evidence. Per `CLAUDE.md`'s instruction to
> never let assumptions silently calcify into facts, and never silently discard evidence either —
> a hypothesis that turned out wrong is still a real finding, just not a live one. Live docs
> (`docs/switch2-gc/protocol.md`, `STATUS.md`, `DATA.md`) should only carry the *current*
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
synthetic `0x28` packet is sent. This result does **not** refute the byte-exact G6/G7/G8 codec or
the candidate “second quaternion/reference vector” interpretation in isolation; it refutes only
the complete template-derived packet. The unsafe UART generator and runtime gate were removed
after this result; only passive analysis and the exact field codec remain. Current evidence and
safe next work:
[`../switch2/uart-magprobe.md`](../switch2/uart-magprobe.md).

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

## Format notes for future entries

Each entry should have: the claim, the confidence level it held, why it was reasonable given the
evidence available *at the time*, and the specific evidence that overturned it. Link forward to
whichever live doc now carries the corrected understanding. Don't delete an entry once a "correct"
replacement is itself later refuted — append a note here instead, so the history of *why* stays
intact.
