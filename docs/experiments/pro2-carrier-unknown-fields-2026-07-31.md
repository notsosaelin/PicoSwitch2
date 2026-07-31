# Pro Controller 2 motion carrier — remaining unknown fields — 2026-07-31

## Question

After the length-`0x1E`/`0x28` decode, is anything left undecoded? A byte-exact
re-encoding test answers this directly: fields that can be reproduced but not
explained are exactly the ones a synthesizer would get wrong.

## Method

`tools/ns2_motion_packet.py` builds complete PDUs; `tools/test_ns2_motion_packet.py`
decodes every genuine PDU in the repository corpus, re-encodes it from the
decoded fields, and compares bytes.

```powershell
python tools\test_ns2_motion_packet.py
python tools\test_ns2_motion_carrier.py
```

Corpus: 52 captures under `dumps/BLE CAPTURE/` and `dumps/motion/`.

## Result: the decode is byte-exact

| Form | Records | Re-encoded byte-for-byte |
|---|---:|---|
| `0x28` high-rate | 858 | ✅ |
| `0x28` normal | 149 | ✅ |
| `0x28` catch-up | 981 | ✅ |
| `0x1E` carrier | 2,070 | ✅ |

Zero mismatches. Every field position, width, sign convention, and the LSB-first
packing are confirmed against genuine hardware.

**Byte-exactness is reproduction, not comprehension.** Two fields are reproduced
without being understood, and the test is what exposed them — an earlier pass
silently zeroed one of them and still produced "plausible" packets.

## Unknown 1: length-`0x1E` byte 12, bit 7

Set in **280 of 2,070** genuine carriers (13.5%).

**It tracks motion.** Median inter-record carrier change:

| Flag | Median max abs change |
|---|---:|
| set | `0.001280` |
| clear | `0.000012` |

A 100× separation. It is 0% across stationary captures and 23–48% across moving
ones (`sw2_native_passthrough_live` 37%, `variant9_fast_link` 39%,
`chart-face-forward` 48%, the three Splatoon boundary captures 23–29%).

It is **not** a simple threshold on that change — thresholding at the median
gives only 21% precision against a 13.4% base rate — and it alternates rapidly
inside moving captures (85 runs across 185 records).

Ruled out, none beating the 86.5% trivial-zero baseline:

| Candidate | Agreement |
|---|---:|
| sign of retained lane 0 / 1 / 2 | 28.1% / 39.4% / 81.1% |
| lane-1 bit 24 / bit 25 | 60.6% / 86.5% (bit 25 is always zero) |
| tick parity | 50.0% |
| previous record's flag | 84.5% |
| retained energy > 0.75 | 20.8% precision |
| retained energy > 1.0 | 14.3% precision |
| max abs retained > 0.95 / 0.99 of limit | 27.0% / 25.9% precision |
| second-largest magnitude > 0.5 | 19.9% precision |

### ❌ Refuted: whole-quaternion sign canonicalization

The obvious candidate was that the bit records whether the omitted component was
negated before packing. Nintendo's Switch 1 DScale packer performs exactly that
negation and does not transmit the result; if Switch 2 transmitted it, this is
where it would live. It also predicted the observed behaviour — a stationary
controller holds one sign, a rotating one crosses zero repeatedly.

**No hardware was needed to test it.** A whole-quaternion negation flips all
three transmitted lanes together, so negating the later record must restore
continuity exactly when the flag toggles. Over 160 adjacent same-chart toggles
across six captures, including the 94-record all-state-3
`pro2-chart-face-forward-no-transition` capture:

| Adjacent same-chart pairs | n | median `d_raw` | median `d_neg` | negation helps |
|---|---:|---:|---:|---:|
| flag toggles | 160 | `0.002511` | `1.318595` | **0 / 160** |
| flag unchanged | 375 | `0.003473` | `1.304876` | 0 / 375 |

The trajectory is already smooth without negation in every single case.
Refuted.

### ❌ Refuted: interleaving or cadence structure

The flag rate is flat regardless of packet context — 12% when the next PDU is a
`0x28` versus 14% when it is not, 13% either way for the preceding PDU, and
13–14% across every tick-gap bucket (5, 6, 11, 12, 13). No relation to the
`0x1E`/`0x28` interleave or to the connection interval.

### Status

Characterized but unexplained. It is **not** relied upon anywhere, and
`tools/ns2_motion_packet.py` preserves it verbatim.

What is established: it tracks motion strongly (100× separation), it is
per-sample rather than modal, and it is none of the twelve candidates tested.
A useful next probe would target what changes *within* motion rather than
between motion and rest, since the toggle rate does not track motion magnitude
(`d_raw` 0.0025 on toggle versus 0.0035 on hold — if anything slightly lower).

### Adjacent bits, for completeness

Bits 2–6 of byte 12, and bits 2–7 of bytes 4 and 8, are zero in **every** genuine
record. Byte 12 bit 1 is never set, which independently confirms carrier lane 1
is **25 bits**, not the 26 the decoder's mask would allow.

## Unknown 2: length-`0x28` status `0x00`

Five packets out of 1,988 carry sensor status `0x00` instead of `0x0D`/`0x0E`/
`0x0F`. All five are in `pro2-imuref-15ms-raw-native-raw-2026-07-29.jsonl`, a
raw↔native handoff capture.

The remaining 1,983 packets agree with layout selection exactly: `0x0D` ⇔
elapsed 0–10, `0x0E` ⇔ 11–14, `0x0F` ⇔ 15+.

`docs/switch2/ble-controller-protocol-inventory.md` records `0x04` as "no data
was read from the IMU" for a different report length, so a "no new IMU data"
meaning for `0x00` is plausible but unproven. Their concentration in a handoff
capture also admits a transitional explanation.

`tools/ns2_motion_packet.py` never emits status `0x00`: a synthesizer must not
claim a condition it does not model. The test counts these rather than folding
them into a layout, and fails if they stop being rare.

## Consequence for a synthesizer

The carrier, chart hysteresis, saturation trigger, prefix slice, epoch, all three
cadence layouts, their packed IMU fields, and the Q3 temperature tail are
decoded and byte-exact.

Two fields are reproduced but not understood. Neither blocks generation — both
are carried through verbatim — but the byte-12 flag should be resolved before
claiming the format is fully understood, because if the sign hypothesis holds it
changes what a decoder can recover.

## Reproduce

```powershell
python tools\test_ns2_motion_packet.py    # 10 tests, byte-exact corpus replay
python tools\test_ns2_motion_carrier.py   # 20 tests, codec + firmware parity
```
