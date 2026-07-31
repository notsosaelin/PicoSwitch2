#!/usr/bin/env python3
"""Bit-exact codec for the genuine Switch 2 orientation carrier.

This is the inverse of the decoding in ``ns2_motion_reference.py``. It exists so
a candidate length-`0x1E` / length-`0x28` generator can be validated entirely
offline, against genuine captures, before any hardware trial. Nothing here is
wired into firmware.

What the captures established
-----------------------------
The length-`0x1E` carrier is three unsigned integer lanes of 26/25/24 bits. Each
spans the full representable range of one retained quaternion component:

    retained[i] = (g[i] / 2^(26-i) - 0.5) * sqrt(2)      -> +/- sqrt(2)/2

Chart selection is **not** strict smallest-three. A genuine controller keeps its
current chart until a projected lane would leave that range, and only then
re-charts. That is why retained energy climbs past 1.0 and peaks exactly at the
observed swap (max 1.346176 across the corpus): between swaps the controller is
transmitting a component that has become the largest. Nine captured boundaries
covering all four chart states support this, and the pre-swap/non-swap value
ranges overlap, which rules out a plain "current sample exceeded a threshold"
rule and leaves the one-sample lookahead implemented here.

The length-`0x28` prefix is a modular slice of the same integers, sampled four
ticks after the preceding carrier:

    high-rate      w0 = (g0 - 2^25) mod 2^24   s24   (same LSB as the carrier)
                   w1 = (g1 - 2^24) mod 2^23   s23
                   w2 = (2*(g2 - 2^23) + 2^24) mod 2^25   s25
    normal/catchup the same values with two low bits dropped (s22/s21/s23)

What the chart state is, and is not
-----------------------------------
The two-bit state is a **cyclic lane rotation**, not a component substitution.
Expressed in a common frame, the records either side of a swap carry the same
three values (canonical triples agree to 0.0007-0.100 on every boundary the
unsigned projection covers). No fourth component is ever observable, so a
decoder cannot recover one, and any analysis that "unions" the two sides of a
swap to reconstruct a full quaternion is invalid.

The carrier is also not three components of a unit quaternion: the canonical
three-vector reaches norm 1.160, and no three components of a unit quaternion
can exceed 1. The likeliest cause is the controller integrating without
renormalizing between infrequent cleanup passes. This is an open question about
*decoding* genuine data; it does not affect generation, which starts from our
own unit quaternion and never inverts the relation.

Rounding
--------
High-rate carries the carrier's own LSB, so no rounding occurs — it is a pure
bit slice, confirmed by an exactly-zero integer residual at rest. The two bits
dropped by the normal/catch-up layouts are not measurable from any existing
capture: `0x1E` only interleaves at the 7.5 ms production cadence, where the
layout is always high-rate, so only five slow-cadence packets in the whole
corpus have a carrier to compare against. It also does not matter -- one LSB is
2.4e-06 deg on lane 0 and 9.7e-06 deg after the two-bit drop, against a
0.000968 deg median decode error we already accept. Nintendo's Switch 1 packer floors, but this project's shipping
DualSense encoder (`encode_switch2_g0/g1/g2` in `src/bt_hid/motion/ns2_ds5_motion.c`)
rounds to nearest and is hardware-validated. `nearest` is therefore the default,
so anything built on this codec is byte-identical to the path already proven on
a console; the mode stays selectable so the choice is visible rather than buried.

The lane arithmetic here is identical to that shipping encoder, which is checked
by a parity test. That encoder's own comment records an independent physical
confirmation of the sqrt(2) scale: decoding without it implies an impossible
~24.2 counts/(deg/s), while restoring it yields ~16.9 against a calibrated
16.384 carrier. The scale is therefore not a candidate explanation for the
canonical norm exceeding 1.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Literal, Sequence

SQRT2 = math.sqrt(2.0)
# A retained component is representable only within +/- sqrt(2)/2. Reaching this
# bound is what forces a chart change.
CARRIER_LIMIT = SQRT2 / 2.0
# Lane i is an unsigned field of CARRIER_BITS[i] bits.
CARRIER_BITS = (26, 25, 24)
# Length-0x28 prefix widths, high-rate then normal/catch-up.
PREFIX_BITS_HIGH_RATE = (24, 23, 25)
PREFIX_BITS_NORMAL = (22, 21, 23)
# The prefix samples the carrier this many ticks after the preceding carrier.
PREFIX_EPOCH_OFFSET = 4

RoundingMode = Literal["truncate", "nearest"]


class CarrierError(ValueError):
    """A carrier value or chart state is outside the encodable range."""


@dataclass(frozen=True)
class CarrierSample:
    """One encoded length-0x1E carrier."""

    state: int
    raw: tuple[int, int, int]
    retained: tuple[float, float, float]
    recharted: bool


def _quantize(value: float, mode: RoundingMode) -> int:
    if mode == "truncate":
        return math.floor(value)
    return math.floor(value + 0.5)


def retained_components(
    quaternion: Sequence[float], state: int
) -> tuple[float, float, float]:
    """The three components a given chart transmits, in wire order.

    ``quaternion`` is WXYZ. ``state`` names the omitted component; the retained
    three follow it cyclically, matching the genuine wire order.
    """

    if len(quaternion) != 4:
        raise CarrierError("quaternion must have four components")
    if not 0 <= state <= 3:
        raise CarrierError(f"chart state out of range: {state}")
    return tuple(  # type: ignore[return-value]
        quaternion[(state + index) & 3] for index in (1, 2, 3)
    )


def select_chart(
    quaternion: Sequence[float], current_state: int | None = None
) -> tuple[int, bool]:
    """Choose the chart for one sample, with the genuine hysteresis.

    Returns ``(state, recharted)``. The current chart is kept whenever it can
    still represent every retained component. Only when one would leave
    +/- sqrt(2)/2 does the chart move, and it then moves to the strict
    smallest-three choice -- omit the largest-magnitude component.

    Passing ``current_state=None`` requests a cold start, which uses the strict
    choice because there is no chart to hold.
    """

    if current_state is not None:
        held = retained_components(quaternion, current_state)
        if max(abs(value) for value in held) < CARRIER_LIMIT:
            return current_state, False
    largest = max(range(4), key=lambda index: abs(quaternion[index]))
    return largest, current_state is not None


def encode_carrier(
    quaternion: Sequence[float],
    current_state: int | None = None,
    mode: RoundingMode = "nearest",
) -> CarrierSample:
    """Encode one orientation into a genuine-shaped length-0x1E carrier."""

    state, recharted = select_chart(quaternion, current_state)
    retained = retained_components(quaternion, state)
    raw: list[int] = []
    for lane, value in enumerate(retained):
        if abs(value) > CARRIER_LIMIT:
            raise CarrierError(
                f"lane {lane} component {value!r} exceeds +/-{CARRIER_LIMIT}"
            )
        field = 1 << CARRIER_BITS[lane]
        scaled = (value / SQRT2 + 0.5) * field
        # Clamp only the exact endpoints; the field cannot represent `field`.
        raw.append(min(max(_quantize(scaled, mode), 0), field - 1))
    return CarrierSample(
        state=state,
        raw=(raw[0], raw[1], raw[2]),
        retained=retained,
        recharted=recharted,
    )


def decode_carrier(raw: Sequence[int]) -> tuple[float, float, float]:
    """Inverse of the lane quantization. Mirrors ns2_motion_reference."""

    if len(raw) != 3:
        raise CarrierError("carrier needs three lanes")
    return tuple(  # type: ignore[return-value]
        ((raw[lane] / float(1 << CARRIER_BITS[lane])) - 0.5) * SQRT2
        for lane in range(3)
    )


def _sign_extend(value: int, bits: int) -> int:
    field = 1 << bits
    value &= field - 1
    return value - field if value >= (field >> 1) else value


def encode_prefix(
    raw: Sequence[int], high_rate: bool
) -> tuple[int, int, int]:
    """Slice a carrier into the length-0x28 prefix's modular windows.

    Lane 2 is centred half a window away from lanes 0 and 1 and carries one
    extra bit of resolution; both facts come from integer residuals measured
    against genuine captures, not from the field widths alone.
    """

    if len(raw) != 3:
        raise CarrierError("carrier needs three lanes")
    centred = (
        raw[0] - (1 << 25),
        raw[1] - (1 << 24),
        2 * (raw[2] - (1 << 23)) + (1 << 24),
    )
    widths = PREFIX_BITS_HIGH_RATE if high_rate else PREFIX_BITS_NORMAL
    shift = 0 if high_rate else 2
    return tuple(  # type: ignore[return-value]
        _sign_extend(centred[lane] >> shift, widths[lane])
        for lane in range(3)
    )


def decode_prefix(
    wire: Sequence[int], reference_raw: Sequence[int], high_rate: bool
) -> tuple[int, int, int]:
    """Unwrap a modular prefix using a recent carrier to pick the window."""

    if len(wire) != 3 or len(reference_raw) != 3:
        raise CarrierError("prefix and reference both need three lanes")
    widths = PREFIX_BITS_HIGH_RATE if high_rate else PREFIX_BITS_NORMAL
    shift = 0 if high_rate else 2
    centred_reference = (
        reference_raw[0] - (1 << 25),
        reference_raw[1] - (1 << 24),
        2 * (reference_raw[2] - (1 << 23)) + (1 << 24),
    )
    out: list[int] = []
    for lane in range(3):
        span = 1 << (widths[lane] + shift)
        target = wire[lane] << shift
        base = centred_reference[lane]
        # Choose the window whose value is nearest the reference.
        candidate = target + round((base - target) / span) * span
        out.append(candidate)
    return (
        out[0] + (1 << 25),
        out[1] + (1 << 24),
        (out[2] - (1 << 24)) // 2 + (1 << 23),
    )


def prefix_epoch(current_tick: int, elapsed_ticks: int) -> int:
    """The carrier tick a prefix represents."""

    return current_tick - elapsed_ticks + PREFIX_EPOCH_OFFSET
