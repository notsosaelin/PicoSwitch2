# Pro Controller 2 reciprocal carrier-chart transition — 2026-07-29

## Question

How does a genuine Pro Controller 2 change the chart/state in its interleaved
length-`0x1E` carrier, and which chart does the length-`0x28` prefix use when its
sampling epoch falls inside that transition?

This matters because the prior diagnostic treated the two-bit state as a strict
smallest-three omitted-quaternion-component selector. That interpretation had
worked in state-stable captures but had never crossed a chart boundary.

## Method

A genuine Pro Controller 2 was placed flat on a lazy susan. The adapter ran the
passive `motionpair trigger` capture, which:

- retained 63 records of rolling prehistory;
- triggered on the first length-`0x1E` state change;
- retained 64 post-trigger records;
- did not change controller output, report cadence, CCC ownership, or the
  production motion path.

The maintainer performed one controlled half-turn and its reciprocal return.
Both controlled captures contain 127 records and zero drops. Three passive
gameplay captures later extended the boundary corpus. The five primary files
are:

- [`pro2-chart-transition-lazy-susan-2026-07-29.jsonl`](../../dumps/BLE%20CAPTURE/pro2-chart-transition-lazy-susan-2026-07-29.jsonl)
- [`pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl`](../../dumps/BLE%20CAPTURE/pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl)
- [`pro2-chart-transition-splatoon-0-to-1-2026-07-29.jsonl`](../../dumps/BLE%20CAPTURE/pro2-chart-transition-splatoon-0-to-1-2026-07-29.jsonl)
- [`pro2-chart-transition-splatoon-3-to-1-2026-07-29.jsonl`](../../dumps/BLE%20CAPTURE/pro2-chart-transition-splatoon-3-to-1-2026-07-29.jsonl)
- [`pro2-chart-transition-3-to-2-2026-07-29.jsonl`](../../dumps/BLE%20CAPTURE/pro2-chart-transition-3-to-2-2026-07-29.jsonl)

The two controlled-capture SHA-256 values are
`2754446EDA522EC1DA3E5AFB588B1ACC8D0F4F0C6A92D1D28024DD2359DD1630` and
`5B819C0A0AAF80868518D4766B79BBE92315A5DC7025954F4374F82B1F914E19`,
respectively.

The third capture came from the unresolved-state trigger during an ordinary
Splatoon raid, with unrestricted movement across all axes. It contains one
zero-drop `0 → 1` boundary and SHA-256
`0DABE00CD81746AA66DA5AE166ABDC47DA117E96849E435D67A49468DD91F4DD`.

The fourth capture came from a separate Splatoon raid with unrestricted rapid
movement. It contains a zero-drop `3 → 1 → 0` sequence, with state 1 present
for exactly one length-`0x1E` record. Its SHA-256 is
`E8454FC73B08458DEFB2C3E6EF506BF29D73BC6E13C92E9434FB15815D760622`.

The fifth capture came from the state-2-only unresolved trigger during ordinary
gameplay. It contains 127 notifications with zero drops: 93 length `0x1E`,
34 length `0x28`, state counts `{0:25, 1:19, 2:26, 3:23}`, and transitions
`0 → 1 → 3 → 2 → 3`. Its SHA-256 is
`FDDD5C028E59D149A424A3226382C7FDDDF98337699677AC870DBEB0F84B2270`.

Reproduce:

```powershell
python tools\ns2_motion_reference.py `
  --motionpair "dumps\BLE CAPTURE\pro2-chart-transition-lazy-susan-2026-07-29.jsonl"

python tools\ns2_motion_reference.py `
  --motionpair "dumps\BLE CAPTURE\pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl"

python tools\ns2_motion_chart_solver.py `
  "dumps\BLE CAPTURE\pro2-chart-transition-lazy-susan-2026-07-29.jsonl" `
  "dumps\BLE CAPTURE\pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl" `
  "dumps\BLE CAPTURE\pro2-chart-transition-splatoon-0-to-1-2026-07-29.jsonl" `
  "dumps\BLE CAPTURE\pro2-chart-transition-splatoon-3-to-1-2026-07-29.jsonl" `
  "dumps\BLE CAPTURE\pro2-chart-transition-3-to-2-2026-07-29.jsonl"

python tools\test_ns2_motion_reference.py
```

An orthogonal control then placed the controller upright, with its face buttons
forward, and rotated it on the same lazy susan. The 30-second trigger window
never observed a chart change: every one of the 94 retained length-`0x1E`
records remained state 3. The ring's `3881` dropped count is expected rolling
pre-trigger overwrite; no trigger ever froze a window. This is useful negative
evidence, but not a transition fixture:

- [`pro2-chart-face-forward-no-transition-2026-07-29.jsonl`](../../dumps/BLE%20CAPTURE/pro2-chart-face-forward-no-transition-2026-07-29.jsonl)
- SHA-256:
  `E7347A368745A1D98D831B8318B5486F277875B6BE6DEE4D3C9F82BC817D7EF1`

## Initial length-`0x1E` result

The first three crossings are each smooth under these state-0-boundary lane
projections:

```text
state 0 wire (G0, G1, G2) -> state-0 boundary projection (G0, G1, G2)
state 1 wire (G0, G1, G2) -> state-0 boundary projection (G2, G0, G1)
state 3 wire (G0, G1, G2) -> state-0 boundary projection (G1, G2, G0)
```

| Capture | Transition | Last old-chart canonical | First new-chart canonical | Delta norm |
|---|---:|---|---|---:|
| half-turn | `3 → 0` | `(0.032449, -0.160403, 0.706999)` | `(0.032420, -0.159819, 0.704504)` | `0.002563` |
| return | `0 → 3` | `(-0.585400, -0.430567, 0.705397)` | `(-0.586127, -0.429904, 0.705957)` | `0.001132` |
| Splatoon raid | `0 → 1` | `(0.699587, -0.641319, -0.298600)` | `(0.690902, -0.651341, -0.287923)` | `0.017025` |

Those observations establish local continuity at the captured state-0 seams.
They do **not**, by themselves, prove that the projections compose into one
global three-lane frame for every state-to-state transition.

The return capture is also direct counter-evidence to a literal strict
smallest-three quaternion. Sixteen of its 93 length-`0x1E` records have
`sum(retained²) > 1`; the maximum is `1.026738`. These are genuine, zero-drop
records around the rapid turn. A positive-square-root omitted component does not
exist for those samples, yet the cyclic three-lane carrier remains smooth across
the transition.

At this stage the evidence established:

- the reciprocal state-0/state-3 edge repeatedly selects `(G1,G2,G0)`;
- the captured `0 → 1` edge selects `(G2,G0,G1)` unambiguously: its `0.017025`
  boundary
  residual is about 29 times smaller than the `0.499391` runner-up;
- state 2 remains unresolved because it has never been observed;
- the old strict-smallest-three reconstruction remains a useful stable-state
  diagnostic and a hardware-validated production approximation for translated
  controllers, but it is not an exact model of genuine Pro2 carrier behavior.

## Rapid `3 → 1 → 0` correction

The fourth zero-drop capture is a held-out stress case for the global
unsigned-permutation hypothesis:

- 127 native notifications: 86 length `0x1E`, 41 length `0x28`;
- state counts `{0: 43, 1: 1, 3: 42}`;
- transitions `3 → 1` and `1 → 0`;
- zero strict-unit violations;
- all 41 length-`0x28` elapsed values match the preceding carrier delta.

Fitting each chart edge independently gives:

| Edge | Observations | Best higher-to-lower unsigned permutation(s) | Residual |
|---|---:|---|---:|
| `0 ↔ 3` | 2 | `(G1,G2,G0)` in both directions | `0.001132..0.002563` |
| `0 ↔ 1` | 2 | `(G2,G0,G1)` and `(G2,G1,G0)` | `0.017025..1.185389` |
| `1 ↔ 3` | 1 | `(G2,G1,G0)` | `0.047878` |

The full corpus cannot be represented by one **stateless unsigned**
permutation per state.
The best global candidate has RMS/max residual `0.818124/1.252822`, and its
largest excess over the independently best edge fits is `1.204945`. The
immediate `1 → 0` boundary cannot be made continuous by any unsigned
permutation (`1.185389` minimum).

The narrower cyclic omission/sign-branch model does explain all five
boundaries without per-edge lane guessing. For chart `s`, the wire order is
the three cyclic component slots following `s`. At an `a ↔ b` boundary, the
component omitted by one chart substitutes for the component omitted by the
other. Canonicalizing the omitted component's sign creates exactly two
branches:

1. same omitted-component sign: use the topology permutation unchanged;
2. opposite omitted-component sign: keep the boundary lane and negate the
   other two.

| Transition | Topology permutation | Selected branch | Residual | Other-branch residual |
|---|---|---|---:|---:|
| `3 → 0` | `(G1,G2,G0)` | same sign | `0.002563` | `0.326736` |
| `0 → 3` | `(G1,G2,G0)` | same sign | `0.001132` | `1.453577` |
| `0 → 1` | `(G2,G0,G1)` | same sign | `0.017025` | `1.419526` |
| `3 → 1` | `(G2,G1,G0)` | same sign | `0.047878` | `1.435034` |
| `1 → 0` | `(G2,G0,G1)` | opposite sign `(+,−,−)` | `0.024716` | `1.378808` |

The model's RMS/max residual is `0.025302/0.047878`; every selected branch
beats its alternative by at least `0.324174`. The negative `1 → 0` crossing is
the decisive observation: it exercises the paired-sign branch predicted by
cyclic omitted-component sign canonicalization.

Nintendo's earlier Switch-1 DScale packer in MissionControl uses the same
cyclic component order and omitted-component sign normalization. That source
was used only to formulate the narrow candidate; the values and branch
selection above come from direct genuine-Pro2 captures. See
[`switch_motion_packing.cpp`](https://github.com/ndeadly/MissionControl/blob/master/mc_mitm/source/controllers/switch_motion_packing.cpp).

This refutes the **stateless global unsigned composition**, not the cyclic
stateful chart law or the captured local seam measurements. The prior state-0
projections are the same-sign branch of the broader law. State 1 now has
same-sign and opposite-sign hardware evidence.

## State-2 closure

The fifth capture closes the previously unseen chart state. It crossed `3 → 2`
and returned `2 → 3` in one zero-drop window. The unsigned lane-permutation
fit is deliberately not accepted here: its residuals are `1.777474` and
`0.985571`. Both directions instead independently select the cyclic topology
permutation `(G2,G0,G1)` and the opposite omitted-sign branch `(+,−,−)`:

| Transition | Selected residual | Other-branch residual | Margin |
|---|---:|---:|---:|
| `3 → 2` | `0.036162` | `1.783082` | `1.746919` |
| `2 → 3` | `0.011824` | `0.996669` | `0.984845` |

The same capture also contains new `0 → 1` and `1 → 3` crossings. Across all
nine accepted boundaries from the five-capture corpus, the cyclic
omitted-component/sign-branch model now has:

- direct adjacent evidence for chart states 0, 1, 2, and 3;
- six same-sign and three opposite-sign selections;
- RMS/max residual `0.023541/0.047878`;
- minimum rejected-branch margin `0.324174`.

Its length-`0x28` prefix at epoch 3892 falls inside the `3 → 2` boundary.
Evaluating both wire charts in the selected state-2 local frame chooses chart 3
with modular windows `(2,2,−2)` and residual `0.003833`; chart 2 leaves
`0.196168`. This is the first direct state-2 prefix-seam result.

This resolves chart-state coverage and supports one stateful topology across
every captured boundary. It does **not** prove the genuine controller's exact
integer projection/rounding, nor does it authorize replacing the
hardware-validated translated length-`0x1E` carrier with a generated
length-`0x28` packet.

## Length-`0x28` prefix at the seam

The established prefix epoch is
`current tick - encoded elapsed + 4`. Each capture contains exactly one prefix
whose epoch falls between the final old-chart and first new-chart length-`0x1E`
record.

| Capture | Bracketing transition | Prefix packet / epoch | Selected chart | Modular windows | Canonical residual |
|---|---:|---:|---:|---|---:|
| half-turn | `3 → 0` | `3569 / 3566` | `0` | `(0, 0, 2)` | `0.000144` |
| return | `0 → 3` | `3726 / 3723` | `0` | `(-2, -1, 2)` | `0.000790` |
| first Splatoon raid | `0 → 1` | `4291 / 4288` | `1` | `(-2, -1, 2)` | `0.010524` |
| second Splatoon raid | `3 → 1` | `4305 / 4301` | `1` | `(1, 2, -2)` | `0.008416` |
| state-2 gameplay | `3 → 2` | `3895 / 3892` | `3` | `(2, 2, -2)` | `0.003833` |

Testing both bracketing charts selects state 0 unambiguously in both
directions. The rejected state-3 residuals are `0.228973` and `0.211835`.
The first Splatoon seam selects state 1 over state 0 (`0.010524` versus
`0.091224`).
Using the capture-proven cyclic local frame, the direct `3 → 1` seam selects
state 1 over state 3 (`0.008416` versus `0.242898`), and the `3 → 2` seam
selects state 3 over state 2 (`0.003833` versus `0.196168`). This establishes
the observed seam choices across all four chart states, not exact integer
projection/rounding. A same-epoch full-resolution reference is still needed
before an exact generator can be claimed.

The prefix remains a deterministic truncated projection of the carrier. Stable
records retain the previously established fixed scales and
`preceding carrier + 4` epoch. Exact integer projection/rounding at the seam is
not proven by interpolation alone.

## Tooling and regression boundary

`tools/ns2_motion_reference.py` now reports:

- state counts and state transitions;
- retained-energy violations of the strict unit constraint;
- the capture-proven stateful cyclic local projection and sign branch for
  every adjacent boundary;
- prefix epochs that fall across a chart transition and both candidate-chart
  residuals, including direct nonzero/nonzero seams.

`tools/test_ns2_motion_reference.py` locks all five boundary captures into the
offline regression suite, including the `3 → 1` and state-2 prefix seams in
their capture-proven local cyclic frames.

`tools/ns2_motion_chart_solver.py` independently searches all unsigned lane
permutations using only adjacent cross-state records. It now reports every
chart edge separately and audits whether those local fits compose into one
global state-0 frame. On the reciprocal state-0/state-3 fixtures it selects
`(G1,G2,G0)` with transition RMS/max residual `0.001981/0.002563`; the
runner-up squared-error margin is `0.122525`. On the four-capture corpus it
rejects global unsigned composition. It then tests only the two sign branches
permitted by the cyclic omitted-component topology; all five observed
boundaries select one unambiguously. An arbitrary signed-lane fit remains
diagnostic only. The solver intentionally refuses to bridge separate captures.

The focused five-capture boundary corpus contains 456 length-`0x1E` records:

| State | Records | Evidence status |
|---:|---:|---|
| 0 | 209 | anchor chart |
| 1 | 63 | same-sign and opposite-sign branches captured |
| 2 | 26 | reciprocal state-2/state-3 edge captured |
| 3 | 158 | reciprocal state-0 and state-2 edges captured |

The wider repository corpus contains 1,030 stable state-1 records, but state 1
first appeared after a 16-minute gap between separately captured stationary
windows. Its abundance did not make that gap a valid continuity constraint.
The opportunistic trigger supplied a clean `0 → 1` boundary, the later
`3 → 1 → 0` counterexample, and finally a reciprocal `3 → 2 → 3` crossing.
All four states are now covered by adjacent transition evidence.

The passive trigger's `unresolved` target mask ignored
state-0/state-1/state-3 transitions while advancing its true baseline and froze only
when state 2 participated. It supplied the missing coverage during ordinary play
without a prescribed physical maneuver. Its pure
state machine is covered by `tools/test_ds5_motion_chart_trigger.c`; it never
changes the controller's native PDU.

No production motion generator changed. Genuine Pro2 packets are still passed
through opaquely, and DualSense/Edge remain on the hardware-validated
length-`0x1E` generator. Extending the genuine carrier model is gated on direct
evidence for exact integer projection/rounding and coherent generation of every
console-relevant changing lane.
