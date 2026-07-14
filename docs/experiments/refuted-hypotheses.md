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

## Format notes for future entries

Each entry should have: the claim, the confidence level it held, why it was reasonable given the
evidence available *at the time*, and the specific evidence that overturned it. Link forward to
whichever live doc now carries the corrected understanding. Don't delete an entry once a "correct"
replacement is itself later refuted — append a note here instead, so the history of *why* stays
intact.
