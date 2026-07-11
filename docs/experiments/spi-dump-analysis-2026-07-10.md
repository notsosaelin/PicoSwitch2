# Experiment — Genuine Pro Controller 2 SPI Flash Dump Analysis (2026-07-10)

**Status:** ✅ Complete for what these two dumps can answer. **Confirms** existing factory-block
RE; **discovers** three previously-undocumented structures (BT bond table, battery discharge
curve, a DSP firmware blob); **does not** resolve report-0x09 motion semantics — the motion
calibration region is unprogrammed on this unit, and the bulk of the image is high-entropy
(encrypted/compiled) data with no plaintext motion/quaternion logic found.
**Date:** 2026-07-10. **Hardware:** two full 2 MB SPI flash dumps of the same physical Pro
Controller 2 (PID `0x2069`), taken 2026-07-06 and 2026-07-10.

---

## 1. Question

Does the controller's own SPI flash contain anything that resolves the report-0x09 motion
format's open questions (phase semantics, the "second lane group" magnetometer lead, calibration
coefficients) — either directly (plaintext data/coefficients) or indirectly (comparing two dumps
taken days apart to see what changes with normal use)?

## 2. Method

Two 2 MB (`0x200000`-byte) dumps, `dumps/2069_spi_dump_2026-07-06_2107.bin` and
`dumps/2069_spi_dump_2026-07-10_1422.bin`. Analysis (pure Python, no external tools): full
byte-diff, per-4KB Shannon entropy map, targeted hex dumps of every offset our firmware currently
synthesizes (`ns2_factory_init()` in `switch_pro2.c`), a search for the motion-cal validity marker
(`B2 A1`, per `report-0x09-motion.md`), ASCII/UTF-16LE string extraction, and a 4-byte-aligned
"magic + plausible length field" scan across the whole image.

## 3. Results

### 3.1 The two dumps are byte-identical
Both files: `0x200000` bytes, SHA-256
`bb5c0fb41d24b93b368ef2194ad876bce524230255e3b49c1045fd96f69a0fb5` (identical hash on both).
`diff == 0` across all `0x200000` bytes. Four days and (presumably) normal use produced **no
change** on this unit's SPI flash. This rules out "diff two dumps to find dynamic state" as a
viable technique *for this pair* — either nothing here is session/use-dependent (most likely: the
flash holds firmware + factory-programmed data, and *runtime* state like bond/bias/bootcount lives
in RAM or a different NVM entirely), or nothing changed to trigger a flash write between captures.

### 3.2 Known factory offsets: exact match, byte-for-byte
Every offset `ns2_factory_init()` (`switch_pro2.c`) currently hard-codes — identity block
(`0x13000`), serial, VID/PID, colours, the two calibration "blk" arrays (`0x13080`/`0x130C0`),
stick calibration (`0x130A8`/`0x130E8`) — matches the dump **exactly**, on both captures. This is
strong independent confirmation that the prior RE of this region (from a third-party USB capture,
not this dump) is correct down to the byte.

### 3.3 Motion calibration region (`0x1FC000`, len `0x40`): unprogrammed
All `0xFF` on both dumps, consistent with `report-0x09-motion.md`'s documented "reset to default"
state (64× `0xFF`) — this unit's motion has never been calibrated through the console's calibration
flow. **No motion coefficients recoverable from this pair.** No occurrence of the `B2 A1` validity
marker anywhere near this region (18 occurrences of that byte pair exist in the image, all inside
the high-entropy blob at `0x02xxxx`-`0x1axxxx` — coincidental, not validity markers; that marker
only means anything at `0x1FC000` per the existing doc).

### 3.4 New, previously-undocumented structure: a real Bluetooth bond table (`0x1FA000`)
Our firmware currently hard-codes this address to read back `0x00` ("zero stored bonds";
`ns2_mem_read()`, `switch_pro2.c`). The genuine unit has **real data** here — an 8-byte table
header (`02 30 05 20 00 00 00 00`) followed by two 40-byte bond records:

```
record[0] @ 0x1FA008: BD_ADDR <redacted, 6 bytes>, then a 16-byte key <redacted>
record[1] @ 0x1FA030: BD_ADDR <redacted, differs from record[0] only in the low byte>, SAME 16-byte key as record[0]
```
(Raw address/key bytes intentionally not reproduced here — they identify a real, specific paired
console. Structure only.) The two addresses differ only in the last byte and **share one key** —
the classic signature of one physical host (a Switch 2 console) bonded via two related BT
identities (e.g. BR/EDR classic + LE, linked by BT 4.2+ cross-transport key derivation) rather than
two different hosts. Slots beyond the two populated records are `0xFF` (empty capacity for more
bonds). **Not directly relevant to report 0x09**, but directly relevant to the standing
"BT pairing reliability — Pro 2 reconnect sometimes needs a triple-tap" item (`STATUS.md`
Deferred/Blocked): our device currently always claims zero bonds, which may itself be part of why
reconnect behaves differently than a genuine, already-bonded unit. Filed as a lead for that
backlog item, not pursued further this pass (out of scope for report 0x09).

**Independently confirmed, field-for-field, 2026-07-10** by `tools/switch2_input_viewer.py` (a
working third-party BLE client added to this repo): its `handle_read_response()` decodes address
`0x1FA000` as `host_address1 = data[0x8:0xE]`, `host_address2 = data[0x30:0x36]`,
`ltk = data[0x1A:0x2A][::-1]` (reversed on wire — the same reversal convention this repo's own
pairing crypto already uses, `ns2_rev16()` in `switch_pro2.c`). Those offsets land exactly on the
two BD_ADDR fields and the shared key this analysis found independently by structure alone —
strong cross-validation from an unrelated source, and confirmation this really is an LTK (Long
Term Key), not merely "some 16-byte value." Raw bytes remain redacted here for the same reason as
above.

### 3.5 New: a real per-unit battery discharge curve (`0x1FB000`)
A short table of monotonically-decreasing `uint16_le` values (`0x0E99, 0x0DD4, 0x0D4C, 0x0CD5,
0x0C90, 0x0C66, 0x0BFD, 0x0BF7, 0x0BAA, 0x0AAA, …`) immediately following a small header
(`02 00 02 00 c8 00 78 00`). Shape and location are consistent with a battery
voltage/capacity lookup table, not motion data. `switch_pro2.c`'s `0x0B` battery-command handler
currently returns fixed placeholder values (`0xA5,0x0E,...` / `0x34,0x00,0x83,0x00`) rather than
anything derived from a real curve. **Not pursued further this pass** (out of scope for report
0x09) — filed for whenever battery-reporting fidelity becomes a priority.

### 3.6 New: an unencrypted(?) DSP firmware/coefficient blob, `"DSPH"` magic at `0x175000`
A clean, self-describing header: magic `44 53 50 48` (`"DSPH"`), then a `uint32_le` length field
`0x00032AB0` (207,536). The data from `0x175000` to `0x175000 + 0x32AB0 = 0x1A7AB0` is **exactly**
followed by zero-padding then erased (`0xFF`) flash — i.e. **the embedded length field is
correct**, confirming this is a real, well-formed, self-contained firmware/data section, not a
false positive from the magic-byte scan (which otherwise produced ~268 noise hits from
high-entropy data misread as 4-letter tags — cross-checked and discarded). The blob's tail
(`0x1A7A70`-`0x1A7AB0`) is a smooth, monotonically-converging sequence of `int16` values
(`0xEBB2 → 0xEB18 → 0xEA82 → … → 0xE000`) — classic shape for a decaying filter-coefficient table
(FIR/IIR taps or a window function), not compiled code or compressed data. The bulk of the blob
between header and tail is high entropy (~7.0-7.4 bits/byte) — likely compiled DSP code or
compressed audio/haptic data. **Almost certainly audio or haptic-waveform DSP** (the PC2's USB
descriptor advertises 3 audio interfaces, and HD-rumble synthesis is DSP-shaped work) rather than
motion/sensor-fusion — there is no independent evidence tying it to the IMU pipeline. **New RE
target, but out of scope for report 0x09**; flagged in `PLAN.md`'s backlog as a candidate for a
future haptics/audio-fidelity pass, not investigated further here.

### 3.7 Update (2026-07-10, later): factory motion calibration WAS recoverable — just not at the address this analysis checked
§3.3 above concluded "no motion coefficients recoverable" because it only checked the *user*
motion-calibration region (`0x1FC000`), which is genuinely unprogrammed on this unit. A separately
supplied tool, `tools/switch2_input_viewer.py` (a working third-party BLE client), decodes a
**different, always-populated factory block** this analysis hadn't targeted: `0x13040`
(temperature + gyro bias, as `float32`) and `0x13100` (magnetometer bias + accelerometer bias, as
`float32`). Applying that tool's exact field offsets to this same dump recovers real, physically
plausible values (accelerometer Z bias ≈ 10.4, close to standard gravity in m/s² — confirming
SI-unit float calibration, not raw counts). Full decode and values:
[`docs/switch2/report-0x09-motion.md`](../switch2/report-0x09-motion.md) "Factory motion
calibration" section. This **revises §4's conclusion below** — see the update there.

### 3.8 The rest of the image: high entropy, consistent with encrypted/compiled firmware
~1.7 MB of the 2 MB image sits at 7.9+ bits/byte entropy (max possible is 8.0) — indistinguishable
from encrypted or well-compressed data by this analysis. No plaintext strings, no recognizable
code patterns, no motion/gyro/quaternion-related structures found anywhere outside the regions
above. This is consistent with the third-party notes in `CORTEX_PARSE.md`: cracking the actual
firmware logic (as opposed to the factory data blocks, which are plaintext and already RE'd)
likely requires a bootrom exploit or glitch attack, not passive dump analysis — **not attempted
this pass** (out of scope; no such capability exists in this project currently).

## 4. Conclusion — relevance to report 0x09

**Mixed result, revised 2026-07-10.** The *user* motion-calibration region (`0x1FC000`) is
unprogrammed on this unit and yields nothing. But the separately-supplied
`tools/switch2_input_viewer.py` revealed this analysis had been looking in only one of two
motion-calibration locations — the *factory* block (`0x13040`/`0x13100`) **was** recoverable and
**was** in this dump all along, just not decoded until that tool's field offsets were applied
(§3.7). Neither region contains recoverable plaintext motion/quaternion *logic* (that, if it
exists as distinct code, is inside the high-entropy region this analysis cannot read) — but the
factory calibration values are real, physically-plausible, per-axis floats, and are now folded
into `docs/switch2/report-0x09-motion.md`. The dumps also independently confirm the existing
factory-block RE is byte-exact, and surface three further new, real, previously-undocumented
structures (BT bond table — since independently cross-validated by the same tool, §3.4; battery
curve; DSP blob) — none of which bear on report-0x09's *wire format*, but all of which are
legitimate RE targets, filed in `PLAN.md`.

## 5. Remaining questions / suggested future work

1. If a future unit's motion **has** been calibrated through the console UI, dumping its flash
   would recover *user*-calibration coefficients at `0x1FC000` too (the *factory* block at
   `0x13040`/`0x13100` is now decoded, §3.7, and needed no such prerequisite). Requires
   deliberately calibrating gyro
   in-console on a genuine unit, then dumping.
2. The BT bond-table format (§3.4) is a candidate for improving pairing-reliability realism —
   worth a dedicated look under the existing "BT pairing reliability" backlog item.
3. The `"DSPH"` blob (§3.6) is a candidate for a future audio/haptics-fidelity investigation, not
   gyro. If pursued, start by diffing its high-entropy body against a second unit's dump (identical
   region = firmware/constant; differing = per-unit calibration) — this pair can't do that (same
   unit, identical dumps).
4. A bootrom/glitch attack against the controller's SoC (per `CORTEX_PARSE.md`) is the only path
   identified so far to read the *executing* firmware logic rather than factory data blocks — not
   scoped or attempted in this project.
